/* SPDX-License-Identifier: MIT
 * Copyright (C) 2026 QuinLED
 *
 * QuinLED Ultra protocol 0.3 (wire VERSION 3), this file: release 0.3.2-draft. Shared by the
 * board firmware (C) and the controllers (C / C++).
 * Master: include/ultra_protocol.h in https://github.com/intermittech/quinled-ultra-protocol (normative companion documents in docs/).
 * Every wire-3 copy must be byte-identical to its pinned draft commit or release
 * (v0.3.y = wire VERSION 3; while below 1.0, the release's MINOR equals the wire VERSION).
 *
 * Layers: transport-independent command/page bytes; Diff-Power uses per-port adaptive pulses, MSB first.
 * See docs/transport.md. Slow line profiles are NOT pixel waveforms; strip pins regenerate the selected chip timing.
 * Frame: N x SLOT (32 bytes, one per board) + HEADER (8 bytes, a trailer).
 *         Normal inter-frame LOW >= max(500 us, LINK.min_gap_us); RW release/window rules below.
 *         Each board takes the first slot of what it sees (it holds the output LOW for those 256
 *         bits, as in WS2805 mode) and forwards the rest, so the position in the chain is the address and every
 *         board sees one contiguous frame ending with the header. Frame bits = 256 * N + 64.
 *         (Why last: with the header first, each board's forwarding pause would leave a 1024 us LOW gap after the header,
 *         ending the frame at the next board, so the boards after the first would never get their slot.)
 * HEADER: MAGIC (2) | VERSION<<4 | FLAGS (1) | TIME (4) | CRC-8 (1, SMBus poly 0x07)
 *         TIME = host clock (us) modulo ULTRA_TIME_WRAP (2^22 x 1000 us, 69.9 min) at the end of the frame (the header's
 *         last bit); every board sees that moment within forwarding delays. FLAGS: ULTRA_HDR_* below. The wrap is whole
 *         ms, so every board's host ms (floor(TIME / 1000), unwrapped) sits on the host's own ms grid, whatever its boot
 *         epoch. A header with TIME >= ULTRA_TIME_WRAP is invalid. START offsets count from floor(TIME / 1000) ms: a
 *         host rounds an absolute start to its ms once, then subtracts the frame's host ms (docs/timing.md), so every
 *         item of one start lands on the same ms on every board.
 * SLOT:   TYPE+FLAGS (1) | TARGET (1) | SEQ (1) | PAYLOAD (27) | CRC-16 (2, CCITT-FALSE)
 *         All multi-byte fields big-endian. Values: 16-bit fractions 0..65535 = 0..1.
 * Each command carries a sequence number per (TARGET, type): the board applies a command once, so the host can
 * repeat it (redundancy) and re-send the current state periodically (keepalive / catch-up after a reset).
 * Preamble: before its first commands (start-up, resync after power-on) the host sends ultra_preamble_frames(N)
 *         frames of DARK slots (all zero, never valid) + header. Adaptive-PHY boards lock on these headers, one
 *         after another. Rate is detected from rising-edge periods before decoding; see docs/timing.md.
 *         Strip outputs must be blank before preamble. Idle frames afterwards use NOP slots.
 *
 * Compatibility rules for this draft: a board ignores what it doesn't know, so a feature
 * it lacks never disturbs it. Hosts validate targets, scopes and resource limits
 * against the descriptor before calling the byte builders.
 *  - TYPE bits 0..4 select the command, bits 5..6 hold its flags, bit 7 is broadcast.
 *    Unknown type or TARGET outside the descriptor's count: ignore the slot. SET alone uses TYPE flags.
 *    TARGET is a light/channel/output index 0..239, or 0xFF for board scope. 0xF0..0xFE are reserved.
 *    PIXEL_FX instead packs output 0..14 in bits 4..7 and segment 0..14 in bits 0..3;
 *    segment 15 means that whole output (including TARGET 0x0F for output 0).
 *  - Unknown CONFIG id/scope: skip that pair; known pairs still apply. Reserved command types: ignore as unknown.
 *    Unknown role: NONE. Reserved bits/bytes are sent zero and ignored unless explicitly must-understand.
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
#define ULTRA_VERSION      3u          /* incompatible wire version; other versions do not lock or execute */
#define ULTRA_HDR_BYTES    8u
#define ULTRA_SLOT_BYTES   32u
#define ULTRA_HDR_BITS     (ULTRA_HDR_BYTES * 8u)
#define ULTRA_TIME_WRAP    4194304000u /* TIME wraps here: 2^22 x 1000 us = 0xFA000000 (69.9 min), a whole number of ms */
#define ULTRA_SLOT_BITS    (ULTRA_SLOT_BYTES * 8u)
#define ULTRA_PAYLOAD      3u          /* payload offset in a slot */
#define ULTRA_PAYLOAD_LEN  27u
#define ULTRA_COMPONENTS   5u          /* logical colour components, never a board channel count */
#define ULTRA_BANK_SIZE    5u
#define ULTRA_TARGET_MAX   239u
#define ULTRA_TARGET_BOARD 0xFFu
#define ULTRA_TYPE_MASK    0x1Fu
#define ULTRA_TYPE_FLAGS   0x60u
#define ULTRA_PIXEL_OUTPUT_MAX 14u
#define ULTRA_PIXEL_SEG_ALL 15u
#define ULTRA_PIXEL_TARGET(output, segment) ((uint8_t)(((output) << 4) | (segment)))

/* header FLAGS (low nibble of byte 2). On an RW or EXT frame a board keeps its lock and clock sync; a board without a
 * return channel (every QuinLED Dig2Analog-Plus-5ch) ignores every slot of it, and a Diff-Power-UR-Analog-M
 * receiver answers its POLL of an RW frame without EXT (the return channel, below). An UPD frame a board leaves
 * to the update side (no normal-command clock sync or slot; update timing lock is separate), HINT it ignores. RW+UPD is reserved for UPD_STATUS, defined by ultra_update.h; normal slot parsers never process UPD. The hub sets none of them in normal frames (0x00).
 * The nibble is full: a later frame-level feature never gets a header bit of its own. An optional one sets
 * HINT and names itself in a broadcast slot (its own TYPE, 0x80-0xFF, after the last board's slot; the payload carries
 * it); several share HINT in one frame, one broadcast slot each, and a board uses those it knows and ignores the rest
 * (HINT with no hint named: nothing). A must-understand one sets EXT and names itself the same way (EXT's cost: the
 * rules above). The broadcast slot types are assigned here, as they come. */
#define ULTRA_HDR_RW       0x01u       /* a response window follows this frame (docs/timing.md) */
#define ULTRA_HDR_HINT     0x02u       /* optional: the frame's broadcast slots name its hints (none defined yet); a
                                          board that knows none of them processes the frame as usual */
#define ULTRA_HDR_EXT      0x04u       /* must understand: the frame's broadcast slots name its extensions (none defined
                                          yet); a board without one of them ignores every slot */
#define ULTRA_HDR_UPD      0x08u       /* firmware update session (ultra_update.h, docs/update.md) */

/* slot types */
enum {
    ULTRA_NOP = 0x00,          /* keepalive, nothing to do */
    ULTRA_LIGHT_DEF = 0x01,    /* light: channels, roles, gamma, min / max power */
    ULTRA_SET = 0x02,          /* light: target state + transition */
    ULTRA_CONFIG = 0x05,       /* target-scoped: up to 9 settings (id, value) */
    ULTRA_IDENTIFY = 0x06,     /* board: blink pattern */
    ULTRA_POLL = 0x07,         /* answer / relay in a response window (RW frames only: the return channel) */
    ULTRA_SCENE = 0x08,        /* light: stored power-on / signal-loss output levels */
    ULTRA_LIGHT_OPT = 0x09,    /* light: options (dim to warm) */
    ULTRA_PIXEL_FX = 0x0B,     /* output/segment: local pixel effect, descriptor capability gated */
    ULTRA_LIGHT_TYPES = 0x0B,  /* analog sequence state uses handled types only; no PIXEL_FX row */
    ULTRA_PIXEL_DATA = 0x0C,   /* byte fragments; TARGET=output; docs/transport.md */
    ULTRA_TYPES = 0x0D,        /* number of assigned command types; 0x0D..0x1F reserved */
    ULTRA_TYPE_BCAST = 0x80,   /* 0x80-0xFF: broadcast / extension slots only (after the last board's slot), never
                                  applied as a board's own slot. (The UPD frame's block types 0x81-0x87 are another
                                  namespace: ultra_update.h) */
};

/* Dispatch membership is separate from CRC validity. Unknown types are ignored. */
static inline int ultra_command_known(uint8_t type)
{
    type &= 0x9Fu;
    return type <= 0x02u || (type >= 0x05u && type <= 0x09u) ||
           type == 0x0Bu || type == 0x0Cu;
}

/* Return channel (ports advertising ULTRA_LINK_RETURN; docs/timing.md).
 * All times in us after t0 = the last falling edge of the RW frame as each board measures it locally
 * (the last edge the tail copy finds in the DIN ring, not the shared clock's phase), in host us (a board scales them
 * with the shared clock's measured rate). During the window forwarding is suspended (DOUT LOW, not high-Z):
 * DIN sees the relayed answers, and a re-armed plan would forward them downstream. Host: no RW frame before the chain
 * is locked (after the DARK preamble) and has sent headers for ULTRA_RW_LOCK_MS, normal POLLs never during an update session (UPD_STATUS has its own request), and never with EXT (until an extension defines it). On a normal Ultra line the host drives the line LOW from t0 for at least
 * ULTRA_RW_RELEASE_MIN_US and has stopped driving by t0 + ULTRA_RW_RELEASE_US; the pair then idles HIGH until a
 * board answers or the host takes the line back. A board that closes a frame by its LOW run (the Diff-Power-UR-Digital-M receiver on a normal Ultra line,
 * alone on it) needs that LOW: 2 us between the last pulse and the release HIGH is no frame end for it. (A digital port: below.) In one RW
 * frame every POLL carries the same R and rate;
 * the answer indices are unique, < R and rise along the chain, and the boards between the hub and an answering board
 * get ULTRA_POLL_RELAY_ONLY. Behind the last board that answers or relays, the slots may be of any type (those boards
 * don't take part and don't turn around). The next frame obeys both W + ULTRA_RW_HOLDOFF_US and the
 * processing LOW after forwarding resumes (ultra_rw_next_us), answers or not; a missing
 * answer (a lost POLL, a flash write just before the frame, a late main loop, a board's clock not rated yet) is retried
 * in a later window with a new SEQ. UID16 in the answers shows index shifts. Board: takes part when its own slot is a
 * valid POLL (ultra_poll_valid) and the header has RW and not EXT. It turns around in DIR_MIN..DIR_MAX, does the
 * turnaround check at CHECK_US (HIGH <= CHECK_HIGH_US). With receive echo available, LOW readback
 * means contention. With ULTRA_CLOCK_NO_ECHO (no own-transmit echo), own-TX RO is high-Z: sample
 * released HIGH while listening before enable, never interpret disabled RO as contention or success.
 * Single-master scheduling and guard intervals prevent normal collisions; faults cannot always be
 * detected. Observable contention suppresses the entire window. Keep the same check pulse and slot
 * schedule on both kinds; qualification records the limitation. Burst timing follows the constants:
 * line HIGH within s_i - EARLY .. s_i + LATE (slot 0: from s_0 -
 * EARLY0), the first start bit START_MIN .. START_MAX after it, then ULTRA_RW_LEAD_BYTES x ULTRA_RSP_LEAD and the
 * 32-byte answer, UART 8N1, LSB first, at the POLL's rate; the line LOW again (idle, a break at the hub's UART) by s_i
 * + pitch - GUARD. Hub: the turnaround checks' pulses reach it as junk UART bytes (CHECK_US .. ~1000 us); it discards
 * every byte before a SYNC without a framing error (any number of lead bytes), takes 32 bytes, and drops the answer on
 * a framing error or a CRC failure (ultra_answer_check). */
#define ULTRA_POLL_RELAY_ONLY 0xFFu    /* POLL index: relay, don't answer (the page and its arguments are ignored) */
#define ULTRA_RW_MAX_SLOTS    16u      /* INDEX is a nibble in the answer */
/* Recorded design limits (timing.md, return windows): the host holds LOW >= 60 us and has stopped driving by
 * 150 us (target 80); a board's slot plan re-arms after 200 us of quiet LOW. Change only by an explicit decision. */
#define ULTRA_RW_RELEASE_US   150u     /* host / sender has stopped driving (at the latest) ... */
#define ULTRA_RW_RELEASE_MIN_US 60u    /* ... and not before this (a normal Ultra line): LOW from the RW frame's last falling edge for at least this long (the aim: +20 us) */
#define ULTRA_RW_REARM_US     200u     /* every board (apps, bootloaders in recovery): the ordinary slot plan (hold the own slot,
                                          forward the rest) re-arms only after this much LOW with no edge after a frame, never in
                                          a released HIGH: past the release (RELEASE_US), so the boards behind see it, and before
                                          the late-release reject (ULTRA_DP_RELEASE_MAX_US) and the 280 us reset */
#define ULTRA_RW_DIR_MIN_US   250u     /* boards turn around between MIN and MAX */
#define ULTRA_RW_DIR_MAX_US   900u
#define ULTRA_RW_CHECK_US     920u     /* the turnaround check, 920-977 us (after DIR_MAX, before slot 0) */
#define ULTRA_RW_CHECK_HIGH_US 5u      /* ... the line HIGH at most this long */
#define ULTRA_RW_FIRST_US     1000u    /* response slot 0 */
#define ULTRA_RW_SLOT_US      500u     /* response slot pitch at 1 Mbaud (ultra_rw_slot_us: every rate) */
#define ULTRA_RW_EARLY_US     20u      /* a burst's line HIGH within s_i - EARLY .. s_i + LATE */
#define ULTRA_RW_EARLY0_US    5u       /* ... slot 0's from s_0 - EARLY0: its lead byte after the check pulses' junk
                                          bytes (with up to 15 us more hop delay to the hub than slot 0's) */
#define ULTRA_RW_LATE_US      80u
#define ULTRA_RW_START_MIN_US 2u       /* the first start bit (the lead byte's) START_MIN .. START_MAX after the line goes
                                          HIGH */
#define ULTRA_RW_START_MAX_US 10u
#define ULTRA_RW_GUARD_US     30u      /* the line LOW again by s_i + pitch - GUARD, the lead byte included (470 us at
                                          1 Mbaud) */
