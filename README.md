# QuinLED Ultra protocol 0.3.2-draft

**Protocol 0.3 draft and C99 sender reference, wire VERSION 3.** This repository defines
shared byte layouts, host behavior and exact physical timing. It contains no
wire-3 receiver, bootloader or hub implementation and no hardware timing
certification. No release or 1.0 compatibility claim is made.

| Contract | Source |
|---|---|
| Normal wire constants and C/C++ codecs | [include/ultra_protocol.h](include/ultra_protocol.h) |
| Target/image/update contract | [include/ultra_update.h](include/ultra_update.h), [update.md](docs/update.md) |
| TLV descriptor, configuration, power and examples | [descriptor.md](docs/descriptor.md) |
| Power JSON v3 and setup S2 schemas/paging | [power-json.md](docs/power-json.md), [setup-s2.md](docs/setup-s2.md) |
| Physical timing and qualification | [timing.md](docs/timing.md) |
| Adapter formats and firmware-owned board definitions | [transport.md](docs/transport.md), [board-ids.md](board-ids.md) |
| Encode/decode vectors and runner | [test-vectors](test-vectors/README.md) |
| Receiver flash/RAM estimates | [costs.md](docs/costs.md) |
| Training, status reboot and stored output levels | [commands.md](docs/commands.md) |

A slot is `TYPE+flags, TARGET, SEQ, payload[27], CRC16`. TARGET is a full byte;
SET flags occupy TYPE bits 5/6. Lights retain five colour components while
physical channels and telemetry grow in banks of five. CONFIG is scoped by
board/channel/output, descriptors are const TLVs, and pixel outputs have explicit
local/stream sources. Ratings and user limits apply in their own voltage domains
through the calibrated model; reported current is measured at the input.

## Effects on analog receivers

The host (ESPHome) runs analog effects by sending SET targets with a transition
time, a few times per second per light. The receiver interpolates each target
with its 16-bit fades. Breathe, colour loop, pulse, sunrise, sunset and effect
ramps stay smooth at this cadence; strobe, flicker, random and candle follow the
bus cadence. Choose SET transition times to bridge successive targets.
`ultra_sender_stream()` sends replaceable targets with a glide; ordinary
`ultra_sender_set()` supplies repeated, scheduled SET commands.

Analog command opcodes `0x03`, `0x04` and `0x0A` are reserved, never reused.
Receivers ignore them as unknown without changing outputs or stored levels.
Their analog effect ID namespace (0..255), OUTPUTS byte 20, DIAG flash-pending
bit `0x10` and IDENTITY feature-word-0 bit `0x00010000` are also reserved.
Reserved command ACKS columns return SEQ zero and NEVER set. The default third
ACKS column is reserved. [Diff-Power Ultra Receiver Digital-Medium](https://github.com/intermittech/quinled-ultra-firmware/wiki/Board-Diff-Power-UR-Digital-M) PIXEL_FX commands and the pixel effect
catalogue describe receiver-local pixel effects.

Version axes: release **0.3.2-draft**, normal wire **3**, independent update trailer
**3** (frozen with ABI **3**), image descriptor **2**, board descriptor **1**, record
format **2**, power JSON **3**, setup **S2**. Prototypes require jig/SWD provisioning;
there is no dual normal-wire stack. Future record migration belongs to the app;
immutable bootloaders do not reject newer app record/wire/descriptor formats.
V203 and future 200 MHz V407 boards share the host-time contract where topology
and qualification permit.

Use the `include/` headers unchanged and pin a release commit. The allocation-free
C99 `sender/` implements wire 3 only; its three normative header copies and
`PROTOCOL-PIN.json` are checked together. Python, C and C++ consume shared binary
fixtures and exact RMT/PARLIO/TIME traces. Repository docs/headers define the
contract. The pixel effect catalogue remains shared; LED chip 7 is reserved.
See [sender usage](sender/README.md) and [command contracts](docs/commands.md).

From the repository root:

```text
python -m pip install jsonschema
python sender/test/run_tests.py --firmware ../quinled-ultra-firmware
```

The runner checks every fixture in Python and the public codecs in C and C++,
and runs the sender against the receiver firmware's own receive path and delay
line (required: a firmware checkout of the same protocol commit).
Product facts live in each board firmware's `src/board/<name>/` and compile into
its descriptor; the controller discovers capabilities and can only lower limits.
This repository defines formats and a product-name-only ID allocation list.
Synthetic fixtures include 2/7/10/20 channels and two pixel outputs without
predefining future products. Assembly identity is an opaque firmware-local byte.

Diff-Power uses **800 kHz normally**, with **600/400/200 kHz per-port fallback**
only on fully inventoried all-Ultra ports. Typical 30+3+3 m chains close the
conditional 800 kHz budget with >29 ns decoder margin; eight worst-case
100 m segments use 200. Strip outputs always regenerate chip timing.
Mixed paths use the GUARANTEE timing bound; qualified paths have an EXTRA
column with coverage 3/4/7/8. GUARANTEE source bounds apply at 5 V only; EXTRA
supply coverage needs written confirmation. Cable/protection and assemblies
still require qualification. See [timing.md](docs/timing.md)
and [sender integration](docs/senders.md).
Local UART/SPI/I2C carry the same command bytes without these pulse assumptions.

Code is [MIT](LICENSE); specification text is [CC BY 4.0](LICENSE-CC-BY-4.0.txt).
Copyright (C) 2026 QuinLED. See [TRADEMARK.md](TRADEMARK.md) for naming rules.
