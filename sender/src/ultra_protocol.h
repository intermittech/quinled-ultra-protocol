/* SPDX-License-Identifier: MIT
 * Copyright (C) 2026 QuinLED
 *
 * QuinLED Ultra protocol 0.2 (wire VERSION 2), this file: release 0.2.8 (CHANGELOG.md). Shared by the
 * board firmware (C) and the controllers (C / C++).
 * Master: include/ultra_protocol.h in https://github.com/intermittech/quinled-ultra-protocol (spec in its wiki).
 * Every copy (board firmware, ESPHome component, other controllers) must be byte-identical to one tagged release
 * (v0.2.y = wire VERSION 2; while below 1.0, the release's MINOR equals the wire VERSION).
 *
 * Link: WS28xx-style one-wire pulses (same timing as WS2915), MSB first, one way host -> boards.
 * Frame:  N x SLOT (32 bytes, one per board)  +  HEADER (8 bytes, a trailer)  +  reset >= 280 us
 *         Each board takes the first slot of what it sees (its hardware gate holds the output LOW for those 256
 *         bits, as in WS2805 mode) and forwards the rest, so the position in the chain is the address and every
 *         board sees one contiguous frame ending with the header. Frame bits = 256 * N + 64.
 *         (Why last: with the header first, each board's closed gate would leave a 320 us LOW gap after the header,
 *         ending the frame at the next board, so the boards after the first would never get their slot.)
 * HEADER: MAGIC (2) | VERSION<<4 | FLAGS (1) | TIME (4) | CRC-8 (1, SMBus poly 0x07)
 *         TIME = host clock (us, low 32 bits) at the end of the frame (the header's last bit); every board sees
 *         that moment within gate delays. FLAGS: ULTRA_HDR_* below.
 * SLOT:   TYPE (1) | LIGHT<<4 | FLAGS (1) | SEQ (1) | PAYLOAD (27) | CRC-16 (2, CCITT-FALSE)
 *         All multi-byte fields big-endian. Values: 16-bit fractions 0..65535 = 0..1.
 * Each command carries a sequence number per (light, type): the board applies a command once, so the host can
 * repeat it (redundancy) and re-send the current state periodically (keepalive / catch-up after a reset).
 * Preamble: before its first commands (start-up, resync after power-on) the host sends ultra_preamble_frames(N)
 *         frames of DARK slots (all zero, never valid) + header. Every board locks to Ultra on the headers, one
 *         after the other, also a board still locked to WS2805 / WS2915 (it shows its WS window of those zeros:
 *         dark, never a colour). Idle frames afterwards use NOP slots.
 *
 * Compatibility rules (the wiki: Compatibility and versioning): a board ignores what it doesn't know, so a feature
 * it lacks never disturbs it; the host sends only values defined here (the builders below never set
 * anything else).
 *  - Unknown slot TYPE (0x0C-0xFF, and ULTRA_PIXEL_FX 0x0B on every board but the QuinLED Diff-Power Ultra-Digital
 *    receiver), LIGHT 5-14, IDENTIFY value or argument, SCENE mode: the slot is ignored.
 *  - Unknown CONFIG id: that pair is skipped, the others apply. Unknown effect id: ULTRA_FX_NONE (the light's plain
 *    state). Unknown role: the channel is not part of the light.
 *  - Unknown flag bits (header HINT, slot FLAGS, SET / LIGHT_DEF / LIGHT_OPT flags), reserved bits and spare payload
 *    bytes: ignored. The host sends them as 0, so a future field's value 0 must always mean the old behaviour.
 *  - Header EXT ("must understand"): a board without the frame's extension keeps its lock and clock sync but ignores
 *    every slot of that frame, as for RW. On a line of mixed firmware that drops those boards' commands for that
 *    frame, so new features go in new slot types and CONFIG ids (ignored where unknown) before EXT; a frame-level
 *    feature only behind EXT (or RW / UPD) when boards that don't know it can't safely process the slots. How later
 *    frame-level features share the full FLAGS nibble: ULTRA_HDR_* below.
 *  - ULTRA_VERSION changes only for incompatible changes: a board rejects every frame of another version.
 *  - Broadcast / extension slots (reserved): k slots after the last board's slot, types >= ULTRA_TYPE_BCAST only,
 *    frame bits 256 x (N + k) + 64. Every board forwards everything after its own slot, so they reach every board;
 *    boards of this firmware never read them, and a board never applies one as its own slot (e.g. when the hub's
 *    board count is wrong). During the preamble they are DARK like the board slots.
 */
#ifndef ULTRA_PROTOCOL_H
#define ULTRA_PROTOCOL_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define ULTRA_MAGIC        0x5AD2u
#define ULTRA_VERSION      2u          /* the wire format (releases 0.2.y). Only an incompatible change bumps
                                          it (release 0.3.0 for 3): a board rejects other versions entirely
                                          (no lock, no clock, no slot) */
#define ULTRA_HDR_BYTES    8u
#define ULTRA_SLOT_BYTES   32u
#define ULTRA_HDR_BITS     (ULTRA_HDR_BYTES * 8u)
#define ULTRA_SLOT_BITS    (ULTRA_SLOT_BYTES * 8u)
#define ULTRA_PAYLOAD      3u          /* payload offset in a slot */
#define ULTRA_PAYLOAD_LEN  27u
#define ULTRA_LIGHTS       5u          /* lights per board (5 channels); LIGHT 5-14 reserved (groups / virtual lights) */
#define ULTRA_LIGHT_BOARD  0x0Fu       /* LIGHT field for board-level commands */
#define ULTRA_MAX_STEPS    8u          /* strobe / sequence steps per light */

/* header FLAGS (low nibble of byte 2). On an RW or EXT frame a board keeps its lock and clock sync; a board without a
 * return channel (every QuinLED Dig2Analog-Ultra-5ch) ignores every slot of it, and a QuinLED Diff-Power Ultra
 * receiver answers its POLL of an RW frame without EXT (the return channel, below). An UPD frame a board leaves
 * to the update side (no clock sync, no slot), HINT it ignores. RW and UPD are never set together; a board ignores
 * such a frame. The hub sets none of them in normal frames (0x00).
 * The nibble is full: a later frame-level feature never gets a header bit of its own. An optional one sets
 * HINT and names itself in a broadcast slot (its own TYPE, 0x80-0xFF, after the last board's slot; the payload carries
 * it); several share HINT in one frame, one broadcast slot each, and a board uses those it knows and ignores the rest
 * (HINT with no hint named: nothing). A must-understand one sets EXT and names itself the same way (EXT's cost: the
 * rules above). The broadcast slot types are assigned here, as they come. */
#define ULTRA_HDR_RW       0x01u       /* a response window follows this frame (wiki: Return channel) */
#define ULTRA_HDR_HINT     0x02u       /* optional: the frame's broadcast slots name its hints (none defined yet); a
                                          board that knows none of them processes the frame as usual */
#define ULTRA_HDR_EXT      0x04u       /* must understand: the frame's broadcast slots name its extensions (none defined
                                          yet); a board without one of them ignores every slot */
#define ULTRA_HDR_UPD      0x08u       /* firmware update session (wiki: Reserved extensions) */

/* slot types */
enum {
    ULTRA_NOP = 0x00,          /* keepalive, nothing to do */
    ULTRA_LIGHT_DEF = 0x01,    /* light: channels, roles, gamma, min / max power */
    ULTRA_SET = 0x02,          /* light: target state + transition */
    ULTRA_EFFECT = 0x03,       /* light: start / stop a board effect */
    ULTRA_STEP = 0x04,         /* light: one strobe step (SEQ = the EFFECT's SEQ) */
    ULTRA_CONFIG = 0x05,       /* board: up to 9 settings (id, value) */
    ULTRA_IDENTIFY = 0x06,     /* board: blink pattern */
    ULTRA_POLL = 0x07,         /* answer / relay in a response window (RW frames only: the return channel) */
    ULTRA_SCENE = 0x08,        /* light: power-on scene (persisted on the board) */
    ULTRA_LIGHT_OPT = 0x09,    /* light: options (dim to warm) */
    ULTRA_SCENE_EFFECT = 0x0A, /* light: power-on effect on the SCENE values (persisted on the board) */
    ULTRA_PIXEL_FX = 0x0B,     /* segment: a local pixel effect (the Ultra-Digital receiver, ULTRA_HWCAP_PIXEL_FX; below).
                                  Every other board ignores it as an unknown type */
    ULTRA_TYPES = 0x0B,        /* the light engine's types are 0 .. ULTRA_TYPES - 1 (the Ultra-Analog receivers and the
                                  Dig2Analog-Ultra-5ch size their per-type state by it and ignore the types from
                                  here on); 0x0C-0x7F: future per-board commands (ignored) */
    ULTRA_TYPE_BCAST = 0x80,   /* 0x80-0xFF: broadcast / extension slots only (after the last board's slot), never
                                  applied as a board's own slot. (The UPD frame's block types 0x81-0x85 are another
                                  namespace: ultra_update.h) */
};

/* Return channel (the QuinLED Diff-Power Ultra-Analog and Ultra-Digital receivers only; wiki:
 * Return channel). All times in us after t0 = the last falling edge of the RW frame as each board measures it locally
 * (the last edge the tail copy finds in the DIN ring, not the shared clock's phase), in host us (a board scales them
 * with the shared clock's measured rate). During the window the forwarding gate is held CLOSED (DOUT LOW, not high-Z):
 * DIN sees the relayed answers, and a re-armed plan would forward them downstream. Host: no RW frame before the chain
 * is locked (after the DARK preamble) and has sent headers for ULTRA_RW_LOCK_MS, none during an update session, never
 * with UPD, and never with EXT (until an extension defines it). In one RW frame every POLL carries the same R and rate;
 * the answer indices are unique, < R and rise along the chain, and the boards between the hub and an answering board
 * get ULTRA_POLL_RELAY_ONLY. Behind the last board that answers or relays, the slots may be of any type (those boards
 * don't take part and don't turn around). The next frame >= W + ULTRA_RW_HOLDOFF_US after t0, answers or not; a missing
 * answer (a lost POLL, a flash write just before the frame, a late main loop, a board's clock not rated yet) is retried
 * in a later window with a new SEQ. UID16 in the answers shows index shifts. Board: takes part when its own slot is a
 * valid POLL (ultra_poll_valid) and the header has RW and not EXT. It turns around in DIR_MIN..DIR_MAX, does the
 * turnaround check at CHECK_US (the line HIGH for <= CHECK_HIGH_US, read back: LOW = the segment above is still driven,
 * a board there doesn't take part), sends its burst in its slot with the same read-back after the line goes HIGH, and
 * is in forward mode again W .. W + ULTRA_RW_END_US. A failed read-back (at the check or at the burst): no answer and
 * no relay for the rest of this window, CONTENTION in its next answer. A valid POLL is always answered, a repeated SEQ
 * too. Burst (board i, s_i = FIRST + i x pitch): the line HIGH at s_i - EARLY .. s_i + LATE (slot 0: from s_0 -
 * EARLY0), the first start bit START_MIN .. START_MAX after it, then ULTRA_RW_LEAD_BYTES x ULTRA_RSP_LEAD and the
 * 32-byte answer, UART 8N1, LSB first, at the POLL's rate; the line LOW again (idle, a break at the hub's UART) by s_i
 * + pitch - GUARD. Hub: the turnaround checks' pulses reach it as junk UART bytes (CHECK_US .. ~1000 us); it discards
 * every byte before a SYNC without a framing error (any number of lead bytes), takes 32 bytes, and drops the answer on
 * a framing error or a CRC failure (ultra_answer_check). */
#define ULTRA_POLL_RELAY_ONLY 0xFFu    /* POLL index: relay, don't answer (the page and its arguments are ignored) */
#define ULTRA_RW_MAX_SLOTS    16u      /* INDEX is a nibble in the answer */
#define ULTRA_RW_RELEASE_US   150u     /* host / sender has stopped driving */
#define ULTRA_RW_DIR_MIN_US   250u     /* boards turn around between MIN and MAX */
#define ULTRA_RW_DIR_MAX_US   900u
#define ULTRA_RW_CHECK_US     920u     /* the turnaround check, 920-977 us (after DIR_MAX, before slot 0) */
#define ULTRA_RW_CHECK_HIGH_US 5u      /* ... the line HIGH at most this long */
#define ULTRA_RW_FIRST_US     1000u    /* response slot 0 */
#define ULTRA_RW_SLOT_US      460u     /* response slot pitch at 1 Mbaud (ultra_rw_slot_us: every rate) */
#define ULTRA_RW_EARLY_US     20u      /* a burst's line HIGH within s_i - EARLY .. s_i + LATE */
#define ULTRA_RW_EARLY0_US    5u       /* ... slot 0's from s_0 - EARLY0: its lead byte after the check pulses' junk
                                          bytes (with up to 15 us more hop delay to the hub than slot 0's) */
#define ULTRA_RW_LATE_US      80u
#define ULTRA_RW_START_MIN_US 2u       /* the first start bit (the lead byte's) START_MIN .. START_MAX after the line goes
                                          HIGH */
#define ULTRA_RW_START_MAX_US 10u
#define ULTRA_RW_GUARD_US     30u      /* the line LOW again by s_i + pitch - GUARD, the lead byte included (430 us at
                                          1 Mbaud) */
#define ULTRA_RW_END_US       80u      /* forward mode again W .. W + END (W = FIRST + R x pitch); a 50 us tick with
                                          +-200 ppm and the ISR latency needs more than 60 */
#define ULTRA_RW_HOLDOFF_US   200u     /* host: next frame >= window end + this */
#define ULTRA_RW_LOCK_MS      1000u    /* host: the first RW frame this long after the chain's preamble */
#define ULTRA_RW_BAUD         1000000u /* UART 8N1, LSB first, line idle LOW between bursts (rate 0) */
#define ULTRA_RW_LEAD_BYTES   1u       /* a board sends exactly this many ULTRA_RSP_LEAD before SYNC */
#define ULTRA_RW_SYNC_IDLE_BITS 9u     /* SYNC's start bit follows >= this many bit times of HIGH (the lead byte's data
                                          and stop bits): the hub's UART is idle at SYNC, whatever it made of the
                                          check pulses and the short HIGH before the lead byte */