#define ULTRA_RW_END_US       80u      /* forward mode again W .. W + END (W = FIRST + R x pitch); a 50 us tick with
                                          +-200 ppm and the ISR latency needs more than 60 */
#define ULTRA_RW_HOLDOFF_US   200u     /* host: next frame >= window end + this */
/* Recorded design limit (timing.md, rate lock): no receiver transmits before
 * >= 1000 ms of valid headers after the preamble. Change only by an explicit decision. */
#define ULTRA_RW_LOCK_MS      1000u    /* host: the first RW frame this long after the chain's preamble */
#define ULTRA_RW_BAUD         1000000u /* UART 8N1, LSB first, line idle LOW between bursts (rate 0) */
#define ULTRA_RW_LEAD_BYTES   1u       /* a board sends exactly this many ULTRA_RSP_LEAD before SYNC */
#define ULTRA_RW_SYNC_IDLE_BITS 9u     /* SYNC's start bit follows >= this many bit times of HIGH (the lead byte's data
                                          and stop bits): the hub's UART is idle at SYNC, whatever it made of the
                                          check pulses and the short HIGH before the lead byte */
/* Digital ports (docs/timing.md): a receiver on a pixel line (the
 * Diff-Power-UR-Digital-M receiver, ULTRA_LINK_MARKER; one per port, so every frame holds N = 1 slot) takes Ultra frames behind a
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
#define ULTRA_DP_QUIET_US       500u   /* the LOW before a marker (a receiver still direct switches to delayed in it) */
#define ULTRA_DP_MARK_MIN_US    15u    /* the marker's HIGH (hub: 20 us); pixels never hold a HIGH this long */
#define ULTRA_DP_MARK_MAX_US    40u
#define ULTRA_DP_GAP_MIN_US     5u     /* the LOW between the marker and the frame (hub: 10 us) */
#define ULTRA_DP_GAP_MAX_US     20u
#define ULTRA_DP_AFTER_US       500u   /* the LOW after a frame without RW, before pixels */
/* Recorded design limits (timing.md, digital ports): a release starting after +250 us is no RW; the back-off
 * gives 3 quick polls, 3 more after 60 s, then none. Change only by an explicit decision. */
#define ULTRA_DP_RELEASE_MAX_US 250u   /* RW: the hub's release starts by t_end + this (aimed: ULTRA_RW_RELEASE_US) */
#define ULTRA_DP_RELEASED_US    20u    /* RW: the receiver drives only after the line was released this long */
#define ULTRA_DP_RETAKE_US      20u    /* pixels this long after the hub retakes the line */
#define ULTRA_DP_POWERUP_MS     1000u  /* no marker this long after a port's power-up or the start of its stream */
#define ULTRA_DP_UPD_LOW_MS     50u    /* an update: the line LOW this long after the ANNOUNCE's window */
#define ULTRA_DP_TRIES          3u     /* the back-off: polls in quick succession, the same after RETRY_S, then none */
#define ULTRA_DP_RETRY_S        60u
/* POLL slot (type ULTRA_POLL, TARGET 0xFF; the payload's offsets, as slot bytes) */
#define ULTRA_POLL_INDEX      3u       /* answer index 0..R-1, or ULTRA_POLL_RELAY_ONLY */
#define ULTRA_POLL_SLOTS      4u       /* R: response slots in this window, 1..ULTRA_RW_MAX_SLOTS */
#define ULTRA_POLL_PAGE       5u       /* ULTRA_PAGE_* */
#define ULTRA_POLL_RATE       6u       /* ULTRA_RW_RATE_* */
#define ULTRA_POLL_ARG        7u       /* page arguments, slot bytes 7..29 ("POLL argument 0" = slot byte 7) */
#define ULTRA_RW_RATE_1M      0u       /* 1 Mbaud: pitch 500 us */
#define ULTRA_RW_RATE_500K    1u       /* 500 kbaud: 835 us (segments over 50 m, longer chains) */
#define ULTRA_RW_RATE_250K    2u       /* 250 kbaud: 1505 us (beyond 100 m) */
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
    ULTRA_PAGE_IDENTITY = 0x04,        /* parts 0 board, 1 features, 2 immutable target/geometry */
    ULTRA_PAGE_FW_UPDATE = 0x05,
    ULTRA_PAGE_CALIBRATION = 0x06,     /* current sense: the model, connected load and limit in effect */
    ULTRA_PAGE_CAL_TEST = 0x07,        /* the stored test, in two parts (POLL argument 0: 0 = CH1-3, 1 = CH4-5 + rest;
                                          2-255: UNSUPPORTED) */
    ULTRA_PAGE_POWER = 0x08,           /* board current limit and overcurrent events (ULTRA_PW_*) */
    ULTRA_PAGE_SUPPLY = 0x09,          /* supply dips (ULTRA_SU_*) */
    ULTRA_PAGE_HEALTH = 0x0A,          /* channel health and the MCU temperature peak (ULTRA_HE_*) */
    ULTRA_PAGE_ENERGY = 0x0B,          /* input energy (ULTRA_EN_*) */
    ULTRA_PAGE_LIFETIME = 0x0C,        /* the board's lifetime record, its odometer (ULTRA_LT_*) */
    ULTRA_PAGE_DIAG = 0x0D,            /* diagnostics: arg0 0 running board, 1 banked fault/trend, 2 link/decline */
    ULTRA_PAGE_LIFE_SYNC = 0x0E,       /* the hub's lifetime counters for the board to merge (ULTRA_LS_*; the
                                          answer: the counters after it and the result) (ULTRA_FEAT_LIFE_SYNC) */
    ULTRA_PAGE_DESCRIPTOR = 0x0F,    /* const TLV bytes; POLL arg0 = 24-byte part */
    ULTRA_PAGE_PIXEL = 0x10,         /* part 0 counters, 1 configuration, 2 packet transfer */
    ULTRA_PAGE_PIXEL_FX = 0x11,      /* parts 0 state, 1 effect membership, 2 palette membership */
    ULTRA_PAGE_LIMITS = 0x12,        /* optional applied snapshot; V203 may answer UNSUPPORTED */
    ULTRA_PAGE_LINK_STATS = 0x13,    /* arg0 ingress, arg1 snapshot/clear, arg2 part 0/1; docs/timing.md */
    ULTRA_PAGE_UPD_STATUS = 0x86,    /* reserved for update replies; ultra_update.h */
    ULTRA_PAGE_BUSY = 0xFE, ULTRA_PAGE_UNSUPPORTED = 0xFF,
};
/* IDENTITY: POLL arg0 selects part. All integers big-endian; unused bytes zero.
 * Part 0: UID[12], firmware u32, bootloader version u8, board u8, revision u8,
 * wire u8, descriptor length u16, descriptor CRC16 u16.
 * Part 1: six u32 feature words; hosts retain all 192 bits, including unknown bits.
 * Part 2: target tuple[16], flash geometry[3] from immutable bootloader info,
 * record format u8, descriptor format u8, entry_10ms u8, zero[2].
 * A non-return board uses its firmware-exported descriptor via jig/sidecar, never an ID lookup.
 * Cache descriptor by (board, revision, complete target tuple, firmware, length, CRC).
 */
#define ULTRA_ID_PART_BOARD       0u
#define ULTRA_ID_PART_FEATURES    1u
#define ULTRA_ID_PART_TARGET      2u
#define ULTRA_ID_UID              0u
#define ULTRA_ID_FW_VERSION       12u
#define ULTRA_ID_BL_VERSION       16u
#define ULTRA_ID_BOARD            17u
#define ULTRA_ID_REVISION         18u
#define ULTRA_ID_WIRE             19u
#define ULTRA_ID_DESC_LEN         20u
#define ULTRA_IDT_RECORD_FORMAT   19u /* part 2 */
#define ULTRA_IDT_DESC_FORMAT     20u /* part 2 */
#define ULTRA_IDT_ENTRY_10MS       21u /* part 2, immutable boot-info bound, 5..255 */
#define ULTRA_ID_DESC_CRC         22u
#define ULTRA_ID_TARGET           0u
#define ULTRA_ID_GEOMETRY         16u
#define ULTRA_IDF_FEATURES        0u
#define ULTRA_IDF_WORDS           6u
#define ULTRA_IDF_BITS            192u
#define ULTRA_RECORD_FORMAT      2u
/* FW_UPDATE (0x05): application confirmation of its currently installed image.
 * All POLL arguments zero. DATA is big-endian; bytes 3 and 20..23 are zero.
 * Flags/trials come from valid metadata status records for this descriptor;
 * absent/revoked/invalid metadata or no update support answers UNSUPPORTED.
 * Confirm success only with VERIFIED|CONFIRMED plus matching image CRCs/length,
 * firmware version and the full IDENTITY tuple (docs/update.md).
 */
#define ULTRA_FU_FORMAT           0u /* u8, value 1 */
#define ULTRA_FU_FLAGS            1u /* u8, ULTRA_FU_F_*; unknown bits ignored */
#define ULTRA_FU_TRIALS           2u /* u8, count of trial records for this image */
#define ULTRA_FU_FW_VERSION       4u /* u32 */
#define ULTRA_FU_BODY_LEN         8u /* u32 */
#define ULTRA_FU_BODY_CRC         12u /* u32 */
#define ULTRA_FU_DESC_CRC         16u /* u32 */
#define ULTRA_FU_FORMAT_VERSION  1u
#define ULTRA_FU_F_VERIFIED      0x01u
#define ULTRA_FU_F_CONFIRMED     0x02u
#define ULTRA_FU_F_TRIAL_SEEN    0x04u /* at least one trial observed a valid host frame */
/* Banking: POLL arg3 (slot byte 10) selects groups of five on OUTPUTS, ACKS,
 * HEALTH, CAL_TEST, CALIBRATION and DIAG part 1. Arrays and mask bits are local
 * to bank b, global index = 5*b + local index. Incomplete final banks pad absent
 * values/mask bits with zero; ACKS absent rows have NEVER set. Board rows repeat.
 * CAL_TEST retains arg0 part 0/1 inside each bank. Pixel CALIBRATION, PIXEL and
 * PIXEL_FX use bank = output instead. A bank outside the applicable count, or
 * nonzero bank on an unbanked page, answers UNSUPPORTED. POWER's arg3 is an
 * existing acknowledgement byte, not a bank. Bank 0 preserves array byte layouts.
 */
#define ULTRA_POLL_BANK           10u
#define ULTRA_POLL_PART           7u
/* Descriptor format 1: see docs/descriptor.md for normative field tables.
 * Header: format,header_bytes,len:u16,crc16,board,n_lights,n_records,flags,variant (11 bytes).
 * CRC16 covers bytes 6..len-1. TLV length counts value bytes only. Maximum
 * blob length 6144 (256 parts); unknown TLV types are skipped by hosts. Hosts parse
 * known records by their minimum prefix and skip any appended tail. Receivers copy
 * board-firmware const bytes and never interpret TLVs. No receiver heap/buffer.
 */
#define ULTRA_DESC_FORMAT       1u
#define ULTRA_DESC_HEADER_BYTES 11u
#define ULTRA_DESC_VARIANT      10u
/* Assembly is an opaque u8 allocated within each board's firmware definition.
 * All 256 values are valid. Only exact equality authorizes an image; capabilities
 * and ratings come from TLVs, never from variant bits or product-name parsing. */
#define ULTRA_DESC_MAX_BYTES    6144u
#define ULTRA_DESC_CRC          4u
#define ULTRA_DESC_F_EXAMPLE    0x01u
#define ULTRA_DESC_F_QUALIFIED  0x02u
#define ULTRA_DESC_LINK         1u /* minimum 16 bytes; adaptive profile mask at 16 */
#define ULTRA_DESC_DOMAIN       2u /* value 13 bytes */
#define ULTRA_DESC_PWM          3u /* value 12 bytes */
#define ULTRA_DESC_PIXEL        4u /* value 31 bytes */
#define ULTRA_DESC_SENSE        5u /* minimum value 8 bytes; full_scale u16 at offset 6 */
#define ULTRA_DESC_TIMING       6u /* minimum 12 bytes; adaptive threshold tail: 19 */
#define ULTRA_DESC_RESOURCES    7u /* value 14 bytes */
#define ULTRA_DESC_NAME         8u /* UTF-8 product string, 1..63 bytes; no NUL */
#define ULTRA_LINK_KIND         10u
#define ULTRA_LINK_PHY          11u
#define ULTRA_LINK_MTU          12u /* u16 maximum protocol object bytes */
#define ULTRA_LINK_REPLY        14u /* NONE=0, SCHEDULED=1, REQUEST=2 */
/* PHY 0 L1 scheduling tail: byte 16 zero; complete prefix is 21 bytes.
 * Missing/incomplete tail or 0xFFFF bound = unknown, no synchronization claim.
 * Bounds include worst-load validation, queues, interrupt interference, clock
 * error and output/latch quantization. See docs/transport.md. */
