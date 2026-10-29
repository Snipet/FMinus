# Circuits: SPICE netlists as FMinus modules

The circuit compiler's example ([docs/circuits.md](../../docs/circuits.md)): the netlists of the report
([docs/research/analog-circuits/README.md](../../docs/research/analog-circuits/README.md)), and `example-circuits`, which
checks the modules `fminus::circuit` generates from them against the report's targets, and reprints the report's
accuracy and cost tables from emitted code.

| Netlist | What | Configuration |
|---|---|---|
| `diodetone.cir` | example 1: a diode clipper, then a tone pot into a low-pass | 4x IIR, plain omega3 + 2 Newton steps |
| `screamer.cir` | example 2: the Tube Screamer's clipping stage, tone control and level, with the report's §9.1 tone-pot line | 4x IIR, ADAA, omega3 + 1 step; the tone block in the region (blocks=auto) |
| `screamer-clip.cir` | example 2's clipping stage alone | 4x IIR, ADAA, omega3 + 1 step |
| `clipper.cir` | Yeh's RC diode clipper, the circuit of fminus.md's Tables 1 to 3 | Phase 0's defaults: 8x IIR, plain omega3 + 2 steps |

```bash
build-dev/bin/fminus examples/circuits/diodetone.cir --emit fm      # the generated module
build-dev/bin/example-circuits --check                               # the targets T1-T4 and the rest (ctest's run)
build-dev/bin/example-circuits --tables                              # report §5.2's table, fminus.md's Tables 1-3 (minutes)
build-dev/bin/example-circuits                                       # the cost table (on a quiet machine)
build-dev/bin/example-circuits --all --only adaa --reps 7 --samples 1000000 --math fast
```

`--check` prints, for each check, the worst value, where it fell, and the report's figure beside it, and exits 1 on a
failure. It times nothing. The cost table prints each row's wall time, and on macOS its cycles and instructions, beside
the report's measurement of the same bench (`experiments/timing1.out`); it asserts nothing.

The files: `main.cpp` (the command line), `harness.h`/`harness.cpp` (building a module with its runners appended,
running it, the double reference), `check.cpp`, `tables.cpp` and `bench.cpp` (the three modes).
