#pragma once

#include <Arduino.h>

namespace network_transport {

enum class ConnectionState : uint8_t {
    Disabled = 0,
    WaitingForWifi,
    Connecting,
    Connected,
};

struct Snapshot {
    ConnectionState state;
    bool tcpRetrying;
    bool taskStartError;
    uint64_t serialToNetworkReceived;
    uint64_t serialToNetworkForwarded;
    uint64_t serialToNetworkDropped;
    size_t serialToNetworkQueued;
    uint64_t networkToSerialReceived;
    uint64_t networkToSerialForwarded;
    uint64_t networkToSerialDropped;
    size_t networkToSerialQueued;
    uint64_t terminalToSerialReceived;
};

void begin();
void serialBytesReceived(const uint8_t *data, size_t length);
bool submitTerminalToSerial(const uint8_t *data, size_t length);
void requestReconnect();
void beginTransportBoundary();
void waitForTerminalTxDrain();
void endTransportBoundary();
Snapshot snapshot();

}  // namespace network_transport
