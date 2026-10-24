# Security design (protocol v1)

Goal: the phone app can only ask the switch "internet on/off/status". It never
holds the Wi-Fi password, the router admin password, or any credential that
reaches the router or the home network. A stolen phone backup, a sniffed LAN,
or a compromised relay server cannot toggle the switch.

## Threat model and answers

| Threat | Defence |
|---|---|
| Kid / guest on the home Wi-Fi sends commands | Every command is signed with a 256-bit key only paired phones have. |
| Someone sniffs the LAN | Pairing uses ECDH (P-256); the key never travels. Commands are signed, single-use, and expire. |
| Replay of a captured command | LAN: single-use server challenge (60 s). Relay: timestamp ±300 s + nonce cache. |
| Relay (MQTT broker) is hacked or the broker password leaks | Broker only carries signed messages; it can drop them (DoS) but never forge a command or a status. |
| Fake broker / TLS interception | ESP32 and app trust only the Let's Encrypt roots (ISRG Root X1/X2) and check the hostname. |
| Phone stolen / app data copied | Auth key lives in Android Keystore (non-exportable). Other data is AES-GCM-encrypted with a Keystore key. Backups disabled. Screenshots blocked. "Unpair" wipes everything. |
| Someone pairs a new phone | Needs the PIN **and** a physical BOOT-button press on the switch within 30 s. 5 wrong PINs lock pairing for 10 min. |
| Lost phone | Hold BOOT 10 s on the switch: all paired phones are erased. |

Not in scope: someone with physical access to the ESP32 can read its flash
(Wi-Fi/router/broker passwords). Keep it somewhere the kids don't play with it.

## Primitives

- ECDH on **secp256r1**. Public keys travel as the 65-byte uncompressed point
  (`0x04 || X || Y`), base64 (standard alphabet, no line breaks).
- Shared secret `Z` = 32-byte big-endian X coordinate.
- `HMAC(k, m)` = HMAC-SHA256. Hex is lowercase.
- Key derivation (both sides, byte-exact):
  - `K_auth = HMAC(Z, "wifiswitch-auth-v1" || appPub || espPub)` (32 bytes)
  - `K_wrap = HMAC(Z, "wifiswitch-wrap-v1" || appPub || espPub)` (32 bytes)
  where `appPub`, `espPub` are the raw 65-byte points.
- Box = AES-256-GCM with `K_wrap`, random 12-byte IV, 16-byte tag, no AAD.
  Encoded `base64(iv || ciphertext || tag)`.

## Pairing (LAN only, phone must be on home Wi-Fi)

`POST /api/pair` with body
`{"v":1,"pub":"<appPub b64>","pin_proof":"<hex HMAC(UTF8(APP_PIN), appPub)>"}`

ESP32:
1. If locked out (5 bad PINs → 10 min) → `429 {"error":"locked"}`.
2. Verify `pin_proof` in constant time; wrong → count failure, `403 {"error":"bad pin"}`.
3. Check the point is on the curve (`mbedtls_ecp_check_pubkey`); bad → `400`.
4. LED blinks **blue**; wait up to 30 s for a BOOT press (handler blocks).
   No press → `408 {"error":"press the BOOT button on the switch"}`.
5. Generate its own ephemeral P-256 key, compute `Z`, derive keys.
6. Store `K_auth` in the first free slot `kid` 0..3 (NVS). All full → `507 {"error":"4 phones already paired, hold BOOT 10 s to reset"}`.
7. Reply `200 {"v":1,"pub":"<espPub b64>","box":"<Box of JSON below>"}`.

Box plaintext:
`{"device_id":"<32 hex>","kid":<0-3>,"mqtt_host":"...","mqtt_port":8883,"mqtt_user":"...","mqtt_pass":"..."}`
(mqtt fields empty / 0 when the relay is not configured). `device_id` is 16
random bytes generated once at first boot and kept in NVS.

The app derives the same keys, opens the box, imports `K_auth` into the
Android Keystore and forgets `Z`, `K_wrap` and its ECDH private key.

## Command

Canonical string: `v1|<kid>|<op>|<arg>|<ts>|<nonce>`
- `op` = `set` (`arg` = `on` / `off`), `status` (`arg` = empty), or `unpair`
  (`arg` = empty; the ESP32 erases that phone's `kid` slot, then replies)
- LAN: `ts` = `0`, `nonce` = challenge from `GET /api/challenge`
- Relay: `ts` = unix seconds, `nonce` = 16 hex chars (8 random bytes)

JSON: `{"v":1,"kid":0,"op":"set","arg":"off","ts":0,"nonce":"...","mac":"<hex HMAC(K_auth[kid], canonical)>"}`

ESP32 accepts only if `kid` slot is paired, the MAC matches (constant-time),
and either:
- `ts == 0` and `nonce` equals the outstanding LAN challenge (issued < 60 s
  ago, then cleared — single use), or
- clock is NTP-synced, `|now - ts| <= 300`, and `nonce` is not among the last
  64 accepted relay nonces.

## Status

Canonical: `v1|state|<device_id>|<internet on/off>|<led>|<router_ok 0/1>|<ts>|<echo>`

JSON: `{"v":1,"internet":"on","led":"online","router_ok":true,"ts":<unix or 0>,"echo":"...","macs":{"<kid>":"<hex HMAC(K_auth[kid], canonical)>",...}}`
— one MAC per paired phone. The app shows nothing it can't verify.

## LAN transport (plain HTTP, port 80, mDNS `wifiswitch.local`)

- `GET /api/challenge` → `{"nonce":"<32 hex>"}` (new random value each call; replaces the previous one).
- `POST /api/cmd` body = command JSON → status JSON (`echo` = command nonce), or `401 {"error":"bad signature"}` / `409 {"error":"stale"}`.
- `GET /api/state?echo=<16 hex>` → status JSON with that `echo` (unauthenticated read, signed reply: the app checks the echo so old replies can't be replayed).
- `POST /api/pair` as above.

## Relay transport (MQTT over TLS, port 8883)

- Broker: any MQTT broker with a Let's Encrypt certificate, e.g. **HiveMQ Cloud** (free tier). Create one username/password.
- Topics: `wifiswitch/<device_id>/cmd` (app → ESP, QoS 1, not retained) and `wifiswitch/<device_id>/state` (ESP → app, QoS 1, retained; `echo` = empty, `ts` = now).
- ESP publishes state on every change, after every accepted command, and every 5 minutes.
- Last-will on `wifiswitch/<device_id>/online` = `0` (retained), `1` after connect. Unsigned, used only as a hint.
- The app uses the relay only when the LAN switch doesn't answer within 3 s, and rejects relayed status older than 15 minutes.
