#include "network_transport.h"

#include <WiFi.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

#include "configuration.h"
#include "display_history.h"
#include "serial_port.h"
#include "transport_buffer.h"

namespace network_transport {
namespace {

constexpr size_t kQueueCapacity = 32 * 1024;
constexpr size_t kChunkSize = 1024;
constexpr uint32_t kReconnectDelays[] = {1000, 2000, 5000, 10000};

uint8_t serialToNetworkStorage[kQueueCapacity];
uint8_t networkToSerialStorage[kQueueCapacity];
transport_buffer::Buffer serialToNetworkQueue;
transport_buffer::Buffer networkToSerialQueue;

portMUX_TYPE countersLock = portMUX_INITIALIZER_UNLOCKED;
uint64_t serialToNetworkReceived = 0;
uint64_t serialToNetworkForwarded = 0;
uint64_t networkToSerialReceived = 0;
uint64_t networkToSerialForwarded = 0;
ConnectionState connectionState = ConnectionState::Disabled;
bool tcpRetrying = false;
volatile uint32_t transportGeneration = 0;

portMUX_TYPE boundaryLock = portMUX_INITIALIZER_UNLOCKED;
volatile uint32_t transportBoundaryGeneration = 0;
volatile bool transportBoundaryActive = false;
bool networkIoInProgress = false;
bool serialIoInProgress = false;

uint32_t currentGeneration() {
    portENTER_CRITICAL(&boundaryLock);
    const uint32_t result = transportGeneration;
    portEXIT_CRITICAL(&boundaryLock);
    return result;
}

uint32_t currentBoundaryGeneration() {
    portENTER_CRITICAL(&boundaryLock);
    const uint32_t result = transportBoundaryGeneration;
    portEXIT_CRITICAL(&boundaryLock);
    return result;
}

bool boundaryActive() {
    portENTER_CRITICAL(&boundaryLock);
    const bool result = transportBoundaryActive;
    portEXIT_CRITICAL(&boundaryLock);
    return result;
}

bool beginNetworkIo(uint32_t workBoundaryGeneration) {
    portENTER_CRITICAL(&boundaryLock);
    const bool allowed = !transportBoundaryActive &&
        workBoundaryGeneration == transportBoundaryGeneration;
    if (allowed) networkIoInProgress = true;
    portEXIT_CRITICAL(&boundaryLock);
    return allowed;
}

void endNetworkIo() {
    portENTER_CRITICAL(&boundaryLock);
    networkIoInProgress = false;
    portEXIT_CRITICAL(&boundaryLock);
}

bool beginSerialIo(uint32_t workBoundaryGeneration) {
    portENTER_CRITICAL(&boundaryLock);
    const bool allowed = !transportBoundaryActive &&
        workBoundaryGeneration == transportBoundaryGeneration;
    if (allowed) serialIoInProgress = true;
    portEXIT_CRITICAL(&boundaryLock);
    return allowed;
}

void endSerialIo() {
    portENTER_CRITICAL(&boundaryLock);
    serialIoInProgress = false;
    portEXIT_CRITICAL(&boundaryLock);
}

void queueNetworkToSerial(
    const uint8_t *data,
    size_t length,
    uint32_t workBoundaryGeneration) {
    portENTER_CRITICAL(&boundaryLock);
    if (transportBoundaryActive ||
            workBoundaryGeneration != transportBoundaryGeneration) {
        transport_buffer::recordDropped(networkToSerialQueue, length);
    } else {
        transport_buffer::push(networkToSerialQueue, data, length);
        for (size_t i = 0; i < length; ++i) {
            display_history::append(data[i], display_history::Direction::NetworkToSerial);
        }
    }
    portEXIT_CRITICAL(&boundaryLock);
}

size_t popPending(
    transport_buffer::Buffer &queue,
    uint8_t *destination,
    size_t capacity,
    uint32_t &boundaryGeneration) {
    portENTER_CRITICAL(&boundaryLock);
    if (transportBoundaryActive) {
        portEXIT_CRITICAL(&boundaryLock);
        return 0;
    }
    boundaryGeneration = transportBoundaryGeneration;
    const size_t length = transport_buffer::pop(queue, destination, capacity);
    portEXIT_CRITICAL(&boundaryLock);
    return length;
}

void finishPendingSend(
    transport_buffer::Buffer &queue,
    uint8_t *pending,
    size_t &pendingLength,
    size_t written,
    uint32_t workBoundaryGeneration) {
    portENTER_CRITICAL(&boundaryLock);
    if (workBoundaryGeneration != transportBoundaryGeneration) {
        transport_buffer::recordDropped(queue, pendingLength - written);
        pendingLength = 0;
    } else if (written != 0) {
        memmove(pending, pending + written, pendingLength - written);
        pendingLength -= written;
    }
    portEXIT_CRITICAL(&boundaryLock);
}

void addSerialToNetworkReceived(size_t amount) {
    portENTER_CRITICAL(&countersLock);
    serialToNetworkReceived += amount;
    portEXIT_CRITICAL(&countersLock);
}

void addSerialToNetworkForwarded(size_t amount) {
    portENTER_CRITICAL(&countersLock);
    serialToNetworkForwarded += amount;
    portEXIT_CRITICAL(&countersLock);
}

void addNetworkToSerialReceived(size_t amount) {
    portENTER_CRITICAL(&countersLock);
    networkToSerialReceived += amount;
    portEXIT_CRITICAL(&countersLock);
}

void addNetworkToSerialForwarded(size_t amount) {
    portENTER_CRITICAL(&countersLock);
    networkToSerialForwarded += amount;
    portEXIT_CRITICAL(&countersLock);
}

void setConnectionState(ConnectionState state) {
    portENTER_CRITICAL(&countersLock);
    connectionState = state;
    portEXIT_CRITICAL(&countersLock);
}

void setTcpRetrying(bool retrying) {
    portENTER_CRITICAL(&countersLock);
    tcpRetrying = retrying;
    portEXIT_CRITICAL(&countersLock);
}

bool configuredEndpoint(const configuration::DeviceConfig &config) {
    return config.tcpHost[0] != '\0' && config.tcpPort != 0;
}

uint32_t reconnectDelay(size_t attempt) {
    const size_t delayIndex = min(attempt, sizeof(kReconnectDelays) / sizeof(kReconnectDelays[0]) - 1);
    return kReconnectDelays[delayIndex];
}

void networkTask(void *) {
    WiFiClient client;
    uint32_t seenGeneration = currentGeneration();
    uint32_t nextAttemptAt = 0;
    size_t attempt = 0;
    uint8_t chunk[kChunkSize];
    uint8_t pending[kChunkSize];
    size_t pendingLength = 0;
    uint32_t pendingBoundaryGeneration = 0;

    for (;;) {
        const uint32_t now = millis();
        const configuration::DeviceConfig config = configuration::snapshot();
        const uint32_t workBoundaryGeneration = currentBoundaryGeneration();

        if (pendingLength != 0 && pendingBoundaryGeneration != workBoundaryGeneration) {
            transport_buffer::recordDropped(serialToNetworkQueue, pendingLength);
            pendingLength = 0;
        }

        if (boundaryActive()) {
            client.stop();
            vTaskDelay(pdMS_TO_TICKS(1));
            continue;
        }

        if (seenGeneration != currentGeneration()) {
            client.stop();
            seenGeneration = currentGeneration();
            nextAttemptAt = now;
            attempt = 0;
        }

        if (!configuredEndpoint(config)) {
            client.stop();
            setTcpRetrying(false);
            setConnectionState(ConnectionState::Disabled);
            vTaskDelay(pdMS_TO_TICKS(50));
            continue;
        }

        if (WiFi.status() != WL_CONNECTED) {
            client.stop();
            setTcpRetrying(false);
            setConnectionState(ConnectionState::WaitingForWifi);
            vTaskDelay(pdMS_TO_TICKS(50));
            continue;
        }

        if (!client.connected()) {
            if (static_cast<int32_t>(now - nextAttemptAt) < 0) {
                setConnectionState(ConnectionState::Connecting);
                vTaskDelay(pdMS_TO_TICKS(20));
                continue;
            }

            setConnectionState(ConnectionState::Connecting);
            setTcpRetrying(false);
            // The TCP client is owned exclusively by this task. Configuration
            // changes signal an epoch; they never call client lifecycle APIs
            // or wait for a potentially blocking connect operation.
            if (client.connect(config.tcpHost, config.tcpPort)) {
                client.setNoDelay(true);
                setTcpRetrying(false);
                setConnectionState(ConnectionState::Connected);
                attempt = 0;
            } else {
                client.stop();
                nextAttemptAt = now + reconnectDelay(attempt++);
                setTcpRetrying(true);
                setConnectionState(ConnectionState::Connecting);
                vTaskDelay(pdMS_TO_TICKS(20));
                continue;
            }
        }

        if (seenGeneration != currentGeneration() ||
                workBoundaryGeneration != currentBoundaryGeneration()) {
            client.stop();
            vTaskDelay(pdMS_TO_TICKS(1));
            continue;
        }

        if (pendingLength == 0) {
            pendingLength = popPending(
                serialToNetworkQueue,
                pending,
                sizeof(pending),
                pendingBoundaryGeneration);
        }
        if (pendingLength != 0) {
            size_t written = 0;
            if (beginNetworkIo(pendingBoundaryGeneration)) {
                written = client.write(pending, pendingLength);
                endNetworkIo();
            }
            addSerialToNetworkForwarded(written);
            finishPendingSend(
                serialToNetworkQueue,
                pending,
                pendingLength,
                written,
                pendingBoundaryGeneration);
            if (pendingLength != 0 && !boundaryActive() && !client.connected()) client.stop();
        }

        const size_t availableSpace = transport_buffer::freeSpace(networkToSerialQueue);
        if (availableSpace != 0 && client.available() != 0) {
            const size_t toRead = min(availableSpace, sizeof(chunk));
            if (beginNetworkIo(workBoundaryGeneration)) {
                const int read = client.read(chunk, toRead);
                if (read > 0) {
                    const size_t received = static_cast<size_t>(read);
                    addNetworkToSerialReceived(received);
                    queueNetworkToSerial(chunk, received, workBoundaryGeneration);
                }
                endNetworkIo();
            }
        }

        if (!client.connected()) {
            client.stop();
            nextAttemptAt = millis() + reconnectDelay(attempt++);
            setTcpRetrying(true);
            setConnectionState(ConnectionState::Connecting);
        }

        vTaskDelay(pdMS_TO_TICKS(1));
    }
}

void serialTxTask(void *) {
    uint8_t chunk[kChunkSize];
    uint8_t pending[kChunkSize];
    size_t pendingLength = 0;
    uint32_t pendingBoundaryGeneration = 0;
    for (;;) {
        const uint32_t workBoundaryGeneration = currentBoundaryGeneration();
        if (pendingLength != 0 && pendingBoundaryGeneration != workBoundaryGeneration) {
            transport_buffer::recordDropped(networkToSerialQueue, pendingLength);
            pendingLength = 0;
        }
        if (pendingLength == 0) {
            pendingLength = popPending(
                networkToSerialQueue,
                pending,
                sizeof(pending),
                pendingBoundaryGeneration);
        }
        if (pendingLength != 0) {
            size_t written = 0;
            if (beginSerialIo(pendingBoundaryGeneration)) {
                written = serial_port::writeBytes(pending, pendingLength);
                endSerialIo();
            }
            addNetworkToSerialForwarded(written);
            finishPendingSend(
                networkToSerialQueue,
                pending,
                pendingLength,
                written,
                pendingBoundaryGeneration);
        }
        vTaskDelay(pdMS_TO_TICKS(1));
    }
}

}  // namespace

void begin() {
    transport_buffer::initialize(serialToNetworkQueue, serialToNetworkStorage, kQueueCapacity);
    transport_buffer::initialize(networkToSerialQueue, networkToSerialStorage, kQueueCapacity);
    xTaskCreatePinnedToCore(
        networkTask, "networkTask", 8192, nullptr, 2, nullptr, 0);
    xTaskCreatePinnedToCore(
        serialTxTask, "serialTxTask", 4096, nullptr, 2, nullptr, 0);
}

void serialBytesReceived(const uint8_t *data, size_t length) {
    if (data == nullptr || length == 0) return;
    addSerialToNetworkReceived(length);
    portENTER_CRITICAL(&boundaryLock);
    if (transportBoundaryActive) {
        transport_buffer::recordDropped(serialToNetworkQueue, length);
    } else {
        transport_buffer::push(serialToNetworkQueue, data, length);
    }
    portEXIT_CRITICAL(&boundaryLock);
}

void requestReconnect() {
    portENTER_CRITICAL(&boundaryLock);
    ++transportGeneration;
    portEXIT_CRITICAL(&boundaryLock);
}

void beginTransportBoundary() {
    portENTER_CRITICAL(&boundaryLock);
    transportBoundaryActive = true;
    ++transportGeneration;
    ++transportBoundaryGeneration;
    portEXIT_CRITICAL(&boundaryLock);

    // Let an already-started bounded UART/TCP operation finish before the
    // boundary clears its queue, so forwarded and dropped counters stay exact.
    while (true) {
        portENTER_CRITICAL(&boundaryLock);
        const bool ioActive = networkIoInProgress || serialIoInProgress;
        portEXIT_CRITICAL(&boundaryLock);
        if (!ioActive) break;
        delay(1);
    }

    portENTER_CRITICAL(&boundaryLock);
    transport_buffer::clear(serialToNetworkQueue);
    transport_buffer::clear(networkToSerialQueue);
    portEXIT_CRITICAL(&boundaryLock);
}

void endTransportBoundary() {
    portENTER_CRITICAL(&boundaryLock);
    // UART capture may have filled SER→NET while the old UART was being
    // stopped. Those bytes belong to the old serial settings and are dropped.
    transport_buffer::clear(serialToNetworkQueue);
    transport_buffer::clear(networkToSerialQueue);
    transportBoundaryActive = false;
    portEXIT_CRITICAL(&boundaryLock);
}

Snapshot snapshot() {
    Snapshot result{};
    portENTER_CRITICAL(&countersLock);
    result.state = connectionState;
    result.tcpRetrying = tcpRetrying;
    result.serialToNetworkReceived = serialToNetworkReceived;
    result.serialToNetworkForwarded = serialToNetworkForwarded;
    result.networkToSerialReceived = networkToSerialReceived;
    result.networkToSerialForwarded = networkToSerialForwarded;
    portEXIT_CRITICAL(&countersLock);
    result.serialToNetworkDropped = transport_buffer::droppedBytes(serialToNetworkQueue);
    result.networkToSerialDropped = transport_buffer::droppedBytes(networkToSerialQueue);
    result.serialToNetworkQueued = transport_buffer::available(serialToNetworkQueue);
    result.networkToSerialQueued = transport_buffer::available(networkToSerialQueue);
    return result;
}

}  // namespace network_transport