#define ULTRA_LINK_LOCAL_LATENCY 17u /* u16 us: acceptance to command-engine dispatch */
#define ULTRA_LINK_LOCAL_JITTER  19u /* u16 us: maximum output lateness from local due time */
#define ULTRA_LINK_LOCAL_BYTES   21u
#define ULTRA_LINK_RS485        1u
#define ULTRA_LINK_UART         2u
#define ULTRA_LINK_SPI          3u
#define ULTRA_LINK_I2C          4u
#define ULTRA_LINK_SINGLE_WIRE  5u
#define ULTRA_LINK_OTHER        255u
#define ULTRA_PHY_NONE          0u
#define ULTRA_PHY_ADAPTIVE      1u
#define ULTRA_PHY_RAW_PIXEL     2u /* optional ingress-only profile, chip bounds separately qualified */
#define ULTRA_REPLY_NONE        0u
#define ULTRA_REPLY_SCHEDULED   1u
#define ULTRA_REPLY_REQUEST     2u
#define ULTRA_LINK_RETURN       0x01u
#define ULTRA_LINK_RELAY        0x02u
#define ULTRA_LINK_FORWARD      0x04u
#define ULTRA_LINK_CHAIN_END    0x08u  /* the port's last board: no FORWARD / RELAY (UPDATE_FWD may be set) */
#define ULTRA_LINK_MARKER       0x10u
#define ULTRA_LINK_UPDATE_FWD   0x20u
/* Board 1 Dig2Analog-Plus-5ch: FORWARD always set, RETURN/RELAY/CHAIN_END clear.
 * Only 3 A / 5 A channel assemblies; no chain/no-chain assembly bit. */
/* LINK bits 6..7 reserved; temperature location belongs to SENSE. */
#define ULTRA_CLOCK_RATE_LOCK   0x01u
#define ULTRA_CLOCK_HSI         0x02u
#define ULTRA_CLOCK_NO_ECHO     0x04u /* no collision observation during local transmission */
#define ULTRA_ENFORCE_HW_CUTOFF  0x01u
#define ULTRA_ENFORCE_HW_RESETTABLE 0x02u
#define ULTRA_ENFORCE_HW_CLIMIT 0x04u
#define ULTRA_ENFORCE_FW_LIMIT  0x08u /* measured in this domain */
#define ULTRA_ENFORCE_FW_MODEL  0x10u /* calibrated conversion into this domain */
#define ULTRA_ENFORCE_LOCAL_ONLY 0x20u /* firmware bits cover local rendering only */
#define ULTRA_LIMIT_UNKNOWN     0xFFFFu /* never unlimited; 0 rating = disabled */
#define ULTRA_DOMAIN_ROOT       15u
#define ULTRA_DOMAIN_INPUT      1u
#define ULTRA_DOMAIN_CONVERTED   2u
#define ULTRA_DOMAIN_EXTERNAL   3u
#define ULTRA_DOMAIN_REGULATED   4u
#define ULTRA_DOMAIN_OUTPUT     5u /* board rail; external installation must qualify separately */
#define ULTRA_PWM_CAL           0x01u
#define ULTRA_PWM_DITHER        0x02u
#define ULTRA_PWM_STAGGER       0x04u
#define ULTRA_PWM_SCENES        0x08u
#define ULTRA_PWM_SKIP_RATE     0x10u /* takes CONFIG 25: pulse skipping at 2 kHz (default) or down to 1 kHz */
#define ULTRA_SKIP_HZ_DEFAULT   2000u /* CONFIG 25 values */
#define ULTRA_SKIP_HZ_SLOW      1000u
#define ULTRA_SKIP_TABLE_MAX    64u   /* a SKIP_1K board's pattern table: at most 64 periods */
#define ULTRA_PIXEL_STREAM      0x01u
#define ULTRA_PIXEL_LOCAL       0x02u
#define ULTRA_PIXEL_CHAIN       0x04u
#define ULTRA_PIXEL_CAL         0x08u
#define ULTRA_PIXEL_MARKER      0x10u
#define ULTRA_PIXEL_PACKET      0x20u
#define ULTRA_SOURCE_LOCAL      0u
#define ULTRA_SOURCE_INGRESS(n) ((uint8_t)(1u+(n)))
#define ULTRA_SOURCE_MASK_LOCAL 0x01u
#define ULTRA_SOURCE_MASK_STREAM 0x02u
#define ULTRA_SENSE_CURRENT     1u /* mA */
#define ULTRA_SENSE_VOLTAGE     2u /* 10 mV */
#define ULTRA_SENSE_TEMPERATURE 3u /* 0.1 C */
#define ULTRA_SENSE_MEASURED    0x01u
#define ULTRA_SENSE_MODELLED    0x02u
#define ULTRA_SENSE_INPUT_SIDE   0x04u
#define ULTRA_SENSE_DIE        0x08u
#define ULTRA_SENSE_BOARD_TEMP   0x10u
#define ULTRA_SENSE_BOARD      0xFFu
#define ULTRA_TIMING_PENDING   0u
#define ULTRA_TIMING_QUALIFIED 1u
/* PIXEL part 1: applied output configuration; bank = output.
 * u16 currents are in that output's domain; measured input remains STATUS.
 */
#define ULTRA_PIXEL_CFG_CHIP       0u /* u8 */
#define ULTRA_PIXEL_CFG_BPP        1u /* u8 */
#define ULTRA_PIXEL_CFG_ORDER      2u /* u16 */
#define ULTRA_PIXEL_CFG_PIXELS     4u /* u16 */
#define ULTRA_PIXEL_CFG_FPS        6u /* u16 requested ceiling after clamping */
#define ULTRA_PIXEL_CFG_SOURCE     8u /* u8: local / ingress+1 */
#define ULTRA_PIXEL_CFG_LIMIT      9u /* u16 */
#define ULTRA_PIXEL_CFG_BYTES      11u /* u16 effective frame bytes */
#define ULTRA_PIXEL_CFG_CRC        13u /* u16 explicit output CONFIG entries */
#define ULTRA_PIXEL_CFG_FLAGS      15u /* bit0 count/FPS clamped; rest zero */
/* bytes 16..23 zero. PIXEL_FX part 0 arg1 selects four-segment group g:
 * RUNNING/SEQ arrays cover 4*g..4*g+3; absent entries zero. Other bytes repeat.
 * CALIBRATION arg1: 0 PWM, 1 pixel. Its arg0 remains the part, arg3 the bank.
 */
#define ULTRA_CAL_KIND_PWM    0u
#define ULTRA_CAL_KIND_PIXEL  1u
/* LIMITS DATA: query arg0 kind (0 domain, 1 PWM channel, 2 pixel output),
 * arg1 index; remaining arguments zero. Unknown scope/index: UNSUPPORTED.
 * Values are in the selected LED / domain units, never a raw input-side current.
 * Unknown values 0xFFFF. Applied user value is resolved, including zero=rating.
 */
#define ULTRA_LIMIT_KIND_DOMAIN 0u
#define ULTRA_LIMIT_KIND_CHANNEL 1u
#define ULTRA_LIMIT_KIND_PIXEL 2u
#define ULTRA_LI_KIND 0u
#define ULTRA_LI_INDEX 1u
#define ULTRA_LI_DOMAIN 2u
#define ULTRA_LI_ENFORCEMENT 3u
#define ULTRA_LI_RATING 4u /* u16 mA */
#define ULTRA_LI_USER 6u /* u16 mA */
#define ULTRA_LI_EFFECTIVE 8u /* u16 mA, model-converted ancestor constraints included */
#define ULTRA_LI_CURRENT 10u /* u16 mA, measured/modelled as quality states */
#define ULTRA_LI_VOLTAGE 12u /* u16 10 mV */
#define ULTRA_LI_TRIP 14u /* u16 mA, 0 none */
#define ULTRA_LI_QUALITY 16u /* bit0 measured, bit1 modelled, bit2 valid, bit3 local-only, bit4 limiting */
#define ULTRA_LI_PARENT 17u /* u8 domain parent, 15 root */
#define ULTRA_LI_CAL_LIMIT 18u /* u16 mA in this domain; bytes 20..23 zero */
/* Timing contract constants; all host-relative, independent of CPU clocks.
 * Recorded design limits below, each with its timing.md section: change only by
 * an explicit decision (test-vectors/check_rules.py guards them).
 * FRAME_GAP: the sender's reset / processing LOW, >= 500 us, and the floor of
 * every port gap, port_gap_us = max(500, LINK.min_gap_us) (four exact profiles;
 * update forwarding and entry). */
#define ULTRA_FRAME_GAP_US       500u
/* Profile IDs are ordered fastest to slowest; no global active timing. */
#define ULTRA_PROFILE_800 0u
#define ULTRA_PROFILE_600 1u
#define ULTRA_PROFILE_400 2u
#define ULTRA_PROFILE_200 3u
#define ULTRA_PROFILE_COUNT 4u
#define ULTRA_PROFILE_AUTO 255u
#define ULTRA_PROFILE_MASK 15u
#define ULTRA_PARLIO_SAMPLE_HZ 9600000u
/* Recorded design limits (timing.md, four exact profiles): the sender residual
 * HIGH / period error <= 10 ns; the qualified 300 ns stage delay of the return
 * path budget. Change only by an explicit decision. */
#define ULTRA_LINE_TX_TOL_NS 10u /* residual error around backend's actual ticks */
#define ULTRA_SAMPLE_QUANTUM_NS 29u
#define ULTRA_HOP_DELAY_NS 300u /* qualified electronics; not GUARANTEE typical propagation */
/* Boards per port, by port type. Each board passes the signal on through its
 * forwarding gate without re-timing, so pulse-width distortion adds up per hop
 * and the timing budget sets the count: never one limit for all ports.
 * Recorded design limits: change only by an explicit decision.
 * Diff-Power (RS-485) port: the 1-100 m GUARANTEE path budget covers eight
 * receivers (timing.md, the eight-board budget); a ninth fails.
 * Dig2Analog-Plus-5ch data pin: the D2A pin link model and its pin table cover
 * 50 boards (timing.md, Dig2Analog-Plus-5ch pin link model); more are refused. */
#define ULTRA_PORT_DIFF_POWER_MAX_BOARDS 8u
#define ULTRA_PORT_D2A_PIN_MAX_BOARDS 50u
/* The Diff-Power path budget (timing.md, the eight-board budget):
 * D(N) = HOP*N + FORWARD*(N-1) + sum(C_i), C_i <= CABLE per 1..100 m segment;
 * the GUARANTEE eight-board path uses 588 ns. All signs accumulate, no credit for
 * typical skew. Recorded design limits: change only by an explicit decision. */
#define ULTRA_CHAIN_GUARANTEE_HOP_NS 20u /* GUARANTEE per-hop width-error bound, mixed paths included */
#define ULTRA_CHAIN_FORWARD_NS 4u /* forwarding allowance per intermediate board */
#define ULTRA_CHAIN_HOP_DISTORTION_NS 24u /* per-hop allowance HOP + FORWARD, ns */
#define ULTRA_CHAIN_CABLE_DISTORTION_NS 50u /* per 1..100 m segment incl protection */
#define ULTRA_CHAIN_CABLE_DELAY_NS 538u /* per <=100 m segment */
#define ULTRA_CHAIN_USED_NS 588u /* eight hop allowances, seven forwarding allowances, cables */
#define ULTRA_LINK_PROFILE_MASK 16u /* optional LINK tail; absent means 800 only */
#define ULTRA_TIMING_PROFILE_MASK 12u /* TIMING tail; thresholds u16 at 13,15,17 */
#define ULTRA_LINK_WARN_MARGIN_NS 20u
#define ULTRA_LINK_UP_MARGIN_NS 60u
#define ULTRA_LINK_DOWN_WINDOWS 3u
#define ULTRA_LINK_UP_WINDOWS 60u
#define ULTRA_LINK_DWELL_MS 60000u
#define ULTRA_PATH_SKEW_US       15u
/* Recorded design limits (timing.md, rate lock): replies need a measured
 * host-relative rate within +-200 ppm; 2 s without valid timing observations
 * unlocks a receiver. Change only by an explicit decision. */
#define ULTRA_RATE_ERROR_PPM     200u
#define ULTRA_RATE_LOCK_LOSS_MS  2000u
#define ULTRA_UART_ERROR_PPM     15000u
#define ULTRA_UART_GAPS_US       8u
#define ULTRA_DE_RELEASE_US      5u
#define ULTRA_RW_DECLINE_UNLOCKED 0x01u
#define ULTRA_RW_DECLINE_LATE     0x02u
#define ULTRA_RW_DECLINE_BUSY     0x04u
#define ULTRA_RW_DECLINE_RATE     0x08u
#define ULTRA_RW_DECLINE_CONTENTION 0x10u
#define ULTRA_DG_PART_LINK        2u
#define ULTRA_DG_DECLINE          0u /* DIAG part 2: latched reasons; read clears reported snapshot */
/* DIAG part 2: byte 0 reasons, 1 rate-lock valid (0/1), 2 timing qualified (0/1),
 * byte 3 supported-rate mask, bytes 4..23 zero. This part is unbanked. */

