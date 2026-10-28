# Commands independent of the link

Slots, headers, answers, descriptors and update blocks are byte objects. Their
CRC, endian order, TARGET, SEQ, page, limits and update acceptance rules do not
depend on Diff-Power, a waveform, a connector, or a board's product name.
The adapter delivers a complete checked object plus the addressed endpoint to
the command engine. TARGET addresses a resource *inside* that endpoint; it is
not an I2C address, SPI chip select, UART address or chain position.

The descriptor has one LINK per exposed interface, with kind, PHY, MTU and reply
mode. Multiple interfaces can expose the same endpoint and UID; the controller
must serialize changes and outstanding queries across them. Unsupported link
kinds permit inventory but not guessed framing. Transport errors never execute
partial commands. CRC is error detection, not authentication.

## Diff-Power adapter

The adaptive pulse PHY uses per-port 800/600/400/200 kHz as defined in
[timing.md](timing.md). MSB first; a normal frame remains N 32-byte slots plus
an eight-byte trailer. Each board removes its first slot. TIME denotes the
last falling edge, computed using the backend's actual final HIGH duration,
not the next nominal bit boundary. All objects in a frame use one profile.
Eight-board frame plus 500 us gap is 3140 / 4020 / 5780 / 11060 us at the four
nominal rates (318.47 / 248.76 / 173.01 / 90.42 command frames/s).

Preamble, normal, marker-command and update traffic use the selected profile;
apps and bootloaders auto-detect from edge periods, including at 200 when 800
no longer decodes. Hardware forwarding introduces no store-and-forward frame
latency. Every connected node, including passive receivers/pixels, belongs in
the inventory: any non-Ultra or unknown node locks the line to 800.
RMT/PARLIO encoding and grouped scheduling are in [senders.md](senders.md).
The C99 sender reference implements wire 3 and exports exact-clock golden traces.

