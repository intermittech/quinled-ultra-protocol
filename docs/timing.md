# CPU-independent timing contract - adaptive per-port profiles, wire 3

Times are physical host microseconds unless marked ns. V203 runs at 144 MHz
and samples at 36 MHz; V407 runs at 200 MHz and samples at 50 MHz. Raw HSI
error is bounded to +/-2.2% for this contract; host rate lock corrects scheduling,
not the raw sample grid. These are requirements, not measured certificates.
Only the Diff-Power/single-wire adapter uses the waveform and return windows.
Local UART/SPI/I2C commands use [transport.md](transport.md).

## Decision and hardware evidence

Normal is **800 kHz**; fallback steps are **600, 400 and 200 kHz**, independently
selected per hub port. A Diff-Power port steps down from measured margins; a
[Dig2Analog-Plus-5ch pin](#dig2analog-plus-5ch-pin-link-model) takes its profile from
its board count. A port may leave 800 only when its complete inventory
contains exclusively adaptive Ultra receivers, including their bootloaders.
A non-Ultra device, actual pixel on the line, unknown node or incomplete inventory
locks the port to 800. Real strips behind [Diff-Power Ultra Receiver Digital-Medium](https://github.com/intermittech/quinled-ultra-firmware/wiki/Board-Diff-Power-UR-Digital-M) always receive
locally regenerated chip timing. No slower waveform reaches an exposed pixel line.

The GUARANTEE per-hop width-error bound is 20 ns, including mixed paths.
The EXTRA qualified-path bound is 7.5 ns and requires a
matching assembly certificate for every stage. The forwarding allowance is 4 ns;
the separate strip-output allowance is 8 ns. Each cable/protection allowance is
<=50 ns. These are conditional bounds requiring complete assembly qualification.
Cable delay is 538 ns/100 m; pair-to-pair skew is not pulse-width distortion.
Supporting simulation gives 11..23 ns forward width error and -12.2..+17.4 ns
reply error at 100 m; it does not certify installed cables or assemblies.

The sender contribution is about 2 ns within its <=10 ns residual. Chain-port
direction control takes effect within 90 ns; restore-forwarding spare remains
>=2.87 us. Input-port timing and its 0.10 us check-path spare are unaffected.
The GUARANTEE source bounds cover 5 V only; 3.3 V operation needs supplier
bounds or full PVT/load/lot qualification. EXTRA supply coverage over 3.0..5.5 V
and -40..125 C needs written confirmation. Typical plots and simulations do not
close these gaps. Cable/protection distortion, direction delays and assemblies
still require qualification at 1/10/50/100 m. Numerical closure is not certification.

## Four exact profiles and the 1-100 m / eight-board budget

The path budget in this section is a Diff-Power (RS-485) port's: at most eight
receivers. A Dig2Analog-Plus-5ch pin uses the profiles but its own
[link model](#dig2analog-plus-5ch-pin-link-model).

One PARLIO sample clock is **9.6 MHz**. Durations below are exact rational
sample counts, never independently rounded integer ns. Sender residual HIGH
and period error is <=10 ns around its actual synthesized durations, including
clock error/jitter/pre-compensation and about 2 ns of sender output-path error
(leaving <=8 ns for the rest). RMT rounding is budgeted separately using
its actual ticks ([senders.md](senders.md)). No mid-frame profile change.

| ID / rate | Period / zero HIGH / one HIGH samples | Physical ns (period / zero / one) | V203 / V407 threshold samples | Certified path admission ns |
|---|---|---|---|---:|
| 0 / 800 kHz normal | 12 / 3 / 9 | 1250 / 312.5 / 937.5 | 22 / 31 | 220 |
| 1 / 600 kHz | 16 / 4 / 12 | 1666.6667 / 416.6667 / 1250 | 30 / 42 | 300 |
| 2 / 400 kHz | 24 / 6 / 18 | 2500 / 625 / 1875 | 45 / 62 | 450 |
| 3 / 200 kHz recovery | 48 / 12 / 36 | 5000 / 1250 / 3750 | 90 / 125 | 750 |

LOW = period - HIGH. Sender reset/processing LOW >=500 us; recognition >=280 us.
Inter-bit LOW is <10 us; 10..280 us is invalid, never a sender reset. No vanished
or extra edge is permissible. RX HIGH validation windows are the backend HIGH
extrema +/- (certified path distortion + 10 ns); classify with the table's
integer threshold and charge a full sample quantum. Use u16 HIGH/LOW and period
run counters: a 5000 ns period at 51.1 MHz is already 255.5 samples, before
period jitter. Acquisition timestamp accumulators use u32. Rate acquisition
and input synchronizers must be qualified. Before the backend is known, accept
the union of qualified RMT/PARLIO width envelopes for the detected profile.

For N boards the host budgets N cable segments, N hop allowances and
N-1 intermediate forwarding allowances. The final board's output is outside its
input path. With per-hop bound H, the worst-sign
bound is `D(N) = H*N + 4*(N-1) + sum(C_i)` ns.
Each qualified 1..100 m segment has C_i <=50 ns including protection/termination.
All signs accumulate; neither typical skew nor cancellation earns credit.
At C_i=50 each, GUARANTEE gives `D(N)=74*N-4`; qualified-path gives `D(N)=61.5*N-4`.
The GUARANTEE eight-board path uses **588 ns**. A ninth board or any uncertified segment fails. Tables use min(M0,M1) across both samplers; qualified-path is EXTRA.

GUARANTEE (20 ns per hop, including mixed paths):

| Profile | Worst-length case covered with C_i=50 ns each | D ns | Remaining admission ns | Worst decoder margin ns | Shortest final HIGH / LOW ns |
|---|---|---:|---:|---:|---:|
| 800 | 3 boards, 3 x 100 m (therefore typical 30+3+3 m too) | 218 | 2 | 29.05 | 84.5 / 74.5 |
| 600 | 4 boards, 4 x 100 m | 292 | 8 | 67.52 | 114.67 / 104.67 |
| 400 | 6 boards, 6 x 100 m | 440 | 10 | 117.86 | 175 / 165 |
| 200 | 8 boards, 8 x 100 m | 588 | 162 | 567.36 | 652 / 642 |

EXTRA (7.5 ns per hop, all stages qualified):

| Profile | Boards covered, C_i=50 ns each | D ns | Remaining admission ns | Worst decoder margin ns | Shortest final HIGH / LOW ns |
|---|---:|---:|---:|---:|---:|
| 800 | 3 | 180.5 | 39.5 | 66.55 | 122.00 / 112.00 |
| 600 | 4 | 242 | 58 | 117.52 | 164.67 / 154.67 |
| 400 | 7 | 426.5 | 23.5 | 131.36 | 188.50 / 178.50 |
| 200 | 8 | 488 | 262 | 667.36 | 752 / 742 |

qualified-path does not cover 4 boards at 800 (242>220), 5 at 600 (303.5>300 by
3.5 ns), or 8 at 400 (488>450). At the GUARANTEE board counts, qualified-path gains
12.5*N = 37.5/50/75/100 ns decoder margin. At 6 boards / 400 its D=365 ns,
remaining admission=85 ns, margin=192.86 ns, shortest HIGH/LOW=250/240 ns.

For independently certified lower C_i, additional hops can use a faster step,
subject to the same margin, forwarding and <=8-board checks. For eight boards, the
maximum total cable allowance per step is GUARANTEE 32 / 112 / 262 / 562 ns or
qualified-path 132 / 212 / 362 / 662 ns. These are admission-derived totals;
each C_i<=50 also caps either total at 400 ns. Thus all eight worst-length
segments require 200 in either column; 400 does not cover their worst case.
At 200, a wider 1000 ns decoder envelope still leaves
>155 ns HIGH-class margin and 230 ns LOW, while admission is capped at 750 ns:
25% reserve to that envelope and 412 ns unused at the exact 588 ns path.

Arithmetic: for threshold k, sampler f MHz, raw HSI +/-2.2%,
`Tmin=1000*k/(f*1.022)`, `Tmax=1000*k/(f*.978)`, `Q=1000/(f*.978)`.
`M0=Tmin-(H0+10+D)-Q`; `M1=(H1-10-D)-Tmax-Q`.
The table is min(M0,M1) over 36/50 MHz; HIGH minimum = H0-10-D and
LOW minimum = period-H1-20-D. Observed reports additionally subtract their
sample-to-host scale uncertainty. The 220 ns admission ceiling leaves >27 ns
ideal decoder margin; 20 ns is the early-warning threshold, not zero.

**Typical installation stays at 800.** Two boards (30+3 m) have
`20*2 hops + 4 forwarding + 2*50 cable = 144 ns`;
three boards (30+3+3 m) have `20*3 + 4*2 + 150 = 218 ns`.
We deliberately charge the full 100 m cable/protection allowance to EACH short
segment, not a guessed length-scaled pulse skew. V203's three-board threshold
is 597.956..624.858 ns, Q=28.403 ns; received zero max=540.5, one min=709.5,
giving M0=29.053 and M1=56.239 ns. V407's smaller margin is 45.704 ns.
qualified-path gives D(2)=119 ns and D(3)=7.5*3+4*2+150=180.5 ns; three-board
V203 M0=66.553, M1=93.739 ns; V407's smaller margin is 83.204 ns.
At 200 ppm scaling, outward-rounded reports retain >=26 ns; no warning/downshift
is required. This proves the conditional budget using GUARANTEE worst-case skew, not
unconditional performance of unqualified 3.3 V parts or arbitrary cable.

Forwarding resumption stays <=100 ns after the removed slot's 256th falling edge,
using timer/DMA, not ISR latency. Check the LOW at every board which has a
downstream device, not the final receiver's smaller LOW. In the typical chain
the last active forwarder is B: its D=144 ns leaves 148.5 ns LOW, hence 48.5 ns
forwarding reserve. A and B remove their own slots; C's output has no load. For the
4/6/8-board cases above the last active forwarder leaves 178.67/239/716 ns LOW.
qualified-path D(2)=119 ns leaves 173.5 ns LOW, hence 73.5 ns forwarding reserve in the
typical chain. At the same 4/6/8 board counts its intermediate LOW is
216.17/301.5/803.5 ns; at its 7-board / 400 coverage count, LOW=240 ns and
forwarding reserve=140 ns.
Admission must reject any other topology/backend whose intermediate LOW is
<=100 ns even if its final decoder margin passes. The RMT40 typical case still
leaves 36 ns forwarding reserve. Slot times are 320 / 426.667 / 640 / 1280 us.

Cable propagation is different from pulse distortion. At 538 ns/100 m, the
typical three segments add 193.68 ns; with 3*300 ns qualified electronics delay,
the forward delay is <=1093.68 ns; two-way allowance is 2.18736 us. Eight 100 m
segments give `2*8*(300+538)=13.408 us`, within the 15 us return-path allowance.
The host uses the qualified 300 ns ULTRA_HOP_DELAY_NS stage bound.
Complete assemblies still need qualification over their intended conditions;
typical propagation measurements alone do not establish this bound.
Qualified-path enable/disable <=70 ns is far below the us-scale
return-window budgets.

Return UART rate is independent of the forward profile. Eight-hop defaults to
qualified **250 kbaud**: conservatively keep round 3's 592 ns plus 8*15 ns relay
allowance, 1.5% accumulated baud error over 9.5 bits and one 1/16-bit sample:
`592+120+(570+250)/.985 = 1544.488 ns <2000 ns`, >455 ns spare. 500 k/1 Mbaud
need their own return-eye qualification. Never confuse 200 kHz forward with
250 kbaud return or infer one eye from the other.

## Dig2Analog-Plus-5ch pin link model

A [Dig2Analog-Plus-5ch](https://github.com/intermittech/quinled-ultra-firmware/wiki/Board-Dig2Analog-Plus-5ch)
data pin is a single-wire chain of up to **50 boards**. It is budgeted here,
separately from the Diff-Power port above, whose eight-receiver budget is
unchanged. Each board passes the line on through its forwarding gate without
re-timing, so width errors add up per hop. The boards are V203 (36 MHz
sampling); the profile table's V203 thresholds and admission column apply.

**Per hop.** The gate's rising and falling propagation delays are each
specified as 1..10 ns over its supply/temperature range at its test load. A
HIGH pulse changes width by their difference, so one hop is bounded by
**+/-9 ns**. This bound is derived from the two delay limits, not a specified
skew; equal typical delays do not prove zero skew. A bench measurement of the
real gate skew may tighten it.

**Path.** Board N sees N-1 upstream gates: `D(N) = 9*(N-1) + E_N` ns.
`E_N >= 0` bounds every other width error up to board N's sampler: all N cable,
connector and input-protection segments, the input threshold crossing and the
MCU input. All signs accumulate. No Diff-Power hop or 4 ns forwarding allowance
applies. The sender residual (+/-10 ns HIGH and period) is charged as above.

**Wiring assumption.** No D2A cable or input bound is qualified yet. The
ceilings below assume ideal wiring, `E_N = 0`; the reserve column is the `E_N`
each choice still tolerates. These are conditional numbers, not an
installation certificate.

A pin admits profile p for N boards only if all of these hold:

1. Final width path: `D(N) <= admission_p` (220 / 300 / 450 / 750 ns). As on a
   Diff-Power port, this keeps every V203 decoder margin positive for every
   sender backend.
2. Forwarding LOW: each forwarding board must reopen its gate (<=100 ns after
   its removed slot) before the next rising edge. The worst is board N-1:
   `LOW_p - 20 - D(N-1) > 100` ns, with `LOW_p` the shortest period minus one
   HIGH of the sender backends: 300 (RMT 40 MHz) / 412.5 (RMT 80 MHz) / 625 /
   1250 ns. PARLIO gives 312.5 / 416.667 / 625 / 1250 ns.
3. Inventory: the pin holds only Ultra boards whose applications and
   bootloaders support p; otherwise it stays at 800 (`ultra_line_allowed`).

With `E_N = 0`, rule 1 gives `N-1 <= 220/9, 300/9, 450/9, 750/9` and rule 2
gives `9*(N-2) < 180, 292.5, 505, 1130`:

| Profile | Rule 1 N <= | Rule 2 N <= | Model ceiling | Pin table | E_N reserve at table edge | Smallest decoder margin there |
|---|---:|---:|---:|---|---|---:|
| 800 | 25 | 21 | 21 | 1..20 | <18 ns (`118 - E > 100`) | 76.053 ns |
| 600 | 34 | 34 | 34 | 21..34 | 3 ns (`297 + E <= 300`) | 54.992 ns |
| 400 | 51 | 58 | 51, capped 50 | not picked | 144 / 99 / 9 ns at 35 / 40 / 50 | 117.479 ns at 50 |
| 200 | 84 | 127 | 84, capped 50 | 35..50 | 309 ns (`441 + E <= 750`) | 714.360 ns |

At the model ceilings the reserve is 9 ns (800 at 21) and 3 ns (600 at 34).
Margins use the V203 `M0`/`M1` formulas above over PARLIO and RMT 40/80 MHz.
Update traffic through a bootloader holding its gate open removes no slots, so
only rule 1 binds it (25 boards at 800); the pin table covers both.

**Pin table** (decided 2026-10-23): default 800 kHz. The hub uses the fastest
profile whose range holds the configured board count: **1..20 boards 800 kHz,
21..34 600 kHz, 35..50 200 kHz**; more than 50 is refused. 400 kHz is not picked
automatically. 800 stops at 20, one hop below its model ceiling, to keep wiring
reserve. A one-way pin has no measured margins: it never steps on its own, and
a manual override may only pick a slower profile than the table. Bench items:
the real gate skew, `E_N` for qualified wiring, and the <=100 ns gate reopening.

## Regeneration, cost and physical output

[Diff-Power Ultra Receiver Analog-Medium](https://github.com/intermittech/quinled-ultra-firmware/wiki/Board-Diff-Power-UR-Analog-M) and Diff-Power-UR-Digital-M chain outputs use transparent
forwarding without arbitrary frame regeneration. PA1 is FWD_EN, PA10 is DE_OUT,
and PA9 is the upstream transmit pin. The digital receiver regenerates decoded
pixel bytes on its separate PA7 strip output.

Full-frame regeneration adds one input-frame acquisition plus output duration;
packet counts/latency are in transport.md. It never adds this latency to command
forwarding. V203 analog retains hardware forwarding, no frame TX buffer and no
per-bit transmit ISR; rate detection and threshold tables need a fresh code-size measurement.
Statistics and coherent snapshots provision <=128 B state per ingress;
accumulate in the existing sampled decode loop, not an edge interrupt. Digital
packet assembly/reusing the existing encoder is estimated +200..600 B code and
16..32 B transaction state, reusing its existing 2000 B frame pool; an optional
second buffer costs another frame and is not required. At 144 MHz the four profiles leave
180 / 240 / 360 / 720 CPU cycles per bit, but SPI DMA still moves 4.5 MB/s if
sampling continuously. No CPU percentage or flash fit is claimed without M1
measurement under effects/ADC/DMA load and the 8 KiB bootloader size check.

For the local WS281x encoder, select timing per advertised chip; there is no
single waveform valid for all WS281x variants. The local Worldsemi WS2915 PDF,
printed page 4 (visually checked), requires T0H 220..380 ns, T1H 520..1000 ns,
both LOWs 580..1000 ns, bit period >=1250 ns and reset >=280 us. An uncorrected
45-sample/36 MHz period can be too short at fast HSI; do not replay that grid.
One compliant encoder recipe for this chip uses the measured sampling frequency
`f` in MHz, accurate within +/-200 ppm: `period=ceil(1.28*f)` samples,
`zero=round(.34*f)`, `one=round(.65*f)`. LOW = period-HIGH. Change the table only
between frames. This changes encoded sample counts, not the CPU clock. Before
rate measurement is valid, blank this output or use a separately qualified
raw-clock table; never assume a scheduling lock physically trims HSI.

With actual HSI anywhere in +/-2.2%, residual estimate error +/-200 ppm and a
**qualified total strip-output width/period error <=12 ns**, extrema are:

| Sampler | T0H ns | T1H ns | T0L ns | T1L ns | Minimum bit ns |
|---|---:|---:|---:|---:|---:|
| V203 | 314.3..365.7 | 624.0..676.0 | 933.8..992.7 | 614.2..680.0 | 1267.7 |
| V407 | 320.6..359.8 | 627.8..672.2 | 922.4..977.2 | 617.7..662.3 | 1267.7 |

The vectors evaluate every rounding/ceiling discontinuity and both sides of
each frequency interval, not just nominal frequencies. Use >=500 us physical
LOW for reset. For this adaptive recipe advertise bit_ns=1310 (a conservative
reference, not the average period): 1310*1.023=1340.13 ns bounds the maximum
1320.09 ns synthesized period including output uncertainty. The 12 ns output allowance is a complete output-path qualification requirement. Other advertised chip presets require their own
HIGH/LOW/period/reset checks. These are regenerated near-800-kHz pixel pulses;
upstream accumulated distortion is absent from the output budget.

## Rate discovery, late join and recovery

Choose **line-rate detection**, not a mandatory announcement at 800. Every app
and immutable bootloader supporting fallback must detect all advertised rates
before interpreting HIGH widths. Measure rising-edge spacing over 256 intervals
within a continuous frame; classify the mean period against the four nominal
periods with nonoverlapping +/-8% acquisition bands. Raw HSI +/-2.2%, backend
rounding and endpoint delay variation must fit those bands. With the qualified
6704 ns one-way bound, even a conservative two-endpoint error of 13408/256 =
52.375 ns plus worst HSI and sample error fits the 800 band's 100 ns allowance.
Require plausible HIGH/LOW runs and three successive valid CRC trailers at the
same rate before executing commands. Missing edges, ambiguity or loss of valid
headers for 2 s returns to acquisition with outputs held to loss policy.
Bootloaders recognize normal DARK-preamble trailer CRCs for acquisition only,
without executing app slots; after lock they retain the existing whole-update
frame forwarding rules. New nodes beyond the configured slot count require
inventory/slot-count expansion and a fresh preamble before normal operation.

Acquisition does not consume extra wire bits: each board retains the existing
edge-counted slot removal, independent of decoded bit values. At the last board
a minimum normal frame still has 256+64 bits. Discard partially acquired frames;
the repeated DARK preamble supplies subsequent complete ones. With slot removal
active even while acquiring, every downstream device can find the new rate.
Repeat the existing 4*N+2 DARK preamble after a switch, then acquire the existing
one-second host scheduling lock before any return transmission. Switch only
after any return window/flash-busy deadline, then >=500 us LOW. Reset the
statistics epoch, framing and timing lock; retain configuration and inventory.

Start at 800. If that fails on a previously inventoried all-Ultra port, send the
same DARK preamble at 200, discover replies there, then probe faster steps. This
requires no decodable 800 announcement. Late joiners listen to the present rate
and use subsequent frames identically; send a DARK preamble at least once after
inventory changes or rejoin detection. For unknown/new installations inventory
must first be supplied by commissioning/jig or learned at 800; never send slow
discovery blindly through potential real pixels. New physical attachments
invalidate inventory authorization until checked. A truly dead/broken path is
not repaired by baud fallback. Recovery at 200 is mandatory for fallback-capable
apps AND bootloaders; update sessions hold their selected profile fixed.

## Per-hop statistics, port policy and commissioning / HA

LINK_STATS page 0x13 uses arg0=ingress, arg1=0 snapshot / 1 snapshot-and-clear,
arg2=part (0 widths, 1 context), remaining args zero. Part 0 increments the
snapshot epoch and atomically freezes both parts with that epoch. Part 1 reads
that frozen snapshot without clearing; arg1 must be 0. Clear only data captured
in a read-and-clear snapshot; keep the frozen pair until the next part-0 request.
A profile change increments the epoch and invalidates the frozen pair. Parts
from different epochs/boots must not be combined; reject snapshots older than
2 s (including across u16 wrap), and serialize snapshot requests per receiver.
Unsupported/non-sampling ports answer UNSUPPORTED; part 1 before a snapshot
returns flags=0/profile=255, never a fabricated valid measurement.

Part 0 DATA retains its 24 bytes: port:u8 at 0, PHY:u8 at 1, flags at 2
(VALID=1, scheduling LOCKED=2, OVERFLOW=4, WARNING=8, RATE_REQUEST=0x10), ceil quantum ns:u8 at 3;
u16 big-endian zero min/max/mean at 4/6/8, one min/max/mean at 10/12/14,
zero/one counts at 16/18, invalid runs at 20, epoch at 22. Class counts saturate
at 65535, freeze their extrema/sum and set overflow; u32 sums suffice even at
the 200 profile. Means round nearest; min floors/max ceils to ns.

Part 1 DATA: port:u8 at 0, profile ID:u8 at 1, same flags at 2, supported mask
at 3; u16 epoch at 4, period min/max at 6/8, inter-bit LOW min/max at 10/12,
CRC-valid frames at 14, CRC-bad frames at 16, missing expected training frames
at 18, sample-to-host uncertainty ppm at 20; 22..23 zero. Byte 18 counts the
training patterns (55 / AA / 00 / FF) absent from each completed one-second
window; a window that never completed leaves VALID clear. Reset/marker LOWs do
not enter inter-bit LOW statistics. All counters saturate and flag overflow.
Invalid profile is 255. VALID requires both classes, >=256 samples of each,
known scaling <=200 ppm, a complete training window and valid profile; stale,
unsupported and absent reports are unavailable, never a perfect zero-error eye.

RATE_REQUEST (DATA byte 2 bit 0x10 in both parts) means "this link may need
a lower rate"; it does not diagnose a cable or change the active profile.
Receivers detect the line rate once per acquisition epoch and never switch
rates on their own. A bad one-second window contains at least four invalid
frame CRCs (failed header/slot validations, including an undecodable header).
A bad slot CRC counts even with a valid trailer; the intentional all-zero
DARK preamble does not count as a bad slot. Three consecutive bad windows
latch RATE_REQUEST; isolated failures and two bad windows do not. A missing
entire window breaks the streak. Delayed calls preserve the original window
boundaries, including across millisecond-counter wrap. Only a new acquisition
epoch clears the latch; clearing reported frame/error counters does not.
Both parts of a frozen snapshot carry the same flag, even if the live latch
changes afterward. Existing return-window qualification still applies:
a header-only tentative lock never answers this or any poll.

The controller reads both parts, checks their common epoch, freshness and
advertised profile support, then decides per port whether to retrain the
whole affected chain coherently at a supported lower rate. RATE_REQUEST
does not itself authorize a switch. Never step up automatically; faster-rate
qualification requires an explicit commissioning decision.

One-second windows include CRC-valid NOP slots whose bytes 3..29 are repeated
55, AA, 00 or FF; TARGET, SEQ and flags remain zero. NOP payload is ignored,
not reserved-zero state. Training requires complete Ultra-only inventory.
During profile acquisition, send a fresh DARK preamble and hold queued controls,
pixel transactions and updates until scheduling lock and fresh positive eye
measurements are established. Explicit commissioning probes require 60 clean windows.
Use CRC-valid traffic for labelled widths; separately record bad CRCs, invalid
runs and expected training-frame loss. A jig compares expected symbols even on
bad CRC to expose survivor bias. The report is **cumulative at each hop** (UID,
position, ingress); only matched-epoch/data means can estimate isolated-hop
change, with uncertainty. Subtracting extrema is not a certified hop bound.

For each receiver calculate signed margin
`min(Tmin-zero_max, one_min-Tmax, min_HIGH, min_LOW) - Q - scale_error`.
Threshold bounds come from its MCU TIMING record; scale_error is ceil(maximum
reported duration * ppm / 1e6). Hub port margin is the minimum of all required
receiver margins, including any externally measured one-way endpoints. Use
actual RMT widths and check intermediate gate LOW reserve as well. Never
extrapolate a live margin at a slow rate into proof of a faster rate.

AUTO starts at 800 and uses the fastest qualified, inventoried profile. Warn
immediately below 20 ns or on any CRC/invalid/missing frame. Three consecutive
bad one-second windows request one slower step. Nonpositive margin or repeated
frame loss stops pixel traffic immediately and requests recovery at 200 when
inventory permits; reacquire before normal traffic. A missing report raises
UNKNOWN/STALE and cannot by itself authorize a change. Downshift may bypass the
dwell for protection. An explicitly requested commissioning upshift needs >=60 s dwell plus 60 consecutive
clean one-second **candidate-rate probe** windows with every margin >=60 ns;
abort a bad probe to the last good rate, reset the good counter, and retain the
warning. Probe during commissioning/idle, holding last output/local effects;
never interrupt a live packet transaction or update. Current-rate good windows
do not count as faster-profile probes. Normal typical ports already at 800 stay
there at their >=26 ns worst-case observed margin; an installation operating
at that exact corner may conservatively remain slow after a previous failure.

Manual AUTO/800/600/400/200 override is per port and persists in hub settings.
Manual bypasses ranking/hysteresis, never inventory, common support, certificates,
positive measured margins or gate checks. Refused overrides report a reason.
No-return board 1 always forwards but cannot report or relay stats: inventory
comes from its firmware artifact/jig, telemetry is UNKNOWN unless externally
measured. Automatic margin-based fallback cannot cover such missing hops; its
pin takes the profile of the [pin table](#dig2analog-plus-5ch-pin-link-model).
Do not invent a return path or infer a non-Ultra device's identity from silence.

Commissioning and HA expose per port: configured mode, active/actual kHz,
profile ID, common supported mask, inventory completeness/800-only reason,
worst margin ns and UID/hop, forwarding reserve, last sample time/age/epoch,
per-hop zero/one/LOW/period statistics, CRC/missing counters, qualified versus
observed status, and effective FPS. States: OK, WARNING, RECOVERING, UNKNOWN,
FAILED; latch last reason/time and retain it across recovery. Warn before zero
margin; do not hide a missing downstream node in the minimum. Show estimated
hop deltas separately. A profile change and its reason are commissioning events.

HA uses a per-port profile sensor (for example `800 kHz`, plus actual-rate
attribute), a numeric margin sensor in ns, a warning binary sensor, and an
AUTO/800/600/400/200 select for the hub override. Unknown/stale margin is
unavailable, never 0 ns; warning remains set with its reason. A synthetic
commissioning display example (not a hardware measurement):

| Port | Mode / active | Minimum margin | Limiting hop | Status | Qualification |
|---|---|---:|---|---|---|
| 0 | AUTO / 800 kHz | 26 ns | C, position 2 | OK | Pending PVT/cable proof |
| 1 | AUTO / 800 kHz | Unavailable | Board 1, no return | UNKNOWN | Needs jig report |

The first row uses outward-rounded typical-chain arithmetic. Display a measured
margin only when a fresh valid snapshot exists, separately from this predicted
margin. Profile selects persist in hub port settings, outside per-board S2.

Optional pre-compensation fits the same residual +/-10 ns launch allowance
and must improve the intersection of every receiver's safe interval. It never
overrides guaranteed limits, pixel chip timing, or the common profile mask.

## Header TIME and shared starts

Header TIME is the host's microsecond clock modulo `ULTRA_TIME_WRAP` =
2^22 x 1000 us (4,194,304,000 us, 69.9 min) at the frame's last falling edge;
a header with TIME at or above the wrap is invalid. Because the wrap is a whole
number of milliseconds, a receiver that unwraps TIME around its own estimate
(within +/-34.95 min, else it resynchronizes) has floor(TIME / 1000) on the
host's millisecond grid, whatever host epoch it first synchronized in.

SET and PIXEL_FX START count from that host millisecond. The host rounds an
absolute start to its millisecond once (half up) and writes START = that
millisecond - floor(frame TIME / 1000), clamped by the field's range; a
`floor` (streamed) SET never starts before its frame's millisecond. Every item
of one start, in any frame, repeat or STREAM, then names one host millisecond
on every board: boards switch on the same receiver tick, within their clock
lock and tick, and at most 500 us from the requested start plus those terms.

## Return windows on an Ultra chain

`t0` is the last falling edge of the frame trailer at each participating board,
not ISR entry or shared-clock phase. The host trailer TIME denotes that frame
end in its own clock; each port bounds its local edge estimate to ±2 µs. A valid
normal POLL uses TARGET 255; all POLLs in a frame agree on R and rate. Answer
indices are unique and increase down the chain; intermediate boards relay only.

| Event | Host µs from local t0 / nominal slot start |
|---|---|
| Host holds LOW and releases | LOW ≥60; stopped driving by 150 (target 80) |
| Slot plan re-arm (every board) | after ≥200 quiet LOW (`ULTRA_RW_REARM_US`), never in a released HIGH |
| Receiver direction change | 250..900 (target 500) |
| Turnaround check HIGH start | 920..977; HIGH duration ≤5 |
| First answer slot | 1000 |
| Answer pitch | `(335 << rate) + 165`: 500 / 835 / 1505 |
| Burst enable relative to slot | −20..+80; first slot −5..+80 |
| First UART start bit after enable | 2..10 |
| End of burst and LOW | by slot start + pitch −30, including propagation budget |
| Return-window end | `W = 1000 + R*pitch`, 1≤R≤16 |
| Restore forwarding | W..W+80; never before W |
| Host's next frame | no earlier than W+200 **and** after the required processing LOW |

Every board holds its own slot and forwards the rest (the ordinary slot plan:
apps, and bootloaders in recovery). It re-arms that plan only after 200 µs of
LOW with no edge after a frame. A re-arm before the host's release (by 150 µs)
would hold the released HIGH from the boards behind: at their turnaround they
would see the line still driven and stay quiet. 200 µs is past the release and
before both the 250 µs late-release reject and the 280 µs reset. In a released
HIGH the gate keeps its level; the plan re-arms in the next 200 µs quiet LOW.

The burst contains exactly one `0xFF` lead byte and a 32-byte answer, UART 8N1,
330 bit times total. Baud rate is 1 M/500 k/250 kbaud. Total baud error relative
to host is at most ±1.5%, including integer BRR quantization. Total inter-byte
gaps ≤8 µs at **every** rate. DE falls only after UART TC, within 5 µs of the last
stop bit; DMA completion or TXE does not establish that the wire is empty.

The scheduling and UART budgets are separate. With the largest window
`W=1000+16*1505=25080 µs`, ±200 ppm contributes at most 5.016 µs.

| Per-port start component | Maximum µs |
|---|---:|
| 20 kHz tick quantization | 50 |
| Burst ISR delay / non-preemptible interference | 15 |
| Local t0 estimate | 2 |
| Locked-rate drift over largest W | 5.016 |
| UART/GPIO arm latency | 3 |
| Total | **75.016 ≤80**, leaving 4.984 |

The check and restore have strict lower bounds as well as deadlines. Schedule
using the latest possible physical time, accounting for the **full interval**
of edge/rate uncertainty, rather than clamping a nominal timestamp after the
fact. Their separate budgets are:

| Operation | Tick | ISR entry/interference | Edge interval | Rate interval | GPIO/arm | Total / allowance |
|---|---:|---:|---:|---:|---:|---:|
| Check HIGH at 920..977 | 50 | 2 | 4 | 0.40 | 0.50 | **56.90 / 57 us** |
| Restore at W..W+80 | 50 | 10 | 4 | 10.04 | 3 | **77.04 / 80 us** |

The rounded rate intervals cover both signs of 200 ppm over 977 and 25080 us,
including conservative rate-to-period conversion. A port with the burst path's
15 us ISR delay in either of these paths does not qualify; it must schedule
differently or decline. The check-path **entry/interference latency** MUST be
<=2 us under worst load: from the scheduled service instant to ISR entry,
including interrupt masking and higher-priority/non-preemptible interference.
This is the latency component in the check-start budget, not the intentional
HIGH pulse width or a limit on the ISR's complete pulse-holding runtime.
Separately, the intentional check HIGH, from physical assertion to physical
deassertion including GPIO/deassert latency and any intervening interruption,
MUST be <=5 us. M1 must measure both bounds under worst load (only 0.10 us
check-start budget spare). If either bound fails, re-derive the check window
before 1.0; do not qualify the port using the 15 us burst-path allowance. At target turnaround 500, `50+15+2+0.1+3=70.1 us`
remains within 250..900. A host aiming release at 80 with +/-2 us edge error and
up to 50 us ISR delay releases in 78..132, within 60..150.

For rate r, the latest wire completion budget is

`80 late +10 lead +330*2^r/0.985 UART +8 gaps +5 DE +15 skew`.

| Rate | Pitch | Latest complete (µs) | Deadline pitch−30 | Spare |
|---|---:|---:|---:|---:|
| 1 Mbaud | 500 | 453.025381 | 470 | 16.974619 |
| 500 kbaud | 835 | 788.050761 | 805 | 16.949239 |
| 250 kbaud | 1505 | 1458.101523 | 1475 | 16.898477 |

“17 µs spare” is rounded; the common guaranteed spare is **16.89 µs**. The
30 µs end guard also covers the next slot's 20 µs permitted early start, leaving
at least 10 µs LOW between compliant bursts after the accounted skew. The maximum
windows are 9000, 14360 and 25080 µs. The 200 us holdoff is only one lower bound:
forwarding may resume as late as W+80, after which each receiver needs its full
processing LOW. For a path gap G, the conservative next-frame time is
`W + max(200, 80 + max(500,G))`. With G=500 this is **W+580 us**, giving
9580 / 14940 / 25660 us for 16 slots. Every sender, hub and decoder derives
deadlines from these constants.

## Rate lock, decline and digital ports

No receiver transmits before at least 1000 ms of valid headers after preamble and
a measured host-relative scheduling rate within ±200 ppm. It tracks rate using
wrap-safe elapsed-time arithmetic. After missing valid timing observations for
2 s, a rate discontinuity, or reset, it is unlocked and must reacquire. UART BRR
uses the measured rate and actual peripheral clock. An implementation cannot
claim rate lock solely because a one-second timer expired.

An unsupported rate/profile produces silence when it cannot safely answer;
UNSUPPORTED is permitted in a later supported window. Unlocked, late, flash-busy
or contended ports suppress the entire burst, never transmit a partial answer
or stretch a slot, and latch a decline reason for DIAG part 2. Contention aborts
both answering and relaying for that window. When local transmission has no receive echo: LINK
clock flag NO_ECHO declares this. Observe released HIGH before enabling; do
not sample disabled RO for an own-burst collision verdict. Keep scheduled check
pulses/guards, but report contention only when actually observable while
listening. Single-master exclusion is required; no fault-contention detection
guarantee is implied for these boards. A later valid supported poll returns
latched reasons; reading DIAG part 2 clears only the reported snapshot. The host
retries with a fresh sequence and never treats silence as update success.

| Marker-mode event | Host-time bound |
|---|---|
| Pre-marker quiet / post-command LOW | ≥500 µs, increased by LINK.min_gap_us if needed |
| Marker HIGH | 15..40 µs (target 20) |
| Marker-to-frame LOW | 5..20 µs (target 10) |
| Host release | by frame-end+150 µs; a start after +250 is rejected as RW |
| Released before receiver drives | ≥20 µs |
| Receiver enables answer | by frame-end+900 µs |
| Listen deadline | frame-end+900+pitch = 1400 / 1735 / 2405 µs |
| Pixel restart after host retake LOW | ≥20 µs and complete output-frame/latch handback |
| Initial marker delay | ≥1000 ms after power-up/start of stream |
| Retry/backoff | 3 quick attempts, 3 more after ≥60 s, then user intervention |
| Update entry LOW | Whole-port entry bound + forwarding blackout, at least 50 ms; see below |

Marker mode has no turnaround check, one receiver and no contention readback.
Its answer fits before `900 +10+330*2^r/0.985+8+5+15`, below `900+pitch−30`.
No pixels are sent during the command, reply, or flash-busy interval. Every
marked update frame retains this envelope; blind mode does not need a reply.
Keep pixels suppressed through commit/restart and the update quiet-close wait,
then use complete output-frame/latch handback.

Local pixel bit time is per output. Use worst-case duration
`ceil(frame_bytes*8*bit_ns*1.023/1000) + delay_us + latch_us` in host µs,
then at least 20 µs retake. The 2.3% allowance includes ±2.2% raw HSI frequency
error and integer-ns declaration rounding. Use the longest active output, and
respect shared resource ceilings. Example V407 6000 B/1280 ns output:
`ceil(6000*8*1280*1.023/1000)+336+500 = 63690 µs`; two parallel outputs require
the same maximum duration, not half the duration. Render/buffer admission still
counts both outputs' bytes. The V203/V407 example replay bounds are 233/336 µs.

## Update forwarding and entry

Every application and bootloader accepts blind declared-profile updates on
CHAIN lines and marker ports. CHAIN forwards complete, unmarked 384-bit update
frames without consuming command slots or replacing them with local pixel data.
A node with a downstream receiver MUST preserve those frames even when the image
does not match its own immutable target. Local update acceptance and a return
reply path are separate abilities; marker transport still has one receiver.

An application acquires UPD transparent mode after two consecutive valid UPD
headers. Every group transfer starts with >=1000 ms of valid UPD PING headers
using the subtransfer's nonzero session, extended until every hop has acquired
transparency, before the application-entry ANNOUNCE pair. Session-zero idle
confirmation is forbidden in this preamble.

A receiving application captures one frame at a time: a frame whose first edge
comes before the application has handled the previous frame is dropped (measured:
97 % of the frames one 500 us port gap behind a captured frame). So
every frame a decision rests on, SELECT, each ANNOUNCE half and each blind END copy, starts at
least **10 ms** plus the port processing gap after the end of the frame before it
(`ULTRA_UPDATE_CONTROL_PACE_US`), as the training PINGs are spaced, the last PING
of a train included. DATA keeps its port-gap spacing: a bootloader decodes every frame
from its ring, not through the one-frame capture, and a transit application
forwards DATA without acting on it. Every forward gate, application
and bootloader on V203 and V407 alike, opens only inside a quiet inter-frame
LOW after at least **350 us** of LOW with no edge, never partway through a frame.
The host's inter-frame LOW is `port_gap_us = max(500, all affected LINK.min_gap_us)`,
so a gate opens inside every gap, the PING preamble's included; 350 us is the
marker quiet rule, above the 280 us reset recognition and the 250 us late-release
limit. While transparent, forward the complete first ordinary frame and close
only on its decoded trailer, or close on idle timeout at a quiet boundary.
The transparent idle-close time is **2000 ms** after the last valid UPD header,
held off through any reserved entry/erase/program/commit interval.

After a valid ANNOUNCE of any target, foreign or own, a forwarder holds
transparency from that frame's physical end for that ANNOUNCE's transit hold,
never a fixed constant. The descriptor declares no erase bound, so the
forwarder counts the image's erase units exactly from INDEX 0 (layout byte 11,
body length, load) and the [layout table](update.md#geometry-and-records), one
metadata unit plus `ceil(body_length/erase_unit)`, and assumes the largest u8
`erase_max_ms`:
`transit_hold_ms = erase_units*2*255 + 100`. It is never shorter than the
declared lead `erase_units*2*erase_max_ms + 100` the host derives for any
nonzero u8 bound; the host refuses a subtransfer whose lead exceeds it. An
INDEX 1 half restarts the hold computed from the last INDEX 0 of the same
session. Any other ANNOUNCE, or an INDEX 0 that fails a forwarder check, holds
the largest valid value, **114340 ms** (full layout-1 slot, 224 units). The
forwarder checks are every descriptor rule INDEX 0 can show for any target:
magic, descriptor version 2, flags zero, `rev_min <= rev_max`, board 1..254,
ABI 3, no required feature bits outside the defined mask, layout 1 or 4,
load = that layout's body start, length nonzero, a multiple of 4 and inside
that layout's body. The target match (tuple, variant, feature subset, revision
in range, minimum bootloader) needs the receiving board, and entry, reserved
bytes and the descriptor CRC lie in INDEX 1; neither is a forwarder check. `upd_transit_hold_ms()` in
`ultra_update.h` is the reference. Examples: a 62 KiB layout-4 body, 17 units,
**8770 ms**; the full layout-4 slot, 124 units, **63340 ms**, which covers that
slot's legal declared lead at `erase_max_ms=200`, `124*2*200+100 =` **49700 ms**
(**24900 ms** at the example 100 ms bound); the full layout-1 slot **114340 ms**.
DATA, END or ABORT of that session ends the hold at once; otherwise the 2000 ms
idle close counts from the hold's end. The hold and that idle close also cover
the port processing gap before DATA. A two-second idle timer therefore cannot
break a long erase wait. Do not send keepalives through a busy interval to hold
the gate open.

A bootloader keeps update forwarding available throughout its stay, including
recovery, nonmatching sessions and failure waits, after a bounded entry/reset
blackout. Reset closes its gate (downstream sees LOW); it opens at the first
quiet LOW under the 350 us rule above. The blackout runs from the hardware reset
that leaves the application, not from the start of bootloader code, to that
earliest opening; it is the trusted `blackout_ms` bound below. It cannot
require selection, a local target match or status polls to enable downstream
delivery. Flash may prevent local capture only during the
declared or acknowledged busy interval; forwarding state stays stable and the
next legal frame is received/forwarded intact. Do not claim return relay from
this forward-only guarantee.

The host inventories entry/reboot and blackout bounds for the **whole port**,
including nonmatching transit nodes. The immutable `entry_10ms` bound is at
least 50 ms; any additional forwarding blackout has a trusted firmware/jig
bound, not a guessed delay. Conservatively use
`entry_wait_us = 1000*(max(50, max_i(10*entry_10ms_i)) + max_i(blackout_ms_i)) + port_gap_us`:
the bounds plus the port processing gap, never the larger of them. Measure
from the physical end of the entry ANNOUNCE pair, with the full wait LOW, then
send valid nonzero-session UPD PING headers for >=1000 ms before the
boot ANNOUNCE pair. Reset clears acquisition/selection; reselect only in
selected mode. Replies require measured host-relative lock within +/-200 ppm,
not just elapsed training time. The same rule, bound plus `port_gap_us` from
the physical end of the frame that starts the wait, applies to the whole-port
reboot wait after END, the commit wait, each selected acknowledgement PING and
the confirmation write after a session-zero PING. A blind transfer starts the reboot
wait at the last of its three END copies (each copy is followed by the port processing
LOW and the 10 ms decision-frame pace); its six session-zero confirmation PING copies each carry the confirmation-write
pause.

After the last UPD frame, the host MUST hold quiet LOW for >=2100 ms (the 2000 ms
idle close plus receiver tick granularity) and until
all outstanding busy reservations expire before ordinary traffic. This applies
after END, ABORT and idle confirmation PING, before each fresh normal acquisition.
It prevents the first ordinary frame from losing its slot-removal boundary.
A transfer abandoned after an ANNOUNCE without DATA or END leaves that
ANNOUNCE's transit hold plus the idle close. The host does not count on its
ABORT reaching every hop: it holds quiet LOW until `transit_hold_ms + 2100` ms
after the last ANNOUNCE's physical end (the same `upd_transit_hold_ms()`
value), and at least 2100 ms after the ABORT. Examples: abandoned after the
boot pair of a full layout-4 slot **65440 ms**; a full layout-1 slot or an
unparseable descriptor **116440 ms**.
[The port job](update.md#port-job) defines acquisition, training and confirmation
ordering. Either MCU-family order must deliver every subtransfer once all
transit nodes meet these bounds; downstream-first ordering is only a scheduling
choice for selected diagnostics that depend on application return relays.

## Flash, busy intervals and qualification

An application defers flash work during an accepted return window. It never
starts a flash operation that invalidates promised forwarding/capture; a busy
port cannot answer. Hardware forwarding state must remain stable and capture
must continue without overrunning its declared capacity. If that cannot be
guaranteed, the host must quiesce the affected line before a declared or
acknowledged update busy interval. No next frame is sent through a busy forwarder
until its bound has expired/readiness is confirmed. A silent one-way node still
requires the full declared pacing. Include all transit nodes in port gaps and
entry/reset waits; use [blind or selected pacing](update.md#transfer-mode) for
the matching participants. A selected readiness reply never shortens a busy
reservation elsewhere on the port.

Flash deadlines use physical elapsed time: operation deadline = twice the
advertised maximum, with watchdog and fault handling inside that bound. The
host uses that same deadline in declared pacing, including failure cases;
[update.md](update.md) gives the arithmetic. Capture/forwarding code, data and
constants needed while flash is unavailable must all reside in usable memory.
V407 flash clocking is independent of its 200 MHz CPU; V203 clock transitions
must preserve elapsed-time accounting. No CPU-loop timeout is a timing proof.

A certificate is per board×revision×assembly-variant×MCU×port. It records clocks and raw HSI
extremes, sample threshold/quantum, every ISR/interference term above, GPIO/TC
latency, maximum capture storage/blackout, forwarding edge/distortion/skew,
supported rates, pixel waveforms, sustained resource ceilings and flash deadlines.
Test normal reception, calibration, maximum rendering, both digital outputs,
DMA contention and flash transitions. Mixed 144/200 MHz chains cover permitted
first/middle/last placement, all return rates and maximum qualified cable/hops.
Every transmitted answer must meet all bounds; qualification additionally
requires no externally observed contention and MISSED<1% over the specified load matrix. Software
vectors check arithmetic and encoding; they do not replace these measurements.
