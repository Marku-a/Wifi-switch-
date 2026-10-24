// Wi-Fi Kill Switch — ESP32-S3 + Xiaomi Mi Router AX1800
//
// Press the BOOT button to turn the household internet OFF / ON.
// "OFF" blocks internet (WAN) for every device on the router except this ESP32
// and the allow-list; Wi-Fi itself stays up so the switch can turn it back on.
//
// The router is the source of truth. The ESP32 stores no ON/OFF state: every 30 s
// (and on each button press) it reads the per-device "Internet access" flags from
// the router. That means the official Mi WiFi app can be used remotely to block or
// unblock devices and the ESP32 never undoes it. New devices that join while the
// internet is OFF are blocked; devices you unblock in the app are left alone.
//
// RGB LED:
//   green           internet ON and working (at least one device is not blocked)
//   red (solid)     internet OFF: every device is blocked. If you unblock some
//                   devices in the Mi WiFi app the LED turns green.
//   red (blinking)  internet ON but the router has no internet (ISP down)
//   yellow (solid)  problem: can't join Wi-Fi, can't reach / log in to router
//   yellow (blink)  working: starting up, connecting, applying a change

#include <Arduino.h>
#include <Preferences.h>
#include <WiFi.h>
#include <time.h>
#include <vector>

#if __has_include("secrets.h")
#include "secrets.h"
#else
#error "Missing include/secrets.h - copy include/secrets.example.h to include/secrets.h and fill it in."
#endif

#include "MiRouter.h"

#ifndef RGB_LED_PIN
#define RGB_LED_PIN 48
#endif
#ifndef LED_BRIGHTNESS
#define LED_BRIGHTNESS 40
#endif

namespace {

const int kButtonPin = 0;  // BOOT button, active low
const uint32_t kStatusIntervalMs = 30000;
const uint32_t kWifiConnectTimeoutMs = 20000;

enum class LedMode { Busy, Online, Blocked, NoInternet, Problem };
volatile LedMode gLed = LedMode::Busy;

MiRouter router(ROUTER_IP, ROUTER_ADMIN_PASSWORD);
Preferences prefs;

bool internetOn = true;             // derived from the router, never persisted
std::vector<String> blockedMacs;    // MACs we blocked, persisted so we can unblock them
std::vector<String> knownMacs;      // MACs seen in the previous refresh (RAM only)
std::vector<MiRouter::Device> devices;  // last device list from the router
bool routerProblem = false;
uint32_t lastStatusMs = 0;

// ---------- LED ----------

void setRgb(uint8_t r, uint8_t g, uint8_t b) {
  neopixelWrite(RGB_LED_PIN, (r * LED_BRIGHTNESS) / 255, (g * LED_BRIGHTNESS) / 255,
                (b * LED_BRIGHTNESS) / 255);
}

void ledTask(void*) {
  for (;;) {
    bool blinkOn = (millis() / 250) % 2 == 0;
    switch (gLed) {
      case LedMode::Online:     setRgb(0, 255, 0); break;
      case LedMode::Blocked:    setRgb(255, 0, 0); break;
      case LedMode::NoInternet: blinkOn ? setRgb(255, 0, 0) : setRgb(0, 0, 0); break;
      case LedMode::Problem:    setRgb(255, 140, 0); break;
      case LedMode::Busy:       blinkOn ? setRgb(255, 140, 0) : setRgb(0, 0, 0); break;
    }
    vTaskDelay(pdMS_TO_TICKS(50));
  }
}

// ---------- persistence ----------

String joinMacs(const std::vector<String>& macs) {
  String s;
  for (auto& m : macs) {
    if (s.length()) s += ',';
    s += m;
  }
  return s;
}

std::vector<String> splitMacs(String csv) {
  std::vector<String> out;
  csv.toUpperCase();
  csv.replace(" ", "");
  int from = 0;
  while (from < (int)csv.length()) {
    int comma = csv.indexOf(',', from);
    if (comma < 0) comma = csv.length();
    String mac = csv.substring(from, comma);
    if (mac.length() == 17) out.push_back(mac);
    from = comma + 1;
  }
  return out;
}

bool contains(const std::vector<String>& v, const String& s) {
  for (auto& x : v) {
    if (x == s) return true;
  }
  return false;
}

void saveState() {
  prefs.putString("blocked", joinMacs(blockedMacs));
}

// ---------- network ----------

bool connectWifi() {
  if (WiFi.status() == WL_CONNECTED) return true;
  gLed = LedMode::Busy;
  Serial.printf("[wifi] connecting to %s\n", WIFI_SSID);
  WiFi.mode(WIFI_STA);
  WiFi.setAutoReconnect(true);
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
  uint32_t start = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - start < kWifiConnectTimeoutMs) delay(200);
  if (WiFi.status() != WL_CONNECTED) {
    Serial.println("[wifi] failed (router off, wrong password, or out of range)");
    return false;
  }
  Serial.printf("[wifi] connected, ip=%s mac=%s\n", WiFi.localIP().toString().c_str(),
                WiFi.macAddress().c_str());
  configTime(0, 0, "pool.ntp.org", "time.google.com");
  return true;
}

