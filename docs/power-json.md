# Power JSON v3

This host/dashboard contract is independent of wire, update and record versions.
The normative structural schema is [power-v3.schema.json](schemas/power-v3.schema.json).
`test-vectors/host_formats.py` adds identity, reference and array-length checks;
`host_vectors.json` contains two complete canonical payloads and their paged text.
No receiver JSON parser, buffer or extra telemetry page is required.

Top level is `{ "v":3, "dom":[...], "out":[...] }`, one board per snapshot.
A control-only node may export both arrays empty. A supply domain denotes its
own hardware supply, not a required Diff-Power input port.
The hub associates the snapshot with its discovered UID/target. Consumers reject
other `v` values and duplicate JSON keys; unknown members are ignored/preserved.
Integers are JSON integers, never strings or floating point. Arrays are ordered
by domain ID, then output `(kind,o)`; identifiers, not array positions, are keys.

| Domain member | Meaning / units |
|---|---|
| `id`, `parent` | Domain 0..14; parent 15 only for input domain 0; others reach 0 without cycles |
| `kind` | 1 input, 2 converted supply, 3 external supply, 4 regulated supply, 5 board output rail |
| `lim_ma` | Applied continuous ceiling, u16 mA; 65535 unknown, 0 disabled |
| `trip_ma` | Protection trip reference, u16 mA; 0 none, 65535 unknown |
| `enforce` | Descriptor enforcement mask: HW_CUTOFF 1, HW_RESETTABLE 2, HW_CLIMIT 4, FW_LIMIT 8, FW_MODEL 16, LOCAL_ONLY 32 |
| `v` | Voltage in **10 mV**, integer 0..65534; null unavailable |
| `ma` | Current, u16 mA; null unavailable |
| `mw` | Power, u32 mW; null unavailable |
| `meas` | Quality bits below, for this domain's readings |
| `idle_ma` | Shared idle contribution in this domain, u16 mA; null unknown; counted once |

| Output member | Meaning / units |
|---|---|
| `kind`, `o` | `pwm`: first physical channel of a five-channel bank (multiple of 5); `pixel`: output ID 0..14 |
| `dom` | Referenced LED-side domain ID |
| `n` | PWM bank channel count 1..5, or pixel address count 1..65535 (IC/groups, not LED packages) |
| `c_ma` | PWM: `n` full-level channel contributions; pixel: five whole-used-strip coefficients in R,G,B,W-or-WW,CW order. All u16 mA in `dom`, excluding shared idle |
| `lim_ma` | Applied aggregate bank/output ceiling, u16 mA; 65535 unknown, 0 disabled |
| `model` | `none`, `configured`, or `calibrated` |
| `quality` | Quality bits below for coefficients/limit applicability |

Quality is MEASURED=1, MODELLED=2, VALID=4, LOCAL_ONLY=8, SATURATED=16.
An absent or stale model clears VALID; a measured value is not inferred from an
output being present. Model coefficients reported by wire calibration in input
units must be converted to the stated output domain using a qualified model;
without one use `model:none`, zero coefficients and VALID clear. They are not
relabelled as LED-domain measurements. Idle lives only in `dom`, so multiple
outputs sharing a converter never duplicate it. A signed dark/offset correction
is not idle demand: the hub applies it internally before reporting nonnegative
physical idle/current. Zero coefficients with VALID clear do not prove zero load.

Readings saturate at 65535 mA / 4294967295 mW, never wrap. Quality SATURATED
also covers ADC saturation below the transport ceiling: the dashboard displays
`>= SENSE.full_scale * 10^exponent` in the sense's base unit, not a fictitious
precise current. Unknown limits remain unknown; they never mean unlimited.
For valid readings, `mw = floor(ma * v / 100)` uses a wide intermediate.
Do not sum currents across voltage domains. Convert demand through qualified
efficiency/model information, and show input and converter headroom separately.
Local-only limits do not claim firmware protection for streamed pixels.
ENERGY's boot mWh wraps modulo 2^32 on the wire; the hub tracks modular deltas
between reboots rather than interpreting wrap as negative consumption.

Canonical download JSON is ASCII (Unicode escaped), sorted object keys, compact
commas/colons, no trailing newline, no NaN/Infinity. `Content-Length` gives its
byte count. The HA path uses the shared [paging envelope](setup-s2.md#paging)
with tag `P3`; each complete part is at most 255 ASCII characters.
Never publish a partially reassembled snapshot.

The two golden cases are `power_65a` (65000 mA at 4800 units = 3120000 mW,
including an unknown limit sentinel) and `power_two_outputs` (two separately
limited 5 V domains with shared input accounting). Their canonical strings,
lengths, SHA-256 digests and every page are checked in `host_vectors.json`.
