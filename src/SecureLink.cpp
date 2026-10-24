#include "SecureLink.h"

#include <ArduinoJson.h>
#include <esp_system.h>
#include <time.h>

#include <vector>

#include "Config.h"
#include "mbedtls/base64.h"
#include "mbedtls/bignum.h"
#include "mbedtls/ecdh.h"
#include "mbedtls/ecp.h"
#include "mbedtls/gcm.h"
#include "mbedtls/md.h"
#include "mbedtls/platform_util.h"

namespace {

const uint32_t kChallengeTtlMs = 60000;
const uint32_t kLockoutMs = 10UL * 60UL * 1000UL;
const int kMaxPinFails = 5;
const int64_t kMaxSkewSecs = 300;
const int64_t kMinValidTime = 1700000000;

int rng(void*, unsigned char* buf, size_t len) {
  esp_fill_random(buf, len);
  return 0;
}

String hexEncode(const uint8_t* d, size_t n) {
  static const char* digits = "0123456789abcdef";
  String s;
  s.reserve(n * 2);
  for (size_t i = 0; i < n; i++) {
    s += digits[d[i] >> 4];
    s += digits[d[i] & 15];
  }
  return s;
}

int hexVal(char c) {
  if (c >= '0' && c <= '9') return c - '0';
  if (c >= 'a' && c <= 'f') return c - 'a' + 10;
  if (c >= 'A' && c <= 'F') return c - 'A' + 10;
  return -1;
}

// Decodes exactly `n` bytes from a 2n-char hex string.
bool hexDecode(const String& s, uint8_t* out, size_t n) {
  if (s.length() != n * 2) return false;
  for (size_t i = 0; i < n; i++) {
    int hi = hexVal(s[2 * i]);
    int lo = hexVal(s[2 * i + 1]);
    if (hi < 0 || lo < 0) return false;
    out[i] = (uint8_t)((hi << 4) | lo);
  }
  return true;
}

bool ctEq(const uint8_t* a, const uint8_t* b, size_t n) {
  uint8_t diff = 0;
  for (size_t i = 0; i < n; i++) diff |= a[i] ^ b[i];
  return diff == 0;
}

bool hmac256(const uint8_t* key, size_t keyLen, const uint8_t* msg, size_t msgLen,
             uint8_t out[32]) {
  const mbedtls_md_info_t* info = mbedtls_md_info_from_type(MBEDTLS_MD_SHA256);
  return info && mbedtls_md_hmac(info, key, keyLen, msg, msgLen, out) == 0;
}

bool hmacStr(const uint8_t* key, const String& msg, uint8_t out[32]) {
  return hmac256(key, 32, (const uint8_t*)msg.c_str(), msg.length(), out);
}

bool b64Decode(const char* s, size_t len, uint8_t* out, size_t outCap, size_t* outLen) {
  return mbedtls_base64_decode(out, outCap, outLen, (const unsigned char*)s, len) == 0;
}

String b64Encode(const uint8_t* d, size_t n) {
  size_t cap = 4 * ((n + 2) / 3) + 1;
  std::vector<unsigned char> buf(cap);
  size_t olen = 0;
  if (mbedtls_base64_encode(buf.data(), cap, &olen, d, n) != 0) return String();
  buf[olen] = 0;
  return String((const char*)buf.data());
}

// HMAC(Z, label || appPub || espPub)
bool derive(const uint8_t z[32], const char* label, const uint8_t appPub[65],
            const uint8_t espPub[65], uint8_t out[32]) {
  uint8_t msg[32 + 65 + 65];
  size_t ll = strlen(label);
  if (ll > 32) return false;
  memcpy(msg, label, ll);
  memcpy(msg + ll, appPub, 65);
  memcpy(msg + ll + 65, espPub, 65);
  bool ok = hmac256(z, 32, msg, ll + 130, out);
  mbedtls_platform_zeroize(msg, sizeof(msg));
  return ok;
}

bool safeField(const String& s, size_t maxLen) {
  if (s.length() > maxLen) return false;
  for (size_t i = 0; i < s.length(); i++) {
    if (s[i] == '|') return false;
  }
  return true;
}

}  // namespace

bool SecureLink::clockSynced() { return time(nullptr) > kMinValidTime; }

uint32_t SecureLink::nowSecs() { return (uint32_t)time(nullptr); }

