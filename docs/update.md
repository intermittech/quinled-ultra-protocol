# Image/update v2 — wire 3, bootloader ABI 3

The authoritative offsets and reference codecs are in `include/ultra_update.h`.
This contract is shared by packager, manifest, jig, application and bootloader.
Only the bootloader's protected info identifies the target. An application
descriptor is not authority to change MCU, package, flash layout or ABI.

## Immutable identity and image descriptor

The 16-byte tuple is `board:u8, mcu:u8, package:u8, layout:u8, ABI:u8,
features:u8, variant:u8, revision:u8, slot_base:u32, slot_size:u32`, all integers big-endian.
IDs: MCU 1 V203, 4 V407 (2/3 reserved); package 1 F8U6, 5 RET6; layout 1/4 below.
Programming requires a firmware-owned target manifest matching the physical fixture;
unknown combinations are not guessed from a central product table. Board IDs 1..254
are independent of board type. PCB revision is `(major << 4) | revision`, with
both components 0..15: v0r1=01, v0r4=04, and the first true release v1r1=11
(hex). Major zero is reserved for prototypes, which therefore sort below v1r1;
10 (v1r0) is reserved and not a release. FF is v15r15, not a wildcard. This
encoding preserves human major/revision order in an unsigned byte. Current
D2A r0.4 is normalized to v0r4=04; boards 2/3 are provisioned v0r1=01.
The first release code is defined, not qualified as hardware yet.

Variant is an opaque exact assembly identity (all u8 values valid), allocated
within each product's firmware definition. It has no protocol-wide chain or
current-rating bits. Fitment, ratings and link abilities come from its descriptor;
unfitted links never advertise forwarding/relay. Product names are not enums.
Variant is copied into board-descriptor header byte 10 and setup S2's opaque
target. Hub, jig and packager use the complete tuple, including variant.
Tuple byte 5 holds the four defined feature bits in u8 and byte 6 holds
variant (target 16, boot info 32, image 64 bytes).
Feature bits are STATUS=1, ERASE_4K=2, ERASE_32K=4, RESUME=8. These are abilities,
not permission to erase outside a layout. The baseline uses only the minimum
erase unit even if larger units are advertised.

Bootloader info at physical `0x08000010` is 32 bytes:
`QDB2[4], boot_version:u8, target[16], geometry[3], entry_10ms:u8, build_id[7]`.
IDENTITY part 2 copies target+geometry, record format 2, descriptor format 1, entry_10ms at byte 21 and two zeros; part 0 supplies
boot_version. The jig verifies silicon identity, fixture/PCB identity, option
bytes and write protection against the firmware-owned board/assembly definition. Firmware never guesses a board
from MCU identity. No field bootloader update is defined.
The entry bound is 5..255 units of 10 ms, rounded up from firmware entry_max_ms.
Hosts use this immutable advertised bound for both transition and reboot waits.
The opaque build ID is seven bytes, keeping protected boot info at 32 bytes.

| Image offset | Bytes | Meaning |
|---|---:|---|
| 0 | 4 | ASCII QDU6 |
| 4, 5 | 1 each | Descriptor version 2, flags zero |
| 6, 7 | 1 each | Inclusive revision min/max |
| 8 | 7 | Tuple bytes 0..6; byte 13 required features:u8, byte 14 exact variant:u8 |
| 15 | 1 | Minimum bootloader version within the exact ABI |
| 16 | 4 | Firmware version: major/minor/patch/pre-release bytes |
| 20, 24 | 4 each | Body length, body CRC32 |
| 28, 32 | 4 each | Physical load address, physical entry address |
| 36, 37, 38 | 1 each | App-owned record/wire/board-descriptor versions (currently 2/3/1) |
| 39 | 1 | Zero |
| 40 | 16 | Opaque build ID |
| 56 | 4 | Zero |
| 60 | 4 | Descriptor CRC32 over bytes 0..59 |

