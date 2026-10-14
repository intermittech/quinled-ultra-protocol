# Changelog

Releases of this repository. While below 1.0 they are numbered 0.x.y, and the MINOR number equals the wire
format's VERSION (`ULTRA_VERSION`, header byte 2): 0.2.y = wire VERSION 2. A compatible addition raises the
PATCH number; an incompatible wire change would be 0.3.0 (VERSION 3).

## v0.2.4 (wire VERSION 2), 2026-10-14

Backward compatible: `ULTRA_VERSION` stays 2. Nothing changes for a board without a return channel (every
dig2analog-ultra) or for a controller that never sets RW. **The return channel is defined:** a controller can now be
written from the spec alone. It is implemented by the QuinLED Diff-Power ultra receiver's firmware (0.6.8, `BOARD=2`).

- **POLL** (slot type 0x07, LIGHT 0xF; `ULTRA_POLL_INDEX` / `_SLOTS` / `_PAGE` / `_RATE` / `_ARG` = slot bytes 3-7):
  answer index, R, page, rate, then the page arguments from slot byte 7. Valid (`ultra_poll_valid`): 1 ≤ R ≤ 16,
  index < R or 0xFF (relay only: the page and its arguments are ignored), a known rate. A valid POLL is always
  answered, a repeated SEQ too.
- **Fixed:** the POWER acknowledge and the SUPPLY selector are at **slot byte 7** ("argument 0"), as the header always
  said; the 0.2.3 text's "payload byte 7" was wrong. CAL_TEST's part is argument 0 too; parts 2-255 answer UNSUPPORTED.
- **Who takes part:** a board whose own slot is a valid POLL in a frame with RW **and without EXT**.
- **Host rules:**
  - the first RW frame `ULTRA_RW_LOCK_MS` (1 s) after the chain's preamble; none during an update session, never with
    UPD or EXT;
  - in one RW frame every POLL has **the same R and rate**; the answer indices are unique, < R and rise along the
    chain; boards between the hub and an answering board get 0xFF;
  - behind the last board that answers or relays, the slots may be of any type;
  - the next frame ≥ W + 200 µs after the frame end, answers or not; a missing answer is retried in a later window
    with a new SEQ.
