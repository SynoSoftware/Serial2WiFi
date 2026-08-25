#pragma once

#include <Arduino.h>

namespace prg_button {

enum class Overlay : uint8_t {
    None = 0,
    Baud,
    ResetWarning,
    ResetComplete,
    ResetFailed,
    SaveFailed,
};

struct HoldEvent {
    bool resetEligible;
};

void begin();
void bootComplete();
void service();
bool isPressed();

bool takeSingleClick();
bool takeHold(HoldEvent &event);
bool takeFactoryResetRequest();
void reportBaudCommit(bool succeeded);
void reportSaveFailed();
void reportFactoryReset(bool succeeded);
bool restartPending();

Overlay overlay();
uint32_t resetCountdown();

}  // namespace prg_button