The CRC is CRC-32/MPEG-2 (poly 0x04C11DB7, init FFFFFFFF, no reflection or final
XOR) fed **little-endian 32-bit words**, MSB first within each word, matching the
reference `upd_crc32_words`. This is not a byte-stream CRC32 or ZIP CRC32. The
stored CRC field is big-endian. Body length is a nonzero multiple of four.
Revision min≤max; disjoint revision ranges use separate descriptors for the same
body. A range never spans assembly variants. The board firmware release pipeline validates every supported physical
revision in that interval against its manifest; descriptors must be byte-identical.
No protocol-side registry or product generator is involved. Entry is a halfword-aligned **physical** address inside the body; target
code can translate to a reset alias when jumping, never when checking bounds.

Before any revoke, erase or program, the bootloader must possess both matching
ANNOUNCE halves and verify:

1. Update descriptor magic, version, CRC and reserved zeros. Bytes 36..38 are
   informational for the hub/application, never bootloader acceptance gates.
2. Exact board/MCU/package/layout/ABI/assembly variant; required features are a subset of its
   provisioned features, with no unknown required bits; revision is within range;
   boot_version meets min_bl. A status-profile selection must also match UID.
3. Actual option-byte split and protected geometry agree with the immutable
   provisioned tuple. Load equals slot_base. Slot_size is nonzero and its end
   cannot wrap. Body length fits both slot_size and 2 MiB transport reach.
4. Entry≥load, `entry−load < length`, aligned to two; length is four-aligned.
   Validate subtraction before addition. Every chunk uses
   `offset≤length && valid_bytes≤length−offset`, with a widened `INDEX*32`.
5. Rounded final program block and every erase unit fit permitted metadata/body
   regions; no overlap with bootloader, records or protected option bytes.
   Supply is safe and outputs have completed a black frame and latch.

Any rejection leaves the installed descriptor and body untouched. FORCE only
permits same/older firmware version; it never bypasses identity, bounds, CRC,
voltage or protection checks. Applications perform the same pure descriptor
check before rebooting into update mode, but bootloaders independently recheck.

## Geometry and records

| Layout | Protected bootloader | Metadata allocation | Body [start,end) | Records [start,end) |
|---|---|---|---|---|
| 1 V203 F8U6 | 08000000..08002000, 8192 B | 08002000..08002100, 256 B | 08002100..08010000, **57088 B** | 08010000..0801C000 |
| 4 V407 RET6 example | 08000000..08004000, 16384 B | 08004000..08005000, 4096 B | 08005000..08080000, **503808 B** | 08080000..0808C000 |

All addresses are hexadecimal physical flash addresses. The V407 example requires
the 512 KiB fast-flash / 200 KiB SRAM split (`RAM_CODE_MOD=0`); hardware and upper
record-region access remain qualification gates. Six record families each have
two separate 4 KiB erase sectors, allocated consecutively from records_base:
family k uses A=`base+8192*k`, B=`A+4096`, k=0..5. Family order is settings,
calibration, scenes, lifetime, calibration history, pixel settings/effects.
Never erase a 32 KiB region spanning record pairs. Store `format:u8,count:u8`
inside each family's CRC-covered header; count is channels for analog families,
outputs for pixel families, 1 for board lifetime. Mismatched format/count loads
defaults if no supported migration is available. Record migration belongs to the
application; the bootloader neither interprets records nor restricts their format. Struct-compatible fixed little-endian payload layouts must be generated
per firmware target with explicit offsets and size assertions; they are internal
storage ABI, not wire structs. Writes retain CRC, generation, revoke and commit-last
protection. Future applications may migrate older records without a bootloader change.

Metadata always uses its first 256 logical bytes: 64-byte descriptor and 96
halfword status records (192 B). On V407 the rest of its erase sector is reserved.
Erased halfwords may read FFFF or E339. Revocation follows all descriptor,
identity, geometry, supply and output-safety checks. Layout 1 writes zero over
the first descriptor halfword. Layout 4 instead erases the complete metadata
sector first; programming zero over its installed descriptor is forbidden.
Read back the entire metadata sector and accept only backend-erased halfwords
before any body erase or programming. A timeout, supply or readback failure
cancels all remaining operations; selected mode reports FAILED. An interruption
during this first erase leaves the body untouched. On restart, only a valid
descriptor and matching body CRC may boot; otherwise require fresh ANNOUNCE
and a complete erase (plus fresh SELECT in selected mode).

