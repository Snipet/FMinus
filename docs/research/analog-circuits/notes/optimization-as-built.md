# Where a compiled circuit's cycles go, as built, and what would cut them

*A research note of [An analog circuit compiler for FMinus](../README.md), written 2026-11-10 against the circuit
compiler as [circuits.md](../../../circuits.md) describes it (Phase 1). It is a report: nothing in `src/` changed.
[optimization.md](optimization.md) is the survey written before the compiler existed; this note looks at the code the
compiler emits today.*

## 0. Summary

**One fact sets the cost of every shipped module: the nonlinear step is a single serial dependency chain, and the CPU
waits on it.** `screamer.cir` runs 982 instructions a host sample in about 850 cycles (IPC about 1.15 on a 4-wide
core); the manual measures IPC 1.1 on an M5 for `otalp.cir`. The state a step writes (`x_C4`) is the first thing the
next step reads, and between the two sit two `expf` kernels, three divides and the omega3 guess, each waiting for the
one before. Everything that is not on that chain is nearly free (the manual's own measurements say so: the TS tone
block inside the 4x region "costs nothing", and `.channels 2` plays both lanes for the price of one).

So there are two ways to make circuits cheaper, and operation counts are not one of them:

1. **Shorten the chain.** Move work off it, or replace a long operation on it with a short one.
2. **Use the idle width.** The core has 3 to 7 times more execution capacity than the chain uses; lanes (voices,
   channels) fill it.

