/* SPDX-License-Identifier: MIT */
#ifndef ULTRA_SENDER_JOB_H
#define ULTRA_SENDER_JOB_H
#include "ultra_sender_update.h"
#ifdef __cplusplus
extern "C" {
#endif
/* No allocation: inventory, artifacts and one result per node are borrowed for
 * the job's lifetime. options.port_type sets the board limit
 * (ultra_port_max_boards); job_begin refuses a larger inventory or line with
 * ULTRA_UPDATE_TOO_MANY_BOARDS, a Dig2Analog-Plus-5ch pin running faster than its
 * speed table with ULTRA_UPDATE_LINE_TOO_FAST and an unknown port type with -1. */
#define ULTRA_UPDATE_ARTIFACT_MAX 255u /* fresh nonzero session per subtransfer */
#define ULTRA_UPDATE_NO_ARTIFACT 65535u
/* Manifest error: two different artifacts for one target tuple/variant with
 * overlapping revision ranges; a receiver could accept either on the wire. */
#define ULTRA_UPDATE_OVERLAPPING_ARTIFACTS (-65)
/* "Update all receivers on this port" refused: a trusted return-path node is
 * silent (reachable=0). A known board that doesn't report back means something
 * is wrong on the port (power, cable, a board left in its recovery bootloader),
 * so nothing is sent. Every result is written: each silent node
 * UNREACHABLE/ULTRA_JOB_NOT_ANSWERING, every other node SKIPPED/ULTRA_JOB_PORT_BLOCKED.
 * ultra_update_job_recover() is the explicit action for one silent board. */
#define ULTRA_UPDATE_SILENT_BOARDS (-68)
/* ultra_update_job_recover() refused, nothing sent, results untouched: the node
 * answers (reachable=1) or has no return path, has no artifact, or its known
 * version is equal or newer. */
#define ULTRA_UPDATE_NOT_RECOVERABLE (-69)
typedef struct {
    ultra_update_participant_t participant;
    uint32_t firmware, blackout_ms, min_gap_us;
    int64_t evidence_us;
    uint8_t trusted, reachable;
} ultra_update_node_t;
typedef struct {
    upd_descriptor_t descriptor;
    ultra_update_read_t read;
    void *context;
    uint8_t authenticated; /* caller authenticated release manifest/descriptor */
    /* Optional streaming lifecycle: prepare returns 0 ready, >0 pending,
     * <0 failed. Verify downloaded signature/hash here, before read/entry.
     * release is called once after preparation is attempted, including abort. */
    int (*prepare)(void *context);
    void (*release)(void *context);
} ultra_update_artifact_t;
enum { ULTRA_JOB_PENDING, ULTRA_JOB_VERIFIED, ULTRA_JOB_CURRENT,
       ULTRA_JOB_SKIPPED, ULTRA_JOB_FAILED, ULTRA_JOB_SENT_UNVERIFIED,
       ULTRA_JOB_UNREACHABLE };
enum { ULTRA_JOB_REASON_NONE, ULTRA_JOB_INCOMPATIBLE, ULTRA_JOB_NEWER,
       ULTRA_JOB_MISSING_ARTIFACT, ULTRA_JOB_NO_RETURN,
       ULTRA_JOB_NO_REPLY, ULTRA_JOB_MISMATCH, ULTRA_JOB_ABORT, ULTRA_JOB_TRANSPORT,
       ULTRA_JOB_REFUSED, ULTRA_JOB_RECOVERY_SENT, ULTRA_JOB_NOT_ANSWERING,
       ULTRA_JOB_PORT_BLOCKED, ULTRA_JOB_OTHER_TARGET };
/* recovery: the target of ultra_update_job_recover(). */
typedef struct {
    uint32_t before, expected, observed;
    int64_t evidence_us;
    uint16_t artifact;
    uint8_t result, reason, transfer_complete, planned, recovery;
} ultra_update_result_t;
typedef struct {
    uint8_t mode, passes, rate, first_session;
    /* BLIND passes <ULTRA_UPDATE_BLIND_MIN_PASSES -> ULTRA_UPDATE_TOO_FEW_PASSES. */
    uint8_t port_type; /* ULTRA_PORT_DIFF_POWER or ULTRA_PORT_D2A_PIN, required */
    uint8_t family_order[2]; /* {0,0}: V407 then V203; otherwise {4,1}/{1,4} */
    uint32_t verify_timeout_ms;
    int64_t deadline_us; /* absolute abort request; busy reservations still win */
} ultra_update_job_options_t;
enum { ULTRA_JOB_READY, ULTRA_JOB_TRANSFER, ULTRA_JOB_VERIFY,
       ULTRA_JOB_ABORTING, ULTRA_JOB_DONE };
typedef struct {
    ultra_update_t transfer;
    ultra_sender_t *line;
    const ultra_update_node_t *nodes;
    const ultra_update_artifact_t *artifacts;
    ultra_update_result_t *results;
    ultra_update_job_options_t options;
    uint16_t node_count, artifact_count, active_artifact, selected_node;
    uint16_t cursor, sessions;
    uint8_t family, state, abort_requested, error, artifact_prepared;
    /* A subtransfer the sender refuses at begin (active_artifact/selected_node)
     * ends the job: begin_code keeps its return code (<0), error its UPD_ERROR_*
     * for -2..-10 (else 0; -1 is a line-state refusal), its nodes
     * FAILED/ULTRA_JOB_REFUSED and every node not yet tried FAILED/ULTRA_JOB_ABORT.
     * 0 while no begin was refused. */
    int begin_code;
    uint32_t gap_us, entry_ms;
    int64_t started_us, due_us, verify_started_us;
} ultra_update_job_t;
/* "Update all receivers on this port". All artifact descriptors are
 * authenticated before begin; overlapping ones refuse the job
 * (ULTRA_UPDATE_OVERLAPPING_ARTIFACTS). Each active source is prepared and
 * body-CRC checked before that subtransfer's first entry. Complete trusted
 * topology includes nonmatching/one-way transit nodes; an untrusted node
 * refuses the job (-UPD_ERROR_BOUNDS) and a silent trusted return-path node
 * refuses it with ULTRA_UPDATE_SILENT_BOARDS. Caller holds power and has
 * drained return windows and other producers. Results are retained on
 * finish/abort. Does not modify inputs on failure; results are written only
 * on success and on ULTRA_UPDATE_SILENT_BOARDS. */
int ultra_update_job_begin(ultra_update_job_t *,ultra_sender_t *,
    const ultra_update_node_t *,uint16_t,const ultra_update_artifact_t *,uint16_t,
    ultra_update_result_t *,const ultra_update_job_options_t *,int64_t now_us);
/* Recover ONE silent known board, nodes[node]: trusted, with a return path,
 * reachable=0 and a known version older than its artifact's (a board revoked
 * before an ABORT sits dark in its recovery bootloader and answers no ordinary
 * poll). Same inventory, artifacts, options and checks as job_begin: the whole
 * port sets the board limit, the D2A speed table check, entry wait and pacing.
 * Only that node's artifact is planned, BLIND with the full group schedule and
 * max(passes, ULTRA_UPDATE_BLIND_MIN_PASSES) passes, also when options.mode is
 * SELECTED (no SELECT frame). The broadcast reaches every board of that exact
 * target: an older one applies it and is planned like any node; an equal or
 * newer one skips it (CURRENT / SKIPPED NEWER), since boards refuse an equal or
 * older version unless the ANNOUNCE carries FORCE, which the sender never sets.
 * Nodes of other artifacts end SKIPPED/ULTRA_JOB_OTHER_TARGET. The target ends
 * VERIFIED when fresh rediscovery (job_confirm) shows the expected firmware,
 * FAILED/MISMATCH with the observed version when it shows another, else
 * UNREACHABLE/ULTRA_JOB_RECOVERY_SENT. Refusals: ULTRA_UPDATE_NOT_RECOVERABLE
 * (above); an untrusted node anywhere -UPD_ERROR_BOUNDS; node >= count -1.
 * Another silent node on the port doesn't refuse it. */
int ultra_update_job_recover(ultra_update_job_t *,ultra_sender_t *,
    const ultra_update_node_t *,uint16_t count,uint16_t node,const ultra_update_artifact_t *,uint16_t,
    ultra_update_result_t *,const ultra_update_job_options_t *,int64_t now_us);
int ultra_update_job_next(ultra_update_job_t *,int64_t now_us,uint8_t frame[48]);
int ultra_update_job_next_frame(ultra_update_job_t *,int64_t now_us,uint8_t *,uint32_t capacity);
int ultra_update_job_answer(ultra_update_job_t *,const uint8_t answer[32],int64_t now_us);
/* Caller supplies a matched fresh discovery/page exchange, never cached pages.
 * expected_answer_index is the FW_UPDATE answer's slot (0..15) in its return
 * window, independent of node_index. Pass original CRC-sealed evidence unchanged.
 * Evidence time must be newer than verify_started_us and the inventory.
 * Malformed/partial/stale evidence is ignored; fresh disagreement is FAILED. */
int ultra_update_job_confirm(ultra_update_job_t *,uint16_t node_index,uint8_t expected_answer_index,
    const ultra_identity_t *,const uint8_t fw_update[32],int64_t evidence_us);
/* End reachable-node discovery early; unresolved reachable nodes -> UNREACHABLE.
 * Otherwise job_next advances automatically on completion or timeout (default
 * 10 s when verify_timeout_ms=0). One-way nodes finish SENT_UNVERIFIED. */
int ultra_update_job_verification_done(ultra_update_job_t *,int64_t now_us);
/* passes: the active (or next prepared) subtransfer's count, 3 for a blind recovery inside a selected job; the
 * options' count while none is active. */
typedef struct {
    uint8_t family,phase,job_state,pass,passes,transfer_complete;
    uint16_t artifact,subtransfer,subtransfers;
    uint32_t bytes_sent,bytes_total;
    uint64_t elapsed_ms,estimated_remaining_ms;
} ultra_update_job_progress_t;
int ultra_update_job_progress(const ultra_update_job_t *,int64_t now_us,ultra_update_job_progress_t *);
void ultra_update_job_abort(ultra_update_job_t *);
#ifdef __cplusplus
}
#endif
#endif
