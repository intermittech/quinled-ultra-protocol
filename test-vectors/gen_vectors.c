/* SPDX-License-Identifier: MIT
 * Copyright (C) 2026 QuinLED
 *
 * QuinLED Ultra protocol - test-vector generator.
 *
 * Builds example slots and frames with the reference header (../include/ultra_protocol.h) and writes them as
 * Markdown (vectors.md) and JSON (vectors.json). Another implementation builds the same inputs and compares byte for
 * byte. Every frame is also checked here: header (magic, version, CRC-8) and every slot's CRC-16.
 *
 * Build and run (any C99 compiler):
 *   cc -std=c99 -O1 -o gen_vectors gen_vectors.c && ./gen_vectors vectors.md vectors.json
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../include/ultra_protocol.h"

/* Bit timing used for the wire-time column (ns): the ESPHome hub's defaults. TIME in these vectors is a fixed value;
 * a real host puts its clock at the frame's last bit there (see the wiki page "Timing and clock"). */
#define T0H 340u
#define T0L 960u
#define T1H 700u
#define T1L 650u

static FILE *md, *js;
static int njs;
static int failures;

static void check(int ok, const char *what)
{
    if (!ok) {
        fprintf(stderr, "CHECK FAILED: %s\n", what);
        failures++;
    }
}

/* first rising edge to the last bit's falling edge (ns) */
static unsigned long long wire_ns(const uint8_t *d, uint32_t n)
{
    unsigned long long t = 0;
    uint32_t i, b;
    for (i = 0; i < n; i++) {
        for (b = 0; b < 8u; b++) {
            t += ((d[i] >> (7u - b)) & 1u) ? (T1H + T1L) : (T0H + T0L);
        }
    }
    return n ? t - ((d[n - 1u] & 1u) ? T1L : T0L) : 0;
}

static void md_hex(const uint8_t *d, uint32_t n, uint32_t per_line)
{
    uint32_t i;
    fprintf(md, "```\n");
    for (i = 0; i < n; i++) {
        fprintf(md, "%02X%s", d[i], (i + 1u) % per_line == 0u || i + 1u == n ? "\n" : " ");
    }
    fprintf(md, "```\n\n");
}

static void js_vector(const char *name, const char *kind, const char *desc, const uint8_t *d, uint32_t n)
{
    uint32_t i;
    fprintf(js, "%s\n    {\"name\": \"%s\", \"kind\": \"%s\", \"bytes\": %u, \"bits\": %u, \"description\": \"%s\", \"hex\": \"",
            njs++ ? "," : "", name, kind, (unsigned)n, (unsigned)(n * 8u), desc);
    for (i = 0; i < n; i++) {
        fprintf(js, "%02x", d[i]);
    }
    fprintf(js, "\"}");
}

/* one slot (32 bytes) */
static void slot_vector(const char *name, const char *title, const char *desc, const uint8_t *s, int valid)
{
    check((ultra_slot_check(s) == 0) == valid, name);
    fprintf(md, "### `%s`: %s\n\n%s\n\n", name, title, desc);
    fprintf(md, "TYPE `0x%02X`, LIGHT %u, FLAGS `0x%X`, SEQ `0x%02X`, CRC-16 `0x%04X` (%s)\n\n", s[0], s[1] >> 4,
            s[1] & 0x0Fu, s[2], (unsigned)ultra_get16(s + 30), valid ? "valid" : "deliberately not valid");
    md_hex(s, ULTRA_SLOT_BYTES, 16);
    js_vector(name, "slot", desc, s, ULTRA_SLOT_BYTES);
}

