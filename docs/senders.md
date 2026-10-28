# Wire-3 sender integration: profiles belong to ports

The allocation-free C99 reference in `sender/src/` implements this contract;
see `sender/README.md` for storage, ownership and integration. Profile IDs 0..3 mean
800/600/400/200 kHz. Keep separate profile, inventory, override, timing,
statistics epochs, queues and hysteresis per port. A slow port must not change
another port's timing. Use `ultra_line_profile`, `ultra_line_rmt`,
`ultra_line_allowed`, `ultra_line_path_ns`, `ultra_d2a_path_ns`, `ultra_d2a_admits`,
`ultra_line_margin_ns` and `ultra_line_candidate` in `include/ultra_transport.h`.
The policy helper returns a candidate, not authorization: apply every timing.md
check before switching.

## Port types and the starting profile

A port is a Diff-Power port (`ULTRA_PORT_DIFF_POWER`, RS-485, at most eight
receivers) or a Dig2Analog-Plus-5ch data pin (`ULTRA_PORT_D2A_PIN`, at most 50
boards). The hub calls `ultra_port_line_profile(port_type, boards, &profile)`
with the configured board count; it returns the reason:

| Reason | Profile |
|---|---|
| `ULTRA_PORT_PROFILE_DEFAULT` | 0 = 800 kHz: every Diff-Power port, a D2A pin of 1..20 boards |
| `ULTRA_PORT_PROFILE_BOARD_COUNT` | D2A pin table: 21..34 boards 1 = 600 kHz, 35..50 boards 3 = 200 kHz |
| `ULTRA_PORT_PROFILE_TOO_MANY` / `NO_BOARDS` / `UNKNOWN_TYPE` | 255: refused |