I measured the first on three shipped netlists by transliterating the emitted modules to C++ with FMinus's own math
kernels ([method](#appendix-how-the-numbers-were-made), [caveats](#caveats)). Cycles a host sample, and the step's
error against the same module in double solved to convergence (the self-test's T1 gate is -60 dB):

| Netlist, as shipped | Today | With the cuts below | Saving | T1-style error, before → after |
|---|---|---|---|---|
| `screamer.cir` (4x, ADAA, omega(1)) | 850 | 653: rounding-level changes and a series for ADAA's cosh (§2.1–2.4) | 24 % | -83.9 → -84.0 dB |
| | | 572–586: those plus a predictor in place of omega3 (§2.5) | 31–33 % | -83.9 → -77.5 dB |
| `otalp.cir` (4x, newton(1)) | 516 | 309: one `tanhf` a step instead of two (§2.6) | 40 % | -89.9 → -90.3 dB |
| `diodetone.cir` (4x, omega(2)) | 840 | 556–567: one Halley step instead of two Newton steps (§2.7) | 33 % | -93.1 → -85.3 dB |

None of these lowers the oversampling factor or touches the halfbands, the guard or the linear blocks, and each keeps
the bracket, the fixed step count and the branch-free shape. All but one change the emitted arithmetic, so they move
`kSelfTestVersion` and the tests that pin text or bits.

The second way is already proven for two lanes and not built beyond them: `.channels 4` and voice banks (§3) are the
largest absolute saving for a polyphonic synth, about 4x per voice on NEON.

The rest of the note: what else I looked at and why most of it is not worth touching (§5, §6), two places where the
configuration search leaves cycles behind (§4), and an order to try things in (§8).

## 1. The evidence that the chain is the cost

- **IPC.** `screamer.cir`'s tick is 982 instructions a host sample (callgrind) and takes 847–860 TSC cycles here;
  `otalp.cir` is 818 instructions in 515. The manual reports 516 instructions at IPC 1.1 on an M5, a core that can
  sustain 6 or more.
- **More work, less time.** Replacing `ei = 1 / e` with a second, parallel `expf(-x)` adds a whole kernel (about 20
  instructions) and removes one divide from the chain: the module gets 3 to 5 % *faster*.
- **Same work, moved.** Computing `1 / dp` as soon as `p` is known, and multiplying by it at the end, is the same
  operation count and 2 to 3 % faster.
- **The manual's own measurements.** The TS chain costs 905–906 cycles "whatever the region" (the tone block at 4x is
  free); `otalp.cir` at `.channels 2` costs 472 cycles for both lanes against 481 for one; two cascaded OTA stages
  cost about what one stage's chain does, because the second stage's chain overlaps the first's.
- **The cost model already says it.** `Search.cpp`'s `CostTable` is commented "cycles on the recursion's critical
  path".

What is on the chain in `screamer.cir`'s `b2` (one step at 4x; the emitted text, `Laws.cpp`'s `solveText` and
`adaaText`):

```c
const float p = p_D1_by_n_p1 * u + p_D1_by_x_C3 * x_C3 + p_D1_by_x_C4 * x_C4;   // reads x_C4: the chain starts
... omega3: fabsf, a multiply-add, a cubic in Horner's form, a multiply-subtract, a select
const float e = expf(v * 22.067577f);          // a kernel: about ten operations deep, each waiting on the last
const float ei = 1 / e;                        // a divide, waiting on e
const float f = v - p + ka_D1 * (e - ei);
const float d = 1 + kb_D1 * (e + ei);
v = fminf(fmaxf(v - f / d, lo), hi);           // a second divide
const float ec = expf(v * 22.067577f);         // ADAA: a second kernel, waiting on the Newton step
const float c = ec + 1 / ec;                   // a third divide
const float vb = ... / dp ...;                 // a fourth
x_C4 = g2_C4 * vb - xa_C4;                     // the chain ends; the next step's p waits on this
```

By my count of operation latencies that is 200 to 240 cycles a step, and four steps a host sample is the measured
figure. `x_C3`, the halfbands, the three linear blocks and the guard hang off the side and overlap it.

## 2. Shortening the chain

Each item gives the change in the emitted text, where it is emitted, what I measured, and what it risks. "Rounding
only" means the same formula with operations reordered or fused: the result moves in its last bits, no more.

### 2.1 `expf(-x)` instead of `1 / e` (rounding only)

```c
const float x = v * 22.067577f;
const float e = expf(x);
const float ei = expf(-x);       // was: 1 / e
```

The two kernels run side by side; the divide (roughly 10 cycles, more on older x86) leaves the chain. It applies to
the pair's Newton step and its ADAA tail (`Laws.cpp` lines 60–61 and 283–285) and to the carry in `b<N>_set`. The
asymmetric pair already runs two kernels side by side (`ef`, `er`). `expf(-x)` is also the more accurate of the two.
**Measured: 3 to 5 %** on the TS. Cost: one more kernel of throughput a use, which the core has spare; in `float2`
lanes the same holds.

A tighter version needs a builtin: `e^r` and `e^-r` share the argument reduction and the polynomial's even and odd
halves, so a `sinhf`/`coshf` pair whose kernels GVN merges would give both for little more than one `expf`. Worth
doing only if the lanes get crowded.

### 2.2 The ADAA divide, hoisted (rounding only)

`dp = p - p_D1` is known at the top of the step. Emit `const float rdp = 1 / dp;` there and end with `* rdp`: the
divide runs while the solver does. **Measured: 2 to 3 %.** The OTA's ADAA tail divides by `dp` and then by `kd` the
same way (`Laws.cpp` line 260); `1 / kd` is a coefficient and belongs in init or `b<N>_set`.

### 2.3 Explicit fused multiply-adds (rounding only; needs `fmaf`)

FMinus contracts nothing (rightly), and `fmaf` is on circuits.md's not-built list, so every `a * b + c` on the chain
is a multiply then an add. With an `fmaf` builtin the emitter could fuse the ones on the chain: `p`, omega3's
argument and cubic, `v`, the Newton step's `f` and `d`, ADAA's numerator and mean, the state update. **Measured: 6 %
alone, 13 % with §2.1 and §2.2.** The kernels already rely on hardware FMA (`O::fma`), so this asks nothing new of a
target. The fused form is also what would let `derive<Val>` print a block's `p` as one chain of `fmaf`s ending on the
state that arrives last.

### 2.4 ADAA's `cosh` from the Newton step's exponentials (a series; accuracy to be gated)

ADAA's tail evaluates `expf` again at the solved `v`, a few millivolts from the point the Newton step just evaluated.
With `dl = (v - v0) / N Vt`, where `v0` is the Newton step's starting point:

```c
const float dl = (v - v0) * 22.067577f;
const float dl2 = dl * dl;
const float c = (e + ei) * (1 + dl2 * (0.5f + 0.041666668f * dl2))     // 2 cosh(x0 + dl), cosh(dl) to dl^4
              + (e - ei) * (dl * (1 + 0.16666667f * dl2));             // sinh(dl) to dl^3
```

That is the addition theorem, with no second kernel and no third divide on the chain. **Measured: 11 % alone; 24 %
with §2.1–2.3**, and the step's error unchanged (-84.0 against -83.9 dB to L, -94.6 against -95.0 at 2.2 L, drive at
0, 0.5 and 1). The risk is a step where the Newton correction is not small (`|dl|` above about 0.3 loses the series'
last digits). On the stress set it never was, since omega3's guess is close, but the self-test should decide, and a
rare-path fallback (`fabsf(dl) < 0.25f ? series : expf`) keeps the exact form available at the price of a branch that
almost never goes the other way. One diode's `c = e^(v / N Vt)` and the asymmetric pair's two take the same form.

