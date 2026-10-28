// SPDX-License-Identifier: MIT
// Copyright (C) 2026 QuinLED
//
// The C++ wrapper (ultra_sender.hpp) at run time (review round 7, finding 1 and 2), and clean at -Wall -Wextra:
//  - begin() with more boards than the object holds is refused, nothing outside it changes;
//  - fewer boards than it holds work, frames included;
//  - it can't be copied or moved (it holds the storage its line points into);
//  - begin() again and the destructor take the line out of its group;
//  - a refused begin() (lights, timing, boards) leaves a running line as it was, in its group (re-check N3).
// Also compiled (only) by the cross compilers' check.

#include "ultra_sender.hpp"

using Small = ultra::Line<1, 0>;
using Four = ultra::Line<4, ULTRA_SENDER_ITEMS(4, 5, 3)>;

// The C++ smoke the cross compilers check (compile only, freestanding: nothing from the standard library)
uint32_t ultra_cpp_wrapper_smoke(int64_t now_us);
uint32_t ultra_cpp_wrapper_smoke(int64_t now_us) {
  static Four line;
  if (line.begin(5, 1234) != ULTRA_OK)
    return 0;
  ultra_light_def_t d;
  ultra_light_def_init(&d);
  d.roles[0] = ULTRA_ROLE_BRIGHTNESS;
  line.light_def(0, 0, d);
  ultra_values_t v{};
  v.m = 32768;
  for (int i = 0; i < 5; i++) {
    v.a[i] = 65535;
    v.b[i] = 65535;
  }
  line.set(0, 0, v, 500, now_us, ULTRA_SET_SMOOTH);
  line.stream(1, 0, v, 250, now_us);
  line.config(ULTRA_SENDER_ALL, ULTRA_CFG_PWM_PRESET, 8);
  line.identify(2, ULTRA_IDENT_VERSION);
  const uint32_t n = line.next_frame(now_us) + (line.busy() ? 1u : 0u) + line.frame()[0];
  line.power_off();
  return n + (line.power_off_at() == INT64_MAX ? 1u : 0u);
}

#ifndef ULTRA_CPP_COMPILE_ONLY
#include <cstdio>
#include <cstring>
#include <type_traits>

static_assert(!std::is_copy_constructible<Four>::value, "a line can't be copied");
static_assert(!std::is_copy_assignable<Four>::value, "a line can't be copied");
static_assert(!std::is_move_constructible<Four>::value, "a line can't be moved");
static_assert(!std::is_move_assignable<Four>::value, "a line can't be moved");

static int failures, checks;
#define CHECK(c, ...) \
  do { \
    checks++; \
    if (!(c)) { \
      failures++; \
      std::printf("FAIL %s:%d: ", __FILE__, __LINE__); \
    } else { \
      std::printf("ok   "); \
    } \
    std::printf(__VA_ARGS__); \
    std::printf("\n"); \
  } while (0)

static uint32_t group_members(const ultra_sender_group_t &g) {
  uint32_t k = 0;
  for (const ultra_sender_t *x = g.lines; x != nullptr && k < 100; x = x->group_next)
    k++;
  return k;
}