/* LINK_STATS data, all u16 big-endian; no stats inferred for nonsampling links. */
#define ULTRA_LS_PORT       0u
#define ULTRA_LS_PHY        1u
#define ULTRA_LS_FLAGS      2u /* VALID=1, LOCKED=2, OVERFLOW=4, WARNING=8, RATE_REQUEST=0x10 */
#define ULTRA_LS_QUANTUM_NS 3u
#define ULTRA_LS_ZERO_MIN   4u
#define ULTRA_LS_ZERO_MAX   6u
#define ULTRA_LS_ZERO_MEAN  8u
#define ULTRA_LS_ONE_MIN    10u
#define ULTRA_LS_ONE_MAX    12u
#define ULTRA_LS_ONE_MEAN   14u
#define ULTRA_LS_ZERO_COUNT 16u
#define ULTRA_LS_ONE_COUNT  18u
#define ULTRA_LS_INVALID   20u
#define ULTRA_LS_EPOCH     22u
/* LINK_STATS part 1 shares the frozen part-0 epoch; see timing.md. */
#define ULTRA_LC_PORT 0u
#define ULTRA_LC_PROFILE 1u
#define ULTRA_LC_FLAGS 2u
#define ULTRA_LC_SUPPORTED 3u
#define ULTRA_LC_EPOCH 4u
#define ULTRA_LC_PERIOD_MIN 6u
#define ULTRA_LC_PERIOD_MAX 8u
#define ULTRA_LC_LOW_MIN 10u
#define ULTRA_LC_LOW_MAX 12u
#define ULTRA_LC_FRAMES 14u
#define ULTRA_LC_CRC_ERRORS 16u
#define ULTRA_LC_MISSING 18u
#define ULTRA_LC_SCALE_PPM 20u
#define ULTRA_LS_VALID 1u
#define ULTRA_LS_LOCKED 2u
#define ULTRA_LS_OVERFLOW 4u
#define ULTRA_LS_WARNING 8u
#define ULTRA_LS_RATE_REQUEST 0x10u /* lower rate may be needed; host decides */
#define ULTRA_PIXEL_DATA_BEGIN  0u
#define ULTRA_PIXEL_DATA_CHUNK  1u
#define ULTRA_PIXEL_DATA_COMMIT 2u
#define ULTRA_PIXEL_DATA_ABORT  3u
#define ULTRA_PIXEL_DATA_BYTES 24u
/* PIXEL_DATA payload: operation:u8, offset:u16, data[24]. BEGIN data starts
 * length:u16, CRC16:u16 then zeros; offset=0. SEQ is transfer ID, NOT a normal
 * per-type dedup key. CHUNK offset in bytes, last chunk zero-padded. COMMIT has
 * offset=length and all-zero data; ABORT offset/data zero. See transaction rules. */
static inline uint8_t ultra_bank_count(uint8_t count) { return (uint8_t)((count+4u)/5u); }
static inline uint8_t ultra_bank_mask(uint8_t count, uint8_t bank) {
    unsigned base=5u*bank, remaining;
    if(base>=count) return 0;
    remaining=count-base; if(remaining>5u) remaining=5u;
    return (uint8_t)((1u<<remaining)-1u);
}
/* Unknown/zero ratings never authorize a positive load. Zero request restores
 * the firmware rating, not an unlimited value. This helper returns no allowance
 * for unknown; a separately qualified model is needed before enabling it. */
static inline uint16_t ultra_lower_limit(uint16_t rating, uint16_t requested) {
    if(rating==ULTRA_LIMIT_UNKNOWN) return 0;
    return !requested || requested>rating ? rating : requested;
}

/* STATUS page DATA: measured board state:
 * the MCU (die) temperature from the MCU profile's internal sensor, int16 0.1 C, ULTRA_ST_NA = not valid (no good
 * conversion yet, or 5 bad ones in a row; core/mcutemp.h), and the 3.3 V rail from the MCU profile voltage reference, u16 mV (0 = not known).
 * The temperature's absolute offset varies per chip (up to ~+-20 C): a trend / over-temperature value. */
#define ULTRA_ST_MCU_TEMP         9u
#define ULTRA_ST_VDDA             15u
#define ULTRA_ST_NA               0x8000u
/* the rest of STATUS */
#define ULTRA_ST_FW_VERSION       0u     /* u24: major, minor, patch (0 = a development build) */
#define ULTRA_ST_BOARD            3u     /* u8: the hardware id, as IDENTITY 17 (its revision: IDENTITY part 0) */
#define ULTRA_ST_UPTIME           4u     /* u24: s since power-up (wraps after 194 days: compare modulo 2^24; after a
                                            restart it starts at 0 again, RESET has the cause) */
#define ULTRA_ST_RESET            7u     /* u8: the reset cause of this start, ULTRA_ST_RST_* (several may be set) */
#define ULTRA_ST_TEMP_BOARD       11u    /* int16: a board sensor, 0.1 C; ULTRA_ST_NA = none (ULTRA_SENSE_BOARD_TEMP) */
#define ULTRA_ST_VBUS             13u    /* u16: the supply / bus voltage, x 10 mV; 0xFFFF = not measured */
#define ULTRA_ST_CURRENT          17u    /* u16: the measured input current (~100 ms average), mA */
#define ULTRA_ST_MEASURED         19u    /* u8: ULTRA_ST_M_* */
#define ULTRA_ST_PWM_PRESET       20u    /* u8: the PWM preset in use (CONFIG 1), 0xFF = a raw period (CONFIG 2) */
#define ULTRA_ST_CONFIG_CRC       21u    /* u16: ultra_crc16 over explicit (TARGET,id,applied value u16), sorted by TARGET/id (ultra_config_crc: the host compares it with what it sent) */
#define ULTRA_ST_MODES            23u    /* u8: the protocol mode (CONFIG 22's values) << 4 | the stagger (CONFIG 7) */
#define ULTRA_ST_RST_PIN          0x01u  /* (normalized causes mapped by each MCU backend) */
#define ULTRA_ST_RST_POWER        0x02u
#define ULTRA_ST_RST_SOFTWARE     0x04u  /* also a firmware update's restart */
#define ULTRA_ST_RST_WATCHDOG     0x08u
#define ULTRA_ST_RST_WWDG         0x10u
#define ULTRA_ST_RST_LOW_POWER    0x20u
#define ULTRA_ST_M_VBUS           0x01u  /* STATUS 13 is measured */
#define ULTRA_ST_M_CURRENT        0x02u  /* STATUS 17 is measured (the current sense in use) */
#define ULTRA_ST_M_BOARD_TEMP    0x04u  /* STATUS 11 is measured */
#define ULTRA_ST_M_CALIBRATED     0x08u  /* the channel currents (OUTPUTS 10..19) come from a calibrated model */
/* OUTPUTS page DATA (optional: a board may answer ULTRA_PAGE_UNSUPPORTED) */
#define ULTRA_OU_LEVEL            0u     /* 5 x u16: CH1..CH5 output now, after every limit (what the PWM shows) */
#define ULTRA_OU_CURRENT          10u    /* 5 x u16: bank-local channel current contributions at the root input, modelled mA */
#define ULTRA_OU_DEFINED          21u    /* u8: lights with a LIGHT_DEF since power-up */
#define ULTRA_OU_SCENE            22u    /* u8: channels still showing the stored scene (bit n = CH n+1) */
#define ULTRA_OU_WHY              23u    /* u8: why the outputs differ from the lights' states now, ULTRA_OU_W_* */
#define ULTRA_OU_W_LIMIT          0x01u  /* the board limit reduces them */
#define ULTRA_OU_W_TRIPPED        0x02u  /* off after a trip */
#define ULTRA_OU_W_CH_LIMIT       0x04u  /* a channel limit (channel CONFIG 12) */
#define ULTRA_OU_W_DARK_HOLD      0x08u  /* IDENTIFY 3 */
#define ULTRA_OU_W_PATTERN        0x10u  /* an identify / acknowledge / version pattern */
#define ULTRA_OU_W_SOFT_START     0x20u  /* the soft start ramps them */
#define ULTRA_OU_W_SIGNAL_LOSS    0x40u  /* signal loss (CONFIG 8) */
#define ULTRA_OU_W_TEST           0x80u  /* a current calibration test or the test jig's pattern */
/* ACKS page (optional): the last SEQ taken per target and slot type. POLL arguments (slot bytes 7, 8, 9): three
 * slot types for the three columns (0 = LIGHT_DEF, SET, reserved respectively).
 * A column selecting a reserved type or PIXEL_FX MUST return SEQ=0 and NEVER=1 in every row,
 * including the board row, regardless of applied segment commands. ACKS has no
 * segment selector. Acknowledge segments only via PIXEL_FX part 0: arg3=output,
 * arg1=segment/4, ULTRA_PXG_SEQ[segment%4] is the last applied command's SEQ.
 * NEVER here does not mean the receiver lacks PIXEL_FX. */
#define ULTRA_AK_SEQ              0u     /* 18 x u8: targets lights 0..4 then the board (TARGET 0xFF), 3 columns each:
                                            byte 3 x target + column */
#define ULTRA_AK_NEVER            18u    /* u24: bit (3 x row + column) = no ACK here; SEQ byte is 0 */
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
/* STATUS byte 8: the board's power state now, so a plain STATUS poll shows it (details: POWER page). Bits 0-3 come
 * with ULTRA_FEAT_POWER_EVENTS: a board without it sends them 0, which says nothing (not "not limiting"). */
#define ULTRA_ST_STATE            8u
#define ULTRA_ST_LIMITING         0x01u  /* the board limit reduces the outputs (held 2 s after the last reduction) */
#define ULTRA_ST_TRIPPED          0x02u  /* outputs off after an overcurrent trip, until the retry */
#define ULTRA_ST_POWER_EVENT      0x04u  /* a power event is latched: a POWER page latch the host hasn't acknowledged */
#define ULTRA_ST_CH_LIMITING      0x08u  /* a channel limit (channel CONFIG 12) reduces a channel (held 2 s) */
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
#define ULTRA_PW_LIMIT_MA         14u    /* u16: board limit in effect, mA (0xFFFF = unknown, never unlimited) */
#define ULTRA_PW_SRC_CAUSE        16u    /* u8: limit source (ULTRA_PW_SRC_*) << 4 | last trip cause (ULTRA_PW_CAUSE_*) */
#define ULTRA_PW_EVENT_MA         17u    /* u16: measured board current when the last limit event started, mA */
#define ULTRA_PW_TRIP_MA          19u    /* u16: measured current at the last trip, mA, saturating (short circuit: the peak sample) */
#define ULTRA_PW_FACTOR           21u    /* u16: the limiter's scale now, 0..65535 = 0..1 (65535 = none, 0 = tripped) */
#define ULTRA_PW_RETRY_S          23u    /* u8: s until the retry after a trip (0 = not tripped, 255 = 255 s or more) */
#define ULTRA_PW_EV_LIMITED       0x01u  /* latched: a limit event */
#define ULTRA_PW_EV_OVERLOAD      0x02u  /* latched: the receiver reported an overload trip */
#define ULTRA_PW_EV_SHORT         0x04u  /* latched: the receiver reported a short-circuit trip */
#define ULTRA_PW_EV_SUPPLY_DIP    0x08u  /* latched: a supply dip (SUPPLY page) */
#define ULTRA_PW_SRC_BOARD        0u     /* limit source: the board's rating (also the CALIBRATION page's encoding) */
#define ULTRA_PW_SRC_USER         1u     /* limit source: CONFIG 21 / the stored setting */
#define ULTRA_PW_SRC_PWM          2u     /* limit source: the PWM operating preset */
#define ULTRA_PW_SRC_PROTECTION  3u     /* limit source: the declared protection ceiling */
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
#define ULTRA_SU_REF_10MV           2u     /* u16: the reference (the slow mean) now, 10 mV; 0 = not known yet */
#define ULTRA_SU_NOW_10MV           4u     /* u16: the supply now (16 ms average), 10 mV */
#define ULTRA_SU_DEEP_10MV          6u     /* u16: the deepest dip since power-up (lowest reading against its reference):
                                            its lowest reading, 10 mV */
#define ULTRA_SU_DEEP_PCT         8u     /* u8: ... that reading in % of its reference */
#define ULTRA_SU_DEEP_AGE         9u     /* u24: ... s since it began */
#define ULTRA_SU_LOGGED           12u    /* u8: dips in the log, 0..ULTRA_SU_LOG */
#define ULTRA_SU_EV_AGE           13u    /* u24: the selected dip: s since it began */
#define ULTRA_SU_EV_10MV            16u    /* u16: its lowest reading, 10 mV */
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
#define ULTRA_HE_DARK_2MA          16u    /* int16: the board current with every output off, over the stored test's zero
                                            (logic + offset), 2 mA per step, saturating */
#define ULTRA_HE_TEMP_PEAK        18u    /* int16: the highest MCU die temperature since power-up, 0.1 C (ULTRA_ST_NA =
                                            none) */
#define ULTRA_HE_TEMP_AGE         20u    /* u24: s since it (0xFFFFFF = none) */
#define ULTRA_HE_F_TESTED         0x01u  /* a test ran since power-up (the classes are its) */
#define ULTRA_HE_F_ABNORMAL       0x02u  /* a channel's class is ULTRA_HE_ABNORMAL */
#define ULTRA_HE_F_DARK           0x04u  /* unexpected measured current with every output commanded off */
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
#define ULTRA_EN_BOOT_MWH         0u     /* u32: since power-up, mWh; wraps modulo 2^32 (about 57 days at 65 A / 48 V) */
#define ULTRA_EN_TOTAL_WH         4u     /* u32: the board's lifetime, Wh (stored on the board at most hourly, and when
                                            its outputs go dark; a factory reset keeps it) */
#define ULTRA_EN_POWER_MW         8u     /* u32: the input power now, mW */
#define ULTRA_EN_SAVED_AGE        12u    /* u24: s since the lifetime value was last stored (0xFFFFFF = not since
                                            power-up) */
#define ULTRA_EN_VB               15u    /* u16: the measured LED supply (V_B), x 10 mV; 0xFFFF
                                            = not measured */
