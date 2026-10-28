# Board descriptors — format 1

This is the wire-3 normative draft. Integers are unsigned big-endian unless marked
otherwise. Offsets in each table start at the TLV **value**, excluding its two-byte
type/length prefix. A receiver serves const bytes compiled from its own `src/board/<name>/`
firmware definition; only hosts parse TLVs. That definition owns name, ratings,
channels, sensing, links, assembly, MCU and pins. Use the same compile-time facts
for the descriptor and protection/allocation constants; static assertions and
firmware-local tests prevent drift. No central registry, bindings or generated
product headers are required. Firmware may generate its own blob at build time.
The hub reads it at runtime and may only lower limits. It must never select
capabilities by product name, ID range, Medium/Mini/Nano suffix or assembly bits.
Only [board IDs](../board-ids.md) are allocated globally; no future board needs
to be predicted. A one-way board exports these same bytes as a firmware/jig
sidecar for explicit inventory, not a protocol-maintained lookup table.

`POLL(page=0x0F, arg0=part, other args=0, TARGET=0xFF)` returns bytes
`24*part .. 24*part+23`, with zero padding on the last part. An out-of-range part
returns UNSUPPORTED, not an empty descriptor. Maximum length is 6144 bytes (256
parts, selected by the existing u8 part index). Hosts validate the complete blob before enabling its advertised controls.
There is no descriptor reassembly allocation on the receiver.

| Header offset | Bytes | Meaning |
|---|---:|---|
| 0 | 1 | Format = 1 |
| 1 | 1 | Header byte count = 11 |
| 2 | 2 | Total byte length, including header, 11..6144 |
| 4 | 2 | CRC-16/CCITT-FALSE over bytes 6 through length-1 |
| 6 | 1 | Board ID, 1..254 |
| 7 | 1 | Logical light capacity, 0..240; each light still has five components |
| 8 | 1 | Number of TLV records, at most 255 |
| 9 | 1 | Flags: bit 0 illustrative example, bit 1 timing qualified; others zero |
| 10 | 1 | Opaque assembly ID, equal to protected target byte 6 ([update.md](update.md)) |

Each TLV is `type:u8, length:u8, value[length]`. Length excludes the prefix.
Canonical order is ascending `(type, instance id)`. Known types require at least the prefix
length below. Hosts parse that prefix and skip the appended tail; a longer known
record never invalidates the blob. New fields only append, with existing meanings
unchanged. A shorter known record is invalid. Duplicate instances, truncated records, invalid references, wrong
CRC, trailing bytes, or a mismatched count invalidate the blob. Hosts skip unknown
record types by length; an unknown output kind is never guessed. Unknown flag bits
are preserved but do not authorize actions. Header format/header-size/length are separately
validated; they are outside the CRC coverage. Unsupported descriptor format means
identity and diagnostics only.

The u16 length leaves room for additional records without continuation encodings.
The transport bounds the blob to 256 parts and the record count to 255. A firmware descriptor
outside either limit fails build validation. The receiver still serves one part from
const flash. IDENTITY part 0 uses bytes 20..21 for length, 22..23 for CRC;
record format is at part 2 byte 19, descriptor format at byte 20 (entry_10ms at 21; 22..23 zero).
Feature bits remain the six full words in IDENTITY part 1; no board-number
capability dispatch is needed.

## LINK — type 1, minimum length 16, one per physical ingress/control interface

| Offset | Type | Meaning |
|---|---|---|
| 0 | u8 | Port ID, 0..15; ingress source IDs use the same numbering |
| 1 | u8 | RETURN=1, RETURN_RELAY=2, FORWARD=4, CHAIN_END=8, MARKER=16, UPDATE_FORWARD=32; bits 6..7 reserved |
| 2 | u8 | Return-rate membership: bit 0 1 Mbaud, bit 1 500 kbaud, bit 2 250 kbaud |
| 3 | u16 | Maximum electronics stage directional delay, ns; 0 on local links |
| 5 | u8 | Certified electronics stage width change, ns (0..255); includes complete functional hop, cable separate |
| 6 | u16 | Minimum processing/reset gap, host µs; adaptive pulse PHY >=500, local links 0 |
| 8 | u8 | Maximum burst-enable lateness, host µs, at most 80; 0 without RETURN |
| 9 | u8 | Clock flags: bit 0 host rate lock required, bit 1 default HSI, bit 2 no receive echo while transmitting |
| 10 | u8 | Kind: 1 RS-485, 2 local UART, 3 SPI, 4 I2C, 5 single-wire, 255 other |
| 11 | u8 | PHY: 0 byte-packet/local, 1 adaptive pulse PHY (800/600/400/200), 2 raw-pixel ingress only; others reserved |
| 12 | u16 | MTU: largest protocol object payload in bytes; at least 48 for update support |
| 14 | u8 | Reply: 0 none, 1 scheduled return UART, 2 request/reply L1 |
| 15 | u8 | Reserved zero |

