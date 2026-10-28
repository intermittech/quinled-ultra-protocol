# QuinLED Ultra sender 0.3.2-draft

The allocation-free C99 host reference speaks wire 3 only. ESPHome, WLED and
the jig share the committed fixtures in `../test-vectors/`. Boards describe
their own capabilities; this package has no product registry.

Compile `src/*.c`, or use the CMake, ESP-IDF, Arduino or PlatformIO package.
`ultra_sender.hpp` provides optional C++11 storage and convenience methods.
All protocol, transport and update headers in `src/` match `../include/`.
`PROTOCOL-PIN.json` and the package test enforce this.

## Frames and caller storage

```c
#include "ultra_sender.h"
static ultra_sender_t port;
static ultra_sender_board_t boards[3];
static ultra_sender_item_t items[ULTRA_SENDER_ITEMS(3, 1, 3)];
static uint8_t frame[ULTRA_SENDER_FRAME_BYTES(3)];

void setup(void) {
    ultra_sender_config_t cfg;
    ultra_sender_config_init(&cfg, 3, 1);
    /* Default: profile 800 at exact 9.6 MHz, >=500 us LOW.
       For RMT, initialize the output first and use its .effective timing. */
    ultra_sender_init(&port, &cfg, boards, items, 9);
    ultra_light_def_t def;
    ultra_light_def_init(&def);
    def.roles[0] = ULTRA_ROLE_BRIGHTNESS;
    for (uint16_t b=0; b<3; ++b) ultra_sender_light_def(&port,b,0,&def);
}

void loop(int64_t now_us) {
    uint32_t n=ultra_sender_next_frame(&port,now_us,frame);
    if (n) transmit_complete_frame(frame,n); /* backend owns bytes until done */
}
```

A line holds `ULTRA_SENDER_MAX_BOARDS` physical boards (default 8; a storage size, set at
build time). The supported board count is per port type: 8 on a Diff-Power port, 50
on a Dig2Analog-Plus-5ch data pin. The default light capacity is five;
set `cfg.light_capacity` for another explicitly inventoried count, or bind a
validated descriptor before queuing controls. A light has five roles and
`def.base_channel` maps them to physical channels base..base+4. Targets run
0..239. Missing final-bank entries never become channels.

The scheduler keeps three command copies, first-copy order, per-board refresh,
group starts, pixel effect phase, the DARK preamble and power-off notices. SET flags
are TYPE bits 5..6. `ultra_sender_due_us()` includes frame/reset and return/busy
deadlines. All times use one monotonic `int64_t` microsecond clock. Build only
when a frame can start; after an unsent/aborted frame, use `retime()` with the
same buffer. Release transport ownership first. Keep line, boards, items,
group and borrowed descriptor storage alive; never copy an initialized line.
One task owns a line/group. `deinit()` before destroying or reinitializing a
group member. Refused commands leave queues unchanged.

Each light state kind takes one 60-byte pool item; both SCENE selectors
are independent items. Channel
CONFIG takes one item per used target; pixel CONFIG takes up to two per output;
domain limits take up to two per board. Board CONFIG 1..24 stays compact.
No channel/segment array is preallocated for the maximum target space.

## Discovery, pages and limits

`ultra_sender_descriptor.h` validates complete TLV blobs, canonical instances,
references, partial PWM banks, converter protection, link kinds and UTF-8 NAME.
Known records parse their prefix and retain appended tails; unknown TLVs are
skipped. Unsupported formats remain identity/diagnostics only. Decode views
borrow the blob. Reassembly uses a caller-owned buffer with
`ultra_descriptor_part()`; the maximum is 6144 bytes. Synthetic descriptors in
the vectors are format tests, never production ratings or timing certificates.

Decode IDENTITY parts with `ultra_identity_decode()`: all six feature words,
descriptor length/CRC, exact 16-byte target, geometry and entry bound survive.
The caller matches response index, sequence, UID, page and outstanding part/bank;
answers do not echo those selectors. Serialize requests per receiver.