/* Digital ports (wiki: Return channel, digital ports): a receiver on a pixel line (the QuinLED Diff-Power
 * Ultra-Digital receiver, ULTRA_HWCAP_PIXEL; one per port, so every frame holds N = 1 slot) takes Ultra frames behind a
 * marker in a latch gap the hub stretches; the strip never sees them. Before every marker frame (RW, IDENTIFY, CONFIG,
 * UPD) the line LOW >= ULTRA_DP_QUIET_US after the last pixel edge, the marker HIGH MARK_MIN..MARK_MAX, LOW
 * GAP_MIN..GAP_MAX, then the frame. After a frame without RW: LOW >= ULTRA_DP_AFTER_US before pixels. An RW frame
 * (t_end = its last falling edge): the hub releases the line by t_end + ULTRA_RW_RELEASE_US (a release starting after
 * t_end + ULTRA_DP_RELEASE_MAX_US is taken as the end of a frame without RW: no answer); the receiver answers once it
 * has seen the line released >= ULTRA_DP_RELEASED_US and by t_end + ULTRA_RW_DIR_MAX_US (the line HIGH, the lead byte
 * and the 32-byte answer at the POLL's rate; no turnaround check: the release is the check, RFLAGS CONTENTION never
 * set); the hub listens until SYNC + 31 bytes or t_end + ultra_dp_window_us(rate), then retakes the line (LOW), pixels
 * >= ULTRA_DP_RETAKE_US later. Never pixels during a window. The hub: no marker for ULTRA_DP_POWERUP_MS after powering
 * a port or starting its stream; an update's frames each behind a marker, the line LOW >= ULTRA_DP_UPD_LOW_MS after
 * the ANNOUNCE's window, no pixels until the session ends; the back-off: a receiver that stops answering gets
 * ULTRA_DP_TRIES polls in quick succession, ULTRA_DP_TRIES more ULTRA_DP_RETRY_S later, then none until the user
 * intervenes (at most 2 x TRIES one-frame glitches on the first pixels from a dead or misconfigured receiver) */
#define ULTRA_DP_QUIET_US       350u   /* the LOW before a marker (a receiver still direct switches to delayed in it) */
#define ULTRA_DP_MARK_MIN_US    15u    /* the marker's HIGH (hub: 20 us); pixels never hold a HIGH this long */
#define ULTRA_DP_MARK_MAX_US    40u
#define ULTRA_DP_GAP_MIN_US     5u     /* the LOW between the marker and the frame (hub: 10 us) */
#define ULTRA_DP_GAP_MAX_US     20u
#define ULTRA_DP_AFTER_US       300u   /* the LOW after a frame without RW, before pixels */
#define ULTRA_DP_RELEASE_MAX_US 250u   /* RW: the hub's release starts by t_end + this (aimed: ULTRA_RW_RELEASE_US) */
#define ULTRA_DP_RELEASED_US    20u    /* RW: the receiver drives only after the line was released this long */
#define ULTRA_DP_RETAKE_US      20u    /* pixels this long after the hub retakes the line */
#define ULTRA_DP_POWERUP_MS     1000u  /* no marker this long after a port's power-up or the start of its stream */
#define ULTRA_DP_UPD_LOW_MS     50u    /* an update: the line LOW this long after the ANNOUNCE's window */
#define ULTRA_DP_TRIES          3u     /* the back-off: polls in quick succession, the same after RETRY_S, then none */
#define ULTRA_DP_RETRY_S        60u
/* POLL slot (type ULTRA_POLL, LIGHT 0xF; the payload's offsets, as slot bytes) */
#define ULTRA_POLL_INDEX      3u       /* answer index 0..R-1, or ULTRA_POLL_RELAY_ONLY */
#define ULTRA_POLL_SLOTS      4u       /* R: response slots in this window, 1..ULTRA_RW_MAX_SLOTS */
#define ULTRA_POLL_PAGE       5u       /* ULTRA_PAGE_* */
#define ULTRA_POLL_RATE       6u       /* ULTRA_RW_RATE_* */
#define ULTRA_POLL_ARG        7u       /* page arguments, slot bytes 7..29 ("POLL argument 0" = slot byte 7) */
#define ULTRA_RW_RATE_1M      0u       /* 1 Mbaud: pitch 460 us */
#define ULTRA_RW_RATE_500K    1u       /* 500 kbaud: 790 us (segments over 50 m, longer chains) */
#define ULTRA_RW_RATE_250K    2u       /* 250 kbaud: 1450 us (beyond 100 m) */
#define ULTRA_RW_RATES        3u       /* 3-255 reserved: not a valid POLL (the board doesn't take part) */
/* response slot: SYNC | PAGE | INDEX<<4 | RFLAGS | SEQ | UID16 (2) | DATA (24) | CRC-16 (2), after the lead byte.
 * UID16: CRC-16 (ultra_crc16) over the 12 bytes of the MCU's unique id in address order (IDENTITY part 0, 0..11); a
 * fingerprint, not an identity (the host reads IDENTITY for that). Unknown RFLAGS bits are ignored. */
#define ULTRA_RSP_LEAD        0xFFu    /* the lead byte: one falling edge, its start bit */
#define ULTRA_RSP_SYNC        0xA5u
#define ULTRA_RSP_DATA        6u
#define ULTRA_RSP_DATA_LEN    24u
#define ULTRA_RSP_EVENT       0x01u    /* a POWER latch is set (ULTRA_PW_LATCHED) */
#define ULTRA_RSP_STALE       0x02u    /* reserved, not sent: every answer is built when its POLL's frame ends (the
                                          pages' own ages say how old an event is) */
#define ULTRA_RSP_CONTENTION  0x04u    /* a turnaround check or a burst found the line taken since the last answer that
                                          went out (that window: no relay, no answer) */
enum {
    ULTRA_PAGE_STATUS = 0x00, ULTRA_PAGE_OUTPUTS = 0x01, ULTRA_PAGE_ACKS = 0x02, ULTRA_PAGE_ERRORS = 0x03,
    ULTRA_PAGE_IDENTITY = 0x04,        /* two parts (POLL argument 0: 0 = the board, 1 = its features; 2-255:
                                          UNSUPPORTED) (ULTRA_ID_*) */
    ULTRA_PAGE_FW_UPDATE = 0x05,
    ULTRA_PAGE_CALIBRATION = 0x06,     /* current sense: the model, connected load and limit in effect */
    ULTRA_PAGE_CAL_TEST = 0x07,        /* the stored test, in two parts (POLL argument 0: 0 = CH1-3, 1 = CH4-5 + rest;
                                          2-255: UNSUPPORTED) */
    ULTRA_PAGE_POWER = 0x08,           /* board current limit and overcurrent events (ULTRA_PW_*) */
    ULTRA_PAGE_SUPPLY = 0x09,          /* supply dips (ULTRA_SU_*) */
    ULTRA_PAGE_HEALTH = 0x0A,          /* channel health and the MCU temperature peak (ULTRA_HE_*) */
    ULTRA_PAGE_ENERGY = 0x0B,          /* input energy (ULTRA_EN_*) */
    ULTRA_PAGE_LIFETIME = 0x0C,        /* the board's lifetime record, its odometer (ULTRA_LT_*) */
    ULTRA_PAGE_DIAG = 0x0D,            /* diagnostics in two parts (POLL argument 0: 0 = the running board, 1 =
                                          the fault memory and the calibration trend; 2-255: UNSUPPORTED) (ULTRA_DG_*) */
    ULTRA_PAGE_LIFE_SYNC = 0x0E,       /* the hub's lifetime counters for the board to merge (ULTRA_LS_*; the
                                          answer: the counters after it and the result) (ULTRA_FEAT_LIFE_SYNC) */
    ULTRA_PAGE_HW = 0x80,              /* 0x0F-0x7F: future official pages; 0x80-0xFD: hardware-specific, by hardware id
                                          (hardware 3, the Ultra-Digital receiver: 0x80 PIXEL, its pixel statistics, and
                                          0x81 PIXEL_FX, its local effects (ULTRA_PXG_* below); its firmware's wiki) */
    ULTRA_PAGE_HW_FX = 0x81,           /* hardware 3: PIXEL_FX, in three parts (POLL argument 0: 0 = state, 1 = the
                                          effects it has, 2 = its palettes; 3-255: UNSUPPORTED) (ULTRA_PXG_*) */
    ULTRA_PAGE_BUSY = 0xFE, ULTRA_PAGE_UNSUPPORTED = 0xFF,
};
/* IDENTITY page DATA, two parts (POLL argument 0 = ULTRA_ID_PART_*, 2-255: UNSUPPORTED). A host reads both at
 * the start and whenever UID16 changes.
 * Part 0, the board: MCU UID (12) | firmware version word (4, ultra_update.h; 0 = unknown) | bootloader version (1,
 * 0 = none) | hardware id (1) | hardware revision (1) | reserved (1, 0) | hardware capabilities (4, ULTRA_HWCAP_*).
 * Part 1, the firmware's features: 192 feature bits in six u32 words (big-endian each, ULTRA_IDF_*): word w holds
 * bits 32 w .. 32 w + 31, bit 32 w its least significant (word 0: the ULTRA_FEAT_* masks; ultra_feat_has). Every bit
 * is official, assigned here in order; a hardware-specific ability goes by the hardware id (its capabilities, pages
 * 0x80-0xFD, CONFIG 128-255), never by a feature bit.
 * Only over the return channel: for a board without one (every
 * Dig2Analog-Ultra-5ch) the host can't know its features and relies on the compatibility rules above (an unknown
 * command does nothing, an unknown effect shows the plain state). */
#define ULTRA_ID_PART_BOARD       0u     /* POLL argument 0 */
#define ULTRA_ID_PART_FEATURES    1u
#define ULTRA_ID_UID              0u     /* part 0 */
#define ULTRA_ID_FW_VERSION       12u
#define ULTRA_ID_BL_VERSION       16u
#define ULTRA_ID_HW_ID            17u    /* 1 Dig2Analog-Ultra-5ch (never answers), 2 Diff-Power Ultra-Analog
                                            receiver, 3 Diff-Power Ultra-Digital receiver; 4-254 later boards
                                            (ultra_update.h UPD_HW_ID_*, assigned there), 0 / 255 never */
#define ULTRA_ID_HW_REV           18u    /* the board's revision: the bit number in a firmware image's revision
                                            mask (ultra_update.h UPD_D_HW_REV_MASK: 16 revisions a board) */
#define ULTRA_ID_CAPS             20u    /* u32, ULTRA_HWCAP_* */
#define ULTRA_IDF_FEATURES        0u     /* part 1: the six words */
#define ULTRA_IDF_WORDS           6u
#define ULTRA_IDF_BITS            192u
#define ULTRA_HWCAP_RETURN        0x00000001u  /* answers POLLs */
#define ULTRA_HWCAP_RELAY         0x00000002u  /* relays the answers of the boards behind it */
#define ULTRA_HWCAP_VBUS          0x00000004u  /* measures its supply / bus voltage (STATUS 13) */
#define ULTRA_HWCAP_CURRENT       0x00000008u  /* measures its input current (STATUS 17) */
#define ULTRA_HWCAP_NTC           0x00000010u  /* a board temperature sensor (STATUS 11) */
#define ULTRA_HWCAP_PIXEL         0x00000040u  /* a pixel delay line on a digital port (the Ultra-Digital
                                                  receiver): polled behind a marker (ULTRA_DP_* above), one per port */
#define ULTRA_HWCAP_PIXEL_FX      0x00000080u  /* runs local pixel effects (the Ultra-Digital receiver): PIXEL_FX,
                                                  CONFIG 129-139, page 0x81 (ULTRA_PX_* below) */
/*      0x00000020                reserved
 *      0x00000100-0x80000000     reserved: later official capabilities */
/* STATUS page DATA (wiki: Return channel), the fields the firmware measures already (console `status`):
 * the MCU (die) temperature from the CH32V203's internal sensor, int16 0.1 C, ULTRA_ST_NA = not valid (no good
 * conversion yet, or 5 bad ones in a row; core/mcutemp.h), and the 3.3 V rail from VREFINT, mV (0 = not known).
 * The temperature's absolute offset varies per chip (up to ~+-20 C): a trend / over-temperature value. */
#define ULTRA_ST_MCU_TEMP         9u
#define ULTRA_ST_VDDA             15u
#define ULTRA_ST_NA               0x8000u
/* the rest of STATUS */
#define ULTRA_ST_FW_VERSION       0u     /* u24: major, minor, patch (0 = a development build) */
#define ULTRA_ST_HW_ID            3u     /* u8: the hardware id, as IDENTITY 17 (its revision: IDENTITY part 0) */
#define ULTRA_ST_UPTIME           4u     /* u24: s since power-up (wraps after 194 days: compare modulo 2^24; after a
                                            restart it starts at 0 again, RESET has the cause) */
#define ULTRA_ST_RESET            7u     /* u8: the reset cause of this start, ULTRA_ST_RST_* (several may be set) */
#define ULTRA_ST_TEMP_BOARD       11u    /* int16: a board sensor, 0.1 C; ULTRA_ST_NA = none (ULTRA_HWCAP_NTC) */
#define ULTRA_ST_VBUS             13u    /* u16: the supply / bus voltage, x 10 mV; 0xFFFF = not measured */
#define ULTRA_ST_CURRENT          17u    /* u16: the measured input current (~100 ms average), mA */
#define ULTRA_ST_MEASURED         19u    /* u8: ULTRA_ST_M_* */
#define ULTRA_ST_PWM_PRESET       20u    /* u8: the PWM preset in use (CONFIG 1), 0xFF = a raw period (CONFIG 2) */
#define ULTRA_ST_CONFIG_CRC       21u    /* u16: ultra_crc16 over (id, value u16) of every CONFIG id the host set, in id
                                            order (ultra_config_crc: the host compares it with what it sent) */
