#include "prg_button.h"

#include <esp_timer.h>
#include <freertos/FreeRTOS.h>
#include <freertos/queue.h>
#include <freertos/task.h>

#include "configuration.h"

namespace prg_button {
namespace {

constexpr uint8_t kPin = 0;
constexpr uint32_t kDebounceMs = 30;
constexpr uint64_t kResetWarningStartUs = 5000000;
constexpr uint64_t kFactoryResetUs = 10000000;
constexpr uint32_t kOverlayMs = 1000;

// The gesture machine is wall-clock based, so this period sets only how finely
// the pin is sampled. 5 ms resolves every transition a 30 ms debounce can
// accept, with six samples to spare, and costs one digitalRead per period.
constexpr uint32_t kPollMs = 5;
// Above the loop task, so no amount of loop work can delay a press, and below
// the Wi-Fi and event tasks, which must not be delayed by a button. Pinned to
// the loop's core rather than core 0, where the Wi-Fi driver runs: the whole
// point of this task is to be free of stalls that belong to other work.
constexpr UBaseType_t kTaskPriority = 3;
constexpr uint32_t kTaskStackBytes = 3072;
// Deep enough that a loop busy with a save still collects every repeat of a
// hold, in order. A full queue means the loop has stopped consuming, and a
// dropped repeat is the honest result of that.
constexpr size_t kEventQueueLength = 8;
// Never more than one outstanding result per gesture; four is slack, not need.
constexpr size_t kCommandQueueLength = 4;

enum class Phase : uint8_t {
    Idle,
    Pressing,
    Repeating,
    ResetWarning,
    ResetFired,
};

// What the loop reports back into a gesture already in progress. Sending these
// rather than writing the state keeps every byte of the machine owned by the
// task that runs it.
enum class Command : uint8_t {
    SaveFailed,
    ResetSucceeded,
    ResetFailed,
};

// A fired reset waits for its result before it can end. Pending is not
// "failed": the two must be told apart, because a release seen during Pending
// is still the release that asks for the restart.
enum class ResetOutcome : uint8_t {
    Pending,
    Succeeded,
    Failed,
};

QueueHandle_t eventQueue = nullptr;
QueueHandle_t commandQueue = nullptr;
portMUX_TYPE statusLock = portMUX_INITIALIZER_UNLOCKED;
Status publishedStatus{Overlay::None, 0};

// Written by the loop, read by the task each pass. Independent scalars with no
// ordering requirement between them: a threshold that takes effect one poll
// late is not a fact anyone can observe.
uint32_t longPressMs = configuration::kDefaultLongPressMs;
uint32_t longPressRepeatMs = configuration::kDefaultLongPressRepeatMs;

// Everything below belongs to the button task alone.
bool rawPressed = false;
bool pressed = false;
uint32_t pressedChangedAt = 0;
Phase phase = Phase::Idle;
uint64_t pressedAtUs = 0;
uint64_t pressLongPressUs = 0;
ResetOutcome resetOutcome = ResetOutcome::Pending;
uint32_t nextRepeatAt = 0;
Overlay result = Overlay::None;
uint32_t overlayStartedAt = 0;

uint64_t heldUs() {
    // The panel and the network still vary between passes even on their own
    // tasks; gesture timing must remain wall-clock based instead of depending
    // on how often this task happens to run.
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

// False when the loop has stopped consuming. A dropped click or repeat is the
// honest result of that; a dropped ResetRequested is not, and enterPhase below
// is the one caller that has to care.
bool emit(Event event) {
    return xQueueSend(eventQueue, &event, 0) == pdTRUE;
}

void expireSaveFailedOverlay() {
    if (result == Overlay::SaveFailed && millis() - overlayStartedAt >= kOverlayMs) {
        result = Overlay::None;
    }
}

void applyCommands() {
    Command command;
    while (xQueueReceive(commandQueue, &command, 0) == pdTRUE) {
        overlayStartedAt = millis();
        switch (command) {
            case Command::SaveFailed:
                result = Overlay::SaveFailed;
                break;
            case Command::ResetSucceeded:
                resetOutcome = ResetOutcome::Succeeded;
                result = Overlay::ResetComplete;
                break;
            case Command::ResetFailed:
                resetOutcome = ResetOutcome::Failed;
                result = Overlay::ResetFailed;
                break;
        }
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
    resetOutcome = ResetOutcome::Pending;
    // Latch the threshold for this press. The web UI can change longPressMs at
    // any moment, and moving the boundary under a gesture already in progress
    // would regress its phase and release a hold as a click.
    pressLongPressUs = static_cast<uint64_t>(
        __atomic_load_n(&longPressMs, __ATOMIC_RELAXED)) * 1000ULL;
}

void enterPhase(Phase next) {
    phase = next;
    if (next == Phase::Repeating) {
        emit(Event::HoldStarted);
        nextRepeatAt = millis() + __atomic_load_n(&longPressRepeatMs, __ATOMIC_RELAXED);
    } else if (next == Phase::ResetFired) {
        // A request nobody received will never be answered, and service()
        // holds the gesture open until it is. Nothing was erased, which is
        // exactly what Failed says, and the press can end normally.
        if (!emit(Event::ResetRequested)) resetOutcome = ResetOutcome::Failed;
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
        emit(Event::Click);
    } else if (ended == Phase::ResetFired && resetOutcome == ResetOutcome::Succeeded) {
        emit(Event::RestartRequested);
    }
}

void service() {
    // Expire first: the press handling below returns early on the release and
    // idle paths, and the overlay must still expire on both.
    expireSaveFailedOverlay();
    updateDebounce();
    // A fired reset owns the gesture until the loop reports how it went.
    // Erasing three NVS namespaces takes the loop real time, and the release
    // that asks for the restart can land inside that window. Ending the press
    // here would spend it on nothing, so the press simply is not ended yet:
    // `pressed` is already false and stays false, so the same release is still
    // waiting on the pass that finds the result.
    if (phase == Phase::ResetFired && resetOutcome == ResetOutcome::Pending) return;
    if (!pressed) { endPress(); return; }
    if (phase == Phase::Idle) { beginPress(); return; }

    const Phase next = phaseFor(heldUs());
    if (next != phase) { enterPhase(next); return; }
    if (phase == Phase::Repeating &&
            static_cast<int32_t>(millis() - nextRepeatAt) >= 0) {
        emit(Event::HoldRepeated);
        nextRepeatAt = millis() + __atomic_load_n(&longPressRepeatMs, __ATOMIC_RELAXED);
    }
}

Status composeStatus() {
    Status current{};
    // The live phase outranks a stored result. The two fields are read as one
    // value and must agree, and a SAVE FAILED raised just before the warning
    // would otherwise hide a running countdown.
    current.overlay = phase == Phase::ResetWarning ? Overlay::ResetWarning : result;
    if (phase != Phase::ResetWarning) return current;
    // The press can cross the reset threshold between this pass and the one
    // that moves the phase. Without this the subtraction below underflows.
    const uint64_t heldForUs = heldUs();
    if (heldForUs >= kFactoryResetUs) return current;
    current.resetCountdown =
        static_cast<uint32_t>((kFactoryResetUs - heldForUs + 999999) / 1000000);
    return current;
}

void publishStatus() {
    const Status current = composeStatus();
    portENTER_CRITICAL(&statusLock);
    publishedStatus = current;
    portEXIT_CRITICAL(&statusLock);
}

void buttonTask(void *) {
    for (;;) {
        applyCommands();
        service();
        publishStatus();
        // The only wait in this task, and it is a scheduler wait: the pin is
        // sampled on a fixed period, never spun on.
        vTaskDelay(pdMS_TO_TICKS(kPollMs));
    }
}

}  // namespace

void begin(uint32_t configuredLongPressMs, uint32_t configuredLongPressRepeatMs) {
    pinMode(kPin, INPUT_PULLUP);
    rawPressed = digitalRead(kPin) == LOW;
    pressed = rawPressed;
    pressedChangedAt = millis();
    phase = Phase::Idle;
    result = Overlay::None;
    longPressMs = configuredLongPressMs;
    longPressRepeatMs = configuredLongPressRepeatMs;
    eventQueue = xQueueCreate(kEventQueueLength, sizeof(Event));
    commandQueue = xQueueCreate(kCommandQueueLength, sizeof(Command));
    // Both queues exist before the task does, so the task never has to test
    // them. The accessors below still do, because the loop calls them whether
    // or not this succeeded.
    if (eventQueue == nullptr || commandQueue == nullptr) return;
    xTaskCreatePinnedToCore(
        buttonTask, "prgButton", kTaskStackBytes, nullptr, kTaskPriority, nullptr,
        ARDUINO_RUNNING_CORE);
}

void setLongPressMs(uint32_t configuredLongPressMs) {
    __atomic_store_n(&longPressMs, configuredLongPressMs, __ATOMIC_RELAXED);
}

void setLongPressRepeatMs(uint32_t configuredLongPressRepeatMs) {
    __atomic_store_n(&longPressRepeatMs, configuredLongPressRepeatMs, __ATOMIC_RELAXED);
}

Event takeEvent() {
    Event event = Event::None;
    if (eventQueue == nullptr) return event;
    xQueueReceive(eventQueue, &event, 0);
    return event;
}

void reportSaveFailed() {
    const Command command = Command::SaveFailed;
    if (commandQueue != nullptr) xQueueSend(commandQueue, &command, 0);
}

void reportFactoryReset(bool succeeded) {
    const Command command = succeeded ? Command::ResetSucceeded : Command::ResetFailed;
    if (commandQueue != nullptr) xQueueSend(commandQueue, &command, 0);
}

Status status() {
    Status current;
    portENTER_CRITICAL(&statusLock);
    current = publishedStatus;
    portEXIT_CRITICAL(&statusLock);
    return current;
}

}  // namespace prg_button
