#include "wifi_access.h"

#include <DNSServer.h>
#include <ESPmDNS.h>
#include <Preferences.h>
#include <cstring>
#include <esp_arduino_version.h>
#include <esp_system.h>

namespace wifi_access {
namespace {

// The radio delivers a disconnect reason and nothing was listening for it, so
// every failure looked the same from the outside. Written from the Wi-Fi event
// task and read from the main loop, hence volatile.
volatile uint8_t lastDisconnectReason = 0;
volatile int8_t lastDisconnectRssi = 0;
volatile bool sawDisconnect = false;
volatile bool sawGotIp = false;

constexpr uint32_t kApGraceMs = 10 * 60 * 1000;
constexpr uint32_t kDnsRetryMs = 5000;
constexpr uint32_t kStaUnavailableMs = 30000;
// While the station is down the device runs one scan-then-attempt cycle on a
// single backoff, never both at once: WiFi.begin() aborts a scan in flight, and
// a blind retry every few seconds clobbered every scan the user asked for, so
// the network list came back empty with no error and no way out. The scan sets
// the timing of the attempt, so the two can never compete. The interval doubles
// while nothing works, up to a cap that still heals a router outage unattended.
constexpr uint32_t kReconnectStartMs = 10000;
constexpr uint32_t kReconnectMaxMs = 15 * 60 * 1000;
// An access point that neither accepts nor rejects produces no disconnect
// event at all. Without a cap the trial would hold the radio for ever, leaving
// the device outside its own reconnect logic and still refusing scans.
constexpr uint32_t kTrialTimeoutMs = 60000;
// A trial ends when we end it, not when the driver stops talking: the core
// retries once per boot on its own, whatever the failure reason, so one begin
// is not one disconnect. Teardown swallows whatever that produces, ending on
// its own disconnect or here, so one attempt can only ever give one verdict.
constexpr uint32_t kTrialTeardownMs = 1000;
constexpr uint8_t kSetupApChannel = 1;
// The one place the setup AP address is decided. It is read back from the
// driver while the AP is up and reported from here when it is down, because
// the status page names the setup network whether it is running or not.
const IPAddress kSetupApAddress(100, 64, 0, 1);
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

// One trial is one bounded operation with a fixed lifecycle. Starting is
// staged work the radio has not begun; TearingDown is the window in which no
// event may be read as a verdict.
enum class TrialPhase : uint8_t {
    Idle = 0,
    Starting,
    Attempting,
    TearingDown,
};

Preferences identityPreferences;
DNSServer dnsServer;
configuration::ApplyCallback applyConfiguration = nullptr;
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
uint32_t stationUnavailableAt = 0;
uint32_t reconnectAt = 0;
uint32_t reconnectBackoffMs = kReconnectStartMs;
bool attemptAfterScan = false;
ScanState currentScanState = ScanState::Idle;
ConnectionOutcome stationOutcome = ConnectionOutcome::None;

TrialPhase trialPhase = TrialPhase::Idle;
WifiCredentials trialCandidate{};
// Execution and result are separate state. A verdict ends execution but stays
// readable until another trial replaces it, which is the only thing that lets
// a phone that lost the page come back to the answer.
ConnectionOutcome trialOutcome = ConnectionOutcome::None;
int32_t trialRssi = 0;
uint32_t trialPhaseDeadline = 0;

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
    // and makes the setup radio depend on concurrent-mode behavior. A trial is
    // the exception that proves the rule: a first-time trial has nothing stored
    // yet — that is the point of it — and still needs the station interface.
    WiFi.mode(stationIsConfigured || trialRunning() ? WIFI_AP_STA : WIFI_AP);
    // Android abandons captive detection before it sends any HTTP probe when
    // the probe hostname resolves to an RFC1918 or link-local address. See
    // NetworkMonitor.hasPrivateIpAddress: 10/8, 172.16/12, 192.168/16 and
    // 169.254/16. The Arduino default AP address sits inside that test, so it
    // loses every Android client no matter what http_server answers. RFC 6598
    // Shared Address Space is outside the test and is reserved for equipment
    // that must not occupy RFC1918. Never move this AP onto a private range.
    if (!WiFi.softAPConfig(
            kSetupApAddress, kSetupApAddress, IPAddress(255, 255, 255, 0)))
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
    // Captive clients ask DNS for their own probe hostnames, and the answer has
    // to be the local web UI. The "*" is a domain wildcard, not an interface
    // one, and the socket listens on every interface: any DNS query reaching
    // this device is answered with the setup address for as long as the AP
    // is up.
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

// What the event task copied out, taken once per pass by the loop task. The
// callbacks copy facts and nothing else; classification, trial transitions,
// commits and radio changes all belong to service(), which runs on loop().
struct StationEvents {
    bool disconnected;
    bool gotIp;
    uint8_t reason;
    int32_t rssi;
};

StationEvents takeStationEvents() {
    StationEvents events{};
    events.disconnected = sawDisconnect;
    events.gotIp = sawGotIp;
    events.reason = lastDisconnectReason;
    events.rssi = lastDisconnectRssi;
    sawDisconnect = false;
    sawGotIp = false;
    return events;
}

// A voluntary disconnect is never a verdict, because in this design the one
// who left is always us: a trial tearing itself down, or the driver leaving the
// old network so a trial can try a new one. The core refuses to reconnect on
// it for the same reason.
bool voluntaryDisconnect(uint8_t reason) {
    return reason == WIFI_REASON_ASSOC_LEAVE;
}

// Report the phase that failed, which is all the driver knows. None of these
// codes means "wrong password": a station is never the authenticator, so a
// wrong key produces silence and then a timeout, never a rejection. Three named
// sets and a default keep this total over an enum that runs past 200 with codes
// for TDLS, QoS, block-ack and fast transition.
ConnectionOutcome classifyDisconnect(uint8_t reason) {
    switch (reason) {
        // The two sides could not agree on crypto, or the driver's own WPA2
        // threshold refused the access point before attempting it. Answering
        // these with "check the password" would blame a step never reached.
        case WIFI_REASON_GROUP_CIPHER_INVALID:
        case WIFI_REASON_PAIRWISE_CIPHER_INVALID:
        case WIFI_REASON_AKMP_INVALID:
        case WIFI_REASON_UNSUPP_RSN_IE_VERSION:
        case WIFI_REASON_INVALID_RSN_IE_CAP:
        case WIFI_REASON_CIPHER_SUITE_REJECTED:
        case WIFI_REASON_BAD_CIPHER_OR_AKM:
        case WIFI_REASON_NO_AP_FOUND_W_COMPATIBLE_SECURITY:
        case WIFI_REASON_NO_AP_FOUND_IN_AUTHMODE_THRESHOLD:
            return ConnectionOutcome::SecurityMismatch;
        case WIFI_REASON_NO_AP_FOUND:
            return ConnectionOutcome::NotFound;
        case WIFI_REASON_AUTH_EXPIRE:
        case WIFI_REASON_MIC_FAILURE:
        case WIFI_REASON_4WAY_HANDSHAKE_TIMEOUT:
        case WIFI_REASON_IE_IN_4WAY_DIFFERS:
        case WIFI_REASON_802_1X_AUTH_FAILED:
        case WIFI_REASON_AUTH_FAIL:
        case WIFI_REASON_HANDSHAKE_TIMEOUT:
            return ConnectionOutcome::AuthFailed;
        default:
            // Including 203, 205 and 200: we do not know what happened, and
            // CouldNotConnect says exactly that.
            return ConnectionOutcome::CouldNotConnect;
    }
}

// The raw reason decides nothing; the outcome above is what anyone acts on.
// It is here for support and for confirming the table on real hardware, so it
// goes to the core log rather than to Serial, which carries the bridge's data.
// Raise CORE_DEBUG_LEVEL to see it, accepting that the log shares that port.
void noteDisconnectReason(const StationEvents &events) {
    log_i("Wi-Fi disconnect reason %u, rssi %d", events.reason,
          static_cast<int>(events.rssi));
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
    // The cycle is scheduled by the attempt it follows, so the scan that opens
    // the next one can never land on top of this attempt.
    reconnectAt = millis() + reconnectBackoffMs;
    reconnectBackoffMs = reconnectBackoffMs > kReconnectMaxMs / 2
        ? kReconnectMaxMs
        : reconnectBackoffMs * 2;
}

// Sequential by construction: a scan in flight defers the attempt, and an
// attempt is only ever started once a scan has finished.
void serviceReconnect(uint32_t now) {
    if (scanState() == ScanState::Scanning) {
        // A user pressing Scan sets the timing of the attempt too, so the
        // reconnect comes forward with the scan instead of racing it.
        attemptAfterScan = true;
        return;
    }
    if (attemptAfterScan) {
        attemptAfterScan = false;
        // Never gated on the scan having seen the network: hidden networks join
        // through this same path, and absence is the driver's answer to report,
        // not ours to infer.
        beginStation(configuration::snapshot());
        return;
    }
    if (static_cast<int32_t>(now - reconnectAt) >= 0) {
        startScan();
        attemptAfterScan = true;
    }
}

void startTrialAttempt(uint32_t now) {
    // The way back in, before the radio moves: a trial started from the LAN
    // drops the station, and the setup AP is the interface the user returns to.
    startAp();
    strncpy(stationSsid, trialCandidate.ssid, sizeof(stationSsid) - 1);
    stationSsid[sizeof(stationSsid) - 1] = '\0';
    WiFi.mode(apIsActive ? WIFI_AP_STA : WIFI_STA);
    // Connect by name and let the driver pick the access point, exactly as any
    // phone does. WiFi.begin() leaves a network it is already on; that
    // departure is a voluntary disconnect, which is never read as a verdict.
    if (trialCandidate.security == static_cast<uint8_t>(configuration::WifiSecurity::Open)) {
        WiFi.begin(trialCandidate.ssid);
    } else {
        WiFi.begin(trialCandidate.ssid, trialCandidate.password);
    }
    trialPhaseDeadline = now + kTrialTimeoutMs;
    trialPhase = TrialPhase::Attempting;
}

// Every ending except a proved connection comes through here.
void finishTrial(ConnectionOutcome outcome, int32_t rssi) {
    trialOutcome = outcome;
    trialRssi = rssi;
    // The verdict and the network name survive for the phone to read; the
    // password does not outlive the trial that used it.
    memset(trialCandidate.password, 0, sizeof(trialCandidate.password));
    // Nothing was committed, so the stored network comes back through the
    // ordinary scan-then-attempt cycle. Restoring it is not an operation the
    // trial performs; it is what the absence of a commit already means.
    reconnectBackoffMs = kReconnectStartMs;
    reconnectAt = millis();
    attemptAfterScan = false;
    WiFi.disconnect(false, false);
    trialPhaseDeadline = millis() + kTrialTeardownMs;
    trialPhase = TrialPhase::TearingDown;
}

void completeTrial() {
    configuration::DeviceConfig candidate = configuration::snapshot();
    strncpy(candidate.ssid, trialCandidate.ssid, sizeof(candidate.ssid) - 1);
    candidate.ssid[sizeof(candidate.ssid) - 1] = '\0';
    candidate.wifiSecurity = trialCandidate.security;
    strncpy(candidate.wifiPassword, trialCandidate.password,
            sizeof(candidate.wifiPassword) - 1);
    candidate.wifiPassword[sizeof(candidate.wifiPassword) - 1] = '\0';
    // The snapshot is taken now, not when the trial started, so a setting the
    // user changed during the attempt is not silently reverted by this commit.
    if (!configuration::commit(candidate, applyConfiguration)) {
        // Keeping a connection that was never saved would leave the device
        // connected but unconfigured, a state service() has no branch for.
        finishTrial(ConnectionOutcome::CouldNotSave, 0);
        return;
    }
    trialOutcome = ConnectionOutcome::Connected;
    memset(trialCandidate.password, 0, sizeof(trialCandidate.password));
    // No teardown: the connection just proved is the product, and dropping it
    // would be the one unforgivable ending.
    trialPhase = TrialPhase::Idle;
}

void serviceTrial(uint32_t now, const StationEvents &events) {
    switch (trialPhase) {
        case TrialPhase::Starting:
            startTrialAttempt(now);
            return;
        case TrialPhase::Attempting:
            if (events.gotIp) {
                completeTrial();
            } else if (events.disconnected && !voluntaryDisconnect(events.reason)) {
                noteDisconnectReason(events);
                finishTrial(classifyDisconnect(events.reason), events.rssi);
            } else if (static_cast<int32_t>(now - trialPhaseDeadline) >= 0) {
                finishTrial(ConnectionOutcome::CouldNotConnect, 0);
            }
            return;
        case TrialPhase::TearingDown:
            // Nothing arriving now can be a verdict, so nothing here reads one:
            // taking the events is already swallowing them.
            if (events.disconnected ||
                    static_cast<int32_t>(now - trialPhaseDeadline) >= 0) {
                trialPhase = TrialPhase::Idle;
            }
            return;
        case TrialPhase::Idle:
            return;
    }
}

} // namespace

void begin(configuration::ApplyCallback apply) {
    applyConfiguration = apply;
    WiFi.persistent(false);
    WiFi.setAutoReconnect(false);
    // Both callbacks run on the Wi-Fi event task, so both only copy facts.
    // Deciding anything here would race everything the loop task owns.
    WiFi.onEvent(
        [](arduino_event_id_t, arduino_event_info_t info) {
            lastDisconnectReason = info.wifi_sta_disconnected.reason;
            lastDisconnectRssi = info.wifi_sta_disconnected.rssi;
            sawDisconnect = true;
        },
        ARDUINO_EVENT_WIFI_STA_DISCONNECTED);
    WiFi.onEvent(
        [](arduino_event_id_t, arduino_event_info_t) { sawGotIp = true; },
        ARDUINO_EVENT_WIFI_STA_GOT_IP);
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
    const uint32_t now = millis();
    const StationEvents events = takeStationEvents();

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
    }

