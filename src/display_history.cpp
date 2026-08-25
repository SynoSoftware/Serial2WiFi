#include "display_history.h"

#include <freertos/FreeRTOS.h>

namespace display_history {
namespace {

portMUX_TYPE historyMux = portMUX_INITIALIZER_UNLOCKED;
DisplayByte history[kCapacity]{};
size_t head = 0;
size_t count = 0;
uint32_t dropped = 0;

}  // namespace

void clear() {
    portENTER_CRITICAL(&historyMux);
    head = 0;
    count = 0;
    dropped = 0;
    portEXIT_CRITICAL(&historyMux);
}

void append(uint8_t value, Direction direction) {
    // History is evidence only. Never make the UART receive path wait for the renderer.
    if (portTRY_ENTER_CRITICAL(&historyMux, 0) != pdTRUE) {
        __atomic_fetch_add(&dropped, 1, __ATOMIC_RELAXED);
        return;
    }
    if (count == kCapacity) {
        head = (head + 1) % kCapacity;
        --count;
        ++dropped;
    }
    const size_t tail = (head + count) % kCapacity;
    history[tail] = {value, direction};
    ++count;
    portEXIT_CRITICAL(&historyMux);
}

size_t snapshot(DisplayByte *destination, size_t capacity) {
    if (destination == nullptr || capacity == 0) return 0;

    size_t copied = 0;
    size_t start = 0;
    if (portTRY_ENTER_CRITICAL(&historyMux, 0) != pdTRUE) return 0;
    const size_t currentCount = count;
    copied = currentCount < capacity ? currentCount : capacity;
    start = currentCount <= capacity ? head : (head + currentCount - capacity) % kCapacity;
    portEXIT_CRITICAL(&historyMux);

    for (size_t i = 0; i < copied; ++i) {
        // Copy one item per non-blocking critical section. A busy history ring
        // loses display evidence instead of delaying serial capture.
        if (portTRY_ENTER_CRITICAL(&historyMux, 0) != pdTRUE) return i;
        destination[i] = history[(start + i) % kCapacity];
        portEXIT_CRITICAL(&historyMux);
    }
    return copied;
}

uint32_t droppedBytes() {
    portENTER_CRITICAL(&historyMux);
    const uint32_t result = dropped;
    portEXIT_CRITICAL(&historyMux);
    return result;
}

}  // namespace display_history