- **Rates** (`ULTRA_RW_RATE_*`): 0 = 1 Mbaud, 1 = 500 kbaud, 2 = 250 kbaud; 3-255 reserved.
- **The burst:** one **0xFF lead byte** (`ULTRA_RSP_LEAD`, `ULTRA_RW_LEAD_BYTES`) and the 32-byte answer, UART 8N1,
  LSB first. The hub discards every byte before a SYNC without a framing error: the turnaround checks' pulses reach it
  as junk bytes, and the lead byte ends any of them before SYNC (`ULTRA_RW_SYNC_IDLE_BITS`: ≥ 9 bit times of HIGH
  before SYNC's start bit).
- **Slot pitch** `(330 << rate) + 130` µs = **460 / 790 / 1450 µs** (`ultra_rw_slot_us`, `ultra_rw_window_us`; the
  burst's 33 bytes plus 130 µs of tolerance); `ULTRA_RW_SLOT_US` is 460.
- **Burst timing** (s_i = 1000 µs + i × pitch): the line HIGH at s_i − 20 / + 80 µs (`ULTRA_RW_EARLY_US`,
  `_LATE_US`; slot 0 from s_0 − 5 µs, `ULTRA_RW_EARLY0_US`), the first start bit 2-10 µs later (`_START_MIN_US`,
  `_START_MAX_US`), the line LOW again by s_i + pitch − 30 µs (`_GUARD_US`), the lead byte included.
- **Window end:** forward mode again W .. **W + 80 µs** (`ULTRA_RW_END_US`; the receiver measures W + 1.3 .. 66.8 µs
  over every R, rate, ±200 ppm and ±2.2 % HSI).
- **Turnaround check** (`ULTRA_RW_CHECK_US` 920, `_CHECK_HIGH_US` 5): every board taking part drives the line HIGH for
  at most 5 µs and reads it back. LOW = a board above it doesn't take part: no relay and no answer for the rest of the
  window. **The same after a burst's read-back finds the line taken.** Either sets CONTENTION in the next answer.
- **Answer:** UID16 = `ultra_crc16` over the 12 unique-id bytes in address order (a fingerprint; IDENTITY has the
  full id). RFLAGS: EVENT and CONTENTION; **STALE is reserved and never sent**; unknown bits are ignored.
- **STATUS** completed (`ULTRA_ST_*`): firmware version, hardware id, uptime, reset cause, board temperature, bus
  voltage, input current, what is measured, PWM preset, CONFIG fingerprint (`ultra_config_crc`), modes.
- **OUTPUTS, ACKS, ERRORS** defined (`ULTRA_OU_*`, `ULTRA_AK_*`, `ULTRA_ER_*`); optional: a board may answer 0xFF.
- **IDENTITY:** offsets named (`ULTRA_ID_BL_VERSION`, `_HW_ID`, `_HW_CAPS`), hardware capabilities defined
  (`ULTRA_HWCAP_RETURN`, `_RELAY`, `_VBUS`, `_CURRENT`, `_NTC`; 0x0020-0x8000 reserved).
- **HEALTH:** no temperature peak yet = 0x8000 and age 0xFFFFFF.
- **LIGHT_DEF:** gamma 0 means linear, as 1000.
- **Host guidance** (wiki: Return channel): decoding, when to poll, the POWER acknowledge, page intervals.
- Host helpers: `ultra_slot_poll`, `ultra_poll_valid`, `ultra_rw_slot_us`, `ultra_rw_window_us`,
  `ultra_answer_check`, `ultra_config_crc`.
- **Test vectors:** `hdr_flag_rw` reworded; new `slot_poll_status`, `slot_poll_relay`, `slot_poll_power_ack` and
  `frame_rw_poll`. Every other vector is unchanged.

## v0.2.3 (wire VERSION 2), 2026-10-13

Backward compatible: `ULTRA_VERSION` stays 2. A board from before 0.2.3 ignores the new slot type and skips the new
CONFIG id (the rest of the slot applies); the new pages are inside the reserved return channel, which only QuinLED
Diff-Power ultra receivers will implement.

- **SCENE_EFFECT, slot type 0x0A (`ULTRA_SCENE_EFFECT`): a power-on effect.** From power-up until the controller
  takes over, the board runs the effect on the light's SCENE values by itself, no controller needed (a candle, a slow
  colour loop, a sunrise).
  - Payload: the effect id (1) and its parameters as in EFFECT, without the start time. `ULTRA_FX_NONE` = the plain
    SCENE values; `ULTRA_FX_STROBE` shows the plain values (its steps aren't stored).
  - The board stores it with the light's LIGHT_DEF, LIGHT_OPT and SCENE, written only when it changes: send a fixed
    power-on effect, not the live one.
  - Its own SEQ space. When the controller takes over, the outputs cross-fade from the effect (the SCENE fade).
  - `ultra_slot_scene_effect()` builds it; future per-board slot types are now 0x0B-0x7F (`ULTRA_TYPES`).
- **CONFIG 23, `ULTRA_CFG_SOFT_START` (persisted):** 0 off, 1 on (the default). After power-up and after a trip's
  retry the board ramps its outputs from 20 to 100 % over 0.3 s, starting 0-100 ms late (from its chip id), so the
  boards on one supply or one Diff-Power port don't switch their load on at once. `ULTRA_CFG_COUNT` is now 24.
- **Monitoring pages** (return channel, Diff-Power receivers):
  - **SUPPLY 0x09 (`ULTRA_SU_*`):** supply dips against a slow mean of the supply (so it works at 12, 24 or 48 V):
    the count since power-up, the reference, the supply now, the deepest dip (mV, %, age) and a log of the last 4
    dips (age, lowest mV and %, duration, the board current then; the POLL argument selects one).
  - **HEALTH 0x0A (`ULTRA_HE_*`):** each channel's class from the last current calibration test (no load, normal,
    abnormal with the reasons: not rising with the level, far above / below the test before), its change against the
    test before, the running model against the stored test, the current while every output is off, and the MCU
    temperature peak since power-up with its age. Display only.
  - **ENERGY 0x0B (`ULTRA_EN_*`):** the input energy since power-up (mWh) and over the board's life (Wh), the power
    now, and the age of the stored lifetime value.
- **POWER page:** a new latch, `ULTRA_PW_EV_SUPPLY_DIP` (0x08). The acknowledge argument gains the dip count (POLL
  bytes 14-15); a 0.2.2 host sends 0 there, so its acknowledge leaves a dip latched.
- **Feature bits 12-16:** `ULTRA_FEAT_SUPPLY_DIPS` (0x00001000), `ULTRA_FEAT_HEALTH` (0x00002000),
  `ULTRA_FEAT_ENERGY` (0x00004000), `ULTRA_FEAT_SOFT_START` (0x00008000), `ULTRA_FEAT_SCENE_EFFECT` (0x00010000).
- **Pages:** 0x09-0x0B are taken; later official pages are 0x0C-0x7F.
- **Wording:** CONFIG 11-21 describe the board limit as boards with the input current sense apply it (the measured
  input current, all channels scaled together); the computed limit of boards without the current sense is gone from
  the comments.
- **Test vectors:** `slot_scene_effect` and `slot_config_soft_start`.

## v0.2.2 (wire VERSION 2), 2026-10-13

Backward compatible: `ULTRA_VERSION` stays 2. Everything is inside the reserved return channel, which only QuinLED
Diff-Power ultra receivers will implement. No frame a controller sends today changes.

- **The board current limit and overcurrent events**, so a controller can show them (e.g. in Home Assistant):
  - **STATUS byte 8 (`ULTRA_ST_STATE`):** the power state now.
    - `ULTRA_ST_LIMITING`: the board limit reduces the outputs.
    - `ULTRA_ST_TRIPPED`: the outputs are off after an overcurrent trip.
    - `ULTRA_ST_POWER_EVENT`: an event is latched.
    - `ULTRA_ST_CH_LIMITING`: a channel limit is active.
    - The limiting bits are held 2 s, so they don't flicker.
  - **POWER page 0x08 (`ULTRA_PAGE_POWER`, `ULTRA_PW_*`):**
    - the state now, and latched events since the controller's last acknowledge (limited, overload trip,
      short-circuit trip);
    - counters for limit events, overload trips and short-circuit trips;
    - the seconds since the last limit event and the last trip;
    - the board limit in effect and its source (board rating, user, PWM, fuse);
    - the last trip's cause;
    - the measured current at the last limit event and at the last trip;
    - the limiter's factor now;
    - the seconds until the retry.
  - **Acknowledge (`ULTRA_PW_ARG_ACK`):** a POLL of the POWER page can carry the counts the controller last read.
    A latch clears only when its count still matches, so an event after the read is never lost, and a repeated POLL
    is harmless.
  - **Feature bit 11, `ULTRA_FEAT_POWER_EVENTS` (0x00000800).**
