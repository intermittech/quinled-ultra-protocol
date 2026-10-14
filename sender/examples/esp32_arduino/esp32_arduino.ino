// SPDX-License-Identifier: MIT
// Copyright (C) 2026 QuinLED
//
// QuinLED Ultra sender: a minimal Arduino-ESP32 sketch (Arduino-ESP32 >= 3.1, ESP-IDF 5.3 inside). Two boards with
// five single-colour lights each; a light chase with 400 ms fades, every board in step.
//
// Install the library first: the sender/ folder of quinled-ultra-protocol (Arduino IDE: zip it, Sketch > Include
// Library > Add .ZIP Library; PlatformIO: lib/ or lib_deps). The data pin goes through a 5 V buffer (74AHCT125) and
// ~33 ohm to the first board.

#include <Arduino.h>

#include "ultra_esp32_rmt.h"
#include "ultra_sender.hpp"

static constexpr int DATA_PIN = 16;
static constexpr uint16_t BOARDS = 2;

static ultra::Line<BOARDS, ULTRA_SENDER_ITEMS(BOARDS, 5, 2)> line;   // LIGHT_DEF + SET per light
static ultra_esp32_rmt_t out;
static bool ready = false;
static bool stuck = false;    // the RMT couldn't be stopped: out and the line's frame stay as they are
static uint32_t pending = 0;  // a frame that still has to go out

void setup() {
  Serial.begin(115200);
  ultra_sender_config_t cfg;
  ultra_sender_config_init(&cfg, BOARDS, 5);
  cfg.seed = esp_random();
  // the RMT first: the line computes TIME with the timing the RMT really sends
  if (ultra_esp32_rmt_begin(&out, DATA_PIN, &cfg.timing) != 0) {
    Serial.println("RMT setup failed");
    return;
  }
  if (line.begin(cfg) != ULTRA_OK) {
    Serial.println("Ultra line setup failed");
    (void)ultra_esp32_rmt_end(&out);  // nothing was sent: it releases
    return;
  }
  for (uint16_t b = 0; b < BOARDS; b++) {
    for (uint8_t l = 0; l < 5; l++) {
      ultra_light_def_t d;
      ultra_light_def_init(&d);  // gamma 2.8 on the board
      d.roles[l] = ULTRA_ROLE_BRIGHTNESS;
      line.light_def(b, l, d);
    }
  }
  ready = true;
}

void loop() {
  if (stuck && ultra_esp32_rmt_end(&out) == 0) {
    stuck = false;  // stopped at last: released (a restart sets it up again)
    Serial.println("RMT released");
  }
  if (!ready)
    return;
  static uint32_t last_ms = 0;
  static uint8_t pos = 0;
  // every 500 ms the next light of the chase: on, the others off, all with a 400 ms fade
  if (millis() - last_ms >= 500) {
    last_ms = millis();
    for (uint16_t b = 0; b < BOARDS; b++) {
      for (uint8_t l = 0; l < 5; l++) {
        ultra_values_t v;
        v.m = (b * 5 + l == pos) ? 65535 : 0;
        for (int i = 0; i < 5; i++) {
          v.a[i] = 65535;
          v.b[i] = 65535;
        }
        line.set(b, l, v, 400, esp_timer_get_time(), ULTRA_SET_SMOOTH);
      }
    }
    pos = (pos + 1) % (BOARDS * 5);
  }
  // a frame whenever one is due: back to back while commands wait, a refresh every 200 ms at rest. One that didn't
  // go out is retimed and sent on the next pass.
  pending = pending != 0 ? line.retime(esp_timer_get_time()) : line.next_frame(esp_timer_get_time());
  if (pending == 0)
    return;
  const int r = ultra_esp32_rmt_send(&out, line.frame(), pending);
  if (r == ULTRA_RMT_OK) {
    pending = 0;
  } else if (r == ULTRA_RMT_STUCK) {
    // the driver may still read line.frame() and out (both static: they stay): released only once it stops
    Serial.println("RMT stuck: output stopped");
    ready = false;
    stuck = ultra_esp32_rmt_end(&out) != 0;  // not yet: loop() tries again
  }
}