#define ULTRA_EN_PEAK_MA          17u    /* u16: the highest input current (~100 ms average) since this page was
                                            last answered, mA, saturating (an answer the host misses takes its window's peak) */
#define ULTRA_EN_SUPPLY_Z         19u    /* u16: the supply / cable impedance, mOhm: the supply's fall over the input
                                            current's rise across settled current steps (0: not known yet; saturating) */
/*      21-23                     reserved (0) */
/* CALIBRATION page byte 1: the current model in use (bits 0-3: 0 none, 1 learned, 2 calibrated) | how the last
 * calibration test (IDENTIFY 2) ended (bits 4-7, ULTRA_CA_END_*). A test that stopped early, or completed without a
 * load or a change (4, 5), stores nothing: the calibration before it stays in use */
#define ULTRA_CA_MODEL            1u
#define ULTRA_CA_END_COMPLETE     0u     /* complete, or no test since power-up */
#define ULTRA_CA_END_TRIP         1u     /* stopped: a trip */
#define ULTRA_CA_END_LIMIT        2u     /* stopped: the installation's limit (CONFIG 21) reached, never above it */
#define ULTRA_CA_END_SUPPLY       3u     /* stopped: a supply dip */
#define ULTRA_CA_END_NOLOAD       4u     /* complete, not taken: no channel drew 20 mA at 100 % */
#define ULTRA_CA_END_FLAT         5u     /* complete, not taken: no step moved off the zero (sense stuck / nothing) */
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
#define ULTRA_LT_CURRENT_PEAK     17u    /* u8: the highest input current (~100 ms average), 0.5 A, saturating */
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
 * (the trips, the energy) plainly (a lower value never). The steps (the Diff-Power-UR-Analog-M receiver):
 * powered time 30 days; lit time the powered time this merge raised + 0.1 h; power-ups, trips and dips one per 10 s
 * of the powered time this merge raised + 64; the energy at input rating * maximum input voltage over the powered time this merge raised + 1 h; none
 * past the counter's room (the receiver's seconds and Wh are 32-bit: powered / lit time stop at 11,930,464 x 0.1 h,
 * the energy at 4,294,967,295 Wh, for good: a CAPPED there is no backlog). They limit what a wrong ledger can do; the
 * ledger's correctness is the hub's: its own accounting, not an echo of these reads. A merged counter counts as stored: it
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
/*      17-18                     reserved (0) */
#define ULTRA_DG_FLASH_ERRS       19u    /* u8: flash operations failed in a row (0: the last one succeeded) */
#define ULTRA_DG_FLASH_PENDING    20u    /* u8: records waiting to be written, ULTRA_DG_FP_* */
#define ULTRA_DG_SAVE_WAIT        21u    /* u8: why the settings / calibration / scene wait, ULTRA_DG_SW_* */
#define ULTRA_DG_INPUT_LOAD      22u    /* u8: reported input load, % of its qualified operating limit (0xFF: not measured) */
#define ULTRA_DG_INPUT_LOAD_PEAK 23u    /* u8: peak reported input load, % (0xFF: not measured) */
#define ULTRA_DG_FP_SETTINGS      0x01u
#define ULTRA_DG_FP_CALIBRATION   0x02u
#define ULTRA_DG_FP_SCENE         0x04u
#define ULTRA_DG_FP_LIFETIME      0x08u  /* a write is due, waiting for the flash's admission and the write budget (a
                                            regular one also for the outputs to go dark; also a supply dip's
                                            write that couldn't be made then, which doesn't wait for dark) */
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
                                            peak), 2 A (bits 2-7, saturating); 0 = none */
#define ULTRA_DG_TRIP_LIT_H       1u     /* u16: ... the lit hours then (LIFETIME's lit time, saturating) */
#define ULTRA_DG_DIP_PCT          3u     /* u8: the last supply dip: its lowest reading, % of its reference; 0 = none */
#define ULTRA_DG_DIP_10MS         4u     /* u8: ... how long, x 10 ms (255: longer) */
#define ULTRA_DG_DIP_CURRENT      5u     /* u8: ... the input current at its lowest, 0.5 A (saturating) */
#define ULTRA_DG_RST              6u     /* u8: the last abnormal reset: 1 watchdog, 2 an exception (then the watchdog)
                                            (bits 0-1) | the exception's mcause (bits 2-7); 0 = none */
#define ULTRA_DG_RST_UP_H         7u     /* u8: ... how long that run had been up, h (saturating) */
#define ULTRA_DG_TREND            8u     /* 5 x int8, CH1..CH5: the calibration trend, the newest stored test at 100 %
                                            against the first, % (ULTRA_HE_NA: not known) */
#define ULTRA_DG_TREND_LIT_H      13u    /* u16: ... over how many lit hours (0xFFFF: not known) */
#define ULTRA_DG_TREND_TESTS      15u    /* u8: tests in that history (0: not known) */
/* the firmware's features: IDENTITY part 1's word 0 (bits 0-31) as masks (ultra_feat_has: any bit, 0-191) */
#define ULTRA_FEAT_SCENE          0x00000001u  /* SCENE: two stored output levels */
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
#define ULTRA_FEAT_POWER_EVENTS   0x00000800u  /* POWER page and STATUS byte 8 bits 0-3 */
#define ULTRA_FEAT_SUPPLY_DIPS    0x00001000u  /* SUPPLY page, POWER latch ULTRA_PW_EV_SUPPLY_DIP */
#define ULTRA_FEAT_HEALTH         0x00002000u  /* HEALTH page */
#define ULTRA_FEAT_ENERGY         0x00004000u  /* ENERGY page */
#define ULTRA_FEAT_SOFT_START     0x00008000u  /* CONFIG 23 */
#define ULTRA_FEAT_LIFETIME       0x00020000u  /* LIFETIME page */
#define ULTRA_FEAT_DIAG           0x00040000u  /* DIAG page */
#define ULTRA_FEAT_RETRY          0x00080000u  /* CONFIG 24: the trips' retry policy */
#define ULTRA_FEAT_LOSS_SCENE     0x00100000u  /* CONFIG 8 value 3: signal loss fades to its stored level */
#define ULTRA_FEAT_STANDBY        0x00200000u  /* IDENTIFY 7: the planned power-off notice */
#define ULTRA_FEAT_LIFE_SYNC      0x00400000u  /* LIFE_SYNC page: the hub's lifetime counters merged */
#define ULTRA_FEAT_PIXEL_CALIBRATION 0x00800000u /* Pixel outputs: explicit current test + address count; page 0x06 parts 0..2 */

/* Digital calibration, per pixel output (TARGET = output; POLL bank = output).
 * Send IDENTIFY 2 to the output, argument 0, ALL remaining payload bytes zero.
 * No rating/profile field. A new SEQ starts once; repeated SEQ never restarts a job.
 * IDENTIFY 4 cancels/restores; IDENTIFY 6 clears the digital model (durable tombstone,
 * no test/no output change). The hub stops its pixel stream/frequent keep-alives,
 * leaves >=48 ms quiet intervals and uses sparse protocol/progress traffic (<=1 Hz),
 * polls until DONE/FAILED, then resumes with its latest frame;
 * host deadline 50 + ceil(1.5 * descriptor cal_ms) ms.
 * Never start during update, flash, undecided AUTO, dip or invalid supply/config.
 * A replacement failure retains a matching old model. No CAL_TEST/HEALTH promise.
 * DATA integers are big endian. Currents are bus INPUT mA, coefficients exclude
 * aggregate idle; use input-domain coefficients without further conversion. Logical R,G,B,W/WW,CW order.
 * Part 0: 0 levels (3 valid, else 0); 1 source (low nibble 2 valid, else 0) |
 * last error (high nibble ULTRA_UCE_*); 2 u16 successful count; 4 u24 age seconds
 * (0xFFFFFF unknown); 7 i16 aggregate idle in saturating 2 mA steps; 9 five u16 whole USED-strip coefficients;
 * 19 u16 saturated coefficient sum; 21 u16 effective limit; 23 reserved zero on UR_DIGITAL_M.
 * Read part 1 -> 0 -> 2 -> 1; accept identical count/epoch/fingerprint/flags and
 * matching part-0/2 count and part-2 epoch. Unknown arguments: UNSUPPORTED.
 * Count means addressable IC/groups, not packages. Fewer than configured: short
 * strip, data break or wrong type; cap: at least. Used=min(configured,detected).
 * Off still includes idle; powered-off does not. Voltage >5% off reference: stale.
 * Part 1 (metadata/progress): */
#define ULTRA_UC_FORMAT          2u
#define ULTRA_UC_FMT             0u     /* u8 = ULTRA_UC_FORMAT */
#define ULTRA_UC_FLAGS           1u     /* ULTRA_UCF_* */
#define ULTRA_UC_PHASE           2u     /* ULTRA_UCS_* */
#define ULTRA_UC_ERROR           3u     /* ULTRA_UCE_* */
#define ULTRA_UC_COUNT           4u     /* u16 successful-test generation, wraps */
#define ULTRA_UC_CONFIGURED      6u     /* u16 configured/applied address count */
#define ULTRA_UC_DETECTED        8u     /* u16 detected address count, 0 unknown/empty */
#define ULTRA_UC_USED           10u     /* u16 coefficient normalization count */
#define ULTRA_UC_CHIP           12u     /* u8 CONFIG 64 */
#define ULTRA_UC_BPP            13u     /* u8 3/4/5 */
#define ULTRA_UC_ORDER          14u     /* u16 resolved ULTRA_PX_ORDER */
#define ULTRA_UC_VBUS           16u     /* u16 reference 10 mV */
#define ULTRA_UC_VLED           18u     /* u16 LED-supply reference, 10 mV */
#define ULTRA_UC_EPOCH          20u     /* u16 changes on invalidation/success */
#define ULTRA_UC_ELAPSED        22u     /* u16 job elapsed ms, includes acquisition */
/* Part 2 (quality): 0 format; 1 algorithm=1; 2 u16 count; 4 u16 epoch;
 * 6 u16 K; 8 u16 noise guard mA; 10 u16 max residual permille;
 * 12 u16 derived upper bound mA/channel/address; 14..23 zero.
 * Residual/repeat tolerance max(5 mA,10%); weak present channels fail, not zero.
 * I0+G<T, L=effective input-domain ceiling from the generated rating and user limit, G=max(50,6*sigma) mA,
 * T=min(70% L,L-200). Start one address/channel at raw byte 64, predict with
 * margin before escalation; guard at 80% L, over-prediction aborts to full black.
 * All samples are fresh, weighted; answer overlap restarts the block. Channel
 * blocks discard 16 ms + two 16 ms means; count pulses use 4 + 6 + 6 ms and
 * bracketing black pulses (linear drift subtraction, full-cap clearing frames).
 * Explicit jobs only, <=5 s measurement deadline; saving waits for natural dark.
 * This is empirical qualification, not hardware short-circuit protection. */
#define ULTRA_UCF_VALID         0x01u
#define ULTRA_UCF_PENDING       0x02u
#define ULTRA_UCF_STALE         0x04u
#define ULTRA_UCF_AGGREGATE     0x08u
#define ULTRA_UCF_SHORT         0x10u
#define ULTRA_UCF_AT_LEAST      0x20u
#define ULTRA_UCF_WRONG_BPP     0x40u /* reserved for byte-boundary evidence; never inferred from SHORT alone */
#define ULTRA_UCF_CLEAR_PENDING 0x80u
enum { ULTRA_UCS_IDLE=0, ULTRA_UCS_QUEUED=1, ULTRA_UCS_ACQUIRE=2,
       ULTRA_UCS_DARK=3, ULTRA_UCS_PROBE=4, ULTRA_UCS_CHANNELS=5,
       ULTRA_UCS_MIXED=6, ULTRA_UCS_VALIDATE=7, ULTRA_UCS_RESTORE=8,
       ULTRA_UCS_DONE=9, ULTRA_UCS_FAILED=10 };
enum { ULTRA_UCE_NONE=0, ULTRA_UCE_LIMIT=1, ULTRA_UCE_SUPPLY=2,
       ULTRA_UCE_NOISE=3, ULTRA_UCE_NONLINEAR=4, ULTRA_UCE_CONFIG=5,
       ULTRA_UCE_CANCELLED=6, ULTRA_UCE_TIMEOUT=7, ULTRA_UCE_ENCODER=8,
       ULTRA_UCE_PREDICTION=9, ULTRA_UCE_ADC=10, ULTRA_UCE_WEAK=11,
       ULTRA_UCE_BUSY=12, ULTRA_UCE_ARGUMENT=13, ULTRA_UCE_EMPTY=14,
       ULTRA_UCE_OVERFLOW=15 };
/*      0x01000000-0x80000000     bits 24-31, then words 1-5 (bits 32-191): later official features, in order (every
 *                                 bit official, none hardware-specific) */
/* feature bit `bit` (0-191) in IDENTITY part 1's DATA f (ULTRA_IDF_FEATURES): 1 = the firmware has it */
static inline int ultra_feat_has(const uint8_t *f, uint32_t bit)
{
    return bit < ULTRA_IDF_BITS && ((f[4u * (bit >> 5) + 3u - ((bit & 31u) >> 3)] >> (bit & 7u)) & 1u);
}

/* SET flags (TYPE bits 5..6 of byte 0; the other slot types send FLAGS 0) */
#define ULTRA_SET_SMOOTH   0x20u       /* ESPHome's smoothstep curve (6x^5 - 15x^4 + 10x^3), else linear */
#define ULTRA_SET_VIA_OFF  0x40u       /* color mode change: fade to 0, switch, fade up (ESPHome behaviour) */
#define ULTRA_SET_FLAGS    (ULTRA_SET_SMOOTH | ULTRA_SET_VIA_OFF)

/* Current fields saturate, never wrap: unsigned u16 mA to 65535 (65534 where
 * FFFF means unknown); u8 0.5 A to 127.5 A; six-bit 2 A to 126 A;
 * signed i16 2 mA to +65.534 A. Quantize magnitude toward zero, then clamp.
 * Every current peak is saturating. A saturated ADC is displayed >= SENSE full scale.
 * LIFE_SYNC energy step uses ceil(rating_ma * v_max_10mv / 100) mW with wide
 * arithmetic. Unknown input rating/voltage rejects the energy merge (no guessed 1 kW).
 * CONFIG allocation: 0 unused; board 1..10 and 21..63; channel 11..20;
 * output 64..127; 128..255 reserved. Unassigned IDs stay within their scope.
 */
/* channel roles (LIGHT_DEF); 8-15 reserved (e.g. amber, UV, lime): a board takes them as ULTRA_ROLE_NONE */
enum {
    ULTRA_ROLE_NONE = 0, ULTRA_ROLE_BRIGHTNESS = 1, ULTRA_ROLE_RED = 2, ULTRA_ROLE_GREEN = 3,
    ULTRA_ROLE_BLUE = 4, ULTRA_ROLE_WHITE = 5, ULTRA_ROLE_COLD_WHITE = 6, ULTRA_ROLE_WARM_WHITE = 7,
};

