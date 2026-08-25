#include "http_server.h"

#include <Arduino.h>
#include <LittleFS.h>
#include <WebServer.h>
#include <cstdint>
#include <esp_system.h>

#include "network_transport.h"
#include "serial_port.h"
#include "wifi_access.h"

namespace http_server {
namespace {

WebServer server(80);
configuration::ApplyCallback applyConfiguration = nullptr;
char csrfToken[33]{};
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
        case network_transport::ConnectionState::Connecting: return "connecting";
        case network_transport::ConnectionState::Connected: return "connected";
    }
    return "disabled";
}

bool fromSetupAp() {
    return wifi_access::requestFromSetupAp(server.client());
}

bool csrfValid() {
    return server.header("X-CSRF-Token") == csrfToken;
}

void sendForbidden() {
    server.send(403, "application/json", "{\"error\":\"configuration_not_allowed\"}");
}

void sendJson(const String &body, int status = 200) {
    server.send(status, "application/json", body);
}

void handleStatus() {
    const configuration::DeviceConfig config = configuration::snapshot();
    const wifi_access::Snapshot wifi = wifi_access::snapshot();
    const network_transport::Snapshot transport = network_transport::snapshot();
    const serial_port::Snapshot serial = serial_port::snapshot();
    String body = "{";
    body += "\"configurationAllowed\":" + String(fromSetupAp() ? "true" : "false");
    body += ",\"wifiConfigured\":" + String(wifi.stationConfigured ? "true" : "false");
    body += ",\"wifiConnected\":" + String(wifi.stationConnected ? "true" : "false");
    body += ",\"wifiApActive\":" + String(wifi.apActive ? "true" : "false");
    body += ",\"setupSsid\":\"" + escaped(wifi.setupSsid) + "\"";
    body += ",\"stationIp\":\"" + escaped(ipString(wifi.stationIp)) + "\"";
    body += ",\"tcpState\":\"" + String(connectionName(transport.state)) + "\"";
    body += ",\"tcpRetrying\":" + String(transport.tcpRetrying ? "true" : "false");
    body += ",\"baud\":" + String(config.baud);
    body += ",\"framing\":\"" + String(configuration::framingName(
            static_cast<configuration::Framing>(config.framing))) + "\"";
    body += ",\"display\":\"" + String(configuration::displayModeName(
            static_cast<configuration::DisplayMode>(config.display))) + "\"";
    body += ",\"serialToNetworkReceived\":" + String(static_cast<unsigned long long>(transport.serialToNetworkReceived));
    body += ",\"serialToNetworkForwarded\":" + String(static_cast<unsigned long long>(transport.serialToNetworkForwarded));
    body += ",\"serialToNetworkDropped\":" + String(static_cast<unsigned long long>(transport.serialToNetworkDropped));
    body += ",\"networkToSerialReceived\":" + String(static_cast<unsigned long long>(transport.networkToSerialReceived));
    body += ",\"networkToSerialForwarded\":" + String(static_cast<unsigned long long>(transport.networkToSerialForwarded));
    body += ",\"networkToSerialDropped\":" + String(static_cast<unsigned long long>(transport.networkToSerialDropped));
    body += ",\"serialFifoOverflowErrors\":" + String(serial.fifoOverflowErrors);
    body += ",\"serialFramingErrors\":" + String(serial.framingErrors);
    body += ",\"serialParityErrors\":" + String(serial.parityErrors);
    body += ",\"serialError\":" + String(serial.error ? "true" : "false");
    body += "}";
    sendJson(body);
}

void handleConfigGet() {
    if (!fromSetupAp()) return sendForbidden();
    const configuration::DeviceConfig config = configuration::snapshot();
    String body = "{";
    body += "\"ssid\":\"" + escaped(config.ssid) + "\"";
    body += ",\"wifiSecurity\":\"" + String(securityName(
            static_cast<configuration::WifiSecurity>(config.wifiSecurity))) + "\"";
    body += ",\"wifiPasswordSaved\":" + String(config.wifiPassword[0] != '\0' ? "true" : "false");
    body += ",\"tcpHost\":\"" + escaped(config.tcpHost) + "\"";
    body += ",\"tcpPort\":" + String(config.tcpPort);
    body += ",\"baud\":" + String(config.baud);
    body += ",\"framing\":\"" + String(configuration::framingName(
            static_cast<configuration::Framing>(config.framing))) + "\"";
    body += ",\"display\":\"" + String(configuration::displayModeName(
            static_cast<configuration::DisplayMode>(config.display))) + "\"";
    body += ",\"csrfToken\":\"" + String(csrfToken) + "\"";
    body += "}";
    sendJson(body);
}

