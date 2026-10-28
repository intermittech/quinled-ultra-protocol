# Expected receiver flash and RAM effects

M0 contains contracts, host codecs, validators and fixtures only. No application
or bootloader was linked, so code deltas below are **estimates**, not measurements.
Blob sizes below are synthetic format sizes, not production board allocations. Compiler/LTO/layout variation can be
hundreds of bytes; the ranges are not a reliable summed image-size prediction.
M1 measures the actual implementations against the supplied adopted baseline.

| Change | Expected flash effect | Expected RAM effect |
|---|---|---|
| Wire VERSION/TYPE flags/full TARGET and renamed IDs | +8..52 B dispatch/check code | 0 B; indices remain u8 |
| LIGHT_DEF base channel, physical CONFIG 2 units | +32..40 B on PWM builds | +5..8 B for five light bases, unless existing padding absorbs them |
| Board/channel/output CONFIG; firmware-owned domain ceilings | +16..64 B indexing/checks, target-specific | Reuse existing settings; +2..4 B per extra domain user limit |
| CONFIG source 77 and independent output scope | +8..32 B on single-output digital; future routing code target-specific | +2 B per output setting; no extra V203 pixel buffer |
| Canonical CONFIG CRC including TARGET | +0..24 B; incremental CRC shares the existing table | 4 B transient scratch, 0 B persistent; no 256-ID array |
| Five-entry bank selection | +0..32 B; five-channel constant folds are expected | 0 B persistent; existing POLL/reply storage reused |
| Const descriptor and IDENTITY | Exact blob +24..40 B slice-copy/bounds code, +16..32 B identity handling | 0 B persistent; zero-padded slice goes directly into existing 24-byte answer data |
| u16 descriptor length / 11-byte header | +3 B const per blob versus an 8-byte header; +0..8 B bounds arithmetic | 0 B; one part at a time, no reassembly |
| SENSE/TIMING/RESOURCES TLVs | Included in blob sizes below, no receiver interpreter | 0 B |
| PIXEL counter consolidation / official page numbers | Counter merge may recover 40..100 B; applied-config page may add 40..100 B | 0 B if existing counters/reply buffer reused |
| LIMITS 0x12 optional applied snapshot | 0 B in V203 (omitted); +60..140 B estimated if enabled; firmware-local field access, no TLV walk | 0 B persistent; existing reply buffer |
| Calibration cal_ms / bank/output selector | +0..24 B, firmware-local constants | 0..4 B scheduling state; no new general job queue |
| User LED-domain/model constraints and pre-frame admission | +30..150 B on affected converter boards, possibly more for a new model | +0..8 B working state on current boards; required local calibrated coefficients retained |
| Return pitch / fixed host-time units | 0 B expected for replacing constants/formula | 0 B |
| Decline flags in DIAG part 2 | +12..40 B reporting/latching | +1 B persistent unless an existing flag byte is reused |
| MCU timing constants / qualified rate lock | +0..64 B on V203; existing rate measurement reused | 0 B expected; future MCU backend has its own budget |
| App-side image v2 acceptance | +16..64 B versus board/revision-only checks | 0 B additional persistent storage |
| V203 bootloader tuple/range/geometry/cursor/deadline checks | +60..220 B estimated, including +16 B boot info | No new reply buffer; removes roughly 96 B erase-operation-list stack; existing multipass pool retained |
| Application relay of UPD_STATUS for downstream targets | +20..64 B using the normal relay engine; no V203 bootloader return path | 0..4 B plan state |
| FW_UPDATE installed-image confirmation page | +40..90 B copying metadata/status into the existing reply | 0 B persistent |
| V203 declared pacing and UPD nibble/index | +0..16 B constants/validation | 0 B |
| V407-only SELECT/STATUS/credit | Roughly +200..600 B control code, unmeasured; 0 B in V203 builds | One 256 B staging block plus about 16..32 B cursor/mask state; not an erase-sized buffer |
| Record format/count header and per-layout page geometry | +0..60 B before repacking; struct-compatible repack may recover 50..250 B, target-dependent | +0..24 B depending on padding/record scratch; no heap |
| Host format parser, vectors, setup/power JSON | 0 B in receiver | 0 B |