bool hasInternet() {
  // The ESP32 itself is never blocked, so this measures the router's WAN link.
  WiFiClient c;
  if (c.connect("connectivitycheck.gstatic.com", 80, 4000)) {
    c.print("GET /generate_204 HTTP/1.0\r\nHost: connectivitycheck.gstatic.com\r\n\r\n");
    uint32_t start = millis();
    while (!c.available() && c.connected() && millis() - start < 4000) delay(10);
    String line = c.readStringUntil('\n');
    c.stop();
    if (line.indexOf(" 204") > 0) return true;
  }
  c.stop();
  // DNS may be the broken part; try a plain IP as a fallback.
  bool ok = c.connect(IPAddress(1, 1, 1, 1), 80, 4000);
  c.stop();
  return ok;
}

// ---------- router actions ----------

bool isControllable(const String& mac) {
  static std::vector<String> allow = splitMacs(ALLOWLIST_MACS);
  String self = WiFi.macAddress();
  self.toUpperCase();
  return mac != self && !contains(allow, mac);
}

void computeInternetOn() {
  int reporting = 0;  // controllable devices whose WAN flag the router reports
  bool allBlocked = true;
  for (auto& d : devices) {
    if (!isControllable(d.mac) || d.wan == -1) continue;
    reporting++;
    if (d.wan != 0) allBlocked = false;
  }
  if (reporting == 0) {
    // Firmware without per-device WAN flags: fall back to what this ESP blocked.
    internetOn = blockedMacs.empty();
    return;
  }
  internetOn = !allBlocked;
}

// Reads the device list from the router and derives internetOn from it.
// On failure sets routerProblem and keeps the previous internetOn.
bool refreshFromRouter() {
  if (WiFi.status() != WL_CONNECTED) {
    routerProblem = true;
    return false;
  }
  if (!router.login() || !router.listDevices(devices)) {
    Serial.printf("[router] %s\n", router.lastError().c_str());
    routerProblem = true;
    return false;
  }
  routerProblem = false;

  // While OFF, block brand-new devices (not seen in the previous refresh).
  // Known devices that are unblocked were unblocked on purpose in the Mi app.
  if (!internetOn) {
    bool changed = false;
    for (auto& d : devices) {
      if (!isControllable(d.mac) || d.wan != 1 || contains(knownMacs, d.mac)) continue;
      if (router.setInternetAccess(d.mac, false)) {
        d.wan = 0;
        if (!contains(blockedMacs, d.mac)) blockedMacs.push_back(d.mac);
        changed = true;
        Serial.printf("[router] blocked new device %s\n", d.mac.c_str());
      } else {
        Serial.printf("[router] %s\n", router.lastError().c_str());
      }
    }
    if (changed) saveState();
  }

  knownMacs.clear();
  for (auto& d : devices) knownMacs.push_back(d.mac);
  computeInternetOn();
  return true;
}

