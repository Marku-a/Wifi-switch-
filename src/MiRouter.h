// Minimal client for the local web API of Xiaomi Mi routers (MiWiFi / luci).
// Tested API shape: Mi Router AX1800 family (also used by AX3000/AX6000).
#pragma once

#include <Arduino.h>
#include <vector>

class MiRouter {
public:
  MiRouter(const char* ip, const char* adminPassword);

  // Logs in and stores the session token (stok). Re-call when an API call fails.
  bool login();
  bool loggedIn() const { return _token.length() > 0; }

  // MACs (upper-case, AA:BB:CC:DD:EE:FF) of devices the router currently knows.
  bool listDevices(std::vector<String>& macs);

  // Allow / block internet (WAN) access for one device. LAN/Wi-Fi stay up.
  bool setInternetAccess(const String& mac, bool allow);

  // True if the router answers on port 80.
  bool reachable();

  const String& lastError() const { return _error; }

private:
  struct Response {
    int status = 0;
    String raw;   // everything the router sent (headers + body)
    String body;  // after the blank line, or raw if headers are malformed
  };

  bool request(const char* method, const String& path, const String& body, Response& out,
               size_t maxBytes = 16384);
  bool detectEncryption();
  String hashHex(const String& input) const;
  uint32_t unixTime();
  void noteDateHeader(const String& raw);

  String _ip;
  String _password;
  String _token;
  String _key;
  bool _sha256 = false;
  bool _encryptionKnown = false;
  String _error;
  uint32_t _routerEpoch = 0;   // from HTTP Date header, fallback when NTP is unavailable
  uint32_t _routerEpochAt = 0; // millis() when _routerEpoch was captured
};
