#include <Arduino.h>

#include <cstring>

#include "browser_terminal.h"
#include "configuration.h"
#include "http_server.h"
#include "network_transport.h"
#include "oled_display.h"
#include "prg_button.h"
#include "serial_port.h"
#include "wifi_access.h"

namespace {

bool applyConfiguration(
    const configuration::DeviceConfig &previous,
    const configuration::DeviceConfig &next) {
    const bool serialChanged = previous.baud != next.baud || previous.framing != next.framing;
    const bool serverChanged = previous.tcpPort != next.tcpPort ||
        strcmp(previous.tcpHost, next.tcpHost) != 0;
    const bool wifiChanged = previous.wifiSecurity != next.wifiSecurity ||
        strcmp(previous.ssid, next.ssid) != 0 ||
        strcmp(previous.wifiPassword, next.wifiPassword) != 0;
    const bool setupApChanged =
        configuration::setupApEnabled(previous) != configuration::setupApEnabled(next);

    bool serialApplied = true;
    if (serialChanged || serverChanged) network_transport::beginTransportBoundary();
    if (serialChanged) {
        // UART reconfiguration may invalidate writes. Accepted browser bytes
        // must finish through the old UART before that operation begins.
        network_transport::waitForTerminalTxDrain();
        serialApplied = serial_port::reconfigure(next);
    }
    if (serialChanged || serverChanged) network_transport::endTransportBoundary();
    if (wifiChanged) {
        wifi_access::configurationChanged(next);
        if (!serialChanged && !serverChanged) network_transport::requestReconnect();
    }
    if (setupApChanged && !wifiChanged) {
        wifi_access::setSetupApEnabled(configuration::setupApEnabled(next));
    }
    return serialApplied;
}

bool commitLocalDisplayChange(const configuration::DeviceConfig &candidate) {
    bool runtimeApplied = false;
    const bool persisted = configuration::commit(candidate, applyConfiguration, &runtimeApplied);
    if (!persisted || !runtimeApplied) prg_button::reportSaveFailed();
    return persisted && runtimeApplied;
}

void applyDisplayMenuAction(oled_display::MenuAction action) {
    if (action == oled_display::MenuAction::None) return;

    configuration::DeviceConfig candidate = configuration::snapshot();
    if (action == oled_display::MenuAction::ToggleLiveView) {
        const configuration::LiveView view = configuration::liveView(candidate);
        const configuration::LiveView next = view == configuration::LiveView::Text ?
            configuration::LiveView::Hex : configuration::LiveView::Text;
        configuration::setLiveView(candidate, next);
        const auto mode = static_cast<configuration::DisplayMode>(candidate.display);
        if (mode == configuration::DisplayMode::Text || mode == configuration::DisplayMode::Hex) {
            candidate.display = static_cast<uint8_t>(configuration::liveDisplayMode(candidate));
        }
    } else if (action == oled_display::MenuAction::CycleStatusBar) {
        const uint8_t current = static_cast<uint8_t>(configuration::statusBar(candidate));
        configuration::setStatusBar(candidate, static_cast<configuration::StatusBar>((current + 1) % 4));
    } else if (action == oled_display::MenuAction::ToggleScreen) {
        const bool wasOff = configuration::screenOff(candidate);
        configuration::setScreenOff(candidate, !wasOff);
        if (wasOff && candidate.display == static_cast<uint8_t>(configuration::DisplayMode::Off)) {
            candidate.display = static_cast<uint8_t>(configuration::liveDisplayMode(candidate));
        }
    }
    commitLocalDisplayChange(candidate);
}

void toggleLiveAndStatistics() {
    configuration::DeviceConfig candidate = configuration::snapshot();
    if (configuration::screenOff(candidate)) return;
    const auto mode = static_cast<configuration::DisplayMode>(candidate.display);
    candidate.display = static_cast<uint8_t>(mode == configuration::DisplayMode::Stats ?
        configuration::liveDisplayMode(candidate) : configuration::DisplayMode::Stats);
    commitLocalDisplayChange(candidate);
}

void serviceButtonActions() {
    if (prg_button::takeSingleClick()) {
        if (oled_display::menuOpen()) {
            oled_display::moveMenuNext();
        } else {
            const configuration::DeviceConfig config = configuration::snapshot();
            if (configuration::screenOff(config)) {
                // The wake click is consumed: restoring the previous page is
                // less surprising than waking and advancing it in one action.
                applyDisplayMenuAction(oled_display::MenuAction::ToggleScreen);
            } else if (oled_display::setupPageActive(
                           config, serial_port::snapshot().trafficSeen)) {
                oled_display::advanceSetupPage();
            } else {
                toggleLiveAndStatistics();
            }
        }
    }

    prg_button::HoldEvent hold{};
    if (prg_button::takeHold(hold)) {
        if (oled_display::menuOpen()) {
            applyDisplayMenuAction(oled_display::selectMenuItem());
        } else {
            const configuration::DeviceConfig config = configuration::snapshot();
            const bool baudPage = oled_display::setupBaudPageActive(
                config, serial_port::snapshot().trafficSeen);
            if (baudPage && !hold.resetEligible) {
                configuration::DeviceConfig candidate = config;
                candidate.baud = configuration::nextBaud(candidate.baud);
                bool runtimeApplied = false;
                const bool persisted = configuration::commit(candidate, applyConfiguration, &runtimeApplied);
                prg_button::reportBaudCommit(persisted && runtimeApplied);
            } else if (!hold.resetEligible) {
                oled_display::openMenu();
            }
        }
    }

    if (prg_button::takeFactoryResetRequest()) {
        prg_button::reportFactoryReset(configuration::factoryReset(applyConfiguration));
    }

    if (prg_button::restartPending()) ESP.restart();

    // Process a click/hold before expiring the menu. Interaction at the
    // timeout boundary is still interaction and must refresh the menu timer.
    oled_display::serviceMenuTimeout(prg_button::isPressed());
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
    result.stationIp = wifi.stationIp;
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
    prg_button::begin();
    configuration::begin();
    wifi_access::begin();
    oled_display::begin();
    network_transport::begin();
    browser_terminal::begin();
    serial_port::setReceiveCallback(network_transport::serialBytesReceived);
    serial_port::begin(configuration::snapshot());
    http_server::begin(applyConfiguration);
    prg_button::bootComplete();
}

void loop() {
    prg_button::service();
    serviceButtonActions();
    wifi_access::service();
    http_server::service();
    browser_terminal::service();
    oled_display::render(
        configuration::snapshot(),
        prg_button::overlay(),
        prg_button::resetCountdown(),
        serial_port::snapshot().trafficSeen,
        runtimeStatus());
    delay(1);
}
