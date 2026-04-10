#pragma once

#include <Arduino.h>

enum NetworkMode {
	NETWORK_MODE_ETH = 0,
	NETWORK_MODE_WIFI = 1,
};

void initNetwork();
bool setNetworkMode(NetworkMode mode);
NetworkMode getNetworkMode();
const char* getNetworkModeString();

bool isNetworkConnected();
String getNetworkIpAddress();
String getNetworkMacAddress();
bool setWifiCredentials(const String& ssid, const String& password);
String getWifiSsid();
String getWifiPassword();

// Backward compatibility wrappers.
void initEthernet();
bool isEthernetConnected();
