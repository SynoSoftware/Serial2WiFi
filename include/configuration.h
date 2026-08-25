#pragma once

#include <Arduino.h>

namespace configuration {

constexpr uint16_t kSchema = 2;
constexpr uint16_t kLegacySchema = 1;
constexpr uint32_t kDefaultBaud = 19200;

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

enum class LiveView : uint8_t {
    Text = 0,
    Hex,
};

enum class StatusBar : uint8_t {
    Auto = 0,
    Serial,
    Connection,
    Network,
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
    uint8_t uiPreferences;
    char ssid[33];
    char wifiPassword[65];
    char tcpHost[254];
    uint16_t tcpPort;
};
#pragma pack(pop)

using ApplyCallback = bool (*)(const DeviceConfig &previous, const DeviceConfig &next);

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
    UiPreferences,
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
bool validate(const DeviceConfig &config);

const char *framingName(Framing framing);
bool framingFromName(const char *name, Framing &framing);
uint32_t serialConfig(Framing framing);
bool supportedBaud(uint32_t baud);
uint32_t nextBaud(uint32_t current);

const char *displayModeName(DisplayMode mode);
bool displayModeFromName(const char *name, DisplayMode &mode);

LiveView liveView(const DeviceConfig &config);
void setLiveView(DeviceConfig &config, LiveView view);
StatusBar statusBar(const DeviceConfig &config);
void setStatusBar(DeviceConfig &config, StatusBar bar);
bool screenOff(const DeviceConfig &config);
void setScreenOff(DeviceConfig &config, bool off);
bool setupApEnabled(const DeviceConfig &config);
void setSetupApEnabled(DeviceConfig &config, bool enabled);
DisplayMode liveDisplayMode(const DeviceConfig &config);

}  // namespace configuration
