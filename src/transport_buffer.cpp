#include "transport_buffer.h"

#include <freertos/FreeRTOS.h>

namespace transport_buffer {

void initialize(Buffer &buffer, uint8_t *storage, size_t capacity) {
    buffer.storage = storage;
    buffer.capacity = capacity;
    buffer.head = 0;
    buffer.count = 0;
    buffer.dropped = 0;
    buffer.lock = portMUX_INITIALIZER_UNLOCKED;
}

size_t push(Buffer &buffer, const uint8_t *data, size_t length) {
    if (data == nullptr || length == 0) return 0;
    portENTER_CRITICAL(&buffer.lock);
    size_t dropped = 0;
    for (size_t i = 0; i < length; ++i) {
        if (buffer.count == buffer.capacity) {
            buffer.head = (buffer.head + 1) % buffer.capacity;
            --buffer.count;
            ++buffer.dropped;
            ++dropped;
        }
        const size_t tail = (buffer.head + buffer.count) % buffer.capacity;
        buffer.storage[tail] = data[i];
        ++buffer.count;
    }
    portEXIT_CRITICAL(&buffer.lock);
    return dropped;
}

size_t pop(Buffer &buffer, uint8_t *destination, size_t capacity) {
    if (destination == nullptr || capacity == 0) return 0;
    portENTER_CRITICAL(&buffer.lock);
    const size_t copied = min(capacity, buffer.count);
    for (size_t i = 0; i < copied; ++i) {
        destination[i] = buffer.storage[buffer.head];
        buffer.head = (buffer.head + 1) % buffer.capacity;
    }
    buffer.count -= copied;
    portEXIT_CRITICAL(&buffer.lock);
    return copied;
}

size_t clear(Buffer &buffer) {
    portENTER_CRITICAL(&buffer.lock);
    const size_t removed = buffer.count;
    buffer.head = 0;
    buffer.count = 0;
    buffer.dropped += removed;
    portEXIT_CRITICAL(&buffer.lock);
    return removed;
}

void recordDropped(Buffer &buffer, size_t amount) {
    if (amount == 0) return;
    portENTER_CRITICAL(&buffer.lock);
    buffer.dropped += amount;
    portEXIT_CRITICAL(&buffer.lock);
}

size_t available(const Buffer &buffer) {
    portENTER_CRITICAL(const_cast<portMUX_TYPE *>(&buffer.lock));
    const size_t result = buffer.count;
    portEXIT_CRITICAL(const_cast<portMUX_TYPE *>(&buffer.lock));
    return result;
}

size_t freeSpace(const Buffer &buffer) {
    portENTER_CRITICAL(const_cast<portMUX_TYPE *>(&buffer.lock));
    const size_t result = buffer.capacity - buffer.count;
    portEXIT_CRITICAL(const_cast<portMUX_TYPE *>(&buffer.lock));
    return result;
}

uint64_t droppedBytes(const Buffer &buffer) {
    portENTER_CRITICAL(const_cast<portMUX_TYPE *>(&buffer.lock));
    const uint64_t result = buffer.dropped;
    portEXIT_CRITICAL(const_cast<portMUX_TYPE *>(&buffer.lock));
    return result;
}

}  // namespace transport_buffer
