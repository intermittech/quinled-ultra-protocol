/* SPDX-License-Identifier: MIT */
#ifndef ULTRA_SENDER_DESCRIPTOR_H
#define ULTRA_SENDER_DESCRIPTOR_H
#include "ultra_sender.h"
#include "ultra_update.h"
#ifdef __cplusplus
extern "C" {
#endif
/* A borrowed, validated view. Keep the immutable blob alive; no allocation or
 * board registry. Unknown TLVs/tails remain available to a newer consumer. */
typedef struct ultra_descriptor {
    const uint8_t *bytes;
    uint16_t length, crc;
    uint8_t board, variant, lights, channels, outputs, records, flags;
} ultra_descriptor_t;
typedef struct { const uint8_t *value; uint8_t type, length; } ultra_tlv_t;
typedef struct {
    uint8_t id, flags, rates, skew_ns, burst_us, clock_flags, kind, phy, reply, profiles;
    uint16_t hop_ns, gap_us, mtu;
    uint16_t local_latency_us, local_jitter_us; /* 0xFFFF: unknown; no L1 sync claim */
} ultra_desc_link_t;
typedef struct {
    uint8_t id, parent, kind, enforcement, sense, efficiency;
    uint16_t vmin_10mv, vmax_10mv, rating_ma, trip_ma;
} ultra_desc_domain_t;
typedef struct {
    uint8_t first, count, domain, flags, enforcement;
    uint16_t channel_ma, bank_ma, cal_ms;
} ultra_desc_pwm_t;
typedef struct {
    uint8_t output, domain, segments, flags, enforcement, sources, control;
    uint16_t frame_bytes, max_pixels[3], bit_ns, delay_us, chips, rating_ma, fps;
    uint16_t ingress_mask, cal_ms, latch_us;
} ultra_desc_pixel_t;
typedef struct {
    uint8_t id, quantity, domain, target, flags;
    int8_t exponent;
    uint16_t full_scale;
} ultra_desc_sense_t;
typedef struct {
    uint8_t port, qualified, quantum_ns, isr_us, t0_us, uart_gap_us, profiles;
    uint16_t certificate, threshold_ns[4], uart_error_ppm;
} ultra_desc_timing_t;
typedef struct {
    uint16_t frame_pool_bytes, simultaneous_bytes;
    uint32_t render_bytes_s, stream_bytes_s;
    uint8_t active_outputs, calibration_jobs;
} ultra_desc_resources_t;
int ultra_descriptor_decode(ultra_descriptor_t *out, const uint8_t *blob, uint16_t size);
/* cursor=11 initially; 1 record, 0 end, -1 invalid. Complete decode first. */
int ultra_descriptor_next(const ultra_descriptor_t *d, uint16_t *cursor, ultra_tlv_t *out);
int ultra_descriptor_find(const ultra_descriptor_t *d,uint8_t type,uint8_t id,ultra_tlv_t *out);
int ultra_descriptor_link(const ultra_descriptor_t *,uint8_t,ultra_desc_link_t *);
int ultra_descriptor_domain(const ultra_descriptor_t *,uint8_t,ultra_desc_domain_t *);
int ultra_descriptor_pwm(const ultra_descriptor_t *,uint8_t,ultra_desc_pwm_t *);
int ultra_descriptor_pixel(const ultra_descriptor_t *,uint8_t,ultra_desc_pixel_t *);
int ultra_descriptor_sense(const ultra_descriptor_t *,uint8_t,ultra_desc_sense_t *);
int ultra_descriptor_timing(const ultra_descriptor_t *,uint8_t,ultra_desc_timing_t *);
int ultra_descriptor_resources(const ultra_descriptor_t *,ultra_desc_resources_t *);
/* Partial bank semantics, including separate ACK column availability. */
typedef struct { uint8_t channels, lights, outputs; } ultra_bank_view_t;
int ultra_descriptor_bank(const ultra_descriptor_t *,uint8_t page,uint8_t part,uint8_t kind,uint8_t bank,ultra_bank_view_t *);
typedef struct {
    ultra_bank_view_t present;
    uint16_t level[5], input_ma[5];
    uint8_t defined,scene,why;
} ultra_outputs_t;
int ultra_outputs_decode(const ultra_descriptor_t *,uint8_t bank,const uint8_t answer[32],ultra_outputs_t *);
uint32_t ultra_descriptor_calibration_ms(const ultra_descriptor_t *,uint8_t kind,uint8_t index);
/* Binds counts/ratings to scheduler validation. Must precede queued controls. */
int ultra_sender_descriptor(ultra_sender_t *,uint16_t board,const ultra_descriptor_t *);
int ultra_descriptor_config(const ultra_descriptor_t *,uint8_t target,uint8_t id,uint16_t requested,uint16_t *applied);
uint32_t ultra_power_mw(uint16_t ma,uint16_t voltage_10mv);
uint32_t ultra_lifetime_energy_allowance(uint16_t rating_ma,uint16_t vmax_10mv,
                                        uint32_t powered_delta_01h,uint32_t remaining_wh);
uint64_t ultra_sense_full_scale(const ultra_desc_sense_t *,uint32_t *divisor);

typedef struct {
    uint8_t uid[12], boot_version, board, revision, wire;
    uint32_t firmware, features[6];
    uint16_t descriptor_length, descriptor_crc;
    upd_target_t target;
    uint8_t geometry[3], record_format, descriptor_format, entry_10ms, parts;
} ultra_identity_t;
/* Caller identifies the outstanding part; answers do not echo it. No partial
 * mutation on error. Reset the object between devices/boots. */
int ultra_identity_decode(ultra_identity_t *,uint8_t part,const uint8_t answer[32]);
/* Caller-owned 6144 B maximum reassembly, contiguous parts, duplicate-safe. */
typedef struct { uint8_t *bytes; uint16_t capacity, received, length; } ultra_descriptor_rx_t;
int ultra_descriptor_part(ultra_descriptor_rx_t *,uint8_t part,const uint8_t data[24]);
#ifdef __cplusplus
}
#endif
#endif
