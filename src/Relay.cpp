#include "Relay.h"

#include <PubSubClient.h>
#include <WiFi.h>
#include <WiFiClientSecure.h>

#include "Config.h"
#include "SecureLink.h"
#include "ca_certs.h"

namespace {

const uint32_t kReconnectMs = 15000;
const unsigned int kMaxPayload = 1024;

WiFiClientSecure net;
PubSubClient mqtt(net);

String clientId, topicCmd, topicState, topicOnline;
String pending;
bool hasPending = false;
bool justConnected = false;
bool triedOnce = false;
uint32_t lastTryMs = 0;
bool started = false;

void onMessage(char*, uint8_t* payload, unsigned int length) {
  if (hasPending || length == 0 || length > kMaxPayload) return;
  pending = "";
  pending.concat((const char*)payload, length);
  hasPending = true;
}

}  // namespace

bool Relay::enabled() const { return strlen(MQTT_HOST) > 0; }

void Relay::begin(const String& deviceId) {
  if (!enabled()) return;
  clientId = "ws-" + deviceId;
  String base = "wifiswitch/" + deviceId;
  topicCmd = base + "/cmd";
  topicState = base + "/state";
  topicOnline = base + "/online";
  net.setCACert(kRelayRootCAs);
  net.setTimeout(5);  // seconds
  mqtt.setServer(MQTT_HOST, MQTT_PORT);
  mqtt.setBufferSize(1024);
  mqtt.setKeepAlive(60);
  mqtt.setSocketTimeout(5);
  mqtt.setCallback(onMessage);
  started = true;
}

void Relay::loop() {
  if (!started) return;
  if (WiFi.status() != WL_CONNECTED) return;
  if (mqtt.connected()) {
    mqtt.loop();
    return;
  }
  // TLS certificate validation needs a valid clock.
  if (!SecureLink::clockSynced()) return;
  if (triedOnce && millis() - lastTryMs < kReconnectMs) return;
  triedOnce = true;
  lastTryMs = millis();
  const char* user = strlen(MQTT_USER) ? MQTT_USER : nullptr;
  const char* pass = strlen(MQTT_USER) ? MQTT_PASS : nullptr;
  if (mqtt.connect(clientId.c_str(), user, pass, topicOnline.c_str(), 1, true, "0")) {
    mqtt.publish(topicOnline.c_str(), "1", true);
    mqtt.subscribe(topicCmd.c_str(), 1);
    justConnected = true;
    Serial.println("[relay] connected");
  } else {
    Serial.printf("[relay] connect failed, state=%d\n", mqtt.state());
  }
}

bool Relay::connected() { return started && mqtt.connected(); }

bool Relay::takeJustConnected() {
  bool r = justConnected;
  justConnected = false;
  return r;
}

bool Relay::takePending(String& out) {
  if (!hasPending) return false;
  out = pending;
  pending = "";
  hasPending = false;
  return true;
}

bool Relay::publishState(const String& json) {
  if (!connected()) return false;
  return mqtt.publish(topicState.c_str(), (const uint8_t*)json.c_str(), json.length(), true);
}
