#include "MiRouter.h"

#include <WiFi.h>
#include <time.h>
#include <mbedtls/sha1.h>
#include <mbedtls/sha256.h>

namespace {

// Public key embedded in the MiWiFi web UI; used when the login page can't be parsed.
const char* kDefaultKey = "a2ffa5c9be07488bbb04a3a47d3c5f6a";
const uint32_t kHttpTimeoutMs = 8000;

String toHex(const uint8_t* data, size_t len) {
  static const char* digits = "0123456789abcdef";
  String s;
  s.reserve(len * 2);
  for (size_t i = 0; i < len; i++) {
    s += digits[data[i] >> 4];
    s += digits[data[i] & 0x0f];
  }
  return s;
}

bool isHex(const String& s) {
  for (size_t i = 0; i < s.length(); i++) {
    if (!isxdigit((unsigned char)s[i])) return false;
  }
  return s.length() > 0;
}

String urlEncode(const String& s) {
  String out;
  for (size_t i = 0; i < s.length(); i++) {
    char c = s[i];
    if (isalnum((unsigned char)c) || c == '-' || c == '_' || c == '.' || c == '~') {
      out += c;
    } else {
      char buf[4];
      snprintf(buf, sizeof(buf), "%%%02X", (unsigned char)c);
      out += buf;
    }
  }
  return out;
}

// Extracts the string value of "field":"value" from a flat JSON text.
String jsonString(const String& json, const char* field, int from = 0) {
  String needle = String("\"") + field + "\"";
  int i = json.indexOf(needle, from);
  if (i < 0) return "";
  i = json.indexOf(':', i + needle.length());
  if (i < 0) return "";
  int q1 = json.indexOf('"', i + 1);
  if (q1 < 0) return "";
  int q2 = json.indexOf('"', q1 + 1);
  if (q2 < 0) return "";
  return json.substring(q1 + 1, q2);
}

// Days since 1970-01-01 for a civil date (Howard Hinnant's algorithm).
int32_t daysFromCivil(int y, unsigned m, unsigned d) {
  y -= m <= 2;
  const int era = (y >= 0 ? y : y - 399) / 400;
  const unsigned yoe = (unsigned)(y - era * 400);
  const unsigned doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;
  const unsigned doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
  return era * 146097 + (int32_t)doe - 719468;
}

}  // namespace

MiRouter::MiRouter(const char* ip, const char* adminPassword)
    : _ip(ip), _password(adminPassword), _key(kDefaultKey) {}

bool MiRouter::reachable() {
  WiFiClient c;
  bool ok = c.connect(_ip.c_str(), 80, 3000);
  c.stop();
  return ok;
}

// Plain HTTP/1.0 over a raw socket. Some Mi firmwares answer set_mac_filter with
// a malformed response line ("set mac: ... succeeded!"), which stricter HTTP
// clients reject, so we keep everything and let the caller inspect it.
bool MiRouter::request(const char* method, const String& path, const String& body,
                       Response& out, size_t maxBytes) {
  out = Response();
  WiFiClient c;
  if (!c.connect(_ip.c_str(), 80, 4000)) {
    _error = "router not reachable at " + _ip;
    return false;
  }
  c.setTimeout(kHttpTimeoutMs / 1000);

  String req;
  req.reserve(256 + body.length());
  req += method;
  req += ' ';
  req += path;
  req += " HTTP/1.0\r\nHost: ";
  req += _ip;
  req += "\r\nUser-Agent: esp32-wifi-switch\r\nConnection: close\r\n";
  if (body.length()) {
    req += "Content-Type: application/x-www-form-urlencoded\r\nContent-Length: ";
    req += body.length();
    req += "\r\n";
  }
  req += "\r\n";
  req += body;
  c.print(req);

  uint32_t start = millis();
  out.raw.reserve(1024);
  while (millis() - start < kHttpTimeoutMs) {
    while (c.available()) {
      int ch = c.read();
      if (ch < 0) break;
      if (out.raw.length() < maxBytes) out.raw += (char)ch;
    }
    if (!c.connected() && !c.available()) break;
    delay(5);
  }
  c.stop();

  if (out.raw.length() == 0) {
    _error = "empty reply from router";
    return false;
  }
  if (out.raw.startsWith("HTTP/")) {
    int sp = out.raw.indexOf(' ');
    if (sp > 0) out.status = out.raw.substring(sp + 1, sp + 4).toInt();
  }
  int split = out.raw.indexOf("\r\n\r\n");
  out.body = split >= 0 ? out.raw.substring(split + 4) : out.raw;
  noteDateHeader(out.raw);
  return true;
}

void MiRouter::noteDateHeader(const String& raw) {
  // "Date: Sat, 24 Oct 2026 10:00:00 GMT"
  int i = raw.indexOf("\nDate: ");
  if (i < 0) return;
  int end = raw.indexOf('\r', i + 1);
  if (end < 0) return;
  String v = raw.substring(i + 7, end);
  int comma = v.indexOf(", ");
  if (comma < 0) return;
  v = v.substring(comma + 2);  // "24 Oct 2026 10:00:00 GMT"
  int day, year, hh, mm, ss;
  char mon[4] = {0};
  if (sscanf(v.c_str(), "%d %3s %d %d:%d:%d", &day, mon, &year, &hh, &mm, &ss) != 6) return;
  static const char* months = "JanFebMarAprMayJunJulAugSepOctNovDec";
  const char* p = strstr(months, mon);
  if (!p || year < 2020) return;
  unsigned month = (unsigned)((p - months) / 3 + 1);
  _routerEpoch = (uint32_t)daysFromCivil(year, month, (unsigned)day) * 86400UL + hh * 3600UL +
                 mm * 60UL + ss;
  _routerEpochAt = millis();
}

