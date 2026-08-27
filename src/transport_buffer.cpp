#include "transport_buffer.h"

#include <cstring>
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

// These run inside interrupts-off critical sections on the forwarding path,
// so bytes move with at most two memcpy segments instead of per-byte loops.

namespace {

// Appends at the tail. The caller holds buffer.lock.
void copyIn(Buffer &buffer, const uint8_t *data, size_t length) {
    const size_t tail = (buffer.head + buffer.count) % buffer.capacity;
    const size_t firstSegment = min(length, buffer.capacity - tail);
    memcpy(buffer.storage + tail, data, firstSegment);
    memcpy(buffer.storage, data + firstSegment, length - firstSegment);
    buffer.count += length;
}

}  // namespace

size_t push(Buffer &buffer, const uint8_t *data, size_t length) {
    if (data == nullptr || length == 0) return 0;
    portENTER_CRITICAL(&buffer.lock);
    // A full buffer keeps the newest bytes: the oldest stored bytes give way
    // first, and input larger than the whole buffer sheds its own oldest bytes.
    size_t dropped = 0;
    if (length >= buffer.capacity) {
        dropped = buffer.count + (length - buffer.capacity);
        data += length - buffer.capacity;
        length = buffer.capacity;
        buffer.head = 0;
        buffer.count = 0;
    } else if (buffer.count + length > buffer.capacity) {
        dropped = buffer.count + length - buffer.capacity;
        buffer.head = (buffer.head + dropped) % buffer.capacity;
        buffer.count -= dropped;
    }
    copyIn(buffer, data, length);
    buffer.dropped += dropped;
    portEXIT_CRITICAL(&buffer.lock);
    return dropped;
}

bool pushIfFits(Buffer &buffer, const uint8_t *data, size_t length) {
    if (data == nullptr || length == 0) return false;
    portENTER_CRITICAL(&buffer.lock);
    if (length > buffer.capacity - buffer.count) {
        portEXIT_CRITICAL(&buffer.lock);
        return false;
    }
    copyIn(buffer, data, length);
    portEXIT_CRITICAL(&buffer.lock);
    return true;
}

size_t pop(Buffer &buffer, uint8_t *destination, size_t capacity) {
    if (destination == nullptr || capacity == 0) return 0;
    portENTER_CRITICAL(&buffer.lock);
    const size_t copied = min(capacity, buffer.count);
    const size_t firstSegment = min(copied, buffer.capacity - buffer.head);
    memcpy(destination, buffer.storage + buffer.head, firstSegment);
    memcpy(destination + firstSegment, buffer.storage, copied - firstSegment);
    buffer.head = (buffer.head + copied) % buffer.capacity;
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
