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

struct HoldEvent {
    bool resetEligible;
    bool first;
};

void begin(uint32_t longPressMs, uint32_t longPressRepeatMs);
void setLongPressMs(uint32_t longPressMs);
void setLongPressRepeatMs(uint32_t longPressRepeatMs);
void service();
bool isPressed();

bool takeSingleClick();
bool takeHold(HoldEvent &event);
bool takeFactoryResetRequest();
void reportSaveFailed();
void reportFactoryReset(bool succeeded);
bool restartPending();

Overlay overlay();
uint32_t resetCountdown();

}  // namespace prg_button
