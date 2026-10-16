// Internal: receiving new firmware over the bus, the way PROTOCOL.md
// describes. It runs on the library's own task, never your loop(), so a
// mistake in your code cannot keep the device from being updated.
#pragma once

#include <Arduino.h>
#include <driver/twai.h>
#include <esp_ota_ops.h>

#include "SmartifyOsProtocol.h"

namespace smartify_os {

class Can;

class Updater {
 public:
  /// Looks at how this firmware started: on trial after an update, or back
  /// on the old one after an update that did not come up.
  void begin(Can* can, uint8_t node);

  /// A request or a data frame for this device.
  void handle(const twai_message_t& frame);

  /// Notices a block that stopped arriving, and a trial that ran out.
  void tick(uint32_t now);

  /// Whether new firmware is being received right now.
  bool isUpdating() const { return handle_ != 0; }

  /// Whether this firmware is new and waits for the car to confirm it.
  bool isOnTrial() const { return onTrial_; }

  /// Whether the last update did not come up and this is the old firmware.
  bool hasRolledBack() const { return rolledBack_; }

 private:
  void begin(const uint8_t* data, uint8_t length);
  void block(const uint8_t* data, uint8_t length);
  void data(const uint8_t* data, uint8_t length);
  void end(const uint8_t* data, uint8_t length);
  void confirm();
  void abort();
  void restart();
  void writeBlock();
  void stopUpdate();
  void reply(uint8_t request, uint8_t status, int64_t next = -1);

  Can* can_ = nullptr;
  uint8_t node_ = 0;

  const esp_partition_t* target_ = nullptr;
  esp_ota_handle_t handle_ = 0;
  uint32_t size_ = 0;
  uint32_t written_ = 0;
  uint32_t crc_ = 0;

  bool blockActive_ = false;
  bool ignoringData_ = false;
  uint32_t blockOffset_ = 0;
  uint16_t blockLength_ = 0;
  uint16_t framesExpected_ = 0;
  uint16_t framesReceived_ = 0;
  uint8_t receivedMap_[protocol::framesPerBlock / 8] = {0};
  uint8_t block_[protocol::blockSize] = {0};
  uint32_t lastDataAt_ = 0;

  bool onTrial_ = false;
  bool rolledBack_ = false;
  uint32_t trialStartedAt_ = 0;
};

/// Internal: the CRC-32 zip uses, carried on from [crc] (0 to start).
uint32_t crc32(uint32_t crc, const uint8_t* data, size_t length);

}  // namespace smartify_os