A Diff-Power port starts at 800 and steps down only from measured margins
(AUTO). A D2A pin has no return path: it runs the
[pin table](timing.md#dig2analog-plus-5ch-pin-link-model) profile, never steps on
its own, and a manual override can only slow it down. Leaving 800 still needs a
complete all-Ultra inventory and the common app/boot mask: `ultra_line_allowed`
with `ultra_d2a_path_ns(boards, 0)`. Without them a pin of more than 20 boards
is refused. Update sessions on the pin use the same profile: every update entry
point refuses a pin line running faster than its table profile with
`ULTRA_UPDATE_LINE_TOO_FAST` before any frame.

## RMT

Each channel uses its own HIGH/LOW duration words, so profiles are native per
port even when the source clock is shared. Use one channel/encoder context per
port; never change a live encoder's durations. Drain that port's TX and return
window, wait its busy/reset gap, then reacquire. Calculate TIME at the actual
final falling edge. The [ESP-IDF RMT reference](https://docs.espressif.com/projects/esp-idf/en/stable/esp32/api-reference/peripherals/rmt.html)
defines channel resolution and symbol durations; check actual clock capability.

| Profile | 80 MHz ticks: bit / zero / one | Actual kbit/s | 40 MHz ticks: bit / zero / one | Actual kbit/s |
|---|---|---:|---|---:|
| 800 | 100 / 25 / 75 | 800 | 50 / 12 / 38 | 800 |
| 600 | 133 / 33 / 100 | 601.503759 | 67 / 17 / 50 | 597.014925 |
| 400 | 200 / 50 / 150 | 400 | 100 / 25 / 75 | 400 |
| 200 | 400 / 100 / 300 | 200 | 200 / 50 / 150 | 200 |

Nearest ticks, with half-tick zero rounding down and one rounding up, maximize
HIGH-class separation. LOW = bit - HIGH; residual jitter/error <=10 ns around
these actual values. Do not add a rounded ns error a second time. The 40 MHz
800 waveform is 300/950 ns HIGH: its typical-chain decoder margin improves,
while intermediate forwarding reserve falls to 36 ns, still positive. Refuse other
resolutions until their actual width/period/forwarding bounds are checked; 20 MHz is
not silently substituted. No claim that a particular ESP32 permits 80 MHz RMT.

## PARLIO shared clock and grouped transfers

At exactly 9.6 MHz, each lane is encoded independently: zero is HIGH for
3/4/6/12 samples and one for 9/12/18/36, in 12/16/24/48-sample bits.
Pack one lane bit into each shared sample word; all idle lanes are LOW. The
clock must be measured/derived correctly: the driver can round a requested
frequency. Reject silently rounded clocks outside the declared timing bound,
or regenerate and qualify a new timing table. A suitable exact source/divider
or external clock is required; setting `output_clk_freq_hz=9600000` alone is
not proof. See the [ESP-IDF PARLIO TX reference](https://docs.espressif.com/projects/esp-idf/en/stable/esp32p4/api-reference/peripherals/parlio/parlio_tx.html).

Group ports by profile into separate logical transfer queues and buffers;
equal-profile ports with compatible return/busy schedules share a transfer.
Never pad every 800 frame to the duration of a slow frame. Schedule each queue
to its own frame deadlines, reset/return windows and freshness target. Buffers
remain owned by DMA until completion. One shared sample clock does not mean
one shared logical frame boundary: per-lane encoders own their boundaries.

**Separate serialized hardware transfers alone cannot preserve full fast-port
FPS.** One TX engine cannot transmit two buffers simultaneously, and idle
time on a slow lane cannot be inserted into the middle of its frame. For one
600-pixel 800 group and one 200 group, alternating 18.5 and 72.5 ms transfers
gives only 10.99 FPS per group, not 54.05 FPS on the fast group. More queue
depth changes neither this bound nor the shared clock. This is a physical
constraint, not an optional optimization.

To meet the fast-port requirement, use the grouped queues as independent
logical transfers and compose their samples concurrently into a continuous
DMA stream, with each group restarting at its own frame boundary. For the
18.5/24.5/36.5/72.5 ms periods the boundaries are all integer samples; an
800 group may begin fresh frames while a 200 group is still transmitting.
No gaps at DMA buffer boundaries; every lane must retain its phase and frame
buffer lifetime. Avoid a fixed 72.5 ms superframe that truncates faster frames.
Alternatively route the slow group to independent RMT/another real TX engine,
keeping separate physical PARLIO transfers for the 800 groups. Do not assume
multiple software handles are independent hardware. If neither is available,
report the measured serialized FPS reduction rather than claim isolation.
RETURN direction/DE still needs per-lane external control and separate receive
resources; idle LOW is not high impedance. The host composer implements these
logical boundaries; the peripheral adapter must maintain continuous DMA and
qualify the hub's actual clock/engine resources.

## 600 RGB pixel arithmetic

600 RGB = 1800 bytes = 14400 bits. At nominal rates, `fps=1/(bits/rate+500 us)`:

| Line profile | Data ms | With reset ms | Serialization ceiling FPS | Relative to 800 |
|---|---:|---:|---:|---:|
| 800 | 18 | 18.5 | 54.05 | 100% |
| 600 | 24 | 24.5 | 40.82 | 75.51% |
| 400 | 36 | 36.5 | 27.40 | 50.68% |
| 200 | 72 | 72.5 | 13.79 | 25.52% |

RMT 600 at 40 MHz gives 24.62 ms / 40.62 FPS; at 80 MHz, 24.44 ms /
40.92 FPS. The other nominal ceilings are unchanged. This table measures
serialization of the pixel payload, not complete PIXEL_DATA command overhead.
The currently specified packet transport uses BEGIN +75 CHUNK +COMMIT frames;
its eight-board single-buffer rates, including nominal 18.5 ms local output,
are <=3.84/3.05/2.16/1.15 FPS (transport.md). Polling/retries, buffers and
resource ceilings reduce these further. No new raw slow-stream format is
implied. [Diff-Power Ultra Receiver Digital-Medium](https://github.com/intermittech/quinled-ultra-firmware/wiki/Board-Diff-Power-UR-Digital-M) always decodes to bytes and regenerates each actual
strip's timing locally; pixel effects on the receiver retain local render cadence.

## Scheduling / profile change checklist for implementers

Inventory includes all attached active/passive devices. A non-Ultra device, real
pixel, unsupported app/bootloader or unknown device forbids leaving 800. Real
pixels behind a qualified Diff-Power-UR-Digital-M output do not constrain its upstream
port. On profile switch discard queued waveforms made with old timing, recompute
TIME/airtime, retain pixel transaction ownership, and follow the reset/preamble
and rate-lock procedure in timing.md. A port joining late detects edge periods
directly, including recovery at 200 when 800 is undecodable. Never wait for an
800-only announcement to enable the detector. HA exposes the effective profile,
observed worst margin, inventory restriction and actual delivered FPS per port.
