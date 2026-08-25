#pragma once

#include "configuration.h"
#include "prg_button.h"

namespace oled_display {

struct RuntimeStatus {
    bool apActive;
    bool stationConnected;
    bool tcpConnected;
    bool serialError;
    char setupSsid[33];
    char setupPassword[17];
    IPAddress apIp;
    IPAddress stationIp;
    uint64_t serialToNetworkReceived;
    uint64_t serialToNetworkDropped;
    uint64_t networkToSerialReceived;
    uint64_t networkToSerialDropped;
    uint32_t serialFifoOverflowErrors;
    uint32_t serialFramingErrors;
    uint32_t serialParityErrors;
};

void begin();
void render(
    const configuration::DeviceConfig &config,
    prg_button::Overlay overlay,
    uint32_t resetCountdown,
    bool serialTrafficSeen,
    const RuntimeStatus &status);

}  // namespace oled_display
