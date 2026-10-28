/* SPDX-License-Identifier: MIT
 * Copyright (C) 2026 QuinLED
 *
 * QuinLED Ultra sender: the reference host library for the QuinLED Ultra protocol 0.3 (wire VERSION 3).
 * Docs: sender/README.md and the wiki page "Sender library". Spec: the protocol wiki (Host behaviour, Timing and clock).
 *
 * One ultra_sender_t drives one data line (pin) of N boards. The caller tells it what the lights should do; the
 * library keeps the boards' state and builds every frame with the host rules of the reference host (the ESPHome
 * component ultra_line, ultra_core.h):
 *  - each command gets a new SEQ and goes out in 3 copies (repeats), first copies first, a newer command for the
 *    same light and type replacing a queued one;
 *  - boards without new commands get their current state repeated (the refresh: CONFIG, then per light LIGHT_DEF,
 *    LIGHT_OPT, SET, SCENE), or a NOP; every 16th slot of a busy board is a refresh;
 *  - a frame at least every refresh_ms (200 ms) at rest, frames back to back while commands wait;
 *  - the start-up preamble (4 N + 2 frames of DARK slots) at init, resync, a supply switch-on, and after the WS
 *    switch sequence; commands queued meanwhile go out after it;
 *  - SET / PIXEL_FX carry one absolute start on the host clock, re-stamped against every frame's TIME; commands close
 *    together share one start (a group start, also across lines);
 *  - TIME = the host clock at the frame's last bit, computed from the frame's own bits.
 * The caller only turns the bytes into pulses (ultra_sender_bits.h) and keeps the reset gap after each frame.
 *
 * Storage: no heap. The caller provides the line (ultra_sender_t), one ultra_sender_board_t per board and a pool of
 * items (sizeof(ultra_sender_item_t) each): a light takes one item per kind of state it ever had (LIGHT_DEF, SET,
 * LIGHT_OPT and two SCENEs) (at most five items); items stay taken until
 * the line is initialized again. ULTRA_SENDER_ITEMS() sizes the pool, ultra_sender_items_used() shows the use.
 *
 * Lifetime and ownership:
 *  - the line, its boards[] and items[], and a group the line joined, stay alive at the same addresses from
 *    ultra_sender_init() to ultra_sender_deinit(): the line keeps pointers to them (never copy or move an initialized
 *    ultra_sender_t; the C++ wrapper can't be copied or moved);
 *  - ultra_sender_deinit() before initializing a line again or dropping it, when it joined a group;
 *  - commands copy their inputs (values and parameters): the caller's buffers are free after the call;
 *  - the frame buffer is the caller's; the line keeps the timing data of the last frame it built only (for
 *    ultra_sender_retime()), so a frame is built when it can go out, not ahead.
 *
 * The scheduler is a producer with state: ultra_sender_next_frame() takes the copies it puts into a frame from the
 * queues, whether the caller sends the frame or not. A frame that couldn't go out: ultra_sender_retime(), then send it
 * again (don't build another). The caller waits for the frame and its reset gap before the next call; a call that
 * comes late sends one frame, not the ones missed. Keep the gaps far below the boards' signal-loss timeout (3 s).
 *
 * Time: every time is one 64-bit microsecond clock (int64_t, e.g. esp_timer_get_time(), time_us_64()), the same for
 * every line of a group, starting near 0 (not near the int64 limits). A 32-bit micros() can be extended with
 * ultra_clock64() (`ultra_clock64_t clk = { 0, 0 };`, called at least every 71 minutes). The header carries it
 * modulo ULTRA_TIME_WRAP (2^22 x 1000 us). SET can start at most 32.7 s after a frame that carries it (its int16 ms offset; later starts are clipped
 * to that), PIXEL_FX 24.8 days.
 *
 * Not thread-safe: call every function of one line (and of the lines of one group) from one task, or under one lock.
 */
