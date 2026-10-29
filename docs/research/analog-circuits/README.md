# An analog circuit compiler for FMinus

Research, not a feature: nothing of the compiler described here is built, in FMinus or anywhere else. This report,
written on 2026-10-29, proposes a compiler from SPICE-style netlists to FMinus source, and measures what its code would
cost on FMinus at commit `c75caab`. This folder holds the report, the four research notes it draws on
([notes/](notes/)), and the sources and results of the experiments it cites ([experiments/](experiments/), whose
[README](experiments/README.md) says how to build and run each).

The report draws together the four notes ([landscape.md](notes/landscape.md), [methods.md](notes/methods.md),
[optimization.md](notes/optimization.md), [fminus.md](notes/fminus.md)) and settles the points where they disagree. It
adds a second round of experiments:
- costs measured with FMinus at `c75caab`, the commit the playground (TestingPlayground: two plugins, an effect and a
  synth, whose DSP is an FMinus program) pins;
- stateful antiderivative antialiasing (ADAA);
- a self-test run against a stress matrix;
- a synth filter;
- biased BJT stages in float;
- the Fuzz Face's 4-port solve.

Nothing was built or edited in any repository. The FMinus driver and a bench host were linked in a scratch folder
outside the repositories, against the c75caab libraries of a playground build (Appendix A).

**Tags.** **[H]**: measured here, or read in a primary source during this research. **[M]**: from a secondary source, or
a reasoned estimate built on [H] data. **[L]**: judgement, not measured. A claim carried over from a note keeps that
note's tag. [landscape.md](notes/landscape.md)'s tags map as [read] = [H], [doc] = [M], [mem] = [L].
[methods.md](notes/methods.md)'s map as [✔] = [H] and [◐]/[○] = [M]/[L].

**Units.** Costs are **cycles per host sample at 48 kHz**, read from the process's own cycle counter (`proc_pid_rusage`,
the method of FMinus's [recipe 24](../../extending.md#recipe-24-the-math-kernels-inline)). They include 10 cycles of
test-signal generation. Over load averages of 3 to 14, cycle counts held within 1–2 %. Wall time did not: the effective
clock ranged from 2.0 to 4.3 GHz depending on the core. "ns at 4.3 GHz" means cycles divided by 4.3, an idle performance
core's figure.

---

## 0. Summary

