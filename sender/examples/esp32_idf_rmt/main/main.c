/* SPDX-License-Identifier: MIT
 * Copyright (C) 2026 QuinLED
 *
 * QuinLED Ultra sender: a minimal ESP-IDF host (ESP-IDF >= 5.3). One data line on DATA_GPIO with 4 boards; each
 * board one RGBWW light (CH1-CH3 RGB, CH4 warm white, CH5 cold white). It fades through a few colours and runs a
 * host-driven pulse, every board in step.
 *
 * One task owns the line and sends the frames; other tasks only queue commands, under the same lock.
 */
#include <string.h>

#include "esp_log.h"
#include "esp_random.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

#include "ultra_esp32_rmt.h"
#include "ultra_sender.h"

#define DATA_GPIO 16                           /* through a 5 V buffer to the first board */
#define BOARDS    4u
#define LIGHTS    1u

static const char *TAG = "ultra";
static ultra_sender_t line;
static ultra_sender_board_t boards[BOARDS];
static ultra_sender_item_t items[ULTRA_SENDER_ITEMS(BOARDS, LIGHTS, 3)];   /* LIGHT_DEF + SET + LIGHT_OPT */
static uint8_t frame[ULTRA_SENDER_FRAME_BYTES(BOARDS)];
static ultra_esp32_rmt_t out;
static SemaphoreHandle_t lock;

/* the frame loop: back to back while commands wait, a refresh every 200 ms at rest */
static void ultra_task(void *arg)
{
    uint32_t n = 0;                             /* a frame that still has to go out */
    (void)arg;
    for (;;) {
        int r;
        xSemaphoreTake(lock, portMAX_DELAY);
        /* TIME: its first bit goes out now (a constant delay to the real first edge shifts every board alike) */
        n = n != 0u ? ultra_sender_retime(&line, esp_timer_get_time(), frame)   /* the same slots, a new TIME */
                    : ultra_sender_next_frame(&line, esp_timer_get_time(), frame);
        xSemaphoreGive(lock);
        if (n == 0u) {
            vTaskDelay(1);                      /* nothing due yet */
            continue;
        }
        r = ultra_esp32_rmt_send(&out, frame, n);
        if (r == ULTRA_RMT_OK) {
            n = 0;
        } else if (r == ULTRA_RMT_STUCK) {
            /* the driver may still read `frame` and `out` (both static: they stay): released only once it stops */
            ESP_LOGE(TAG, "RMT stuck: output stopped");
            while (ultra_esp32_rmt_end(&out) != 0) {
                ESP_LOGE(TAG, "RMT can't be stopped yet: left as it is (a restart recovers)");
                vTaskDelay(pdMS_TO_TICKS(1000));
            }
            vTaskDelete(NULL);
        } else {
            ESP_LOGW(TAG, "frame not sent (%d): again", r);
            vTaskDelay(1);                      /* the frame stays; retimed on the next pass */
        }
    }
}

static void set_all(uint8_t r, uint8_t g, uint8_t b, uint8_t ww, uint8_t cw, uint8_t level, uint32_t fade_ms)
{
    static const uint8_t roles[5] = { ULTRA_ROLE_RED, ULTRA_ROLE_GREEN, ULTRA_ROLE_BLUE, ULTRA_ROLE_WARM_WHITE,
                                      ULTRA_ROLE_COLD_WHITE };
    ultra_color_t c = { 0 };
    ultra_values_t v;
    uint16_t k;
    c.brightness = ULTRA_8TO16(level);
    c.red = ULTRA_8TO16(r);
    c.green = ULTRA_8TO16(g);
    c.blue = ULTRA_8TO16(b);
    c.rgb_level = 65535;
    c.warm_white = ULTRA_8TO16(ww);
    c.cold_white = ULTRA_8TO16(cw);
    ultra_values_from_color(roles, &c, &v);
    xSemaphoreTake(lock, portMAX_DELAY);
    for (k = 0; k < BOARDS; k++) {             /* one start for all of them (a group start) */
        ultra_sender_set(&line, k, 0, &v, fade_ms, ULTRA_SET_SMOOTH, esp_timer_get_time());
    }
    xSemaphoreGive(lock);
}

void app_main(void)
{
    ultra_sender_config_t cfg;
    ultra_light_def_t def;
    uint16_t k;

    ultra_sender_config_init(&cfg, BOARDS, LIGHTS);
    cfg.seed = esp_random();
    /* the RMT first: the line computes TIME with the timing the RMT really sends */
    if (ultra_esp32_rmt_begin(&out, DATA_GPIO, &cfg.timing) != 0) {
        ESP_LOGE(TAG, "RMT setup failed");
        return;
    }
    if (ultra_sender_init(&line, &cfg, boards, items, sizeof items / sizeof items[0]) != ULTRA_OK) {
        ESP_LOGE(TAG, "line setup failed");
        (void)ultra_esp32_rmt_end(&out);        /* nothing was sent: it releases */
        return;
    }
    ultra_light_def_init(&def);                /* gamma 2.8, applied on the board */
    def.roles[0] = ULTRA_ROLE_RED;
    def.roles[1] = ULTRA_ROLE_GREEN;
    def.roles[2] = ULTRA_ROLE_BLUE;
    def.roles[3] = ULTRA_ROLE_WARM_WHITE;
    def.roles[4] = ULTRA_ROLE_COLD_WHITE;
    def.flags = ULTRA_DEF_CONSTANT_BRIGHTNESS;
    for (k = 0; k < BOARDS; k++) {
        ultra_sender_light_def(&line, k, 0, &def);
    }
    ultra_sender_config(&line, ULTRA_SENDER_ALL, ULTRA_CFG_PWM_PRESET, 8);   /* 25 kHz */
    lock = xSemaphoreCreateMutex();
    xTaskCreate(ultra_task, "ultra", 4096, NULL, 10, NULL);
    ESP_LOGI(TAG, "%u boards on GPIO %d: frame %u us, preamble %u frames", (unsigned)BOARDS, DATA_GPIO,
             (unsigned)ultra_sender_frame_us(&line), (unsigned)ultra_preamble_frames(BOARDS));

    for (;;) {
        set_all(255, 120, 0, 0, 0, 200, 2000);  /* orange, 2 s fade */
        vTaskDelay(pdMS_TO_TICKS(5000));
        set_all(0, 0, 0, 255, 60, 255, 2000);   /* warm white */
        vTaskDelay(pdMS_TO_TICKS(5000));
        /* ESPHome-style host effect: four SETs/second with local transitions. */
        for (unsigned step = 0; step < 48u; ++step) {
            const unsigned phase = step % 16u;
            const uint8_t level = (uint8_t)((phase <= 8u ? phase : 16u-phase) * 31u);
            set_all(0, 0, 0, 255, 60, level, 250);
            vTaskDelay(pdMS_TO_TICKS(250));
        }
    }
}
