# The experiments

The sources of the experiments that [An analog circuit compiler for FMinus](../README.md) (the report) and its
[notes](../notes/) cite, and the results the report quotes. They are research code, kept as it was run on 2026-10-28 and
2026-10-29: not part of FMinus's build (no CMake reaches `docs/`), not wrapped to FMinus's 120 columns, not tested. The
C++ needs only the standard library, except `fmbench.cpp`, a host of FMinus's C++ API (below); the FMinus programs ran
on FMinus at `c75caab`, or on the 2026-10-23 build where [fminus.md](../notes/fminus.md#61-what-the-experiments-show)
says so. One exception to "not tested": FMinus's suite now compiles [`ex1c.fm`](ex1c.fm) and [`ts2.fm`](ts2.fm) beside
the modules the circuit compiler generates (`tests/lit/circuit/equiv-*.fm`), so an edit of either can fail it.

## Running them

Work on a copy, since the generators write into the current directory:

```bash
cp -R docs/research/analog-circuits/experiments ../analog-experiments && cd ../analog-experiments
clang++ -std=c++20 -O2 -ffp-contract=off adaa.cpp -o adaa      # all but fmbench.cpp and tone_lib.cpp build alone
./adaa worst                                                  # prints adaa-worst.out's table
fminus check.fm --run dc 1                                    # an FMinus program, in the JIT
python3 mkbench.py ts                                         # writes bench_ts.fm and bench_ts.names
fmbench bench_ts.fm vector 1000000 7 $(cat bench_ts.names)    # the report's cycle counts: 10^6 samples, best of 7
```

- **C++.** `clang++ -std=c++20 -O2 -ffp-contract=off <file>.cpp -o <file>`, as the report's numbers were built (Apple
  clang 21). The includes are relative: `selftest.cpp` includes `adaa.cpp`, `bjt.cpp` and `ota.cpp` include `dk.hpp`,
  and `tone2.cpp` includes `tone_lib.cpp`. On one core, `./adaa worst` took about 3 minutes and `./selftest` 37 to 81
  seconds.
