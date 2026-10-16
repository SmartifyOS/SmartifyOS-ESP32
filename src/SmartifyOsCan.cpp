#include "SmartifyOsCan.h"

#include <esp_attr.h>
#include <esp_system.h>

smartify_os::Can SmartifyOsCan;

namespace smartify_os {

namespace {

// How many crashes in a row put the device in safe mode.
constexpr uint32_t crashesForSafeMode = 3;

// How long the device has to run before the crashes counted so far are
// forgotten.
constexpr uint32_t healthyAfterMs = 30000;

// How long loop() may take to get back to update() before the device counts
// as stuck.
constexpr uint32_t stuckAfterMs = 3000;

// Kept through a crash and a restart, not through a loss of power. The magic
// number tells kept memory from what was there at power on.
constexpr uint32_t crashMagic = 0x534F5343;  // "SOSC"
RTC_NOINIT_ATTR uint32_t keptMagic;
RTC_NOINIT_ATTR uint32_t keptCrashes;

bool wasCrash(esp_reset_reason_t reason) {
  return reason == ESP_RST_PANIC || reason == ESP_RST_INT_WDT ||
         reason == ESP_RST_TASK_WDT || reason == ESP_RST_WDT;
}

// One preset per bitrate. Each is its own initialisation, since the presets
// are lists of named fields that only initialise, never assign.
#define SMARTIFY_OS_TIMING(rate, preset)    \
  case rate: {                              \
    twai_timing_config_t config = preset(); \
    *timing = config;                       \
    return true;                            \
  }

bool timingFor(uint32_t bitrate, twai_timing_config_t* timing) {
  switch (bitrate) {
    SMARTIFY_OS_TIMING(1000000, TWAI_TIMING_CONFIG_1MBITS)
    SMARTIFY_OS_TIMING(800000, TWAI_TIMING_CONFIG_800KBITS)
    SMARTIFY_OS_TIMING(500000, TWAI_TIMING_CONFIG_500KBITS)
    SMARTIFY_OS_TIMING(250000, TWAI_TIMING_CONFIG_250KBITS)
    SMARTIFY_OS_TIMING(125000, TWAI_TIMING_CONFIG_125KBITS)
    SMARTIFY_OS_TIMING(100000, TWAI_TIMING_CONFIG_100KBITS)
    SMARTIFY_OS_TIMING(50000, TWAI_TIMING_CONFIG_50KBITS)
    SMARTIFY_OS_TIMING(25000, TWAI_TIMING_CONFIG_25KBITS)
    default:
      return false;
  }
}

#undef SMARTIFY_OS_TIMING

}  // namespace

void Can::forgetCrashes() {
  keptMagic = crashMagic;
  keptCrashes = 0;
}

bool Can::begin(uint8_t node, gpio_num_t tx, gpio_num_t rx,
                FirmwareVersion firmware, uint32_t bitrate) {
  node_ = node;
  firmware_ = firmware;

  if (keptMagic != crashMagic) forgetCrashes();
  if (wasCrash(esp_reset_reason())) keptCrashes++;
  safeMode_ = keptCrashes >= crashesForSafeMode;

  twai_general_config_t general =
      TWAI_GENERAL_CONFIG_DEFAULT(tx, rx, TWAI_MODE_NORMAL);
  general.tx_queue_len = 16;
  general.rx_queue_len = 64;
  general.alerts_enabled = TWAI_ALERT_BUS_OFF | TWAI_ALERT_BUS_RECOVERED;
  twai_timing_config_t timing;
  if (!timingFor(bitrate, &timing)) {
    log_e("SmartifyOS: %lu bit/s is not a bitrate the ESP32 can do",
          static_cast<unsigned long>(bitrate));
    return false;
  }
  twai_filter_config_t filter = TWAI_FILTER_CONFIG_ACCEPT_ALL();

  if (twai_driver_install(&general, &timing, &filter) != ESP_OK ||
      twai_start() != ESP_OK) {
    log_e("SmartifyOS: could not start CAN, check the pins");
    return false;
  }
  started_ = true;
  inbox_ = xQueueCreate(32, sizeof(twai_message_t));
  updater_.begin(this, node_);
  sendHeartbeat(protocol::stateStarting);

  // Everything that has to keep going whatever your code does runs here:
  // hearing requests, saying the device is there, getting back on the bus.
#if CONFIG_FREERTOS_UNICORE
  xTaskCreate(taskMain, "smartify_os", 4096, this, 5, nullptr);
#else
  xTaskCreatePinnedToCore(taskMain, "smartify_os", 4096, this, 5, nullptr, 0);
#endif

  if (safeMode_) {
    log_w("SmartifyOS: crashed %lu times in a row, in safe mode until it gets "
          "new firmware or is restarted",
          static_cast<unsigned long>(keptCrashes));
    // Never back to setup(), so none of the code that kept crashing runs.
    for (;;) vTaskDelay(pdMS_TO_TICKS(1000));
  }
  return true;
}

void Can::taskMain(void* can) { static_cast<Can*>(can)->run(); }

void Can::run() {
  uint32_t lastHeartbeat = millis();
  const uint32_t startedAt = millis();
  bool healthy = false;

  for (;;) {
    twai_message_t frame;
    if (twai_receive(&frame, pdMS_TO_TICKS(20)) == ESP_OK && !frame.rtr) {
      if (!frame.extd && frame.identifier == protocol::requestBase + node_) {
        updater_.handle(frame);
      } else if (!safeMode_) {
        // Full means loop() is not keeping up; the newest is dropped.
        xQueueSend(inbox_, &frame, 0);
      }
    }

    handleAlerts();
    const uint32_t now = millis();
    updater_.tick(now);

    if (now - lastHeartbeat >= heartbeatIntervalMs) {
      lastHeartbeat = now;
      sendHeartbeat(safeMode_               ? protocol::stateSafeMode
                    : updater_.isUpdating() ? protocol::stateUpdating
                                            : protocol::stateRunning);
    }
    if (!healthy && !safeMode_ && now - startedAt >= healthyAfterMs) {
      healthy = true;
      forgetCrashes();
    }
  }
}

void Can::update() {
  lastUpdateCall_ = millis();
  if (inbox_ == nullptr) return;
  twai_message_t frame;
  while (xQueueReceive(inbox_, &frame, 0) == pdTRUE) {
    if (anyFrame_) anyFrame_(frame);
    for (const Handler& handler : handlers_) {
      if (handler.id == frame.identifier &&
          handler.extended == (frame.extd != 0)) {
        handler.call(frame);
      }
    }
  }
}

bool Can::sendFrame(uint32_t id, const uint8_t* data, uint8_t length,
                    bool extended) {
  if (!started_ || busOff_ || length > 8) return false;
  twai_message_t frame = {};
  frame.identifier = id;
  frame.extd = extended ? 1 : 0;
  frame.data_length_code = length;
  memcpy(frame.data, data, length);
  // Not waiting: a full queue means nothing on the bus is answering, and
  // loop() must not stop for it.
  return twai_transmit(&frame, 0) == ESP_OK;
}

void Can::sendHeartbeat(uint8_t state) {
  const bool codeRunning =
      !safeMode_ && millis() - lastUpdateCall_ < stuckAfterMs;
  const uint8_t flags =
      (codeRunning ? protocol::flagCodeRunning : 0) |
      (updater_.isOnTrial() ? protocol::flagOnTrial : 0) |
      (updater_.hasRolledBack() ? protocol::flagRolledBack : 0);
  const uint8_t data[8] = {state,
                           SMARTIFY_OS_PROTOCOL_VERSION,
                           firmware_.major,
                           firmware_.minor,
                           firmware_.patch,
                           flags,
                           0,
                           0};
  sendFrame(protocol::heartbeatBase + node_, data, sizeof data);
}

void Can::handleAlerts() {
  uint32_t alerts = 0;
  if (twai_read_alerts(&alerts, 0) != ESP_OK) return;
  if (alerts & TWAI_ALERT_BUS_OFF) {
    // Too many errors in a row, usually a wire off or no termination. The
    // controller waits for the bus to be quiet for a moment and comes back.
    busOff_ = true;
    log_w("SmartifyOS: the CAN bus has a fault, recovering");
    twai_initiate_recovery();
  }
  if (alerts & TWAI_ALERT_BUS_RECOVERED) {
    busOff_ = false;
    twai_start();
    log_i("SmartifyOS: back on the CAN bus");
  }
}

}  // namespace smartify_os
