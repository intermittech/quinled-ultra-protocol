/* SPDX-License-Identifier: MIT
 * Copyright (C) 2026 QuinLED
 *
 * QuinLED Ultra sender: the smallest complete host, platform-neutral. Two boards: board 0 one RGBWW light, board 1
 * five single-colour lights. It sets a colour, starts a breathe, and runs the frame loop on a simulated clock,
 * printing every frame it would send. On real hardware replace now_us() and send_frame() (see esp32_idf_rmt/).
 *
 *   cc -std=c99 -I../src host_minimal.c ../src/ultra_sender.c ../src/ultra_sender_bits.c
 */
#include <stdio.h>
#include <string.h>

#include "ultra_sender.h"

#define BOARDS 2u

static ultra_sender_t line;
static ultra_sender_board_t boards[BOARDS];
static ultra_sender_item_t items[ULTRA_SENDER_ITEMS(BOARDS, 5, 3)];   /* LIGHT_DEF + SET + EFFECT per light */
static uint8_t frame[ULTRA_SENDER_FRAME_BYTES(BOARDS)];

static int64_t clock_us = 1000000;              /* a monotonic microsecond clock (esp_timer_get_time(), ...) */
static int quiet;

static int64_t now_us(void)
{
    return clock_us;
}

/* hand the bytes to the output (RMT, SPI + DMA, ...) in one go, then keep the line LOW for the reset gap */
static void send_frame(const uint8_t *f, uint32_t n)
{
    uint32_t i;
    if (!quiet) {
        printf("%7.1f ms  %3u bytes  ", (double)clock_us / 1000.0, (unsigned)n);
        for (i = 0; i < 12u && i < n; i++) {
            printf("%02X", f[i]);
        }
        printf(" ...  TIME %08X\n", (unsigned)ultra_get32(f + n - 5));
    }
    /* simulated: the frame and its reset gap take this long */
    clock_us = ultra_sender_last_time_us(&line) + line.timing.reset_ns / 1000u;
}

int main(int argc, char **argv)
{
    ultra_sender_config_t cfg;
    ultra_light_def_t def;
    ultra_color_t color;
    ultra_values_t v;
    uint8_t p[ULTRA_FX_PARAMS];
    uint8_t l;
    int frames = 0;
    quiet = argc > 1 && strcmp(argv[1], "--quiet") == 0;

    /* 1. the line: 2 boards, up to 5 lights each */
    ultra_sender_config_init(&cfg, BOARDS, 5);
    cfg.seed = 0x1234567u;                       /* use a random number per boot */
    if (ultra_sender_init(&line, &cfg, boards, items, (uint16_t)(sizeof items / sizeof items[0])) != ULTRA_OK) {
        return 1;
    }
    ultra_sender_set_lights(&line, 0, 1);        /* board 0: one light (indices 1-4 are cleared) */

    /* 2. the lights: board 0 = RGBWW (CH4 warm, CH5 cold white), board 1 = five single-colour lights */
    ultra_light_def_init(&def);                  /* gamma 2.8 on the board, full power range */
    def.roles[0] = ULTRA_ROLE_RED;
    def.roles[1] = ULTRA_ROLE_GREEN;
    def.roles[2] = ULTRA_ROLE_BLUE;
    def.roles[3] = ULTRA_ROLE_WARM_WHITE;
    def.roles[4] = ULTRA_ROLE_COLD_WHITE;
    ultra_sender_light_def(&line, 0, 0, &def);
    for (l = 0; l < 5u; l++) {
        ultra_light_def_init(&def);
        def.roles[l] = ULTRA_ROLE_BRIGHTNESS;
        ultra_sender_light_def(&line, 1, l, &def);
    }
    ultra_sender_config(&line, ULTRA_SENDER_ALL, ULTRA_CFG_PWM_PRESET, 8);   /* 25 kHz on every board */

    /* 3. a colour: orange at 80 %, fading in over 1 s (8-bit values x 257) */
    memset(&color, 0, sizeof color);
    color.brightness = ULTRA_8TO16(204);
    color.red = ULTRA_8TO16(255);
    color.green = ULTRA_8TO16(100);
    color.rgb_level = 65535;
    {
        static const uint8_t rgbww[5] = { ULTRA_ROLE_RED, ULTRA_ROLE_GREEN, ULTRA_ROLE_BLUE, ULTRA_ROLE_WARM_WHITE,
                                           ULTRA_ROLE_COLD_WHITE };
        ultra_values_from_color(rgbww, &color, &v);   /* the light's roles: M, A and B per channel */
    }
    ultra_sender_set(&line, 0, 0, &v, 1000, ULTRA_SET_SMOOTH, now_us());
    /* board 1: all five on at 50 %, then a breathe on them, in step */
    for (l = 0; l < 5u; l++) {
        v.m = 32768;
        memset(v.a, 0xFF, sizeof v.a);
        memset(v.b, 0xFF, sizeof v.b);
        ultra_sender_set(&line, 1, l, &v, 500, 0, now_us());
        ultra_sender_effect(&line, 1, l, ULTRA_FX_BREATHE, p, ultra_fx_breathe(p, 4000, 0, 65535), now_us());
    }

    /* 4. the loop: send a frame whenever one is due (back to back while commands wait, every 200 ms at rest) */
    while (now_us() < 3000000) {
        const uint32_t n = ultra_sender_next_frame(&line, now_us(), frame);
        if (n != 0u) {
            send_frame(frame, n);
            frames++;
        } else {
            clock_us += 1000;                    /* (on hardware: sleep a little, or until ultra_sender_due_us()) */
        }
    }
    printf("%d frames in 2 s: the preamble (%u), the commands (3 copies each), then a refresh every 200 ms\n", frames,
           (unsigned)ultra_preamble_frames(BOARDS));
    return frames > 20 ? 0 : 1;
}