#define ULTRA_ST_MODES            23u    /* u8: the protocol mode (CONFIG 22's values) << 4 | the stagger (CONFIG 7) */
#define ULTRA_ST_RST_PIN          0x01u  /* (the CH32V203's RSTSCKR bits 26-31) */
#define ULTRA_ST_RST_POWER        0x02u
#define ULTRA_ST_RST_SOFTWARE     0x04u  /* also a firmware update's restart */
#define ULTRA_ST_RST_WATCHDOG     0x08u
#define ULTRA_ST_RST_WWDG         0x10u
#define ULTRA_ST_RST_LOW_POWER    0x20u
#define ULTRA_ST_M_VBUS           0x01u  /* STATUS 13 is measured */
#define ULTRA_ST_M_CURRENT        0x02u  /* STATUS 17 is measured (the current sense in use) */
#define ULTRA_ST_M_NTC            0x04u  /* STATUS 11 is measured */
#define ULTRA_ST_M_CALIBRATED     0x08u  /* the channel currents (OUTPUTS 10..19) come from a calibrated model */
/* OUTPUTS page DATA (optional: a board may answer ULTRA_PAGE_UNSUPPORTED) */
#define ULTRA_OU_LEVEL            0u     /* 5 x u16: CH1..CH5 output now, after every limit (what the PWM shows) */
#define ULTRA_OU_CURRENT          10u    /* 5 x u16: CH1..CH5 current from the channel model at that output, mA */
#define ULTRA_OU_EFFECTS          20u    /* u8: lights with an effect running (bit n = light n) */
#define ULTRA_OU_DEFINED          21u    /* u8: lights with a LIGHT_DEF since power-up */
#define ULTRA_OU_SCENE            22u    /* u8: channels still showing the power-on scene / effect (bit n = CH n+1) */
#define ULTRA_OU_WHY              23u    /* u8: why the outputs differ from the lights' states now, ULTRA_OU_W_* */
#define ULTRA_OU_W_LIMIT          0x01u  /* the board limit reduces them */
#define ULTRA_OU_W_TRIPPED        0x02u  /* off after a trip */
#define ULTRA_OU_W_CH_LIMIT       0x04u  /* a channel limit (CONFIG 16-20) */
#define ULTRA_OU_W_DARK_HOLD      0x08u  /* IDENTIFY 3 */
#define ULTRA_OU_W_PATTERN        0x10u  /* an identify / acknowledge / version pattern */
#define ULTRA_OU_W_SOFT_START     0x20u  /* the soft start ramps them */
#define ULTRA_OU_W_SIGNAL_LOSS    0x40u  /* signal loss (CONFIG 8) */
#define ULTRA_OU_W_TEST           0x80u  /* a current calibration test or the test jig's pattern */
/* ACKS page (optional): the last SEQ taken per target and slot type. POLL arguments (slot bytes 7, 8, 9): three
 * slot types for the three columns (0 = LIGHT_DEF, SET, EFFECT respectively). */
#define ULTRA_AK_SEQ              0u     /* 18 x u8: targets lights 0..4 then the board (LIGHT 0xF), 3 columns each:
                                            byte 3 x target + column */
#define ULTRA_AK_NEVER            18u    /* u24: bit (3 x target + column) = none since power-up (its byte reads 0) */
#define ULTRA_AK_CONFIG_CRC       21u    /* u16: as ULTRA_ST_CONFIG_CRC */
/* ERRORS page (optional): u16 counters since power-up (wrap; the host takes differences) */
#define ULTRA_ER_FRAMES           0u     /* frame ends seen */
#define ULTRA_ER_HEADERS          2u     /* Ultra headers decoded */
#define ULTRA_ER_TAIL_ERRORS      4u     /* frames of an Ultra length without a valid header */
#define ULTRA_ER_DECODED          6u     /* own slots decoded */
#define ULTRA_ER_DECODE_ERRORS    8u     /* captures that failed to decode */
#define ULTRA_ER_WINDOWS          10u    /* response windows taken part in (before this one) */
#define ULTRA_ER_ANSWERS          12u    /* answers sent (before this one) */
#define ULTRA_ER_MISSED           14u    /* POLLs decoded but no part taken (clock not rated, too late) */
#define ULTRA_ER_ABORTED          16u    /* windows given up for contention (check or burst) */
#define ULTRA_ER_FLASH_WRITES     18u    /* flash records written */
#define ULTRA_ER_STACK            20u    /* the stack's high-water mark, bytes */
/*      22-23                     reserved */
/* STATUS byte 8: the board's power state now, so a plain STATUS poll shows it (details: POWER page). */
#define ULTRA_ST_STATE            8u
#define ULTRA_ST_LIMITING         0x01u  /* the board limit reduces the outputs (held 2 s after the last reduction) */
#define ULTRA_ST_TRIPPED          0x02u  /* outputs off after an overcurrent trip, until the retry */
#define ULTRA_ST_POWER_EVENT      0x04u  /* a power event is latched: a POWER page latch the host hasn't acknowledged */
#define ULTRA_ST_CH_LIMITING      0x08u  /* a channel limit (CONFIG 16-20) reduces a channel (held 2 s) */
/* bits 4-7: why the light isn't as the host set it now, the first of these that applies (the limits: bits 0
 * and 3, alongside); 0 = nothing (as set) */
#define ULTRA_ST_WHY_SHIFT        4u
#define ULTRA_ST_WHY_NONE         0u
#define ULTRA_ST_WHY_SHORT        1u     /* off after a short-circuit trip, until the retry */
#define ULTRA_ST_WHY_OVERLOAD     2u     /* off after an overload trip, until the retry */
#define ULTRA_ST_WHY_RETRIES_OUT  3u     /* off: the retries used up (CONFIG 24), until a new command or a power cycle */
#define ULTRA_ST_WHY_TEST         4u     /* the test jig's mode */
#define ULTRA_ST_WHY_CALIBRATING  5u     /* a current calibration test (IDENTIFY 2) */
#define ULTRA_ST_WHY_HOT          6u     /* too hot: the MCU die over its warning level (a warning only: nothing dims) */
#define ULTRA_ST_WHY_SUPPLY       7u     /* a supply dip now or in the last ULTRA_ST_DIP_RECENT_S */
#define ULTRA_ST_WHY_SIGNAL_LOSS  8u     /* signal loss (CONFIG 8) */
#define ULTRA_ST_WHY_SOFT_START   9u     /* the soft start ramps the outputs */
#define ULTRA_ST_WHY_DARK_HOLD    10u    /* IDENTIFY 3 */
#define ULTRA_ST_WHY_PATTERN      11u    /* an identify / acknowledge / version / fault-code pattern */
#define ULTRA_ST_DIP_RECENT_S     30u
/*      0x10-0x80                   reserved (0) */
/* POWER page DATA (0x08): the board current limit and overcurrent events of boards with the input current
 * sense (ULTRA_FEAT_POWER_EVENTS); others answer ULTRA_PAGE_UNSUPPORTED. A limit event starts when the limiter
 * reduces the outputs after ULTRA_PW_HOLD_MS without reducing them. Counters are u16 and wrap; ages are u24 seconds
 * (0xFFFFFF = none since power-up). While a latch is set, every answer carries RFLAGS ULTRA_RSP_EVENT and STATUS
 * byte 8 ULTRA_ST_POWER_EVENT. */
#define ULTRA_PW_NOW              0u     /* u8: ULTRA_ST_LIMITING | ULTRA_ST_TRIPPED | ULTRA_ST_CH_LIMITING now */
#define ULTRA_PW_LATCHED          1u     /* u8: ULTRA_PW_EV_* since the host's last acknowledge */
#define ULTRA_PW_LIMIT_EVENTS     2u     /* u16: limit events since power-up */
#define ULTRA_PW_OVL_TRIPS        4u     /* u16: overload trips since power-up */
#define ULTRA_PW_SHORT_TRIPS      6u     /* u16: short-circuit trips since power-up */
#define ULTRA_PW_LIMIT_AGE        8u     /* u24: s since the last limit event started */
#define ULTRA_PW_TRIP_AGE         11u    /* u24: s since the last trip */
#define ULTRA_PW_LIMIT_MA         14u    /* u16: board limit in effect, mA (0xFFFF = none) */
#define ULTRA_PW_SRC_CAUSE        16u    /* u8: limit source (ULTRA_PW_SRC_*) << 4 | last trip cause (ULTRA_PW_CAUSE_*) */
#define ULTRA_PW_EVENT_MA         17u    /* u16: measured board current when the last limit event started, mA */
#define ULTRA_PW_TRIP_MA          19u    /* u16: measured current at the last trip, mA (short circuit: the peak sample) */
#define ULTRA_PW_FACTOR           21u    /* u16: the limiter's scale now, 0..65535 = 0..1 (65535 = none, 0 = tripped) */
#define ULTRA_PW_RETRY_S          23u    /* u8: s until the retry after a trip (0 = not tripped, 255 = 255 s or more) */
#define ULTRA_PW_EV_LIMITED       0x01u  /* latched: a limit event */
#define ULTRA_PW_EV_OVERLOAD      0x02u  /* latched: an overload trip (above the limit's trip level for 200 ms) */
#define ULTRA_PW_EV_SHORT         0x04u  /* latched: a short-circuit trip (>= 1.5 x the fuse rating for 50-100 us) */
#define ULTRA_PW_EV_SUPPLY_DIP    0x08u  /* latched: a supply dip (SUPPLY page) */
#define ULTRA_PW_SRC_BOARD        0u     /* limit source: the board's rating (also the CALIBRATION page's encoding) */
#define ULTRA_PW_SRC_USER         1u     /* limit source: CONFIG 21 / the stored setting */
#define ULTRA_PW_SRC_PWM          2u     /* limit source: the PWM preset (input capacitor) */
#define ULTRA_PW_SRC_FUSE         3u     /* limit source: the soldered fuse */
#define ULTRA_PW_CAUSE_NONE       0u
#define ULTRA_PW_CAUSE_SHORT      1u
#define ULTRA_PW_CAUSE_OVERLOAD   2u
#define ULTRA_PW_HOLD_MS          2000u  /* LIMITING / CH_LIMITING hold, and the quiet time that ends a limit event */
/* POWER page POLL arguments (slot bytes 7..): flags (1) | limit events (2) | overload trips (2) | short trips (2) |
 * supply dips (2: the SUPPLY page's count), the counts the host last read. With ULTRA_PW_ARG_ACK a latch clears
 * when its count equals the acknowledged one, so an event after the host's read stays latched, and a repeated or lost
 * POLL does no harm. The answer shows the state after the acknowledge. (A host that sends 0 for the dips leaves
 * a dip latched.) */
#define ULTRA_PW_ARG_ACK          0x01u
/* SUPPLY page DATA (0x09): dips of the supply / bus voltage (ULTRA_FEAT_SUPPLY_DIPS). A dip: the 1 ms readings
 * fall clearly below a slow mean of the supply (the firmware's: below 90 % of a ~4 s mean for >= 3 ms; it ends above
 * 93 % for >= 3 ms), so it works at 12, 24 or 48 V alike. Each dip also latches ULTRA_PW_EV_SUPPLY_DIP on the POWER
 * page. POLL argument (slot byte 7): the logged dip the ULTRA_SU_EV_* fields show, 0 = the newest. Counters wrap; ages
 * are u24 seconds (0xFFFFFF = none since power-up). */
#define ULTRA_SU_DIPS             0u     /* u16: dips since power-up */
#define ULTRA_SU_REF_MV           2u     /* u16: the reference (the slow mean) now, mV; 0 = not known yet */
#define ULTRA_SU_NOW_MV           4u     /* u16: the supply now (16 ms average), mV */
#define ULTRA_SU_DEEP_MV          6u     /* u16: the deepest dip since power-up (lowest reading against its reference):
                                            its lowest reading, mV */
#define ULTRA_SU_DEEP_PCT         8u     /* u8: ... that reading in % of its reference */
#define ULTRA_SU_DEEP_AGE         9u     /* u24: ... s since it began */
#define ULTRA_SU_LOGGED           12u    /* u8: dips in the log, 0..ULTRA_SU_LOG */
#define ULTRA_SU_EV_AGE           13u    /* u24: the selected dip: s since it began */
#define ULTRA_SU_EV_MV            16u    /* u16: its lowest reading, mV */
#define ULTRA_SU_EV_PCT           18u    /* u8: that reading in % of its reference */
#define ULTRA_SU_EV_MS            19u    /* u16: how long it lasted, ms (65535 = 65.5 s or longer; a dip still going
                                            isn't in the log yet) */
#define ULTRA_SU_EV_MA            21u    /* u16: the board current at its lowest reading, mA */
#define ULTRA_SU_LOG              4u     /* dips the board keeps (RAM: a power cycle clears them) */
/* HEALTH page DATA (0x0A): channel health from the current calibration test (IDENTIFY 2) and the running
 * channel model, and the MCU temperature peak (ULTRA_FEAT_HEALTH). Display only: nothing here changes the outputs. */
#define ULTRA_HE_FLAGS            0u     /* u8: ULTRA_HE_F_* */
#define ULTRA_HE_CLASS            1u     /* 5 x u8, CH1..CH5: the last test's class (ULTRA_HE_UNTESTED..) | reasons */
#define ULTRA_HE_TEST_DEV         6u     /* 5 x int8: the last test at 100 % against the test stored before it, %
                                            (ULTRA_HE_NA: not known) */
#define ULTRA_HE_MODEL_DEV        11u    /* 5 x int8: the running model at 100 % against the stored test, % */
#define ULTRA_HE_DARK_MA          16u    /* int16: the board current with every output off, over the stored test's zero
                                            (logic + offset), mA */
#define ULTRA_HE_TEMP_PEAK        18u    /* int16: the highest MCU die temperature since power-up, 0.1 C (ULTRA_ST_NA =
                                            none) */
#define ULTRA_HE_TEMP_AGE         20u    /* u24: s since it (0xFFFFFF = none) */
#define ULTRA_HE_F_TESTED         0x01u  /* a test ran since power-up (the classes are its) */
#define ULTRA_HE_F_ABNORMAL       0x02u  /* a channel's class is ULTRA_HE_ABNORMAL */
#define ULTRA_HE_F_DARK           0x04u  /* current with every output off (a FET stuck on, leakage) */
#define ULTRA_HE_F_DRIFT          0x08u  /* the running model is far off the stored test on a channel */
#define ULTRA_HE_F_CAL_SUGGESTED  0x10u  /* a calibration test is suggested:
                                            the model far off the stored test for 5 min lit, or a new load */
