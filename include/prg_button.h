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

void begin();
void service();

bool takeShortTap();
bool takeFactoryResetRequest();
void reportBaudCommit(bool succeeded);
void reportFactoryReset(bool succeeded);
bool restartPending();

Overlay overlay();
uint32_t resetCountdown();

}  // namespace prg_button