void configError(const char *field, const char *message) {
    String body = "{\"error\":\"" + String(message) + "\",\"field\":\"" + String(field) + "\"}";
    sendJson(body, 400);
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
        case configuration::ValidationError::Display:
            return configError("display", "invalid_display");
        case configuration::ValidationError::WifiSsid:
            return configError("ssid", "too_long");
        case configuration::ValidationError::WifiSecurity:
            return configError("wifiSecurity", "invalid_security");
        case configuration::ValidationError::WifiPassword:
            return configError("wifiPassword", "invalid_password");
        case configuration::ValidationError::TcpHost:
            return configError("tcpHost", "too_long");
        case configuration::ValidationError::TcpPort:
            return configError("tcpPort", "invalid_port");
    }
}

void handleConfigPost() {
    if (!fromSetupAp()) return sendForbidden();
    if (!csrfValid()) return sendJson("{\"error\":\"csrf\"}", 403);

    const configuration::DeviceConfig current = configuration::snapshot();
    configuration::DeviceConfig candidate = current;
    const String ssid = server.arg("ssid");
    const String securityValue = server.arg("wifiSecurity");
    const String password = server.arg("wifiPassword");
    const String host = server.arg("tcpHost");
    const String port = server.arg("tcpPort");
    const String baud = server.arg("baud");
    const String framing = server.arg("framing");
    const String display = server.arg("display");

    if (ssid.length() > 32) return configError("ssid", "too_long");
    if (password.length() > 64) return configError("wifiPassword", "too_long");
    if (host.length() > 253) return configError("tcpHost", "too_long");
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

    host.toCharArray(candidate.tcpHost, sizeof(candidate.tcpHost));
    uint32_t portNumber = 0;
    if (!parseUnsigned(port, portNumber) || portNumber > 65535) {
        return configError("tcpPort", "invalid_port");
    }
    candidate.tcpPort = static_cast<uint16_t>(portNumber);

    uint32_t baudNumber = 0;
    if (!parseUnsigned(baud, baudNumber)) return configError("baud", "invalid_baud");
    candidate.baud = baudNumber;
    configuration::Framing framingValue;
    if (!configuration::framingFromName(framing.c_str(), framingValue)) {
        return configError("framing", "invalid_framing");
    }
    candidate.framing = static_cast<uint8_t>(framingValue);
    configuration::DisplayMode displayValue;
    if (!configuration::displayModeFromName(display.c_str(), displayValue)) {
        return configError("display", "invalid_display");
    }
    candidate.display = static_cast<uint8_t>(displayValue);

    const configuration::ValidationError validation = configuration::validationError(candidate);
    if (validation != configuration::ValidationError::None) {
        return sendValidationError(validation);
    }
    if (!configuration::commit(candidate, applyConfiguration)) {
        return sendJson("{\"error\":\"save_failed\"}", 500);
    }
    sendJson("{\"ok\":true}");
}

void handleScanPost() {
    if (!fromSetupAp()) return sendForbidden();
    if (!csrfValid()) return sendJson("{\"error\":\"csrf\"}", 403);
    wifi_access::startScan();
    sendJson("{\"ok\":true}", 202);
}

void handleScanGet() {
    if (!fromSetupAp()) return sendForbidden();
    const wifi_access::ScanState state = wifi_access::scanState();
    const char *stateName = state == wifi_access::ScanState::Scanning ? "scanning" :
        state == wifi_access::ScanState::Ready ? "ready" :
        state == wifi_access::ScanState::Failed ? "failed" : "idle";
    wifi_access::ScanResult results[32];
    const size_t count = wifi_access::copyScanResults(results, 32);
    String body = "{\"state\":\"" + String(stateName) + "\",\"networks\":[";
    for (size_t i = 0; i < count; ++i) {
        if (i != 0) body += ',';
    body += "{\"ssid\":\"" + escaped(results[i].ssid) + "\",\"rssi\":" +
        String(results[i].rssi) + ",\"secured\":" +
        String(results[i].secured ? "true" : "false") + "}";
    }
    body += "]}";
    sendJson(body);
}

void serveFile(const char *path, const char *contentType) {
    if (!filesystemReady) return server.send(500, "text/plain", "LittleFS unavailable");
    File file = LittleFS.open(path, "r");
    if (!file) return server.send(404, "text/plain", "Not found");
    server.streamFile(file, contentType);
    file.close();
}

void handleNotFound() {
    if (server.method() == HTTP_GET && fromSetupAp()) return serveFile("/index.html", "text/html");
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
    const char *headers[] = {"X-CSRF-Token"};
    server.collectHeaders(headers, 1);
    server.on("/", HTTP_GET, []() { serveFile("/index.html", "text/html"); });
    server.on("/style.css", HTTP_GET, []() { serveFile("/style.css", "text/css"); });
    server.on("/app.js", HTTP_GET, []() { serveFile("/app.js", "application/javascript"); });
    server.on("/api/status", HTTP_GET, handleStatus);
    server.on("/api/config", HTTP_GET, handleConfigGet);
    server.on("/api/config", HTTP_POST, handleConfigPost);
    server.on("/api/wifi/scan", HTTP_POST, handleScanPost);
    server.on("/api/wifi/scan", HTTP_GET, handleScanGet);
    server.onNotFound(handleNotFound);
    server.begin();
}

void service() {
    server.handleClient();
}

}  // namespace http_server