void SecureLink::begin() {
  prefs_.begin("wslink", false);
  memset(keys_, 0, sizeof(keys_));
  used_ = prefs_.getUChar("used", 0) & 0x0F;
  if (prefs_.getBytesLength("keys") == sizeof(keys_)) {
    prefs_.getBytes("keys", keys_, sizeof(keys_));
  } else {
    used_ = 0;
  }
  deviceId_ = prefs_.getString("device_id", "");
  if (deviceId_.length() != 32) {
    uint8_t id[16];
    esp_fill_random(id, sizeof(id));
    deviceId_ = hexEncode(id, sizeof(id));
    prefs_.putString("device_id", deviceId_);
  }
}

int SecureLink::pairedCount() const {
  int n = 0;
  for (int i = 0; i < kSlots; i++) {
    if (slotUsed(i)) n++;
  }
  return n;
}

void SecureLink::saveKeys() {
  prefs_.putBytes("keys", keys_, sizeof(keys_));
  prefs_.putUChar("used", used_);
}

void SecureLink::unpair(int kid) {
  if (kid < 0 || kid >= kSlots) return;
  used_ &= (uint8_t) ~(1 << kid);
  memset(keys_[kid], 0, 32);
  saveKeys();
}

void SecureLink::unpairAll() {
  used_ = 0;
  memset(keys_, 0, sizeof(keys_));
  saveKeys();
}

int SecureLink::checkPairRequest(const String& body, uint8_t appPub[65], String& err) {
  if (locked_) {
    if ((int32_t)(millis() - lockUntil_) < 0) {
      err = "locked";
      return 429;
    }
    locked_ = false;
    pinFails_ = 0;
  }

  JsonDocument doc;
  if (deserializeJson(doc, body) || doc["v"].as<int>() != 1 ||
      !doc["pub"].is<const char*>() || !doc["pin_proof"].is<const char*>()) {
    err = "malformed";
    return 400;
  }
  String pub = doc["pub"].as<String>();
  String proofHex = doc["pin_proof"].as<String>();

  uint8_t raw[96];
  size_t rawLen = 0;
  if (!b64Decode(pub.c_str(), pub.length(), raw, sizeof(raw), &rawLen) || rawLen != 65) {
    err = "bad pub";
    return 400;
  }
  memcpy(appPub, raw, 65);

  uint8_t proof[32];
  uint8_t expect[32];
  bool proofOk = hexDecode(proofHex, proof, 32) &&
                 hmac256((const uint8_t*)APP_PIN, strlen(APP_PIN), appPub, 65, expect) &&
                 ctEq(proof, expect, 32);
  mbedtls_platform_zeroize(expect, sizeof(expect));
  if (!proofOk) {
    if (++pinFails_ >= kMaxPinFails) {
      locked_ = true;
      lockUntil_ = millis() + kLockoutMs;
      pinFails_ = 0;
      Serial.println("[pair] too many wrong PINs, pairing locked for 10 min");
    }
    err = "bad pin";
    return 403;
  }
  pinFails_ = 0;

  mbedtls_ecp_group grp;
  mbedtls_ecp_point pt;
  mbedtls_ecp_group_init(&grp);
  mbedtls_ecp_point_init(&pt);
  bool onCurve = mbedtls_ecp_group_load(&grp, MBEDTLS_ECP_DP_SECP256R1) == 0 &&
                 mbedtls_ecp_point_read_binary(&grp, &pt, appPub, 65) == 0 &&
                 mbedtls_ecp_check_pubkey(&grp, &pt) == 0;
  mbedtls_ecp_point_free(&pt);
  mbedtls_ecp_group_free(&grp);
  if (!onCurve) {
    err = "bad pub";
    return 400;
  }
  return 0;
}

