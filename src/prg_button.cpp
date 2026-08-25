#include "prg_button.h"

namespace prg_button {
namespace {

constexpr uint8_t kPin = 0;
constexpr uint32_t kDebounceMs = 30;
constexpr uint32_t kLongHoldMs = 250;
constexpr uint32_t kClickMaxMs = kLongHoldMs - 1;
constexpr uint32_t kResetWarningStartMs = 5000;
constexpr uint32_t kFactoryResetMs = 10000;
constexpr uint32_t kStartupResetWindowMs = 10000;
constexpr uint32_t kOverlayMs = 1000;

bool rawState = true;
bool stableState = true;
enum class GestureState : uint8_t {
    WaitingForPress,
    Pressed,
};
GestureState gestureState = GestureState::WaitingForPress;
uint32_t changedAt = 0;
uint32_t pressedAt = 0;
uint32_t bootedAt = 0;
bool resetWindowActive = false;
bool resetEligibleAtPress = false;
bool resetAttempted = false;
bool resetSucceeded = false;
bool singleClick = false;
HoldEvent holdEvent{};
bool holdEventPending = false;
bool factoryResetRequest = false;
bool reboot = false;
Overlay active = Overlay::None;
uint32_t overlayStartedAt = 0;

void clearReleaseOverlays() {
    if (active == Overlay::ResetWarning) {
        active = Overlay::None;
    }
}

}  // namespace

void begin() {
    pinMode(kPin, INPUT_PULLUP);
    rawState = digitalRead(kPin) != LOW;
    stableState = rawState;
    gestureState = GestureState::WaitingForPress;
    changedAt = millis();
    bootedAt = 0;
    resetWindowActive = false;
}

void bootComplete() {
    bootedAt = millis();
    resetWindowActive = true;
}

bool isPressed() {
    return gestureState == GestureState::Pressed;
}

void service() {
    const uint32_t now = millis();
    const bool raw = digitalRead(kPin) != LOW;

    if (raw != rawState) {
        rawState = raw;
        changedAt = now;
    }

    if (rawState != stableState && now - changedAt >= kDebounceMs) {
        stableState = rawState;
        if (!stableState) {
            if (gestureState == GestureState::WaitingForPress) {
                pressedAt = now;
                resetEligibleAtPress = resetWindowActive && now - bootedAt < kStartupResetWindowMs;
                resetAttempted = false;
                resetSucceeded = false;
                gestureState = GestureState::Pressed;
            }
        } else {
            if (gestureState == GestureState::Pressed) {
                const uint32_t heldFor = now - pressedAt;
                if (!resetAttempted && heldFor >= kLongHoldMs &&
                        heldFor < kResetWarningStartMs) {
                    holdEvent.resetEligible = resetEligibleAtPress;
                    holdEventPending = true;
                } else if (!resetAttempted && heldFor <= kClickMaxMs) {
                    // A released, debounced click is the common operation. Emit
                    // it now; waiting for a possible second click makes page
                    // cycling feel broken.
                    singleClick = true;
                }
                if (resetAttempted && resetSucceeded) reboot = true;
            }
            gestureState = GestureState::WaitingForPress;
            clearReleaseOverlays();
            if (active == Overlay::ResetComplete || active == Overlay::ResetFailed) active = Overlay::None;
        }
    }

    if (gestureState == GestureState::Pressed && !resetAttempted) {
        const uint32_t heldFor = now - pressedAt;
        if (heldFor >= kFactoryResetMs) {
            if (resetEligibleAtPress) {
                resetAttempted = true;
                factoryResetRequest = true;
            }
        } else if (resetEligibleAtPress && heldFor >= kResetWarningStartMs) {
            active = Overlay::ResetWarning;
        }
    }

    if ((active == Overlay::Baud || active == Overlay::SaveFailed) &&
            now - overlayStartedAt >= kOverlayMs) {
        active = Overlay::None;
    }

}

bool takeSingleClick() {
    const bool result = singleClick;
    singleClick = false;
    return result;
}

bool takeHold(HoldEvent &event) {
    if (!holdEventPending) return false;
    event = holdEvent;
    holdEventPending = false;
    return true;
}

bool takeFactoryResetRequest() {
    const bool result = factoryResetRequest;
    factoryResetRequest = false;
    return result;
}

void reportBaudCommit(bool succeeded) {
    active = succeeded ? Overlay::Baud : Overlay::SaveFailed;
    overlayStartedAt = millis();
}

void reportSaveFailed() {
    active = Overlay::SaveFailed;
    overlayStartedAt = millis();
}

void reportFactoryReset(bool succeeded) {
    resetSucceeded = succeeded;
    active = succeeded ? Overlay::ResetComplete : Overlay::ResetFailed;
    overlayStartedAt = millis();
}

bool restartPending() {
    if (!reboot || digitalRead(kPin) == LOW) return false;
    reboot = false;
    return true;
}

Overlay overlay() {
    return active;
}

uint32_t resetCountdown() {
    if (gestureState != GestureState::Pressed || !resetEligibleAtPress) return 0;
    const uint32_t heldFor = millis() - pressedAt;
    if (heldFor < kResetWarningStartMs) return 0;
    const uint32_t clamped = min(heldFor, kFactoryResetMs);
    return (kFactoryResetMs - clamped + 999) / 1000;
}

}  // namespace prg_button
