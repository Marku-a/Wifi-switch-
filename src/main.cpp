// Wi-Fi Kill Switch — ESP32-S3 + Xiaomi Mi Router AX1800
//
// Press the BOOT button to turn the household internet OFF / ON.
// "OFF" blocks internet (WAN) for every device on the router except this ESP32
// and the allow-list; Wi-Fi itself stays up so the switch can turn it back on.
//
// RGB LED:
//   green           internet ON and working
//   red (solid)     internet OFF (blocked by the button)
//   red (blinking)  internet ON but the router has no internet (ISP down)
//   yellow (solid)  problem: can't join Wi-Fi, can't reach / log in to router
//   yellow (blink)  working: starting up, connecting, applying a change
//   blue (blink)    pairing: press BOOT within 30 s to pair the phone
//
// BOOT: short press toggles; hold 10 s erases all paired phones.

#include <Arduino.h>
#include <Preferences.h>
#include <ESPmDNS.h>
#include <WebServer.h>
#include <WiFi.h>
#include <time.h>
#include <vector>

#include "Config.h"  // includes secrets.h, defaults MQTT_* when missing

#ifndef APP_PIN
#error "Add APP_PIN to include/secrets.h (see secrets.example.h)"
#endif

#include "MiRouter.h"
#include "Relay.h"
#include "SecureLink.h"

#ifndef RGB_LED_PIN
#define RGB_LED_PIN 48
#endif
#ifndef LED_BRIGHTNESS
#define LED_BRIGHTNESS 40
#endif

