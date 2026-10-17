# Changelog

Releases of this repository. While below 1.0 they are numbered 0.x.y, and the MINOR number equals the wire
format's VERSION (`ULTRA_VERSION`, header byte 2): 0.2.y = wire VERSION 2. A compatible addition raises the
PATCH number; an incompatible wire change would be 0.3.0 (VERSION 3).

## v0.2.8 (wire VERSION 2), not tagged yet

`ULTRA_VERSION` stays 2. **Local pixel effects** of the QuinLED Diff-Power Ultra-Digital receiver (hardware id 3,
`ULTRA_HWCAP_PIXEL_FX`): the hub says which effect, and the receiver renders the strip itself.

- **PIXEL_FX** (slot type 0x0B, `ULTRA_PIXEL_FX`; LIGHT = the segment 0-3): effect, START (the effect's time zero on the
  host clock), speed, intensity, palette, three colours, brightness, transition, the segment's pixels, flags, two custom
  sliders. `ultra_slot_pixel_fx()` / `ultra_pixel_fx_parse()`. `ULTRA_TYPES` stays 0x0B: the Dig2Analog-Ultra and the
  Ultra-Analog receivers ignore the type as an unknown one. `ultra_slot_config()` passes the hardware-specific ids
  (128-255) now.
- **CONFIG 129-139** (the strip's chip, wire order, pixels, frame rate, LED current and limit, auto-white, virtual
  strip, standalone policy, signal loss timeout) and **page 0x81** (`ULTRA_PAGE_HW_FX`: the state, the effects and
  palettes the firmware has). Spec: the header's comments.
- **The catalogue** (`catalogue/pixel_fx_catalogue.json`, `check_catalogue.py`, `test_check_catalogue.py`): effect and
  palette ids and names, never reused; a host builds its effect list from it.
- **Test vectors:** `slot_pixel_fx`, `slot_pixel_fx_stream`, `slot_config_pixel`, `slot_poll_pixel_fx_effects`.
- **Sender library 0.2.8:** `ultra_sender_pixel_fx()` / `_at()`, `ultra_sender_pixel_fx_start()` and
  `ultra_sender_pixel_config()` (a new effect takes a new START, a change of its parameters keeps it), the START
  stamped like an EFFECT's, the refresh and a resync covering them; `test_pixel`.

## v0.2.7 (wire VERSION 2), not tagged yet

`ULTRA_VERSION` stays 2: the frames to the boards are unchanged. **One break in the return channel's answers**, made
once before production (no receiver has been built yet): IDENTITY and STATUS byte 3, below. A host reading them must
take this release; the names of the changed fields changed with them, so code written for 0.2.6 doesn't build. The rest:
housekeeping, and the QuinLED Diff-Power Ultra-Digital receiver (hardware id 3) and its digital ports.

- **IDENTITY in two parts** (POLL argument 0: `ULTRA_ID_PART_BOARD`, `ULTRA_ID_PART_FEATURES`; 2-255: UNSUPPORTED),
  room for the protocol's life instead of a field that was nearly full (23 of 24 official feature bits):
  - part 0, the board: the UID, the firmware and bootloader versions, the hardware id, **the hardware revision**
    (`ULTRA_ID_HW_REV`, new), **32-bit capabilities** (`ULTRA_ID_CAPS` at 20; 0.2.6: 16 bits at 18);
  - part 1, the firmware's features: **192 feature bits**, six big-endian u32 words (`ULTRA_IDF_*`; word 0 = the
    `ULTRA_FEAT_*` masks, bits 0-22 as before); every bit official, bits 23-191 free. No hardware-specific feature
    bits any more (0.2.6: bits 24-31): a hardware-specific ability goes by the hardware id (its capabilities, pages
    0x80-0xFD, CONFIG 128-255). `ultra_feat_has()` reads any bit.