int main() {
  // 1: more boards than the object holds: refused, the memory around it untouched
  {
    struct Guarded {
      unsigned char before[256];
      Small line;
      unsigned char after[16384];
    };
    static Guarded t;
    std::memset(t.before, 0xA5, sizeof t.before);
    std::memset(t.after, 0xA5, sizeof t.after);
    ultra_sender_config_t cfg;
    ultra_sender_config_init(&cfg, 64, 1);
    const int rc = t.line.begin(cfg);
    unsigned changed = 0;
    for (unsigned i = 0; i < sizeof t.after; i++)
      changed += t.after[i] != 0xA5;
    for (unsigned i = 0; i < sizeof t.before; i++)
      changed += t.before[i] != 0xA5;
    CHECK(rc == ULTRA_ERR_ARG && changed == 0, "1: Line<1, 0>.begin(64 boards): refused (%d), %u bytes around it changed",
          rc, changed);
    CHECK(t.line.next_frame(1000) == 0 && !t.line.busy(), "1: ... and it sends nothing");
    ultra_sender_config_init(&cfg, 0, 1);
    CHECK(t.line.begin(cfg) == ULTRA_ERR_ARG, "1: 0 boards refused");
  }
  // fewer boards than it holds: the frames are that many boards long
  {
    static Four line;
    CHECK(line.begin(1, 99, 2) == ULTRA_OK, "1: Line<4, ...>.begin(2 boards)");
    const uint32_t n = line.next_frame(1000);
    CHECK(n == ultra_frame_bytes(2), "1: its frames are 2 boards long (%u bytes)", n);
    CHECK(line.set_lights(2, 1) == ULTRA_ERR_ARG, "1: board 2 doesn't exist on it");
    ultra_sender_config_t cfg;
    ultra_sender_config_init(&cfg, 4, 1);
    CHECK(line.begin(cfg) == ULTRA_OK && line.next_frame(2000) == ultra_frame_bytes(4), "1: begin again with all 4");
  }
  // 2 + 4: group lifecycle through the wrapper
  {
    ultra_sender_group_t g;
    ultra_sender_group_init(&g);
    static Four a;
    a.begin(1, 1);
    a.join(g);
    {
      Four b;                                    // a line that goes away while in the group
      b.begin(1, 2);
      b.join(g);
      CHECK(group_members(g) == 2, "2: two lines in the group");
    }
    CHECK(group_members(g) == 1 && g.lines == a.c(), "2: the destroyed one left the group");
    a.begin(1, 3);                               // begin again: out of the group, a group of its own
    CHECK(group_members(g) == 0, "4: begin() again leaves the group");
    a.join(g);
    a.join(g);
    CHECK(group_members(g) == 1, "4: joined twice: once");
    ultra_values_t v{};
    CHECK(a.set(0, 0, v, 0, 5000) == ULTRA_OK, "4: a start time in the group returns");
    a.end();
    CHECK(group_members(g) == 0 && a.next_frame(6000) == 0, "4: end(): out of the group, no frames");
  }
  // N3: a refused begin() leaves a running line running, in its group (and the C init leaves a line untouched)
  {
    ultra_sender_group_t g;
    ultra_sender_group_init(&g);
    static Four line;
    ultra_sender_config_t ok;
    ultra_sender_config_init(&ok, 2, 1);
    CHECK(line.begin(ok) == ULTRA_OK, "N3: a line of 2 boards");
    line.join(g);
    const uint32_t first = line.next_frame(1000000);
    struct Bad {
      const char *what;
      ultra_sender_config_t c;
    } bad[4];
    for (auto &b : bad)
      b.c = ok;
    bad[0].what = "6 lights";
    bad[0].c.lights = 6;
    bad[1].what = "T0H 100 ns";
    bad[1].c.timing.t0h_ns = 100;
    bad[2].what = "5 boards (holds 4)";
    bad[2].c.boards = 5;
    bad[3].what = "0 boards";
    bad[3].c.boards = 0;
    for (auto &b : bad) {
      ultra_sender_t before;
      std::memcpy(&before, line.c(), sizeof before);
      const int rc = line.begin(b.c);
      CHECK(rc == ULTRA_ERR_ARG && std::memcmp(&before, line.c(), sizeof before) == 0 && line.c()->n == 2 &&
                g.lines == line.c() && group_members(g) == 1,
            "N3: begin(%s) refused (%d): the line unchanged, still in its group", b.what, rc);
    }
    CHECK(first == ultra_frame_bytes(2) && line.next_frame(2000000) == ultra_frame_bytes(2),
          "N3: and it keeps sending its 2-board frames");
    ultra_sender_t raw, before;
    static ultra_sender_board_t bd[2];
    std::memset(&raw, 0x5A, sizeof raw);
    std::memcpy(&before, &raw, sizeof raw);
    CHECK(ultra_sender_init(&raw, &bad[0].c, bd, nullptr, 0) == ULTRA_ERR_ARG &&
              ultra_sender_init(&raw, &bad[1].c, bd, nullptr, 0) == ULTRA_ERR_ARG &&
              std::memcmp(&raw, &before, sizeof raw) == 0 && ultra_sender_config_check(&ok) == ULTRA_OK &&
              ultra_sender_config_check(&bad[0].c) == ULTRA_ERR_ARG,
          "N3: ultra_sender_init() refusing a configuration changes nothing; ultra_sender_config_check() agrees");
    line.end();
  }
  // 0.2.6: the power-off notice through the wrapper
  {
    static Four line;
    line.begin(1, 5);
    CHECK(line.power_off_at() == INT64_MIN && line.power_off() == ULTRA_OK && line.power_off_at() == INT64_MAX &&
              line.power_off() == ULTRA_ERR_ARG,
          "power_off(): given once, the cut not known before its first copy");
    const uint32_t n = line.next_frame(1000000);
    CHECK(n == ultra_frame_bytes(4) && line.frame()[0] == ULTRA_IDENTIFY && line.frame()[ULTRA_PAYLOAD] ==
              ULTRA_IDENT_POWER_OFF, "power_off(): the next frame is the notice");
    line.end();
  }
  // the line's pointers stay inside the object
  {
    static Four line;
    line.begin(5, 7);
    const ultra_sender_t *l = line.c();
    const char *lo = reinterpret_cast<const char *>(&line), *hi = lo + sizeof line;
    CHECK(reinterpret_cast<const char *>(l->boards) >= lo && reinterpret_cast<const char *>(l->boards) < hi &&
              reinterpret_cast<const char *>(l->items) >= lo && reinterpret_cast<const char *>(l->items) < hi,
          "2: the line's storage is the object's own");
  }
  CHECK(ultra_cpp_wrapper_smoke(1000000) != 0, "the smoke builds a frame");
  std::printf("%s: %d checks, %d failures\n", failures ? "FAILED" : "ALL C++ WRAPPER TESTS PASSED", checks, failures);
  return failures ? 1 : 0;
}
#endif