namespace {

const int kButtonPin = 0;  // BOOT button, active low
const uint32_t kStatusIntervalMs = 30000;
const uint32_t kReapplyBlockIntervalMs = 60000;  // catch devices that join while blocked
const uint32_t kWifiConnectTimeoutMs = 20000;
const uint32_t kPairWindowMs = 30000;
const uint32_t kShortPressMaxMs = 2000;
const uint32_t kLongPressMs = 10000;
const uint32_t kRelayStateIntervalMs = 5UL * 60UL * 1000UL;

enum class LedMode { Busy, Online, Blocked, NoInternet, Problem, Pairing, Reset };
volatile LedMode gLed = LedMode::Busy;

MiRouter router(ROUTER_IP, ROUTER_ADMIN_PASSWORD);
Preferences prefs;
WebServer server(80);
SecureLink secureLink;
Relay relay;

bool internetAllowed = true;        // desired state, persisted
std::vector<String> blockedMacs;    // MACs we blocked, persisted so we can unblock them
bool routerProblem = false;
uint32_t lastStatusMs = 0;
uint32_t lastReapplyMs = 0;

// Last relay state we published (to detect changes).
bool pubAllowed = true;
LedMode pubLed = LedMode::Busy;
uint32_t lastPubMs = 0;
bool pubValid = false;
bool gBtnConsumed = false;  // set when a press was used for pairing

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
      case LedMode::Pairing:    blinkOn ? setRgb(0, 0, 255) : setRgb(0, 0, 0); break;
      case LedMode::Reset:      ((millis() / 80) % 2 == 0) ? setRgb(0, 0, 255) : setRgb(0, 0, 0); break;
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
  prefs.putBool("allowed", internetAllowed);
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
  MDNS.end();
  MDNS.begin("wifiswitch");
  MDNS.addService("http", "tcp", 80);
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

bool applyBlock(bool showBusy) {
  if (showBusy) gLed = LedMode::Busy;
  if (!router.login()) {
    Serial.printf("[router] %s\n", router.lastError().c_str());
    return false;
  }
  std::vector<String> devices;
  if (!router.listDevices(devices)) {
    Serial.printf("[router] %s\n", router.lastError().c_str());
    return false;
  }
  std::vector<String> allow = splitMacs(ALLOWLIST_MACS);
  String self = WiFi.macAddress();
  self.toUpperCase();

  bool ok = true;
  for (auto& mac : devices) {
    if (mac == self || contains(allow, mac)) continue;
    if (router.setInternetAccess(mac, false)) {
      if (!contains(blockedMacs, mac)) blockedMacs.push_back(mac);
      Serial.printf("[router] blocked %s\n", mac.c_str());
    } else {
      ok = false;
      Serial.printf("[router] %s\n", router.lastError().c_str());
    }
  }
  saveState();
  return ok;
}

bool applyAllow(bool showBusy) {
  if (showBusy) gLed = LedMode::Busy;
  if (!router.login()) {
    Serial.printf("[router] %s\n", router.lastError().c_str());
    return false;
  }
  std::vector<String> targets = blockedMacs;
  std::vector<String> devices;
  if (router.listDevices(devices)) {
    for (auto& mac : devices) {
      if (!contains(targets, mac)) targets.push_back(mac);
    }
  }
  String self = WiFi.macAddress();
  self.toUpperCase();

  std::vector<String> stillBlocked;
  for (auto& mac : targets) {
    if (mac == self) continue;
    if (router.setInternetAccess(mac, true)) {
      Serial.printf("[router] allowed %s\n", mac.c_str());
    } else {
      Serial.printf("[router] %s\n", router.lastError().c_str());
      if (contains(blockedMacs, mac)) stillBlocked.push_back(mac);
    }
  }
  blockedMacs = stillBlocked;
  saveState();
  return stillBlocked.empty();
}

// showBusy=false for background re-syncs so the LED doesn't flicker every minute.
bool applyDesiredState(bool showBusy = true) {
  bool ok = internetAllowed ? applyAllow(showBusy) : applyBlock(showBusy);
  routerProblem = !ok;
  lastReapplyMs = millis();
  return ok;
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
  if (!internetAllowed) {
    gLed = LedMode::Blocked;
    Serial.println("[status] internet OFF (blocked)");
    return;
  }
  bool online = hasInternet();
  gLed = online ? LedMode::Online : LedMode::NoInternet;
  Serial.printf("[status] internet ON, %s\n", online ? "online" : "router has NO internet");
}

// Shared by the BOOT button and the HTTP API.
void requestInternet(bool allowed) {
  internetAllowed = allowed;
  saveState();
  Serial.printf("[switch] turning internet %s\n", internetAllowed ? "ON" : "OFF");
  if (connectWifi()) applyDesiredState();
  else routerProblem = true;  // retry once Wi-Fi is back
  updateStatus();
}

// ---------- HTTP API ----------

const char* ledName() {
  switch (gLed) {
    case LedMode::Online:     return "online";
    case LedMode::Blocked:    return "blocked";
    case LedMode::NoInternet: return "no_internet";
    case LedMode::Problem:    return "problem";
    case LedMode::Busy:       return "busy";
    case LedMode::Pairing:    return "busy";
    case LedMode::Reset:      return "busy";
  }
  return "busy";
}

void sendJson(int code, const String& body) {
  server.send(code, "application/json", body);
}

void sendError(int code, const String& msg) {
  sendJson(code, "{\"error\":\"" + msg + "\"}");
}

// ---------- button ----------

enum class Btn { None, Down, Short, Long };

// Debounced. Down = press edge, Short = released after < 2 s, Long = held >= 10 s.
// A press marked consumed (gBtnConsumed) produces no Short/Long.
Btn pollButton() {
  static bool stable = HIGH;
  static bool lastRead = HIGH;
  static uint32_t changedAt = 0;
  static uint32_t downAt = 0;
  static bool longFired = false;
  bool now = digitalRead(kButtonPin);
  if (now != lastRead) {
    lastRead = now;
    changedAt = millis();
  }
  if (millis() - changedAt > 40 && now != stable) {
    stable = now;
    if (stable == LOW) {
      downAt = millis();
      longFired = false;
      gBtnConsumed = false;
      return Btn::Down;
    }
    uint32_t held = millis() - downAt;
    if (!longFired && !gBtnConsumed && held < kShortPressMaxMs) return Btn::Short;
    return Btn::None;
  }
  if (stable == LOW && !longFired && !gBtnConsumed && millis() - downAt >= kLongPressMs) {
    longFired = true;
    return Btn::Long;
  }
  return Btn::None;
}

// ---------- relay state ----------

void publishRelayState() {
  if (!relay.enabled() || !relay.connected() || !SecureLink::clockSynced()) return;
  String j = secureLink.stateJson(internetAllowed, ledName(), !routerProblem,
                                  SecureLink::nowSecs(), "");
  if (relay.publishState(j)) {
    pubAllowed = internetAllowed;
    pubLed = gLed;
    lastPubMs = millis();
    pubValid = true;
  }
}

// ---------- commands (shared by LAN and relay) ----------

int processCommand(const String& json, SecureLink::Command& cmd) {
  int code = secureLink.verifyCommand(json, cmd);
  if (code != 200) {
    Serial.printf("[cmd] rejected (%d)\n", code);
    return code;
  }
  Serial.printf("[cmd] ok op=%s\n", cmd.op.c_str());
  if (cmd.op == "set") requestInternet(cmd.arg == "on");
  else if (cmd.op == "unpair") secureLink.unpair(cmd.kid);
  return 200;
}

String lanState(const String& echo) {
  return secureLink.stateJson(internetAllowed, ledName(), !routerProblem,
                              SecureLink::clockSynced() ? SecureLink::nowSecs() : 0, echo);
}

// ---------- HTTP API ----------

void handlePair() {
  uint8_t appPub[65];
  String err;
  int code = secureLink.checkPairRequest(server.arg("plain"), appPub, err);
  if (code != 0) {
    Serial.printf("[pair] rejected (%d)\n", code);
    sendError(code, err);
    return;
  }
  Serial.println("[pair] PIN ok, press BOOT within 30 s");
  gLed = LedMode::Pairing;
  bool pressed = false;
  uint32_t start = millis();
  while (millis() - start < kPairWindowMs) {
    if (pollButton() == Btn::Down) {
      gBtnConsumed = true;  // this press must not toggle the switch
      pressed = true;
      break;
    }
    delay(10);
  }
  if (!pressed) {
    Serial.println("[pair] timed out");
    sendError(408, "press the BOOT button on the switch");
    updateStatus();
    return;
  }
  String body;
  code = secureLink.completePair(appPub, body, err);
  if (code == 200) sendJson(200, body);
  else sendError(code, err);
  updateStatus();
  publishRelayState();
}

void handleApi() {
  String uri = server.uri();
  HTTPMethod method = server.method();
  if (uri == "/api/challenge" && method == HTTP_GET) {
    sendJson(200, "{\"nonce\":\"" + secureLink.newChallenge() + "\"}");
  } else if (uri == "/api/cmd" && method == HTTP_POST) {
    SecureLink::Command cmd;
    int code = processCommand(server.arg("plain"), cmd);
    if (code == 200) sendJson(200, lanState(cmd.nonce));
    else if (code == 401) sendError(401, "bad signature");
    else if (code == 409) sendError(409, "stale");
    else sendError(400, "malformed");
  } else if (uri == "/api/state" && method == HTTP_GET) {
    sendJson(200, lanState(server.arg("echo")));
  } else if (uri == "/api/pair" && method == HTTP_POST) {
    handlePair();
  } else {
    sendError(404, "not found");
  }
}

}  // namespace

