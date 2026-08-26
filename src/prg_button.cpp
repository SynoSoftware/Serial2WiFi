#include "prg_button.h"

#include <esp_timer.h>

#include "configuration.h"

namespace prg_button {
namespace {

constexpr uint8_t kPin = 0;
constexpr uint32_t kDebounceMs = 30;
constexpr uint64_t kResetWarningStartUs = 5000000;
constexpr uint64_t kFactoryResetUs = 10000000;
constexpr uint32_t kOverlayMs = 1000;

bool rawState = true;
bool stableState = true;
enum class GestureState : uint8_t {
    WaitingForPress,
    Pressed,
};
GestureState gestureState = GestureState::WaitingForPress;
uint32_t changedAt = 0;
uint64_t pressedAtUs = 0;
bool resetEligibleAtPress = false;
bool resetAttempted = false;
bool resetSucceeded = false;
bool singleClick = false;
HoldEvent holdEvent{};
bool holdEventPending = false;
bool holdStarted = false;
uint32_t nextHoldEventAt = 0;
uint32_t longPressMs = configuration::kDefaultLongPressMs;
uint32_t longPressRepeatMs = configuration::kDefaultLongPressRepeatMs;
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

void begin(uint32_t configuredLongPressMs, uint32_t configuredLongPressRepeatMs) {
    pinMode(kPin, INPUT_PULLUP);
    rawState = digitalRead(kPin) != LOW;
    stableState = rawState;
    gestureState = GestureState::WaitingForPress;
    changedAt = millis();
    longPressMs = configuredLongPressMs;
    longPressRepeatMs = configuredLongPressRepeatMs;
}

void setLongPressMs(uint32_t configuredLongPressMs) {
    longPressMs = configuredLongPressMs;
}

void setLongPressRepeatMs(uint32_t configuredLongPressRepeatMs) {
    longPressRepeatMs = configuredLongPressRepeatMs;
}

bool isPressed() {
    return gestureState == GestureState::Pressed;
}

void service() {
    const uint32_t now = millis();
    // OLED and network work can vary between loop iterations; gesture timing
    // must remain wall-clock based instead of depending on loop frequency.
    const uint64_t nowUs = static_cast<uint64_t>(esp_timer_get_time());
    const bool raw = digitalRead(kPin) != LOW;

    if (raw != rawState) {
        rawState = raw;
        changedAt = now;
    }

    if (rawState != stableState && now - changedAt >= kDebounceMs) {
        stableState = rawState;
        if (!stableState) {
            if (gestureState == GestureState::WaitingForPress) {
                pressedAtUs = nowUs;
                // Factory reset is an emergency action available throughout
                // runtime. The ten-second threshold below is the accidental-
                // reset guard; it is not a startup-only availability window.
                resetEligibleAtPress = true;
                resetAttempted = false;
                resetSucceeded = false;
                holdStarted = false;
                nextHoldEventAt = 0;
                gestureState = GestureState::Pressed;
            }
        } else {
            if (gestureState == GestureState::Pressed) {
                const uint64_t heldForUs = nowUs - pressedAtUs;
                if (!resetAttempted && !holdStarted &&
                        heldForUs < static_cast<uint64_t>(longPressMs) * 1000ULL) {
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
        const uint64_t heldForUs = nowUs - pressedAtUs;
        if (heldForUs >= kFactoryResetUs) {
            if (resetEligibleAtPress) {
                resetAttempted = true;
                factoryResetRequest = true;
            }
        } else if (resetEligibleAtPress && heldForUs >= kResetWarningStartUs) {
            active = Overlay::ResetWarning;
        } else if (!holdStarted &&
                heldForUs >= static_cast<uint64_t>(longPressMs) * 1000ULL) {
            holdStarted = true;
            holdEvent.resetEligible = resetEligibleAtPress;
            holdEvent.first = true;
            holdEventPending = true;
            nextHoldEventAt = now + longPressRepeatMs;
        } else if (holdStarted && heldForUs < kResetWarningStartUs &&
                static_cast<int32_t>(now - nextHoldEventAt) >= 0) {
            holdEvent.resetEligible = resetEligibleAtPress;
            holdEvent.first = false;
            holdEventPending = true;
            nextHoldEventAt = now + longPressRepeatMs;
        }
    }

    if (active == Overlay::SaveFailed &&
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
    const uint64_t heldForUs = static_cast<uint64_t>(esp_timer_get_time()) - pressedAtUs;
    if (heldForUs < kResetWarningStartUs) return 0;
    const uint64_t clamped = min(heldForUs, kFactoryResetUs);
    return static_cast<uint32_t>((kFactoryResetUs - clamped + 999999) / 1000000);
}

}  // namespace prg_button