- **Pages:** 0x08 is taken; later official pages are 0x09-0x7F.

## v0.2.1 (wire VERSION 2), 2026-10-13

Backward compatible: `ULTRA_VERSION` stays 2, and boards and controllers of 0.2.0 work with 0.2.1 ones.

- **IDENTIFY value 5, `ULTRA_IDENT_VERSION`:** the board blinks its firmware version once on its feedback channel:
  - 3 fast;
  - then major, minor and patch as value codes;
  - then alpha / beta / rc as 1 / 2 / 3 short.

  Firmware from before 0.2.1 ignores it (the value was reserved), so no blink means older firmware.
  `ultra_slot_identify()` builds it; values 6-255 stay reserved.
- **Feature bit 10, `ULTRA_FEAT_VERSION_BLINK` (0x00000400):** reports IDENTIFY 5 on the reserved IDENTITY page.
- **Test vector** `slot_identify_version`.
- The header's first line names its release.

## v0.2.0 (wire VERSION 2), 2026

The first public version.

- **Frame:** N slots (32 bytes each, board 0 first), then the 8-byte header as a **trailer**.
- **TIME** is the host clock at the end of the frame (the falling edge of the header's last bit).
- **Start-up preamble:** 4 N + 2 frames of DARK slots (zero bits).
- **Slot types:**
  - 0 NOP, 1 LIGHT_DEF, 2 SET, 3 EFFECT, 4 STEP, 5 CONFIG, 6 IDENTIFY, 8 SCENE, 9 LIGHT_OPT;
  - 7 POLL is reserved.
- **Reserved:**
  - header FLAGS RW (return channel), HINT, EXT, UPD (firmware update);
  - ranges for slot types, lights, roles, effects, CONFIG ids, return-channel pages and feature bits;
  - broadcast / extension slots after the last board's slot.

## Format 1 (withdrawn, never released)

The header came first.

- A board blocks its own slot while it forwards the rest, so the next board saw a ~320 µs LOW gap after the header.
- That gap ended the frame there, so boards after the first never got their slot.
- Wire VERSION 2 (release 0.2.0) moved the header to the end. Boards and controllers reject each other's
  formats.