#define ULTRA_HE_F_HOT            0x20u  /* the MCU die over its warning level now (STATUS 8: ULTRA_ST_WHY_HOT) */
#define ULTRA_HE_UNTESTED         0u
#define ULTRA_HE_NO_LOAD          1u     /* nothing connected (and nothing was at the test before) */
#define ULTRA_HE_NORMAL           2u
#define ULTRA_HE_ABNORMAL         3u     /* see the reasons */
#define ULTRA_HE_R_NOT_RISING     0x10u  /* the current didn't rise with the level (25 / 50 / 75 / 100 %) */
#define ULTRA_HE_R_ABOVE          0x20u  /* far above the test before */
#define ULTRA_HE_R_BELOW          0x40u  /* far below the test before, or no load where it had one */
#define ULTRA_HE_NA               (-128)
/* ENERGY page DATA (0x0B): the input energy, supply / bus voltage x the measured input current
 * (ULTRA_FEAT_ENERGY) */
#define ULTRA_EN_BOOT_MWH         0u     /* u32: since power-up, mWh */
#define ULTRA_EN_TOTAL_WH         4u     /* u32: the board's lifetime, Wh (stored on the board at most hourly, and when
                                            its outputs go dark; a factory reset keeps it) */
#define ULTRA_EN_POWER_MW         8u     /* u32: the input power now, mW */
#define ULTRA_EN_SAVED_AGE        12u    /* u24: s since the lifetime value was last stored (0xFFFFFF = not since
                                            power-up) */
#define ULTRA_EN_VB               15u    /* u16: the LED supply after the receiver's buck (V_B), x 10 mV; 0xFFFF
                                            = not measured */
#define ULTRA_EN_PEAK_MA          17u    /* u16: the highest input current (~100 ms average) since this page was
                                            last answered, mA (an answer the host misses takes its window's peak) */
/* CALIBRATION page byte 1: the current model in use (bits 0-3: 0 none, 1 learned, 2 calibrated) | how the last
 * calibration test (IDENTIFY 2) ended (bits 4-7, ULTRA_CA_END_*). A test that stopped early stores nothing: the
 * calibration before it stays in use */
#define ULTRA_CA_MODEL            1u
#define ULTRA_CA_END_COMPLETE     0u     /* complete, or no test since power-up */
#define ULTRA_CA_END_TRIP         1u     /* stopped: a trip */
#define ULTRA_CA_END_LIMIT        2u     /* stopped: the installation's limit (CONFIG 21) reached, never above it */
#define ULTRA_CA_END_SUPPLY       3u     /* stopped: a supply dip */
/* LIFETIME page DATA (0x0C): the board's lifetime record, its odometer: kept across power cuts (stored with the
 * lifetime energy, ENERGY page; a factory reset keeps it). The values as they are now: the stored ones and what came
 * since (ULTRA_FEAT_LIFETIME). Counters: their low 24 / 16 bits */
#define ULTRA_LT_POWERED          0u     /* u24: time powered, 0.1 h */
#define ULTRA_LT_LIT              3u     /* u24: time with an output on, 0.1 h */
#define ULTRA_LT_POWER_UPS        6u     /* u24 */
#define ULTRA_LT_OVERLOAD_TRIPS   9u     /* u16 (saturating) */
#define ULTRA_LT_SHORT_TRIPS      11u    /* u16 (saturating) */
#define ULTRA_LT_DIPS             13u    /* u24: supply dips */
#define ULTRA_LT_TEMP_PEAK        16u    /* u8: the highest MCU die temperature, 0.5 C (0 = none yet) */
#define ULTRA_LT_CURRENT_PEAK     17u    /* u8: the highest input current (~100 ms average), 0.1 A */
#define ULTRA_LT_WATCHDOG         18u    /* u8: watchdog resets (saturating) */
#define ULTRA_LT_UPDATES          19u    /* u16: firmware updates (starts with a new app version) */
#define ULTRA_LT_RECORDS          21u    /* u24: the record's writes (its flash wear) */
/* LIFE_SYNC page (0x0E, ULTRA_FEAT_LIFE_SYNC): a hub that keeps each board's lifetime counters (its ledger,
 * by the board's full UID, IDENTITY part 0) sends them back after the board's power-up, so what the board couldn't
 * store before a power cut isn't lost. The board merges once per power-up, and only when the POLL's UID16 and RECORDS
 * low 8 bits are its own (the record the hub reconciled against: a board that wrote since, or another board, merges
 * nothing). Each counter is raised by the hub's step d ahead (in the wire's units) when 0 < d <= its step, else kept:
 * nothing is ever lowered; a counter ahead by more than its step is ignored (result CAPPED), the others still merge.
 * Ahead: the 24-bit fields (powered, lit, power-ups, dips) modulo 2^24 (by less than 2^23), the 16- and 32-bit ones
 * (the trips, the energy) plainly (a lower value never). The steps (the QuinLED Diff-Power Ultra-Analog receiver):
 * powered time 30 days; lit time the powered time this merge raised + 0.1 h; power-ups, trips and dips one per 10 s
 * of the powered time this merge raised + 64; the energy 1 kW over the powered time this merge raised + 1 h; none
 * past the counter's room (the receiver's seconds and Wh are 32-bit: powered / lit time stop at 11,930,464 x 0.1 h,
 * the energy at 4,294,967,295 Wh, for good: a CAPPED there is no backlog). They limit what a wrong ledger can do; the
 * ledger's correctness is the hub's: its own accounting, not an echo of these reads (the wiki: Return channel, "The
 * hub's ledger"). A merged counter counts as stored: it
 * never makes the board write; it is stored with the board's own next lifetime write, and the board's write allowance
 * counts only its own powered time. Hub: once per board power-up, when the ledger is ahead; then the notice (IDENTIFY
 * 7) if the line is to be switched off, and no POLL until the cut. A board without the feature answers
 * ULTRA_PAGE_UNSUPPORTED. POLL arguments ("POLL argument 0" = slot byte 7): */
#define ULTRA_LS_UID16            0u     /* u16: the board's UID16 (as in its answers) */
#define ULTRA_LS_LIFETIME         2u     /* 16 bytes: LIFETIME page bytes 0..15, the same fields and layout (powered,
                                            lit, power-ups, overload trips, short trips, supply dips) */
#define ULTRA_LS_TOTAL_WH         18u    /* u32: the lifetime energy, Wh (as ENERGY 4) */
#define ULTRA_LS_RECORDS          22u    /* u8: the low 8 bits of the board's RECORDS (LIFETIME 21) the ledger was
                                            reconciled against */
/* ... the answer's DATA: bytes 0..15 as LIFETIME's (after the merge), then: */
#define ULTRA_LS_A_TOTAL_WH       16u    /* u32: the lifetime energy, Wh, after the merge */
#define ULTRA_LS_A_RESULT         20u    /* u8: ULTRA_LS_R_* */
#define ULTRA_LS_A_APPLIED        21u    /* u8: the counters raised, ULTRA_LS_F_* */
#define ULTRA_LS_A_RECORDS        22u    /* u16: RECORDS, its low 16 bits */
#define ULTRA_LS_R_APPLIED        0u     /* merged (every counter that was ahead) */
#define ULTRA_LS_R_NOOP           1u     /* nothing was ahead (doesn't use up the power-up's merge) */
#define ULTRA_LS_R_UID            2u     /* UID16 not the board's: nothing merged */
#define ULTRA_LS_R_RECORDS        3u     /* RECORDS low 8 bits not the board's: nothing merged */
#define ULTRA_LS_R_ONCE           4u     /* already merged this power-up: nothing merged */
#define ULTRA_LS_R_CAPPED         5u     /* a counter ahead by more than its step or its room: ignored (the others:
                                            APPLIED's; with none applied it doesn't use up the power-up's merge) */
#define ULTRA_LS_F_POWERED        0x01u
#define ULTRA_LS_F_LIT            0x02u
#define ULTRA_LS_F_UPS            0x04u
#define ULTRA_LS_F_TRIPS          0x08u  /* overload or short */
#define ULTRA_LS_F_DIPS           0x10u
#define ULTRA_LS_F_WH             0x20u
/* DIAG page DATA (0x0D): what service looks at first (ULTRA_FEAT_DIAG). POLL argument 0, the running board: */
#define ULTRA_DG_RESET            0u     /* u8: this start's reset cause, ULTRA_ST_RST_* (as STATUS 7) */
#define ULTRA_DG_BC_CAUSE         1u     /* u8: the last run's breadcrumbs (kept by a watchdog, software or pin reset):
                                            the exception's cause (RISC-V mcause; 0 = none: a watchdog reset without
                                            one); 0xFF = none kept (a power-up) */
#define ULTRA_DG_BC_PC            2u     /* u16: ... where the exception came from (the address's low 16 bits) */
#define ULTRA_DG_BC_UPTIME        4u     /* u24: ... how long that run had been up, s (saturating) */
#define ULTRA_DG_CPU              7u     /* u16: CPU load of the last second, 0.1 % */
#define ULTRA_DG_CPU_PEAK         9u     /* u16: the busiest second since power-up, 0.1 % */
#define ULTRA_DG_OVR_CAPTURES     11u    /* u16: frame captures dropped since power-up (saturating) */
#define ULTRA_DG_OVR_FRAME_ENDS   13u    /* u16: frame ends not classified (saturating) */
#define ULTRA_DG_OVR_LATE_MS      15u    /* u16: ms the main loop ran late (saturating) */
#define ULTRA_DG_SUPPLY_Z         17u    /* u16: the supply / cable impedance, mOhm (0: not known yet; saturating) */
#define ULTRA_DG_FLASH_ERRS       19u    /* u8: flash operations failed in a row (0: the last one succeeded) */
#define ULTRA_DG_FLASH_PENDING    20u    /* u8: records waiting to be written, ULTRA_DG_FP_* */
#define ULTRA_DG_SAVE_WAIT        21u    /* u8: why the settings / calibration / scene / effects wait, ULTRA_DG_SW_* */
#define ULTRA_DG_C10_LOAD         22u    /* u8: the input capacitor's ripple load, % of its rating (0xFF: not measured) */
#define ULTRA_DG_C10_PEAK         23u    /* u8: ... its peak (0xFF: not measured) */
#define ULTRA_DG_FP_SETTINGS      0x01u
#define ULTRA_DG_FP_CALIBRATION   0x02u
#define ULTRA_DG_FP_SCENE         0x04u
#define ULTRA_DG_FP_LIFETIME      0x08u  /* a write is due, waiting for the flash's admission and the write budget (a
                                            regular one also for the outputs to go dark; also a supply dip's
                                            write that couldn't be made then, which doesn't wait for dark) */
#define ULTRA_DG_FP_EFFECTS       0x10u  /* the power-on effects */
#define ULTRA_DG_FP_EXHAUSTED     0x80u  /* not a record: the lifetime record's flash-wear allowance is used up
                                            (the receiver: 32,768 writes + one per 3 h powered); it is written again
                                            once a run earns a write (one per 3 h powered in one run; shorter runs
                                            don't add up). Its counts since the last write are lost at a power cut */
#define ULTRA_DG_SW_NONE          0u     /* nothing pending */
#define ULTRA_DG_SW_DEBOUNCE      1u     /* the last change is under 2 s old */
#define ULTRA_DG_SW_DARK          2u     /* the outputs are bright and this hour's write while bright is used */
#define ULTRA_DG_SW_BUDGET        3u     /* the write budget is used up for now */
#define ULTRA_DG_SW_ERROR         4u     /* the hold-off after a failed flash operation */
#define ULTRA_DG_SW_SUPPLY        5u     /* the 3.3 V rail is too low to write */
/* POLL argument 1: the fault memory (the lifetime record's: kept across power cuts once stored) and the calibration
 * trend */
#define ULTRA_DG_TRIP             0u     /* u8: the last trip: 1 short, 2 overload (bits 0-1) | its current (a short's
                                            peak), 0.5 A (bits 2-7, saturating); 0 = none */
#define ULTRA_DG_TRIP_LIT_H       1u     /* u16: ... the lit hours then (LIFETIME's lit time, saturating) */
#define ULTRA_DG_DIP_PCT          3u     /* u8: the last supply dip: its lowest reading, % of its reference; 0 = none */
#define ULTRA_DG_DIP_10MS         4u     /* u8: ... how long, x 10 ms (255: longer) */
#define ULTRA_DG_DIP_CURRENT      5u     /* u8: ... the input current at its lowest, 0.1 A (saturating) */
#define ULTRA_DG_RST              6u     /* u8: the last abnormal reset: 1 watchdog, 2 an exception (then the watchdog)
                                            (bits 0-1) | the exception's mcause (bits 2-7); 0 = none */
#define ULTRA_DG_RST_UP_H         7u     /* u8: ... how long that run had been up, h (saturating) */
#define ULTRA_DG_TREND            8u     /* 5 x int8, CH1..CH5: the calibration trend, the newest stored test at 100 %
                                            against the first, % (ULTRA_HE_NA: not known) */
