#pragma once

#include <Arduino.h>

namespace prg_button {

enum class Overlay : uint8_t {
    None = 0,
    ResetWarning,
    ResetComplete,
    ResetFailed,
    SaveFailed,
};

enum class Event : uint8_t {
    None = 0,
    Click,
    HoldStarted,
    HoldRepeated,
    ResetRequested,
    RestartRequested,
};

void begin(uint32_t longPressMs, uint32_t longPressRepeatMs);
void setLongPressMs(uint32_t longPressMs);
void setLongPressRepeatMs(uint32_t longPressRepeatMs);
void service();

Event takeEvent();
void reportSaveFailed();
void reportFactoryReset(bool succeeded);

Overlay overlay();
uint32_t resetCountdown();

}  // namespace prg_button
