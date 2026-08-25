#include <Arduino.h>
#include <Adafruit_GFX.h>
#include <Adafruit_SSD1306.h>
#include <Preferences.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>

#include <cstring>

namespace {

constexpr gpio_num_t UART_RX_PIN = GPIO_NUM_3;
constexpr gpio_num_t UART_TX_PIN = GPIO_NUM_1;
constexpr uint8_t PRG_PIN = 0;
constexpr uint8_t OLED_SDA_PIN = 4;
constexpr uint8_t OLED_SCL_PIN = 15;
constexpr uint8_t OLED_RESET_PIN = 16;
constexpr uint8_t OLED_ADDRESS = 0x3C;

constexpr uint32_t DEFAULT_BAUD = 9600;
constexpr size_t DISPLAY_HISTORY_CAPACITY = 1024;
constexpr uint32_t BUTTON_DEBOUNCE_MS = 30;
constexpr uint32_t SHORT_TAP_MAX_MS = 1000;
constexpr uint32_t RESET_WARNING_START_MS = 5000;
constexpr uint32_t FACTORY_RESET_MS = 10000;
constexpr uint32_t BAUD_OVERLAY_MS = 1000;
constexpr uint32_t DISPLAY_REFRESH_MS = 100;

constexpr uint16_t CONFIG_SCHEMA = 1;

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

enum class Direction : uint8_t {
  SerialToNetwork = 0,
  NetworkToSerial,
};

struct DisplayByte {
  uint8_t value;
  Direction direction;
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

static_assert(sizeof(DeviceConfig) < 400, "configuration must remain a small NVS blob");

Adafruit_SSD1306 oled(128, 64, &Wire, OLED_RESET_PIN);
Preferences preferences;
SemaphoreHandle_t configurationMutex = nullptr;

DeviceConfig currentConfig{};
portMUX_TYPE historyMux = portMUX_INITIALIZER_UNLOCKED;
DisplayByte displayHistory[DISPLAY_HISTORY_CAPACITY]{};
size_t historyHead = 0;
size_t historyCount = 0;
uint32_t historyDropped = 0;

bool serialTrafficSeen = false;
bool oledReady = false;
uint32_t lastDisplayRefresh = 0;

enum class Overlay : uint8_t {
  None = 0,
  Baud,
  ResetWarning,
  ResetComplete,
  ResetFailed,
  SaveFailed,
};

Overlay activeOverlay = Overlay::None;
uint32_t overlayStartedAt = 0;
uint32_t lastResetCountdown = UINT32_MAX;
bool resetSucceeded = false;

bool rawButtonState = true;
bool stableButtonState = true;
uint32_t buttonChangedAt = 0;
uint32_t buttonPressedAt = 0;
bool resetAttempted = false;

constexpr uint32_t SUPPORTED_BAUDS[] = {
    2400, 4800, 9600, 19200, 38400, 57600,
    115200, 230400, 460800, 921600, 1000000};
constexpr size_t SUPPORTED_BAUD_COUNT = sizeof(SUPPORTED_BAUDS) / sizeof(SUPPORTED_BAUDS[0]);

void copyField(char *destination, size_t destinationSize, const char *source) {
  if (destinationSize == 0) {
    return;
  }
  strncpy(destination, source, destinationSize - 1);
  destination[destinationSize - 1] = '\0';
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

uint32_t serialConfigFor(Framing framing) {
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
  for (uint32_t supported : SUPPORTED_BAUDS) {
    if (supported == baud) return true;
  }
  return false;
}

size_t boundedLength(const char *value, size_t capacity) {
  return strnlen(value, capacity);
}

bool validConfiguration(const DeviceConfig &candidate) {
  if (candidate.schema != CONFIG_SCHEMA || !supportedBaud(candidate.baud)) {
    return false;
  }

  Framing ignoredFraming;
  if (!framingFromName(framingName(static_cast<Framing>(candidate.framing)), ignoredFraming)) {
    return false;
  }

  if (candidate.display > static_cast<uint8_t>(DisplayMode::Off)) {
    return false;
  }
  if (candidate.wifiSecurity > static_cast<uint8_t>(WifiSecurity::Secured)) {
    return false;
  }
  if (boundedLength(candidate.ssid, sizeof(candidate.ssid)) >= sizeof(candidate.ssid) ||
      boundedLength(candidate.wifiPassword, sizeof(candidate.wifiPassword)) >= sizeof(candidate.wifiPassword) ||
      boundedLength(candidate.tcpHost, sizeof(candidate.tcpHost)) >= sizeof(candidate.tcpHost)) {
    return false;
  }

  const bool wifiConfigured = candidate.ssid[0] != '\0';
  if (!wifiConfigured) {
    if (candidate.wifiPassword[0] != '\0' || candidate.wifiSecurity != static_cast<uint8_t>(WifiSecurity::Unset)) {
      return false;
    }
  } else if (candidate.wifiSecurity == static_cast<uint8_t>(WifiSecurity::Unset)) {
    return false;
  }

  const bool tcpConfigured = candidate.tcpHost[0] != '\0';
  if (!tcpConfigured) {
    if (candidate.tcpPort != 0) return false;
  } else if (candidate.tcpPort == 0) {
    return false;
  }

  return true;
}

DeviceConfig factoryConfiguration() {
  DeviceConfig config{};
  config.schema = CONFIG_SCHEMA;
  config.baud = DEFAULT_BAUD;
  config.framing = static_cast<uint8_t>(Framing::EightN1);
  config.display = static_cast<uint8_t>(DisplayMode::Text);
  config.wifiSecurity = static_cast<uint8_t>(WifiSecurity::Unset);
  config.tcpPort = 0;
  return config;
}

bool loadConfiguration(DeviceConfig &config) {
  config = factoryConfiguration();
  if (!preferences.begin("s2w", true)) {
    return false;
  }

  const size_t storedLength = preferences.getBytesLength("cfg");
  if (storedLength != sizeof(DeviceConfig)) {
    preferences.end();
    return false;
  }

  DeviceConfig stored{};
  const size_t readLength = preferences.getBytes("cfg", &stored, sizeof(stored));
  preferences.end();
  if (readLength != sizeof(DeviceConfig) || !validConfiguration(stored)) {
    return false;
  }

  config = stored;
  return true;
}

void clearDisplayHistory() {
  portENTER_CRITICAL(&historyMux);
  historyHead = 0;
  historyCount = 0;
  historyDropped = 0;
  portEXIT_CRITICAL(&historyMux);
}

void appendDisplayByte(uint8_t value, Direction direction) {
  portENTER_CRITICAL(&historyMux);
  if (historyCount == DISPLAY_HISTORY_CAPACITY) {
    historyHead = (historyHead + 1) % DISPLAY_HISTORY_CAPACITY;
    historyCount--;
    historyDropped++;
  }
  const size_t tail = (historyHead + historyCount) % DISPLAY_HISTORY_CAPACITY;
  displayHistory[tail] = {value, direction};
  historyCount++;
  portEXIT_CRITICAL(&historyMux);
}

size_t copyDisplaySnapshot(DisplayByte *destination, size_t capacity) {
  portENTER_CRITICAL(&historyMux);
  const size_t count = historyCount < capacity ? historyCount : capacity;
  const size_t start = historyCount <= capacity
                           ? historyHead
                           : (historyHead + historyCount - capacity) % DISPLAY_HISTORY_CAPACITY;
  for (size_t i = 0; i < count; ++i) {
    destination[i] = displayHistory[(start + i) % DISPLAY_HISTORY_CAPACITY];
  }
  portEXIT_CRITICAL(&historyMux);
  return count;
}

void handleSerialReceive() {
  while (Serial.available() > 0) {
    const int value = Serial.read();
    if (value < 0) break;
    serialTrafficSeen = true;
    appendDisplayByte(static_cast<uint8_t>(value), Direction::SerialToNetwork);
  }
}

void handleSerialError(hardwareSerial_error_t error) {
  (void)error;
}

void startSerial(uint32_t baud, Framing framing, bool clearInput) {
  Serial.end();
  if (clearInput) {
    while (Serial.available() > 0) {
      Serial.read();
    }
  }
  Serial.setRxBufferSize(8192);
  Serial.begin(baud, serialConfigFor(framing), UART_RX_PIN, UART_TX_PIN, false);
  Serial.setHwFlowCtrlMode(UART_HW_FLOWCTRL_DISABLE);
  Serial.onReceive(handleSerialReceive, false);
  Serial.onReceiveError(handleSerialError);
}

bool applySerialConfiguration(uint32_t baud, Framing framing) {
  clearDisplayHistory();
  serialTrafficSeen = false;
  startSerial(baud, framing, true);
  return true;
}

bool commitConfiguration(const DeviceConfig &candidate) {
  if (configurationMutex == nullptr || xSemaphoreTake(configurationMutex, portMAX_DELAY) != pdTRUE) {
    return false;
  }

  bool committed = false;
  do {
    if (!validConfiguration(candidate)) break;

    const bool serialChanged = candidate.baud != currentConfig.baud ||
                               candidate.framing != currentConfig.framing;

    if (!preferences.begin("s2w", false)) break;
    const size_t written = preferences.putBytes("cfg", &candidate, sizeof(candidate));
    preferences.end();
    if (written != sizeof(candidate)) break;

    currentConfig = candidate;
    if (serialChanged) {
      applySerialConfiguration(candidate.baud, static_cast<Framing>(candidate.framing));
    }
    committed = true;
  } while (false);

  xSemaphoreGive(configurationMutex);
  return committed;
}

uint32_t nextBaud(uint32_t current) {
  for (size_t i = 0; i < SUPPORTED_BAUD_COUNT; ++i) {
    if (SUPPORTED_BAUDS[i] == current) {
      return SUPPORTED_BAUDS[(i + 1) % SUPPORTED_BAUD_COUNT];
    }
  }
  return SUPPORTED_BAUDS[0];
}

void setOverlay(Overlay overlay) {
  activeOverlay = overlay;
  overlayStartedAt = millis();
  lastResetCountdown = UINT32_MAX;
}

void cycleBaud() {
  DeviceConfig candidate = currentConfig;
  candidate.baud = nextBaud(currentConfig.baud);
  if (commitConfiguration(candidate)) {
    setOverlay(Overlay::Baud);
  } else {
    setOverlay(Overlay::SaveFailed);
  }
}

bool factoryReset() {
  if (!preferences.begin("s2w", false)) {
    return false;
  }
  const bool cleared = preferences.clear();
  preferences.end();
  if (cleared) {
    currentConfig = factoryConfiguration();
    clearDisplayHistory();
    serialTrafficSeen = false;
  }
  return cleared;
}

void updateButton() {
  const uint32_t now = millis();
  const bool raw = digitalRead(PRG_PIN) != LOW;

  if (raw != rawButtonState) {
    rawButtonState = raw;
    buttonChangedAt = now;
  }

  if (rawButtonState != stableButtonState && now - buttonChangedAt >= BUTTON_DEBOUNCE_MS) {
    stableButtonState = rawButtonState;
    if (!stableButtonState) {
      buttonPressedAt = now;
      resetAttempted = false;
      lastResetCountdown = UINT32_MAX;
    } else {
      const uint32_t heldFor = now - buttonPressedAt;
      if (!resetAttempted && heldFor < SHORT_TAP_MAX_MS) {
        cycleBaud();
      }
      if (resetAttempted && resetSucceeded) {
        ESP.restart();
      }
      activeOverlay = Overlay::None;
    }
  }

  if (!stableButtonState && !resetAttempted) {
    const uint32_t heldFor = now - buttonPressedAt;
    if (heldFor >= RESET_WARNING_START_MS) {
      const uint32_t remaining = (FACTORY_RESET_MS - min(heldFor, FACTORY_RESET_MS) + 999) / 1000;
      if (remaining != lastResetCountdown) {
        lastResetCountdown = remaining;
        setOverlay(Overlay::ResetWarning);
      }
    }
    if (heldFor >= FACTORY_RESET_MS) {
      resetAttempted = true;
      resetSucceeded = factoryReset();
      setOverlay(resetSucceeded ? Overlay::ResetComplete : Overlay::ResetFailed);
    }
  }
}

struct TextLine {
  char value[22];
  uint8_t length;
};

void resetLine(TextLine &line, Direction direction) {
  memset(line.value, ' ', sizeof(line.value));
  line.value[21] = '\0';
  line.length = 0;
  const char *tag = direction == Direction::SerialToNetwork ? "S> " : "<S ";
  memcpy(line.value, tag, 3);
  line.length = 3;
}

void renderLiveText() {
  DisplayByte snapshot[DISPLAY_HISTORY_CAPACITY];
  const size_t snapshotCount = copyDisplaySnapshot(snapshot, DISPLAY_HISTORY_CAPACITY);
  TextLine lines[7]{};
  size_t currentLine = 0;
  size_t lineCount = 0;
  Direction lineDirection = Direction::SerialToNetwork;
  bool lineStarted = false;

  auto startLine = [&](Direction direction) {
    if (lineStarted) {
      currentLine = (currentLine + 1) % 7;
      if (lineCount < 7) lineCount++;
    } else {
      currentLine = 0;
      lineCount = 1;
      lineStarted = true;
    }
    lineDirection = direction;
    resetLine(lines[currentLine], direction);
  };

  for (size_t i = 0; i < snapshotCount; ++i) {
    const DisplayByte &byte = snapshot[i];
    if (!lineStarted) startLine(byte.direction);
    if (byte.direction != lineDirection && lines[currentLine].length > 3) {
      startLine(byte.direction);
    }
    if (byte.value == '\n' || lines[currentLine].length >= 21) {
      startLine(byte.direction);
    }
    if (byte.value == '\r' || byte.value == '\n') continue;
    char rendered = (byte.value >= 0x20 && byte.value <= 0x7E)
                        ? static_cast<char>(byte.value)
                        : '.';
    if (lines[currentLine].length < 21) {
      lines[currentLine].value[lines[currentLine].length++] = rendered;
    }
  }

  if (!lineStarted && !serialTrafficSeen) {
    startLine(Direction::SerialToNetwork);
    copyField(lines[currentLine].value + 3, 18, "WAITING FOR SERIAL");
    lines[currentLine].length = 21;
  }

  oled.setTextSize(1);
  oled.setTextColor(SSD1306_WHITE);
  oled.setCursor(0, 0);
  char status[22];
  snprintf(status, sizeof(status), "%lu %s %s",
           static_cast<unsigned long>(currentConfig.baud),
           framingName(static_cast<Framing>(currentConfig.framing)),
           serialTrafficSeen ? "SER" : "WAIT");
  oled.print(status);

  const size_t firstLine = lineCount == 7 ? (currentLine + 1) % 7 : 0;
  for (size_t row = 0; row < 7; ++row) {
    oled.setCursor(0, static_cast<int16_t>((row + 1) * 8));
    if (row < lineCount) {
      const size_t index = (firstLine + row) % 7;
      oled.print(lines[index].value);
    }
  }
}

void renderOverlay() {
  oled.setTextSize(1);
  oled.setTextColor(SSD1306_WHITE);
  oled.setTextWrap(false);
  oled.setCursor(0, 0);

  switch (activeOverlay) {
    case Overlay::Baud:
      oled.setCursor(43, 16);
      oled.print("BAUD");
      oled.setCursor(34, 32);
      oled.print(currentConfig.baud);
      break;
    case Overlay::ResetWarning: {
      const uint32_t heldFor = millis() - buttonPressedAt;
      const uint32_t remaining = (FACTORY_RESET_MS - min(heldFor, FACTORY_RESET_MS) + 999) / 1000;
      oled.setCursor(24, 8);
      oled.print("FACTORY RESET");
      oled.setCursor(24, 24);
      oled.print("Keep holding...");
      oled.setCursor(61, 40);
      oled.print(remaining);
      break;
    }
    case Overlay::ResetComplete:
      oled.setCursor(25, 24);
      oled.print("RESET COMPLETE");
      break;
    case Overlay::ResetFailed:
      oled.setCursor(31, 24);
      oled.print("RESET FAILED");
      break;
    case Overlay::SaveFailed:
      oled.setCursor(34, 24);
      oled.print("SAVE FAILED");
      break;
    case Overlay::None:
      break;
  }
}

void updateOverlayLifetime() {
  if (activeOverlay == Overlay::Baud && millis() - overlayStartedAt >= BAUD_OVERLAY_MS) {
    activeOverlay = Overlay::None;
  }
}

void renderDisplay() {
  const uint32_t now = millis();
  if (!oledReady || now - lastDisplayRefresh < DISPLAY_REFRESH_MS) return;
  lastDisplayRefresh = now;
  updateOverlayLifetime();

  const bool overlayVisible = activeOverlay != Overlay::None;
  const DisplayMode mode = static_cast<DisplayMode>(currentConfig.display);
  if (mode == DisplayMode::Off && !overlayVisible) {
    return;
  }

  oled.clearDisplay();
  if (overlayVisible) {
    renderOverlay();
  } else {
    // Milestone 1A intentionally implements only the factory Live Text renderer.
    // Hex and Statistics are added in Milestone 2.
    renderLiveText();
  }
  oled.display();
}

void initializeOled() {
  pinMode(OLED_RESET_PIN, OUTPUT);
  digitalWrite(OLED_RESET_PIN, LOW);
  delay(20);
  digitalWrite(OLED_RESET_PIN, HIGH);
  Wire.begin(OLED_SDA_PIN, OLED_SCL_PIN);
  oledReady = oled.begin(SSD1306_SWITCHCAPVCC, OLED_ADDRESS);
  if (oledReady) {
    oled.clearDisplay();
    oled.setTextColor(SSD1306_WHITE);
    oled.display();
  }
}

}  // namespace

void setup() {
  pinMode(PRG_PIN, INPUT_PULLUP);
  rawButtonState = digitalRead(PRG_PIN) != LOW;
  stableButtonState = rawButtonState;
  buttonChangedAt = millis();

  configurationMutex = xSemaphoreCreateMutex();
  currentConfig = factoryConfiguration();
  loadConfiguration(currentConfig);

  initializeOled();
  startSerial(currentConfig.baud, static_cast<Framing>(currentConfig.framing), false);
}

void loop() {
  updateButton();
  renderDisplay();
  delay(1);
}
