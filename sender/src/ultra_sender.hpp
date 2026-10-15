/* SPDX-License-Identifier: MIT
 * Copyright (C) 2026 QuinLED
 *
 * QuinLED Ultra sender: an optional, thin C++ wrapper (C++11) around ultra_sender.h. A line with its storage in one
 * object, no heap:
 *
 *   static ultra::Line<4, ULTRA_SENDER_ITEMS(4, 5, 3)> line;    // up to 4 boards; 5 lights each, DEF + SET + EFFECT
 *   line.begin(5, esp_random());
 *   ultra_light_def_t d; ultra_light_def_init(&d); d.roles[0] = ULTRA_ROLE_BRIGHTNESS;
 *   line.light_def(0, 0, d);
 *   ...
 *   if (uint32_t n = line.next_frame(now_us)) send(line.frame(), n);
 *
 * The object holds the line's storage and the line points into it: it can't be copied or moved (keep it at one
 * place: a static, a member of an object that stays put). begin() again sets the line up anew (it leaves its group);
 * a begin() that is refused leaves a running line as it was. The destructor leaves the group too. Every C function
 * stays available on line.c().
 */
#ifndef ULTRA_SENDER_HPP
#define ULTRA_SENDER_HPP

#include "ultra_sender.h"

namespace ultra {

template<uint16_t Boards, uint16_t Items> class Line {
  static_assert(Boards >= 1 && Boards <= ULTRA_SENDER_MAX_BOARDS, "Boards: 1..ULTRA_SENDER_MAX_BOARDS");

 public:
  Line() = default;
  Line(const Line &) = delete;
  Line &operator=(const Line &) = delete;
  Line(Line &&) = delete;
  Line &operator=(Line &&) = delete;
  ~Line() { this->end(); }

  /// c.boards <= Boards (the storage), the rest as ultra_sender_config_check(); ULTRA_ERR_ARG otherwise: nothing
  /// changed (a running line keeps running, in its group)
  int begin(const ultra_sender_config_t &c) {
    if (c.boards > Boards || ultra_sender_config_check(&c) != ULTRA_OK)
      return ULTRA_ERR_ARG;
    this->end();
    const int r = ultra_sender_init(&line_, &c, boards_, items_, Items);
    begun_ = r == ULTRA_OK;
    return r;
  }
  /// `boards` boards (default: Boards) with `lights` lights each, the recommended timing; seed: random per boot
  int begin(uint8_t lights, uint32_t seed, uint16_t boards = Boards) {
    ultra_sender_config_t c;
    ultra_sender_config_init(&c, boards, lights);
    c.seed = seed;
    return this->begin(c);
  }
  /// the line stops (leaves its group); begin() sets it up again
  void end() {
    if (begun_)
      ultra_sender_deinit(&line_);
    begun_ = false;
  }

  /// the next frame into frame() if one is due (its first bit at now_us): its length, 0 = nothing now
  uint32_t next_frame(int64_t now_us) { return begun_ ? ultra_sender_next_frame(&line_, now_us, frame_) : 0; }
  uint32_t retime(int64_t now_us) { return begun_ ? ultra_sender_retime(&line_, now_us, frame_) : 0; }
  const uint8_t *frame() const { return frame_; }
  bool busy() const { return begun_ && ultra_sender_busy(&line_) != 0; }
  int64_t due_us() const { return ultra_sender_due_us(&line_); }

  int set_lights(uint16_t board, uint8_t lights) { return ultra_sender_set_lights(&line_, board, lights); }
  int light_def(uint16_t board, uint8_t light, const ultra_light_def_t &d) {
    return ultra_sender_light_def(&line_, board, light, &d);
  }
  int light_opt(uint16_t board, uint8_t light, const ultra_light_opt_t &o) {
    return ultra_sender_light_opt(&line_, board, light, &o);
  }
  int set(uint16_t board, uint8_t light, const ultra_values_t &v, uint32_t transition_ms, int64_t now_us,
          uint8_t flags = 0) {
    return ultra_sender_set(&line_, board, light, &v, transition_ms, flags, now_us);
  }
  int stream(uint16_t board, uint8_t light, const ultra_values_t &v, uint32_t glide_ms, int64_t now_us) {
    return ultra_sender_stream(&line_, board, light, &v, glide_ms, now_us);
  }
  int effect(uint16_t board, uint8_t light, uint8_t fx, const uint8_t *params, uint32_t n, int64_t now_us) {
    return ultra_sender_effect(&line_, board, light, fx, params, n, now_us);
  }
  int stop_effect(uint16_t board, uint8_t light, int64_t now_us) {
    return ultra_sender_effect(&line_, board, light, ULTRA_FX_NONE, nullptr, 0, now_us);
  }
  int strobe(uint16_t board, uint8_t light, const ultra_strobe_step_t *steps, uint8_t count, int64_t now_us) {
    return ultra_sender_strobe(&line_, board, light, steps, count, now_us);
  }
  int scene(uint16_t board, uint8_t light, uint8_t mode, uint8_t fade_100ms, const ultra_values_t &v) {
    return ultra_sender_scene(&line_, board, light, mode, fade_100ms, &v);
  }
  int scene_effect(uint16_t board, uint8_t light, uint8_t fx, const uint8_t *params, uint32_t n) {
    return ultra_sender_scene_effect(&line_, board, light, fx, params, n);
  }
  int config(uint16_t board, uint8_t id, uint16_t value) { return ultra_sender_config(&line_, board, id, value); }
  int identify(uint16_t board, uint8_t value, uint8_t arg = 0) {
    return ultra_sender_identify(&line_, board, value, arg);
  }
  void resync(int64_t now_us) { ultra_sender_resync(&line_, now_us); }
  void power(bool on, int64_t now_us) { ultra_sender_power(&line_, on ? 1 : 0, now_us); }
  int power_off() { return ultra_sender_power_off(&line_); }
  int64_t power_off_at() const { return ultra_sender_power_off_at(&line_); }
  /// share starts with the other lines of g (g outlives the line, or the line leaves first)
  void join(ultra_sender_group_t &g) {
    if (begun_)
      ultra_sender_join_group(&line_, &g);
  }
  void leave() {
    if (begun_)
      ultra_sender_leave_group(&line_);
  }

  ultra_sender_t *c() { return &line_; }
  const ultra_sender_t *c() const { return &line_; }

 private:
  ultra_sender_t line_{};
  ultra_sender_board_t boards_[Boards]{};
  ultra_sender_item_t items_[Items > 0 ? Items : 1]{};
  uint8_t frame_[ULTRA_SENDER_FRAME_BYTES(Boards)]{};
  bool begun_{false};
};

}  // namespace ultra

#endif
