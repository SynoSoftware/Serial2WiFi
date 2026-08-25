#include "oled_display.h"

#include <Adafruit_GFX.h>
#include <Adafruit_SSD1306.h>
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

Adafruit_SSD1306 oled(128, 64, &Wire, kResetPin);
bool ready = false;
bool displayHasContent = false;
uint32_t lastRefresh = 0;
uint32_t lastRateSample = 0;
uint64_t lastSerialReceived = 0;
uint64_t lastNetworkReceived = 0;
uint32_t serialRate = 0;
uint32_t networkRate = 0;

struct TextLine {
    char value[22];
    uint8_t length;
};

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
        current = (current + 1) % 7;
        if (count < 7) ++count;
    } else {
        count = 1;
        started = true;
    }
    resetLine(lines[current], direction);
}

void renderStatusRow(
    const RuntimeStatus &status,
    const configuration::DeviceConfig &config,
    bool serialTrafficSeen) {
    char line[22];
    const uint32_t phase = millis() % 6000;
    if (status.serialError) {
        snprintf(line, sizeof(line), "SERIAL ERROR");
    } else if (status.apActive && phase >= 4000) {
        snprintf(line, sizeof(line), "P:%s", status.setupPassword);
    } else {
        snprintf(line, sizeof(line), "%lu %s %s",
            static_cast<unsigned long>(config.baud),
            configuration::framingName(static_cast<configuration::Framing>(config.framing)),
            serialTrafficSeen ? "SER" : "WAIT");
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
    TextLine lines[7]{};
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

    renderStatusRow(status, config, serialTrafficSeen);
    const size_t firstLine = lineCount == 7 ? (currentLine + 1) % 7 : 0;
    for (size_t row = 0; row < 7; ++row) {
        oled.setCursor(0, static_cast<int16_t>((row + 1) * 8));
        if (row < lineCount) oled.print(lines[(firstLine + row) % 7].value);
    }
}

void renderLiveHex(
    const configuration::DeviceConfig &config,
    const RuntimeStatus &status,
    bool serialTrafficSeen) {
    display_history::DisplayByte snapshot[display_history::kCapacity];
    const size_t snapshotCount = display_history::snapshot(snapshot, display_history::kCapacity);
    size_t firstByte = snapshotCount > 42 ? snapshotCount - 42 : 0;
    renderStatusRow(status, config, serialTrafficSeen);
    for (size_t row = 0; row < 7; ++row) {
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
    oled.printf("U %lu/%lu/%lu", static_cast<unsigned long>(status.serialFifoOverflowErrors),
        static_cast<unsigned long>(status.serialFramingErrors),
        static_cast<unsigned long>(status.serialParityErrors));
}

void renderSetup(const configuration::DeviceConfig &config, const RuntimeStatus &status) {
    oled.setCursor(0, 0);
    oled.print(status.setupSsid);
    oled.setCursor(0, 8);
    oled.print("P:");
    oled.print(status.setupPassword);
    oled.setCursor(0, 24);
    oled.print("http://192.168.4.1");
    oled.setCursor(0, 40);
    oled.printf("BAUD %lu", static_cast<unsigned long>(config.baud));
    oled.setCursor(0, 48);
    oled.printf("FRAME %s", configuration::framingName(
            static_cast<configuration::Framing>(config.framing)));
    oled.setCursor(0, 56);
    oled.print("Send serial data");
}

void renderSerialError() {
    oled.setCursor(22, 16);
    oled.print("SERIAL ERROR");
    oled.setCursor(16, 40);
    oled.print("Reboot to retry");
}

void renderOverlay(prg_button::Overlay overlay, uint32_t countdown, uint32_t baud) {
    switch (overlay) {
        case prg_button::Overlay::Baud:
            oled.setCursor(43, 16);
            oled.print("BAUD");
            oled.setCursor(34, 32);
            oled.print(baud);
            break;
        case prg_button::Overlay::ResetWarning:
            oled.setCursor(24, 8);
            oled.print("FACTORY RESET");
            oled.setCursor(24, 24);
            oled.print("Keep holding...");
            oled.setCursor(61, 40);
            oled.print(countdown);
            break;
        case prg_button::Overlay::ResetComplete:
            oled.setCursor(25, 24);
            oled.print("RESET COMPLETE");
            break;
        case prg_button::Overlay::ResetFailed:
            oled.setCursor(31, 24);
            oled.print("RESET FAILED");
            break;
        case prg_button::Overlay::SaveFailed:
            oled.setCursor(34, 24);
            oled.print("SAVE FAILED");
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
        oled.setTextSize(1);
        oled.setTextColor(SSD1306_WHITE);
        oled.setTextWrap(false);
        oled.display();
    }
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
    if (now - lastRateSample >= 1000) {
        const uint32_t elapsed = now - lastRateSample;
        serialRate = static_cast<uint32_t>((status.serialToNetworkReceived - lastSerialReceived) * 1000ULL / elapsed);
        networkRate = static_cast<uint32_t>((status.networkToSerialReceived - lastNetworkReceived) * 1000ULL / elapsed);
        lastSerialReceived = status.serialToNetworkReceived;
        lastNetworkReceived = status.networkToSerialReceived;
        lastRateSample = now;
    }

    const bool overlayVisible = activeOverlay != prg_button::Overlay::None;
    const bool unconfigured = config.ssid[0] == '\0';
    const auto mode = static_cast<configuration::DisplayMode>(config.display);
    if (mode == configuration::DisplayMode::Off && !overlayVisible && !status.serialError) {
        if (displayHasContent) {
            oled.clearDisplay();
            oled.display();
            displayHasContent = false;
        }
        return;
    }

    oled.clearDisplay();
    oled.setTextSize(1);
    oled.setTextColor(SSD1306_WHITE);
    oled.setTextWrap(false);
    if (overlayVisible) {
        renderOverlay(activeOverlay, countdown, config.baud);
    } else if (status.serialError) {
        renderSerialError();
    } else if (mode == configuration::DisplayMode::Text &&
            unconfigured && !serialTrafficSeen) {
        renderSetup(config, status);
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