- **The architecture stands** ([fminus.md §3–5](notes/fminus.md#3-a-where-the-compiler-lives)) [M]. A C++ library,
  `fminus::circuit`, turns a SPICE-style netlist into ordinary FMinus source. Per block it uses the Holters–Zölzer
  generalized state space. Blocks are cut at ideal op-amps, capacitors use trapezoidal companions, and the output is
  straight-line float code with fixed iteration counts. This round changes *how* the compiler optimises, not *where* it
  lives.
- **Aliasing is the binding constraint, and ADAA is the biggest measured lever on the 1-port blocks** [H].
  - Stateful first-order ADAA (Holters 2020) needs no antiderivative table. Its antiderivative is closed-form:
    `F(p) = p·v − v²/2 − k·nV_t·(cosh(v/nV_t) − 1)`, where `v` is the solver's own result.
  - On the TS clipping stage it buys one octave of oversampling. ADAA at 2x reaches −52.6 dB against −53.6 dB plain at
    4x. ADAA at 4x reaches −75.1 dB against −72.9 dB plain at 8x.
  - It also makes the output insensitive to solver error to second order. ω + 1 Newton step with ADAA lands −81 to −84
    dB from its converged solve; without ADAA the same solver lands at −50 dB.
  - At the common target, the TS stage costs **907 cycles with ADAA at 4x, against 1,867 plain at 8x (2.06×)**.
- **The closed-form guess pays through robustness, not through a cut in flops** [H].
  - ω3 costs about one Newton step: ω + 1 step is 164 cycles, previous sample + 2 steps is 170.
  - Starting from the previous sample never reaches −60 dB on the TS stage. With 6 steps at 8x the worst error is still
    −56.7 dB, at 3,551 cycles.
  - ω + 2 steps at 8x passes at 1,861.
- **The recursion is latency-bound, so rankings by flop count mislead** [H].
  - Running the TS's three linear blocks at 1x instead of 8x saves 12 % of instructions and 0 % of cycles (1,867 against
    1,865).
  - FIR halfbands execute 30 % more instructions than IIR ones but cost only 0–4 % more cycles.
  - A per-step critical-path model predicts the measured costs within about 5 %: ω about 65 cycles, a Newton step 70,
    ADAA 57, and 10 for the multiply-adds (§3.2).
- **One target set, used everywhere** (§2): solver ≤ −60 dB, alias ≤ −60 dB, H1 and H3 within ±0.5 dB, finite output,
  all checked on one stress matrix. The notes' −50 dB, −80 dB and 0.2 % targets are retired.
- **The decision rule is now a table a compiler can apply** (§6). It keys on M, the law family, bias, the control rate,
  and whether the block is an effect or a voice. It includes a run-time guard and an outcome for when no configuration
  meets the target. The self-test cost is measured: on one TS block, 15–26 s for the reference plus 18 s for an ordered
  search, single-threaded. It must therefore run asynchronously and be cached.
- **Float is fine for unbiased blocks; biased blocks need the offset formulation** [H].
  - On a 9 V Rangemaster-type stage, absolute float states reach only −46 to −87 dB against double, and get worse with
    oversampling.
  - Storing offsets from the DC operating point reaches −81 to −133 dB at 10–100 mV.
  - At hard drive (1 V) both forms sit at −55 to −87 dB, and the fixed-K solver becomes the limit.
- **Hard-driven BJT feedback circuits do not converge at a fixed iteration count** [H].
  - On the Fuzz Face (4 ports, dense K), offline damped Newton needs 14–118 iterations per sample on average.
  - Adding junction capacitances cuts that to 3.8–16, as Holmes et al. 2017 predicted.
  - Float Newton from the previous sample, with capped steps and K ≤ 8, reaches −78 dB only at 8x with a 10 mV input. It
    fails at guitar level.
  - Phase 2 is therefore riskier and costlier than fminus.md planned.
- **The synth path now has a worked example: an OTA state-variable filter** (§7) [H].
  - The cutoff (I_abc) enters only the device law, so a per-sample CV costs no matrix update.
  - The fastest correct solver is the full 2-D Newton with 2 steps, at 248 cycles per voice. The torn 1-D Newton costs
    313–376, because it serialises the two tanh evaluations.
  - A float4 voice bank runs at 47 cycles per voice (5.3×), but needs a host change.
- **Tearing (BLT plus a tear set) is the right structural pass, but no free win** (§8). On the dense Fuzz Face it cannot
  shrink the 4-D Newton below 3 variables. On the OTA SVF it cuts 2-D to 1-D, yet runs slower on an out-of-order core.
  The compiler should generate both forms and pick by measured cost.
- **Verification fixes** (§9).
  - Example 2's tone stage matches Yeh's eq. 24 exactly (to 1e-14 dB) once the pot's ends are swapped: as written,
    tone = 1 was dark.
  - At 1x the trapezoidal tone block errs by 1.4 dB at 10 kHz and 9 dB at 20 kHz, so it needs 2x or a magnitude-matched
    design.
  - [`Math::Fast`](../../extending.md#recipe-15-math-that-gives-a-float-one-result), the frozen copy's math mode, is
    11–12 % *faster* on these circuits. That retires the risk in [fminus.md
    §7](notes/fminus.md#7-d-what-fminus-gives-for-free-and-what-it-lacks).
  - Local `const` arrays are re-stored on every call when indexed at run time, and fold to constants when indexed by
    constants.
  - The notes' "converged" double reference could stop short after large jumps in p. It now starts from ω, and every
    sine table reproduced.

---

## 1. Rulings where the notes disagree

| Question | landscape / methods / optimization / fminus said | Ruling | Evidence |
|---|---|---|---|
| ADAA: a default, or a later option? | default for scalar stages ([opt. §4.5](notes/optimization.md#45-adaa-versus-oversampling), [§8 step 8](notes/optimization.md#8-what-the-compiler-should-do-in-order-l)); "later option, not a default" ([fminus §2](notes/fminus.md#2-the-methods-in-one-page), [Phase 4](notes/fminus.md#10-phased-plan)) | **A first-class candidate in Phase 1, chosen by the self-test, preferred on a tie.** For 1-port effect blocks it won on the TS (2.06×) and tied on example 1 under T3 (866 against 912 cycles). Not a default for multi-port blocks (p is not scalar), nor in voices that already meet the target at 1x | §5 [H] |
| Does FMinus need `as_int`/`as_float`? | yes, or pay Faust's 3–4× ([landscape §2.7](notes/landscape.md#27-faust-wd-wdmodelslib-roosenburg-stine-michon-chowdhury-smc-21), [§6.9](notes/landscape.md#6-what-fminus-should-borrow-ranked-optimisation-first)); not worth adding ([fminus §7](notes/fminus.md#7-d-what-fminus-gives-for-free-and-what-it-lacks)) | **No.** ω3 is a cubic, a few selects and one `logf`; it measured about 65 cycles, roughly one Newton step. Faust's penalty came from a fixed-iteration Lambert W, not from missing bit casts. The later saving is an `omegaf` math kernel, not bit casts (about 40 cycles a step [M]) | §3.2 [H]; the Faust attribution is Chowdhury 2022, via [landscape](notes/landscape.md#27-faust-wd-wdmodelslib-roosenburg-stine-michon-chowdhury-smc-21) [H] |
| What ranks first: the closed form, or partitioning? | closed form, 3–10× ([opt. §0](notes/optimization.md#0-the-ranking-in-one-page); [methods §0.8](notes/methods.md#0-findings-that-matter-for-the-compiler)); partition ([fminus §6.2](notes/fminus.md#62-the-optimisations-ranked-by-what-they-save)) | **Neither, as stated.** The closed form is worth its robustness at fixed K: it is the only guess that meets −60 dB on the TS (≥ 1.9× at target, where prev never passes). Partitioning saved 0 cycles on a latency-bound chain. The top measured levers are ADAA on 1-port blocks, and `float2` for stereo | §4 [H] |
| Accuracy targets | alias −50 dB ([fminus `.target`](notes/fminus.md#4-b-the-syntax)); in-band −80 dB ([opt. §6.5](notes/optimization.md#65-the-search)); H2–H9 ≤ 0.2 % ([opt. §6.5](notes/optimization.md#65-the-search)) | **One set, T1–T4 (§2):** solver ≤ −60 dB, alias ≤ −60 dB, H1/H3 within ±0.5 dB, finite output. −80 dB alias costs another octave on every clipper. −50 dB is audible on sustained tones [L]. The 0.2 % harmonic figure is a SPICE-matching goal (Oyama), not a solver gate | §2 |
| Pots | Woodbury on the 16-sample grid ([fminus](notes/fminus.md#62-the-optimisations-ranked-by-what-they-save)); per-sample pot-as-port ([opt. §3.5](notes/optimization.md#35-pots-rank-updates-instead-of-re-inversion)) | **Both, chosen by input rate.** A dial (a 20 ms glide) uses Woodbury on the absolute grid. An audio-rate resistive control (LDR, JFET as a resistor, a `.input`-driven resistor) uses pot-as-port. A controlled-source gain (OTA I_abc, VCA) enters the device law and needs neither | §6.2, §7 [H for the OTA case] |
| Is float DK still an open question? | open ([landscape §0.6](notes/landscape.md#0-findings-in-brief), [§10](notes/landscape.md#10-open-questions)); fine at −117 to −146 dB ([fminus §6.1](notes/fminus.md#61-what-the-experiments-show)) | **Settled, with a split.** Unbiased 1-port blocks are fine: −117 to −146 dB in [fminus §6.1](notes/fminus.md#61-what-the-experiments-show) [H], and −122 to −137 dB at small signals in this round. Biased blocks lose 50–80 dB with absolute states; offsets restore it (§9.3) | §9.3 [H] |
| SIMD across voices (rank 4, 2.5–4×) | available ([opt. §0](notes/optimization.md#0-the-ranking-in-one-page)) | **Not available today:** the playground runs one `Voice` instance per note. Measured potential on the OTA SVF with a float4 bank: 4.4–5.3×. It needs a host change (§7.6) | §7.4 [H] |
| Cost anchors | 64–77 ns per step, "4 × 140 cycles" ([fminus §6.1](notes/fminus.md#61-what-the-experiments-show)); flop counts with exp at 15–20 ([opt. §7](notes/optimization.md#7-per-circuit-playbooks)); PedalKernel's Fuzz Face ≈ 8 ns ([landscape §2.10](notes/landscape.md#210-pedalkernel-avery-wagar-rust-agpl-30--commercial)) | **Replaced** by c75caab cycle counts. ω + 1 step costs 155–165 cycles, so 4 × 160 at 4x: "4 × 140" was close. PedalKernel's 8 ns is dropped as an anchor: a 2-BJT feedback circuit cannot run Newton in 34 cycles, so it is most likely a table or a simplified model [L] | §3 [H] |

---

## 2. One target set, one stress matrix

**Targets.** Each one gates. A configuration passes only when it meets all four.

| | Gate | Measured against |
|---|---|---|
| **T1 solver** | in-band (< 20 kHz) error ≤ −60 dB, relative to the in-band signal | the same configuration solved to convergence in double (same discretisation, rate and ADAA) |
| **T2 alias** | inharmonic energy below 20 kHz ≤ −60 dB, relative to all energy below 20 kHz | that configuration's converged output, for sines at or below `.target level` |
| **T3 response** | H1 and H3 within ±0.5 dB of the 64x reference (H2–H5 in the compiler; only H1 and H3 were measured here) | a converged 64x non-ADAA reference |
| **T4 robustness** | finite output on every stress signal, including 2.2 × the target level | – |

**Stress matrix.** The compiler's self-test runs it per block.
- **Pots:** every pot at 0, 0.5 and 1. With three pots or fewer, the full grid. With more, each pot alone at those three
  points with the others at default, plus the all-ends corners. Only pots that change the block count.
- **Sines:** 5 levels (target/15, target/4.5, target/2.25, target, 2.2 × target) × 3 frequencies (about 100 Hz, 1 kHz
  and 3 kHz). Each sits on an **odd FFT bin**, so no alias can land on a harmonic bin; §9.7 shows a 3000.0 Hz tone
  hiding every alias.
- **Sweep and noise:** a 20 Hz–20 kHz log sweep and a band-limited noise burst, both at target level and gated on T1 and
  T4.
- **Synth blocks add:** a band-limited saw; resonance at its ends and middle; self-oscillation, judged by frequency and
  level, not by the waveform (§7.3); and per-sample cutoff modulation of ±2 octaves at 37 Hz.

The reference case used below is example 2 at `.target level=0.45V freq=3k`, with drive at its ends and middle.

---

## 3. How the costs were measured, and what the old figures were

### 3.1 Method [H]

- [`fmbench`](experiments/fmbench.cpp) (Appendix A) compiles a `.fm` file with FMinus c75caab, with `Denormals::Flush`,
  one thread, and `Math::Vector` unless stated otherwise.
- It calls `float f(float n)` entry points in-process and keeps the best of 7 runs of 10⁶ host samples.
- The circuit code is what the compiler would emit ([`mkbench.py`](experiments/mkbench.py)): straight-line code,
  constants hoisted into `set()`, divides by constants turned into products, and halfbands with their taps and
  coefficients as literals.
- Calibration came from the same run ([`cal.fm`](experiments/cal.fm)). A dependent `fadd` costs 2.2 cycles. In a
  dependent chain, `expf` costs 45 cycles ([recipe 24](../../extending.md#recipe-24-the-math-kernels-inline): 41.7) and
  `logf` 55 (recipe 24: 52.2). A divide costs 10.1. The counts therefore line up with FMinus's published ones.

### 3.2 The critical path of one step

| Piece of a 1-port step | Cycles | Notes |
|---|---|---|
| p = G x + H u (2–3 dependent multiply-adds) | ~5–10 | |
| ω3 guess | ~65 | `logf` is on the path because the piecewise `?:` is if-converted to selects. A branch would cost ~15 when the argument is below 8 [L] |
| one Newton step | ~70 | `expf` 40, reciprocal ~8, divide ~10, about 10 adds, multiplies and clamps |
| ADAA tail | ~57 | `expf` for the cosh, a reciprocal, the divide by Δp, the averages |
| state update | ~5 | |

Measured per oversampled step: ω + 1 step, 154–159 cycles; ω + 2 steps, 223–225; previous sample + 2 steps, 160;
previous sample + 4 steps, 299; ADAA with ω + 1, 211–217. The model matches within about 5 % [H].

Extending the model to a BJT block: one 4-port Newton iteration costs ~150 cycles [M]. That is 2 exponentials per
transistor, run in parallel (~45), plus an unrolled 4×4 solve on the chain (4 divides and about 30 dependent operations,
~100).

### 3.3 The old figures, corrected

[fminus.md](notes/fminus.md)'s "64–77 ns a step" came from the 2026-10-23 build at load 7–52. On that loaded machine, 64
ns is roughly 140–280 cycles depending on the core. The c75caab figure is 155–165 cycles, or 36–38 ns at 4.3 GHz [H].

[Optimization §7](notes/optimization.md#7-per-circuit-playbooks) counted flops, with an exp at 15–20. On the recursion's
path an exp is ~40 cycles of latency and a divide ~10. Its playbook, restated in cycles:

| Circuit ([opt. §7](notes/optimization.md#7-per-circuit-playbooks)) | Configuration at T1–T4 | Cycles per host sample | Tag |
|---|---|---|---|
| diode clipper (example 1, with its tone rheostat) | 4x, plain ω + 2, or ADAA ω + 1 at 4x | 912 / 866 for one channel; `float2` stereo 965 / 957 for both | [H] |
| Tube Screamer clip + tone + level | clip with ADAA ω + 1 at 4x, linear blocks at 1x | 907 | [H] |
| OTA SVF voice (2 ports) | 2-D Newton, 2 steps, 1x | 248 per voice; float4 bank 47 | [H] |
| Moog or transistor ladder voice (4 tanh, torn to 1-D or solved 4-D) | 2 steps, 1x | ~400–600 per voice, scalar | [M] |
| Fuzz Face (4-port BJT) | 8x, previous sample + capped steps, K = 6 | ~7,200 (8 × 6 × 150), and it does not meet T1 above 10 mV | [M] cost, [H] convergence |
| op-amp filter (M = 0) | biquad/TPT at 1x, or TR at 2x | ~10–20 | [M] |

The 8–12× for the ladder in [opt. §7](notes/optimization.md#7-per-circuit-playbooks) assumed voice SIMD, which is not
available (§7.6).

---

## 4. The optimisation ranking, measured

Everything here meets T1–T4 unless marked otherwise. "Before" is the configuration the technique replaces **at the same
target**. Paper numbers appear only as support.

### 4.1 Works in FMinus and the playground today

| # | Technique | Block | Before → after (cycles per host sample) | Saving | Notes |
|---|---|---|---|---|---|
| 1 | **ADAA (stateful, closed-form F) instead of a higher rate** | TS clip, all drives | plain 8x ω + 2: 1,861 → ADAA 4x ω + 1: 903 (chain 1,867 → 907) | **2.06×** | buys one octave and one Newton step (§5). Holters 2020 judged ADAA at 2x ≈ plain at 5x by eye [H]; Albertini et al. 2020 found ADAA at 2x ≈ plain at 6x, for 2nd-order ADAA [H] |
| | | example 1 | plain 4x ω + 2: 912 → ADAA 4x ω + 1: 866 | 1.05× | with T3's ±0.5 dB at H3 = 9 kHz. ADAA at 2x (441, 2.07×) fails T3 by 0.4 dB of droop |
| 2 | **Closed-form ω guess instead of the previous sample** | TS clip | prev + 6 steps at 8x: 3,551, still failing T1 at −56.7 dB → ω + 2 at 8x: 1,861 | ≥ 1.9× | prev fails T1 from 1x to 8x with K ≤ 6. Holmes's "new iterate" result is of the same kind [H] |
| | | example 1 | prev + 4 steps at 4x: 1,257 → ω + 2 at 4x: 912 | 1.38× | |
| 3 | **Stereo in one `float2` recursion** | example 1, 4x ω + 2 | 2 × 912 → 965 | 1.89× per channel pair | the two-lane kernels pair the channels. ADAA at 2x: 2 × 441 → 452–479 |
| 4 | **Frozen copy in `Math::Fast`** | example 1, TS | 912 → 808; 1,861 → 1,660; ADAA 903 → 796 | 1.11–1.13× | the opposite of [fminus §7](notes/fminus.md#7-d-what-fminus-gives-for-free-and-what-it-lacks)'s worry. The OTA voice: scalar 313 → 176, but the float4 bank 283 → 353 (slower). Measure per program |
| 5 | **IIR halfbands instead of FIR** | all | 4x: 636 → 623; 8x: 1,146 → 1,122 | 1.00–1.04× in cycles, 1.3–1.4× fewer instructions | the real reason is latency: the FIR round trip is 40 / 47 / 49.5 host samples at 2x / 4x / 8x (0.83–1.03 ms), the IIR 3.2 / 4.3 / 4.7 (group delay at 1 kHz). Every stage reaches ≥ 92 dB stopband ([`gen.cpp`](experiments/gen.cpp)) |
| 6 | **Partition, with linear blocks at 1x** | TS chain | all at 8x: 1,865 → linear at 1x: 1,867 | none (−12 % instructions) | the out-of-order core hides the linear work behind the solve's chain. Pays when the linear part is large (N ≥ 8) or the core is throughput-bound [M]. A fidelity condition for linear blocks at 1x is in §9.2 |
| 7 | **Fewer Newton steps where T1 allows** | TS, example 1 | ω + 2 → ω + 1: −69 cycles per step | 1.4× | ω + 1 alone fails T1 (worst −50 dB) in every plain configuration, but passes with ADAA (−81 to −84 dB) |
| 8 | **Divides by constants emitted as products** | all | ~7 cycles per replaced divide on the chain [M] | small | already in the emitted code. [fminus.md](notes/fminus.md#61-what-the-experiments-show)'s 77 → 64 ns was not a controlled comparison |
| 9 | **2-D Newton instead of the torn 1-D one** | OTA SVF voice | torn, 2 steps: 376 with an exact final evaluation, 313 linearised → 2-D, 2 steps: 248 | 1.26–1.5× | tearing serialises the two tanh evaluations (§8.4). Torn with 1 step (165) wins where it meets T1, at moderate Q |
| 10 | **Offset formulation for biased blocks** | Rangemaster topology | absolute float −54 to −87 dB → offset −81 to −133 dB (≤ 100 mV) | precision, not speed | same cost [M]. Needs `expm1` (§9.3) |
| 11 | **Junction capacitances in the BJT model** | Fuzz Face | reference iterations 118 / 43 / 25 → 10 / 4.5 / 3.8 (1x / 4x / 8x, 10 mV) | 5–12× fewer offline iterations | Holmes et al. 2017 saw the same direction (Rangemaster 95.9 → 75.8 ms) [H]. Not enough for a fixed K (§8.2) |

### 4.2 Needs an FMinus or host change

| Change | Where it pays | Measured or estimated gain | Effort |
|---|---|---|---|
| **Voice bank (host):** 4 voices in `float4` lanes, with notes routed to lanes | every per-voice circuit | OTA SVF: 248 → 47 per voice (2-D), 313 → 71 (torn, linearised) | host side 3–5 agent-days [L] |
| `omegaf` math kernel | every diode or junction 1-port | ω from ~65 to ~25 cycles, −25 % of a 1-port step [M] | 2–3 |
| `expm1f` builtin | the offset formulation | precision without the tanh identity (§9.3), one select fewer [M] | 0.5 |
| `fmaf` | multiply-add-heavy linear parts, Newton updates | fmadd latency ≈ 4 cycles against mul + add ≈ 5.2 on the chain: ≤ 5 % on these blocks [M] | 0.5 |
| file-scope `const` arrays | tables indexed at run time (Γ(p), DC-point grids) | required: local arrays are re-stored on every call (§9.6) | 1.5–2 |
| `int latency()` in the contract (host) | FIR oversampling | reports 47 samples at 4x (IIR: ~4) | 1 |
| `#line` | error messages in `.cir` terms | – | 1 |

### 4.3 What the measurements overturn

[Optimization §0](notes/optimization.md#0-the-ranking-in-one-page) lists factors drawn from different papers, tolerances
and baselines, and those factors do not compose. On a latency-bound recursion, cycles follow the chain, not the
operation count.
- Partitioning and the choice of halfband moved cycles by 0–4 %.
- ADAA and the closed-form guess moved them by about 2×.
- What decides cost is the latency of the transcendentals on the chain (ω, exp, the ADAA cosh) and the oversampling
  factor.

The compiler should rank candidates by **measured cycles of its own emitted code**, with the §3.2 model as a pre-filter,
and not by flops.

---

## 5. ADAA for the 1-port blocks

### 5.1 Method

Each block is cast in Holters' form (Appl. Sci. 10(1):20, 2020, eqs. 8–19, read in full) [H]:

```
p(n) = C_p x(n-1) + d_p u(n)                       v = G(p) solves  v + k sinh(v / nVt) = p   (the delay-free loop's solution)
x(n) = A x(n-1) + B u(n) + W G(p(n))               y(n) = C_y x(n-1) + D_y u(n) + w_y G(p(n))
ADAA:  x(n) = ½A (x(n-1) + x(n-2)) + ½B (u(n) + u(n-1)) + W Ḡ(n)
       y(n) = ½C_y (x(n-1) + x(n-2)) + ½D_y (u(n) + u(n-1)) + w_y Ḡ(n)
       Ḡ(n) = (F(p(n)) - F(p(n-1))) / (p(n) - p(n-1)),   every coefficient designed for fs' = 2/3 fs
```

The 2/3-rate design absorbs the feedback's extra half-sample delay. Holters shows it preserves stability. Werner
(DAFx-20in21) reaches the same result from equivalent circuits and extends it to second order [H].

**The antiderivative needs no table** [H; the derivation was checked numerically]. Holters tabulated F at 1024 points
with cubic interpolation. For the inverse of `h(v) = v + k sinh(v/nVt)`, integration by parts gives
```
F(p) = p·v − H(v),   H(v) = v²/2 + k·nVt·cosh(v/nVt),   with v = G(p)
∂F/∂v = p − h(v) = 0 at the root
```
So F is **stationary in v**: a solver error δv moves F only by O(δv²). That is the second-order insensitivity measured
below. The emitted form, which avoids cancellation:
```
Ḡ = v̄ + [Δv·(p̄ − v̄) − k·nVt·(cosh(v₁/nVt) − cosh(v₀/nVt))] / Δp      (v̄, p̄ are means; Ḡ = v̄ when |Δp| < 1e-5 V)
```

The same identity works for any 1-port whose law has a closed-form antiderivative; an OTA's `tanh` gives `log cosh`.
Albertini, Bernardini & Sarti (DAFx-20) use the one-diode ω form instead, whose antiderivative is `ω²/2 + ω` up to
scaling (their eqs. 15 and 20) [H]. The identity above is exact for the diode pair, and holds whatever solver produced
v.

### 5.2 Results

From [`adaa.cpp`](experiments/adaa.cpp) ([`adaa-worst.out`](experiments/adaa-worst.out)): worst case over pots at {0,
0.5, 1} × the three cases of Tables 1–3.

| Block | Rate | Alias, plain / ADAA | H1 / H3 change with ADAA | Solver vs own converged: plain ω + 1 / ω + 2 · ADAA ω + 0 / ω + 1 | Delay added |
|---|---|---|---|---|---|
| TS clip | 1x | −22.7 / −35.9 | −0.12 / −0.58 dB | −51.7 / −92.2 · −43.4 / **−83.4** | 0.5 host samples |
| | 2x | −31.1 / −52.6 | −0.04 / −0.17 | −49.8 / −89.4 · −46.3 / −83.7 | 0.25 |
| | **4x** | −53.6 / **−75.1** | −0.01 / −0.06 | −51.1 / −91.8 · −44.3 / −81.2 | 0.125 |
| | 8x | −72.9 / −89.7 | −0.00 / −0.03 | −54.4 / −94.4 · −42.3 / −82.6 | 0.06 |
| example 1 | 1x | −42.0 / −58.0 | −0.40 / −3.74 | −54.0 / −93.2 · −51.3 / −89.1 | 0.5 |
| | **2x** | −52.4 / **−81.9** | −0.12 / −0.91 | −54.2 / −93.9 · −45.9 / −83.2 | 0.25 |
| | 4x | −79.8 / −107.6 | −0.04 / −0.24 | −54.5 / −93.7 · −45.8 / −84.2 | 0.125 |

- **Self-test result.** On the full stress matrix (§6.6), the TS stage passes T1 and T2 with **ADAA at 4x** (worst alias
  −71.9 dB) or with plain ω + 2 at 8x (−68.9 dB). Nothing at 2x passes; ADAA there reaches −52.7 dB [H].
- **Cost.** ADAA costs 57 cycles a step, about one Newton step, and it lets the solver drop one step (§3.2) [H].
- **The price:**
  - a high-frequency droop from ½(1 + z⁻¹) and the 2/3-rate warp: H3 of a 3 kHz tone is down 0.9 dB at 2x and 0.24 dB at
    4x;
  - 0.5/M host samples of delay;
  - an in-band null against the 64x reference of only −40 to −66 dB, even with the delay compensated. That is phase and
    linear-response error, inaudible at these levels [L], which is why T3 gates magnitudes rather than the null.

### 5.3 Decision

- **Phase 1 candidate** for every 1-port block whose law has a closed-form H: diodes, diode pairs, strings, a diode with
  R_s, an OTA, a soft op-amp. The self-test chooses it when it meets T1–T4 at a lower total cost, and prefers it on a
  tie, where it also needs one Newton step fewer.
- **Not for multi-port blocks.** p is a vector, so its antiderivative needs a line integral (Holters §2). Carson's
  two-port extension is cited by Werner but was not read [M].
- **Not for self-oscillating voice filters without a listening check.** ADAA changes a resonance's frequency response;
  Holters warns about high-Q all-passes [H].
- **Second-order ADAA later.** Albertini et al. found it better than first order at 2x. Werner's analysis suggests it
  needs more oversampling than first order [H].

---

## 6. The decision rule the compiler applies

### 6.1 What it computes per block, after reduction

- **M:** the number of ports, after merging antiparallel and series diodes and folding constant subproblems.
- **d:** the index dimension, the rank of `[P_h | P_u,signal]` after Holters' QR.
- **The law family of each port:**
  - **EA (exp-affine):** after Thévenin reduction the port equation is `v + K·I_s(e^{v/nV_t} − 1) = p`, or the pair's
    `2I_s sinh`, with K a positive scalar. A series string of n diodes becomes nV_t ← n·nV_t. A diode's R_s is absorbed
    into K (K ← K + R_s). An asymmetric pair uses ω per sign.
  - **SM (smooth monotone):** tanh (an OTA, a soft op-amp) or a softplus.
  - **MP (multi-port coupled):** the 2 ports of a BJT or of a triode.
- **Bias:** whether the DC operating point is nonzero.
- **Context:** effect or voice.
- **The rate of each input:**
  - dial: a 20 ms glide, flat when still;
  - per-sample CV in a law (I_abc, a VCA gain);
  - per-sample resistance (LDR, JFET as a resistor);
  - switch: an int knob.

### 6.2 The rule table

The self-test measures the candidates each row names.

| Block | Solver and guess | K tried | OS tried | ADAA | Integrator, precision, pots |
|---|---|---|---|---|---|
| M = 0 (linear) | none | – | 1x only with a magnitude-matched design (bilinear prewarped at the highest pole, or MZT/Vicanek-style, from the analog H(s, pots)); otherwise 2x TR | – | coefficients as closed-form functions of the pots (Lcapy/SymPy at compile time, §8.6); dials by grid, audio-rate R by pot-as-port |
| M = 1, EA | ω3 + Newton, kept in the bracket [min(0, p), max(0, p)] | 1, 2 | 1, 2, 4, 8 | yes (closed-form F) | TR; offset form if biased |
| M = 1, SM | previous sample + Newton | 1, 2, 3 | 1, 2, 4 | yes (F with log cosh) | TR. Precedent: a measured tanh ladder (unpublished), 2 Newton steps within −50 dB at 48 kHz [H] |
| M = 2–4 with a structural chain (some K_jj = 0, or K triangular after BLT) | both the full M-D Newton and the torn 1-D Newton, from the previous sample | 1, 2, 3 | 1, 2, 4 | no | pick by measured cycles; on an out-of-order core the M-D form often wins (§8.4) |
| M = 2–4, MP dense (BJT stages, global feedback) | M-D Newton from the previous sample, junction steps capped at 4nV_t, junction capacitances added (with a note) if the model lacks them | 2–8 | 4, 8 | no | offset form, mandatory when biased; a table instead if d ≤ 2 (K-method, file-scope const) |
| M ≥ 5 | decompose first (BLT, then Holters' F_q decomposition), then apply the rows above to each sub-block | – | – | – | if a sub-block keeps M ≥ 5: warn, and use M-D Newton with K = 8 |

**Integrator.** TR by default. BDF2 is a candidate only when the eigenvalue check flags ringing *and* the 64x comparison
shows excess energy above 20 kHz ([fminus §5](notes/fminus.md#5-c-the-pipeline), stage 7).

**Controls.**
- **Dials:** Woodbury on the absolute 16-sample grid; a frozen dial folds into the constants.
- **Audio-rate resistances:** pot-as-port, `(R(t) + Z_rr)⁻¹` per sample ([opt.
  §3.5](notes/optimization.md#35-pots-rank-updates-instead-of-re-inversion)) [M].
- **Controlled-source gains (I_abc, a VCA):** enter the device law, with no matrix change (§7) [H].
- **Switches:** ramped resistors (rank 1), or int knobs with up to 8 versions ([knob
  versioning](../../extending.md#recipe-16-knob-versioning-and-flat-streams)).

**Order of trial.** Candidates run cheapest first by the §3.2 model, scaled by M. The first one to pass T1–T4 wins, and
its measured cycles are recorded in the notes.

### 6.3 Gates

T1–T4 on the stress matrix of §2.

### 6.4 The run-time guard, emitted for every block

- **1-port blocks:** the bracket clamp keeps every iterate finite and the solution physical, at no extra cost [H].
- **Multi-port blocks:**
  - Each junction voltage is clamped to [−V_rail, V_jmax]. V_jmax is the largest value seen on the stress set, plus a
    margin.
  - On the absolute 16-sample grid the block checks that its states are finite. If one is not, it resets to its DC
    operating point and fades its output in over 20 ms. The reset is deterministic and does not depend on how the host
    splits blocks, because it happens on the grid.
- **Last line:** the Engine's ±4 output guard stays.

### 6.5 When no configuration meets the target

The compiler emits the most accurate configuration under a cost cap, taking the largest K and OS that fit. For an effect
the cap is 4 % of a core per channel at 48 kHz, about 3,500 cycles at 4.3 GHz [L]. It adds a warning that names the
worst stress points:

`pedal.cir:3:1: warning: block 2 (Q1, Q2): no configuration meets -60 dB at 100 mV (best: 8x, K=8, -27 dB); using it`

The Fuzz Face lands here today (§8.2).

### 6.6 What the self-test costs, and when it runs

Measured on the TS block, single-threaded, in C++ ([`selftest.cpp`](experiments/selftest.cpp),
[`selftest.out`](experiments/selftest.out)) [H].

| Step | Time |
|---|---|
| converged 64x double reference, 3 drives × 17 signals (8192-point windows, 0.68 s of audio each) | **15.2 s** at load 3–4; 26.0 s at load 6–8 |
| one candidate on all 51 signals | 0.35 s (1x) to 5.6 s (8x ADAA) |
| exhaustive search over 24 candidates | 41–55 s |
| ordered search to the first pass (4x ADAA ω + 1) | 17.7 s after the reference |

[fminus.md](notes/fminus.md#5-c-the-pipeline)'s "tens to hundreds of ms" was off by two orders of magnitude.

**Ruling.** The self-test never blocks a compile.
- The compile emits a provisional configuration at once, from the rule table's default row (EA: 4x, ω + 2, no ADAA; MP:
  8x, K = 6).
- The self-test runs **asynchronously** on worker threads. The 51 reference jobs are independent, so on 10 cores the
  reference takes ~2–3 s [M].
- The result is cached by netlist hash and pot set. When it lands, the program recompiles with the chosen configuration,
  as a [frozen-dial](../../extending.md#recipe-14-parameters-the-hosts-values) recompile does, and crossfades.
- Frozen recompiles reuse the cache. This answers [optimization §9 Q7](notes/optimization.md#9-open-questions).
- A 32x reference started from ω, which converges in a few steps (the fix in §9.7), would roughly halve the reference
  time [M].

---

## 7. The synth path

### 7.1 Example 3: an OTA state-variable filter (LM13700-style), as a netlist

```
* otasvf.cir: 2-pole OTA SVF voice filter. Cutoff by Iabc (a per-sample CV), resonance a pot.
.title OtaSvf
.input  u    in   scale=1
.input  cv   -    kind=current      ; Iabc in amps, a per-sample stream (the expo converter is the program's)
.output lp   l
.output bp   b
.pot    res  0.5 lin
E1  na  0   in  l   1               ; summer, ideal: hp = in - lp - kq bp
E2  nb  0   b   0   {2.0 - 1.96*res}  ; kq: Q 0.5 .. 25
E3  hp  0   na  nb  1
R1  hp  a1  100k
R2  a1  0   220                     ; OTA input attenuator (x 2.2e-3)
G1  0   b   OTA a1 0 {cv}           ; Iout = Iabc tanh(Vd / 2Vt), into b
C1  b   0   1n
R3  b   0   10meg                   ; OTA output resistance
E4  bb  0   b   0   1               ; buffer
R4  bb  a2  100k
R5  a2  0   220
G2  0   l   OTA a2 0 {cv}
C2  l   0   1n
R6  l   0   10meg
.end
```

The netlist needs three new elements:
- `G … OTA cp cn {gain}`: a VCCS with `I = gain·tanh(V_d/2V_t)`. This is the macro a review of the report's first draft
  asked for, and it keeps M = 2. A netlist-level LM13700 would need 2 differential-pair BJTs per OTA, about 8 ports [M].
- `E` sources.
- `.input … kind=current`: an input that enters a device law instead of driving a source.

### 7.2 The DK form the compiler emits

Derived by hand. It matches [dk.hpp](experiments/dk.hpp)'s MNA exactly: K11 −3.42817, K12 −11.4272, K21 11.4272, K22 0
[H].

```
p1 = att (u − h2/G2 − kq h1/G1),  p2 = att h1/G1           att = 220/100220, G = 2C fs + 1/10M
v1 = p1 + K11 i1 + K12 i2,        v2 = p2 + K21 i1          K11 = −att kq/G1, K12 = −att/G2, K21 = att/G1, K22 = 0
i_j = I_abc tanh(v_j / 2Vt);  bp = (i1 + h1)/G1, lp = (i2 + h2)/G2;  h ← 2 gC v − h
```

I_abc appears only in `i_j`. A cutoff that moves every sample therefore costs nothing in the matrices: no Woodbury, no
pot-as-port. That is the common synth case.

### 7.3 Accuracy

From [`ota.cpp`](experiments/ota.cpp) ([`ota.out`](experiments/ota.out)): float with a fixed K, against a converged
double solve at the same rate [H].

| Case (1x unless stated) | Converged iterations, 2-D / torn | Float, previous-sample guess: torn K1, K2 / 2-D K1, K2 | With per-sample cutoff modulation (±2 oct, 37 Hz): torn K1, K2 / 2-D K1, K2 | Alias (saw) | vs 64x |
|---|---|---|---|---|---|
| fc 1 kHz, Q 0.7, 1 V saw | 2.42 / 2.32 | −105, −124 / −109, −124 | −97, −119 / −103, −119 | −127 dB | −63 dB |
| fc 1 kHz, Q 10, 10 V saw | 3.34 / 3.06 | −80, −107 / −54, −108 | −59, −106 / −38, −104 | −77 dB | −41 dB |
| fc 4 kHz, Q 25, 5 V saw | 3.37 / 3.22 | −49, −111 / −32, −115 | −22, −81 / −20, −93 | −74 dB | −9.5 dB |
| same at 2x | 3.23 / 3.04 | −74, −105 / −49, −105 | −38, −101 / −32, −99 | −162 dB | −21 dB |
| self-oscillation (kq −0.02), 1 V saw | 4.17 / 3.64 | frequency and level identical in float and double (1517.6 Hz, 52.6 V rms) | – | −81 dB | – |

- **Two steps are enough everywhere tested, with or without modulation.** One torn step is enough up to moderate Q.
- **Aliasing is low.** The OTA's input attenuator keeps tanh nearly linear except under heavy drive, so 1x often meets
  T2.
- **The error against 64x is warping, not aliasing.** At fc 4 kHz the trapezoidal rule moves a Q = 25 resonance enough
  to give −9.5 dB at 1x. The compiler should either prewarp the cutoff law, `I_abc ← I_abc·tan(π f_c/f_s)/(π f_c/f_s)`
  with f_c ∝ I_abc (one `tanf` per sample, off the chain) [L, untested], or run at 2x.
- **The macro model lacks rails** [H for the symptom, L for the fix]. Self-oscillation settles at 52 V rms, limited only
  by I_abc; a real filter clips at its buffers' rails. The synth library needs a soft-clipping op-amp (Holters 2015's
  `V_max tanh`) on the buffers and the summer before self-oscillation sounds right.
- **Waveform nulls are meaningless for self-oscillation**, because float and double trajectories drift apart. The
  self-test checks frequency and level instead.

### 7.4 Cost per voice

FMinus c75caab ([`bench_ota.fm`](experiments/bench_ota.fm), [`timing-ota.out`](experiments/timing-ota.out)). One voice
is a saw oscillator, an expo converter (`expf`) and the filter [H].

| Solver (2 steps unless stated) | Scalar 1x | Scalar 2x (IIR decimator) | float4 bank 1x, per voice | float4 bank 2x, per voice |
|---|---|---|---|---|
| 2-D Newton | **248** | 494 | **47** | 92 |
| torn, exact final currents (6 tanh) | 376 | 751 | 76 | 153 |
| torn, linearised final currents (4 tanh) | 313 | 627 | 71 | 142 |
| torn, 1 step | 165 | – | 41 | – |

Sixteen voices of the scalar 2-D filter at 1x take 16 × 48,000 × 248 = 190 M cycles/s, 4.4 % of a 4.3 GHz core. With a
bank, 0.8 %. [Optimization §1.3](notes/optimization.md#13-the-budget) allowed about 300 cycles per voice-subsample, a
quarter of a core at 4x. The 2-D filter costs about 250 per subsample, so it fits that budget scalar.

### 7.5 What the phase plan needs for synths

- **Phase 1 (+2 agent-days):**
  - the `OTA` VCCS macro and the `E` and `G` controlled sources;
  - `.input kind=current|cv` streams into device laws;
  - the expo converter as library FMinus (one `expf` per sample), not as a circuit;
  - the cutoff-prewarp option;
  - the self-oscillation stress test (frequency and level).
- **Phase 2 (+3):**
  - the soft-clipping op-amp on buffers and summers;
  - a transistor or diode ladder as a 4-port tanh chain, with both the torn and the full candidate;
  - a stability check under per-sample modulation. McClellan DAFx-26 gives CQLF bounds for linear SVF, Sallen-Key and
    ladder filters [M].
- **Later:** BJT-level OTA and expo-converter models, for character studies.

### 7.6 The voice-bank SIMD option and its host change ([optimization §9 Q1](notes/optimization.md#9-open-questions))

**Today.** The playground plays one `Voice` instance per note, so voice code is scalar.

**The option.** A `VoiceBank` contract: one FMinus instance holds 4 voices in `float4` lanes and receives a lane mask
and per-lane note events.

**The host change.**
- The playground's voice allocator assigns notes to lanes.
- It sums the lanes in a fixed order, to stay deterministic.
- It steals voices per lane.
- A per-lane `start` resets only that lane's states, by select.

**The gain and the compiler's part.** 4.4–5.3× per voice on the OTA SVF [H]. The circuit compiler would emit a bank
variant (`.channels 4` in a voice context) beside the scalar one [L].

---

## 8. Shrinking the nonlinear solve: BLT and tearing, Holters' decomposition, blockwise

### 8.1 The three approaches

**BLT plus tearing**, from equation-based compilers [H for the definitions, M for applicability].
- **Sorting:** a matching and Tarjan's strongly connected components order the equations into blocks (BLT).
- **Tearing:** inside each block it picks a small set z of tearing variables. Given z, the other unknowns are computed
  one equation at a time, each explicitly, and only the residual system r(z) = 0 is iterated. The definition here is
  from Baharev, Neumaier & Schichl (Modelica 2017), who cite Elmqvist & Otter (ESM'94, pp. 326–332).
- **In practice:** ModelingToolkit runs alias elimination, Pantelides index reduction with dummy derivatives, BLT
  (`but_ordered_incidence`) and `tearing` inside `mtkcompile` (docs, internals/structural_transformation). Jorissen,
  Wetter & Helsen (J. Building Perf. Sim. 2018) split a Modelica model's algebraic loop of 86 iteration variables into
  parts of at most 5 [M, abstract].
- **Caution:** Baharev et al. show that tearing can return wrong results without warning, or fail, because the torn
  triangular solve can be ill-conditioned [H, abstract and introduction].

**Holters–Zölzer exact decomposition** (DAFx-17) [H via
[optimization.md](notes/optimization.md#32-keeping-m-small-decomposition-sim-model-choice)]. It finds a regular R that
makes F_q·R block lower-triangular, checked in exact rationals. It is **not purely structural**: because it recombines
equations linearly, it can split blocks that BLT sees as one.

**PedalKernel's blockwise decomposition** [M, its docs].
- **What it does:** nonlinear devices that sit in separate subtrees joined only by linear or reactive passives each
  become a WDF stage with its own root. Its example: a "4×4 NR system" becomes "four cascaded single-port solves".
- **Exactness:** the coupling is delay-free by default, and the docs call any deviation from the monolithic solve "a
  bug". They do not show that it is exact.
- **Approximate modes (opt-in):** a one-sample delay (`force_serial_blockwise`), or a table-driven fixed point.
- **Validation:** its own suite fails its Big Muff stage and its fuzz core (91/100).

### 8.2 The Fuzz Face

NPN silicon, 4 ports [H for the runs; M for the values, from coda-effects' topology]. The runs are
[`bjt.cpp`](experiments/bjt.cpp)'s (`./bjt fuzz`, `./bjt fuzzcaps`; [`bjt.out`](experiments/bjt.out),
[`bjt-caps.out`](experiments/bjt-caps.out)).

K in V/A, ports ordered Q1:BE, Q1:CE, Q2:BE, Q2:CE:
```
without junction caps:                    with them (20 pF B–E, 5 pF B–C):
 −994.8     0        0.9989   0.9989        −992.4  −14.41  −13.41   0.9413
  0       −3.3e4   −3.3e4     0             −14.41  −3.0e4  −3.0e4  −115.2
  0.9989  −3.3e4   −3.31e4  −100.4          −13.41  −3.0e4  −3.0e4  −215.6
  0.9989    0      −100.4   −8498            0.9413 −115.2  −215.6   −8465
```

What each approach can do here:
- **BLT:** each BJT's two currents depend on both of its ports, so every equation reaches every port. The result is one
  block of 4.
- **Holters' decomposition:** separates nothing, as Holters reports for circuits with global feedback (here the 100 k
  from Q2's emitter to Q1's base).
- **Tearing:** the incidence is complete, so the smallest tear set is 3 of 4 [M, argued]. Tearing before elimination, on
  node voltages, also gives 3: tear V_b1, V_c1 and V_e2, then get V_c2 by ω from Q2's collector KCL [M, argued, not
  run].
- **Blockwise:** does not apply, since no coupling is passive-only.

Convergence on a 1 kHz sine. The reference is damped Newton with capped junction steps, run to 1e-10 V.

| Rate | Reference iterations, mean (no caps / caps) | Best float with fixed K: offset form, previous-sample guess, capped steps (caps) |
|---|---|---|
| 1x | 118 / 10.0 (10 mV), 93 / 12.1 (100 mV) | ≥ −7 dB at any K ≤ 8 |
| 4x | 43 / 4.5, 34 / 4.8 | 10 mV: −20 dB at K8; 100 mV: −3 dB |
| 8x | 25 / 3.8, 22 / 4.2 | 10 mV: **−77.6 dB at K6**; 100 mV and 1 V: fails (NaN or > 0 dB) |

The linearised extrapolated guess (Holters EUSIPCO-16) diverged to NaN everywhere here. With β = 200, the silicon Fuzz
Face turns 10 mV into a 3.4 V rms square wave.

What follows:
1. A fixed-K Newton is not enough for this class of circuit.
2. Phase 2 should add the junction capacitances by default. It should try a per-port ω guess for each junction [L,
   untested], and accept a "no configuration meets the target" outcome at guitar level (§6.5).
3. [fminus.md](notes/fminus.md#10-phased-plan)'s estimate of 3,000–4,000 cycles for a Fuzz Face at 4x is low. At 8x with
   K6 the estimate is about 8 × 6 × 150 ≈ 7,200 cycles [M].

### 8.3 The SD-1/SuperOver (8 equations)

[H via [optimization.md](notes/optimization.md#32-keeping-m-small-decomposition-sim-model-choice) and
[methods.md](notes/methods.md#35-making-the-nonlinear-solve-cheap-measured-levers); not rerun.]
- **Holters' decomposition** found groups of 1, 2, 3 and 2, with index dimensions 0/2/1/2 instead of 5. Mean iterations
  went from 2.965 to 1.460/2.924/1.696, but run time only from 6.82 to 6.79 s, because of LAPACK overhead. In unrolled
  code the M³ term would drop from 512 to 44 [M].
- **Holters' k-d cache** on the same circuit gave 2.31 iterations on average, with 0.14 % of samples above 5 and a worst
  case of 500.
- **BLT** on its K would find no more than Holters, whose method is a superset because it allows recombination. Tearing
  would then act inside the 2- and 3-groups [M].
- **The bias caveat:** this all needed an idealised bias (an ideal 4.5 V source). With the real bias network only the
  supply diode separates.

### 8.4 The OTA SVF and ladders: tearing to 1-D, measured [H]

Because K22 = 0, v2 follows explicitly from i1. The torn residual
`r(v1) = v1 − p1 − K11 i1(v1) − K12 i2(p2 + K21 i1(v1))` is therefore scalar.
- **Iterations:** tearing converges in slightly fewer (2.3–3.6 against 2.4–4.2).
- **Accuracy:** at 1 step the torn solve is much more accurate (−80 against −54 dB at 10 V). At 2 steps the two are
  equal.
- **Cost:** the torn solve is slower, 313–376 against 248 cycles, because it chains the two tanh evaluations that the
  2-D Newton evaluates in parallel.

A transistor ladder (4 tanh in a chain with one feedback path) tears to 1 variable, which is Zavalishin's
zero-delay-feedback solution [M]. The same trade applies there: 4 serial tanh at about 220 cycles per iteration, against
about 60 for 4 parallel tanh plus a 4×4 solve.

### 8.5 Recommendation

- **In the pipeline, after DK reduction (stage 6):**
  1. BLT on the port incidence of K and of the device Jacobians (cheap, exact).
  2. Holters' F_q decomposition per block (exact rationals).
  3. A tear set per remaining block; Cellier-style heuristics suffice at M ≤ 8.
- **For M = 2–4, emit both the torn and the full solve,** and let the self-test choose by measured cycles.
- **Use tearing only when it is safe:** every torn equation must be explicit (solvable through its law: ω, a log, an
  affine step), and the self-test must confirm conditioning (Baharev et al.'s failure modes).
- **No silent approximations:** do not adopt blockwise decompositions that delay the coupling unless the user asks, and
  then report the error.

### 8.6 Linear blocks in closed form

Lcapy (Hayes, PeerJ CS 8:e875, 2022) parses SPICE-like netlists and gives symbolic transfer functions through SymPy [M,
docs]. The compiler can use the same approach in C++, or at design time, to emit a pot-parameterised biquad or TPT
cascade for M = 0 blocks; Yeh's Fender tone stack is the precedent ([opt.
§2.3](notes/optimization.md#23-model-order-reduction-balanced-truncation-and-its-limits)). This is also the route to the
magnitude-matched 1x designs that §9.2 requires.

---

## 9. Verification status of the worked examples

### 9.1 Example 2's tone stage against Yeh DAFx-07, eq. 24 [H]

[`tone.cpp`](experiments/tone.cpp) compares the netlist's small-signal response (ideal op-amp) with eq. 24, read from
the paper during this research. Eq. 24's W, X, Y, ω_z and ω_p are used as printed, with R_l = T·20k, R_r = (1 − T)·20k,
R_z 220, C_z 0.22 µ, R_i 10k, R_s 1k, C_s 0.22 µ and R_f 1k.

**The topology is right:** the difference is **≤ 7e-15 dB** at T = 0, 0.5 and 1, provided the pot's section on the input
side is R_l = T·20k.

**The pot was wired backwards.** The netlist line `Ptone t1 tw tn 20k {tone}`, under
[fminus.md](notes/fminus.md#4-b-the-syntax)'s convention R(e1–w) = (1 − t)R, puts (1 − tone)·20k on the input side. That
reverses the knob: the two orientations differ by 16.4 dB at T = 0 and 1, and tone = 1 was dark. The corrected line
(tone = 1 bright; T = 1 gives +0.5 dB at 1 kHz and −3.6 dB at 5 kHz):
```
Ptone  tn  tw  t1  20k {tone}       ; R(tw-t1) = tone*20k on the + input side, as Yeh's R_l = T*20k
```

### 9.2 1x is not free for linear blocks [H]

Error of the trapezoidal tone block against its analog response ([`tone2.cpp`](experiments/tone2.cpp)):

| Rate | Worst error to 5 kHz | to 10 kHz | to 20 kHz |
|---|---|---|---|
| 48 kHz | 0.20–0.30 dB | 1.08–1.36 dB | 8.7–9.0 dB |
| 96 kHz | 0.05–0.07 | 0.24–0.31 | 1.28–1.37 |

This is bilinear warping; at 20 kHz the analog response is already −28 dB. Hence the M = 0 row of §6.2: a
magnitude-matched design at 1x, otherwise 2x. [fminus.md](notes/fminus.md#4-b-the-syntax)'s "blocks 3 and 4 at 1x" holds
for block 4 (a 15.9 Hz highpass). Block 3 needs the matched design.

### 9.3 Float on biased circuits

From [`bjt.cpp`](experiments/bjt.cpp) (`./bjt range`, [`bjt-range.out`](experiments/bjt-range.out)), on the Rangemaster
topology at 9 V: 470k/68k bias, 3.9k ∥ 47 µF emitter, 10k collector, 5 nF in, 10 nF out [H; values M]. Float with a
converged solve, against double; silicon is I_s 1e-14, germanium-like is I_s 5 µA.

| Rate, input | Silicon: absolute / offset | Germanium-like: absolute / offset |
|---|---|---|
| 1x, 10 mV | −77.3 / **−133.0** | −77.8 / −129.3 |
| 1x, 100 mV | −76.3 / −103.8 | −85.4 / −123.9 |
| 1x, 1 V | −68.3 / −71.8 | −87.3 / −78.5 |
| 4x, 10 mV | −70.8 / −132.1 | −75.5 / −126.1 |
| 4x, 100 mV | −64.0 / −89.6 | −74.8 / −115.8 |
| 8x, 10 mV | **−53.7** / −120.3 | −69.1 / −119.6 |
| 8x, 1 V | −45.5 / −55.1 | −61.0 / −62.9 |

**Absolute states** get worse as the rate rises: a larger g_C means larger history states and poles nearer z = 1, the
mechanisms in [optimization §5.2](notes/optimization.md#52-float-versus-double). They fail T1 at 8x even at 10 mV.

**The offset formulation** stores `x − x₀` and computes each law as `i(v₀ + Δv) − i(v₀)`, with two rules:
- A junction that conducts at the operating point uses `e₀·expm1(Δv/nV_t)`. FMinus has no `expm1f`, so it uses
  `2t/(1 − t)` with `t = tanhf(x/2)` for |x| < 10, and `expf(x) − 1` beyond.
- A junction that is reverse-biased there (e₀ < 1e-12) uses `exp(v₀/nV_t + Δv/nV_t) − e₀`. A first version without this
  split produced NaN at saturation, from 0·∞.

**At hard drive** neither form meets T1 (−55 to −87 dB), and the fixed-K Newton is the limit:
- at 1 V, capped steps from the previous sample with K ≤ 4 give about 0 dB at every rate;
- at 100 mV, 1x needs K4 and still fails (−45.7 dB), while 4x needs K2 (−89.6 with offsets).

### 9.4 The Fuzz Face

See §8.2.

### 9.5 SPICE goldens: the next step

ngspice was not installed for this research. Every number in this report therefore compares a model against the same
model in double at 64x, or against a tighter solve. That validates the implementation, not the circuit or the device
model.

The next step, once ngspice is installed (`brew install ngspice` on macOS), is a script that:
1. writes each `.cir` as a plain SPICE deck, with `*fm` lines ignored, pots at {0, 0.5, 1} and the stress sines as PWL
   sources;
2. runs `.tran` with `RELTOL=1e-6 VNTOL=1e-9 ABSTOL=1e-15` and `TMAX ≤ 1/(16·fs)`, starting from `.op` (no `uic`);
3. writes 64x band-limited goldens, plus an `.op` check;
4. gates on PedalKernel-style metrics: RMS, audio-band spectrum, and THD against level ([opt.
   §6.1](notes/optimization.md#61-reference), [landscape §9](notes/landscape.md#9-test-circuits-and-reference-data))
   [M].

### 9.6 FMinus constant data [H]

From `fminus --emit ll` on c75caab, on [`lc.fm`](experiments/lc.fm):
- A function-local `const float t[8] = {…}` **indexed at run time** compiles to an `alloca` plus 8 stores *on every
  call*.
- The same array with **constant indices** folds to immediates: `tapsum` became 4 multiplies by literals.

So halfband taps and emitted coefficients need no change. Γ(p) tables, DC-point grids and K-method tables need
file-scope `const` arrays, or module fields filled in `init` (one copy per instance). This corrects [fminus.md
§7](notes/fminus.md#7-d-what-fminus-gives-for-free-and-what-it-lacks)'s "no constant data" row.

### 9.7 Two harness fixes that change the method

1. **The double reference could stop short.** In [`clip.cpp`](experiments/clip.cpp),
   [`clip2.cpp`](experiments/clip2.cpp) and [`clip3.cpp`](experiments/clip3.cpp), the "converged" double reference
   starts Newton from the previous sample. After a large jump in p it needs about (|p| − 0.6 V)/nV_t steps; for |p| ≈ 13
   V that is about 270 of its 300. On the TS stage's noise burst at 1x it stopped short, at v = −2.2 V on a diode.
   Started from ω, it converges in a few steps. The notes' sine tables are unaffected: ω + 2 matched them to −92 to −114
   dB, and the rerun reproduced Tables 1–3 identically. **The compiler's reference must start from the closed form, or
   use homotopy.**
2. **A tone at an integer fraction of the rate hides its aliases.** A 3000.0 Hz tone on a power-of-two FFT at 48 kHz
   (bin 512 of 8192) puts every alias on a harmonic bin, because 96000/3000 is an integer. The alias then read −62 dB
   where it was −53 dB. Stress tones go on odd bins, and the playground's proposed `alias_db` metric needs the same
   care.

---

## 10. The compiler in brief, as revised

This summarises [fminus.md §3–5](notes/fminus.md#3-a-where-the-compiler-lives), which still holds [M].

**Where.** `include/fminus/Circuit.h` and `src/circuit/` in the FMinus repository: C++23 inside, a C++17 header, no
LLVM. The entry point is `generate(netlist, fileName, options) → {source, errors, notes}`. The playground runs it on
each `.cir` file in `Sources`.

**Syntax.** A SPICE subset plus `.input`, `.output`, `.pot`, `.oversample`, `.target`, `.method`, `.solver`, `.op` and
`.channels`. Changes from this report:
- `.target solver=-60dB alias=-60dB level=0.45V freq=3k` replaces `alias=-50dB` (§2);
- `.adaa auto|on|off`;
- `E` and `G` controlled sources, with `G … OTA` as a macro;
- `.input … kind=cv|current` for inputs that enter a device law;
- `.channels 4` emits a voice bank where a host supports one;
- the tone pot orientation of §9.1.

**Pipeline.** [fminus §5](notes/fminus.md#5-c-the-pipeline)'s 13 stages, with three changes:
- stage 6 gains BLT and tearing (§8.5);
- stage 8 (solver choice) becomes §6's table, stress matrix and asynchronous cached self-test, with a reference started
  from ω;
- stage 9 (oversampling) runs M = 0 blocks at 1x only with a matched design.

**Generated interface.** Unchanged: `init(rate)`, `set(pots…)` on the 16-sample grid, `tick(in)`, `step`, `clear()`, and
fields named after the parts.

---

## 11. Phased plan, revised

| Phase | Deliverables | Agent-days | Changes from [fminus.md §10](notes/fminus.md#10-phased-plan) |
|---|---|---|---|
| 0. Spike | 1-port generator; both examples through `fminus --emit fm`; this report's tables reproduced from emitted code | 2 | – |
| 1. First slice | [fminus.md](notes/fminus.md#10-phased-plan)'s Phase 1, plus: the ADAA candidate with closed-form F (+1); stress matrix, asynchronous self-test with cache, run-time guard and no-config warning (+2); matched design for linear blocks at 1x (+1); `OTA`, `E` and `G` elements, law inputs and the prewarp option (+2) | **16** (was 10) | ADAA moved in from Phase 4; the self-test grows from "tens of ms" to an asynchronous job |
| 2. Transistors and op-amp models | [fminus.md](notes/fminus.md#10-phased-plan)'s Phase 2, plus: junction capacitances by default; the offset formulation, mandatory; BLT, decomposition and tearing, emitting both solvers; the soft-rail op-amp; the ladder as a 4-port tanh chain | **13** (was 9) | the Fuzz Face may end in the no-config outcome; budget about 7,000 cycles at 8x |
| 3. Tubes, tables and FMinus additions | [fminus.md](notes/fminus.md#10-phased-plan)'s Phase 3, plus `expm1f` (0.5); `omegaf` stays | 10.5 | – |
| 4. Voice bank (host) and A1 sugar | the `VoiceBank` contract and allocator (§7.6), `.channels 4`; the `circuit` block sugar | 5–8 | makes voice SIMD explicit |

Total: about 47–50 agent-days to the end of Phase 4 [L].

---

## 12. Open questions

1. **T3's tolerance.** Is ±0.5 dB at H3 right? At ±1 dB, ADAA at 2x passes on example 1 and halves its cost. This needs
   a listening test [L].
2. **The voice bank.** Will the playground adopt the `VoiceBank` contract (§7.6), and with how many lanes (4 on NEON, 8
   on AVX2)?
3. **Fuzz-class circuits at guitar level.** Accept the no-config warning, or pay for more: K up to 12, 16x, or a
   per-junction ω guess (untested)?
4. **The ordered search.** Is it acceptable that it returns the first configuration that passes rather than the cheapest
   one? Candidates run cheapest-first by the §3.2 model, so the two coincide when the model ranks them correctly.
5. **Cutoff prewarp.** Should it be on by default for synth filters, given that it changes the CV law?
6. **Still unverified** (each marked in place):
   - circuit values (Fuzz Face, Rangemaster) against real schematics;
   - any SPICE comparison;
   - the ladder and op-amp filter costs in §3.3;
   - Carson's two-port ADAA;
   - the pre-elimination tear set for the Fuzz Face;
   - whether PedalKernel's blockwise decomposition is exact;
   - H2–H5 in T3.

---

## 13. Sources

Read during this research, in full unless marked:
- M. Holters, "Antiderivative Antialiasing for Stateful Systems", Appl. Sci. 10(1):20, 2020.
  https://www.hsu-hh.de/ant/wp-content/uploads/sites/699/2020/10/applsci-10-00020-v2.pdf [H]
- D. Albertini, A. Bernardini, A. Sarti, "Antiderivative Antialiasing in Nonlinear Wave Digital Filters", DAFx-20.
  https://re.public.polimi.it/retrieve/e0c31c10-6d11-4599-e053-1705fe0aef77/DAFx2020_paper_35.pdf [H]
- Follow-up: "Antiderivative Antialiasing Techniques in Nonlinear Wave Digital Structures", JAES 69(7/8):448–464, 2021
  (pth order; one-port and multiport). https://re.public.polimi.it/handle/11311/1208018 [M, abstract]
- K. J. Werner, "An Equivalent Circuit Interpretation of Antiderivative Antialiasing", DAFx-20in21.
  https://dafx2020.mdw.ac.at/proceedings/papers/DAFx20in21_paper_42.pdf [H]. Carson's second-order and two-port
  extension is cited there [M].
- D. T. Yeh, J. S. Abel, J. O. Smith, "Simplified, Physically-Informed Models of Distortion and Overdrive Guitar Effects
  Pedals", DAFx-07, §4.2 (eqs. 20–23) and §4.3 (eq. 24, fig. 15). https://dafx.de/paper-archive/2007/Papers/p189.pdf [H]
- A. Baharev, A. Neumaier, H. Schichl, "Failure Modes of Tearing and a Novel Robust Approach", Modelica 2017.
  https://www.mat.univie.ac.at/~herman/fwf-P27891-N32/failure_modes_of_tearing.pdf [H, abstract and §1]
- ModelingToolkit, structural transformation internals.
  https://docs.sciml.ai/ModelingToolkit/stable/internals/structural_transformation/ [H]
- H. Elmqvist, M. Otter, "Methods for Tearing Systems of Equations in Object-Oriented Modeling", ESM'94, pp. 326–332.
  https://elib.dlr.de/28159/ [M, bibliographic only]
- F. Jorissen, M. Wetter, L. Helsen, "Simplifications for hydronic system models in Modelica", J. Building Perf.
  Sim., 2018. https://energy.lbl.gov/publications/simplifications-hydronic-system [M, abstract via search]
- PedalKernel, compiler internals. https://pedalkernel.com/book/compiler-internals/ [M]
- Lcapy: https://lcapy.readthedocs.io; M. Hayes, PeerJ CS 8:e875, 2022, https://peerj.com/articles/cs-875 [M]
- Fuzz Face topology (the 100 k runs from Q2's emitter to Q1's base).
  https://coda-effects.com/p/circuit-analysis-fuzz-face.html [M, via search]
- The HIIR polyphase IIR halfband design (L. de Soras, WTFPL). The algorithm was reimplemented from memory in
  [`gen.cpp`](experiments/gen.cpp), and the responses were checked by simulation [H for the responses, L for
  provenance].
- FMinus c75caab: [`include/fminus/FMinus.h`](../../../include/fminus/FMinus.h),
  [`docs/extending.md`](../../extending.md) ([recipe 24](../../extending.md#recipe-24-the-math-kernels-inline)),
  [`tools/fminus/main.cpp`](../../../tools/fminus/main.cpp) [H].

From the notes, with the tags they carry there: [optimization.md](notes/optimization.md#sources) [S1]–[S36],
[landscape.md §§2–9](notes/landscape.md#2-tool-by-tool), [methods.md §§2–7](notes/methods.md#2-discretisation) and
[fminus.md §11](notes/fminus.md#11-sources-and-confidence). This report leans most on:
- Holters & Zölzer: DAFx-11, EUSIPCO-15, EUSIPCO-16, DAFx-17;
- Holmes & van Walstijn, DAFx-15; Holmes, Holters & van Walstijn, DAFx-17;
- D'Angelo et al., DAFx-19;
- Chowdhury 2022 (chowdsp_wdf); Chowdhury & Rau, DAFx-26 (wdf_compiler);
- Yeh, Abel & Smith, TASLP 2010.

---

## Appendix A: experiments (reproducible)

The experiments' sources, and the results this report quotes, are in [experiments/](experiments/); its
[README](experiments/README.md) lists each file, what it measures, and how to build and run it.

**The FMinus build:**
- `fminus` is c75caab's `tools/fminus/main.cpp`, taken with `git archive`.
- `fmbench` is [`fmbench.cpp`](experiments/fmbench.cpp), an in-process timer that reads cycles and instructions from
  `proc_pid_rusage` and runs at QoS user-interactive.
- Both link against `libfminus.a` and `libfminus_runtime.a` from a TestingPlayground build tree (its FetchContent build
  of FMinus, at c75caab), and against FMinus's LLVM 23.1.3 Release-assert install.

**The experiments:**
- [`adaa.cpp`](experiments/adaa.cpp): §5, and Tables 1–3 with pots at {0, 0.5, 1}. `./adaa` prints every case;
  `./adaa worst` prints the worst-case table. The reference starts from ω (§9.7).
- [`selftest.cpp`](experiments/selftest.cpp): §6.6's timing and the stress-matrix pass/fail.
- [`gen.cpp`](experiments/gen.cpp): designs the FIR and IIR halfbands and writes `halfbands.fm` and
  [`hb-coefs.txt`](experiments/hb-coefs.txt).
- [`mkbench.py`](experiments/mkbench.py): writes `bench_dt.fm`, `bench_ts.fm`, `bench_tsfull.fm` and `bench_dt2.fm`.
  [`bench_ota.fm`](experiments/bench_ota.fm) (§7.4) was generated by an inline script; the file is in the folder.
- [`timing1.out`](experiments/timing1.out) and [`timing-ota.out`](experiments/timing-ota.out): every cycle count in §3–4
  and §7.4.
- [`tone.cpp`](experiments/tone.cpp) and [`tone2.cpp`](experiments/tone2.cpp): §9.1–9.2.
- [`dk.hpp`](experiments/dk.hpp): a netlist → MNA → DK compiler in double. Elements: R, C, V, E, ideal op-amps, and
  ports (diode pair, Ebers–Moll BJT, OTA). It includes offset-form laws.
- [`bjt.cpp`](experiments/bjt.cpp): §9.3 (`./bjt range`) and §8.2 (`./bjt fuzz`, `./bjt fuzzcaps`).
- [`ota.cpp`](experiments/ota.cpp): §7.3, with the cross-check of the hand DK form against dk.hpp.

**Builds and settings.** C++ was compiled with `clang++ -std=c++20 -O2 -ffp-contract=off` (Apple clang 21, M5). The
FMinus runs used `Denormals::Flush`, one thread, and the best of 7 × 10⁶ host samples.