FORWARD covers normal command forwarding, UPDATE_FORWARD covers the complete
bootloader session, and RETURN_RELAY is independent. They are not interchangeable.
A chain-end port has neither FORWARD nor RETURN_RELAY; it may keep UPDATE_FORWARD (the last
board of a port that still passes update sessions on). A marker control port admits
one receiver. The host checks the installed topology and sums actual hop/cable
bounds; the sixteen answer slots are not a guarantee of sixteen physical hops.
Local UART/SPI/I2C links use PHY 0, reply 2 with RETURN (or reply 0 without it), and zero
rates/hop/distortion/gap/lateness/clock flags. They have no implicit chain, marker,
return window or rate-lock requirement. The adaptive pulse PHY permits kinds 1/5 and reply 0/1;
RETURN and a nonempty UART-rate mask agree with reply 1. A no-return final
receiver still declares its electronics stage timing for path admission, even
without FORWARD. MTU is at least 32; unknown PHY/kind is not a usable link.
Kinds 2/3/4 use L1 framing from [transport.md](transport.md); other adapters need
their own documented framing. Board firmware owns endpoints and link setup.
LINK is a capability description, not a requirement to fit any particular link.
For PHY 0 local LINK kinds 2/3/4, the scheduling tail is:

| Offset | Type | Meaning |
|---|---|---|
| 16 | u8 | Reserved zero on L1 (adaptive PHY uses this byte for its profile mask) |
| 17 | u16 | `local_latency_us`: worst acceptance-to-command-engine dispatch delay, us |
| 19 | u16 | `local_jitter_us`: worst physical output/latch lateness from local due time, us |

Both bounds are required for synchronized L1 scheduling. A record shorter than
21 bytes has unknown bounds; FFFF in either field is unknown, never unlimited
or zero. Bytes after 20 are skipped. Existing 16-byte local records remain
usable for commands without a synchronization claim. A board must qualify
the complete worst-load path and reject requests it cannot schedule within
these bounds. Acceptance, due time, phase and cross-chip skew are defined in
[transport.md](transport.md); no trailer TIME is required. Base physical fields
at 2..9 stay zero on L1; these bounds do not describe a return window.

PHY 2 uses kind 1 or 5, zero control/return flags/rates/gap/lateness/clock fields,
and MTU 0 (no command objects); its chip/decode envelope is firmware-qualified.
It may feed a PIXEL source mask but cannot be its control LINK. PHY 1 with MARKER
may interleave separately qualified raw pixels and adaptive pulse PHY commands at marker/reset
boundaries; the eight-hop guarantee applies to adaptive pulse PHY only. A mode is never guessed
from a product name. NAME is mandatory for every descriptor.

## DOMAIN — type 2, length 13

| Offset | Type | Meaning |
|---|---|---|
| 0 | u8 | Domain ID in high nibble, parent in low nibble; IDs 0..14, parent 15 = root |
| 1 | u8 | Kind: 1 bus/input, 2 converted supply, 3 external supply, 4 regulated supply, 5 board output rail |
| 2, 4 | u16 each | Minimum/maximum voltage, 10 mV; both 0xFFFF = unknown |
| 6 | u16 | Continuous current rating and default user ceiling, mA in this domain |
| 8 | u16 | Protection trip reference, mA; 0 = none, 0xFFFF = unknown |
| 10 | u8 | Enforcement flags below |
| 11 | u8 | Sense summary: bit 0 measured I, bit 1 measured V, bit 2 modelled I |
| 12 | u8 | Conservative conversion efficiency, percent 1..100; 0 = unknown/use qualified calibrated model |

