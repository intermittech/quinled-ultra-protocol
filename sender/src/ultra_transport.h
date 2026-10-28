/* SPDX-License-Identifier: MIT
 * Optional host/adapter helpers. No receiver-side descriptor parser or heap.
 * Normative adapter semantics: docs/transport.md.
 */
#ifndef ULTRA_TRANSPORT_H
#define ULTRA_TRANSPORT_H
#include "ultra_protocol.h"
#ifdef __cplusplus
extern "C" {
#endif
#define ULTRA_LOCAL_OVERHEAD 10u
#define ULTRA_LOCAL_READY   0u
#define ULTRA_LOCAL_SLOT    1u
#define ULTRA_LOCAL_ANSWER  2u
#define ULTRA_LOCAL_UPDATE  3u
static inline uint16_t ultra_local_payload_size(uint8_t kind) {
    return kind==0u?0u:kind==1u||kind==2u?32u:kind==3u?48u:65535u;
}
/* Kind 1 START/phase offsets use receiver packet acceptance, not trailer TIME.
 * Acceptance is the captured end of the final CRC byte of a complete valid
 * packet, before dispatch; invalid packets never establish an origin. Duplicate
 * txid/command SEQ does not reapply or move the original start (transport.md).
 * Returns packet bytes, or 0 without writing on invalid input/capacity. */
static inline uint16_t ultra_local_build(uint8_t *dst, uint16_t capacity,
        uint8_t endpoint, uint8_t kind, uint16_t txid,
        const uint8_t *payload, uint16_t length) {
    unsigned i;
    if(!dst || endpoint==255u || length!=ultra_local_payload_size(kind) ||
       kind>3u || capacity<(uint32_t)length+10u || (length&&!payload)) return 0;
    dst[0]=0xa6;dst[1]=0x31;dst[2]=endpoint;dst[3]=kind;
    ultra_put16(dst+4,txid);ultra_put16(dst+6,length);
    for(i=0;i<length;++i)dst[8+i]=payload[i];
    ultra_put16(dst+8+length,ultra_crc16(dst,8u+length));
    return (uint16_t)(length+10u);
}
static inline int ultra_local_check(const uint8_t *packet, uint16_t size) {
    uint16_t n;
    if(!packet || size<10u || packet[0]!=0xa6 || packet[1]!=0x31 ||
       packet[2]==255u || packet[3]>3u) return -1;
    n=(uint16_t)ultra_get16(packet+6);
    if(n!=ultra_local_payload_size(packet[3]) || (uint32_t)n+10u!=size) return -1;
    if(ultra_crc16(packet,size-2u)!=ultra_get16(packet+size-2u)) return -1;
    /* An intact envelope cannot authorize a corrupt inner command/answer. */
    if(packet[3]==ULTRA_LOCAL_SLOT && ultra_slot_check(packet+8)) return -1;
    if(packet[3]==ULTRA_LOCAL_ANSWER && ultra_answer_check(packet+8)) return -1;
    return 0;
}
/* Decode the local logical start of a new kind-1 command. Caller supplies the
 * captured acceptance in monotonic us, >=0, and performs capability/dedup checks.
 * SET has signed i16 ms; PIXEL_FX signed i32 ms. Untimed slots use zero.
 * A past start catches up at acceptance; never shift it to handler entry time.
 * Actual dispatch/output must meet finite LINK local latency/jitter bounds.
 * Returns -1 on invalid packet/input/overflow, without writing *start_us. */
static inline int ultra_local_start_us(const uint8_t *packet,uint16_t size,
        int64_t accepted_us,int64_t *start_us) {
    const uint8_t *s;uint8_t type;uint32_t raw;int64_t ms=0,delta;
    if(!start_us || accepted_us<0 || ultra_local_check(packet,size) ||
       packet[3]!=ULTRA_LOCAL_SLOT) return -1;
    s=packet+8;type=s[0]&ULTRA_TYPE_MASK;
    if(type==ULTRA_SET) {
        raw=ultra_get16(s+28);ms=raw;
        if(raw&0x8000u) ms-=65536;
    } else if(type==ULTRA_PIXEL_FX) {
        raw=ultra_get32(s+4);ms=raw;
        if(raw&0x80000000u) ms-=INT64_C(4294967296);
    }
    delta=ms*1000;
    if(delta>0 && accepted_us>INT64_MAX-delta) return -1;
    *start_us=accepted_us+delta;return 0;
}
/* Exact sample counts at 9.6 MHz. Tables are const, no mutable global profile.
 * Backend duration = count / actual clock; do not round each ns then re-encode. */
typedef struct {
    uint8_t bit_samples, zero_samples, one_samples;
    uint8_t threshold_36mhz, threshold_50mhz;
    uint16_t admission_ns;
} ultra_line_profile_t;
/* The four exact profiles and their certified path admission 220 / 300 / 450 /
 * 750 ns (timing.md, four exact profiles). Recorded design limit: change only by
 * an explicit decision. */
static inline const ultra_line_profile_t *ultra_line_profile(uint8_t id) {
    static const ultra_line_profile_t profiles[4]={
        {12,3,9,22,31,220}, {16,4,12,30,42,300},
        {24,6,18,45,62,450}, {48,12,36,90,125,750}
    };
    return id<4u ? &profiles[id] : (const ultra_line_profile_t *)0;
}
/* Functional path budget: n hops and n-1 forwarding allowances.
 * Cable bound is the sum of independently qualified segments, not propagation. */
static inline uint16_t ultra_line_path_ns(uint8_t boards, uint16_t cable_sum_ns) {
    uint32_t value;
    if(!boards || boards>ULTRA_PORT_DIFF_POWER_MAX_BOARDS) return 65535u;
    value=ULTRA_CHAIN_GUARANTEE_HOP_NS*(uint32_t)boards+ULTRA_CHAIN_FORWARD_NS*((uint32_t)boards-1u)+cable_sum_ns;
    return value<65535u?(uint16_t)value:65535u;
}
/* Inventory_complete/all_ultra include passive pixels and unresponsive devices.
 * common_mask includes every app AND bootloader. Never infer it from silence. */
static inline int ultra_line_allowed(uint8_t profile, int inventory_complete,
        int all_ultra, uint8_t common_mask, uint16_t path_ns) {
    const ultra_line_profile_t *p=ultra_line_profile(profile);
    return p && (common_mask&(1u<<profile)) && path_ns<=p->admission_ns &&
        (profile==0u || (inventory_complete && all_ultra));
}
/* RMT rounding to nearest tick. At exact half ticks widen the HIGH separation:
 * zero rounds down, one rounds up; period rounds nearest/up. 40/80 MHz only.
 * Use actual durations/rate for TIME and margin, with residual +/-10 ns. */
typedef struct { uint16_t bit_ticks, zero_ticks, one_ticks; uint32_t hz; } ultra_line_rmt_t;
static inline int ultra_line_rmt(uint8_t id, uint32_t hz, ultra_line_rmt_t *out) {
    const ultra_line_profile_t *p=ultra_line_profile(id);
    uint32_t mhz;
    if(!p || !out || (hz!=40000000u && hz!=80000000u)) return -1;
    mhz=hz/1000000u;
    out->bit_ticks=(uint16_t)((p->bit_samples*mhz*5u+24u)/48u);
    out->zero_ticks=(uint16_t)((p->zero_samples*mhz*5u+23u)/48u);
    out->one_ticks=(uint16_t)((p->one_samples*mhz*5u+24u)/48u);
    out->hz=hz;return 0;
}
/* Dig2Analog-Plus-5ch data pin, never the Diff-Power path above (timing.md, D2A
 * link model): board N sees N-1 forwarding gates of ULTRA_D2A_HOP_NS each plus
 * wiring_ns, every other cable/input width error up to its sampler (0 = ideal
 * wiring, the documented assumption). 1..ULTRA_PORT_D2A_PIN_MAX_BOARDS boards.
 * Recorded design limits (timing.md, D2A pin link model): the +-9 ns hop bound
 * from the gate's 1..10 ns delay limits; a forwarding board reopens its gate
 * <= 100 ns after its removed slot; the sender's +-10 ns HIGH and period residual
 * charges 20 ns on a LOW. Change only by an explicit decision. */
#define ULTRA_D2A_HOP_NS 9u
#define ULTRA_D2A_GATE_REOPEN_NS 100u
#define ULTRA_D2A_LOW_RESIDUAL_NS 20u
static inline uint16_t ultra_d2a_path_ns(uint8_t boards, uint16_t wiring_ns) {
    uint32_t value;
    if(!boards || boards>ULTRA_PORT_D2A_PIN_MAX_BOARDS) return 65535u;
    value=ULTRA_D2A_HOP_NS*((uint32_t)boards-1u)+wiring_ns;
    return value<65535u?(uint16_t)value:65535u;
}
/* Timing admission of a D2A pin: the final path within the profile's admission,
 * and the last forwarding board's LOW, after the sender backend with the shortest
 * LOW (PARLIO, RMT 40/80 MHz) and the 20 ns residual, above the 100 ns gate
 * reopening. Exact in 1/6 ns. Inventory and common mask: ultra_line_allowed. */
static inline int ultra_d2a_admits(uint8_t profile, uint8_t boards, uint16_t wiring_ns) {
    const ultra_line_profile_t *p=ultra_line_profile(profile);
    ultra_line_rmt_t r;
    uint32_t low,x,mhz;
    if(!p || ultra_d2a_path_ns(boards,wiring_ns)>p->admission_ns) return 0;
    if(boards<2u) return 1;
    low=(uint32_t)(p->bit_samples-p->one_samples)*625u;   /* 9.6 MHz sample = 625/6 ns */
    for(mhz=40u;mhz<=80u;mhz+=40u) {
        if(ultra_line_rmt(profile,mhz*1000000u,&r)) return 0;
        x=(uint32_t)(r.bit_ticks-r.one_ticks)*(6000u/mhz);
        if(x<low) low=x;
    }
    return low>6u*(ULTRA_D2A_GATE_REOPEN_NS+ULTRA_D2A_LOW_RESIDUAL_NS+
                   (uint32_t)ultra_d2a_path_ns((uint8_t)(boards-1u),wiring_ns));
}
/* Signed observed margin; extrema must include outward measurement rounding.
 * threshold bounds include raw HSI/quantization; uncertainty includes sample
 * quantum plus calibrated scale error. Invalid/missing reports are NOT zero. */
static inline int32_t ultra_line_margin_ns(uint16_t zero_max, uint16_t one_min,
        uint16_t high_min, uint16_t low_min, uint16_t threshold_min,
        uint16_t threshold_max, uint16_t uncertainty_ns) {
    int32_t m=(int32_t)threshold_min-zero_max-uncertainty_ns;
    int32_t x=(int32_t)one_min-threshold_max-uncertainty_ns;
    if(x<m)m=x;
    x=(int32_t)high_min-uncertainty_ns;if(x<m)m=x;
    x=(int32_t)low_min-uncertainty_ns;if(x<m)m=x;
    return m;
}
/* Single-window policy decision; caller owns per-port consecutive counters,
 * 60-second dwell, live probe, inventory and qualification checks. Returns a
 * candidate only, NEVER permission to transmit an unsupported profile. */
static inline uint8_t ultra_line_candidate(uint8_t current, uint8_t override_id,
        int reports_valid, int hard_fault, uint8_t bad_windows,
        uint8_t good_probe_windows, uint32_t dwell_ms) {
    if(current>=4u) return 0u;
    if(override_id<4u) return override_id;
    if(override_id!=255u || !reports_valid) return current;
    if(hard_fault) return 3u;
    if(bad_windows>=ULTRA_LINK_DOWN_WINDOWS && current<3u)
        return (uint8_t)(current+1u);
    if(current && good_probe_windows>=ULTRA_LINK_UP_WINDOWS &&
       dwell_ms>=ULTRA_LINK_DWELL_MS) return (uint8_t)(current-1u);
    return current;
}
/* Byte builder only: transaction offset/CRC/admission checks belong to the
 * receiver state machine. data is 24 bytes, including required zero padding. */
static inline int ultra_slot_pixel_data(uint8_t *slot, uint8_t output, uint8_t seq,
        uint8_t operation, uint16_t offset, const uint8_t *data) {
    unsigned i;
    if(!slot || !data || output>ULTRA_PIXEL_OUTPUT_MAX || operation>3u) return -1;
    ultra_slot_begin(slot,ULTRA_PIXEL_DATA,output,0,seq);
    slot[3]=operation;ultra_put16(slot+4,offset);
    for(i=0;i<24u;++i)slot[6+i]=data[i];
    ultra_slot_seal(slot);return 0;
}
#ifdef __cplusplus
}
#endif
#endif
