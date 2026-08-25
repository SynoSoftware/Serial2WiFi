#pragma once

#include <Arduino.h>

namespace transport_buffer {

struct Buffer {
    uint8_t *storage;
    size_t capacity;
    size_t head;
    size_t count;
    uint64_t dropped;
    portMUX_TYPE lock;
};

void initialize(Buffer &buffer, uint8_t *storage, size_t capacity);
size_t push(Buffer &buffer, const uint8_t *data, size_t length);
size_t pop(Buffer &buffer, uint8_t *destination, size_t capacity);
size_t clear(Buffer &buffer);
void recordDropped(Buffer &buffer, size_t amount);
size_t available(const Buffer &buffer);
size_t freeSpace(const Buffer &buffer);
uint64_t droppedBytes(const Buffer &buffer);

}  // namespace transport_buffer
