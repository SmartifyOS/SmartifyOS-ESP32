// SmartifyOS for an ESP32 on the car's CAN bus.
//
// Wire the ESP32 to a CAN transceiver, connect the transceiver to the bus,
// and:
//
//   #include <SmartifyOsCan.h>
//   #include <smartify_os_network.h>   // written by SmartifyOS for your car
//
//   void setup() {
//     // First thing in setup(), see "Safe mode" in PROTOCOL.md.
//     SmartifyOsCan.begin(network::nodes::headlights, GPIO_NUM_5, GPIO_NUM_4);
//
//     SmartifyOsCan.on<network::HeadlightsRequest>(
//         [](const network::HeadlightsRequest& request) {
//           moveLights(request.up);
//         });
//   }
//
//   void loop() {
//     SmartifyOsCan.update();
//   }
//
// From then on this ESP32 shows up in SmartifyOS (Settings, Vehicle, Devices
// in the car) with its firmware version, SmartifyOS notices when it goes
// quiet, and it can be given new firmware over the bus.
#pragma once

#include <Arduino.h>
#include <driver/twai.h>

#include <functional>
#include <vector>

#include "SmartifyOsProtocol.h"
#include "SmartifyOsUpdater.h"

// The protocol this library speaks, checked against the one the header
// SmartifyOS wrote for your car was written for, whichever is included first.
#define SMARTIFY_OS_PROTOCOL_VERSION 1
#if defined(SMARTIFY_OS_NETWORK_PROTOCOL) && \
    SMARTIFY_OS_NETWORK_PROTOCOL != SMARTIFY_OS_PROTOCOL_VERSION
#error "smartify_os_network.h was written for another version of this library"
#endif

// Your firmware's version, which the smartify-os CLI sets when it builds it.
// Pass one to SmartifyOsCan.begin to set it yourself instead.
#ifndef SMARTIFY_OS_FIRMWARE_VERSION
#define SMARTIFY_OS_FIRMWARE_VERSION 0, 0, 0
#endif

namespace smartify_os {

/// The version of your firmware, which SmartifyOS shows next to the device.
struct FirmwareVersion {
  uint8_t major;
  uint8_t minor;
  uint8_t patch;
};

/// The CAN bus, as one device on it sees it. Use the `SmartifyOsCan` there
/// already is rather than making your own.
class Can {
 public:
  /// How often this device says it is there.
  static constexpr uint32_t heartbeatIntervalMs = 1000;

  /// Joins the bus as device [node] (from `network::nodes`), with the
  /// transceiver on [tx] and [rx]. Every device on the bus has to use the
  /// same [bitrate], 500 kbit/s unless the car's app says otherwise.
  ///
  /// Call it first thing in `setup()`. A device that crashed three times in
  /// a row never comes back from it: it stays in safe mode, where only the
  /// bus and the updater run, until it gets new firmware or is restarted
  /// from SmartifyOS.
  ///
  /// Returns false when the CAN driver could not be started, which is a
  /// problem with the pins.
  bool begin(uint8_t node, gpio_num_t tx, gpio_num_t rx,
             FirmwareVersion firmware = {SMARTIFY_OS_FIRMWARE_VERSION},
             uint32_t bitrate = 500000);

  /// Hands what came in to your handlers. Call it in every `loop()`, and keep
  /// `loop()` quick: a device whose loop does not get back here for three
  /// seconds shows as stuck in SmartifyOS.
  void update();

  /// Sends [message], one of the structs in smartify_os_network.h. Returns
  /// false when it could not be queued, for example while the bus has a
  /// fault.
  template <typename Message>
  bool send(const Message& message) {
    uint8_t data[8] = {0};
    message.encode(data);
    return sendFrame(Message::id, data, Message::length, Message::extended);
  }

  /// Calls [handler] every time [Message] comes in, from inside [update].
  template <typename Message>
  void on(std::function<void(const Message&)> handler) {
    handlers_.push_back(
        {Message::id, Message::extended,
         [handler](const twai_message_t& frame) {
           handler(Message::decode(frame.data, frame.data_length_code));
         }});
  }

  /// Sends a frame as it is, for anything without a definition.
  bool sendFrame(uint32_t id, const uint8_t* data, uint8_t length,
                 bool extended = false);

  /// Calls [handler] for every frame that comes in, whatever it is.
  void onFrame(std::function<void(const twai_message_t&)> handler) {
    anyFrame_ = handler;
  }

  /// Whether the bus is working: false while it has a fault (a wire off),
  /// until it recovers by itself.
  bool isBusOk() const { return started_ && !busOff_; }

  /// Whether new firmware is coming in right now. The device restarts into
  /// it once it is all there, so stop anything that must not stop halfway.
  bool isUpdating() const { return updater_.isUpdating(); }

  /// This device's node number.
  uint8_t node() const { return node_; }

  /// Internal: forgets the crashes counted towards safe mode, before a
  /// restart that is meant.
  static void forgetCrashes();

 private:
  struct Handler {
    uint32_t id;
    bool extended;
    std::function<void(const twai_message_t&)> call;
  };

  static void taskMain(void* can);
  void run();
  void sendHeartbeat(uint8_t state);
  void handleAlerts();

  uint8_t node_ = 0;
  FirmwareVersion firmware_ = {0, 0, 0};
  bool started_ = false;
  bool busOff_ = false;
  bool safeMode_ = false;
  volatile uint32_t lastUpdateCall_ = 0;
  QueueHandle_t inbox_ = nullptr;
  Updater updater_;
  std::vector<Handler> handlers_;
  std::function<void(const twai_message_t&)> anyFrame_;
};

}  // namespace smartify_os

/// The CAN bus. Start it with `SmartifyOsCan.begin(...)` in `setup()`.
extern smartify_os::Can SmartifyOsCan;