void handleButton() {
  gLed = LedMode::Busy;
  if (!refreshFromRouter()) return;
  bool turnOff = internetOn;
  Serial.printf("[button] turning internet %s\n", turnOff ? "OFF" : "ON");

  if (turnOff) {
    for (auto& d : devices) {
      if (!isControllable(d.mac) || d.wan == 0) continue;
      if (router.setInternetAccess(d.mac, false)) {
        if (!contains(blockedMacs, d.mac)) blockedMacs.push_back(d.mac);
        Serial.printf("[router] blocked %s\n", d.mac.c_str());
      } else {
        Serial.printf("[router] %s\n", router.lastError().c_str());
      }
    }
  } else {
    std::vector<String> targets;
    for (auto& d : devices) {
      if (isControllable(d.mac) && d.wan == 0) targets.push_back(d.mac);
    }
    for (auto& m : blockedMacs) {
      if (isControllable(m) && !contains(targets, m)) targets.push_back(m);
    }
    for (auto& mac : targets) {
      if (router.setInternetAccess(mac, true)) {
        Serial.printf("[router] allowed %s\n", mac.c_str());
        std::vector<String> rest;
        for (auto& m : blockedMacs) {
          if (m != mac) rest.push_back(m);
        }
        blockedMacs = rest;
      } else {
        Serial.printf("[router] %s\n", router.lastError().c_str());
      }
    }
  }
  refreshFromRouter();  // show what the router really says
  saveState();
}

void updateStatus() {
  lastStatusMs = millis();
  if (WiFi.status() != WL_CONNECTED) {
    gLed = LedMode::Problem;
    return;
  }
  if (routerProblem || !router.reachable()) {
    Serial.println("[status] problem talking to router");
    gLed = LedMode::Problem;
    return;
  }
  if (!internetOn) {
    gLed = LedMode::Blocked;
    Serial.println("[status] internet OFF (blocked)");
    return;
  }
  bool online = hasInternet();
  gLed = online ? LedMode::Online : LedMode::NoInternet;
  Serial.printf("[status] internet ON, %s\n", online ? "online" : "router has NO internet");
}

// ---------- button ----------

bool buttonPressed() {
  static bool lastStable = HIGH;
  static bool lastRead = HIGH;
  static uint32_t changedAt = 0;
  bool now = digitalRead(kButtonPin);
  if (now != lastRead) {
    lastRead = now;
    changedAt = millis();
  }
  if (millis() - changedAt > 40 && now != lastStable) {
    lastStable = now;
    return lastStable == LOW;
  }
  return false;
}

}  // namespace

void setup() {
  Serial.begin(115200);
  pinMode(kButtonPin, INPUT_PULLUP);
  xTaskCreate(ledTask, "led", 2048, nullptr, 1, nullptr);
  randomSeed(esp_random());

  prefs.begin("wifiswitch", false);
  blockedMacs = splitMacs(prefs.getString("blocked", ""));
  Serial.println("\n[boot] Wi-Fi switch starting");

  if (connectWifi()) refreshFromRouter();
  else routerProblem = true;
  updateStatus();
}

void loop() {
  if (buttonPressed()) {
    if (connectWifi()) handleButton();
    else routerProblem = true;
    updateStatus();
  }

  if (millis() - lastStatusMs > kStatusIntervalMs) {
    if (connectWifi()) refreshFromRouter();  // quiet: LED stays as is until updateStatus()
    else routerProblem = true;
    updateStatus();
  } else if (WiFi.status() != WL_CONNECTED) {
    gLed = LedMode::Problem;
  }
  delay(10);
}