/* one frame: n slots + header */
static void frame_vector(const char *name, const char *title, const char *desc, const uint8_t *f, uint32_t nslots,
                         int dark)
{
    const uint32_t len = ultra_frame_bytes(nslots);
    const uint8_t *h = f + ultra_header_offset(nslots);
    uint32_t t = 0, i;
    check(ultra_parse_header(h, &t) == 0, name);
    for (i = 0; i < nslots; i++) {
        check((ultra_slot_check(f + ULTRA_SLOT_BYTES * i) == 0) == !dark, name);
    }
    fprintf(md, "### `%s`: %s\n\n%s\n\n", name, title, desc);
    fprintf(md, "%u slot(s) + header = %u bytes = %u bits. Header TIME `0x%08X`, FLAGS `0x%X`, CRC-8 `0x%02X`. "
                "On the wire with the hub's default bit timing: %.1f us from the first rising edge to the last "
                "falling edge, then the reset gap (>= 280 us).\n\n",
            (unsigned)nslots, (unsigned)len, (unsigned)(len * 8u), (unsigned)t, ultra_header_flags(h), h[7],
            (double)wire_ns(f, len) / 1000.0);
    fprintf(md, "One line per 32-byte slot, the 8-byte header last:\n\n");
    md_hex(f, len, 32);
    js_vector(name, "frame", desc, f, len);
}

static void header_vector(const char *name, const char *title, const char *desc, const uint8_t *h, int valid)
{
    uint32_t t = 0;
    check((ultra_parse_header(h, &t) == 0) == valid, name);
    fprintf(md, "### `%s`: %s\n\n%s\n\nA board (wire VERSION 2) %s this header.\n\n", name, title, desc,
            valid ? "**accepts**" : "**rejects**");
    md_hex(h, ULTRA_HDR_BYTES, 8);
    js_vector(name, valid ? "header" : "header-invalid", desc, h, ULTRA_HDR_BYTES);
}

