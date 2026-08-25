#pragma once

#include "configuration.h"

namespace serial_port {

using ReceiveCallback = void (*)(const uint8_t *data, size_t length);

struct Snapshot {
    bool trafficSeen;
    bool error;
    uint32_t fifoOverflowErrors;
    uint32_t framingErrors;
    uint32_t parityErrors;
};

void begin(const configuration::DeviceConfig &config);
bool reconfigure(const configuration::DeviceConfig &config);
void setReceiveCallback(ReceiveCallback callback);
size_t writeBytes(const uint8_t *data, size_t length);
Snapshot snapshot();

}  // namespace serial_port
