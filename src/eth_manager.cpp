#include "eth_manager.h"
#include <ETH.h>
#include <WiFi.h>
#include <Arduino.h>

#ifndef WIFI_SSID
#define WIFI_SSID ""
#endif

#ifndef WIFI_PASS
#define WIFI_PASS ""
#endif

// ─────────────────────────────────────────────
//  State
// ─────────────────────────────────────────────
static volatile bool eth_connected = false;
static volatile bool wifi_connected = false;
static volatile bool event_hooked = false;
static volatile bool ethernet_started = false;
static NetworkMode active_mode = NETWORK_MODE_ETH;
static String wifi_ssid = WIFI_SSID;
static String wifi_password = WIFI_PASS;
static IPAddress eth_ip_address;
static IPAddress wifi_ip_address;
static const uint32_t NETWORK_SWITCH_TIMEOUT_MS = 10000;
static const uint32_t NETWORK_SWITCH_POLL_MS = 100;

static bool hasWifiCredentials() {
    return wifi_ssid.length() > 0;
}

static bool hasValidIp(const IPAddress& address) {
    return address != INADDR_NONE && address[0] != 0;
}

static bool activeModeHasIp() {
    if (active_mode == NETWORK_MODE_WIFI) {
        if (wifi_connected && hasValidIp(wifi_ip_address)) {
            return true;
        }
        IPAddress current = WiFi.localIP();
        return current[0] != 0;
    }

    if (eth_connected && hasValidIp(eth_ip_address)) {
        return true;
    }
    IPAddress current = ETH.localIP();
    return current[0] != 0;
}

static bool waitForActiveNetwork(uint32_t timeout_ms) {
    uint32_t start_ms = millis();
    while (millis() - start_ms < timeout_ms) {
        if (isNetworkConnected() && activeModeHasIp()) {
            return true;
        }
        delay(NETWORK_SWITCH_POLL_MS);
    }
    return isNetworkConnected() && activeModeHasIp();
}

static void stopEthernet() {
    // ETH.stop() is not available on this Arduino-ESP32 version.
    // We switch active mode in software and keep event state tracking.
    eth_connected = false;
    eth_ip_address = INADDR_NONE;
}

static void stopWifi() {
    WiFi.disconnect(true, true);
    WiFi.mode(WIFI_MODE_NULL);
    wifi_connected = false;
    wifi_ip_address = INADDR_NONE;
}

static void startEthernet() {
    stopWifi();
    if (ethernet_started) {
        Serial.println("NET: Ethernet already started");
        return;
    }

    Serial.println("NET: starting Ethernet");
    ETH.begin(ETH_PHY_ADDR, ETH_PHY_POWER, ETH_PHY_MDC, ETH_PHY_MDIO,
              ETH_PHY_LAN8720, ETH_CLK_MODE);
    ethernet_started = true;
    //Serial.println("ETH: Initialising...");
}

static bool startWifi() {
    if (!hasWifiCredentials()) {
        //Serial.println("WIFI: Credentials not configured (WIFI_SSID/WIFI_PASS)");
        return false;
    }

    stopEthernet();
    Serial.print("NET: starting WiFi for SSID '");
    Serial.print(wifi_ssid);
    Serial.println("'");
    WiFi.mode(WIFI_MODE_STA);
    WiFi.begin(wifi_ssid.c_str(), wifi_password.c_str());
    //Serial.printf("WIFI: Connecting to %s\n", wifi_ssid.c_str());
    return true;
}

