#include "wifi_access.h"

#include <ESPmDNS.h>
#include <Preferences.h>
#include <esp_system.h>

namespace wifi_access {
namespace {

constexpr uint32_t kApGraceMs = 60000;
constexpr uint32_t kStaUnavailableMs = 30000;
constexpr uint32_t kStaRetryMs = 5000;
constexpr char kPasswordAlphabet[] = "23456789ABCDEFGHJKLMNPQRSTUVWXYZ";

Preferences identityPreferences;
char deviceName[33]{};
char devicePassword[17]{};
char mdnsName[33]{};

bool apIsActive = false;
bool stationIsConfigured = false;
bool wasStationConnected = false;
uint32_t apCloseAt = 0;
uint32_t stationUnavailableAt = 0;
uint32_t stationRetryAt = 0;
ScanState currentScanState = ScanState::Idle;

bool configuredWifi(const configuration::DeviceConfig &config) {
    return config.ssid[0] != '\0';
}

void makeDeviceName() {
    const uint64_t chipId = ESP.getEfuseMac();
    snprintf(deviceName, sizeof(deviceName), "Serial2WiFi-%04lX",
        static_cast<unsigned long>(chipId & 0xFFFF));
    snprintf(mdnsName, sizeof(mdnsName), "serial2wifi-%04lx",
        static_cast<unsigned long>(chipId & 0xFFFF));
}

void generatePassword() {
    for (size_t i = 0; i < 16; ++i) {
        devicePassword[i] = kPasswordAlphabet[esp_random() % (sizeof(kPasswordAlphabet) - 1)];
    }
    devicePassword[16] = '\0';
}

void loadIdentity() {
    makeDeviceName();
    generatePassword();
    if (!identityPreferences.begin("s2id", false)) return;

    const String storedName = identityPreferences.getString("name", "");
    const String storedPassword = identityPreferences.getString("pass", "");
    if (storedName.length() != 0) storedName.toCharArray(deviceName, sizeof(deviceName));
    if (storedPassword.length() == 16) storedPassword.toCharArray(devicePassword, sizeof(devicePassword));
    else identityPreferences.putString("pass", devicePassword);
    identityPreferences.putString("name", deviceName);
    identityPreferences.end();
}

void startAp() {
    if (apIsActive) return;
    WiFi.mode(WIFI_AP_STA);
    apIsActive = WiFi.softAP(deviceName, devicePassword);
    apCloseAt = 0;
}

void stopAp() {
    if (!apIsActive) return;
    // Close only the setup interface; the STA/LAN connection must remain up.
    WiFi.softAPdisconnect(false);
    apIsActive = false;
    apCloseAt = 0;
    if (stationIsConfigured) WiFi.mode(WIFI_STA);
}

void beginStation(const configuration::DeviceConfig &config) {
    stationIsConfigured = configuredWifi(config);
    if (!stationIsConfigured) return;
    WiFi.mode(WIFI_AP_STA);
    if (config.wifiSecurity == static_cast<uint8_t>(configuration::WifiSecurity::Open)) {
        WiFi.begin(config.ssid);
    } else {
        WiFi.begin(config.ssid, config.wifiPassword);
    }
    stationRetryAt = millis() + kStaRetryMs;
}

}  // namespace

void begin() {
    WiFi.persistent(false);
    WiFi.setAutoReconnect(false);
    WiFi.mode(WIFI_AP_STA);
    loadIdentity();

    const configuration::DeviceConfig config = configuration::snapshot();
    startAp();
    beginStation(config);
    if (stationIsConfigured) apCloseAt = millis() + kApGraceMs;
}

void service() {
    const configuration::DeviceConfig config = configuration::snapshot();
    const bool connected = stationIsConfigured && WiFi.status() == WL_CONNECTED;
    const uint32_t now = millis();

    if (!stationIsConfigured) {
        startAp();
        return;
    }

    if (connected) {
        if (!wasStationConnected) {
            apCloseAt = now + kApGraceMs;
            if (MDNS.begin(mdnsName)) MDNS.addService("http", "tcp", 80);
        }
        wasStationConnected = true;
        stationUnavailableAt = 0;
        if (apIsActive && apCloseAt != 0 && static_cast<int32_t>(now - apCloseAt) >= 0) {
            stopAp();
        }
        return;
    }

    if (wasStationConnected) {
        wasStationConnected = false;
        MDNS.end();
    }
    if (stationUnavailableAt == 0) stationUnavailableAt = now;
    if (static_cast<int32_t>(now - stationRetryAt) >= 0) {
        beginStation(config);
    }
    if (now - stationUnavailableAt >= kStaUnavailableMs) {
        startAp();
        apCloseAt = 0;
    }
}

void configurationChanged(const configuration::DeviceConfig &next) {
    stationIsConfigured = configuredWifi(next);
    stationUnavailableAt = 0;
    if (wasStationConnected) MDNS.end();
    wasStationConnected = false;
    if (stationIsConfigured) {
        startAp();
        apCloseAt = millis() + kApGraceMs;
        beginStation(next);
    } else {
        WiFi.disconnect(false, false);
        startAp();
        apCloseAt = 0;
    }
}

Snapshot snapshot() {
    Snapshot result{};
    result.apActive = apIsActive;
    result.stationConfigured = stationIsConfigured;
    result.stationConnected = stationIsConfigured && WiFi.status() == WL_CONNECTED;
    strncpy(result.setupSsid, deviceName, sizeof(result.setupSsid) - 1);
    strncpy(result.setupPassword, devicePassword, sizeof(result.setupPassword) - 1);
    result.apIp = WiFi.softAPIP();
    result.stationIp = WiFi.localIP();
    return result;
}

bool requestFromSetupAp(const WiFiClient &client) {
    return apIsActive && client.localIP() == WiFi.softAPIP();
}

void startScan() {
    WiFi.scanDelete();
    WiFi.scanNetworks(true);
    currentScanState = ScanState::Scanning;
}

ScanState scanState() {
    if (currentScanState != ScanState::Scanning) return currentScanState;
    const int result = WiFi.scanComplete();
    if (result == WIFI_SCAN_FAILED) {
        currentScanState = ScanState::Failed;
    } else if (result >= 0) {
        currentScanState = ScanState::Ready;
    }
    return currentScanState;
}

size_t copyScanResults(ScanResult *results, size_t capacity) {
    if (scanState() != ScanState::Ready || results == nullptr) return 0;
    const int count = WiFi.scanComplete();
    if (count <= 0 || capacity == 0) return 0;

    size_t copied = 0;
    for (int scanIndex = 0; scanIndex < count; ++scanIndex) {
        ScanResult candidate{};
        WiFi.SSID(scanIndex).toCharArray(candidate.ssid, sizeof(candidate.ssid));
        candidate.rssi = WiFi.RSSI(scanIndex);
        candidate.secured = WiFi.encryptionType(scanIndex) != WIFI_AUTH_OPEN;
        if (copied < capacity) {
            results[copied++] = candidate;
        } else if (candidate.rssi <= results[copied - 1].rssi) {
            continue;
        } else {
            results[copied - 1] = candidate;
        }

        for (size_t position = copied - 1; position > 0; --position) {
            if (results[position].rssi <= results[position - 1].rssi) break;
            const ScanResult temporary = results[position];
            results[position] = results[position - 1];
            results[position - 1] = temporary;
        }
    }
    return copied;
}

}  // namespace wifi_access
