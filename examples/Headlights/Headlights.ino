// The ESP32 in the nose of a Mazda MX-5 NB that moves its pop-up headlights,
// the way the SmartifyOS Miata app (smartify_os_miata_app/lib/can.dart)
// defines it.
//
// It listens for two things: the wink button on the button ESP32, and the
// screen asking for the lights to go up or down. Whenever the lights move,
// and once a second besides, it says where they are, which is what moves
// the lights of the car on the screen.
//
// smartify_os_network.h next to this file is the one SmartifyOS wrote for the
// Miata. Your own car's comes from its app, see DEVICES.md in SmartifyOS.

#include <SmartifyOsCan.h>

#include "smartify_os_network.h"

// The transceiver's TX and RX pins.
constexpr gpio_num_t canTx = GPIO_NUM_5;
constexpr gpio_num_t canRx = GPIO_NUM_4;

// All the way up, in degrees.
constexpr uint8_t up = 45;

// Where each light is and where it is going.
uint8_t angle[2] = {0, 0};
uint8_t target[2] = {0, 0};

// Winking the left light: up, then back down once it got there.
bool winking = false;

uint32_t lastMove = 0;
uint32_t lastReport = 0;

void report() {
  network::HeadlightsState state;
  state.leftAngle = angle[0];
  state.rightAngle = angle[1];
  SmartifyOsCan.send(state);
  lastReport = millis();
}

void setup() {
  SmartifyOsCan.begin(network::nodes::headlights, canTx, canRx);

  SmartifyOsCan.on<network::HeadlightsRequest>(
      [](const network::HeadlightsRequest& request) {
        target[0] = target[1] = request.up ? up : 0;
        winking = false;
      });

  SmartifyOsCan.on<network::ButtonPressed>(
      [](const network::ButtonPressed& pressed) {
        if (pressed.button != network::MiataButton::wink) return;
        target[0] = up;
        winking = true;
      });
}

void loop() {
  SmartifyOsCan.update();

  // A real car drives the motors here. This one only pretends, a degree
  // every 20 ms.
  if (millis() - lastMove >= 20) {
    lastMove = millis();
    bool moved = false;
    for (int i = 0; i < 2; i++) {
      if (angle[i] == target[i]) continue;
      angle[i] += angle[i] < target[i] ? 1 : -1;
      moved = true;
    }
    if (winking && angle[0] == up) {
      target[0] = 0;
      winking = false;
    }
    if (moved) report();
  }

  if (millis() - lastReport >= 1000) report();
}