- **FMinus programs.** `fminus` is FMinus's driver, `build-dev/bin/fminus` after `cmake --workflow --preset dev`
  ([README](../../../../README.md#quick-start)): `fminus prog.fm` compiles and prints the optimised IR, and
  `fminus prog.fm --run f x` calls `f(x)`.
- **`fmbench`** ([`fmbench.cpp`](fmbench.cpp)) is a host of FMinus's C++ API: build it as any host is built, linking
  `FMinus::FMinus` ([README](../../../../README.md#using-fminus-as-a-library)), at `c75caab` for the report's
  numbers. It runs on macOS only (`proc_pid_rusage`, QoS classes). `fmbench file.fm vector|fast|libm n reps fn...`
  compiles the file in that math mode with subnormals flushed and one thread, then calls each `float fn(float n)` `reps`
  times and prints the best wall time, cycles and instructions per unit of `n`.

## The files

C++ experiments:

| File | What it measures | Used in |
|---|---|---|
| [`clip.cpp`](clip.cpp) | first pass on Yeh's RC diode clipper and the Tube Screamer clipping stage: full-band solver errors, the ω approximations alone, aliasing against the oversampling factor, rough C++ timings | [fminus.md §6.1](../notes/fminus.md#61-what-the-experiments-show) (Table 3); report §9.7 |
| [`clip2.cpp`](clip2.cpp) | in-band errors; trapezoidal, BDF2 and backward Euler against a 64x reference; float against double | fminus.md §6.1 (Table 2); report §9.7 |
| [`clip3.cpp`](clip3.cpp) | initial guesses (previous sample, extrapolated, ω, limited) × Newton counts, in band, in float | fminus.md §6.1 (Table 1); report §9.7 |
| [`halfband.cpp`](halfband.cpp) | Kaiser-windowed halfband FIRs for the 2x stages: taps, stopband, multiplies a host sample | fminus.md §6.1 (Table 4) |
| [`gen.cpp`](gen.cpp) | designs the FIR (Kaiser) and IIR (polyphase allpass, HIIR's design) halfbands for 48 → 96 → 192 → 384 kHz, checks each by simulation, and writes `hb-coefs.txt` and `halfbands.fm` into the current directory | [report §4.1](../README.md#41-works-in-fminus-and-the-playground-today) (row 5), §13 |
| [`adaa.cpp`](adaa.cpp) | stateful first-order ADAA with the closed-form antiderivative, on example 1 and the TS clipping stage, with pots at {0, 0.5, 1}: alias, H1 and H3, the solver against its own converged solve. `./adaa`: every case; `./adaa worst`: the worst-case table; `./adaa quick`: every case at pot 1 only | [report §5](../README.md#5-adaa-for-the-1-port-blocks), §1 |
| [`selftest.cpp`](selftest.cpp) | the compiler's self-test on the TS block: a converged 64x double reference on the stress matrix, float candidates against it, pass or fail, and the time of each step | [report §6.6](../README.md#66-what-the-self-test-costs-and-when-it-runs) |
| [`dk.hpp`](dk.hpp) | a netlist → MNA → DK compiler in double, with a runtime in float or double: R, C, voltage sources, ideal op-amps, and ports (diode pair, Ebers–Moll BJT, OTA), with offset-form laws | used by `bjt.cpp` and `ota.cpp` |
| [`bjt.cpp`](bjt.cpp) | `./bjt range`: a 9 V common-emitter stage (the Rangemaster's topology), float against double, absolute states against offsets from the DC point. `./bjt fuzz`, `./bjt fuzzcaps`: the Fuzz Face's 4-port Newton, its K, iterations a sample and fixed-K accuracy, without and with junction capacitances. `./bjt`: both, without capacitances | [report §9.3](../README.md#93-float-on-biased-circuits), [§8.2](../README.md#82-the-fuzz-face) |
| [`ota.cpp`](ota.cpp) | the OTA state-variable filter: the hand DK form against `dk.hpp`'s MNA, 2-D and torn Newton in float with and without per-sample cutoff modulation, aliasing, the error against 64x, self-oscillation | [report §7.2–7.3](../README.md#73-accuracy), [§8.4](../README.md#84-the-ota-svf-and-ladders-tearing-to-1-d-measured-h) |
| [`tone.cpp`](tone.cpp) | example 2's tone stage: the netlist's small-signal response against Yeh, Abel & Smith's eq. 24, both pot orientations, and the trapezoidal block at 48 kHz | [report §9.1](../README.md#91-example-2s-tone-stage-against-yeh-dafx-07-eq-24-h) |
| [`tone2.cpp`](tone2.cpp), [`tone_lib.cpp`](tone_lib.cpp) | the trapezoidal tone block's worst error against its analog response, to 5, 10 and 20 kHz, at 48, 96 and 192 kHz (`tone_lib.cpp` is `tone.cpp` with its `main` renamed) | [report §9.2](../README.md#92-1x-is-not-free-for-linear-blocks-h) |
| [`fmbench.cpp`](fmbench.cpp) | the timer above | [report §3.1](../README.md#31-method-h), Appendix A |

FMinus programs:

| File | What it is | Used in |
|---|---|---|
| [`cal.fm`](cal.fm) | dependent chains of `fadd`, `expf`, `logf` and a divide, the calibration: `fmbench cal.fm vector 1000000 7 addchain expchain expchain2 logchain divchain` | report §3.1 |
| [`lc.fm`](lc.fm) | a local `const` array indexed at run time (`lookup`) and by constants (`tapsum`): `fminus lc.fm` shows the first's 8 stores a call and the second folded | [report §9.6](../README.md#96-fminus-constant-data-h) |
| [`diodetone.fm`](diodetone.fm) | example 1 (the diode clipper with a tone pot) as the compiler would emit it, with a timing harness: `--run main 48000000` against `--run baseline 48000000` | fminus.md [§4](../notes/fminus.md#example-1-a-diode-clipper-with-a-tone-pot), §6.1 |
| [`ex1b.fm`](ex1b.fm), [`ex1b_time.fm`](ex1b_time.fm) | example 1 with a stand-in for the oversampler, and the same with the timing harness | fminus.md §4, §6.1 |
| [`ex1c.fm`](ex1c.fm) | example 1's module and program exactly as fminus.md prints them, with the oversampler stand-in: `fminus ex1c.fm` compiles it | fminus.md §4 |
| [`check.fm`](check.fm) | example 1's DC sanity check: `--run dc 0.1`, `1` and `5` (fminus.md: 0.09995, 0.512 and 0.612 V), and `peak(amp)` for a 1 kHz sine | fminus.md §4 |
| [`ts.fm`](ts.fm), [`ts_time.fm`](ts_time.fm) | example 2's block 2 (the TS clipping stage) with the oversampler stand-in and an `Effect` around it, and the same with the timing harness | fminus.md [§4](../notes/fminus.md#example-2-the-tube-screamers-clipping-and-tone-stages), §6.1 |
| [`ts2.fm`](ts2.fm) | block 2 exactly as fminus.md prints it, with the stand-in | fminus.md §4 |
| [`bench_ota.fm`](bench_ota.fm), [`bench_ota.names`](bench_ota.names) | the OTA SVF voice benches: 2-D and torn Newton, 1 or 2 steps, scalar and as a `float4` bank, at 1x and 2x. Written by a script that was not kept, so the file is here: `fmbench bench_ota.fm vector 1000000 7 $(cat bench_ota.names)` | [report §7.4](../README.md#74-cost-per-voice) |

Generators and data:

| File | What it is | Used in |
|---|---|---|
| [`mkbench.py`](mkbench.py) | writes the benches of the two 1-port blocks in every variant the ranking compares (guess and Newton count, ADAA, oversampling factor, FIR or IIR halfbands, `float` or `float2`, the TS chain partitioned or not): `python3 mkbench.py dt`, `ts`, `tsfull` or `dt2` writes `bench_<name>.fm` and `bench_<name>.names`, reading `hb-coefs.txt` | [report §3–4](../README.md#3-how-the-costs-were-measured-and-what-the-old-figures-were) |
| [`hb-coefs.txt`](hb-coefs.txt) | `gen.cpp`'s halfband coefficients: the FIR taps and IIR allpass coefficients of stages S1 to S3 | `mkbench.py` |

Results, as the runs wrote them:

| File | Written by | Used in |
|---|---|---|
| [`adaa.out`](adaa.out) | `./adaa`, before the reference fix of report §9.7 (which leaves the sine cases as they were) | report §1 (float against double), §5 |
| [`adaa-worst.out`](adaa-worst.out) | `./adaa worst`; a rerun after the reference fix of report §9.7 wrote the same file | report §5.2 |
| [`selftest.out`](selftest.out) | `./selftest`, at load 6–8 | report §6.6 |
| [`bjt.out`](bjt.out) | `./bjt`, from an earlier `bjt.cpp`, before the offset form's split for reverse-biased junctions: its Rangemaster offset column shows the NaN at saturation that report §9.3 describes; its Fuzz Face part (no junction capacitances) gives §8.2's iteration counts | report §8.2, §9.3 |
| [`bjt-range.out`](bjt-range.out) | `./bjt range` | report §9.3's table |
| [`bjt-caps.out`](bjt-caps.out) | `./bjt fuzzcaps` | report §8.2 |
| [`ota.out`](ota.out) | `./ota` | report §7.2–7.3, §8.4 |
| [`timing1.out`](timing1.out) | `fmbench` on `bench_dt.fm`, `bench_ts.fm`, `bench_tsfull.fm` and `bench_dt2.fm` in `Math::Vector`, then `bench_dt.fm` and `bench_ts.fm` in `Math::Fast` | report §3–4 |
| [`timing-ota.out`](timing-ota.out) | `fmbench` on `bench_ota.fm`, in `Math::Vector`, then the linearised torn voice, scalar and banked, in `Math::Fast` | report §7.4, §4.1 (row 4) |

## What was left out

- Binaries, LLVM IR dumps (`*.ll`) and other build outputs, and the FMinus driver and `fmbench` builds the
  measurements used.
- Files the sources here regenerate byte for byte (checked): `halfbands.fm` (`gen.cpp`), and `bench_dt.fm`,
  `bench_ts.fm`, `bench_tsfull.fm`, `bench_dt2.fm` with their `.names` (`mkbench.py`).
- Debug scratch that nothing cites: four probes of the Fuzz Face's operating point and solve built on `bjt.cpp`, a
  noise-burst probe and a debug copy of the self-test built on `adaa.cpp`, an earlier draft of `ex1b.fm`, and a
  one-line driver check.
- The rerun's duplicate of `adaa-worst.out`, its timings (quoted above), and the report's superseded first draft.

The folder holds about 310 KB.
