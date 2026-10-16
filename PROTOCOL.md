# The SmartifyOS CAN protocol

How the car's computer and its devices talk over CAN. The ESP32 library speaks it for
you; this is for writing firmware for something else, and for knowing what is on the
bus when you look at it with a CAN tool.

Protocol version **1**.

## The bus

- Classic CAN (CAN 2.0), not CAN FD: an ESP32 cannot send CAN FD.
- **500 kbit/s**, unless the car's app says otherwise (`CanConfig.bitrate`).
- Standard 11 bit identifiers. Extended ones are only there for reading a car's own bus.
- A 120 ohm resistor at each of the two ends.

## Identifiers

An identifier says what a frame is about, not who it is for: every device hears every
frame and keeps what it needs. A lower identifier wins when two devices send at the same
moment.

| Identifiers | What |
| --- | --- |
| `0x000` | Kept free, the most important identifier on any bus. |
| `0x001` to `0x57F` | The car's own messages, defined in its app. |
| `0x580` + node | A device answering the car's computer. |
| `0x600` + node | The car's computer asking a device something. |
| `0x700` + node | A device's heartbeat. |

The last three are where CANopen has the same things, so CAN tools already show them
sensibly. Node numbers go from 1 to 127.

**Only one device sends each message.** Two devices sending the same identifier at the
same moment garble each other, which CAN reports as errors. When two devices can ask
for the same thing (the buttons and the screen both raising the headlights), each has a
message of its own.

## Messages

A message is a list of fields, packed one after the other with no gaps, **lowest bit
first**: the first field starts at bit 0 of byte 0, and each field starts at the bit after
the one before it ended. A field longer than the bits left in a byte carries on into the
next byte, its lower bits first. This is what a DBC file calls Intel byte order, so a DBC
for the bus has each field's start bit at the sum of the bits before it.

| Kind | Bits | On the bus |
| --- | --- | --- |
| Bool | 1 | 1 is true |
| Whole number | 1 to 32 | As it is; signed ones in two's complement |
| Number | 1 to 32 | `round((value - offset) / scale)`, signed or not |
| Float | 32 | IEEE 754 single precision |
| Choice | as few as the choices need | The choice's place in the list, from 0 |

A frame is as long as its fields need, rounded up to whole bytes. A frame shorter than
its message reads its missing fields as 0, which is how firmware that does not know about
a field added later still works, and how a newer device still reads an older one. That is
also why new fields go at the end, and an identifier is never reused for something else.

## Heartbeat

Every device sends this on `0x700` + its node, right after it starts and then once a
second, from a task of its own, so it keeps going even while the device's own code is
stuck. The car's computer counts a device as gone after 3.5 seconds without one.

| Byte | What |
| --- | --- |
| 0 | State, see below |
| 1 | The protocol version it speaks, `1` |
| 2, 3, 4 | Its firmware version: major, minor, patch |
| 5 | Flags, see below |
| 6, 7 | Not used yet, `0` |

State, which means what it means in a CANopen heartbeat:

| Value | State |
| --- | --- |
| `0x00` | Just started (only the very first heartbeat) |
| `0x05` | Running its own code |
| `0x04` | Safe mode: it crashed again and again, so only the bus and the updater run |
| `0x7F` | Receiving new firmware |

Flags:

| Bit | Set when |
| --- | --- |
| 0 | Its own code is running: `loop()` called `update()` within the last 3 seconds |
| 1 | It runs new firmware that has not been confirmed yet, see "Firmware updates" |
| 2 | It went back to its old firmware, because the last update did not come up |

Only byte 0 is required; a device that sends just that shows up without a firmware
version, as running.

## Requests and answers

The car's computer asks one device something on `0x600` + node, and the device answers
on `0x580` + node. Byte 0 of a request is what it asks for; byte 0 of the answer is the
same with the top bit set (`0x01` is answered with `0x81`), and byte 1 is how it went:

