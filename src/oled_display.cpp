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
constexpr uint32_t kMenuInactivityMs = 10000;
constexpr size_t kPayloadRows = 7;
constexpr uint8_t kCompactFont = 1;
constexpr uint8_t kLargeFont = 2;
constexpr uint8_t kSetupQrModulePixels = 2;
constexpr int16_t kSetupQrLeftPixels = (128 - 50) / 2;

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
bool menuIsOpen = false;
uint8_t menuRow = 0;
uint32_t lastMenuInteraction = 0;

enum class MenuRow : uint8_t {
    LiveView = 0,
    StatusBar,
    Screen,
};

enum class SetupPage : uint8_t {
    Baud,
    Details,
    Qr,
    Brand,
};

SetupPage setupPage = SetupPage::Baud;

struct TextLine {
    char value[22];
    uint8_t length;
};

void formatRate(char *destination, size_t capacity, uint32_t value);
void formatBytes(char *destination, size_t capacity, uint64_t value);

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

void drawLargeProductTitle() {
    constexpr char title[] = "Serial2WiFi";
    constexpr int16_t glyphAdvance = 11;
    constexpr int16_t titleWidth = (sizeof(title) - 1) * glyphAdvance - 1;
    constexpr int16_t centerY = 16;

    oled.setTextSize(kLargeFont);
    int16_t textX = 0;
    int16_t textY = 0;
    uint16_t unusedWidth = 0;
    uint16_t textHeight = 0;
    oled.getTextBounds(title, 0, 0, &textX, &textY, &unusedWidth, &textHeight);
    const int16_t firstX = (128 - titleWidth) / 2;
    const int16_t firstY = centerY - static_cast<int16_t>(textHeight) / 2 - textY;
    for (size_t index = 0; index < sizeof(title) - 1; ++index) {
        oled.drawChar(
            firstX + static_cast<int16_t>(index) * glyphAdvance,
            firstY,
            title[index],
            SSD1306_WHITE,
            SSD1306_BLACK,
            kLargeFont);
    }
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
    } else if (status.apActive && phase >= 4000) {
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
                if (status.apActive) {
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
    const RuntimeStatus &status) {
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
    oled.setCursor(0, 56);
    oled.printf("U %lu/%lu/%lu/%lu", static_cast<unsigned long>(status.serialFifoOverflowErrors),
        static_cast<unsigned long>(status.serialBufferOverflowErrors),
        static_cast<unsigned long>(status.serialFramingErrors),
        static_cast<unsigned long>(status.serialParityErrors));
}

void renderSetupDetails(
    const configuration::DeviceConfig &config,
    const RuntimeStatus &status) {
    // The values themselves are the useful setup instructions. Literal
    // protocol labels consume the same pixels and make the credentials harder
    // to read on this small display.
    drawCenteredText(status.setupSsid, kLargeFont, 0, 128, 10);
    drawCenteredText(status.setupPassword, kLargeFont, 0, 128, 32);

    char serialSettings[24];
    snprintf(serialSettings, sizeof(serialSettings), "%lu %s",
        static_cast<unsigned long>(config.baud),
        configuration::framingName(static_cast<configuration::Framing>(config.framing)));
    oled.setTextSize(kLargeFont);
    int16_t textX = 0;
    int16_t textY = 0;
    uint16_t textWidth = 0;
    uint16_t textHeight = 0;
    oled.getTextBounds(serialSettings, 0, 0, &textX, &textY, &textWidth, &textHeight);
    const uint8_t serialFont = textWidth <= 128 ? kLargeFont : kCompactFont;
    drawCenteredText(serialSettings, serialFont, 0, 128, 54);
}

void renderSetupBaud(const configuration::DeviceConfig &config) {
    // The fitted large title leaves the baud value as the only changing field.
    drawLargeProductTitle();
    char baud[12];
    snprintf(baud, sizeof(baud), "%lu", static_cast<unsigned long>(config.baud));
    drawCenteredText(baud, kLargeFont, 0, 128, 48);
}

void renderSetupQr(const RuntimeStatus &status) {
    char wifiPayload[96];
    snprintf(wifiPayload, sizeof(wifiPayload), "WIFI:T:WPA;S:%s;P:%s;;",
        status.setupSsid, status.setupPassword);

    uint8_t qrStorage[128]{};
    QRCode qr;
    // Setup credentials are migrated to the compact form before this page is
    // shown. Version 2-L at 2x is the one supported physical layout; never
    // silently substitute a smaller or denser legacy QR.
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
}

void renderSetupBrand() {
    drawLargeProductTitle();
    drawCenteredText("synapse.sr", kLargeFont, 0, 128, 48);
}

void renderMenu(const configuration::DeviceConfig &config) {
    const bool screenIsOff = configuration::screenOff(config);
    const char *liveView = configuration::liveView(config) == configuration::LiveView::Hex ?
        "Hex" : "Text";
    const char *statusBar = "Auto";
    switch (configuration::statusBar(config)) {
        case configuration::StatusBar::Auto: statusBar = "Auto"; break;
        case configuration::StatusBar::Serial: statusBar = "Ser"; break;
        case configuration::StatusBar::Connection: statusBar = "Conn"; break;
        case configuration::StatusBar::Network: statusBar = "Net"; break;
    }

    const char *labels[] = {"Live", "Status", "Screen"};
    const char *values[] = {liveView, statusBar, screenIsOff ? "Off" : "On"};
    constexpr int16_t rowTop = 2;
    constexpr int16_t rowHeight = 20;
    constexpr int16_t rowTextLeft = 2;
    constexpr int16_t valueRight = 126;

    oled.setTextSize(kLargeFont);
    for (uint8_t row = 0; row < 3; ++row) {
        const int16_t y = rowTop + static_cast<int16_t>(row) * rowHeight;
        const bool selected = row == menuRow;
        if (selected) {
            oled.fillRect(0, y, 128, 18, SSD1306_WHITE);
            oled.setTextColor(SSD1306_BLACK, SSD1306_WHITE);
        } else {
            oled.setTextColor(SSD1306_WHITE, SSD1306_BLACK);
        }
        oled.setCursor(rowTextLeft, y + 1);
        oled.print(labels[row]);

        if (values[row][0] != '\0') {
            int16_t x1 = 0;
            int16_t y1 = 0;
            uint16_t width = 0;
            uint16_t height = 0;
            oled.getTextBounds(values[row], 0, 0, &x1, &y1, &width, &height);
            oled.setCursor(valueRight - static_cast<int16_t>(width) - x1, y + 1 - y1);
            oled.print(values[row]);
        }
    }
    oled.setTextColor(SSD1306_WHITE);
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
    uint32_t countdown,
    uint32_t baud) {
    switch (overlay) {
        case prg_button::Overlay::Baud: {
            drawCenteredText("BAUD", kLargeFont, 0, 128, 16);
            char baudText[12];
            snprintf(baudText, sizeof(baudText), "%lu", static_cast<unsigned long>(baud));
            drawCenteredText(baudText, kLargeFont, 0, 128, 44);
            break;
        }
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
    pinMode(kResetPin, OUTPUT);
    digitalWrite(kResetPin, LOW);
    delay(20);
    digitalWrite(kResetPin, HIGH);
    Wire.begin(kSdaPin, kSclPin);
    ready = oled.begin(SSD1306_SWITCHCAPVCC, kAddress);
    if (ready) {
        oled.clearDisplay();
        oled.setTextSize(kCompactFont);
        oled.setTextColor(SSD1306_WHITE);
        oled.setTextWrap(false);
        oled.display();
    }
}

void openMenu() {
    menuIsOpen = true;
    menuRow = 0;
    lastMenuInteraction = millis();
}

void closeMenu() {
    menuIsOpen = false;
}

bool menuOpen() {
    return menuIsOpen;
}

void moveMenuNext() {
    if (!menuIsOpen) return;
    if (menuRow == 2) {
        menuRow = 0;
        menuIsOpen = false;
        setupPage = SetupPage::Brand;
    } else {
        ++menuRow;
    }
    lastMenuInteraction = millis();
}

MenuAction selectMenuItem() {
    if (!menuIsOpen) return MenuAction::None;
    lastMenuInteraction = millis();
    switch (static_cast<MenuRow>(menuRow)) {
        case MenuRow::LiveView: return MenuAction::ToggleLiveView;
        case MenuRow::StatusBar: return MenuAction::CycleStatusBar;
        case MenuRow::Screen: return MenuAction::ToggleScreen;
    }
    return MenuAction::None;
}

void serviceMenuTimeout(bool buttonPressed) {
    if (menuIsOpen && !buttonPressed && millis() - lastMenuInteraction >= kMenuInactivityMs) {
        closeMenu();
    }
}

void advanceSetupPage() {
    if (menuIsOpen) {
        moveMenuNext();
    } else if (setupPage == SetupPage::Baud) {
        setupPage = SetupPage::Details;
    } else if (setupPage == SetupPage::Details) {
        setupPage = SetupPage::Qr;
    } else if (setupPage == SetupPage::Qr) {
        openMenu();
    } else {
        setupPage = SetupPage::Baud;
    }
}

bool setupPageActive(const configuration::DeviceConfig &config, bool serialTrafficSeen) {
    return config.ssid[0] == '\0' && !serialTrafficSeen;
}

bool setupBaudPageActive(
    const configuration::DeviceConfig &config,
    bool serialTrafficSeen) {
    return setupPageActive(config, serialTrafficSeen) && setupPage == SetupPage::Baud;
}

void render(
    const configuration::DeviceConfig &config,
    prg_button::Overlay activeOverlay,
    uint32_t countdown,
    bool serialTrafficSeen,
    const RuntimeStatus &status) {
    const uint32_t now = millis();
    if (!ready || now - lastRefresh < kRefreshMs) return;
    lastRefresh = now;

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
    const bool setupPageIsActive = setupPageActive(config, serialTrafficSeen);
    const auto mode = static_cast<configuration::DisplayMode>(config.display);
    if (!setupPageIsActive) setupPage = SetupPage::Baud;
    if (configuration::screenOff(config) && !overlayVisible && !menuIsOpen && !status.serialError) {
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
    if (overlayVisible) {
        renderOverlay(activeOverlay, countdown, config.baud);
    } else if (menuIsOpen) {
        renderMenu(config);
    } else if (status.serialError) {
        renderSerialError();
    } else if (setupPageIsActive) {
        if (setupPage == SetupPage::Baud) renderSetupBaud(config);
        else if (setupPage == SetupPage::Qr) renderSetupQr(status);
        else if (setupPage == SetupPage::Brand) renderSetupBrand();
        else renderSetupDetails(config, status);
    } else if (mode == configuration::DisplayMode::Hex) {
        renderLiveHex(config, status, serialTrafficSeen);
    } else if (mode == configuration::DisplayMode::Stats) {
        renderStats(config, status);
    } else {
        renderLiveText(config, status, serialTrafficSeen);
    }
    oled.display();
    displayHasContent = true;
}

}  // namespace oled_display
