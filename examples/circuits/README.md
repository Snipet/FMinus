# Circuits: SPICE netlists as FMinus modules

The circuit compiler's example ([docs/circuits.md](../../docs/circuits.md)): the netlists of the report
([docs/research/analog-circuits/README.md](../../docs/research/analog-circuits/README.md)), and `example-circuits`,
which checks the modules `fminus::circuit` generates from them against the report's targets, and reprints the report's
accuracy and cost tables from emitted code.

| Netlist | What | Configuration |
|---|---|---|
| `diodetone.cir` | example 1: a diode clipper, then a tone pot into a low-pass | 4x IIR, plain omega3 + 2 Newton steps |
| `screamer.cir` | example 2: the Tube Screamer's clipping stage, tone control and level, with the report's §9.1 tone-pot line | 4x IIR, ADAA, omega3 + 1 step; the tone block in the region (blocks=auto) |
| `screamer-clip.cir` | example 2's clipping stage alone | 4x IIR, ADAA, omega3 + 1 step |
| `clipper.cir` | Yeh's RC diode clipper, the circuit of fminus.md's Tables 1 to 3 | none: the self-test's (4x IIR, ADAA, omega3 + 1 step); provisional, 8x IIR, plain omega3 + 2 steps |
| `otalp.cir` | the synth path: two buffered OTA one-poles (LM13700-style), the cutoff set by `iabc`, a per-sample law input (`.input iabc - kind=current range=10u:2m`) | 4x IIR, the previous step's v + 1 Newton step: the self-test's answer, pinned |
| `screamer-biased.cir` | `screamer.cir` with its 4.5 V reference drawn (V1, and R1 and R8 to it): the module runs in offsets from its DC point, and is screamer.cir's but for its comments (`.op` prints the point) | screamer.cir's |
| `halfwave.cir` | a half-wave clipper: one diode (the built-in 1N4148, no `.model` card) across C1 | 4x IIR, omega3 + 1 step: the self-test's answer, pinned |
| `asym.cir` | an asymmetric clipper: one 1N914 against two in series (an asymmetric pair) | 4x IIR, ADAA, omega3 + 1 step: the self-test's answer, pinned |
| `wah.cir` | an RLC band-pass, a wah's tank (500 mH, 47 nF), its Q set by a pot, buffered | linear: 1x (with the 1x check's warning) |

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
points, the carry under ADAA; n: a per-sample law input's lanes and block splits), and Phase 1's elements (o: the new
netlists, screamer-biased.cir's offset form against screamer.cir, the self-test's answers on halfwave.cir and
asym.cir, wah.cir's inductor against its trapezoidal response). It times nothing. With
no flags it prints generate()'s and the self-test's times, then the cost table: each row's wall time, and on macOS its
cycles and instructions, beside the self-test's cost model and the report's measurement of the same bench
(`experiments/timing1.out`); it asserts nothing.

The files: `main.cpp` (the command line), `harness.h`/`harness.cpp` (building a module with its runners appended,
running it, the double reference), `check.cpp`, `tables.cpp` and `bench.cpp` (the three modes).
