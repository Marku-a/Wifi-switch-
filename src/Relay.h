// Optional MQTT-over-TLS relay (see docs/SECURITY.md). No-op when MQTT_HOST is empty.
#pragma once

#include <Arduino.h>

class Relay {
 public:
  bool enabled() const;
  void begin(const String& deviceId);
  void loop();
  bool connected();
  // True once after each (re)connect so the caller can publish fresh state.
  bool takeJustConnected();
  // Copies out and clears the pending command received on the cmd topic.
  bool takePending(String& out);
  bool publishState(const String& json);
};
