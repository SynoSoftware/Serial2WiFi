#include <Arduino.h>

#include <cstring>

#include "configuration.h"
#include "http_server.h"
#include "network_transport.h"
#include "oled_display.h"
#include "prg_button.h"
#include "serial_port.h"
#include "wifi_access.h"

namespace {

void applyConfiguration(
    const configuration::DeviceConfig &previous,
    const configuration::DeviceConfig &next) {
    const bool serialChanged = previous.baud != next.baud || previous.framing != next.framing;
    const bool serverChanged = previous.tcpPort != next.tcpPort ||
        strcmp(previous.tcpHost, next.tcpHost) != 0;
    const bool wifiChanged = previous.wifiSecurity != next.wifiSecurity ||
        strcmp(previous.ssid, next.ssid) != 0 ||
        strcmp(previous.wifiPassword, next.wifiPassword) != 0;

    if (serialChanged || serverChanged) network_transport::beginTransportBoundary();
    if (serialChanged) {
        serial_port::reconfigure(next);
    }
    if (serialChanged || serverChanged) network_transport::endTransportBoundary();
    if (wifiChanged) {
        wifi_access::configurationChanged(next);
        if (!serialChanged && !serverChanged) network_transport::requestReconnect();
    }
}

void serviceButtonActions() {
    if (prg_button::takeShortTap()) {
        configuration::DeviceConfig candidate = configuration::snapshot();
        candidate.baud = configuration::nextBaud(candidate.baud);
        prg_button::reportBaudCommit(configuration::commit(candidate, applyConfiguration));
    }

    if (prg_button::takeFactoryResetRequest()) {
        prg_button::reportFactoryReset(configuration::factoryReset(applyConfiguration));
    }

    if (prg_button::restartPending()) ESP.restart();
}

oled_display::RuntimeStatus runtimeStatus() {
    const wifi_access::Snapshot wifi = wifi_access::snapshot();
    const network_transport::Snapshot transport = network_transport::snapshot();
    const serial_port::Snapshot serial = serial_port::snapshot();
    oled_display::RuntimeStatus result{};
    result.apActive = wifi.apActive;
    result.stationConnected = wifi.stationConnected;
    result.tcpConnected = transport.state == network_transport::ConnectionState::Connected;
    result.serialError = serial.error;
    strncpy(result.setupSsid, wifi.setupSsid, sizeof(result.setupSsid) - 1);
    strncpy(result.setupPassword, wifi.setupPassword, sizeof(result.setupPassword) - 1);
    result.apIp = wifi.apIp;
    result.stationIp = wifi.stationIp;
    result.serialToNetworkReceived = transport.serialToNetworkReceived;
    result.serialToNetworkDropped = transport.serialToNetworkDropped;
    result.networkToSerialReceived = transport.networkToSerialReceived;
    result.networkToSerialDropped = transport.networkToSerialDropped;
    result.serialFifoOverflowErrors = serial.fifoOverflowErrors;
    result.serialFramingErrors = serial.framingErrors;
    result.serialParityErrors = serial.parityErrors;
    return result;
}

}  // namespace

void setup() {
    prg_button::begin();
    configuration::begin();
    wifi_access::begin();
    oled_display::begin();
    network_transport::begin();
    serial_port::setReceiveCallback(network_transport::serialBytesReceived);
    serial_port::begin(configuration::snapshot());
    http_server::begin(applyConfiguration);
}

void loop() {
    prg_button::service();
    serviceButtonActions();
    wifi_access::service();
    http_server::service();
    oled_display::render(
        configuration::snapshot(),
        prg_button::overlay(),
        prg_button::resetCountdown(),
        serial_port::snapshot().trafficSeen,
        runtimeStatus());
    delay(1);
}
