#include "serial_port.h"

#include <Arduino.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

#include "display_history.h"

namespace serial_port {
namespace {

constexpr gpio_num_t kRxPin = GPIO_NUM_3;
constexpr gpio_num_t kTxPin = GPIO_NUM_1;

volatile bool captureEnabled = false;
bool uartReady = false;
bool captureInProgress = false;
bool writeInProgress = false;
bool receivedAny = false;
ReceiveCallback receiveCallback = nullptr;
portMUX_TYPE stateMux = portMUX_INITIALIZER_UNLOCKED;
uint32_t fifoOverflowErrors = 0;
uint32_t bufferOverflowErrors = 0;
uint32_t framingErrors = 0;
uint32_t parityErrors = 0;

void receiveHandler() {
    portENTER_CRITICAL(&stateMux);
    if (!captureEnabled) {
        portEXIT_CRITICAL(&stateMux);
        return;
    }
    captureInProgress = true;
    const ReceiveCallback callback = receiveCallback;
    portEXIT_CRITICAL(&stateMux);

    uint8_t received[256];
    size_t receivedLength = 0;
    // The callback must drain the driver buffer completely. HardwareSerial
    // dispatches one callback for a UART_DATA event; returning with bytes
    // still queued can strand them until a later event and overflow the RX
    // buffer during a burst.
    while (Serial.available() > 0) {
        const int value = Serial.read();
        if (value < 0) break;
        portENTER_CRITICAL(&stateMux);
        receivedAny = true;
        portEXIT_CRITICAL(&stateMux);
        display_history::append(
            static_cast<uint8_t>(value),
            display_history::Direction::SerialToNetwork);
        if (receivedLength < sizeof(received)) {
            received[receivedLength++] = static_cast<uint8_t>(value);
        }
        if (receivedLength == sizeof(received) && callback != nullptr) {
            callback(received, receivedLength);
            receivedLength = 0;
        }
    }
    if (callback != nullptr && receivedLength != 0) {
        callback(received, receivedLength);
    }
    portENTER_CRITICAL(&stateMux);
    captureInProgress = false;
    portEXIT_CRITICAL(&stateMux);
}

void errorHandler(hardwareSerial_error_t error) {
    portENTER_CRITICAL(&stateMux);
    switch (error) {
        case UART_FIFO_OVF_ERROR: ++fifoOverflowErrors; break;
        case UART_BUFFER_FULL_ERROR: ++bufferOverflowErrors; break;
        case UART_FRAME_ERROR: ++framingErrors; break;
        case UART_PARITY_ERROR: ++parityErrors; break;
        default: break;
    }
    portEXIT_CRITICAL(&stateMux);
}

void start(
    const configuration::DeviceConfig &config,
    bool clearInput,
    bool clearHistory) {
    portENTER_CRITICAL(&stateMux);
    captureEnabled = false;
    uartReady = false;
    portEXIT_CRITICAL(&stateMux);
    Serial.onReceive(nullptr, false);
    while (true) {
        portENTER_CRITICAL(&stateMux);
        const bool inProgress = captureInProgress || writeInProgress;
        portEXIT_CRITICAL(&stateMux);
        if (!inProgress) break;
        vTaskDelay(pdMS_TO_TICKS(1));
    }
    if (clearInput) {
        while (Serial.available() > 0) Serial.read();
    }
    if (clearHistory) {
        portENTER_CRITICAL(&stateMux);
        receivedAny = false;
        portEXIT_CRITICAL(&stateMux);
        display_history::clear();
    }
    // Waiting for writeBytes() only proves the UART accepted the bytes, not
    // that they left the wire, and Serial.end() detaches TX mid-frame. The
    // drain is a busy wait on this task, at most ~0.5 s of FIFO at 2400 baud.
    Serial.flush(true);
    Serial.end();
    Serial.setRxBufferSize(8192);
    Serial.begin(
        config.baud,
        configuration::serialConfig(static_cast<configuration::Framing>(config.framing)),
        kRxPin,
        kTxPin,
        false);
    // HardwareSerial::begin() is void; its bool conversion is the framework's
    // only driver-start result, so failed restarts must leave forwarding off.
    if (!Serial) return;
    Serial.setHwFlowCtrlMode(UART_HW_FLOWCTRL_DISABLE);
    Serial.onReceive(receiveHandler, false);
    Serial.onReceiveError(errorHandler);
    portENTER_CRITICAL(&stateMux);
    captureEnabled = true;
    uartReady = true;
    portEXIT_CRITICAL(&stateMux);
}

}  // namespace

void begin(const configuration::DeviceConfig &config) {
    portENTER_CRITICAL(&stateMux);
    receivedAny = false;
    fifoOverflowErrors = 0;
    bufferOverflowErrors = 0;
    framingErrors = 0;
    parityErrors = 0;
    portEXIT_CRITICAL(&stateMux);
    start(config, false, true);
}

bool reconfigure(const configuration::DeviceConfig &config) {
    start(config, true, true);
    portENTER_CRITICAL(&stateMux);
    const bool ready = uartReady;
    portEXIT_CRITICAL(&stateMux);
    return ready;
}

void setReceiveCallback(ReceiveCallback callback) {
    receiveCallback = callback;
}

size_t writeBytes(const uint8_t *data, size_t length) {
    if (data == nullptr || length == 0) {
        return 0;
    }
    portENTER_CRITICAL(&stateMux);
    if (!uartReady || writeInProgress) {
        portEXIT_CRITICAL(&stateMux);
        return 0;
    }
    writeInProgress = true;
    portEXIT_CRITICAL(&stateMux);

    // Reconfiguration marks the UART unavailable and waits for this bounded
    // operation before calling Serial.end()/begin(). No mutex is held across
    // Arduino UART I/O, so the state lock cannot delay the receive callback.
    const int available = Serial.availableForWrite();
    const size_t toWrite = available > 0 ?
        min(length, static_cast<size_t>(available)) : 0;
    const size_t written = toWrite == 0 ? 0 : Serial.write(data, toWrite);
    // The sole UART TX path is the one place that knows what the UART actually
    // accepted, and the only point both the transport queue and the browser
    // terminal pass through. Appending here, still inside writeInProgress,
    // keeps a reconfiguration's history clear from overtaking these bytes.
    for (size_t i = 0; i < written; ++i) {
        display_history::append(data[i], display_history::Direction::NetworkToSerial);
    }
    portENTER_CRITICAL(&stateMux);
    writeInProgress = false;
    portEXIT_CRITICAL(&stateMux);
    return written;
}

Snapshot snapshot() {
    Snapshot result{};
    portENTER_CRITICAL(&stateMux);
    result.trafficSeen = receivedAny;
    result.error = !uartReady;
    result.fifoOverflowErrors = fifoOverflowErrors;
    result.bufferOverflowErrors = bufferOverflowErrors;
    result.framingErrors = framingErrors;
    result.parityErrors = parityErrors;
    portEXIT_CRITICAL(&stateMux);
    return result;
}

}  // namespace serial_port