Use `ultra_sender_config()` for board IDs, and
`ultra_sender_config_target()` for channels/domains; pixel calls take an explicit
output. IDs 11/12 are channel model current/user ceiling, 21 is root-domain
ceiling, 33..46 other domains, 64..77 pixel settings. CONFIG 2 uses 10 ns units.
Bound descriptor ratings clamp requested ceilings; zero restores the rating.
FFFF is unknown, never unlimited or a settable current. Unknown ratings need
qualification outside this package before enabling a load. Unbound byte-building
APIs require the caller to provide equivalent capability/power admission.
`ultra_sender_config_crc()` covers sorted explicit TARGET/id/applied-value
records, including channel and pixel scopes sharing the CONFIG sequence space.

`ultra_descriptor_bank()` returns separate channel/light/output masks;
`ultra_outputs_decode()` checks partial-bank padding. Other page fields use
the public header offsets. Calibration timeouts come from descriptor `cal_ms`
with the 1.5x margin. Current fields use u16 mA, SENSE full scale retains its
decimal exponent, power uses u32 mW with wide multiplication, and lifetime
energy allowance uses rated voltage/current. ENERGY boot mWh deltas are modulo
2^32 and must be tracked separately per boot.

PIXEL_FX takes the packed TARGET `ULTRA_PIXEL_TARGET(output,segment)`;
segment 15 selects the whole output. CONFIG 77 chooses the source separately.
ACKS columns selecting PIXEL_FX always report NEVER with SEQ zero. Acknowledge
segments using PIXEL_FX state part 0, arg3=output, arg1=segment/4 and the
SEQ/RUNNING entry segment%4; NEVER in ACKS does not report effect support.
RESTART exists only in command copies; refresh/resync preserves phase.
The descriptor owns chip/count/FPS/resource/power ceilings. A constrained
converter feeding streams requires hardware current limiting. Strip timing is
regenerated locally by the receiver.

L1 kind-1 slots schedule relative to the receiver's captured completion of the
final packet CRC byte. `ultra_local_start_us()` validates both CRCs and resolves
signed millisecond START/phase offsets from the supplied acceptance timestamp;
no trailer TIME is needed. The adapter captures acceptance before dispatch and
deduplicates txid/command SEQ without moving the original start. LINK decoding
exposes `local_latency_us` and `local_jitter_us` (FFFF means unknown). Claim
synchronized output only against finite, qualified bounds and include per-chip
packet acceptance/offset rounding in the skew budget; see `docs/transport.md`.

## Exact per-port output

`ultra_timing_profile()` selects profile 800/600/400/200 (IDs 0..3). The
authoritative fields are clock Hz and integer HIGH/period ticks. The ns fields
are rounded display values. LOW is period minus HIGH; every bit uses the same
recipe. No bit-period dither is used. TIME is solved against the actual final
falling edge, including the trailer CRC bit. The integer timestamp error is
bounded to 2 us; the transport's residual physical HIGH/period error must stay
within 10 ns, including the sender's qualified output-path contribution.

| Backend | Period ticks | Zero HIGH | One HIGH |
|---|---|---|---|
| PARLIO 9.6 MHz | 12 / 16 / 24 / 48 | 3 / 4 / 6 / 12 | 9 / 12 / 18 / 36 |
| RMT 40 MHz | 50 / 67 / 100 / 200 | 12 / 17 / 25 / 50 | 38 / 50 / 75 / 150 |
| RMT 80 MHz | 100 / 133 / 200 / 400 | 25 / 33 / 50 / 100 | 75 / 100 / 150 / 300 |

At 600, actual RMT rates are 597.014925 and 601.503759 kbit/s. Never derive
TIME from a nominal rate or individually rounded ns values. Other RMT clocks
are refused. `ultra_encode_pulses()` returns exact ticks; divide by its clock.
The optional single-lane SPI encoder uses exactly 9.6 MHz and 12 samples/bit
for profile 800. Every backend keeps reset/processing LOW >=500 us.

