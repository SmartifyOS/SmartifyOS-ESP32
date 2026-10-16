// The ESP32 behind the dashboard of a Mazda MX-5 NB that reads its buttons,
// the way the SmartifyOS Miata app (smartify_os_miata_app/lib/can.dart)
// defines it.
//
// Each button press goes out as one ButtonPressed message, for everyone on
// the bus: the headlights wink by themselves on the wink button, and the
// screen skips tracks on the other two.
//
// smartify_os_network.h next to this file is the one SmartifyOS wrote for the
// Miata. Your own car's comes from its app, see DEVICES.md in SmartifyOS.

#include <SmartifyOsCan.h>

#include "smartify_os_network.h"

constexpr gpio_num_t canTx = GPIO_NUM_5;
constexpr gpio_num_t canRx = GPIO_NUM_4;

// Each button, wired between its pin and ground.
struct Button {
  uint8_t pin;
  network::MiataButton button;
  bool wasDown;
  uint32_t changedAt;
};

Button buttons[] = {
    {12, network::MiataButton::wink, false, 0},
    {13, network::MiataButton::previous, false, 0},
    {14, network::MiataButton::next, false, 0},
};

// How long a button has to stay where it is before it counts, so one press
// does not bounce into several.
constexpr uint32_t debounceMs = 30;

void setup() {
  for (Button& b : buttons) pinMode(b.pin, INPUT_PULLUP);
  SmartifyOsCan.begin(network::nodes::buttons, canTx, canRx);
}

void loop() {
  SmartifyOsCan.update();

  for (Button& b : buttons) {
    const bool down = digitalRead(b.pin) == LOW;
    if (down == b.wasDown || millis() - b.changedAt < debounceMs) continue;
    b.wasDown = down;
    b.changedAt = millis();
    if (!down) continue;

    network::ButtonPressed pressed;
    pressed.button = b.button;
    SmartifyOsCan.send(pressed);
  }
}
