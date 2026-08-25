#include "configuration.h"

#include <Preferences.h>
#include <cstring>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>

namespace configuration {
namespace {

Preferences preferences;
SemaphoreHandle_t configurationMutex = nullptr;
SemaphoreHandle_t commitMutex = nullptr;
DeviceConfig currentConfig{};

constexpr uint32_t kSupportedBauds[] = {
        2400, 4800, 9600, 19200, 38400, 57600,
        115200, 230400, 460800, 921600, 1000000};
constexpr size_t kSupportedBaudCount = sizeof(kSupportedBauds) / sizeof(kSupportedBauds[0]);

size_t boundedLength(const char *value, size_t capacity) {
    return strnlen(value, capacity);
}

bool readStored(DeviceConfig &config) {
    if (!preferences.begin("s2w", true)) return false;
    const size_t length = preferences.getBytesLength("cfg");
    if (length != sizeof(DeviceConfig)) {
        preferences.end();
        return false;
    }
    DeviceConfig stored{};
    const size_t read = preferences.getBytes("cfg", &stored, sizeof(stored));
    preferences.end();
    if (read != sizeof(DeviceConfig) ||
            validationError(stored) != ValidationError::None) return false;
    config = stored;
    return true;
}

}  // namespace

DeviceConfig factoryDefaults() {
    DeviceConfig config{};
    config.schema = kSchema;
    config.baud = kDefaultBaud;
    config.framing = static_cast<uint8_t>(Framing::EightN1);
    config.display = static_cast<uint8_t>(DisplayMode::Text);
    config.wifiSecurity = static_cast<uint8_t>(WifiSecurity::Unset);
    return config;
}

void begin() {
    configurationMutex = xSemaphoreCreateMutex();
    commitMutex = xSemaphoreCreateMutex();
    currentConfig = factoryDefaults();
    readStored(currentConfig);
}

DeviceConfig snapshot() {
    DeviceConfig result = factoryDefaults();
    if (configurationMutex != nullptr &&
            xSemaphoreTake(configurationMutex, portMAX_DELAY) == pdTRUE) {
        result = currentConfig;
        xSemaphoreGive(configurationMutex);
    }
    return result;
}

const char *framingName(Framing framing) {
    switch (framing) {
        case Framing::EightN1: return "8N1";
        case Framing::EightN2: return "8N2";
        case Framing::EightE1: return "8E1";
        case Framing::EightO1: return "8O1";
        case Framing::SevenE1: return "7E1";
        case Framing::SevenO1: return "7O1";
    }
    return "8N1";
}

bool framingFromName(const char *name, Framing &framing) {
    if (strcmp(name, "8N1") == 0) framing = Framing::EightN1;
    else if (strcmp(name, "8N2") == 0) framing = Framing::EightN2;
    else if (strcmp(name, "8E1") == 0) framing = Framing::EightE1;
    else if (strcmp(name, "8O1") == 0) framing = Framing::EightO1;
    else if (strcmp(name, "7E1") == 0) framing = Framing::SevenE1;
    else if (strcmp(name, "7O1") == 0) framing = Framing::SevenO1;
    else return false;
    return true;
}

uint32_t serialConfig(Framing framing) {
    switch (framing) {
        case Framing::EightN1: return SERIAL_8N1;
        case Framing::EightN2: return SERIAL_8N2;
        case Framing::EightE1: return SERIAL_8E1;
        case Framing::EightO1: return SERIAL_8O1;
        case Framing::SevenE1: return SERIAL_7E1;
        case Framing::SevenO1: return SERIAL_7O1;
    }
    return SERIAL_8N1;
}

bool supportedBaud(uint32_t baud) {
    for (uint32_t supported : kSupportedBauds) {
        if (supported == baud) return true;
    }
    return false;
}

uint32_t nextBaud(uint32_t current) {
    for (size_t i = 0; i < kSupportedBaudCount; ++i) {
        if (kSupportedBauds[i] == current) {
            return kSupportedBauds[(i + 1) % kSupportedBaudCount];
        }
    }
    return kSupportedBauds[0];
}

const char *displayModeName(DisplayMode mode) {
    switch (mode) {
        case DisplayMode::Text: return "text";
        case DisplayMode::Hex: return "hex";
        case DisplayMode::Stats: return "stats";
        case DisplayMode::Off: return "off";
    }
    return "text";
}

bool displayModeFromName(const char *name, DisplayMode &mode) {
    if (strcmp(name, "text") == 0) mode = DisplayMode::Text;
    else if (strcmp(name, "hex") == 0) mode = DisplayMode::Hex;
    else if (strcmp(name, "stats") == 0) mode = DisplayMode::Stats;
    else if (strcmp(name, "off") == 0) mode = DisplayMode::Off;
    else return false;
    return true;
}

ValidationError validationError(const DeviceConfig &candidate) {
    if (candidate.schema != kSchema) return ValidationError::Schema;
    if (!supportedBaud(candidate.baud)) return ValidationError::Baud;
    if (candidate.framing > static_cast<uint8_t>(Framing::SevenO1)) {
        return ValidationError::Framing;
    }
    if (candidate.display > static_cast<uint8_t>(DisplayMode::Off)) {
        return ValidationError::Display;
    }
    if (candidate.wifiSecurity > static_cast<uint8_t>(WifiSecurity::Secured)) {
        return ValidationError::WifiSecurity;
    }
    if (boundedLength(candidate.ssid, sizeof(candidate.ssid)) >= sizeof(candidate.ssid)) {
        return ValidationError::WifiSsid;
    }
    if (boundedLength(candidate.wifiPassword, sizeof(candidate.wifiPassword)) >=
            sizeof(candidate.wifiPassword)) {
        return ValidationError::WifiPassword;
    }
    if (boundedLength(candidate.tcpHost, sizeof(candidate.tcpHost)) >= sizeof(candidate.tcpHost)) {
        return ValidationError::TcpHost;
    }

    const bool wifiConfigured = candidate.ssid[0] != '\0';
    if (!wifiConfigured) {
        if (candidate.wifiPassword[0] != '\0') return ValidationError::WifiPassword;
        if (candidate.wifiSecurity != static_cast<uint8_t>(WifiSecurity::Unset)) {
            return ValidationError::WifiSecurity;
        }
    } else {
        if (candidate.wifiSecurity == static_cast<uint8_t>(WifiSecurity::Unset)) {
            return ValidationError::WifiSecurity;
        }
        if (candidate.wifiSecurity == static_cast<uint8_t>(WifiSecurity::Secured) &&
                candidate.wifiPassword[0] == '\0') return ValidationError::WifiPassword;
        if (candidate.wifiSecurity == static_cast<uint8_t>(WifiSecurity::Open) &&
                candidate.wifiPassword[0] != '\0') return ValidationError::WifiPassword;
    }

    const bool tcpConfigured = candidate.tcpHost[0] != '\0';
    if (!tcpConfigured && candidate.tcpPort != 0) return ValidationError::TcpPort;
    if (tcpConfigured && candidate.tcpPort == 0) return ValidationError::TcpPort;
    return ValidationError::None;
}

bool validate(const DeviceConfig &candidate) {
    return validationError(candidate) == ValidationError::None;
}

bool commit(const DeviceConfig &candidate, ApplyCallback apply) {
    if (configurationMutex == nullptr || commitMutex == nullptr ||
            xSemaphoreTake(commitMutex, portMAX_DELAY) != pdTRUE) {
        return false;
    }

    bool committed = false;
    DeviceConfig previous{};
    if (validate(candidate) && preferences.begin("s2w", false)) {
        const size_t written = preferences.putBytes("cfg", &candidate, sizeof(candidate));
        preferences.end();
        if (written == sizeof(candidate)) {
            if (xSemaphoreTake(configurationMutex, portMAX_DELAY) != pdTRUE) {
                xSemaphoreGive(commitMutex);
                return false;
            }
            previous = currentConfig;
            currentConfig = candidate;
            xSemaphoreGive(configurationMutex);
            committed = true;
        }
    }

    if (committed && apply != nullptr) {
        // The commit lock serializes runtime transitions, while the state lock
        // is released before UART, Wi-Fi, or transport work begins.
        apply(previous, candidate);
    }
    xSemaphoreGive(commitMutex);
    return committed;
}

bool factoryReset(ApplyCallback apply) {
    if (configurationMutex == nullptr || commitMutex == nullptr ||
            xSemaphoreTake(commitMutex, portMAX_DELAY) != pdTRUE) {
        return false;
    }

    bool reset = false;
    if (preferences.begin("s2w", false)) {
        reset = preferences.clear();
        preferences.end();
        if (reset) {
            if (xSemaphoreTake(configurationMutex, portMAX_DELAY) != pdTRUE) {
                xSemaphoreGive(commitMutex);
                return false;
            }
            const DeviceConfig previous = currentConfig;
            const DeviceConfig next = factoryDefaults();
            currentConfig = next;
            xSemaphoreGive(configurationMutex);
            if (apply != nullptr) apply(previous, next);
        }
    }

    xSemaphoreGive(commitMutex);
    return reset;
}

}  // namespace configuration
