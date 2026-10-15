# quinled-ultra-protocol

The **QuinLED Ultra protocol**: how a controller talks to LED boards such as the **QuinLED Dig2Analog-Ultra-5ch** in
Ultra mode. The controller can be ESPHome, WLED, MoonLight or your own firmware.

The controller sends **what** should happen: a colour with a transition length, an effect, a setting. The board
computes **how**, every millisecond, in 16 bit, in step with every other board on the line. The line is the same
single data wire as WS28xx LEDs (800 kbit/s), and the boards are daisy-chained.

## What's here

| | |
|---|---|
| [`include/ultra_protocol.h`](include/ultra_protocol.h) | The reference header: constants, CRCs and slot / frame builders. Plain C99, no dependencies, also usable from C++. |
| [Wiki](https://github.com/intermittech/quinled-ultra-protocol/wiki) | The specification: physical layer, frame layout, every slot type, timing, host behaviour, compatibility rules. |
| [`test-vectors/`](test-vectors) | Example slots and frames as hex, generated from the header. Check your implementation byte for byte. |
| [`examples/`](examples) | A platform-neutral C example that builds frames, plus notes on driving them out (RMT, SPI, I2S, PARLIO). |
| [`sender/`](sender) | The reference sender library (C99 + a C++ wrapper): the scheduler, copies, timing and the bit encoding, with ESP32 RMT examples and PlatformIO / Arduino / ESP-IDF packaging. The easiest way to add Ultra to WLED, Tasmota, MoonLight or your own firmware. |

## Who it's for

- **Controller software** that wants to drive QuinLED Ultra boards: the
  [host implementer's guide](https://github.com/intermittech/quinled-ultra-protocol/wiki/Host-Implementers-Guide)
  lists the minimum (light definition, state, refresh) and the optional parts (effects, scenes, settings). The
  [sender library](https://github.com/intermittech/quinled-ultra-protocol/wiki/Sender-Library) implements it.
- **Board firmware** that wants to understand the same frames.

## Status

- **Protocol 0.2** (wire VERSION 2). Nothing is released as 1.0 yet.
- The QuinLED Dig2Analog-Ultra-5ch is in development; its firmware implements this version.
- The reference controller, and the most complete implementation, is ESPHome with the `dig2analog_ultra`
  component from [intermittech/quinled-ultra-esphome](https://github.com/intermittech/quinled-ultra-esphome).
- **The return channel** (board → controller, 0.2.4; the LIFETIME and DIAG pages 0.2.5) is QuinLED Diff-Power only:
  the QuinLED Diff-Power Ultra-Analog receiver's firmware build implements it (firmware 0.6.8, `BOARD=2`; 0.2.5's pages: 0.6.9). The Dig2Analog-Ultra never answers. A controller that
  never sets RW needs none of it. Spec:
  [Return channel](https://github.com/intermittech/quinled-ultra-protocol/wiki/Return-Channel).
- Firmware-update frames are QuinLED's own (the bootloader's format) and not needed by a controller.

## Using the header

- Copy `include/ultra_protocol.h` **unchanged** from a tagged release into your project.
- Every copy (the board firmware, the ESPHome component, your project) must be byte-identical to the header of one
  tag, so a SHA-256 or a plain diff shows which version you have.
- Tags: `v0.2.y` = wire VERSION 2 (`ULTRA_VERSION`). While below 1.0 the MINOR number is the wire VERSION; a
  compatible addition raises the PATCH number. See
  [Compatibility and versioning](https://github.com/intermittech/quinled-ultra-protocol/wiki/Compatibility-and-Versioning).

## License

- **Code** (`include/`, `sender/`, `test-vectors/gen_vectors.c`, `examples/`): [MIT](LICENSE). It can be included in projects
  under any license (GPL-3.0, EUPL-1.2, proprietary, ...).
- **Specification text** (the wiki and the documents in this repository):
  [CC BY 4.0](LICENSE-CC-BY-4.0.txt). Share and adapt it, with attribution to QuinLED.
- Copyright (C) 2026 QuinLED.
- **The name:** an implementation may say it is "compatible with the QuinLED Ultra protocol". It may not use
  "QuinLED" as (part of) its product name. See [TRADEMARK.md](TRADEMARK.md).