#ifndef ULTRA_SENDER_H
#define ULTRA_SENDER_H

#include <limits.h>
#include <stdint.h>
#include "ultra_protocol.h"
#include "ultra_sender_bits.h"

/* A 32-bit (or wider) int. The protocol header is 16-bit-int safe, but this library has never been
 * built or tested where int has 16 bits (its uint16_t arithmetic would promote to a 16-bit int there). */
#if UINT_MAX < 0xFFFFFFFFu
#error "QuinLED Ultra sender: needs an unsigned int of at least 32 bits; 16-bit-int targets (AVR, MSP430) are not supported"
#endif

#ifdef __cplusplus
extern "C" {
#endif

#define ULTRA_SENDER_VERSION_STR "0.3.2-draft"   /* this library: the protocol release it targets (its ultra_protocol.h) */

#ifndef ULTRA_SENDER_MAX_BOARDS
#define ULTRA_SENDER_MAX_BOARDS 8u        /* storage of one line, not a board limit: that is per port type (ULTRA_PORT_*_MAX_BOARDS) */
#endif
#define ULTRA_SENDER_ALL       0xFFFFu     /* board: every board of the line (CONFIG, IDENTIFY) */
#define ULTRA_NO_START         INT64_MIN
#define ULTRA_SENDER_CFG_COUNT 26u /* domain CONFIG uses optional pool items */
#define ULTRA_SENDER_DEFAULT_LIGHTS 5u
/* CONFIG slots a board needs for every id (up to 9 pairs each, in id order) */
#define ULTRA_SENDER_CFG_PAGES 2u        /* fourteen assigned board IDs, nine per slot */
#define ULTRA_SENDER_CFG_WORDS ((ULTRA_SENDER_CFG_COUNT + 31u) / 32u)

/* items for a pool: `per_light` 2 = LIGHT_DEF + SET; 3 = + LIGHT_OPT;
 * 5 = + both stored SCENEs. An Diff-Power-UR-Digital-M receiver (local pixel effects) takes one item per
 * segment it runs (PIXEL_FX) and 2 for its CONFIG 64-77 */
#define ULTRA_SENDER_ITEMS(boards, lights, per_light) ((boards) * (lights) * (per_light))
/* the frame buffer for n boards (also fits the WS switch frames) */
#define ULTRA_SENDER_FRAME_BYTES(n) (32u * (n) + 8u)

/* results */
#define ULTRA_OK        0
#define ULTRA_ERR_ARG   (-1)               /* board, light, id or value out of range (nothing changed) */
#define ULTRA_ERR_FULL  (-2)               /* the item pool is exhausted (nothing changed) */

/* ------------------------------------------------------------------------------------------------ values */
/* What SET and SCENE carry (wiki: Slot types, 0x02 SET): M = state x brightness, per channel CH1..CH5
 * A = the channel's colour component (65535 on a brightness channel, 0 outside the light) and B = the colour
 * brightness on RGB channels (65535 elsewhere). All 0..65535 = 0..1, linear: the board applies gamma. */
typedef struct {
    uint16_t m;
    uint16_t a[5];
    uint16_t b[5];
} ultra_values_t;

/* An RGB / white / CCT colour, linear 0..65535 (8-bit: x 257, ULTRA_8TO16) */
typedef struct {
    uint16_t brightness;               /* M: 0 = off */
    uint16_t red, green, blue;         /* the RGB part */
    uint16_t rgb_level;                /* B on the RGB channels: the colour's brightness (65535 = full) */
    uint16_t white, cold_white, warm_white;
} ultra_color_t;
#define ULTRA_8TO16(v) ((uint16_t)((uint32_t)(v) * 257u))

/* the values for a light with these roles (ULTRA_ROLE_* per channel): A from the colour per role, B = rgb_level on
 * the RGB channels; channels without a role get A = 0, B = 65535 */
void ultra_values_from_color(const uint8_t roles[5], const ultra_color_t *c, ultra_values_t *v);

/* LIGHT_DEF (wiki: Slot types, 0x01): which channels form the light and how the board drives them. The board takes
 * a channel for one light only: the caller keeps the lights of a board on separate channels. */
typedef struct {
    uint8_t  base_channel;             /* roles address base..base+4 */
    uint8_t  roles[5];                 /* ULTRA_ROLE_* for CH1..CH5 (QuinLED: CH4 / W1 warm white, CH5 / W2 cold) */
    uint16_t gamma_x1000;              /* 2800 = 2.8, applied on the board; 1000 (or 0) = linear */
    uint8_t  flags;                    /* ULTRA_DEF_ZERO_MEANS_ZERO, ULTRA_DEF_CONSTANT_BRIGHTNESS */
    uint16_t min_power[5];             /* out = min + out x (max - min), 0..65535 */
    uint16_t max_power[5];
} ultra_light_def_t;
/* roles none, gamma 2.8, flags 0, min 0, max 65535 */
void ultra_light_def_init(ultra_light_def_t *d);

/* LIGHT_OPT: dim to warm (wiki: Slot types, 0x09) */
typedef struct {
    uint8_t  flags;                    /* ULTRA_OPT_DIM_TO_WARM, ULTRA_OPT_POINTS */
    uint16_t cw_mired10, ww_mired10;   /* the LEDs, mireds x 10 (6500 K = 1538) */
    uint16_t warm_mired10;             /* the warm end (at M = 0, or the warmest set point) */
    uint16_t red_assist;               /* 0..65535 = 0..100 % */
    uint8_t  points;                   /* set points used, <= ULTRA_OPT_POINTS_MAX */
    uint16_t pt_m[ULTRA_OPT_POINTS_MAX], pt_mired10[ULTRA_OPT_POINTS_MAX];
} ultra_light_opt_t;

/* ------------------------------------------------------------------------------------------------ storage */
typedef struct {
    uint32_t order;                    /* queue order (oldest first) */
    uint8_t  sends, copies;            /* copies sent, copies to send (0 = not queued) */
    uint8_t  behind;                   /* it replaced a command none of whose copies went out */
    uint8_t  flags;                    /* an item's (IT_*: the item's own byte; unused on a board's queues) */
} ultra_sender_q_t;

/* one piece of a light's state (opaque; 60 bytes: 32-bit fields only, so no 8-byte alignment) */
typedef struct {
    uint32_t start_lo, start_hi;       /* SET / PIXEL_FX: the absolute start on the host clock (int64_t, in halves) */
    ultra_sender_q_t q;                /* (q.flags: the item's IT_*) */
    uint32_t aux;                      /* streamed SET: the glide asked for, ms */
    uint32_t last_us;                  /* streamed SET: when the light's last one went out (low 32 bits) */
    uint16_t next, key;                /* the board's next item (pool index); light << 8 | rank << 4 | sub */
    uint8_t  slot[ULTRA_SLOT_BYTES];   /* the slot as sent, its SEQ in byte 2 */
} ultra_sender_item_t;

/* one board (opaque; the fields in an order without padding) */
typedef struct {
    uint16_t cfg[ULTRA_SENDER_CFG_COUNT];
    uint16_t first;                    /* first item (sorted by key) */
    uint16_t rr;                       /* refresh position */
    uint16_t unsent;                   /* queued commands none of whose copies went out yet */
    uint32_t cfg_have[ULTRA_SENDER_CFG_WORDS];
    ultra_sender_q_t cfg_q[ULTRA_SENDER_CFG_PAGES];
    ultra_sender_q_t id_q, clr_q;
    uint32_t f_dur;                    /* the frame being built: a SET's duration */
    int64_t  f_start;                  /* ... this slot's absolute start */
    uint8_t  cfg_pseq[ULTRA_SENDER_CFG_PAGES];
    uint8_t  cfg_seq, cfg_dirty;
    uint8_t  id_seq, id_val, id_arg;
    uint16_t clr_pos;
    uint8_t  lights, capacity, channels, clr_seq, clr_min;
    const struct ultra_descriptor *descriptor; /* caller-owned validated blob/view */
    uint8_t  urgent_run, f_floor, poll_seq;
    uint8_t  clr_round;                /* refresh rounds since init / resync / set_lights: 0..6, then 3..6 again */
} ultra_sender_board_t;

struct ultra_sender;

/* lines whose commands share one start (lights on several pins that change together) */
typedef struct {
    int64_t start;
    struct ultra_sender *lines;
} ultra_sender_group_t;

/* one data line (opaque; fill with ultra_sender_init) */
typedef struct ultra_sender {
    ultra_sender_board_t *boards;
    ultra_sender_item_t *items;
    uint16_t n, n_items, used;
    uint16_t preamble_left;
    ultra_timing_t timing;
    uint32_t refresh_us, frame_us, switch_us;
    int32_t  start_delay_us;
    uint32_t order, rng, queued, frames;
    ultra_sender_group_t *group;       /* (here: no padding before the 64-bit times) */
    int64_t  last_start_us, last_time_us;
    int64_t  hold_us;                  /* no frame before (a response window) */
    int64_t  ready_us;                 /* the end of the last preamble */
    int64_t  off_cut_us;               /* the power-off notice: the cut (its first copy + 200 ms) */
    ultra_sender_group_t own_group;
    struct ultra_sender *group_next;
    uint8_t  repeats, group_commands, max_lights, clear_unused;
    uint8_t  preamble_full, synced_once, powered, has_sent;
    uint8_t  switch_frame, kind, hdr_flags;
    uint8_t  hw_dirty;                 /* a pixel CONFIG page changed (flushed with the next frame) */
    uint8_t  digital;                  /* a digital port (ultra_sender_digital): marker framing; measured scheduling lock still required */
    uint8_t  gc_auto;                  /* group_commands follows the most lights on a board */
    uint8_t  last_ends_preamble;       /* the last frame built is the preamble's last (retime moves ready_us) */
    uint8_t  off;                      /* the power-off notice: 0 none, 1 its copies out (quiet until the wake),
                                          2-4 copies still to send + 1 */
    uint8_t  last_notice;              /* the last frame built is one of them (retime moves their times) */
    uint8_t  profile_training;         /* acquisition/probe: DARK/NOP/read-only traffic only */
    uint16_t last_bytes;
    uint16_t ready_frames;             /* frames since the last preamble (saturating) */
    uint16_t profile_epoch;
    uint8_t tx_active, update_active;  /* update 1 transfer, 2 app confirmation; both freeze profile */
} ultra_sender_t;

/* ------------------------------------------------------------------------------------------------ set-up */
typedef struct {
    uint16_t boards;                   /* 1..ULTRA_SENDER_MAX_BOARDS */
    uint8_t  light_capacity;           /* 0..240, defaults to 5 */
    uint8_t  lights;                   /* lights per board, 0..240 (per board: ultra_sender_set_lights) */
    uint8_t  repeats;                  /* copies of every command (3) */
    uint16_t refresh_ms;               /* a frame at least this often at rest (200; well below the boards' signal
                                          loss timeout, CONFIG 9, default 3 s) */
    int32_t  start_delay_us;           /* how far ahead commands start; -1 = auto (the frames a change needs) */
    uint8_t  group_commands;           /* auto start delay: commands one change gives the busiest board (0 = the
                                          most lights on a board, following ultra_sender_set_lights) */
    uint8_t  clear_unused;             /* 1: light indices >= lights get an empty LIGHT_DEF, both zero-valued SCENEs (what an earlier configuration left on the boards) */
    uint32_t seed;                     /* random per boot (esp_random(), ...): the first SEQ numbers */
    ultra_timing_t timing;             /* ULTRA_TIMING_RECOMMENDED */
} ultra_sender_config_t;

void ultra_sender_config_init(ultra_sender_config_t *c, uint16_t boards, uint8_t lights);
/* ULTRA_OK = ultra_sender_init() takes the configuration (boards 1..ULTRA_SENDER_MAX_BOARDS, lights 0..240, a timing
 * inside the spec), ULTRA_ERR_ARG = it refuses it; changes nothing (check before stopping a running line) */
int  ultra_sender_config_check(const ultra_sender_config_t *c);
/* boards: c->boards entries; items: a pool of n_items (may be 0 / NULL: board commands only). Storage may be
 * uninitialized. The line starts with the preamble; queue the lights' state now (LIGHT_DEF, SET, ...): it goes out
 * after it, and automatic starts count the preamble in. A line that joined a group: ultra_sender_deinit() first.
 * ULTRA_ERR_ARG (a configuration ultra_sender_config_check() refuses, no board storage): l unchanged. */
int  ultra_sender_init(ultra_sender_t *l, const ultra_sender_config_t *c, ultra_sender_board_t *boards,
                       ultra_sender_item_t *items, uint16_t n_items);
/* the line leaves its group and takes no more commands or frames; its storage is free afterwards */
void ultra_sender_deinit(ultra_sender_t *l);
/* Marker-framed control of one receiver. Startup omits the chain preamble;
 * send ordinary training headers before polling. Profile switches still send
 * a fresh DARK preamble. ultra_dport.h schedules markers, streams and retake. */
int  ultra_sender_digital(ultra_sender_t *l, int on);
/* lights in use on one board (0..240): commands to other indices are refused, and with clear_unused they are cleared.
 * Lowering it stops sending that light's state; raising it sends it again. */
int  ultra_sender_set_lights(ultra_sender_t *l, uint16_t board, uint8_t lights);

/* ------------------------------------------------------------------------------------------------ frames */
/* The next frame into buf (ULTRA_SENDER_FRAME_BYTES(n)) if one is due at now_us, the moment its first bit goes out:
 * returns its length in bytes, or 0 (nothing due: at rest, the refresh interval hasn't passed). Call it whenever the
 * line is free (the previous frame and its reset gap are out), send the bytes in one go (inter-bit LOW <10 us), then keep
 * the line LOW for the reset gap. A WS switch frame (after ultra_sender_switch_to_ultra) is shorter; send it alike. */
uint32_t ultra_sender_next_frame(ultra_sender_t *l, int64_t now_us, uint8_t *buf);
/* A frame now, due or not (the refresh interval is not waited for): a digital port's header (ultra_dport.h: the local
 * effects' beacon, the marker frame after a hand-back). Nothing queued: a refresh slot or a NOP. 0 only while a response
 * window or a power-off holds the line. */
uint32_t ultra_sender_beacon_frame(ultra_sender_t *l, int64_t now_us, uint8_t *buf);
/* the last frame couldn't be sent: the same slots again with a new TIME for now_us (nothing more is taken from the
 * queues; buf must still hold that frame); returns its length */
uint32_t ultra_sender_retime(ultra_sender_t *l, int64_t now_us, uint8_t *buf);
/* frames should go back to back (commands queued, the preamble or the switch sequence running) */
int      ultra_sender_busy(const ultra_sender_t *l);
/* when the next frame is due: now if busy, else the last frame's start + refresh_ms; never before a response
 * window (ultra_sender_rc.h) has ended */
int64_t  ultra_sender_due_us(const ultra_sender_t *l);
/* the last frame's TIME on the 64-bit clock (its last bit); the line is free again at this + the reset gap */
int64_t  ultra_sender_last_time_us(const ultra_sender_t *l);
/* the longest frame of this line plus its reset gap, us */
uint32_t ultra_sender_frame_us(const ultra_sender_t *l);

/* The preamble again (4 N + 2 DARK frames), then everything again as new commands: boards that kept their state
 * ignore the repeats, the others take it. Transitions restart, pixel effects keep their own start (they join in phase).
 * After a manual resync, or when the host can't know what the boards saw. */
void ultra_sender_resync(ultra_sender_t *l, int64_t now_us);
/* The boards' supply. Off: the host knows they have no power. On (call it after the supply's power-on delay):
 * a short preamble (10 frames) when a full one reached the powered boards before, else the full one, then
 * everything again (fades that ran in the dark are replayed). */
void ultra_sender_power(ultra_sender_t *l, int on, int64_t now_us);
/* The planned power-off notice (IDENTIFY 7), for a host
 * that switches the line's power: from the next frame on the line sends only the notice, in every board's slot, 3
 * copies with one SEQ at least 50 ms apart (0, 50, 100 ms), then nothing until ultra_sender_power(l, 1, ...) wakes it
 * (a preamble and every state again: commands given meanwhile go out then). No take-back: once given, cut the power.
 * Boards with ULTRA_FEAT_STANDBY (Diff-Power-UR-Analog-M receivers report it) fade to dark and take the cut as planned (no
 * supply dip, no flash write for it); every other board ignores the notice. ULTRA_ERR_ARG: a notice is already under
 * way. */
int  ultra_sender_power_off(ultra_sender_t *l);
/* When the power may be cut: 200 ms after the notice's first copy went out, and not before its last copy's end.
 * INT64_MAX while copies are still to go out; INT64_MIN: no notice (none given, or the line woken since). */
int64_t ultra_sender_power_off_at(const ultra_sender_t *l);
/* Boards forced to WS2805 / WS2915 (they ignore Ultra frames): the WS config-frame sequence (ultra_switch_frame,
 * 10 frames), then the full preamble and everything again. boot_protocol: CONFIG 22 for every board afterwards
 * (0 auto, 3 Ultra), 0xFF = leave it; another value: ULTRA_ERR_ARG, nothing happens. Send it only on request. */
int  ultra_sender_switch_to_ultra(ultra_sender_t *l, int64_t now_us, uint8_t boot_protocol);

/* ------------------------------------------------------------------------------------------------ start times */
/* The start for a command queued at now_us: far enough ahead that its copies reach every board first (one slot per
 * board per frame, after the preamble or switch sequence still to send), shared with the other commands queued close
 * together on every line of the group. It assumes frames go out back to back while commands wait; a later frame
 * still works (a late copy catches up), the start of a fade then shows partly done. The *_at() calls take a start
 * the caller computed (e.g. this one, for several lights that must start together). */
int64_t ultra_sender_start_time(ultra_sender_t *l, uint16_t board, int64_t now_us);
/* the commands one change can give the busiest board, for the automatic start (a SET per light; + SCENE
 * when a change sends stored levels too); 0 = the most lights on a board */
int     ultra_sender_set_group_commands(ultra_sender_t *l, uint8_t commands);
void    ultra_sender_group_init(ultra_sender_group_t *g);
/* the line's commands share their starts with the other lines of g (each line starts in a group of its own). The
 * group stays alive while lines are in it. */
void    ultra_sender_join_group(ultra_sender_t *l, ultra_sender_group_t *g);
/* back to a group of its own */
void    ultra_sender_leave_group(ultra_sender_t *l);

/* ------------------------------------------------------------------------------------------------ lights */
/* Commands for light `light` (0..lights-1) of `board`: ULTRA_OK, ULTRA_ERR_ARG or ULTRA_ERR_FULL (then nothing
 * changed, the group's start neither). Undefined values are refused, never sent: roles 0-7, defined flag bits,
 * SCENE selectors 0 / 1. LIGHT_DEF,
 * LIGHT_OPT and SCENE with unchanged content queue nothing. */
int ultra_sender_light_def(ultra_sender_t *l, uint16_t board, uint8_t light, const ultra_light_def_t *d);
int ultra_sender_light_opt(ultra_sender_t *l, uint16_t board, uint8_t light, const ultra_light_opt_t *o);
/* SET: go to v over transition_ms (0 = jump); flags ULTRA_SET_SMOOTH (smoothstep), ULTRA_SET_VIA_OFF (colour mode
 * change: fade out, switch, fade in). Starts at ultra_sender_start_time(), or at start_us. */
int ultra_sender_set(ultra_sender_t *l, uint16_t board, uint8_t light, const ultra_values_t *v,
                     uint32_t transition_ms, uint8_t flags, int64_t now_us);
int ultra_sender_set_at(ultra_sender_t *l, uint16_t board, uint8_t light, const ultra_values_t *v,
                        uint32_t transition_ms, uint8_t flags, int64_t start_us);
/* A host-side effect step (or a pixel of a host-driven animation): a linear SET over glide_ms (= the update
 * interval: the board glides between steps), sent once (the next step replaces a lost one), never starting before
 * it arrives. When the board's queue was behind, the glide stretches to the time since the light's last step. It
 * interpolates with the receiver's 16-bit fades. */
int ultra_sender_stream(ultra_sender_t *l, uint16_t board, uint8_t light, const ultra_values_t *v, uint32_t glide_ms,
                        int64_t now_us);
/* Stored level: mode 0 power-on, 1 signal loss; fade_100ms fades INTO it.
 * NULL v stores dark. Each selector retains its own item/refresh; their SEQs must differ in the shared TARGET/type namespace. */
int ultra_sender_scene(ultra_sender_t *l, uint16_t board, uint8_t light, uint8_t mode, uint8_t fade_100ms,
                       const ultra_values_t *v);
/* Local effects: packed output/segment TARGET, independently of light count.
 * Descriptors bound segments, chip formats, frame bytes, resource/power limits.
 * RESTART is copied only in queued commands; refresh preserves phase.
 * CONFIG takes explicit output and IDs 64..77. */
int ultra_sender_pixel_fx(ultra_sender_t *l, uint16_t board, uint8_t target, const ultra_pixel_fx_t *fx, int64_t now_us);
int ultra_sender_pixel_fx_at(ultra_sender_t *l, uint16_t board, uint8_t target, const ultra_pixel_fx_t *fx,
                             int64_t start_us);
/* the START of the segment's last PIXEL_FX (ULTRA_NO_START: none) */
int64_t ultra_sender_pixel_fx_start(const ultra_sender_t *l, uint16_t board, uint8_t target);
/* CONFIG 64-77 (ULTRA_CFG_PX_*) of the strip: sent with the next frame, in their own CONFIG slots (an unchanged value
 * queues nothing), part of the refresh. ULTRA_ERR_ARG: not one of these ids, or a value the receiver would refuse */
int ultra_sender_pixel_config(ultra_sender_t *l, uint16_t board, uint8_t output, uint8_t id, uint16_t value);
int ultra_pixel_config_value_ok(uint8_t id, uint16_t value);
/* Line mode: a pixel node sits at any position of a CHAIN line (it holds its
 * slot and forwards the rest, up to the port's board limit); STREAM only on a
 * digital port, alone. A bound descriptor decides from its LINK: a CHAIN_END
 * port only last, a FORWARD port anywhere. Sources on separate raw ingresses
 * remain independent of command transport. */
#define ULTRA_PXT_CHAIN   0u               /* the line's normal transport */
#define ULTRA_PXT_DIGITAL 1u               /* ultra_dport.h */
int     ultra_pixel_line_mode_ok(uint8_t mode, int digital_line, uint16_t board, uint16_t boards);
uint8_t ultra_pixel_transport(uint8_t mode, int digital_line);
int     ultra_sender_pixel_line_mode(ultra_sender_t *l, uint16_t board, uint8_t output, uint8_t mode);

/* ------------------------------------------------------------------------------------------------ board */
/* Board CONFIG 1..10,21..25,33..46; channel/output settings use
 * ultra_sender_config_target(). Limits resolve against a bound descriptor.
 * CRC covers the explicitly applied state; verify receiver readback when a
 * hardware-specific period/count/FPS is clamped or rounded. */
uint16_t ultra_sender_config_crc(const ultra_sender_t *l,uint16_t board);
int ultra_config_scope_ok(uint8_t target, uint8_t id);
int ultra_sender_config_target(ultra_sender_t *l, uint16_t board, uint8_t target, uint8_t id, uint16_t value);
int ultra_sender_config(ultra_sender_t *l, uint16_t board, uint8_t id, uint16_t value);
/* CONFIG 24's value: count 0 = never retry, 1-254 retries, 255 = forever; interval_s 1-255 (0 = 1 s) */
#define ULTRA_SENDER_RETRY(count, interval_s) \
    ((uint16_t)((((uint32_t)(interval_s) & 0xFFu) << 8) | ((uint32_t)(count) & 0xFFu)))
/* 1 = the id is defined and the value inside its range (wiki: CONFIG settings) */
int ultra_config_value_ok(uint8_t id, uint16_t value);
/* PWM frequency (Hz) -> CONFIG 1 (preset, when one is within 0.5 %) and 2 (the raw period, else 0) */
int ultra_sender_pwm_hz(ultra_sender_t *l, uint16_t board, uint32_t hz);
void ultra_pwm_for_hz(uint32_t hz, uint16_t *preset, uint16_t *period);
/* IDENTIFY (sent 3 times, not refreshed): ULTRA_IDENT_STOP / _ON (blink 30 s) / _VERSION (blink the firmware
 * version) / _RELEASE with arg 0; _CAL (current calibration: arg 0 = the standard test, 5..100 = one duty %) and
 * _DARK (hold dark: arg = seconds, 0 = 60). One board or ULTRA_SENDER_ALL. (_POWER_OFF: ultra_sender_power_off) */
int ultra_sender_identify(ultra_sender_t *l, uint16_t board, uint8_t value, uint8_t arg);
/* the board's last IDENTIFY still has copies to send (a calibration run waits for it) */
int ultra_sender_identify_pending(const ultra_sender_t *l, uint16_t board);

/* ------------------------------------------------------------------------------------------------ inspection */
int      ultra_sender_pending(const ultra_sender_t *l);           /* commands queued */
uint16_t ultra_sender_waiting(const ultra_sender_t *l, uint16_t board);   /* ... none of whose copies went out */
uint16_t ultra_sender_items_used(const ultra_sender_t *l);
/* The SEQ the first command of (light, type) on a board takes (light ULTRA_LIGHT_BOARD for CONFIG / IDENTIFY / POLL;
 * CONFIG: its first slot, the next ones count on). Before that light's first command of the type (else
 * ULTRA_ERR_ARG). For tests and hosts that keep SEQs across restarts; never needed otherwise (a board resets its SEQ
 * tracking when TIME jumps). */
int      ultra_sender_preset_seq(ultra_sender_t *l, uint16_t board, uint8_t light, uint8_t type, uint8_t seq);

/* ------------------------------------------------------------------------------------------------ clock */
/* a 64-bit microsecond clock from a 32-bit one (call at least every 71 minutes) */
typedef struct {
    uint32_t last;
    int64_t  high;
} ultra_clock64_t;
static inline int64_t ultra_clock64(ultra_clock64_t *c, uint32_t now_us)
{
    if (now_us < c->last) {
        c->high += (int64_t)1 << 32;
    }
    c->last = now_us;
    return c->high + (int64_t)now_us;
}

#ifdef __cplusplus
}
#endif

#endif