| Status | Meaning |
| --- | --- |
| `0x00` | Done |
| `0x01` | The firmware is bigger than the space for it |
| `0x02` | This device has no space for an update (its partition table has no second app slot) |
| `0x03` | An update is already running, or none is |
| `0x04` | Writing to flash failed |
| `0x05` | The new firmware is not a valid firmware for this chip |
| `0x06` | The checksum does not match what arrived |
| `0x07` | Not the part that comes next, byte 2 onwards says which does |
| `0x08` | Parts of the last block did not arrive, send it again |

A device ignores a request it does not know, and frames on `0x600` + node that are not
meant for anything running.

| Request | Bytes | What |
| --- | --- | --- |
| `0x01` Begin | `01`, size (4) | Get ready for a firmware of this many bytes: makes room for it, which can take several seconds. |
| `0x02` Block | `02`, offset (4), length (2) | The next part of the firmware follows, as data frames. |
| data | `80`+seq, up to 7 bytes | Part of a block. `seq` counts from 0 within the block. |
| `0x03` End | `03`, CRC-32 (4) | That was all of it. Checked, then the device restarts into it. |
| `0x04` Confirm | `04` | Keep the new firmware. |
| `0x05` Abort | `05` | Throw away an update that has not ended yet. |
| `0x06` Restart | `06` | Restart, which also leaves safe mode. |

Numbers are little endian. The CRC-32 is the usual one (the one zip uses: polynomial
`0xEDB88320`, starting at `0xFFFFFFFF`, inverted at the end), over the whole firmware.

The answer to a block carries, in bytes 2 to 5, the offset the device wants next: the
end of the block when it went well, or the start of what to send again.

## Firmware updates

A device that has been flashed once over USB with firmware using this library can be
given new firmware over the bus from then on. The updater runs inside the normal
firmware (no bootloader of its own), on the library's own task, so a mistake in the
device's own code cannot keep it from being updated.

1. **Begin.** The car sends the size. The device makes room for it in its other app
   slot and answers when it is done. The car waits up to 30 seconds for that.
2. **Blocks.** The firmware goes over in blocks of at most 896 bytes (128 data frames of
   7). The device keeps a block in memory and only writes it to flash once all of it is
   there, since an ESP32 cannot receive while it writes to flash, then answers. The car
   sends the next block only after that answer, and sends a block again when no answer
   comes within a second and a half, or the device says parts of it went missing. A block
   the device already has is answered at once without writing it twice, so a lost answer
   costs nothing.
3. **End.** The car sends the CRC-32 of everything. The device checks it, has ESP-IDF
   check that it is a complete firmware for this chip, answers, and restarts into it.
4. **Confirm.** The new firmware starts on trial: its heartbeats have flag bit 1 set.
   The car confirms it once it hears one, which proves the new firmware runs and can be
   reached over the bus. A firmware that crashes before that, hangs, or is never
   confirmed within 60 seconds is undone: the device restarts into the old one, which
   says so with flag bit 2.

Throughput is about 10 to 25 KB a second at 500 kbit/s, so a large firmware takes about a
minute.

### Safe mode

A device that crashes three times in a row (a panic or a watchdog, not a loss of power)
starts in safe mode: heartbeat state `0x04`, nothing but the bus and the updater running,
until it is given new firmware or told to restart. This is for a firmware that was
confirmed and only crashes later, which the trial above cannot catch. The library starts
it from `SmartifyOsCan.begin`, so `begin` has to be the first thing in `setup()`.

### On the ESP32

- The partition table needs two app slots and `otadata`. Arduino's default for 4 MB of
  flash has them; a "huge app" one does not.
- Arduino marks a new firmware as good the moment it starts, which would leave nothing
  to undo. The library tells it not to (`verifyRollbackLater`), and marks it good itself
  on Confirm.
- The bootloader has to support going back to the old firmware. Arduino's has since
  version 2.0.4.