An erase cursor covers
`[metadata_base, round_up(load+length, erase_unit))` one unit at a time; no bounded
operation-list truncation. The layout-4 revocation is its first operation,
counted once, including the reserved remainder of that sector. In selected mode
announce its full deadline in BUSY before starting; blind mode uses the full
declared erase lead below. Verify every complete unit erased before programming.
After body readback/full CRC succeeds, program descriptor bytes 2..63, verify,
then commit bytes 0..1 (`QD`) last. A torn/absent magic or wrong descriptor/body
CRC is not bootable. Bootloader/app append VERIFIED/CONFIRMED/trial records using
the header's values; a full status area never wraps over the descriptor.

The tuple contains body geometry, while the firmware-owned layout supplies trusted
metadata/record geometry. Image bytes never supply erase addresses. Program
staging remains 256 B / eight 32 B chunks on both families; the erase unit is
256 B on layout 1 and 4096 B on layout 4. There is no 4096 B staging buffer.
Final transport padding is zero and excluded from the body CRC; unused bytes
of a final flash program block use the backend's erased fill and are outside the
body. Rounded programming must still stay inside the slot.

Geometry is three bytes: `program_log2<<4 | erase_log2`, `program_max_ms`,
`erase_max_ms`, nonzero timing bytes. V203 = `88 0A 14`; V407 example = `8C 1E 64`.
The V203 erase bound is 20 ms. Per-profile airtime is re-derived at the end of this document. Keep the 9060 ms full-slot erase lead-in and existing pacing;
a future bench measurement may tighten the bound, without changing the format.
The bounds are engineering declarations pending qualification, not silicon
measurements. Use a monotonic physical-time deadline of **2×** each bound, including
watchdog/error cleanup. The host also waits 2×, so an operation that reaches its
failure deadline cannot collide with the next frame. The same bound applies to both sides.

## Frames and sessions

A 48-byte frame is `BLOCK[40] + trailer[8]`. Block:
`TYPE:u8, SESSION:u8, INDEX:u16, PASS:u8, FLAGS:u8, DATA[32], CRC16:u16`.
Block CRC is CCITT-FALSE over bytes 0..37; trailer magic is 5AD2, version nibble UPD_HDR_VERSION=3 (frozen with ABI 3, independent of ULTRA_VERSION),
UPD flag, TIME u32 and CRC8/SMBus. Only UPD_STATUS sets RW as well; no normal
command slots are parsed from any UPD frame. Invalid CRC/type/header combinations
cause no state change. Control blocks use INDEX=PASS=FLAGS=0 except ANNOUNCE's
index/force. Session 1..255 is chosen anew for a transfer; 0 is reserved for idle
PING. All blocks in a transfer carry its session; stale session blocks are ignored.
No normal RW windows or pixel streams run during a session.

ANNOUNCE INDEX 0/1 carries descriptor halves. Duplicate identical halves/chunks
are harmless; conflicting repeated bytes fail the session. A different
descriptor requires a new session. DATA INDEX is the body chunk number, up to
65535, covering offsets 0..2097120 (last byte 2097151). END contains body CRC and
length, both equal to the descriptor; remaining DATA bytes zero. Only full body
CRC/readback permits commit. ABORT cancels RAM state; after revocation it cannot
restore the old app. PING may cause a running app to confirm successful boot;
it cannot commit an incomplete image. Lost END may be repeated idempotently: a blind host sends END three times
(`ULTRA_UPDATE_END_COPIES`), each followed by the port processing gap and the 10 ms decision-frame pace, and a receiver
that already took END ignores a repeat.

### Transfer mode

Every application and bootloader supports the **blind group / declared profile**
on CHAIN lines and marker ports. The host chooses blind or selected mode for
each new transfer, independently of bootloader size and the STATUS feature.
STATUS advertises optional SELECT + UPD_STATUS feedback when a qualified return
path exists; it never requires the host to use it. A transfer cannot change mode
while active. Selection belongs only to its session: that session's END or ABORT,
the inactivity timeout, reset, or a valid ANNOUNCE or SELECT starting a different
session ends it, also after revocation or a failure. Within its session a node
that is not selected neither mutates flash nor answers; a later blind transfer
with a new session is accepted by every matching node, whatever it saw before.
Nonmatching nodes forward the transfer without flash changes or answers; only
exact immutable-target matches may accept the image.