/* Reserved: analog command types 0x03, 0x04, 0x0A and their effect IDs 0..255.
 * OUTPUTS byte 20, DIAG flash-pending bit 0x10 and feature bit 0x00010000
 * are reserved, never reused. Receivers ignore reserved commands as unknown. */

/* CONFIG ids (value 16-bit). Host-managed: RAM only, they end with Ultra mode - except the ids marked
 * "persisted", which the board stores in flash (written only when the value differs from the stored one). */
enum {
    ULTRA_CFG_NONE = 0,
    ULTRA_CFG_PWM_PRESET = 1,      /* 0..14 */
    ULTRA_CFG_PWM_PERIOD = 2,      /* period in 10 ns units, 1..65535; rounded to a supported timer period; 0 = use the preset */
    ULTRA_CFG_DITHER = 3,          /* 0 on, 1 off at low levels, 2 off */
    ULTRA_CFG_DITHER_THRESH = 4,   /* 1..255 = 0.1..25.5 % */
    ULTRA_CFG_DIM_FLOOR = 5,       /* 0 skip, 1 clamp, 2 off */
    ULTRA_CFG_MIN_PULSE = 6,       /* 5..100 x 10 ns */
    ULTRA_CFG_STAGGER = 7,         /* 0 off, 1 fixed (thirds), 2 fixed + spread spectrum,
                                      3 adaptive (least LED_+ ripple), 4 adaptive + spread spectrum */
    ULTRA_CFG_LOSS = 8,            /* 0 hold, 1 off, 2 fade off, 3 scene: fade to the stored signal-loss
                                      level (default all zero = dark) */
    ULTRA_CFG_LOSS_TIMEOUT = 9,    /* x 100 ms, 5..600 */
    ULTRA_CFG_FEEDBACK = 10,       /* identify channel: 0 red, 1 all, 2 off */
    ULTRA_CFG_CH_CURRENT = 11,     /* channel TARGET: full-scale LED-domain model current, mA */
    ULTRA_CFG_CH_MAX = 12,         /* channel TARGET: user ceiling, mA; 0 restores rated ceiling */
    ULTRA_CFG_BOARD_MAX = 21,      /* board TARGET: root input-domain user ceiling, mA; 0 = rating */
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
    ULTRA_CFG_SKIP_RATE = 25,      /* the slowest repeat of the pulse-skipping patterns (levels below the minimum pulse,
                                      CONFIG 5 / 6), Hz: 2000 (the default) or 1000 (ULTRA_SKIP_HZ_*); other values are
                                      ignored. Only a board whose PWM banks have ULTRA_PWM_SKIP_RATE takes it. Its tables
                                      hold at most ULTRA_SKIP_TABLE_MAX periods: from 64 kHz up the slowest rate is
                                      f_pwm / 64 (70.31 kHz 1.10 kHz, 100 kHz 1.56 kHz). Slower patterns reach lower levels
                                      with full-size pulses (at 1 kHz about half the 2 kHz level) */
    ULTRA_CFG_DOMAIN_FIRST = 32,   /* board TARGET: id 32+d sets domain d (0..14), mA; 0 = rating.
                                      Domain 0 is the input domain; CONFIG 21 is its canonical id.
                                      Id 32 is reserved to avoid two names for the same setting. */
    ULTRA_CFG_MCU_TEMP_OFFSET = 48, /* reserved, not settable */
    /* Pixel settings use TARGET = output, independent of board id. */
    ULTRA_CFG_PX_CHIP = 64,           /* the strip's chip (ULTRA_PXC_*): its timing, bytes per pixel and colour order */
    ULTRA_CFG_PX_ORDER = 65,          /* the wire order of the channels (ULTRA_PX_ORDER; 0 = the chip's own) */
    ULTRA_CFG_PX_PIXELS = 66,         /* pixels on the strip (0 = none); clamped to the PIXEL descriptor limits by chip format */
    ULTRA_CFG_PX_FPS = 67,            /* requested FPS ceiling (default 60; 0 = fastest permitted by all descriptor budgets) */
    ULTRA_CFG_PX_MA_CH = 68,          /* LED current of one channel at 255, mA, at the strip's supply (0 = 20 mA): the
                                          limiter's estimate */
    ULTRA_CFG_PX_MAX = 69,       /* output-domain user ceiling, mA; 0 restores rated ceiling.
                                          Board input ceiling is CONFIG 21; conversions use the calibrated model. */
    ULTRA_CFG_PX_WHITE = 70,          /* bits 0-1: the default auto-white mode (ULTRA_PXW_*; 0 = accurate); bits 8-15: the
                                          WS2805 white split, the share of the cold white (0 = even) */
    ULTRA_CFG_PX_VOFFSET = 71,        /* this strip starts at this pixel of a virtual strip (an effect continuing over
                                          ports and receivers) */
    ULTRA_CFG_PX_VLENGTH = 72,        /* ... the virtual strip's length (0 = this strip alone) */
    ULTRA_CFG_PX_POLICY = 73,         /* standalone behaviour (ULTRA_PXP_*) */
    ULTRA_CFG_PX_LOSS_TIMEOUT = 74,   /* x 100 ms with no pixel frame and no marker = signal loss, 5..600 (0 = 30) */
    ULTRA_CFG_PX_LINE_MODE = 75,      /* what the line carries (ULTRA_PXLM_*): 0 AUTO (the default: the board decides once per
                                          power-up from what it sees), 1 STREAM (a pixel line: the hub's pixels pass, polls
                                          behind markers), 2 CHAIN (a normal Ultra line: plain Ultra frames, the board takes
                                          the first slot it sees; separate physical stream ingresses remain usable) */
    ULTRA_CFG_PX_SMOOTH = 76,         /* pixel smoothing (ULTRA_PXS_*): how the receiver takes its colour to the strip's 8 bits. The 16-bit paths do the
                                          gamma, the brightness and the auto-white at 16 bits and round at the end; a dither adds an ordered, stateless
                                          threshold (host frame index and pixel index) first, so receivers on one grid make the same bytes and the average
                                          over 256 frames is the 16-bit value. 0 AUTO (the default: 16 bits; at 50 frames a second or more it dithers the channels at
                                          wire level 4 and up and rounds the dimmer ones, below 50 frames it rounds: ULTRA_PXS_AUTO_FPS and ULTRA_PXS_AUTO_MIN_LEVEL,
                                          both bench items), 1 ROUND, 2 DITHER (all levels), 3 OFF (8-bit processing) */
    ULTRA_CFG_PX_SOURCE = 77,        /* 0 local; 1..16 stream ingress 0..15; descriptor membership required */
    ULTRA_CFG_PX_FIRST = 64,
    ULTRA_CFG_PX_COUNT = 14,           /* ids 64..77 */
};

/* ----------------------------------------------------------------------------------- local pixel effects */
/* Pixel outputs are described individually. CONFIG 77 selects local (0) or a
 * physical stream ingress (1..16). PIXEL_FX never changes this routing setting:
 * effect 0 stops local rendering and holds black while source is local. Switching
 * source is applied only at a complete-frame latch; no partial frame is exposed.
 * One ingress serves at most one output. Two streams require two declared ingresses.
 * All local outputs share the transport's time origin and descriptor's budgets:
 * adaptive PHY uses the header's host ms; L1 kind 1 uses packet acceptance, no trailer TIME.
 * PIXEL_FX TARGET = output<<4 | segment; segment 15 replaces the entire output.
 * A segment index outside the output's count is ignored. Payload offsets below
 * are slot offsets: effect; signed start_ms; speed/intensity/palette; RGB c1/c2/c3;
 * brightness; transition_ms u16; segment start/length u16; flags; custom1/custom2.
 * Unknown effects show Solid. Duplicate suppression is per (output, segment, type).
 */
#define ULTRA_PX_STATE_SEGMENTS 4u /* four segments per PIXEL_FX state part */
/* Pixel/frame limits are descriptor fields, never universal constants. Actual
 * frames include every pixel and reset. Requests are clamped to the advertised
 * per-output and shared ceilings; PIXEL part 1 reports effective values. */
#define ULTRA_PX_DEFAULT_FPS  60u
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
#define ULTRA_PX_FX_STOP    0u       /* EFFECT 0: stop local rendering */
#define ULTRA_PX_PAL_COLORS   0xFEu    /* PALETTE: c1 -> c2 -> c3 -> c1 */
#define ULTRA_PX_PAL_DEFAULT  0xFFu    /* PALETTE: the effect's own */
/* FLAGS. STORE: this segment's fields (not START and SEQ) and CONFIG 64-76 become the power-on / standalone effect
 * (CONFIG 73); with EFFECT 0 the stored effect is cleared. The record is written only in stream mode or while the strip
 * is dark, never as a visible pause. LOCK: within a configured streamed source, a returning stream does not take the strip back
 * (otherwise, with ULTRA_PXP_STREAM_WINS, it does; CONFIG 77 never changes). RESTART: this slot is not a renewal of the running effect: the
 * time-driven effects begin again at START (so START must be new; the same START restarts no time) and the random ones
 * are re-seeded. Without it, the same effect with every parameter the same and only START new (the hub renewing an old
 * START before its signed 32-bit ms run out, ~every 20 days) changes nothing: phase and picture go on. RESTART is an
 * action, not a setting: STORE keeps it out of the stored effect. Bit 7: reserved, sent as 0 */
#define ULTRA_PXF_REVERSE     0x01u
#define ULTRA_PXF_MIRROR      0x02u
#define ULTRA_PXF_WHITE_MASK  0x0Cu    /* auto-white mode, ULTRA_PXW_*, of RGBW and RGBCCT strips (0: CONFIG 70's) */
#define ULTRA_PXF_WHITE_SHIFT 2u
#define ULTRA_PXF_STORE       0x10u
#define ULTRA_PXF_LOCK        0x20u
#define ULTRA_PXF_RESTART     0x40u
#define ULTRA_PXW_DEFAULT     0u       /* in FLAGS: the strip's own (CONFIG 70); in CONFIG 70: accurate */
#define ULTRA_PXW_OFF         1u       /* no white from the colour (white only where an effect asks for it) */
#define ULTRA_PXW_ACCURATE    2u       /* W = min(R, G, B), taken out of R, G, B: the colour stays true */
#define ULTRA_PXW_BRIGHT      3u       /* W = min(R, G, B), R, G, B kept: brighter, paler */

/* CONFIG 64: the chip (one preset: the timing on the MCU profile's declared bit grid, the bytes per pixel, the default
 * colour order; the strip's reset time). The presets of the V5 class share one timing */
#define ULTRA_PXC_WS2812B_V5      0u   /* WS2812B V5 / 2020, WS2811 V2.2 and later: RGB, wire order G R B */
#define ULTRA_PXC_WS2812B_CLASSIC 1u   /* WS2812B before V5: RGB, G R B */
#define ULTRA_PXC_SK6812_RGB      2u   /* SK6812, SK6812 B/1 RGB: G R B */
#define ULTRA_PXC_SK6812_RGBW     3u   /* SK6812 RGBW: G R B W */
#define ULTRA_PXC_WS2814_RGBW     4u   /* WS2814 (12 V: R G B W; the 24 V WS2814A / WS2814F strips are W R G B): the default
                                          is R G B W, ULTRA_CFG_PX_ORDER picks the other */
#define ULTRA_PXC_WS2805_RGBCCT   5u   /* WS2805 (RGB + warm + cold white, 5 bytes): R G B WW CW */
#define ULTRA_PXC_WS2815          6u   /* WS2815 (12 V, backup data line): G R B */
/* Value 7 is reserved: CONFIG 64 rejects it without changing the preset. */
#define ULTRA_PXC_COUNT           7u
/* CONFIG 65: wire byte k (0-4) = the channel in bits 3k .. 3k + 2 (ULTRA_PXCH_*); only as many bytes as the chip has,
 * each channel once; anything else is ignored. 0 = the chip's own order */
#define ULTRA_PXCH_R   0u
#define ULTRA_PXCH_G   1u
#define ULTRA_PXCH_B   2u
#define ULTRA_PXCH_W   3u              /* the white (WS2805: the warm white) */
#define ULTRA_PXCH_CW  4u              /* WS2805: the cold white */
#define ULTRA_PX_ORDER(b0, b1, b2, b3, b4) \
    ((uint16_t)((b0) | ((b1) << 3) | ((b2) << 6) | ((b3) << 9) | ((b4) << 12)))
/* CONFIG 73 (0 = none of them: the hub's stream is all the strip shows, as before). The stored effect is the one a
 * PIXEL_FX with STORE left */
#define ULTRA_PXP_POWER_ON     0x01u   /* from power-up: when the line stays quiet (no pixel frame, no marker) for 2 s,
                                          the stored effect fades in */
#define ULTRA_PXP_ON_LOSS      0x02u   /* signal loss (CONFIG 74) starts the stored effect (else the strip holds its last
                                          frame, as a WS28xx strip does) */
#define ULTRA_PXP_STREAM_WINS  0x04u   /* within a configured streamed source, a returning stream takes over its local fallback
                                          that isn't locked (ULTRA_PXF_LOCK): at the second frame, inside a latch */