int SecureLink::completePair(const uint8_t appPub[65], String& responseJson, String& err) {
  int kid = -1;
  for (int i = 0; i < kSlots; i++) {
    if (!slotUsed(i)) {
      kid = i;
      break;
    }
  }
  if (kid < 0) {
    err = "4 phones already paired, hold BOOT 10 s to reset";
    return 507;
  }

  mbedtls_ecp_group grp;
  mbedtls_mpi d, zMpi;
  mbedtls_ecp_point q, a;
  mbedtls_ecp_group_init(&grp);
  mbedtls_mpi_init(&d);
  mbedtls_mpi_init(&zMpi);
  mbedtls_ecp_point_init(&q);
  mbedtls_ecp_point_init(&a);

  uint8_t z[32] = {0};
  uint8_t espPub[65] = {0};
  uint8_t kAuth[32] = {0};
  uint8_t kWrap[32] = {0};
  size_t olen = 0;
  int status = 500;
  err = "pairing failed";

  do {
    if (mbedtls_ecp_group_load(&grp, MBEDTLS_ECP_DP_SECP256R1) != 0) break;
    if (mbedtls_ecp_point_read_binary(&grp, &a, appPub, 65) != 0) break;
    if (mbedtls_ecp_check_pubkey(&grp, &a) != 0) break;
    if (mbedtls_ecdh_gen_public(&grp, &d, &q, rng, nullptr) != 0) break;
    if (mbedtls_ecdh_compute_shared(&grp, &zMpi, &a, &d, rng, nullptr) != 0) break;
    if (mbedtls_mpi_write_binary(&zMpi, z, 32) != 0) break;
    if (mbedtls_ecp_point_write_binary(&grp, &q, MBEDTLS_ECP_PF_UNCOMPRESSED, &olen, espPub,
                                       sizeof(espPub)) != 0 ||
        olen != 65)
      break;
    if (!derive(z, "wifiswitch-auth-v1", appPub, espPub, kAuth)) break;
    if (!derive(z, "wifiswitch-wrap-v1", appPub, espPub, kWrap)) break;

    // Box plaintext
    JsonDocument pj;
    pj["device_id"] = deviceId_;
    pj["kid"] = kid;
    pj["mqtt_host"] = String(MQTT_HOST);
    pj["mqtt_port"] = strlen(MQTT_HOST) ? (int)MQTT_PORT : 0;
    pj["mqtt_user"] = strlen(MQTT_HOST) ? String(MQTT_USER) : String("");
    pj["mqtt_pass"] = strlen(MQTT_HOST) ? String(MQTT_PASS) : String("");
    String plain;
    serializeJson(pj, plain);
    size_t plen = plain.length();

    // iv(12) || ciphertext || tag(16)
    std::vector<uint8_t> box(12 + plen + 16);
    esp_fill_random(box.data(), 12);
    mbedtls_gcm_context gcm;
    mbedtls_gcm_init(&gcm);
    int g = mbedtls_gcm_setkey(&gcm, MBEDTLS_CIPHER_ID_AES, kWrap, 256);
    if (g == 0) {
      g = mbedtls_gcm_crypt_and_tag(&gcm, MBEDTLS_GCM_ENCRYPT, plen, box.data(), 12, nullptr, 0,
                                    (const unsigned char*)plain.c_str(), box.data() + 12, 16,
                                    box.data() + 12 + plen);
    }
    mbedtls_gcm_free(&gcm);
    plain = String();
    pj.clear();
    if (g != 0) break;

    memcpy(keys_[kid], kAuth, 32);
    used_ |= (uint8_t)(1 << kid);
    saveKeys();

    responseJson = "{\"v\":1,\"pub\":\"" + b64Encode(espPub, 65) + "\",\"box\":\"" +
                   b64Encode(box.data(), box.size()) + "\"}";
    status = 200;
    err = "";
  } while (false);

  mbedtls_platform_zeroize(z, sizeof(z));
  mbedtls_platform_zeroize(kAuth, sizeof(kAuth));
  mbedtls_platform_zeroize(kWrap, sizeof(kWrap));
  mbedtls_mpi_free(&zMpi);
  mbedtls_mpi_free(&d);
  mbedtls_ecp_point_free(&q);
  mbedtls_ecp_point_free(&a);
  mbedtls_ecp_group_free(&grp);
  if (status == 200) Serial.printf("[pair] phone paired as kid %d\n", kid);
  return status;
}

String SecureLink::newChallenge() {
  uint8_t r[16];
  esp_fill_random(r, sizeof(r));
  challenge_ = hexEncode(r, sizeof(r));
  challengeAt_ = millis();
  haveChallenge_ = true;
  return challenge_;
}

