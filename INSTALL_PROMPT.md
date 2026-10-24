# Prompt for Claude Code on your PC

Plug the ESP32-S3 into your computer with a USB **data** cable (use the port
labelled **COM/UART** if your board has two). Open Claude Code in an empty
folder and paste everything below the line.

---

Install my ESP32-S3 Wi-Fi kill switch firmware from GitHub onto the board that
is plugged into this computer.

1. Clone https://github.com/Marku-a/Wifi-switch- (branch
   `claude/esp32-wifi-router-control-ny6go1`, or `main` if that branch is gone)
   and cd into it.
2. Make sure PlatformIO is installed (`pip install platformio` from PyPI, the
   official source). Python 3 is required; if it's missing, tell me how to
   install it from python.org, and don't use any other source.
3. Copy `include/secrets.example.h` to `include/secrets.h`. Ask me, in ONE
   message, for: my Wi-Fi name (the 2.4 GHz one, not the "5G" one), my Wi-Fi password, my Mi router admin
   password, and the MAC addresses of devices that must never be blocked
   (I can skip that). Fill them in. Never print the passwords back,
   never commit `secrets.h`.
4. Find the ESP32's serial port (`pio device list`). On Windows, if no port
   appears, tell me to install the USB driver (CP210x from silabs.com or
   CH340 from wch-ic.com, whichever matches the chip on my board).
5. Run `pio run -t upload`. If the upload fails to connect, tell me to hold
   BOOT, tap RESET, release BOOT, and retry.
6. Run `pio device monitor` for about 60 seconds and explain the log to me:
   did it join Wi-Fi, log in to the router, and what colour the LED should be?
   If you see "login rejected", the router admin password is wrong; ask me
   again. If the LED doesn't light up, change `RGB_LED_PIN` to 38 and re-flash.
7. Finally, tell me to press the BOOT button once (LED should turn red and a
   phone on Wi-Fi should lose internet), then again (LED back to green).
   Ask me to confirm in the Mi WiFi app that the devices showed "Internet
   access" off after the first press. If the log never shows devices being
   blocked or the LED doesn't change, read the log with me and fix it.
