// Pairing, command verification and signed status. Protocol: docs/SECURITY.md
#pragma once

#include <Arduino.h>
#include <Preferences.h>

class SecureLink {
 public:
  struct Command {
    int kid = -1;
    String op, arg, nonce;
    uint32_t ts = 0;
  };

  void begin();
  const String& deviceId() const { return deviceId_; }
  int pairedCount() const;

  // True once NTP has set the clock.
  static bool clockSynced();
  static uint32_t nowSecs();

  // 0 = ok to proceed (appPub filled), else HTTP status with err text.
  int checkPairRequest(const String& body, uint8_t appPub[65], String& err);
  // 200 (responseJson filled) or 507/500 with err text.
  int completePair(const uint8_t appPub[65], String& responseJson, String& err);

  String newChallenge();
  // 200 ok, 400 malformed, 401 bad signature / unpaired kid, 409 stale or replayed.
  int verifyCommand(const String& json, Command& out);

  void unpair(int kid);
  void unpairAll();

  String stateJson(bool internetOn, const char* led, bool routerOk, uint32_t ts,
                   const String& echo);

 private:
  static const int kSlots = 4;
  static const int kNonceCache = 64;

  void saveKeys();
  bool slotUsed(int kid) const { return kid >= 0 && kid < kSlots && (used_ & (1 << kid)); }

  Preferences prefs_;
  String deviceId_;
  uint8_t keys_[kSlots][32];
  uint8_t used_ = 0;

  int pinFails_ = 0;
  bool locked_ = false;
  uint32_t lockUntil_ = 0;

  bool haveChallenge_ = false;
  String challenge_;
  uint32_t challengeAt_ = 0;

  String nonces_[kNonceCache];
  int nonceNext_ = 0;
};