#define ULTRA_DG_TREND_LIT_H      13u    /* u16: ... over how many lit hours (0xFFFF: not known) */
#define ULTRA_DG_TREND_TESTS      15u    /* u8: tests in that history (0: not known) */
/* the firmware's features: IDENTITY part 1's word 0 (bits 0-31) as masks (ultra_feat_has: any bit, 0-191) */
#define ULTRA_FEAT_SCENE          0x00000001u  /* SCENE: power-on scene */
#define ULTRA_FEAT_DIM_TO_WARM    0x00000002u  /* LIGHT_OPT dim to warm with red assist */
#define ULTRA_FEAT_DTW_POINTS     0x00000004u  /* LIGHT_OPT set points */
#define ULTRA_FEAT_CURRENT_SENSE  0x00000008u  /* the board current sense in use (CONFIG 21 = the measured current) */
#define ULTRA_FEAT_CALIBRATION    0x00000010u  /* IDENTIFY 2 (current calibration), pages CALIBRATION / CAL_TEST */
#define ULTRA_FEAT_DARK_HOLD      0x00000020u  /* IDENTIFY 3 / 4 */
#define ULTRA_FEAT_FW_UPDATE      0x00000040u  /* UPD sessions (the app with a bootloader) */
#define ULTRA_FEAT_BOOT_PROTOCOL  0x00000080u  /* CONFIG 22 */
#define ULTRA_FEAT_STAGGER_ADAPTIVE 0x00000100u /* CONFIG 7 values 3 / 4 */
#define ULTRA_FEAT_MCU_TEMP       0x00000200u  /* STATUS: the MCU temperature and the 3.3 V rail (ULTRA_ST_*) */
#define ULTRA_FEAT_VERSION_BLINK  0x00000400u  /* IDENTIFY 5: the firmware version as blinks */
#define ULTRA_FEAT_POWER_EVENTS   0x00000800u  /* POWER page and STATUS byte 8 */
#define ULTRA_FEAT_SUPPLY_DIPS    0x00001000u  /* SUPPLY page, POWER latch ULTRA_PW_EV_SUPPLY_DIP */
#define ULTRA_FEAT_HEALTH         0x00002000u  /* HEALTH page */
#define ULTRA_FEAT_ENERGY         0x00004000u  /* ENERGY page */
#define ULTRA_FEAT_SOFT_START     0x00008000u  /* CONFIG 23 */
#define ULTRA_FEAT_SCENE_EFFECT   0x00010000u  /* SCENE_EFFECT: power-on effects */
#define ULTRA_FEAT_LIFETIME       0x00020000u  /* LIFETIME page */
#define ULTRA_FEAT_DIAG           0x00040000u  /* DIAG page */
#define ULTRA_FEAT_RETRY          0x00080000u  /* CONFIG 24: the trips' retry policy */
#define ULTRA_FEAT_LOSS_SCENE     0x00100000u  /* CONFIG 8 value 3: signal loss fades to the power-on scene */
#define ULTRA_FEAT_STANDBY        0x00200000u  /* IDENTIFY 7: the planned power-off notice */
#define ULTRA_FEAT_LIFE_SYNC      0x00400000u  /* LIFE_SYNC page: the hub's lifetime counters merged */
/*      0x00800000-0x80000000     bits 23-31, then words 1-5 (bits 32-191): later official features, in order (every
 *                                 bit official, none hardware-specific) */
/* feature bit `bit` (0-191) in IDENTITY part 1's DATA f (ULTRA_IDF_FEATURES): 1 = the firmware has it */
static inline int ultra_feat_has(const uint8_t *f, uint32_t bit)
{
    return bit < ULTRA_IDF_BITS && ((f[4u * (bit >> 5) + 3u - ((bit & 31u) >> 3)] >> (bit & 7u)) & 1u);
}

/* SET flags (low nibble of byte 1; the other slot types send FLAGS 0) */
#define ULTRA_SET_SMOOTH   0x01u       /* ESPHome's smoothstep curve (6x^5 - 15x^4 + 10x^3), else linear */
#define ULTRA_SET_VIA_OFF  0x02u       /* color mode change: fade to 0, switch, fade up (ESPHome behaviour) */
#define ULTRA_SET_FLAGS    (ULTRA_SET_SMOOTH | ULTRA_SET_VIA_OFF)

/* channel roles (LIGHT_DEF); 8-15 reserved (e.g. amber, UV, lime): a board takes them as ULTRA_ROLE_NONE */
enum {
    ULTRA_ROLE_NONE = 0, ULTRA_ROLE_BRIGHTNESS = 1, ULTRA_ROLE_RED = 2, ULTRA_ROLE_GREEN = 3,
    ULTRA_ROLE_BLUE = 4, ULTRA_ROLE_WHITE = 5, ULTRA_ROLE_COLD_WHITE = 6, ULTRA_ROLE_WARM_WHITE = 7,
};

/* effects (EFFECT byte 0); parameters at payload offset 5.. (big-endian, <= 22 bytes) */
enum {
    ULTRA_FX_NONE = 0,         /* stop: back to the light's state */
    ULTRA_FX_PULSE = 1,        /* on_ms (3) off_ms (3) interval_ms (3) min (2) max (2)   - like ESPHome pulse */
    ULTRA_FX_STROBE = 2,       /* steps (1); steps sent as ULTRA_STEP                    - like ESPHome strobe */
    ULTRA_FX_FLICKER = 3,      /* alpha (2) intensity (2) interval_ms (2)                - like ESPHome flicker */
    ULTRA_FX_RANDOM = 4,       /* transition_ms (3) interval_ms (3)                      - like ESPHome random */
    ULTRA_FX_CANDLE = 5,       /* intensity (2) speed (2, % of nominal)                  - flame flicker */
    ULTRA_FX_BREATHE = 6,      /* period_ms (3) min (2) max (2)                          - sine breathing */
    ULTRA_FX_COLOR_LOOP = 7,   /* period_ms (3) saturation (2)                           - hue rotation (RGB) */
    ULTRA_FX_SUNRISE = 8,      /* duration_ms (3)                                         - warm dawn to the state */
    ULTRA_FX_SUNSET = 9,       /* duration_ms (3)                                         - state to warm dusk, off */
    ULTRA_FX_COUNT = 10,       /* 10-127: future official effects; a board runs an unknown id as ULTRA_FX_NONE */
    ULTRA_FX_CUSTOM = 0x80,    /* 128-255: custom effects */
};

/* CONFIG ids (value 16-bit). Host-managed: RAM only, they end with Ultra mode - except the ids marked
 * "persisted", which the board stores in flash (written only when the value differs from the stored one). */
enum {
    ULTRA_CFG_NONE = 0,
    ULTRA_CFG_PWM_PRESET = 1,      /* 0..14 */
    ULTRA_CFG_PWM_PERIOD = 2,      /* raw period in 144 MHz counts, 1440..65535; 0 = use the preset */
    ULTRA_CFG_DITHER = 3,          /* 0 on, 1 off at low levels, 2 off */
    ULTRA_CFG_DITHER_THRESH = 4,   /* 1..255 = 0.1..25.5 % */
    ULTRA_CFG_DIM_FLOOR = 5,       /* 0 skip, 1 clamp, 2 off */
    ULTRA_CFG_MIN_PULSE = 6,       /* 5..100 x 10 ns */
    ULTRA_CFG_STAGGER = 7,         /* 0 off, 1 fixed (thirds), 2 fixed + spread spectrum,
                                      3 adaptive (least LED_+ ripple), 4 adaptive + spread spectrum */
    ULTRA_CFG_LOSS = 8,            /* 0 hold, 1 off, 2 fade off, 3 scene: fade to the light's stored power-on
                                      scene / effect (a light without one holds) */
    ULTRA_CFG_LOSS_TIMEOUT = 9,    /* x 100 ms, 5..600 */
    ULTRA_CFG_FEEDBACK = 10,       /* identify channel: 0 red, 1 all, 2 off */
    ULTRA_CFG_CH_CURRENT = 11,     /* 11..15: LED current of CH1..CH5 at 100 %, mA (for the channel limits) */
    ULTRA_CFG_CH_MAX = 16,         /* 16..20: limit per channel, mA (0 = none), on the currents of 11..15 */
    ULTRA_CFG_BOARD_MAX = 21,      /* limit for the board, mA: the board holds its measured input current to it, all
                                      channels scaled together (0 = the board rating) */
    ULTRA_CFG_BOOT_PROTOCOL = 22,  /* persisted: protocol mode after power-up, 0 auto, 1 WS2915, 2 WS2805, 3 Ultra
                                      (auto / Ultra apply at once; WS2915 / WS2805 from the next power-up on) */
    ULTRA_CFG_SOFT_START = 23,     /* persisted: 0 off, 1 on (the default). After power-up (the first light)
                                      and after a trip's retry the board ramps its outputs from 20 to 100 % over 0.3 s,
                                      starting 0-100 ms late (from its chip id), so the boards of a pin don't switch
                                      their load on at once */
    ULTRA_CFG_RETRY = 24,          /* persisted: the trips' retry policy, count | interval << 8. Count: 0
                                      never retry, 1-254 retries, 255 forever; the interval 1-255 s (0 = 1 s). The
                                      default 0x010A: 10 retries 1 s apart. After the last one the outputs stay off
                                      until a new command or frame value, or a power cycle; 60 s without a trip
                                      starts the count again */
    ULTRA_CFG_COUNT = 25,          /* 25-127: future official ids; a board skips an unknown id (the others apply) */
    ULTRA_CFG_HW = 128,            /* 128-255: hardware-specific (e.g. a Diff-Power receiver, a current-sense revision) */
    ULTRA_CFG_MCU_TEMP_OFFSET = 128,   /* reserved, not settable yet: MCU temperature offset, int16 0.1 C (a one-point
                                          calibration, core/mcutemp.h); the board skips it like any unknown id */
    /* hardware 3 (the Ultra-Digital receiver): the local pixel effects' settings, all persisted (ULTRA_PX_* below) */
    ULTRA_CFG_PX_CHIP = 129,           /* the strip's chip (ULTRA_PXC_*): its timing, bytes per pixel and colour order */
    ULTRA_CFG_PX_ORDER = 130,          /* the wire order of the channels (ULTRA_PX_ORDER; 0 = the chip's own) */
    ULTRA_CFG_PX_PIXELS = 131,         /* pixels on the strip (0 = none yet: no local effect runs) */
    ULTRA_CFG_PX_FPS = 132,            /* frames per second at most (0 = as many as the line allows) */
    ULTRA_CFG_PX_MA_CH = 133,          /* LED current of one channel at 255, 0.1 mA (0 = 20 mA), for the current limit */
    ULTRA_CFG_PX_LIMIT_MA = 134,       /* the strip's current limit, mA (0 = the link's, 2,250 mA) */
    ULTRA_CFG_PX_WHITE = 135,          /* bits 0-1: the default auto-white mode (ULTRA_PXW_*; 0 = accurate); bits 8-15: the
                                          WS2805 white split, the share of the cold white (0 = even) */
    ULTRA_CFG_PX_VOFFSET = 136,        /* this strip starts at this pixel of a virtual strip (an effect continuing over
                                          ports and receivers) */
    ULTRA_CFG_PX_VLENGTH = 137,        /* ... the virtual strip's length (0 = this strip alone) */
    ULTRA_CFG_PX_POLICY = 138,         /* standalone behaviour (ULTRA_PXP_*) */
    ULTRA_CFG_PX_LOSS_TIMEOUT = 139,   /* x 100 ms with no pixel frame and no marker = signal loss, 5..600 (0 = 30) */
    ULTRA_CFG_PX_FIRST = 129,
    ULTRA_CFG_PX_COUNT = 11,           /* ids 129 .. 139 */
};

/* ----------------------------------------------------------------------------------- local pixel effects (hardware 3) */
/* The QuinLED Diff-Power Ultra-Digital receiver (ULTRA_HWCAP_PIXEL_FX) can make the strip's data itself. The hub starts
 * an effect with PIXEL_FX, a command like any other on a digital port (behind a marker, ULTRA_DP_*; applied once per
 * SEQ, repeated for redundancy). Until then, and after a PIXEL_FX with effect 0, the hub's pixels pass (stream mode,
 * the delay line). The receiver changes between the two only inside a latch: the strip gets whole frames only (the
 * hub's last one, then its own, or the other way round), and a poll never shows. The effects run on the host clock
 * (the header TIME): receivers that got the same PIXEL_FX render the same picture at the same host time, and their frames
 * start on a host-time grid (ULTRA_CFG_PX_FPS), so ports and receivers stay in step. The hub sends a header (a poll, a
 * NOP) at least once a second to a local port: the receiver's phase comes from it. Frames are as long as the strip is
 * (ULTRA_CFG_PX_PIXELS): the frame rate the line allows is the chip's, not the hub's.
 *
 * PIXEL_FX slot (type ULTRA_PIXEL_FX; LIGHT = the segment 0 .. ULTRA_PX_SEGMENTS - 1, ULTRA_LIGHT_BOARD = the whole
 * strip as one segment 0 with the others stopped; LIGHT 4-14 and slot FLAGS: ignored, sent as 0). Offsets are slot
 * bytes, 16- and 32-bit fields big-endian:
 *   EFFECT      u8      0 = stream (leave local mode: LIGHT and every field but STORE are ignored), 1-127 the official
 *                       catalogue (catalogue.json in the protocol repository; ids are never reused), 128-255 custom. An
 *                       effect the receiver doesn't have shows Solid (page ULTRA_PAGE_HW_FX part 1 says which it has)
 *   START       i32     ms relative to the header TIME: the effect's time zero (the phase anchor, as EFFECT's start). A
 *                       PIXEL_FX applies when the host clock reaches it (a late copy catches up)
 *   SPEED, INTENSITY, PALETTE  u8 each; the palette 0-127 built-in (the catalogue), ULTRA_PX_PAL_COLORS c1 -> c2 -> c3,
 *                       ULTRA_PX_PAL_DEFAULT the effect's own
 *   C1, C2, C3  R, G, B (white comes from the auto-white mode)
 *   BRIGHTNESS  u8      0 = dark (the strip is held LOW: flash writes allowed), 255 = full; scaled down by the current limit
 *   TRANSITION  u16     ms: brightness and colour ramps, and the crossfade from the effect it replaces
 *   SEG_START, SEG_LEN  u16 each: the segment's first pixel and its length (0 = to the end of the strip). Segments are
 *                       drawn in order, a higher one over a lower one
 *   FLAGS       u8      ULTRA_PXF_*
 *   CUSTOM1, CUSTOM2  u8 each: the effect's extra sliders (the catalogue names them)
 */
#define ULTRA_PX_SEGMENTS     4u
#define ULTRA_PX_EFFECT       3u
#define ULTRA_PX_START        4u
#define ULTRA_PX_SPEED        8u
#define ULTRA_PX_INTENSITY    9u
#define ULTRA_PX_PALETTE      10u
#define ULTRA_PX_C1           11u      /* C2 at 14, C3 at 17 */
#define ULTRA_PX_C2           14u
#define ULTRA_PX_C3           17u
#define ULTRA_PX_BRIGHTNESS   20u
#define ULTRA_PX_TRANSITION   21u
#define ULTRA_PX_SEG_START    23u
#define ULTRA_PX_SEG_LEN      25u
#define ULTRA_PX_FLAGS        27u
#define ULTRA_PX_CUSTOM1      28u
#define ULTRA_PX_CUSTOM2      29u
#define ULTRA_PX_FX_STREAM    0u       /* EFFECT 0 */
#define ULTRA_PX_PAL_COLORS   0xFEu    /* PALETTE: c1 -> c2 -> c3 -> c1 */
#define ULTRA_PX_PAL_DEFAULT  0xFFu    /* PALETTE: the effect's own */
/* FLAGS. STORE: this segment's fields (not START and SEQ) and CONFIG 129-139 become the power-on / standalone effect
 * (CONFIG 138); with EFFECT 0 the stored effect is cleared. The record is written only in stream mode or while the strip
 * is dark, never as a visible pause. LOCK: a plain pixel stream from the controller doesn't take the strip back
 * (otherwise, with ULTRA_PXP_STREAM_WINS, it does). Bits 6-7: reserved, sent as 0 */
