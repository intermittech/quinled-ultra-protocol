# QuinLED Ultra sender library

A portable C99 library that lets any controller speak the **QuinLED Ultra protocol 0.2** (wire VERSION 2) to
QuinLED Dig2Analog-Ultra-5ch boards and QuinLED Diff-Power Ultra receivers (Ultra-Digital and Ultra-Analog):
WLED, Tasmota, MoonLight, or your own ESP32 / RP2040 / STM32 firmware.

You say what the lights should do. The library keeps every board's state and builds each frame by the host rules of
the reference controller (ESPHome's `dig2analog_ultra`). You only turn the bytes into pulses.

- **No dependencies, no heap.** The caller provides all storage; nothing needs an RTOS or Arduino.
- **MIT**, like the protocol header `ultra_protocol.h`, which it builds every slot with.
- **Tested** against every protocol test vector (byte for byte), and through the board firmware's own receive path
  and light engine with 1-64 boards, frame loss and power cycles ([Tests](#tests)).

The rules it implements, and why: the protocol wiki's
[Host behaviour](https://github.com/intermittech/quinled-ultra-protocol/wiki/Host-Behaviour) and
[Timing and clock](https://github.com/intermittech/quinled-ultra-protocol/wiki/Timing-and-Clock). An overview for
integrators: the wiki page [Sender library](https://github.com/intermittech/quinled-ultra-protocol/wiki/Sender-Library).

## Install

Version 0.2.8: the protocol release it targets. One package for PlatformIO, Arduino and ESP-IDF; everything is in
`src/`, also `ultra_protocol.h` (a byte-identical copy of the release's `include/ultra_protocol.h`).

- **PlatformIO** (WLED, MoonLight, Tasmota): `library.json`. Put `sender/` in the project's `lib/` folder, or
  `lib_deps = symlink://path/to/quinled-ultra-protocol/sender`.
- **Arduino IDE:** `library.properties`. Zip the `sender` folder and use *Sketch > Include Library > Add .ZIP
  Library*, or copy it to `Arduino/libraries/`.
- **ESP-IDF** (5.0 or later; the RMT output 5.3): `idf_component.yml` + `CMakeLists.txt`. In your `main/idf_component.yml`:
  ```yaml
  dependencies:
    ultra_sender:
      git: https://github.com/intermittech/quinled-ultra-protocol.git
      path: sender
      version: v0.2.8
  ```
- **Anything else** (CMake, Make, an IDE): compile `src/*.c` (C99) with `src/` on the include path.

## Files

| | |
|---|---|
| `src/ultra_sender.h`, `.c` | The scheduler and frame builder: the line, lights, effects, settings, start times. |
| `src/ultra_sender_bits.h`, `.c` | Bit timing, and the encoders: `{high_ns, low_ns}` pulses, ESP32 RMT symbols, SPI bytes. |
| `src/ultra_esp32_rmt.h` | ESP32 only, header-only: an RMT output for a line (ESP-IDF 5.3+, Arduino-ESP32 3.1+). |
| `src/ultra_sender_rc.h`, `.c` | Optional: the return channel (POLL frames, answers; Diff-Power Ultra receivers only). |
| `src/ultra_dport.h` | Optional, header-only: a digital port (an Ultra-Digital receiver in a pixel strip's data path): when a transmission may start, the LOW around it, the back-off, the RMT words. |
| `src/ultra_sender.hpp` | Optional: a thin C++ wrapper with the storage built in. |
| `src/ultra_protocol.h` | The protocol header of release 0.2.8, unchanged. |
| `examples/` | `host_minimal.c` (any PC), `esp32_idf_rmt/` (ESP-IDF, the library as a component), `esp32_arduino/` (Arduino-ESP32). |
| `test/` | `run_tests.py` and the tests (not part of the packages). |

## Quick start

```c
#include "ultra_sender.h"

#define BOARDS 4
static ultra_sender_t line;
static ultra_sender_board_t boards[BOARDS];
static ultra_sender_item_t items[ULTRA_SENDER_ITEMS(BOARDS, 1, 3)];  /* per light: LIGHT_DEF, SET, EFFECT */
static uint8_t frame[ULTRA_SENDER_FRAME_BYTES(BOARDS)];

void ultra_setup(void)
{
    ultra_sender_config_t cfg;
    ultra_light_def_t def;
    ultra_sender_config_init(&cfg, BOARDS, 1);         /* 4 boards, 1 light each */
    cfg.seed = random32();                             /* a random number per boot */
    ultra_sender_init(&line, &cfg, boards, items, sizeof items / sizeof items[0]);

    ultra_light_def_init(&def);                        /* gamma 2.8, applied by the board */
    def.roles[0] = ULTRA_ROLE_RED;  def.roles[1] = ULTRA_ROLE_GREEN;  def.roles[2] = ULTRA_ROLE_BLUE;
    def.roles[3] = ULTRA_ROLE_WARM_WHITE;  def.roles[4] = ULTRA_ROLE_COLD_WHITE;
    for (uint16_t b = 0; b < BOARDS; b++)
        ultra_sender_light_def(&line, b, 0, &def);
}

void ultra_set_color(uint16_t board, uint8_t r, uint8_t g, uint8_t b, uint8_t level, uint32_t fade_ms)
{
    static const uint8_t roles[5] = { ULTRA_ROLE_RED, ULTRA_ROLE_GREEN, ULTRA_ROLE_BLUE,
                                      ULTRA_ROLE_WARM_WHITE, ULTRA_ROLE_COLD_WHITE };
    ultra_color_t c = { 0 };
    ultra_values_t v;
    c.brightness = ULTRA_8TO16(level);                 /* 8-bit x 257, linear: no gamma on the host */
    c.red = ULTRA_8TO16(r);  c.green = ULTRA_8TO16(g);  c.blue = ULTRA_8TO16(b);
    c.rgb_level = 65535;
    ultra_values_from_color(roles, &c, &v);
    ultra_sender_set(&line, board, 0, &v, fade_ms, ULTRA_SET_SMOOTH, now_us());
}

void ultra_loop(void)                                  /* call often; one task only */
{
    uint32_t n = ultra_sender_next_frame(&line, now_us(), frame);   /* now_us(): its first bit goes out now */
    if (n)
        send_frame(frame, n);                          /* in one go, then >= 280 us LOW */
}
```

That is a complete host: the start-up preamble, 3 copies per command, the refresh every 200 ms, TIME and the start
offsets all happen inside `ultra_sender_next_frame()`. `examples/host_minimal.c` runs it on a PC.

## API

All times are one 64-bit microsecond clock (`int64_t`, e.g. `esp_timer_get_time()`, `time_us_64()`; a 32-bit
`micros()`: `ultra_clock64_t clk = { 0, 0 };` and `ultra_clock64(&clk, micros())` at least every 71 minutes). Calls
return `ULTRA_OK`, `ULTRA_ERR_ARG` (an undefined value: refused before anything changes) or `ULTRA_ERR_FULL` (the
item pool is full; nothing changed, the group's start neither).

**The line**

| Call | |
|---|---|
| `ultra_sender_config_init(&cfg, boards, lights)` | Defaults: 3 copies, refresh 200 ms, start delay auto, recommended timing, unused light indices cleared. |
| `ultra_sender_init(&line, &cfg, boards[], items[], n_items)` | 1-64 boards. Starts with the preamble; queue the lights' state right away (automatic starts count the preamble in). |
| `ultra_sender_deinit(&line)` | Leaves its group, takes no more commands or frames; before initializing it again or dropping it. |
| `ultra_sender_set_lights(&line, board, n)` | Lights in use on one board (0-5); the others are cleared (empty LIGHT_DEF, SCENE 0, no power-on effect): 3 copies, then in the refresh's first 3 rounds after init, a resync or a change of `n`, then in every 4th round (a 1-light board's SET every ~1.2 s at rest). |
| `ultra_sender_next_frame(&line, now_us, buf)` | The next frame if one is due, else 0. Back to back while commands wait, every `refresh_ms` at rest. |
| `ultra_sender_retime(&line, now_us, buf)` | The last frame couldn't be sent: the same slots with a new TIME (don't build another: `next_frame()` took its copies from the queues). |
| `ultra_sender_busy()`, `ultra_sender_due_us()` | When to call again. |
| `ultra_sender_resync(&line, now_us)` | The preamble again, then everything as new commands (a manual resync). |
| `ultra_sender_power(&line, on, now_us)` | The boards' supply: after a switch-on (and its power-on delay) a short preamble (10 frames) once a full one reached them, then everything again. |
| `ultra_sender_power_off(&line)`, `ultra_sender_power_off_at(&line)` | The planned power-off notice (a host that switches the line's power, e.g. a Diff-Power port in standby): the next frames carry IDENTIFY 7 in every board's slot, 3 copies with one SEQ per board, 50 ms apart; then the line sends nothing until `ultra_sender_power(&line, 1, now_us)` wakes it (commands given meanwhile go out then). `_power_off_at()`: when the power may be cut, 200 ms after the first copy (never before the last one's end); `INT64_MAX` while copies are still to go out. No take-back: once given, cut. |
| `ultra_sender_switch_to_ultra(&line, now_us, boot)` | Boards forced to WS2805 / WS2915: the WS config-frame sequence, the preamble, everything again; CONFIG 22 = `boot` (0 auto, 3 Ultra, 0xFF leave; another value: `ULTRA_ERR_ARG`). |

**Lights** (`board`, `light` = 0..lights-1)

| Call | Slot |
|---|---|
| `ultra_sender_light_def(&line, b, l, &def)` | LIGHT_DEF: roles per channel, gamma × 1000, flags, min / max power. |
| `ultra_sender_light_opt(&line, b, l, &opt)` | LIGHT_OPT: dim to warm, set points. |
| `ultra_sender_set(&line, b, l, &v, fade_ms, flags, now_us)` | SET: M, A, B; `ULTRA_SET_SMOOTH`, `ULTRA_SET_VIA_OFF`. `_set_at()` takes an absolute start. |
| `ultra_sender_stream(&line, b, l, &v, glide_ms, now_us)` | A host-side effect step: sent once, linear over the update interval. |
| `ultra_sender_effect(&line, b, l, fx, params, n, now_us)` | EFFECT 0-9 (not strobe); `ULTRA_FX_NONE` stops it. Exactly the effect's parameter bytes, from `ultra_fx_pulse()`, `_flicker()`, `_random()`, `_candle()`, `_breathe()`, `_color_loop()`, `_sunrise()`, `_sunset()` (their times saturate at 16,777,215 ms). |
| `ultra_sender_strobe(&line, b, l, steps, count, now_us)` | Strobe: EFFECT + 1-8 STEP slots. |
| `ultra_sender_scene(&line, b, l, mode, fade_100ms, &v)` | SCENE: the power-on state. |
| `ultra_sender_scene_effect(&line, b, l, fx, params, n)` | SCENE_EFFECT: the power-on effect, any but strobe (a board stores no strobe steps: `ULTRA_ERR_ARG`). |

LIGHT_DEF, LIGHT_OPT, SCENE and SCENE_EFFECT with unchanged content send nothing. Undefined roles (8-15), flag bits,
effect ids, parameter lengths, SCENE modes and CONFIG values are refused. `ultra_values_from_color()` turns an RGB /
white / CCT colour into M, A and B for a light's roles.

- `ultra_sender_stream()` doesn't stop a board effect: send `ULTRA_FX_NONE` first when a host animation takes over.
- A board keeps its stored power-on effect until a SCENE_EFFECT changes it: send `ULTRA_FX_NONE` for lights without
  one, not nothing.
- The board takes each channel for one light: keep a board's lights on separate channels.
- A SET starts at most 32.7 s after the frame that carries it (its offset is an int16 of ms; later starts are clipped).

**Boards**

| Call | |
|---|---|
| `ultra_sender_config(&line, board, id, value)` | CONFIG (every id the header defines) on one board or `ULTRA_SENDER_ALL`; changes go out with the next frame and stay in the refresh. Values outside the id's range are refused (`ultra_config_value_ok()`). Signal loss 3 fades to the power-on scene; CONFIG 24, the trips' retry policy: `ULTRA_SENDER_RETRY(count, interval_s)`. |
| `ultra_sender_pwm_hz(&line, board, hz)` | PWM frequency → CONFIG 1 / 2. |
| `ultra_sender_identify(&line, board, value, arg)` | IDENTIFY: blink, version, current calibration, hold dark / release. Three copies, not refreshed. |

**Local pixel effects** (protocol 0.2.8; the QuinLED Diff-Power Ultra-Digital receiver):

| Call | |
|---|---|
| `ultra_sender_pixel_fx(&line, board, segment, &fx, now_us)` | PIXEL_FX for a segment (0-3) of the receiver on `board`: a new effect, its START the group start for now. `fx.effect` 0 hands the strip back to the hub's stream. |
| `ultra_sender_pixel_fx_at(&line, board, segment, &fx, start_us)` | The same with the START you give: pass `ultra_sender_pixel_fx_start()` again for a change of the effect's parameters, colours or brightness, so it carries on in phase. Renew it after ~20 days (the START is an int32 of ms). |
| `ultra_sender_pixel_config(&line, board, id, value)` | CONFIG 129-139 of the strip (`ULTRA_CFG_PX_*`: chip, wire order, pixels, ...), in their own CONFIG slots, one SEQ per changed page, part of the refresh. |

A receiver on a digital port has no lights: call `ultra_sender_set_lights(&line, board, ULTRA_LIGHTS)` for it (nothing
is cleared) and give it one pool item per segment and two for its CONFIG. The library builds the slots; the line's
marker framing and the pixel stream (`ULTRA_DP_*`, the return channel's digital ports) are the caller's. The effect
list is the catalogue (`catalogue/pixel_fx_catalogue.json` in the protocol repository).

**Digital ports** (`ultra_dport.h`; `ultra_sender_digital(&line, 1)` on a line of one receiver). Every Ultra frame of such
a port goes out *behind a marker* in a latch gap the hub stretches (the strip never sees it), the pixel stream goes
between the frames, and a poll leaves the line to the receiver's answer. The library's frames are unchanged (no preamble
and no chain lock on this line, the poll's window is the digital one); `ultra_dport.h` decides **when** a transmission
may start and **what LOW goes around it**, and gives the ESP32's RMT words; you drive the output and build the frames:

```c
ultra_dp_t dp;
ultra_dp_init(&dp, 300 /* the strip's reset, us */);
ultra_dp_powered(&dp, now_us);
/* ... in the loop: */
unsigned want = (ultra_sender_busy(&line) ? ULTRA_DP_W_CMD : 0) | (poll_due ? ULTRA_DP_W_POLL : 0) |
                (frame_ready ? ULTRA_DP_W_PIXELS : 0);
ultra_dp_tx_t tx;
if (ultra_dp_next(&dp, now_us, want, &tx)) {
    /* tx.kind: PIXELS, FRAME (a command, a refresh, a header) or POLL; tx.lead_low_us / tx.tail_low_us: the LOW around
       it. A marker frame starts its first bit at now_us + tx.lead_low_us + ULTRA_DP_MARKER_US + ULTRA_DP_GAP_HUB_US:
       build it there (ultra_sender_next_frame() / ultra_sender_rc_frame()), encode it with ultra_dp_rmt_encode(). */
    ultra_dp_started(&dp, &tx, now_us, wire_us, t_end_us, ULTRA_RW_RATE_1M);
}
/* when a poll's window is over: */
ultra_dp_poll_result(&dp, now_us, answered);
```

| Call | |
|---|---|
| `ultra_dp_next()` / `ultra_dp_started()` | The next transmission that may start (a command or refresh frame, a poll, the header a local port is due, then pixels; a pixel frame that is due goes before a second marker frame in a row, a poll before the next command frame) and what it did to the line. |
| `ultra_dp_powered()` | No marker for `ULTRA_DP_POWERUP_MS` after the port is powered (pixels may go). |
| `ultra_dp_local()` / `ultra_dp_handback()` | A local effect runs: no pixels, a header at least every `ULTRA_DP_BEACON_MS`. PIXEL_FX 0: the pixels wait for the receiver's frame and a reset. |
| `ultra_dp_poll_result()` / `ultra_dp_retry()` / `ultra_dp_blocked()` | The back-off: 3 polls in quick succession, 3 more 60 s later, then no marker frame until the user's retry; an answer in between starts it over. |
| `ultra_dp_rmt_*()` | The RMT words (lead LOW, marker, bits, tail LOW) at any resolution, and an encoder step in the shape of ESP-IDF's simple-encoder callback. |

The ESPHome component `dig2analog_ultra` (`digital:`) keeps a byte-identical copy of `ultra_dport.h`.

**Start times.** `ultra_sender_set()`, `_effect()` and `_strobe()` start at `ultra_sender_start_time()`: far
enough ahead that the commands reach every board first (one slot per board per frame), and shared by the commands
queued close together, also across the lines of a group (`ultra_sender_group_init()`,
`ultra_sender_join_group()`, `ultra_sender_leave_group()`), and after the preamble or switch sequence still to send.
It assumes frames go out back to back while commands wait; later frames still work (a late copy catches up), a fade
then shows partly done. The budget is a SET per light of the fullest board (it follows `ultra_sender_set_lights()`);
when one change also sends EFFECTs, STEPs or SCENEs, raise it: `ultra_sender_set_group_commands()`. Periodic effects
start on the clock's phase, so lights started apart run in step. `cfg.start_delay_us` sets a fixed lead instead.

**Storage.** Size it with `sizeof`: on the ESP32 `ultra_sender_t` is 144 bytes, `ultra_sender_board_t` 128 per
board, `ultra_sender_item_t` 60 per piece of a light's state it ever had (LIGHT_DEF, SET, EFFECT, LIGHT_OPT, SCENE,
SCENE_EFFECT, each strobe step: at most 14 per light; items stay taken until the line is initialized again,
`ultra_sender_items_used()` shows them), plus the frame buffer (32 N + 8 bytes). 50 boards with one RGBWW light each
(3 items): ~17 KB; 10 boards × 5 lights: ~10.5 KB. The library itself keeps two function pointers (8 bytes, set by
`ultra_sender_switch_to_ultra()` and the return channel) and no other state.

**Lifetime.** `init()` keeps pointers to the line, its boards and items and its group: they stay alive at the same
addresses until `ultra_sender_deinit()` (never copy an initialized `ultra_sender_t`; the C++ `ultra::Line` can't be
copied or moved). A refused `init()` or C++ `begin()` changes nothing: a running line keeps running, in its group
(`ultra_sender_config_check()` tells beforehand). Commands copy their inputs. The frame buffer is the caller's; the line
keeps the timing data of the last frame it built only, so build a frame when it can go out.

**Code size** (`-Os`, after the linker drops what isn't called): ~6.1 KB on an ESP32 (Xtensa) and ~7.1 KB on RV32IMC
for a host that only sets states and refreshes; ~8.3 / 9.7 KB for one that defines lights, sets states, runs effects
and sends settings; every function: 13.4 / 15.2 KB; the return channel +1.2 / 1.4 KB. Cortex-M (Thumb-2) is usually a
little smaller than RV32IMC. No 64-bit division from libgcc (the library divides with its own exact helper, a 32-bit
division in the frame path); from libc only `memset` / `memcpy`. The WS switch frames and the return channel's
response window link in only with `ultra_sender_switch_to_ultra()` / `ultra_sender_rc_frame()`.

**Threads.** One line (or one group of lines) belongs to one task. Commands from other tasks: take the same lock
around them and around `ultra_sender_next_frame()`. Don't change the frame buffer while it is being sent.

**TIME accuracy.** TIME is computed from each frame's own bits, and the start offsets are bits of the frame too: the
line stamps and measures until the frame carries the TIME it measures. The bounds (64 boards, 1000 frames per timing;
`test_review` checks them on the 16 corners of the legal timing box and 160 random timings):

| The line's timing | TIME vs. the frame's end | Every start |
|---|---|---|
| '0' and '1' periods within 150 ns of each other (the recommended timing, the ESP32 RMT at 40 or 8 MHz, SPI) | within 2 µs (recommended, RMT: 1 µs; SPI at 4 MHz: exact, other clocks add their whole-ns rounding, at most 0.05 % of the frame) | within 502 µs (the nearest ms) |
| Any other legal timing (periods up to 12 µs apart) | within 100 µs | within 2 ms |

Measured over 3,000 random legal timings and 2,000 near-equal ones: 2 µs / 502 µs near-equal, 57 µs / 1.46 ms
anywhere (the worst: a long '0' against a short '1').

## Bit timing

| | Allowed | Recommended (`ULTRA_TIMING_RECOMMENDED`) |
|---|---|---|
| '0' high | 220-380 ns | 250 ns |
| '0' low | 0.45-10 µs | 1050 ns |
| '1' high | 580 ns - 2 µs | 700 ns |
| '1' low | 0.45-10 µs | 650 ns |
| Reset (LOW after each frame) | ≥ 280 µs | 300 µs |

MSB first, idle LOW. The board measures only the HIGH time, so a constant bit period is fine.

**The line's timing must be the wire's.** The line computes every frame's TIME from `cfg.timing`; give it the timing
the output really sends, before `ultra_sender_init()`:

| Output | Set up |
|---|---|
| ESP32 RMT | `ultra_esp32_rmt_begin(&out, gpio, &cfg.timing)` first: it rounds to whole RMT ticks and writes back what it sends (40 MHz: the recommended timing exactly). Lower level: `ultra_rmt_init()`, `.effective`. A resolution that rounds a value out of the spec is refused. |
| SPI / I2S + DMA | `ultra_spi_timing(spi_hz, &cfg.timing)` with the real SPI clock (about 2.7-4.45 MHz, any other clock is refused; 4 MHz: 250 / 1000 / 750 / 500 ns), then `ultra_spi_encode()`; the reset: `ULTRA_SPI_RESET_BYTES(spi_hz)` zero bytes or a 300 µs wait. |
| RP2040 PIO (below) | `cfg.timing = (ultra_timing_t){ 260, 1040, 780, 520, 300000 };` |

**No gap of 50 µs or more inside a frame.** A board ends its capture there and drops the slot; 100-150 µs ends the
frame. Send each frame in one go: DMA, or a FIFO refilled well within 50 µs. Never bit-bang with interrupts on.

| Output | Helper | Notes |
|---|---|---|
| ESP32 RMT | `ultra_rmt_init()`, `ultra_rmt_encode()`; `ultra_esp32_rmt.h` | Encodes from the frame bytes in chunks (an RMT simple-encoder callback in IRAM): no symbol buffer. 40 MHz resolution. `ultra_esp32_rmt_send()`: `ULTRA_RMT_OK`; `_NOT_SENT` or `_ABORTED` (a stalled frame is stopped first): the buffer is free, retime and send again; `_STUCK`: the driver may still read it: `ultra_esp32_rmt_end()`, which releases the channel only once it has stopped (0); -1 = not stopped: it keeps the handles, and the output struct and the frame buffer must stay in place (static) and untouched; call it again later or restart. Not for return-channel RW frames (it appends the 300 µs reset). |
| SPI / I2S + DMA (any MCU) | `ultra_spi_encode()` | 5 SPI bits per bit at 4 MHz ('0' = `10000`, '1' = `11100`: 250 / 750 ns high); 2.7-4.4 MHz is inside the spec. MOSI must idle LOW. |
| Timers, PIO, anything else | `ultra_encode_pulses()` | `{high_ns, low_ns}` per bit; the last one's low includes the reset. |

RP2040: the pico-examples `ws2812` PIO program works with T1 = 2, T2 = 4, T3 = 4 cycles of 130 ns (1.3 µs per bit:
'0' 260 / 1040 ns, '1' 780 / 520 ns). Its stock 800 kHz timing doesn't: a '1' would be LOW for only 375 ns.

**Level:** the Dig2Analog-Ultra wants 5 V data: a 74AHCT125-type buffer with ~33 Ω in series.

## Chain length and timing

| Boards | Frame + reset | Frames / s | Preamble (4 N + 2) | Auto start delay, 1 / 5 lights per board |
|---|---|---|---|---|
| 1 | 0.7 ms | ~1,380 | 6 frames, 4 ms | ~14 / ~19 ms |
| 10 | 3.8 ms | ~260 | 42 frames, 0.16 s | ~23 / ~40 ms |
| 25 | 8.9 ms | ~110 | 102 frames, 0.9 s | ~39 / ~77 ms |
| 50 | 17.4 ms | ~57 | 202 frames, 3.5 s | ~65 / ~137 ms |

QuinLED supports up to 50 boards per data pin; the library takes 64. Each board takes one command per frame: five
lights streamed on one board get every fifth frame each, so let the board glide (`ultra_sender_stream()`).

## Porting notes

**WLED (a usermod).** One `ultra_sender_t` per Ultra output pin, created from the usermod's settings (boards, lights
per board, roles). Map each WLED segment (or a range of LEDs) to a board's light:

- Read the segment's colour and brightness as WLED stores them (before the output gamma its bus layer applies) and
  send them linear; the board's gamma (LIGHT_DEF) does the curve, in 16 bit.
- A change of the segment (colour, brightness, on / off) is `ultra_sender_set()` with WLED's transition time; leave
  the fading to the board.
- WLED effects run on the host: stream every frame's value with `ultra_sender_stream()` (glide = the effect's
  frame interval), at most one update per light per Ultra frame. Or map simple effects (breathe, colour loop,
  candle, strobe) to board effects, which run in step on every board without traffic.
- Output: on the ESP32 an RMT channel the bus manager doesn't use (`ultra_esp32_rmt.h`), or I2S / SPI with
  `ultra_spi_encode()`. Call `ultra_sender_next_frame()` from the usermod's `loop()`, not from the LED bus's show
  path.
- WLED's CCT (0-255) splits into `warm_white` / `cold_white`; the board's `ULTRA_DEF_CONSTANT_BRIGHTNESS` keeps
  the total.

**Tasmota.** A driver (`xdrv`) owning one pin: lights map to Tasmota's channels (`light_state`); `Fade` / `Speed`
become the SET's transition. Call `ultra_sender_next_frame()` from `FUNC_LOOP` (it returns 0 until a frame is
due). On the ESP32 use the RMT helper. The ESP8266 has no RMT and its SPI no DMA: an I2S-DMA output (as NeoPixelBus
uses) fed with the bit pattern of `ultra_spi_encode()`.

**MoonLight (MoonModules).** Ultra boards fit as a driver node that owns a pin: each board's lights are channels
of its layout. MoonLight's effects stream per light (`ultra_sender_stream()`), or a light runs a board effect that
needs no traffic. On the ESP32-S3 / P4 the RMT with DMA (`ULTRA_RMT_DMA 1`) carries long chains; PARLIO (P4) drives
several pins in parallel (the bit patterns: the protocol's `examples/README.md`).

**Your own firmware.** Any MCU with SPI + DMA, a timer + DMA, or PIO. Keep one 64-bit µs clock, call
`ultra_sender_next_frame()` whenever the line is free, and give `cfg.seed` a random value per boot.

## Return channel (optional)

`ultra_sender_rc.h` builds RW frames with POLL slots for Diff-Power Ultra receivers and extracts their answers from
the UART bytes of each response slot. The Ultra-Analog (hardware 2) and the Ultra-Digital (hardware 3) receivers
answer; a page a receiver doesn't have comes back as `ULTRA_PAGE_UNSUPPORTED`. The Dig2Analog-Ultra never answers (it
doesn't report back); a host that never polls needs none of it. Tested against the protocol's vectors and the
receiver firmware (`test_sim_rx`, below).

- No RW frame until the chain has seen a second of headers after its preamble (time and frames).
- The transport sends the RW frame without the reset gap and releases the line within 150 µs.
- Any page with its arguments: `ultra_rc_request_add(&rq, board, ULTRA_PAGE_DIAG, &part, 1)` (`uint8_t part`: 0 the
  running board, 1 the fault memory); a board without the page answers `ULTRA_PAGE_UNSUPPORTED`.
- The answer's DATA with the header's offsets and `ultra_get16/24/32`; `ultra_rc_features()` /
  `ultra_rc_feature(a, bit)` (IDENTITY part 1, `part` = `ULTRA_ID_PART_FEATURES`: what the board takes,
  `ULTRA_FEAT_*`, bits 0-191; part 0 is the board: UID, versions, hardware id and revision, capabilities) and
  `ultra_rc_status_why()` (STATUS: why the light isn't as set, `ULTRA_ST_WHY_*`).
- **The hub's lifetime counters** (LIFE_SYNC, `ULTRA_FEAT_LIFE_SYNC`): a hub that keeps a ledger of each
  receiver's LIFETIME counters (by its UID) gives them back after the receiver's power-up, so what it couldn't store
  before a power cut isn't lost: `ultra_rc_request_life_sync(&rq, board, uid16, lifetime, total_wh, records_lo)`
  (LIFETIME bytes 0-15 as the ledger has them, the lifetime Wh, the low byte of the RECORDS the ledger was reconciled
  against). The receiver merges once per power-up, only with its own UID16 and RECORDS byte, raises each counter by at
  most its step and never lowers one. The answer: `ultra_rc_ls_result()` (`ULTRA_LS_R_*`), `ultra_rc_ls_applied()`
  (`ULTRA_LS_F_*`), `ultra_rc_ls_total_wh()`, `ultra_rc_ls_records()`, and the counters after the merge in DATA
  0-15. Once per receiver power-up when the ledger is ahead; before a planned power-off, sync first, then the notice.
  The ledger is the hub's own accounting, never an echo of the board's reads: each session's seconds and mWh (STATUS
  uptime / the port's on time, ENERGY's BOOT_MWH) added to fine totals and sent as whole wire units; POWER_UPS = the
  ledger + the power-on just performed; a board read raises a time / energy total only by its lead beyond one unit
  (the wiki: Return channel, "The hub's ledger").
- `ultra_rc_answer()` checks SYNC, framing and CRC; match INDEX, SEQ, PAGE and UID16 to the window you sent, start each
  window's parsing afresh, and poll little enough that commands and the refresh still get their frames.

Firmware updates over the data line are not part of this library: the update sender is ESPHome's
([quinled-ultra-esphome](https://github.com/intermittech/quinled-ultra-esphome), `components/dig2analog_ultra`:
`update_session.h`, `fw_update.cpp`, `ultra_update.h`).

## Protocol updates

A new protocol release: `python sender/test/run_tests.py --sync-header` copies `include/ultra_protocol.h` into
`src/`; then set the new version in `library.json`, `library.properties`, `idf_component.yml` and
`ULTRA_SENDER_VERSION_STR` (the `manifests` step checks all four against the header's release).

The library builds every slot with the header and follows its constants: new CONFIG ids
(`ULTRA_CFG_COUNT`) get storage and CONFIG slots by themselves, new effects (`ULTRA_FX_COUNT`) and POLL pages pass
through. What needs a line of code: the value range of a new or extended CONFIG id (`cfg_range` in
`src/ultra_sender.c`), the period of a new periodic effect (`ultra_fx_period_ms`), a new IDENTIFY value
(`build_identify`), a new slot type (a rank in the item key, and its builder).

## Tests

```
python sender/test/run_tests.py
```

- **`manifests`** (no compiler needed; `--only manifests` for CI): the four manifests list exactly the sources and
  headers in `src/`, every version is the release of `src/ultra_protocol.h`, that header and `LICENSE` are
  byte-identical to the repository's, and `src/` includes nothing from outside itself.
- **`test_dport`:** a digital port (0.2.8): `ultra_dport.h`'s planner and RMT words, and the sender library as a hub (a
  reference hub in `test/dport_driver.h`), checked on the wire the words describe: the LOW before every marker and after a
  frame, the 20 µs marker, the window and the retake, a header a second to a local port, the hand-back, the wait after the
  power-up, the 3 + 3 back-off, a command storm that starves neither the stream nor the polls; negative controls.
- **`dline`** (needs the firmware, like `test_sim`): the same reference hub on the Ultra-Digital receiver's own delay line
  (`core/dline.c` on the firmware's sample-exact model of its hardware, `test/test_dline.c` patched as text): every pixel
  frame whole and in order on the strip, no marker, frame, release or answer on it, every poll answered, for the chips of
  the receiver's table, frames back to back, commands, and a local effect.
- **`test_pixel`:** the local pixel effects (0.2.8): PIXEL_FX (3 copies, the START against TIME, a parameter change
  keeping it, replacement, the refresh, a resync), the pixel CONFIG pages (one SEQ per changed page, an unchanged value
  queues nothing), the refusals and the pool, and a board next to a pixel receiver sending what it sent without.
- **`test_review`:** the independent reviews' findings (round 7, its re-check, round 14), each a regression that
  failed before its fix: start-up and raised-light-count starts, the retimed preamble, partial / NULL effect
  parameters, group lifecycle, RMT rounding, the TIME / start bounds over the legal timing space, SPI timing and every
  SPI clock, CONFIG ids 16 and up, a stray 0xA5 before an answer, the longest reset, no power-on strobe; and the
  probes that passed (exact-size answer buffers, corruptions, a request's limits, 52 days of uptime).
- **`test_cpp`** also: a refused `begin()` (lights, timing, boards) leaves a running line as it was, in its group.
- **`test_cpp`:** the C++ wrapper at run time: capacity, no copy / move, begin again, group teardown.
- **`test_equiv`:** the arithmetic the size pass changed (round 9) against its old code, kept verbatim in the test:
  the division helper against 64-bit division, the whole TIME / start-offset pipeline on random frames, the effects'
  phase, the PWM presets, the RMT ticks, the frame time and the SPI encoder: identical results. Round 11's CPU pass
  too: the 1 bits counted once per frame and unchanged SETs not sealed again, no CRC at queue time, the SPI nibble
  table and the equal-period shortcut (SPI timing): the same frames.
- **`test_rotation`:** the refresh rotation round by round (the clearing slots in rounds 0, 1, 2, 6, 10, ... after
  init, a light-count change and a resync, each reset mid-round), against `test/rotation_trace.txt`: the golden that
  ESPHome's scheduler matches line for line.
- **`test_rmt`:** `ultra_esp32_rmt.h` against a mocked ESP-IDF RMT driver (refusing what ESP-IDF refuses): the IRAM
  encoder's symbols, unwinding a failed set-up, OK / not sent / aborted after a stall / stuck, and `end()` keeping
  everything while the channel can't be stopped or a transaction is still in flight.
- **`asan`:** those tests again under AddressSanitizer (MSVC; UBSan too with gcc / clang).
- **`--release`:** a step that can't run (no firmware for `test_sim`) fails instead of being skipped.
- **Warnings:** the library at `-Wall -Wextra -Wpedantic -Werror` with every compiler found (MSVC `/W4 /WX`, GCC,
  Clang, the ESP32 and RISC-V cross compilers), and the C++ wrapper.
- **`test_vectors`:** every vector of `test-vectors/vectors.json`, produced through the library's API with the clock
  set to the vector's TIME, byte for byte; the receiver-only vectors with the header's parser; a CONFIG vector whose
  pairs aren't in id order (`slot_config_retry_loss`) as the same pairs (the library packs them in id order).
- **`test_unit`:** the host rules frame by frame (preamble, copies, order, refresh, NOP, TIME, start offsets, group
  starts, CONFIG, clearing and its rotation, power-on, resync, streams, strobe, the pool, the encoders), the wraps of
  the queue order and the stream history, `ultra_rc_answer()` (valid, junk, A5 in DATA, truncated, corrupt, framing
  errors), the pulse / RMT / SPI output decoded on its own against TIME, every effect's parameter layout.
- **`test_sim_rx`:** the same firmware built as the Diff-Power Ultra-Analog receiver (`BOARD=2`). LIFE_SYNC: the library's POLL
  through the line's RW frame into the receiver's own merge (`energy_sync`) and answer, read back with the accessors:
  merged once per power-up, a wrong UID16 or RECORDS byte merges nothing, a ledger behind lowers nothing. The power-off
  notice: applied once on every board, also with a flash stall over one or two of its copies; after the cut every board
  powers up dark and has the host's state right after the wake's preamble (`test_sim`: a Dig2Analog-Ultra ignores the
  notice). The receiver's own standby (the 0.3 s fade, the 1.5 s grace before its power-on scene) is its main loop's,
  not simulated.
- **`test_sim`:** the frames through the board firmware's own receive path and light engine (`frame_rx.c`,
  `proto.c`, `ultra.c`), each board with its own clock error, forwarding by its own gate plan: every command type on
  1-64 boards with 20 % frame loss, new and WS2805-locked boards through the preamble, effects and transitions in
  step across two lines, power cycles, a host restart, a streamed effect, a fade queued during the preamble, TIME's
  32-bit wrap with SEQ 255 → 0, every effect 1-9 (and as the power-on effect: the library takes exactly what the
  board runs at power-on), first copies before the group's start, signal loss 3 and the retry policy in every
  board's settings. It needs the firmware's `src/core` (`--firmware DIR` or `ULTRA_FIRMWARE`) of
  the same protocol release: its `ultra_protocol.h` byte-identical to `src/ultra_protocol.h` (both sides share one
  translation unit), else the step fails; without a firmware it is skipped (a failure with `--release`).
- **Targets:** an `unsigned int` of 32 bits or more (`ultra_sender.h` stops the build otherwise: the protocol header is
  16-bit-`int` safe; the library has never been built or tested on such a target); any byte order (every
  multi-byte field is built byte by byte; not run on a big-endian target here). RMT timing on hardware isn't covered.

## License

MIT, Copyright (C) 2026 QuinLED. See [`LICENSE`](../LICENSE). "QuinLED" is a trademark: see
[`TRADEMARK.md`](../TRADEMARK.md).