    // One radio, one owner, and while a trial runs the owner is the trial. The
    // phone is sitting on the setup AP, which stays up above, but the stored
    // network is neither scanned for nor attempted underneath the attempt.
    if (trialPhase != TrialPhase::Idle) {
        serviceTrial(now, events);
        return;
    }

    if (!stationIsConfigured) {
        startAp();
        apCloseAt = 0;
        stationUnavailableAt = 0;
        return;
    }

    if (stationConnected()) {
        stationOutcome = ConnectionOutcome::Connected;
        reconnectBackoffMs = kReconnectStartMs;
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
    // The same classifier the trial uses, on the same events. A network that
    // changes its password a year later has to be described by the display and
    // by /api/status, and two classifications of one event would drift apart.
    if (events.disconnected && !voluntaryDisconnect(events.reason)) {
        noteDisconnectReason(events);
        stationOutcome = classifyDisconnect(events.reason);
    }
    // Stored credentials are retried for ever on the backoff: they earned that
    // by having worked once, and it is what heals a router reboot unattended.
    serviceReconnect(now);
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
    stationOutcome = ConnectionOutcome::None;
    reconnectBackoffMs = kReconnectStartMs;
    attemptAfterScan = false;
    if (wasStationConnected)
        MDNS.end();
    wasStationConnected = false;
    apCloseAt = 0;
    stationUnavailableAt = 0;
    // A trial commits the credentials it has just proved, and the station is
    // already on that network. Restarting it would drop the very connection the
    // trial exists to establish; service() adopts the live one on its next
    // pass, which is also where mDNS and the setup AP's grace period restart.
    if (stationIsConfigured && stationConnected() &&
            strcmp(stationSsid, next.ssid) == 0) {
        return;
    }
    if (!stationIsConfigured) {
        stationSsid[0] = '\0';
        WiFi.disconnect(false, false);
        startAp();
        return;
    }
    startAp();
    beginStation(next);
}

void beginTrial(const WifiCredentials &candidate) {
    trialCandidate = candidate;
    trialOutcome = ConnectionOutcome::None;
    trialRssi = 0;
    // Staged only. The radio work starts on the next service() pass, after the
    // HTTP response has left: the attempt can drop the very link that response
    // has to travel on, and a phone that never receives it is left guessing.
    trialPhase = TrialPhase::Starting;
}

void cancelTrial() {
    switch (trialPhase) {
        case TrialPhase::Attempting:
            // An attempt the user abandoned has no verdict to report.
            finishTrial(ConnectionOutcome::None, 0);
            return;
        case TrialPhase::TearingDown:
            // Let the teardown finish. Cutting it short would let its own
            // events reach the next trial and answer for it.
            trialOutcome = ConnectionOutcome::None;
            return;
        case TrialPhase::Starting:
        case TrialPhase::Idle:
            // Staged but never started, or already finished: no radio work to
            // undo. Dismissing a verdict is the same request, and it is what
            // stops a later page load resurrecting an old failure.
            trialPhase = TrialPhase::Idle;
            trialOutcome = ConnectionOutcome::None;
            return;
    }
}

bool trialRunning() {
    return trialPhase != TrialPhase::Idle;
}

TrialStatus trialStatus() {
    TrialStatus result{};
    result.running = trialRunning();
    // The verdict is published only once the trial is fully idle, so one press
    // gives one answer and Try again is never refused by a teardown.
    result.outcome = result.running ? ConnectionOutcome::None : trialOutcome;
    strncpy(result.ssid, trialCandidate.ssid, sizeof(result.ssid) - 1);
    result.rssi = trialRssi;
    return result;
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
    result.stationOutcome = stationOutcome;
    result.stationRssi = result.stationConnected ? WiFi.RSSI() : 0;
    strncpy(result.stationSsid, stationSsid, sizeof(result.stationSsid) - 1);
    strncpy(result.setupSsid, deviceName, sizeof(result.setupSsid) - 1);
    strncpy(result.setupPassword, devicePassword, sizeof(result.setupPassword) - 1);
    // How many stations the driver actually holds. What each one is cannot be
    // asked of it, and a browser naming itself is not the same question.
    result.setupClients = WiFi.softAPgetStationNum();
    // An AP that has shut itself down still has an address to come back on,
    // and the status page names that network in both states. The OLED's setup
    // pages exist only while the AP is up, where the driver answers anyway.
    const IPAddress apIp = WiFi.softAPIP();
    result.setupIp = apIp == IPAddress(0, 0, 0, 0) ? kSetupApAddress : apIp;
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
    // A scan outranks an attempt in flight. The driver refuses to scan while
    // it is connecting -- esp_wifi_scan_start returns ESP_ERR_WIFI_STATE, and
    // esp_wifi_connect warns that a scan "will not be effective until
    // connection between device and the AP is established" -- so an attempt
    // that is still running would cost the user a press and answer "Could not
    // scan". Dropping it costs nothing: the attempt returns on the next cycle,
    // which serviceReconnect already sequences behind this scan. The departure
    // is voluntary, and voluntaryDisconnect() keeps it out of the classifier.
    // Never do this while connected: the setup AP can be scanned from during
    // the grace window, and a live station connection is not a scan's to spend.
    if (!stationConnected())
        WiFi.disconnect(false, false);
    WiFi.scanDelete();
    // Report the scan that actually started. Forcing Scanning after a failed
    // start made scanState() resolve to Failed a moment later, so the user saw
    // "Could not scan" and had to press twice.
    currentScanState = WiFi.scanNetworks(true) == WIFI_SCAN_RUNNING
        ? ScanState::Scanning
        : ScanState::Failed;
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