#define ULTRA_PXF_REVERSE     0x01u
#define ULTRA_PXF_MIRROR      0x02u
#define ULTRA_PXF_WHITE_MASK  0x0Cu    /* auto-white mode, ULTRA_PXW_*, of RGBW and RGBCCT strips (0: CONFIG 135's) */
#define ULTRA_PXF_WHITE_SHIFT 2u
#define ULTRA_PXF_STORE       0x10u
#define ULTRA_PXF_LOCK        0x20u
#define ULTRA_PXW_DEFAULT     0u       /* in FLAGS: the strip's own (CONFIG 135); in CONFIG 135: accurate */
#define ULTRA_PXW_OFF         1u       /* no white from the colour (white only where an effect asks for it) */
#define ULTRA_PXW_ACCURATE    2u       /* W = min(R, G, B), taken out of R, G, B: the colour stays true */
#define ULTRA_PXW_BRIGHT      3u       /* W = min(R, G, B), R, G, B kept: brighter, paler */

/* CONFIG 129: the chip (one preset: the timing on the receiver's 1.333 us bit grid, the bytes per pixel, the default
 * colour order; the strip's reset time). The presets of the V5 class share one timing */
#define ULTRA_PXC_WS2812B_V5      0u   /* WS2812B V5 / 2020, WS2811 V2.2 and later: RGB, wire order G R B */
#define ULTRA_PXC_WS2812B_CLASSIC 1u   /* WS2812B before V5: RGB, G R B */
#define ULTRA_PXC_SK6812_RGB      2u   /* SK6812, SK6812 B/1 RGB: G R B */
#define ULTRA_PXC_SK6812_RGBW     3u   /* SK6812 RGBW: G R B W */
#define ULTRA_PXC_WS2814_RGBW     4u   /* WS2814 (24 V): R G B W; ULTRA_CFG_PX_ORDER picks WRGB and the others */
#define ULTRA_PXC_WS2805_RGBCCT   5u   /* WS2805 (RGB + warm + cold white, 5 bytes): R G B WW CW */
#define ULTRA_PXC_WS2815          6u   /* WS2815 (12 V, backup data line): G R B */
#define ULTRA_PXC_WS2811_400K     7u   /* WS2811 at 400 kHz: R G B */
#define ULTRA_PXC_COUNT           8u
/* CONFIG 130: wire byte k (0-4) = the channel in bits 3k .. 3k + 2 (ULTRA_PXCH_*); only as many bytes as the chip has,
 * each channel once; anything else is ignored. 0 = the chip's own order */
#define ULTRA_PXCH_R   0u
#define ULTRA_PXCH_G   1u
#define ULTRA_PXCH_B   2u
#define ULTRA_PXCH_W   3u              /* the white (WS2805: the warm white) */
#define ULTRA_PXCH_CW  4u              /* WS2805: the cold white */
#define ULTRA_PX_ORDER(b0, b1, b2, b3, b4) \
    ((uint16_t)((b0) | ((b1) << 3) | ((b2) << 6) | ((b3) << 9) | ((b4) << 12)))
/* CONFIG 138 (0 = none of them: the hub's stream is all the strip shows, as before). The stored effect is the one a
 * PIXEL_FX with STORE left */
#define ULTRA_PXP_POWER_ON     0x01u   /* from power-up: when the line stays quiet (no pixel frame, no marker) for 2 s,
                                          the stored effect fades in */
#define ULTRA_PXP_ON_LOSS      0x02u   /* signal loss (CONFIG 139) starts the stored effect (else the strip holds its last
                                          frame, as a WS28xx strip does) */
#define ULTRA_PXP_STREAM_WINS  0x04u   /* a pixel stream from the controller takes the strip back from a local effect
                                          that isn't locked (ULTRA_PXF_LOCK): at the second frame, inside a latch */

/* Page ULTRA_PAGE_HW_FX (0x81), three parts (POLL argument 0), big-endian. Part 0, the state: */
#define ULTRA_PXG_STATE        0u      /* part 0 */
#define ULTRA_PXG_CATALOGUE    0u      /* u16: the catalogue's version (catalogue.json) */
#define ULTRA_PXG_EFFECTS      2u      /* u8: effects the firmware has */
#define ULTRA_PXG_PALETTES     3u      /* u8: palettes */
#define ULTRA_PXG_CHIPS        4u      /* u8: chip presets (ULTRA_PXC_COUNT) */
#define ULTRA_PXG_SEGMENTS     5u      /* u8: segments (ULTRA_PX_SEGMENTS) */
#define ULTRA_PXG_MAX_PIXELS   6u      /* u16: the most pixels CONFIG 131 takes with the chip's bytes per pixel now */
#define ULTRA_PXG_MODE         8u      /* u8: ULTRA_PXM_* */
#define ULTRA_PXG_RUNNING      9u      /* 4 x u8: each segment's running effect (0 = none) */
#define ULTRA_PXG_SEQ          13u     /* 4 x u8: the SEQ of each segment's last PIXEL_FX applied */
#define ULTRA_PXG_FPS10        17u     /* u16: frames per second x 10 (local frames, the last second) */
#define ULTRA_PXG_LOAD         19u     /* u8: the render's CPU share, 0.5 % steps (0-200) */
#define ULTRA_PXG_LIMIT        20u     /* u8: the current limiter's brightness scale now, 255 = none */
#define ULTRA_PXG_LATE         21u     /* u8: frames that started after their grid time (saturating) */
#define ULTRA_PXG_CONFIG_CRC   22u     /* u16: ultra_px_config_crc over CONFIG 129-139 as the receiver has them */
/* part 1: bit (id & 7) of byte id >> 3 (ids 0-191) = the effect id is in the firmware's catalogue; part 2 the same for
 * the palettes */
#define ULTRA_PXG_PART_STATE    0u
#define ULTRA_PXG_PART_EFFECTS  1u
#define ULTRA_PXG_PART_PALETTES 2u
#define ULTRA_PXM_STREAM        0u     /* the hub's pixels pass */
#define ULTRA_PXM_LOCAL         1u     /* a PIXEL_FX effect runs */
#define ULTRA_PXM_STANDALONE    2u     /* the stored effect runs (CONFIG 138: power-on, signal loss) */

/* ----------------------------------------------------------------------------------------------- helpers */
/* CRC-8/SMBus (poly 0x07, init 0) and CRC-16/CCITT-FALSE (poly 0x1021, init 0xFFFF), a nibble at a time: 16-entry
 * tables (16 / 32 bytes) instead of 8 shifts per byte, and no 256-entry table (GCC 15 turns the bitwise loop into
 * one) */
static inline uint8_t ultra_crc8(const uint8_t *d, uint32_t n)
{
    static const uint8_t t[16] = { 0x00, 0x07, 0x0E, 0x09, 0x1C, 0x1B, 0x12, 0x15,
                                   0x38, 0x3F, 0x36, 0x31, 0x24, 0x23, 0x2A, 0x2D };
    uint32_t c = 0;
    while (n--) {
        c ^= *d++;
        c = ((c << 4) & 0xFFu) ^ t[c >> 4];
        c = ((c << 4) & 0xFFu) ^ t[c >> 4];
    }
    return (uint8_t)c;
}

static inline uint16_t ultra_crc16(const uint8_t *d, uint32_t n)
{
    static const uint16_t t[16] = { 0x0000, 0x1021, 0x2042, 0x3063, 0x4084, 0x50A5, 0x60C6, 0x70E7,
                                    0x8108, 0x9129, 0xA14A, 0xB16B, 0xC18C, 0xD1AD, 0xE1CE, 0xF1EF };
    uint32_t c = 0xFFFFu;
    while (n--) {
        c ^= (uint32_t)*d++ << 8;
        c = ((c << 4) & 0xFFFFu) ^ t[c >> 12];
        c = ((c << 4) & 0xFFFFu) ^ t[c >> 12];
    }
    return (uint16_t)c;
}

static inline void ultra_put16(uint8_t *p, uint32_t v) { p[0] = (uint8_t)(v >> 8); p[1] = (uint8_t)v; }
static inline void ultra_put24(uint8_t *p, uint32_t v) { p[0] = (uint8_t)(v >> 16); p[1] = (uint8_t)(v >> 8); p[2] = (uint8_t)v; }
static inline void ultra_put32(uint8_t *p, uint32_t v) { ultra_put16(p, v >> 16); ultra_put16(p + 2, v); }
static inline uint32_t ultra_get16(const uint8_t *p) { return ((uint32_t)p[0] << 8) | p[1]; }
static inline uint32_t ultra_get24(const uint8_t *p) { return ((uint32_t)p[0] << 16) | ((uint32_t)p[1] << 8) | p[2]; }
static inline uint32_t ultra_get32(const uint8_t *p) { return (ultra_get16(p) << 16) | ultra_get16(p + 2); }

/* ----------------------------------------------------------------------------------------------- frame */
/* layout (wire VERSION 2): slot i at byte ULTRA_SLOT_BYTES x i, the header after the n slots */
static inline uint32_t ultra_frame_bytes(uint32_t n) { return ULTRA_SLOT_BYTES * n + ULTRA_HDR_BYTES; }
static inline uint32_t ultra_header_offset(uint32_t n) { return ULTRA_SLOT_BYTES * n; }

/* Preamble frames for a chain of n boards. A board locks to Ultra on 3 headers (a DARK slot is never valid; a board
 * locked to a WS protocol decodes only its WS window anyway) and forwards with the Ultra plan one frame later, so
 * the next board sees Ultra lengths ~4 frames after its predecessor; + 2 margin. Bit-level chain test
 * (firmware test_wire.c): 10 boards locked to WS2805 / WS2915 need 40, all boards get the first command after 42. */
static inline uint32_t ultra_preamble_frames(uint32_t n) { return 4u * n + 2u; }

/* ----------------------------------------------------------------------------------------------- header */
/* the trailer: time_us = host clock at the end of the frame, flags = ULTRA_HDR_* (0 for normal frames) */
static inline void ultra_build_header_flags(uint8_t *h, uint32_t time_us, uint8_t flags)
{
    ultra_put16(h, ULTRA_MAGIC);
    h[2] = (uint8_t)((ULTRA_VERSION << 4) | (flags & 0x0Fu));
    ultra_put32(h + 3, time_us);
    h[7] = ultra_crc8(h, 7);
}

static inline void ultra_build_header(uint8_t *h, uint32_t time_us)
{
    ultra_build_header_flags(h, time_us, 0);
}

/* 0 = valid Ultra header (magic, version, CRC) */
static inline int ultra_parse_header(const uint8_t *h, uint32_t *time_us)
{
    if (ultra_get16(h) != ULTRA_MAGIC || (h[2] >> 4) != ULTRA_VERSION || ultra_crc8(h, 7) != h[7]) {
        return -1;
    }
    *time_us = ultra_get32(h + 3);
    return 0;
}

static inline uint8_t ultra_header_flags(const uint8_t *h)
{
    return (uint8_t)(h[2] & 0x0Fu);
}

/* ----------------------------------------------------------------------------------------------- slots */
static inline void ultra_slot_begin(uint8_t *s, uint8_t type, uint8_t light, uint8_t flags, uint8_t seq)
{
    uint32_t i;
    for (i = 0; i < ULTRA_SLOT_BYTES; i++) {
        s[i] = 0;
    }
    s[0] = type;
    s[1] = (uint8_t)((light << 4) | (flags & 0x0Fu));
    s[2] = seq;
}

static inline void ultra_slot_seal(uint8_t *s)
{
    ultra_put16(s + 30, ultra_crc16(s, 30));
}

/* 0 = CRC ok */
static inline int ultra_slot_check(const uint8_t *s)
{
    return ultra_crc16(s, 30) == ultra_get16(s + 30) ? 0 : -1;
}

/* SET: target M (state x brightness) and per channel A (color component: red / green / blue, white, cold_white,
 * warm_white; 65535 for a brightness channel) and B (second factor: color_brightness on RGB channels, else 65535).
 * The board interpolates M, A and B separately (like ESPHome interpolates its LightColorValues) from its current
 * values over dur_ms (curve per flags), starting at header TIME + start_ms (signed: a late copy catches up), and
 * computes every 1 ms with an exact power-law gamma on the full product, no 8-bit / table steps:
 *   out = gamma(M x A x B),  or for the CW / WW channels of a constant_brightness light
 *   out = g(M) x max(g(cw), g(ww)) x g(this) / (g(cw) + g(ww))   (ESPHome's formula, g = gamma)
 * then min / max power: out = min + out x (max - min) (0 stays 0 with zero_means_zero). */
static inline void ultra_slot_set(uint8_t *s, uint8_t light, uint8_t seq, uint8_t flags, uint16_t m,
                                  const uint16_t *a, const uint16_t *b, uint32_t dur_ms, int16_t start_ms)
{
    uint8_t *p = s + ULTRA_PAYLOAD;
    uint32_t i;
    ultra_slot_begin(s, ULTRA_SET, light, (uint8_t)(flags & ULTRA_SET_FLAGS), seq);
    ultra_put16(p, m);
    for (i = 0; i < 5u; i++) {
        ultra_put16(p + 2 + 2 * i, a[i]);
        ultra_put16(p + 12 + 2 * i, b[i]);
    }
    ultra_put24(p + 22, dur_ms > 0xFFFFFFu ? 0xFFFFFFu : dur_ms);
    ultra_put16(p + 25, (uint16_t)start_ms);
    ultra_slot_seal(s);
}

/* LIGHT_DEF: role per channel CH1..CH5 (ULTRA_ROLE_NONE = not part of this light), gamma x1000 (0 = linear, as 1000),
 * flags, min / max power per channel (0..65535 = 0..1, ESPHome output min_power / max_power).
 * Payload: roles as nibbles (p0 = CH1<<4 | CH2, p1 = CH3<<4 | CH4, p2 = CH5<<4), gamma (2), flags (1),
 * min[5] (2 each), max[5] (2 each), 1 spare byte. */