Return UARTs retain their separate rates and scheduled windows. A no-return
single-wire adapter can forward adaptive pulse PHY and gets its descriptor from a firmware
artifact/jig; it does not pretend to have a return port. [Dig2Analog-Plus-5ch](https://github.com/intermittech/quinled-ultra-firmware/wiki/Board-Dig2Analog-Plus-5ch) always
forwards; lack of RS-485 or return is not lack of forward capability.

## Local UART / SPI / I2C adapter (L1)

Use a single transport packet format: `A6 31 | endpoint:u8 | kind:u8 | txid:u16 |
length:u16 | payload[length] | CRC16:u16`. Integers are big-endian; CCITT-FALSE
covers all bytes before CRC. Payload length is 0..6144, additionally limited by
LINK.MTU (maximum *payload* bytes). Packet size is length+10. Kind 0 is an empty
READY query/reply, 1 a normal slot (32 bytes), 2 a normal answer (32 bytes),
3 an update frame (48 bytes, including its independently versioned trailer).
Other kinds are rejected. UART is 8N1 at a board-declared commissioning rate;
resynchronize by magic, plausible length and CRC after timeout/error. No packet
may execute until fully received; bound assembly timeout in the board definition.
Endpoint 0..254 names a WCH node in the local controller's firmware/topology;
255 is reserved. A reply echoes endpoint/txid; one outstanding transaction per
endpoint is the baseline. Wrap is safe only after the previous transaction ends.

For a kind-1 slot, **acceptance** is the captured physical completion of the
final envelope CRC byte of a complete, correctly addressed packet with valid
envelope and slot CRCs: UART end of the final stop bit, SPI sampling edge of
the final bit, or I2C falling edge of the final byte's ninth (ACK) clock. Exact
packet length and the transport transaction must be valid. Capture this instant
before validation/queue/ISR/handler delay; a bad or truncated packet establishes
no time origin and executes nothing. "Kind 1" here is the L1 packet kind;
the descriptor LINK kind is UART 2, SPI 3 or I2C 4.

START and every phase offset are relative to that receiver-local acceptance
instant, in their existing units: SET signed i16 milliseconds and
PIXEL_FX signed i32 milliseconds. Logical start = acceptance_us + 1000*offset_ms;
zero means acceptance, a positive offset schedules ahead, a negative offset
catches up the transition/pixel-effect phase when applied. No trailer TIME or shared
host clock is needed on L1. Dispatch delay never changes the phase origin.
Durations and segment positions retain their existing meanings and units.
Retries with the same txid, or an already-applied command SEQ, preserve the
original start; they never restart the effect relative to a later acceptance.

An L1 board claiming synchronized output MUST declare both bounds in the LINK
tail ([descriptor.md](descriptor.md)): `local_latency_us` at 17..18 is the
maximum acceptance-to-command-engine dispatch delay; `local_jitter_us` at 19..20
is maximum physical output/latch lateness relative to
`due=max(acceptance,logical_start)`, with no output before due. These bounds
include worst-load CRC validation, queues, interrupt entry/interference, local
clock uncertainty and output/latch quantization for every accepted offset.
At dispatch, evaluate phase from the original logical start, including elapsed
time for a past start. A request that cannot meet the declared bounds must be
declined before applying it. Missing bounds or FFFF mean unknown and do not
authorize synchronization claims. Zero is a qualified zero bound.

For chips on one board, the controller accounts for each chip's actual packet
acceptance instant and adjusts offsets in their existing millisecond units.
Pairwise output skew is bounded conservatively by the difference in their due
times plus both declared jitter bounds, including uncertainty in acceptance
and offset rounding. Serial packet delivery does not imply simultaneous starts.
For one accepted command fanned out inside a board, the same acceptance origin
applies to every affected output and the LINK bounds cover that internal fanout.

SPI uses one EXTRA-low transaction per packet; I2C uses a single addressed write
per packet. Responses use a two-step read: exactly two bytes pending packet
length (big-endian, zero=not ready), then exactly that many bytes. SPI clocks
the two reads in separate EXTRA transactions with dummy zero MOSI; I2C uses separate
read transactions. The length read snapshots the response until the payload
read completes; an incomplete read restarts at byte zero. The controller polls
within the firmware-declared timeout. UART responses are ordinary packets.
Adapter retries use the same txid; a duplicate request returns the cached answer
or BUSY and never reapplies a non-idempotent command. Cache until the next txid;
reboot clears it and requires rediscovery. Packet queues and timeouts are local
firmware resources, not a required allocation on Diff-Power V203 builds.

POLL executes after packet acceptance and returns its same 32-byte page answer, with index 0;
its index/R/rate must be 0/1/0 in L1. There is no RW trailer, marker, slot stripping,
HSI rate lock, turnaround check, relay, or 500 us reset requirement on L1.
Update commands retain image/session/CRC/flash pacing and selection semantics;
the adaptive pulse PHY trailer TIME is ignored here. READY does not authorize ignoring an
advertised flash-busy bound. Other adapters may be specified later without
changing command/page formats. Unknown controllers can discover IDENTITY and
DESCRIPTOR using the same POLL commands before any power or channel setup.

## Pixels across an Ultra chain

Slow-profile pulses and command bytes must never reach a real WS281x strip. [Diff-Power Ultra Receiver Digital-Medium](https://github.com/intermittech/quinled-ultra-firmware/wiki/Board-Diff-Power-UR-Digital-M) holds its strip
blank during commands/updates and uses PA7 MOSI to emit newly encoded
chip-compliant pulses. It does not replay sampled bits for this path.
Chain output remains transparent forwarding; no regeneration is claimed. Local effects use the same chip encoder.

For controller-streamed pixels over adaptive pulse PHY or L1, PIXEL_DATA (type 12, TARGET=output)
transports bytes in the existing 27-byte command payload. LINK ingress and PIXEL
source masks must authorize that route. A LINK with adaptive pulse PHY or L1 may carry these
packets without MARKER; MARKER is only required when control shares a *raw pixel*
ingress. PIXEL flags bit 5 PACKET_STREAM declares this ability; STREAM and the
stream source-mask bit must also be set. Unknown capability means no packet stream.

Payload: operation:u8, offset:u16, data[24]. SEQ is a transfer ID shared by its
BEGIN/CHUNK/COMMIT operations, exempt from ordinary (type,TARGET,SEQ) dedup.
BEGIN(0) has offset zero, data length:u16 then complete-frame CRC16:u16 then
20 zeros. Length must equal configured addresses times bytes/address, fit both
output and aggregate frame storage, and pass power admission. A fresh BEGIN
aborts incomplete assembly; duplicate identical BEGIN preserves progress. A
different BEGIN with the same active SEQ is rejected. Output takes ownership of
its frame buffer at a latch boundary before accepting chunks; it may retain the
previous physical LED state, but cannot read a buffer being overwritten.

CHUNK(1) carries up to 24 bytes at the next contiguous byte offset; trailing bytes
of the last chunk are zero. Offsets need not be a multiple of 24, but every
nonfinal chunk is exactly 24 bytes. Accept identical retransmission wholly below
the accepted cursor; reject conflicting, overlapping or out-of-order data. A
different SEQ, invalid length/offset, or bad slot CRC cannot advance progress.
COMMIT(2) has offset=length and zero data; accept only complete length and matching
whole-frame CRC, recheck limits, then encode the entire frame and latch it. Repeated
COMMIT is idempotent. ABORT(3) has offset/data zero and discards that SEQ. Keep
one active transfer per output and reject a new BEGIN until a committed buffer
has finished output. Inactivity for 2 s aborts assembly; loss policy then applies.
Reserve the frame buffer before acknowledging BEGIN; on a one-way link the host
must wait the descriptor's worst complete-output/latch time before BEGIN. A
receiver still busy ignores the whole new transaction, never partially overwrites
the buffer being encoded. After timeout/CRC failure the invalid buffer is not
displayed; regenerate a configured local/black fallback after the output latch.

PIXEL page part 2 (arg3=output) returns SEQ at byte 0, state at 1 (0 idle, 1
receiving, 2 committed/outputting, 3 error), accepted cursor:u16 at 2, total:u16
at 4, expected CRC16 at 6, last successfully latched SEQ at 8, valid flag at 9;
10..23 zero. Query/retry is optional for a one-way path, never inferred success.
Single-buffer operation is legal: local rendering pauses while assembling, with
no additional full-frame buffer. Streaming is resource-limited, not a guaranteed
FPS. For 600 RGB pixels, BEGIN + 75 CHUNK + COMMIT = 77 normal frames.
At eight boards this takes >=241.78 / 309.54 / 445.06 / 851.62 ms at
800/600/400/200, before local encoding, polls and retries; faster steps require
a separately qualified smaller-distortion eight-hop path. At nominal 18.5 ms
local output/reset the single-buffer ceilings are 3.84 / 3.05 / 2.16 / 1.15 FPS.
These packet rates differ from the continuous 600-pixel serialization ceilings
in senders.md. No high-FPS raw slow-profile stream is added by this change.
Local effects render at local pixel speed, independent of line profile.

A separate single-segment raw pixel ingress can retain a qualified chip decoder
and marker mode, followed by regeneration. It must advertise its own limits;
raw pixel timing is not the eight-hop command guarantee. On reset the existing
digital output defaults to bypass: the host must remain LOW until application boot
and strip blanking are complete, then send adaptive pulse PHY. A lost/reset/failed receiver can
glitch the strip through that bypass; firmware cannot make reset-default bypass
fail-closed without a hardware change. The guarantee is for operating firmware.