- **STATUS byte 3 is the hardware id** (`ULTRA_ST_HW_ID`; 0.2.6 `ULTRA_ST_HW`: hardware id << 4 | revision, so 15
  boards at most). Hardware ids are one byte everywhere now (the update descriptor, the bootloader, IDENTITY,
  STATUS): 4-254 for later boards of the series; the revision is in IDENTITY part 0 (16 a board: the update
  descriptor's revision mask).
- **The header's FLAGS nibble is full; the rule for later frame-level features:** none gets a header bit of its own.
  An optional one sets HINT and names itself in a broadcast slot (its own type, 0x80-0xFF, after the last board's
  slot); several share HINT in one frame, one broadcast slot each; a board uses those it knows. A must-understand one
  sets EXT and names itself the same way. Boards that don't know EXT ignore every slot of such a frame, which on a
  line of mixed firmware drops the older boards' commands, so new features go in new slot types and CONFIG ids
  first. The comments on HINT / EXT say so; no wire change.
- **The planned power-off notice's text:** a supply dip in the 10 s after IDENTIFY 7 is the cut's; still powered 2 s
  after it with the supply steady, the outputs come back; it is forgotten after 10 s (the receiver's firmware after
  0.6.10; 0.6.10: 2 s). A comment.
- **Hardware id 3, the QuinLED Diff-Power Ultra-Digital receiver** (IDENTITY 17), and **`ULTRA_HWCAP_PIXEL` 0x40:** a
  pixel delay line on a digital port. Page 0x80 on hardware 3 is PIXEL (hardware-specific: its firmware's wiki).
- **Digital ports** (`ULTRA_DP_*`, `ultra_dp_window_us()`): one receiver per pixel port takes Ultra frames (one
  slot) behind a marker in a stretched latch gap: the line LOW >= 350 us after the last pixel edge, the marker HIGH
  15-40 us, LOW 5-20 us, the frame. An RW frame's window is keyed to the hub's release (by t_end + 150 us, at the
  latest 250 us): the receiver answers once the line has been released 20 us and by t_end + 900 us, the hub listens
  until the answer or t_end + 900 us + the slot pitch, then retakes the line, pixels 20 us later. The hub's rules: no
  poll in the first second after a port's power-up, updates behind markers, and the back-off for a receiver that
  stops answering (3 polls, 3 more after 60 s, then none until the user intervenes: at most 6 one-frame glitches
  from a dead or misconfigured receiver). Wiki: Return channel, digital ports.
- **Compatibility and versioning:** "new slot types and CONFIG ids before EXT", the reserved ranges for 0.2.7, the
  pre-production break.
- **Test vectors:** the 0.2.6 ones unchanged; new: `slot_poll_identity_features`, `answer_identity_board`,
  `answer_identity_features`.
- **The header's comments describe the current state** (round 14): no per-release notes ("0.2.x:", "before 0.2.x",
  the withdrawn format 1); the return channel's comment names both Ultra receivers. Comments only: the code is the
  same token for token, but the file's md5 changed, so every copy must be updated.

## v0.2.6 (wire VERSION 2), 2026-10-15

Backward compatible: `ULTRA_VERSION` stays 2. Every board ignores IDENTIFY 7 before 0.2.6 (a reserved value), and every
QuinLED Dig2Analog-Ultra-5ch ignores it; a board before 0.2.6 answers page 0x0E UNSUPPORTED. Implemented by the QuinLED
Diff-Power Ultra-Analog receiver's firmware 0.6.10 (`BOARD=2`).

- **The planned power-off notice, IDENTIFY 7** (`ULTRA_IDENT_POWER_OFF`, argument 0; `ultra_slot_power_off()`): a host
  that switches a line's power sends it in every board's slot, 3 copies with the same SEQ at 0, 50 and 100 ms, and cuts
  the power at 200 ms. Once it is sent, the power is always cut (no take-back). A receiver fades to dark (0.3 s), stops
  a running calibration, and takes the coming cut as planned: a supply dip starting in the next 2 s shows nowhere, and
  the receiver writes its lifetime record only if there is something worth storing (the regular rules: new energy, a
  trip / watchdog / update, or 10 min powered), so a standby cycle costs no flash write by itself. Still powered 2 s
  after the notice, it forgets it. Nothing is stored for the next start: the host counts the standby cycles, and a
  receiver's power-ups include its standby wakes. Wiki: Host behaviour, "A planned power-off".
- **The receiver's power-up** (firmware 0.6.10, not a protocol rule): dark until the first valid frame or 1.5 s,
  then its power-on scene, so a woken line shows the host's state, not the scene first.
- **Feature bit 21** (`ULTRA_FEAT_STANDBY`): the board takes the notice. Bit 22 is LIFE_SYNC's (below); 0x00800000
  stays free.
- **DIAG 0, byte 20 bit 7** (`ULTRA_DG_FP_EXHAUSTED`): the lifetime record's flash-wear allowance is used up; it is
  written again once a run earns a write (the receiver: one per 3 h powered in one run). A diagnostic, not a problem.
  Bit 3 (`ULTRA_DG_FP_LIFETIME`) also covers a supply dip's write that couldn't be made then (no dark wait).
- **The hub's lifetime counters, page 0x0E LIFE_SYNC** (`ULTRA_PAGE_LIFE_SYNC`, `ULTRA_LS_*`; feature bit 22,
  `ULTRA_FEAT_LIFE_SYNC`): a hub that keeps each board's counters sends them back after the board's power-up; the
  board raises each counter by a plausible step (never lowers one), once per power-up, only when the POLL carries its
  UID16 and its RECORDS' low byte. The answer: the counters after the merge, the energy, a result and the fields
  raised. The board's steps limit what a wrong ledger can do; the ledger's correctness is the hub's. A merge never
  makes the board write; the board's flash-wear allowance counts only its own powered time. Boards before it answer
  UNSUPPORTED. The hub's rules: the ledger is the hub's own accounting in finer units (each session's seconds and
  mWh: STATUS uptime / the port's on time, ENERGY's BOOT_MWH), sent as whole wire units, never an echo of the board's
  floor reads (a board drops each session's part below a unit at the cut: an echo restores nothing of short
  sessions); POWER_UPS = the ledger + the power-on just performed, then the max with the board's read; and the
  board's reads raise the time and energy totals only by a lead beyond one wire unit, at first contact and a session's
  start only (within one, it is the board's own rounding: a merge keeps the board's remainder; a plain max takes it
  back as time at every write). The receiver's merge also stops at each counter's
  room: powered / lit time in 32-bit seconds (11,930,464 units of 0.1 h), the energy in 32-bit Wh; a step past it is
  CAPPED and the counter stays there for good (not a backlog). The dependent steps use the powered step the board
  accepted; power-ups and dips compare modulo 2^24, the trips and the energy plainly. Wiki: Return channel, "0x0E
  LIFE_SYNC".
