// Written by SmartifyOS from the CAN bus defined in the
// car's app. Do not change it here: change the app and run it
// on your computer, and SmartifyOS writes it again.
#pragma once

#include <stdint.h>
#include <string.h>

// The protocol this header was written for, checked against the
// SmartifyOS-ESP32 library's own whichever is included first.
#define SMARTIFY_OS_NETWORK_PROTOCOL 1
#if defined(SMARTIFY_OS_PROTOCOL_VERSION) && \
    SMARTIFY_OS_PROTOCOL_VERSION != SMARTIFY_OS_NETWORK_PROTOCOL
#error "This header was written for another version of the SmartifyOS-ESP32 library"
#endif

namespace network {

// How the fields sit in a frame: one after the other, lowest bit first, each
// starting where the one before ended, the same as SmartifyOS reads them.
namespace bits {

inline void writeBits(uint8_t* data, uint8_t start, uint8_t bits, uint32_t raw) {
  for (uint8_t i = 0; i < bits; i++) {
    const uint16_t bit = start + i;
    if ((raw >> i) & 1u) data[bit >> 3] |= static_cast<uint8_t>(1u << (bit & 7));
  }
}

// Bits past the frame read as zero, so a shorter frame from older firmware
// still reads.
inline uint32_t readBits(const uint8_t* data, uint8_t length, uint8_t start,
                         uint8_t bits) {
  uint32_t raw = 0;
  for (uint8_t i = 0; i < bits; i++) {
    const uint16_t bit = start + i;
    if ((bit >> 3) >= length) break;
    if ((data[bit >> 3] >> (bit & 7)) & 1u) raw |= (1u << i);
  }
  return raw;
}

inline int64_t clampInt(int64_t value, int64_t low, int64_t high) {
  return value < low ? low : (value > high ? high : value);
}

inline int32_t signExtend(uint32_t raw, uint8_t bits) {
  if (bits >= 32) return static_cast<int32_t>(raw);
  const uint32_t sign = 1u << (bits - 1);
  return static_cast<int32_t>((raw ^ sign) - sign);
}

inline int64_t scaledToRaw(float value, float scale, float offset, uint8_t bits,
                           bool isSigned) {
  const float steps = (value - offset) / scale;
  const int64_t rounded =
      static_cast<int64_t>(steps < 0 ? steps - 0.5f : steps + 0.5f);
  const int64_t low = isSigned ? -(int64_t(1) << (bits - 1)) : 0;
  const int64_t high =
      isSigned ? (int64_t(1) << (bits - 1)) - 1 : (int64_t(1) << bits) - 1;
  return clampInt(rounded, low, high);
}

inline uint32_t floatToRaw(float value) {
  uint32_t raw;
  memcpy(&raw, &value, sizeof raw);
  return raw;
}

inline float rawToFloat(uint32_t raw) {
  float value;
  memcpy(&value, &raw, sizeof value);
  return value;
}

// A number past the last choice reads as the first, which is what a device
// with older firmware gets for a choice added later.
template <typename E>
inline E choice(uint32_t raw, uint32_t count) {
  return static_cast<E>(raw < count ? raw : 0);
}

}  // namespace bits

/// Every device on the bus. Pass yours to SmartifyOsCan.begin.
namespace nodes {
/// Buttons
constexpr uint8_t buttons = 1;
/// Pop-up headlights
constexpr uint8_t headlights = 2;
}  // namespace nodes

enum class MiataButton : uint8_t {
  wink = 0,
  previous = 1,
  next = 2,
};

struct ButtonPressed {
  static constexpr uint32_t id = 0x100;
  static constexpr bool extended = false;
  static constexpr uint8_t length = 1;

  MiataButton button = MiataButton::wink;

  void encode(uint8_t* data) const {
    memset(data, 0, length);
    bits::writeBits(data, 0, 2, static_cast<uint32_t>(button));
  }

  static ButtonPressed decode(const uint8_t* data, uint8_t dataLength) {
    ButtonPressed message;
    message.button = bits::choice<MiataButton>(bits::readBits(data, dataLength, 0, 2), 3);
    return message;
  }
};

struct HeadlightsState {
  static constexpr uint32_t id = 0x200;
  static constexpr bool extended = false;
  static constexpr uint8_t length = 2;

  uint8_t leftAngle = 0;
  uint8_t rightAngle = 0;

  void encode(uint8_t* data) const {
    memset(data, 0, length);
    bits::writeBits(data, 0, 8, static_cast<uint32_t>(bits::clampInt(leftAngle, 0, 255)));
    bits::writeBits(data, 8, 8, static_cast<uint32_t>(bits::clampInt(rightAngle, 0, 255)));
  }

  static HeadlightsState decode(const uint8_t* data, uint8_t dataLength) {
    HeadlightsState message;
    message.leftAngle = static_cast<uint8_t>(bits::readBits(data, dataLength, 0, 8));
    message.rightAngle = static_cast<uint8_t>(bits::readBits(data, dataLength, 8, 8));
    return message;
  }
};

struct HeadlightsRequest {
  static constexpr uint32_t id = 0x300;
  static constexpr bool extended = false;
  static constexpr uint8_t length = 1;

  bool up = false;

  void encode(uint8_t* data) const {
    memset(data, 0, length);
    bits::writeBits(data, 0, 1, up ? 1u : 0u);
  }

  static HeadlightsRequest decode(const uint8_t* data, uint8_t dataLength) {
    HeadlightsRequest message;
    message.up = bits::readBits(data, dataLength, 0, 1) != 0;
    return message;
  }
};

}  // namespace network
