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

// Why the station is not on the network. The radio reports this; without it
// a wrong password and an absent access point look identical to the user.
enum class StationState : uint8_t {
    Unconfigured = 0,
    Connecting,
    Connected,
    BadPassword,
    NotFound,
    Failed,
};

struct Snapshot {
    bool setupApActive;
    bool stationConfigured;
    bool stationConnected;
    StationState stationState;
    // Sticky verdict on the last credentials the user saved. The live state
    // returns to connected or unconfigured once the failed credentials are
    // withdrawn, so without this the reason for the withdrawal is lost before
    // it can be shown.
    StationState provisioningFailure;
    int32_t stationRssi;
    char stationSsid[33];
    char setupSsid[33];
    char setupPassword[17];
    IPAddress setupIp;
    IPAddress stationIp;
};

struct ScanResult {
    char ssid[33];
    int32_t rssi;
    bool secured;
};

    void begin();
    void service();
    // True once, when credentials have been rejected often enough to be judged
    // wrong. The caller withdraws them; they never became a working config.
    bool takeCredentialRejection();
    void clearProvisioningFailure();
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
