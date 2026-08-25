#pragma once

#include <WiFi.h>

namespace browser_terminal {

void begin();
bool accept(WiFiClient client, const char *webSocketKey, bool allowTransmit);
void service();
void onSerialData(const uint8_t *data, size_t length);

}  // namespace browser_terminal
