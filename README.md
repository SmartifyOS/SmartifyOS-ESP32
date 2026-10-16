# SmartifyOS-ESP32

Put an ESP32 on your car's CAN bus, and it becomes part of
[SmartifyOS](https://github.com/Mauznemo/smartify_os_flutter_test):

- It shows up on the car's screen (Settings, Vehicle, Devices in the car), with its
  firmware version, and SmartifyOS notices when it goes quiet or its program gets stuck.
- It sends and receives the messages defined in the car's app, by name, as C++ structs.
- It can be given new firmware over the bus, from a USB stick plugged into the car, so a
  device hidden behind the dashboard never needs a cable again. A firmware that does not
  start, or cannot be reached, is undone by itself.

You write normal Arduino code. The library does the CAN part. You do not need to know
PlatformIO: the `smartify-os` CLI creates the project, builds it for your board and puts
it on the ESP32. **The guide for all of that is
[DEVICES.md](https://github.com/Mauznemo/smartify_os_flutter_test/blob/main/DEVICES.md)
in SmartifyOS.** This page is about the library itself.

## The code

```cpp
#include <SmartifyOsCan.h>
#include <smartify_os_network.h>  // written by SmartifyOS for your car

void setup() {
  // First thing in setup(): this device, and the transceiver's TX and RX pins.
  SmartifyOsCan.begin(network::nodes::headlights, GPIO_NUM_5, GPIO_NUM_4);

  // Something you want to hear about.
  SmartifyOsCan.on<network::HeadlightsRequest>(
      [](const network::HeadlightsRequest& request) {
        moveLights(request.up);
      });
}

void loop() {
  SmartifyOsCan.update();  // every loop, and keep the loop quick

  // Something you want to say.
  network::HeadlightsState state;
  state.leftAngle = 45;
  state.rightAngle = 45;
  SmartifyOsCan.send(state);
}
```

| | |
| --- | --- |
| `begin(node, tx, rx)` | Joins the bus. First thing in `setup()`. Optionally the firmware version and the bitrate, both of which the CLI normally sets. |
| `update()` | Hands what came in to your handlers. Call it every `loop()`. |
| `send(message)` | Sends one of the structs from `smartify_os_network.h`. |
| `on<Message>(handler)` | Calls `handler` from `update()` every time `Message` comes in. |
| `sendFrame(id, data, length)` / `onFrame(handler)` | Raw frames, for anything without a definition. |
| `isBusOk()` | False while the bus has a fault, until it recovers by itself. |
| `isUpdating()` | True while new firmware comes in. The device restarts into it at the end, so finish anything that must not stop halfway. |

Your handlers run inside `update()`, on the same task as `loop()`, so nothing in your code
runs at the same time as anything else in your code.

## What runs on its own

`begin()` starts a task of the library's own, next to `loop()`. It hears the bus, says
the device is there once a second, gets back on the bus after a fault, and receives new
firmware. None of that waits for `loop()`, so a device whose own code hangs can still be
updated, and shows as stuck rather than as gone.

- **Updates** go into the ESP32's second app slot, and the device restarts into them. The
  new firmware runs on trial until the car confirms it, which proves it starts and can be
  reached over the bus. Not confirmed within a minute, or crashing before that, and the
  device goes back to its old firmware.
- **Safe mode**: a device that crashes three times in a row (a panic or a watchdog, not a
  loss of power) stays in `begin()` and runs nothing but the bus and the updater, until it
  is given new firmware or restarted from the car. That is why `begin()` comes first.

Both need the default partition table, with two app slots. A "huge app" table has only
one, and then the device can only be flashed over USB.

## Wiring, in short

ESP32 TX and RX pins to an SN65HVD230 transceiver's TX (CTX/TXD) and RX (CRX/RXD), the
transceiver's CANH and CANL on the bus, and a common ground. The bus is one line from
device to device, with a 120 ohm resistor at each of its two ends and nowhere else. More,
including which transceivers to buy, in DEVICES.md.

## Something other than an ESP32

Anything can join, as long as it speaks the same protocol: [PROTOCOL.md](PROTOCOL.md).
The header SmartifyOS writes for a car is plain C++ with nothing ESP32 in it.

## Versions

A release of this library speaks one version of the protocol, and SmartifyOS names the
release it works with (`smartify_os: firmware:` in SmartifyOS's `pubspec.yaml`), which is
the one the CLI builds with. The header SmartifyOS writes refuses to compile against a
library that speaks another protocol.

Built and tried with PlatformIO's `espressif32` 7.1.3 (Arduino for ESP32 2.0.17), on the
ESP32, ESP32-S3 and ESP32-C3.
