# Changelog

A summary of the QuinLED Ultra protocol's releases. While below 1.0 they are numbered 0.x.y; the MINOR number equals
the wire format's VERSION (`ULTRA_VERSION`, header byte 2). This repository holds the current release only; the
specification, the reference header and the sender library describe it as it is now.

## 0.3.2-draft (wire VERSION 3, update ABI 3), 2026-10-31

- CONFIG 25 (`ULTRA_CFG_SKIP_RATE`): the slowest repeat of the pulse-skipping patterns (levels below the minimum
  pulse), 2000 Hz (the default) or 1000 Hz; other values are ignored. Only a board whose PWM banks have the flag
  `ULTRA_PWM_SKIP_RATE` (0x10) takes it, and the sender library sends it only to such a board.
- Sender library: every update decision frame starts at least 10 ms after the frame before it
  (`ULTRA_UPDATE_CONTROL_PACE_US`): the last PING of each training run, SELECT / RESELECT, ANNOUNCE and the first two
  blind END copies. A receiving app handles one frame at a time and dropped an ANNOUNCE that followed a PING by one
  port gap, so the update never started. DATA frames keep their pace.
- CALIBRATION page: end codes 4 (complete, not taken: no output drew 20 mA) and 5 (complete, not taken: no step moved
  off the zero).
- Sender library: a blind update's END goes out three times (`ULTRA_UPDATE_END_COPIES`), each followed by the port
  gap; the waits and the 2,100 ms quiet count from the last copy.
- Sender library: the idle application confirmation is six session-zero PINGs (`ULTRA_UPDATE_CONFIRM_COPIES`), each
  with its confirmation-write pause: an app captures whole UPD frames from the fourth in a row.
- No frame format change: the wire VERSION stays 3.

## 0.3.1-draft (wire VERSION 3, update ABI 3), 2026-10-27

- `ULTRA_RW_REARM_US` (200 µs): every board (apps, and bootloaders in recovery) re-arms its ordinary slot plan only
  after this much LOW with no edge after a frame: past the host's release, before the late-release reject and the
  reset. A re-arm in a released HIGH would hide it from the boards behind.
- PIXEL page part 0: BUSY (byte 21) counts the relay windows given up at the turnaround because the line above was
  still driven; LONG now means boards behind this one, not an error.
- A pixel receiver may sit at any position of a CHAIN line (a bound descriptor still decides from its LINK). The RW
  plan relays through every bound board in front of the last one asked.
- Compatible additions only: the wire VERSION stays 3.

## 0.3.0-draft (wire VERSION 3, update ABI 3), 2026-10-23

- Wire 3: a full 8-bit TARGET, SET flags in TYPE, five colour components, channels and telemetry in banks of five,
  board descriptors (TLV), scoped CONFIG, records with a commit-last CRC, power JSON v3 and the setup S2 schema.
- Per-port line profiles: Diff-Power ports run at 800 kHz with 600 / 400 / 200 kHz fallback steps; a link model and a
  speed table by board count for Dig2Analog-Plus-5ch data pins; GUARANTEE and EXTRA timing bounds.
- Board limits per port type: at most 8 receivers on a Diff-Power port, up to 50 boards on a Dig2Analog-Plus-5ch pin.
- Firmware updates over the data line (`ultra_update.h`): "update all receivers on this port" as blind group
  broadcasts per chip family and exact target, at least three complete DATA passes, an installed-version gate, and a
  per-board recover action; a chain-end board may still pass update sessions on (LINK UPDATE_FORWARD).
- Pixel effect catalogue version 4: every effect takes a palette. PALETTE 0xFF is Effect default (the effect's
  default palette or its own colours); per effect the catalogue lists the colour slots it still uses while a palette
  is chosen and what the palette colours.
- Telemetry: the supply / cable impedance is on the ENERGY page (bytes 19-20, mOhm; it was on DIAG). The POWER,
  SUPPLY and DIAG pages each come with their IDENTITY feature bit, so a board may leave them out (the V203
  Digital-Medium does); STATUS byte 8 bits 0-3 belong to the POWER events feature.
- Header TIME counts the host us modulo 2^22 x 1000 (it wraps on a whole ms, every 69.9 min; a higher TIME is an
  invalid header); START counts from the header's host ms, so one shared start lands on one tick on every board.
- LIFE_SYNC energy step: ceil(rating mA x maximum voltage 10 mV x (powered 0.1 h + 10) / 10^6) Wh from the board
  descriptor's domain 0; an unknown input merges no energy.
- Transport-independent command bytes with UART / SPI / I2C envelopes (`ultra_transport.h`).
- Sender library: allocation-free C99 for wire 3 with descriptor, profile, update and port-job modules; golden RMT,
  PARLIO and TIME traces; shared test vectors checked from Python, C and C++.

## 0.2.9, 2026-10-17

- The Diff-Power Ultra Receiver Digital-Medium can run as the last board of a normal Ultra line (CONFIG 140 line
  mode: auto, stream or chain; page 0x81 part 3; capability bit `ULTRA_HWCAP_CHAIN`).
- CONFIG 141, pixel smoothing (16-bit gamma and brightness with dithering).
- A lower bound for the host's release after an RW frame (`ULTRA_RW_RELEASE_MIN_US`).
- Pixel effect catalogue version 3: 36 effects, 31 palettes.

## 0.2.8, 2026-10-17

- Local pixel effects on the Digital-Medium receiver: slot type PIXEL_FX, CONFIG 129-139, page 0x81, the effect
  catalogue with its checker, the RESTART flag.
- IDENTITY in two parts (board, then 192 feature bits); STATUS byte 3 is the one-byte hardware id.
- Digital ports: one receiver per pixel port takes Ultra frames behind a marker in a stretched latch gap; the sender
  library's digital port planner (`ultra_dport.h`).

## 0.2.6, 2026-10-15

- The planned power-off notice (IDENTIFY 7) for Diff-Power standby, and LIFE_SYNC (page 0x0E): the hub restores a
  receiver's lifetime counters.

## 0.2.5, 2026-10-14

- CONFIG 24 (the retry policy after a trip), signal loss "scene", the LIFETIME and DIAG pages, why the light is not
  as set (STATUS), more ENERGY and HEALTH fields.
- The reference sender library (C99 with a C++ wrapper, ESP32 RMT examples).

## 0.2.4, 2026-10-14

- The return channel is defined: POLL, rates, slot pitch, burst timing, the turnaround check, and the STATUS,
  OUTPUTS, ACKS, ERRORS and IDENTITY pages. A controller can be written from the specification alone.

## 0.2.3, 2026-10-13

- Power-on effects (SCENE_EFFECT), soft start (CONFIG 23), and the monitoring pages.

## 0.2.2, 2026-10-13

- The board current limit and overcurrent events on the return channel.

## 0.2.1, 2026-10-13

- IDENTIFY 5 blinks the firmware version; 0.x version numbers.

## 0.2.0, 2026-10-12

- The first public version: the frame (slots, then the header as a trailer), TIME, the start-up preamble, the slot
  types, CONFIG, the reference header and the test vectors.
