#include "oled_display.h"

#include <Adafruit_GFX.h>
#include <Adafruit_SSD1306.h>
#include <qrcode.h>
#include <Wire.h>
#include <cstring>

#include "display_history.h"

namespace oled_display {
namespace {

constexpr uint8_t kSdaPin = 4;
constexpr uint8_t kSclPin = 15;
constexpr uint8_t kResetPin = 16;
constexpr uint8_t kAddress = 0x3C;
constexpr uint32_t kRefreshMs = 100;
constexpr uint32_t kRateSampleMs = 3000;
constexpr uint32_t kBootBrandingMs = 1000;
constexpr uint32_t kPageTitleMs = 1000;
constexpr size_t kPayloadRows = 7;
constexpr uint8_t kCompactFont = 1;
constexpr uint8_t kLargeFont = 2;
constexpr int16_t kSetupQrModuleCount = 25;
constexpr uint8_t kSetupQrModulePixels = 2;
constexpr int16_t kSetupQrSizePixels = kSetupQrModuleCount * kSetupQrModulePixels;
constexpr int16_t kSetupQrLeftPixels = 0;
constexpr int16_t kSetupQrTextLeftPixels = kSetupQrSizePixels + 4;
constexpr int16_t kSetupQrTextWidth = 128 - kSetupQrTextLeftPixels;

// Dense screens must retain every required value. Short action and fault
// feedback can use the larger font without sacrificing operational data.

Adafruit_SSD1306 oled(128, 64, &Wire, kResetPin);
bool ready = false;
bool displayHasContent = false;
uint32_t lastRefresh = 0;
uint32_t lastRateSample = 0;
uint64_t lastSerialReceived = 0;
uint64_t lastNetworkReceived = 0;
uint32_t serialRate = 0;
uint32_t networkRate = 0;
char firmwareBuildNumber[14]{};

enum class Page : uint8_t {
    Brand = 0,
    Qr,
    Credentials,
    LiveText,
    LiveHex,
    Statistics,
    Serial,
    Count,
};

using PageRenderer = void (*)(
    const configuration::DeviceConfig &config,
    const RuntimeStatus &status,
    bool serialTrafficSeen);

struct PageDescription {
    PageAction action;
    const char *introduction;
    PageRenderer show;
    bool setupOnly;
};

Page currentPage = Page::Brand;
bool pageInitialized = false;
bool displayWokenFromOff = false;
bool screenSaverActive = false;
bool renderRequested = true;
uint32_t bootBrandingStartedAt = 0;
uint32_t lastUserInteraction = 0;
bool pageTitleVisible = false;
uint32_t pageTitleStartedAt = 0;
bool serialTrafficRedirected = false;

struct TextLine {
    char value[22];
    uint8_t length;
};

struct PageObservation {
    bool unconfigured;
    bool setupApAvailable;
    bool screenOff;
    uint8_t displayPreference;
};

PageObservation previousObservation{};

void formatRate(char *destination, size_t capacity, uint32_t value);
void formatBytes(char *destination, size_t capacity, uint64_t value);
void drawCenteredText(
    const char *text,
    uint8_t textSize,
    int16_t regionLeft,
    int16_t regionWidth,
    int16_t centerY);
uint8_t largestReadableFont(const char *text, int16_t regionWidth);
void formatBuildNumber(char *destination, size_t capacity);
void renderLiveText(
    const configuration::DeviceConfig &config,
    const RuntimeStatus &status,
    bool serialTrafficSeen);
void renderLiveHex(
    const configuration::DeviceConfig &config,
    const RuntimeStatus &status,
    bool serialTrafficSeen);
void renderStats(
    const configuration::DeviceConfig &config,
    const RuntimeStatus &status,
    bool serialTrafficSeen);
void renderSetupCredentials(
    const configuration::DeviceConfig &config,
    const RuntimeStatus &status,
    bool serialTrafficSeen);
void renderWifiState(
    const configuration::DeviceConfig &config,
    const RuntimeStatus &status,
    bool serialTrafficSeen);
void renderSetupQr(
    const configuration::DeviceConfig &config,
    const RuntimeStatus &status,
    bool serialTrafficSeen);
void renderSerialPage(
    const configuration::DeviceConfig &config,
    const RuntimeStatus &status,
    bool serialTrafficSeen);
void renderBrandPage(
    const configuration::DeviceConfig &config,
    const RuntimeStatus &status,
    bool serialTrafficSeen);
void prepareNextBaud(configuration::DeviceConfig &candidate);

// The enum order and this table are the one carousel definition. Keeping the
// renderer, setup-only rule, and optional page title together prevents page
// behavior from being duplicated across navigation and rendering code.
constexpr PageDescription kPageDescriptions[] = {
    {nullptr, nullptr, renderBrandPage, false},
    {nullptr, nullptr, renderWifiState, false},
    {nullptr, nullptr, renderSetupCredentials, false},
    {nullptr, "LIVE TEXT", renderLiveText, false},
    {nullptr, "LIVE HEX", renderLiveHex, false},
    {nullptr, "STATISTICS", renderStats, false},
    {prepareNextBaud, nullptr, renderSerialPage, false},
};
constexpr size_t kPageCount = static_cast<size_t>(Page::Count);
static_assert(
    sizeof(kPageDescriptions) / sizeof(kPageDescriptions[0]) == kPageCount,
    "page descriptions must match Page");

const PageDescription &pageDescription(Page page) {
    return kPageDescriptions[static_cast<size_t>(page)];
}

bool setupOnlyPage(Page page) {
    return pageDescription(page).setupOnly;
}

Page preferredRuntimePage(const configuration::DeviceConfig &config) {
    switch (static_cast<configuration::DisplayMode>(config.display)) {
        case configuration::DisplayMode::Hex:
            return Page::LiveHex;
        case configuration::DisplayMode::Stats:
            return Page::Statistics;
        case configuration::DisplayMode::Text:
        case configuration::DisplayMode::Off:
            return Page::LiveText;
    }
    return Page::LiveText;
}

void prepareNextBaud(configuration::DeviceConfig &candidate) {
    candidate.baud = configuration::nextBaud(candidate.baud);
}

void requestRender() {
    renderRequested = true;
}

void selectPage(Page page, bool showTitle) {
    currentPage = page;
    pageTitleVisible = showTitle && pageDescription(page).introduction != nullptr;
    pageTitleStartedAt = pageTitleVisible ? millis() : 0;
    requestRender();
}

void showCurrentPageTitle() {
    selectPage(currentPage, true);
}

void initializePage(
    const configuration::DeviceConfig &config,
    bool setupApAvailable,
    bool serialTrafficSeen) {
    currentPage = preferredRuntimePage(config);
    pageInitialized = true;
    previousObservation = {
        config.ssid[0] == '\0',
        setupApAvailable,
        configuration::screenOff(config),
        config.display,
    };
    serialTrafficRedirected = serialTrafficSeen;
}

void normalizePage(
    const configuration::DeviceConfig &config,
    bool setupApAvailable,
    bool serialTrafficSeen) {
    const PageObservation observation = {
        config.ssid[0] == '\0',
        setupApAvailable,
        configuration::screenOff(config),
        config.display,
    };
    const bool becameConfigured =
        previousObservation.unconfigured && !observation.unconfigured;
    const bool becameUnconfigured =
        !previousObservation.unconfigured && observation.unconfigured;
    const bool setupApAppeared =
        !previousObservation.setupApAvailable && observation.setupApAvailable;
    const bool setupApDisappeared =
        previousObservation.setupApAvailable && !observation.setupApAvailable;
    const bool displayPreferenceChanged =
        previousObservation.displayPreference != observation.displayPreference;
    const bool screenOffChanged =
        previousObservation.screenOff != observation.screenOff;

    if (becameConfigured && setupOnlyPage(currentPage)) {
        selectPage(preferredRuntimePage(config), false);
    } else if (becameUnconfigured && observation.setupApAvailable) {
        selectPage(
            serialTrafficSeen ? preferredRuntimePage(config) : Page::Qr,
            false);
        serialTrafficRedirected = serialTrafficSeen;
    } else if (setupApAppeared && observation.unconfigured &&
            !serialTrafficSeen) {
        selectPage(Page::Qr, false);
    } else if (setupApDisappeared && setupOnlyPage(currentPage)) {
        selectPage(preferredRuntimePage(config), false);
    } else if (!serialTrafficRedirected && observation.unconfigured &&
            serialTrafficSeen) {
        serialTrafficRedirected = true;
        if (setupApAvailable && setupOnlyPage(currentPage)) {
            selectPage(preferredRuntimePage(config), false);
        }
    }
    if (displayPreferenceChanged) {
        if (!setupOnlyPage(currentPage)) {
            selectPage(preferredRuntimePage(config), false);
        }
        // Setup pages remain a deliberate transient navigation choice; the
        // preference is recorded below without rewriting the next page click.
    }
    if (screenOffChanged) {
        displayWokenFromOff = false;
        requestRender();
    }
    previousObservation = observation;
}

void advancePage(bool setupApAvailable) {
    size_t next = (static_cast<size_t>(currentPage) + 1) % kPageCount;
    while (kPageDescriptions[next].setupOnly && !setupApAvailable) {
        next = (next + 1) % kPageCount;
    }
    selectPage(static_cast<Page>(next), true);
}

enum class WifiIcon : uint8_t { Wifi, High, Low, Zero, Off };

// These 1-bit crops are rendered offline from the supplied Lucide SVG paths
// at the OLED's 64px height. The device draws fixed flash data; it never
// parses SVG or uses a vector renderer at runtime.
const uint8_t kWifiOuterBitmap[] PROGMEM = {
    0x00, 0x00, 0x00, 0x7F, 0xE0, 0x00, 0x00, 0x00, 0x00, 0x00, 0x1F, 0xFF, 0xFF, 0x80, 0x00, 0x00,
    0x00, 0x00, 0xFF, 0xFF, 0xFF, 0xF0, 0x00, 0x00, 0x00, 0x07, 0xFF, 0xFF, 0xFF, 0xFE, 0x00, 0x00,
    0x00, 0x1F, 0xFF, 0xFF, 0xFF, 0xFF, 0x80, 0x00, 0x00, 0x7F, 0xFF, 0xFF, 0xFF, 0xFF, 0xE0, 0x00,
    0x01, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xF8, 0x00, 0x03, 0xFF, 0xFC, 0x00, 0x03, 0xFF, 0xFC, 0x00,
    0x0F, 0xFF, 0xC0, 0x00, 0x00, 0x3F, 0xFF, 0x00, 0x1F, 0xFF, 0x00, 0x00, 0x00, 0x0F, 0xFF, 0x80,
    0x3F, 0xFC, 0x00, 0x00, 0x00, 0x03, 0xFF, 0xC0, 0x7F, 0xF0, 0x00, 0x00, 0x00, 0x00, 0xFF, 0xE0,
    0xFF, 0xC0, 0x00, 0x00, 0x00, 0x00, 0x3F, 0xF0, 0xFF, 0x80, 0x00, 0x00, 0x00, 0x00, 0x1F, 0xF0,
    0xFE, 0x00, 0x00, 0x00, 0x00, 0x00, 0x07, 0xF0, 0x7C, 0x00, 0x00, 0x00, 0x00, 0x00, 0x03, 0xE0,
    0x38, 0x00, 0x00, 0x00, 0x00, 0x00, 0x01, 0xC0,
};
const uint8_t kWifiMiddleBitmap[] PROGMEM = {
    0x00, 0x03, 0xFF, 0xFC, 0x00, 0x00, 0x00, 0x1F, 0xFF, 0xFF, 0x80, 0x00, 0x00, 0x7F, 0xFF, 0xFF,
    0xE0, 0x00, 0x01, 0xFF, 0xFF, 0xFF, 0xF8, 0x00, 0x07, 0xFF, 0xFF, 0xFF, 0xFE, 0x00, 0x0F, 0xFF,
    0xFF, 0xFF, 0xFF, 0x00, 0x1F, 0xFF, 0x80, 0x1F, 0xFF, 0x80, 0x7F, 0xFC, 0x00, 0x03, 0xFF, 0xE0,
    0x7F, 0xE0, 0x00, 0x00, 0x7F, 0xE0, 0xFF, 0xC0, 0x00, 0x00, 0x3F, 0xF0, 0xFF, 0x00, 0x00, 0x00,
    0x0F, 0xF0, 0xFE, 0x00, 0x00, 0x00, 0x07, 0xF0, 0x7C, 0x00, 0x00, 0x00, 0x03, 0xE0,
};
const uint8_t kWifiInnerBitmap[] PROGMEM = {
    0x01, 0xFF, 0x80, 0x07, 0xFF, 0xE0, 0x1F, 0xFF, 0xF8, 0x3F, 0xFF, 0xFE, 0xFF, 0xFF, 0xFF, 0xFF,
    0xFF, 0xFF, 0xFF, 0xC3, 0xFF, 0xFE, 0x00, 0x7F, 0xFC, 0x00, 0x3F, 0x78, 0x00, 0x1E,
};
const uint8_t kWifiDotBitmap[] PROGMEM = {0x78, 0xFC, 0xFC, 0xFC, 0xFC, 0x7C};
const uint8_t kWifiOffBitmap[] PROGMEM = {
    0x38,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x7C,0x00,0x00,0x00,0x00,0x00,0x00,0x00,
    0xFE,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0xFF,0x00,0x00,0x00,0x00,0x00,0x00,0x00,
    0xFF,0x80,0x00,0x00,0x00,0x00,0x00,0x00,0x7F,0xC0,0x00,0x00,0x00,0x00,0x00,0x00,
    0x3F,0xE0,0x00,0x00,0x00,0x00,0x00,0x00,0x1F,0xF0,0x00,0x00,0x00,0x00,0x00,0x00,
    0x0F,0xF8,0x00,0x7F,0xF0,0x00,0x00,0x00,0x07,0xFC,0x00,0xFF,0xFF,0x80,0x00,0x00,
    0x03,0xFE,0x01,0xFF,0xFF,0xF0,0x00,0x00,0x01,0xFF,0x01,0xFF,0xFF,0xFE,0x00,0x00,
    0x00,0xFF,0x81,0xFF,0xFF,0xFF,0x80,0x00,0x00,0x7F,0xC0,0xFF,0xFF,0xFF,0xE0,0x00,
    0x01,0xFF,0xE0,0x7D,0xFF,0xFF,0xF8,0x00,0x03,0xFF,0xF0,0x00,0x03,0xFF,0xFC,0x00,
    0x0F,0xFF,0xF8,0x00,0x00,0x3F,0xFF,0x00,0x1F,0xFF,0xFC,0x00,0x00,0x0F,0xFF,0x80,
    0x3F,0xFF,0xFE,0x00,0x00,0x03,0xFF,0xC0,0x7F,0xF1,0xFF,0x00,0x00,0x00,0xFF,0xE0,
    0xFF,0xC0,0xFF,0x80,0x00,0x00,0x3F,0xF0,0xFF,0x80,0x7F,0xC0,0x00,0x00,0x1F,0xF0,
    0xFE,0x00,0x3F,0xE0,0x00,0x00,0x07,0xF0,0x7C,0x00,0x1F,0xF0,0x00,0x00,0x03,0xE0,
    0x38,0x00,0x7F,0xF8,0x00,0x00,0x01,0xC0,0x00,0x01,0xFF,0xFC,0x00,0x78,0x00,0x00,
    0x00,0x07,0xFF,0xFE,0x00,0x7E,0x00,0x00,0x00,0x0F,0xFF,0xFF,0x00,0xFF,0x00,0x00,
    0x00,0x1F,0xFF,0xFF,0x80,0xFF,0x80,0x00,0x00,0x3F,0xFC,0x7F,0xC0,0xFF,0xE0,0x00,
    0x00,0x7F,0xE0,0x3F,0xE0,0x7F,0xE0,0x00,0x00,0xFF,0xC0,0x1F,0xF0,0x3F,0xF0,0x00,
    0x00,0xFF,0x00,0x0F,0xF8,0x0F,0xF0,0x00,0x00,0xFE,0x00,0x07,0xFC,0x07,0xF0,0x00,
    0x00,0x7C,0x00,0x03,0xFE,0x03,0xE0,0x00,0x00,0x00,0x00,0x7F,0xFF,0x00,0x00,0x00,
    0x00,0x00,0x01,0xFF,0xFF,0x80,0x00,0x00,0x00,0x00,0x07,0xFF,0xFF,0xC0,0x00,0x00,
    0x00,0x00,0x0F,0xFF,0xFF,0xE0,0x00,0x00,0x00,0x00,0x3F,0xFF,0xFF,0xF0,0x00,0x00,
    0x00,0x00,0x3F,0xFF,0xFF,0xF8,0x00,0x00,0x00,0x00,0x3F,0xF0,0xFF,0xFC,0x00,0x00,
    0x00,0x00,0x3F,0x80,0x1F,0xFE,0x00,0x00,0x00,0x00,0x3F,0x00,0x0F,0xFF,0x00,0x00,
    0x00,0x00,0x1E,0x00,0x07,0xFF,0x80,0x00,0x00,0x00,0x00,0x00,0x00,0x7F,0xC0,0x00,
    0x00,0x00,0x00,0x00,0x00,0x3F,0xE0,0x00,0x00,0x00,0x00,0x00,0x00,0x1F,0xF0,0x00,
    0x00,0x00,0x00,0x0F,0x00,0x0F,0xF8,0x00,0x00,0x00,0x00,0x1F,0x80,0x07,0xFC,0x00,
    0x00,0x00,0x00,0x1F,0x80,0x03,0xFE,0x00,0x00,0x00,0x00,0x1F,0x80,0x01,0xFF,0x00,
    0x00,0x00,0x00,0x1F,0x80,0x00,0xFF,0x80,0x00,0x00,0x00,0x0F,0x80,0x00,0x7F,0xC0,
    0x00,0x00,0x00,0x00,0x00,0x00,0x3F,0xE0,0x00,0x00,0x00,0x00,0x00,0x00,0x1F,0xF0,
    0x00,0x00,0x00,0x00,0x00,0x00,0x0F,0xF0,0x00,0x00,0x00,0x00,0x00,0x00,0x07,0xF0,
    0x00,0x00,0x00,0x00,0x00,0x00,0x03,0xE0,0x00,0x00,0x00,0x00,0x00,0x00,0x01,0xC0,
};

void drawWifiIcon(WifiIcon icon) {
    constexpr int16_t left = 32;
    if (icon == WifiIcon::Off) {
        oled.drawBitmap(left + 2, 2, kWifiOffBitmap, 60, 60, SSD1306_WHITE);
        return;
    }
    if (icon == WifiIcon::Wifi) {
        oled.drawBitmap(left + 2, 10, kWifiOuterBitmap, 60, 17, SSD1306_WHITE);
    }
    if (icon == WifiIcon::Wifi || icon == WifiIcon::High) {
        oled.drawBitmap(left + 10, 24, kWifiMiddleBitmap, 44, 13, SSD1306_WHITE);
    }
    if (icon != WifiIcon::Zero) {
        oled.drawBitmap(left + 20, 37, kWifiInnerBitmap, 24, 10, SSD1306_WHITE);
    }
    oled.drawBitmap(left + 29, 50, kWifiDotBitmap, 6, 6, SSD1306_WHITE);
}

WifiIcon stationWifiIcon(int32_t rssi) {
    if (rssi >= -60) return WifiIcon::Wifi;
    if (rssi >= -70) return WifiIcon::High;
    if (rssi >= -80) return WifiIcon::Low;
    return WifiIcon::Zero;
}

void renderWifiState(
    const configuration::DeviceConfig &config,
    const RuntimeStatus &status,
    bool) {
    if (status.setupApActive) {
        renderSetupQr(config, status, false);
        return;
    }

    const WifiIcon icon = status.stationConnected ?
        stationWifiIcon(status.stationRssi) : WifiIcon::Off;
    drawWifiIcon(icon);
}

void renderSetupCredentials(
    const configuration::DeviceConfig &,
    const RuntimeStatus &status,
    bool) {
    oled.setTextSize(kCompactFont);
    if (status.setupApActive) {
        // This is the setup recovery page. Keep its proven SSID/password
        // layout intact; Screen 2's QR is only shown for this same live AP.
        drawCenteredText("Wi-Fi", kCompactFont, 0, 128, 6);
        drawCenteredText(
            status.setupSsid,
            largestReadableFont(status.setupSsid, 128),
            0,
            128,
            18);
        drawCenteredText("Password", kCompactFont, 0, 128, 36);
        drawCenteredText(
            status.setupPassword,
            largestReadableFont(status.setupPassword, 128),
            0,
            128,
            51);
        return;
    }

    if (status.stationConnected) {
        char rssiText[16];
        snprintf(rssiText, sizeof(rssiText), "%ld dBm", static_cast<long>(status.stationRssi));
        drawCenteredText("Connected", kCompactFont, 0, 128, 6);
        drawCenteredText(
            status.stationSsid,
            largestReadableFont(status.stationSsid, 128),
            0,
            128,
            22);
        drawCenteredText(rssiText, kLargeFont, 0, 128, 49);
    } else if (status.stationConfigured) {
        drawCenteredText(
            status.stationSsid,
            largestReadableFont(status.stationSsid, 128),
            0,
            128,
            22);
        drawCenteredText("Disconnected", kCompactFont, 0, 128, 44);
    } else {
        drawCenteredText("Wi-Fi not configured", kCompactFont, 0, 128, 32);
    }
}

void renderSerialPage(
    const configuration::DeviceConfig &config,
    const RuntimeStatus &,
    bool) {
    char baudText[12];
    snprintf(baudText, sizeof(baudText), "%lu", static_cast<unsigned long>(config.baud));
    drawCenteredText(baudText, kLargeFont, 0, 128, 18);
    drawCenteredText(
        configuration::framingName(static_cast<configuration::Framing>(config.framing)),
        kLargeFont,
        0,
        128,
        39);
    drawCenteredText("Hold to change", kCompactFont, 0, 128, 59);
}

void drawCenteredText(
    const char *text,
    uint8_t textSize,
    int16_t regionLeft,
    int16_t regionWidth,
    int16_t centerY) {
    oled.setTextSize(textSize);
    int16_t textX = 0;
    int16_t textY = 0;
    uint16_t textWidth = 0;
    uint16_t textHeight = 0;
    oled.getTextBounds(text, 0, 0, &textX, &textY, &textWidth, &textHeight);
    oled.setCursor(
        regionLeft + (regionWidth - static_cast<int16_t>(textWidth)) / 2 - textX,
        centerY - static_cast<int16_t>(textHeight) / 2 - textY);
    oled.print(text);
}

uint8_t largestReadableFont(const char *text, int16_t regionWidth) {
    oled.setTextSize(kLargeFont);
    int16_t textX = 0;
    int16_t textY = 0;
    uint16_t textWidth = 0;
    uint16_t textHeight = 0;
    oled.getTextBounds(text, 0, 0, &textX, &textY, &textWidth, &textHeight);
    return textWidth <= static_cast<uint16_t>(regionWidth) ? kLargeFont : kCompactFont;
}

void drawCompressedText(
    const char *text,
    uint8_t textSize,
    int16_t glyphAdvance,
    int16_t regionLeft,
    int16_t regionWidth,
    int16_t centerY) {
    oled.setTextSize(textSize);
    int16_t textX = 0;
    int16_t textY = 0;
    uint16_t unusedWidth = 0;
    uint16_t textHeight = 0;
    oled.getTextBounds(text, 0, 0, &textX, &textY, &unusedWidth, &textHeight);
    const int16_t textWidth = static_cast<int16_t>(strlen(text)) * glyphAdvance - 1;
    const int16_t firstX = regionLeft + (regionWidth - textWidth) / 2;
    const int16_t firstY = centerY - static_cast<int16_t>(textHeight) / 2 - textY;
    for (size_t index = 0; text[index] != '\0'; ++index) {
        oled.drawChar(
            firstX + static_cast<int16_t>(index) * glyphAdvance,
            firstY,
            text[index],
            SSD1306_WHITE,
            SSD1306_BLACK,
            textSize);
    }
}

void drawLargeProductTitle() {
    drawCompressedText("Serial2WiFi", kLargeFont, 11, 0, 128, 16);
}

void renderBrandPage(
    const configuration::DeviceConfig &,
    const RuntimeStatus &,
    bool) {
    drawLargeProductTitle();
    drawCenteredText("synapse.sr", kLargeFont, 0, 128, 48);
}

void renderPageTitle(Page page) {
    drawCenteredText(pageDescription(page).introduction, kLargeFont, 0, 128, 32);
}

void resetLine(TextLine &line, display_history::Direction direction) {
    memset(line.value, ' ', sizeof(line.value));
    line.value[21] = '\0';
    line.length = 3;
    memcpy(line.value, direction == display_history::Direction::SerialToNetwork ? "S> " : "<S ", 3);
}

void startNextLine(
    TextLine *lines,
    size_t &current,
    size_t &count,
    bool &started,
    display_history::Direction direction) {
    if (started) {
        current = (current + 1) % kPayloadRows;
        if (count < kPayloadRows) ++count;
    } else {
        count = 1;
        started = true;
    }
    resetLine(lines[current], direction);
}

void renderStatusRow(
    const configuration::DeviceConfig &config,
    const RuntimeStatus &status,
    bool serialTrafficSeen) {
    char line[22];
    const uint32_t phase = millis() % 6000;
    oled.setTextSize(kCompactFont);
    if (status.serialError) {
        snprintf(line, sizeof(line), "SERIAL ERROR");
    } else if (status.setupApActive && phase >= 4000) {
        snprintf(line, sizeof(line), "P:%s", status.setupPassword);
    } else {
        configuration::StatusBar bar = configuration::statusBar(config);
        const bool trafficSeen = serialTrafficSeen ||
            status.serialToNetworkReceived != 0 || status.networkToSerialReceived != 0;
        if (bar == configuration::StatusBar::Auto && trafficSeen) {
            char rateText[12];
            char totalText[12];
            const bool showSerialToNetwork = (millis() / kRateSampleMs) % 2 == 0;
            if (showSerialToNetwork) {
                formatRate(rateText, sizeof(rateText), serialRate);
                formatBytes(totalText, sizeof(totalText), status.serialToNetworkReceived);
                snprintf(line, sizeof(line), "S>N %s %s", rateText, totalText);
            } else {
                formatRate(rateText, sizeof(rateText), networkRate);
                formatBytes(totalText, sizeof(totalText), status.networkToSerialReceived);
                snprintf(line, sizeof(line), "N>S %s %s", rateText, totalText);
            }
        } else {
            if (bar == configuration::StatusBar::Auto &&
                    (status.stationConnected || status.tcpConnected)) {
                bar = configuration::StatusBar::Connection;
            } else if (bar == configuration::StatusBar::Auto) {
                bar = configuration::StatusBar::Serial;
            }

            if (bar == configuration::StatusBar::Serial) {
                snprintf(line, sizeof(line), "%lu %s", static_cast<unsigned long>(config.baud),
                    configuration::framingName(static_cast<configuration::Framing>(config.framing)));
            } else if (bar == configuration::StatusBar::Connection) {
                snprintf(line, sizeof(line), "W:%s T:%s",
                    status.stationConnected ? "OK" : "--",
                    status.tcpConnected ? "OK" : "--");
            } else {
                if (status.setupApActive) {
                    snprintf(line, sizeof(line), "SETUP ON");
                } else if (status.stationConnected) {
                    const String ip = status.stationIp.toString();
                    snprintf(line, sizeof(line), "IP %s", ip.c_str());
                } else {
                    snprintf(line, sizeof(line), "NETWORK --");
                }
            }
        }
    }
    oled.setCursor(0, 0);
    oled.print(line);
}

void renderLiveText(
    const configuration::DeviceConfig &config,
    const RuntimeStatus &status,
    bool serialTrafficSeen) {
    display_history::DisplayByte snapshot[display_history::kCapacity];
    const size_t snapshotCount = display_history::snapshot(snapshot, display_history::kCapacity);
    TextLine lines[kPayloadRows]{};
    size_t currentLine = 0;
    size_t lineCount = 0;
    bool started = false;
    display_history::Direction direction = display_history::Direction::SerialToNetwork;

    for (size_t i = 0; i < snapshotCount; ++i) {
        const display_history::DisplayByte &byte = snapshot[i];
        if (!started) {
            startNextLine(lines, currentLine, lineCount, started, byte.direction);
            direction = byte.direction;
        }
        if (byte.direction != direction) {
            if (lines[currentLine].length == 3) {
                resetLine(lines[currentLine], byte.direction);
            } else {
                startNextLine(lines, currentLine, lineCount, started, byte.direction);
            }
            direction = byte.direction;
        }
        if (byte.value == '\n' || lines[currentLine].length >= 21) {
            startNextLine(lines, currentLine, lineCount, started, byte.direction);
            direction = byte.direction;
        }
        if (byte.value == '\r' || byte.value == '\n') continue;
        if (lines[currentLine].length < 21) {
            lines[currentLine].value[lines[currentLine].length++] =
            byte.value >= 0x20 && byte.value <= 0x7E ? static_cast<char>(byte.value) : '.';
        }
    }

    if (!started && !serialTrafficSeen) {
        startNextLine(
            lines,
            currentLine,
            lineCount,
            started,
            display_history::Direction::SerialToNetwork);
        memcpy(lines[currentLine].value + 3, "WAITING FOR SERIAL", 18);
        lines[currentLine].length = 21;
    }

    renderStatusRow(config, status, serialTrafficSeen);
    oled.setTextSize(kCompactFont);
    const size_t firstLine = lineCount == kPayloadRows ?
        (currentLine + 1) % kPayloadRows : 0;
    for (size_t row = 0; row < kPayloadRows; ++row) {
        oled.setCursor(0, static_cast<int16_t>((row + 1) * 8));
        if (row < lineCount) oled.print(lines[(firstLine + row) % kPayloadRows].value);
    }
}

void renderLiveHex(
    const configuration::DeviceConfig &config,
    const RuntimeStatus &status,
    bool serialTrafficSeen) {
    display_history::DisplayByte snapshot[display_history::kCapacity];
    const size_t snapshotCount = display_history::snapshot(snapshot, display_history::kCapacity);
    size_t firstByte = snapshotCount > kPayloadRows * 6 ?
        snapshotCount - kPayloadRows * 6 : 0;
    renderStatusRow(config, status, serialTrafficSeen);
    oled.setTextSize(kCompactFont);
    for (size_t row = 0; row < kPayloadRows; ++row) {
        oled.setCursor(0, static_cast<int16_t>((row + 1) * 8));
        if (firstByte >= snapshotCount) continue;
        const display_history::Direction direction = snapshot[firstByte].direction;
        oled.print(direction == display_history::Direction::SerialToNetwork ? "S> " : "<S ");
        size_t displayed = 0;
        while (firstByte < snapshotCount && displayed < 6 &&
                snapshot[firstByte].direction == direction) {
            if (displayed != 0) oled.print(' ');
            if (snapshot[firstByte].value < 16) oled.print('0');
            oled.print(snapshot[firstByte].value, HEX);
            ++firstByte;
            ++displayed;
        }
    }
}

void formatRate(char *destination, size_t capacity, uint32_t value) {
    if (value >= 1024 * 1024) {
        snprintf(destination, capacity, "%.1fM/s", static_cast<double>(value) / (1024.0 * 1024.0));
    } else if (value >= 1024) {
        snprintf(destination, capacity, "%.1fk/s", static_cast<double>(value) / 1024.0);
    } else {
        snprintf(destination, capacity, "%luB/s", static_cast<unsigned long>(value));
    }
}

void formatBuildNumber(char *destination, size_t capacity) {
    static const char *months[] = {
        "Jan", "Feb", "Mar", "Apr", "May", "Jun",
        "Jul", "Aug", "Sep", "Oct", "Nov", "Dec",
    };
    char buildDate[12]{};
    char buildTime[9]{};
    strncpy(buildDate, __DATE__, sizeof(buildDate) - 1);
    strncpy(buildTime, __TIME__, sizeof(buildTime) - 1);
    uint8_t monthNumber = 0;
    for (uint8_t month = 0; month < 12; ++month) {
        if (strncmp(buildDate, months[month], 3) == 0) {
            monthNumber = month + 1;
            break;
        }
    }
    const char dayTens = buildDate[4] == ' ' ? '0' : buildDate[4];
    snprintf(destination, capacity, "%c%c%c%c%c%c-%c%c%c%c%c%c%c",
        buildDate[9], buildDate[10],
        static_cast<char>('0' + monthNumber / 10),
        static_cast<char>('0' + monthNumber % 10),
        dayTens, buildDate[5],
        buildTime[0], buildTime[1], buildTime[3], buildTime[4],
        buildTime[6], buildTime[7], buildTime[8]);
}

void formatBytes(char *destination, size_t capacity, uint64_t value) {
    if (value >= 1024ULL * 1024ULL) {
        snprintf(destination, capacity, "%.1fMB", static_cast<double>(value) / (1024.0 * 1024.0));
    } else if (value >= 1024ULL) {
        snprintf(destination, capacity, "%.1fKB", static_cast<double>(value) / 1024.0);
    } else {
        snprintf(destination, capacity, "%lluB", static_cast<unsigned long long>(value));
    }
}

void renderStats(
    const configuration::DeviceConfig &config,
    const RuntimeStatus &status,
    bool) {
    char serialRateText[12];
    char networkRateText[12];
    char serialTotalText[12];
    char networkTotalText[12];
    char serialDropText[12];
    char networkDropText[12];
    formatRate(serialRateText, sizeof(serialRateText), serialRate);
    formatRate(networkRateText, sizeof(networkRateText), networkRate);
    formatBytes(serialTotalText, sizeof(serialTotalText), status.serialToNetworkReceived);
    formatBytes(networkTotalText, sizeof(networkTotalText), status.networkToSerialReceived);
    formatBytes(serialDropText, sizeof(serialDropText), status.serialToNetworkDropped);
    formatBytes(networkDropText, sizeof(networkDropText), status.networkToSerialDropped);

    oled.setTextSize(kCompactFont);
    oled.setCursor(0, 0);
    if (status.serialError) {
        oled.print("SERIAL ERROR");
    } else {
        oled.print(status.stationConnected ? "WiFi OK" : "WiFi --");
    }
    oled.setCursor(12 * 6, 0);
    oled.print(status.tcpConnected ? "TCP OK" : "TCP --");
    oled.setCursor(0, 8);
    oled.printf("S>N %-10s", serialRateText);
    oled.setCursor(0, 16);
    oled.printf("    %s", serialTotalText);
    oled.setCursor(0, 24);
    oled.printf("N>S %-10s", networkRateText);
    oled.setCursor(0, 32);
    oled.printf("    %s", networkTotalText);
    oled.setCursor(0, 40);
    oled.printf("DROP %s/%s", serialDropText, networkDropText);
    oled.setCursor(0, 48);
    oled.printf("%lu %s", static_cast<unsigned long>(config.baud),
        configuration::framingName(static_cast<configuration::Framing>(config.framing)));
    // Keep the full baud/framing value readable on the left. The compact
    // build string fits on the right without overwriting it, including at
    // the widest supported baud rate.
    drawCompressedText(firmwareBuildNumber, kCompactFont, 4, 72, 56, 52);
    oled.setCursor(0, 56);
    oled.printf("U %lu/%lu/%lu/%lu", static_cast<unsigned long>(status.serialFifoOverflowErrors),
        static_cast<unsigned long>(status.serialBufferOverflowErrors),
        static_cast<unsigned long>(status.serialFramingErrors),
        static_cast<unsigned long>(status.serialParityErrors));
}

void renderSetupQr(
    const configuration::DeviceConfig &,
    const RuntimeStatus &status,
    bool) {
    char wifiPayload[96];
    snprintf(wifiPayload, sizeof(wifiPayload), "WIFI:T:WPA;S:%s;P:%s;;",
        status.setupSsid, status.setupPassword);

    uint8_t qrStorage[128]{};
    QRCode qr;
    // Setup credentials are migrated to the compact form before this page is
    // shown. Version 2-L at 2x is the one supported physical layout; never
    // silently substitute a smaller or denser legacy QR. Keep it pinned to
    // the left so the right column can carry the manual OPEN 192.168.4.1
    // fallback.
    constexpr uint8_t qrVersion = 2;
    if (qrcode_initText(&qr, qrStorage, qrVersion, ECC_LOW, wifiPayload) < 0) {
        return;
    }
    const uint8_t modulePixels = kSetupQrModulePixels;
    const int16_t qrX = kSetupQrLeftPixels;
    constexpr int16_t qrY = 7;
    for (uint8_t y = 0; y < qr.size; ++y) {
        for (uint8_t x = 0; x < qr.size; ++x) {
            if (qrcode_getModule(&qr, x, y)) {
                oled.fillRect(
                    qrX + x * modulePixels,
                    qrY + y * modulePixels,
                    modulePixels,
                    modulePixels,
                    SSD1306_WHITE);
            }
        }
    }

    const int16_t textLeft = kSetupQrTextLeftPixels;
    const int16_t textWidth = kSetupQrTextWidth;
    drawCenteredText("OPEN", largestReadableFont("OPEN", textWidth), textLeft, textWidth, 24);
    drawCenteredText(
        "192.168.4.1",
        largestReadableFont("192.168.4.1", textWidth),
        textLeft,
        textWidth,
        40);
}

void renderSerialError() {
    oled.setTextSize(kLargeFont);
    oled.setCursor(28, 4);
    oled.print("SERIAL");
    oled.setCursor(34, 22);
    oled.print("ERROR");
    oled.setTextSize(kCompactFont);
    oled.setCursor(16, 48);
    oled.print("Reboot to retry");
}

void renderOverlay(
    prg_button::Overlay overlay,
    uint32_t countdown) {
    switch (overlay) {
        case prg_button::Overlay::ResetWarning:
            oled.setTextSize(kLargeFont);
            oled.setCursor(22, 0);
            oled.print("FACTORY");
            oled.setCursor(34, 16);
            oled.print("RESET");
            oled.setTextSize(kCompactFont);
            oled.setCursor(24, 36);
            oled.print("Keep holding...");
            oled.setTextSize(kLargeFont);
            oled.setCursor(61, 48);
            oled.print(countdown);
            break;
        case prg_button::Overlay::ResetComplete:
            oled.setTextSize(kLargeFont);
            oled.setCursor(34, 8);
            oled.print("RESET");
            oled.setCursor(16, 32);
            oled.print("COMPLETE");
            break;
        case prg_button::Overlay::ResetFailed:
            oled.setTextSize(kLargeFont);
            oled.setCursor(34, 8);
            oled.print("RESET");
            oled.setCursor(28, 32);
            oled.print("FAILED");
            break;
        case prg_button::Overlay::SaveFailed:
            oled.setTextSize(kLargeFont);
            oled.setCursor(40, 8);
            oled.print("SAVE");
            oled.setCursor(28, 32);
            oled.print("FAILED");
            break;
        case prg_button::Overlay::None:
            break;
    }
}

}  // namespace

void begin() {
    formatBuildNumber(firmwareBuildNumber, sizeof(firmwareBuildNumber));
    pinMode(kResetPin, OUTPUT);
    digitalWrite(kResetPin, LOW);
    delay(20);
    digitalWrite(kResetPin, HIGH);
    Wire.begin(kSdaPin, kSclPin);
    ready = oled.begin(SSD1306_SWITCHCAPVCC, kAddress);
    // Start the visible boot indication on the first render, after the rest
    // of firmware startup has completed.
    bootBrandingStartedAt = 0;
    pageInitialized = false;
    displayWokenFromOff = false;
    screenSaverActive = false;
    renderRequested = true;
    lastUserInteraction = 0;
    pageTitleVisible = false;
    pageTitleStartedAt = 0;
    serialTrafficRedirected = false;
    previousObservation = {};
    if (ready) {
        oled.clearDisplay();
        oled.setTextSize(kCompactFont);
        oled.setTextColor(SSD1306_WHITE);
        oled.setTextWrap(false);
        oled.display();
    }
}

void handleShortClick(
    const configuration::DeviceConfig &config,
    bool setupApAvailable,
    bool serialTrafficSeen) {
    const bool wakingScreenSaver = screenSaverActive;
    const bool showingBootBranding = bootBrandingStartedAt == 0 ||
        millis() - bootBrandingStartedAt < kBootBrandingMs;
    noteUserInteraction();
    if (!pageInitialized) initializePage(config, setupApAvailable, serialTrafficSeen);
    normalizePage(config, setupApAvailable, serialTrafficSeen);
    if (showingBootBranding) {
        if (configuration::screenOff(config) && !displayWokenFromOff) {
            displayWokenFromOff = true;
        }
        selectPage(preferredRuntimePage(config), true);
        return;
    }
    if (wakingScreenSaver) {
        showCurrentPageTitle();
        return;
    }
    if (configuration::screenOff(config) && !displayWokenFromOff) {
        if (setupOnlyPage(currentPage)) {
            selectPage(Page::LiveText, true);
        } else {
            showCurrentPageTitle();
        }
        displayWokenFromOff = true;
        return;
    }
    advancePage(setupApAvailable);
}

void noteUserInteraction() {
    lastUserInteraction = millis();
    screenSaverActive = false;
    requestRender();
}

PageAction currentPageAction() {
    return pageDescription(currentPage).action;
}

void render(
    const configuration::DeviceConfig &config,
    prg_button::Overlay activeOverlay,
    uint32_t countdown,
    bool serialTrafficSeen,
    const RuntimeStatus &status) {
    const uint32_t now = millis();
    if (!ready || (!renderRequested && now - lastRefresh < kRefreshMs)) return;
    lastRefresh = now;
    renderRequested = false;
    if (bootBrandingStartedAt == 0) bootBrandingStartedAt = now;
    if (lastUserInteraction == 0) lastUserInteraction = now;

    if (lastRateSample == 0) lastRateSample = now;
    if (now - lastRateSample >= kRateSampleMs) {
        const uint32_t elapsed = now - lastRateSample;
        serialRate = static_cast<uint32_t>((status.serialToNetworkReceived - lastSerialReceived) * 1000ULL / elapsed);
        networkRate = static_cast<uint32_t>((status.networkToSerialReceived - lastNetworkReceived) * 1000ULL / elapsed);
        lastSerialReceived = status.serialToNetworkReceived;
        lastNetworkReceived = status.networkToSerialReceived;
        lastRateSample = now;
    }

    const bool overlayVisible = activeOverlay != prg_button::Overlay::None;
    const bool showingBootBranding = now - bootBrandingStartedAt < kBootBrandingMs;
    if (showingBootBranding && !overlayVisible) {
        oled.clearDisplay();
        oled.setTextSize(kCompactFont);
        oled.setTextColor(SSD1306_WHITE);
        oled.setTextWrap(false);
        pageDescription(Page::Brand).show(config, status, serialTrafficSeen);
        oled.display();
        displayHasContent = true;
        return;
    }

    if (!pageInitialized) initializePage(config, status.setupApActive, serialTrafficSeen);
    normalizePage(config, status.setupApActive, serialTrafficSeen);

    const bool screenIsOff = configuration::screenOff(config);

    if (config.screenSaverSeconds == 0) {
        screenSaverActive = false;
    } else if (!screenSaverActive &&
            static_cast<uint64_t>(now - lastUserInteraction) >=
                static_cast<uint64_t>(config.screenSaverSeconds) * 1000ULL &&
            !overlayVisible && !status.serialError) {
        screenSaverActive = true;
    }

    if (((screenIsOff && !displayWokenFromOff) || screenSaverActive) &&
            !overlayVisible && !status.serialError) {
        if (displayHasContent) {
            oled.clearDisplay();
            oled.display();
            displayHasContent = false;
        }
        return;
    }

    oled.clearDisplay();
    oled.setTextSize(kCompactFont);
    oled.setTextColor(SSD1306_WHITE);
    oled.setTextWrap(false);
    const PageDescription &description = pageDescription(currentPage);
    const bool titleShowing = pageTitleVisible && description.introduction != nullptr &&
        now - pageTitleStartedAt < kPageTitleMs;
    if (pageTitleVisible && !titleShowing) pageTitleVisible = false;
    if (overlayVisible) {
        renderOverlay(activeOverlay, countdown);
    } else if (status.serialError) {
        renderSerialError();
    } else if (titleShowing) {
        renderPageTitle(currentPage);
    } else {
        description.show(config, status, serialTrafficSeen);
    }
    oled.display();
    displayHasContent = true;
}

}  // namespace oled_display
