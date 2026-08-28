#pragma once

#include <WiFi.h>

namespace browser_terminal {

// Sent as the first byte of every observation frame. The browser shows both
// sides of the bridge, so a frame that did not say which side it came from
// could not be attributed.
enum class Direction : uint8_t {
    FromSerial = 0,
    ToSerial,
};

void begin();
bool accept(WiFiClient client, const char *webSocketKey);
void service();
void onSerialTraffic(Direction direction, const uint8_t *data, size_t length);

}  // namespace browser_terminal
