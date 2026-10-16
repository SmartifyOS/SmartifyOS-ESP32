// The SmartifyOS CAN protocol's numbers, the same ones PROTOCOL.md describes.
// Used inside the library; your own code never needs them.
#pragma once

#include <stdint.h>

namespace smartify_os {
namespace protocol {

// A device answering the car's computer, the car's computer asking a device,
// and a device saying it is there: each plus the device's node number.
constexpr uint32_t replyBase = 0x580;
constexpr uint32_t requestBase = 0x600;
constexpr uint32_t heartbeatBase = 0x700;

// Byte 0 of a heartbeat.
constexpr uint8_t stateStarting = 0x00;
constexpr uint8_t stateSafeMode = 0x04;
constexpr uint8_t stateRunning = 0x05;
constexpr uint8_t stateUpdating = 0x7F;

// Byte 5 of a heartbeat.
constexpr uint8_t flagCodeRunning = 0x01;
constexpr uint8_t flagOnTrial = 0x02;
constexpr uint8_t flagRolledBack = 0x04;

// Byte 0 of a request. The answer has the top bit set.
constexpr uint8_t requestBegin = 0x01;
constexpr uint8_t requestBlock = 0x02;
constexpr uint8_t requestEnd = 0x03;
constexpr uint8_t requestConfirm = 0x04;
constexpr uint8_t requestAbort = 0x05;
constexpr uint8_t requestRestart = 0x06;

// Set in byte 0 of a frame carrying part of a block, its place in the block
// below it.
constexpr uint8_t dataFlag = 0x80;
constexpr uint8_t bytesPerFrame = 7;
constexpr uint16_t framesPerBlock = 128;
constexpr uint16_t blockSize = bytesPerFrame * framesPerBlock;

// Byte 1 of an answer.
constexpr uint8_t statusDone = 0x00;
constexpr uint8_t statusTooBig = 0x01;
constexpr uint8_t statusNoSpace = 0x02;
constexpr uint8_t statusWrongTime = 0x03;
constexpr uint8_t statusFlashFailed = 0x04;
constexpr uint8_t statusInvalidFirmware = 0x05;
constexpr uint8_t statusChecksumMismatch = 0x06;
constexpr uint8_t statusOutOfOrder = 0x07;
constexpr uint8_t statusIncomplete = 0x08;

}  // namespace protocol
}  // namespace smartify_os
