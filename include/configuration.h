#pragma once

#include <Arduino.h>

namespace configuration {

constexpr uint16_t kSchema = 1;
constexpr uint32_t kDefaultBaud = 9600;

enum class Framing : uint8_t {
    EightN1 = 0,
    EightN2,
    EightE1,
    EightO1,
    SevenE1,
    SevenO1,
};

enum class DisplayMode : uint8_t {
    Text = 0,
    Hex,
    Stats,
    Off,
};

enum class WifiSecurity : uint8_t {
    Unset = 0,
    Open,
    Secured,
};

#pragma pack(push, 1)
struct DeviceConfig {
    uint16_t schema;
    uint32_t baud;
    uint8_t framing;
    uint8_t display;
    uint8_t wifiSecurity;
    uint8_t reserved;
    char ssid[33];
    char wifiPassword[65];
    char tcpHost[254];
    uint16_t tcpPort;
};
#pragma pack(pop)

using ApplyCallback = void (*)(const DeviceConfig &previous, const DeviceConfig &next);

enum class ValidationError : uint8_t {
    None = 0,
    Schema,
    Baud,
    Framing,
    Display,
    WifiSsid,
    WifiSecurity,
    WifiPassword,
    TcpHost,
    TcpPort,
};

DeviceConfig factoryDefaults();
void begin();
DeviceConfig snapshot();
bool commit(const DeviceConfig &candidate, ApplyCallback apply);
bool factoryReset(ApplyCallback apply);
ValidationError validationError(const DeviceConfig &config);
bool validate(const DeviceConfig &config);

const char *framingName(Framing framing);
bool framingFromName(const char *name, Framing &framing);
uint32_t serialConfig(Framing framing);
bool supportedBaud(uint32_t baud);
uint32_t nextBaud(uint32_t current);

const char *displayModeName(DisplayMode mode);
bool displayModeFromName(const char *name, DisplayMode &mode);

}  // namespace configuration
