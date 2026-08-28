#include "http_server.h"

#include <Arduino.h>
#include <LittleFS.h>
#include <WebServer.h>
#include <cerrno>
#include <cstdint>
#include <esp_system.h>
#include <sys/socket.h>

#include "browser_terminal.h"
#include "build_number.h"
#include "management_auth.h"
#include "network_transport.h"
#include "serial_port.h"
#include "wifi_access.h"

namespace http_server {
namespace {

WebServer server(configuration::kHttpPort);
configuration::ApplyCallback applyConfiguration = nullptr;
char csrfToken[33]{};
// The frontend build number is packed into the filesystem image, so it is the
// image's own property and cannot be a compile-time constant of the firmware.
char frontendBuildNumber[14]{};
bool filesystemReady = false;

const char *securityName(configuration::WifiSecurity security) {
    switch (security) {
        case configuration::WifiSecurity::Unset: return "unset";
        case configuration::WifiSecurity::Open: return "open";
        case configuration::WifiSecurity::Secured: return "secured";
    }
    return "unset";
}

bool parseSecurity(const String &value, configuration::WifiSecurity &security) {
    if (value == "unset") security = configuration::WifiSecurity::Unset;
    else if (value == "open") security = configuration::WifiSecurity::Open;
    else if (value == "secured") security = configuration::WifiSecurity::Secured;
    else return false;
    return true;
}

bool parseUnsigned(const String &text, uint32_t &value) {
    if (text.length() == 0) return false;
    uint32_t parsed = 0;
    for (size_t i = 0; i < text.length(); ++i) {
        const char character = text[i];
        if (character < '0' || character > '9') return false;
        const uint32_t digit = static_cast<uint32_t>(character - '0');
        if (parsed > (UINT32_MAX - digit) / 10U) return false;
        parsed = parsed * 10U + digit;
    }
    value = parsed;
    return true;
}

String escaped(const String &value) {
    String result;
    result.reserve(value.length() + 8);
    for (size_t i = 0; i < value.length(); ++i) {
        const uint8_t character = static_cast<uint8_t>(value[i]);
        if (character == '\\') result += "\\\\";
        else if (character == '"') result += "\\\"";
        else if (character == '\n') result += "\\n";
        else if (character == '\r') result += "\\r";
        else if (character == '\b') result += "\\b";
        else if (character == '\f') result += "\\f";
        else if (character == '\t') result += "\\t";
        else if (character < 0x20) {
            char unicode[7];
            snprintf(unicode, sizeof(unicode), "\\u%04X", character);
            result += unicode;
        } else {
            result += static_cast<char>(character);
        }
    }
    return result;
}

String ipString(const IPAddress &address) {
    return address.toString();
}

const char *connectionName(network_transport::ConnectionState state) {
    switch (state) {
        case network_transport::ConnectionState::Disabled: return "disabled";
        case network_transport::ConnectionState::WaitingForWifi: return "waiting_for_wifi";
        case network_transport::ConnectionState::Listening: return "listening";
        case network_transport::ConnectionState::Connecting: return "connecting";
        case network_transport::ConnectionState::Retrying: return "retrying";
        case network_transport::ConnectionState::Connected: return "connected";
        case network_transport::ConnectionState::Failure: return "failure";
    }
    return "disabled";
}

// An image built without the stamp leaves the number empty rather than
// reporting a value the filesystem cannot support.
void loadFrontendBuildNumber() {
    if (!filesystemReady) return;
    File file = LittleFS.open("/build-stamp.txt", "r");
    if (!file) return;
    const size_t length =
        file.readBytes(frontendBuildNumber, sizeof(frontendBuildNumber) - 1);
    file.close();
    frontendBuildNumber[length] = '\0';
    for (size_t index = 0; index < length; ++index) {
        const char character = frontendBuildNumber[index];
        if (character == '\r' || character == '\n') {
            frontendBuildNumber[index] = '\0';
            break;
        }
    }
}

const char *stationStateName(wifi_access::StationState state) {
    switch (state) {
        case wifi_access::StationState::Connecting: return "connecting";
        case wifi_access::StationState::Connected: return "connected";
        case wifi_access::StationState::BadPassword: return "bad_password";
        case wifi_access::StationState::NotFound: return "not_found";
        case wifi_access::StationState::Failed: return "failed";
        case wifi_access::StationState::Unconfigured: break;
    }
    return "unconfigured";
}

bool fromSetupAp() {
    return wifi_access::requestFromSetupAp(server.client());
}

bool fromLocalInterface() {
    return wifi_access::requestFromLocalInterface(server.client());
}

bool matchesHost(const String &hostHeader, const String &expectedHost) {
    String host = hostHeader;
    host.trim();
    return host.equalsIgnoreCase(expectedHost) ||
        host.equalsIgnoreCase(expectedHost + ":" + String(configuration::kHttpPort));
}

bool canonicalSetupHost() {
    return matchesHost(server.hostHeader(), WiFi.softAPIP().toString());
}

bool canonicalLocalHost() {
    if (canonicalSetupHost()) return true;

    const IPAddress stationIp = WiFi.localIP();
    if (stationIp != IPAddress(0, 0, 0, 0) &&
            matchesHost(server.hostHeader(), stationIp.toString())) {
        return true;
    }
    return matchesHost(server.hostHeader(), wifi_access::mdnsHost());
}

bool csrfValid() {
    return server.header("X-CSRF-Token") == csrfToken;
}

void sendJson(const String &body, int status = 200) {
    server.sendHeader("Cache-Control", "no-store");
    server.send(status, "application/json", body);
}

bool sessionAuthenticated() {
    return management_auth::authenticated(server.header("Cookie").c_str());
}

const char *authenticationState(bool authenticated) {
    if (!management_auth::passwordSet()) return "password_not_configured";
    return authenticated ? "authenticated" : "login_required";
}

void sendForbidden() {
    server.sendHeader("Cache-Control", "no-store");
    server.send(403, "application/json", "{\"error\":\"configuration_not_allowed\"}");
}

void sendUnauthorized() {
    sendJson("{\"error\":\"authentication_required\"}", 401);
}

void sendAuthError(const char *field, const char *message, int status = 400) {
    String body = "{\"error\":\"" + String(message) + "\",\"field\":\"" +
        String(field) + "\"}";
    sendJson(body, status);
}

bool canonicalLocalRequest() {
    return fromLocalInterface() && canonicalLocalHost();
}

bool requireConfigurationAccess() {
    if (!canonicalLocalRequest()) {
        sendForbidden();
        return false;
    }
    if (!management_auth::passwordSet()) {
        if (fromSetupAp()) return true;
        sendJson("{\"error\":\"admin_password_required\"}", 403);
        return false;
    }
    if (!sessionAuthenticated()) {
        sendUnauthorized();
        return false;
    }
    return true;
}

bool requireSetupAccess() {
    if (!fromSetupAp() || !canonicalSetupHost()) {
        sendForbidden();
        return false;
    }
    if (management_auth::passwordSet() && !sessionAuthenticated()) {
        sendUnauthorized();
        return false;
    }
    return true;
}

void setSessionCookie() {
    server.sendHeader(
        "Set-Cookie",
        "s2w_session=" + String(management_auth::sessionToken()) +
            "; HttpOnly; SameSite=Strict; Path=/",
        true);
}

void clearSessionCookie() {
    server.sendHeader(
        "Set-Cookie",
        "s2w_session=; HttpOnly; SameSite=Strict; Path=/; Max-Age=0",
        true);
}

bool sameOrigin() {
    if (!canonicalSetupHost()) return false;

    String origin = server.header("Origin");
    if (origin.length() == 0) return false;

    const int schemeEnd = origin.indexOf("://");
    if (schemeEnd < 0) return false;
    if (!origin.substring(0, schemeEnd).equalsIgnoreCase("http")) return false;
    origin = origin.substring(schemeEnd + 3);
    const int pathStart = origin.indexOf('/');
    if (pathStart >= 0) origin = origin.substring(0, pathStart);
    return matchesHost(origin, WiFi.softAPIP().toString());
}

bool headerContainsToken(const String &header, const char *token) {
    int start = 0;
    while (start <= header.length()) {
        int end = header.indexOf(',', start);
        if (end < 0) end = header.length();
        String value = header.substring(start, end);
        value.trim();
        if (value.equalsIgnoreCase(token)) return true;
        start = end + 1;
    }
    return false;
}

void handleTerminal() {
    // The terminal carries serial data, so it is part of the editable setup
    // boundary rather than the status-only LAN surface.
    if (!requireSetupAccess()) return;
    if (!server.header("Upgrade").equalsIgnoreCase("websocket") ||
            !headerContainsToken(server.header("Connection"), "Upgrade") ||
            server.header("Sec-WebSocket-Version") != "13" ||
            !sameOrigin()) {
        return server.send(403, "text/plain", "WebSocket rejected");
    }
    if (!browser_terminal::accept(
            server.client(),
            server.header("Sec-WebSocket-Key").c_str())) {
        return server.send(503, "text/plain", "Terminal unavailable");
    }
}

void appendTcpAndSerialConfiguration(
    String &body,
    const configuration::DeviceConfig &config) {
    body += ",\"tcpMode\":\"" + String(configuration::tcpModeName(
            static_cast<configuration::TcpMode>(config.tcpMode))) + "\"";
    body += ",\"tcpListenPort\":" + String(config.tcpListenPort);
    body += ",\"tcpRemoteHost\":\"" + escaped(config.tcpRemoteHost) + "\"";
    body += ",\"tcpRemotePort\":" + String(config.tcpRemotePort);
    body += ",\"baud\":" + String(config.baud);
    body += ",\"framing\":\"" + String(configuration::framingName(
            static_cast<configuration::Framing>(config.framing))) + "\"";
}

void handleStatus() {
    if (!canonicalLocalRequest()) {
        return server.send(404, "text/plain", "Not found");
    }

    const configuration::DeviceConfig config = configuration::snapshot();
    const wifi_access::Snapshot wifi = wifi_access::snapshot();
    const network_transport::Snapshot transport = network_transport::snapshot();
    const serial_port::Snapshot serial = serial_port::snapshot();
    const bool authenticated = sessionAuthenticated();
    const bool passwordSet = management_auth::passwordSet();
    String body;
    body.reserve(1536);
    body += "{";
    body += "\"configurationAllowed\":" + String(
        (!passwordSet ? fromSetupAp() : authenticated) ? "true" : "false");
    body += ",\"passwordSet\":" + String(passwordSet ? "true" : "false");
    body += ",\"authenticated\":" + String(authenticated ? "true" : "false");
    body += ",\"authState\":\"" + String(authenticationState(authenticated)) + "\"";
    body += ",\"terminalAvailable\":" + String(fromSetupAp() ? "true" : "false");
    body += ",\"firmwareBuild\":\"" + String(build_number::kFirmware) + "\"";
    body += ",\"frontendBuild\":\"" + escaped(frontendBuildNumber) + "\"";
    body += ",\"wifiConfigured\":" + String(wifi.stationConfigured ? "true" : "false");
    body += ",\"wifiConnected\":" + String(wifi.stationConnected ? "true" : "false");
    body += ",\"wifiState\":\"" + String(stationStateName(wifi.stationState)) + "\"";
    body += ",\"wifiProvisionFailure\":\"" +
        String(stationStateName(wifi.provisioningFailure)) + "\"";
    body += ",\"wifiApActive\":" + String(wifi.setupApActive ? "true" : "false");
    body += ",\"setupSsid\":\"" + escaped(wifi.setupSsid) + "\"";
    body += ",\"stationIp\":\"" + escaped(ipString(wifi.stationIp)) + "\"";
    body += ",\"tcpState\":\"" + String(connectionName(transport.state)) + "\"";
    body += ",\"transportTaskError\":" + String(transport.taskStartError ? "true" : "false");
    appendTcpAndSerialConfiguration(body, config);
    body += ",\"serialToNetworkReceived\":" + String(static_cast<unsigned long long>(transport.serialToNetworkReceived));
    body += ",\"serialToNetworkForwarded\":" + String(static_cast<unsigned long long>(transport.serialToNetworkForwarded));
    body += ",\"serialToNetworkDropped\":" + String(static_cast<unsigned long long>(transport.serialToNetworkDropped));
    body += ",\"networkToSerialReceived\":" + String(static_cast<unsigned long long>(transport.networkToSerialReceived));
    body += ",\"networkToSerialForwarded\":" + String(static_cast<unsigned long long>(transport.networkToSerialForwarded));
    body += ",\"networkToSerialDropped\":" + String(static_cast<unsigned long long>(transport.networkToSerialDropped));
    body += ",\"terminalToSerialReceived\":" + String(static_cast<unsigned long long>(transport.terminalToSerialReceived));
    body += ",\"serialFifoOverflowErrors\":" + String(serial.fifoOverflowErrors);
    body += ",\"serialBufferOverflowErrors\":" + String(serial.bufferOverflowErrors);
    body += ",\"serialFramingErrors\":" + String(serial.framingErrors);
    body += ",\"serialParityErrors\":" + String(serial.parityErrors);
    body += ",\"serialError\":" + String(serial.error ? "true" : "false");
    body += "}";
    sendJson(body);
}

void handleAuthGet() {
    if (!canonicalLocalRequest()) return sendForbidden();
    const bool authenticated = sessionAuthenticated();
    String body;
    body.reserve(256);
    body += "{\"passwordSet\":" + String(
        management_auth::passwordSet() ? "true" : "false");
    body += ",\"authenticated\":" + String(authenticated ? "true" : "false");
    body += ",\"authState\":\"" + String(authenticationState(authenticated)) + "\"";
    body += ",\"terminalAvailable\":" + String(fromSetupAp() ? "true" : "false");
    body += ",\"csrfToken\":\"" + String(csrfToken) + "\"}";
    sendJson(body);
}

void handleAuthLogin() {
    if (!canonicalLocalRequest()) return sendForbidden();
    if (!csrfValid()) return sendJson("{\"error\":\"csrf\"}", 403);
    if (!server.hasArg("password")) {
        return sendAuthError("password", "password_required");
    }

    switch (management_auth::login(server.arg("password").c_str())) {
        case management_auth::LoginResult::Authenticated:
            setSessionCookie();
            return sendJson("{\"ok\":true,\"authenticated\":true}");
        case management_auth::LoginResult::PasswordNotSet:
            return sendJson("{\"error\":\"password_not_set\"}", 409);
        case management_auth::LoginResult::RateLimited:
            server.sendHeader("Retry-After", "1");
            return sendJson("{\"error\":\"too_many_attempts\"}", 429);
        case management_auth::LoginResult::InvalidPassword:
            return sendJson("{\"error\":\"invalid_password\"}", 401);
    }
    sendJson("{\"error\":\"login_failed\"}", 500);
}

void handleAuthLogout() {
    if (!canonicalLocalRequest()) return sendForbidden();
    if (!csrfValid()) return sendJson("{\"error\":\"csrf\"}", 403);
    management_auth::logout();
    clearSessionCookie();
    sendJson("{\"ok\":true,\"authenticated\":false}");
}

void handleAuthPassword() {
    if (!canonicalLocalRequest()) return sendForbidden();
    if (!csrfValid()) return sendJson("{\"error\":\"csrf\"}", 403);
    management_auth::PasswordResult result;
    if (!management_auth::passwordSet()) {
        if (!fromSetupAp()) {
            return sendJson("{\"error\":\"bootstrap_requires_setup_ap\"}", 403);
        }
        if (!server.hasArg("newPassword")) {
            return sendAuthError("newPassword", "password_required");
        }
        result = management_auth::createPassword(
            server.arg("newPassword").c_str());
    } else {
        if (!sessionAuthenticated()) {
            return sendUnauthorized();
        }
        if (!server.hasArg("currentPassword")) {
            return sendAuthError("currentPassword", "password_required");
        }
        if (!server.hasArg("newPassword")) {
            return sendAuthError("newPassword", "password_required");
        }
        result = management_auth::changePassword(
            server.arg("currentPassword").c_str(),
            server.arg("newPassword").c_str());
    }

    switch (result) {
        case management_auth::PasswordResult::Success:
            clearSessionCookie();
            return sendJson("{\"ok\":true}");
        case management_auth::PasswordResult::Invalid:
            return sendAuthError("newPassword", "invalid_password");
        case management_auth::PasswordResult::AlreadySet:
            return sendJson("{\"error\":\"password_already_set\"}", 409);
        case management_auth::PasswordResult::CurrentPasswordIncorrect:
            return sendAuthError("currentPassword", "incorrect_password", 403);
        case management_auth::PasswordResult::StorageFailure:
            return sendJson("{\"error\":\"password_save_failed\"}", 500);
    }
    sendJson("{\"error\":\"password_change_failed\"}", 500);
}

void handleConfigGet() {
    if (!requireConfigurationAccess()) return;
    const configuration::DeviceConfig config = configuration::snapshot();
    String body;
    body.reserve(768);
    body += "{";
    body += "\"ssid\":\"" + escaped(config.ssid) + "\"";
    body += ",\"wifiSecurity\":\"" + String(securityName(
            static_cast<configuration::WifiSecurity>(config.wifiSecurity))) + "\"";
    body += ",\"wifiPasswordSaved\":" + String(config.wifiPassword[0] != '\0' ? "true" : "false");
    appendTcpAndSerialConfiguration(body, config);
    body += ",\"longPressMs\":" + String(config.longPressMs);
    body += ",\"longPressRepeatMs\":" + String(config.longPressRepeatMs);
    body += ",\"screenSaverSeconds\":" + String(config.screenSaverSeconds);
    body += ",\"csrfToken\":\"" + String(csrfToken) + "\"";
    body += "}";
    sendJson(body);
}

void configError(const char *field, const char *message) {
    String body = "{\"error\":\"" + String(message) + "\",\"field\":\"" + String(field) + "\"}";
    sendJson(body, 400);
}

bool completeConfigurationRequest() {
    constexpr const char *fields[] = {
        "ssid",
        "wifiSecurity",
        "wifiPassword",
        "tcpMode",
        "tcpListenPort",
        "tcpRemoteHost",
        "tcpRemotePort",
        "baud",
        "framing",
        "longPressMs",
        "longPressRepeatMs",
        "screenSaverSeconds",
    };
    for (const char *field : fields) {
        if (!server.hasArg(field)) return false;
    }
    return true;
}

void sendValidationError(configuration::ValidationError error) {
    switch (error) {
        case configuration::ValidationError::None:
            return;
        case configuration::ValidationError::Schema:
            return configError("configuration", "invalid_schema");
        case configuration::ValidationError::Baud:
            return configError("baud", "unsupported_baud");
        case configuration::ValidationError::Framing:
            return configError("framing", "invalid_framing");
        case configuration::ValidationError::WifiSsid:
            return configError("ssid", "too_long");
        case configuration::ValidationError::WifiSecurity:
            return configError("wifiSecurity", "invalid_security");
        case configuration::ValidationError::WifiPassword:
            return configError("wifiPassword", "invalid_password");
        case configuration::ValidationError::TcpMode:
            return configError("tcpMode", "invalid_mode");
        case configuration::ValidationError::TcpListenPort:
            return configError("tcpListenPort", "invalid_port");
        case configuration::ValidationError::TcpRemoteHost:
            return configError("tcpRemoteHost", "invalid_host");
        case configuration::ValidationError::TcpRemotePort:
            return configError("tcpRemotePort", "invalid_port");
        case configuration::ValidationError::LongPress:
            return configError("longPressMs", "invalid_timeout");
        case configuration::ValidationError::LongPressRepeat:
            return configError("longPressRepeatMs", "invalid_timeout");
        case configuration::ValidationError::ScreenSaver:
            return configError("screenSaverSeconds", "invalid_timeout");
        default:
            sendJson("{\"error\":\"invalid_configuration\"}", 500);
            return;
    }
}

void handleConfigPost() {
    if (!requireConfigurationAccess()) return;
    if (!csrfValid()) return sendJson("{\"error\":\"csrf\"}", 403);
    if (!completeConfigurationRequest()) {
        return configError("configuration", "incomplete_request");
    }

    const configuration::DeviceConfig current = configuration::snapshot();
    configuration::DeviceConfig candidate = current;
    const String ssid = server.arg("ssid");
    const String securityValue = server.arg("wifiSecurity");
    const String password = server.arg("wifiPassword");
    const String tcpModeValue = server.arg("tcpMode");
    const String tcpListenPort = server.arg("tcpListenPort");
    const String tcpRemoteHost = server.arg("tcpRemoteHost");
    const String tcpRemotePort = server.arg("tcpRemotePort");
    const String baud = server.arg("baud");
    const String framing = server.arg("framing");
    const String longPress = server.arg("longPressMs");
    const String longPressRepeat = server.arg("longPressRepeatMs");
    const String screenSaver = server.arg("screenSaverSeconds");

    if (ssid.length() > 32) return configError("ssid", "too_long");
    if (password.length() > 64) return configError("wifiPassword", "too_long");
    ssid.toCharArray(candidate.ssid, sizeof(candidate.ssid));
    configuration::WifiSecurity security;
    if (!parseSecurity(securityValue, security)) {
        return configError("wifiSecurity", "invalid_security");
    }
    candidate.wifiSecurity = static_cast<uint8_t>(security);

    if (candidate.ssid[0] == '\0') {
        candidate.wifiSecurity = static_cast<uint8_t>(configuration::WifiSecurity::Unset);
        candidate.wifiPassword[0] = '\0';
    } else if (candidate.wifiSecurity == static_cast<uint8_t>(configuration::WifiSecurity::Open)) {
        candidate.wifiPassword[0] = '\0';
    } else if (candidate.wifiSecurity == static_cast<uint8_t>(configuration::WifiSecurity::Secured)) {
        if (password.length() == 0) {
            if (strcmp(current.ssid, candidate.ssid) == 0 &&
                current.wifiSecurity == static_cast<uint8_t>(configuration::WifiSecurity::Secured) &&
                current.wifiPassword[0] != '\0') {
                strncpy(candidate.wifiPassword, current.wifiPassword, sizeof(candidate.wifiPassword) - 1);
                candidate.wifiPassword[sizeof(candidate.wifiPassword) - 1] = '\0';
            } else {
                return configError("wifiPassword", "password_required");
            }
        } else {
            password.toCharArray(candidate.wifiPassword, sizeof(candidate.wifiPassword));
        }
    }

    configuration::TcpMode tcpMode;
    if (!configuration::tcpModeFromName(tcpModeValue.c_str(), tcpMode)) {
        return configError("tcpMode", "invalid_mode");
    }
    candidate.tcpMode = static_cast<uint8_t>(tcpMode);
    if (tcpRemoteHost.length() >= sizeof(candidate.tcpRemoteHost)) {
        return configError("tcpRemoteHost", "too_long");
    }
    memset(candidate.tcpRemoteHost, 0, sizeof(candidate.tcpRemoteHost));
    tcpRemoteHost.toCharArray(candidate.tcpRemoteHost, sizeof(candidate.tcpRemoteHost));

    uint32_t listenPortNumber = 0;
    if (!parseUnsigned(tcpListenPort, listenPortNumber) || listenPortNumber > 65535) {
        return configError("tcpListenPort", "invalid_port");
    }
    candidate.tcpListenPort = static_cast<uint16_t>(listenPortNumber);

    uint32_t remotePortNumber = 0;
    if (!parseUnsigned(tcpRemotePort, remotePortNumber) || remotePortNumber > 65535) {
        return configError("tcpRemotePort", "invalid_port");
    }
    candidate.tcpRemotePort = static_cast<uint16_t>(remotePortNumber);

    uint32_t baudNumber = 0;
    if (!parseUnsigned(baud, baudNumber)) return configError("baud", "invalid_baud");
    candidate.baud = baudNumber;
    configuration::Framing framingValue;
    if (!configuration::framingFromName(framing.c_str(), framingValue)) {
        return configError("framing", "invalid_framing");
    }
    candidate.framing = static_cast<uint8_t>(framingValue);
    uint32_t longPressNumber = 0;
    if (!parseUnsigned(longPress, longPressNumber)) {
        return configError("longPressMs", "invalid_timeout");
    }
    candidate.longPressMs = longPressNumber;
    uint32_t longPressRepeatNumber = 0;
    if (!parseUnsigned(longPressRepeat, longPressRepeatNumber)) {
        return configError("longPressRepeatMs", "invalid_timeout");
    }
    candidate.longPressRepeatMs = longPressRepeatNumber;
    uint32_t screenSaverNumber = 0;
    if (!parseUnsigned(screenSaver, screenSaverNumber)) {
        return configError("screenSaverSeconds", "invalid_timeout");
    }
    candidate.screenSaverSeconds = screenSaverNumber;
    const configuration::ValidationError validation = configuration::validationError(candidate);
    if (validation != configuration::ValidationError::None) {
        return sendValidationError(validation);
    }
    bool runtimeApplied = false;
    if (!configuration::commit(candidate, applyConfiguration, &runtimeApplied)) {
        return sendJson("{\"error\":\"save_failed\"}", 500);
    }
    if (!runtimeApplied) {
        // configuration::commit persists and publishes before applying UART
        // changes. A 503 here therefore reports an applied-runtime failure,
        // not a rollback; the saved candidate remains authoritative.
        return sendJson("{\"error\":\"serial_error\",\"persisted\":true}", 503);
    }
    sendJson("{\"ok\":true}");
}

void handleScanPost() {
    if (!requireSetupAccess()) return;
    if (!csrfValid()) return sendJson("{\"error\":\"csrf\"}", 403);
    wifi_access::startScan();
    sendJson("{\"ok\":true}", 202);
}

void handleScanGet() {
    if (!requireSetupAccess()) return;
    const wifi_access::ScanState state = wifi_access::scanState();
    const char *stateName = state == wifi_access::ScanState::Scanning ? "scanning" :
        state == wifi_access::ScanState::Ready ? "ready" :
        state == wifi_access::ScanState::Failed ? "failed" : "idle";
    wifi_access::ScanResult results[32];
    const size_t count = wifi_access::copyScanResults(results, 32);
    String body;
    body.reserve(4096);
    body += "{\"state\":\"" + String(stateName) + "\",\"networks\":[";
    for (size_t i = 0; i < count; ++i) {
        if (i != 0) body += ',';
    body += "{\"ssid\":\"" + escaped(results[i].ssid) + "\",\"rssi\":" +
        String(results[i].rssi) + ",\"secured\":" +
        String(results[i].secured ? "true" : "false") + "}";
    }
    body += "]}";
    sendJson(body);
}

void handleCaptiveProbe() {
    if (!fromSetupAp()) return server.send(404, "text/plain", "Not found");

    // Never answer a probe with 204 or any other success code, however
    // tempting it looks for clearing the phone's sign-in prompt. Success
    // claims this network carries the Internet, which this device never does.
    // The phone would then treat the setup AP as a working default route and
    // send real traffic into a dead end. Redirecting is the truthful answer
    // for as long as the phone stays here; the phone clears the prompt itself
    // when it leaves the AP.
    // A probe is sent to a public hostname which DNS has mapped to this AP.
    // An absolute local URL is required because some clients do not resolve
    // a relative Location against the probe hostname before opening the page.
    const String setupLocation = "http://" + WiFi.softAPIP().toString() + "/";
    server.sendHeader("Location", setupLocation, true);
    server.sendHeader("Cache-Control", "no-store");
    server.send(302, "text/plain", server.method() == HTTP_HEAD ? "" : "Captive portal");
}

// loop() is cooperative: the PRG button (including the factory-reset gesture),
// the display, captive DNS and every other web client are serviced from it.
// streamFile() must not be used here because NetworkClient::write() waits up to
// ten one-second select intervals for each 1360-byte chunk and restarts that
// budget on any partial write, so a phone that leaves the setup AP mid-download
// holds loop() for minutes. These deadlines drop such a client instead. The
// total deadline puts the floor for the largest asset near 65 kbps; a slower
// client must move closer and retry, because button and display responsiveness
// outrank serving pages at extreme range.
constexpr uint32_t kFileSendStallMs = 1500;
constexpr uint32_t kFileSendDeadlineMs = 10000;
constexpr size_t kFileSendChunkSize = 1024;

// Returns false when the client stalled, vanished or failed, so the caller can
// drop the connection.
bool sendFileBody(File &file, int descriptor) {
    if (descriptor < 0) return false;

    uint8_t chunk[kFileSendChunkSize];
    size_t chunkLength = 0;
    size_t chunkOffset = 0;
    const uint32_t startedAt = millis();
    uint32_t lastProgressAt = startedAt;

    for (;;) {
        if (chunkOffset == chunkLength) {
            chunkLength = file.read(chunk, sizeof(chunk));
            chunkOffset = 0;
            if (chunkLength == 0) return true;
        }

        const ssize_t sent = send(
            descriptor,
            chunk + chunkOffset,
            chunkLength - chunkOffset,
            MSG_DONTWAIT);
        if (sent > 0) {
            chunkOffset += static_cast<size_t>(sent);
            lastProgressAt = millis();
        } else if (sent < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
            // The socket send buffer is full. Yield one tick so this loop does
            // not spin; the deadlines below still bound the total wait.
            delay(1);
        } else {
            return false;
        }

        const uint32_t now = millis();
        if (now - lastProgressAt >= kFileSendStallMs) return false;
        if (now - startedAt >= kFileSendDeadlineMs) return false;
    }
}

void serveFile(const char *path, const char *contentType) {
    if (!fromLocalInterface() || !canonicalLocalHost()) {
        if (fromSetupAp()) return handleCaptiveProbe();
        return server.send(404, "text/plain", "Not found");
    }
    if (!filesystemReady) return server.send(500, "text/plain", "LittleFS unavailable");

    File file = LittleFS.open(path, "r");
    if (!file) return server.send(404, "text/plain", "Not found");
    server.sendHeader("Cache-Control", "no-store");
    // The header block is a few hundred bytes into an empty send buffer, so the
    // normal response path cannot block. Only the body needs the bounded sender.
    server.setContentLength(file.size());
    server.send(200, contentType, "");
    WiFiClient &client = server.client();
    if (!sendFileBody(file, client.fd())) client.stop();
    file.close();
}

void handleNotFound() {
    // DNS sends arbitrary probe hostnames to the setup AP. Only foreign
    // captive hostnames redirect; unknown paths on the canonical host remain
    // ordinary 404s.
    if (fromSetupAp() && !canonicalSetupHost() &&
            (server.method() == HTTP_GET || server.method() == HTTP_HEAD)) {
        return handleCaptiveProbe();
    }
    server.send(404, "text/plain", "Not found");
}

}  // namespace

void begin(configuration::ApplyCallback callback) {
    applyConfiguration = callback;
    const uint32_t tokenA = esp_random();
    const uint32_t tokenB = esp_random();
    snprintf(csrfToken, sizeof(csrfToken), "%08lX%08lX",
        static_cast<unsigned long>(tokenA), static_cast<unsigned long>(tokenB));
    filesystemReady = LittleFS.begin(false);
    loadFrontendBuildNumber();
    const char *requestHeaders[] = {
        "X-CSRF-Token",
        "Upgrade",
        "Connection",
        "Origin",
        "Sec-WebSocket-Key",
        "Sec-WebSocket-Version",
        "Cookie",
    };
    server.collectHeaders(requestHeaders, sizeof(requestHeaders) / sizeof(requestHeaders[0]));
    server.on("/", HTTP_GET, []() { serveFile("/index.html", "text/html; charset=utf-8"); });
    server.on("/style.css", HTTP_GET, []() { serveFile("/style.css", "text/css; charset=utf-8"); });
    server.on("/app.js", HTTP_GET, []() {
        serveFile("/app.js", "application/javascript; charset=utf-8");
    });
    server.on("/api/status", HTTP_GET, handleStatus);
    server.on("/api/auth", HTTP_GET, handleAuthGet);
    server.on("/api/auth/login", HTTP_POST, handleAuthLogin);
    server.on("/api/auth/logout", HTTP_POST, handleAuthLogout);
    server.on("/api/auth/password", HTTP_POST, handleAuthPassword);
    server.on("/api/config", HTTP_GET, handleConfigGet);
    server.on("/api/config", HTTP_POST, handleConfigPost);
    server.on("/api/wifi/scan", HTTP_POST, handleScanPost);
    server.on("/api/wifi/scan", HTTP_GET, handleScanGet);
    server.on("/generate_204", HTTP_ANY, handleCaptiveProbe);
    server.on("/gen_204", HTTP_ANY, handleCaptiveProbe);
    server.on("/hotspot-detect.html", HTTP_ANY, handleCaptiveProbe);
    server.on("/library/test/success.html", HTTP_ANY, handleCaptiveProbe);
    server.on("/connecttest.txt", HTTP_ANY, handleCaptiveProbe);
    server.on("/ncsi.txt", HTTP_ANY, handleCaptiveProbe);
    server.on("/redirect", HTTP_ANY, handleCaptiveProbe);
    server.on("/terminal", HTTP_GET, handleTerminal);
    server.onNotFound(handleNotFound);
    server.begin();
}

void service() {
    server.handleClient();
}

}  // namespace http_server
