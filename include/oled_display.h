#pragma once

#include "configuration.h"
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
void handleShortClick(
    const configuration::DeviceConfig &config,
    bool setupApAvailable,
    bool serialTrafficSeen);
void noteUserInteraction();
PageAction currentPageAction();
void render(
    const configuration::DeviceConfig &config,
    prg_button::Overlay overlay,
    uint32_t resetCountdown,
    bool serialTrafficSeen,
    const RuntimeStatus &status);

}  // namespace oled_display