### 2.5 A predictor in place of omega3 (a new guess; a self-test candidate)

With ADAA the module already stores the last point `(v_D1, p_D1)` and `c_D1 = 2 cosh`. The curve's slope and
curvature there are free: `dv/dp = r = 1 / (1 + kb c)` and `d²v/dp² = -kb (2 sinh) r³ / N Vt`. Both can be computed
at the *end* of a step, off the chain, and the next step's guess is two multiplies and two adds:

```c
float v = fminf(fmaxf(v_D1 + dp * (r_D1 + dp * r2_D1), lo), hi);    // was: omega3, ~40 cycles and a logf arm
```

**Measured: 18 to 21 % alone; 31 to 33 % with §2.1, §2.2 and §2.4**, at -77.5 dB (the shipped solver's -83.9; the
gate is -60). The first-order predictor (`r` alone) is 2 % faster still and fails: -58.5 dB. What it needs before it
can be believed: the full stress matrix (the sweep and the noise are the signals where the last point predicts least),
pots and switches moving (`r_` and `r2_` belong to the old curve when `ka` moves: recompute them beside the carry in
`b<N>_set`, or accept one poorer guess, which the Newton step and the bracket still bound), and two more fields in
`clear()` and the guard's sum. It fits the existing machinery as a third guess beside `omega` and the previous `v`, so
the self-test can try it cheapest-first and fall back to omega3 where it fails.

### 2.6 An OTA step with one `tanhf` instead of two

Today's step (`Laws.cpp` lines 179–194) evaluates `tanhf` at the guess and again at the solved `v`. The guess is the
previous step's `v`, whose `tanhf` the previous step already computed. Two forms:

- **Bit-identical:** keep `th` in a field (ADAA already has it, `t_<G>`), and recompute only when the bracket moved
  the guess: `float tn = t_G1; if (v != v_G1) tn = tanhf(v * 19.331198f);`. The output cannot change. **Measured: 14
  %**: less than the unclamped form because the reuse misses whenever the bracket has moved the guess (`|p|` below
  the last `|v|`), and the test is a data-dependent branch.
