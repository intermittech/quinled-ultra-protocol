# Examples

## `host_frame_example.c`

A minimal, platform-neutral host in plain C99. It shows:

- the start-up preamble;
- LIGHT_DEF and SET with SEQ numbers and 3 copies;
- the refresh;
- TIME computed from the frame's bits;
- start offsets re-stamped per frame;
- the bytes turned into pulses or an SPI stream.

```
cc -std=c99 -O1 -o host_frame_example host_frame_example.c && ./host_frame_example
```

Replace `now_us()` with your monotonic microsecond clock, and send each frame with one of the drivers below.

## Driving the frames out

Whatever the driver, you need:

- '0' = 220-380 ns high, '1' = 580 ns - 2 µs high, MSB first, idle LOW;
- **no pause of 50 µs or more inside a frame;**
- ≥ 280 µs LOW after each frame.

See [Physical layer](https://github.com/intermittech/quinled-ultra-protocol/wiki/Physical-Layer).

| Peripheral | How | Notes |
|---|---|---|
| **ESP32 RMT** (ESP-IDF `rmt_tx` + bytes encoder) | resolution 20 MHz: '0' = 7 ticks high + 19 low (350 / 950 ns), '1' = 14 + 13 (700 / 650 ns), MSB first, end-of-transmission level LOW | A 50-board frame is 12,864 bits, far more than the RMT memory. With DMA (ESP32-S3, -P4) that's no issue. On the ESP32 classic the driver refills the RMT memory from an interrupt: give the channel several memory blocks, and keep other interrupts short. A refill later than 50 µs splits the frame, and the affected slots are dropped. |
| **SPI** (MOSI only, DMA) | 3.0 MHz, 4 SPI bits per bit: '0' = `1000`, '1' = `1100` (333 / 667 ns high, 1.33 µs per bit); `to_spi()` in the example | MOSI must idle LOW. Append ~300 µs of zero bytes (≥ 113 bytes at 3 MHz) as the reset, or wait. 4 bytes per frame byte: a 50-board frame is ~6.4 KB. |
| **I2S** (serial, DMA) | the same pattern as SPI at a 3.0 MHz bit clock | The ESP32 classic's I2S LCD / parallel mode can drive several pins at once. |
| **PARLIO** (ESP32-P4 / -S31) | up to 16 data pins in parallel; 160 MHz ÷ 54 ≈ 2.96 MHz, 4 samples per bit: '0' = `1000` (338 ns), '1' = `1100` (676 ns) | All pins share one transfer. Pad shorter frames with LOW at the start so they all end together. The idle value is LOW. |
| **Bit-banging** | only with interrupts off for the whole frame | Not recommended: a 50-board frame takes 17 ms. |

**Levels:**

- The QuinLED dig2analog-ultra-5ch expects **5 V** data: use a 5 V buffer such as the 74AHCT125 family, with ~33 Ω in
  series.
- For long runs, QuinLED Diff-Power carries the same signal over RS-485.