| Synthetic case | Descriptor bytes | Parts |
|---|---:|---:|
| nano2 | 78 | 4 |
| partial7 | 95 | 4 |
| local10 | 144 | 6 |
| analog20 | 123 | 6 |
| dual_pixel | 178 | 8 |
| single_pixel | 120 | 5 |

LINK is at least 16 value bytes; L1 synchronization uses a 21-byte value
including latency/jitter bounds. NAME adds 2+UTF-8 byte count. An
actual board compiles only its own records; a one-way build may export its
descriptor in a jig sidecar instead. No product parser, registry table or heap
is added to receivers. Qualified TIMING adds 14 B per independent profile.

V203 builds keep their actual five lights/channels or single pixel output. The
wire does not allocate 240 entries. Twenty full light states would add 15×592 =
8880 B relative to five with the supplied engine struct; that is a future V407
choice, not a V203 cost. Two pixel frame buffers similarly belong only to the
dual-output firmware definition and share its declared pool.

Soft flash reserves may be spent while preserving
hard memory bounds and the 2 KiB stack. [Diff-Power Ultra Receiver Analog-Medium](https://github.com/intermittech/quinled-ultra-firmware/wiki/Board-Diff-Power-UR-Analog-M)'s SRAM floor is **1024 B**,
512 B below the default (decided 2026-10-19). The source audit puts
existing analog builds around 1.1 KiB free after reservations: 1024 B preserves
a real reserve while allowing that baseline. On 20480 B SRAM, subtract 2048 B
stack, 112 B reserved holes and 1024 B floor: <=17296 B for static/other storage.
M1 must account all DMA buffers, .data/.bss, noinit, heap (if any) and alignment,
and measure worst-load stack high-water within 2048 B; lower allocations if
the fit fails. The floor is not permission to use stack or reserved holes.
Other boards keep 1536 B. No linked image fit is asserted by these spec checks.
Assembly matching uses an existing target byte (0 B extra tuple/RAM); descriptor
variant adds 1 B const flash, with approximately 4..16 B match code. D2A's extra
15 B output-domain record can stay in its firmware-exported sidecar. Rating/window/floor constants
add no receiver RAM. Separate variant artifacts need no runtime variant parser.
Analog sequence state uses ULTRA_LIGHT_TYPES=11, avoiding the extra 24 B that
ULTRA_TYPES=13 would add to six rows of u16 sequence state. SENSE range adds
2 B per sensor in const flash and no RAM; known-prefix host parsing costs no
receiver bytes. Saturating current units/voltage rescaling replace constants;
the bootloader needs no record, wire or descriptor format comparisons. Boot info entry bound reuses one build-ID byte.
Power JSON/S2 schema/paging remains host-only (0 B receiver). The first gate remains actual V203 analog,
digital and 8 KiB bootloader size/stack measurements; this draft does not assert
that any unbuilt image fits.

Adaptive analog forwarding retains the forwarding/edge counter and adds no TX frame
buffer. Four thresholds, period acquisition and hysteresis integration require
fresh code/CPU measurements; do not retain round 3's 0..64 B decoder estimate.
Budget <=128 B per ingress for statistics, frozen two-part snapshot and period
acquisition state (including u16 run lengths). Profile selection/hysteresis lives
on the hub. At 144 MHz the four rates allow 180/240/360/720 cycles per bit;
continuous 36 MHz sampling remains 4.5 MB/s. No CPU benchmark is claimed.
Digital packet assembly plus encoder reuse estimates +200..600 B flash and
16..32 B state; reuse the single frame buffer and pause local rendering while
assembling. Full-frame buffering adds input acquisition/output latency, not
chain-command latency. No regeneration buffer is added to analog receivers.
The generic L1 adapter is optional and absent from Diff-Power-only firmware;
its queue/cache costs belong to the local board firmware. Lower-limit helpers
constant-fold for fixed ratings. Board-local generation changes ownership, not
the receiver's const-data/runtime cost. M1 linker and stack measurements remain
required, particularly for the V203 8 KiB bootloader and analog SRAM reserve.