void setup() {
  Serial.begin(115200);
  pinMode(kButtonPin, INPUT_PULLUP);
  xTaskCreate(ledTask, "led", 2048, nullptr, 1, nullptr);
  randomSeed(esp_random());

  secureLink.begin();
  relay.begin(secureLink.deviceId());
  prefs.begin("wifiswitch", false);
  internetAllowed = prefs.getBool("allowed", true);
  blockedMacs = splitMacs(prefs.getString("blocked", ""));
  Serial.printf("\n[boot] Wi-Fi switch starting, saved state: internet %s\n",
                internetAllowed ? "ON" : "OFF");

  if (connectWifi()) {
    applyDesiredState();  // re-sync the router with the saved state
  } else {
    routerProblem = true;  // re-sync once Wi-Fi is back
  }
  updateStatus();

  server.onNotFound(handleApi);
  server.collectHeaders(nullptr, 0);
  server.begin();
}

void loop() {
  server.handleClient();

  Btn btn = pollButton();
  if (btn == Btn::Short) {
    Serial.println("[button] pressed");
    requestInternet(!internetAllowed);
  } else if (btn == Btn::Long) {
    secureLink.unpairAll();
    Serial.println("[pair] all phones unpaired");
    gLed = LedMode::Reset;
    delay(2000);
    updateStatus();
    publishRelayState();
  }

  relay.loop();
  String relayMsg;
  if (relay.takePending(relayMsg)) {
    SecureLink::Command cmd;
    if (processCommand(relayMsg, cmd) == 200) publishRelayState();
  }
  if (relay.takeJustConnected() || !pubValid || pubAllowed != internetAllowed ||
      pubLed != gLed || millis() - lastPubMs >= kRelayStateIntervalMs) {
    publishRelayState();
  }

  if (WiFi.status() != WL_CONNECTED) {
    gLed = LedMode::Problem;
    if (millis() - lastStatusMs > kStatusIntervalMs) {
      if (connectWifi()) applyDesiredState();
      else routerProblem = true;  // retry once Wi-Fi is back
      updateStatus();
    }
  } else {
    bool retry = routerProblem && millis() - lastReapplyMs > kStatusIntervalMs;
    bool reblock = !internetAllowed && millis() - lastReapplyMs > kReapplyBlockIntervalMs;
    if (retry || reblock) applyDesiredState(retry);
    if (retry || reblock || millis() - lastStatusMs > kStatusIntervalMs) updateStatus();
  }
  delay(10);
}