uint32_t MiRouter::unixTime() {
  time_t now = time(nullptr);
  if (now > 1700000000) return (uint32_t)now;  // NTP synced
  if (_routerEpoch) return _routerEpoch + (millis() - _routerEpochAt) / 1000;
  return 1700000000UL + millis() / 1000;  // last resort; most firmwares accept it
}

// The login page tells us which hash the firmware wants (SHA-1 on older,
// SHA-256 when "newEncryptMode" is 1) and the public key.
bool MiRouter::detectEncryption() {
  Response r;
  if (!request("GET", "/cgi-bin/luci/web", "", r, 131072)) return false;
  if (r.status != 200) {
    _error = "login page returned HTTP " + String(r.status);
    return false;
  }

  int m = r.body.indexOf("newEncryptMode");
  if (m >= 0) {
    String tail = r.body.substring(m + 14, m + 30);
    tail.replace(" ", "");
    tail.replace("'", "");
    tail.replace("\"", "");
    _sha256 = tail.startsWith(":1");
  }

  int k = r.body.indexOf("key:");
  while (k >= 0) {
    int q1 = k + 4;
    while (q1 < (int)r.body.length() && r.body[q1] == ' ') q1++;
    char quote = r.body[q1];
    if (quote == '\'' || quote == '"') {
      int q2 = r.body.indexOf(quote, q1 + 1);
      String candidate = r.body.substring(q1 + 1, q2);
      if (q2 > 0 && candidate.length() == 32 && isHex(candidate)) {
        _key = candidate;
        break;
      }
    }
    k = r.body.indexOf("key:", k + 4);
  }

  _encryptionKnown = true;
  Serial.printf("[router] hash=%s key=%s...\n", _sha256 ? "SHA256" : "SHA1",
                _key.substring(0, 6).c_str());
  return true;
}

String MiRouter::hashHex(const String& input) const {
  const uint8_t* data = (const uint8_t*)input.c_str();
  if (_sha256) {
    uint8_t out[32];
    mbedtls_sha256(data, input.length(), out, 0);
    return toHex(out, sizeof(out));
  }
  uint8_t out[20];
  mbedtls_sha1(data, input.length(), out);
  return toHex(out, sizeof(out));
}

bool MiRouter::login() {
  _token = "";
  if (!_encryptionKnown && !detectEncryption()) return false;

  String mac = WiFi.macAddress();
  String nonce = "0_" + mac + "_" + String(unixTime()) + "_" + String(random(1000, 10000));
  String password = hashHex(nonce + hashHex(_password + _key));

  String form = "username=admin&logtype=2&password=" + password + "&nonce=" + urlEncode(nonce);
  Response r;
  if (!request("POST", "/cgi-bin/luci/api/xqsystem/login", form, r)) return false;

  String token = jsonString(r.body, "token");
  if (token.length() == 0) {
    String code = r.body.substring(0, 120);
    code.trim();
    _error = "login rejected (wrong admin password?) reply: " + code;
    // The login page may have been misread; detect the hash mode again next time.
    _encryptionKnown = false;
    _sha256 = false;
    _key = kDefaultKey;
    return false;
  }
  _token = token;
  return true;
}

bool MiRouter::listDevices(std::vector<Device>& out) {
  out.clear();
  if (!loggedIn() && !login()) return false;
  Response r;
  if (!request("GET", "/cgi-bin/luci/;stok=" + _token + "/api/misystem/devicelist", "", r,
               65536)) {
    return false;
  }
  if (r.body.indexOf("\"code\":0") < 0) {
    _token = "";  // most likely an expired token
    _error = "devicelist failed: " + r.body.substring(0, 120);
    return false;
  }
  int from = 0;
  while (true) {
    int i = r.body.indexOf("\"mac\"", from);
    if (i < 0) break;
    String mac = jsonString(r.body, "mac", i);
    from = i + 5;
    mac.toUpperCase();
    if (mac.length() != 17) continue;

    // "wan" must be inside this list entry: before the next "mac" (or end of body).
    int next = r.body.indexOf("\"mac\"", from);
    int wan = -1;
    int w = r.body.indexOf("\"wan\"", i);
    if (w >= 0 && (next < 0 || w < next)) {
      int colon = r.body.indexOf(':', w + 5);
      if (colon >= 0 && (next < 0 || colon < next)) {
        int p = colon + 1;
        while (p < (int)r.body.length() && (r.body[p] == ' ' || r.body[p] == '"')) p++;
        if (p < (int)r.body.length()) {
          if (r.body[p] == '0') wan = 0;
          else if (r.body[p] == '1') wan = 1;
        }
      }
    }

    bool dup = false;
    for (auto& d : out) dup |= (d.mac == mac);
    if (!dup) out.push_back(Device{mac, wan});
  }
  return true;
}

bool MiRouter::setInternetAccess(const String& mac, bool allow) {
  if (!loggedIn() && !login()) return false;
  Response r;
  String path = "/cgi-bin/luci/;stok=" + _token + "/api/xqsystem/set_mac_filter?mac=" +
                urlEncode(mac) + "&wan=" + (allow ? "1" : "0");
  if (!request("GET", path, "", r)) return false;
  if (r.raw.indexOf("\"code\":0") >= 0 || r.raw.indexOf("succeeded") >= 0) return true;
  if (r.raw.indexOf("\"code\":401") >= 0 || r.raw.indexOf("Invalid token") >= 0) _token = "";
  _error = "set_mac_filter " + mac + " failed: " + r.raw.substring(0, 120);
  return false;
}
