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

// The overlay and the countdown describe one moment of one gesture and must
// agree: a countdown drawn beside the wrong overlay is a lie about a factory
// reset. They are published together so a reader cannot catch the pair
// half-updated between two calls.
struct Status {
    Overlay overlay;
    uint32_t resetCountdown;
};

// Starts the polling task. Everything below is called from the loop task; the
// button state itself belongs to that task and to nothing else.
void begin(uint32_t longPressMs, uint32_t longPressRepeatMs);
void setLongPressMs(uint32_t longPressMs);
void setLongPressRepeatMs(uint32_t longPressRepeatMs);

Event takeEvent();
void reportSaveFailed();
void reportFactoryReset(bool succeeded);

Status status();

}  // namespace prg_button