#define ULTRA_DEF_ZERO_MEANS_ZERO      0x01u
#define ULTRA_DEF_CONSTANT_BRIGHTNESS  0x02u
#define ULTRA_DEF_FLAGS                (ULTRA_DEF_ZERO_MEANS_ZERO | ULTRA_DEF_CONSTANT_BRIGHTNESS)
static inline void ultra_slot_light_def(uint8_t *s, uint8_t light, uint8_t seq, const uint8_t *roles,
                                        uint16_t gamma_x1000, uint8_t def_flags, const uint16_t *min_p,
                                        const uint16_t *max_p)
{
    uint8_t *p = s + ULTRA_PAYLOAD;
    uint32_t i;
    ultra_slot_begin(s, ULTRA_LIGHT_DEF, light, 0, seq);
    for (i = 0; i < 5u; i++) {
        const uint8_t r = roles[i] > ULTRA_ROLE_WARM_WHITE ? (uint8_t)ULTRA_ROLE_NONE : roles[i];
        p[i >> 1] |= (uint8_t)(r << ((i & 1u) ? 0 : 4));
        ultra_put16(p + 6 + 2 * i, min_p[i]);
        ultra_put16(p + 16 + 2 * i, max_p[i]);
    }
    ultra_put16(p + 3, gamma_x1000);
    p[5] = (uint8_t)(def_flags & ULTRA_DEF_FLAGS);
    ultra_slot_seal(s);
}

/* LIGHT_OPT: light options beyond LIGHT_DEF.
 * Dim to warm (flag ULTRA_OPT_DIM_TO_WARM, needs cold_white + warm_white channels): every 1 ms the board moves the
 * CW / WW mix with M (state x brightness), linearly in mireds, from the light's own colour temperature at M = 1 to
 * warm_mired at M = 0, keeping ESPHome's normalisation (constant_brightness: CW + WW = g(M) x max(g(cw), g(ww));
 * else the brighter channel stays at g(M) x max), so at full brightness nothing changes. Below the WW LED's
 * temperature (warm_mired > ww_mired) red assist adds red (and red / 4 on green) up to red_assist x the WW level at
 * M = 0, on the light's own red / green channels while its RGB part is off.
 * Set points (flag ULTRA_OPT_POINTS): instead of the straight line, up to ULTRA_OPT_POINTS_MAX (M, mireds x 10)
 * pairs at payload 9.. (u16 each, unused = 0, any order). The board interpolates linearly in mireds between them;
 * above the highest point up to the light's own temperature at M = 1 (unless a point is at M = 65535), below the
 * lowest point it holds that point's temperature. Never colder than the light's own temperature. warm_mired10 = the
 * warmest point (the red assist scale).
 * Mireds x 10 (u16); red_assist 0..65535 = 0..100 %. */
#define ULTRA_OPT_DIM_TO_WARM 0x01u
#define ULTRA_OPT_POINTS      0x02u
#define ULTRA_OPT_FLAGS       (ULTRA_OPT_DIM_TO_WARM | ULTRA_OPT_POINTS)
#define ULTRA_OPT_POINTS_MAX  4u
static inline void ultra_slot_light_opt_points(uint8_t *s, uint8_t light, uint8_t seq, uint8_t flags,
                                               uint16_t cw_mired10, uint16_t ww_mired10, uint16_t warm_mired10,
                                               uint16_t red_assist, uint32_t n, const uint16_t *pt_m,
                                               const uint16_t *pt_mired10)
{
    uint8_t *p = s + ULTRA_PAYLOAD;
    uint32_t i;
    ultra_slot_begin(s, ULTRA_LIGHT_OPT, light, 0, seq);
    p[0] = (uint8_t)(flags & ULTRA_OPT_FLAGS);
    ultra_put16(p + 1, cw_mired10);
    ultra_put16(p + 3, ww_mired10);
    ultra_put16(p + 5, warm_mired10);
    ultra_put16(p + 7, red_assist);
    for (i = 0; i < n && i < ULTRA_OPT_POINTS_MAX; i++) {
        ultra_put16(p + 9 + 4 * i, pt_m[i]);
        ultra_put16(p + 11 + 4 * i, pt_mired10[i]);
    }
    ultra_slot_seal(s);
}

static inline void ultra_slot_light_opt(uint8_t *s, uint8_t light, uint8_t seq, uint8_t flags, uint16_t cw_mired10,
                                        uint16_t ww_mired10, uint16_t warm_mired10, uint16_t red_assist)
{
    ultra_slot_light_opt_points(s, light, seq, flags, cw_mired10, ww_mired10, warm_mired10, red_assist, 0, 0, 0);
}

/* EFFECT: id, start time (ms, signed 32-bit, relative to header TIME: the effect's phase, so a board that
 * joins later runs in step), raw parameter bytes (<= 22). The board switches (also to FX_NONE) at that time. */
#define ULTRA_FX_PARAMS 22u
/* parameter bytes of each effect (the table above); the rest of the payload is spare (0) */
static inline uint32_t ultra_fx_param_bytes(uint8_t fx)
{
    static const uint8_t n[ULTRA_FX_COUNT] = { 0, 13, 1, 6, 6, 4, 7, 5, 3, 3 };
    return fx < ULTRA_FX_COUNT ? n[fx] : 0u;
}

static inline void ultra_slot_effect(uint8_t *s, uint8_t light, uint8_t seq, uint8_t fx, int32_t start_ms,
                                     const uint8_t *params, uint32_t nparams)
{
    uint8_t *p = s + ULTRA_PAYLOAD;
    uint32_t i;
    ultra_slot_begin(s, ULTRA_EFFECT, light, 0, seq);
    p[0] = fx < ULTRA_FX_COUNT ? fx : (uint8_t)ULTRA_FX_NONE;
    ultra_put32(p + 1, (uint32_t)start_ms);
    for (i = 0; i < nparams && i < ultra_fx_param_bytes(p[0]); i++) {
        p[5 + i] = params[i];
    }
    ultra_slot_seal(s);
}

/* STEP: index / count, duration and transition (ms), values like SET. seq = the strobe EFFECT's seq. */
static inline void ultra_slot_step(uint8_t *s, uint8_t light, uint8_t seq, uint8_t index, uint8_t count,
                                   uint16_t dur_ms, uint16_t trans_ms, uint16_t m, const uint16_t *a,
                                   const uint16_t *b)
{
    uint8_t *p = s + ULTRA_PAYLOAD;
    uint32_t i;
    ultra_slot_begin(s, ULTRA_STEP, light, 0, seq);
    p[0] = (uint8_t)((index << 4) | (count & 0x0Fu));
    ultra_put16(p + 1, dur_ms);
    ultra_put16(p + 3, trans_ms);
    ultra_put16(p + 5, m);
    for (i = 0; i < 5u; i++) {
        ultra_put16(p + 7 + 2 * i, a[i]);
        ultra_put16(p + 17 + 2 * i, b[i]);
    }
    ultra_slot_seal(s);
}

/* CONFIG: up to 9 (id, value) pairs; an unused pair is (0, 0). Ids that aren't defined (25-127) are left out; the
 * hardware-specific ids (128-255) pass: the board that has them takes them, the others skip them. */
static inline void ultra_slot_config(uint8_t *s, uint8_t seq, const uint8_t *ids, const uint16_t *values, uint32_t n)
{
    uint8_t *p = s + ULTRA_PAYLOAD;
    uint32_t i, j = 0;
    ultra_slot_begin(s, ULTRA_CONFIG, ULTRA_LIGHT_BOARD, 0, seq);
    for (i = 0; i < n && j < 9u; i++) {
        if (ids[i] != ULTRA_CFG_NONE && (ids[i] < ULTRA_CFG_COUNT || ids[i] >= ULTRA_CFG_HW)) {
            p[3 * j] = ids[i];
            ultra_put16(p + 3 * j + 1, values[i]);
            j++;
        }
    }
    ultra_slot_seal(s);
}

/* IDENTIFY: on = 1 blinks the board's identify pattern (30 s), 0 stops it.
 * ULTRA_IDENT_CAL: boards with the input current sense measure each channel alone (zero, then R, G, B, W1, W2) and
 * store the result: `arg` 0 = the standard test at 25 / 50 / 75 / 100 % (0.95 s), 5..100 = at that duty only
 * (0.18 s). Boards without it ignore it.
 * ULTRA_IDENT_DARK: every output fades to dark (0.3 s) and stays dark for `arg` s (0 = 60 s) or until
 * ULTRA_IDENT_RELEASE, which fades back; the light engine keeps running underneath, so states and effects continue.
 * The hub darkens a whole pin this way and then calibrates its boards one after the other.
 * ULTRA_IDENT_VERSION: the board blinks its firmware version once on its feedback channel (3 fast, then
 * major, minor and patch as value codes, then alpha / beta / rc as 1 / 2 / 3 short), then shows its outputs again.
 * ULTRA_IDENT_POWER_OFF (ultra_slot_power_off): a planned power-off notice. The hub sends it in the slot of every
 * board of a line, 3 copies with the same SEQ (applied once) at 0, 50 and 100 ms, and cuts the line's power at 200 ms;
 * once sent it always cuts (no take-back: a command after it waits for the power to be off, then wakes the line). A
 * board that knows it (a QuinLED Diff-Power Ultra receiver, ULTRA_FEAT_STANDBY) fades to dark (0.3 s) and takes the
 * coming cut as planned: no supply dip in the 10 s after it, and the lifetime record written only if there is something
 * worth storing (a board doesn't write its record for every standby cycle). Still powered 2 s after it with the supply
 * steady, its outputs come back; after 10 s without the cut it forgets the notice. Its
 * next start is a power-up as any: such a receiver stays dark after every power-up until the first valid
 * frame or 1.5 s, then shows its power-on scene. Other boards ignore it.
 * Values 6 and 8-255 and CAL arguments 1-4 / 101-255 are reserved: a board ignores the slot. STOP / ON / RELEASE /
 * VERSION / POWER_OFF send argument 0. */
#define ULTRA_IDENT_STOP 0u
#define ULTRA_IDENT_ON   1u
#define ULTRA_IDENT_CAL  2u
#define ULTRA_IDENT_DARK 3u
#define ULTRA_IDENT_RELEASE 4u
#define ULTRA_IDENT_VERSION 5u
#define ULTRA_IDENT_POWER_OFF 7u       /* (6: reserved) */
#define ULTRA_IDENT_CAL_MIN 5u         /* CAL argument: 0 = the standard test, else MIN..MAX % */
#define ULTRA_IDENT_CAL_MAX 100u
static inline void ultra_slot_identify(uint8_t *s, uint8_t seq, uint8_t on)
{
    ultra_slot_begin(s, ULTRA_IDENTIFY, ULTRA_LIGHT_BOARD, 0, seq);
    s[ULTRA_PAYLOAD] = (on <= ULTRA_IDENT_RELEASE || on == ULTRA_IDENT_VERSION) ? on : (uint8_t)ULTRA_IDENT_STOP;
    ultra_slot_seal(s);
}

/* the planned power-off notice (ULTRA_IDENT_POWER_OFF above) */
static inline void ultra_slot_power_off(uint8_t *s, uint8_t seq)
{
    ultra_slot_begin(s, ULTRA_IDENTIFY, ULTRA_LIGHT_BOARD, 0, seq);
    s[ULTRA_PAYLOAD] = ULTRA_IDENT_POWER_OFF;
    ultra_slot_seal(s);
}

static inline void ultra_slot_calibrate(uint8_t *s, uint8_t seq, uint8_t duty_pct)
{
    ultra_slot_begin(s, ULTRA_IDENTIFY, ULTRA_LIGHT_BOARD, 0, seq);
    s[ULTRA_PAYLOAD] = ULTRA_IDENT_CAL;
    s[ULTRA_PAYLOAD + 1] = duty_pct == 0u ? 0u : duty_pct < ULTRA_IDENT_CAL_MIN ? (uint8_t)ULTRA_IDENT_CAL_MIN
                         : duty_pct > ULTRA_IDENT_CAL_MAX ? (uint8_t)ULTRA_IDENT_CAL_MAX : duty_pct;
    ultra_slot_seal(s);
}

static inline void ultra_slot_hold_dark(uint8_t *s, uint8_t seq, uint8_t hold_s)
{
    ultra_slot_begin(s, ULTRA_IDENTIFY, ULTRA_LIGHT_BOARD, 0, seq);
    s[ULTRA_PAYLOAD] = ULTRA_IDENT_DARK;
    s[ULTRA_PAYLOAD + 1] = hold_s;
    ultra_slot_seal(s);
}

/* SCENE: what the light's channels show from the board's power-up until the controller takes over (stored in the
 * board's flash, written only when the resulting outputs change). mode 0 = not part of the scene (dark), 1 = these
 * values (like SET: M, A x5, B x5; the board computes the outputs with the light's definition and the power limit);
 * modes 2-255 reserved (a board ignores the slot, its stored scene stays).
 * fade_100ms: the take-over cross-fade from the scene to the controller's first state (x 100 ms). */
static inline void ultra_slot_scene(uint8_t *s, uint8_t light, uint8_t seq, uint8_t mode, uint8_t fade_100ms, uint16_t m,
                                    const uint16_t *a, const uint16_t *b)
{
    uint8_t *p = s + ULTRA_PAYLOAD;
    uint32_t i;
    ultra_slot_begin(s, ULTRA_SCENE, light, 0, seq);
    p[0] = mode != 0u ? 1u : 0u;
    p[1] = fade_100ms;
    ultra_put16(p + 2, m);
    for (i = 0; i < 5u; i++) {
        ultra_put16(p + 4 + 2 * i, a[i]);
        ultra_put16(p + 14 + 2 * i, b[i]);
    }
    ultra_slot_seal(s);
}