// ─────────────────────────────────────────────
//  Event handler
// ─────────────────────────────────────────────
static void onEthEvent(WiFiEvent_t event) {
    switch (event) {
        case ARDUINO_EVENT_ETH_START:
            //Serial.print("MAC: ");
            //Serial.println(ETH.macAddress());
            break;

        case ARDUINO_EVENT_ETH_CONNECTED:
            Serial.println("NET: Ethernet link up");
            break;

        case ARDUINO_EVENT_ETH_GOT_IP:
            eth_connected = true;
            eth_ip_address = ETH.localIP();
            Serial.print("NET: Ethernet IP ");
            Serial.println(eth_ip_address);
            break;

        case ARDUINO_EVENT_ETH_DISCONNECTED:
            Serial.println("NET: Ethernet disconnected");
            eth_connected = false;
            eth_ip_address = INADDR_NONE;
            break;

        case ARDUINO_EVENT_ETH_STOP:
            Serial.println("NET: Ethernet stopped");
            eth_connected = false;
            eth_ip_address = INADDR_NONE;
            break;

        case ARDUINO_EVENT_WIFI_STA_START:
            //Serial.println("WIFI: Started");
            break;

        case ARDUINO_EVENT_WIFI_STA_CONNECTED:
            Serial.println("NET: WiFi connected");
            break;

        case ARDUINO_EVENT_WIFI_STA_GOT_IP:
            wifi_connected = true;
            wifi_ip_address = WiFi.localIP();
            Serial.print("NET: WiFi IP ");
            Serial.println(wifi_ip_address);
            break;

        case ARDUINO_EVENT_WIFI_STA_DISCONNECTED:
            Serial.println("NET: WiFi disconnected");
            wifi_connected = false;
            wifi_ip_address = INADDR_NONE;
            break;

        default:
            break;
    }
}

// ─────────────────────────────────────────────
//  Public API
// ─────────────────────────────────────────────
void initNetwork() {
    if (!event_hooked) {
        WiFi.onEvent(onEthEvent);
        event_hooked = true;
    }

    setNetworkMode(active_mode);
}

bool setNetworkMode(NetworkMode mode) {
    Serial.print("NET: switching mode to ");
    Serial.println(mode == NETWORK_MODE_WIFI ? "wifi" : "eth");

    if (active_mode == mode && activeModeHasIp()) {
        Serial.println("NET: requested mode already active");
        return true;
    }

    active_mode = mode;

    if (mode == NETWORK_MODE_WIFI) {
        if (!startWifi()) {
            active_mode = NETWORK_MODE_ETH;
            startEthernet();
            return false;
        }
        bool ready = waitForActiveNetwork(NETWORK_SWITCH_TIMEOUT_MS);
        Serial.print("NET: wifi switch result connected=");
        Serial.print(isNetworkConnected() ? "true" : "false");
        Serial.print(" ip=");
        Serial.print(getNetworkIpAddress());
        Serial.print(" ready=");
        Serial.println(ready ? "true" : "false");
        return true;
    }

    startEthernet();
    bool ready = waitForActiveNetwork(NETWORK_SWITCH_TIMEOUT_MS);
    Serial.print("NET: eth switch result connected=");
    Serial.print(isNetworkConnected() ? "true" : "false");
    Serial.print(" ip=");
    Serial.print(getNetworkIpAddress());
    Serial.print(" ready=");
    Serial.println(ready ? "true" : "false");
    return true;
}

NetworkMode getNetworkMode() {
    return active_mode;
}

const char* getNetworkModeString() {
    return active_mode == NETWORK_MODE_WIFI ? "wifi" : "eth";
}

bool isNetworkConnected() {
    if (active_mode == NETWORK_MODE_WIFI) {
        return wifi_connected;
    }
    return eth_connected;
}

String getNetworkIpAddress() {
    if (active_mode == NETWORK_MODE_WIFI) {
        if (hasValidIp(wifi_ip_address)) {
            return wifi_ip_address.toString();
        }
        return WiFi.localIP().toString();
    }
    if (hasValidIp(eth_ip_address)) {
        return eth_ip_address.toString();
    }
    return ETH.localIP().toString();
}

String getNetworkMacAddress() {
    if (active_mode == NETWORK_MODE_WIFI) {
        return WiFi.macAddress();
    }
    return ETH.macAddress();
}

bool setWifiCredentials(const String& ssid, const String& password) {
    wifi_ssid = ssid;
    wifi_password = password;

    if (active_mode == NETWORK_MODE_WIFI) {
        return startWifi();
    }

    return true;
}

String getWifiSsid() {
    return wifi_ssid;
}

String getWifiPassword() {
    return wifi_password;
}

void initEthernet() {
    active_mode = NETWORK_MODE_ETH;
    initNetwork();
}

bool isEthernetConnected() {
    return active_mode == NETWORK_MODE_ETH && eth_connected;
}