int SecureLink::verifyCommand(const String& json, Command& out) {
  JsonDocument doc;
  if (deserializeJson(doc, json)) return 400;
  if (!doc["v"].is<int>() || doc["v"].as<int>() != 1) return 400;
  if (!doc["kid"].is<int>() || !doc["op"].is<const char*>() ||
      !doc["nonce"].is<const char*>() || !doc["mac"].is<const char*>() ||
      !doc["ts"].is<int64_t>())
    return 400;
  if (!doc["arg"].isNull() && !doc["arg"].is<const char*>()) return 400;

  int kid = doc["kid"].as<int>();
  String op = doc["op"].as<String>();
  String arg = doc["arg"].isNull() ? String("") : doc["arg"].as<String>();
  String nonce = doc["nonce"].as<String>();
  String macHex = doc["mac"].as<String>();
  int64_t ts = doc["ts"].as<int64_t>();

  if (kid < 0 || kid >= kSlots) return 400;
  if (ts < 0 || ts > 0xFFFFFFFFLL) return 400;
  if (!safeField(op, 16) || !safeField(arg, 16) || !safeField(nonce, 64) || nonce.length() == 0)
    return 400;
  if (op == "set") {
    if (arg != "on" && arg != "off") return 400;
  } else if (op == "status" || op == "unpair") {
    if (arg.length() != 0) return 400;
  } else {
    return 400;
  }
  uint8_t mac[32];
  if (!hexDecode(macHex, mac, 32)) return 400;

  if (!slotUsed(kid)) return 401;

  String canonical = "v1|" + String(kid) + "|" + op + "|" + arg + "|" +
                     String((unsigned long)ts) + "|" + nonce;
  uint8_t expect[32];
  if (!hmacStr(keys_[kid], canonical, expect)) return 401;
  bool macOk = ctEq(mac, expect, 32);
  mbedtls_platform_zeroize(expect, sizeof(expect));
  if (!macOk) return 401;

  if (ts == 0) {
    // LAN: single-use challenge, consumed only after a valid MAC.
    bool fresh = haveChallenge_ && (millis() - challengeAt_) <= kChallengeTtlMs &&
                 nonce == challenge_;
    haveChallenge_ = false;
    challenge_ = "";
    if (!fresh) return 409;
  } else {
    if (!clockSynced()) return 409;
    int64_t now = (int64_t)time(nullptr);
    int64_t diff = now - ts;
    if (diff < 0) diff = -diff;
    if (diff > kMaxSkewSecs) return 409;
    for (int i = 0; i < kNonceCache; i++) {
      if (nonces_[i] == nonce) return 409;
    }
    nonces_[nonceNext_] = nonce;
    nonceNext_ = (nonceNext_ + 1) % kNonceCache;
  }

  out.kid = kid;
  out.op = op;
  out.arg = arg;
  out.nonce = nonce;
  out.ts = (uint32_t)ts;
  return 200;
}

String SecureLink::stateJson(bool internetOn, const char* led, bool routerOk, uint32_t ts,
                             const String& echoIn) {
  // The echo goes into a JSON string and the signed text: allow only [0-9A-Za-z], <= 64.
  String echo = echoIn;
  bool echoOk = echo.length() <= 64;
  for (size_t i = 0; echoOk && i < echo.length(); i++) {
    if (!isAlphaNumeric(echo[i])) echoOk = false;
  }
  if (!echoOk) echo = "";

  const char* inet = internetOn ? "on" : "off";
  String canonical = "v1|state|" + deviceId_ + "|" + inet + "|" + led + "|" +
                     (routerOk ? "1" : "0") + "|" + String((unsigned long)ts) + "|" + echo;

  String j = "{\"v\":1,\"internet\":\"";
  j += inet;
  j += "\",\"led\":\"";
  j += led;
  j += "\",\"router_ok\":";
  j += routerOk ? "true" : "false";
  j += ",\"ts\":" + String((unsigned long)ts);
  j += ",\"echo\":\"" + echo + "\",\"macs\":{";
  bool first = true;
  for (int i = 0; i < kSlots; i++) {
    if (!slotUsed(i)) continue;
    uint8_t mac[32];
    if (!hmacStr(keys_[i], canonical, mac)) continue;
    if (!first) j += ",";
    first = false;
    j += "\"" + String(i) + "\":\"" + hexEncode(mac, 32) + "\"";
  }
  j += "}}";
  return j;
}
