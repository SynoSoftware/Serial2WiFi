#pragma once

#include "configuration.h"
#include "wifi_access.h"
#include "prg_button.h"

namespace oled_display {

struct RuntimeStatus {
    bool setupApActive;
    bool stationConfigured;
    bool stationConnected;
    int32_t stationRssi;
    char stationSsid[33];
    bool tcpConnected;
    bool serialError;
    char setupSsid[33];
    char setupPassword[17];
    wifi_access::StationState provisioningFailure;
    IPAddress setupIp;
    IPAddress stationIp;
    uint64_t serialToNetworkReceived;
    uint64_t serialToNetworkDropped;
    uint64_t networkToSerialReceived;
    uint64_t networkToSerialDropped;
    uint32_t serialFifoOverflowErrors;
    uint32_t serialBufferOverflowErrors;
    uint32_t serialFramingErrors;
    uint32_t serialParityErrors;
};

using PageAction = void (*)(configuration::DeviceConfig &candidate);

void begin();
void advanceToNextPage(
    const configuration::DeviceConfig &config,
    bool setupApAvailable);
// True when the selected page was not visible, so this press only restored the
// view and must not also act on it. Every press restarts the screen-saver timer,
// hidden or not, which the name does not convey.
bool restoreIfHidden();
void noteUserInteraction();
PageAction currentPageAction();
// True when render() would actually redraw. loop() checks this first so the
// status gathering that feeds render() runs at the refresh cadence, not on
// every loop pass.
bool renderDue();
void render(
    const configuration::DeviceConfig &config,
    prg_button::Overlay overlay,
    uint32_t resetCountdown,
    bool serialTrafficSeen,
    const RuntimeStatus &status);

}  // namespace oled_display
