/* SPDX-License-Identifier: MIT */
#ifndef ULTRA_SENDER_PROFILE_H
#define ULTRA_SENDER_PROFILE_H
#include "ultra_sender.h"
#ifdef __cplusplus
extern "C" {
#endif
/* Admission evidence is per port. All masks include every app and immutable
 * bootloader. Observed/gate masks are qualified measurements at THAT profile. */
typedef struct {
    uint8_t inventory_complete, all_ultra, common_mask, qualified_mask;
    uint8_t observed_mask, gate_mask;
    uint16_t path_ns;
    uint32_t min_gap_us;
} ultra_profile_evidence_t;
int ultra_profile_allowed(uint8_t profile, const ultra_profile_evidence_t *e);
int ultra_profile_trainable(uint8_t profile,const ultra_profile_evidence_t *e);
/* Caller must have released DMA ownership. The scheduler also checks its last
 * frame/reset, return/busy deadline and update lease. Discard old waveforms. */
int ultra_sender_profile(ultra_sender_t *l, uint8_t profile, uint32_t clock_hz,
                         const ultra_profile_evidence_t *e, int64_t now_us);
/* Acquires an unmeasured candidate using qualified inventory/path evidence.
 * Queued controls remain blocked until accept() sees positive fresh margins.
 * Manual overrides use the stricter profile() entry point. */
int ultra_sender_profile_acquire(ultra_sender_t *,uint8_t profile,uint32_t clock_hz,
                                 const ultra_profile_evidence_t *,int64_t now_us);
int ultra_sender_profile_accept(ultra_sender_t *,const ultra_profile_evidence_t *);
/* phase 0..3 = 55/AA/00/FF. No command queue is consumed. Send known training
 * windows at the refresh cadence, alongside header/return scheduling. */
uint32_t ultra_sender_training_frame(ultra_sender_t *,uint8_t phase,
                                     const ultra_profile_evidence_t *,int64_t now_us,uint8_t *frame);
typedef struct {
    uint8_t port, phy, flags, quantum_ns, profile, supported;
    uint16_t zero_min,zero_max,zero_mean,one_min,one_max,one_mean;
    uint16_t zeros,ones,invalid,epoch,period_min,period_max,low_min,low_max;
    uint16_t frames,crc_bad,missing,scale_ppm;
    uint32_t boot;
    int64_t captured_ms;
    uint8_t parts;
    uint8_t rate_request;             /* decoded RATE_REQUEST; host decides per port */
} ultra_link_stats_t;
/* part 0 starts a new frozen pair. Boot is the caller's discovery generation;
 * distinct sessions/epochs never combine, even across u16 epoch wrap. */
int ultra_link_stats_decode(ultra_link_stats_t *,uint8_t part,const uint8_t answer[32],
                            uint32_t boot,int64_t now_ms);
int ultra_link_stats_margin(const ultra_link_stats_t *,uint32_t boot,int64_t now_ms,
                            uint16_t threshold_min,uint16_t threshold_max,int32_t *margin_ns);
enum { ULTRA_AUTO_OK, ULTRA_AUTO_WARNING, ULTRA_AUTO_RECOVERING,
       ULTRA_AUTO_UNKNOWN, ULTRA_AUTO_FAILED };
enum { ULTRA_AUTO_NONE, ULTRA_AUTO_DOWN, ULTRA_AUTO_RECOVER,
       ULTRA_AUTO_PROBE, ULTRA_AUTO_REVERT, ULTRA_AUTO_UP, ULTRA_AUTO_MANUAL };
enum { ULTRA_AUTO_REASON_NONE, ULTRA_AUTO_REASON_MARGIN, ULTRA_AUTO_REASON_ERRORS,
       ULTRA_AUTO_REASON_MISSING, ULTRA_AUTO_REASON_ADMISSION,
       ULTRA_AUTO_REASON_PROBE, ULTRA_AUTO_REASON_OVERRIDE };
typedef struct {
    uint8_t active, last_good, probing, bad, good, state, reason, warning;
    uint8_t mode;                     /* 255 AUTO, else profile 0..3 */
    int64_t changed_ms, last_window_ms, reason_ms;
} ultra_auto_t;
typedef struct {
    int64_t now_ms;
    uint8_t valid, profile, crc_bad, invalid_runs, missing_frames, repeated_loss;
    uint8_t idle, transaction_active, update_active;
    int32_t margin_ns;                /* min of ALL required fresh reports */
    ultra_profile_evidence_t evidence;
} ultra_auto_input_t;
typedef struct {
    uint8_t action, profile, stop_pixels, state, reason;
} ultra_auto_result_t;
void ultra_auto_init(ultra_auto_t *s, int64_t now_ms);
/* Pure policy: call once per completed one-second training window. Caller
 * applies any requested switch at a frame boundary and reacquires. Current
 * rate clean windows NEVER count as faster-rate probes. */
ultra_auto_result_t ultra_auto_step(ultra_auto_t *s, const ultra_auto_input_t *in);

/* Portable PARLIO sample composer. A group is a logical transfer per profile;
 * groups run concurrently in one continuous 9.6 MHz stream. No shared slow
 * superframe, heap, peripheral driver or implied rounded clock acceptance. */
typedef struct {
    const uint8_t *bytes;             /* retained until done, including reset */
    uint32_t length, sample, reset_samples;
    uint8_t profile, lane, active;
} ultra_parlio_lane_t;
typedef struct { uint32_t lanes; uint8_t profile; } ultra_parlio_group_t;
int ultra_parlio_groups(const ultra_parlio_lane_t *lanes, uint8_t count,
                        ultra_parlio_group_t groups[4]);
int ultra_parlio_begin(ultra_parlio_lane_t *lane, uint8_t pin, uint8_t profile,
                       const uint8_t *bytes, uint32_t size, uint32_t actual_hz,
                       uint32_t gap_us);
/* Writes count uint32 sample words; inactive lanes LOW. completed is a pin mask.
 * Call without a DMA gap; refill/enqueue completed fast lanes independently. */
void ultra_parlio_encode(ultra_parlio_lane_t *lanes, uint8_t n, uint32_t *out,
                         uint32_t count, uint32_t *completed);
#ifdef __cplusplus
}
#endif
#endif
