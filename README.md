# Wi-Fi Kill Switch (ESP32-S3 + Xiaomi Mi Router AX1800)

A one-button remote for the home internet. Plug the ESP32-S3 into any USB
charger. Press the **BOOT** button to turn the household internet **off** or
back **on**.

"Off" blocks internet access on the router for every device, except this ESP32
and the phones you put on the allow-list. Wi-Fi itself stays up. The switch
needs Wi-Fi to talk to the router, so if it turned Wi-Fi off it could never
turn it back on.

## LED meaning (built-in RGB LED)

| LED | Meaning |
|-----|---------|
| 🟢 green | Internet is ON and working |
| 🔴 red, solid | Internet is OFF (you switched it off) |
| 🔴 red, blinking | Internet is ON, but the router has no internet (ISP/modem problem) |
| 🟡 yellow, solid | Problem: can't join Wi-Fi, router is off, or the router admin password is wrong |
| 🟡 yellow, blinking | Working: starting up or applying a change |

## How it works

1. On power-up it joins your Wi-Fi and logs in to the router's local admin API
   (`http://192.168.31.1`), the same API the Mi WiFi web page uses.
2. It restores the last state (ON/OFF is saved, so it survives unplugging).
3. When OFF it calls `xqsystem/set_mac_filter?wan=0` for every connected
   device. Every minute it re-checks, so a device that joins later gets blocked too.
4. When ON it un-blocks every device it blocked (`wan=1`).
5. Every 30 s it checks the router and the internet
   (`connectivitycheck.gstatic.com/generate_204`) and updates the LED.

Blocking is per device, the same as the **"Internet access"** toggle per device
in the Mi WiFi app. You can see it working there.

## Setup

1. `cp include/secrets.example.h include/secrets.h` and fill in:
   - `WIFI_SSID` / `WIFI_PASSWORD`: your Wi-Fi network (2.4 GHz)
   - `ROUTER_ADMIN_PASSWORD`: the password you use at http://miwifi.com
   - `ALLOWLIST_MACS`: devices that must never be blocked (your phone, your
     wife's phone, work laptop…). Turn off "private/random MAC address" for the
     home network on those phones so their MAC stays the same.
   - `RGB_LED_PIN`: 48 for DevKitC-1 v1.0, 38 for v1.1. If the LED never
     lights up, try the other one.
2. Flash: `pio run -t upload`, then `pio device monitor` to watch the log.

`include/secrets.h` is in `.gitignore`; never commit it.

## Install from your PC with Claude Code

Open Claude Code on the computer the ESP32 is plugged into and paste the
prompt in [`INSTALL_PROMPT.md`](INSTALL_PROMPT.md).

## Phone app (Android)

On your phone, open this repo's **Releases** page and download the latest
`WifiSwitch.apk`, then open it to install (allow "install unknown apps").
In the app's settings enter the switch address (`wifiswitch.local`, or the ESP's
IP from the serial log / the Mi WiFi app device list) and your `APP_PIN`, then
Save, or tap **Find switch automatically**. It works while the phone is on the
home Wi-Fi (put your phone on `ALLOWLIST_MACS` so it keeps internet while the
switch is OFF; the app works on the LAN either way).

HTTP API (LAN only, PIN required via `X-Pin` header or `?pin=`):

```
curl -H "X-Pin: 1234" http://wifiswitch.local/api/status
curl -X POST -H "X-Pin: 1234" "http://wifiswitch.local/api/set?state=off"   # or state=on
curl -X POST -H "X-Pin: 1234" http://wifiswitch.local/api/toggle
# -> {"internet":"on","led":"online","router_ok":true,"ip":"192.168.31.50","version":1}
```

## Notes / limits

- The ESP32 only sees 2.4 GHz Wi-Fi. That's fine: the AX1800 has a 2.4 GHz band.
- Anyone who knows the router admin password can undo the block in the Mi WiFi
  app. That cuts both ways: if the ESP32 is broken or lost, you can always
  restore internet from the app.
- The router lists only connected devices. A device that was blocked and is
  away when you switch ON again stays in the ESP's memory and is unblocked
  anyway.
