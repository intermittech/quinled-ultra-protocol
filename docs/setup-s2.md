# Setup export S2

S2 is a hub-generated JSON setup snapshot, consumed by the setup generator.
The normative structural schema is [setup-s2.schema.json](schemas/setup-s2.schema.json),
with semantic reference checks in `test-vectors/host_formats.py`. This is a
separate format from the wire, update ABI, flash records and power JSON v3.
S1 import is unsupported. Unknown top-level schema values and duplicate JSON
keys are rejected; unknown members are preserved or ignored, never executed.

The top level is `{ "schema":"S2", "boards":[...] }`. Each board has:

| Member | Schema / meaning |
|---|---|
| `uid` | 24 lowercase hex digits, MCU UID bytes in address order; null only for explicitly inventoried non-return boards without a read UID |
| `port`, `position` | Controller interface u8 and adapter endpoint ordinal 0..239 (chain position on Diff-Power); the pair is unique in this export |
| `target` | 32 lowercase hex digits encoding the exact immutable 16-byte target tuple from update.md; never a guessed board ID |
| `desc_crc` | u16 descriptor CRC-16/CCITT-FALSE; re-read descriptor when it changes |
| `domains` | Array of `{id,lim_ma}`: ID 0..14, applied domain ceiling u16 mA; includes root 0 when nonempty; empty on a control-only node |
| `channels` | Array of `{i,dom,lim_ma}`: contiguous physical index 0..239, referenced domain, applied per-channel ceiling u16 mA |
| `outputs` | Array of `{o,dom,n,source,lim_ma}`: contiguous pixel output 0..14, domain, applied address count u16, source 0 local or ingress+1 (1..16), ceiling u16 mA |
| `config` | Explicit settings as `{target,id,value}` records, sorted by target then id; u16 applied values, no duplicates |

Arrays carry numeric indices, so channel 10 is one channel, not characters 1 and
0. Domain/output/channel ceilings use 65535 for unknown and 0 for disabled.
CONFIG zero has its setting-specific meaning; user ceilings exported in `config`
are resolved applied values, matching the CRC convention. Values in `config`
retain each ID's units: currents mA (including ID 68), CONFIG 2 in 10 ns,
pixel count in addresses, FPS in frames/s, and source as above. A zero-output
board exports `outputs:[]`; a pixel-only board exports `channels:[]`. The format
supports up to 240 boards, 240 channels per board and 15 pixel outputs per board.
Transport capacity does not authorize a longer physical chain: the hub admits
at most eight boards per adaptive pulse PHY chain, with the cable/distortion reserve in timing.md.
Target byte 6 carries opaque exact assembly identity; capabilities/ratings come from the descriptor,
not identity bits; importing a different assembly's snapshot cannot override that identity.
UIDs, when present, are unique; topology positions and all indexed array keys
are unique. Arrays are sorted by their keys; boards by `(port,position)`.

Before applying an import, the hub must match live UID (where available), complete
target tuple and descriptor CRC, then validate indices, domains, source/ingress
masks, counts and every limit against that descriptor. A mismatch requires
rediscovery/review; never issue guessed commands. Unknown boards/formats may be
shown with generic read-only identity controls. Unsupported IDs are retained or
skipped according to wire CONFIG rules. Import is not authority to raise a rating,
alter immutable identity or change firmware. It does not contain credentials,
firmware, calibration records, lifetime counters or opaque flash state. It is a
configuration/control snapshot; it does not promise backup of runtime animation
state. If limits and known CONFIG entries disagree, reject before applying.

The download is canonical JSON with `Content-Type: application/json` and exact
HTTP `Content-Length`. Canonical encoding uses sorted object keys, ASCII with
Unicode escaped, compact commas/colons, no trailing newline and no NaN/Infinity.
JSON integer types and schema bounds are checked before generating commands.
The same payload is length-delimited by the following envelope on the HA path.

## Paging

Power v3 and S2 use the same ASCII envelope (`P3` or `S2` tag):

```text
TAG:sha256:part:count:length:chunk
```

`sha256` is the full 64-character lowercase hexadecimal SHA-256 of the complete
canonical ASCII JSON bytes. `part` and `count` are exactly four lowercase hex
digits; `part` is zero-based, `count` is 1..65535. `length` is exactly eight
lowercase hex digits for total JSON byte count. All separators are literal `:`.
The fixed header is 87 characters, leaving **168** for each chunk: every part
is at most **255** characters including the envelope. Nonfinal chunks contain
exactly 168 characters; the final chunk contains the remainder (1..168).
Count must equal `ceil(length/168)`. Parts split the encoded ASCII text anywhere,
including inside an escape sequence; decode JSON only after reassembly.

A receiver groups by `(TAG,sha256,count,length)`, accepts out-of-order parts and
identical duplicates, rejects conflicting duplicates/mixed transfers, and waits
for every part. It then checks exact byte count, digest, JSON syntax, canonical
encoding, schema and references. Missing, corrupt or truncated pages never yield
a partial setup. Implementations may impose a smaller memory/size ceiling, but
must report that refusal explicitly. A digest is transfer integrity, not an
authorization or signature. A new export must not mix with an earlier one.

Golden cases in `test-vectors/host_vectors.json` are `setup_analog20` (channel 19,
four banks and scoped settings) and `setup_dual_pixel`
(two local/stream-capable outputs with independent source/limits, plus an
synthetic one-output device with unknown rating sentinels). Both include canonical JSON, byte length, SHA-256 and all
parts. The runner checks round trips, reversed order, identical duplicates and
rejection of mixed/missing/corrupt pages, S1, oversized currents and bad scopes.