Domains may be omitted if no powered output/sense references one. If present,
domain 0 is the sole root supply domain (not necessarily a Diff-Power port); all other domains reach it without cycles.
`0xFFFF` is **unknown**, never unlimited. A continuous rating of zero disables the
load. Trip current is not extra continuous-current allowance. External converter
ratings cannot be guessed from the board name. Kind 5 describes the board's
output hardware rating, not a converter's rating. Its parent is the supply
domain; an external conversion between them uses the calibrated voltage and
efficiency model, and must satisfy its separately qualified installation limit.
Kind 5 never implies HW_CLIMIT. An installed constrained converter feeding a
streamed load still requires hardware current limiting, even if the converter
is external to the board definition. Missing qualification prevents load admission.

Enforcement bits apply independently: `0x01 HW_CUTOFF`, `0x02 HW_RESETTABLE`,
`0x04 HW_CLIMIT`, `0x08 FW_LIMIT` (measured in this domain), `0x10 FW_MODEL`
(calibrated conversion), `0x20 LOCAL_ONLY` (firmware bits do not cover streamed
pixels). Hardware flags describe a physical constraint, including an upstream
upstream protection when that is the input rating's origin; firmware limiting
does not imply hardware enforcement. Any rated supply (kinds 2..4) reachable from a streamed output MUST have
HW_CLIMIT. Firmware is its secondary layer. A stream cannot be made safe merely
by blocking further data: LEDs can retain the last bright frame.

## PWM bank — type 3, length 12

| Offset | Type | Meaning |
|---|---|---|
| 0, 1 | u8 each | First physical channel (multiple of 5), count 1..5 |
| 2 | u8 | LED-side power domain |
| 3 | u8 | CAL=1, DITHER=2, STAGGER=4, SCENES=8, SKIP_RATE=16 |
| 4 | u16 | Continuous rating per channel, LED-domain mA |
| 6 | u16 | Shared continuous bank rating, same-domain mA |
| 8 | u8 | Enforcement flags |
| 9 | u16 | `cal_ms`: upper calibration duration per channel, ms |
| 11 | u8 | Reserved zero |

