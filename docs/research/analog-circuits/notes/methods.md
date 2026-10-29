# Real-time analog circuit simulation: the methods, and how each turns a netlist into per-sample code

*A research note of [An analog circuit compiler for FMinus](../README.md); where the two disagree, the report holds.*

Research note for an FMinus circuit compiler (TestingPlayground effect and synth). Written 2026-10-29.
Angle: modified nodal analysis, discretisation, the K/DK/state-space family, wave digital filters, port-Hamiltonian
systems, Cytomic-style trapezoidal MNA; the device models audio needs; cost, accuracy, stability and how hard each
method is to automate. The emphasis throughout is on what makes the compiled circuit cheap per sample.

**Confidence tags** (after a claim or a section):
- **[✔]**: read in the primary source during this research (paper text, the tool's code or docs).
- **[◐]**: from an abstract, a secondary source, or partly extracted text.
- **[○]**: my own background knowledge, not re-checked here.
- **[=]**: computed here from the stated formula.

---

## 0. Findings that matter for the compiler

1. **The methods share one pipeline.** Every practical method (a) replaces each capacitor and inductor by a discretised
   companion model, (b) eliminates the linear network once, offline, and (c) solves at each sample a small nonlinear
   system whose size is the **number of nonlinear ports**, not the number of nodes. They differ in the variables they
   use (Kirchhoff, waves or energy), in how they do the elimination, and in what they guarantee. Yeh notes that a
   nonlinear WDF *is* a trapezoidal-rule solver whose nonlinear equations are solved locally [✔ Yeh 2007].
2. **The nonlinear solve dominates the cost.** The linear part is a few dense mat-vecs, `N·(N+M+K)` MACs for
   N states, M inputs and K nonlinear ports. One Newton iteration costs ≈253 operations for a 1-D diode clipper and
   ≈646 for the 2-D BJT Rangemaster, counting an `exp` as 40 [✔ Holmes & van Walstijn 2015]. Whether a circuit is
   cheap therefore depends on (i) K, (ii) the iteration count, and (iii) the cost of each device evaluation.
3. **Audio mostly uses the trapezoidal rule** (bilinear; WDFs build it in). It is 2nd order and adds no damping, but
   it is not L-stable: a stiff mode rings at Nyquist. At 48 kHz, a conducting diode's 2.6 Ω across 10 nF (τ = 26 ns)
   gives a per-sample factor of −0.995, which needs 461 samples to decay by 20 dB. Backward Euler gives 0.0012 [=].
   The fixes are backward Euler or BDF2 locally, the α-transform (α ≈ 0.11 for the diode clipper at 44.1 kHz)
   [✔ Germain & Werner 2015], or oversampling.
4. **Aliasing, not truncation error, sets the oversampling factor.** At 8× the implicit methods are indistinguishable
   in the audio band. Explicit methods need 30–38× just to stay stable [✔ Yeh, Abel & Smith 2007].
5. **Netlist-to-code is solved for the state-space family.** The nodal DK method [✔ Yeh 2010; Holters & Zölzer 2011]
   and the generalised element-matrix method behind ACME.jl [✔ Holters & Zölzer 2015] are fully automatic. WDFs can
   be automated (SPQR decomposition plus MNA-derived R-type scattering) [✔ Werner et al. 2015], but it takes more
   steps. PHS is automatic in PyPHS [✔ docs].
6. **Pots are cheap in DK.** A Woodbury low-rank update inverts an r×r matrix, where r is the number of variable
   resistors (2 per pot), in place of the whole system matrix (15×15 for the Crybaby) [✔ Holters & Zölzer 2011].
   With frozen dials everything folds to constants.
7. **Guarantees.** WDF linear parts are incrementally passive. PHS with a discrete gradient keeps an exact discrete
   power balance, provided its implicit equation is solved exactly. SIM converges for monotone one-port
   nonlinearities. DK, K and MNA guarantee nothing for nonlinear circuits beyond the A-stability of the
   discretisation. For time-varying *linear* trapezoidal filters, a continuous-time common quadratic Lyapunov
   function carries over to discrete time [◐ McClellan 2026].
8. **Shrinking the nonlinear problem is the big lever.** In order of payoff:
   1. closed forms where only one exponential dominates (Wright ω: the diode clipper costs 5 adds, 9 mults, 1 sign
      and 1 ω per sample) [✔ D'Angelo et al. 2019];
   2. exact decomposition of the nonlinear system into block-triangular subsystems [✔ Holters & Zölzer 2017];
   3. minimal-dimension parameter vectors by rank factorisation [✔ Holters & Zölzer 2016];
   4. good Newton initial guesses: previous sample plus linear extrapolation [✔ ibid.];
   5. port resistances matched to the device slope in WD solvers [✔ Bernardini et al. 2021].

   Grid tables explode: Werner's Big Muff root needs a 201³ grid, 8.1 M points [✔/=].
9. **FMinus v0 is single precision only** ("functions over `float`, `int` and `bool`", [README](../../../../README.md))
   [✔]. A float32 `exp` overflows when its argument passes 88.7, about 2.29 V across a junction with Vt = 25.85 mV [=].
   Device evaluation therefore needs limiting or log-domain/Wright-ω forms. Matrices need offline scaling: they are
   computed in double, offline, and emitted as float constants.
10. **Fixed topology means generated monomorphic code wins by 2–6× on linear circuits.** chowdsp_wdf's template API
    beats its runtime-polymorphic API by 6.4× on LPF2 (0.434 s against 2.763 s per 1000 s of 48 kHz audio), 4.0× on the
    Bassman tone stack and 2.2× on FF-2, and by about 1× on the diode clipper [✔ Chowdhury 2022]. A JIT can go further
    and unroll the fixed-size solves. ACME's decomposition gained little because of LAPACK call overhead (a size-8 solve
    took less than 5× as long as a size-1 solve) [✔ Holters & Zölzer 2017]. A JIT with inlined fixed-size code does not
    pay that overhead [○ inference].

---

## 1. Modified nodal analysis (MNA) and companion models

**Formulation** [○ Ho, Ruehli & Brennan 1975; Nagel 1975 SPICE2]. The unknowns are the node voltages `v` (ground
removed) plus the branch currents `j` of the elements that cannot be written as conductances: voltage sources,
VCVSs, the output of an ideal op-amp, and sometimes inductors:

```
[ G  B ] [v]   [i_s]
[ C  D ] [j] = [ e ]
```

Each element adds a fixed "stamp" (Yeh 2010 gives the stamp tables used for netlist parsing [✔]):

- **Resistor**: `+g` on the diagonal at its two nodes, `−g` off the diagonal.
- **Voltage source**: a ±1 column and row coupling `j` to `v`.
- **Ideal op-amp (nullor)**: a constraint row `v+ − v− = 0` and a free output-current column.

Holters & Zölzer 2015 write the same thing per element as `M_v·v + M_i·i + M_x·x + M_ẋ·ẋ + M_q·q = u, f(q) = 0`
[✔].

**Companion models.** Discretising `i = C·dv/dt` turns each reactance into a conductance in parallel with a
history current source. The trapezoidal form, as Simper writes it [✔ Cytomic SvfLinearTrapOptimised2]:

```
capacitor:  i[n] = gC·v[n] − ieq[n],  gC = 2C/T,  ieq[n+1] = 2·gC·v[n] − ieq[n]
inductor:   i[n] = gL·v[n] + jeq[n],  gL = T/(2L), jeq[n+1] = 2·gL·v[n] + jeq[n]     [○ same algebra]
BE cap:     gC = C/T,   ieq = gC·v[n−1]                                                [○]
BDF2 cap:   gC = 3C/(2T), ieq = (C/T)·(2·v[n−1] − v[n−2]/2)                            [○]
```

Holters & Zölzer use the equivalent canonical states `x_C(n) = 2·(2C/T)·v_C(n) − x_C(n−1)`, so that a capacitor
becomes a resistor `R = T/(2C)` beside a current source [✔ 2011].

**Nonlinear devices** are linearised at each Newton iterate: `i ≈ f(v_k) + f′(v_k)(v − v_k)`, that is, a
conductance `f′(v_k)` in parallel with a current source. SPICE re-stamps and re-factors the full matrix on every
iteration. It also limits junction-voltage steps (`pnjlim`), adds `gmin`, and uses source stepping for the DC point
[○].

**Cost of "SPICE in a loop".** A dense LU of an n×n system costs about 2n³/3 flops: 43 for n = 4, 5,333 for
n = 20 [=]. Re-factoring every iteration on every sample is why SPICE is not real-time. Yeh adds two more reasons:
SPICE needs manual supervision when Newton fails to converge, and its error control picks tiny steps [✔ Yeh 2010].

**The key optimisation is the Schur complement onto the nonlinear ports.** The linear part of the MNA matrix is
constant when no pots move, so invert it once. Then

```
v_n = G·x + H·u + K·i_n
```

where `K = Ñ_n·S⁻¹·Ñ_nᵀ` is K×K. This *is* the nodal DK method (Holters & Zölzer 2011, eqs. 44–47) [✔]. Only K
unknowns are iterated at run time.

**Automation**: trivial (stamps). **Accuracy, stability**: whatever the discretisation gives; the formulation
guarantees nothing.

---

## 2. Discretisation

### 2.1 The one-step and two-step rules on the test equation ẋ = λx, z = hλ

| rule | update | order | stability function R(z) | as z → −∞ |
|---|---|---|---|---|
| forward Euler | `x+ = x + h·f(x)` | 1 | 1 + z | unbounded (explicit) |
| backward Euler (BE) | `x+ = x + h·f(x+)` | 1 | 1/(1−z) | 0 (L-stable) |
| trapezoidal (TR) = bilinear | `x+ = x + h/2·(f + f+)` | 2 | (1+z/2)/(1−z/2) | **−1** (A-, not L-stable) |
| BDF2 (Gear 2) | `x+ = 4/3·x − 1/3·x− + 2h/3·f(x+)` | 2 | (two-step) | 0 (L-stable) |
| TR-BDF2, γ = 2 − √2 | TR to t + γh, then BDF2 to t + h | 2 | — | 0 (L-stable) |
| α-transform / Möbius | `x+ = x + h·(f+ + α·f)/(1+α)` | 1 (2 at α = 1) | — | −α |

Sources: Yeh 2007 for FE, BE, TR, BDF2 and RK4 [✔]; Germain & Werner 2015 for the Möbius family [✔]; Hosea &
Shampine 1996 and Bonaventura & Della Rocca for TR-BDF2's L-stability at γ = 2 − √2 [◐]; DAFx26 paper 31 for the
L-stability of BE and the A-stability of TR [✔].

### 2.2 Trapezoidal frequency warping

For linear parts TR is the bilinear transform: `ω_d = (2/T)·atan(ω_a·T/2)`. Two consequences:

- **Prewarping** fixes the response at one frequency only.
- **Warping breaks the TR equivalence** and adds error terms [✔ Germain & Werner].

How far an analog pole moves [=]:

| fs | 5 kHz | 10 kHz | 15 kHz | 20 kHz |
|---|---|---|---|---|
| 44.1 kHz | 4803 Hz (−3.9 %) | 8689 (−13.1 %) | 11490 (−23.4 %) | 13459 (−32.7 %) |
| 48 kHz | 4832 (−3.4 %) | 8855 (−11.5 %) | 11859 (−20.9 %) | 14033 (−29.8 %) |
| 96 kHz | 4956 (−0.9 %) | 9664 (−3.4 %) | 13944 (−7.0 %) | 17709 (−11.5 %) |
| 192 kHz | 4989 (−0.2 %) | 9912 (−0.9 %) | 14709 (−1.9 %) | 19329 (−3.4 %) |

Resonant filter poles above about 5 kHz need prewarping when they are tuned by a dial, or oversampling. Simper's
filters prewarp the cutoff: `g = tan(π·fc/fs)` [○].

### 2.3 Trapezoidal ringing on stiff modes

R_TR(z) tends to −1, so a fast-decaying analog mode becomes a slowly decaying alternating-sign mode at Nyquist [=]:

| τ of the mode (48 kHz) | z = −T/τ | R_TR | samples to −20 dB | R_BE |
|---|---|---|---|---|
| 1 µs | −20.8 | −0.825 | 12 | 0.046 |
| 26 ns (diode at 10 mA, Vt/I = 2.6 Ω, on 10 nF) | −801 | −0.995 | 461 | 0.0012 |
| 2.6 ns | −8013 | −0.9995 | 4613 | 0.00013 |

**The diode clipper shows this directly.** Germain & Werner map its instantaneous pole as the diodes conduct
[✔]. With the bilinear transform, the pole's discrete frequency jumps to π/T at high diode voltage, which causes
spurious high-frequency oscillations. With BE, and with the Möbius transform at α = 0.11, the pole frequency stays
at 0 and nothing rings. For that circuit at 44.1 kHz (1N914, R = 2.2 kΩ, C = 10 nF):

- damping monotonicity holds for α ≲ 0.11;
- stability holds for α ≲ 1.5.

**First-order schemes are less prone to the oscillation.** Ducceschi et al. likewise find first-order schemes less
prone than TR to spurious oscillations at high diode-clipper input [✔ 2021].

### 2.4 What audio work uses, and what it costs

**Yeh, Abel & Smith 2007** (DAFx best paper) compared methods on the diode clipper. The cost is in calls to the
derivative or Jacobian per audio sample, at a 48 kHz base rate, with n = 1.2 Newton iterations [✔]:

| method | oversampling needed | f-calls/step | f-calls per audio sample |
|---|---|---|---|
| forward Euler | 38× | 1 | 38 |
| RK4 | 30× | 4 | 120 |
| BE | 8× | 2n | 19.2 |
| BE semi-implicit (1 Newton step) | 8× | 2 | 16 |
| TR | 8× | 1 + 2n | 27.2 |
| TR semi-implicit | 8× | 3 | 24 |
| BDF2 | 8× | 2n | 19.2 |
| static nonlinearity after an RC lowpass | 8× | lookup | — |

- At 8× the guitar signals needed only 1.03–1.38 Newton iterations per step on average (Table 1). One iteration
  ("semi-implicit") suffices when oversampled.
- Explicit methods "require very high sampling rates for stability".
- The static approximation is often perceptually adequate, with error typically below −40 dB [✔].

**Oversampling in later work**:

| work | factor |
|---|---|
| Bassman WDF | 4× (176.4 kHz) [✔ Dunkel et al. 2016] |
| SIM diode clipper | 4× [✔ Proverbio et al. 2020] |
| germanium-BJT listening tests | 8× [✔ Holmes et al. 2017] |
| DK wah | base rate, with damped Newton [✔ Holters & Zölzer 2011] |

**Discretisation choices by work**:
- WDF builds in TR.
- DK as published by Yeh and Holters uses TR [✔].
- Cytomic uses TR [✔], and per the reproduction blog its ADC 2020 talk also shows a θ-blend toward BE
  (m = 0.5 is TR, m = 1 is BE) [◐].
- PHS uses the discrete gradient, which is the midpoint rule for quadratic energies [○].

**TR-BDF2**: I found no audio paper that uses it [◐ search]. It costs two implicit stages per step, roughly 2× TR.

### 2.5 Non-iterative alternatives

- **Linearly implicit schemes** (Ducceschi, Bilbao & Webb 2021) [✔]. The nonlinearity enters linearly in `x[n+1]`,
  so each step is one linear solve with a fixed cost.
  - The first-order scheme is unconditionally stable for sector-bounded nonlinearities. The second-order one is
    too when `f′ ≥ f/x`.
  - At audio rate, TR and midpoint needed about 5–6 Newton iterations (tolerance 1e-15) on the diode clipper with a
    4 V input.
  - One non-iterative step costs about one Newton iteration ("two nonlinear function calls"), so they ran it at 4×
    for the same CPU, with less aliasing.
  - For the ring modulator (a 5×5 system), all schemes took roughly equal C++ time.
  - The midpoint rule is also unconditionally stable for sector-bounded `f`.
- **Mystran's "cheap" zero-delay-feedback method** [◐ KVR thread 2012]. Replace `tanh(x)` by a secant gain
  `tanh(x0)/x0` taken at the previous state, then solve the now-linear zero-delay loop exactly. Either "Euler for
  the nonlinearity, trapezoidal for the linear part" or a half-sample input average. It needs one evaluation and no
  iteration, but forum users report limited accuracy, and it may fail for sinh-type diode laws.
- **QP / pseudo-inverse with a first-order surrogate of the device law, adaptive multirate** (Zea & Rivera, DAFx26)
  [◐]. It is single-step, but its adaptive step size went down to 200 ns, 128× oversampling, on a Colpitts
  oscillator. Adaptive steps also break a fixed sample grid, so it fits our block-invariance rule poorly.

**For the compiler.** Default to TR and keep `θ` or BE as a per-element option. The case for BE is the reactance
that sits across a junction which conducts hard (junction capacitances, the clipper's C). Offer
α-transform tuning per circuit, using the pole range at the operating extremes as Germain & Werner do. Offer a
"non-iterative" backend for voice-heavy synth use, where cost must be fixed.

---

## 3. The state-space family: K-method, NK, DK, the generalised method and ACME.jl

### 3.1 K-method (Borin, De Poli & Rocchesso 2000)

Restated in Yeh 2010 [✔]; original paper [◐]. The continuous form is

```
ẋ = A·x + B·u + C·i,    i = f(v),    v = D·x + E·u + F·i,    y = L·x + M·u + N·i
```

Discretise, e.g. with BE `x_n = α·H·x_{n−1} + H·(B·u_n + C·i_n)`, where `H = (αI − A)⁻¹` and α = 1/T. That gives

```
p_n = α·D·H·x_{n−1} + (D·H·B + E)·u_n,   K = D·H·C + F,   0 = f(p_n + K·i_n) − i_n
```

The condition for an explicit `i = g(p)` is `det(J_f·K − I) ≠ 0`. That condition also makes the Newton system
solvable. Because `g` is memoryless, it can be **tabulated offline** (Borin et al.'s point).

### 3.2 NK and DK methods (Yeh, Abel & Smith 2010) [✔]

- **NK**: build an MNA system whose unknowns include the capacitor derivatives (the diode clipper's unknowns are
  `V1, V2, I_V, V̇_Cl`), then read off the K-method matrices.
- **DK**: discretise first (companion models), then apply nodal analysis. That gives the discrete K-form directly,
  with `R(v) = p + F·f(v) − v` and Jacobian `F·J_f − I`.
- **Modified K**: iterating on the device *voltages* converges faster than iterating on currents, because the
  voltages are compressive.
- **Homotopy** (`R_H = R(x) − (1−λ)·R(x0)`, λ stepped adaptively) is globally convergent but unbounded in time, so
  it is used offline only.
- **Tables**: a uniform grid with multilinear interpolation (Octave's `interpn`). Dial parameters can be extra table
  dimensions. A rough approximation can seed Newton.

### 3.3 Nodal DK in matrix form, and pots by Woodbury (Holters & Zölzer 2011) [✔]

Incidence matrices `N_R, N_v, N_x, N_u, N_n, N_o` (resistors, variable resistors, reactances, sources, nonlinear
ports, output) and diagonal conductance matrices give the system matrix:

```
S = [ N_Rᵀ·G_R·N_R + N_vᵀ·R_v⁻¹·N_v + N_xᵀ·G_x·N_x    N_uᵀ ]
    [ N_u                                            0    ]
```

Every state-space matrix is incidence × S⁻¹ × incidence: for example `K = Ñ_n·S⁻¹·Ñ_nᵀ`, `G = Ñ_n·S⁻¹·Ñ_xᵀ`. The
run-time schedule per sample is:

1. `p = G·x + H·u`
2. solve `p + K·i − v = 0` with `i = f(v)`
3. `y = D·x + E·u + F·i`
4. `x = A·x + B·u + C·i`

**Pots.** Split `S = S0 + Ñ_vᵀ·R_v⁻¹·Ñ_v`; then

```
S⁻¹ = S0⁻¹ − S0⁻¹·Ñ_vᵀ·(R_v + Q)⁻¹·Ñ_v·S0⁻¹,    Q = Ñ_v·S0⁻¹·Ñ_vᵀ
```

and each matrix gets a low-rank correction, e.g. `K = K0 − U_n·(R_v + Q)⁻¹·U_nᵀ`.

- For the Crybaby this replaces inverting a 15×15 matrix with inverting a 2×2 one (one pot, `R_v = diag(a·VR1,
  (1−a)·VR1)`).
- It never inverts `R_v`, so pot ends with a = 0 or 1 and zero resistance are safe.
- Damped Newton warm-started from the previous sample converged in "typically less than ten" iterations. The model
  ran in real time as an LV2 plugin.
- It matched the measured small-signal response at three pot positions.

**Cost per pot move** [=]: invert an r×r matrix plus about `r·Σ(rows·cols)` MACs over the nine matrices. That is
O(r·(N+M+K)²): about 160 MACs for N = 6, M = 1, K = 2, r = 2. It is cheap enough on a control grid (for example
every 16 absolute samples, which keeps output independent of how blocks are split).

### 3.4 The generalised method (Holters & Zölzer 2015), as in ACME.jl [✔]

**Elements** are described by `M_v·v_e + M_i·i_e + M_x·x_e + M_ẋ·ẋ_e + M_q·q_e = u_e`, `f_e(q_e) = 0`. Table 1
covers sources, R, C, L, the ideal op-amp, a **soft-clipping op-amp** (`V_max·tanh(A·q1/V_max) − q2 = 0`) and the
diode. **Kirchhoff's laws** enter as `T_v·v = 0` and `T_i·i = 0`.

**Discretisation** uses canonical states, so that `ẋ(n) = (x(n) − x(n−1))/T` and `x̂(n) = (x(n) + x(n−1))/2`.
That leaves an under-determined linear system whose general solution gives

```
x(n) = A·x(n−1) + B·u(n) + C·z(n),   y(n) = D·x(n−1) + E·u(n) + F·z(n),
q(n) = D_q·x(n−1) + E_q·u(n) + F_q·z(n),   f(q(n)) = 0
```

- **Gain**: it places no restriction on topology beyond physical plausibility. Their example clipper cannot be
  handled by nodal DK, because one node connects only to nonlinear elements. New element models only extend Table 1.
- **Cost**: a larger one-time linear solve; run time is the same as nodal DK. Choose `D_q` and `E_q` orthogonal to
  `F_q`, and the table dimension matches nodal DK.
- **ACME.jl** implements it. Its README says the solver "saves solutions to use as starting points", and warns that
  complex circuits may be slow or fail [✔]. Gummel–Poon and Jiles–Atherton transformer elements were added later
  [✔ Holmes et al. 2017; Holters & Zölzer 2016].

### 3.5 Making the nonlinear solve cheap: measured levers

- **Op counts per Newton iteration** [✔ Holmes & van Walstijn 2015]. The cost model puts `exp` at 40 ops.

  | method | diode clipper (1-D) init / per iteration | Rangemaster (2-D) init / per iteration |
  |---|---|---|
  | Newton | 234 / 253 | 624 / 646 |
  | chord (Jacobian reused) | 234 / 132 | 624 / 287 |
  | damped Newton | 234 / 261 + 117 per sub-iteration | 624 / 658 + 252 per sub-iteration |

  The chord method is cheaper per iteration but diverges more easily when its first step overshoots. Their
  **capped step** limits each Newton step to the distance of the transitional voltage where the exponential term
  starts to dominate, which keeps iterates representable.
- **Previous-sample warm start plus linear extrapolation** [✔ Holters & Zölzer 2016]:
  `z(0) = z* − (J·F_q)⁻¹·J·Q·(p − p*)`. On "Der SuperOver" (an SD-1 clone: 4 diodes, 2 BJTs, 11 states, 8
  equations in 16 variables, a 7-D parameter vector):

  | initial guess | average iterations | samples needing > 15 iterations |
  |---|---|---|
  | previous sample + extrapolation | 2.586 | 4357 |
  | k-d cache + extrapolation | 2.308 | 88 |

- **k-d tree solution cache** (same paper). On the 2-D treble booster, a 10000×400 grid filled offline left
  **388** cached points, with iterations held well below N_max = 5.
  - On SuperOver, training took almost an hour and cached **27,039** solutions.
  - Even then, 0.14 % of samples needed more than 5 iterations, 88 needed more than 15, and the worst case needed 500.
  - For FMinus a cache built at run time is ruled out: it allocates, and it makes output depend on history.
    Only an offline cache is possible.
- **Dimension reduction** [✔ Holters & Zölzer 2016]. Factorise `[D_q Ẽ_q] = Q·[D̂_q Ê_q]` by QR so that the parameter
  `p = D̂_q·x + Ê_q·ũ` has minimal dimension.
  - Constant sources fold into `q0`.
  - The treble booster drops from 3 dimensions to 2: the 100 µF supply capacitor and the protection diode drop out.
- **Exact decomposition** [✔ Holters & Zölzer 2017]. Find a grouping of the nonlinear elements and an invertible
  `R` such that `F_q·R` is block lower-triangular. The subsystems are then solved in sequence.
  - The search tries at most 2^N − 1 groupings (1023 for N = 10).
  - Treble booster: 2.45 s → 1.99 s (−19 %).
  - Overdrive: split into subsystems of 1, 2, 3 and 2 equations. Mean iterations went from 2.965 for the whole
    system to 1.460, 2.924 and 1.696 for the subsystems, but time only from 6.82 s to 6.79 s, because of LAPACK call
    overhead.
  - It worked only after fixing the bias voltage as ideal. The real bias network couples everything (global
    feedback), and then only the supply diode separates.
- **Tables and their dimensionality**. The Big Muff root needed a 201×201×201 grid (8.1 M points) [✔ Werner et al.
  2015 / =]. Tables are only practical for K ≤ 2, or K = 3 at coarse resolution.

### 3.6 Cost per sample (worked)

A dense precomputed state space with N states, M time-varying inputs, K nonlinear dimensions and Y outputs [=]:

```
linear:   p: K(N+M)   x: N(N+M+K)   y: Y(N+M+K)      MACs
per Newton iteration:  K device evaluations (+ Jacobian) + K×K solve (~2K³/3 + 2K²) + K² for K·J_f
```

- Example: N = 6, M = 1, K = 2, Y = 1 gives 77 MACs linear, plus about 250–650 ops per iteration (Holmes'
  numbers), times 2–3 iterations, times the oversampling factor.
- **Device evaluation and iteration count, not the matrices, set the bill.** That is why closed forms (§4.2) and
  decomposition pay.
- **Sparsity**: A and K are usually dense after elimination [○]. The incidence structure is lost, so dense SIMD
  mat-vecs are the right primitive. FMinus [`float4`/`float8`](../../../simd.md#recipe-7-float2-float4-and-float8) fit.

### 3.7 Other state-space practice

- Dempwolf, Holters & Zölzer, DAFx-10: parametric circuits [◐].
- Mačák & Schimmel: approximating the differential equations [◐].
- Yeh Part II (2012): preamps [◐].
- Holmes & van Walstijn: Rangemaster [✔ refs].
- Eichas et al., DAFx-14: the MXR Phase 90 phaser, JFETs as variable resistors, nodal DK [◐].

**Accuracy** comes from the device models and the discretisation. **Stability** has no nonlinear guarantee; the
practical failure mode is Newton not converging at extreme drive.

---

## 4. Wave digital filters (WDF)

### 4.1 Basics and costs (Fettweis 1986) [○ theory, ✔ for the TR equivalence via Yeh 2007]

**Waves**: `a = v + R·i`, `b = v − R·i`. Elements become one-ports:

| element | port resistance | reflected wave |
|---|---|---|
| capacitor | R = T/(2C) | `b[n] = a[n−1]` |
| inductor | R = 2L/T | `b[n] = −a[n−1]` |
| resistor | R | `b = 0` |
| resistive source | R_s | `b = e` |

Each element costs zero multiplies. Reactances realise the bilinear transform exactly.

**Adaptors.** A 3-port parallel adaptor with port 3 adapted (`G3 = G1 + G2`, γ = G1/G3) [= derivation]:

```
b3 = a2 + γ(a1 − a2);   b1 = b3 + a3 − a1;   b2 = b3 + a3 − a2
```

That is 1 multiply and 6 adds; the series adaptor is the dual.

**Tree evaluation**: waves go up from the leaves, the root (one non-adaptable element, for example the
nonlinearity) reflects, and waves go back down. **Properties** [○]: incremental passivity and pseudo-passivity give
robustness to coefficient rounding and suppress parasitic oscillations. That suits float32.

### 4.2 Closed-form nonlinear roots: Wright ω

**Single diode at the root** (Shockley, port resistance R) [✔ chowdsp_wdf code, eq. 10 of Werner et al. 2015;
re-derived here]:

```
b = a + 2·R·Is − 2·Vt·ω( ln(R·Is/Vt) + (a + R·Is)/Vt ),     ω(x) = W0(eˣ)
```

The log term is a constant, so no `exp` runs at audio rate [✔ D'Angelo et al. 2019]. A diode's series resistance
just adds to R (the same algebra as Banwell & Jayakumar's 2000 closed form) [○].

**Diode pair.** Paiva et al. 2012 treat the reverse diode as open. Werner et al. 2015 add a correction from two
W functions; this is chowdsp's `DiodePair` "Good" (eq. 18) and "Best" (eq. 39) [✔ code; ◐ paper]. Paiva's
op-amp-plus-diode WDF costs 10 multiplies, 22 adds and 2 table interpolations per sample [◐].

**Approximations of ω** (D'Angelo, Gabrielli & Turchet, DAFx-19) [✔]:

| approximation | form | notes |
|---|---|---|
| ω1 | `max(0, x)` | |
| ω2 | C¹ cubic spline on (x1, x2) = (−3.684, 1.973), with ω2 = x above x2 | |
| ω3 | the spline joined to `x − ln x` at x2 = 8 | x2 is a power of 2, so a bit-trick log2 is exact there |
| ω4 | ω3 plus one Newton step, `ω4 = ω3 − (ω3 − e^{x−ω3})/(ω3 + 1)` | |

- `log2` and `exp2` come from exponent extraction plus a C¹ cubic on the mantissa.
- **Results**: the common-collector buffer is solved exactly through ω. The dynamic diode clipper with TR (one
  diode dominating, `sinh → sgn·e^{|v|}/2`) costs **5 adds, 9 mults, 1 sign and 1 ω per sample**.
- They report it stable for all inputs and amplitudes tested, with any of the approximations, because the explicit
  solution relaxes the stiffness.

### 4.3 Arbitrary topologies: R-type adaptors [✔ Werner, Smith & Abel 2015; Werner et al. 2015]

**Decomposition**: an SPQR tree (Fränken, Ochs & Ochs' graph method [◐]) splits the circuit into series (S),
parallel (P) and rigid (R) parts. The R-type junction's scattering matrix comes from MNA on its instantaneous
Thévenin port equivalent (each port is a voltage source `a_n` behind `R_n`):

```
S = I + 2·[0  R]·X⁻¹·[0  I]ᵀ,     R = diag(R_I, R_E),   X = the MNA matrix
```

**One port of the junction can be made adapted**, so it fits a standard tree.

**Case studies**: the Bassman tone stack and the Tube Screamer tone/volume stage. Werner et al. 2018 (TCAS-I) give
four MNA-based derivations, including absorbed active linear multiports [◐].

### 4.4 Multiple and multiport nonlinearities at the root

**Werner, Nangia, Smith & Abel 2015** [✔]. Pull every nonlinear element out of the R-type root and above it, so the
root couples a K-dimensional Kirchhoff nonlinearity `i_C = h(v_C)` to the scattering. Consolidate:

```
v_C = E·a_E + F·i_C,   b_E = M·a_E + N·i_C,   K = F
```

where `E = C12(I + S11·H·C22)·S12`, `F = C12·S11·H·C21 + C11`, `M = S21·H·C22·S12 + S22`, `N = S21·H·C21` and
`H = (I − C22·S11)⁻¹`. Then apply the K-method: `i_C = g(p)`, `p = E·a_E`.

- **Big Muff Pi clipping stage**: 3 nonlinear ports (the diode pair, V_BE and V_BC of a 2N5089), solved through a
  201³ scattered-interpolation table. "Nearly a match" to SPICE.
- **Device values**: 1N914 Is = 2.52 nA, Vt = 25.85 mV; BJT Is = 5.911 fA, αF = 0.9993, αR = 0.5579.

**Newton at the root** in place of a table: Olsen, Werner & Smith, DAFx-16 [◐]; Dunkel et al., DAFx-16 [✔].

- **Bassman 5F6-A / JTM45 preamp**: a 25-port R-type root (8 internal ports to 4 triodes, 17 external ports), so an
  8-D Newton with backtracking.
- **Iterations**: 3.03 per sample on average, with 0.08 backtracks, at 4× (176.4 kHz).
- **Error**: maximum 30 mV on about 165 Vpp (< 0.2 %). The first harmonics' peaks agree within 0.1 dB, with 0.63 dB
  at most.
- **Speed**: at the time it ran "slightly slower than real-time" in RT-WDF.
- **What this means [○ inference]**: a WDF with an R-type root and a K-D Newton has the same nonlinear cost as DK.
  WDF saves only in the linear subtrees.

### 4.5 Iterative WD solvers with convergence results

**SIM (Scattering Iterative Method)** [✔ Proverbio, Bernardini & Sarti 2020; ◐ the convergence theorem]. With J
one-port nonlinearities and a single multi-port junction, each iteration does:

1. a local stage: J scalar solves;
2. a global scattering stage: `a = S·b`;
3. a convergence check.

- **Guarantee**: it converges for monotone i–v characteristics (such as diodes), and the J local solves can run in
  parallel.
- **Port resistances** are re-chosen each sample from each device's local Thévenin slope. That makes S change every
  sample: forming S took 72.3 % of run time.
- **DSR** (recompute S only when the slopes move by more than a threshold) improved a 5-diode asymmetric clipper at
  4×:

  | measure | standard SIM | DSR-SIM |
  |---|---|---|
  | MATLAB real-time ratio | 1.43 | 0.89 |
  | iterations per sample | 1.48 | 1.60 |
  | C++ plugin CPU | 3.5 % | 2.4 % |

**WD Newton–Raphson** (Bernardini, Bozzo, Fontana & Sarti, TASLP 2021) [✔]. The basin of quadratic convergence
grows when each port resistance is set to the slope of the device's v–i curve, `Z_n ≈ v′_n(i_n)`.

- Ring modulator (4 diodes): 4.41 iterations on average, 7 at most, at the optimum.
- Deviating around +1000 % gave 4.67 and 8; larger deviations failed to converge (capped at 25 iterations).
- **Lesson [○ generalised]**: in any Kirchhoff-domain solver, choosing the right linearisation conductance for each
  device's companion is the same lever.

**Multidimensional WDFs** with proven contractivity for multiple nonlinearities: Schwerdtfeger & Kummert,
EUSIPCO 2014 [◐].

### 4.6 Libraries and measured speed

**chowdsp_wdf** (Chowdhury 2022) [✔]: header-only C++; a template-metaprogramming API for fixed topologies or
runtime-polymorphic classes; XSIMD batches; an R-type root with a user functor that refills S when impedances
change. Seconds to process 1000 s of 48 kHz audio on a 3.2 GHz i7 (2018 Mac mini):

| circuit | template | polymorphic | Faust wdmodels | RT-WDF |
|---|---|---|---|---|
| LPF2 (2nd-order RC) | 0.434 | 2.763 | 0.797 | 2.959 |
| FF-2 (Klon Centaur sub-circuit) | 2.763 | 6.127 | 2.126 | 11.73 |
| diode clipper | 2.041 | 2.091 | 7.805 | N/A |
| Bassman tone stack | 0.705 | 2.849 | 0.770 | 7.978 |

Per sample [=]: LPF2 9.0 ns (about 29 cycles), diode clipper 42.5 ns (about 136 cycles), Bassman tone stack (R-type)
14.7 ns (about 47 cycles). For comparison, SIM's C++ clipper used 2.4 % CPU at 4× [✔].

**RT-WDF** (Rest, Dunkel, Werner & Smith, DAFx-16): runtime, arbitrary topologies, multiple nonlinearities [◐].

### 4.7 Time-varying parts and automation

**Pots.**
- Changing a resistor changes the port resistances on its path to the root. Each adaptor coefficient on that path
  needs one division: O(depth).
- At an R-type root, S must be recomputed (an n×n inverse) unless a rank-1 or rank-2 update is used.
- **Theory gap [◐]**: Strube 1982 shows that physically consistent time-varying reactances need structural changes
  to the adaptors and one-ports. Libraries simply update coefficients [○]. The classical passivity proof does not
  cover voltage waves with modulated port resistances.

**Automation**: possible, but it takes several steps:
1. graph and SPQR decomposition;
2. choose the root and the adapted ports;
3. collect the nonlinearities;
4. derive S by MNA;
5. handle non-adaptable elements such as ideal sources and op-amps (nullors: Werner et al., EUSIPCO 2016;
   Giampiccolo et al. [◐]).

That is harder than DK. In return: closed-form roots when there is one nonlinearity, robust linear parts, and cheap
local updates.

---

## 5. Port-Hamiltonian systems (Falaize & Hélie 2016; PyPHS)

### 5.1 Model [◐ paper abstract, ○ standard form]

```
[ ẋ ]          [ ∇H(x) ]
[ w ]  = J ·   [ z(w)  ],     J = −Jᵀ (skew-symmetric interconnection)
[ −y ]         [ u     ]

power balance:  dH/dt = ∇H·ẋ = −z(w)·w + u·y ≤ u·y      (dissipation z(w)·w ≥ 0)
```

Storage (C, L, nonlinear storages), dissipation (R, diodes, transistors) and ports (sources) stay separate. The
structure comes **automatically from the netlist graph** plus a component library [◐ abstract].

### 5.2 Discrete gradient [○ standard; ◐ the paper names a "discrete-gradient based" scheme]

```
δx = x[n+1] − x[n];   for separable H = Σ H_k(x_k):  [∇_d H]_k = (H_k(x_k + δx_k) − H_k(x_k)) / δx_k
                      (falling back to H_k′(x_k + δx_k/2) as δx_k → 0)
(H(x[n+1]) − H(x[n]))/T = −z(w̄)·w̄ + u·ȳ    exactly, for every sample
```

- For quadratic storage `H = xᵀQx/2` this is the midpoint rule, which equals TR on linear parts: second order.
- The implicit equation is solved by Newton–Raphson with a **fixed number of iterations** (their Algorithm 2) [◐].
- **PyPHS** reads SPICE-like `.net` netlists, builds the PHS by graph analysis, offers theta, trapezoidal and
  discrete-gradient methods, and exports C++ (Eigen) [✔ repo]. Its stated aim is "passive-guaranteed simulation"
  plus JUCE C++ code [◐].
- **Examples**: diode clipper, common-emitter BJT amplifier and Crybaby wah, compared with LTspice [◐]. An earlier
  PHS wah appeared at AES 2013 [✔ title].
- **Antialiasing on passive simulations**: Müller & Hélie, DAFx-17 [○].

### 5.3 What the guarantee is worth

- **What it gives**: unconditional energy stability. With no input, H never increases, so the state stays bounded
  (for H radially unbounded). This holds for any number of nonlinearities, including nonlinear storage such as a
  saturating inductor's φ(i).
- **When it holds**: only if the implicit equation is solved exactly. A capped Newton, or float32 rounding, leaves a
  residual that can inject energy [○]. The guarantee is about stability, not accuracy.
- **What it does not cover**: hysteresis (Jiles–Atherton) is not a pure storage. It needs a dissipative split, and
  that is non-trivial [○].
- **Cost**: Newton on the nonlinear subsystem each sample. It is typically larger than DK's K, because energy
  variables are kept [○]. The fixed iteration count gives a predictable cost.

---

## 6. Cytomic (Andy Simper) trapezoidal MNA, and its TPT/ZDF relatives

**Method** [✔ SvfLinearTrapOptimised2, 2013]:
1. Replace each capacitor with the trapezoidal companion `i = gc·v − iceq`, with `iceq ← 2·gc·v − iceq`.
2. Write the nodal equations.
3. Normalise `gc = 1` and move the cutoff into `g`.
4. Solve symbolically by grouped row reduction. Each eliminated dependency costs one division, and divisions can be
   batched.

The result is an explicit update. Simper keeps scaling terms bounded and computes v1 and v2 directly from v0, so
they evaluate in parallel. A form with two fewer operations exists but serialises them. The SVF tick is

```
v3 = v0 − ic2eq;  v1 = a1·ic1eq + a2·v3;  v2 = ic2eq + a2·ic1eq + a3·v3;  ic1eq = 2v1 − ic1eq;  ic2eq = 2v2 − ic2eq
```

That is about 5 multiplies and 7 adds, with `a1 = 1/(1 + g(g+k))`, `a2 = g·a1`, `a3 = g·a2`, `g = tan(π·fc/fs)`,
`k = 1/Q` [○ from memory of the paper's code]. All outputs (LP, BP, HP, notch, peak, all-pass, bell, shelves) are
output mixes.

**Behaviour under modulation**: Simper says it stays stable and smooth even with audio-rate modulation [✔], and it
has lower noise than DF1 (Cytomic noise plots) [✔ page]. Two proofs back the stability claim:

- Wishnick, DAFx-14: a proof of stable time-varying behaviour for the SVF [◐ abstract].
- McClellan, DAFx26: a continuous-time common quadratic Lyapunov function carries over through discretisation. It
  gives new proofs for the SVF and Sallen–Key filters and stable parameter ranges for the Moog and diode ladders
  [◐ abstract].

**Use for a compiler [○ inference]**: for the linear time-varying part (pots on a filter), search offline for a CQLF
over the pot range with an LMI solve. If one exists, the TR model is stable under arbitrary dial motion.

**Nonlinear** (ADC 2020, "From Circuit To Code"): MNA with Newton on the nonlinear nodes [◐]. I could not read the
25 MB slides. The reproduction blog shows MNA, an LU solve and a TR/BE blend parameter. Zavalishin's *The Art of VA
Filter Design* (TPT/ZDF) is the same idea in block-diagram form [○].

**Automation**: MNA plus symbolic Gaussian elimination plus common-subexpression elimination. A compiler can emit
exactly this kind of straight-line code for any linear sub-circuit [○]. Commercial practice is semi-manual.

---

## 7. Device models audio needs

### 7.1 Diodes

**Model**:
- Shockley: `i = Is·(e^{v/(n·Vt)} − 1)`.
- Antiparallel pair: `i = 2·Is·sinh(v/Vt)` [✔ Yeh 2007].
- 1N914: Is = 2.52 nA, Vt = 25.85 mV [✔ Werner 2015]. Yeh's netlist uses Is = 2.52e-9 and an effective Vt = 0.045
  [✔].

**Series resistance**: closed form through Lambert W or Wright ω (Banwell & Jayakumar 2000 [✔ cited]). In
practice: `i = (n·Vt/Rs)·ω(ln(Is·Rs/(n·Vt)) + (v + Is·Rs)/(n·Vt)) − Is` [=].

**Extensions**:
- Junction capacitance `Cj = Cj0/(1 − v/Vj)^m`, diffusion capacitance `TT·di/dv`, and breakdown `BV` (zeners)
  [○ SPICE]. The capacitances turn the diode into a dynamic element with a state.
- "Extended Shockley" (series plus parallel R) keeps a monotone v–i curve and suits WD-NR [✔ Bernardini 2021].

**Robustness in float32**:
- `exp` overflows above a 2.29 V junction voltage at Vt = 25.85 mV [=].
- Remedies: SPICE `pnjlim`-style voltage limiting [○]; capped Newton steps [✔ Holmes 2015]; or reformulate to ω.
  An ω reformulation keeps the exponential's argument a constant log term [✔ D'Angelo].
- **Cost**: `exp` counts as 40 ops in Holmes' model; ω3 is a cubic or `x − log x` plus a few compares [✔ form].

### 7.2 BJTs

**Ebers–Moll** (transport form, as used by Werner and Holters [✔]):

```
i_C = Is(e^{V_BE/Vt} − 1) − (Is/α_R)(e^{V_BC/Vt} − 1),    i_E = (Is/α_F)(e^{V_BE/Vt} − 1) − Is(e^{V_BC/Vt} − 1)
```

**Gummel–Poon** (Holmes, Holters & van Walstijn 2017) [✔]:

```
I_be = I_f/β_f + I_se(e^{V_eb/(N_e·Vt)} − 1),   I_bc = I_r/β_r + I_sc(e^{V_cb/(N_c·Vt)} − 1)
I_cc = (I_f − I_r)/q_b,   q_b = (q1/2)(1 + √(1 + 4·q2)),   q2 = I_f/I_kf + I_r/I_kr
```

- q1 carries the Early effect (V_AF, V_AR); the I_kf/I_kr terms give high-injection gain roll-off. External
  components: R_b, R_e, R_c, C_eb, C_cb. The form follows SPICE [○]; the paper's q1 notation differs slightly.
- **Parameters**: fitted by direct extraction plus Nelder–Mead and interior-point optimisation for OC44 and AC128
  germanium devices. Examples: forward Early voltage about 8.2 V and 19.7 V, I_kf about 44 mA and 463 mA, for the
  two devices [◐: the table extraction lost its column alignment].

**Cost** (nodal DK, time per second of audio) [✔]:

| model | Rangemaster | Fuzz Face |
|---|---|---|
| DC Ebers–Moll | 95.9 ms | 341.6 ms |
| AC Ebers–Moll (+ C_eb, C_cb) | 75.8 ms | 376.5 ms |
| DC Gummel–Poon | 371.8 ms | 819.1 ms |
| AC Gummel–Poon | 357.0 ms | 769.9 ms |

- About 3–3.6 Newton iterations per sample throughout.
- Listeners heard differences: Ebers–Moll had the most high-frequency content.
- **Advice from the paper**: add the junction capacitances first (almost free, and they reduce solver stress), and
  only then DC Gummel–Poon (2.4–3.9× the cost).
- **Dimension**: each BJT is 2 nonlinear ports (V_BE, V_BC). R_b, R_e and R_c add internal nodes, which raises K
  unless they are absorbed into the linear network (they are linear) [○].

### 7.3 JFETs and MOSFETs [○ mostly]

**Shichman–Hodges (SPICE level 1) square law**:
- saturation: `I_D = β(V_GS − V_T)²(1 + λV_DS)`;
- triode: `β[2(V_GS − V_T)V_DS − V_DS²](1 + λV_DS)`;
- cutoff: 0.

It is only C¹ at the region boundaries, which hurts Newton near pinch-off. **EKV** (Enz, Krummenacher & Vittoz 1995) is
C∞ across weak and strong inversion, built from `ln²(1 + e^{(V_P − V_{S,D})/2U_T})` terms. That is better for Newton,
and it maps onto softplus, which the compiler can evaluate stably.

**Audio uses**: JFETs as voltage-controlled resistors in phasers (MXR Phase 90 with nodal DK [◐ Eichas et al.
2014]); JFET and MOSFET gain stages in some overdrives and fuzzes.

### 7.4 Triodes and pentodes

**Koren (1996)** [✔ normankoren.com]:

```
E1 = (E_P/k_P)·ln(1 + exp(k_P·(1/μ + E_G/√(k_VB + E_P²)))),    I_P = (E1^X / k_G1)·(1 + sgn(E1))
```

- 12AX7: μ = 100, X = 1.4, k_G1 = 1060, k_P = 600, k_VB = 300.
- Grid current: a diode-like path through R_GI = 2 kΩ for E_G > 0.
- Pentode: `E1 = (E_G2/k_P)·ln(1 + exp(k_P(1/μ + E_G/E_G2)))`, `I_P = (E1^X/k_G1)(1 + sgn E1)·atan(E_P/k_VB)`,
  `I_G2 = (E_G + E_G2/μ)^{3/2}/k_G2`, with k_G2 4200–4800 for KT88, 6L6CG and 6550.

**Dempwolf & Zölzer (DAFx-11)** [✔ structure via Dunkel et al.; ○ exact form]:

```
I_k = G·(ln(1 + e^{C(V_pk/μ + V_gk)})/C)^γ,    I_g = G_g·(ln(1 + e^{C_g·V_gk})/C_g)^ξ + I_g0,    I_p = I_k − I_g
```

- All parts are smooth (softplus), with grid current. Parameters are fitted to measured 12AX7s.
- It was chosen for Newton over **Cardarilli et al.** (ISCAS 2009), whose grid law is piecewise around a critical
  grid voltage (about 0.2 V). That kink "can lead to poor performance with Newton-based root finders" [✔ Dunkel
  et al.].
- Cardarilli's model still gives better transconductance in saturation. D'Angelo, Pakarinen & Välimäki 2013 built
  3-port WD triodes on it: a secant method on 1–2 port voltages, about 50 % less CPU than earlier models
  [◐ abstract].
- **Why grid current matters**: it causes the asymmetric clipping and even harmonics of an overdriven preamp
  [✔ Dunkel et al.].
- **Miller capacitance**: the plate–grid capacitance creates feedback around the tube. It matters for tone and
  computability [◐ Werner 2015 citing Karjalainen & Pakarinen].

**Cost**: one device evaluation is 1–2 softplus, a sqrt and a `pow` (`exp(X·ln E1)`). That is about 4–5
transcendentals per triode per iteration [○]. A 4-triode preamp is an 8-D Newton (Bassman).

### 7.5 Op-amps

| level | model | stated cost or source |
|---|---|---|
| ideal | nullor: `v+ = v−`, free output current; no state, removes a node | [✔ Holters 2015 Table 1] |
| soft-clip rails | `V_max·tanh(A·(v+ − v−)/V_max) − v_o = 0`, one nonlinear dimension | [✔ ibid.] |
| finite GBW | one pole, `τ·v̇_o = A(v+ − v−) − v_o`, GBW = A/(2πτ); adds a state | [○] |
| slew limit | clamp v̇_o to ±SR | [○] |
| Boyle macromodel | differential pair, gain stage, dominant-pole C, output diodes | Boyle et al. 1974 [○] |
| WDF | ideal op-amp WDF (Paiva 2012); nullors and macromodels in arbitrary topologies | [◐ Paiva 2012; Werner et al. 2016] |

- The slew clamp is non-smooth, so make it smooth (`SR·tanh(...)`) for Newton.
- **When slew matters**: a µA741 (0.5 V/µs typical) limits a 4 V-peak sine above `SR/(2π·4 V)` ≈ 19.9 kHz [=]. It
  rarely matters in 9 V pedals, but it does in high-gain stages driven hard.

### 7.6 Transformers and inductor saturation

**Jiles–Atherton as a circuit element** (Holters & Zölzer, DAFx-16) [✔]:

```
M = M_rev + M_irr,  M_rev = c(M_an − M_irr),  M_an = M_s·L((H + αM)/a),  L(x) = coth x − 1/x
dM/dt = [(1−c)·δ_M·(M_an − M)/((1−c)δk − α(M_an − M))·Ḣ + c·(M_s/a)(Ḣ + αṀ)·L′((H+αM)/a)]
δ = sign(Ḣ),  δ_M = 1 if δ and (M_an − M) agree in sign, else 0
H = (1/D)·Σ n_k·i_k   (Ampère, toroid),    v_k = μ0·A·n_k·(Ḣ + Ṁ)   (Faraday)
```

- **States**: H and M. **Auxiliary vector**: `q = [(H + αM)/a, M, Ḣ, Ṁ]`. The Jacobian is given in closed form.
- **Numerics**:
  - evaluate L and L′ by Taylor series near 0 (|x| < 1e-4; 1e-3 for L″);
  - set δ = 1 when Ḣ = 0;
  - scale the residual by 1e4/M_s so it is comparable with diode residuals.
- **Parameter sets in Table 1**: (M_s 1.6e6 A/m, a 1.1e3, α 1.6e-3, k 4.0e2, c 0.17) and (2.75e5, 14.1, 5e-5, 17.8,
  0.55).
- **Tests**: hysteresis loops, and a simplified output-transformer stage.

**Other models**:
- Chowdhury, DAFx-19: a real-time Jiles–Atherton tape-hysteresis model [◐]. The solver options (RK and NR variants)
  are [○].
- Paiva et al. 2011: real-time audio transformer emulation [◐].

**Cheaper**: anhysteretic saturation only, with `φ = L0·I_sat·tanh(i/I_sat)` or a Langevin function as a nonlinear
inductor [○]. It is smooth and passive, and fits PHS as nonlinear storage. Hysteresis needs `sign(Ḣ)`, which is
non-smooth: Newton needs care and the model memory doubles.

### 7.7 Potentiometers

**Model**: two resistors, `R1 = R·f(θ)` and `R2 = R·(1 − f(θ))`, plus a small end resistance. Nodal DK can use
Woodbury and so never needs `1/R` at the ends [✔ Holters 2011].

**Tapers**:

| taper | f(θ) |
|---|---|
| linear (B) | θ |
| audio / log (A) | `(b^θ − 1)/(b − 1)` with `f(0.5) = 1/(√b + 1)`: b = 81 gives 10 % at mid-rotation, b ≈ 32.1 gives 15 % [=] |
| reverse log (C) | `1 − f_A(1 − θ)` |

- **Real "commercial log" pots** are often two linear tracks of different resistivity joined together, and the join
  is audible [✔ ESP].
- **Law-faking**: 22 kΩ across a 100 kΩ linear pot comes within +1.58 / −1.7 dB of log [✔ ESP]. The compiler should
  offer measured or segment tapers.
- **Dial binding**: θ is a [knob or stream](../../../extending.md#recipe-14-parameters-the-hosts-values). **Frozen
  dials** fold `f(θ)` and every Woodbury product to constants. While a dial moves, apply the r×r update on the absolute
  control grid [○].

---

## 8. Cross-cutting issues for an FMinus compiler [○ inference unless tagged]

- **Precision**: FMinus v0 has only float32 [✔ README]. Compute every matrix offline in double; it is generated
  code, so the host does this in C++.
  - Scale units (V, mA, kΩ, nF, µs) to balance entries. In SI units, Holters' 2016 treble-booster matrices span
    about 5e-8 (E_q) to 7 (D_q) [✔].
  - Prefer solving for voltages (compressive) [✔ Yeh].
  - Bound device arguments.
  - The residual tolerance can only be about 1e-6 relative.
- **Block-size invariance and real-time rules**:
  - The per-sample state-space or WDF update is block-invariant by construction.
  - Newton needs a fixed maximum and deterministic warm starts from the previous sample's state. Those are state,
    not block, dependent.
  - No run-time caches (they allocate), and no adaptive time steps (they break the sample grid).
  - Pot updates go on the absolute sample grid.
- **Voices and SIMD**: [poly float](../../../simd.md#recipe-9-poly-float) lanes run the same straight-line code. Newton
  iteration counts vary per lane, so run to the lane maximum with masks; this favours fixed-iteration schemes (§2.5,
  PHS). chowdsp_wdf proposes up to 4× for a 4-voice SIMD synth (a theoretical estimate) [✔].
- **Oversampling and aliasing**: aliasing sets the factor [✔ Yeh 2007]. Antiderivative antialiasing for *stateful*
  systems (Holters, DAFx-19 best paper) covers a class that includes single one-port nonlinearities [◐]. It can
  replace part of the oversampling.

---

## 9. Summary table

| Method | Automatable from an arbitrary netlist | Nonlinear support | Time-varying (pots) | Cost per sample (dominant term) | Stability guarantee | Best use |
|---|---|---|---|---|---|---|
| **Full MNA + Newton each sample** (SPICE-style, fixed step) | Trivial (stamps) | Any number, any model | Trivial (re-stamp) | Re-factor the n×n system per iteration (dense ~2n³/3: 5.3 kflop at n = 20); worst | None (A-stable linear part with TR/BE); needs limiting and gmin | Offline reference, compile-time validation, golden tests |
| **K-method** (Borin et al.), hand-derived ODE | No (needs K-form); NK automates it | Vector nonlinearity; tables for K ≤ 2–3 | Recompute K matrices; tables gain a dimension | Linear MACs + table lookup or K-D Newton | None for nonlinear; implicit-map condition `det(J_f·K − I) ≠ 0` | Acoustic or small circuits with 1–2 nonlinearities |
| **Nodal DK** (Yeh; Holters & Zölzer) | **Yes, fully** (some topologies fail, e.g. a node touching only nonlinear elements) | Many, solved jointly (K-D Newton, tables, k-d cache) | **Woodbury r×r update**, cheap for few pots | `N(N+M+K)` MACs + iterations × (K devices + K×K solve); ~250–650 ops per iteration (K = 1–2) | None; Newton convergence is the risk | General pedals and preamps; the default backend |
| **Generalised state space** (Holters & Zölzer 2015; ACME.jl) | **Yes, any physically plausible circuit**; element table extensible | Any element model (soft-clip op-amp, Gummel–Poon, Jiles–Atherton) | As DK (low-rank) [○] | Same run time as DK; larger offline solve | As DK | **Compiler IR**: the most general; add decomposition and dimension reduction |
| **Nonlinear state space, explicit or semi-implicit, hand-derived** (e.g. Huovilainen-style ladders, mystran's cheap ZDF) | No | Smooth nonlinearities; accuracy limited | Excellent (coefficients closed form) | Lowest: one evaluation per nonlinearity, no iteration | None (explicit methods need 30–38× for stiff circuits) | Synth filters with tanh-type stages where character beats exactness |
| **Linearly implicit / non-iterative** (Ducceschi et al. 2021) | Yes (from state space) | Smooth, sector-bounded | Yes | One linear solve per step, fixed; run at 2–4× | Unconditional (1st-order scheme, sector-bounded f) | Voice-heavy synths that need fixed CPU |
| **WDF, classic** (one nonlinearity or a diode pair at the root) | Yes for series-parallel (SPQR), moderate effort | One, closed form (Wright ω) | Coefficient updates on the path to the root, O(depth) | **Lowest**: 1 mul + ~6 adds per adaptor + one ω; clipper ≈ 43 ns per sample, LPF2 ≈ 9 ns (chowdsp) | Incremental passivity of linear parts; robust to rounding | Passive networks (tone stacks), single clippers, modular hand-tuned models |
| **WDF R-type + multiple nonlinearities** (Werner: K-method or Newton at the root) | Yes, but complex (SPQR + MNA for S + collect nonlinearities) | Many (vector Newton or table) | S recompute at the R-type root (matrix inverse), subtrees cheap | Same K-D Newton as DK + cheap subtrees; Bassman 8-D: 3.03 iterations per sample at 4× | Passive linear subtrees only | Tube preamps with complex topology (25-port Bassman) |
| **SIM / WD-NR** (Bernardini et al.) | Yes (one junction from the topology) | J one-ports by scalar local solves; NR for multiport | S recompute is the dominant cost (72 %); DSR cuts it | Iterations (~1.5) × (J scalar solves + S·b, O(n²)) | **SIM converges for monotone one-ports**; NR basin largest at `Z = v′(i)` | Many diodes (multi-diode clippers, ring modulators); parallel local solves |
| **Port-Hamiltonian + discrete gradient** (Falaize & Hélie; PyPHS) | **Yes** (PyPHS: netlist to C++) | Any, including nonlinear storage (saturating L) | Parametric yes; modulated storage needs care [○] | Newton on the nonlinear subsystem, fixed iteration count; often larger than DK | **Discrete power balance, so passive and stable** if the implicit solve is exact | Circuits where guaranteed stability matters: high feedback, self-oscillation, nonlinear reactances |
| **Cytomic trapezoidal MNA / TPT-ZDF** (Simper; Zavalishin) | Yes in principle (MNA + symbolic elimination); semi-manual in practice | Newton on MNA, or one-step secant linearisation | **Best**: per-sample coefficients, audio-rate modulation | Linear SVF ≈ 5 mul + 7 add; plus Newton for nonlinear nodes | Linear TR SVF and Sallen–Key proven stable under modulation (Wishnick; McClellan CQLF); none for nonlinear | Filters (SVF, Sallen–Key, ladders) under modulation; synth voices |
| **Precomputed solution** (K-method tables, k-d cache) | Yes (offline) | Any, but only low-dimensional parameter vectors | Pots become table dimensions | Lookup and interpolation (2^K corners), or 1–2 Newton steps from a cached seed | Offline homotopy avoids run-time failure; table error bounded by the grid | K ≤ 2 after decomposition and reduction; seeding Newton |

---

## 10. Open questions and gaps in this research

- Simper's ADC 2020 slides (25 MB) were not readable here. How Cytomic solves nonlinear nodes (iteration count,
  initial guess, caching) is unverified [◐].
- The full texts of Falaize & Hélie 2016 (MDPI returned 403) and of the SIM convergence theorem were not read. Two
  points rest on secondary sources: the "fixed Newton iterations" claim, and the exact scope of the monotone-one-port
  guarantee.
- I found no published comparison of DK, WDF and PHS on the *same* circuit with cycle counts. The numbers above come
  from different machines and languages: Julia (ACME), MATLAB and C++.
- The Dempwolf parameter values were not verified, so I give the structure only.
- Whether ACME's generalised method supports variable resistors directly (as the 2011 Woodbury nodal DK does) is
  unverified.

---

## Sources

Primary sources read during this research are marked ✔ above; links follow.

- Yeh, Abel, Smith. *Automated Physical Modeling of Nonlinear Audio Circuits for Real-Time Audio Effects, Part I*,
  IEEE TASLP 2010. https://ccrma.stanford.edu/~dtyeh/papers/yeh10_taslp.pdf
- Yeh, Abel, Smith. *Simulation of the diode limiter in guitar distortion circuits by numerical solution of ODEs*,
  DAFx-07. https://www.dafx.de/paper-archive/2007/Papers/p197.pdf
- Holters, Zölzer. *Physical modelling of a wah-wah effect pedal … nodal DK method to circuits with variable parts*,
  DAFx-11.
  https://www.hsu-hh.de/ant/wp-content/uploads/sites/699/2017/10/Holters-Zölzer-2011-Physical-Modelling-of-a-Wah-wah-Effect-Pedal-as-a-Case-Study-for-Application-of-the-Nodal-DK-Method-to-Circuit.pdf
- Holters, Zölzer. *A generalized method for the derivation of non-linear state-space models from circuit
  schematics*, EUSIPCO 2015. https://new.eurasip.org/Proceedings/Eusipco/Eusipco2015/papers/1570103545.pdf
- Holters, Zölzer. *A k-d tree based solution cache for the non-linear equation of circuit simulations*,
  EUSIPCO 2016. https://www.hsu-hh.de/ant/wp-content/uploads/sites/699/2017/10/Holters_treecachesolver_EUSIPCO16.pdf
- Holters, Zölzer. *Automatic decomposition of non-linear equation systems in audio effect circuit simulation*,
  DAFx-17. https://www.dafx.de/paper-archive/2017/papers/DAFx17_paper_29.pdf
- Holters, Zölzer. *Circuit simulation with inductors and transformers based on the Jiles-Atherton model of
  magnetization*, DAFx-16. https://www.hsu-hh.de/ant/wp-content/uploads/sites/699/2017/10/Holters_jamodel_DAFx16.pdf
- ACME.jl. https://github.com/HSU-ANT/ACME.jl
- Holmes, van Walstijn. *Improving the robustness of the iterative solver in state-space modelling of guitar
  distortion circuitry*, DAFx-15. https://pureadmin.qub.ac.uk/ws/files/17395135/improving_the_robustness.pdf
- Holmes, Holters, van Walstijn. *Comparison of germanium bipolar junction transistor models for real-time circuit
  simulation*, DAFx-17.
  https://www.hsu-hh.de/ant/wp-content/uploads/sites/699/2017/10/Holmes-Holters-van-Walstijn-2017-Comparison-of-Germanium-Bipolar-Junction-Transistor-Models-for-Real-time-Circuit-Simulation2.pdf
- Borin, De Poli, Rocchesso. *Elimination of delay-free loops in discrete-time models of nonlinear acoustic
  systems*, IEEE TSAP 8(5), 2000 (abstract via
  https://www.research.unipd.it/handle/11577/1343915)
- Germain, Werner. *Design principles for lumped model discretisation using Möbius transforms*, DAFx-15.
  https://www.dafx.de/paper-archive/2015/DAFx-15_submission_52.pdf
- Ducceschi, Bilbao, Webb. *Non-iterative schemes for the simulation of nonlinear audio circuits*, DAFx20in21.
  https://www.dafx.de/paper-archive/2021/proceedings/papers/DAFx20in21_paper_9.pdf
- Zea, Rivera. *Residual-driven adaptive multi-rate QP framework for nonlinear analog audio circuit emulation*,
  DAFx26. https://www.dafx.de/paper-archive/2026/papers/DAFx26_paper_31.pdf
- Hosea, Shampine. *Analysis and implementation of TR-BDF2*, Appl. Numer. Math. 20, 1996.
  https://pages.mtu.edu/~struther/Courses/OLD/Other/LessOld/5628_11/TRBDF2_Hosea_AppNumMath_20_pp21-37_1996.pdf ;
  Bonaventura, Della Rocca, arXiv 1510.04303. https://arxiv.org/pdf/1510.04303
- Werner, Nangia, Smith, Abel. *Resolving wave digital filters with multiple/multiport nonlinearities*, DAFx-15.
  https://www.dafx.de/paper-archive/2015/DAFx-15_submission_54.pdf
- Werner, Smith, Abel. *Wave digital filter adaptors for arbitrary topologies and multiport linear elements*,
  DAFx-15. https://dafx.de/paper-archive/details/dFlSsTeUmyOhfdA7HIi7Aw
- Dunkel, Rest, Werner, Olsen, Smith. *The Fender Bassman 5F6-A family of preamplifier circuits: a WDF case study*,
  DAFx-16. https://www.dafx.de/paper-archive/2016/dafxpapers/37-DAFx-16_paper_53-PN.pdf
- Werner et al. *An improved and generalized diode clipper model for wave digital filters*, AES 139, 2015.
  https://ccrma.stanford.edu/papers/improved-and-generalized-diode-clipper-model-wave-digital-filters
- D'Angelo, Gabrielli, Turchet. *Fast approximation of the Lambert W function for virtual analog modelling*,
  DAFx-19. https://iris.unitn.it/bitstream/11572/250641/4/DAFx2019_paper_5.pdf
- Chowdhury. *chowdsp_wdf: an advanced C++ library for wave digital circuit modelling*, arXiv 2210.12554, 2022.
  https://arxiv.org/html/2210.12554v1 ; code:
  https://raw.githubusercontent.com/Chowdhury-DSP/chowdsp_wdf/main/include/chowdsp_wdf/wdft/wdft_nonlinearities.h
- Proverbio, Bernardini, Sarti. *Toward the wave digital real-time emulation of audio circuits with multiple
  nonlinearities*, EUSIPCO 2020.
  https://re.public.polimi.it/retrieve/e0c31c10-7864-4599-e053-1705fe0aef77/Proverbio_Bernardini_Sarti_Eusipco2020_NoOrdNumb.pdf
- Bernardini, Bozzo, Fontana, Sarti. *A wave digital Newton-Raphson method for virtual analog modeling of audio
  circuits with multiple one-port nonlinearities*, IEEE/ACM TASLP 2021.
  https://re.public.polimi.it/bitstream/11311/1203185/1/WD_NR_TASLP_Bernardini_etal_2021_RG.pdf
- Schwerdtfeger, Kummert. *A multidimensional approach to wave digital filters with multiple nonlinearities*,
  EUSIPCO 2014. https://new.eurasip.org/Proceedings/Eusipco/Eusipco2014/HTML/papers/1569923213.pdf
- Werner, Dunkel, Rest, Olsen, Smith. *Wave digital filter modeling of circuits with operational amplifiers*,
  EUSIPCO 2016. https://ccrma.stanford.edu/papers/wave-digital-filter-modeling-of-circuits-with-operational-amplifiers
- Falaize, Hélie. *Passive guaranteed simulation of analog audio circuits: a port-Hamiltonian approach*, Applied
  Sciences 6(10):273, 2016. https://mdpi.com/2076-3417/6/10/273 ; summary:
  https://g-haine.github.io/phraise/passive-guaranteed-simulation-of-analog-audio-circuits-a-port-hamiltonian-approach
- PyPHS. https://www.github.com/pyphs/pyphs
- Simper (Cytomic). *Linear trapezoidal integrated SVF* (2013) and the technical papers index.
  https://cytomic.com/files/dsp/SvfLinearTrapOptimised2.pdf ; https://cytomic.com/technical-papers/ ; ADC 2020
  reproduction: https://spiceworld-blog.netlify.app/posts/simper/simper_reproduce.html
- Wishnick. *Time-varying filters for musical applications*, DAFx-14.
  https://dafx.de/paper-archive/details/yfd4LbdXr2Cywsqxfby8ug
- McClellan. *Stability analysis of time-varying virtual analog filters*, DAFx26.
  https://dafx.de/paper-archive/details/nzadVise8gmeLmPAFmfe2g
- Mystran (Voipio). *Cheap non-linear zero-delay filters*, KVR forum, 2012.
  https://www.kvraudio.com/forum/viewtopic.php?p=4925988
- Koren. *Improved vacuum tube models for SPICE simulations* (Glass Audio 1996; web 2003).
  https://www.normankoren.com/Audio/Tubemodspice_article.html
- Dempwolf, Zölzer. *A physically-motivated triode model for circuit simulations*, DAFx-11.
  https://dafx.de/paper-archive/details/MGMeIyM6_9oAzOgKCuzEqw
- D'Angelo, Pakarinen, Välimäki. *New family of wave-digital triode models*, IEEE TASLP 21(2), 2013.
  https://aaltodoc.aalto.fi/server/api/core/bitstreams/c94fca6e-463e-4f8e-a25d-9dce4aa4e8c8/content
- Cardarilli, Re, Di Carlo. *Improved large-signal model for vacuum triodes*, ISCAS 2009.
  https://researchr.org/publication/CardarilliRC09/authors
- Paiva, D'Angelo, Pakarinen, Välimäki. *Emulation of operational amplifiers and diodes in audio distortion
  circuits*, IEEE TCAS-II 59(10), 2012. https://manuscript.isc.ac/Inventory/49/1288417.htm
- Chowdhury. *Real-time physical modelling for analog tape machines*, DAFx-19.
  https://dafx.de/paper-archive/details/Z5Ow_E5YHVxvndTAyeEuzA
- Holters. *Antiderivative antialiasing for stateful systems*, DAFx-19.
  https://www.hsu-hh.de/ant/wp-content/uploads/sites/699/2020/10/DAFx2019_paper_4.pdf
- Eichas, Fink, Holters, Zölzer. *Physical modeling of the MXR Phase 90 guitar effect pedal*, DAFx-14.
  https://dafx14.fau.de/papers/dafx14_felix_eichas_physical_modeling_of_the_.pdf
- Elliott (ESP). *Beginners' guide to potentiometers*. https://sound-au.com/pots.htm
- FMinus's [README](../../../../README.md) (float-only v0, knobs, streams and frozen constants, math builtins).
- Not re-checked here [○]: Ho, Ruehli, Brennan, *The modified nodal approach to network analysis*, IEEE TCAS 1975;
  Nagel, SPICE2 (UCB ERL-M520, 1975); Fettweis, *Wave digital filters: theory and practice*, Proc. IEEE 74(2), 1986;
  Strube, *Time-varying wave digital filters*, IEEE TASSP 1982; Ebers & Moll, Proc. IRE 1954; Gummel & Poon, BSTJ
  1970; Shockley, BSTJ 1949; Jiles & Atherton, JMMM 61, 1986; Boyle et al., IEEE JSSC 1974; Enz, Krummenacher &
  Vittoz (EKV), AICSP 1995; Banwell & Jayakumar, Electronics Letters 2000; Fränken, Ochs & Ochs, IEEE TCAS-I 2005;
  Zavalishin, *The Art of VA Filter Design*.