- **32-bit safe on 16-bit `int` hosts:** `ultra_config_crc()` builds its id mask as `(uint32_t)1u << id` (ids 16-24
  were undefined behaviour where `int` has 16 bits), and `ultra_rw_slot_us()` shifts a `uint32_t`. Results are
  unchanged on 32-bit hosts.
- **Test vectors:** `slot_identify_power_off`, `frame_power_off`, and a check of `ultra_config_crc()` over ids 23 and
  24. The 0.2.5 vectors are unchanged.

## v0.2.5 (wire VERSION 2), 2026-10-14

Backward compatible: `ULTRA_VERSION` stays 2. A board from before 0.2.5 skips CONFIG id 24, ignores signal loss
value 3 (out of its range: its own setting stays) and answers the new pages UNSUPPORTED (0xFF). Implemented by the
Dig2Analog-Ultra firmware 0.6.9 (the CONFIG additions) and the QuinLED Diff-Power Ultra-Analog receiver's (`BOARD=2`: the
pages too).

- **CONFIG 24, the trips' retry policy** (`ULTRA_CFG_RETRY`, persisted like 22 and 23): count | interval << 8. Count
  0 = never retry, 1-254 retries, 255 = forever; interval 1-255 s (0 = 1 s). Default 0x010A: 10 retries 1 s apart.
  After the last retry the outputs stay off until a new command or frame value, or a power cycle; 60 s without a
  trip starts the count again. `ULTRA_CFG_COUNT` is 25.
- **Signal loss value 3, "scene"** (CONFIG 8): when the controller stops, the board fades to the light's stored
  power-on scene / effect; a light without one holds. The controller's next frames take over as after power-up.
