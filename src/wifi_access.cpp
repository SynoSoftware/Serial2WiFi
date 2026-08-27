#include "wifi_access.h"

#include <DNSServer.h>
#include <ESPmDNS.h>
#include <Preferences.h>
#include <cstring>
#include <esp_arduino_version.h>
#include <esp_system.h>

namespace wifi_access {
namespace {

constexpr uint32_t kApGraceMs = 10 * 60 * 1000;
constexpr uint32_t kDnsRetryMs = 5000;
constexpr uint32_t kStaRetryMs = 5000;
constexpr uint32_t kStaUnavailableMs = 30000;
constexpr uint8_t kSetupApChannel = 1;
// Keep these credentials compact. The OLED uses a fixed Version 2-L QR at
// 2x scale; do not lengthen the SSID, password, or alphabet without first
// recalculating the WIFI payload capacity and validating the physical layout.
constexpr char kPasswordAlphabet[] = "abcdefghjkmnpqrtuvwxyz2346789";
constexpr size_t kSetupPasswordLength = 8;
constexpr size_t kSetupSsidLength = sizeof("S2W-00") - 1;
constexpr size_t kWifiQrFixedPayloadLength =
    sizeof("WIFI:T:WPA;S:") - 1 + sizeof(";P:") - 1 + sizeof(";;") - 1;
constexpr size_t kQrVersion2LowEccByteCapacity = 32;
static_assert(
    kSetupSsidLength + kSetupPasswordLength + kWifiQrFixedPayloadLength <=
        kQrVersion2LowEccByteCapacity,
    "Setup Wi-Fi credentials exceed the fixed Version 2-L QR capacity. The QR version and dimensions are immutable: do not enlarge it and do not switch to Version 3. Shorten the generated setup SSID until the payload fits Version 2-L.");

Preferences identityPreferences;
DNSServer dnsServer;
char deviceName[33]{};
char devicePassword[17]{};
char mdnsName[33]{};
char mdnsHostName[33]{};
char stationSsid[33]{};

bool apIsActive = false;
bool stationIsConfigured = false;
bool wasStationConnected = false;
uint32_t apCloseAt = 0;
uint32_t dnsRetryAt = 0;
uint32_t stationRetryAt = 0;
uint32_t stationUnavailableAt = 0;
ScanState currentScanState = ScanState::Idle;

bool configuredWifi(const configuration::DeviceConfig &config) {
    return config.ssid[0] != '\0';
}

uint8_t deriveDeviceSuffix(const char *password) {
    const uint64_t chipId = ESP.getEfuseMac();
    // The setup SSID is minted only when a new identity is created. Folding in
    // the UID and the freshly generated password keeps factory-reset identities
    // distinct without recomputing the SSID on later boots.
    uint32_t hash = 2166136261u;
    auto mixByte = [&hash](uint8_t value) {
        hash ^= value;
        hash *= 16777619u;
    };
    for (size_t index = 0; index < sizeof(chipId); ++index) {
        mixByte(static_cast<uint8_t>(chipId >> (index * 8)));
    }
    for (size_t index = 0; password[index] != '\0'; ++index) {
        mixByte(static_cast<uint8_t>(password[index]));
    }
    return static_cast<uint8_t>(hash);
}

void makeDeviceName(const char *password) {
    const uint64_t chipId = ESP.getEfuseMac();
    snprintf(deviceName, sizeof(deviceName), "S2W-%02X",
             static_cast<unsigned>(deriveDeviceSuffix(password)));
    snprintf(mdnsName, sizeof(mdnsName), "serial2wifi-%04lx",
             static_cast<unsigned long>(chipId & 0xFFFF));
    snprintf(mdnsHostName, sizeof(mdnsHostName), "%s.local", mdnsName);
}

bool compactPassword(const String &password) {
    if (password.length() != kSetupPasswordLength)
        return false;
    for (size_t index = 0; index < kSetupPasswordLength; ++index) {
        if (strchr(kPasswordAlphabet, password[index]) == nullptr)
            return false;
    }
    return true;
}

bool validSetupName(const String &name) {
    if (name.length() != kSetupSsidLength)
        return false;
    if (strncmp(name.c_str(), "S2W-", 4) != 0)
        return false;
    for (size_t index = 4; index < kSetupSsidLength; ++index) {
        const char value = name[index];
        const bool decimal = value >= '0' && value <= '9';
        const bool upperHex = value >= 'A' && value <= 'F';
        const bool lowerHex = value >= 'a' && value <= 'f';
        if (!decimal && !upperHex && !lowerHex)
            return false;
    }
    return true;
}

void generatePassword() {
    for (size_t i = 0; i < kSetupPasswordLength; ++i) {
        devicePassword[i] = kPasswordAlphabet[esp_random() % (sizeof(kPasswordAlphabet) - 1)];
    }
    devicePassword[kSetupPasswordLength] = '\0';
}

void persistIdentity() {
    identityPreferences.putString("name", deviceName);
    identityPreferences.putString("pass", devicePassword);
}

void loadIdentity() {
    if (!identityPreferences.begin("s2id", false)) {
        generatePassword();
        makeDeviceName(devicePassword);
        return;
    }

    const String storedName = identityPreferences.getString("name", "");
    const String storedPassword = identityPreferences.getString("pass", "");
    const bool hasName = validSetupName(storedName);
    const bool hasPassword = compactPassword(storedPassword);

    // `name` and `pass` are a paired identity, not editable configuration.
    // If either half is missing or invalid, recreate the whole identity once
    // instead of trying to infer a different SSID from a surviving password.
    if (hasName && hasPassword) {
        storedName.toCharArray(deviceName, sizeof(deviceName));
        storedPassword.toCharArray(devicePassword, sizeof(devicePassword));
        identityPreferences.end();
        return;
    }

    generatePassword();
    makeDeviceName(devicePassword);
    persistIdentity();
    identityPreferences.end();
}

void startCaptiveDns() {
    if (dnsServer.isUp())
        return;
    const uint32_t now = millis();
    if (static_cast<int32_t>(now - dnsRetryAt) < 0)
        return;
    if (!dnsServer.start(53, "*", WiFi.softAPIP())) {
        dnsRetryAt = now + kDnsRetryMs;
    }
}

void startAp() {
    if (apIsActive && WiFi.AP.started())
        return;
    apIsActive = false;
    // Before station credentials exist, setup must be a plain AP. Starting an
    // unused STA interface in that recovery state gives it no product value
    // and makes the setup radio depend on concurrent-mode behavior.
    WiFi.mode(stationIsConfigured ? WIFI_AP_STA : WIFI_AP);
    // Android abandons captive detection before it sends any HTTP probe when
    // the probe hostname resolves to an RFC1918 or link-local address. See
    // NetworkMonitor.hasPrivateIpAddress: 10/8, 172.16/12, 192.168/16 and
    // 169.254/16. The Arduino default AP address sits inside that test, so it
    // loses every Android client no matter what http_server answers. RFC 6598
    // Shared Address Space is outside the test and is reserved for equipment
    // that must not occupy RFC1918. This call is the one place the setup AP
    // address is decided; everything else reads it back from the driver.
    // Never move this AP onto a private range.
    const IPAddress apAddress(100, 64, 0, 1);
    if (!WiFi.softAPConfig(apAddress, apAddress, IPAddress(255, 255, 255, 0)))
        return;
    if (!WiFi.softAP(deviceName, devicePassword, kSetupApChannel, 0, 4))
        return;
    apIsActive = WiFi.AP.started();
    if (!apIsActive)
        return;

    // Do not add WiFi.AP.enableDhcpCaptivePortal() here. It advertises DHCP
    // option 114 (RFC 8910), which promises a captive-portal JSON API that
    // RFC 8908 requires to be served over TLS. This device has no TLS, so
    // Android prefers that API flow, fails it, and captive detection breaks.
    // It was added and reverted three times. Capture works only through the
    // legacy path: the wildcard DNS below plus the HTTP 302 probe responses
    // in http_server.
    // Captive clients ask DNS for their own probe hostnames. Wildcarding only
    // the setup interface sends those probes to the local web UI.
    startCaptiveDns();
}

void stopAp() {
    if (!apIsActive && !WiFi.AP.started())
        return;
    // Close only the setup interface; the STA/LAN connection must remain up.
    dnsServer.stop();
    WiFi.softAPdisconnect(false);
    apIsActive = WiFi.AP.started();
    apCloseAt = 0;
    dnsRetryAt = 0;
    if (stationIsConfigured && !apIsActive)
        WiFi.mode(WIFI_STA);
}

void beginStation(const configuration::DeviceConfig &config) {
    stationIsConfigured = configuredWifi(config);
    strncpy(stationSsid, config.ssid, sizeof(stationSsid) - 1);
    stationSsid[sizeof(stationSsid) - 1] = '\0';
    if (!stationIsConfigured)
        return;
    WiFi.mode(apIsActive ? WIFI_AP_STA : WIFI_STA);
    if (config.wifiSecurity == static_cast<uint8_t>(configuration::WifiSecurity::Open)) {
        WiFi.begin(config.ssid);
    } else {
        WiFi.begin(config.ssid, config.wifiPassword);
    }
    stationRetryAt = millis() + kStaRetryMs;
}

} // namespace

void begin() {
    WiFi.persistent(false);
    WiFi.setAutoReconnect(false);
    loadIdentity();

    const configuration::DeviceConfig config = configuration::snapshot();
    stationIsConfigured = configuredWifi(config);
    // A reboot must leave a reachable configuration path before a healthy
    // station connection has proved itself. Otherwise an unconfigured or
    // disconnected device could lose its only recovery path.
    startAp();
    beginStation(config);
}

void service() {
    const bool connected = stationConnected();
    const uint32_t now = millis();

    // The AP can disappear independently of this state machine. Clear the
    // cached flag before servicing DNS so the display never advertises a QR
    // code for an interface that the Wi-Fi driver no longer has running.
    if (apIsActive && !WiFi.AP.started()) {
        apIsActive = false;
        dnsServer.stop();
        dnsRetryAt = 0;
    }

    if (apIsActive) {
        startCaptiveDns();
        dnsServer.processNextRequest();
    }

    if (!stationIsConfigured) {
        startAp();
        apCloseAt = 0;
        stationUnavailableAt = 0;
        return;
    }

    if (connected) {
        if (!wasStationConnected) {
            if (MDNS.begin(mdnsName))
                MDNS.addService("http", "tcp", 80);
            stationUnavailableAt = 0;
            if (!apIsActive)
                startAp();
            apCloseAt = now + kApGraceMs;
        }
        wasStationConnected = true;
        if (apIsActive && apCloseAt != 0 &&
            static_cast<int32_t>(now - apCloseAt) >= 0) {
            stopAp();
        }
        return;
    }

    if (wasStationConnected) {
        wasStationConnected = false;
        MDNS.end();
        stationUnavailableAt = now;
        apCloseAt = 0;
    }
    if (stationUnavailableAt == 0)
        stationUnavailableAt = now;
    if (static_cast<int32_t>(now - stationUnavailableAt) >= kStaUnavailableMs) {
        startAp();
    }
    if (static_cast<int32_t>(now - stationRetryAt) >= 0) {
        beginStation(configuration::snapshot());
    }
}

bool clearIdentity() {
    if (!identityPreferences.begin("s2id", false))
        return false;
    const bool cleared = identityPreferences.clear();
    identityPreferences.end();
    return cleared;
}

void configurationChanged(const configuration::DeviceConfig &next) {
    stationIsConfigured = configuredWifi(next);
    if (wasStationConnected)
        MDNS.end();
    wasStationConnected = false;
    apCloseAt = 0;
    stationUnavailableAt = 0;
    if (!stationIsConfigured) {
        stationSsid[0] = '\0';
        WiFi.disconnect(false, false);
        startAp();
        return;
    }
    startAp();
    beginStation(next);
}

bool stationConnected() {
    return stationIsConfigured && WiFi.status() == WL_CONNECTED;
}

Snapshot snapshot() {
    Snapshot result{};
    // The OLED's QR must describe the driver-visible AP, never saved intent
    // or an earlier softAP() call whose interface has since disappeared.
    result.setupApActive = WiFi.AP.started();
    result.stationConfigured = stationIsConfigured;
    result.stationConnected = stationConnected();
    result.stationRssi = result.stationConnected ? WiFi.RSSI() : 0;
    strncpy(result.stationSsid, stationSsid, sizeof(result.stationSsid) - 1);
    strncpy(result.setupSsid, deviceName, sizeof(result.setupSsid) - 1);
    strncpy(result.setupPassword, devicePassword, sizeof(result.setupPassword) - 1);
    result.setupIp = WiFi.softAPIP();
    result.stationIp = WiFi.localIP();
    return result;
}

const char *mdnsHost() {
    return mdnsHostName;
}

bool requestFromSetupAp(const WiFiClient &client) {
    return WiFi.AP.started() && client.localIP() == WiFi.softAPIP();
}

bool requestFromLocalInterface(const WiFiClient &client) {
    if (requestFromSetupAp(client))
        return true;
    const IPAddress stationIp = WiFi.localIP();
    return stationIsConfigured && stationIp != IPAddress(0, 0, 0, 0) &&
           client.localIP() == stationIp;
}

void startScan() {
    WiFi.scanDelete();
    WiFi.scanNetworks(true);
    currentScanState = ScanState::Scanning;
}

ScanState scanState() {
    if (currentScanState != ScanState::Scanning)
        return currentScanState;
    const int result = WiFi.scanComplete();
    if (result == WIFI_SCAN_FAILED) {
        currentScanState = ScanState::Failed;
    } else if (result >= 0) {
        currentScanState = ScanState::Ready;
    }
    return currentScanState;
}

size_t copyScanResults(ScanResult *results, size_t capacity) {
    if (scanState() != ScanState::Ready || results == nullptr)
        return 0;
    const int count = WiFi.scanComplete();
    if (count <= 0 || capacity == 0)
        return 0;

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
            if (results[position].rssi <= results[position - 1].rssi)
                break;
            const ScanResult temporary = results[position];
            results[position] = results[position - 1];
            results[position - 1] = temporary;
        }
    }
    return copied;
}

} // namespace wifi_access
