#pragma once

#include <WiFi.h>

#include "configuration.h"

namespace wifi_access {

enum class ScanState : uint8_t {
    Idle = 0,
    Scanning,
    Ready,
    Failed,
};

struct Snapshot {
    bool apActive;
    bool stationConfigured;
    bool stationConnected;
    char setupSsid[33];
    char setupPassword[17];
    IPAddress apIp;
    IPAddress stationIp;
};

struct ScanResult {
    char ssid[33];
    int32_t rssi;
    bool secured;
};

void begin();
void service();
void configurationChanged(const configuration::DeviceConfig &next);
Snapshot snapshot();
bool requestFromSetupAp(const WiFiClient &client);

void startScan();
ScanState scanState();
size_t copyScanResults(ScanResult *results, size_t capacity);

}  // namespace wifi_access