/* Page ULTRA_PAGE_PIXEL_FX (0x11), three parts (POLL argument 0), big-endian. Part 0, the state: */
#define ULTRA_PXG_STATE        0u      /* part 0 */
#define ULTRA_PXG_CATALOGUE    0u      /* u16: the catalogue's version (catalogue.json) */
#define ULTRA_PXG_EFFECTS      2u      /* u8: effects the firmware has */
#define ULTRA_PXG_PALETTES     3u      /* u8: palettes */
#define ULTRA_PXG_CHIPS        4u      /* u8: chip presets (ULTRA_PXC_COUNT) */
#define ULTRA_PXG_SEGMENTS     5u      /* u8: total declared segments on this output, 1..15 */
#define ULTRA_PXG_MAX_PIXELS   6u      /* u16: the most pixels CONFIG 66 takes with the chip's bytes per pixel now */
#define ULTRA_PXG_MODE         8u      /* u8: ULTRA_PXM_* */
#define ULTRA_PXG_RUNNING      9u      /* 4 x u8: each segment's running effect (0 = none) */
#define ULTRA_PXG_SEQ          13u     /* 4 x u8: the SEQ of each segment's last PIXEL_FX applied */
#define ULTRA_PXG_FPS10        17u     /* u16: frames per second x 10 (local frames, the last second) */
#define ULTRA_PXG_LOAD         19u     /* u8: the CPU load of the last second, 0.5 % steps (0-200) */
#define ULTRA_PXG_LIMIT        20u     /* u8: the current limiter's brightness scale now, 255 = none */
#define ULTRA_PXG_LATE         21u     /* u8: frames that started after their grid time (saturating) */
#define ULTRA_PXG_CONFIG_CRC   22u     /* u16: CRC of explicitly set output CONFIG entries */
/* part 1: bit (id & 7) of byte id >> 3 (ids 0-191) = the effect id is in the firmware's catalogue; part 2 the same for
 * the palettes */
#define ULTRA_PXG_PART_STATE    0u
#define ULTRA_PXG_PART_EFFECTS  1u
#define ULTRA_PXG_PART_PALETTES 2u
#define ULTRA_PXM_STREAM        0u     /* the hub's pixels pass */
#define ULTRA_PXM_LOCAL         1u     /* a PIXEL_FX effect runs */
#define ULTRA_PXM_STANDALONE    2u     /* the stored effect runs (CONFIG 73: power-on, signal loss) */

/* Line mode selects the command transport on the output's control port:
 * AUTO determines plain Ultra CHAIN vs marker STREAM from valid traffic; silence
 * does not decide. CHAIN consumes a positional slot, STREAM uses marker frames.
 * Routing is separate (CONFIG 77). A control port shared by outputs has one line
 * mode: the last valid write updates the port and every attached output view.
 * A stream source sharing that control port is valid only in STREAM mode; reject
 * a source/mode change that would invalidate any active route.
 * A CHAIN_END port must be last; a middle port needs FORWARD. A downstream
 * return path also requires RETURN_RELAY; board 1 never supplies that path.
 */
