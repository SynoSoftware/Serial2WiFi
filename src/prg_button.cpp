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

enum class Phase : uint8_t {
    Idle,
    Pressing,
    Repeating,
    ResetWarning,
    ResetFired,
};

bool rawPressed = false;
bool pressed = false;
uint32_t pressedChangedAt = 0;
Phase phase = Phase::Idle;
Event pending = Event::None;
uint64_t pressedAtUs = 0;
uint64_t pressLongPressUs = 0;
bool resetSucceeded = false;
uint32_t nextRepeatAt = 0;
uint32_t longPressMs = configuration::kDefaultLongPressMs;
uint32_t longPressRepeatMs = configuration::kDefaultLongPressRepeatMs;
Overlay result = Overlay::None;
uint32_t overlayStartedAt = 0;

uint64_t heldUs() {
    // OLED and network work can vary between loop iterations; gesture timing
    // must remain wall-clock based instead of depending on loop frequency.
    return static_cast<uint64_t>(esp_timer_get_time()) - pressedAtUs;
}

Phase phaseFor(uint64_t heldForUs) {
    // Longest threshold first. Any other order lets an earlier branch match a
    // longer hold, so the reset thresholds would never be reached.
    if (heldForUs >= kFactoryResetUs) return Phase::ResetFired;
    if (heldForUs >= kResetWarningStartUs) return Phase::ResetWarning;
    if (heldForUs >= pressLongPressUs) return Phase::Repeating;
    return Phase::Pressing;
}

void expireSaveFailedOverlay() {
    if (result == Overlay::SaveFailed && millis() - overlayStartedAt >= kOverlayMs) {
        result = Overlay::None;
    }
}

void updateDebounce() {
    const uint32_t now = millis();
    const bool raw = digitalRead(kPin) == LOW;
    if (raw != rawPressed) {
        rawPressed = raw;
        pressedChangedAt = now;
    }
    if (rawPressed != pressed && now - pressedChangedAt >= kDebounceMs) {
        pressed = rawPressed;
    }
}

void beginPress() {
    phase = Phase::Pressing;
    pressedAtUs = static_cast<uint64_t>(esp_timer_get_time());
    resetSucceeded = false;
    // Latch the threshold for this press. The web UI can change longPressMs
    // from the same loop iteration, and moving the boundary under a gesture
    // already in progress would regress its phase and release a hold as a
    // click.
    pressLongPressUs = static_cast<uint64_t>(longPressMs) * 1000ULL;
}

void enterPhase(Phase next) {
    phase = next;
    if (next == Phase::Repeating) {
        pending = Event::HoldStarted;
        nextRepeatAt = millis() + longPressRepeatMs;
    } else if (next == Phase::ResetFired) {
        pending = Event::ResetRequested;
    }
}

void endPress() {
    if (phase == Phase::Idle) return;
    const Phase ended = phase;
    phase = Phase::Idle;
    if (result == Overlay::ResetComplete || result == Overlay::ResetFailed) {
        result = Overlay::None;
    }
    if (ended == Phase::Pressing) {
        pending = Event::Click;
    } else if (ended == Phase::ResetFired && resetSucceeded) {
        pending = Event::RestartRequested;
    }
}

}  // namespace

void begin(uint32_t configuredLongPressMs, uint32_t configuredLongPressRepeatMs) {
    pinMode(kPin, INPUT_PULLUP);
    rawPressed = digitalRead(kPin) == LOW;
    pressed = rawPressed;
    pressedChangedAt = millis();
    phase = Phase::Idle;
    pending = Event::None;
    result = Overlay::None;
    longPressMs = configuredLongPressMs;
    longPressRepeatMs = configuredLongPressRepeatMs;
}

void setLongPressMs(uint32_t configuredLongPressMs) {
    longPressMs = configuredLongPressMs;
}

void setLongPressRepeatMs(uint32_t configuredLongPressRepeatMs) {
    longPressRepeatMs = configuredLongPressRepeatMs;
}

void service() {
    // Expire first: the press handling below returns early on the release and
    // idle paths, and the overlay must still expire on both.
    expireSaveFailedOverlay();
    updateDebounce();
    if (!pressed) { endPress(); return; }
    if (phase == Phase::Idle) { beginPress(); return; }

    const Phase next = phaseFor(heldUs());
    if (next != phase) { enterPhase(next); return; }
    if (phase == Phase::Repeating &&
            static_cast<int32_t>(millis() - nextRepeatAt) >= 0) {
        pending = Event::HoldRepeated;
        nextRepeatAt = millis() + longPressRepeatMs;
    }
}

Event takeEvent() {
    const Event event = pending;
    pending = Event::None;
    return event;
}

void reportSaveFailed() {
    result = Overlay::SaveFailed;
    overlayStartedAt = millis();
}

void reportFactoryReset(bool succeeded) {
    resetSucceeded = succeeded;
    result = succeeded ? Overlay::ResetComplete : Overlay::ResetFailed;
    overlayStartedAt = millis();
}

Overlay overlay() {
    // The live phase outranks a stored result. overlay() and resetCountdown()
    // are read in the same frame and must agree, and a SAVE FAILED raised just
    // before the warning would otherwise hide a running countdown.
    if (phase == Phase::ResetWarning) return Overlay::ResetWarning;
    return result;
}

uint32_t resetCountdown() {
    if (phase != Phase::ResetWarning) return 0;
    // service() is the only writer of phase, and this runs later in the same
    // loop pass, so the press can already have crossed the reset threshold
    // while phase still reads ResetWarning. Without this the subtraction
    // below underflows.
    const uint64_t heldForUs = heldUs();
    if (heldForUs >= kFactoryResetUs) return 0;
    return static_cast<uint32_t>((kFactoryResetUs - heldForUs + 999999) / 1000000);
}

}  // namespace prg_button