### Port job

One "update all receivers on this port" action is a port job. One broadcast per
MCU family means a family job containing **sequential image subtransfers**, one
per exact board/MCU/package/layout/ABI/variant target. Each subtransfer broadcasts
one descriptor/body once to the whole port, with its declared DATA passes inside
that transfer. It is not a multi-image container. Revision ranges, required
features, minimum boot version and slot geometry still pass every acceptance
check above. Deduplicate identical artifacts; different revision ranges or
images require separate subtransfers and sessions.

The host MUST perform these steps:

1. The port type sets the board limit: at most eight boards on a Diff-Power
   port, the [eight-board forward timing budget](timing.md#four-exact-profiles-and-the-1-100-m--eight-board-budget),
   and at most 50 on a Dig2Analog-Plus-5ch data pin, the
   [D2A link model](timing.md#dig2analog-plus-5ch-pin-link-model). Each board forwards the signal
   without regenerating it, so pulse distortion adds up per hop. Refuse a port
   above its limit, with that reason; never apply one limit to every port.
   Hold port power and acquire exclusive transport ownership. Finish outstanding
   return windows; stop pixels, effects/configuration/calibration transactions
   and rate changes. Snapshot configured topology, fresh IDENTITY parts 0/1/2,
   descriptors/LINK and versions, with trusted firmware/jig facts for one-way
   nodes. Include nonmatching transit nodes. Keep unknown, one-way and
   unreachable nodes explicit; silence never establishes absence or identity.
   A one-way node whose UID is unknown is entered by its position with an
   all-zero UID; it is never selected or confirmed and its result stays with
   that position. Any other repeated UID refuses the job.
2. Resolve authenticated release artifacts by the complete immutable target and
   all revision/feature/boot/geometry constraints. Verify release signatures,
   hashes, descriptor and body CRC before any entry. Record each node's before
   and expected version; a missing artifact gives an explicit skip reason. Two
   different artifacts for one target and variant with overlapping revision
   ranges are a manifest error: refuse the job, never pick one. Automatic full-port delivery requires a complete trusted target and
   timing inventory; never guess a target from a product name or nonresponse.
   A node without a trusted target refuses the job. A known (trusted) node
   with a return path that doesn't answer also refuses it: a board on the
   port that doesn't report back means something is wrong (power, cable, a
   board left in its recovery bootloader). The refusal names every silent
   node ("not answering: check power and cable") and sends nothing. Such a
   board is reached only by the separate
   [recover action](#recover-one-silent-board).
3. Queue V407 family subtransfers, then V203. Complete optional downstream
   selected diagnostics while their required application return relays still
   run. Either family order MUST work when every transit node meets the
   [forwarding contract](timing.md#update-forwarding-and-entry); ordering cannot
   compensate for an unqualified or blocking transit bootloader.
4. For each subtransfer choose a fresh nonzero session and fixed common line
   profile; a Dig2Analog-Plus-5ch pin uses its
   [pin table](timing.md#dig2analog-plus-5ch-pin-link-model) profile. Train CHAIN acquisition/transparency with valid nonzero-session UPD
   PINGs for >=1000 ms before entry ANNOUNCE, and longer if needed to acquire
   the entire path. These PINGs use the subtransfer's nonzero session.
   Send SELECT only for selected mode, then both entry ANNOUNCE halves. Every
   decision frame (SELECT, each ANNOUNCE half, each blind END copy) starts at least
   10 ms plus the port gap after the frame before it (`ULTRA_UPDATE_CONTROL_PACE_US`,
   [timing](timing.md#update-forwarding-and-entry)): a receiving application captures
   one frame at a time and drops a frame that starts before it has handled the previous one. Hold LOW
   for the whole-port entry wait, covering the largest immutable entry bound
   plus the largest entry/reset forwarding blackout plus the port processing
   gap, from the entry pair's physical end. Then send valid nonzero
   PING training for >=1000 ms before resending both boot ANNOUNCE halves;
   selected mode first reselects after reset. Timed replies additionally require
   measured rate lock. Never use session-zero confirmation PING to train an
   active transfer or repeat entry halves outside this procedure.
5. Blind mode sends no SELECT, requires no reply and uses the whole erase lead,
   increasing-index complete DATA passes and block pauses in the
   [declared profile](#blind-group--declared-profile). Selected mode uses the
   [status profile](#selected--optional-status-profile). Both honor whole-port
   processing gaps and every busy/failure deadline from physical frame end.
   Blind erase lead, block pauses and commit waits cover every node that
   accepts the image on the wire (target, revision, feature and boot checks),
   planned for it or not.
6. CHAIN uses unmarked update frames. A marker port has one receiver and wraps
   every update frame in the [quiet/marker/LOW envelope](timing.md#rate-lock-decline-and-digital-ports).
   Suppress pixels through entry, flash, commit and restart; marker polling
   never bypasses a busy wait. Hand output back only with a complete output
   frame and latch.
7. After END (a blind host's last copy), wait the largest verification/commit bound, then the whole-port
   reboot/blackout bound, plus the port processing gap, from END's physical end. Selected COMMITTED requires its acknowledgement
   before reboot. After the last UPD frame, hold quiet LOW for at least 2100 ms
   (the 2000 ms transparent idle close plus tick margin) and any outstanding busy reservation before
   ordinary traffic. Reacquire normal framing with DARK, train scheduling before
   replies, and send session-zero PING only for idle application confirmation:
   six copies (`ULTRA_UPDATE_CONFIRM_COPIES`), each with its full
   [confirmation-write pause](#application-confirmation-page): an application takes UPD
   transparency after two UPD headers in a row and captures the next frame with its old gate
   plan, so its first confirming capture is the fourth copy; an application that is
   already confirmed ignores a repeat.
   Observe the quiet-close rule again after the last copy, then refresh each
   reachable node's full UID/target/version and FW_UPDATE, even with retries
   disabled. A new subtransfer repeats acquisition and entry.
8. Retain a job report after completion or abort: inventory/plan, family/artifact,
   phase, byte progress, elapsed/estimated time, and per-node before/expected/
   observed version with a [result and reason](#job-results). Keep mixed versions
   visible. A retry is a new visible job with a new session and fresh preflight;
   it is never hidden inside "one broadcast".

### Recover one silent board

Recover is a separate, explicit action for ONE known board that doesn't
answer: trusted target and version, a return path, and that version older
than its artifact's. A board revoked before an ABORT (host restart or power
loss mid-transfer) waits dark in its recovery bootloader and answers no
ordinary poll, so only blind delivery reaches it. The host plans that board's
artifact alone and sends it in blind mode with at least the blind minimum
passes and the full group schedule (preamble, entry, training, erase lead,
passes, commit, quiet >= 2100 ms), whatever mode the port normally uses. The
port's board limit, its speed table and its timing still cover the whole
inventory, and an untrusted node still refuses it.

The broadcast reaches every board of that exact target. An older one applies
it and is reported like any updated node. An equal or newer one skips it:
applications and bootloaders refuse an equal or older firmware version unless
the ANNOUNCE carries FORCE (bootloaders before any revoke, the selected path
too), and the sender never sets FORCE (descriptor bytes 36..38 are
informational, not that version). For the same reason recover is refused for a
board whose known version is equal or newer: a live board would only skip it.
It is also refused for a board that answers (use the port update) or has no
return path. The recovered board ends verified when fresh rediscovery shows the
expected firmware, failed with the observed version when it shows another, and
otherwise unreachable with reason "recovery sent".

### Blind group / declared profile

No bootloader reply path is required. Matching immutable-target/revision receivers
accept ANNOUNCE as a group; nonmatching nodes forward without touching flash.
The host inventories each participant from IDENTITY or explicit provisioning,
and chooses the slowest declared bounds on the affected port, including transit
nodes. A STATUS-capable receiver accepts this mode without SELECT and never
waits for a status poll to start erase/program/commit. Follow the acquisition,
entry wait and >=1000 ms boot training in the [port job](#port-job); resend both
halves before DATA. No production target may omit its entry bound.

`erase_units = ceil((load+length−metadata_base)/erase_unit)`;
`lead_in_ms = max_i(erase_units_i * (2*erase_max_ms_i) + 100)` after the final boot ANNOUNCE;
`program_pause_ms = max_i(2*program_max_ms_i) + ceil(port_gap_us/1000)` after each eight DATA chunks, including
the final partial block. Here i ranges over matching participants and
`port_gap_us = max(500, all affected LINK.min_gap_us)` includes every transit node.
Count the actual image's erase units, not the whole slot: the layouts above use
one metadata unit plus `ceil(body_length/erase_unit)` body units. Use checked/wide
arithmetic, not u16 ms. At the maximum V203 body,
`(0x10000−0x2000)/256 =224` erase units gives **9060 ms lead-in** and
**21 ms block pauses** for a 500 us port gap. The layout-4 example has 124 erase
units at full slot, **24900 ms lead-in** and **61 ms block pauses** at that gap.
A 62 KiB (63488-byte) layout-4 body uses `1+ceil(63488/4096)=17` units:
3400 ms erase time plus 100 ms margin gives **3500 ms lead-in**, with the same
61 ms block pause. The added gap bounds dispatch from the final DATA edge before
the flash deadline starts; every receiver must dispatch within its LINK gap. Full erase precedes
DATA. Entry, blackout and training waits are additional. No frames, including
keepalive PINGs, may shorten a busy wait. Transit transparency must survive it.

Blind delivery uses bounded multipass collection: send complete body passes in
increasing INDEX order with declared block pauses. Every family requires at least
three complete DATA passes (decided 2026-10-23: reliability across the whole chain).
The sender names this minimum `ULTRA_UPDATE_BLIND_MIN_PASSES` and refuses fewer
with `ULTRA_UPDATE_TOO_FEW_PASSES`. Missing chunks may be supplied by later
passes; PASS high/low nibble is informational and never a correctness condition.
No host may infer completion from a fixed number of passes. Receivers verify
programmed repeats rather than treating them as new erases. After END allow
`max_i(100 + 2*program_max_ms_i)` ms for verification/dispatch and metadata commit,
then the whole-port entry/reboot plus blackout wait and the port processing gap
(from the last END copy's physical end) before acquisition and result
polling; a receiver that cannot meet this bound does not qualify. Fresh identity
and [FW_UPDATE confirmation](#application-confirmation-page) establish verified
success; no-return nodes finish **sent, unverified**. Reset
during transfer restarts from ANNOUNCE and a complete erase; no persistent resume
is implied. A ten-second inactivity timeout excludes advertised entry/erase/program
busy intervals, including the whole declared erase lead. It cannot expire during
the 9.06 s or 24.9 s examples. A blind receiver computes that lead for its own
image, `erase_units*(2*erase_max_ms)+100` ms from the final boot ANNOUNCE (every
node accepting the image shares its target and geometry), and starts the
inactivity timer only after it, even when its own erase finishes sooner.
[Transit idle-close](timing.md#update-forwarding-and-entry) is also held off
for the reserved busy interval.

### Selected / optional status profile

Before the application-entry ANNOUNCE, UPD_SELECT carries `UID[12] + exact immutable tuple[16] + zero[4]`
and binds that session in RAM. SELECT marks a selected session; only its matching
UID/tuple may accept ANNOUNCE, and unselected applications do not reboot.
Only that UID may mutate flash; other nodes keep qualified
update forwarding without answering. Use this mode only for a STATUS-capable
target with a qualified reply path, one selected target at a time. Support for
blind mode does not imply a local status answer or return relay. Selection ends
with its session ([transfer mode](#transfer-mode)).

After both validated entry ANNOUNCE halves, use the whole-port entry/blackout
wait from the [port job](#port-job). Reset clears selection: resend SELECT to the
bootloader. Send
valid UPD PING headers for at least 1000 ms and until host-relative rate lock can
be established, then resend both ANNOUNCE halves. PING with the selected nonzero
session is a keepalive/rate-training frame; session 0 PING is idle confirmation.
Training never revokes or erases. A duplicate SELECT with the same tuple/session
retains clock observations; a changed selection discards transfer state.

The update engine measures valid UPD trailer TIME independently of the normal
application command clock, which ignores UPD frames. That measurement follows
the same +/-200 ppm / 1000 ms rules. Normal command parsing remains disabled during
update. Keepalive PINGs are permitted only outside declared/acknowledged busy
intervals. This avoids requiring a bootloader reply before it can acquire rate
lock, or using an uncalibrated HSI UART for its first status answer.

A status reply path requires LINK RETURN_RELAY and UPDATE_FORWARD at every
intervening node, for the application or bootloader state actually used. Blind
forwarding does not establish return relay: a node advertising UPDATE_FORWARD
without RETURN_RELAY passes update frames on but no answers back, so the nodes
behind it are updated blind. Complete downstream selected
diagnostics while any necessary application relays are still running; do not
assume their bootloaders relay replies. A qualified RETURN_RELAY node must relay
nonselected status polls even without a local answer. Reset/failure of a
necessary forwarder stops the status transfer until the path is restored;
silence is not readiness.

UPD_STATUS request DATA is `UID[12], answer_index=0, R=1, rate:u8, zero[17]`;
INDEX/PASS/FLAGS are zero. Qualified nonselected intervening applications/status bootloaders
relay only. The reply uses the normal 32-byte envelope, PAGE 86, index 0,
SEQ=SESSION, and UID16=CRC16(full UID). Chain timing follows timing.md; marker
ports use their single-receiver envelope. Poll only supported rates and only after
rate lock. The DATA layout is:

`version=1, state, error, credit, session, flags, chunk_mask:u16,
accepted:u32, committed:u32, retry_after_ms:u32, image_body_crc:u32`.

All counts are bytes; credit is 0..8 **chunks** in the current 256-byte staging
block. Bits 0..7 of the mask indicate its chunks; high bits zero. Block base is
`floor(committed/256)*256`; accepted is the contiguous received prefix, not the
sum of out-of-order bytes. Committed counts the readback-verified flash prefix.
Only the final block can commit a non-256-multiple length. A final short chunk
still consumes one credit. Identical repeat chunks consume none. Host sends only
missing chunks within credit and never advances to a new block without readiness.

State progression: IDLE → PREPARED → BUSY(erase) → READY → RECEIVE → BUSY(program)
→ READY, then VERIFY → COMMITTED. Before **each** blocking operation, answer a
poll with BUSY, zero credit and its full deadline in retry_after_ms; begin only
after that poll's return window closes and forwarding has been restored (by W+80 us). That BUSY announcement applies once;
repeated polls before starting return the same reservation, not repeated erases.
The host sends nothing until `W + 80 + 1000*retry_after_ms` host us, then supplies
the full LINK processing LOW before polling readiness (common schedule:
`W + 1000*retry_after_ms + 580` host us). The ordinary W+200 us holdoff also
remains a lower bound. Failure gives FAILED with a specific error, zero credit. Only
COMMITTED plus app confirmation is success. A status bootloader holds COMMITTED
and continues answering status until a PING with the same selected session
acknowledges it. It then reboots within its declared entry bound. The host may
send at most three acknowledgement copies, leaving the full entry bound plus
the port processing gap after each copy's physical end; an application ignores nonzero-session PINGs. Only a session-zero idle
PING can authorize application confirmation. A poll can be lost: the host waits
the previously declared worst operation deadline before retrying, and a receiver
reports its current state on retry; a retry never repeats a completed erase. Full initial erase can use one cursor unit per
BUSY/readiness exchange; no 24-operation array exists.

Resume across reset is optional and must set RESUME. Without it, state is
RESTART_REQUIRED and offsets/mask reset. With it, a journal binds full target,
descriptor/body CRC and verified sector prefix; re-erase any incomplete sector
before resuming. Byte counters alone are not persistent-resume evidence. Program
staging is one 256-byte block; blind receivers may collect missing chunks across
passes. Neither profile provides A/B application rollback.

## Application confirmation page

After the application restarts, ordinary POLL page FW_UPDATE (0x05), with all
arguments zero, reports the **installed** image in the following 24-byte DATA:

| Offset | Bytes | Meaning |
|---|---:|---|
| 0 | 1 | Page format 1 |
| 1 | 1 | Flags: VERIFIED=1, CONFIRMED=2, TRIAL_SEEN=4; remaining bits zero |
| 2 | 1 | Count of valid trial records for this image (0..96) |
| 3 | 1 | Zero |
| 4 | 4 | Firmware version from installed descriptor |
| 8 | 4 | Installed body length |
| 12 | 4 | Installed body CRC32 |
| 16 | 4 | Installed descriptor CRC32 |
| 20 | 4 | Zero |

All multibyte values are big-endian. Flags derive from readback-verified metadata
status records: VERIFIED records completed body validation; CONFIRMED records
successful application startup self-checks and receipt of an idle UPD_PING;
TRIAL_SEEN means at least one trial observed a valid host frame. A PING alone
never proves successful startup or bypasses self-checks. After an idle PING the
host leaves LOW for `100 + 2*program_max_ms` ms plus `port_gap_us` after its
physical end, allowing bounded dispatch and
the confirmation write; no page reports CONFIRMED before that write verifies.
If the status area is full, an unconfirmed image remains unconfirmed; it is not
erased or silently promoted. The page is UNSUPPORTED for invalid/revoked/absent
metadata, unsupported arguments, or an application without update support.

The host requires both VERIFIED and CONFIRMED, the expected body length, body
CRC, descriptor CRC and firmware version, and a matching full IDENTITY tuple.
The answer's index must be the return-window slot its poll assigned to that
node, independent of the node's inventory position; an answer in another slot
is no evidence for it. Use the CRC-sealed answer as received.
Lost replies are retried; a timeout or missing page is never success. This page
does not retain a failed attempt's diagnostics across reset and does not require
an extra receiver RAM buffer. A node without a return path cannot establish a
verified result through this protocol.

## Job results

Results are per node, with evidence time and a concrete reason; an aggregate
must retain every node's result. Transmission progress is not installation
evidence. Use these terminal outcomes:

| Result | Required evidence / meaning |
|---|---|
| verified | Fresh postboot UID and full target match, expected version, and FW_UPDATE VERIFIED + CONFIRMED with matching length/body/descriptor CRCs |
| already current | Fresh inventory or explicitly identified provisioning evidence shows the requested version; no image sent for this node |
| skipped: incompatible / newer / missing artifact / port blocked / other target | Exact target or revision/feature/boot/geometry check fails, installed version is newer without authorized FORCE, no authenticated artifact exists, a known board on the port doesn't answer (port update refused), or a recover action for another board; retain the specific reason |
| sent, unverified | Declared transmission and waits completed, but no usable return path exists; expected version is not an observed installed version |
| unreachable | An expected reachable node cannot be inventoried or rediscovered, or its path is unavailable; silence is not success or proof of absence. A known board that doesn't answer when a port update is asked for: reason "not answering" (the port update doesn't run; the other nodes are "skipped: port blocked"). A recovered board that isn't rediscovered after its blind delivery: reason "recovery sent" |
| failed | Explicit receiver failure, host/transport failure or abort, a subtransfer the host refuses at begin (retain that refusal code), or fresh telemetry disagrees with the expected image/confirmation |

A node whose UID/tuple changed during the job remains an explicit mismatch or
unreachable inventory record; never attribute its predecessor's update to a
replacement. Preserve skipped, unverified and unreachable records alongside
verified nodes, including when retries are disabled. A retry is a separate,
visible port job and does not rewrite the previous job's evidence.

## Per-profile update airtime

All 384-bit update frames hold the chosen profile through the entire session.
Including the >=500 us gap, frames take 980 / 1140 / 1460 / 2420 us at
800/600/400/200. A full 57088-byte body has 1784 chunks and 223 program pages:
one pass takes >=1.74832 / 2.03376 / 2.60464 / 4.31728 s wire time.
Add 9.060 s erase lead-in and up to 223*21 ms =4.683 s program pauses,
then ANNOUNCE/END, metadata, transition/reboot and polling. Multiple passes
multiply wire time; these are not complete-operation upper bounds. Calculate
actual RMT tick durations, not a nominal rate label. Recovery uses line-period
detection at 200 on inventoried all-Ultra ports, never an 800-only announcement.
Only an app AND bootloader common mask allows a fallback. Mid-update profile
changes are forbidden: quiesce/abort with normal session rules, wait for every
busy deadline, then reacquire. Local links retain their independent framing.
