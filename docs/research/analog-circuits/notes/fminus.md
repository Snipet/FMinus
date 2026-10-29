# An analog circuit compiler for FMinus: how it fits FMinus and the playground

*A research note of [An analog circuit compiler for FMinus](../README.md); where the two disagree, the report holds.*

Research note, 2026-10-29. Read-only survey of FMinus at `c75caab` and TestingPlayground at `45b726c`, primary sources
read in full where marked, and four small experiments run in a scratch folder (C++ and FMinus, nothing built in any
repository; Appendix A; their sources are in [../experiments/](../experiments/)). Confidence tags: **[H]** read in a
primary source or measured here, **[M]** a secondary source or a reasoned estimate, **[L]** a forum post or my own
unmeasured judgement.

## 0. Summary

- **Where:** a front end, `fminus::circuit`, a JUCE-free and LLVM-free C++ library in the FMinus repository, that turns
  a SPICE-style netlist (`pedal.cir`) into an ordinary FMinus module. The playground serves `#include "pedal.cir"` by
  running it. That puts the compiler where FMinus is, gives the user code they can read, and leaves frozen dials,
  knob versioning, `trace`, `compare` and the checks working with no change. A `circuit` block in the FMinus language
  can come later as sugar over the same library. Emitting LLVM IR directly is the wrong layer (section 3).
- **Method:** the Holters–Zölzer generalized state-space formulation (the DK family, as in ACME.jl), applied to blocks
  that the compiler cuts at ideal op-amp outputs and inputs. Trapezoidal by default. Pots enter as Woodbury low-rank
  updates computed at control rate. Each nonlinear port has a fixed-count solver chosen at compile time: a closed
  form (the Wright ω function) plus 1–2 Newton steps for diode pairs, and limited Newton for coupled ports such as
  BJTs. Only the nonlinear blocks are oversampled.
- **Measured here [H]:** on Yeh's diode clipper and the Tube Screamer clipping stage, an ω initial guess plus one
  Newton step in float keeps the solver error in band at −52 dB or better at every drive level at 1x. Two steps reach
  −92 to −137 dB. Starting from the previous sample's solution fails when the stage is driven hard: two steps leave an
  error at or above the signal at 1x (+26 to +28 dB on the TS at 450 mV, 3 kHz), and still −17 dB at 4x. Aliasing,
  not the solver, sets the rate: the TS stage at 200 mV is at −31 dB inharmonic at 1x, −58 dB at 2x, −81 dB at 4x and
  −118 dB at 8x. Float32 against double, once converged, is −117 to −146 dB. The generated-style FMinus compiles with
  today's FMinus and runs a circuit step in about 64–77 ns on a heavily loaded M5, using a driver build from before the
  inline math kernels.
- **FMinus:** a first slice needs no change to FMinus. The smallest additions that would help are `#line`, `fmaf`,
  file-scope `const` arrays, and later an `omegaf` kernel. Do not add `double` or matrix types: the precomputation
  belongs in the compiler's double-precision C++.
- **Plan:** a first useful slice of about 10 agent-days (diodes, ideal op-amps, pots, oversampling, both examples, the
  playground integration), then BJTs and op-amp macromodels (about 9), then tubes, tables and the FMinus additions
  (about 10). Section 10 gives the phases.

## 1. What the compiler has to fit (from the code)

**FMinus** ([README](../../../../README.md), [`docs/extending.md`](../../../extending.md),
[`docs/simd.md`](../../../simd.md), [`src/ir/Calls.cpp`](../../../../src/ir/Calls.cpp)) [H]:

- Types are `float` (IEEE single), `int` and `bool`, plus `float2/4/8` and `poly float`. There is no `double`, no 2-D
  array, no global or file-scope constant, no `switch`, no bitwise operator and no function pointer. Module fields
  take no initialiser.
- Builtins (`kBuiltins`): `fabsf sqrtf floorf fminf fmaxf` (exact) and `sinf cosf tanf expf logf tanhf`. There is no
  `fmaf`, `exp2f`, `log1pf`, `powf`, `atanf` or `sinhf`; a nonblocking libm prototype reaches the others at a call's
  cost. Arithmetic runs with `-ffp-contract=off` and no fast-math flags, so `x * 0.0f` is never folded away and
  `x / 0.0453f` stays a divide.