- **Start from the previous `v` unclamped,** always with its stored `tanhf`, and clamp only the result:

  ```c
  float v = v_G1;                                  // was: fminf(fmaxf(v_G1, lo), hi), then tanhf(v ...)
  const float tn = t_G1;
  const float f = v - p + kg * tn;
  const float d = 1 + kgs * (1 - tn * tn);         // still at least 1
  v = fminf(fmaxf(v - f / d, lo), hi);             // still in the bracket, still finite
  const float th = tanhf(v * 19.331198f);          // the only kernel on the chain
  ```

  **Measured: 40 %** (516 → 309 cycles, 818 → 622 instructions) with the error unchanged: -90.3 dB to L (today
  -89.9), -89.6 at 2.2 L (-90.7), -127.2 with iabc swept ±2 octaves at 37 Hz (-127.3). The step stays finite for any
  input for the same reason it does today (`d >= 1`, the end clamp, and `fmaxf` turning a NaN into the bracket's end).
  With `newton(K)` it is K kernels instead of K + 1.

The same reuse applies to a diode port on the previous-`v` guess (`newton(K)`): its first `expf` is the last step's.

### 2.7 One Halley step where two Newton steps are chosen today

`diodetone.cir` needs `omega(2)`: `omega(1)` fails T1 at -54.2 dB. The second Newton step costs a whole `expf` and two
divides on the chain. A Halley step uses the same exponentials as one Newton step (`f'' = kb (e - ei) / N Vt` is
free) and one divide:

```c
const float sh = e - ei;
const float f = v - p + ka_D1 * sh;
const float d = 1 + kb_D1 * (e + ei);
const float h = (kb_D1 * 22.067577f) * sh;
v = fminf(fmaxf(v - (2 * f * d) / (2 * d * d - f * h), lo), hi);
```

**Measured: 556–567 cycles against 840 (33 %), at -85.3 dB** (two Newton steps: -93.1; one: -54.2, which my harness
reproduces to the manual's figure exactly). The denominator cannot vanish from a guess inside the bracket: where `f`
and `sinh` differ in sign it is `2 d²` or more, and where they agree `f h < (kb 2 sinh)² < d²`, so it stays above
`d²`, which is at least 1. The result is clamped to the bracket as Newton's is. This is another candidate shape for the
search (`halley(1)` between `omega(1)` and `omega(2)` in the cost order), and it applies to `tanh` too.

### 2.8 Tried, no gain

- **Estrin's form for omega3's cubic:** within noise (±1 %). The cubic is short and the select after it waits on the
  compare anyway.
- **Retiming the state update** (folding `x_C4 = g2_C4 * vb - xa_C4` into the next `p` so only one multiply-add
  follows `vb`): within noise as separate multiplies and adds. §2.3's fused form gets the same effect.

## 3. Using the idle width

- **Lanes for voices: `.channels 4` and wider.** Two lanes are free today (472 against 481 cycles on `otalp.cir`).
  The emitter is already generic in its type (`T`), the kernels exist at four lanes (`fminus_rt_exp_f4`), and the
  solver is branch-free with fixed steps for exactly this reason. What is missing is the Phase 2 list's "voice banks":
  per-lane law inputs (today a law input is broadcast to both lanes), and a lane count above 2. For 16 voices of an
  OTA filter that is 4 chains instead of 16 on NEON. Nothing in §2 comes close to this for a synth.
- **In lanes, both arms of every `?:` are computed.** A scalar module's omega3 guess can branch around its `logf`
  arm (a scalar `?:` is blocks and a phi, `Expressions.cpp`); in `float2` it is a select, the arm is always evaluated
  and it is the longest of the three, so the chain is longer by the difference between a `logf` kernel and a cubic
  (not measured). The same holds for the OTA's ADAA tail (`logf(zu)` against `lc - lc_P`). A predictor guess (§2.5)
  removes the first; the second is worth a look only if ADAA'd OTA banks matter.
- **Cascaded blocks of one shape.** `otalp.cir`'s two stages are the same code on different fields, and the second
  reads the first's output only at its end. They overlap today only as far as the out-of-order window reaches (see
  §7). Packed as two lanes of one `float2` step with the second lane one step behind, they would be one chain's
  instructions. That adds a step of delay between the stages (a quarter host sample at 4x), so it changes the
  module's latency and is a design decision, not a tweak; I list it because a 4-pole cascade is the common synth case.

## 4. The configuration search

- **New step shapes are new candidates.** §2.5–2.7 each change how accurate a given (factor, steps) pair is. The
  ordered search is the right place to decide them. The cost table needs their figures, and it is already far from
  circuits.md's measurements on the synth path: 1,528 for `otalp.cir` at 4x with ADAA against 1,108 measured, and 284
  for 1x newton(2) against 481 (it leaves out an OTA's last `tanhf`, as circuits.md says). Since the first candidate
  to pass wins, a mis-ordered model picks a costlier answer without saying so. A table derived from the chain (kernel
  depth, divides, which blocks overlap) would stay deterministic and be closer.
- **A missed gate by a hair costs a whole factor.** `otalp.cir` fails T3 at every 2x candidate by 0.03 to 0.04 dB and
  ships at 4x: 815 cycles against 530 for 2x newton(2), 54 % more. The 0.5 dB tolerance is your ruling and I am not
  suggesting moving it; the observation is that factors of 2 are coarse where the limiting error is the trapezoidal
  warp of H3. A 3x region (it needs third-band filters, which the halfband design work on the not-built list would
  cover) or a per-block factor would sit in that gap.
- **The provisional configuration is 2x the cost.** 8x omega(2) is 1,803 cycles against 904 for the TS stage's
  answer. It plays for the seconds the self-test takes, or for as long as a host never stores the answer. The disk
  cache of answers (not built, playground list) and shipping the built-in netlists' `Choice::text()` lines would make
  that window rare.

## 5. Off the per-sample path: `set()`

`set()` on `screamer.cir` costs 193 cycles a call (circuits.md), 12 cycles a host sample when a host calls it every
16: about 1.3 %. Small, but the fixes are mechanical and two are bit-identical:

- **`set()` recomputes every block every call** (`Emit.cpp` line 1926). `b3_set` is 200 operations with three
  `expf`, two `sinf` and five `sqrtf`, and runs when only `drive` moved. Keeping each block's last arguments and
  skipping a block whose pots and ramps are where they were gives the same coefficients by construction (and skips
  the carry, which adds `0 * (e - 1 / e)` today, a NaN if `e` ever overflowed).
- **Divides by the same divisor, and by constants.** `b3_set` has 27 divides. Nineteen of them share six divisors:
  `t9` four times, `t15`, `w3_0`, `w3_1` and `w3_3` three times each, and the literal `0.001f` three times (`t31 =
  t30 / 0.001f`). With one reciprocal each (init's, for the `w` fields and the literal) those nineteen become two.
  Rounding only.
- **A divide that undoes a multiply.** `t11 = t10 / 1.9607844e-05f; t12 = 1.9607844e-05f * t11;` in `b2_set`: the
  elimination normalises a row by its pivot and the back-substitution multiplies it back. The code builder could
  cancel the pair when it sees `(x / c) * c`. Rounding only.

A law input's per-sample work (`kg`, `kgs`, the clamp) is two multiplies off the chain: nothing to gain.

## 6. Looked at, not worth touching

- **The guard:** 7 cycles a host sample, off the chain.
- **Probes, disarmed:** within the bench's noise (circuits.md).
- **IIR halfbands:** 24 allpass sections a host sample at 4x, each a subtract, a multiply and an add, all off the
  chain. They overlap the steps.
- **Linear blocks** (trapezoidal or matched): a handful of multiply-adds, off the chain.
- **`os1`, `os2_*` as fields rather than locals:** six stores a tick.
- **FIR halfbands:** each phase is a 20-term sum added strictly left to right, a 20-add serial chain. It is
  feed-forward, so it costs throughput, not chain; pairing the terms as a tree would shorten it. Only matters to a
  netlist that asks for `fir`.
- **`generate()` and the self-test's own time** (0.1 to 220 ms; 1.2 to 5 s): not real-time cost. Not examined.

## 7. The unexplained prewarp row

circuits.md records `otalp.cir` at 1x newton(2) at 481 cycles and IPC 1.1 without prewarp, and 293 cycles and IPC 2.7
with it, "the IR has not been compared yet". I could not reproduce it: natively on x86 the same two modules run 180
and 250 cycles, the prewarped one slower, as its extra `tanf` predicts.

A hypothesis that fits the numbers, untested: 481 is almost exactly two chains in series and 293 one. Block 2's
whole step (three `tanhf` kernels, about 190 instructions) waits on block 1's last value. If those waiting
instructions fill the core's floating-point scheduler, the next sample's block 1 cannot be issued until block 2
drains, and the two chains run end to end instead of overlapping. Whether that happens depends on instruction order
and count at the margin, which the prewarp code changes. The quick test is the manual's bench with one OTA stage
deleted from the netlist: if one stage costs about 250 cycles with or without prewarp, the two-stage figure is the
overlap failing, and §2.6 (which removes a third of each block's instructions) or §3's packed lanes is the fix
rather than anything in the prewarp code.

## 8. An order to try things in

1. **§2.6, the OTA's stored `tanhf`** (the unclamped form). The largest measured saving for the least text: four
   emitted lines and one field that ADAA modules already have. Gate: the synth stress matrix.
2. **§2.1 and §2.2** together: two small edits in `Laws.cpp`, rounding only, 5 to 8 %. They bump `kSelfTestVersion`
   and the equivalence tests' expected bits, so they are best landed with whatever else moves the arithmetic.
3. **§2.4, ADAA's series,** and **§2.7, Halley,** as new shapes behind the self-test's gates: 11 % and 33 % where
   they apply.
4. **`fmaf`** in the language, then §2.3.
5. **§2.5, the predictor,** once the search has a place for a third guess; it needs the most validation.
6. **§3, lanes for voices,** which is Phase 2 work of a different size, and the one that changes what a synth can
   afford.
7. **§5** whenever `Emit.cpp`'s code builder is open anyway.

## Caveats

- **Not the JIT, not an M5.** The figures are from the emitted text compiled by clang 18 at `-O2 -march=native
  -ffp-contract=off` on a virtualised Xeon (AVX, FMA), with the math calls bound to `src/runtime/MathKernels.h`'s
  `Scalar` kernels, inlined, and subnormals flushed. FMinus's pipeline inlines the same kernels at the same level,
  so the shape should carry; the percentages will differ on Apple cores, whose divides and adds are shorter relative
  to their multiplies. Cycles are TSC cycles, best of 7 runs of 10^6 host samples; run-to-run spread was about ±1 %.
- **The error figure is T1-like, not T1.** It is the tick's output against the same module in double with 80 Newton
  steps, over sines at L/15 to L (and 2.2 L apart) at 105, 996 and 3012 Hz, 4096 samples after 1024 of warm-up, with
  the block's pot (or iabc) at three positions. It is not decimated in band and has no sweep, noise or saw. It
  reproduces circuits.md's T1 for `diodetone.cir` to the digit (-54.2 dB at omega(1), -93.1 at omega(2)), which is
  why I trust it for ranking, but T2 and T3 were not measured, and none of the changes was run with a pot gliding.
- **Every change but §2.6's first form and §5's first item moves output bits.** `screamer-biased.cir`'s bit-for-bit
  equality with `screamer.cir` survives (both would change together); the lit tests against the research's
  hand-written modules (`equiv-ex1c.fm`, `equiv-ts2.fm`) and `run.test`'s pinned values would need new expectations.

## Appendix: how the numbers were made

The circuit library builds without LLVM, so `generate()` was linked into a 15-line driver and run on
`examples/circuits/*.cir`. A script turned each emitted module into a C++ `struct` (`module` → `struct`, the
`[[clang::nonblocking]]` attributes dropped, `restrict` → `__restrict`, `expf`/`logf`/`tanhf`/`sinf`/`tanf` bound to
`fminus::kernels::exp(Scalar&, x)` and the rest, `fminf`/`fmaxf`/`fabsf`/`sqrtf` to the builtins) and, for the
reference, into the same struct with `float` → `double`, libm's double functions, and each Newton block wrapped in
`for (int it_ = 0; it_ < 80; ++it_)`. Variants replace one method's body (`b2` of the Screamer, `b1` and `b2` of
OtaLp, `b1` of DiodeTone) with the text shown in §2. Instruction counts are callgrind's, the difference of two run
lengths.

| `screamer.cir` variant | Cycles a host sample | Error to L / at 2.2 L (dB) |
|---|---|---|
| as emitted | 847–860 | -83.9 / -95.0 |
| §2.1 `expf(-x)` | 819–825 | -83.6 / -94.7 |
| §2.2 `1 / dp` hoisted | 831 | -83.9 / -95.0 |
| §2.3 fused multiply-adds | 806 | -84.0 / -93.2 |
| Estrin cubic | 854–869 | -83.9 / -94.2 |
| retimed state update | 859–867 | -84.1 / -94.6 |
| §2.4 series for ADAA's cosh | 755–761 | -84.0 / -94.6 |
| §2.1 + §2.2 + §2.3 | 746 | -84.2 / -92.9 |
| §2.1 + §2.2 + §2.3 + §2.4 | 653 | -84.0 / -95.6 |
| first-order predictor | 682 | -58.5 / -71.1 |
| §2.5 second-order predictor | 700 | -75.6 / -91.8 |
| §2.1 + §2.2 + §2.4 + §2.5 | 572–586 | -77.5 / -94.0 |

| `otalp.cir` variant | Cycles | Error to L / at 2.2 L / iabc swept (dB) |
|---|---|---|
| as emitted | 515–518 | -89.9 / -90.7 / -127.3 |
| §2.6 stored `tanhf`, recomputed when clamped | 441–447 | the same bits |
| §2.6 stored `tanhf`, guess unclamped | 307–311 | -90.3 / -89.6 / -127.2 |
| second `tanhf` by a cubic series from the first | 492 | -89.8 / -91.6 / -127.3 |

| `diodetone.cir` variant | Cycles | Error to L / at 2.2 L (dB) |
|---|---|---|
| omega3 + 1 Newton | 544 | -54.2 / -57.2 |
| omega3 + 2 Newton (as shipped) | 840 | -93.1 / -98.0 |
| omega3 + 2 Newton, §2.1 | 833 | -93.1 / -98.0 |
| §2.7 omega3 + 1 Halley | 567 | -85.3 / -88.7 |
| §2.7 with §2.1 | 556 | -85.3 / -88.7 |

The scripts and the harness were scratch files of the session that wrote this note and are not in the repository.