- **LIFETIME page** (0x0C, `ULTRA_LT_*`): the board's odometer from its lifetime record: time powered and lit (0.1 h),
  power-ups, overload and short trips, supply dips, the MCU temperature and input current peaks, watchdog resets,
  firmware updates, the record's writes.
- **DIAG page** (0x0D, `ULTRA_DG_*`), two parts (POLL argument 0):
  - part 0, the running board: the reset cause, the last run's breadcrumbs (the exception's cause, its address, that
    run's uptime), the CPU load and its recent peak, the overruns, the supply / cable impedance, the flash (failures in
    a row, the records waiting, why they wait: `ULTRA_DG_FP_*`, `ULTRA_DG_SW_*`) and the input capacitor's ripple load
    (0xFF: not measured);
  - part 1, the fault memory (kept in the lifetime record across power cuts): the last trip (cause, current, the lit
    hours then), the last supply dip (depth, length, current), the last abnormal reset (watchdog or exception, that
    run's uptime); and the calibration trend (per channel, % since the first stored test; `ULTRA_HE_NA` / 0xFFFF / 0
    when not known);
  - parts 2-255 answer UNSUPPORTED.
- **CALIBRATION byte 1** (`ULTRA_CA_MODEL`): its high nibble says how the last calibration test ended
  (`ULTRA_CA_END_*`: complete, or stopped by a trip, the installation's limit or a supply dip; a stopped test stores
  nothing).
- **STATUS byte 8, bits 4-7: why the light isn't as set** (`ULTRA_ST_WHY_*`, the first that applies): off after a
  short / an overload, off with the retries used up, the test jig, a calibration test, too hot, a supply dip in the
  last 30 s, signal loss, the soft start, a dark hold, a blink pattern; 0 = as set. The limits stay in bits 0 and 3.
  Polled with STATUS (every 2-5 s), it gives a host its status sensor without another page.
- **ENERGY bytes 15-18:** the LED supply after a Diff-Power receiver's buck (`ULTRA_EN_VB`, × 10 mV) and the input
  current's highest ~100 ms average since the page was last answered (`ULTRA_EN_PEAK_MA`: the PTC-threatening
  peaks between polls).
- **HEALTH flags:** 0x10 `ULTRA_HE_F_CAL_SUGGESTED` named (sent since firmware 0.6.6), 0x20 `ULTRA_HE_F_HOT` (the MCU
  die over its warning level; firmware 0.6.9: 95 °C, until 85 °C).
- **Which side:** on the Diff-Power Ultra-Analog receiver every current and power is the bus side, before its buck (STATUS
  17, POWER, ENERGY, the channel model's shares on OUTPUTS); its limits are the link's (2.25 A, 3 × 750 mA PTCs per
  port). The LED side: ENERGY's V_B only (wiki: Return channel).
- **Features** (IDENTITY): `ULTRA_FEAT_LIFETIME`, `_DIAG`, `_RETRY`, `_LOSS_SCENE`; 0x00200000-0x00800000 stay free.
- Page ids 0x0E-0x7F stay reserved for future official pages.
- **Test vectors:** new `slot_config_retry_loss`, `slot_poll_lifetime`, `slot_poll_diag_faults`. Every other vector
  is unchanged.
- **New: the reference sender library** (`sender/`, MIT, version 0.2.5): C99 with a C++ wrapper, ESP32 RMT examples
  (ESP-IDF and Arduino), PlatformIO / Arduino / ESP-IDF component manifests, and its own tests against the vectors and
  the board firmware. It is meant for projects such as WLED, Tasmota or MoonLight that want to drive Ultra boards
  (wiki: Sender library).

## v0.2.4 (wire VERSION 2), 2026-10-14

Backward compatible: `ULTRA_VERSION` stays 2. Nothing changes for a board without a return channel (every
Dig2Analog-Ultra) or for a controller that never sets RW. **The return channel is defined:** a controller can now be
written from the spec alone. It is implemented by the QuinLED Diff-Power Ultra-Analog receiver's firmware (0.6.8, `BOARD=2`).

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
Diff-Power Ultra-Analog receivers will implement.

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
Diff-Power Ultra-Analog receivers will implement. No frame a controller sends today changes.

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
