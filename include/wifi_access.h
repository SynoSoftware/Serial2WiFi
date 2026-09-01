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

// How an attempt on the station ended. The driver names the phase that failed
// and never the cause: no station-side reason code means "wrong password", so
// this vocabulary does not claim one. Execution and configuration are not
// outcomes; Snapshot answers those with stationConnected and stationConfigured.
enum class ConnectionOutcome : uint8_t {
    None = 0,       // nothing attempted since boot
    Connected,
    SecurityMismatch,
    NotFound,
    AuthFailed,
    CouldNotConnect,
    CouldNotSave,   // trial only: joined the network, commit() failed
};

// Candidate credentials for a trial. Credentials only, never a whole
// DeviceConfig: an attempt can last a minute, and committing a snapshot taken
// at the start would silently revert any unrelated setting changed meanwhile.
struct WifiCredentials {
    char ssid[33];
    char password[65];
    uint8_t security;
};

struct TrialStatus {
    // The verdict of the last trial. It outlives the page that started the
    // trial, because the phone's link usually breaks during the attempt.
    ConnectionOutcome outcome;
    bool running;
    char ssid[33];
    // Signal at the moment the attempt failed, from the disconnect event.
    int32_t rssi;
};

struct Snapshot {
    bool setupApActive;
    bool stationConfigured;
    bool stationConnected;
    ConnectionOutcome stationOutcome;
    int32_t stationRssi;
    char stationSsid[33];
    char setupSsid[33];
    char setupPassword[17];
    uint8_t setupClients;
    IPAddress setupIp;
    IPAddress stationIp;
};

struct ScanResult {
    char ssid[33];
    int32_t rssi;
    bool secured;
};

    // A proved connection is committed from here, so this module needs the
    // same apply callback the HTTP configuration path uses.
    void begin(configuration::ApplyCallback apply);
    void service();
    bool clearIdentity();
    void configurationChanged(const configuration::DeviceConfig &next);
    Snapshot snapshot();
    bool stationConnected();
    const char *mdnsHost();
    bool requestFromSetupAp(const WiFiClient &client);
    bool requestFromLocalInterface(const WiFiClient &client);

    // Nothing is stored until it has worked: beginTrial stages a candidate, and
    // only a connection that completes is committed. The radio work starts on a
    // later service() pass, after the HTTP response the attempt may cut off.
    void beginTrial(const WifiCredentials &candidate);
    // Cancels a running trial, and dismisses a finished one's verdict. Both are
    // the same request: this trial no longer interests the user.
    void cancelTrial();
    bool trialRunning();
    TrialStatus trialStatus();

void startScan();
ScanState scanState();
size_t copyScanResults(ScanResult *results, size_t capacity);

}  // namespace wifi_access