#define ULTRA_PXS_AUTO          0u     /* CONFIG 76 values (pixel smoothing) */
#define ULTRA_PXS_ROUND         1u
#define ULTRA_PXS_DITHER        2u
#define ULTRA_PXS_OFF           3u
#define ULTRA_PXS_AUTO_FPS      50u    /* AUTO dithers from this frame rate (the frame grid's, or the measured one without a grid) */
#ifndef ULTRA_PXS_AUTO_MIN_LEVEL
#define ULTRA_PXS_AUTO_MIN_LEVEL 4u    /* ... and only a channel whose wire level is at least this (AUTO rounds the dimmer ones: a dither at the bottom flips a pixel between
                                          levels 1 and 0 at the frame rate, a visible shimmer at low rates; at level 4 one step is at most 25 % of the light); 0 = every
                                          channel (the packer's code for the level is then left out). Bench item, like the frame rate above
                                          ROUND / DITHER ignore it */
#endif
#define ULTRA_PXLM_AUTO         0u     /* CONFIG 75 values */
#define ULTRA_PXLM_STREAM       1u
#define ULTRA_PXLM_CHAIN        2u
/* PIXEL part 0: line and counters, 24 bytes, big-endian. These counters are
 * owned here; ERRORS on a pixel-only board answers UNSUPPORTED. */
#define ULTRA_PIXEL_PART_LINE   0u
#define ULTRA_PIXEL_PART_CONFIG 1u
#define ULTRA_PXL_CONFIG        0u     /* u8: CONFIG 75 as stored (ULTRA_PXLM_*) */
#define ULTRA_PXL_MODE          1u     /* u8: the mode in effect: 0 undecided (AUTO: no evidence yet; the strip is held for the first 2 s), 1 STREAM, 2 CHAIN */
#define ULTRA_PXL_FLAGS         2u     /* u8: ULTRA_PXL_F_* */
#define ULTRA_PXL_SLOTS         3u     /* u8: slots in the last Ultra frame ((bits - 64) / 256; 0 none; saturating) */
#define ULTRA_PXL_FRAMES        4u     /* u16: Ultra frames with a right header and length since power-up (wraps) */
#define ULTRA_PXL_NONULTRA      6u     /* u16: frames that are no Ultra frame (CHAIN or deciding: a pixel stream, noise; wraps) */
#define ULTRA_PXL_LONG          8u     /* u16: Ultra frames of more than one slot (boards behind this one; wraps) */
#define ULTRA_PXL_WINDOWS       10u    /* u16: RW windows this board took part in (a valid POLL for it; wraps) */
#define ULTRA_PXL_ANSWERS       12u    /* u16: ... answered (wraps) */
#define ULTRA_PXL_MISSED        14u    /* u16: ... a POLL for this board that could not be answered in its slot (late, flash, the clock's rate not measured yet; wraps) */
#define ULTRA_PXL_BITS          16u    /* u24: bits of the last Ultra frame */
#define ULTRA_PXL_MARKERS       19u    /* u16: marker frames seen while CHAIN (a pixel-line hub talking to a chain board; wraps) */
#define ULTRA_PXL_BUSY          21u    /* u16: relay windows given up at the turnaround: the line above still driven (no relay, no answer; wraps) */
#define ULTRA_PXL_F_LOCKED      0x01u  /* CHAIN: a right Ultra frame within the last 2 s */
#define ULTRA_PXL_F_NONULTRA    0x02u  /* a frame that is no Ultra frame was seen in this mode (cleared by a mode change) */
#define ULTRA_PXL_F_LONG        0x04u  /* a frame of more than one slot was seen (boards behind this one) */
#define ULTRA_PXL_F_MARKERS     0x08u  /* marker frames seen while CHAIN */
#define ULTRA_PXL_F_LIT         0x10u  /* the board is running a local effect */

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

static inline uint16_t ultra_crc16_update(uint16_t seed, const uint8_t *d, uint32_t n)
{
    static const uint16_t t[16] = { 0x0000, 0x1021, 0x2042, 0x3063, 0x4084, 0x50A5, 0x60C6, 0x70E7,
                                    0x8108, 0x9129, 0xA14A, 0xB16B, 0xC18C, 0xD1AD, 0xE1CE, 0xF1EF };
    uint32_t c = seed;
    while (n--) {
        c ^= (uint32_t)*d++ << 8;
        c = ((c << 4) & 0xFFFFu) ^ t[c >> 12];
        c = ((c << 4) & 0xFFFFu) ^ t[c >> 12];
    }
    return (uint16_t)c;
}

static inline uint16_t ultra_crc16(const uint8_t *d, uint32_t n)
{ return ultra_crc16_update(0xFFFFu,d,n); }

static inline void ultra_put16(uint8_t *p, uint32_t v) { p[0] = (uint8_t)(v >> 8); p[1] = (uint8_t)v; }
static inline void ultra_put24(uint8_t *p, uint32_t v) { p[0] = (uint8_t)(v >> 16); p[1] = (uint8_t)(v >> 8); p[2] = (uint8_t)v; }
static inline void ultra_put32(uint8_t *p, uint32_t v) { ultra_put16(p, v >> 16); ultra_put16(p + 2, v); }
static inline uint32_t ultra_get16(const uint8_t *p) { return ((uint32_t)p[0] << 8) | p[1]; }
static inline uint32_t ultra_get24(const uint8_t *p) { return ((uint32_t)p[0] << 16) | ((uint32_t)p[1] << 8) | p[2]; }
static inline uint32_t ultra_get32(const uint8_t *p) { return (ultra_get16(p) << 16) | ultra_get16(p + 2); }

/* ----------------------------------------------------------------------------------------------- frame */
/* layout (wire VERSION 3): slot i at byte ULTRA_SLOT_BYTES x i, the header after the n slots */
static inline uint32_t ultra_frame_bytes(uint32_t n) { return ULTRA_SLOT_BYTES * n + ULTRA_HDR_BYTES; }
static inline uint32_t ultra_header_offset(uint32_t n) { return ULTRA_SLOT_BYTES * n; }

/* Preamble frames for n boards. Acquire edge periods before classifying HIGHs,
 * then require 3 valid trailers; DARK slots cannot execute. Retain 4*n+2 for
 * cascaded acquisition/rejoin. Slot removal uses edge count even while acquiring.
 * Firmware validates acquisition at the full board count of each port type (8 on a
 * Diff-Power port, 50 on a Dig2Analog-Plus-5ch data pin); a shorter chain proves nothing
 * for a longer one. */
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

/* 0 = valid Ultra header (magic, version, CRC, TIME below ULTRA_TIME_WRAP) */
static inline int ultra_parse_header(const uint8_t *h, uint32_t *time_us)
{
    uint32_t t = ultra_get32(h + 3);
    if (ultra_get16(h) != ULTRA_MAGIC || (h[2] >> 4) != ULTRA_VERSION || ultra_crc8(h, 7) != h[7] || t >= ULTRA_TIME_WRAP) {
        return -1;
    }
    *time_us = t;
    return 0;
}

/* a host clock (us, 64 bit) as header TIME: modulo ULTRA_TIME_WRAP = 1000 x 2^22, with 32-bit divisions only
 * ((us >> 22) mod 1000 from its two 21-bit halves; 2^21 mod 1000 = 152) */
static inline uint32_t ultra_time_wire(uint64_t us)
{
    const uint64_t q = us >> 22;
    const uint32_t r = ((uint32_t)(q >> 21) % 1000u * 152u + ((uint32_t)q & 0x1FFFFFu)) % 1000u;
    return (r << 22) | ((uint32_t)us & 0x3FFFFFu);
}

/* TIME t + k us (t, k < ULTRA_TIME_WRAP), modulo the wrap */
static inline uint32_t ultra_time_add(uint32_t t, uint32_t k)
{
    const uint32_t s = t + k;
    return (s < t || s >= ULTRA_TIME_WRAP) ? s - ULTRA_TIME_WRAP : s;
}

/* a - b for two TIMEs (each < ULTRA_TIME_WRAP), signed, modulo the wrap: -34.95 min .. +34.95 min */
static inline int32_t ultra_time_sub(uint32_t a, uint32_t b)
{
    uint32_t d = a - b;
    if (a < b) {
        d += ULTRA_TIME_WRAP;
    }
    return (int32_t)(d >= ULTRA_TIME_WRAP / 2u ? d - ULTRA_TIME_WRAP : d);
}

static inline uint8_t ultra_header_flags(const uint8_t *h)
{
    return (uint8_t)(h[2] & 0x0Fu);
}

/* ----------------------------------------------------------------------------------------------- slots */
static inline void ultra_slot_begin(uint8_t *s, uint8_t type, uint8_t target, uint8_t flags, uint8_t seq)
{
    uint32_t i;
    for (i = 0; i < ULTRA_SLOT_BYTES; i++) {
        s[i] = 0;
    }
    s[0] = (uint8_t)((type & (ULTRA_TYPE_MASK | ULTRA_TYPE_BCAST)) | (flags & ULTRA_TYPE_FLAGS));
    s[1] = target;
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
 * values over dur_ms (curve per flags), starting at origin + start_ms (signed: a late copy catches up), and
 * computes every 1 ms with an exact power-law gamma on the full product, no 8-bit / table steps:
 *   out = gamma(M x A x B),  or for the CW / WW channels of a constant_brightness light
 *   out = g(M) x max(g(cw), g(ww)) x g(this) / (g(cw) + g(ww))   (ESPHome's formula, g = gamma)
 * then min / max power: out = min + out x (max - min) (0 stays 0 with zero_means_zero). */
/* Time origin: adaptive PHY header host ms floor(TIME / 1000); L1 kind 1 packet acceptance (transport.md).
 * start_ms and every phase offset retain their signed millisecond units. */
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
 * min[5] (2 each), max[5] (2 each), base channel (1). Only non-NONE roles must fit the channel count. */
#define ULTRA_DEF_ZERO_MEANS_ZERO      0x01u
#define ULTRA_DEF_CONSTANT_BRIGHTNESS  0x02u
#define ULTRA_DEF_FLAGS                (ULTRA_DEF_ZERO_MEANS_ZERO | ULTRA_DEF_CONSTANT_BRIGHTNESS)
static inline void ultra_slot_light_def(uint8_t *s, uint8_t light, uint8_t seq, const uint8_t *roles,
                                        uint16_t gamma_x1000, uint8_t def_flags, const uint16_t *min_p,
                                        const uint16_t *max_p, uint8_t base_channel)
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
    p[26] = base_channel;
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

/* CONFIG: one target per slot, up to nine (id,u16) pairs, unused = (0,0).
 * Board TARGET accepts ids 1..10,21..25,33..46; channel TARGET accepts 11,12;
 * pixel-output TARGET accepts 64..77. Unknown pairs are ignored. Above-rating
 * limits clamp to the rating; 0 restores it, never removes protection. 0xFFFF
 * is not a settable current. Unknown ratings require an explicitly qualified
 * model/bound; they never mean unlimited. Values retained and CRC'd are applied
 * values, so hosts compare after clamping/rounding. Duplicate ids: last pair wins.
 */
static inline void ultra_slot_config(uint8_t *s, uint8_t target, uint8_t seq,
                                      const uint8_t *ids, const uint16_t *values, uint32_t n)
{
    uint32_t i;
    ultra_slot_begin(s, ULTRA_CONFIG, target, 0, seq);
    for (i=0; i<n && i<9u; ++i) {
        s[3u+3u*i]=ids[i]; ultra_put16(s+4u+3u*i,values[i]);
    }
    ultra_slot_seal(s);
}

/* IDENTIFY: on = 1 blinks the board's identify pattern (30 s), 0 stops it.
 * ULTRA_IDENT_CAL: boards with the input current sense measure each channel alone (zero, then R, G, B, W1, W2) and
 * store the result: `arg` 0 = the standard test at 25 / 50 / 75 / 100 % (descriptor cal_ms per channel), 5..100 = at that duty only
 * (descriptor cal_ms bound). Boards without it ignore it.
 * ULTRA_IDENT_DARK: every output fades to dark (0.3 s) and stays dark for `arg` s (0 = 60 s) or until
 * ULTRA_IDENT_RELEASE, which fades back; the light engine keeps running underneath, so transitions continue.
 * The hub darkens a whole pin this way and then calibrates its boards one after the other.
 * ULTRA_IDENT_VERSION: the board blinks its firmware version once on its feedback channel (3 fast, then
 * major, minor and patch as value codes, then alpha / beta / rc as 1 / 2 / 3 short), then shows its outputs again.
 * ULTRA_IDENT_POWER_OFF (ultra_slot_power_off): a planned power-off notice. The hub sends it in the slot of every
 * board of a line, 3 copies with the same SEQ (applied once) at 0, 50 and 100 ms, and cuts the line's power at 200 ms;
 * once sent it always cuts (no take-back: a command after it waits for the power to be off, then wakes the line). A
 * board that knows it (a Diff-Power-UR-Analog-M receiver, ULTRA_FEAT_STANDBY) fades to dark (0.3 s) and takes the
 * coming cut as planned: no supply dip in the 10 s after it, and the lifetime record written only if there is something
 * worth storing (a board doesn't write its record for every standby cycle). Still powered 2 s after it with the supply
 * steady, its outputs come back; after 10 s without the cut it forgets the notice. Its
 * next start is a power-up as any: such a receiver stays dark after every power-up until the first valid
 * frame or 1.5 s, then shows its power-on scene. Other boards ignore it.
 * Values 8-255 and CAL arguments 1-4 / 101-255 are reserved: a board ignores the slot. STOP / ON / RELEASE /
 * VERSION / POWER_OFF send argument 0. */
#define ULTRA_IDENT_STOP 0u
#define ULTRA_IDENT_ON   1u
#define ULTRA_IDENT_CAL  2u
#define ULTRA_IDENT_DARK 3u
#define ULTRA_IDENT_RELEASE 4u
#define ULTRA_IDENT_VERSION 5u
#define ULTRA_IDENT_PIXEL_CLEAR 6u   /* pixel-output scope / PIXEL_CALIBRATION only: erase model, argument/reserved zero */
#define ULTRA_IDENT_POWER_OFF 7u
#define ULTRA_IDENT_CAL_MIN 5u         /* CAL argument: 0 = the standard test, else MIN..MAX % */
#define ULTRA_IDENT_CAL_MAX 100u
static inline void ultra_slot_identify(uint8_t *s, uint8_t seq, uint8_t on)
{
    ultra_slot_begin(s, ULTRA_IDENTIFY, ULTRA_TARGET_BOARD, 0, seq);
    s[ULTRA_PAYLOAD] = (on <= ULTRA_IDENT_RELEASE || on == ULTRA_IDENT_VERSION) ? on : (uint8_t)ULTRA_IDENT_STOP;
    ultra_slot_seal(s);
}

/* the planned power-off notice (ULTRA_IDENT_POWER_OFF above) */
static inline void ultra_slot_power_off(uint8_t *s, uint8_t seq)
{
    ultra_slot_begin(s, ULTRA_IDENTIFY, ULTRA_TARGET_BOARD, 0, seq);
    s[ULTRA_PAYLOAD] = ULTRA_IDENT_POWER_OFF;
    ultra_slot_seal(s);
}

static inline void ultra_slot_calibrate(uint8_t *s, uint8_t seq, uint8_t duty_pct)
{
    ultra_slot_begin(s, ULTRA_IDENTIFY, ULTRA_TARGET_BOARD, 0, seq);
    s[ULTRA_PAYLOAD] = ULTRA_IDENT_CAL;
    s[ULTRA_PAYLOAD + 1] = duty_pct == 0u ? 0u : duty_pct < ULTRA_IDENT_CAL_MIN ? (uint8_t)ULTRA_IDENT_CAL_MIN
                         : duty_pct > ULTRA_IDENT_CAL_MAX ? (uint8_t)ULTRA_IDENT_CAL_MAX : duty_pct;
    ultra_slot_seal(s);
}

/* Pixel-output calibration request/cancel/clear; shared sensors serialize jobs. */
static inline void ultra_slot_pixel_calibration(uint8_t *s,uint8_t output,uint8_t seq,uint8_t action)
{
    ultra_slot_begin(s,ULTRA_IDENTIFY,output,0,seq);
    s[ULTRA_PAYLOAD]=(action==ULTRA_IDENT_CAL || action==ULTRA_IDENT_RELEASE || action==ULTRA_IDENT_PIXEL_CLEAR)
        ? action : (uint8_t)ULTRA_IDENT_STOP;
    ultra_slot_seal(s);
}

static inline void ultra_slot_hold_dark(uint8_t *s, uint8_t seq, uint8_t hold_s)
{
    ultra_slot_begin(s, ULTRA_IDENTIFY, ULTRA_TARGET_BOARD, 0, seq);
    s[ULTRA_PAYLOAD] = ULTRA_IDENT_DARK;
    s[ULTRA_PAYLOAD + 1] = hold_s;
    ultra_slot_seal(s);
}

/* SCENE: two flash-stored output levels, selector 0 power-on, 1 signal loss.
 * Payload 0 selector (2..255 reserved, receiver ignores); 1 fade_100ms into the
 * level; 2 M:u16; 4 A[5]:u16; 14 B[5]:u16; 24..26 reserved zero.
 * Power-on fades up from dark; signal loss fades from current outputs.
 * All-zero levels mean dark. The first controller SET supplies its own transition.
 * Persist only when levels change, while outputs are dark (no output blink). */
#define ULTRA_SCENE_POWER_ON 0u
#define ULTRA_SCENE_SIGNAL_LOSS 1u
static inline void ultra_slot_scene(uint8_t *s, uint8_t light, uint8_t seq, uint8_t mode, uint8_t fade_100ms, uint16_t m,
                                    const uint16_t *a, const uint16_t *b)
{
    uint8_t *p = s + ULTRA_PAYLOAD;
    uint32_t i;
    ultra_slot_begin(s, ULTRA_SCENE, light, 0, seq);
    p[0] = mode;
    p[1] = fade_100ms;
    ultra_put16(p + 2, m);
    for (i = 0; i < 5u; i++) {
        ultra_put16(p + 4 + 2 * i, a[i]);
        ultra_put16(p + 14 + 2 * i, b[i]);
    }
    ultra_slot_seal(s);
}

/* PIXEL_FX (the Diff-Power-UR-Digital-M receiver's local pixel effects, ULTRA_PX_* above) */
typedef struct {
    uint8_t  effect, speed, intensity, palette;
    uint8_t  c[3][3];                  /* c1, c2, c3: R, G, B */
    uint8_t  brightness, flags, custom1, custom2;
    int32_t  start_ms;                 /* ms relative to the header's host ms or L1 packet acceptance */
    uint16_t transition_ms, seg_start, seg_len;
} ultra_pixel_fx_t;

static inline void ultra_slot_pixel_fx(uint8_t *s, uint8_t output, uint8_t segment, uint8_t seq, const ultra_pixel_fx_t *f)
{
    uint32_t i;
    ultra_slot_begin(s, ULTRA_PIXEL_FX, ULTRA_PIXEL_TARGET(output, segment), 0, seq);
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
    s[ULTRA_PX_FLAGS] = (uint8_t)(f->flags & 0x7Fu);
    s[ULTRA_PX_CUSTOM1] = f->custom1;
    s[ULTRA_PX_CUSTOM2] = f->custom2;
    ultra_slot_seal(s);
}

/* Decode a CRC-checked PIXEL_FX. Return packed output/segment TARGET or -1.
 * Caller validates both indices against its generated descriptor constants. */
static inline int ultra_pixel_fx_parse(const uint8_t *s, ultra_pixel_fx_t *f)
{
    const uint32_t target=s[1], flags=s[ULTRA_PX_FLAGS];
    uint32_t i;
    if ((s[0] & (ULTRA_TYPE_MASK | ULTRA_TYPE_BCAST))!=ULTRA_PIXEL_FX || (target>>4)>ULTRA_PIXEL_OUTPUT_MAX) return -1;
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
    f->flags = (uint8_t)(flags & 0x7Fu);
    f->custom1 = s[ULTRA_PX_CUSTOM1];
    f->custom2 = s[ULTRA_PX_CUSTOM2];
    return (int)target;
}

/* CRC for a complete output settings snapshot (all 14 ids explicitly set). */
static inline uint16_t ultra_px_config_crc(uint8_t output, const uint16_t *values)
{
    uint8_t b[4]; uint32_t i; uint16_t crc=0xFFFFu;
    for(i=0;i<ULTRA_CFG_PX_COUNT;++i) {
        b[0]=output; b[1]=(uint8_t)(ULTRA_CFG_PX_FIRST+i);
        ultra_put16(b+2u,values[i]); crc=ultra_crc16_update(crc,b,4);
    }
    return crc;
}

/* NOP: a valid slot that does nothing; bytes 3..29 are ignored training data,
 * not reserved-zero fields. TARGET, SEQ and flags remain zero. */
static inline void ultra_slot_nop(uint8_t *s)
{
    ultra_slot_begin(s, ULTRA_NOP, 0, 0, 0);
    ultra_slot_seal(s);
}
/* Training NOP: payload is explicitly permitted test data, not reserved state.
 * A receiver ignores it exactly like NOP. Fully inventoried Ultra-only ports
 * use 55/AA/00/FF patterns in known one-second LINK_STATS windows. */
static inline void ultra_slot_training_nop(uint8_t *s,uint8_t pattern)
{
    uint32_t i;
    ultra_slot_begin(s,ULTRA_NOP,0,0,0);
    for(i=3u;i<30u;++i)s[i]=pattern;
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
    ultra_slot_begin(s, ULTRA_POLL, ULTRA_TARGET_BOARD, 0, seq);
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
    return ((s[0] & (ULTRA_TYPE_MASK | ULTRA_TYPE_BCAST)) == ULTRA_POLL && s[1] == ULTRA_TARGET_BOARD && r - 1u < ULTRA_RW_MAX_SLOTS &&
            (i < r || i == ULTRA_POLL_RELAY_ONLY) && s[ULTRA_POLL_RATE] < ULTRA_RW_RATES) ? 0 : -1;
}

/* the slot pitch and the window W for a rate, us (host: next frame >= W + ULTRA_RW_HOLDOFF_US after the frame end).
 * Pitch = (335 << rate) + 165 us = 500 / 835 / 1505 us. The complete wire budget is
 * 80 late + 10 lead + 330*2^rate/0.985 UART + 8 gaps + 5 DE-after-TC + 15 skew,
 * all in us, and must finish by pitch-30. This leaves >=16.89 us spare in addition
 * to the 30 us end guard (which includes the next burst's 20 us early allowance).
 * Host restart also requires forwarding restoration and processing LOW below.
 * Recorded design limit (timing.md, return windows): change only by an explicit decision. */
#define ULTRA_RW_PITCH_SCALED_US 335u  /* the part that doubles per rate step */
#define ULTRA_RW_PITCH_FIXED_US  165u
static inline uint32_t ultra_rw_slot_us(uint8_t rate) { return rate < ULTRA_RW_RATES ? ((uint32_t)ULTRA_RW_PITCH_SCALED_US << rate) + ULTRA_RW_PITCH_FIXED_US : 0u; }   /* (32-bit) */
static inline uint32_t ultra_rw_window_us(uint8_t slots, uint8_t rate)
{
    return ULTRA_RW_FIRST_US + ultra_rw_slot_us(rate) * slots;
}
/* Conservative next normal-frame start: all forwarders restored, then a full
 * processing LOW. gap_us is the maximum LINK gap (clamped to the common 500 us). */
static inline uint32_t ultra_rw_next_us(uint8_t slots, uint8_t rate, uint32_t gap_us)
{
    uint32_t gap = gap_us < ULTRA_FRAME_GAP_US ? ULTRA_FRAME_GAP_US : gap_us;
    uint32_t after = ULTRA_RW_END_US + gap;
    return ultra_rw_window_us(slots,rate) + (after > ULTRA_RW_HOLDOFF_US ? after : ULTRA_RW_HOLDOFF_US);
}
/* a digital port's window W_D, us after t_end: the hub listens this long at most (1,400 / 1,735 / 2,405 us) */
static inline uint32_t ultra_dp_window_us(uint8_t rate) { return ULTRA_RW_DIR_MAX_US + ultra_rw_slot_us(rate); }

/* an answer (32 bytes as received): 0 = SYNC and CRC valid */
static inline int ultra_answer_check(const uint8_t *a)
{
    return (a[0] == ULTRA_RSP_SYNC && ultra_crc16(a, 30) == ultra_get16(a + 30)) ? 0 : -1;
}

/* CONFIG CRC: CCITT-FALSE over canonical sorted (target u8,id u8,value BE u16)
 * records, lexicographic (target,id), applied values only. Board STATUS/ACKS cover
 * all explicit settings since reset; per-output PIXEL_FX CRC covers that output's
 * ids 64..77. Defaults not explicitly set are omitted. No heap or 256-id bitmap
 * is required: receivers walk generated constant id lists and incremental CRC.
 */
static inline uint16_t ultra_config_crc(const uint8_t *canonical_records, uint32_t count)
{ return ultra_crc16(canonical_records, count*4u); }

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
