#pragma once

#include <Arduino.h>

namespace configuration {

constexpr uint16_t kSchema = 7;
constexpr uint16_t kHttpPort = 80;
constexpr uint32_t kDefaultBaud = 19200;
constexpr uint32_t kDefaultLongPressMs = 250;
constexpr uint32_t kDefaultLongPressRepeatMs = 500;
constexpr uint32_t kDefaultScreenSaverSeconds = 60;
constexpr uint32_t kMinimumLongPressMs = 100;
constexpr uint32_t kMaximumLongPressMs = 1000;
constexpr uint32_t kMinimumLongPressRepeatMs = 250;
constexpr uint32_t kMaximumLongPressRepeatMs = 1000;
constexpr uint32_t kMinimumScreenSaverSeconds = 5;

enum class Framing : uint8_t {
    EightN1 = 0,
    EightN2,
    EightE1,
    EightO1,
    SevenE1,
    SevenO1,
};

enum class WifiSecurity : uint8_t {
    Unset = 0,
    Open,
    Secured,
};

enum class TcpMode : uint8_t {
    Listen = 0,
    Connect,
};

#pragma pack(push, 1)
struct DeviceConfig {
    uint16_t schema;
    uint32_t baud;
    uint8_t framing;
    // These bytes carry no meaning. They retain the existing NVS record layout
    // after display mode and status bar removal so firmware upgrades preserve
    // the user's network and serial settings. Do not reuse or remove them:
    // readStored rejects any record whose length differs.
    uint8_t reserved;
    uint8_t wifiSecurity;
    uint8_t reserved2;
    char ssid[33];
    char wifiPassword[65];
    uint8_t tcpMode;
    uint16_t tcpListenPort;
    char tcpRemoteHost[254];
    uint16_t tcpRemotePort;
    uint32_t longPressMs;
    uint32_t longPressRepeatMs;
    uint32_t screenSaverSeconds;
};
#pragma pack(pop)

using ApplyCallback = bool (*)(const DeviceConfig &previous, const DeviceConfig &next);

enum class ValidationError : uint8_t {
    None = 0,
    Schema,
    Baud,
    Framing,
    WifiSsid,
    WifiSecurity,
    WifiPassword,
    TcpMode,
    TcpListenPort,
    TcpRemoteHost,
    TcpRemotePort,
    LongPress,
    LongPressRepeat,
    ScreenSaver,
};

DeviceConfig factoryDefaults();
void begin();
DeviceConfig snapshot();
bool commit(
    const DeviceConfig &candidate,
    ApplyCallback apply,
    bool *runtimeApplied = nullptr);
bool factoryReset(ApplyCallback apply, bool *runtimeApplied = nullptr);
ValidationError validationError(const DeviceConfig &config);

const char *tcpModeName(TcpMode mode);
bool tcpModeFromName(const char *name, TcpMode &mode);

const char *framingName(Framing framing);
bool framingFromName(const char *name, Framing &framing);
uint32_t serialConfig(Framing framing);
bool supportedBaud(uint32_t baud);
uint32_t nextBaud(uint32_t current);

}  // namespace configuration
