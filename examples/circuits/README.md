# Circuits: SPICE netlists as FMinus modules

The circuit compiler's example ([docs/circuits.md](../../docs/circuits.md)): the netlists of the report
([docs/research/analog-circuits/README.md](../../docs/research/analog-circuits/README.md)), and `example-circuits`,
which checks the modules `fminus::circuit` generates from them against the report's targets, and reprints the report's
accuracy and cost tables from emitted code.

| Netlist | What | Configuration |
|---|---|---|
| `diodetone.cir` | example 1: a diode clipper, then a tone pot into a low-pass | 4x IIR, plain omega3 + 2 Newton steps |
| `screamer.cir` | example 2: the Tube Screamer's clipping stage, tone control and level, with the report's §9.1 tone-pot line | 4x IIR, ADAA, omega3 + 1 step; the tone block a matched design at 1x (blocks=auto) |
| `screamer-clip.cir` | example 2's clipping stage alone | 4x IIR, ADAA, omega3 + 1 step |
| `clipper.cir` | Yeh's RC diode clipper, the circuit of fminus.md's Tables 1 to 3 | none: the self-test's (4x IIR, ADAA, omega3 + 1 step); provisional, 8x IIR, plain omega3 + 2 steps |
| `otalp.cir` | the synth path: two buffered OTA one-poles (LM13700-style), the cutoff set by `iabc`, a per-sample law input (`.input iabc - kind=current range=10u:2m`) | 4x IIR, the previous step's v + 1 Newton step: the self-test's answer, pinned |
| `screamer-biased.cir` | `screamer.cir` with its 4.5 V reference drawn (V1, and R1 and R8 to it): the module runs in offsets from its DC point, and is screamer.cir's but for its comments (`.op` prints the point) | screamer.cir's |
| `halfwave.cir` | a half-wave clipper: one diode (the built-in 1N4148, no `.model` card) across C1 | 4x IIR, omega3 + 1 step: the self-test's answer, pinned |
| `asym.cir` | an asymmetric clipper: one 1N914 against two in series (an asymmetric pair) | 4x IIR, ADAA, omega3 + 1 step: the self-test's answer, pinned |
| `wah.cir` | an RLC band-pass, a wah's tank (500 mH, 47 nF), its Q set by a pot, buffered | linear: a matched design at 1x (0.35 dB; its trapezoidal response errs by 13.4 dB) |
| `rc8k.cir` | an RC low-pass at 8 kHz, a linear block of one state | linear: a matched one-pole at 1x (0.32 dB; trapezoidal, 12.8 dB) |
| `sallen-key.cir` | a unity-gain Sallen-Key low-pass at 2.1 kHz, Q 0.74, a linear block of two states | linear: a matched biquad at 1x (0.45 dB; trapezoidal, 26.8 dB) |
| `tonestack.cir` | the Fender Bassman's tone stack (treble, bass and mid pots), a linear block of three states | linear: a matched design is of order 1 or 2, so a 4x region of its own, the smallest at which its trapezoidal response passes the 1x check |
| `switch.cir` | example 1 with a clipping switch: S1 in series with the diodes, closed at `.switch clip` position 1 | 4x IIR, ADAA, omega3 + 1 step; S1 ramped over 20 ms of set() calls |
| `ringing.cir` | an inductive kick: R1 100 into 1 mH, a diode to ground at its far end with 1 nF across it, which rings at 159 kHz when the diode turns off | 2x IIR, ADAA, omega3 + 1 step: the self-test's answer, pinned; at 1x the self-test measures it ringing at Nyquist, warns, and tries BDF2 twins |

```bash
build-dev/bin/fminus examples/circuits/diodetone.cir --emit fm   # the generated module
build-dev/bin/example-circuits --check                            # the targets T1-T4 and the rest (ctest's run)
build-dev/bin/example-circuits --tables                           # report §5.2's table, fminus.md's Tables 1-3
build-dev/bin/example-circuits                                    # the cost table (on a quiet machine)
build-dev/bin/example-circuits --all --only adaa --reps 7 --samples 1000000 --math fast
```

`--check` prints, for each check, the worst value, where it fell, and the report's figure beside it, and exits 1 on a
failure: the report's targets (a-h), then the self-test's search without the netlists' configuration directives, which
must choose what they ship (i, j), the run-time guard (k), the provisional configuration (l), and the synth path on
`otalp.cir` (m: T1-T4 on the synth's stress matrix, the self-test's choice, tick against step, the prewarp's -3 dB
points, the carry under ADAA; n: a per-sample law input's lanes and block splits), Phase 1's elements (o: the new
netlists, screamer-biased.cir's offset form against screamer.cir, the self-test's answers on halfwave.cir and asym.cir,
wah.cir's matched design), and Phase 1's coefficients and linear blocks (p: the Woodbury form's coefficients on every
shipped netlist against the double derivation all in set, switch.cir's ramp, its block splits and its ADAA residual, the
matched designs of rc8k.cir and sallen-key.cir, the tone stack's linear-only region, BDF2 against its reference, and the
method check: ringing.cir ringing at 1x, the TS stage flagged without ringing; with d, a tone glide through the TS tone
block matched at 1x). It times nothing. With no flags it prints generate()'s and the self-test's times, set()'s cycles
on the TS tone block in the Woodbury form and all in set, then the cost table: each row's wall time, and on macOS its
cycles and instructions, beside the self-test's cost model and the report's measurement of the same bench
(`experiments/timing1.out`); it asserts nothing.

The files: `main.cpp` (the command line), `harness.h`/`harness.cpp` (building a module with its runners appended,
running it, the double reference), `check.cpp`, `tables.cpp` and `bench.cpp` (the three modes).
