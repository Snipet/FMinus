# Optimizing a compiled analog circuit for FMinus

*A research note of [An analog circuit compiler for FMinus](../README.md); where the two disagree, the report holds.*

**Angle:** making a netlist-compiled circuit cheap per sample at the same accuracy (circuit, solver, nonlinearity and
code level), with the accuracy loop that decides what is "the same". **Target:** FMinus programs in the
TestingPlayground plugins: float only, no automatic FMA, `poly float` lanes (4 on NEON), knobs / 20 ms-ramp streams /
frozen constants, 16 synth voices as separate `Voice` instances, a 16-sample part grid, subnormals flushed, no
allocation on the audio thread.

**Confidence tags:** **[H]** read in the primary source (paper or the tool's docs) during this research; **[M]**
standard result I know well, a secondary source, or my derivation from [H] material; **[L]** my estimate, extrapolation
or design proposal, not measured. Sources are numbered `[S#]` and listed at the end with URLs.

---

## 0. The ranking in one page

Per-sample cost of a compiled circuit is set by **four numbers and one factor**: N (reactive states), M (dimension of
the nonlinear solve), I (iterations per sample), the cost of one device-law evaluation, and R (oversampling factor).
Node count, resistor count and topology complexity cost nothing at run time once the linear network is eliminated
offline (DK method) or reduced (WDF). So the techniques rank by how much they cut M·I·R and N².

| Rank | Technique | Typical saving | Where it pays most | Evidence |
|---|---|---|---|---|
| 1 | Closed-form or table solution of a scalar nonlinearity (Wright ω for diodes, quadric triode), so I = 0 | 3–10× on the solve | diode clipper, TS clipper, triode | Newton clipper ≈ 1,000–1,250 weighted ops/sample at 1× [S3] vs closed form 5 adds + 9 mults + 1 ω [S7] [H; ratio L]; ω by bit-trick approximation 3.8× faster than ω by fixed-iteration Newton [S6]; quadric triode 4.6× vs Cardarilli [S22] [H] |
| 2 | Stage splitting at buffers/ideal op-amps + multirate (oversample only the nonlinear stage, run linear stages at 1× as exact low-order filters) | 2–8× | TS, Big Muff, amp chains | partition rule [S9][S10] [H]; factor [L] |
| 3 | Anti-aliasing choice: antiderivative anti-aliasing (ADAA) at 2× instead of plain 5–12× | 2–4× | every clipper with one scalar nonlinearity | hard clip 12× → 4× (1st order) / 3× (2nd) at equal SNR [S12]; stateful ADAA at 2× ≈ plain 5× [S13] [H] |
| 4 | SIMD across voices (synth) or channels, which needs fixed iteration counts | 2.5–4× throughput | Moog ladder, any per-voice circuit | batch<float> RC: 2.20 ns per 4 lanes vs 1.37 ns scalar [S5]; "up to 4×" [S6] [H] |
| 5 | Generated straight-line code with constants (no library/interpreter), circuit reductions in WDF | 1.1–5× | all, biggest on small circuits | wdf_compiler 1.4–5× vs libraries; reductions alone 3.3× [S5] [H] |
| 6 | Keep M small: merge ports (antiparallel diodes, diode strings), rank-reduce the index vector, exact block decomposition | 1.2–3× (and makes tables feasible) | SD-1/TS-style, preamps | 8 eq → 1+2+3+2 [S2]; 3-D → 2-D index [S1] [H] |
| 7 | Good predictor + audio-appropriate tolerance + junction limiting / capped step | 1.3–3×, removes non-convergence | fuzz, treble booster, hot clippers | Newton fails at 1×/2× where capped/new-iterate converge [S3] [H] |
| 8 | Pots: never re-invert; Woodbury (15×15 → 2×2 [S4]) or "pot as a port" (§3.5) per sample; fold when frozen | 5–20× on parameter updates [L] | wah, tone stacks, drive knobs | [S4] [H]; port form [M] |
| 9 | Device-model choice (Ebers–Moll vs Gummel–Poon, Koren vs quadric) | 2–4× | fuzz, tubes | GP 2.2–3.9× slower [S21] [H] |
| 10 | Precomputed tables / k-d solution caches for M ≥ 2 | ~2–8× but memory-bound | tube preamps, multi-device stages | 10% vs 76% CPU [S11]; 388 cached points instead of a 4 M grid [S1] [H] |
| 11 | Non-iterative (linearly implicit / Rosenbrock) schemes | 4–6× vs Newton, loses accuracy at extremes | moderate-drive circuits | ≈ one Newton iteration vs 5–6 [S15] [H] |
| 12 | Parasitic/state pruning, balanced truncation | 1.0–1.3×, sometimes negative | bypass caps, supply networks | pruning can *raise* iterations [S21] [H] |
| 13 | Cheap exp/log, float vs double, denormals, frozen constant folding beyond the above | 1.0–1.3× each | exp-bound inner loops | §5 |

Per-circuit numbers are in §7.

---

## 1. Cost model

### 1.1 The compiled form (nodal DK / generalized state space)

After trapezoidal discretization and offline elimination of the whole linear network, a circuit becomes [S8][S2][S3]
[H]:

```
p[n] = G·x[n-1] + H·u[n]                  M×N, M×U
solve  v = p[n] + K·i(v)   for v           M unknowns, K is M×M (the Thevenin matrix seen by the devices)
x[n] = A·x[n-1] + B·u[n] + C·i(v)          N×N, N×U, N×M
y[n] = D·x[n-1] + E·u[n] + F·i(v)          O×N, O×U, O×M
```

`i(v)` are the device laws (Shockley, Ebers–Moll, Koren...). ACME's equivalent form solves `f(q0 + Q·p + Fq·z) = 0`
for z [S1][S2] [H]. Per sample (MACs, R = oversampling factor, I = Newton iterations, c_f = one device evaluation):

```
C_lin    = M(N+U) + N(N+U+M) + O(N+U+M)
C_newton = I · [ M·c_f + 2M² (Jacobian K·diag(f') − I, residual) + M³/3 + M² (LU + solve) ]
C_total  = R · (C_lin + C_newton) + C_resample(R)
```

Holmes & van Walstijn give the same structure with weighted op counts (exp = 40, ÷ = 8, LU solve = M³ + M²/2 + 29M/2 −
8): one Newton iteration costs **253 ops for a diode clipper (M = 1)** and **646 ops for a Rangemaster (M = 2)**; the
chord method's iteration (Jacobian reused) costs 132 / 287 [S3] [H].

Consequences [M]:
- **Node and resistor count do not appear.** A 30-node circuit with N = 3, M = 1 costs the same as a 5-node one.
- **N² dominates the linear part** once N ≳ 8; **M³·I dominates the solve** once M ≳ 4.
- **R multiplies everything**, including the parts that are linear (unless stages are split, §2.4).

### 1.2 The WDF form

A WDF costs a few adds and 1–2 multiplies per adaptor port per sample, plus the root nonlinearity. One one-port
nonlinearity at the root is solved in closed form (ω) or by a scalar iteration; several non-separable nonlinearities
need an R-type root with an M-dimensional solve (Werner's K-method variant) or SIM (§3.2) [S24][S18] [H]. Measured,
generated WDF code on an Apple M1 runs an RC lowpass in **1.37 ns/sample**, a diode clipper (ω, no oversampling) in **35
ns**, a Baxandall EQ (R-type) in **11.4 ns**; on Zen 4 the clipper takes 22.8 ns [S5] [H].

### 1.3 The budget

At 48 kHz one core has 20.8 µs per sample. A 16-voice synth at 4× oversampling leaves 20.8 µs / 64 ≈ **325 ns per
voice-subsample for the whole core**, so ≈ 80 ns (≈ 300 cycles) if the filter may take a quarter of it [L]. Rough
conversion for dependent scalar code on Apple cores: 1,000 flops ≈ 0.2–0.3 µs (IPC ≈ 1, measured 0.67–2.7 for WDF code
[S5]) [L]. A vectorized `expf` (the system's vector library, FMinus's [fast math
mode](../../../simd.md#recipe-8-a-vector-math-library)) costs ≈ 0.40 ns per float in throughput on an M5, scalar libm
0.98 ns (FMinus's [simd.md](../../../simd.md)) [H].

---

## 2. Circuit-level optimizations

### 2.1 Series/parallel merging, node elimination, Thévenin/Norton

- **DK:** node elimination is the method itself. The MNA matrix is inverted offline; at run time the nonlinear solver
  sees only the M-port Thévenin equivalent `(p, K)` of the whole linear network. Merging resistors changes nothing at
  run time; merging **capacitors** (parallel caps, series caps with no tap) reduces N and saves ≈ 2N+U+M+O MACs per
  removed state [M].
- **WDF:** every adaptor is run-time work, so merging pays. wdf_compiler performs series/parallel/RC-element merges
  automatically (an element whose voltage or current is an output is never merged): on its reductions test circuit,
  **16 reductions cut instructions to < 25% and cycles to < 50%** (10.69 ns vs 34.99 ns unreduced; chowdsp_wdf 40.51 ns)
  [S5] [H]. A 2-multiply series adaptor (more ops, more ILP) beat the 1-multiply form 5.01 vs 6.51 ns [S5] [H]: on
  superscalar cores, count critical-path latency, not operations.

### 2.2 Remove what cannot be heard or cannot move

- **Uncontrollable/unobservable parts are exact removals.** Holters' rank factorization of the index matrix shows a
  supply-polarity diode and a 100 µF supply cap have *no* influence: the diode's equation has an empty index vector
  (precomputed offline) and the cap's state column is zero [S1][S2] [H]. A compiler should do this automatically
  (Kalman/minimal-realization check of each state and port) [M].
- **Parasitics below audibility:** a first-order pole at f_p changes the response at 20 kHz by
  `−10·log10(1 + (20k/f_p)²)` dB: 0.17 dB at 100 kHz, 0.04 dB at 200 kHz [M]. But inside a nonlinear feedback loop a
  "parasitic" can be the sound: in the TS-808 the 51 pF across the 51 kΩ + 500 kΩ drive feedback makes a ≈ 5.7 kHz
  lowpass inside the clipping loop at full drive [M]. And removal can make the solver *slower*: adding the BJT junction
  capacitances reduced iterations and time (Rangemaster 95.9 → 75.8 ms per second of audio) because they take
  high-frequency stress off Newton [S21] [H]. Rule [L]: prune by measured error and measured cost, never by component
  value alone.
- **Idealized bias networks:** replacing the TS/SD-1-style Vb divider + 100 µF by an ideal 4.5 V source is what made
  Holters' overdrive decomposable (§3.2); with the real bias network everything couples and only the protection diode
  separates [S2] [H]. The bias is nearly constant, so this is an accurate approximation, but it has to be verified
  against SPICE [S2] [H].

### 2.3 Model order reduction (balanced truncation) and its limits

- Balanced truncation of the linear part (inputs `[u; i]`, outputs `[y; p]`) keeps the r states with the largest
  Hankel singular values σ_j and guarantees `‖G − G_r‖∞ ≤ 2·Σ_{j>r} σ_j` [M]. Limits for audio [M]:
  1. Audio circuits are already low order (N ≈ 1–14); there is little to truncate beyond the exact removals in §2.2.
  2. The device currents `i` are broadband, large inputs, so truncated states must be negligible for them too, and an
     H∞ error on the linear part can be amplified by the nonlinear feedback.
  3. Balanced coordinates lose physical meaning (no "capacitor voltage" to inspect, initialize or clamp) and depend on
     the pots; plain BT also does not preserve passivity (positive-real BT does).
  4. The frozen copy in TestingPlayground runs on the *same instance state*, so the realization must not differ between
     the knob and frozen versions (§5.4).
- Nonlinear MOR (POD, TPWL, empirical BT) exists for IC simulation, but I found no audio VA paper applying it [M:
  absence after search]; payoff for N ≤ 14 is doubtful [L].
- **What does pay is the choice of realization for linear sub-networks** [M]: a dense N-state update costs N² MACs; the
  same transfer function as a cascade of TPT/SVF or biquad sections costs ≈ 2.5 MACs per order. Yeh derived the Fender
  tone stack exactly as a 3rd-order transfer function whose coefficients are closed-form functions of the three pots
  (instead of a 20-points-per-knob table of 33,200 entries) [S9] [H].

### 2.4 Ideal op-amps, buffers and stage splitting

- An ideal op-amp is a nullor: it removes unknowns from the MNA system and, more importantly, **cuts the backward
  coupling** so the DK matrices become block-triangular [M].
- Yeh partitions guitar circuits "where an active element with low source impedance drives a high impedance load", and
  the DS-1 / Tube Screamer models are cascades of conditioning filter → nonlinearity → EQ derived from the circuit
  [S9][S10] [H].
- **The large saving is multirate** [L]: once stages are separate DK blocks, only the stage that generates harmonics
  needs R× oversampling. A TS-808 is input buffer (emitter follower, nearly linear) → clipping stage (op-amp + diode
  pair, N = 2, M = 1) → tone stage (linear op-amp) → output buffer. Run the clipper at 2× with ADAA and everything else
  at 1× as exact low-order filters.
- Buffers that are not ideal (emitter followers) have closed forms too: the common-collector stage is solved exactly
  with one exponential of the input and one ω evaluation [S7] [H].
- Splits through finite impedances (Big Muff stage-to-stage coupling, tube stages) are approximations; Holters calls
  automatic *approximate* decomposition open work [S2] [H]; Mačák split a two-triode Fender preamp exactly into two
  one-tube parts by feeding the first plate current into the second stage's index [S11] [H].

### 2.5 Separate the linear part from the nonlinear part (the dimension M)

The DK form already restricts the iterative solve to M device ports [S8]. Further reductions [H unless marked]:
- **Shared controlling voltages:** antiparallel diodes are one port with `i = 2·Is·sinh(v/(n·Vt))` (Yeh detects
  duplicate controlling pairs automatically) [S8]; identical series diodes are one diode with n scaled [M]. The SIM
  paper models a five-diode asymmetric clipper as five ports [S18]; folding its series strings would leave one port per
  conduction direction, or a single port with a two-term law, depending on the arrangement [M].
- **Rank-reduce the index vector:** factor `(D_q  E_q) = Q·(D̂_q  Ê_q)` with full row rank; the treble booster's index
  drops from 3-D to 2-D, the SuperOver's is 7-D [S1].
- **Tube Screamer's three diodes** (two in one direction) reduce to a scalar nonlinearity by the same rank factorization
  [S13].
- **Exact block decomposition** (§3.2) splits the M-dim solve into a chain of smaller ones [S2].

### 2.6 DC operating point precomputation

- Circuits start from a bias point that real hardware reaches in seconds (100 µF caps, tube cathode caps). ACME's
  `steadystate!` sets the states to a steady state, "often useful for circuits whose bias voltages settle slowly"
  [S26] [H]; Holmes initialized both SPICE and the model from steady state to compare them [S3] [H].
- Compute it offline: ngspice-style DC with caps open and inductors shorted, Newton with gmin stepping then source
  stepping (`GMINSTEPS`, `SRCSTEPS`, `NOOPITER`; `GMIN` = 1e-12) [S27] [H], then iterate the discrete model at
  constant input to its fixed point. Store `x0` and the bias solution `v0` as the first Newton guess [M].
- With pots that move the bias (fuzz, bias trims), the DC point is a function of the pot. Precompute it on a grid, or
  re-run it off the audio thread when the dial freezes [L].

---

## 3. Solver-level optimizations

### 3.1 What DK precomputes

Offline: MNA assembly, inversion (in double or exact rationals), every product in `A…K`, the index-vector reduction, the
decomposition, the DC point. Holters used exact rational arithmetic for structural decisions (is this block zero?)
because float thresholds were unreliable, then converted to floating point for simulation [S2] [H]. At run time only the
products in §1.1 remain [H]. The run-time code is a fixed sequence of matrix-vector products: emit it as straight-line
code with only the structural non-zeros (§3.7).

### 3.2 Keeping M small: decomposition, SIM, model choice

- **Exact decomposition [S2] [H]:** choose an element grouping and a regular R with `F_q ← F_q·R` so the blocks above
  the diagonal vanish; solve group 1, then group 2 using z_1, and so on. A greedy search over subsets costs at most
  2^N − 1 trials offline (1023 for 10 devices). Results (Julia, Xeon E5-1620v2, 44.1 kHz guitar):
  - treble booster: 2.45 s → 1.99 s (**19% faster**), 1.789 iterations/sample on average;
  - "Der Super Over" (SD-1 clone, ideal op-amps, ideal Vb): 8 equations → groups of **1, 2, 3, 2**; iterations 2.965 →
    1.460 / 2.924 / 1.696; index dims 0 / 2 / 1 / 2 instead of 5; run time 6.82 → 6.79 s. The authors blame constant
    overhead (LAPACK: a size-8 solve took < 5× a size-1 solve).
  - My reading [L]: in generated straight-line code the overhead disappears and the M³ term drops from 512 to
    1 + 8 + 27 + 8 = 44, so the gain should be closer to the asymptotic one. The main win is that tables become
    feasible (2-D and 1-D instead of 5-D).
- **SIM (Scattering Iterative Method) [S18] [H]:** M one-dimensional solvers instead of one M-dim Newton, fixed-point
  outer loop, parallelizable. On a 5-diode asymmetric clipper at 4× it averaged **1.48 iterations/sample**, but
  **72% of the time went to updating port resistances and the scattering matrix**; updating them only when a threshold
  is crossed ("DSR") cut total time 8.13 → 5.06 µs/sample (MATLAB) and plugin CPU by ≈ 30% at 4×, 25% at 8×. Useful
  when M is large and the devices are one-ports; for M ≤ 4 Newton on the reduced system is simpler [L].
- **WD Newton–Raphson [S17] [H]:** choosing each port resistance at the diode's incremental resistance `Z_n = v'(i_n)`
  keeps the basin of quadratic convergence largest: 4.41 average and 7 maximum iterations on a 4-diode ring modulator
  (tolerance 1e-8). The DK analogue: iterate in the variable that keeps the Jacobian best conditioned; Yeh found
  iterating on terminal voltages converges faster than on currents [S8] [H].
- **Device-model choice [S21] [H]:** Gummel–Poon instead of Ebers–Moll costs 371.8 vs 95.9 ms per second of audio on a
  Rangemaster (3.9×) and 769.9 vs 341.6 ms on a Fuzz Face (2.2×), with *fewer* iterations, so the cost is the model
  evaluation. Quadric triode instead of Cardarilli: 4.6× faster in a WD common-cathode stage, with closed-form solutions
  when coupled to linear networks [S22] [H].

### 3.3 Newton–Raphson variants

```
g(v) = p + K·f(v) − v,   J = K·diag(f'(v)) − I,   v ← v − J⁻¹·g
diode: f = Is·(e^{v/nVt} − 1),  f' = (f + Is)/(nVt)        (the derivative is free once e^{v/nVt} is known) [M]
```

Measured behaviour (Holmes & van Walstijn, tolerance 1e-12, ≤ 100 iterations, damped sub-iterations ≤ 3) [S3] [H]:

| Case | Newton avg/peak iterations | Damped | Chord | Capped step | "New iterate" |
|---|---|---|---|---|---|
| diode clipper 1 V, 1× | 3.2 / 5 | 3.2 / 5 | 9.1 / 48 | 3.2 / 5 | 5.5 / 6 |
| diode clipper 4.5 V, 1× | 4.0 / 13 | 3.8 / 7 | diverges | 4.0 / 12 | 5.5 / 6 |
| diode clipper 4.5 V, 8× | 2.7 / 4 | 2.7 / 4 | 4.7 / 17 | 2.7 / 4 | 5.8 / 6 |
| Rangemaster 300 mV, 1× | diverges | 6.4 / 19 | diverges | 3.8 / 26 | 8.4 / 12 |
| Rangemaster 300 mV, 4× | 2.8 / 41 | 3.9 / 23 | diverges | 2.7 / 21 | 8.4 / 13 |

What to take from it [S3] [H unless marked]:
- **Plain Newton fails exactly where audio needs it** (hot signals, low rates). Damping (halve the step until the
  residual drops) fixes it at the price of a branchy inner loop.
- **Capped step:** limit |Δv| to the "transitional voltage" where the exponential starts to dominate,
  `V_tr = nVt·ln(−nVt/(K·Is))`. Same average cost as Newton, no failures in these tests. SPICE does the same with
  junction-voltage limiting (`pnjlim`, a logarithmic step above a critical voltage) [M].
- **Chord** (reuse the first Jacobian) halves the cost per iteration (132 vs 253 ops) but needs many more iterations and
  diverges on hot inputs. **Broyden** was less robust than chord; **Halley** less robust than Newton; **Brent** was the
  most robust in 1-D but does not extend to M > 1.
- **"New iterate"** starts from the inverse of the dominant nonlinear term, `v0 = f⁻¹(−p/K)`, and needs an almost
  constant number of iterations at every amplitude and rate, so a **fixed iteration count** is safe. The authors point
  out that the fixed count avoids control dependencies; they did not measure that gain.
- **Tolerance:** Yeh stops at a 5 mV correction, arguing component noise is larger; at 8× the iteration count averaged
  over 32-sample frames (at 48 kHz) **never exceeds 1.8**, and no single sample needs more than 8 [S9] [H]. Holmes's
  1e-12 cannot be reached in float anyway; a float model should stop at ≈ 1e-5 relative or run a fixed K chosen offline
  (§3.4) [M].
- **Semi-implicit (one Newton step per sample):** Yeh's table for the clipper at 8×: TR 36.8 f-calls/sample, TR
  semi-implicit 24, BE semi-implicit 16, with ringing on extreme high-frequency inputs [S9] [H].

**Initial guesses from the past** [H unless marked]:
- previous solution (standard); polynomial extrapolation in time (ngspice is built with `--enable-predictor` by default
  [S27] [H]; SPICE's predictor extrapolates past timepoints to start Newton [M]);
- **linearized extrapolation in the index vector:** `z0 = z* − (J·F_q)⁻¹·J·Q·(p − p*)` from the nearest known solution
  [S1]. ACME's `SimpleSolver` starts every sample this way [S26]. On the SuperOver, previous-sample + extrapolation
  averaged 2.59 iterations; adding a k-d tree cache of 27,039 solutions gave 2.31 and cut samples needing > 15
  iterations from 4,357 to 88 (worst case still 500) [S1];
- "new iterate" from the inverted device law [S3]; a table or polynomial approximation of the solved nonlinearity,
  refined by one Newton step (proposed by Yeh) [S8].

### 3.4 Fixed iteration counts, no branches (determinism and SIMD)

- The Faust WDF library evaluates its diode's ω with a **fixed number of Newton iterations** [S6] [H]. Holmes shows
  which starting point makes a fixed count safe [S3] [H]. Diva's manual reports filters "usually need one or two passes"
  but up to 15 at high resonance with complex input [S31] [M], so variable counts mean CPU spikes.
- Pattern for FMinus [L]: `for (k = 0; k < K; k++)` with constant K (unrolled by LLVM); each step capped
  (`fminf`/`fmaxf` on Δv, no branch); convergence handled by **selects**, not exits: `dv = converged ? 0 : dv`. Across
  `poly float` lanes every lane runs K iterations; an `any()`-style early exit per vector is possible but costs a
  reduction per iteration. Choose K offline as the worst case over the test suite plus one (§6.5). Output stays
  block-split invariant either way (the algorithm is per sample and deterministic); fixed K buys bounded CPU and SIMD.

### 3.5 Pots: rank updates instead of re-inversion

**Woodbury (Holters & Zölzer, wah pedal) [S4] [H].** Split the system matrix into a fixed part and the variable
resistors: `S = S0 + N_vᵀ·R_v⁻¹·N_v`. Then
`S⁻¹ = S0⁻¹ − S0⁻¹N_vᵀ·(R_v + Q)⁻¹·N_vS0⁻¹`, `Q = N_v S0⁻¹ N_vᵀ`, and every DK matrix is
`X = X0 − U_x·(R_v + Q)⁻¹·U_yᵀ` with precomputed `X0, U`. For the Crybaby this replaced a **15×15 inversion with a
2×2** per pot change; `R_v → 0` at the pot's ends needs no special case. Dempwolf, Holters & Zölzer report lower
recalculation cost than the DK method through minimized matrix formulations [S28] [H, abstract only].
Cost per change ≈ k·(N+O+M)·(N+U+M) MACs + a k×k inverse (k = number of variable resistors; a pot is k = 2) [M].

**"Pot as a port" (my derivation, equivalent to Woodbury applied to vectors) [M].** In TestingPlayground a turning float
dial is a 20 ms ramp read per sample, so pots *do* change every sample while moving. Instead of rewriting matrices, keep
them constant and treat the k variable resistors as extra ports with a linear law:

```
Thevenin at the variable-resistor ports:  v_r = p_r − Z_rr·i_r − Z_rn·i_n      (p_r = G_r·x + H_r·u)
element law:                              v_r = R(t)·i_r
⇒  i_r = (R(t) + Z_rr)⁻¹ · (p_r − Z_rn·i_n)                                     k×k, SPD for a passive network
```

`(R + Z_rr)` is exactly Holters' `(R_v + Q)`. Substituting into the device equation gives an effective
`K_eff = K_nn + K_nr·(R+Z_rr)⁻¹·K_rn` (k·M² MACs) and rank-k corrections to x and y. Per-sample overhead
≈ k·(N+U) + k³ + k·M² + k·(N+O) MACs: for N = 11, M = 8, k = 2 about **190 MACs per sample**, versus ≈ 800 MACs for a
full Woodbury rewrite of `A…K` on every sample [L]. It is exact, well-conditioned at R = 0, and handles audio-rate
modulation (LDR/JFET phasers, OTA cutoffs). When the dial freezes, fold `R` in and the ports disappear (§5.4).

**Update rate [L]:** on a moving dial, a Woodbury rewrite every 16 samples (TestingPlayground's absolute part grid)
costs ≈ 50 MACs/sample amortized and stays block-split invariant; the port form is per-sample exact. **WDF:** a resistor
change recomputes port resistances up the tree (a few divisions); an R-type adaptor needs a matrix inverse, so updates
are deferred until all children changed (chowdsp_wdf) [S6] [H] and SIM recomputes the scattering matrix only when
needed [S18] [H].

### 3.6 Precompute per parameter value

- Add the pot as a table dimension (Yeh suggests sweeping parameters into the function approximation) [S8] [H]. Mačák et
  al. ran a two-triode Fender preamp with four pots this way: the raw tables were **53 GB each**; non-uniform splines,
  point removal, dropping inputs a current barely depends on (from covariance analysis), and an exact split into two
  one-tube tables brought them down to sizes they deemed real-time capable (largest ≈ 65,520 / 4,095 units
  total/run-time; I could not recover the unit from the PDF text, likely kB) [S11] [H/M]. The parameter interpolation
  runs only when a pot moves; per sample only the signal dimensions are interpolated. Result: **≈ 10% CPU vs ≈ 76%** for
  the Newton state-space model (2.66 GHz Intel, C mex) [S11] [H].
- Linear sub-networks: compute coefficients from closed forms in the pot values (tone stack, [S9]) rather than tables.
- In FMinus, "frozen dials" make per-value precomputation automatic for anything expressed as code (§5.4). Tables for
  the *current* frozen values could be built in the background compile, but the frozen copy runs on the same instance
  and must sound the same (open question, §9).

### 3.7 Sparse LU with a fixed pivot order, symbolic code generation

- SPICE factors sparse MNA matrices with Markowitz ordering and refactors numerically with the same pivots [M]. A
  real-time compiler does better: the topology is fixed, so the whole large solve happens offline (DK). At run time the
  only factorizations are the M×M Jacobian and the k×k pot block, small and dense: emit them **fully unrolled** (M = 1:
  one division; M = 2: Cramer, 1 division + 6 MACs; M ≤ 4: unrolled LU without pivoting when the Jacobian is diagonally
  dominant, which holds for `K·diag(f') − I` with passive K and monotone devices) [M].
- **Exploit Jacobian structure** [M]: a ladder's Jacobian is lower-bidiagonal plus one feedback corner; solve it in O(M)
  by eliminating along the chain and solving a scalar equation for the feedback variable (the standard ZDF ladder
  solution), not by M³/3 LU.
- **Generated code beats libraries** [S5] [H]: wdf_compiler (same equations, static code, minimal persistent state,
  constants as `constexpr`) runs 1.1–5× faster than chowdsp_wdf and wdmodels and up to 16× faster than RT-WDF (RC: 1.37
  vs 6.97/6.02/22.54 ns; pre-amp EQ 18.42 vs 20.71/22.91/107.66; diode clipper 35.08 vs 41.05/116.52; Baxandall 11.35 vs
  20.52/18.54/183.01, M1), recompiles a circuit in well under a second for hot reloading, and generated C++ was faster
  than C, Jai and Rust in most cases. In the library world, compile-time templates vs run-time polymorphism alone is
  1–6× (chowdsp_wdf: LPF2 0.434 vs 2.763 s, Bassman tone stack 0.705 vs 2.849 s, diode clipper 2.041 vs 2.091 s per 1000
  s of audio) [S6] [H]. An FMinus circuit compiler sits on the fast side by construction: it emits straight-line FMinus
  (or IR) per circuit.

### 3.8 Homotopy and source stepping (offline only)

- Newton homotopy `R_H(x, λ) = R(x) − (1−λ)·R(x0)`, λ stepped 0 → 1, is globally convergent in practice but has no time
  bound; Yeh uses it **offline** to fill tables [S8] [H]. ACME's default solver is `HomotopySolver{CachingSolver
  {SimpleSolver}}`: Newton from an extrapolated guess, a k-d cache filled when Newton needs > 5 iterations, homotopy as
  the fallback [S26] [H].
- ngspice: gmin stepping, source stepping, and `optran` (operating point by a transient run) for hard DC points [S27]
  [H].
- For FMinus [L]: run homotopy/gmin/source stepping only at compile time (DC point, table and cache filling, worst-case
  K search). At run time rely on a bounded scheme (fixed K with capped steps, or table + one refinement step).

### 3.9 Non-iterative schemes

- **Linearly implicit (Rosenbrock-type) schemes** solve one linear system per step and cost ≈ one Newton iteration.
  On a diode clipper at 4×48 kHz with a 5 kHz input, the trapezoid and midpoint rules needed **5–6 Newton iterations
  per sample** (tolerance 1e-15); on a ring modulator 6.2 (trapezoid) and 26.8 (midpoint) iterations at 2 V. The
  non-iterative scheme had the largest error and showed spurious oscillation at 3 V carrier [S15] [H]. Andò, Bozzo &
  Fontana place these among Rosenbrock, W-methods (approximate Jacobians) and exponential Rosenbrock methods and compare
  A- and L-stability; only L-stable schemes damp stiff parasitic poles [S16] [H].
- **Explicit methods are not an option for stiff clippers:** forward Euler needed 38× and RK4 30× oversampling for a
  stable diode clipper (38 and 120 f-calls per base sample, vs 28.8 for implicit BE at 8×) [S9] [H]. Huovilainen's
  explicit Moog ladder works because the ladder is weakly nonlinear and its eigenvalues stay bounded [S9] [H].
- The "cheap" ZDF trick (replace tanh(x) by x·tanh(x0)/x0 from the previous sample and solve the linear ZDF system) is
  a one-step linearly implicit scheme in practice [S35] [M].
- Adaptive-step schemes (e.g. Zea & Rivera's residual-driven multirate QP, whose minimum step reached ≈ 200 ns, an
  oversampling factor of 128 relative to 44.1 kHz [S36] [H]) are incompatible with a fixed per-sample budget unless
  their steps snap to the sample grid; their residual is still useful offline as an error indicator [L].

---

## 4. Nonlinearity-level optimizations

### 4.1 Closed forms

- **Wright ω** `ω(x) = W0(eˣ)` solves "exponential + linear" without computing the exponential [S7] [H]. For a diode
  with Thévenin (p, R) (R = −K in DK form, a = n·Vt) [M]:
  ```
  v = p + R·Is − a·ω( (p + R·Is)/a + ln(R·Is/a) )        ln(R·Is/a) is constant per pot setting
  ```
  Antiparallel pairs use the sign trick `sinh(x) ≈ ½·sgn(x)·(e^{|x|} − 1)` [S7] [H]. D'Angelo's whole dynamic diode
  clipper costs **5 adds, 9 multiplies, 1 sign and 1 ω per sample** and is stable for all inputs [S7] [H].
- **ω approximations** [S7] [H]: ω1 = max(0, x); ω2 = cubic spline between two breakpoints; ω3 = 0 / cubic /
  `x − ln x` (with a bit-trick log2 + cubic); ω4 = ω3 + one Newton step. Measured whole-clipper costs, both using the
  ω diode model: **chowdsp_wdf (bit-trick ω approximation) 2.04 s per 1000 s of audio vs 7.81 s for Faust wdmodels (ω by
  a fixed number of Newton iterations), 3.8×** [S6] [H]; wdf_compiler 35 ns/sample (M1), 22.8 ns (Zen 4) [S5] [H].
- **Triode quadric surface**: `i_p = k_p2·Vpk² + k_g2·Vgk² + k_pg·Vgk·Vpk + k_p·Vpk + k_g·Vgk + k_0` gives a quadratic
  (closed form) when coupled to a linear network: 4.6× faster than Cardarilli in a WD common-cathode stage, nearly as
  accurate, more accurate than Koren [S22] [H].
- Closed forms need a scalar (or separable) nonlinearity; that is why §2.5/§3.2 come first.

### 4.2 Cheap exp/log

- **How much accuracy a diode needs [M]:** if `exp` has relative error ε, the solved diode voltage moves by
  `n·Vt·ln(1+ε) ≈ n·Vt·ε`. FMinus's expf (≈ 1 ulp, 6e-8) moves it by ≈ 2e-9 V. A cubic 2^x approximation with
  ε ≈ 1e-4 moves it by ≈ 3 µV, below a 16-bit LSB at 1 V full scale (15 µV). Device laws therefore tolerate cheap
  exponentials far better than oscillators do.
- D'Angelo gives C¹ cubic-spline log2 and 2^x on the mantissa with exponent bit tricks [S7] [H]. FMinus's own math
  kernels are already inline at -O1 and clamp exp's argument to [−104, 89] (FMinus's
  [`MathKernels.h`](../../../../src/runtime/MathKernels.h)) [H]; a circuit compiler should still clamp `v/(nVt)` so a
  wild Newton iterate cannot produce inf (capped step does that) [M].
- Use the derivative for free (`f' = (f + Is)/(nVt)`), and evaluate `sinh` as `(e − 1/e)/2` from one exp and one
  reciprocal [M].
- Expected gain: modest. An exp is ≈ 0.4 ns vectorized (§1.3); it matters when the solve is exp-bound (BJT circuits,
  2 exps per transistor per iteration) [L].

### 4.3 Tables of the solved nonlinearity

- **K-method / DK tables:** tabulate `i = g(p)` over the index vector with multilinear interpolation (Yeh, Octave
  `interpn`) [S8] [H]. Memory grows exponentially with the index dimension, which is why §2.5's reductions matter.
- **Interpolation cost grows fast with dimension** [S11] [H]: a bicubic spline costs 5 one-dimensional splines (2 with
  SIMD), a tricubic 21 (6 with SIMD) and 64 coefficients per cell; local cubic Hermite from raw points is ≈ 3× slower
  than precomputed spline coefficients.
- **Non-uniform storage:** Holters' k-d tree cache stores solutions only where Newton (with extrapolation) needs more
  than N_max = 5 iterations. On the treble booster, a 10,000 × 400 grid (4 million points) reduced to **388 stored
  points**, filled in minutes; the SuperOver's 7-D cache held 27,039 points after an hour of training and still missed
  0.14% of a new 35 s track [S1] [H]. Holters also notes that decomposed caches store N1 + N2 points instead of N1·N2
  [S1] [H].
- **Costs that tables add** [M]: cache misses (a 2-D 256×256 float table is 256 kB, beyond L1), interpolation error,
  extra aliasing from linear interpolation's slope discontinuities, and a dimension per moving pot (or a rebuild).
- Use tables when M ≥ 2 and the index dimension is ≤ 2–3 after reduction; otherwise iterate from a good guess [L].

### 4.4 Neural and polynomial surrogates

- **State-trajectory networks** replace the whole state update with an MLP [S19] [H]. Ops per sample: DS-1 2×128 ReLU
  **34,822**; diode clipper 2×8 tanh **686**; MS-20 filter 3×4 tanh **524** (tanh counted as 30 ops), trained at
  192 kHz. The physical diode clipper costs ≈ 1,000–1,250 weighted ops/sample on average at 1× with Newton (peaks
  1,500–3,500; exp counted as 40) [S3] and ≈ 15 + one ω with a closed form [S7], so a small network is no cheaper than
  physics for small circuits [M].
- **Neural device models inside a WDF** (Fender Pro Junior triode): real-time factors 25.8–39.6× at 96 kHz on a 2.4 GHz
  i9 (single network: 4×8 ELU or 2×32) [S20] [H].
- **Polynomial / Chebyshev fits** of a 1-D solved nonlinearity cost ≈ 2·degree flops by Horner and integrate exactly
  (for ADAA) [M]. PolyADAA uses Chebyshev approximation to get higher-order ADAA with Lagrange interpolation: 16–27 dB
  SNR over the trivial method but K = 8–32 transcendental evaluations per sample [S30] [H].
- Recommendation [L]: surrogates for multi-dimensional device solves (M ≥ 3) where tables explode, never for M = 1.

### 4.5 ADAA versus oversampling

First order: `y[n] = (F(x[n]) − F(x[n−1]))/(x[n] − x[n−1])`, with `F' = f`, falling back to `f((x[n]+x[n−1])/2)` when
`|x[n] − x[n−1]|` is below a tolerance [S32] [H]. It adds half a sample of delay and a mild lowpass, countered by ≈ 2×
oversampling [S13] [H].

| Result | Numbers | Source |
|---|---|---|
| Hard clipper, input gain 10, oversampling for ≈ 46.5 dB alias SNR | none: **12×** (46.7 dB); 1st-order ADAA: **4×** (46.3 dB); 2nd-order: **3×** (46.6 dB) | [S12] [H] |
| Stateful ADAA (nonlinearity inside the filter, delay absorbed into the integrator) on a diode clipper | full ADAA at **2× (88.2 kHz)** ≈ plain **5× (220.5 kHz)** for low-frequency aliases | [S13] [H] |
| Cost of stateful ADAA per scalar nonlinearity | a few adds and multiplies by ½, **one division**, an occasional extra table lookup; impractical when the nonlinearity's argument is multi-dimensional (antiderivative tables double in dimension) | [S13] [H] |
| Delay in feedback loops | compensate by removing an equivalent delay from the filter structure; demonstrated on a Moog-like ladder | [S12] [H] |
| Heavy guitar distortion without ADAA | 8× is enough, aliases are masked by the dense spectrum | [S9] [H] |

Rule [L]: one scalar nonlinearity per stage → stateful ADAA + 2×. Several coupled nonlinearities in a resonant loop
(ladders, fuzz) → plain 2–4× oversampling, ADAA only where the structure allows. Oversampling cost is R× the whole stage
plus resampling: a 2× polyphase IIR halfband pair costs ≈ 10–20 multiplies per base-rate sample each way [L].

---

## 5. Code-level optimizations (FMinus-specific)

### 5.1 SIMD across voices or channels

- A WDF or DK model whose numeric type is a 4-wide vector processes 4 voices at once: chowdsp_wdf's polyphonic voice
  example promises "up to 4×" [S6] [H]; measured on M1, wdf_compiler's `batch<float>` RC lowpass takes **2.20 ns for 4
  lanes vs 1.37 ns for one**, i.e. 2.5× throughput per lane (chowdsp_wdf's batch version 14.11 ns) [S5] [H].
- In FMinus the lane type is [`poly float`](../../../simd.md#recipe-9-poly-float) (4 lanes on NEON). TestingPlayground
  calls one `Voice` instance per note, so batching needs either a voice-bank module that holds 4 voices in lanes or host
  support (open question, §9). Stereo effects can use `float2` (FMinus's SLP pass already pairs stereo calls: [recipe
  24](../../../extending.md#two-lanes)).
- Prerequisites [M]: fixed iteration counts (§3.4), branch-free device laws (selects for piecewise ω, clamps for
  limiting), the same circuit in every lane (pot values may differ per lane).
- Within one circuit, the N×N matrix-vector product vectorizes over rows when N ≥ 4 [M].

### 5.2 float versus double

- FMinus has no `double`. On Apple cores scalar double is as fast as float (wdf_compiler RC: 1.37 ns both; chowdsp_wdf
  7.08 vs 6.97 ns) [S5] [H]; double's real cost is half the SIMD width.
- **Where float is fragile** [M]: (1) *inverting* ill-conditioned MNA matrices (relative error ≈ κ·6e-8; κ = 1e6 gives
  6%); (2) entries spanning many decades in SI units (Holters' treble booster: 1e-7 to 1e4 in one matrix [S1]); (3)
  DC-dominated states (9 V bias plus µV signal); (4) poles near z = 1 (a 1 Hz pole at 384 kHz is 1 − 1.6e-5 away from 1,
  resolved to ≈ 0.4%).
- Remedies [M]: do all inversions offline in double or exact rationals (as ACME does for structure [S2]); at run time
  only multiply by precomputed matrices and solve M×M or k×k systems that are diagonally dominant; scale states to volts
  and port variables so entries are O(1) (diagonal similarity transform, exact in real arithmetic); subtract the DC
  operating point from states (`x = x0 + Δx`); keep Newton tolerances above float resolution. WDFs are robust to finite
  wordlength by construction (Fettweis's pseudo-passivity) [S34] [M].

### 5.3 Denormals

TestingPlayground compiles with FMinus's
[`Denormals::Flush`](../../../extending.md#recipe-20-subnormals-flushed-for-the-audio-thread) and the audio thread runs
flush-to-zero (README) [H], so decaying states cost nothing extra. Keep it for determinism (x86 subnormal assists cost
tens to ~100 cycles [M]).

### 5.4 Frozen dials: what constant folding buys

TestingPlayground recompiles with every dial as a constant after 3 s of stillness and in fast math; across its examples
the frozen copy takes 78% of the effect's time and 75% of the synth's, mostly from fast math, because FMinus already
[versions methods](../../../extending.md#recipe-16-knob-versioning-and-flat-streams) on small int/bool knobs and hoists
still-dial arithmetic out of loops (README) [H]. For a compiled circuit the frozen build buys more [L]:
- **DK:** pot-dependent matrices `X0 − U(R+Q)⁻¹Uᵀ` (or the port terms of §3.5) fold into literal matrices; per-sample
  pot work (≈ 190 MACs in the example above) disappears; numerical zeros and ±1 entries fold away; `ln(R·Is/a)`
  constants in ω fold; knob-indexed table dimensions collapse. Expected 1.3–2× for pot-heavy circuits on top of the 0.75
  ratio.
- **WDF:** adaptor coefficients become immediates; wdf_compiler's `constexpr` parameters are part of why it runs 5×
  faster than chowdsp_wdf on an RC lowpass [S5] [H].
- **Constraint:** the frozen copy runs on the **same instance state** and must sound the same (TestingPlayground
  measures every octave band within 0.1 dB). So the state realization, the oversampling factor and the solver structure
  must be identical in both builds; only constants may differ. Switching to a table or a modal realization only when
  frozen would need a state mapping at the switch (§9).
- **FMA:** FMinus fuses nothing ("FMinus fuses and reassociates nothing",
  [README](../../../../README.md#notes-for-running-inside-a-plugin)) and has no `fmaf` builtin, so every MAC in the
  generated matrix code is two instructions with two roundings. An explicit fma builtin would halve the linear-part
  instruction count and stay deterministic on AArch64 [L].

### 5.5 Block-rate versus sample-rate parameters and smoothing

- TestingPlayground: a float dial is read per sample (a 20 ms ramp) while it moves; int/bool dials once per call; calls
  end on absolute 16-sample boundaries (README) [H]. Host automation per block remains a documented exception.
- Options [L]: per-sample exact update via the port form (§3.5, cheapest for moving pots); Woodbury every 16 samples on
  the absolute grid (cheap, block-split invariant, 0.33 ms steps at 48 kHz on an already smoothed value); never
  interpolate DK matrices element-wise (an interpolated A is not the discretization of any circuit and need not be
  stable). WDF resistor updates are cheap enough per sample [S6].

---

## 6. Accuracy controls: measure against ngspice, choose the cheapest settings that pass

### 6.1 Reference

- Generate the SPICE netlist **from the same circuit description** the compiler reads, with the same device equations
  (diode `RS = 0`, `CJO = 0` unless the compiled model has them; same Is, n, Ebers–Moll parameters) [M].
- Run ngspice transient with tight options [S27] [H for option meanings, L for values]: `RELTOL = 1e-6`,
  `VNTOL = 1e-9`, `ABSTOL = 1e-15`, `METHOD = gear` or `trap`, and `TMAX` ≤ 1/8 of the reference sample period
  (Oyama used ngspice 46 with the step capped at 1/8 of the period, then resampled [S23] [H]). Defaults are much looser:
  `RELTOL = 1e-3`, `VNTOL = 1 µV`, `ABSTOL = 1 pA`, `ITL4 = 10`, trapezoid, `TRTOL = 7`; `.tran` limits the step to
  `min(tstep, (tstop − tstart)/50)` unless `tmax` is given; `INTERP` writes output on the TSTEP grid by linear
  interpolation [S27] [H].
- Start from the same DC operating point (do not use `uic`; ngspice computes it with caps open and inductors shorted)
  [S27] [H].
- Check the reference itself: halve `TMAX` until the output changes by less than −100 dB [L]. Yeh used the same model at
  32× trapezoid as an in-model reference [S9] [H]; that is cheaper for regression tests and separates discretization
  error from device-model mismatch.

### 6.2 Metrics

| Metric | Definition | Used by |
|---|---|---|
| Linear response error | dB RMSE of the small-signal magnitude response, 20 Hz–20 kHz (swept sine, deconvolved) | [S23] [H] |
| Harmonic error | H2–H9 levels normalized to H1, RMSE vs SPICE (Farina sweep, time-gated harmonics) | [S23] [H] |
| Large-signal behaviour | THD and output level vs input level; cutoff shift and self-oscillation level for filters | [S4][S23] [H] |
| Time-domain error | error-to-signal ratio after latency alignment (ADAA adds ½ sample, resamplers add more) | [M] |
| Aliasing | SNR outside a harmonic mask, reference computed at a very high rate (Parker used 11.2 MHz) | [S12] [H] |
| Robustness | max iterations, non-convergence count, NaN/inf, DC drift, stability at extreme pots and levels | [S3] [M] |

Reference points: on Oyama's benchmark the best ladder implementations reach 0.04–0.10% H1 harmonic error and
0.12–0.40 dB linear error; one with saturation only at input and feedback reaches 0.65 / 3.39 / 3.03% [S23] [H].

### 6.3 Test signals

Sines at several levels and frequencies (1 kHz and 5 kHz at the circuit's maximum realistic level, e.g. 4.5 V for a
clipper behind gain, 300 mV for a guitar-level input [S3]), log sweeps, white noise ramps, guitar DI, sawtooth for synth
filters, self-oscillation and cutoff sweeps (filters), every pot at its ends and middle [L].

### 6.4 Error budgets

- Device-law tolerance: Yeh's 5 mV Newton tolerance on the grounds that component noise is larger [S9] [H].
- Propagation: in Mačák's preamp, a current-approximation error ε becomes ε × plate resistor × next stage's gain at the
  output [S11] [H]; budget each approximation at the output, not at the device [M].

### 6.5 The search

[L] For each circuit, enumerate the discrete choices: oversampling R ∈ {1, 2, 4, 8}; ADAA order ∈ {0, 1, 2}; solver
(closed form / Newton fixed K ∈ {1…6} / table resolution); exp approximation; stage split on or off; each parasitic or
state pruned or kept; ideal or real bias network; device model. Cost = measured ns/sample from the FMinus JIT (or op
count as a pre-filter). Error = §6.2 metrics on §6.3 signals. Keep the Pareto front, pick the cheapest configuration
under the targets, and pick K as the worst observed iteration count plus one. Re-run when the netlist changes; cache
results by netlist hash. Holters' k-d cache training and ACME's homotopy fallback are the same idea applied to the
solver [S1][S26] [H].

Suggested targets (to be tuned by listening) [L]: linear ≤ 0.2 dB; H2–H9 ≤ 0.2% of H1 at nominal drive; in-band
aliases ≤ −80 dB re fundamental for a 1 kHz sine at nominal drive; no failures on the stress set; output peaks within
0.1 dB of the reference.

---

## 7. Per-circuit playbooks

Estimates are flops per output sample at 48 kHz, scalar float, using §1.1 with exp/tanh ≈ 15–20 flops (inline vector
kernel), division ≈ 4, resampling 2×/4×/8× ≈ 30/50/70 flops per base sample. **All per-circuit flop counts and factors
in this table are [L]**; the anchors cited are [H].

| Circuit | Full-circuit size | Generic compiled cost | Optimized recipe | Optimized cost | Anchors |
|---|---|---|---|---|---|
| **Diode clipper** (RC + diode pair) | N = 1, M = 1 | ≈ 700 (8×, Newton ≈ 2 it.) | ω closed form, stateful ADAA at 2×, SIMD over channels/voices | ≈ 120–150 (≈ 5×; ≈ 40 per lane with SIMD) | ω clipper 35 ns/sample at 1× [S5]; 2×+ADAA ≈ 5× OS [S13] |
| **Tube Screamer** | N ≈ 6, M = 1 | ≈ 1,600 (8×) | split at the op-amps; clipper (N = 2, M = 1) at 2× with ω + ADAA; buffers linear (or ω CC buffer); tone stage as an exact pot-parameterized filter at 1×; drive pot as a port | ≈ 170–250 (≈ 7×) | TS-like ACME model reduces to a scalar nonlinearity [S13]; stage models [S10] |
| **Moog ladder** (per voice) | 1 + 4 tanh stages, N = 4 | ≈ 2,000 (ZDF Newton on 4-D, 4×) | structured solve (scalar feedback equation, O(M)), fixed K = 2, 2× OS, cheap tanh, 4 voices per vector | ≈ 150–250 per voice (≈ 8–12× incl. SIMD) | Huovilainen 1+4 tanh at 2× has the lowest error vs SPICE (0.04–0.17% H1) [S23]; linearizing stages 3–4 saves 2 of 5 tanh but raises drive error 0.06 → 1.49% [S23] |
| **Big Muff** | N ≈ 14, M ≈ 10 (4 BJTs + 2 diode pairs) | ≈ 20,000 (4×) | approximate split into 4 stages (M = 2, 3, 3, 2), oversample only the two clipping stages, tone stack exact at 1×, DC point precomputed, cache or 2-D tables for the M = 3 stages | ≈ 2,500–4,000 (≈ 5–8×), split error to be verified vs SPICE | clipping stage = BJT + diode pair solved jointly [S24] (M = 3 is my count); plug-in generation from netlist [S25] |
| **Triode preamp** (2 × 12AX7) | N ≈ 8, M = 4 (Koren) | ≈ 4,600 (4×) | exact per-tube split; quadric model (closed form) or 2-D spline tables; grid current only where grid conduction matters | ≈ 700–1,000 (≈ 5–7×) | tables 10% vs Newton 76% CPU [S11]; quadric 4.6× [S22] |
| **Transistor fuzz** (Fuzz Face) | N ≈ 4, M = 4, global feedback | ≈ 3,000–6,000 (4–8×) | Ebers–Moll, predictor + capped step with fixed K = 3, DC bias precomputed per fuzz-pot value, 4× OS, fuzz pot as a port | ≈ 1,500–2,500 (≈ 2×) | ≈ 3.6 iterations average, GP 2.2× slower [S21]; no exact decomposition (global feedback) |
| **Op-amp filter** (Sallen-Key/MFB, ideal op-amps) | N = 2, M = 0 | ≈ 20 (dense 2-state DK) | exact biquad/TPT SVF with closed-form pot→coefficient code; frozen → constants; add a 1-port saturator (M = 1) only if the rails clip | ≈ 10–12 (+ ≈ 30 at 2× with a saturator) | Korg MS-20 STN 524 ops [S19]; non-iterative Korg35 [S15] |

Ranking matrix (★★★ ≥ 3×, ★★ 1.5–3×, ★ < 1.5×, – n/a) [L]:

| Technique | Clipper | TS | Ladder | Muff | Triode | Fuzz | Op-amp |
|---|---|---|---|---|---|---|---|
| Closed form / scalar solve | ★★★ | ★★★ | ★★ | ★ | ★★★ | – | – |
| Stage split + multirate | – | ★★★ | – | ★★★ | ★★ | – | ★ |
| ADAA instead of high OS | ★★★ | ★★★ | ★ | ★★ | ★★ | ★ | ★ |
| SIMD across voices | ★★ | ★★ | ★★★ | ★★ | ★★ | ★★ | ★★ |
| Generated straight-line code | ★★ | ★★ | ★★ | ★★ | ★★ | ★★ | ★★★ |
| Reduce M / decompose | ★ | ★★ | ★★ | ★★★ | ★★ | ★ | – |
| Predictor, capping, tolerance | ★ | ★ | ★★ | ★★ | ★★ | ★★ | – |
| Pot as port / Woodbury / frozen fold | ★ | ★★ | ★★ | ★★ | ★★ | ★★ | ★★ |
| Model choice (EM vs GP, quadric) | – | – | – | ★★ | ★★★ | ★★ | – |
| Tables / caches | ★ | ★ | – | ★★ | ★★★ | ★ | – |
| Non-iterative scheme | ★★ | ★★ | ★★ | ★ | ★ | ★ | – |
| Parasitic pruning / MOR | ★ | ★ | – | ★ | ★ | ★ | ★ |
| Cheap exp/tanh | ★ | ★ | ★★ | ★★ | ★ | ★★ | – |

---

## 8. What the compiler should do, in order [L]

1. Parse the netlist; build MNA in exact rationals or double.
2. DC operating point with gmin/source stepping; store x0, v0 (per pot grid point when pots move the bias).
3. Remove uncontrollable/unobservable states and empty device ports (exact); merge parallel/series capacitors and
   duplicate-controlled devices (antiparallel pairs, series strings).
4. Find stage boundaries (ideal op-amps, buffers, optionally user-marked approximate cuts); give each stage its own DK
   model and rate.
5. Per stage: DK matrices, index-vector rank reduction, exact block decomposition of the device equations.
6. Per device group: closed form if scalar (ω, quadric); else Newton with predictor + capped step and fixed K; else
   table / k-d cache when the index is ≤ 2–3-D.
7. Pots: port form (or Woodbury) code expressed in the dial parameters, so FMinus's frozen binding folds it.
8. Anti-aliasing: stateful ADAA where a stage has one scalar nonlinearity, R× oversampling where it does not.
9. Emit straight-line FMinus with structural zeros removed, scaled variables, DC-offset states; offer a `poly float`
   voice-bank variant.
10. Run the §6 search against ngspice and against the in-model 32× reference; record K, R, ADAA order and pruning
    decisions with the netlist hash.

---

## 9. Open questions

1. **Voice batching:** TestingPlayground runs one `Voice` instance per note. Should the circuit compiler emit a 4-voice
   `poly float` bank (needs the host to route notes to lanes), or should FMinus vectorize across instances?
2. **double and FMA:** FMinus is float-only and fuses nothing. Should the circuit compiler compute pot-dependent
   matrices on the host in double (C++ side, per 16-sample part) instead of in FMinus float, and should FMinus get an
   explicit `fmaf` builtin for generated matrix code?
3. **Frozen-only optimizations:** tables or modal realizations built for the frozen pot values need a state mapping or
   identical state layout when the frozen copy takes over the same instance. Allowed, or must both builds share the
   exact algorithm?
4. **Approximate decompositions** (Big Muff, real bias networks): who signs off the error, and should the compiler
   propose cuts automatically and verify them with ngspice?
5. **Moving pots:** is per-sample exactness (port form) required, or is a 16-sample update grid acceptable?
6. **Accuracy targets:** what thresholds (§6.5) count as "the same" for this project, and is SPICE or the in-model
   high-rate reference the regression baseline?
7. **Host-side compile step:** ngspice runs, homotopy, table and cache filling take seconds to minutes. Do they run when
   the program is typed (blocking the swap) or asynchronously, with a plain-Newton fallback until done?

---

## Sources

- [S1] M. Holters, U. Zölzer, "A k-d Tree Based Solution Cache for the Non-linear Equation of Circuit Simulations",
  EUSIPCO 2016. https://www.hsu-hh.de/ant/wp-content/uploads/sites/699/2017/10/Holters_treecachesolver_EUSIPCO16.pdf
- [S2] M. Holters, U. Zölzer, "Automatic Decomposition of Non-linear Equation Systems in Audio Effect Circuit
  Simulation", DAFx-17.
  https://www.hsu-hh.de/ant/wp-content/uploads/sites/699/2017/10/Holters-Zölzer-2017-Automatic-Decomposition-of-Non-linear-Equation-Systems-in-Audio-Effect-Circuit-Simulation2.pdf
- [S3] B. Holmes, M. van Walstijn, "Improving the Robustness of the Iterative Solver in State-Space Modelling of Guitar
  Distortion Circuitry", DAFx-15. https://pureadmin.qub.ac.uk/ws/files/17395135/improving_the_robustness.pdf
- [S4] M. Holters, U. Zölzer, "Physical Modelling of a Wah-wah Effect Pedal as a Case Study for Application of the Nodal
  DK Method to Circuits with Variable Parts", DAFx-11.
  https://www.hsu-hh.de/ant/wp-content/uploads/sites/699/2017/10/Holters-Zölzer-2011-Physical-Modelling-of-a-Wah-wah-Effect-Pedal-as-a-Case-Study-for-Application-of-the-Nodal-DK-Method-to-Circuit.pdf
- [S5] J. Chowdhury, M. Rau, "Performance-Oriented Wave Digital Circuit Emulation" (wdf_compiler), DAFx26.
  https://www.dafx.de/paper-archive/2026/papers/DAFx26_paper_23.pdf ; code:
  https://github.com/Chowdhury-DSP/wdf_compiler
- [S6] J. Chowdhury, "chowdsp_wdf: An Advanced C++ Library for Wave Digital Circuit Modelling", arXiv:2210.12554.
  https://arxiv.org/pdf/2210.12554
- [S7] S. D'Angelo, L. Gabrielli, L. Turchet, "Fast Approximation of the Lambert W Function for Virtual Analog
  Modelling", DAFx-19. https://iris.unitn.it/bitstream/11572/250641/4/DAFx2019_paper_5.pdf
- [S8] D. T. Yeh, J. S. Abel, J. O. Smith, "Automated Physical Modeling of Nonlinear Audio Circuits for Real-Time Audio
  Effects — Part I: Theoretical Development", IEEE TASLP 18(4), 2010.
  https://ccrma.stanford.edu/~dtyeh/papers/yeh10_taslp.pdf
- [S9] D. T. Yeh, "Digital Implementation of Musical Distortion Circuits by Analysis and Simulation", PhD thesis,
  Stanford, 2009. https://ccrma.stanford.edu/~dtyeh/papers/DavidYehThesisdoublesided.pdf
- [S10] D. T. Yeh, J. S. Abel, J. O. Smith, "Simplified, Physically-Informed Models of Distortion and Overdrive Guitar
  Effects Pedals", DAFx-07 (abstract). https://dafx.de/paper-archive/details/mIq7vlQKu6pUlF467vvhgw
- [S11] J. Mačák, J. Schimmel, M. Holters, "Simulation of Fender Type Guitar Preamp Using Approximation and State-Space
  Model", DAFx-12.
  https://www.hsu-hh.de/ant/wp-content/uploads/sites/699/2017/10/Mačák-Holters-Schimmel-2012-Simulation-of-Fender-Type-Guitar-Preamp-using-Approximation-and-State-Space-Model.pdf
- [S12] J. Parker, V. Zavalishin, E. Le Bivic, "Reducing the Aliasing of Nonlinear Waveshaping Using Continuous-Time
  Convolution", DAFx-16. https://dafx.de/paper-archive/2016/dafxpapers/20-DAFx-16_paper_41-PN.pdf
- [S13] M. Holters, "Antiderivative Antialiasing for Stateful Systems", Applied Sciences 10(1):20, 2020 (extends
  DAFx-19). https://www.hsu-hh.de/ant/wp-content/uploads/sites/699/2020/10/applsci-10-00020-v2.pdf
- [S14] S. Bilbao, F. Esqueda, J. Parker, V. Välimäki, "Antiderivative Antialiasing for Memoryless Nonlinearities",
  IEEE SPL 24(7), 2017 (abstract only; first order equals [S12], higher orders via repeated antiderivatives) [M].
- [S15] M. Ducceschi, S. Bilbao, "Non-Iterative Simulation Methods for Virtual Analog Modelling", IEEE/ACM TASLP
  30, 2022.
  https://cris.unibo.it/retrieve/50aa41af-7920-496e-8bba-7ca3824e9ba3/Non-Iterative_Simulation_Methods_for_Virtual_Analog_Modelling.pdf
- [S16] A. Andò, E. Bozzo, F. Fontana, "Non-Iterative Numerical Simulation in Virtual Analog: A Framework Incorporating
  Current Trends", DAFx25. https://dafx25.dii.univpm.it/wp-content/uploads/2025/09/DAFx25_paper_6.pdf
- [S17] A. Bernardini, E. Bozzo, F. Fontana, A. Sarti, "A Wave Digital Newton-Raphson Method for Virtual Analog Modeling
  of Audio Circuits with Multiple One-Port Nonlinearities", IEEE/ACM TASLP 2021.
  https://re.public.polimi.it/bitstream/11311/1203185/1/WD_NR_TASLP_Bernardini_etal_2021_RG.pdf
- [S18] A. Proverbio, A. Bernardini, A. Sarti, "Toward the Wave Digital Real-Time Emulation of Audio Circuits with
  Multiple Nonlinearities", EUSIPCO 2020.
  https://re.public.polimi.it/retrieve/e0c31c10-7864-4599-e053-1705fe0aef77/Proverbio_Bernardini_Sarti_Eusipco2020_NoOrdNumb.pdf
- [S19] J. D. Parker, F. Esqueda, A. Bergner, "Modelling of Nonlinear State-Space Systems Using a Deep Neural Network",
  DAFx-19. https://dafx.de/paper-archive/2019/DAFx2019_paper_42.pdf
- [S20] C. C. Darabundit, D. Roosenburg, J. O. Smith, "Neural Net Tube Models for Wave Digital Filters", DAFx20in22.
  https://dafx2020.mdw.ac.at/proceedings/papers/DAFx20in22_paper_13.pdf
- [S21] B. Holmes, M. Holters, M. van Walstijn, "Comparison of Germanium Bipolar Junction Transistor Models for
  Real-time Circuit Simulation", DAFx-17.
  https://www.hsu-hh.de/ant/wp-content/uploads/sites/699/2017/10/Holmes-Holters-van-Walstijn-2017-Comparison-of-Germanium-Bipolar-Junction-Transistor-Models-for-Real-time-Circuit-Simulation2.pdf
- [S22] R. Giampiccolo, S. D'Angelo, A. Bernardini, A. Sarti, "A Quadric Surface Model of Vacuum Tubes for Virtual
  Analog Applications", DAFx-23.
  https://re.public.polimi.it/retrieve/a1844311-c89f-4257-a993-126fe4a5304c/GiampiccoloEtAl_DAFx23.pdf
- [S23] H. Oyama, "Quantifying Nonlinear Behavior in Digital Moog Ladder Filters: Cross-Implementation Comparison and
  Common-Core Ablation", DAFx26. https://www.dafx.de/paper-archive/2026/papers/DAFx26_paper_29.pdf
- [S24] K. J. Werner, V. Nangia, J. O. Smith, J. S. Abel, "Resolving Wave Digital Filters with Multiple/Multiport
  Nonlinearities", DAFx-15 (abstract; Big Muff clipping stage).
  https://dafx.de/paper-archive/details/4fsq4pZNdCbUzyzAL8xsQg
- [S25] R. Giampiccolo, S. Ravasi, A. Bernardini, "Automatic Generation of Virtual Analog Audio Plug-ins based on Wave
  Digital Filters" (VIOLA: SPICE netlist → WDF plug-in; Op Amp Big Muff, Overdrive Preamp 250), JAES 2025 (abstract).
  https://re.public.polimi.it/handle/11311/1292265
- [S26] ACME.jl user guide (solvers, `steadystate!`). https://hsu-ant.github.io/ACME.jl/stable/ug/
- [S27] ngspice manual, §11.1 (options), §11.3.10 (`.tran`), §12 (predictor, `optran`).
  https://ngspice.sourceforge.io/docs/ngspice-html-manual/manual.xhtml
- [S28] K. Dempwolf, M. Holters, U. Zölzer, "Discretization of Parametric Analog Circuits for Real-Time Simulations",
  DAFx-10 (abstract). https://www.dafx.de/paper-archive/details/74c2I_PbWhkM6MUukii5IQ
- [S29] S. D'Angelo, V. Välimäki, "Generalized Moog Ladder Filter: Part II — Explicit Nonlinear Model through a Novel
  Delay-Free Loop Implementation Method", IEEE/ACM TASLP 22(12), 2014 (abstract: non-iterative delay-free-loop
  method, "modest computational load penalty") [M].
- [S30] L. Gabrielli, S. Squartini, "PolyADAA: Improving Aliasing Reduction in Memoryless Nonlinearities Using Lagrange
  Interpolation and Polynomial Approximation", DAFx26. https://www.dafx.de/paper-archive/2026/papers/DAFx26_paper_47.pdf
- [S31] u-he Diva user guide (accuracy modes; filters need 1–2 passes, up to 15 at high resonance), via a manual mirror
  and KVR developer posts [M]. https://manualmachine.com/uhe/diva/17903499-user-manual
- [S32] J. Chowdhury, "Practical considerations for antiderivative anti-aliasing" (notebook).
  https://ccrma.stanford.edu/~jatin/Notebooks/adaa.html
- [S33] FMinus's [README](../../../../README.md) and [simd.md](../../../simd.md) (float only, no fusion, `poly float`,
  expf throughput on M5), [`src/runtime/MathKernels.h`](../../../../src/runtime/MathKernels.h); TestingPlayground's
  README (frozen dials, dial streams, voices, flush-to-zero). Read 2026-10-28.
- [S34] A. Fettweis, "Wave Digital Filters: Theory and Practice", Proc. IEEE 74(2), 1986 (finite-wordlength robustness)
  [M].
- [S35] "Cheap non-linear zero-delay filters" (KVR forum thread on the one-step tanh linearization) [M].
  https://www.kvraudio.com/forum/viewtopic.php?p=4928592
- [S36] M. Zea, L. A. Rivera, "Residual-Driven Adaptive Multi-Rate Quadratic Programming Framework for Nonlinear Analog
  Audio Circuit Emulation", DAFx26 (paper 31 in the DAFx26 archive; text read from a local copy).
