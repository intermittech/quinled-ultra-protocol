# Changelog

Releases of this repository. While below 1.0 they are numbered 0.x.y, and the MINOR number equals the wire
format's VERSION (`ULTRA_VERSION`, header byte 2): 0.2.y = wire VERSION 2. A compatible addition raises the
PATCH number; an incompatible wire change would be 0.3.0 (VERSION 3).

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