`ultra_sender_profile.h` groups PARLIO ports by profile and composes independent
lane phases concurrently into continuous 32-bit sample words. Zero-initialize
lane state before `ultra_parlio_begin()`. Enqueue a fresh fast frame as soon as
its own reset completes while slow groups continue. Keep source buffers owned
until lane completion; keep DMA continuous across chunks. Idle lanes are LOW.
The composer is portable; peripheral clock setup, DMA ownership and per-lane DE
are the adapter's responsibility. `actual_hz` must be exactly 9600000; verify
the real source/divider. Serialized profile transfers reduce fast-group FPS.

RMT uses one encoder/channel per port. `ultra_esp32_rmt_profile()` checks driver
completion before changing words and scheduler together. The IDF driver can
round channel resolution: qualify the actual source/divider and residual
clock error ([driver source](https://github.com/espressif/esp-idf/blob/v5.3/components/esp_driver_rmt/src/rmt_tx.c)).
No hardware qualification is implied by mocked-driver tests. `send()` owns the
buffer until OK/ABORTED; STUCK retains ownership until `end()` succeeds.
For RW, use the separate RW encoder and a hardware/ISR-timed transceiver release;
the normal blocking RMT sender appends reset and is unsuitable for RW.

Profile changes wait for frame/reset, return and flash-busy deadlines; transport
ownership (`tx_active`) and the update lease prevent changes. Discard old encoded
buffers, then run the new DARK preamble and scheduling lock. Any non-Ultra device,
real pixel, unknown node or incomplete inventory locks a port to 800. Common
support includes all apps and immutable bootloaders. Qualification, positive
measured margins and intermediate forwarding reserve are separate admission checks.
For recovery/probes, `ultra_sender_profile_acquire()` permits only qualified,
inventoried acquisition traffic before a live eye measurement exists. It blocks
queued controls/updates while DARK, training NOPs and diagnostic polls run.
`ultra_sender_training_frame()` sends known 55/AA/00/FF NOP payloads without
consuming command queues. After fresh positive margins, `profile_accept()` opens
the control gate; a faster probe still needs its full 60 clean windows. Manual
overrides use the stricter `profile()` call. RMT has matching acquire/profile
helpers that first check channel completion.
Mixed paths retain the GUARANTEE timing bound. Typical 30+3+3 m
fits 800; eight worst-length segments use 200 under the qualified timing budget.

## Return windows and AUTO

`ultra_sender_rc.h` builds POLLs and extracts framed CRC-valid UART answers.
Pitch is `(335 << rate)+165`: 500/835/1505 us. Chain restart uses
`ultra_rw_next_us()`: W+580 us at a 500 us processing gap. Host release is
80 us after the final falling edge, inside the 60..150 us envelope. Descriptor
RETURN/RELAY and supported UART rates are checked when inventory is bound.
[Dig2Analog-Plus-5ch](https://github.com/intermittech/quinled-ultra-firmware/wiki/Board-Dig2Analog-Plus-5ch) always forwards commands and never answers or relays returns.

Marker ports use `ultra_dport.h`: quiet/post-command LOW >=500 us, 20 us HIGH
marker, 10 us marker gap, and separate receive/retake/handback scheduling.
`ultra_dp_output_us()` derives complete output duration from PIXEL timing.
Header training and measured clock lock still precede polling.

LINK_STATS decoding combines frozen width/context parts only within the same
boot/epoch and 2 s freshness bound. Missing, stale, overflowed or unscaled
reports are unavailable. Margin subtracts sample quantum and outward-rounded
scale uncertainty. Aggregate every required hop before calling the pure
`ultra_auto_step()` state machine once per one-second training window.
Warn below 20 ns/on errors; three consecutive bad windows request one slower
step. Nonpositive margin/repeated loss stops pixels and requests 200 recovery.
Missing reports cannot authorize fallback. Faster probes require idle state,
60 s dwell and 60 consecutive clean candidate-rate windows with >=60 ns margin.
Bad probes revert to the last good profile. Transactions/updates prevent probes;
manual overrides still require inventory, certificates, eye and gate admission.
The caller applies requested changes at drained boundaries and reacquires.

## Host updates

`ultra_sender_update.h` provides streaming preflight, declared multipass pacing,
UID-selected status/credit flow, BUSY deadlines and installed-app confirmation.
The callback reads immutable body bytes; no image buffer is allocated. Preflight
checks both descriptor CRC and full body CRC before emitting ANNOUNCE, exact
tuple/assembly, revision range, minimum bootloader and overflow-safe rounded
erase/program bounds from trusted firmware/jig layout facts.

Blind transfers require at least three complete ascending DATA passes for both
V203 and V407 (`ULTRA_UPDATE_BLIND_MIN_PASSES`); fewer passes are refused with
`ULTRA_UPDATE_TOO_FEW_PASSES`. They retain both
ANNOUNCE halves before/after entry, doubled erase/program deadlines and all
declared gaps. Full-slot erase lead-in is 9060 ms; block pause is 21 ms for the
declared 10/20 ms geometry. `begin_group()` validates every participant and uses
the slowest matching erase/program/commit bound. Port jobs use all transit
entry/blackout bounds and default to V407 then V203.

Call `ultra_update_begin_mode()` with `ULTRA_UPDATE_BLIND` or
`ULTRA_UPDATE_SELECTED`. The convenience `begin()` and `begin_group()` choose
blind mode, including for STATUS-capable targets. STATUS and bootloader size
never choose the transfer mode. Each selected session selects one UID,
reselects after entry, trains UPD headers for >=1 s, then uses only reported
missing-chunk credit. BUSY suppresses all traffic through W+retry+processing LOW;
silence grants no credit. Keep receive buffers specific to the current poll.
After COMMITTED, selected-session PING acknowledges it; three bounded copies
cover a lost acknowledgement before reboot/idle confirmation.
Profile changes are forbidden throughout the session, including failures until
the busy deadline and explicit abort. After reboot, idle PING and its confirmation
write deadline precede ordinary discovery/result polls.
Every transfer now starts with >=1000 ms nonzero-session PING acquisition,
entry ANNOUNCE pair, whole-port entry LOW, >=1000 ms boot training, then the
boot ANNOUNCE pair and actual-image erase lead. END/commit/reboot is followed
by >=2100 ms quiet (`ULTRA_UPDATE_QUIET_MS`: the 2000 ms idle close plus tick
margin), fresh DARK/NOP acquisition and session-zero confirmation. A blind END goes out
three times (`ULTRA_UPDATE_END_COPIES`, each followed by the port gap and 10 ms) and the idle
confirmation PING six times (`ULTRA_UPDATE_CONFIRM_COPIES`, each with its confirmation-write
pause); the waits and the quiet count from the last copy. Every decision frame (SELECT, each
ANNOUNCE half, each END copy) starts at least 10 ms plus the port gap after the frame before it
(`ULTRA_UPDATE_CONTROL_PACE_US`): a receiving application captures one frame at a time.
Another >=2100 ms quiet and the full confirmation-write bound precede results.
ABORT also retains the lease through its quiet-close deadline; after an ANNOUNCE
with no DATA or END since, that deadline is the ANNOUNCE's transit hold
(`upd_transit_hold_ms()`) + 2100 ms from its physical end. Keep driving
`next_frame()` until the lease releases, even after failure.
The profile remains frozen during app confirmation while normal headers and
result queries are permitted. A fresh preamble/header lock follows reboot.

Success requires installed FW_UPDATE VERIFIED+CONFIRMED, expected version,
length, both CRCs and the full rediscovered identity. Declared groups track each
participant. Port jobs finish one-way boards as sent-unverified and release
the lease after the required waits.
This library never treats a fixed pass count, silence or transport completion as
successful installation. Firmware revision/assembly qualification and safe supply,
route, blanking and resource admission remain with the integrator.

`ultra_sender_job.h` accepts a complete trusted port inventory, authenticated
artifacts, options and a caller-owned result array. It preflights descriptors,
deduplicates identical descriptors and queues exact-target subtransfers in
V407/V203 order (or the caller's explicit reverse order). The inventory includes
nonmatching transit nodes: entry/reboot uses the maximum entry bound plus the
maximum trusted blackout, and processing LOW uses the maximum LINK gap.
The caller authenticates signatures/hashes, holds port power, drains windows
and stops other transport producers before begin; buffers stay immutable until
completion. Artifact `prepare(context)` may return positive while a streamed
download is pending, zero when signature/hash verification and reading are
ready, or negative on failure. Only then does the library CRC-check that body
before entry; DATA rereads it. `release(context)` runs once on finish/abort,
including an attempted pending preparation. Only one image must be resident.
An absolute job deadline requests abort without shortening busy.

Every update entry point states its port type, which sets the board limit: 8 on a
Diff-Power port (`ULTRA_PORT_DIFF_POWER`, the
[eight-board forward timing budget](../docs/timing.md#four-exact-profiles-and-the-1-100-m--eight-board-budget)),
50 on a Dig2Analog-Plus-5ch data pin (`ULTRA_PORT_D2A_PIN`). Boards forward the
signal without regenerating it, so distortion adds up per hop. A larger
inventory or line is refused with `ULTRA_UPDATE_TOO_MANY_BOARDS` before any
frame; the limits are not build options. A 50-board line also needs normal
framing built with `ULTRA_SENDER_MAX_BOARDS` >= 50. The line profile for a
port's configured board count comes from
`ultra_port_line_profile(port_type, boards, &profile)`, which returns its reason
([port types](../docs/senders.md#port-types-and-the-starting-profile)): 800 kHz
on Diff-Power ports and for 1..20 D2A boards, 600 kHz for 21..34, 200 kHz for
35..50, 255 when refused. Every update entry point, `ultra_update_job_begin()`
included, refuses a D2A line running faster than that profile with
`ULTRA_UPDATE_LINE_TOO_FAST` before any frame; a slower profile is admitted. Artifact queues have at most 255
entries and subtransfers use fresh nonzero sessions (at most 255 per job).
Use `ultra_update_job_next_frame()` with capacity at least
`max(48, ultra_frame_bytes(node_count))`: acquisition emits normal DARK/NOP
frames as well as 48-byte update frames. `transfer.transport` describes the
last frame's envelope. For CHAIN it is unmarked; for a digital line emit its
lead LOW, 20 us marker HIGH, 10 us marker LOW, bytes and tail LOW exactly as
returned. `now_us` is the envelope start; TIME refers to the enclosed frame's
physical end. STATUS includes release/listen bounds. Do not independently
retime or wrap the returned bytes with the ordinary digital poll scheduler.
The caller observes the initial power-up marker guard, suppresses pixels for
the entire job, and completes a full output frame/latch handback on release.

In `ULTRA_JOB_VERIFY`, supply fresh IDENTITY parts 0/1/2 and FW_UPDATE through
`ultra_update_job_confirm(job,node_index,expected_answer_index,identity,answer,evidence_us)`.
`expected_answer_index` is the answer's slot (0..15) in its return window,
independent of the inventory node index; pass the original CRC-sealed answer
unchanged. Its evidence
time must be newer than `verify_started_us` and inventory evidence. The caller
correlates fresh discovery and page exchanges; cached answers are forbidden.
Fresh mismatches produce FAILED, malformed/stale evidence is ignored, and
silence never verifies. `transfer_complete` records delivery independently.
No-return nodes finish SENT_UNVERIFIED without changing their observed version.
`job_next_frame()` automatically advances when all active results are terminal
or verification times out (default 10 s); `job_verification_done()` ends
discovery earlier and labels unresolved reachable nodes UNREACHABLE. Each
result retains before/expected/observed version, artifact, reason and time.
A subtransfer the sender refuses at begin (for example its body CRC, or a pin
now faster than its speed table) ends the job: `job.begin_code` keeps that
begin return code and `job.error` its `UPD_ERROR_*` when it is one; its nodes
end FAILED with reason REFUSED, nodes not yet tried FAILED with reason ABORT.
`ultra_update_job_begin()` ("update all receivers on this port") refuses
with `ULTRA_UPDATE_SILENT_BOARDS` before any frame when a trusted node with a
return path is unreachable (`trusted=1`, `reachable=0`): a known board that
doesn't report back means something is wrong on the port. Every result is
written: each silent node UNREACHABLE with reason NOT_ANSWERING (show "not
answering: check power and cable"), every other node SKIPPED with reason
PORT_BLOCKED. An untrusted node still refuses with `-UPD_ERROR_BOUNDS`.

`ultra_update_job_recover(job,line,nodes,count,node,artifacts,artifact_count,results,options,now_us)`
is the explicit action for one such board (`result.recovery`): a revoked board
sits dark in its recovery bootloader and answers no ordinary poll. Same
inventory, artifacts, options and checks as `job_begin()`, so the port limit
and the speed table cover the whole port. Only that node's artifact goes out,
blind with the full group schedule and at least
`ULTRA_UPDATE_BLIND_MIN_PASSES`, also with selected options. Older boards of
the same target apply it and are planned; equal or newer ones skip it
(CURRENT / NEWER): applications and bootloaders refuse an equal or older
firmware version unless the ANNOUNCE carries FORCE, which the sender never
sets. Other artifacts' nodes end SKIPPED with reason OTHER_TARGET. The target
ends VERIFIED when fresh rediscovery shows the expected firmware,
FAILED/MISMATCH with the observed version when it shows another, else
UNREACHABLE with reason RECOVERY_SENT. A node that answers, has no return
path or artifact, or whose known version is equal or newer returns
`ULTRA_UPDATE_NOT_RECOVERABLE` (nothing sent, results untouched).
`ultra_update_job_progress()` exposes family/artifact, job state, transfer
phase, subtransfer count, current-pass bytes, one-based pass, elapsed time and
a conservative remaining-time estimate; it is an estimate, not a deadline.

## Validation

Run `python sender/test/run_tests.py` from the repository root. Build artifacts
default to the workspace's `../tmp/sender-build`; `ULTRA_TEST_BUILD` overrides it.
Every child build/test has a timeout. Python, C and C++ use committed shared
fixtures without regenerating expectations. The sender suites include ASan,
exact-clock TIME sweeps, grouped-lane boundaries, scheduler regressions, RMT
ownership and update failure paths. Regeneration is explicit:
`python test-vectors/gen_vectors.py`.

The default run includes the receiver simulators (test_sim, test_sim_rx,
dline_sim) on the firmware's own receive path and delay line: `--firmware PATH`
or `ULTRA_FIRMWARE` (else `../quinled-ultra-firmware`), at a commit whose
protocol pin is this one. Missing or mismatched firmware fails; another wire
version is never substituted. Physical
PVT/cable, DMA starvation, ISR timing and supply qualification require hardware.

ESP32 Xtensa GCC 14.2, `-Os`, all object code/literals/rodata before link GC:
33,815 bytes; static library globals: 8 bytes. Caller-owned state: line 168 B,
board 128 B, pool item 60 B, AUTO 40 B, LINK_STATS 64 B, update 240 B,
PARLIO lane 20 B, descriptor view 16 B plus its borrowed blob. Optional state
is allocated only when its feature is used. CMake builds the six-source ESP32
static library; peripheral integration still needs an ESP-IDF application.

MIT, Copyright (C) 2026 QuinLED; see `LICENSE` and `../TRADEMARK.md`.
