#include "SmartifyOsUpdater.h"

#include <esp_system.h>

#include "SmartifyOsCan.h"

// Arduino marks a new firmware as good the moment it starts, which would
// leave nothing to undo when it turns out not to work. This tells it to wait:
// the updater marks it good itself, once the car has confirmed it.
extern "C" bool verifyRollbackLater() { return true; }

namespace smartify_os {

namespace {

// How long a block may stop arriving before the car is told to send it again.
constexpr uint32_t blockTimeoutMs = 300;

// How long new firmware has to be confirmed by the car before the device goes
// back to its old firmware.
constexpr uint32_t trialMs = 60000;

uint32_t readUint32(const uint8_t* data) {
  return static_cast<uint32_t>(data[0]) |
         (static_cast<uint32_t>(data[1]) << 8) |
         (static_cast<uint32_t>(data[2]) << 16) |
         (static_cast<uint32_t>(data[3]) << 24);
}

}  // namespace

uint32_t crc32(uint32_t crc, const uint8_t* data, size_t length) {
  crc = ~crc;
  for (size_t i = 0; i < length; i++) {
    crc ^= data[i];
    for (int bit = 0; bit < 8; bit++) {
      crc = (crc & 1) ? (crc >> 1) ^ 0xEDB88320u : crc >> 1;
    }
  }
  return ~crc;
}

void Updater::begin(Can* can, uint8_t node) {
  can_ = can;
  node_ = node;

  const esp_partition_t* running = esp_ota_get_running_partition();
  esp_ota_img_states_t state;
  if (running != nullptr &&
      esp_ota_get_state_partition(running, &state) == ESP_OK &&
      state == ESP_OTA_IMG_PENDING_VERIFY) {
    onTrial_ = true;
    trialStartedAt_ = millis();
    log_i("SmartifyOS: new firmware, waiting for the car to confirm it");
  }
  rolledBack_ = esp_ota_get_last_invalid_partition() != nullptr;
}

void Updater::handle(const twai_message_t& frame) {
  if (frame.data_length_code == 0) return;
  const uint8_t* data = frame.data;
  const uint8_t length = frame.data_length_code;

  if (data[0] & protocol::dataFlag) {
    this->data(data, length);
    return;
  }
  switch (data[0]) {
    case protocol::requestBegin:
      begin(data, length);
      break;
    case protocol::requestBlock:
      block(data, length);
      break;
    case protocol::requestEnd:
      end(data, length);
      break;
    case protocol::requestConfirm:
      confirm();
      break;
    case protocol::requestAbort:
      abort();
      break;
    case protocol::requestRestart:
      restart();
      break;
    default:
      // A request from a newer SmartifyOS: not for this firmware.
      break;
  }
}

void Updater::tick(uint32_t now) {
  if (blockActive_ && now - lastDataAt_ > blockTimeoutMs) {
    blockActive_ = false;
    reply(protocol::requestBlock, protocol::statusIncomplete, blockOffset_);
  }
  if (onTrial_ && now - trialStartedAt_ > trialMs) {
    log_w("SmartifyOS: the new firmware was never confirmed, going back");
    Can::forgetCrashes();
    esp_ota_mark_app_invalid_rollback_and_reboot();
  }
}

void Updater::begin(const uint8_t* data, uint8_t length) {
  if (length < 5) return;
  const uint32_t size = readUint32(data + 1);

  // A new begin throws away an update that never ended, the car starting
  // over after losing the device halfway.
  stopUpdate();

  target_ = esp_ota_get_next_update_partition(nullptr);
  if (target_ == nullptr) {
    reply(protocol::requestBegin, protocol::statusNoSpace);
    return;
  }
  if (size == 0 || size > target_->size) {
    reply(protocol::requestBegin, protocol::statusTooBig);
    return;
  }
  // Erased a sector at a time as the firmware comes in, rather than all at
  // once now, which would keep the device off the bus for many seconds.
  if (esp_ota_begin(target_, OTA_WITH_SEQUENTIAL_WRITES, &handle_) != ESP_OK) {
    handle_ = 0;
    reply(protocol::requestBegin, protocol::statusFlashFailed);
    return;
  }
  size_ = size;
  written_ = 0;
  crc_ = 0;
  log_i("SmartifyOS: receiving %lu bytes of new firmware",
        static_cast<unsigned long>(size));
  reply(protocol::requestBegin, protocol::statusDone);
}

void Updater::block(const uint8_t* data, uint8_t length) {
  if (length < 7) return;
  if (handle_ == 0) {
    reply(protocol::requestBlock, protocol::statusWrongTime);
    return;
  }
  const uint32_t offset = readUint32(data + 1);
  const uint16_t blockLength = data[5] | (data[6] << 8);

  if (offset < written_) {
    // Its answer went missing: this one is written already.
    blockActive_ = false;
    ignoringData_ = true;
    reply(protocol::requestBlock, protocol::statusDone, written_);
    return;
  }
  if (offset > written_ || blockLength == 0 ||
      blockLength > protocol::blockSize || offset + blockLength > size_) {
    blockActive_ = false;
    ignoringData_ = true;
    reply(protocol::requestBlock, protocol::statusOutOfOrder, written_);
    return;
  }

  blockActive_ = true;
  ignoringData_ = false;
  blockOffset_ = offset;
  blockLength_ = blockLength;
  framesExpected_ =
      (blockLength + protocol::bytesPerFrame - 1) / protocol::bytesPerFrame;
  framesReceived_ = 0;
  memset(receivedMap_, 0, sizeof receivedMap_);
  lastDataAt_ = millis();
}

void Updater::data(const uint8_t* data, uint8_t length) {
  if (!blockActive_ || ignoringData_) return;
  const uint8_t seq = data[0] & 0x7F;
  if (seq >= framesExpected_) return;
  lastDataAt_ = millis();

  const uint16_t start = seq * protocol::bytesPerFrame;
  const uint16_t left = blockLength_ - start;
  const uint16_t count = length - 1 < left ? length - 1 : left;
  memcpy(block_ + start, data + 1, count);
  if (!(receivedMap_[seq >> 3] & (1 << (seq & 7)))) {
    receivedMap_[seq >> 3] |= 1 << (seq & 7);
    framesReceived_++;
  }

  if (framesReceived_ == framesExpected_) {
    writeBlock();
  } else if (seq == framesExpected_ - 1) {
    // The last one arrived and some before it did not.
    blockActive_ = false;
    reply(protocol::requestBlock, protocol::statusIncomplete, blockOffset_);
  }
}

void Updater::writeBlock() {
  blockActive_ = false;
  // Nothing is received while this writes, which is why the car waits for
  // the answer before sending more.
  if (esp_ota_write(handle_, block_, blockLength_) != ESP_OK) {
    reply(protocol::requestBlock, protocol::statusFlashFailed);
    stopUpdate();
    return;
  }
  crc_ = crc32(crc_, block_, blockLength_);
  written_ += blockLength_;
  reply(protocol::requestBlock, protocol::statusDone, written_);
}

void Updater::end(const uint8_t* data, uint8_t length) {
  if (length < 5) return;
  if (handle_ == 0 || written_ != size_) {
    reply(protocol::requestEnd, protocol::statusWrongTime);
    return;
  }
  if (readUint32(data + 1) != crc_) {
    reply(protocol::requestEnd, protocol::statusChecksumMismatch);
    stopUpdate();
    return;
  }

  // Checks it is a whole firmware for this chip, with its own checksum.
  const esp_err_t checked = esp_ota_end(handle_);
  handle_ = 0;
  if (checked != ESP_OK) {
    reply(protocol::requestEnd, checked == ESP_ERR_OTA_VALIDATE_FAILED
                                    ? protocol::statusInvalidFirmware
                                    : protocol::statusFlashFailed);
    return;
  }
  if (esp_ota_set_boot_partition(target_) != ESP_OK) {
    reply(protocol::requestEnd, protocol::statusFlashFailed);
    return;
  }

  reply(protocol::requestEnd, protocol::statusDone);
  log_i("SmartifyOS: new firmware is in, restarting into it");
  // Long enough for the answer to go out.
  vTaskDelay(pdMS_TO_TICKS(100));
  Can::forgetCrashes();
  esp_restart();
}

void Updater::confirm() {
  if (onTrial_) {
    if (esp_ota_mark_app_valid_cancel_rollback() != ESP_OK) {
      reply(protocol::requestConfirm, protocol::statusFlashFailed);
      return;
    }
    onTrial_ = false;
    rolledBack_ = false;
    log_i("SmartifyOS: the car confirmed the new firmware");
  }
  // Also when there was nothing to confirm, so a confirm sent again after a
  // lost answer still hears it went well.
  reply(protocol::requestConfirm, protocol::statusDone);
}

void Updater::abort() {
  stopUpdate();
  reply(protocol::requestAbort, protocol::statusDone);
}

void Updater::restart() {
  reply(protocol::requestRestart, protocol::statusDone);
  vTaskDelay(pdMS_TO_TICKS(100));
  Can::forgetCrashes();
  esp_restart();
}

void Updater::stopUpdate() {
  if (handle_ != 0) {
    esp_ota_abort(handle_);
    handle_ = 0;
  }
  blockActive_ = false;
  ignoringData_ = false;
  size_ = 0;
  written_ = 0;
  crc_ = 0;
}

void Updater::reply(uint8_t request, uint8_t status, int64_t next) {
  uint8_t data[6] = {static_cast<uint8_t>(0x80 | request), status, 0, 0, 0, 0};
  uint8_t length = 2;
  if (next >= 0) {
    const uint32_t value = static_cast<uint32_t>(next);
    data[2] = value & 0xFF;
    data[3] = (value >> 8) & 0xFF;
    data[4] = (value >> 16) & 0xFF;
    data[5] = (value >> 24) & 0xFF;
    length = 6;
  }
  can_->sendFrame(protocol::replyBase + node_, data, length);
}

}  // namespace smartify_os
