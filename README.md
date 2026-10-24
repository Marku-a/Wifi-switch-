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
| 🔴 red, solid | Internet is OFF: every device is blocked (green again as soon as any device is unblocked) |
| 🔴 red, blinking | Internet is ON, but the router has no internet (ISP/modem problem) |
| 🟡 yellow, solid | Problem: can't join Wi-Fi, router is off, or the router admin password is wrong |
| 🟡 yellow, blinking | Working: starting up or applying a change |

## How it works

1. On power-up it joins your Wi-Fi and logs in to the router's local admin API
   (`http://192.168.31.1`), the same API the Mi WiFi web page uses.
2. The router is the source of truth. The ESP32 stores no ON/OFF state; it reads
   each device's "Internet access" flag from the router every 30 s. The button
   and the Mi WiFi app never fight each other.
3. Button press: if any device has internet, it blocks every device
   (`xqsystem/set_mac_filter?wan=0`). If all are blocked, it un-blocks them
   (`wan=1`), including devices it blocked earlier that are now offline.
4. While everything is OFF, a brand-new device that joins gets blocked too.
   A device you unblock in the Mi WiFi app is respected and left alone.
5. Every 30 s it also checks the internet
   (`connectivitycheck.gstatic.com/generate_204`) and updates the LED.

Blocking is per device, the same as the **"Internet access"** toggle per device
in the Mi WiFi app. You can see it working there.

## Turn it off from outside the house (Mi WiFi app)

1. Install the official **Mi WiFi** app (Google Play / App Store, publisher
   Xiaomi) and sign in with the Xiaomi account the router is bound to. The app
   shows the router when you're away only if it was bound in the app, so do
   this once at home: Mi WiFi app, add/bind router.
2. Turn internet OFF for a device: open the app, go to **Devices** (device
   list), tap the device, and switch **Internet access** (may be called
   "Network access") off. Repeat for each kid's device.
3. Turn it back ON the same way. The ESP32's LED shows green as soon as any
   device is unblocked, and red when all are blocked (updates within 30 s).
4. Tip: give kids' devices recognisable names in the app (tap device, rename)
   so they're easy to find. Labels can differ slightly between app versions.

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

## Notes / limits

- The ESP32 only sees 2.4 GHz Wi-Fi. That's fine: the AX1800 has a 2.4 GHz band.
- Anyone who knows the router admin password can undo the block in the Mi WiFi
  app. That cuts both ways: if the ESP32 is broken or lost, you can always
  restore internet from the app.
- The router lists only connected devices. A device that was blocked and is
  away when you switch ON again stays in the ESP's memory and is unblocked
  anyway.
