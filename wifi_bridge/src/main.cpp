// Flipper Zero Wi-Fi devboard <-> ATS-Mini TCP bridge.
//
// Bytes from the Flipper (UART) go to the ATS-Mini's ad hoc remote TCP port
// and everything the receiver sends goes back to the Flipper unchanged.
//
// The bridge reports its own state to the Flipper as lines starting with '~'
// (the ATS-Mini never starts a line with '~'):
//   ~BRIDGE:READY
//   ~WIFI:CONNECTING
//   ~WIFI:UP <ip>
//   ~TCP:CONNECTING
//   ~TCP:CONNECTED
//   ~TCP:FAILED
//   ~TCP:LOST
// While not connected to the receiver, input from the Flipper is discarded.

#include <Arduino.h>
#include <ESPmDNS.h>
#include <WiFi.h>

#include "config.h"

enum class LinkState { WifiDown, WifiUp, Connected };

static WiFiClient client;
static LinkState state = LinkState::WifiDown;
static uint32_t lastConnectAttempt = 0;
static bool mdnsStarted = false;

static const uint32_t CONNECT_RETRY_MS = 2000;
static const uint32_t CONNECT_TIMEOUT_MS = 3000;

static void report(const char* msg, const char* arg = nullptr) {
    // Leading CRLF ends any partial line so the Flipper parser stays in sync.
    Serial.print("\r\n~");
    Serial.print(msg);
    if(arg) {
        Serial.print(' ');
        Serial.print(arg);
    }
    Serial.print("\r\n");
}

static void discardUartInput() {
    while(Serial.available()) Serial.read();
}

static bool resolveHost(IPAddress& ip) {
    String host = BRIDGE_ATS_HOST;
    if(ip.fromString(host)) return true;

    if(host.endsWith(".local")) {
        if(!mdnsStarted) mdnsStarted = MDNS.begin("flipper-ats-bridge");
        if(!mdnsStarted) return false;
        ip = MDNS.queryHost(host.substring(0, host.length() - 6), 2000);
        return ip != IPAddress((uint32_t)0);
    }
    return WiFi.hostByName(host.c_str(), ip) == 1;
}

void setup() {
    Serial.setRxBufferSize(1024);
    Serial.begin(BRIDGE_UART_BAUD);

    WiFi.mode(WIFI_STA);
    WiFi.setSleep(false); // lower latency for remote-control keypresses
    WiFi.setAutoReconnect(true);
    WiFi.begin(BRIDGE_WIFI_SSID, BRIDGE_WIFI_PASSWORD);

    report("BRIDGE:READY");
    report("WIFI:CONNECTING", BRIDGE_WIFI_SSID);
}

void loop() {
    if(WiFi.status() != WL_CONNECTED) {
        if(state != LinkState::WifiDown) {
            client.stop();
            state = LinkState::WifiDown;
            report("WIFI:CONNECTING", BRIDGE_WIFI_SSID);
        }
        discardUartInput();
        delay(50);
        return;
    }

    if(state == LinkState::WifiDown) {
        state = LinkState::WifiUp;
        lastConnectAttempt = 0;
        report("WIFI:UP", WiFi.localIP().toString().c_str());
    }

    if(state == LinkState::WifiUp) {
        discardUartInput();
        if(lastConnectAttempt && millis() - lastConnectAttempt < CONNECT_RETRY_MS) {
            delay(10);
            return;
        }
        lastConnectAttempt = millis();
        report("TCP:CONNECTING", BRIDGE_ATS_HOST);

        IPAddress ip;
        if(resolveHost(ip) && client.connect(ip, BRIDGE_ATS_PORT, CONNECT_TIMEOUT_MS)) {
            client.setNoDelay(true);
            state = LinkState::Connected;
            discardUartInput(); // drop keypresses queued while disconnected
            report("TCP:CONNECTED");
        } else {
            report("TCP:FAILED");
        }
        return;
    }

    // Connected: pump bytes both ways.
    if(!client.connected()) {
        client.stop();
        state = LinkState::WifiUp;
        lastConnectAttempt = millis();
        report("TCP:LOST");
        return;
    }

    uint8_t buf[256];
    size_t n = Serial.available();
    if(n) {
        n = Serial.readBytes(buf, min(n, sizeof(buf)));
        client.write(buf, n);
    }

    int avail = client.available();
    if(avail > 0) {
        int r = client.read(buf, min((size_t)avail, sizeof(buf)));
        if(r > 0) Serial.write(buf, r);
    }

    if(!n && avail <= 0) delay(1);
}
