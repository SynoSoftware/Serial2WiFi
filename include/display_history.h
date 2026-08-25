#pragma once

#include <Arduino.h>

namespace display_history {

constexpr size_t kCapacity = 1024;

enum class Direction : uint8_t {
    SerialToNetwork = 0,
    NetworkToSerial,
};

struct DisplayByte {
    uint8_t value;
    Direction direction;
};

void clear();
void append(uint8_t value, Direction direction);
size_t snapshot(DisplayByte *destination, size_t capacity);
uint32_t droppedBytes();

}  // namespace display_history
