#include "serial_port.h"

#include <Arduino.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

#include "display_history.h"

namespace serial_port {
namespace {

constexpr gpio_num_t kRxPin = GPIO_NUM_3;
constexpr gpio_num_t kTxPin = GPIO_NUM_1;
constexpr size_t kMaxCallbackBytes = 1024;

volatile bool captureEnabled = false;
bool uartReady = false;
bool captureInProgress = false;
bool writeInProgress = false;
bool receivedAny = false;
ReceiveCallback receiveCallback = nullptr;
portMUX_TYPE stateMux = portMUX_INITIALIZER_UNLOCKED;
uint32_t fifoOverflowErrors = 0;
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
    size_t drained = 0;
    // Return after a bounded batch so the UART event task can schedule
    // transport work while the driver buffer continues receiving bytes.
    while (Serial.available() > 0 && drained < kMaxCallbackBytes) {
        const int value = Serial.read();
        if (value < 0) break;
        ++drained;
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
    if (!Serial) {
        portENTER_CRITICAL(&stateMux);
        writeInProgress = false;
        portEXIT_CRITICAL(&stateMux);
        return;
    }
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
    result.framingErrors = framingErrors;
    result.parityErrors = parityErrors;
    portEXIT_CRITICAL(&stateMux);
    return result;
}

}  // namespace serial_port