- The math kernels ([recipes 23](../../../extending.md#recipe-23-math-kernels-of-fminuss-own) and
  [24](../../../extending.md#recipe-24-the-math-kernels-inline)) give one result for a float wherever it is computed,
  inline from `-O1`, with two-lane variants that SLP pairs at a cost threshold of 10. Errors: exp 1.05 ulp, log 0.99,
  tanh 1.52. Measured as a dependent chain, which is what a circuit's recursion is: `expf` 41.7 cycles and `logf` 52.2
  inline, against 52.6 and 73.7 out of line. In a vectorised loop: 1.55 and 2.41 cycles a sample. A constant's kernel
  folds at compile time.
- Parameters are bound as [knobs, streams or frozen
  constants](../../../extending.md#recipe-14-parameters-the-hosts-values). **[Knob
  versioning](../../../extending.md#recipe-16-knob-versioning-and-flat-streams)** gives a method a copy of its loop for
  each value of a `bool` or small-range `int` (at most 8 copies), and for a stream a *flat* copy in which `x[i]` is one
  value and LICM hoists everything computed from it ([recipe 16](../../../extending.md#flat-streams)). Only a method
  with a loop of its own is versioned.
- Effects: a `[[clang::nonblocking]]` entry point that could reach the heap is refused ([recipe
  11](../../../extending.md#recipe-11-the-heap-kept-off-the-audio-thread)). Subnormals are flushed when the host asks
  ([recipe 20](../../../extending.md#recipe-20-subnormals-flushed-for-the-audio-thread)). `parallel { }` costs about 4
  µs a region on an M5: worth it only for tens of µs of work.
- `#include "file"` is the only directive. Hex floats are allowed. Errors read `file:line:col: error: ...`.
- Compiling takes milliseconds to a few hundred: the playground's modular voice took 120 ms, and 154 ms once the math
  kernels went inline. Every knob-versioned copy and every vectorised loop part carries its own kernels, so a generated
  module full of `expf`/`logf` should keep its loops few ([recipe
  24](../../../extending.md#recipe-24-the-math-kernels-inline)).

**The playground** (README, `CLAUDE.md`, `Source/fminus/Patch.h`, `Patch.cpp`, `Source/dsp/Engine.h`, `Measure.cpp`)
[H]:

- The effect plays `Effect::process(left, right, n)` in place; the synth plays one `Voice` instance per note, 16 at
  once, called in parts cut on the absolute 16-sample grid. Voices are summed in float, in a fixed slot order.
- The dials are the main module's first 32 parameters. A float dial is a stream of 20 ms glides, null (flat) while it
  holds still; an int or bool is a knob. **The parameters of held modules stay at their defaults**, so a circuit
  module must take its pot positions as arguments.
- **Frozen dials:** after 3 s still, the program is recompiled with the dials as constants, in `Math::Fast`, and must
  keep the main module's layout (`Patch::freeze` returns null otherwise).
- Output must not depend on block splits: the examples are checked at 64 and 333 samples, and `trace` re-renders in
  1-sample calls and compares. Every change ramps. The output guard keeps samples finite and within ±4.
- Includes resolve through a lambda over the program's files and `host.fm` (`Patch.cpp` around line 311). `.gui` files
  are already a non-FMinus file kind that `Patch::compile` reads.
- Nothing oversamples (no halfband in `Source/fminus/library`), and the plugin reports no latency to the host.
- The tools: `measure_output` (effect metrics include `thd_db` of one fixed 220 Hz, −12 dBFS sine), `view_sound`,
  `set_checks`, `tune_dials`, `compare`, `trace`, `capture`, `listen`. The same tools reach Claude Desktop through MCP.

**FSynth's own data** (`docs/research/B-filters.md` §2.2) [H]: on a tanh ladder, two Newton steps from the previous
sample stay within −50 dB at 48 kHz and −85 dB at 96 kHz. One step reached +24 dB at 12 kHz. A tanh saturates; a
diode's exponential does not, and that is where the previous-sample guess breaks down (section 6.1).

## 2. The methods, in one page

**Nonlinear state space (the DK family)** [H, Yeh–Abel–Smith 2010; Holters–Zölzer 2011, 2015]. Discretise each
capacitor and inductor with a companion circuit (trapezoidal: a conductance `G_C = 2C/T` and a history current). Treat
each nonlinear device as a port. Then nodal or tableau analysis, solved once, gives

```
x[n] = A x[n-1] + B u[n] + C i[n]          states: the companions' histories (charges, in Holters' canonical form)
y[n] = D x[n-1] + E u[n] + F i[n]          outputs
v[n] = G x[n-1] + H u[n] + K i[n]          the ports' controlling voltages
i[n] = f(v[n])                             the devices' laws (Shockley, Ebers–Moll, Koren, ...)
```

Each sample: `p = G x + H u`, then solve `p + K f(v) − v = 0` for the `m` port voltages (`m` is the number of nonlinear
ports, whatever the size of the linear network), then update the state and the output. Yeh found that iterating on the
voltages converges faster than iterating on the currents. Holters–Zölzer 2015 replace nodal analysis with a
sparse-tableau-like analysis in which each element is a row of a table (`M_v, M_i, M_x, M_ẋ, M_q, u, f`). That handles
what the nodal DK method cannot: a node joined only to nonlinear elements, an ideal or soft-clipping op-amp
(`V_max·tanh(A q₁/V_max) − q₂`), and transformers. It also makes a new device a new row rather than a new method.
ACME.jl implements this formulation.

**Pots** [H, Holters–Zölzer DAFx-11]. Write the system matrix as `S = S₀ + N_vᵀ R_v⁻¹ N_v`. By Woodbury,
`S⁻¹ = S₀⁻¹ − S₀⁻¹ N_vᵀ (R_v + Q)⁻¹ N_v S₀⁻¹` with `Q = N_v S₀⁻¹ N_vᵀ`. Every coefficient matrix then becomes
`X = X₀ − U_a (R_v + Q)⁻¹ U_bᵀ`, with all `U` precomputed. For the Crybaby, a 15×15 inverse becomes a 2×2 one, and `R_v`
is never inverted, so a pot at either end (0 Ω) is safe. They solved with damped Newton from the previous sample, and it
usually took fewer than ten iterations.

**Solving the ports** [H]:

- *Tables* (the K-method; Yeh 2010). Precompute `v = Γ(p)` offline (homotopy makes that robust) and interpolate at
  run time. The table has `dim p` dimensions, so it suits 1–2-D ports.
- *Caches* (Holters–Zölzer EUSIPCO-16). Store solutions in a k-d tree, extrapolate from the nearest by the Jacobian
  (`z⁽⁰⁾ = z* − (J F_q)⁻¹ J Q (p − p*)`), and polish with Newton. On a Boss SD-1 clone (4 diodes, 2 BJTs, 11 states,
  parameter dimension 7 after QR reduction), 27,039 cached solutions brought the average to 2.31 iterations. The same
  extrapolation from the previous sample alone gave 2.59. Still, 2,172 samples (0.14 %) needed more than 5 iterations
  and the worst needed 500.
- *Decomposition* (ACME's `nldecompose!`, read in source). Make `F_q` block-triangular, solve each block in turn, and
  solve once at construction any block that no state touches (the supply's reverse-polarity diode, for instance).
- *Closed forms* (D'Angelo–Gabrielli–Turchet DAFx-19). When one exponential dominates, `e^{ax+b} = cx + d` is solved
  by `ω(x) = W₀(eˣ)`, with no audio-rate exponential in the argument. Their cheap approximation `ω₃` (a cubic between
  −3.34 and 8, then `x − log x`) and `ω₄` (one Newton step) solve the RC diode clipper in 5 adds, 9 multiplies, a sign
  and one ω a sample.

**Wave digital filters** [H, Chowdhury 2022; M, Werner thesis 2016 and Bernardini et al. 2021 as cited]. A WDF is
modular and explicit for a single nonlinearity at the root. Non-series-parallel topologies need R-type adaptors
(scattering matrices derived from MNA), and several nonlinearities need an iterative or multiport solve. At that
point it costs what DK costs. chowdsp_wdf with its templated API processed 1000 s at 48 kHz in 2.04 s for the
diode clipper (42 ns a sample, Wright-ω diode) and 0.71 s for the Bassman tone stack (R-type), on a 2018 Intel i7.
WD Newton's convergence basin depends on the port resistances (Bernardini et al. 2021).

**Aliasing** [H, Yeh–Abel–Smith DAFx-07]. They write that "typical digital implementations of distortion upsample by a
factor of eight or ten". The same paper cuts a circuit into stages wherever an active element with a low source
impedance drives a high-impedance load. Antiderivative antialiasing has been extended to stateful systems, but to
one-port (Holters 2020) or two-port (Carson) nonlinearities [M]. It adds half a sample of delay inside loops (FSynth B
§2.3) and can change the frequency response (La Pastina–D'Angelo 2022) [M]. It is a later option for single-port blocks
at 1x, not a default.

**Choice:** the generalized state-space formulation, because it handles op-amps and arbitrary devices, keeps physical
states (which pot moves and switch ramps rely on), reduces the nonlinear work to `m` ports, and produces straight-line
code that FMinus optimises well. A WDF back end would add nothing the DK path cannot do for these circuits [L].

## 3. (a) Where the compiler lives

| | A1: an FMinus language feature (`circuit` block) | A2: netlist → FMinus source (recommended) | A3: a library that emits LLVM IR |
|---|---|---|---|
| Change to FMinus | grammar (a sub-lexer for netlist lines), AST, lowering, Diagnostics | none for v1; small additions later (§7) | a new API to link foreign IR, its effects and TBAA |
| What the user sees | only IR (`--emit ll`) | a readable `.fm` module, which can be ejected into a tab and hand-tuned | nothing |
| Playground tools | `fieldMap`/`trace` only if lowered as fields | all work unchanged: the states are module fields named after the parts | `trace`, frozen dials, knob versioning and nonblocking checks all bypassed |
| Frozen dials | yes | yes (the frozen recompile regenerates the same text) | needs its own path |
| Errors | FMinus's, exact | the compiler's, with `.cir` lines; FMinus's on generated lines until `#line` exists | its own |
| Editor (`Lang.h`, formatter) | must learn a non-C island | `.cir` highlighting only | none |
| Effort to the first slice | ~15–20 agent-days [L] | ~10 [M] | ~15 + an FMinus API [L] |

A2 keeps FMinus a C subset and keeps its invariants (exhaustive walks, a syntax tree free of LLVM). It also keeps
the playground's single path from source text to a `Patch`. A1 can come later as sugar: a `#circuit "x.cir"` or a
`circuit` block would call the same library and parse what it returns. A3 would put a second compiler beside FMinus;
the playground lints already forbid LLVM headers in `Source/` (`lint.layers`).

**Where A2's code lives:** in the FMinus repository, as `include/fminus/Circuit.h` and `src/circuit/` (C++23 inside,
a C++17 header, no LLVM), so the `fminus` driver, `examples/plugin` and the playground all reach it through the pin.

```cpp
namespace fminus::circuit
{
    struct Options { double sampleRate = 0; std::string moduleName; bool reference = false; };   // reference: §9
    struct Result
    {
        std::string source;     // the FMinus module (empty on error)
        std::string errors;     // "pedal.cir:7:1: error: ..." (clang's format, like FMinus's)
        std::string notes;      // the block plan, the DC operating point, the self-test's predicted error and cost
    };
    Result generate(std::string_view netlist, std::string_view fileName, const Options& options);   // pure, deterministic
}
```

- Driver: `fminus pedal.cir --emit fm`, and `fminus prog.fm` resolves `#include "pedal.cir"` the same way.
- Playground: `Patch::compile` (and `check`) run `generate` on each `.cir` file of `Sources` first. Its errors go
  into `Result::errors` and stop the compile; its notes go into `notes`. The include resolver returns the generated
  text for a `.cir` name. `Recipe` already keeps the files, so the frozen copy regenerates identical text.
  `lint.layers` needs one line allowing `fminus/Circuit.h`, and `Lang.h` needs the `.cir` tokens and the generated
  module's symbols for completion and hover.

## 4. (b) The syntax

A SPICE subset (element lines, `.model`, `{expressions}`, SPICE's units), plus directives for what an audio module
needs. SPICE syntax means existing netlists, LTspice exports and Claude's knowledge of SPICE all carry over.

| Line | Meaning |
|---|---|
| `R1 a b 2.2k`, `C1 a 0 10n`, `L1 a b 500m` | passive parts. Units are SPICE's: `f p n u m k meg g`. **`1M` is 1 milli-ohm**: the compiler warns on a resistor under 1 Ω written with `M` and suggests `meg` |
| `D1 anode cathode 1N914` | a diode, with a `.model NAME D(IS= N= RS=)` card. Antiparallel pairs merge into one port |
| `Q1 c b e 2N3904`, `J1 d g s 2N5457` | BJT (Ebers–Moll), JFET (Shichman–Hodges): phase 2 |
| `XU1 inp inn out opamp` | an ideal op-amp (a nullor). `opamp(vmax=4 gain=1e5)` is Holters' soft-clipping model; `gbw=` adds a pole (phase 2) |
| `Pname e1 w e2 100k {x} [taper=lin\|audio\|rlog]` | a 3-terminal pot: `R(e1–w) = (1 − t(x)) R`, `R(w–e2) = t(x) R`, with `x` in 0..1 |
| `Pname a b 500k {x} [taper=…]` | a 2-terminal rheostat, `R = t(x) R` |
| `R6 a b {51k + 500k*drive^2}` | a resistor that is any expression of pots |
| `Sname a b {pos == 1}` | a switch, compiled as a resistor that ramps over 20 ms between 10 MΩ and 1 Ω, so it never steps |
| `.input NAME node [scale=1]` | an input: a voltage source from `node` to ground; sample 1.0 is `scale` volts. Order = `tick`'s arguments |
| `.output NAME node [scale=1]` | an output voltage. The first is `tick`'s result; the others are fields |
| `.pot NAME [default] [taper]` | a pot position. Order = the arguments of `set` |
| `.oversample 1\|2\|4\|8\|auto [fir\|iir]` | the rate of the nonlinear blocks (`auto` picks the least factor that meets `.target`) |
| `.target alias=-60dB level=1V freq=3k` | what `auto` and the self-test aim for |
| `.method tr\|bdf2\|be` | the integrator (default `tr`) |
| `.solver auto\|omega\|newton(K)\|table` | the per-port solver (default `auto`, chosen by the self-test) |
| `.channels 1\|2` | 2 emits `float2 tick2(float2)`: both channels in one recursion, on the two-lane kernels |
| `.op` | print the DC operating point (node voltages, device currents) as notes |

**The generated module's interface:**

- `init(rate)` sets the rate-dependent constants and the states at the DC operating point.
- `set(pot1, pot2, …)` runs at control rate: Woodbury factors, constants of the closed forms.
- `tick(in…)` advances one host-rate sample (oversampling inside) and returns the first output; `step(…)` is one
  oversampled step.
- `clear()` returns to the DC operating point.
- Fields are named after the parts (`x_C1`, `v_D1`), so `trace` speaks the schematic's language.

**Binding pots to dials:** the main module's `param float`s are the dials, and the program calls
`circuit.set(drive[i], tone[i])` on the absolute 16-sample grid. When a dial is still, the stream is flat and the
call is near free. When it glides, the coefficients step every 16 samples along a 20 ms ramp. Frozen, they fold. An
`.effect` directive that emits the whole `Effect` with its params is an easy later convenience, but writing the
wrapper keeps the routing visible, which is how the playground already works.

### Example 1: a diode clipper with a tone pot

```spice
* diodetone.cir: a passive diode clipper, then a tone pot into a low-pass
.title DiodeTone
.input  u    in   scale=1          ; 1.0 = 1 V into R1
.output vout b
.pot    tone 0.5 lin               ; 1: bright (0 ohm), 0: dark (25k)
.oversample 4
R1     in a  2.2k
C1     a  0  10n
D1     a  0  1N914
D2     0  a  1N914
R2     a  c  470
Ptone  c  b  25k {1 - tone}        ; a rheostat
C2     b  0  22n
.model 1N914 D(IS=2.52n N=1.752)
.end
```

The compiler's plan (as notes): one block (the diode pair's node `a` couples to `b` through the pot), 2 states,
1 port (D1|D2 merged: `i = 2 I_s sinh(v/(N V_t))`), pot rank 1, 4x. The DK reduction is small enough to show. The
nodes are `a` and `b`; `s₁₁ = G₁ + G_{C1} + G_s`, `s₂₂ = G_s + G_{C2}`, `Δ = s₁₁ s₂₂ − G_s²`:

```
p   = (s₂₂ (G₁ u + x₁) + G_s x₂) / Δ          v_a = p − K i,   K = s₂₂ / Δ,   i = 2 I_s sinh(v_a / N V_t)
v_b = (G_s (G₁ u + x₁ − i) + s₁₁ x₂) / Δ       x₁ ← 2 G_{C1} v_a − x₁,   x₂ ← 2 G_{C2} v_b − x₂
```

The generated module follows, with a stand-in for the library's oversampler. It compiles with today's FMinus
([`ex1c.fm`](../experiments/ex1c.fm) is this text exactly, checked with the 2026-10-23 driver), passes a DC sanity check
(0.1 V in → 0.09995 V; 1 V → 0.512 V; 5 V → 0.612 V), and its step takes about 64 ns under load (Appendix A). The
self-test figures in its header are Tables 1 and 3's for the clipper alone, at 4x.

```c
// diodetone.cir, compiled by fminus-circuit. Do not edit: edit diodetone.cir.
//   method: DK, trapezoidal, 1 block, 4x; port D1|D2 by omega3 + 1 Newton step; states x_C1, x_C2
//   self-test (float against double, 64x reference): solver -55 dB in band at 4.5 V, 3 kHz; alias -75 dB (4x)
//   cost a host-rate sample: 4 x (1 logf when a diode conducts, 1 expf, 2 divides, ~31 flops) + Oversample4 (68 mult.)
module DiodeTone
{
    float x_C1, x_C2;                 // the capacitors' companion histories
    float g1, gc1, gc2;               // 1/R1, 2 C1 fs, 2 C2 fs
    float cin, cx1, cx2, kk;          // p = cin u + cx1 x_C1 + cx2 x_C2;  v = p - kk i
    float gk, k2, kvt, lna;           // 1 / kk, 2 Is kk, 2 Is kk / (N Vt), log(Is kk / (N Vt))
    float din, dx1, dx2, di;          // v_b = din u + dx1 x_C1 + dx2 x_C2 - di i
    Oversample4 os;                   // library: two halfband stages each way
    float u4[4];
    float y4[4];

    void init(float rate)             // rate: the host's; the circuit runs at 4 rate
    {
        g1 = 1.0f / 2200.0f;
        gc1 = 2.0f * 10e-9f * (4 * rate);
        gc2 = 2.0f * 22e-9f * (4 * rate);
        clear();
        set(0.5f);
    }

    void clear()                      // the DC operating point: 0 V everywhere (no bias in this circuit)
    {
        x_C1 = 0;
        x_C2 = 0;
        os.clear();
    }

    void set(float tone)              // .pot tone: 0..1, lin; Ptone = 25k (1 - tone)
    {
        const float t = fminf(fmaxf(tone, 0), 1);
        const float gs = 1.0f / (470.0f + 25000.0f * (1 - t));
        const float s11 = g1 + gc1 + gs;
        const float s22 = gs + gc2;
        const float inv = 1.0f / (s11 * s22 - gs * gs);
        cin = s22 * g1 * inv;
        cx1 = s22 * inv;
        cx2 = gs * inv;
        kk = s22 * inv;
        gk = (s11 * s22 - gs * gs) / s22;
        din = gs * g1 * inv;
        dx1 = gs * inv;
        dx2 = s11 * inv;
        di = gs * inv;
        k2 = 5.04e-9f * kk;           // .model 1N914 D(IS=2.52n N=1.752): N Vt = 45.3 mV at 27 C
        kvt = k2 * 22.0750552f;       // 1 / (N Vt) as a product: a constant's divide stays a divide in FMinus
        lna = logf(0.5f * kvt);
    }

    float step(float u)               // one sample at 4 rate
    {
        const float p = cin * u + cx1 * x_C1 + cx2 * x_C2;
        const float q = fabsf(p);
        const float a = 0.5f * k2;
        const float arg = (q + a) * 22.0750552f + lna;
        const float w = arg <= -3.34145955f ? 0
                      : arg < 8 ? ((-1.31429315e-3f * arg + 4.77593136e-2f) * arg + 3.63195266e-1f) * arg + 6.31318346e-1f
                                : arg - logf(fmaxf(arg, 8));
        float v = q + a - 0.0453f * w;                         // omega3: the conducting diode alone
        v = p < 0 ? -v : v;
        const float e = expf(v * 22.0750552f);
        const float ei = 1 / e;
        const float f = v - p + k2 * 0.5f * (e - ei);
        const float d = 1 + kvt * 0.5f * (e + ei);
        v = fminf(fmaxf(v - f / d, fminf(p, 0)), fmaxf(p, 0));  // one Newton step on both diodes, kept in bracket
        const float i = (p - v) * gk;                          // the port's current, from the linear network
        const float b = din * u + dx1 * x_C1 + dx2 * x_C2 - di * i;
        x_C1 = 2 * gc1 * v - x_C1;
        x_C2 = 2 * gc2 * b - x_C2;
        return b;
    }

    float tick(float u)               // one sample at the host's rate
    {
        os.up(u, u4);
        for (int k = 0; k < 4; ++k)
            y4[k] = step(u4[k]);
        return os.down(y4);
    }
};
```

The program around it, which Claude or the user writes. It compiles too:

```c
#include "diodetone.cir"

module Effect
{
    param float tone [[fminus::range(0, 1)]] [[fminus::default(0.5)]];
    param float input [[fminus::range(-24, 24)]] [[fminus::default(0)]];   // dB; 0 dB: full scale is 1 V
    param float level [[fminus::range(-36, 6)]] [[fminus::default(-6)]];   // dB
    DiodeTone left_, right_;
    int grid;                                                              // the absolute 16-sample grid

    void init(float rate)
    {
        left_.init(rate);
        right_.init(rate);
        grid = 0;
    }

    void process(float* restrict left, float* restrict right, int n)
    {
        for (int i = 0; i < n; ++i)
        {
            if (grid == 0)
            {
                left_.set(tone[i]);
                right_.set(tone[i]);
            }
            grid = grid == 15 ? 0 : grid + 1;
            const float gin = expf(0.115129255f * input[i]);
            const float gout = expf(0.115129255f * level[i]);
            left[i] = gout * left_.tick(gin * left[i]);
            right[i] = gout * right_.tick(gin * right[i]);
        }
    }
};
```

Notes on what the code shows:

- The bracket clamp `[min(0,p), max(0,p)]` holds because `f(v) = v − p + k sinh(v/V_t)` is monotone. It keeps every
  iterate finite whatever the input.
- `i = (p − v)·G_K` takes the port current from the linear network instead of a second exponential, so KCL holds
  exactly even when the solve is approximate.
- `gk` and the reciprocal constants are there because FMinus, without fast-math, keeps `x / c` as a divide (§6.2, item
  5).
- `.channels 2` would emit the same code on `float2`. Both channels then share one chain on the two-lane kernels, with
  `?:` selecting per lane.

### Example 2: the Tube Screamer's clipping and tone stages

Values from Yeh–Abel–Smith DAFx-07, figs. 14 and 15 and eq. 24 (clipping stage [H]; the tone stage's wiring is
reconstructed from fig. 15 and eq. 24 [M]). The 4.5 V reference is AC ground, valid while the op-amps stay inside
their rails. The input and output emitter followers are left out.

```spice
* screamer.cir: TS808 clipping stage, tone control and level
.title Screamer
.input  u    in   scale=1
.output vout out
.pot    drive 0.5 audio
.pot    tone  0.5 lin
.pot    level 0.7 audio
.oversample auto                    ; the self-test picks 4x for alias <= -50 dB at 0.45 V, 3 kHz
.target alias=-50dB level=0.45V freq=3k
* clipping stage (fig. 14)
C1     in  p1  1u
R1     p1  0   10k
XU1    p1  n1  v2  opamp            ; ideal: V(n1) = V(p1), no input current
R4     n1  z1  4.7k
C3     z1  0   47n
R6     n1  f1  51k
Pdrive f1  v2  500k {drive} taper=audio
C4     n1  v2  51p
D1     n1  v2  1N914
D2     v2  n1  1N914
* tone control (fig. 15, eq. 24): an active treble control
R7     v2  t1  1k
C5     t1  0   0.22u
R8     t1  0   10k
XU2    t1  tn  v3  opamp
R9     v3  tn  1k
Ptone  t1  tw  tn  20k {tone}
R10    tw  tz  220
C6     tz  0   0.22u
* level
C7     v3  o1  1u
R11    o1  o2  1k
Plevel o2  out 0   100k {level} taper=audio
.model 1N914 D(IS=2.52n N=1.752)
.end
```

The compiler's plan (notes):

```
screamer.cir: 4 blocks, 6 states, 1 nonlinear port, 3 pots
  block 1  in -> p1   linear, 1 state (C1), 1x                cut: U1's + input draws no current (exact)
  block 2  p1 -> v2   nonlinear, 2 states (C3, C4), port D1|D2 (V = v2 - n1), pot drive (rank 1), 4x
  block 3  v2 -> v3   linear, 2 states (C5, C6), pot tone (rank 2), 1x     cut: U1's output is ideal (exact)
  block 4  v3 -> out  linear, 1 state (C7), pot level (wiper unloaded: a divider), 1x   cut: U2's output is ideal
  solver   D1|D2: omega3 + 1 Newton step (self-test in band: -71 dB at 0.45 V, 3 kHz; -60 dB at 0.2 V, 1 kHz)
  alias    -53 dB at 0.45 V, 3 kHz; -81 dB at 0.2 V, 1 kHz (4x; 1x would be -23 and -31)
  cost     host-rate sample: 4 x (1 logf, 1 expf, 2 divides, ~30 flops) + 1x blocks ~26 flops + Oversample4 68 mult.
```

Block 2 as emitted (it compiles: [`ts2.fm`](../experiments/ts2.fm) is this text with the oversampler stub). Its scalar
equation has the same form as example 1: `V = p − k sinh(V/V_t)`, with `V_out = V_in + V` (Yeh eqs. 20–23).

```c
module ScreamerClip                   // block 2; Screamer holds it with blocks 1, 3, 4 and an Oversample4
{
    float x_C3, x_C4;
    float g4, gc3, gc4, inA;          // 1/R4, 2 C3 fs, 2 C4 fs, 1 / (g4 + gc3)
    float invB, k2, kvt, lna;         // drive-dependent: 1 / (gf + gc4), 2 Is K, 2 Is K / (N Vt), log(Is K / (N Vt))

    void init(float rate)             // rate: the oversampled rate
    {
        g4 = 1.0f / 4700.0f;
        gc3 = 2.0f * 47e-9f * rate;
        gc4 = 2.0f * 51e-12f * rate;
        inA = 1.0f / (g4 + gc3);
        x_C3 = 0;
        x_C4 = 0;
        set(0.5f);
    }

    void set(float drive)             // the audio taper as the compiler emits it: (81^x - 1) / 80
    {
        const float d = fminf(fmaxf(drive, 0), 1);
        const float gf = 1.0f / (51000.0f + 500000.0f * (expf(4.39444915f * d) - 1) * 0.0125f);
        invB = 1.0f / (gf + gc4);
        k2 = 5.04e-9f * invB;
        kvt = k2 * 22.0750552f;
        lna = logf(0.5f * kvt);
    }

    float step(float vin)
    {
        const float va = (vin * g4 + x_C3) * inA;                   // the R4-C3 node
        const float p = ((vin - va) * g4 + x_C4) * invB;            // the port's open-circuit voltage
        const float q = fabsf(p);
        const float a = 0.5f * k2;
        const float arg = (q + a) * 22.0750552f + lna;
        const float w = arg <= -3.34145955f ? 0
                      : arg < 8 ? ((-1.31429315e-3f * arg + 4.77593136e-2f) * arg + 3.63195266e-1f) * arg + 6.31318346e-1f
                                : arg - logf(fmaxf(arg, 8));
        float v = q + a - 0.0453f * w;
        v = p < 0 ? -v : v;
        const float e = expf(v * 22.0750552f);
        const float ei = 1 / e;
        const float f = v - p + k2 * 0.5f * (e - ei);
        const float dd = 1 + kvt * 0.5f * (e + ei);
        v = fminf(fmaxf(v - f / dd, fminf(p, 0)), fmaxf(p, 0));
        x_C3 = 2 * gc3 * va - x_C3;
        x_C4 = 2 * gc4 * v - x_C4;
        return vin + v;                                             // Vout = Vin + V
    }
};
```

Blocks 1, 3 and 4 are linear and emitted the same way, with `p` absent. Block 3 has 2 states and a rank-2 pot: its
`set(tone)` inverts a 2×2 `(R_v + Q)` (one divide) and updates about 12 coefficients. The `Screamer` module holds the
four blocks. Its `tick(u)` runs block 1, upsamples, runs four `ScreamerClip.step`s, decimates, then runs blocks 3
and 4. Its `set(drive, tone, level)` forwards each pot to its block. The program is example 1's with
`Screamer pedal;` and `pedal.set(drive[i], tone[i], level[i])` on the grid.

## 5. (c) The pipeline

| Stage | What | Runs |
|---|---|---|
| 1. Parse | element lines, `.model`, `.pot`, `{expr}` (a small expression grammar over pots), units; positions kept for every token | compile time (C++) |
| 2. Elaborate | subcircuits, models with defaults (unknown SPICE parameters such as `CJO` and `TT` are ignored with a note), pots and switches into variable resistors, antiparallel diodes merged | compile time |
| 3. Topology checks | floating nodes, loops of voltage sources and inductors, cut-sets of capacitors and current sources (DC undefined), op-amps without negative feedback, unknown models, pot names, pin counts | compile time |
| 4. DC operating point | Newton with source stepping (homotopy, as Yeh and ACME use it), in double, at the default pots; `.op` notes | compile time |
| 5. Partition | cut at ideal op-amp outputs and inputs and controlled sources (exact); a buffer cut only on request, with its error measured on the small-signal response; strongly connected components of the "drives" graph, in topological order | compile time |
| 6. Per block | Holters–Zölzer element tables → tableau → discretise (TR/BDF2/BE) → general solution → `A…K`; Woodbury factors per pot; `F_q` decomposition; constant subproblems folded; QR reduction of the port parameters | compile time, double |
| 7. Method check | eigenvalues of the linearised `A` at the operating point and with each port at its largest conductance. An eigenvalue near −1 can ring under the trapezoidal rule, so the self-test then compares the energy above 20 kHz with the 64x reference, and warns (suggesting BDF2 or more oversampling) only when it is in excess. An eigenvalue alone would be a false alarm: the TS stage has one near −0.997 while a diode conducts (my estimate, `−1 + 2G_C/g_d` at 1x) [M] and showed no excess (§6.1) | compile time |
| 8. Solver choice | per port: closed form, table or Newton(K), the least that meets `.target`. The compiler runs its own float emulation against a double reference (converged, 64x) on a sweep and sines at three levels | compile time, tens to hundreds of ms; cached by netlist hash |
| 9. Oversampling plan | the smallest set of blocks holding every nonlinear block and every linear block on a path between two of them; halfband design | compile time |
| 10. Emission | sparse straight-line FMinus: zeros elided, ones as adds, constants printed with 9 significant digits (round-trips a float), `// from line N` comments | compile time |
| 11. Init | rate-dependent conductances; for blocks too big for closed forms, an unrolled elimination with the pivot order fixed at compile time (LiveSPICE fixes its pivots too [L]); states at the DC point | `init`, off the audio thread |
| 12. Control rate | `(R_v + Q)⁻¹` (k×k, k = 1–2 per pot) and the affected coefficients, the closed forms' log constants | `set`, on the 16-sample grid; hoisted when flat; folded when frozen |
| 13. Per sample | `p = Gx + Hu` (sparse), the port solve, outputs, state update; the halfbands | `step`/`tick` |

**Messages** on broken variants of the examples (file:line:col, in clang's words, as FMinus's):

```
pedal.cir:12:1: error: node 'f1' has one connection (R6); a node needs two
pedal.cir:9:1: error: node 'z2' reaches ground only through capacitors (C3, C9): its DC voltage is undefined (add 10meg to ground)
pedal.cir:11:16: error: unknown model '1N4148X' (the models: 1N914, 1N4148, 1N34A, BAT41, LED-RED, ...)
pedal.cir:10:1: error: op amp XU1 has no negative feedback: an ideal op amp needs one (opamp(vmax=...) clips instead)
pedal.cir:14:12: warning: '1M' is 1 milliohm in SPICE; write 1meg for a megohm
pedal.cir:3:1: warning: at 1x block 2 rings at Nyquist (12 dB more above 20 kHz than the 64x reference): .oversample 4 or .method bdf2
pedal.cir: note: block 2 (D1, D2, C3, C4): omega3 + 1 Newton step, -71 dB in band; alias -53 dB at 4x; about 4 x 140 cycles a sample
pedal.cir: error: no DC operating point (Newton with source stepping did not converge): check Q1's pins (c b e)
```

**Per-sample cost of a block** (flops, before sparsity, for `N` states, `M` inputs, `m` ports, `P` outputs):
`m(N+M)` for `p`, `P(N+M+m)` for the outputs, `N(N+M+m)` for the state update, plus the solve. One diode-pair port:
one exponential, one logarithm when a diode conducts, two divides and about 25 flops. An `m`-port Newton step: `m`
to `2m` exponentials, an `m×m` Jacobian, and an unrolled LU of about `m³/3` flops with `m` divides. A pot of rank
`k`: one `k×k` inverse plus `k` flops per affected coefficient, every 16 samples.

## 6. Optimising the circuit

### 6.1 What the experiments show

Setup [H]: Yeh's RC diode clipper (2.2 kΩ, 10 nF, 1N914 pair with `I_s = 2.52 nA`, `N V_t = 45 mV`) and the TS
clipping stage at full drive (551 kΩ), both reduced to `v = p − k sinh(v/V_t)` with trapezoidal companions. Sines are
bin-aligned. "In band" means FFT bins under 20 kHz in steady state, in dB relative to the reference's in-band energy.
The circuit is driven at the oversampled rate and decimated with a long windowed-sinc FIR in double, so the filter
does not limit the measurement. The code is in Appendix A.

**Table 1: the solver.** Float, trapezoidal; in-band error against a converged double solve of the same
discretisation and rate. `prev` starts from the last sample's solution. `extr` is that solution moved along the
linearised port (Holters EUSIPCO-16, eq. 13, with `p*` the last `p`). `ω` is the closed form above, K0 meaning the
closed form alone. `lim` adds SPICE-style junction limiting to `prev`. Kn = n Newton steps.

| circuit, input | rate | prev K1 / K2 | extr K1 / K2 | ω K0 / K1 / K2 | lim K1 / K2 |
|---|---|---|---|---|---|
| clipper 0.3 V, 1 kHz | 1x | −73 / −137 | −137 / −137 | −42 / −114 / −137 | −73 / −137 |
| clipper 2 V, 1 kHz | 1x | −31 / −39 | −38 / −51 | −34 / −55 / −95 | −26 / −36 |
| clipper 4.5 V, 3 kHz | 1x | −2 / −4 | −3 / −5 | −39 / −62 / −104 | −12 / −19 |
| clipper 4.5 V, 3 kHz | 4x | −33 / −42 | −39 / −49 | −35 / −55 / −95 | −30 / −41 |
| TS 30 mV, 1 kHz | 1x | −24 / −30 | −30 / −38 | −31 / −52 / −92 | −22 / −33 |
| TS 200 mV, 1 kHz | 1x | −4 / −7 | −5 / −9 | −41 / −63 / −104 | −23 / −32 |
| TS 450 mV, 3 kHz | 1x | **+27 / +26** | **+27 / +28** | −50 / −73 / −114 | −20 / −33 |
| TS 450 mV, 3 kHz | 4x | −14 / −17 | −14 / −18 | −46 / −71 / −114 | −30 / −43 |

The diode's exponential is the trouble. Started from the left of the root with a large residual, Newton overshoots
into the exponential, then comes down by only about `V_t` per step, so a fixed count stops far from the root. In
float the overshoot can also overflow `expf` (`v/V_t` above 88). The closed form starts on the right branch at every
level, and one step lands at least 52 dB under the signal. Its K0 error (−31 to −50 dB) is the one-diode approximation
corrected by the step. Junction limiting helps but does not reach −50 dB.

**Table 2: the integrator.** Converged solve; in-band error against a 64x trapezoidal reference (this includes
aliasing).

| circuit, input | method | 1x | 2x | 4x | 8x |
|---|---|---|---|---|---|
| clipper 2 V, 1 kHz | TR / BDF2 / BE | −50 / −43 / −35 | −62 / −53 / −41 | −74 / −64 / −47 | −86 / −75 / −53 |
| clipper 4.5 V, 3 kHz | TR / BDF2 / BE | −27 / −26 / −20 | −45 / −38 / −25 | −58 / −51 / −31 | −70 / −62 / −37 |
| TS 200 mV, 1 kHz | TR / BDF2 / BE | −31 / −32 / −32 | −54 / −45 / −48 | −67 / −58 / −54 | −79 / −70 / −59 |
| TS 450 mV, 3 kHz | TR / BDF2 / BE | −22 / −23 / −23 | −30 / −31 / −30 | −53 / −42 / −36 | −68 / −53 / −42 |

The trapezoidal rule is the most accurate from 2x up. At 1x on the TS stage the three methods are within 1.5 dB of
each other, because aliasing dominates there. The trapezoidal rule showed no extra energy above 20 kHz against the
reference in these circuits (for example, at 2 V, 1 kHz: −55 dB against the reference's −52 dB). BDF2 stays as the
compile-time fallback when the eigenvalue check flags ringing (LiveSPICE's author reports circuits that became
stable with it [L]). Backward Euler is too inaccurate to be a default.

**Table 3: aliasing.** Converged trapezoidal; energy under 20 kHz that is not on a harmonic, relative to all energy
under 20 kHz.

| circuit, input | 1x | 2x | 4x | 8x |
|---|---|---|---|---|
| clipper 2 V, 1 kHz | −62 | −105 | −178 | −188 |
| clipper 4.5 V, 3 kHz | −29 | −48 | −75 | −108 |
| TS 30 mV, 1 kHz | −54 | −93 | −157 | −181 |
| TS 200 mV, 1 kHz | −31 | −58 | −81 | −118 |
| TS 450 mV, 3 kHz | −23 | −31 | −53 | −73 |

**Precision:** the float32 solve against the double solve, where eight Newton steps converge, is −134 to −146 dB
(clipper, 4x), −117 to −132 dB (TS, 1x and 4x), and −137 dB at small signals at 1x. In the hardest TS case eight steps
from the previous sample do not converge; that is the guess, not the precision. Float32 is not what limits these
circuits; aliasing and the solver's guess are. Biased circuits (a fuzz with 9 V on its collectors and millivolt signals)
are untested. Section 6.2, item 8, says what to do there.

**FMinus timing:** a circuit step written as the compiler would emit it, run by `fminus --run` (the 2026-10-23 driver,
math out of line, load average 7–52). Example 1's step took 77 ns, and 64 ns after replacing constant divides with
products (under a different load, so this is not a controlled comparison). The TS stage's step took 70 ns. These are
rough. [Recipe 24](../../../extending.md#recipe-24-the-math-kernels-inline)'s chain measurements (exp 52.6→41.7 cycles,
log 73.7→52.2) suggest a quarter to a third less with the current build [M]. At 4x, both examples cost roughly 1–1.5 %
of one core per channel at 48 kHz, plus the halfbands. The playground's Drive example costs about 30 cycles a sample: a
circuit model costs a few tens of tanh drives.

**Table 4: halfband stages** (Kaiser-windowed, measured on a 4000-point grid).

| stage | taps | stopband | multiplies per host sample, each way | delay, host samples, each way |
|---|---|---|---|---|
| 48→96 kHz (20 → 28 kHz) | 63 / 79 / 95 | −80 / −95 / −109 dB | 16 / 20 / 24 | 15.5 / 19.5 / 23.5 |
| 96→192 kHz (20 → 76 kHz) | 23 / 27 / 31 | −83 / −94 / −103 dB | 12 / 14 / 16 | 2.8 / 3.2 / 3.8 |

A 4x oversampler at about −95 dB costs 68 multiplies a host sample in and out, and about 45 samples (0.95 ms) of
latency, which the playground does not report to the host today. Allpass polyphase IIR halfbands (as in HIIR) give a
few samples of delay for similar cost but bend the phase [M]. The compiler should offer both, with IIR as the default
for pedals, and the playground should report a program's latency (§7).

### 6.2 The optimisations, ranked by what they save

1. **Partition, and oversample only what is nonlinear.** Cutting at ideal op-amp outputs and inputs is exact. In
   example 2, only 2 of the 6 states run at 4x. One dense 6-state block at 4x would be about
   `4·(6·8 + 6 + 6)` = 240 flops a host sample before the solve; partitioned, it is about 4·10 + 26 [M].
   Approximate cuts (buffers, Yeh's low-to-high impedance rule) are taken only on request, with the error reported.
2. **Shrink the nonlinear problem.** Merge antiparallel diodes into one port (Yeh). Decompose `F_q` into
   block-triangular pieces solved in sequence (ACME's `nldecompose!`). Fold subproblems no state touches (ACME solves
   them at construction). Reduce the port parameters by QR (Holters EUSIPCO-16: the treble booster went from 3 to 2, and
   the SD-1, with 11 states, needed a 7-dimensional parameter space). A 1-port block is a scalar equation, which is
   where the cheap solvers live.
3. **Choose each port's solver at compile time.** Closed form plus 1–2 Newton steps for diode-like 1-ports (Table 1).
   Newton with a fixed count, a linearised guess and junction limiting for coupled ports. A table (`v = Γ(p)`,
   interpolated) where the port has 1–2 parameters and a table is cheaper than the closed form: about 15 cycles
   against about 140 [M]; this needs file-scope `const` arrays (§7). The count is the least that meets `.target` in
   the compiler's own test. Counts are fixed, so cost is predictable, `float2` lanes stay together, and the output is
   the same however blocks are split.
4. **Pots by Woodbury at control rate.** A rheostat is rank 1, a 3-terminal pot rank 2. Updates run on the absolute
   16-sample grid, which FSynth's control grid also uses. A still dial is a flat stream and its `set` costs nothing; a
   frozen one folds. Switches are ramped resistors (rank 1), so a topology change never steps, and it costs what a pot
   costs.
5. **Emit sparse straight-line code.** LLVM folds `x * 1.0f`, but not `x * 0.0f` or `x + 0.0f` without
   `nnan`/`nsz`, which FMinus never sets, so the compiler drops structural zeros itself. Divides by constants become
   products by reciprocals, because FMinus keeps `x / c` exact, so it stays a divide. Constants
   are printed with 9 significant digits (exact for a float). Nothing goes through loops over dense arrays, which
   LLVM would keep in memory.
6. **Pick the integrator per block.** Trapezoidal by default (Table 2). BDF2 when the eigenvalue check flags ringing.
   Backward Euler only for the DC solve.
7. **Size the oversampling from measurement.** `.oversample auto` runs Table 3's measurement at the declared level
   and frequency. Halfband stages as in Table 4; for 2x, one stage (40 multiplies, 0.8 ms FIR, or IIR).
8. **Look after precision cheaply.** All matrices are computed in double at compile time. Units are scaled (kΩ, nF,
   mA) for whatever runs in float at init. A biased circuit is simulated as offsets from its DC operating point, so
   float spends its digits on the signal, not the 4.5 V bias. Every new program starts at the DC point, so a crossfade
   never meets a power-on thump.
9. **Use the lanes there are.** `.channels 2` puts left and right in one `float2` recursion on the two-lane kernels. For
   a drive, [recipe 24](../../../extending.md#two-lanes) measured −28 % cycles from pairing. Voices stay scalar, since
   each is its own instance. `parallel { left; right; }` pays only for heavy blocks on large host blocks: a 4-port fuzz
   at 4x (about 0.9 µs a sample, §10 Phase 2) over 512 samples is about 460 µs a channel, against the region's 4 µs [M].
10. **What not to do.** Learned caches (k-d trees filled while playing, as in ACME's default solver): their cost
    varies (500 iterations in the worst case above) and their result depends on what was played before, so a fresh
    instance and an old one differ. Homotopy at run time: unbounded. Data-dependent iteration counts: lanes diverge
    and the cost depends on the input. Each of these is fine offline, in the compiler, which is where they belong.

## 7. (d) What FMinus gives for free, and what it lacks

**Free:**

- **Constant folding.** Every literal the compiler emits folds, including `logf` and `expf` of constants ([recipe
  24](../../../extending.md#recipe-24-the-math-kernels-inline)). Frozen pots fold `set` entirely.
- **Flat streams** ([recipe 16](../../../extending.md#flat-streams)). A still dial's `set` work leaves the loop, when it
  reads `pot[i]` in a method with a loop of its own (the `Effect`'s `process`).
- **Knob versioning.** An int switch on a stepping topology gets a copy per position, at most 8. Ramped switches (item
  4 above) do not need it.
- **The math kernels.** Exp and log are deterministic across block sizes, widths and machines, inline in the
  recursion, with two lanes for stereo.
- **Flush-to-zero.** Decaying capacitor states never go subnormal.
- **The nonblocking check.** Generated code is verified allocation-free, the same as hand-written code.
- **SLP.** It may pack independent state updates. Explicit `float2` emission is more reliable than relying on it.

**A risk:** the frozen copy compiles in
[`Math::Fast`](../../../extending.md#recipe-15-math-that-gives-a-float-one-result), which makes `expf`/`logf` calls into
the system's libm. In a recursion those chains were slower than the inline kernels (52.6 against 41.7 cycles for exp,
73.7 against 52.2 for log, recipe 24). The frozen copy of a circuit program could therefore be slower than its own code,
as the Filter example already is. Measure it, and compile frozen copies of programs that include a `.cir` in
`Math::Vector` if so [M].

**Lacking, with the smallest addition that helps:**

| Gap | Smallest addition | Why | Effort |
|---|---|---|---|
| Errors in generated code cite generated lines | `#line N "file"` (C's) in the lexer and `Diagnostics` | an error points into the `.cir` | 1 agent-day |
| No fused multiply-add | `fmaf(a, b, c)` → `llvm.fma`: explicit, exact, deterministic; contraction stays off | one instruction for each matrix-vector term and each Newton update, and a better-rounded residual | 0.5 |
| No constant data | file-scope `const float t[N] = {…};` → an internal constant global | halfband taps, ω or `Γ(p)` tables, per-rate matrices, without filling them per instance in `init` | 1.5–2 |
| No ω builtin | `omegaf(x)` as a math kernel ([recipe 23](../../../extending.md#recipe-23-math-kernels-of-fminuss-own)'s template, its three runtime names, its ulp table, two lanes) | the closed form for every diode or junction port; replaces `ω₃` + a `logf` | 2–3 (later; the emitted `ω₃` + Newton works today) |
| No latency in the contract (playground) | `int latency()` on `Effect`, read on the message thread into `setLatencySamples` | 0.95 ms of FIR oversampling goes unreported | 1 |

Not worth adding: `double` (the precomputation is the compiler's, in C++ double, and float measured fine), small
matrix types or a `solve` builtin (generated code is unrolled), `sinhf`/`coshf` (one `expf` and a reciprocal), and
bitwise operators (the fast-log tricks of D'Angelo et al. are beaten by the inline kernels).

## 8. (e) The house rules, kept

- **Nothing allocates on the audio thread.** States, coefficients and halfband lines are fixed fields. `init` and
  `set` do arithmetic only, and FMinus checks the effects as for any program.
- **Same output however blocks are split.** All state is per sample. Pot updates run on an absolute 16-sample grid
  (a field counter, never `i % 16` within a call). Iteration counts are fixed. Oversampling filters are per-sample
  recurrences. `fminus.examples` (64 and 333) and `trace`'s 1-sample pass test exactly this. Proposed probe:
  `fminus.circuit` runs the `.cir` examples at 1, 64 and 333.
- **Every change ramps.** Dials glide 20 ms and pots follow them on the grid. Switches are resistors ramped over
  20 ms (rank-1 updates), not topology steps. The states are physical (companion histories that depend on `C` and
  `T`, not on any pot), so a coefficient change acts as a moving pot acts. Programs crossfade in the Engine and start
  at the DC point.
- **Voices summed exactly.** That is FSynth's rule (int64); the playground sums voices in float in a fixed slot order.
  A circuit voice only has to be deterministic per instance, which it is. In a `Voice`, `start` resets to the DC
  point only when the voice was silent, as the playground's practice says for any state.
- **Finite output.** The 1-port bracket keeps iterates finite. Multi-port blocks clamp junction voltages to the rails
  and, if a state goes non-finite, return to the DC point at the next grid boundary. The Engine's guard stays as the
  last line.
- **Frozen dials.** The generated layout depends on the netlist only, never on pot values, so `Patch::freeze` finds
  the same layout.

## 9. (f) Claude, the chat and Claude Desktop

**Writing a circuit:**

- Claude writes `pedal.cir` with `write_file`, as it does `panel.gui`, then the `Effect` around it, then
  `run_program`. The compiler's errors come back with `.cir` lines, and the `.op` notes give node voltages to check
  against a schematic's voltage chart.
- A new guide part, *circuits* (the syntax, the model library, the two examples), joins the prompt's parts, and
  Claude Desktop gets the same part through MCP. The model library is data: `.model` cards for 1N914, 1N4148, 1N34A,
  BAT41, LEDs, and later 2N3904/2N5088/BC109/AC128, TL072/JRC4558 macromodels and 12AX7.
- Users can attach a schematic picture (the chat already takes six a message), and Claude transcribes it. The
  topology checks and `.op` catch most misreadings: a reversed transistor finds no sane operating point; a missing
  ground fails the checks.

**Testing it with the playground's tools:**

- `measure_output` and `set_checks`: `thd_db`, `sine_gain_db`, `centroid_out_hz`, `cpu_percent`.
  - Add a scenario input `sine: {hz, db}`, so THD can be measured against level and frequency. A clipper's character
    is its THD curve, and today's sine is fixed at 220 Hz, −12 dBFS.
  - Add a metric `alias_db`, the inharmonic energy under 20 kHz of a bin-aligned sine (Table 3's measure).
- `compare` with a reference build. Proposed `.options quality=reference` (or the API's `reference = true`) compiles
  the same netlist at 16x, with Newton to 8 steps, as the reference against which the shipping build is measured.
  `compare {a: "editor", b: "editor", try_edits on b}` then gives the null depth, which is exactly Tables 1–3's
  number. A metric `reference_null_db` would make it a check ("≤ −60 dB at drive 1").
- `view_sound`: a sweep's spectrogram shows aliasing as lines folding back, and the tone pot's effect as the shape of
  the average spectrum.
- `trace`: the fields are named after the parts (`clip.x_C4`), so "x_C4 went NaN at sample 18,233" points at the
  part. Its 1-sample pass checks block invariance for free.
- `tune_dials`: fit pots to goals, for example "THD −20 dB at −12 dBFS with drive" or a tone setting for a
  centroid. It works without change, since pots are ordinary dials.
- `listen` and `capture`: unchanged.

**Risks:** Claude miswiring a schematic (mitigated by `.op` and the checks); unit slips (`1M`), which the warning
catches; and model parameters copied from LTspice libraries that ignore `CJO` and `TT`, which the compiler says it
ignores.

## 10. Phased plan

| Phase | Deliverables | Agent-days | Risks |
|---|---|---|---|
| **0. Spike** | the generator for 1-port diode blocks on scratch C++; both examples end to end through `fminus --emit fm`; Tables 1–3 reproduced from emitted code | 2 | none new; settles the API |
| **1. First useful slice** | `fminus::circuit` (parser, units, `.model D`, R C L, ideal op-amp, pots and rheostats with tapers, ramped switches; checks with lines; DC point; partition at op-amps; per-block DK in double; Woodbury; TR with BDF2 fallback; 1-port closed form + Newton; self-test, solver and oversampling choice; emission; the `fminus` driver) — 6.5. Library `oversample.fm` (FIR and IIR halfbands, 2x and 4x) and `circuits` docs — 1. Playground: `.cir` in `Patch::compile`/`check`, the include resolver, `Lang.h` tokens, `lint.layers`, the `fminus.circuit` probe (examples compile with no message; 64/333 bits; freeze; `trace` pass 2), the *circuits* guide part and the two examples — 2.5 | **10** | generated-code compile time with many blocks (estimate under 100 ms); latency unreported (IIR default meanwhile); the frozen copy's `Math::Fast` slower (measure); a reconstructed TS tone stage wired wrong (check against a schematic, or validate with ngspice offline if the user installs it) |
| **2. Transistors and op-amp models** | Ebers–Moll BJT and Shichman–Hodges JFET; soft-clipping and GBW op-amp; multi-port Newton (m ≤ 4, unrolled LU, fixed pivots) with linearised guesses and junction limiting; `F_q` decomposition; offset formulation for biased circuits; examples: Rangemaster (1 BJT), Fuzz Face (2 BJTs, 4 ports), Big Muff stage; `alias_db`, the sine scenario and `reference_null_db` in the playground | **9** | coupled-port convergence at hard drive (Table 1's failure, in 4-D; a per-port closed-form guess may help); cost: a Fuzz Face at 4x estimated at 3,000–4,000 cycles a host sample (0.7–0.9 µs, about 4 % of a core a channel) [M]; float accuracy with bias |
| **3. Tubes, tables and FMinus additions** | FMinus: `#line` (1), `fmaf` (0.5), file-scope `const` arrays (2), `omegaf` kernel (2.5); tables for 1–2-parameter ports (Yeh's K-method, nonuniform grids); Koren/Dempwolf triodes, linear transformers; `.channels 2` emission; a measured latency in the contract | **10** | tube models' parameter fits; table memory per voice (put it in `Shared`); FMinus-pin churn across both products |
| 4. (optional) | A1 sugar (`circuit` block calling the library); ADAA for 1-port blocks at 1x; a WDF back end for passive networks | 5–10 | only if A2 proves clunky |

Total about 31 agent-days to Phase 3 [M]. Phase 1 is useful alone: diode clippers, op-amp drives (TS, Distortion+,
DS-1 clipping), passive and active tone stacks, wahs (inductor and pot), all with pots on dials.

## 11. Sources and confidence

Read in full:

- M. Holters, U. Zölzer, "A Generalized Method for the Derivation of Non-linear State-space Models from Circuit
  Schematics", EUSIPCO 2015 — [PDF](https://new.eurasip.org/Proceedings/Eusipco/Eusipco2015/papers/1570103545.pdf) [H]
- M. Holters, U. Zölzer, "A k-d Tree Based Solution Cache for the Non-linear Equation of Circuit Simulations", EUSIPCO
  2016 — [PDF](https://www.hsu-hh.de/ant/wp-content/uploads/sites/699/2017/10/Holters_treecachesolver_EUSIPCO16.pdf) [H]
- M. Holters, U. Zölzer, "Physical Modelling of a Wah-wah Effect Pedal as a Case Study for Application of the Nodal DK
  Method to Circuits with Variable Parts", DAFx-11 —
  [PDF](https://www.hsu-hh.de/ant/wp-content/uploads/sites/699/2017/10/Holters-Zölzer-2011-Physical-Modelling-of-a-Wah-wah-Effect-Pedal-as-a-Case-Study-for-Application-of-the-Nodal-DK-Method-to-Circuit.pdf)
  [H]
- D. T. Yeh, J. S. Abel, J. O. Smith, "Automated Physical Modeling of Nonlinear Audio Circuits for Real-Time Audio
  Effects — Part I", IEEE TASLP 18(4), 2010 — [PDF](https://ccrma.stanford.edu/~dtyeh/papers/yeh10_taslp.pdf) [H]
- D. T. Yeh, J. S. Abel, J. O. Smith, "Simplified, Physically-Informed Models of Distortion and Overdrive Guitar
  Effects Pedals", DAFx-07 — [PDF](https://dafx.de/paper-archive/2007/Papers/p189.pdf) [H]
- S. D'Angelo, L. Gabrielli, L. Turchet, "Fast Approximation of the Lambert W Function for Virtual Analog Modelling",
  DAFx-19 — [PDF](https://iris.unitn.it/bitstream/11572/250641/4/DAFx2019_paper_5.pdf) [H]
- J. Chowdhury, "chowdsp_wdf: An Advanced C++ Library for Wave Digital Circuit Modelling", 2022 —
  [arXiv:2210.12554](https://arxiv.org/pdf/2210.12554) [H]
- ACME.jl: [user guide](https://hsu-ant.github.io/ACME.jl/dev/ug) (solvers, default
  `HomotopySolver{CachingSolver{SimpleSolver}}`), [getting
  started](https://hsu-ant.github.io/ACME.jl/stable/gettingstarted/) (the `@circuit` syntax), and
  [`src/ACME.jl`](https://raw.githubusercontent.com/HSU-ANT/ACME.jl/main/src/ACME.jl) (`nldecompose!`) [H]

Abstract or summary only:

- D. T. Yeh, J. S. Abel, J. O. Smith, "Simulation of the Diode Limiter ... by Numerical Solution of ODEs", DAFx-07
  (abstract: implicit methods preferred) — [archive](https://dafx.de/paper-archive/details/pjj7lul37xFyy74-VjhoYw) [M]
- M. Holters, "Antiderivative Antialiasing for Stateful Systems", Applied Sciences 10(1):20, 2020 —
  [MDPI](https://www.mdpi.com/2076-3417/10/1/20); with Carson's second-order extension as cited by Werner (DAFx-20) and
  La Pastina–D'Angelo's compensation, DAFx-22 —
  [archive](https://dafx.de/paper-archive/details/vhT2Sz-JpSJ4Br8g236cRg) [M]
- A. Bernardini et al., wave digital Newton–Raphson, IEEE TASLP 2021 —
  [PDF](https://re.public.polimi.it/bitstream/11311/1203185/1/WD_NR_TASLP_Bernardini_etal_2021_RG.pdf); R. Proverbio, A.
  Bernardini, A. Sarti, SIM, EUSIPCO 2020 —
  [PDF](https://re.public.polimi.it/retrieve/e0c31c10-7864-4599-e053-1705fe0aef77/Proverbio_Bernardini_Sarti_Eusipco2020_NoOrdNumb.pdf)
  [M]

Forum and secondary:

- LiveSPICE's author on fixed pivots, BDF2 and abandoned symbolic reduction — [KVR
  thread](https://www.kvraudio.com/forum/viewtopic.php?p=7936765), [repo](https://github.com/dsharlet/LiveSPICE) [L]
- SPICE element and diode `.model` syntax —
  [All About Circuits](https://www.allaboutcircuits.com/textbook/semiconductors/chpt-3/spice-models) [M]

In-house:

- FMinus [`README.md`](../../../../README.md), [`docs/extending.md`](../../../extending.md) (recipes 14, 16, 23, 24),
  [`docs/architecture.md`](../../../architecture.md), [`src/ir/Calls.cpp`](../../../../src/ir/Calls.cpp) [H]
- TestingPlayground `README.md`, `CLAUDE.md`, `Source/fminus/Patch.h`, `Patch.cpp`, `Source/dsp/Engine.h`,
  `Measure.cpp`, `Source/plugin/ChatPrompt.cpp`, `ChatAgent.cpp` [H]
- FSynth `docs/research/B-filters.md` §2 [H]

## Appendix A: the experiments (reproducible)

The sources are in [../experiments/](../experiments/), whose [README](../experiments/README.md) says how to build and
run each:

- [`clip.cpp`](../experiments/clip.cpp): first pass, full-band errors; ω approximations alone; aliasing (Table 3); rough
  C++ timings.
- [`clip2.cpp`](../experiments/clip2.cpp): in-band errors; TR, BDF2 and BE against a 64x reference (Table 2); float
  against double.
- [`clip3.cpp`](../experiments/clip3.cpp): initial guesses × Newton counts, in band, in float (Table 1).
- [`halfband.cpp`](../experiments/halfband.cpp): Table 4.
- [`diodetone.fm`](../experiments/diodetone.fm), [`ex1b.fm`](../experiments/ex1b.fm), [`ts.fm`](../experiments/ts.fm),
  [`check.fm`](../experiments/check.fm): the generated-style FMinus; [`ex1c.fm`](../experiments/ex1c.fm) and
  [`ts2.fm`](../experiments/ts2.fm) hold the code exactly as printed in section 4. Compiled and run with FMinus's
  `build-dev/bin/fminus` as built on 2026-10-23 (no `tanf` builtin, math out of line). `--run main 48000000` against
  `--run baseline`, which plays the same sine with no circuit, gives the per-step time
  ([`ex1b_time.fm`](../experiments/ex1b_time.fm) and [`ts_time.fm`](../experiments/ts_time.fm) are `ex1b.fm` and `ts.fm`
  with that harness).

Build: `clang++ -std=c++20 -O2 -ffp-contract=off`, Apple clang 21, M5. The load average was 7–52 throughout, so
timings are rough; errors and attenuations are exact properties of the arithmetic.
