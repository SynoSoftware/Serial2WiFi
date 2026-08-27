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
    bool setupApActive;
    bool stationConfigured;
    bool stationConnected;
    int32_t stationRssi;
    char stationSsid[33];
    char setupSsid[33];
    char setupPassword[17];
    IPAddress stationIp;
};

struct ScanResult {
    char ssid[33];
    int32_t rssi;
    bool secured;
};

    void begin();
    void service();
    bool clearIdentity();
    void configurationChanged(const configuration::DeviceConfig &next);
    Snapshot snapshot();
    bool stationConnected();
    const char *mdnsHost();
    bool requestFromSetupAp(const WiFiClient &client);
    bool requestFromLocalInterface(const WiFiClient &client);

void startScan();
ScanState scanState();
size_t copyScanResults(ScanResult *results, size_t capacity);

}  // namespace wifi_access
