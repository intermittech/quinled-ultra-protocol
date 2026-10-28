# Wire-3 shared format and timing vectors

`vectors.json` contains 181 binary encode/decode fixtures; `vectors.md` lists
their bytes and `shared_vectors.h` exports every fixture for firmware tests.
`local_vectors.json` and `.h` add 9 local-link envelopes, including six signed
START/phase goldens anchored to packet acceptance with declared output bounds.
`host_vectors.json` has
four canonical power/S2 JSON and paging cases. Synthetic descriptors exercise
formats and admission; they are never production ratings or certificates.

`golden_traces.json` and `.h` contain 36 profile/clock/start cases, each with
normal and update waveforms: 72 complete frames. They specify bytes, HIGH/LOW
ticks, final-edge TIME and reset for all four profiles at 9.6, 40 and 80 MHz,
including timestamp wrap. Match these in ESPHome, WLED, firmware and the jig.
`vectors_timing.json` adds profile/return arithmetic, GUARANTEE guarantee and qualified-path
bounds, sample/HSI phase corners, forwarding reserve, acquisition and lane scheduling.

Run from the repository root:

```text
python -m pip install jsonschema
python sender/test/run_tests.py --only vectors,wire3,wire3cpp,manifests --skip-cross
python test-vectors/check_vectors.py
python test-vectors/check_timing.py
python test-vectors/check_rules.py --mutants
```

`check_rules.py` guards the recorded design limits: the per-port-type board
limits (Diff-Power port 8, Dig2Analog-Plus-5ch pin 50, never one number for all),
the D2A pin table, the path budgets and the update timing. Each lives in one
named constant whose comment gives its reason; the guard holds the recorded
numbers, checks timing.md and update.md state them, and (in run_tests.py's
`rules` step, with `sender/test/rules_probe.c`) that the sender refuses 9
Diff-Power and 51 D2A boards. A failure names the rule: change a limit only by
an explicit decision, then its constant, the documents and the guard together.

Python independently round-trips and validates the committed bytes. C and C++
each run 225 public-codec checks plus 1860 sender/shared-vector/state checks.
The sender harness covers every binary fixture, partial banks, TLV prefixes,
unknown records, IDENTITY, scoped targets, lowered limits, coherent LINK_STATS,
AUTO acquisition/hysteresis, concurrent PARLIO lanes, update preflight/pacing/
credit/confirmation and golden physical-edge deadlines. Expectations are never
regenerated during tests. MSVC and GCC/Clang are supported; the default runner
also runs sanitizers, scheduler regressions, RMT ownership and examples.
The standalone timing runner checks 313992 assertions against committed
goldens; the sender's arithmetic/profile/TIME suite checks 268012 cases.

Regeneration is explicit and must be followed by independent checks:

```text
python test-vectors/gen_vectors.py
python sender/test/run_tests.py
```

The default run also needs a matching wire-3 receiver checkout (`--firmware
PATH`, `ULTRA_FIRMWARE` or `../quinled-ultra-firmware`) and runs its
receive/light-engine and delay-line integration. Missing or mismatched firmware
fails; it cannot be replaced with codec-only tests.
Target/pin/rating/layout validation belongs to each board's firmware pipeline.

Mixed chains use the GUARANTEE timing bound; EXTRA requires qualified assemblies.
Every HIGH/LOW uses the exact profile recipe; there is no bit-period dither.
Arithmetic and mocked drivers do not qualify PVT, cable, DMA, ISR or supplies.