SKIP_RATE: the bank takes CONFIG 25, pulse skipping at 2 kHz (the default) or down
to 1 kHz. Its pattern tables hold at most 64 periods (`ULTRA_SKIP_TABLE_MAX`), so from
64 kHz up the slowest rate is f_pwm / 64. Without it a bank stays at 2 kHz and a host
does not send CONFIG 25 (it is not in the board's CONFIG CRC).

Banks cover contiguous channels without overlap or holes. Ratings can differ
between banks; per-channel user limits are independent. The first five channels
remain bank 0. A fifteen/twenty-channel board has three/four banks; no five-channel
array or u8 mask is widened in the small receiver. LIGHT_DEF's final payload byte
is base channel; its five roles refer to base..base+4. A NONE role need not fit the
channel count. Multiple lights addressing a physical channel are applied in light
index order, with the highest defined light owning that channel. Hosts should
avoid such overlaps. State allocation uses the firmware definition's light count, not 240.
A 2-channel Nano has one bank `(first=0,count=2)`; masks are 0x03, last three
entries zero, absent ACKS rows NEVER. A 10-channel board has `(0,5),(5,5)`;
bank 1 entry 4 is channel 9, bank 2 is UNSUPPORTED. A 7-channel board has
`(0,5),(5,2)`, proving a partial bank after a full one. Only the final bank may
be partial. LIGHT_DEF on two channels uses NONE for the other three roles;
no commands, calibration or output state may access padded entries.

## PIXEL output — type 4, length 31

| Offset | Type | Meaning |
|---|---|---|
| 0, 1, 2 | u8 each | Output 0..14, power domain, segment count 1..15 |
| 3 | u8 | STREAM=1, LOCAL_FX=2, CHAIN=4, CAL_PIXEL=8, MARKER=16, PACKET_STREAM=32 |
| 4 | u16 | Maximum stored frame bytes |
| 6, 8, 10 | u16 each | Maximum addresses for 3/4/5 bytes per address; 0 = unsupported |
| 12 | u16 | Declared local pixel bit-time reference, ns; times 1.023 must bound synthesis/clock/output variation (timing.md) |
| 14 | u16 | Worst forwarding/replay delay, host µs, including oscillator tolerance |
| 16 | u16 | Chip-preset membership, bit = ID; bit 7 remains reserved/zero |
| 18 | u16 | Continuous output current rating, mA in the output domain |
| 20 | u16 | Maximum requested local FPS; actual complete-frame rate may be lower |
| 22 | u8 | Enforcement flags |
| 23 | u8 | Source abilities: bit 0 local, bit 1 streamed |
| 24 | u16 | Membership of physical ingress ports usable by this output |
| 26 | u16 | `cal_ms`: upper calibration duration for this output, ms |
| 28 | u16 | Minimum latch/LOW time, host µs |
| 30 | u8 | Control LINK port; outputs sharing it share command transport mode |

MARKER requires MARKER on the control LINK. CHAIN requires FORWARD or CHAIN_END
on that LINK. STREAM agrees with the source mask and a nonempty ingress mask;
when raw pixels share a control port, that LINK must support MARKER.
PACKET_STREAM instead permits PIXEL_DATA over adaptive pulse PHY/L1 on a shared ingress
without MARKER. Separate raw stream ingress permits CHAIN control without MARKER.
Validate these capabilities from records; a local-link pixel board needs no chain.

CONFIG TARGET is the output index. IDs 64..76 hold chip, order, count, FPS,
model-channel-current, **output-domain** current ceiling, white mode, virtual
offset/length, policy, loss timeout, command line mode and smoothing. ID 77 is
source: 0 local, 1..16 ingress 0..15. Source must be in both advertised masks.
A raw-pixel ingress sharing the control port requires STREAM line mode; a separate
ingress can operate with CHAIN control. The last valid line-mode write updates
all output views of that shared port; reject any change invalidating an active
route. Raw pixels on one physical ingress may drive at most one output at a time; conflicting routes
are rejected without changing the setting. Two outputs can both run local, one
local and one streamed, or both independently streamed. Packet streaming addresses outputs explicitly and serializes transactions per
output; aggregate resource ceilings still apply. Raw streams are not duplicated. Source changes occur at a
complete-frame latch. All outputs share the resource and power budgets.

PIXEL_FX TARGET is `output<<4 | segment`, segment 15 means the whole output.
Its flags stay in its payload. Effect 0 stops local rendering; while source is
local the output becomes black. Effect commands do not implicitly change CONFIG
77. With a streamed source, configured standalone/loss policy can run the stored
local effect when the ingress is absent; the next permitted complete stream frame
takes it back. These rules preserve independent source choice per output.

## SENSE — type 5, minimum length 8

`0 id:u8, 1 quantity:u8, 2 domain:u8, 3 target:u8, 4 flags:u8, 5 exponent:i8,
6 full_scale:u16`. Full scale is the largest measurable magnitude in base units
multiplied by 10^exponent; current range is expressed in mA. It is a sensing range, not a continuous rating.
Zero means range unknown (never unlimited); qualified measured current senses
require a nonzero range. `FFFF` is 65535 here, not a rating sentinel.
At saturation hosts show “≥ full scale”, even when the transport can encode more.
For example, a 10 A board could declare 16500 mA full scale with exponent 0;
that is a sensing range, not its rating. Values are firmware-local facts.
Quantity 1 = current (mA), 2 = voltage (10 mV), 3 = temperature (0.1 °C); the
base unit is multiplied by 10^exponent. Flags: MEASURED=1, MODELLED=2,
INPUT_SIDE=4, DIE=8, BOARD_TEMP=16. DIE/BOARD_TEMP are mutually exclusive temperature
locations; they are not repeated LINK capabilities. Target 255 is domain/board aggregate; a channel target identifies
an individual model. This declares topology, not a new arbitrary sensor query.
STATUS reports root input I/V and MCU temperature; ENERGY reports domain-1 LED
voltage; banked calibration pages report modelled channel contributions.
Additional senses may remain descriptive metadata until a page is assigned;
their presence never implies an invented page layout.

## TIMING — type 6, minimum length 12, optional until qualified

`0 port:u8, 1 qualified:u8, 2 certificate_id:u16, 4 sample_quantum_ns:u8,
5 threshold_ns:u16, 7 uart_error_ppm:u16, 9 burst_isr_us:u8,
10 t0_error_us:u8, 11 total_uart_gap_us:u8`.
Port 255 applies to every LINK on the board. Emit one board-wide record when
port timings agree; differing ports require one record per port with no board-wide
record. Unqualified firmware definitions omit TIMING entirely. Absence means unqualified.
Qualified 0, if received, means an intended budget, not measured capability; certificate is 0.
Threshold is round(1000 * threshold_samples / sample_mhz) ns for profile 800:
V203 22/36 MHz ->611 ns, V407 31/50 MHz ->620 ns. Quantum rounds upward at the
slow HSI extreme. For adaptive links append a seven-byte TIMING tail: supported
profile mask:u8 at 12 and nominal threshold_ns:u16 for 600/400/200 at 13/15/17
(V203 833/1250/2500, V407 840/1240/2500). Round threshold bounds outward with
an extra 1 ns when reconstructing from integer ns. Tail absence supports only
800; masks must match LINK and advertised app/bootloader capabilities. The
fixed sampler thresholds remain normative; scheduling lock does not trim HSI.
UART/ISR/edge/gap fields retain their independent meaning.
Qualified 1 requires a board-owned bench certificate and compliance with the applicable parts of
of timing.md, including the separate turnaround-check ISR bound. All advertised operating profiles must qualify; local links do not claim adaptive pulse PHY timing.
The descriptor qualification flag is set only when all advertised profiles qualify. Host test tools can exercise draft fixtures;
production discovery does not silently treat pending values as qualification.

## RESOURCES — type 7, length 14, one per board with pixel outputs

`0 frame_pool_bytes:u16, 2 simultaneous_frame_bytes:u16,
4 local_render_bytes_s:u32, 8 stream_bytes_s:u32,
12 max_active_pixel_outputs:u8, 13 max_simultaneous_calibration_jobs:u8`.
Byte-throughput fields bound aggregate processed frame payload, excluding wire
reset. Values are ceilings, not delivered FPS guarantees. Every operating point
must meet count, frame-byte, simultaneous-pool, byte-throughput, wire-time and
power limits together. Zero means unavailable; draft fixture values are not
certified. Boards without pixels omit this record and serialize calibration (one job).
Analog banks don't consume pixel-output slots. Shared sensors serialize
calibration; another output must be held at a known safe baseline during a probe.

## NAME - type 8, length 1..63, exactly one per board

UTF-8 product display name, no NUL or control characters. No instance prefix.
Unlike fixed-field records, NAME uses its whole value as text, with no appended binary tail.
Medium/Mini/Nano are ordinary text. Unknown names do not affect support; the
hub displays controls from the validated capability records. Local links may
omit power domains/senses entirely on control-only nodes; unavailable telemetry
returns UNSUPPORTED or its existing unavailable sentinel, never fabricated zero.
If domains exist, domain 0 means the board's supply, not a 2250 mA RS-485 port.

## Configuration, telemetry and power

Allocation is fixed even for unassigned IDs:

| IDs | Scope |
|---|---|
| 0 | Unused |
| 1..10, 21..63 | Board (TARGET 255) |
| 11..20 | Channel |
| 64..127 | Output |
| 128..255 | Reserved |

Board TARGET 255 takes board IDs 1..10, 21..25 and domain limits `32+d` for domain
1..14 (33..46); root domain 0 uses CONFIG 21 only. Channel TARGET 0..239 takes
11 CURRENT and 12 MAX. Output TARGET 0..14 takes 64..77. Scope is determined by
ID, so channel 1 and output 1 cannot be confused. CONFIG 2 is in 10 ns units.
CONFIG 25 (pulse-skip rate, Hz) takes 2000 or 1000 on a SKIP_RATE board.
Unknown scope/id pairs are ignored. Values above a known rating clamp to the
rating; 0 restores the rating. No setting raises a hardware ceiling. Unknown
ratings require a separately qualified model/operating bound before applying a
load. Invalid model/voltage data causes conservative limiting or refusal.

For each domain, sum demand and enforce its own effective ceiling. Convert between voltage domains using
`I_input = I_LED * V_LED / (V_bus * efficiency) + I_logic`; do not take a minimum
of currents at different voltages. Channel limits of 3 A/5 A are at LED voltage;
reported current is measured at the input and allocated through the calibrated model.
The board must keep the coefficients needed for local protection. Count shared
idle current once, and require pre-frame admission on constrained local outputs.
Calibration uses the same ceilings and aborts on overload; existing empirical
calibration is not short-circuit protection.

Synthetic example: a 5 V/2 A supply domain with conservative efficiency 85%, a 48 V input and 20 mA
logic contributes `2000*5/(48*0.85)+20 = 265.10 mA` input. A user 1500 mA
LED-domain limit lowers this to 203.83 mA input. Input rating 2250 mA is checked
separately. Two 5 V domains limited to 2 A and 3 A together contribute 632.75 mA
input including the logic once. A 3 A channel in the 2 A domain remains subject to
the domain's 2 A aggregate limit even if its user channel setting is 3 A.

POLL argument 3 (slot byte 10) banks OUTPUTS, ACKS, HEALTH, CAL_TEST,
CALIBRATION and DIAG part 1 in groups of five. Array and mask offsets are
unchanged; local bit k maps to global index 5*bank+k. ACKS' board row repeats.
OUTPUTS banks are valid through `ceil(max(channels,lights)/5)-1`; channel-only
pages use channel count, ACKS uses max(lights, channels, pixel outputs), with NEVER set separately for each unsupported column type. Missing final-bank entries are
zero; absent ACKS rows set NEVER. CALIBRATION arg1 chooses 0 PWM or 1 pixel;
pixel calibration and PIXEL/PIXEL_FX bank directly by output. PIXEL part 0 owns
line/counters, part 1 applied configuration. PIXEL_FX has parts 0 state, 1 effect
membership, 2 palette membership; part 0 arg1 selects groups of four segments.
An ACKS column selecting PIXEL_FX (type 11) MUST report SEQ zero and NEVER set
for every row, including the board row, even after successfully applying it.
ACKS cannot select a segment; NEVER in this column is not a capability report.
For segment acknowledgement, poll PIXEL_FX part 0 with arg3=output and
arg1=floor(segment/4). Entry segment%4 in RUNNING/SEQ describes that segment;
SEQ is its last applied PIXEL_FX command. Check the requested segment's SEQ
and resulting state, never an ACKS output/light row or the POLL answer's SEQ.
Read all affected groups for a command changing multiple segments. The total
segment count repeats in byte 5; absent entries in the final group are zero.
Banks are not inserted into POWER's acknowledgement arguments or LIFE_SYNC.
Unknown parts/kinds/banks return UNSUPPORTED. Keep at most one outstanding
query per receiver/SEQ; replies do not echo the requested bank/part. CONFIG
sequence numbers share the TARGET/type namespace across channel/output scopes.

CONFIG CRC is CCITT-FALSE over sorted `(TARGET:u8,id:u8,applied_value:u16)`
records for explicitly set settings. Sort by target then id. STATUS/ACKS cover
the board's whole set, pixel state covers just that output's IDs 64..77. Defaults
not explicitly set are omitted; a setting sent as zero contributes its resolved
applied value. Hosts preserve requested values separately if needed.

LIMITS page 0x12 is optional; V203 builds omit it and answer UNSUPPORTED.
Hosts derive ceilings from descriptor ratings, applied CONFIG/CRC and calibration,
retaining unknown quality when a model is unavailable. If supported, it reads applied ceilings without parsing a descriptor on the receiver.
POLL arg0 = kind (0 domain, 1 channel, 2 pixel), arg1 = index, other arguments zero.
DATA: bytes 0 kind, 1 index, 2 domain, 3 enforcement; u16 at 4 rating, 6 applied
user ceiling, 8 effective ceiling, 10 current, 12 voltage in 10 mV, 14 trip;
byte 16 quality (measured=1, modelled=2, valid=4, local-only=8, limiting=16),
17 parent (15 root); u16 at 18 calibration ceiling; 20..23 zero. Currents are mA
in the selected domain, including model-converted ancestor constraints in the
effective value. Unknown values are FFFF; trip 0 means none. OUTPUTS and stored
calibration coefficients remain root-input contributions, preserving bank-0
semantics. This page exposes the LED-domain view and effective user settings.

Calibration's host timeout is `50 + ceil(1.5 * sum(cal_ms for requested work))`
ms, with completion/failure on CALIBRATION ending the wait. Pixel jobs have their
own cal_ms; analog work multiplies each bank's per-channel value by its channel
count. Completion states, not a guessed fixed five-channel delay, drive workflow.

## Firmware ownership and format examples

Each firmware repository maintains `src/board/<name>/` with product string,
allocated board ID, opaque assembly IDs, ratings, channels, senses, link abilities,
MCU/pins and const descriptor. Firmware-local tooling produces an image manifest
and jig sidecar with the same bytes. The protocol repository holds only formats
and synthetic tests (board ID 254), never a source of production ratings.
New products add a firmware definition and one ID-list entry when needed.

Changing assembly ratings, pin routing or chain fitment requires a distinct
firmware-local assembly identity where incompatible images could otherwise
match. Image revision intervals must be validated in that firmware's release
pipeline against every supported physical revision of the exact assembly, with
byte-identical descriptors. No unlisted revision is silently covered. The full
0..255 test range is encoding coverage, not a releasable image.

Synthetic vectors include 2-channel and 7-channel partial-bank devices, a
10-channel local SPI/I2C/UART controller with no Diff-Power/relay assumptions,
20-channel and dual-pixel format stress cases. Their names and numeric limits
are arbitrary test data, not future-product definitions. Board facts can change
without a protocol release. Requested user limits clamp to the declared rating;
unknown ratings never authorize load and zero requests restore the firmware rating.

CALIBRATION part 0 byte 7 is signed i16 **2 mA per step**, for both PWM
input offset plus board idle and pixel aggregate idle. HEALTH byte 16 uses the
same signed 2 mA unit for dark-current deviation. Hosts convert each to physical
mA exactly once, using a signed 32-bit destination: raw -32768 is -65536 mA,
raw 32767 is 65534 mA. Calibration channel coefficients at byte 9 and the
connected load at byte 19 remain unsigned u16 mA. Published model JSON uses
physical mA for `idle_ma`, `ip_ma` and channel coefficients; readers do not
apply the wire conversion again.

All unsigned current readings/peaks saturate at their field maximum, never wrap.
Compact lifetime/dip currents use 0.5 A steps, DIAG trip uses 2 A; signed dark/idle
currents use 2 mA steps (±65.5 A). CONFIG 68 uses mA. SUPPLY and pixel calibration
voltages use 10 mV, matching STATUS VBUS and DOMAIN. STATUS VDDA is unsigned
u16 mV (0 unknown, maximum 65535 mV), matching both encoder and decoder.
ENERGY boot mWh wraps modulo 2^32;
hosts accumulate modular deltas and reset tracking on reboot. LIFE_SYNC bounds
energy by input continuous rating × input maximum voltage; unknown inputs reject
the energy merge (other eligible counters still merge). With powered-time increase
`dt` in 0.1 h units, the allowed energy increase is
`ceil(rating_ma * v_max_10mv * (dt + 10) / 1000000)` Wh, capped to the counter's
remaining room; use at least u64 intermediates. At 65000 mA and 4800 units of
10 mV, the one-hour allowance alone is 3120 Wh. See
[power JSON v3](power-json.md) and [setup S2](setup-s2.md).

Receiver cost estimates, including the expanded example sizes, are in
[costs.md](costs.md). These are spec-derived storage and planning estimates, not
linked firmware measurements.

### Board 1 invariant

[Dig2Analog-Plus-5ch](https://github.com/intermittech/quinled-ultra-firmware/wiki/Board-Dig2Analog-Plus-5ch) (board 1) ALWAYS forwards: LINK.FORWARD is set on its
single-wire command link, CHAIN_END is clear. It has NO return channel:
RETURN and RETURN_RELAY are clear, reply=NONE, return timing/rate fields zero.
There is no chain/no-chain assembly bit or variant. Its only assembly variants
are 3 A and 5 A channels, represented by opaque IDs owned by board firmware.
The firmware artifact/jig supplies its descriptor to the inventory; absence of
replies is expected and never means an empty port. It cannot relay downstream
telemetry. UPDATE_FORWARD is a separate bootloader capability, not implied by
FORWARD. This allocation document is not a numeric assembly-ID registry.

### Adaptive LINK tail

Append profile_mask:u8 at LINK value offset 16 (bits 0..3 = 800/600/400/200;
upper bits zero, bit 0 mandatory). Prefix-only readers safely treat an absent
tail as 800-only; they must not authorize fallback by recognizing PHY=1 alone.
The hub intersects every node's app and bootloader masks, and separately checks
complete inventory, timing qualification and observed margins. Tail data does
not make a RETURN-less node reply. Profile ID is runtime state in LINK_STATS,
not mutable identity or an assembly variant.