/* SCENE_EFFECT: the light's power-on effect. From the board's power-up until the controller takes over, the
 * board runs this effect on the light's SCENE values (mode 1) by itself, no controller needed: e.g. a candle, a slow
 * colour loop, a sunrise. The board stores it in its flash together with the light's LIGHT_DEF, LIGHT_OPT and SCENE
 * (written only when it changes, a few times a day at most: send a fixed power-on effect, not the live one).
 * Payload: effect id (1) + its parameters (as EFFECT, ultra_fx_param_bytes; no start time: the effect starts at
 * power-up). ULTRA_FX_NONE = no effect (the plain SCENE values). ULTRA_FX_STROBE is shown as the plain values (its
 * steps aren't stored). The effect stays until a SCENE_EFFECT changes it or a factory reset; a light whose SCENE mode
 * is 0 shows none. When the controller takes over (its first SET / EFFECT, or WS data), the outputs cross-fade from
 * the effect as from a plain scene (the SCENE fade). A board without it ignores the slot (an unknown type). */
static inline void ultra_slot_scene_effect(uint8_t *s, uint8_t light, uint8_t seq, uint8_t fx, const uint8_t *params,
                                           uint32_t nparams)
{
    uint8_t *p = s + ULTRA_PAYLOAD;
    uint32_t i;
    ultra_slot_begin(s, ULTRA_SCENE_EFFECT, light, 0, seq);
    p[0] = fx < ULTRA_FX_COUNT ? fx : (uint8_t)ULTRA_FX_NONE;
    for (i = 0; i < nparams && i < ultra_fx_param_bytes(p[0]); i++) {
        p[1 + i] = params[i];
    }
    ultra_slot_seal(s);
}

/* PIXEL_FX (the Ultra-Digital receiver's local pixel effects, ULTRA_PX_* above) */
typedef struct {
    uint8_t  effect, speed, intensity, palette;
    uint8_t  c[3][3];                  /* c1, c2, c3: R, G, B */
    uint8_t  brightness, flags, custom1, custom2;
    int32_t  start_ms;                 /* relative to the header TIME */
    uint16_t transition_ms, seg_start, seg_len;
} ultra_pixel_fx_t;

static inline void ultra_slot_pixel_fx(uint8_t *s, uint8_t segment, uint8_t seq, const ultra_pixel_fx_t *f)
{
    uint32_t i;
    ultra_slot_begin(s, ULTRA_PIXEL_FX, segment, 0, seq);
    s[ULTRA_PX_EFFECT] = f->effect;
    ultra_put32(s + ULTRA_PX_START, (uint32_t)f->start_ms);
    s[ULTRA_PX_SPEED] = f->speed;
    s[ULTRA_PX_INTENSITY] = f->intensity;
    s[ULTRA_PX_PALETTE] = f->palette;
    for (i = 0; i < 9u; i++) {
        s[ULTRA_PX_C1 + i] = f->c[i / 3u][i % 3u];
    }
    s[ULTRA_PX_BRIGHTNESS] = f->brightness;
    ultra_put16(s + ULTRA_PX_TRANSITION, f->transition_ms);
    ultra_put16(s + ULTRA_PX_SEG_START, f->seg_start);
    ultra_put16(s + ULTRA_PX_SEG_LEN, f->seg_len);
    s[ULTRA_PX_FLAGS] = (uint8_t)(f->flags & 0x3Fu);
    s[ULTRA_PX_CUSTOM1] = f->custom1;
    s[ULTRA_PX_CUSTOM2] = f->custom2;
    ultra_slot_seal(s);
}

/* a CRC-checked slot of type ULTRA_PIXEL_FX into f: the segment (LIGHT) 0 .. ULTRA_PX_SEGMENTS - 1 or ULTRA_LIGHT_BOARD
 * (the whole strip), else -1 (LIGHT 4-14: the slot is ignored) */
static inline int ultra_pixel_fx_parse(const uint8_t *s, ultra_pixel_fx_t *f)
{
    const uint32_t light = (uint32_t)s[1] >> 4, flags = s[ULTRA_PX_FLAGS];
    uint32_t i;
    if (light >= ULTRA_PX_SEGMENTS && light != ULTRA_LIGHT_BOARD) {
        return -1;
    }
    f->effect = s[ULTRA_PX_EFFECT];
    f->start_ms = (int32_t)ultra_get32(s + ULTRA_PX_START);
    f->speed = s[ULTRA_PX_SPEED];
    f->intensity = s[ULTRA_PX_INTENSITY];
    f->palette = s[ULTRA_PX_PALETTE];
    for (i = 0; i < 9u; i++) {
        f->c[i / 3u][i % 3u] = s[ULTRA_PX_C1 + i];
    }
    f->brightness = s[ULTRA_PX_BRIGHTNESS];
    f->transition_ms = (uint16_t)ultra_get16(s + ULTRA_PX_TRANSITION);
    f->seg_start = (uint16_t)ultra_get16(s + ULTRA_PX_SEG_START);
    f->seg_len = (uint16_t)ultra_get16(s + ULTRA_PX_SEG_LEN);
    f->flags = (uint8_t)(flags & 0x3Fu);
    f->custom1 = s[ULTRA_PX_CUSTOM1];
    f->custom2 = s[ULTRA_PX_CUSTOM2];
    return (int)light;
}

/* page 0x81's CONFIG CRC: ultra_crc16 over (id, value u16) of ids 129 .. 139 in order; values[k] = id 129 + k */
static inline uint16_t ultra_px_config_crc(const uint16_t *values)
{
    uint8_t b[ULTRA_CFG_PX_COUNT * 3u];
    uint32_t k;
    for (k = 0; k < ULTRA_CFG_PX_COUNT; k++) {
        b[3u * k] = (uint8_t)(ULTRA_CFG_PX_FIRST + k);
        ultra_put16(b + 3u * k + 1u, values[k]);
    }
    return ultra_crc16(b, (uint32_t)sizeof b);
}

/* NOP: a valid slot that does nothing (keep-alive frames to boards that are locked to Ultra) */
static inline void ultra_slot_nop(uint8_t *s)
{
    ultra_slot_begin(s, ULTRA_NOP, 0, 0, 0);
    ultra_slot_seal(s);
}

/* DARK: 256 zero bits, deliberately not a valid slot (the CRC-16 of zeros is not zero), so every board ignores it.
 * The preamble uses it: a board still locked to WS2805 / WS2915 cuts its WS window anywhere in the slots (along a
 * chain the windows of WS2805 boards cover every bit of a slot), so any 1 bit there - like a NOP's CRC - would show
 * as a colour; zeros show dark. Boards lock to Ultra on the trailer headers of these frames (3 frames each). */
static inline void ultra_slot_dark(uint8_t *s)
{
    uint32_t i;
    for (i = 0; i < ULTRA_SLOT_BYTES; i++) {
        s[i] = 0;
    }
}

/* POLL (host side): in an RW frame (ultra_build_header_flags ULTRA_HDR_RW), board k's slot. index 0..slots-1 or
 * ULTRA_POLL_RELAY_ONLY; up to 23 argument bytes */
static inline void ultra_slot_poll(uint8_t *s, uint8_t seq, uint8_t index, uint8_t slots, uint8_t page, uint8_t rate,
                                   const uint8_t *arg, uint32_t nargs)
{
    uint32_t i;
    ultra_slot_begin(s, ULTRA_POLL, ULTRA_LIGHT_BOARD, 0, seq);
    s[ULTRA_POLL_INDEX] = index;
    s[ULTRA_POLL_SLOTS] = slots;
    s[ULTRA_POLL_PAGE] = page;
    s[ULTRA_POLL_RATE] = rate;
    for (i = 0; i < nargs && i < 23u; i++) {
        s[ULTRA_POLL_ARG + i] = arg[i];
    }
    ultra_slot_seal(s);
}

/* a slot (CRC-checked) is a valid POLL (the board's rule): 0, else -1 */
static inline int ultra_poll_valid(const uint8_t *s)
{
    uint32_t r = s[ULTRA_POLL_SLOTS], i = s[ULTRA_POLL_INDEX];
    return (s[0] == ULTRA_POLL && (s[1] >> 4) == ULTRA_LIGHT_BOARD && r - 1u < ULTRA_RW_MAX_SLOTS &&
            (i < r || i == ULTRA_POLL_RELAY_ONLY) && s[ULTRA_POLL_RATE] < ULTRA_RW_RATES) ? 0 : -1;
}

/* the slot pitch and the window W for a rate, us (host: next frame >= W + ULTRA_RW_HOLDOFF_US after the frame end).
 * The pitch: the burst's 33 bytes (the lead byte and the answer, 330 bit times) + 130 us for EARLY / LATE / START /
 * GUARD and the gap to the next slot */
static inline uint32_t ultra_rw_slot_us(uint8_t rate) { return ((uint32_t)330u << rate) + 130u; }   /* (32-bit) */
static inline uint32_t ultra_rw_window_us(uint8_t slots, uint8_t rate)
{
    return ULTRA_RW_FIRST_US + ultra_rw_slot_us(rate) * slots;
}
/* a digital port's window W_D, us after t_end: the hub listens this long at most (1,360 / 1,690 / 2,350 us) */
static inline uint32_t ultra_dp_window_us(uint8_t rate) { return ULTRA_RW_DIR_MAX_US + ultra_rw_slot_us(rate); }

/* an answer (32 bytes as received): 0 = SYNC and CRC valid */
static inline int ultra_answer_check(const uint8_t *a)
{
    return (a[0] == ULTRA_RSP_SYNC && ultra_crc16(a, 30) == ultra_get16(a + 30)) ? 0 : -1;
}

/* STATUS 21 / ACKS 21: ultra_crc16 over (id, value) of the CONFIG ids set (bit id of `valid`), in id order */
static inline uint16_t ultra_config_crc(uint32_t valid, const uint16_t *values)
{
    uint8_t b[ULTRA_CFG_COUNT * 3u];
    uint32_t id, n = 0;
    for (id = 0; id < ULTRA_CFG_COUNT; id++) {
        if (valid & ((uint32_t)1u << id)) {   /* (32-bit: ids >= 16 on a 16-bit int target) */
            b[n] = (uint8_t)id;
            ultra_put16(b + n + 1u, values[id]);
            n += 3u;
        }
    }
    return ultra_crc16(b, n);
}

/* ------------------------------------------------- switching boards that are forced to WS2805 / WS2915 to Ultra */
/* A board forced to a WS protocol (boot_protocol ws2915 / ws2805, config frame or console `protocol 1` / `2`) ignores
 * Ultra frames. The hub reaches it with the board's own WS config frames (firmware config.h): the message
 * MAGIC 0xD2A5 | CMD | ARG | CRC-8 (SMBus), in WS2805 format as the 5 channel bytes of each slot, in WS2915 format
 * after a 32-bit header as the 10-byte slot (the message, then its complement). A board takes a command after 3
 * identical frames, the same command again only after a normal frame. The hub can't know the boards' format, so
 * the sequence has both:
 *   WS2805: ULTRA_WSCFG_COPIES x "protocol = Ultra", then one normal frame (all zero: dark);  then the same in WS2915.
 * Padding: consecutive frames differ in length by 8 bits (1 or 2 zero bytes at the end, alternating). Every board
 * sees a frame minus whole bytes (40, 80 or 256 bits per board before it), so of two consecutive frames at most one
 * has a WS2805 or WS2915 length at any board: no board counts 3 frames of one WS protocol in a row, and every board
 * keeps its lock (Ultra boards and boards in auto locked to the other WS format). A board of the frame's format still
 * decodes its slot (its config frames need identical slots, not lengths).
 * Each board that takes it blinks the acknowledge pattern and runs forced Ultra at once (so it ignores the second
 * format). A board of the second format shows the first format's config frames as its slot: a colour flash of up
 * to ULTRA_WSCFG_COPIES frames, then dark. Then the hub sends the start-up preamble (ultra_preamble_frames) and sets
 * the boot protocol it wants (auto or Ultra) with CONFIG ULTRA_CFG_BOOT_PROTOCOL.
 * Limit: a board in Ultra mode blocks the first 256 bits of every frame, so WS boards behind it get the frames cut at
 * the wrong place and are not switched; neither are boards behind a board of the other WS format. */
#define ULTRA_WSCFG_MAGIC      0xD2A5u
#define ULTRA_WSCFG_PROTOCOL   0x01u   /* firmware CMD_PROTOCOL; ARG 3 = Ultra */
#define ULTRA_WSCFG_COPIES     4u      /* identical frames per format (a board needs 3) */
#define ULTRA_SWITCH_WS_FRAMES (2u * (ULTRA_WSCFG_COPIES + 1u))
/* bytes of the longest frame of the sequence for n boards */
static inline uint32_t ultra_switch_frame_bytes(uint32_t n) { return 4u + 10u * n + 2u; }

/* frame i (0 .. ULTRA_SWITCH_WS_FRAMES - 1) of the sequence for n boards into f; returns its length in bytes */
static inline uint32_t ultra_switch_frame(uint8_t *f, uint32_t i, uint32_t n)
{
    const uint32_t ws2915 = i > ULTRA_WSCFG_COPIES, k = ws2915 ? i - (ULTRA_WSCFG_COPIES + 1u) : i;
    const uint32_t len = (ws2915 ? 4u + 10u * n : 5u * n) + 1u + (i & 1u);   /* + 1 / 2 bytes of padding */
    uint8_t m[10];
    uint32_t b, j;
    for (j = 0; j < len; j++) {
        f[j] = 0;
    }
    for (j = 0; j < 10u; j++) {
        m[j] = 0;                                        /* the normal frame: all zero */
    }
    if (k < ULTRA_WSCFG_COPIES) {
        m[0] = (uint8_t)(ULTRA_WSCFG_MAGIC >> 8);
        m[1] = (uint8_t)(ULTRA_WSCFG_MAGIC & 0xFFu);
        m[2] = ULTRA_WSCFG_PROTOCOL;
        m[3] = 3u;
        m[4] = ultra_crc8(m, 4);
        for (j = 0; j < 5u; j++) {
            m[5u + j] = (uint8_t)~m[j];
        }
    }
    if (ws2915) {
        f[0] = 0xFFu;                                    /* header: gains 31 (the boards ignore them), check bits 0 */
        f[1] = 0xFFu;
        f[2] = 0xFFu;
        f[3] = 0x80u;
        for (b = 0; b < n; b++) {
            for (j = 0; j < 10u; j++) {
                f[4u + 10u * b + j] = m[j];
            }
        }
    } else {
        for (b = 0; b < n; b++) {
            for (j = 0; j < 5u; j++) {
                f[5u * b + j] = m[j];
            }
        }
    }
    return len;
}

#ifdef __cplusplus
}
#endif

#endif
