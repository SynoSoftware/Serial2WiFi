#include <Arduino.h>

#include <cstring>

#include "browser_terminal.h"
#include "configuration.h"
#include "http_server.h"
#include "management_auth.h"
#include "network_transport.h"
#include "oled_display.h"
#include "prg_button.h"
#include "serial_port.h"
#include "wifi_access.h"

namespace {

bool pageActionStopped = false;

bool applyConfiguration(
    const configuration::DeviceConfig &previous,
    const configuration::DeviceConfig &next) {
    const bool serialChanged = previous.baud != next.baud || previous.framing != next.framing;
    const bool modeChanged = previous.tcpMode != next.tcpMode;
    const bool listenEndpointChanged = previous.tcpListenPort != next.tcpListenPort;
    const bool remoteEndpointChanged = previous.tcpRemotePort != next.tcpRemotePort ||
        strcmp(previous.tcpRemoteHost, next.tcpRemoteHost) != 0;
    const bool transportChanged = modeChanged ||
        (next.tcpMode == static_cast<uint8_t>(configuration::TcpMode::Listen) ?
            listenEndpointChanged : remoteEndpointChanged);
    const bool wifiChanged = previous.wifiSecurity != next.wifiSecurity ||
        strcmp(previous.ssid, next.ssid) != 0 ||
        strcmp(previous.wifiPassword, next.wifiPassword) != 0;

    if (previous.longPressMs != next.longPressMs) {
        prg_button::setLongPressMs(next.longPressMs);
    }
    if (previous.longPressRepeatMs != next.longPressRepeatMs) {
        prg_button::setLongPressRepeatMs(next.longPressRepeatMs);
    }

    bool serialApplied = true;
    if (serialChanged || transportChanged) network_transport::beginTransportBoundary();
    if (serialChanged) {
        // UART reconfiguration may invalidate writes. Accepted browser bytes
        // must finish through the old UART before that operation begins.
        network_transport::waitForTerminalTxDrain();
        serialApplied = serial_port::reconfigure(next);
    }
    if (serialChanged || transportChanged) network_transport::endTransportBoundary();
    if (wifiChanged) {
        wifi_access::configurationChanged(next);
        if (!serialChanged && !transportChanged) network_transport::requestReconnect();
    }
    return serialApplied;
}

bool factoryReset() {
    // A factory reset must recover the setup path, not merely forget the
    // station settings. Each module clears the NVS namespace it owns; this
    // deliberately leaves firmware and non-application flash data untouched.
    // Reboot is deliberately gated on PRG release. Applying the factory
    // configuration live here would block that result/release phase while
    // transport and Wi-Fi tear down and restart; the persisted defaults take
    // effect on the imminent reboot instead.
    const bool configurationCleared = configuration::factoryReset(nullptr);
    const bool identityCleared = wifi_access::clearIdentity();
    const bool passwordCleared = management_auth::clearPassword();
    return configurationCleared && identityCleared && passwordCleared;
}

void applyPageAction() {
    if (pageActionStopped) return;
    const oled_display::PageAction action = oled_display::currentPageAction();
    if (action == nullptr) return;

    configuration::DeviceConfig candidate = configuration::snapshot();
    action(candidate);
    bool runtimeApplied = false;
    if (!configuration::commit(candidate, applyConfiguration, &runtimeApplied) ||
            !runtimeApplied) {
        pageActionStopped = true;
        prg_button::reportSaveFailed();
    }
}

void serviceButtonActions() {
    switch (prg_button::takeEvent()) {
        case prg_button::Event::Click:
            if (oled_display::restoreIfHidden()) break;
            oled_display::advanceToNextPage(
                configuration::snapshot(), wifi_access::snapshot().setupApActive);
            break;
        case prg_button::Event::HoldStarted:
            // Clear before the wake test. A hold that only restores a hidden
            // view returns early, and a flag left set by the previous hold
            // would swallow every repeat in this one.
            pageActionStopped = false;
            if (oled_display::restoreIfHidden()) break;
            applyPageAction();
            break;
        case prg_button::Event::HoldRepeated:
            if (oled_display::restoreIfHidden()) break;
            applyPageAction();
            break;
        case prg_button::Event::ResetRequested:
            prg_button::reportFactoryReset(factoryReset());
            break;
        case prg_button::Event::RestartRequested:
            ESP.restart();
            break;
        case prg_button::Event::None:
            break;
    }
}

oled_display::RuntimeStatus runtimeStatus() {
    const wifi_access::Snapshot wifi = wifi_access::snapshot();
    const network_transport::Snapshot transport = network_transport::snapshot();
    const serial_port::Snapshot serial = serial_port::snapshot();
    oled_display::RuntimeStatus result{};
    result.setupApActive = wifi.setupApActive;
    result.stationConfigured = wifi.stationConfigured;
    result.stationConnected = wifi.stationConnected;
    result.stationRssi = wifi.stationRssi;
    strncpy(result.stationSsid, wifi.stationSsid, sizeof(result.stationSsid) - 1);
    result.tcpConnected = transport.state == network_transport::ConnectionState::Connected;
    result.serialError = serial.error;
    strncpy(result.setupSsid, wifi.setupSsid, sizeof(result.setupSsid) - 1);
    strncpy(result.setupPassword, wifi.setupPassword, sizeof(result.setupPassword) - 1);
    wifi.setupIp.toString().toCharArray(result.setupIp, sizeof(result.setupIp));
    result.stationOutcome = wifi.stationOutcome;
    wifi.stationIp.toString().toCharArray(result.stationIp, sizeof(result.stationIp));
    result.serialToNetworkReceived = transport.serialToNetworkReceived;
    result.serialToNetworkDropped = transport.serialToNetworkDropped;
    result.networkToSerialReceived = transport.networkToSerialReceived;
    result.networkToSerialDropped = transport.networkToSerialDropped;
    result.serialFifoOverflowErrors = serial.fifoOverflowErrors;
    result.serialBufferOverflowErrors = serial.bufferOverflowErrors;
    result.serialFramingErrors = serial.framingErrors;
    result.serialParityErrors = serial.parityErrors;
    return result;
}

}  // namespace

void setup() {
    configuration::begin();
    const configuration::DeviceConfig initialConfig = configuration::snapshot();
    prg_button::begin(initialConfig.longPressMs, initialConfig.longPressRepeatMs);
    wifi_access::begin(applyConfiguration);
    management_auth::begin();
    oled_display::begin();
    network_transport::begin();
    browser_terminal::begin();
    serial_port::setReceiveCallback(network_transport::serialBytesReceived);
    serial_port::begin(configuration::snapshot());
    http_server::begin(applyConfiguration);
}

void loop() {
    // The button is polled on its own task and the panel is drawn on another.
    // What is left here is every decision either of them produces, which is
    // what keeps the radio, the configuration and the carousel single-owned.
    serviceButtonActions();
    wifi_access::service();
    http_server::service();
    browser_terminal::service();
    if (oled_display::renderDue()) {
        // One read of the button, not two: the overlay and its countdown
        // describe the same moment of the same gesture.
        const prg_button::Status button = prg_button::status();
        oled_display::render(
            configuration::snapshot(),
            button.overlay,
            button.resetCountdown,
            serial_port::snapshot().trafficSeen,
            runtimeStatus());
    }
    delay(1);
}
