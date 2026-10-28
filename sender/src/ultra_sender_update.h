/* SPDX-License-Identifier: MIT */
#ifndef ULTRA_SENDER_UPDATE_H
#define ULTRA_SENDER_UPDATE_H
#include "ultra_sender_descriptor.h"
#ifdef __cplusplus
extern "C" {
#endif
/* Port types. Each has its own board limit, ULTRA_PORT_DIFF_POWER_MAX_BOARDS
 * and ULTRA_PORT_D2A_PIN_MAX_BOARDS (ultra_protocol.h, with their reasons).
 * Not build options, never one limit for all. */
enum { ULTRA_PORT_DIFF_POWER=1, ULTRA_PORT_D2A_PIN=2 };
#define ULTRA_UPDATE_MAX_BOARDS ULTRA_PORT_D2A_PIN_MAX_BOARDS /* storage: the largest port type */
/* The port type's board limit; 0 for an unknown type. */
uint16_t ultra_port_max_boards(uint8_t port_type);
/* Dig2Analog-Plus-5ch pin line speed by board count (decided 2026-10-23):
 * 1..20 boards 800 kHz, 21..34 600 kHz, 35..50 200 kHz; 400 kHz is not picked
 * automatically. Inside the D2A link model's ideal-wiring ceilings 21 / 34 / 50
 * (docs/timing.md, ultra_d2a_admits); 800 stops one hop early to keep wiring
 * reserve. A bench measurement of the real gate skew may move these edges.
 * 35..ULTRA_PORT_D2A_PIN_MAX_BOARDS run 200 kHz. Recorded design limits (timing.md,
 * D2A pin link model, pin table): change only by an explicit decision. */
#define ULTRA_D2A_800_MAX_BOARDS 20u
#define ULTRA_D2A_600_MAX_BOARDS 34u
enum { ULTRA_PORT_PROFILE_DEFAULT=0, ULTRA_PORT_PROFILE_BOARD_COUNT=1,
       ULTRA_PORT_PROFILE_TOO_MANY=2, ULTRA_PORT_PROFILE_NO_BOARDS=3,
       ULTRA_PORT_PROFILE_UNKNOWN_TYPE=4 };
/* The line profile (0..3 = 800/600/400/200 kHz) a port runs for its configured
 * board count; returns the reason. DEFAULT: 800 kHz (a Diff-Power port starts
 * there and steps down only from measured margins). BOARD_COUNT: the slower D2A
 * pin table profile. TOO_MANY / NO_BOARDS / UNKNOWN_TYPE refuse: *profile=255.
 * Leaving 800 still needs a complete all-Ultra inventory (ultra_line_allowed). */
uint8_t ultra_port_line_profile(uint8_t port_type, uint16_t boards, uint8_t *profile);
/* Refusal of a Dig2Analog-Plus-5ch pin line running faster than the table profile
 * for its board count (line->timing.profile below ultra_port_line_profile());
 * every update entry point, before any frame. Slower profiles are admitted;
 * Diff-Power ports are not refused here (they step down from measured margins). */
#define ULTRA_UPDATE_LINE_TOO_FAST (-67)
/* Nonzero when that refusal applies to this line. */
int ultra_port_line_too_fast(uint8_t port_type, const ultra_sender_t *line);
/* Refusal of a port above its type's limit (every update entry point). */
#define ULTRA_UPDATE_TOO_MANY_BOARDS (-64)
/* Decided 2026-10-23: reliability across the whole chain takes priority over
 * speed: every blind family sends >=3 complete passes
 * (update.md, blind group). Recorded design limit: change only by an explicit decision. */
#define ULTRA_UPDATE_BLIND_MIN_PASSES 3u
#define ULTRA_UPDATE_TOO_FEW_PASSES (-66)
/* Quiet LOW after the last UPD frame before ordinary traffic: the 2000 ms
 * transparent idle close (UPD_IDLE_CLOSE_MS) plus receiver tick granularity;
 * reliability over speed (timing.md, update forwarding and entry; update.md,
 * port job). Recorded design limit: change only by an explicit decision. */
#define ULTRA_UPDATE_QUIET_MS 2100u
/* At most three selected-session acknowledgement PINGs, each followed by the
 * full entry bound plus the port gap (update.md, selected status profile).
 * Recorded design limit: change only by an explicit decision. */
#define ULTRA_UPDATE_ACK_COPIES 3u
/* A blind transfer's END has no feedback and a lost one leaves every board of its target in recovery: it is sent
 * three times, each followed by the port gap, and the commit / reboot waits count from the last copy. A receiver
 * ignores a repeat (update.md, blind group). Recorded design limit: change only by an explicit decision. */
#define ULTRA_UPDATE_END_COPIES 3u
/* The session-zero confirmation PING is sent six times, each followed by its confirmation-write pause. An
 * application takes UPD transparency only after two UPD headers in a row and captures the next frame with its old
 * gate plan, so the first confirming capture is the fourth copy (receiver firmware host test); six copies leave
 * three chances for a refused write, and a confirmed application ignores a repeat. The 100-160 ms pauses stay
 * inside the 2000 ms idle close, so the run is never broken. Recorded design limit: change only by an explicit decision. */
#define ULTRA_UPDATE_CONFIRM_COPIES 6u
/* A receiving application captures one frame at a time: a frame that starts before it has handled the previous one
 * is dropped (bench 2026-10-30: 97 % of the frames one 500 us port gap behind a captured one). Every frame a decision
 * rests on (SELECT, each ANNOUNCE half, each END copy) starts at least this long plus the port gap after the frame
 * before it, as the training PINGs do (the last PING too). Recorded design limit: change only by an explicit decision. */
#define ULTRA_UPDATE_CONTROL_PACE_US 10000u
typedef int (*ultra_update_read_t)(void *context,uint32_t offset,uint8_t *data,uint32_t length);
/* Trusted firmware/jig layout facts, NOT addresses supplied by an image. */
typedef struct {
    uint32_t metadata_base, metadata_bytes, bootloader_bytes;
    uint8_t boot_version, geometry[3], entry_10ms;
} ultra_update_layout_t;
typedef struct {
    uint8_t version,state,error,credit,session,flags;
    uint16_t chunk_mask;
    uint32_t accepted,committed,retry_ms,image_crc;
} ultra_update_status_t;
int ultra_update_status_decode(ultra_update_status_t *,const uint8_t answer[32],
                               uint8_t session,const uint8_t uid[12],uint32_t body_length,uint32_t image_crc);
typedef struct {
    uint32_t erase_units, lead_ms, pause_ms, finish_ms, entry_ms;
    uint32_t length, body_crc, descriptor_crc, firmware;
    uint8_t status;
} ultra_update_plan_t;
typedef struct {
    upd_target_t target;
    ultra_update_layout_t layout;
    uint8_t uid[12], has_return;
} ultra_update_participant_t;
/* Fixed for a session. STATUS is only a capability for optional feedback. */
enum { ULTRA_UPDATE_BLIND, ULTRA_UPDATE_SELECTED };
/* Pure preflight; zero is compatible. Full source body is CRC checked before
 * any ANNOUNCE. No erase permission is inferred from a board number. */
int ultra_update_plan(ultra_update_plan_t *,const upd_descriptor_t *,const upd_target_t *,
                       const ultra_update_layout_t *,uint32_t min_gap_us,
                       ultra_update_read_t,void *context);
/* Pure descriptor/geometry preflight for streaming jobs; body CRC is mandatory
 * separately at begin_mode, after the active artifact is prepared. */
int ultra_update_plan_metadata(ultra_update_plan_t *,const upd_descriptor_t *,const upd_target_t *,
                       const ultra_update_layout_t *,uint32_t min_gap_us);
enum { ULTRA_UPDATE_PREAMBLE,ULTRA_UPDATE_SELECT,ULTRA_UPDATE_ANNOUNCE,ULTRA_UPDATE_ENTRY,
       ULTRA_UPDATE_RESELECT,ULTRA_UPDATE_TRAIN,ULTRA_UPDATE_BOOT_ANNOUNCE,
       ULTRA_UPDATE_POLL,ULTRA_UPDATE_DATA,ULTRA_UPDATE_END,ULTRA_UPDATE_ACK,ULTRA_UPDATE_REBOOT,
       ULTRA_UPDATE_QUIET,ULTRA_UPDATE_DARK,ULTRA_UPDATE_NORMAL_TRAIN,ULTRA_UPDATE_CONFIRM_WAIT,
       ULTRA_UPDATE_CONFIRM,ULTRA_UPDATE_DONE,ULTRA_UPDATE_FAILED,ULTRA_UPDATE_ABORTED };

typedef struct {
    uint32_t lead_low_us,marker_high_us,marker_low_us,tail_low_us;
    uint32_t release_us,listen_us; /* relative to physical frame end; STATUS only */
    int64_t frame_start_us,frame_end_us;
    uint8_t marker, handback_required; /* caller completes output frame/latch on release */
} ultra_update_transport_t;
typedef struct {
    ultra_sender_t *line;
    ultra_update_read_t read;
    void *context;
    upd_descriptor_t descriptor;
    upd_target_t target;
    ultra_update_layout_t layout;
    ultra_update_plan_t plan;
    uint8_t uid[12],session,state,half,pass,passes,rate,credit,mask,poll_pending;
    uint32_t offset,committed;
    int64_t due_us,train_until_us,poll_end_us,status_deadline_us;
    int64_t progress_deadline_us;
    uint16_t epoch;
    uint8_t error,mode;
    const ultra_update_participant_t *participants; /* borrowed immutable inventory */
    uint8_t participant_count, confirmations;
    uint8_t confirmed[(ULTRA_UPDATE_MAX_BOARDS+7u)/8u]; /* one bit per participant */
    uint32_t gap_us,dark_left;
    int64_t last_training_us;
    int64_t transit_until_us; /* last ANNOUNCE end + its transit hold; 0 once DATA/END is sent */
    uint8_t transfer_complete;
    ultra_update_transport_t transport; /* adapter instructions for last emitted frame */
} ultra_update_t;
/* Session 1..255, passes 1..15; BLIND requires >=ULTRA_UPDATE_BLIND_MIN_PASSES
 * (fewer: ULTRA_UPDATE_TOO_FEW_PASSES). SELECTED uses status/credit feedback.
 * Inventory/routing must authorize all update participants before begin.
 * Every entry point requires port_type; unknown types return -1 and an
 * oversized physical line returns ULTRA_UPDATE_TOO_MANY_BOARDS. */
int ultra_update_begin_mode(ultra_update_t *,ultra_sender_t *,const upd_descriptor_t *,const upd_target_t *,
                        const ultra_update_layout_t *,const uint8_t uid[12],uint8_t port_type,uint8_t session,
                        uint8_t passes,uint8_t return_rate,uint8_t mode,int64_t now_us,ultra_update_read_t,void *);
/* Convenience entry point explicitly choosing BLIND; selected callers use begin_mode. */
int ultra_update_begin(ultra_update_t *,ultra_sender_t *,const upd_descriptor_t *,const upd_target_t *,
                        const ultra_update_layout_t *,const uint8_t uid[12],uint8_t port_type,uint8_t session,
                        uint8_t passes,uint8_t return_rate,int64_t now_us,ultra_update_read_t,void *);
/* Declared groups use the slowest bound of EVERY matching participant. Status
 * sessions are deliberately one UID at a time, downstream first. More
 * participants or line boards than ultra_port_max_boards(port_type):
 * ULTRA_UPDATE_TOO_MANY_BOARDS; unknown port type: -1. */
int ultra_update_begin_group(ultra_update_t *,ultra_sender_t *,const upd_descriptor_t *,
                              const ultra_update_participant_t *,uint16_t count,uint8_t port_type,
                              uint8_t session,uint8_t passes,int64_t now_us,ultra_update_read_t,void *);
/* Frame bytes, 0 waiting, -1 failed. now is the envelope start. For CHAIN
 * acquisition, capacity must fit ultra_frame_bytes(line->n). Follow transport
 * envelope and due_us; do not retime an emitted frame. No source buffer retained. */
int ultra_update_next_frame(ultra_update_t *,int64_t now_us,uint8_t *,uint32_t capacity);
/* 48-byte convenience wrapper (one-node normal acquisition only). */
int ultra_update_next(ultra_update_t *,int64_t now_us,uint8_t frame[48]);
int ultra_update_answer(ultra_update_t *,const uint8_t answer[32],int64_t now_us);
/* Requires matching full rediscovered identity and installed verified+confirmed
 * FW_UPDATE page; missing/one-way evidence NEVER becomes success. */
int ultra_update_confirm(ultra_update_t *,const ultra_identity_t *,const uint8_t answer[32]);
/* Emits ABORT only outside every busy/return deadline and after the last
 * ordinary frame plus the processing LOW (0: retry at due_us); old image may be revoked.
 * Quiet after it: ULTRA_UPDATE_QUIET_MS, and while an ANNOUNCE has no DATA/END
 * after it, its transit hold + ULTRA_UPDATE_QUIET_MS from that ANNOUNCE's end. */
int ultra_update_abort(ultra_update_t *,int64_t now_us,uint8_t frame[48]);
#ifdef __cplusplus
}
#endif
#endif