int main(int argc, char **argv)
{
    static const uint8_t check_str[] = "123456789";
    static const uint16_t full[5] = { 65535, 65535, 65535, 65535, 65535 };
    static const uint16_t zero5[5] = { 0, 0, 0, 0, 0 };
    static const uint8_t roles_rgbww[5] = { ULTRA_ROLE_RED, ULTRA_ROLE_GREEN, ULTRA_ROLE_BLUE, ULTRA_ROLE_WARM_WHITE,
                                            ULTRA_ROLE_COLD_WHITE };
    static const uint16_t a_orange[5] = { 65535, 26214, 0, 0, 0 };
    static const uint16_t a_cct[5] = { 0, 0, 0, 65535, 32768 };
    static const uint16_t a_red[5] = { 65535, 0, 0, 0, 0 };
    static const uint8_t pulse_p[13] = { 0x00, 0x01, 0xF4,  0x00, 0x01, 0xF4,  0x00, 0x03, 0xE8,  0x00, 0x00,  0xFF, 0xFF };
    static const uint8_t breathe_p[7] = { 0x00, 0x13, 0x88,  0x19, 0x99,  0xFF, 0xFF };
    static const uint8_t strobe_p[1] = { 2 };
    static const uint8_t cfg_ids[8] = { ULTRA_CFG_PWM_PRESET, ULTRA_CFG_STAGGER, 11, 12, 13, 14, 15, ULTRA_CFG_BOARD_MAX };
    static const uint16_t cfg_vals[8] = { 8, 4, 3000, 3000, 3000, 5000, 5000, 8000 };
    static const uint16_t pt_m[3] = { 65535, 16384, 3277 };
    static const uint16_t pt_mired10[3] = { 3704, 4545, 5556 };
    static const uint8_t candle_p[4] = { 0x80, 0x00,  0x00, 0x64 };
    static const uint8_t ss_id[1] = { ULTRA_CFG_SOFT_START };
    static const uint16_t ss_val[1] = { 0 };
    uint8_t s[ULTRA_SLOT_BYTES], h[ULTRA_HDR_BYTES], f[8 * ULTRA_SLOT_BYTES + ULTRA_HDR_BYTES];
    uint8_t sw[64];
    uint32_t i, n, t;

    if (argc != 3) {
        fprintf(stderr, "usage: %s vectors.md vectors.json\n", argv[0]);
        return 2;
    }
    md = fopen(argv[1], "wb");          /* LF line endings on every platform */
    js = fopen(argv[2], "wb");
    if (!md || !js) {
        perror("open");
        return 2;
    }

    fprintf(md, "# QuinLED Ultra protocol 0.%u (wire VERSION %u): test vectors\n\n", ULTRA_VERSION, ULTRA_VERSION);
    fprintf(md, "Generated by `gen_vectors.c` from `include/ultra_protocol.h`. Do not edit by hand: change the generator "
                "and run it again.\nBytes are hex, in wire order: MSB of byte 0 first. Multi-byte fields are "
                "big-endian. An implementation that builds the same inputs must produce exactly these bytes.\n\n");
    fprintf(js, "{\n  \"wire_version\": %u,\n  \"generator\": \"test-vectors/gen_vectors.c\",\n  \"vectors\": [",
            ULTRA_VERSION);

    /* ------------------------------------------------------------------------------------------------ CRCs */
    fprintf(md, "## 1. CRC check values\n\nThe standard check string `123456789` (ASCII):\n\n");
    fprintf(md, "| CRC | Parameters | Result |\n|---|---|---|\n");
    fprintf(md, "| CRC-8/SMBus (header) | poly 0x07, init 0x00, no reflection, no final XOR | `0x%02X` |\n",
            ultra_crc8(check_str, 9));
    fprintf(md, "| CRC-16/CCITT-FALSE (slots) | poly 0x1021, init 0xFFFF, no reflection, no final XOR | `0x%04X` |\n\n",
            ultra_crc16(check_str, 9));
    check(ultra_crc8(check_str, 9) == 0xF4u, "crc8 check value");
    check(ultra_crc16(check_str, 9) == 0x29B1u, "crc16 check value");

    /* ---------------------------------------------------------------------------------------------- headers */
    fprintf(md, "## 2. Headers\n\nMAGIC `5A D2`, VERSION << 4 | FLAGS, TIME (4, big-endian), CRC-8 over the first 7 "
                "bytes.\n\n");
    ultra_build_header(h, 0x00ABCDEFu);
    header_vector("hdr_normal", "a normal header", "TIME 0x00ABCDEF, FLAGS 0.", h, 1);
    ultra_build_header(h, 0u);
    header_vector("hdr_time_zero", "TIME 0", "TIME is the host's microsecond clock, low 32 bits; 0 is valid.", h, 1);
    ultra_build_header(h, 0xFFFFFFFFu);
    header_vector("hdr_time_max", "TIME 0xFFFFFFFF", "The clock wraps every 71.6 minutes; boards handle the wrap.",
                  h, 1);
    ultra_build_header_flags(h, 0x00ABCDEFu, ULTRA_HDR_HINT);
    header_vector("hdr_flag_hint", "FLAGS 0x02 (HINT, reserved)",
                  "A valid header. A board that doesn't know a hint processes the frame as usual. Hosts send FLAGS 0 "
                  "until a hint is defined.", h, 1);
    ultra_build_header_flags(h, 0x00ABCDEFu, ULTRA_HDR_RW);
    header_vector("hdr_flag_rw", "FLAGS 0x01 (RW, reserved for the return channel)",
                  "A valid header. Today's boards take the clock sync but ignore every slot of the frame and keep "
                  "their lock. Hosts must not send it yet.", h, 1);
    ultra_build_header(h, 0x00ABCDEFu);
    h[6] ^= 0x01u;
    header_vector("hdr_bad_crc", "one TIME bit flipped, CRC not updated",
                  "Same as hdr_normal with the last TIME bit flipped: the CRC-8 no longer matches.", h, 0);
    ultra_build_header(h, 0x00ABCDEFu);
    h[2] = 0x10u;
    h[7] = ultra_crc8(h, 7);
    header_vector("hdr_version1", "VERSION 1 with a correct CRC",
                  "Format 1 (header first, withdrawn, never released). The CRC is correct, but a VERSION 2 board "
                  "rejects every frame of "
                  "another version: no lock, no clock sync, no slot.", h, 0);
    ultra_build_header(h, 0x00ABCDEFu);
    h[0] = 0x5Bu;
    h[7] = ultra_crc8(h, 7);
    header_vector("hdr_bad_magic", "wrong magic with a correct CRC", "MAGIC 0x5BD2 instead of 0x5AD2.", h, 0);

    /* ------------------------------------------------------------------------------------------------ slots */
    fprintf(md, "## 3. Slots, one of each type\n\nTYPE, LIGHT << 4 | FLAGS, SEQ, PAYLOAD (27), CRC-16 over the "
                "first 30 bytes.\n\n");
    ultra_slot_nop(s);
    slot_vector("slot_nop", "NOP", "A valid slot that does nothing (idle frames). LIGHT 0, SEQ 0.", s, 1);
    ultra_slot_dark(s);
    slot_vector("slot_dark", "DARK (preamble)", "256 zero bits. Deliberately not a valid slot: every board ignores it.",
                s, 0);
    ultra_slot_light_def(s, 0, 0x11, roles_rgbww, 2800, 0, zero5, full);
    slot_vector("slot_light_def", "LIGHT_DEF, light 0 = RGBWW",
                "Roles CH1-CH5 = red, green, blue, warm white, cold white (2 3 4 7 6), gamma 2.8 (2800), flags 0, "
                "min power 0 and max power 65535 on every channel. SEQ 0x11.", s, 1);
    ultra_slot_set(s, 0, 0x22, ULTRA_SET_SMOOTH, 49151, a_orange, full, 1000, 30);
    slot_vector("slot_set", "SET, light 0 to orange at 75 %",
                "Flags 0x01 (smoothstep). M = 49151 (0.75), A = 65535 26214 0 0 0, B = 65535 x 5, duration 1000 ms, "
                "start +30 ms after this frame's TIME. SEQ 0x22.", s, 1);
    ultra_slot_effect(s, 0, 0x33, ULTRA_FX_PULSE, 30, pulse_p, sizeof pulse_p);
    slot_vector("slot_effect_pulse", "EFFECT pulse",
                "Effect 1, start +30 ms (int32). Parameters: on 500 ms (u24), off 500 ms (u24), interval 1000 ms "
                "(u24), min 0, max 65535. SEQ 0x33.", s, 1);
    ultra_slot_effect(s, 1, 0x34, ULTRA_FX_BREATHE, 30, breathe_p, sizeof breathe_p);
    slot_vector("slot_effect_breathe", "EFFECT breathe on light 1",
                "Effect 6, start +30 ms. Parameters: period 5000 ms (u24), min 6553 (0.1), max 65535. SEQ 0x34.", s, 1);
    ultra_slot_effect(s, 0, 0x35, ULTRA_FX_STROBE, 30, strobe_p, sizeof strobe_p);
    slot_vector("slot_effect_strobe", "EFFECT strobe with 2 steps",
                "Effect 2, start +30 ms, step count 2. The steps follow as STEP slots with the same SEQ 0x35.", s, 1);
    ultra_slot_step(s, 0, 0x35, 0, 2, 100, 0, 65535, a_red, full);
    slot_vector("slot_step", "STEP 0 of 2 (for slot_effect_strobe)",
                "Index 0, count 2, duration 100 ms, transition 0 ms, M 65535, A = red, B = 65535 x 5. SEQ = the "
                "strobe EFFECT's SEQ 0x35.", s, 1);
    ultra_slot_config(s, 0x44, cfg_ids, cfg_vals, 8);
    slot_vector("slot_config", "CONFIG, 8 settings",
                "LIGHT 0xF (board). Pairs (id, value): PWM preset 8 (25 kHz), stagger 4 (adaptive + spread), LED "
                "current CH1-CH3 3000 mA, CH4-CH5 5000 mA, board limit 8000 mA; the 9th pair unused (0, 0). "
                "SEQ 0x44.", s, 1);
    ultra_slot_identify(s, 0x55, ULTRA_IDENT_ON);
    slot_vector("slot_identify_on", "IDENTIFY on", "Value 1: blink the identify pattern (30 s). SEQ 0x55.", s, 1);
    ultra_slot_calibrate(s, 0x56, 0);
    slot_vector("slot_identify_cal", "IDENTIFY current calibration",
                "Value 2, argument 0: the standard test at 25 / 50 / 75 / 100 %. Boards without the input current "
                "sense ignore it. SEQ 0x56.", s, 1);
    ultra_slot_hold_dark(s, 0x57, 10);
    slot_vector("slot_identify_dark", "IDENTIFY dark for 10 s",
                "Value 3, argument 10: fade to dark and hold 10 s (or until value 4, RELEASE). SEQ 0x57.", s, 1);
    ultra_slot_identify(s, 0x58, ULTRA_IDENT_VERSION);
    slot_vector("slot_identify_version", "IDENTIFY version (0.2.1)",
                "Value 5: blink the firmware version once. Firmware from before 0.2.1 ignores it. SEQ 0x58.", s, 1);
    ultra_slot_scene(s, 0, 0x66, 1, 10, 32768, a_cct, full);
    slot_vector("slot_scene", "SCENE, light 0",
                "Mode 1, cross-fade 10 x 100 ms, M 32768, A = 0 0 0 65535 32768 (warm-ish white), B = 65535 x 5. "
                "SEQ 0x66.", s, 1);
    ultra_slot_light_opt(s, 0, 0x77, ULTRA_OPT_DIM_TO_WARM, 1538, 3704, 5556, 19661);
    slot_vector("slot_light_opt", "LIGHT_OPT dim to warm",
                "Flags 0x01. CW LED 153.8 mireds (6500 K) = 1538, WW LED 370.4 mireds (2700 K) = 3704, warm end "
                "555.6 mireds (1800 K) = 5556, red assist 19661 (30 %). SEQ 0x77.", s, 1);
    ultra_slot_light_opt_points(s, 0, 0x78, ULTRA_OPT_DIM_TO_WARM | ULTRA_OPT_POINTS, 1538, 3704, 5556, 19661, 3,
                                pt_m, pt_mired10);
    slot_vector("slot_light_opt_points", "LIGHT_OPT dim to warm with set points",
                "Flags 0x03. As slot_light_opt plus 3 points (M, mireds x 10): (65535, 3704) 2700 K at full, "
                "(16384, 4545) 2200 K at 25 %, (3277, 5556) 1800 K at 5 %; the 4th point unused (0, 0). SEQ 0x78.",
                s, 1);
    ultra_slot_scene_effect(s, 0, 0x79, ULTRA_FX_CANDLE, candle_p, sizeof candle_p);
    slot_vector("slot_scene_effect", "SCENE_EFFECT candle on light 0 (0.2.3)",
                "Effect 5 (candle) as light 0's power-on effect, on its SCENE values (slot_scene). Parameters as "
                "EFFECT without the start time: intensity 32768 (0.5), speed 100 %. Boards from before 0.2.3 ignore "
                "it. SEQ 0x79.", s, 1);
    ultra_slot_config(s, 0x45, ss_id, ss_val, 1);
    slot_vector("slot_config_soft_start", "CONFIG soft start off (0.2.3)",
                "LIGHT 0xF (board). One pair: id 23 (soft start), value 0 (off; persisted). The other 8 pairs unused "
                "(0, 0). SEQ 0x45.", s, 1);

    /* ----------------------------------------------------------------------------------------------- frames */
    fprintf(md, "## 4. Frames\n\nN slots (board 0 first) + the header as a trailer. Every board takes the first slot "
                "of what reaches it and forwards the rest.\n\n");

    ultra_slot_set(f, 0, 0x22, ULTRA_SET_SMOOTH, 49151, a_orange, full, 1000, 30);
    ultra_build_header(f + ultra_header_offset(1), 0x00ABCDEFu);
    frame_vector("frame_1board_set", "1 board, SET",
                 "Board 0: slot_set. Header TIME 0x00ABCDEF.", f, 1, 0);

    ultra_slot_light_def(f, 0, 0x11, roles_rgbww, 2800, 0, zero5, full);
    ultra_slot_effect(f + 32, 1, 0x34, ULTRA_FX_BREATHE, 30, breathe_p, sizeof breathe_p);
    ultra_slot_nop(f + 64);
    ultra_build_header(f + ultra_header_offset(3), 0x00ABCDEFu);
    frame_vector("frame_3board_mixed", "3 boards: LIGHT_DEF, EFFECT, NOP",
                 "Board 0: slot_light_def. Board 1: slot_effect_breathe. Board 2: NOP (nothing to do). The first "
                 "board sees all 104 bytes, the second 72, the third 40.", f, 3, 0);

    ultra_slot_config(f, 0x44, cfg_ids, cfg_vals, 8);
    ultra_build_header(f + ultra_header_offset(1), 0x00ABCDEFu);
    frame_vector("frame_config", "1 board, CONFIG", "Board 0: slot_config.", f, 1, 0);

    ultra_slot_effect(f, 0, 0x33, ULTRA_FX_PULSE, 30, pulse_p, sizeof pulse_p);
    ultra_build_header(f + ultra_header_offset(1), 0x00ABCDEFu);
    frame_vector("frame_effect", "1 board, EFFECT pulse", "Board 0: slot_effect_pulse.", f, 1, 0);

    for (i = 0; i < 2u; i++) {
        ultra_slot_nop(f + 32 * i);
    }
    ultra_build_header(f + ultra_header_offset(2), 0x00ABCDEFu);
    frame_vector("frame_idle_2boards", "2 boards, idle (NOP)",
                 "Keep-alive / refresh frame when there is nothing to send: NOP in every slot.", f, 2, 0);

    for (i = 0; i < 2u; i++) {
        ultra_slot_dark(f + 32 * i);
    }
    ultra_build_header(f + ultra_header_offset(2), 0x00ABCDEFu);
    n = ultra_preamble_frames(2);
    {
        char d[256];
        snprintf(d, sizeof d, "The start-up preamble for 2 boards is %u of these frames (4 x N + 2), each with a "
                              "fresh TIME. DARK slots are zero bits and never valid.", (unsigned)n);
        frame_vector("frame_preamble_dark_2boards", "2 boards, preamble (DARK)", d, f, 2, 1);
    }
    check(n == 10u, "preamble count for 2 boards");

    /* ----------------------------------------------------------------------- WS config-frame switch sequence */
    fprintf(md, "## 5. Switching boards forced to WS2805 / WS2915: the WS config-frame sequence for 2 boards\n\n");
    fprintf(md, "%u frames, sent with the same WS bit timing and a reset gap after each. Frames 0-3: WS2805 format "
                "(5 bytes per board = the message `D2 A5 01 03 CRC`), frame 4: WS2805 all zero; frames 5-8: WS2915 "
                "format (header `FF FF FF 80`, then per board the message and its complement), frame 9: WS2915 all "
                "zero. Consecutive frames differ in length by 1 byte of zero padding. Afterwards the host sends the "
                "start-up preamble and everything again.\n\n", (unsigned)ULTRA_SWITCH_WS_FRAMES);
    fprintf(md, "| Frame | Bytes | Hex |\n|---|---|---|\n");
    for (i = 0; i < ULTRA_SWITCH_WS_FRAMES; i++) {
        uint32_t len = ultra_switch_frame(sw, i, 2), j;
        char name[40];
        fprintf(md, "| %u | %u | `", (unsigned)i, (unsigned)len);
        for (j = 0; j < len; j++) {
            fprintf(md, "%02X%s", sw[j], j + 1u < len ? " " : "");
        }
        fprintf(md, "` |\n");
        snprintf(name, sizeof name, "ws_switch_2boards_%u", (unsigned)i);
        js_vector(name, "ws-config-frame", "WS config-frame sequence 'protocol = Ultra' for 2 boards, see the wiki",
                  sw, len);
        check(len <= ultra_switch_frame_bytes(2), "switch frame length");
    }
    t = ultra_crc8((const uint8_t *)"\xD2\xA5\x01\x03", 4);
    fprintf(md, "\nThe message: MAGIC `D2 A5`, CMD `01` (protocol), ARG `03` (Ultra), CRC-8/SMBus over those 4 bytes = "
                "`%02X`.\n", (unsigned)t);

    fprintf(js, "\n  ]\n}\n");
    fclose(md);
    fclose(js);
    if (failures) {
        fprintf(stderr, "%d check(s) failed\n", failures);
        return 1;
    }
    printf("ok: all checks passed\n");
    return 0;
}
