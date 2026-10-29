# Real-time circuit compilers: the landscape, and what FMinus should borrow

*A research note of [An analog circuit compiler for FMinus](../README.md); where the two disagree, the report holds.*

Research note, 2026-10-29. Angle: the existing tools that compile or simulate circuits for real-time audio (what
each does, how well, and what to take), then the netlist syntax, model library and test data an FMinus circuit
compiler should start from. Optimisation (cost per sample at fixed accuracy and stability) is the thread through all
of it.

**Confidence tags.** **[read]**: I read the primary source during this research (paper text, the tool's own code or
docs). **[doc]**: an abstract, README or project page read during this research, not the full text. **[mem]**: my prior
knowledge, not re-checked during this research; verify before relying on it. **[inf]**: my own inference from the
sources.

**FMinus facts this note leans on** [read, FMinus's [README](../../../../README.md)]: arithmetic is `float` and `int`
only (no `double`); no bit reinterpretation builtin for user code (the compiler's own math kernels use `bitcast`
internally); builtins `fabsf sqrtf floorf fminf fmaxf sinf cosf tanf expf logf tanhf` (no `powf`, `exp2f`, `log1pf`);
`param` fields read as a **knob** (once per call), a **stream** (per sample) or **frozen** (a constant; [recipe
14](../../../extending.md#recipe-14-parameters-the-hosts-values)), with method copies for `bool` and small `int` knobs
(≤ 8 values; [knob versioning](../../../extending.md#recipe-16-knob-versioning-and-flat-streams)); `poly float` lanes;
`parallel for`; `[[clang::nonblocking]]` checked. TestingPlayground adds: dials freeze into constants after 3 s still;
synth voices; output independent of host block splits; no allocation on the audio thread.

---

## 0. Findings in brief

1. **Every tool is one of two formulations**: Kirchhoff-domain state-space (nodal DK, Holters' generalisation, MNA
   with Newton: LiveSPICE, ACME, Guitarix, RTspice, pedal-sim, PyPHS) or wave-domain (WDF: chowdsp_wdf,
   wdf_compiler, Faust `wd`, RT-WDF, VIOLA, PedalKernel). Both end in the same run-time shape: a few matrix-vector
   products plus a small nonlinear solve whose dimension is set by the nonlinear devices, not by the circuit size.
2. **The biggest measured wins come from compile-time work, not from the solver.** LiveSPICE's symbolic
   pre-elimination of linear unknowns gave **33×** [read]. wdf_compiler's "one specialised function, minimal state,
   constants folded, elements merged" is **1.1–5× faster than chowdsp_wdf and 8–19× faster than RT-WDF** on the same
   circuits, and its reductions alone cut instructions to **< 25%** [read]. Holters' exact decomposition gave only
   **0–19%** in Julia [read] (overheads dominated), but it is what makes tables feasible.
3. **Closest prior art to "FMinus circuit compiler"**: LiveSPICE (netlist-like schematic → MNA → computer algebra →
   JIT-compiled .NET code, Newton per sample) and wdf_compiler (DAFx 2026: a circuit DSL → specialised C/C++/Rust/
   Jai, hot-reloaded in < 1 s). Neither does what FMinus needs on parameters: LiveSPICE re-analyses and recompiles
   on every pot change (100 ms debounce, stale audio meanwhile); wdf_compiler regenerates per topology and takes
   params at run time unless the description fixes them (then `constexpr`). FMinus's knob/stream/frozen split
   covers both cases, which neither tool does alone.
4. **Pots are the unsolved part almost everywhere.** ACME has none [read]; Guitarix replaces them with fixed
   resistors [read]; LiveSPICE recompiles [read]; chowdsp_wdf and PedalKernel recompute adaptor impedances up the
   tree and batch R-type matrix recomputes [read]; Holters & Zölzer (DAFx-11) use a **Woodbury low-rank update**
   [read]. FMinus's frozen dial = LiveSPICE's path done right; moving dial = a parametric path recomputing
   coefficients on an absolute sample grid.
5. **Nonlinear solving: fixed cost beats clever-but-unbounded.** Holmes & van Walstijn show a good analytic first
   guess ("new iterate") makes a **fixed iteration count** safe where plain Newton diverges at high level/low fs
   [read]. The k-d tree solution cache (ACME) averages 2.3 iterations but has a **500-iteration worst case** and
   inserts at run time [read]: not real-time safe. Explicit forms (Wright ω for one diode/diode pair, canonical
   piecewise-linear for grouped devices, DAFx-26) remove iteration entirely where they apply.
6. **Precision**: LiveSPICE, RT-WDF and ACME run in `double` [read]; chowdsp_wdf, wdf_compiler and Faust `wd` run
   well in `float` (wdf_compiler's max error vs chowdsp_wdf 7.12e-5) [read]. No tool I found runs a DK state-space
   model in float and reports on it. FMinus is float-only: a real open question for the Kirchhoff-domain route.
7. **Netlist**: no audio tool reads full SPICE; the ones that read SPICE-like text (pedal-sim, RTspice, VIOLA, PyPHS,
   R-Solver) take small subsets. Recommendation (§7): a SPICE3/ngspice subset plus `.param`/`{expr}`, with FMinus's
   extras (dials, inputs, outputs) in `*fm` comment lines so the **same file runs unmodified in ngspice** and
   produces its own reference output.
8. **Start the model library from LiveSPICE's MIT-licensed part tables** (16 diodes, 39 BJTs, 8 JFET entries, 12 tubes,
   12 op-amps) [read], ACME's MIT equations (Gummel-Poon BJT, Jiles-Atherton cores) [read], and manufacturer `.model`
   lines pasted by the user; consult but do not copy Guitarix (GPL), RT-WDF (GPL), PedalKernel (AGPL).
9. **Reference data exists**: LiveSPICE's 20 example circuits + 29 test circuits with per-node stats CSVs and 3
   known-unstable circuits; ACME's 4 examples; chowdsp's 9 example circuits; Holmes' DAFx-15/-19 pages (SPICE models,
   MATLAB, measured pot laws); VIOLA's LTspice reference audio for 4 pedals; Yeh & Smith's closed-form Bassman tone
   stack; EGFxSet hardware recordings. PedalKernel's ngspice goldens show the validation pattern to copy (§9).
10. "**NodalWDF**", "**Mason**" and "**Crossroads**" netlist-to-WDF tools: **not found** after several searches. The
   netlist-to-WDF tools that do exist are VIOLA (Polimi), PedalKernel, Python-WDF and the Ravasi thesis.

---

## 1. The formulations, with the equations that matter

### 1.1 Companion models (all tools)

Trapezoidal rule on a capacitor, in Cytomic's "equivalent current" form [read, Simper, *SvfLinearTrapOptimised2*]:

    i_C[n] = g_C v_C[n] - i_eq[n],   g_C = 2C/T
    i_eq[n+1] = 2 g_C v_C[n] - i_eq[n]        (one multiply-add per capacitor per sample)

An inductor is the dual (`g_L = T/(2L)`). Backward Euler (`g_C = C/T`) is L-stable but first order; BDF2 (LiveSPICE)
is second order and L-stable [read]. The capacitor becomes a conductance plus a current source; the circuit becomes
a resistive network solved once per sample.

### 1.2 Kirchhoff domain: nodal DK and Holters' generalised state space

**Nodal DK** (Yeh, Abel, Smith, IEEE TASLP 18(4) 2010) [mem; form confirmed in Holters 2015, read]: discretise
reactive parts, run MNA with every nonlinear device replaced by a current source, then

    x[n] = A x[n-1] + B u[n] + C i[n]        states (one per capacitor/inductor)
    y[n] = D x[n-1] + E u[n] + F i[n]        outputs
    v[n] = G x[n-1] + H u[n] + K i[n]        voltages across the nonlinear devices
    i[n] = f(v[n])                           device laws

Per sample: solve the `N_nl`-dimensional implicit equation `i = f(p + K i)` with `p = G x + H u`, then two
matrix-vector products. The **K-method** tabulates `i(p)`; the table is indexed by `p`, so its dimension is what
matters.

**Holters & Zölzer, EUSIPCO 2015** [read]: each element is described by `M_v v + M_i i + M_x x + M_ẋ ẋ + M_q q = u`,
`f(q) = 0` (an implicit law in its own variables `q`), stacked block-diagonally and joined with both Kirchhoff laws at
once (`T_v v = 0`, `T_i i = 0`, a sparse-tableau-like system). With canonical states `x(n) = x̂(n) + (T/2) dx̂/dt(n)`
the trapezoidal rule becomes `ẋ̂ = (x(n) - x(n-1))/T`, `x̂ = (x(n) + x(n-1))/2`, and the general solution of the linear
part gives

    x(n) = A x(n-1) + B u(n) + C z(n)
    y(n) = D x(n-1) + E u(n) + F z(n)
    q(n) = D_q x(n-1) + E_q u(n) + F_q z(n),    f(q(n)) = 0

Why it matters: no nodal-analysis restrictions (nodes touched only by nonlinear elements, transformer-isolated
sections, a clipping op-amp as a nonlinear controlled voltage source all work), and adding a device is adding a row
to a table, not changing the method. Cost: `q` is larger than DK's `v`. The diode-clipper example needs a
**one-dimensional** table index (a fixed combination of the capacitor state, coefficient ~88 200, and the input,
coefficient 1e-3) although `q` has four entries [read].

**Dimension reduction** (Holters & Zölzer, EUSIPCO 2016) [read]: split `u` into constant and varying parts, then
rank-factorise `[D_q  Ẽ_q] = Q [D̂_q  Ê_q]` (QR) so the solver sees `p = D̂_q x + Ê_q ũ` of minimal dimension, and
`q = q0 + Q p + F_q z`. In their treble booster, the 9 V protection diode and the 100 µF supply cap drop out
automatically and `p` is 2-D, not 3-D. A Boss SD-1 clone (4 diodes, 2 BJTs, 11 caps; `f` is 8 equations in 16
unknowns) reduces to a **7-D** `p`.

**Decomposition** (Holters & Zölzer, DAFx-17) [read]: find a block-triangular grouping of the nonlinear system
automatically (exact, using arbitrary-precision rationals because "near zero" thresholds failed), each block with
its own minimal `p_m`. SD-1-like overdrive: one 5-D problem becomes blocks with `p` of 0, 2, 1 and 2 dimensions.

### 1.3 Wave domain (WDF)

Waves `a = v + R_p i`, `b = v - R_p i` per port; elements and series/parallel adaptors are explicit; a tree with a
reflection-free port towards the root is computable without iteration. Non-series-parallel topologies need an
**R-type adaptor**, derived from MNA (Werner, Smith, Abel, DAFx-15) [read]:

    S = I + 2 [0  R] X⁻¹ [0  I]ᵀ          (X: MNA matrix of the circuit with each port as a Thévenin source)

One nonlinear one-port at the root is solved explicitly (diode: Lambert W / Wright ω). Several nonlinearities need
a joint solve at the root (RT-WDF: multidimensional Newton [read]), the scattering iterative method (SIM: one local
solver per nonlinearity, fixed-point in the wave domain; Proverbio, Bernardini, Sarti, EUSIPCO 2020 [read]), or
the wave-digital Newton–Raphson (Bernardini et al., IEEE/ACM TASLP 29, 2021 [read]), whose quadratic-convergence
basin grows when each port resistance is set close to the device's `dv/di`.

### 1.4 Port-Hamiltonian (PHS)

Netlist → graph → `(storage, dissipation, sources)` with an energy-conserving interconnection; discrete-gradient
schemes keep a discrete power balance, hence **guaranteed passivity / numerical stability** (Falaize & Hélie,
Applied Sciences 6(10):273, 2016 [doc]). Implemented in PyPHS (§2.4).

### 1.5 Non-iterative time-stepping (where the field is moving)

Ducceschi & Bilbao's non-iterative schemes (DAFx20in21; IEEE/ACM TASLP 30, 2022) [doc via citations], shown by
Holters (DAFx-24) to work for general ODEs at second order and to relate to implicit midpoint [doc]; Andò, Bozzo,
Fontana (DAFx-25) place them among Rosenbrock and exponential methods and report they can beat iterative methods in
total cost, with A-/L-stability analysis [read]. Zea & Rivera (DAFx-25, DAFx-26) linearise device laws into a
QP/pseudo-inverse step with residual-driven adaptive sub-stepping, tested against SPICE on a diode clipper, BJT
stage and Colpitts oscillator [doc]. These give constant cost per sample by construction: worth a dedicated note.

---

## 2. Tool by tool

### 2.1 LiveSPICE (Dillon Sharlet) — the closest Kirchhoff-domain analogue

| | |
|---|---|
| Method | MNA → DAE `A y' + B y + F(y) = c`; row-reduce into an ODE group (BDF2 or trapezoidal) and an algebraic group; **symbolic** (own CAS, `ComputerAlgebra`) back-substitution so only the nonlinear unknowns are solved numerically at run time [read: blog, `Simulation.cs`] |
| Input | Own schematic editor, `.schx` (XML); SPICE model types D, NPN, PNP, NJF, PJF; subcircuits [doc] |
| Devices | Shockley diode (no Rs), **Ebers–Moll transport BJT** (IS, BF, BR only; `LinExpm1` guards overflow), JFET, Koren triodes/pentodes and Dempwolf–Zölzer 12AX7, op-amp macromodel (Rin, Aol, single pole at GBP/Aol, Rout, soft rail clamp by two diodes 2 V inside the rails), transformers, pots, switches, speaker [read] |
| Solver | Newton per substep, start from the previous step, exit when `|Δv| < 1e-4·|v| + 1e-6` or after **8 iterations** (silently keeps the last iterate); linear system solved at run time by **Gauss–Jordan with partial pivoting** (`Vector<double>` row ops); NaN/Inf check every 256 samples throws [read] |
| Rate | `Oversample` default **8** (app) / **2** (VST); input linearly interpolated across substeps, output **box-averaged** [read] |
| Parameters | Pot wipe and values are plain doubles baked into the analysis. A pot change is debounced **100 ms**, then the circuit is re-analysed and re-solved on a background task, swapped in under a lock; audio keeps running on the old solution meanwhile [read, `SimulationProcessor.cs`]. The compiled delegate is invalidated by the swap and rebuilt lazily inside the next `Run()`, i.e. on the audio thread [read, moderately sure] |
| Codegen | `System.Linq.Expressions` → .NET JIT, all `double` [read] |
| Performance | Symbolic pre-elimination: **33×** faster, taking one amp circuit from below real time to about 7× real time (2014, machine unstated) [read, blog] |
| Licence / maturity | MIT; 558 stars; Windows/.NET 6 only; hobby project, active 2013–2020s [doc] |

*Does well*: the right architecture (circuit → algebra at analysis time → JIT code), broad device set, a real test
corpus. *Does badly*: O(n³) pivoted solve inside every Newton iteration; data-dependent iteration count; crude
anti-aliasing (linear-interp in, box filter out); pots cost a full re-analysis plus recompile; BJTs without Early
effect or terminal resistances; three circuits in `Tests/Unstable` (Fuzz Face, Orange Squeezer, a triode stage)
[read]. The blog notes analysis-time pivot choice can fight numerical pivoting [read].
**Borrow**: symbolic elimination of every linear unknown before code generation; the test corpus and its `Stats`
CSVs; the MIT part library (§8); the debounce-then-rebuild shape for frozen dials (but compile off the audio thread
and keep the state across the swap).

### 2.2 ACME.jl (Martin Holters, HSU Hamburg) — the reference implementation of the generalised state space

| | |
|---|---|
| Method | Holters–Zölzer 2015 (§1.2), with dimension reduction (2016) and automatic decomposition (2017, exact rationals) [read] |
| Input | A Julia macro, `@circuit begin … end`, `refdes = element(...)`, `a[pin] ⟷ b[pin]`; **no netlist file format** [read] |
| Devices | R, C, L, **Jiles–Atherton** inductor/transformer, coupled transformer, sources with series/parallel R, probes, Shockley diode (`is`, `η`), **Gummel–Poon BJT** (16 params), MOSFET (vt, α, λ; polynomial in v_GS), linear op-amp (finite gain, GBW), clipping op-amp (Mačák) [read] |
| Solver | `HomotopySolver{CachingSolver{SimpleSolver}}` by default: Newton from a Jacobian-extrapolated guess; homotopy for global convergence; k-d tree cache stores a solution whenever Newton needs > 5 iterations [read] |
| Parameters | Fixed at element construction; no potentiometers or run-time parameter path [read] |
| Rate | Whatever `Ts` you give `DiscreteModel` [read] |
| Performance | Treble booster: 33.6 s of guitar in 2.45 s (decomposed 1.99 s) ≈ **1.6 µs/sample**; SD-1-like overdrive 6.82 s ≈ **4.6 µs/sample**, 2.97 Newton iterations/sample (Julia 0.6, Xeon E5-1620v2) [read]. k-d cache on the SD-1 clone: 27 039 stored solutions after ~1 h training, mean **2.31** iterations, 88 samples > 15 iterations, **worst 500** [read] |
| Licence / maturity | MIT ("Expat"); v0.10.0 (2023-01-27) is the latest release; the README says it is aimed more at deriving the model matrices than at running models fast [read] |

*Does well*: the cleanest general derivation; device description by implicit equations; automatic removal of
irrelevant parts; steady-state initialisation (`steadystate!`); exact arithmetic during derivation.
*Does badly*: it runs in Julia, not as generated code; no pots; the cache allocates and has no worst-case bound.
Decomposition gains were small because of constant per-call overhead: with LAPACK, a size-8 solve took less than
5× a size-1 solve [read]: an artefact a JIT with fully unrolled small solves does not have
[inf]. **Borrow**: the element table form, `p` dimension reduction, exact decomposition, Jacobian extrapolation of
the initial guess (`z0 = z* - (J F_q)⁻¹ J Q (p - p*)`), steady-state init; the k-d cache only as an **offline**
training aid to build fixed tables, never at run time.

### 2.3 Guitarix DK simulator ("dkbuilder", `trunk/tools/ampsim/DK`)

| | |
|---|---|
| Method | Nodal DK; Python (2.7) builds the model, generates and compiles C++ for analysis, emits **Faust IIR for the linear part** and **precomputed tables** for nonlinear stages (`trany_table_gen.py`, `diode_table_gen.py`; spline code `intpp`, MINPACK `hybrd`) [read: README, file list] |
| Input | Hand-written netlist (`circ.py`) or gEDA `gschem` schematics with custom symbols [read] |
| Devices | R, C, L, V, VCCS, linear and tanh-saturating op-amp, pots (`P_parallel` log-like taper `a/(2-a)` / `(1-a)/(1+a)`, `P_single` linear), Shockley diode, diode pair (`2 Is sinh`), Ebers–Moll BJT, Koren triode and pentode with grid current, Frohlich and power-law transformer cores [read, `models.py`] |
| Parameters | The README's FuzzFace example replaces the volume pot by a fixed 1 MΩ and re-adds volume in Faust [read] |
| Performance | README: the full nonlinear C++ path (`Circuit.deploy()`) costs far more CPU than is acceptable [read] |
| Licence / maturity | GPL; marked experimental and unfinished; Python 2.7, best run in its Docker image [read] |

**Borrow**: the split "linear stages as IIR, each nonlinearity as a low-dimensional table" is the cheapest form when
it is valid; the device equations are a useful cross-check (do not copy: GPL).

### 2.4 PyPHS (Falaize, Hélie; IRCAM/La Rochelle)

| | |
|---|---|
| Method | Port-Hamiltonian; netlist → graph → `Core` → `Method` (theta schemes, trapezoidal, **discrete gradient**) [doc] |
| Input | `.net`, "very similar to SPICE": `electronics.diode D ('n1', 'n2'): Is=('Is', 2e-9);`, ground `'#'` [doc] |
| Output | C++ (`core.cpp/.h`, `parameters.cpp/.h`, Eigen) and LaTeX [doc] |
| Licence / maturity | Docs say CeCILL-B; the GitHub sidebar shows GPL-3.0 (check `LICENSE`); Beta, last active ~2019, Python 2.7/3.5 [doc] |
| Performance | No claims [doc] |

**Borrow**: passivity as a compile-time guarantee option for circuits that blow up under the trapezoidal rule (self
oscillators, high-gain feedback); a netlist with named parameters.

### 2.5 chowdsp_wdf (Jatin Chowdhury) + R-Solver

| | |
|---|---|
| Method | WDF; two APIs: run-time polymorphic and **compile-time templates** (adaptor children as template types; the voltage-divider example compiles to zero calls) [read, arXiv 2210.12554] |
| R-type | User supplies the scattering matrix through a functor (`calcImpedance`, `getPortImpedances`, `setSMatrixData`), typically from **R-Solver** [read] |
| R-Solver | Python + SageMath, symbolic: netlist of port Thévenin pairs (`Va 2 1`, `Ra 2 3`, …) → MNA `X` → `S` in terms of port resistances, optional port adaptation, `simplify_rational` [read, `r_solver.py`, `netlists/bassman.txt`]; web version at chowdsp.com/rsolver; same name and purpose as Samuel Schachter's MATLAB R-solver (Rochester MS thesis, 2021), relation not stated [doc] |
| Devices | Diodes and diode pairs via Wright ω (D'Angelo's MIT approximation) [read] |
| Parameters | Component setters propagate impedance changes up the tree; **deferred updates** batch several changes before the R-type recompute [read] |
| SIMD | XSIMD batches: several circuits (voices) per vector, or the R-type matrix multiply [read] |
| Performance | 1000 s of audio at 48 kHz on a 2018 Mac mini (i7 3.2 GHz), units presumed seconds: diode clipper **2.04 s ≈ 42 ns/sample**; Bassman tone stack 0.71 s ≈ 15 ns; RT-WDF 7.98 s ≈ 166 ns on the tone stack [read] |
| Licence / maturity | BSD-3; used in BYOD, ChowKick, ChowCentaur [read] |

*Does badly*: you build the tree by hand; one nonlinearity per tree; R-type matrices come from an external symbolic
tool. **Borrow**: SIMD over voices, deferred impedance updates, the Wright ω diode, R-Solver's symbolic `S`.

### 2.6 wdf_compiler (Chowdhury & Rau, MIT; DAFx-26) — the closest wave-domain analogue

| | |
|---|---|
| Method | Static code generation: a declarative WDF DSL → C, C++, Rust or Jai with three structs (`Params`, `Impedances`, `State`), `calc_impedances` and `process` [read, DAFx26 paper 23] |
| Input | A **WDF connection tree**, not a netlist: `circuit: IVs(Vin); Series(S1); R(R1, 1k); C(C1, 1u);  inputs: Vin  outputs: V:C1  meta: namespace:rc_lowpass`; translating netlists into the DSL is stated future work [read] |
| Optimisations | Single function, **minimal persistent state** (RC lowpass: 1 state, 1 impedance vs ~20 persistent variables in chowdsp_wdf); static params become `constexpr`; **circuit reduction** (merge series/parallel R, RC pairs; never an element whose voltage/current is an output) — 16 reductions on a test circuit cut instructions to < 25% and cycles to < 50%; **2-multiply series adaptor** for ILP (fewer cycles although more multiplies); optional per-parameter `calc_impedances` variants for parameters that change more often [read] |
| Devices | R, C, L, series/parallel, inverters; library elements: diodes and diode pairs (Wright ω, D'Angelo approximation), R-type adaptors with SSE/NEON [read] |
| Accuracy | Max error vs chowdsp_wdf 7.12e-5 in single precision [read] |
| Performance (ns/sample, Zen 4 / M1) | RC 1.21 / 1.37; pre-amp EQ 12.8 / 18.4; diode clipper **22.8 / 35.1**; Baxandall EQ (R-type) **8.6 / 11.4**. chowdsp_wdf: 6.4/7.0, 19.0/20.7, 28.3/41.1, 22.2/20.5; Faust `wd`: 3.0/6.0, 15.9/22.9, 90.2/116.5, 14.7/18.5; RT-WDF: 20.3/22.5, 100/108, –, 161/183. `batch<float>`: 2.2 ns vs chowdsp 14.1 ns [read] |
| Hot reload | Prototype plugin watches the circuit file, regenerates Jai, builds a dylib and swaps the callback in well under a second [read] |
| Licence / maturity | MIT; written in **Jai**, which is not publicly available, so most people cannot build it [read] |

**Borrow, nearly all of it**: the three-struct split maps onto FMinus (`Params` ↔ `param` fields, `Impedances` ↔
coefficients recomputed when a knob moves or folded when frozen, `State` ↔ module fields); the reductions; the
2-multiply adaptor; the paper's "performance score" `S = (L - M)/(L - T)` against latency- and throughput-bound
estimates as a regression metric for the generated code; the hot-reload loop, which FMinus already has.

### 2.7 Faust `wd` (wdmodels.lib; Roosenburg, Stine, Michon, Chowdhury; SMC-21)

| | |
|---|---|
| Method | WDF adaptors as Faust functions; circuit is a tree written with Faust's `:` and `( , )`, built with `buildtree` or `builddown ~ buildup : buildout` [doc] |
| Devices | R, C, L (V/I out), sources, ideal switch, ideal / Chua / single / pair / antiparallel diodes (`lambert` Halley with a **fixed 3 iterations**, or `omega` Wright ω with polynomial + Newton), ideal and active transformers, 3-port series/parallel, one R-type (`u_sixportPassive`), `genericNode` for custom scattering [read] |
| Parameters | Component values may be signals or sliders (time-varying tests in the doc) [read]; Faust hoists slider-only computations to block rate [mem] |
| Performance | See wdf_compiler row: fast on linear circuits, ~3–4× slower than chowdsp_wdf and wdf_compiler on the diode clipper; the paper attributes it to fixed-iteration Lambert W, because Faust cannot express the bit-level approximation [read] |
| Licence | Per-function `LicenseRef-STK-4.3` (MIT-like) [read] |

**Lesson for FMinus** [inf]: a language without bit reinterpretation pays ~3–4× on diode circuits. FMinus needs
`as_int`/`as_float` builtins (or the circuit compiler emits IR directly) to use the fast Wright ω.

### 2.8 RT-WDF (Rest, Dunkel, Werner, Smith; DAFx-16)

| | |
|---|---|
| Method | Object-oriented WDF tree in C++ (`wdfTree`, `wdfRootSimple`, `wdfRootNL`), SPQR-derived adaptor structures, R-type root with matrices `E, F, M, N`; **multidimensional Newton** at the root (e.g. a 12AX7 two-port `f_NL(v_pk, v_gk) = (i_p, i_g)` with its Jacobian) [read] |
| Performance | 2013 i7 2.4 GHz, double, 44.1 kHz: attenuator ≈ 0.11 µs/sample, Fender tone stack ≈ 0.39 µs, common-cathode triode amp ≈ **2.1 µs/sample** (normalised 0.092; 0.35 at 4× oversampling); **48–369× faster than LTspice** on the same circuits [read] |
| Licence / maturity | GPL; Armadillo; 41 commits, effectively unmaintained since 2016 [read/doc] |

**Lesson**: virtual dispatch and per-object state cost 8–19× (wdf_compiler's measurements). **Borrow**: the
nonlinear-root interface (device returns `f` and `J`), the comparison method against SPICE.

### 2.9 VIOLA and the Polimi line (Bernardini, Giampiccolo, Sarti et al.)

- **VIOLA** (Giampiccolo, Ravasi, Bernardini, JAES 2025) [doc]: **SPICE netlist → WDF → MATLAB Audio Toolbox
  plug-in** with generated GUI; this release handles circuits whose only nonlinear elements are **diodes**; tested on
  the Op-Amp Big Muff Pi and Digitech Overdrive Preamp 250; the project page has LTspice reference audio for these
  plus the Bluesbreaker and Distortion+ at stated pot settings. Code availability: not stated; full text restricted.
  Ravasi's MSc thesis (Polimi, Dec 2024) describes the same generator for "multiple nonlinear one-port elements" [doc].
- **SIM + DSR** (Proverbio et al., EUSIPCO 2020) [read]: SIM's bottleneck is recomputing `S` every sample; DSR only
  updates the port resistances when the diodes' Thévenin resistances drift past a threshold. A 5-diode asymmetric
  clipper plugin: **~30% less CPU at 4×, ~25% at 8× oversampling**.
- **WD Newton–Raphson** (TASLP 2021) [read]: port resistance ≈ device `dv/di` maximises the convergence basin;
  tested on a 4-diode ring modulator.
- **Explicit models**: canonical piecewise-linear (CPWL) curves in the WD domain (Bernardini & Sarti, EUSIPCO 2017)
  [doc]; Fulltone OCD with MOSFETs + germanium diode grouped into **one explicit CPWL element, no iteration**, real-time
  JUCE plugin (DAFx-26) [doc]; MXR Phase 90 with JFETs as time-varying resistances, MATLAB + LTspice files on GitHub
  (DAFx-24) [doc]; neural device models inside WDFs (hysteresis, EURASIP JASMP 2023; a pentode, ISCAS 2025; KAN vs MLP,
  DAFx-26) [doc].

**Borrow**: SIM's one-solver-per-device structure (parallel across devices, cheap per iteration); DSR's
"recompute only on drift"; port-resistance choice; CPWL for grouped devices when a table would be ≥ 3-D.

### 2.10 PedalKernel (Avery Wagar; Rust; AGPL-3.0 + commercial)

The most complete open netlist-to-real-time compiler found, and the most recent [read: docs site].

| | |
|---|---|
| Input | Own `.pedal` DSL: `components`, `nets`, `controls` blocks, e.g. `R1: resistor(4.7k)` |
| Pipeline | graph → op-amp neighbourhood analysis → DSP-block pre-pass (BBD, VCO, VCA, reverb) → signal-flow grouping that cuts at active outputs (ProCo RAT: **16 → 5 stages**) → per-instance **DC bias analysis** → custom series/parallel ("SPQR") decomposition, irreducible parts to MNA-derived R-type → per-stage root choice (~20 kinds: diodes explicit Wright ω, tubes Newton on Koren, BJT/JFET/MOSFET/OTA roots) → multi-nonlinearity groups by multi-port Newton or **blockwise decomposition** (e.g. a TB-303 ladder: a 4×4 Newton → four cascaded 1-port solves, only if each block has state and the coupling is passive) |
| Tables | K-method tables for memoryless roots: **1-D 1024 points** (diodes, zeners), **2-D 256×256** (BJT, JFET, MOSFET, triode), bilinear interpolation, every grid point a cold-start Newton solve |
| Linear parts | Lowered to biquad cascades when possible, else state-space `(A, B, C, D)` |
| Pots | 10 ms one-pole smoothing, `set_pot` per sample, dirty-path impedance updates (~O(log n)), multi-nonlinearity scattering re-inversion batched **every 32 samples**; biquad coefficient recompute (sqrt, sin, cos, ~25 ops) not yet throttled |
| Validation | `pedalkernel-validate`: ngspice goldens committed as `.npy`, metrics RMS / peak / spectral (full band and audio band) / THD in dB; CI gate ≥ 90% pass; latest run **91/100** (Big Muff stage and fuzz core fail with audible divergence) |
| Performance (Apple M, 48 kHz, no oversampling) | Fuzz Face ≈ 8 ns/sample; RAT ≈ 63; Tube Screamer ≈ **263**; Klon ≈ 286; Big Muff ≈ 357; Blues Driver ≈ 1222. Diode Newton ≈ 220 FLOPs (55 × 4 iterations) of a ~250-FLOP clipper stage |
| Maturity | ~1 281 commits, 20 stars; docs flag failing pot-sweep and gain-recompute tests at the reviewed commit |

*Caveat*: its decomposition is a home-grown series/parallel detector, not a published algorithm (its docs say so), and
it silently approximates (cut at "ideal" op-amp outputs, tables). **Borrow**: stage cutting at low-impedance
outputs (big win, validate it per circuit); bias analysis; table sizes as a starting point; the validation harness
design. Do not copy code (AGPL).

### 2.11 Smaller netlist-to-audio projects

| Project | What | Licence / state |
|---|---|---|
| **RTspice** (thadeuluiz) | SPICE-like netlist (V/I incl. external JACK inputs, E/F/G/H, R incl. knob-bound, C, L, ideal op-amp, diode IS/N, BJT IS/BF/BR, `PROBE`) → sparse MNA, trapezoidal, Newton; JACK host; lists CUDA, Boost.Spirit, Qt5 | GPL-3.0; 24 commits, 2019 [doc] |
| **pedal-sim** (RPHeidrick) | SPICE `.cir` **and LTspice `.asc`** import (rotations, mirroring, T-junctions); `.step param` on resistors become knobs; MNA + compiled sparse LU + trapezoidal + Newton per sample; **polyphase FIR oversampling 1–8×** with load-adaptive steps; C++→WASM in an AudioWorklet, float32 | "All rights reserved"; 2 commits, 2026 [doc] |
| **Python-WDF** (Liguha) | `Netlist` class → S, P, R adaptors built automatically; MNA stamps per element | MIT; 15 commits [doc] |
| **pywdf** (Anthon, Lizarraga; DAFx-23) | Python WDF prototyping, diode clipper, frequency/transient analysis | [doc] |
| **LiveSPICEVst** | VST3 host for `.schx` circuits (now inside the LiveSPICE repo) | MIT [read] |

### 2.12 ngspice and the other SPICEs (references, not real-time engines)

- **ngspice**: base licence BSD-3 (the FAQ calls it the new BSD licence from UCB); `src/osdi` (Verilog-A via OpenVAF,
  since ngspice-39) is MPL-2.0 [doc]. Latest: Wikipedia lists ngspice-47 (11 Aug 2026) [doc, unverified]. Reads PSpice/
  LTspice model parameters and HSPICE libraries [doc].
- **Shared library** (`sharedspice.h`): load a netlist from memory, run in a background thread, read vectors at each
  time point, alter parameters and resume; **external sources** fed by host callbacks registered with
  `ngSpice_Init_Sync(GetVSRCData, GetISRCData, GetSyncData, …)` [doc]. KiCad uses it. Good for an off-thread
  "check this circuit" in the plugin and for generating goldens; not for audio (adaptive steps, allocation).
- **LTspice** (free, closed) is what most papers use as the reference (RT-WDF, VIOLA, Phase 90) [read/doc];
  **Xyce** (Sandia, GPL-3) and **QSPICE** (free, closed) are alternatives [mem].

### 2.13 What was searched for and not found

"NodalWDF", a "Mason" or "Crossroads" netlist-to-WDF tool, a SPICE-to-Faust translator, and recent ICASSP papers on
automatic circuit-to-DSP: several searches each, nothing. The netlist-to-WDF tools that exist are VIOLA, PedalKernel,
Python-WDF and the Ravasi thesis; the netlist-to-MNA ones are LiveSPICE, RTspice and pedal-sim.

---

## 3. Side by side

### 3.1 Features

| Tool | Domain | Input | Multi-NL solver | Pots at run time | Codegen | Precision | Licence |
|---|---|---|---|---|---|---|---|
| LiveSPICE | MNA, BDF2/trap | `.schx` schematic | Newton ≤ 8, pivoted Gauss–Jordan | re-analyse + recompile (100 ms debounce) | .NET JIT | double | MIT |
| ACME.jl | generalised SS | Julia macro | Newton + homotopy + k-d cache | none | none (Julia) | double | MIT |
| Guitarix DK | nodal DK | gschem / Python netlist | tables (offline Newton) | workarounds | Faust IIR + C++ | double | GPL |
| PyPHS | PHS | SPICE-like `.net` | discrete gradient (solver not documented) | params in C++ | C++/Eigen | double | CeCILL-B? |
| chowdsp_wdf | WDF | C++ code | one NL at root | impedance propagation, deferred | templates | float/SIMD | BSD-3 |
| wdf_compiler | WDF | WDF-tree DSL | one NL / custom | `calc_impedances` variants | C, C++, Rust, Jai | float/SIMD | MIT |
| Faust `wd` | WDF | Faust tree | one NL at root | signals / sliders | Faust → C++ | float | STK-4.3 |
| RT-WDF | WDF | C++ code | Newton at R-type root | manual | runtime objects | double | GPL |
| VIOLA | WDF (SIM) | SPICE netlist | SIM (diodes) | generated GUI | MATLAB plug-in | double | ? |
| PedalKernel | WDF + SS | `.pedal` DSL | Newton, blockwise, tables | smoothed, per sample, 32-sample batches | Rust runtime | float? | AGPL |
| RTspice | MNA | SPICE-like | Newton | knob-bound R | runtime | ? | GPL |
| pedal-sim | MNA | `.cir`, `.asc` | Newton | `.step param` knobs | C++→WASM | float32 | proprietary |

### 3.2 Cost per sample (rough: different machines, rates, circuits)

| Circuit | Tool | ns / sample | Machine |
|---|---|---|---|
| RC lowpass | wdf_compiler | 1.2–1.4 | Zen 4 / M1 |
| RC lowpass | RT-WDF | 20–23 | Zen 4 / M1 |
| Bassman / Baxandall (R-type) | wdf_compiler | 8.6–11 | Zen 4 / M1 |
| Bassman tone stack | chowdsp_wdf | ~15 | i7 3.2 GHz (2018) |
| Diode clipper | wdf_compiler / chowdsp_wdf / Faust `wd` | 23–35 / 28–42 / 90–117 | Zen 4 / M1 |
| Fuzz Face | PedalKernel | ~8 | Apple M |
| Tube Screamer | PedalKernel | ~263 | Apple M |
| Big Muff | PedalKernel | ~357 | Apple M |
| Klon (nodal + WDF) | Chowdhury 2020 | ~1 500 (0.066 s per s of audio, 44.1 kHz assumed) | i7 2.9 GHz (2017) |
| Triode common-cathode | RT-WDF | ~2 100 | i7 2.4 GHz (2013) |
| Treble booster / SD-1-like | ACME (Julia) | ~1 600 / ~4 600 | Xeon 3.7 GHz |
| "an amp circuit" | LiveSPICE | ~3 000 (≈ 7× real time, 2014) | unstated |

Holmes & van Walstijn's operation counts (exp = 40 ops; an LU solve ≈ `M³ + M²/2 + 14.5 M` ops): one Newton iteration
costs **253 ops** for a diode clipper and **646** for the Rangemaster, plus 234 / 624 setup [read]. At 48 kHz,
16 voices, 2× oversampling and ~1 000 ops/sample, a voice-circuit costs ~1.5 GFLOP/s: fine for one effect, tight for
a 16-voice synth unless the compiler gets the per-voice circuit down to a few hundred ops [inf].

---

## 4. Industry: what Cytomic, NI and UA have published

- **Cytomic (Andrew Simper)** [read, technical papers page and *SvfLinearTrapOptimised2*]: nodal analysis with
  trapezoidal companion currents (§1.1), solved symbolically by hand-ordered elimination; the papers note that each
  eliminated dependency costs a division, that grouped direct solutions grow exponentially for larger systems, and
  that the form with **fewer operations can be slower than the more parallel one** (profile both). The same lesson
  as wdf_compiler's 2-multiply adaptor. ADC 2020 talk "From Circuit To Code: Under the Hood of Analog Modelling"
  (slides > 10 MB, not read).
- **Native Instruments**: Zavalishin, *The Art of VA Filter Design* (rev. 2.1.0, 2018, free from NI): topology-
  preserving transform / zero-delay feedback, the standard for hand-derived filters [doc]. ADAA originates there
  (Parker, Zavalishin, Le Bivic, DAFx-16) [mem]. Parker, Esqueda, Bergner, *Modelling of nonlinear state-space systems
  using a deep neural network* (DAFx-19): a DNN inside a discrete state-space structure, two diode distortions and the
  MS-20 filter [doc]. Esqueda, Kuznetsov, Parker, *Differentiable white-box virtual analog modeling* (DAFx-21):
  trapezoidal discretisation, mesh analysis for the FMV tone stack, Newton for the Tube Screamer clipper, component
  values learned by backpropagation from device recordings and a 2-layer tanh MLP for each pot law; TS MSE
  6.4e-3 → 1.5e-3 after training [read]. **Borrow**: fit unknown component values and pot laws offline against
  recordings; it plugs straight into a white-box compiler.
- **Universal Audio (Berners, Abel)**: component-level ("physical", "bottom-up") modelling from schematics, with
  targeted measurements designed *after* the model exists to pin unknown parts; expensive parts dropped only when
  psychoacoustics says inaudible, sometimes shipping two plug-in versions [doc, CDM interview 2011]. Little on
  solvers is public (AES papers on shelf-filter design, spring reverb, nonlinear system measurement) [doc/mem].
- **Outside audio, same problem**: Simscape's *partitioning solver* splits a network into switched-linear partitions
  joined by nonlinear functions and **caches matrix inverses per mode** (2ⁿ modes; "exhaustive" precompute or "as
  needed"; fixed-cost iterations for real time) [doc]; **PLECS** embeds state-space matrices for every reachable
  switch combination in generated real-time code (Tustin discretisation) [doc]. **Borrow** for piecewise-linear device
  models (ideal diodes, CPWL): precompute per-mode matrices at compile time, select per sample.

---

## 5. Papers on automatic circuit → DSP, 2018–2026 (most relevant to a compiler)

| Year, venue | Authors | Contribution | Relevance |
|---|---|---|---|
| 2018 TCAS-I | Werner, Bernardini, Smith, Sarti | arbitrary topologies + active linear multiports in WDF (MNA-derived R-type) | R-type generation |
| 2018 DAFx | Müller, Hélie | power-balanced modelling as skew-gradient systems | passivity |
| 2018 DAFx | Zhang, Smith | modified blockwise method for cascaded tube stages | decomposition |
| 2018/2019 TASLP | Fontana, Bozzo | explicit fixed-point / Newton for nonlinear delay-free loops | solvers |
| 2019 DAFx | D'Angelo, Gabrielli, Turchet | Wright ω reformulation + fast approximations (ω1–ω4), no audio-rate exp | diode cost |
| 2019 DAFx | Holmes, van Walstijn | pot laws from I/O measurements (Big Muff tone stack; data online) | pots |
| 2019 DAFx | Holters | ADAA for stateful systems | less oversampling |
| 2020 EUSIPCO | Proverbio, Bernardini, Sarti | SIM cost analysis + DSR | multi-NL |
| 2020 arXiv | Chowdhury | Klon: nodal vs WDF vs RNN, desktop + embedded | method choice |
| 2021 TASLP | Bernardini, Bozzo, Fontana, Sarti | WD Newton–Raphson, port resistances vs convergence basin | multi-NL |
| 2021 DAFx | Esqueda, Kuznetsov, Parker (NI) | differentiable white-box models | calibration |
| 2021/2022 DAFx/TASLP | Ducceschi, Bilbao (+Webb) | non-iterative schemes for nonlinear circuits | constant cost |
| 2021 SMC | Roosenburg et al. | Faust `wd` | library |
| 2022 arXiv; SMC | Chowdhury; Chowdhury & Clarke | chowdsp_wdf; differentiable WDF diodes | library, calibration |
| 2022 DAFx | Darabundit, Roosenburg, Smith | neural tube models for WDF | device models |
| 2023 DAFx | Giampiccolo, D'Angelo, Bernardini, Sarti | quadric-surface vacuum-tube model | device models |
| 2023 SPL | Fontana, Bozzo, Bernardini | extended fixed-point methods | solvers |
| 2024 DAFx | Holters | 2nd-order non-iterative scheme for general ODEs, link to implicit midpoint | constant cost |
| 2024 DAFx | Giampiccolo et al. | MXR Phase 90 WDF, JFET as time-varying R; code + LTspice on GitHub | test circuit |
| 2025 JAES | Giampiccolo, Ravasi, Bernardini | **VIOLA**: SPICE netlist → WDF plug-in | prior art |
| 2025 DAFx | Andò, Bozzo, Fontana | non-iterative methods: Rosenbrock / DB / exponential, A/L-stability | constant cost |
| 2025, 2026 DAFx | Zea, Rivera | constraint-stabilised QP; residual-driven adaptive multi-rate | constant cost |
| 2026 DAFx | **Chowdhury, Rau** | **wdf_compiler**: static WDF codegen, reductions, performance score | prior art |
| 2026 DAFx | Giampiccolo et al. | explicit WDF of the Fulltone OCD via CPWL (no iteration) | explicit NL |
| 2026 DAFx | McClellan | BIBO stability of time-varying VA filters via common quadratic Lyapunov functions (SVF, Sallen-Key; bounds for Moog and diode ladders) | moving dials |
| 2026 DAFx | Oyama | five Moog ladder implementations vs a SPICE reference; linear agreement ≠ nonlinear agreement | synth test |
| 2026 DAFx | Gabrielli, Squartini | PolyADAA | aliasing |

---

## 6. What FMinus should borrow, ranked (optimisation first)

1. **Do all algebra at compile time, off the audio thread, in double or exact rationals; emit float code.**
   Symbolically eliminate every linear unknown (LiveSPICE, 33×), reduce `p` by rank factorisation (Holters 2016),
   decompose exactly into block-triangular nonlinear subsystems (Holters 2017), drop parts with no influence on any
   output (protection diodes, supply bypass caps: they vanish in Holters' matrices). Dropping near-unity-eigenvalue
   states such as a 100 µF supply cap also helps float precision [inf].
2. **Generate one specialised function with minimal state** (wdf_compiler): constants folded, elements merged,
   no indirection, ILP-friendly orderings (2-multiply adaptor; Simper's "fewer ops can be slower"). FMinus's LLVM
   pipeline does the folding once the compiler emits constants; the reductions must happen before codegen.
3. **Three parameter paths, mapped onto FMinus's own** [inf]:
   - *Frozen dial*: re-derive with the value as a constant and recompile (LiveSPICE's path; FMinus already
     recompiles after 3 s still). Keep `State` across the swap; compile off-thread.
   - *Moving dial*: coefficients recomputed from the knob on an **absolute 16- or 32-sample grid** (PedalKernel
     batches every 32 samples; FMinus must anchor the grid to the absolute sample index, not to call starts, or
     block splits change the output), via (a) a symbolic
     coefficient function of the pot (`calc_impedances` variant per dial, wdf_compiler), (b) a Woodbury low-rank
     update when few parts vary (Holters & Zölzer DAFx-11), or (c) per-sample streams where the dial is
     audio-rate (wah sweeps).
   - *Switches and rotary selectors*: `int` knobs with ≤ 8 values, which FMinus already versions; Simscape/PLECS
     show the same per-mode precomputation for switch states.
4. **Fixed-cost nonlinear solves.** Explicit where possible (Wright ω for a diode or diode pair; CPWL for grouped
   devices); otherwise Newton with a **fixed iteration count** from an analytic initial guess (Holmes' "new iterate":
   invert the dominant exponential, e.g. `v0 = ±N Vt log(1 + |p|/(K Is))` by the sign of `p`) or Jacobian extrapolation
   (Holters), step capping instead of damping (no branch), port resistance / linearisation point near `dv/di` (WD-NR);
   fixed counts also keep `poly float` voice lanes in lockstep [inf]. Tables (1-D 1024, 2-D 256², PedalKernel) for `p`
   of dimension ≤ 2 after reduction; no run-time caches.
5. **Cut at low-impedance outputs** (PedalKernel: RAT 16 → 5 stages) and lower purely linear stages to biquads or
   small state-space blocks (Guitarix, PedalKernel), but verify each cut against the uncut model (§9); the cut is an
   approximation the user should see.
6. **DC operating point at compile time** (ACME `steadystate!`, LiveSPICE initial conditions, PedalKernel bias
   analysis), so a new compile does not thump.
7. **Oversampling and aliasing**: polyphase FIR (pedal-sim) rather than LiveSPICE's linear-interp/box-average;
   ADAA where the nonlinearity is memoryless or Holters' stateful variant, to lower the factor.
8. **Validation harness** from day one: ngspice goldens with spectral metrics (PedalKernel), LiveSPICE-style
   per-node stats, wdf_compiler's performance score for the generated code.
9. **Bit-level builtins** (`as_int`/`as_float`) so the fast Wright ω and log/exp approximations are expressible
   (Faust's cost without them: ~3–4× on a diode clipper).

What to avoid: run-time pivoted Gaussian elimination inside Newton (LiveSPICE); convergence-tested loops with silent
caps; run-time solution caches (ACME); per-object virtual dispatch (RT-WDF); compiling or allocating on the audio
thread; per-call (block-relative) parameter updates.

---

## 7. The netlist FMinus should accept

**Recommendation**: a SPICE3/ngspice subset with the common LTspice extensions, so users can paste from LTspice
(*View → SPICE Netlist*), KiCad 8+ (`kicad-cli sch export netlist --format spice`) [doc], ngspice decks, electrosmash-
style analyses and papers. FMinus-only information goes in comment lines starting `*fm`, which ngspice and LTspice
ignore, so **one file is both the plugin's DSP and its own SPICE reference**.

### 7.1 Cards

| Card | v1 | Notes |
|---|---|---|
| `R C L` | yes | values may be `{expr}` of `.param`s |
| `K` (coupled inductors) | yes | linear transformers; Jiles–Atherton later (ACME has it) |
| `V I` | yes | DC / AC / SIN / PULSE / PWL for tests; the plugin's inputs are sources marked `*fm input` |
| `E F G H` | yes | controlled sources (op-amp macromodels need them) |
| `B` (behavioural) | yes | needed for SPICE tube and OTA subcircuits; requires symbolic differentiation in the compiler (LiveSPICE has its own CAS) |
| `D` | yes | `.model D(IS N RS BV IBV CJO …)`: static parameters used; charge parameters ignored with a warning |
| `Q` | yes | `.model NPN/PNP`: IS BF BR (Ebers–Moll), then NF NR VAF RB RE RC (Gummel–Poon subset as in ACME) |
| `J` | yes | `.model NJF/PJF`: VTO BETA LAMBDA IS (Shichman–Hodges) |
| `M` | level 1 | VTO KP LAMBDA; OCD/MOSFET clippers |
| `X`, `.subckt/.ends` with params | yes | vendor op-amp macromodels accepted, but suggest the native op-amp (cost) |
| `S W` switches | yes | mapped to `int` knobs |
| `.model .param .func .include .lib .end .ic .nodeset .temp` | yes | `.temp` sets Vt |
| `.tran .op .ac .meas .options .step` | parsed, not compiled | `.tran` + PWL input defines a test; `.step param` can seed dial ranges (pedal-sim) |
| `T O U`, XSPICE `A`, Verilog-A | no | |

Number syntax gotchas [mem, high confidence]: suffixes `T G MEG K M U N P F` are case-insensitive and **`M` is milli,
`MEG` mega** (`1M` = 1 mΩ); trailing letters after a suffix are ignored (`10kOhm`, `4.7uF`); netlists from LTspice may
contain `µ` or the `4k7` style: accept and warn.

### 7.2 FMinus extensions (comments)

    *fm input  Vin                       the source the plugin drives (several allowed: stereo, sidechain)
    *fm output V(out) V(out2,vb) I(Rload) an output expression
    *fm dial   drive 0 1 0.5 taper=A      binds .param drive to a plugin dial (range, default, taper)
    *fm switch mode 0 2                   binds an integer .param (S/W cards or {mode==1 ? … })
    *fm rate   2x                         oversampling hint;  *fm solver newton iters=3 / table / explicit
    *fm keep   R6 C4                      do not merge (wdf_compiler's "outputs are never merged" rule, made explicit)

Potentiometers: SPICE has no pot card. Ship a library `fm.lib` whose `POT` subcircuit is real SPICE (two resistors
`{R*(1-pos)}`, `{R*pos}` clamped away from 0) so the file still simulates; the compiler recognises `POT` and builds a
native three-terminal pot with the taper law (linear, A/B log, reverse; Holmes & van Walstijn's measured laws as
options). Example (illustrative TS-style clipper; values not checked against a schematic):

    * TS-style clipping stage
    .include fm.lib
    .param drive=0.5
    *fm input Vin
    *fm output V(out)
    *fm dial drive 0 1 0.5 taper=A
    Vin in 0 SIN(0 0.3 440)
    Vb  vb 0 DC 4.5
    Ri  in p 1k
    Rb  p vb 1Meg
    XU1 p n out OPAMP_BEH AOL=200k GBW=3Meg VMAX=4.0
    R4  n a 4.7k
    C3  a vb 47n
    R6  n x 51k
    XP1 x out out POT R=500k pos={drive} taper=A
    C4  out n 51p
    D1  out n D1N914
    D2  n out D1N914
    .model D1N914 D(IS=2.52n N=1.752)
    .tran 10u 50m
    .end

Import later: LiveSPICE `.schx` (XML, MIT corpus), LTspice `.asc` (pedal-sim shows it is doable: rotations, mirrors,
T-junctions). Not worth importing: ACME's macro, Faust trees, `.pedal`.

---

## 8. Model library to start with

**Equations (v1)**, each with analytic Jacobian:

- **Diode**: `i = Is (exp(v/(N Vt)) - 1)`, optional `Rs` (makes it implicit: use Wright ω), antiparallel pair
  `i = 2 Is sinh(v/(N Vt))` (Guitarix `D2`), zener via `BV, IBV`. Wright ω form (D'Angelo et al. 2019).
- **BJT**: Ebers–Moll transport as in LiveSPICE (`iF = Is(exp(vbe/Vt)-1)`, `iR = Is(exp(vbc/Vt)-1)`,
  `ie = iF - αR iR`, `ic = αF iF - iR`, `αF = BF/(1+BF)`) [read]; then the Gummel–Poon subset (VAF, VAR, IKF, ISE/NE,
  RB, RE, RC) following ACME's MIT implementation [read].
- **JFET**: Shichman–Hodges (SPICE level 1) [mem]; Phase 90 shows the time-varying-resistance simplification.
- **MOSFET**: level 1 (ACME's `vt, α, λ`) [read].
- **Triode**: Koren, `E1 = (Vpk/Kp) log(1 + exp(Kp (1/μ + Vgk/√(Kvb + Vpk²))))`, `Ip = E1^Ex / Kg1` (E1 > 0), grid
  current `Ig = Gcf (Vgk - Gco)^1.5` above `Gco` [read, Guitarix `models.py`]; Dempwolf–Zölzer with grid current
  (LiveSPICE 12AX7: G 2.242e-3, γ 1.26, C 3.4, Gg 6.177e-4, Cg 9.901, ξ 1.314, Ig0 8.025e-8) [read]. Needs `powf`
  and a softplus: FMinus has `expf`/`logf` only, so `pow(a,b) = exp(b log a)` and `log(1+exp(x))` written stably.
- **Pentode**: Koren (LiveSPICE: 6L6GC, 6V6GTA, EL34, EL84, 6550, KT88) [read].
- **Op-amp**: behavioural, as LiveSPICE (Rin, Aol, one pole at GBW/Aol, Rout, soft rail clamp) or ACME/Mačák's
  clipping op-amp; vendor macromodels only on request (many internal nodes).
- **OTA** (CA3080, LM13700) for the synth: `Iout = Iabc tanh(Vd/(2Vt))` [mem].
- **Transformer**: linear coupled inductors; Jiles–Atherton (ACME) later.
- **Pot**: three-terminal, taper laws; tolerance/ageing parameters optional.

**Parameter sets (seed)**: LiveSPICE's MIT XML tables [read] — diodes: Si, Ge, LEDs (red `Is 93 pA, n 3.73`), 1N4001,
**1N914/1N4148 `Is 2.52 nA, n 1.752`**, 1N4448, **1N34A `Is 200 nA, n 1.3`**, zeners; BJTs incl. 2N3904, 2N2222,
**2N5088 `Is 5.911 fA, BF 1122, BR 1.271`**, 2N5089, BC107–109, BC549, MPSA18, **AC128 (Ge PNP) `Is 100 nA, BF 90,
BR 20`**, 2N3906, 2N5087; JFETs 2N5952, 2N5457, 2N5458, J112, J174, 2N4392; tubes 12AT7, 12AU7, **12AX7**, 12AY7,
12AZ7, 6SL7 and six pentodes; op-amps LM308, 4558, UA741, OP27, TL06x/07x/08x (e.g. TL07x `Aol 200k, GBW 5.25 MHz`).
Gaps to fill: no diode `Rs`, no Early voltage, one value looks wrong (MPS8599 `IS 120.32e-15 fA`). Add: manufacturer
`.model` lines the user pastes (onsemi, Nexperia, Diodes Inc., TI) and Koren's published tube constants. Record
provenance and licence per part. Do not copy from Guitarix (GPL), RT-WDF (GPL), PedalKernel (AGPL); LTspice's
bundled `standard.*` model files have unclear redistribution terms [inf].

---

## 9. Test circuits and reference data

| Circuit | Why | Where (netlist / reference) |
|---|---|---|
| RC lowpass, RLC, Sallen-Key | linear sanity, closed form | LiveSPICE `Tests/Circuits`; ACME `sallenkey.jl`; chowdsp `WaveDigitalFilters` |
| '59 Bassman tone stack | linear, 3 pots, R-type | Yeh & Smith DAFx-06 closed-form transfer function [mem]; LiveSPICE; R-Solver `netlists/bassman.txt`; chowdsp_wdf / RT-WDF benchmarks |
| Baxandall EQ | linear R-type | wdf_compiler paper Fig. 4; chowdsp examples; R-Solver netlist |
| Diode clipper (sym/asym) | 1 NL | everywhere; Holmes DAFx-15 page has SPICE models + MATLAB (bholmesqub.github.io/DAFx15) |
| Treble booster / Rangemaster | BJT, protection diode | ACME `birdie.jl`; Holmes DAFx-15/-16 |
| Boss SD-1 / Der SuperOver | 4 diodes + 2 BJTs, decomposition | ACME `superover.jl`; Holters 2016/2017 numbers; LiveSPICE example |
| Tube Screamer TS-9 | op-amp + diodes in feedback, pots | LiveSPICE example; NI DAFx-21 (hardware data); PedalKernel `hardware-goldens/ts9_noon` (AGPL repo) |
| Big Muff Pi (and Op-Amp Big Muff) | 4 cascaded stages, tone stack | LiveSPICE; VIOLA LTspice reference audio; Holmes DAFx-19 measured tone stack and pot laws |
| Fuzz Face | 2 BJT feedback, **known LiveSPICE instability** | LiveSPICE `Tests/Unstable` |
| Pro Co RAT, MXR Distortion+, Bluesbreaker, Klon | common pedals | LiveSPICE examples; VIOLA page; Chowdhury 2020 (Klon, SPICE-trained) |
| Cry Baby wah | pot-swept resonance | Holters & Zölzer DAFx-11 (LV2 + audio online then); LiveSPICE example |
| MXR Phase 90 | JFETs, LFO | DAFx-24 Phase 90 WDF (MATLAB + LTspice on GitHub); LiveSPICE example |
| Fulltone OCD | MOSFET + Ge diode group | DAFx-26 explicit CPWL paper |
| Common-cathode triode, Fender 5F6-A, JCM800 preamp | tubes | RT-WDF paper; Dunkel et al. DAFx-16; LiveSPICE examples |
| Diode ring modulator | 4 diodes, multi-NL | Bernardini et al. TASLP 2021 |
| Moog ladder, diode ladder, MS-20, OTA SVF, wavefolders, LPG | synth voices | Oyama DAFx-26 (SPICE reference, open method); McClellan DAFx-26 stability bounds; NI: Buchla LPG (Parker & D'Angelo DAFx-13), Lockhart/Serge folders (Esqueda et al. 2017) [mem] |
| Wien bridge, Colpitts | self-oscillation | LiveSPICE example; Zea & Rivera DAFx-26 |

**Hardware recordings**: EGFxSet (8 970 five-second Stratocaster notes through 12 real units incl. Boss, Ibanez,
Pro Co, MXR, with knob metadata; Creative Commons; Zenodo 7044411) [doc]; the DAFx-26 compressor dataset with
control-voltage measurements (Thompson & Heilemann) [doc]; NI DAFx-21's recordings were not released as far as I
found.

**How to make SPICE references that mean something** [inf, from how the papers did it]: RT-WDF ran LTspice with
`numdgt=10` and max step 20 µs ≈ 1/fs [read], too coarse to separate discretisation error from model error. Two
references per test instead: (a) ngspice with a tight `TMAX` (≤ 1/(16 fs)), tight `RELTOL`/`VNTOL`, input as PWL,
output band-limited and decimated, to measure total error; (b) the same circuit with the compiler's own
discretisation run in double with full Newton convergence, to measure implementation error (float, fixed
iterations, tables) alone. Gate on PedalKernel-style metrics (RMS, peak, audio-band spectral and THD differences in
dB), plus a DC operating-point check against ngspice `.op`.

---

## 10. Open questions

- **Float-only state-space**: will DK/Holters models with widely spread time constants hold up in float, or does
  FMinus need `double` (or a compensated/delta form) for the state update? No source tested this. Worth one
  experiment on the Bassman stack plus a 100 µF bypass cap.
- **WDF or Kirchhoff domain as the core?** WDF is proven in float and fastest on linear and single-NL circuits;
  Kirchhoff state space handles arbitrary multi-NL circuits, pots via low-rank updates, and decomposition. A hybrid
  (WDF/linear blocks between nonlinear state-space blocks, as PedalKernel and Chowdhury 2020 do) is likely; needs a
  measured comparison on the same netlists.
- **Moving-dial stability**: McClellan's Lyapunov bounds (DAFx-26) apply to specific filters; a compiler would need
  a general check or a conservative rule for how fast a dial may move per grid step.
- **Where to cut**: PedalKernel's cuts at active outputs are approximations; an automatic test of the cut's error
  (e.g. loading impedance ratio) is not published anywhere I found.
- **VIOLA's code and algorithms** (restricted full text) and Simper's ADC 2020 slides (not read) may hold more.

---

## Sources

Tools
- LiveSPICE: https://www.livespice.org/ · https://github.com/dsharlet/LiveSPICE (MIT) · solver blog
  https://dsharlet.com/2014/03/28/how-livespice-works-numerically-solving-circuit-equations.html ·
  `Circuit/Simulation/Simulation.cs`, `LiveSPICEVst/SimulationProcessor.cs`,
  `Circuit/Components/{OpAmp,BipolarJunctionTransistor,Potentiometer}.cs`, `{Diodes,Transistors,Tubes,OpAmps}.xml`
  [read]
- ACME.jl: https://github.com/HSU-ANT/ACME.jl · docs https://hsu-ant.github.io/ACME.jl/stable/ug/ ,
  https://hsu-ant.github.io/ACME.jl/stable/elements/ · releases API [read]
- Guitarix DK simulator: https://github.com/brummer10/guitarix/tree/master/trunk/tools/ampsim/DK (README, models.py)
  [read]
- PyPHS: https://github.com/pyphs/pyphs · https://pyphs.github.io/pyphs/ [doc]
- chowdsp_wdf: https://github.com/Chowdhury-DSP/chowdsp_wdf · paper https://arxiv.org/html/2210.12554v1 [read]
- R-Solver: https://github.com/jatinchowdhury18/R-Solver [read]; Schachter thesis
  https://hajim.rochester.edu/ece/news-events/events/archives/2021/2021-06-18_schachter.html [doc]
- wdf_compiler: Chowdhury & Rau, *Performance-Oriented Wave Digital Circuit Emulation*, DAFx-26,
  https://www.dafx.de/paper-archive/2026/papers/DAFx26_paper_23.pdf · https://github.com/Chowdhury-DSP/wdf_compiler
  [read]
- Faust wdmodels: https://faustlibraries.grame.fr/libs/wdmodels/ ·
  https://raw.githubusercontent.com/grame-cncm/faustlibraries/master/wdmodels.lib [read]; SMC-21 paper
  https://zenodo.org/records/5045088 [doc]
- RT-WDF: https://github.com/RT-WDF/rt-wdf_lib · DAFx-16
  https://www.dafx.de/paper-archive/2016/dafxpapers/40-DAFx-16_paper_35-PN.pdf [read]
- VIOLA: https://polimi-ispl.github.io/viola · https://re.public.polimi.it/handle/11311/1292265 [doc]; Ravasi thesis
  https://www.politesi.polimi.it/handle/10589/230385 [doc]
- PedalKernel: https://github.com/ajmwagar/pedalkernel · https://pedalkernel.com/book/compiler-internals/ ·
  /book/controls-and-pots/ · /book/performance/ · https://pedalkernel.com/accuracy/ [read]
- RTspice https://github.com/thadeuluiz/RTspice · pedal-sim https://github.com/RPHeidrick/pedal-sim · Python-WDF
  https://github.com/Liguha/Python-WDF · pywdf https://dafx.de/paper-archive/details/ZyY5H6ybinaUMpQ4CPaMwQ [doc]
- ngspice: https://ngspice.sourceforge.io/shared.html · https://ngspice.sourceforge.io/osdi.html ·
  https://en.wikipedia.org/wiki/Ngspice [doc]
- Simscape partitioning solver:
  https://www.mathworks.com/help/simscape/ug/increase-simulation-speed-using-the-partitioning-solver.html · PLECS code
  generation https://docs.plexim.com/plecs/latest/codegeneration/ [doc]

Papers and industry
- Holters & Zölzer, EUSIPCO 2015 https://new.eurasip.org/Proceedings/Eusipco/Eusipco2015/papers/1570103545.pdf [read]
- Holters & Zölzer, k-d tree cache, EUSIPCO 2016
  https://www.hsu-hh.de/ant/wp-content/uploads/sites/699/2017/10/Holters_treecachesolver_EUSIPCO16.pdf [read]
- Holters & Zölzer, automatic decomposition, DAFx-17
  https://www.hsu-hh.de/ant/wp-content/uploads/sites/699/2017/10/Holters-Zölzer-2017-Automatic-Decomposition-of-Non-linear-Equation-Systems-in-Audio-Effect-Circuit-Simulation2.pdf
  [read]
- Holters & Zölzer, wah with variable parts (Woodbury), DAFx-11
  https://www.hsu-hh.de/ant/wp-content/uploads/sites/699/2017/10/Holters-Zölzer-2011-Physical-Modelling-of-a-Wah-wah-Effect-Pedal-as-a-Case-Study-for-Application-of-the-Nodal-DK-Method-to-Circuit.pdf
  [read]
- Dempwolf, Holters, Zölzer, parametric circuits, DAFx-10
  https://www.dafx.de/paper-archive/details/74c2I_PbWhkM6MUukii5IQ [doc]
- Holmes & van Walstijn, solver robustness, DAFx-15
  https://pureadmin.qub.ac.uk/ws/files/17395135/improving_the_robustness.pdf · https://bholmesqub.github.io/DAFx15
  [read]
- Holmes & van Walstijn, pot laws, DAFx-19 https://bholmesqub.github.io/DAFx19 [doc]
- Werner, Smith, Abel, R-type adaptors, DAFx-15 https://www.dafx.de/paper-archive/2015/DAFx-15_submission_53.pdf [read]
- D'Angelo, Gabrielli, Turchet, Wright ω, DAFx-19 https://iris.unitn.it/bitstream/11572/250641/4/DAFx2019_paper_5.pdf
  [read]
- Proverbio, Bernardini, Sarti, SIM + DSR, EUSIPCO 2020
  https://re.public.polimi.it/retrieve/e0c31c10-7864-4599-e053-1705fe0aef77/Proverbio_Bernardini_Sarti_Eusipco2020_NoOrdNumb.pdf
  [read]
- Bernardini, Bozzo, Fontana, Sarti, WD Newton–Raphson, TASLP 2021
  https://re.public.polimi.it/bitstream/11311/1203185/1/WD_NR_TASLP_Bernardini_etal_2021_RG.pdf [read]
- Andò, Bozzo, Fontana, DAFx-25 https://dafx.de/paper-archive/2025/DAFx25_paper_6.pdf [read]
- Holters, DAFx-24 https://dafx.de/paper-archive/2024/papers/DAFx24_paper_18.pdf [doc]
- Zea & Rivera, DAFx-25 https://dafx.de/paper-archive/2025/DAFx25_paper_45.pdf · DAFx-26
  https://www.dafx.de/paper-archive/2026/papers/DAFx26_paper_31.pdf [doc]
- Giampiccolo et al., Fulltone OCD CPWL, DAFx-26 https://www.dafx.de/paper-archive/2026/papers/DAFx26_paper_24.pdf [doc]
- Giampiccolo et al., MXR Phase 90, DAFx-24 https://www.dafx.de/paper-archive/2024/papers/DAFx24_paper_13.pdf [doc]
- McClellan, DAFx-26 https://www.dafx.de/paper-archive/2026/papers/DAFx26_paper_28.pdf · Oyama, DAFx-26
  https://www.dafx.de/paper-archive/2026/papers/DAFx26_paper_29.pdf [doc]
- DAFx-26 listing (circuit papers) https://www.dafx.de/paper-archive/search.php?years=2026&p=1&s=newest (pages 1–9)
  [doc]
- Chowdhury, Klon comparison, 2020 https://arxiv.org/html/2009.02833v1 [read]
- Esqueda, Kuznetsov, Parker (NI), DAFx-21 http://dafx2020.mdw.ac.at/proceedings/papers/DAFx20in21_paper_39.pdf [read]
- Parker, Esqueda, Bergner (NI), DAFx-19 https://dafx.de/paper-archive/2019/DAFx2019_paper_42.pdf [doc]
- Zavalishin (NI), *The Art of VA Filter Design* 2.1.0
  https://www.native-instruments.com/fileadmin/ni_media/downloads/pdf/VAFilterDesign_2.1.0.pdf [doc]
- Cytomic technical papers https://cytomic.com/technical-papers/ ·
  https://cytomic.com/files/dsp/SvfLinearTrapOptimised2.pdf [read]
- UA: CDM interview with David Berners (2011)
  https://cdm.link/2011/02/modeling-analog-in-a-digital-age-a-conversation-with-universal-audios-chief-scientist/ ; Abel
  & Berners AES 2003 shelf filters https://aes.org/publications/elibrary-page/?id=12440 [doc]
- Falaize & Hélie, Applied Sciences 2016 https://mdpi.com/2076-3417/6/10/273 [doc]
- KiCad netlist export formats https://kibot.readthedocs.io/en/master/configuration/outputs/NetlistOptions.html [doc]
- EGFxSet https://zenodo.org/record/7044411 [doc]
