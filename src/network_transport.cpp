#include "network_transport.h"

#include <WiFi.h>
#include <cstring>
#include <errno.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <lwip/dns.h>
#include <lwip/sockets.h>

#include "configuration.h"
#include "display_history.h"
#include "browser_terminal.h"
#include "serial_port.h"
#include "transport_buffer.h"

namespace network_transport {
namespace {

constexpr size_t kQueueCapacity = 32 * 1024;
constexpr size_t kChunkSize = 1024;
// A browser frame is admitted only when it fits; this bounds terminal TX
// without dropping the suffix of an admitted frame.
constexpr size_t kTerminalTxCapacity = 1024;
constexpr uint32_t kReconnectDelays[] = {1000, 2000, 5000, 10000};

uint8_t serialToNetworkStorage[kQueueCapacity];
uint8_t networkToSerialStorage[kQueueCapacity];
uint8_t terminalTxStorage[kTerminalTxCapacity];
transport_buffer::Buffer serialToNetworkQueue;
transport_buffer::Buffer networkToSerialQueue;
transport_buffer::Buffer terminalTxQueue;

portMUX_TYPE countersLock = portMUX_INITIALIZER_UNLOCKED;
uint64_t serialToNetworkReceived = 0;
uint64_t serialToNetworkForwarded = 0;
uint64_t networkToSerialReceived = 0;
uint64_t networkToSerialForwarded = 0;
uint64_t terminalToSerialReceived = 0;
ConnectionState connectionState = ConnectionState::Disabled;
bool tcpRetrying = false;
bool taskStartError = false;
volatile uint32_t transportGeneration = 0;

portMUX_TYPE boundaryLock = portMUX_INITIALIZER_UNLOCKED;
volatile uint32_t transportBoundaryGeneration = 0;
volatile bool transportBoundaryActive = false;
bool transportBoundaryReleaseRequested = false;
bool networkIoInProgress = false;
bool serialIoInProgress = false;
bool terminalTxInProgress = false;
bool tcpConnectionWaitingForTerminalTx = false;

portMUX_TYPE dnsLock = portMUX_INITIALIZER_UNLOCKED;
bool dnsInProgress = false;
bool dnsComplete = false;
bool dnsSucceeded = false;
char dnsHostname[254]{};
ip_addr_t dnsAddress{};

void completeTransportBoundaryIfReady();

void dnsFound(
    const char *,
    const ip_addr_t *address,
    void *) {
    portENTER_CRITICAL(&dnsLock);
    if (dnsInProgress) {
        dnsSucceeded = address != nullptr;
        if (address != nullptr) dnsAddress = *address;
        dnsComplete = true;
        dnsInProgress = false;
    }
    portEXIT_CRITICAL(&dnsLock);
}

void startDnsLookup(const char *hostname) {
    strncpy(dnsHostname, hostname, sizeof(dnsHostname) - 1);
    dnsHostname[sizeof(dnsHostname) - 1] = '\0';
    portENTER_CRITICAL(&dnsLock);
    dnsInProgress = true;
    dnsComplete = false;
    dnsSucceeded = false;
    portEXIT_CRITICAL(&dnsLock);

    ip_addr_t address{};
    const err_t result = dns_gethostbyname(
        dnsHostname,
        &address,
        dnsFound,
        nullptr);

    portENTER_CRITICAL(&dnsLock);
    if (result == ERR_OK) {
        dnsAddress = address;
        dnsSucceeded = true;
        dnsComplete = true;
        dnsInProgress = false;
    } else if (result != ERR_INPROGRESS && dnsInProgress) {
        dnsSucceeded = false;
        dnsComplete = true;
        dnsInProgress = false;
    }
    portEXIT_CRITICAL(&dnsLock);
}

bool takeDnsResult(IPAddress &address, bool &succeeded) {
    portENTER_CRITICAL(&dnsLock);
    if (!dnsComplete) {
        portEXIT_CRITICAL(&dnsLock);
        return false;
    }
    succeeded = dnsSucceeded;
    if (succeeded) address = dnsAddress.u_addr.ip4.addr;
    dnsComplete = false;
    portEXIT_CRITICAL(&dnsLock);
    return true;
}

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

bool beginNetworkIo(uint32_t workBoundaryGeneration, bool connecting) {
    portENTER_CRITICAL(&boundaryLock);
    const bool transportReady = !transportBoundaryActive &&
        workBoundaryGeneration == transportBoundaryGeneration &&
        (!connecting || (!terminalTxInProgress &&
            transport_buffer::available(terminalTxQueue) == 0));
    if (connecting && !transportReady && !transportBoundaryActive &&
            workBoundaryGeneration == transportBoundaryGeneration &&
            (terminalTxInProgress || transport_buffer::available(terminalTxQueue) != 0)) {
        // Close admission after this attempt observes already-accepted browser
        // bytes. This prevents new browser traffic from starving TCP forever.
        tcpConnectionWaitingForTerminalTx = true;
    }
    const bool allowed = transportReady;
    if (allowed) networkIoInProgress = true;
    if (allowed && connecting) tcpConnectionWaitingForTerminalTx = false;
    portEXIT_CRITICAL(&boundaryLock);
    return allowed;
}

bool connectionWaitingForTerminalTx() {
    portENTER_CRITICAL(&boundaryLock);
    const bool result = tcpConnectionWaitingForTerminalTx;
    portEXIT_CRITICAL(&boundaryLock);
    return result;
}

void endNetworkIo() {
    portENTER_CRITICAL(&boundaryLock);
    networkIoInProgress = false;
    portEXIT_CRITICAL(&boundaryLock);
    completeTransportBoundaryIfReady();
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
    completeTransportBoundaryIfReady();
}

void completeTransportBoundaryIfReady() {
    portENTER_CRITICAL(&boundaryLock);
    if (transportBoundaryActive && transportBoundaryReleaseRequested &&
            !networkIoInProgress && !serialIoInProgress && !terminalTxInProgress) {
        transport_buffer::clear(serialToNetworkQueue);
        transport_buffer::clear(networkToSerialQueue);
        transportBoundaryActive = false;
        transportBoundaryReleaseRequested = false;
    }
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

void addTerminalToSerialReceived(size_t amount) {
    if (amount == 0) return;
    portENTER_CRITICAL(&countersLock);
    terminalToSerialReceived += amount;
    portEXIT_CRITICAL(&countersLock);
}

void setConnectionState(ConnectionState state) {
    portENTER_CRITICAL(&boundaryLock);
    connectionState = state;
    portEXIT_CRITICAL(&boundaryLock);
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
    bool dnsRequested = false;
    uint32_t dnsRequestGeneration = 0;

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
            portENTER_CRITICAL(&boundaryLock);
            tcpConnectionWaitingForTerminalTx = false;
            portEXIT_CRITICAL(&boundaryLock);
            setTcpRetrying(false);
            setConnectionState(ConnectionState::Disabled);
            vTaskDelay(pdMS_TO_TICKS(50));
            continue;
        }

        if (WiFi.status() != WL_CONNECTED) {
            client.stop();
            portENTER_CRITICAL(&boundaryLock);
            tcpConnectionWaitingForTerminalTx = false;
            portEXIT_CRITICAL(&boundaryLock);
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
            IPAddress serverAddress;
            bool addressReady = serverAddress.fromString(config.tcpHost);
            if (!addressReady && !dnsRequested) {
                dnsRequestGeneration = currentGeneration();
                startDnsLookup(config.tcpHost);
                dnsRequested = true;
            }
            if (!addressReady && dnsRequested) {
                bool lookupSucceeded = false;
                if (!takeDnsResult(serverAddress, lookupSucceeded)) {
                    vTaskDelay(pdMS_TO_TICKS(1));
                    continue;
                }
                dnsRequested = false;
                if (!lookupSucceeded || dnsRequestGeneration != currentGeneration()) {
                    client.stop();
                    nextAttemptAt = now + reconnectDelay(attempt++);
                    setTcpRetrying(true);
                    setConnectionState(ConnectionState::Connecting);
                    vTaskDelay(pdMS_TO_TICKS(20));
                    continue;
                }
                addressReady = true;
            }
            if (!addressReady) {
                vTaskDelay(pdMS_TO_TICKS(1));
                continue;
            }

            // The TCP client is owned exclusively by this task. Mark the
            // connection attempt as network I/O so a configuration boundary
            // cannot release while connect is using the client.
            bool connected = false;
            if (beginNetworkIo(workBoundaryGeneration, true)) {
                connected = client.connect(serverAddress, config.tcpPort, 1000);
                if (connected) {
                    client.setNoDelay(true);
                    // Publish ownership before releasing the connect guard.
                    // Browser TX cannot enter between a connected socket and
                    // the state that grants TCP exclusive UART ownership.
                    setTcpRetrying(false);
                    setConnectionState(ConnectionState::Connected);
                    attempt = 0;
                }
                endNetworkIo();
            }
            if (!connected) {
                if (connectionWaitingForTerminalTx()) {
                    vTaskDelay(pdMS_TO_TICKS(1));
                    continue;
                }
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
            if (beginNetworkIo(pendingBoundaryGeneration, false)) {
                const int socketDescriptor = client.fd();
                if (socketDescriptor >= 0) {
                    // WiFiClient::write() can wait up to ten one-second
                    // select intervals when the peer stops accepting data.
                    // A non-blocking send preserves partial-write handling
                    // without stalling UART servicing or a configuration
                    // boundary.
                    const ssize_t result = send(
                        socketDescriptor, pending, pendingLength, MSG_DONTWAIT);
                    if (result > 0) {
                        written = static_cast<size_t>(result);
                    } else if (result < 0 && errno != EAGAIN && errno != EWOULDBLOCK) {
                        client.stop();
                    }
                }
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
            if (beginNetworkIo(workBoundaryGeneration, false)) {
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
    uint8_t pending[kChunkSize];
    uint8_t terminalPending[kTerminalTxCapacity];
    size_t pendingLength = 0;
    uint32_t pendingBoundaryGeneration = 0;
    size_t terminalPendingLength = 0;
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

        if (terminalPendingLength == 0) {
            portENTER_CRITICAL(&boundaryLock);
            if (!networkIoInProgress &&
                    transport_buffer::available(terminalTxQueue) != 0) {
                terminalPendingLength = transport_buffer::pop(
                    terminalTxQueue,
                    terminalPending,
                    sizeof(terminalPending));
                terminalTxInProgress = true;
            }
            portEXIT_CRITICAL(&boundaryLock);
        }

        if (terminalPendingLength != 0) {
            size_t written = 0;
            // Boundary admission is closed, but these bytes were accepted
            // before it began and must finish through the sole UART TX path.
            written = serial_port::writeBytes(terminalPending, terminalPendingLength);
            addTerminalToSerialReceived(written);
            if (written != 0) {
                memmove(
                    terminalPending,
                    terminalPending + written,
                    terminalPendingLength - written);
                terminalPendingLength -= written;
            }
            if (terminalPendingLength == 0) {
                portENTER_CRITICAL(&boundaryLock);
                terminalTxInProgress = false;
                portEXIT_CRITICAL(&boundaryLock);
                completeTransportBoundaryIfReady();
            }
        }
        vTaskDelay(pdMS_TO_TICKS(1));
    }
}

}  // namespace

void begin() {
    transport_buffer::initialize(serialToNetworkQueue, serialToNetworkStorage, kQueueCapacity);
    transport_buffer::initialize(networkToSerialQueue, networkToSerialStorage, kQueueCapacity);
    transport_buffer::initialize(terminalTxQueue, terminalTxStorage, kTerminalTxCapacity);
    const BaseType_t networkTaskResult = xTaskCreatePinnedToCore(
        networkTask, "networkTask", 8192, nullptr, 2, nullptr, 0);
    const BaseType_t serialTaskResult = xTaskCreatePinnedToCore(
        serialTxTask, "serialTxTask", 4096, nullptr, 2, nullptr, 0);
    taskStartError = networkTaskResult != pdPASS || serialTaskResult != pdPASS;
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
    browser_terminal::onSerialData(data, length);
}

bool submitTerminalToSerial(const uint8_t *data, size_t length) {
    if (data == nullptr || length == 0 || length > kTerminalTxCapacity) return false;

    portENTER_CRITICAL(&boundaryLock);
    const bool allowed = !transportBoundaryActive &&
        !networkIoInProgress && !tcpConnectionWaitingForTerminalTx &&
        connectionState != ConnectionState::Connected &&
        transport_buffer::pushIfFits(terminalTxQueue, data, length);
    portEXIT_CRITICAL(&boundaryLock);
    return allowed;
}

void requestReconnect() {
    portENTER_CRITICAL(&boundaryLock);
    ++transportGeneration;
    portEXIT_CRITICAL(&boundaryLock);
}

void beginTransportBoundary() {
    portENTER_CRITICAL(&boundaryLock);
    transportBoundaryActive = true;
    transportBoundaryReleaseRequested = false;
    ++transportGeneration;
    ++transportBoundaryGeneration;
    tcpConnectionWaitingForTerminalTx = false;
    transport_buffer::clear(serialToNetworkQueue);
    transport_buffer::clear(networkToSerialQueue);
    portEXIT_CRITICAL(&boundaryLock);
}

void waitForTerminalTxDrain() {
    for (;;) {
        portENTER_CRITICAL(&boundaryLock);
        const bool pending = terminalTxInProgress ||
            transport_buffer::available(terminalTxQueue) != 0;
        portEXIT_CRITICAL(&boundaryLock);
        if (!pending) return;
        delay(1);
    }
}

void endTransportBoundary() {
    portENTER_CRITICAL(&boundaryLock);
    // UART capture may have filled SER→NET while the old UART was being
    // stopped. Those bytes belong to the old serial settings and are dropped.
    transportBoundaryReleaseRequested = true;
    if (!networkIoInProgress && !serialIoInProgress && !terminalTxInProgress) {
        transport_buffer::clear(serialToNetworkQueue);
        transport_buffer::clear(networkToSerialQueue);
        transportBoundaryActive = false;
        transportBoundaryReleaseRequested = false;
    }
    portEXIT_CRITICAL(&boundaryLock);
}

Snapshot snapshot() {
    Snapshot result{};
    portENTER_CRITICAL(&boundaryLock);
    result.state = connectionState;
    portEXIT_CRITICAL(&boundaryLock);
    portENTER_CRITICAL(&countersLock);
    result.tcpRetrying = tcpRetrying;
    result.taskStartError = taskStartError;
    result.serialToNetworkReceived = serialToNetworkReceived;
    result.serialToNetworkForwarded = serialToNetworkForwarded;
    result.networkToSerialReceived = networkToSerialReceived;
    result.networkToSerialForwarded = networkToSerialForwarded;
    result.terminalToSerialReceived = terminalToSerialReceived;
    portEXIT_CRITICAL(&countersLock);
    result.serialToNetworkDropped = transport_buffer::droppedBytes(serialToNetworkQueue);
    result.networkToSerialDropped = transport_buffer::droppedBytes(networkToSerialQueue);
    result.serialToNetworkQueued = transport_buffer::available(serialToNetworkQueue);
    result.networkToSerialQueued = transport_buffer::available(networkToSerialQueue);
    return result;
}

}  // namespace network_transport
