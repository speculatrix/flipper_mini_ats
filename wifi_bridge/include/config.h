#pragma once

// Wi-Fi network the bridge joins.
// Default: the ATS-Mini's own access point (Settings -> Wi-Fi -> AP Only),
// which is an open network called "ATS-Mini".
#define BRIDGE_WIFI_SSID "ATS-Mini"
#define BRIDGE_WIFI_PASSWORD "" // empty = open network

// Address of the ATS-Mini. In AP mode the receiver is always 10.1.1.1.
// If both are on your home Wi-Fi, use "atsmini.local" (resolved via mDNS)
// or the receiver's IP address.
#define BRIDGE_ATS_HOST "10.1.1.1"
#define BRIDGE_ATS_PORT 60000

// UART to the Flipper. Must match the Flipper app.
#define BRIDGE_UART_BAUD 115200
