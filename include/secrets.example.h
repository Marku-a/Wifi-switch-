// Copy this file to include/secrets.h and fill in your values.
// secrets.h is git-ignored, so your passwords never get pushed to GitHub.
#pragma once

// Your home Wi-Fi (the ESP32 connects to it like any other device).
// ESP32 only supports 2.4 GHz. The AX1800 broadcasts 2.4 GHz by default.
#define WIFI_SSID      "MyHomeWiFi"
#define WIFI_PASSWORD  "wifi-password"

// Mi Router admin page (default http://192.168.31.1) and its admin password
// (the one you type at http://miwifi.com, often the same as the Wi-Fi password).
#define ROUTER_IP              "192.168.31.1"
#define ROUTER_ADMIN_PASSWORD  "router-admin-password"

// Devices that are NEVER blocked (e.g. parents' phones), comma separated.
// Use the MAC shown in the Mi WiFi app / router page for that device.
// Tip: on phones, turn off "private / random MAC" for your home network
// so the MAC stays the same.
#define ALLOWLIST_MACS  ""   // e.g. "AA:BB:CC:DD:EE:01,AA:BB:CC:DD:EE:02"

// PIN the phone app must send. Change it so kids can't toggle from their devices.
#define APP_PIN  "1234"

// Built-in RGB LED pin. ESP32-S3-DevKitC-1 v1.0 = 48, v1.1 = 38.
#define RGB_LED_PIN     48
#define LED_BRIGHTNESS  40   // 0..255
