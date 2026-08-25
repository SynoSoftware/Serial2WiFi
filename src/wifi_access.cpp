#include "wifi_access.h"

#include <DNSServer.h>
#include <ESPmDNS.h>
#include <Preferences.h>
#include <cstring>
#include <esp_system.h>

namespace wifi_access {
namespace {

constexpr uint32_t kStaRetryMs = 5000;
constexpr char kPasswordAlphabet[] = "abcdefghjkmnpqrtuvwxyz2346789";
constexpr size_t kSetupPasswordLength = 8;

Preferences identityPreferences;
DNSServer dnsServer;
char deviceName[33]{};
char devicePassword[17]{};
char mdnsName[33]{};

bool apIsActive = false;
bool setupApIsEnabled = true;
bool stationIsConfigured = false;
bool wasStationConnected = false;
uint32_t stationRetryAt = 0;
ScanState currentScanState = ScanState::Idle;

bool configuredWifi(const configuration::DeviceConfig &config) {
    return config.ssid[0] != '\0';
}

void makeDeviceName() {
    const uint64_t chipId = ESP.getEfuseMac();
    snprintf(deviceName, sizeof(deviceName), "S2W-%02lX",
        static_cast<unsigned long>(chipId & 0xFF));
    snprintf(mdnsName, sizeof(mdnsName), "serial2wifi-%04lx",
        static_cast<unsigned long>(chipId & 0xFFFF));
}

void generatePassword() {
    for (size_t i = 0; i < kSetupPasswordLength; ++i) {
        devicePassword[i] = kPasswordAlphabet[esp_random() % (sizeof(kPasswordAlphabet) - 1)];
    }
    devicePassword[kSetupPasswordLength] = '\0';
}

bool compactDeviceName(const String &name) {
    if (name.length() != 6 || !name.startsWith("S2W-")) return false;
    for (size_t index = 4; index < 6; ++index) {
        const char value = name[index];
        const bool hexadecimal =
            (value >= '0' && value <= '9') || (value >= 'A' && value <= 'F');
        if (!hexadecimal) return false;
    }
    return true;
}

bool compactPassword(const String &password) {
    if (password.length() != kSetupPasswordLength) return false;
    for (size_t index = 0; index < kSetupPasswordLength; ++index) {
        if (strchr(kPasswordAlphabet, password[index]) == nullptr) return false;
    }
    return true;
}

void loadIdentity() {
    makeDeviceName();
    generatePassword();
    if (!identityPreferences.begin("s2id", false)) return;

    const String storedName = identityPreferences.getString("name", "");
    const String storedPassword = identityPreferences.getString("pass", "");
    // Legacy credentials are too long for the readable setup page and the
    // fixed Version 2 QR layout. Compact credentials remain stable; only an
    // obsolete or invalid value is replaced during boot.
    if (compactDeviceName(storedName)) {
        storedName.toCharArray(deviceName, sizeof(deviceName));
    } else {
        identityPreferences.putString("name", deviceName);
    }

    if (compactPassword(storedPassword)) {
        storedPassword.toCharArray(devicePassword, sizeof(devicePassword));
    } else {
        identityPreferences.putString("pass", devicePassword);
    }
    identityPreferences.end();
}

void startAp() {
    if (apIsActive) return;
    WiFi.mode(WIFI_AP_STA);
    apIsActive = WiFi.softAP(deviceName, devicePassword);
    if (apIsActive) {
        // Captive clients ask DNS for their own probe hostnames. Wildcarding
        // only the setup interface sends those probes to the local web UI.
        dnsServer.start(53, "*", WiFi.softAPIP());
    }
}

void stopAp() {
    if (!apIsActive) return;
    // Close only the setup interface; the STA/LAN connection must remain up.
    dnsServer.stop();
    WiFi.softAPdisconnect(false);
    apIsActive = false;
    if (stationIsConfigured) WiFi.mode(WIFI_STA);
}

void beginStation(const configuration::DeviceConfig &config) {
    stationIsConfigured = configuredWifi(config);
    if (!stationIsConfigured) return;
    WiFi.mode(apIsActive ? WIFI_AP_STA : WIFI_STA);
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
    setupApIsEnabled = configuration::setupApEnabled(config);
    if (!configuredWifi(config) || setupApIsEnabled) startAp();
    beginStation(config);
}

void service() {
    const configuration::DeviceConfig config = configuration::snapshot();
    const bool connected = stationIsConfigured && WiFi.status() == WL_CONNECTED;
    const uint32_t now = millis();

    if (apIsActive) dnsServer.processNextRequest();

    // The persisted setup-AP choice is authoritative. Connectivity changes
    // must not create a timer-driven AP lifecycle behind the user's back.
    if (!stationIsConfigured) {
        startAp();
        return;
    }

    if (setupApIsEnabled) startAp();
    else stopAp();

    if (connected) {
        if (!wasStationConnected) {
            if (MDNS.begin(mdnsName)) MDNS.addService("http", "tcp", 80);
        }
        wasStationConnected = true;
        return;
    }

    if (wasStationConnected) {
        wasStationConnected = false;
        MDNS.end();
    }
    if (static_cast<int32_t>(now - stationRetryAt) >= 0) {
        beginStation(config);
    }
}

void configurationChanged(const configuration::DeviceConfig &next) {
    stationIsConfigured = configuredWifi(next);
    setupApIsEnabled = configuration::setupApEnabled(next);
    if (wasStationConnected) MDNS.end();
    wasStationConnected = false;
    if (!stationIsConfigured) {
        WiFi.disconnect(false, false);
        startAp();
        return;
    }
    if (setupApIsEnabled) startAp();
    else stopAp();
    beginStation(next);
}

void setSetupApEnabled(bool enabled) {
    setupApIsEnabled = enabled;
    if (!stationIsConfigured || enabled) startAp();
    else stopAp();
}

Snapshot snapshot() {
    Snapshot result{};
    result.apActive = apIsActive;
    result.stationConfigured = stationIsConfigured;
    result.stationConnected = stationIsConfigured && WiFi.status() == WL_CONNECTED;
    strncpy(result.setupSsid, deviceName, sizeof(result.setupSsid) - 1);
    strncpy(result.setupPassword, devicePassword, sizeof(result.setupPassword) - 1);
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
