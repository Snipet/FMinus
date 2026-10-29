# Circuits: SPICE netlists to FMinus modules (Phase 0, and Phase 1 as built so far)

`fminus::circuit` turns a SPICE-style netlist (a `.cir` file) into the text of one FMinus module, which compiles like
any other source. It is the analog circuit compiler that [research/analog-circuits/](research/analog-circuits/README.md)
proposes (its §11). Phase 0 built the 1-port generator, both of the report's examples through `fminus --emit fm`, and
the report's accuracy and cost tables reprinted from emitted code. Phase 1 is being built in slices; the first added the
public API it needs, the self-test that chooses a netlist's configuration on the report's stress matrix and targets
(asynchronously for a host, which keeps the answer), the run-time guard every module carries, and FMinus's messages
re-cited at the netlist's lines; the second, the synth path (report §7): the OTA, law inputs that move a device's law
every sample, the tanh law's solve and ADAA with their carry, the cutoff prewarp, and the self-test's smooth-monotone
row. This page says what it does, how to use it, what it decided where the design left a choice or the code proved the
design wrong, and what is not built yet.

## Status

- **Built:** netlists of resistors, capacitors, pots, ideal op amps, antiparallel diode pairs and OTAs, cut into blocks
  at the op amps; each block's equations (modified nodal analysis, trapezoidal companions, its one nonlinear port in
  Holters' form: a diode pair's voltage, an OTA's current) eliminated with a pivot order fixed at generation and
  printed as straight-line float code; the closed-form omega3 guess with fixed Newton steps (a diode's), the previous
  step's v with fixed Newton steps (an OTA's); Holters' antiderivative antialiasing (ADAA) for both laws; IIR or FIR
  halfband oversampling of the region that needs it; `.channels 2`. From Phase 1: the self-test (the stress matrix, the
  converged references, T1-T4, the exp-affine and smooth-monotone candidates in the report's cost-model order, the
  ordered search, the no-configuration answer), the provisional configuration a netlist plays until it answers, the
  run-time guard, the map from each generated line to the netlist's (`fminus --self-test`, `--notes`); law inputs
  (`.input NAME - kind=cv|current`) in an OTA's gain, each sample's, with ADAA's carry; `.prewarp on`.
- **Checked:** the parser, the plan and the equations against hand derivations, Yeh's eq. 24, report §7.2's OTA stage
  and textbook responses, each law's antiderivative and bracket, the carry's residual (`tests/unit/CircuitTest.cpp`);
  the emitted coefficients against the double derivation, every configuration compiling nonblocking, the OTA's in 40
  combinations, the bracket through the JIT (`tests/unit/CircuitEmitTest.cpp`); the self-test's order (both rows),
  search, gates, key and answers, the synth's stress, T1 on the region's own input, and its determinism, cancel,
  budget and errors (`tests/unit/CircuitSearchTest.cpp`); the text, the driver and the equivalence with the research's
  hand-written modules (`tests/lit/circuit/`); the report's targets on its full stress set, the self-test's choices,
  the guard, and the synth path's gates, prewarp, carry, lanes and block splits, through the JIT (`example-circuits
  --check`).
- **Not built:** the rest of Phase 1, and later, at the end of this page.

## Use

```bash
build-dev/bin/fminus examples/circuits/diodetone.cir --emit fm        # the generated module
build-dev/bin/fminus examples/circuits/diodetone.cir                  # compiled, as that text would be (--emit ll)
build-dev/bin/fminus play.fm -I examples/circuits --run dc 1          # a program that does #include "diodetone.cir"
```

`fminus` reads a `.cir` input (any case) as the text the circuit compiler generates from it: `--emit fm` prints that
text (to `-o`), and `--emit ll`, `asm`, `ast`, `obj` and `--run` compile it, with FMinus's messages citing
`<file>.cir.fm`, the lines `--emit fm` prints. An `#include "x.cir"` generates too: the file is read as `x.cir.fm`,
once. The circuit compiler's errors and warnings go to stderr, as clang words them (`pedal.cir:7:1: error: ...`), and an
error stops `fminus` before FMinus's own messages. `--emit fm` on anything but a `.cir` is an error.

From C++, `include/fminus/Circuit.h` (C++17, no LLVM; a host links `FMinus::FMinus`):

```cpp
const fminus::circuit::Result result = fminus::circuit::generate(netlist, "pedal.cir");
if (!result.errors.empty())
    return report(result.errors);                        // "pedal.cir:7:1: error: ..."
auto program = fminus::Program::compile(result.source, errors);
```

`Result` also holds `warnings`, `notes` (the plan: each block, its rate and solver, the latency), `module` (its name),
`pots` (`set()`'s arguments, in order) and the fields [below](#result). `Options` override the netlist's directives.
`generate()` is pure and deterministic (the same text and options give the same `Result`, byte for byte, on any
machine), safe on any thread, and allocates: call it off the audio thread. A host that serves `#include` itself
generates in its resolver, as the playground would:

```cpp
options.include = [&](std::string_view name, std::string_view from) -> std::optional<fminus::Program::IncludedFile> {
    std::optional<std::string> text = readFile(name, from);              // the host's files
    if (!text)
        return std::nullopt;
    if (name.size() < 4 || name.substr(name.size() - 4) != ".cir")
        return fminus::Program::IncludedFile{std::string(name), *text};
    const fminus::circuit::Result r = fminus::circuit::generate(*text, name);
    circuitErrors += r.errors;                                           // the resolver cannot report them itself
    return fminus::Program::IncludedFile{std::string(name) + ".fm", r.source};
};
```

`tests/library/CircuitTest.cpp` does this; `examples/circuits/` has the report's netlists, and `otalp.cir`, the synth
path's.

A netlist with law inputs (`.input iabc - kind=current range=10u:2m`) makes `tick` and `step` take them after the
signal, as floats (`Result::inputs` lists them, with their kinds and ranges): a voice computes its OTA filter's bias
current every sample, from its note, its envelope and a dial, through one `expf` (the expo converter is the program's,
not the circuit's), and passes it in.

```c
const float iabc = 1e-5f * expf(0.0577623f * (note - 36 + 48 * cutoff + 24 * env));   // 10 uA at C2, x2 an octave
const float y = filter.tick(osc, iabc);                                                // clamped to 10u:2m in tick
```

A netlist that leaves its configuration to the self-test (no `.oversample`, `.adaa` or `.solver`, or `auto`) generates
at once with a provisional configuration, `Result::provisional` set and `Result::selfTestKey` naming it. The self-test
takes seconds (a one-pot clipper's whole search, about 3 s on one core of an M5, 0.8 s on eight), so a host runs it
apart, on its own threads, and keeps the answer:

```cpp
fminus::circuit::Result r = fminus::circuit::generate(netlist, "pedal.cir");      // provisional: play it meanwhile
if (r.provisional && !store.count(r.selfTestKey))
    worker.post([=] {                                                              // never the audio thread
        fminus::circuit::SelfTestOptions test;
        test.parallel = myPool;                // runs job(0..count-1) on the host's threads; empty: the calling one
        test.cancelled = [&] { return stale; };
        const auto answer = fminus::circuit::selfTest(netlist, "pedal.cir", {}, test);
        if (answer.choice)
            store[answer.key] = answer.choice->text();                             // in memory, in a saved session
    });
// later, or on the next compile:
fminus::circuit::Options options;
if (auto it = store.find(r.selfTestKey); it != store.end())
    options.chosen = fminus::circuit::Choice::parse(it->second);                  // nothing (and why) if malformed
r = fminus::circuit::generate(netlist, "pedal.cir", options);                      // the self-test's configuration
```

`fminus pedal.cir --self-test` does the same on the command line (on `--threads` threads, 0 for one a core), prints
each candidate it tried and a line to paste into the netlist (`pedal.cir: note: the self-test chose .oversample 4,
.adaa on, .solver omega(1)`), and generates with the answer; `--notes` prints `Result::notes`. FMinus's own messages on
a generated module (`pedal.cir.fm:120:5: error: ...`, the lines `--emit fm` prints) are re-cited at the netlist's line
the generated line comes from: `pedal.cir:9:1: error: ... (in the module generated from pedal.cir, line 120)`.

## The netlist

A hand-written line scanner reads it (`src/circuit/Netlist.cpp`): SPICE is line-oriented, with continuations,
case-folding and unit suffixes, which Flex and Bison fit poorly.

- **Lines.** A line whose first non-blank character is `*` is a comment, and `;` starts one anywhere. A line starting
  with `+` continues the one before; every token keeps its own line and column. `.end` stops reading. There is no
  implicit title line. Element kinds, directives, models, nodes, parts and pots compare without case and are spelled as
  first written. `0` and `gnd` are ground.
- **Numbers.** `[+-]digits[.digits][e[+-]digits][suffix][letters]`: suffixes `t g meg k m u n p f` (`meg` before `m`),
  trailing letters ignored (`10nF`, `2.2kohm`, `-60dB`). `1M` is milli, as in SPICE: an R or P value under 1 ohm written
  with `m` or `M` warns `'1M' is 1 milliohm in SPICE; write 1meg for a megohm`. A part's value lies in 1e-30 to 1e30
  (ohms or farads), which float coefficients hold; should the values still fold to a coefficient beyond float's range,
  that is an error of the netlist ("the netlist's values give a coefficient of 1e+45, beyond float's range").
- **Expressions** `{...}` over the pots: numbers, pot names, `+ - * /`, unary minus, `( )`, and `^` with an integer
  exponent 0 to 8 (`-x^2` is `-(x^2)`; `^` prints as repeated products). They evaluate in double and print as FMinus
  over the clamped pots. One that names no pot is a constant. A part without a pot must be a finite float (`{1/0 +
  tone}` is an error), and an expression has at most 64 levels of nesting and 256 operators: every walk of its tree
  recurses, and `generate()` may run on a thread with a small stack.

| Element | Meaning |
|---|---|
| `R<name> n+ n- value` | a resistor (positive); `R<name> n+ n- {expr}` one whose resistance is the expression of pots |
| `C<name> n+ n- value` | a capacitor: a trapezoidal companion, `gc = 2 C fs'`, its history injected into n+ and drawn from n- |
| `D<name> anode cathode model` | a diode: it must pair with an antiparallel twin of the same model, the pair one port, `i = 2 IS sinh(v / N Vt)` |
| `P<name> e1 w e2 value {expr} [taper=...]` | a pot: `R(e1-w) = (1 - t) value`, `R(w-e2) = t value` |
| `P<name> a b value {expr} [taper=...]` | a rheostat: `R = t value` |
| `X<name> inp inn out opamp` | an ideal op amp (a nullor): `V(inp) = V(inn)`, any output current, no input current |
| `G<name> n+ n- OTA nc+ nc- gain` | an OTA (report §7.1's macro): `i = g tanh((V(nc+) - V(nc-)) / 2 Vt)`, leaving n+ and entering n- (SPICE's G direction), 2 Vt = 2 k 300.15 / q = 51.73 mV; `g`, amps, a positive number or a `{expr}` of pots and law inputs, not negative anywhere on their grid |

`t = taper(clamp01(expr))`: `lin` x; `audio` (81^x - 1) / 80, emitted as `(expf(4.394449f * x) - 1) * 0.0125f`; `rlog`
1 - audio(1 - x). The taper is the P line's `taper=`, else the `.pot`'s word when `{expr}` is that pot alone, else lin
(a warning when both are given and differ). L, V, I, E, F, H, Q, J, M, S, K, T and B elements, a G that is not an OTA,
and `opamp(...)` parameters, are refused: "the inductor L1 is not supported yet (Phase 1)". A law input may appear only
in an OTA's gain: "law input 'cv' in R1's value: a law input enters an OTA's gain only (pot-as-port is Phase 2)".

| Directive | Meaning |
|---|---|
| `.title NAME` | the module's name (default: the file's stem, its first letter capitalised) |
| `.input NAME node [kind=voltage] [scale=x]` | exactly one, the signal: a voltage source from node to ground; sample 1.0 is `scale` volts; tick's first argument |
| `.input NAME - kind=cv\|current [scale=x] [range=lo:hi]` | a law input: no node (`-`); tick's and step's float after the signal, in `.input` order; its value is the sample times `scale`, clamped to `range` (`fmaxf` first, so a NaN is the low end) where it is used, and held over a host sample's oversampled steps; `current` (amps) needs `range=`, `cv` is 0:1 unless it says; an unused one warns |
| `.output NAME node [scale=x]` | exactly one: tick returns V(node) / scale; the last block must compute it |
| `.pot NAME [default] [lin\|audio\|rlog]` | `set()`'s arguments, in this order (default 0.5); an unused pot warns |
| `.model NAME D(IS=x N=x ...)` | a diode model: IS (1e-14) and N (1); RS other than 0 is an error (Phase 1); CJO, TT, BV, IBV, M, VJ, EG, XTI, FC, KF and AF warn "ignored" |
| `.oversample 1\|2\|4\|8\|auto [iir\|fir] [blocks=auto\|nonlinear\|all]` | the oversampling factor, the halfbands, and which blocks run oversampled; `auto`, or no `.oversample`, leaves the factor to the self-test |
| `.adaa on\|off\|auto` | Holters' first-order ADAA on the port; `auto` or none: the self-test's |
| `.solver omega\|omega(K)\|newton(K)\|auto` | the port's solver: the omega3 guess or the previous step's solution, then K Newton steps; `auto`, none, or bare `omega` (an exp-affine port's candidates are all omega's): the self-test's. An OTA's is `newton(K)`: omega3 is the diode's closed form, and is refused for it |
| `.channels 1\|2` | 2: tick and step take and return `float2`s |
| `.method tr\|auto` | the integrator; `auto`, or none, is TR (BDF2, for a block that rings, is a later slice of Phase 1) |
| `.prewarp on\|off` | off (the default, the user's ruling): an OTA's cutoff law is the circuit's; on: prewarped ([below](#prewarp)) |
| `.target [solver=xdB] [alias=xdB] [level=xV] [freq=xHz]` | echoed in the header; `level`, a positive voltage, is the self-test's stress level L (else `.input`'s scale); `examples/circuits` checks against it |

`.op` warns that it is ignored (Phase 1: the DC operating point); SPICE's simulator directives (`.tran`, `.ac`,
`.print`, ...) are ignored with a note; `.subckt`, `.param`, `.include`, `.lib`, `.options`, `.temp`, `.ic`, `.nodeset`
and `.func` are errors, as is any other directive.

**Checks**, with the report's wording: a node with one connection; a node that reaches ground only through capacitors
("its DC voltage is undefined (add 10meg to ground)"); an unknown model (with the list); a diode without an
antiparallel partner, or a pair of different models; an op amp without negative feedback, or whose output is driven
twice or is `.input`'s node; two nonlinear ports in a block ("multi-port Newton is Phase 2"); a feedback loop that
crosses blocks (Phase 2); a block that
reads two inputs or none, or that is off the chain from `.input` to `.output`; a singular block ("its equations are
singular at drive=0, 44.1 kHz: an op amp without negative feedback, or a node cut off from the rest"); a port whose
self-impedance K is not positive somewhere on that grid of rates and pots ("port D1|D2 sees a negative resistance
(K = -123.2 ohm at 22.05 kHz): positive feedback around X1 is not supported yet"); an OTA with no feedback around it
("OTA G1 has no feedback around it (an open-loop OTA, a VCA): Phase 2") or with positive feedback ("OTA G1 sees
positive feedback (kd = -24.86 ohm at 22.05 kHz): its output drives its + input; not supported yet"); diode and OTA
ports in one region ("one solver serves a region (per-family solvers are Phase 2)"); an OTA's gain negative somewhere
on its grid; pot, input and law input names that are FMinus
keywords, builtins or names the module uses (`set`, `tick`, `b2`, `p`, `v`, `ua`, `os1`, ...), and a module named by any
word its text uses (`step`, `rate`, `t3`, `x`, ...: FMinus reads a module's name as a type wherever it appears). Names
compare with their case, as FMinus's do: `.title Tone` with `.pot tone` is fine.

## Ports and laws

A block holds at most one nonlinear port (two are Phase 2's multi-port Newton). Reduced, its equation is
`v + k phi(v) = p` (report §6.1), `p` linear in the block's states and input, `k > 0` everywhere on the block's grid of
rates and pots (`checkPort`):

| Law | Family | Port | phi | k | Solve | ADAA's antiderivative |
|---|---|---|---|---|---|---|
| a diode pair | exp-affine (EA) | the v-form: its voltage `v` the source, its current `j` an unknown | `sinh(v / N Vt)` | `2 Is K` | omega3, or the previous v, then K Newton steps | `F = p v - v^2 / 2 - k N Vt cosh(v / N Vt)` |
| an OTA | smooth monotone (SM) | the i-form: its current `i` the source, its control voltage `v = p - kd i` a readout | `tanh(v / 2 Vt)` | `kd g`, `kd = -dv/di` | the previous v, then K Newton steps (their derivative is at least 1) | `F = p v - v^2 / 2 - k 2 Vt lc(v / 2 Vt)`, `lc = log cosh` |

Every Newton step stays in the bracket `[min(0, p), max(0, p)]`, where the root is (`h(v) = v + k phi(v)` rises
through 0), so every iterate is finite for any `p`. The single diode and the asymmetric pair (EA) are the next slice's.

- **The i-form.** An OTA's port is its current: the block's readouts (its output, its capacitors' voltages) are linear
  in its states and `i`, which the law gives at the solved `v`, `g tanh(v / 2 Vt)`. In the v-form (`v` the source) an
  OTA one-pole's output is `u - v / att`, a small difference of two volts-sized numbers (`att = 220 / 100220`), and the
  loop's low gain at a small bias current amplifies its rounding: the self-test measured T1 -56 dB on otalp.cir at 10
  uA; the i-form, `(x + i) / G`, -97 dB. `kd`, the OTA's loop, is `-dv/di`; an open-loop OTA (`kd = 0`) is refused.
- **ADAA** (Holters 2020) means the port's source over each step: a diode pair's `v`, an OTA's `i`. An OTA's mean
  current is `(p - G(p)) / kd`'s mean over `[p_P, p]`, written as the midpoint's, `wm / kd` with `wm = kg (th + t_P) /
  2` from the law (not from `p - v`, a small difference), plus a correction, `(kgv dl - wm (v - v_P)) / dp`. `dl = log
  cosh x - log cosh x_P` takes one of two forms by the step:
  - below `|x - x_P| = 1`, `log1p(z)`, `z = cosh x / cosh x_P - 1 = 2 sinh(h) (t_P cosh(h) + sinh(h))` with `h = (x -
    x_P) / 2` from the stored `v`s' own difference, `sinh(h)` a polynomial, and log1p in Kahan's form, `log(1 + z) z /
    ((1 + z) - 1)` (FMinus has no `log1pf`). Two rounded `log cosh` (or `tanh`) values differ by their rounding where
    `x` is near `x_P`, which ruined the mean (T1 -26 dB); this form gives -94 to -136 dB, as the plain solve does;
  - from 1 on, the stored values' difference, `lc - lc_P`, with `lc = |x| - log(1 + |tanh x|)`, log cosh in a form
    that cannot overflow (a `logf` a step, where the polynomial saves the `expf` a sinh took). There `z` alone
    cancels: once `t_P` has rounded to +-1 (`|x_P| > 9`, 0.47 V of control voltage), a step back toward 0 of `|h| > 4`
    gave `1 + z <= 0` and a NaN, or `dl = 0` for -39: an OTA driven that hard played NaNs and the guard's resets with
    ADAA (a review found it; otalp.cir's stages, at 25 mV, never get there). A step that long has no cancellation to
    fear.

  Over `x_P` and `x` in `[-40, 40]` the emitted tail's mean is within 3e-6 kg of the double one
  (`CircuitEmitTest.cpp`). Below `|dp| = 1e-5 V` the mean is the midpoint's, as Phase 0's. Every ADAA block is designed
  at 2/3 of its rate.
- **The carry.** ADAA's mean pairs the stored point (`v_P`, `p_P`) with this step's on one curve, `p = v + k phi(v)`.
  When `k` moves (a pot moving a diode pair's K, an OTA's law input or a pot moving `kd g`), the point moves onto the
  new curve with `v_P` held, keeping its Newton residual (the lead's ruling): a diode pair's in `b<N>_set` (Phase 0's
  move), an OTA's at the start of each step, `p_P += (kg - kgo_P) t_P`, which covers a pot's move of `kd` and a law
  input's of `g` alike (`kgo_P` is the last step's `kd g`, `t_P` its `tanh(v_P / 2 Vt)`).

## The module

```c
module DiodeTone                     // .title
{
    void init(float rate)            // the host's rate; every state at 0 V (no bias in Phase 0); the pots' defaults
    void set(float tone)             // the pots, 0 to 1, in .pot order (clamped): on the absolute 16-sample grid
    float tick(float u)              // one host sample: .input's volts to .output's (then the law inputs, if any:
                                     // float tick(float u, float iabc))
    float step(float u)              // one step of the oversampled region (absent if there is none)
    void clear()                     // back to 0 V
    void b1_set(float tone)          // block 1's coefficients, from its pots
    float b1(float u)                // one step of block 1 at its rate
    void hb1_up(float x, float* restrict o)   float hb1_down(float v0, float v1)   // the halfbands
};
```

The header's comment says where each choice of the configuration came from (`configuration: .oversample 4, .adaa on,
.solver omega(1) (the self-test 087ad67714f6249a); iir blocks=auto, .method tr (default)`), as `Result::configuration`
does.

- **Names.** States `x_<C>` (C's trapezoidal state); with ADAA `xo_<C>`, `uo_<input>`, `p_<D>` and `c_<D>`; the port's
  last voltage `v_<D>` (always stored: the previous-solution guess, ADAA's v(n-1), what `trace` reads); rates `gc_<C>` =
  2 C fs' and `g2_<C>` = 2 gc; coefficients `<readout>_by_<source>` (`p_D1_by_x_C1`, `n_b_by_v_D1`, `vc_C3_by_n_p1`) and
  `ka_`, `kb_`, `lna_`, `kc_<D>` (literals instead when K is a constant: a port block with no capacitor and no pot); an
  OTA's `kd_<G>` (its loop), `g_<G>` (its gain, when it moves or is prewarped: each host sample's, set's or init's),
  `<readout>_by_i_<G>` (its current's), with ADAA `t_<G>` and `kgo_<G>`, with `.prewarp on` `pws_<G>` and `pwk_<G>` (2
  fs' and its inverse) and the method `pw<N>_`, and `pv_<pot>` for a pot a law input's gain reads (tick computes it);
  tick's and step's law inputs, clamped, `l_<name>`;
  halfband state `hb<s>u_*` and `hb<s>d_*`; the region's samples `os1`, `os2_0`, ...; the guard's `ck_`, `fd_` and
  `fdstep_`, and tick's locals `sum_` and `gain_` ([the guard](#the-guard)). Characters outside
  `[A-Za-z0-9_]` in part and node names become `_`, and a name taken already gets `_2` (a pot's sections `Ptone_a`
  and `Ptone_b` included).
- **Layout.** It depends on the netlist and the options only, never on a pot's value or `.pot` default, so a frozen copy
  of a program keeps its layout.
- **Rates.** Region blocks run at oversample x the rate, the others at the host's; an ADAA port block is designed at
  2/3 of its rate (Holters 2020). Rate factors (`gc_`, `g2_`) are init's; a block's coefficients are its
  `b<N>_set`'s, or init's when it has no pots.
- **The region** is one run of the block chain, through one up and one down halfband chain: `blocks=nonlinear` the
  first to last nonlinear block, `all` every block, `auto` (the default) those widened to every linear block that fails
  the 1x check: its trapezoidal response `H(j 2 fs tan(pi f / fs))` against the analog `H(j 2 pi f)`, at 44.1 kHz, over
  20 Hz-20 kHz in 1.01x steps at each point of its pot grid, counting only within 40 dB of the maximum, fails above
  0.5 dB (T3's tolerance). The Tube Screamer's tone block (13.4 dB off at 44.1 kHz, 0.38 dB at 4x) joins the region;
  its two 15.9 Hz and 1.6 Hz high-passes stay at 1x. A linear-only netlist runs at 1x, with a warning naming its error.
- **Latency** of the IIR halfbands' round trip at 1 kHz: 2.7, 3.5 and 3.8 host samples at 2x, 4x and 8x (the phase is
  not linear); of the FIR's, 40, 47 and 49.5; ADAA adds 0.5 / oversample for each port block it runs in. The header
  prints it. (The report's 3.2 / 4.3
  / 4.7 for the IIR count each decimator's group delay without the half high-rate sample its polyphase structure
  advances by: `tests/unit/CircuitTest.cpp` measures both.)
- **Determinism.** Straight-line float code with fixed step counts: the output never depends on how a host splits its
  blocks, and `set()` belongs on the absolute 16-sample grid (`example-circuits --check`, h).
- **Pots under ADAA.** ADAA's closed-form mean pairs the last step's point (`v_D1`, `p_D1`) with this step's on one
  curve, `p = v + 2 ka sinh(v / N Vt)`. A pot that moves K moves the curve, so `b<N>_set` moves `p_D1` onto the new one
  with `v_D1` held (its Newton residual kept); without it the mean spikes when the signal's move happens to cancel the
  curve's (on the TS's drive glides, about one glide in 60 to 200 clicks by 0.1 to 0.4 V, against an error of 0.07 V
  with the pot held). A held pot gives the same `ka`, and changes nothing.

### DC

A Phase 0 circuit has no bias: every state starts and clears to 0 V. The DC output is where the solver's fixed point
lands, which is not exactly the circuit's DC point:

| Example 1, tone 1, DC in | plain omega3 + 2 steps at 4x (as shipped) | ADAA, omega3 + 1 step at 4x | exact |
|---|---|---|---|
| 0.1 V | 0.0999503 | 0.0999503 | 0.0999503 |
| 1 V | 0.5155959 | 0.5156211 | 0.5155961 |
| 5 V | 0.6154355 | 0.61540 to 0.61566 (does not settle) | 0.6154227 |

With ADAA and one Newton step, the mean of ADAA switches to the midpoint ½ (v(n) + v(n-1)) when |p(n) - p(n-1)| falls
under 1e-5 V. The midpoint carries the step's first-order error (about 1 mV at 5 V), while the closed form is exact to
second order. At high DC levels the two disagree by more than the switch can settle, and the output wanders by about
0.2 mV around a mean near the exact root; float and double wander apart. A Newton step more (or no ADAA) removes it.
`example-circuits --check` (b) shows both, and `tests/lit/circuit/run.test` pins the shipped values.

### The guard

Every module carries a run-time guard (report §6.4). `ck_` counts ticks from `init` and `clear`, mod 16; when it is 0,
`tick` sums every field that carries from one sample to the next (the capacitors' states, the ports' last voltages,
ADAA's last points and inputs, an OTA's stored tanh and kd g, the halfbands' delay lines), and when the sum is not finite (`!(sum_ - sum_ == 0)`: a NaN
or an infinity anywhere in it), it calls `clear()` and sets the fade `fd_` to 0. The output is `gain_ < 1 ? y * gain_ :
y`, with `gain_` the fade before it rises by `fdstep_ = 1 / (0.02 rate)` a tick, to exactly 1 after 960 ticks at 48 kHz.
Either lane of a `float2` resets both. Settled, the gain is 1 and the output is the circuit's, bit for bit (Phase 0's
equivalence tests stay exact); the check is on the tick count, so how a host splits its blocks never changes it. It
costs a sum every 16 ticks, the halfbands' arrays in loops: about 7 cycles a host sample on the shipped modules. The
solver's bracket stays (it keeps `v` finite: a NaN written into `x_C1` leaves diodetone's output finite until the guard
resets it), and the playground's ±4 output guard is the last line (`example-circuits --check`, k).

### Result

| Field | What |
|---|---|
| `source` | the module's FMinus text; empty when there are errors |
| `errors`, `warnings`, `notes` | `pedal.cir:7:1: error: ...` lines, as clang words them; the notes are the plan (each block, its rate and solver, the latency) and the provisional note |
| `module`, `pots` | the module's name; `set()`'s float arguments, in `.pot` order |
| `inputs` | tick's and step's arguments: the signal (`Input::Kind::Voltage`, its name and scale), then the law inputs (`Cv` or `Current`, their scales and ranges), in `.input` order |
| `switches` | `set()`'s int arguments after the pots (later in Phase 1) |
| `configuration` | one line: each choice, grouped by where it came from (the netlist, Options, the self-test and its key, provisional, default) |
| `provisional` | a dimension is open and no `Options::chosen` for this netlist answered it |
| `selfTestKey` | the key the self-test's answer goes by, when a dimension is open; empty when the netlist names its whole configuration |
| `latency` | host samples of delay at 1 kHz: the halfbands' round trip, and ADAA's half step for each port block it runs in |
| `lines` | for each line of `source`, the netlist's line it comes from (0: none): the module's frame at `.title`, `set` at the first `.pot`, a block's `b<N>_set` and `b<N>` at its first part, `tick` and `step` at `.input`, a part's fields at its line. `fminus` re-cites FMinus's messages with it |

## Configuration and the self-test

The configuration has six dimensions: the oversampling factor, the halfbands (IIR or FIR), the region (`blocks=`), ADAA,
the solver and the integrator (`.method`), with `.channels` beside them. Four are searched: oversampling, ADAA, the
solver and the integrator. Each choice comes, in this order, from:

1. `Options` (a member that is set);
2. the netlist's directive;
3. the self-test's answer, `Options::chosen`, for a searched dimension that neither names (or the netlist writes
   `auto`): an **open** dimension. The answer counts only when its key is the netlist's; otherwise it is ignored with
   the note `Options::chosen is another netlist's or other Options' (key ...; this one's is ...): it is ignored`;
4. the provisional configuration, for an open dimension nothing answered: `Result::provisional` is then set, with one
   note, `pedal.cir: note: the self-test chooses this circuit's configuration; provisional: 8x IIR, omega(2)`;
5. the default, for the rest: IIR halfbands, `blocks=auto`, TR, one channel.

A netlist without a port has nothing to choose: its dimensions are the defaults, never open. The integrator is never
open yet either: its only candidate is TR until BDF2 comes (for a block that rings, a later slice of Phase 1). The
header and `Result::configuration` say where each choice came from: the netlist, Options, the self-test and its key,
provisional, or default.

**The provisional configuration** is Phase 0's measured default for a 1-port exp-affine block: **8x, IIR halfbands, no
ADAA, omega3 + 2 Newton steps** (omega3 + 1 when the netlist pins `.adaa on`). The design had set ADAA at 4x with
omega3 + 1 step (the report's §5.2 measured it best on the TS). On report §2's full stress set that fails T3 on
example 1: H3 droops 0.57 dB at 0.3 V, 3 kHz, tone 0 (the converged double ADAA reference shows the same; the report
measured three cases, -0.24 dB at worst). Plain omega3 + 2 at 4x passes example 1 but fails T2 on the TS (-53.3 dB, the
report's -53.6). 8x plain omega3 + 2 meets T1-T4 on both blocks' stress sets (`example-circuits --check`, c and e), at
about twice the 4x cost: costly, but safe to play while the self-test runs. A smooth-monotone port's (an OTA's) is
**2x, IIR halfbands, no ADAA, the previous step's v + 2 Newton steps**, report §6.2's SM row's middle: otalp.cir's
measures T1 -92.3, T2 -75.7, T3 0.53 dB there (T3 misses by 0.03 dB: the warp of H3 at 9 kHz).

**The key** a self-test's answer goes by (`selfTestKey()`, `Result::selfTestKey`) is 16 hex digits of FNV-1a 64 over
`kSelfTestVersion`, the netlist's lines as SPICE reads them (continuations joined, comments and blank lines dropped,
tokens lowercased and single-spaced, the spaces inside `{...}` and around `=` removed, a `.model` card read as the
parser reads it, its parentheses and commas separators, and written as `d(is=2.52n n=1.752)`; `.title`, `.channels`,
`.op`, `.end` and simulator directives left out) and the `Options` that change the answer (all but `moduleName`,
`channels` and `chosen`). A comment, spacing, case or a new title keeps it; a value, a directive, a model's parameter,
a pot's default or `.target` changes it, and so does any change of the gates, the candidates or the emitted
arithmetic, which bumps `kSelfTestVersion` (1 until Phase 1's branch merges).

**The stress matrix** (report §2; `src/circuit/Search.h`) is a unit for each point and signal:

- points: the axes are the pots the region and the blocks before it read, each at 0, 0.5 and 1, and the law inputs,
  each at its low end, its middle (geometric; the arithmetic one when its range starts at 0) and its high end: the full
  grid for three axes or fewer, otherwise each axis alone (the others at their defaults, a law input at its middle),
  then all low and all high;
- signals: sines at L/15, L/4.5, L/2.25, L and 2.2 L on odd bins 9, 85 and 257 of a 4096-sample window at 48 kHz (105,
  996 and 3012 Hz), after 1024 samples of warm-up; a 20 Hz-20 kHz log sweep and band-limited noise at L. L is `.target
  level`, else `.input`'s scale (full scale). With law inputs, a synth's too (report §2): a band-limited saw at L on bin
  9 (its harmonics below 20 kHz; T1, T2 and T4), and for each law input a 996 Hz sine at L with that input moving +-2
  octaves around its middle at 37 Hz (linearly over its range when it starts at 0; T1 and T4), run at the points where
  that input is at its middle. A law input's value is held over a host sample's region steps, in the module and in the
  references alike.

**The gates** (T3 stays within 0.5 dB, the user's ruling):

- **T1**, the solver: the region's emitted step against the same configuration (rate, ADAA, integrator) in double,
  solved to convergence, both decimated to the window, in band, at most -60 dB on every signal but 2.2 L. Both run on
  what the region sees: the signal through the blocks before it (in double at the region's rate), so a booster ahead
  of a clipper drives the clipper at its own level, and the pots before the region reach T1;
- **T2**, the alias: the tick's inharmonic energy below 20 kHz, in excess of the converged 64x chain's, bin by bin, at
  most -60 dB of the tick's energy there, for sines up to L. The excess, not the energy itself: the TS chain's 1.6 Hz
  output coupling (C7 into the level pot) has not settled in the window, its transient fills the inharmonic bins at
  -48.5 dB on every configuration alike, and it is not an alias; the 64x reference has the same transient, and on a
  block without one the excess is the energy (diodetone's -79.0 dB either way);
- **T3**, the response: |dH1| and |dH3| at most 0.5 dB against the converged 64x chain (every block, TR, no ADAA), on
  bins 85 and 257; H3 counts within 100 dB of H1 and at least -120 dB of the input sine. The second bound is this
  slice's: a low-pass far below its cutoff (otalp.cir at 10 uA, its cutoff 83 Hz, a 3 kHz sine) has an H3 160 dB under
  its input, where float's own rounding (some -140 dB of the circuit's largest signals) and the window's transients are
  most of what the bin holds, and two double references disagree by 0.4 to 2.3 dB;
- **T4**: finite on every signal, and every reference converged.

A gate passes only where some unit measured it: a unit whose output and reference are both silent there (a pot at a
muting end) gives that gate nothing, and one silent against the other fails it.

The 64x chain runs once a unit (for the units T2 and T3 read), the converged region once a unit for each oversampling
and ADAA a candidate needs (its Newton steps share it).

**The candidates** (report §6.2): an exp-affine port's (a diode pair's) are 1, 2, 4 and 8x, ADAA off and on, omega3 + 1
or 2 Newton steps; a smooth-monotone port's (an OTA's), 1, 2 and 4x, ADAA off and on, the previous step's v + 1, 2 or 3
Newton steps. A dimension the netlist or the Options pin drops out, and one configuration serves every port block of
the region (so a region's ports are of one family). They run **cheapest first** by the report's §3.2 model, in cycles
a host sample: each region block times its rate (a port block: 10 + 2 a state for p, 65 for omega3's guess or 5 for
the previous step's, 70 a Newton step of a diode pair or 60 of an OTA's tanh, 57 for ADAA's tail on an exp-affine law
or 100 on tanh's, 5 for the state update; a linear block: 5 + 3 a state), each block outside the region once, and 12
for each sample a halfband stage runs at its high rate, up and down together. Candidates within 5 % of the
cheapest of their group go ADAA first, then fewer steps. The **first** to pass T1-T4 wins (the user's ruling: not an
exhaustive search for the cheapest); its notes list every candidate tried, in order, with its model cost and verdict.
On the shipped netlists without their configuration directives:

| Netlist | The self-test's answer | Candidate | Its T1, T2, T3 | Before it |
|---|---|---|---|---|
| `diodetone.cir` | 4x, omega(2) (968 model cycles) | 11 of 16 | -93.1, -79.0, 0.10 dB | 4x omega(1) fails T1 (-54.2 dB); 4x ADAA omega(1) fails T3 (0.57 dB); every 2x fails T2 or T3 |
| `screamer-clip.cir` | 4x, ADAA, omega(1) (916) | 10 of 16 | -78.7, -73.5, 0.14 dB | 4x omega(1) fails T1 (-49.8 dB); 2x ADAA fails T2 (-52.7 dB) |
| `screamer.cir` (the chain) | 4x, ADAA, omega(1) (976) | 10 of 16 | -81.1, -66.6, 0.21 dB | the same |
| `clipper.cir` | 4x, ADAA, omega(1) (908) | 10 of 16 | -80.6, -84.1, 0.44 dB | |
| `otalp.cir` | 4x, newton(1) (728) | 10 of 18 | -89.0, -73.2, 0.13 dB | 1x newton(1) fails T1 at 2 mA (-61.0 dB); every 1x fails T3 (2.3 dB: the trapezoidal rule's warp of H3 at 9 kHz, through two poles), every 2x by 0.03-0.04 dB; ADAA adds its droop |

These are the configurations the report and Phase 0 chose by hand, which the shipped netlists name (`example-circuits
--check`, i):

| Netlist | Configuration | Why |
|---|---|---|
| `diodetone.cir` | 4x, plain omega3 + 2 | the report's example-1 row (912 cycles); passes T1-T4; the self-test's answer |
| `screamer.cir`, `screamer-clip.cir` | 4x, ADAA, omega3 + 1 | the report's choice for the TS (903 / 907 cycles); passes T1-T4; the self-test's answer |
| `clipper.cir` | none: the self-test's | |
| `otalp.cir` | 4x, the previous v + 1 | the self-test's answer, pinned (no hand choice: the report measured an SVF, §7.3) |

**When nothing passes** (report §6.5), the answer is the candidate with the least excess over its worst gate (the
largest of T1 + 60, T2 + 60 and T3 - 0.5) among those under 3,500 model cycles, ties to the higher oversampling, never
one that is not finite somewhere or whose references did not converge. `Choice::met` is then false, and `generate()`
with it warns at the region's first port: `pedal.cir:9:1: warning: block 2 (D1, D2): no configuration meets the targets
(worst: T2 -55.1 dB at 2.2 V, 3012 Hz, drive 1); using the most accurate under the cost cap: 8x, ADAA, omega(2)`.

**How it runs.** `selfTest()` is the one part of the circuit compiler that needs the JIT, so it lives in `fminus_lib`
(`src/api/CircuitSelfTest.cpp`); its arithmetic, the references and the verdicts, stays in `fminus_circuit`
(`Search.cpp`). Each candidate is generated with `Options::chosen` and one channel, compiled on the calling thread
through `fminus::Program` (`Math::Vector`, subnormals flushed, one thread, -O2) with `Runners.h`'s `fm_set`, `fm_steps`
and `fm_ticks` appended, and run on every unit through `SelfTestOptions::parallel`, each in an aligned, zeroed instance
(`init(48000)`, `set`, the run). It is blocking, pure and deterministic: each job writes its own unit's slot and the
slots are reduced in index order, so the answer and the notes are the same, byte for byte, on any number of threads
(`example-circuits --check`, j). `SelfTestOptions::cancelled` is polled between units and every 4096 reference steps;
cancelled, the test returns `cancelled` and no answer. The budget is in steps (the references' and the candidates'),
never wall time, and is checked before each stage: a test that would go over stops there, with no answer and a note
(diodetone.cir's with a budget of 1000: `the self-test stops before the 64x references: it needs 11943936 steps more,
and 0 of its budget of 1000 are spent; no answer`). What a job throws (`std::bad_alloc` on a unit's buffers, say) is
caught on the job's thread and rethrown on the caller's, the lowest unit's first, as an internal error in
`SelfTestResult::errors`: a host's pool needs no try/catch. FMinus owns no threads and no cache for it: a host runs it,
stores `Choice::text()` under the key, and generates again with `Choice::parse()` of what it stored (above).

`Choice::text()` is one line (diodetone.cir's, without its directives: `fminus-circuit-choice 1 key=d603a5e6059d602d
oversample=4 adaa=off solver=omega(2) method=tr met=yes t1=-93.0903786120034 t2=-79.0209349673603
t3=0.10430918903576991`), its figures each the shortest text that reads back as the same double, then ` worst=...`
(the rest of the line) when not met. `Choice::parse()` reads it back, or gives nothing and why:
another version's, a field missing or out of range, or not one at all.

The rulings the implementation follows (the design's R1-R11):

- **R1 Rate and pots at run time.** Each block's equations are eliminated in float in its `b<N>_set` (or init), with
  the pivot order fixed at generation; there is no Woodbury update (Phase 1).
- **R2 One derivation.** `solve<T>` and `derive<T>` (`src/circuit/Mna.h`) run on doubles (the reference's matrices),
  `std::complex` (a linear block's H(s)) and `Val` (the FMinus text), over the same system and pivots.
- **R3 Variable resistors in the impedance form**: a pot section's current is an unknown, with the row
  `V(a) - V(b) - r i = 0`, so its 0-ohm end is exact. No clamp or floor.
- **R4 The port is a voltage source v** (Holters' v-form): structural values come out exact (example 1's C1 voltage
  is v; the TS block's V(n1) = u, V(v2) = u - v). `K = -1 / (dj/dv)` must be positive, and generation checks it at
  every point of the pivots' grid (`checkPort` in `src/circuit/Mna.h`).
- **R5 Defaults:** above (changed from the design's, for the reason above); now the provisional configuration, which the
  self-test replaces.
- **R6 N Vt is physical:** N k 300.15 / q (SPICE's 27 C), 45.315 mV for the 1N914's N = 1.752. The research's 45.3 and
  45.0 mV are N = 1.75140653 and 1.73980781; the tables and the equivalence tests rewrite N so.
- **R7** The shipped TS netlist has the report's §9.1 tone-pot line, `Ptone tn tw t1 20k {tone}`.
- **R8 The region**, above.
- **R9 Literals** are `std::to_chars`'s shortest round trip of a float (locale-free), with `.0` when integral and `f`;
  small integers print as ints (`2 * gc_C1`). Each re-reads to the float it names. ln 81 is a literal.
- **R10** `src/circuit/` is its own library, `fminus_circuit` (`FMinus::Circuit`), with no LLVM and no grammar.
- **R11 Names:** pots and inputs are `[A-Za-z][A-Za-z0-9]*`, not an FMinus keyword or builtin, not each other's or the
  module's, and not a name the module uses; generated names carry `_` (but for `os1`, which is reserved) or are `t<N>`
  temporaries, which skip pot names. The module's name is none of the words the module's text uses.

Two more choices the code made:

- **Pivots.** An exact +-1 entry is preferred (least Markowitz cost) only where it is at least 0.1 of its column at
  every point of the block's grid of rates (22.05-384 kHz x its factors) and pots (threshold pivoting); otherwise the
  entry with the largest worst-case ratio to its column is. The design's "+-1 first" alone pivoted the tone block on a
  KCL entry of 1 beside a pot's 20k, and its float coefficients lost three digits (`CircuitEmitTest.cpp` caught it);
  with the threshold the worst coefficient is within 1e-5 of the double derivation, row-normalised. An order is
  accepted when every pivot is at least 1e-9 of its column on the grid and the double residual is under 1e-9.
- **The code builder** shares identical subexpressions and folds a temporary into the field assignment that uses it
  once, so `b<N>_set` reads like the hand-written modules (`p_D1_by_x_C1 = t16;`, `kb_D1 = 22.067577f * ka_D1;`).

## Prewarp

`.prewarp on` (or `Options::prewarp`; off by default, the user's ruling) moves an OTA one-pole's cutoff law from the
circuit's to the trapezoidal rule's prewarped one, so its -3 dB point is the analog circuit's at every gain. At
generation the block's small-signal pole is fitted as affine in the gain, `pole = a + b g` (rad/s), from the ends of
the gain's range on its grid, and checked at the middle (a residual of 1e-6 at most, else a warning that prewarp is
ignored); a law input's range makes a gain's, and the fit covers the OTA's own 10 MEG leak (otalp.cir's stage: `a =
-1 / (10 MEG 1 nF) = -100`, `b = -att / (2 Vt 1 nF) = -4.2435e7`; at 10 uA the leak is a fifth of the pole, which a
fit proportional to `g` alone would miss). Where `g` is computed (each host sample for a law input, `b<N>_set` for
pots, `init` for a constant), it becomes `g' = (sign(a + b g) 2 fs' tan(w) - a) / b`, `w = |a + b g| / 2 fs'` below
1.5, with `fs'` the block's design rate (2/3 of its rate with ADAA): the method `pw<N>_`, one `tanf` a host sample, off
the recursion's chain. The notes print `a` and `b`. It applies to an OTA in a one-state block whose network has no pot
(the fit is made at generation); elsewhere it warns that it is ignored, as it does in a netlist without an OTA. The
self-test's T1 reference prewarps as the module does; the 64x reference, the circuit's truth, does not.

At 1x, 44.1 and 48 kHz, cutoffs of 1, 4 and 10 kHz, a 10 mV signal (`example-circuits --check`, m): with `.prewarp on`
the -3 dB point is the analog's within 0.0001 %; off, the trapezoidal rule's warp, -0.14 to -0.17 % at 1 kHz, -2.2 to
-2.6 % at 4 kHz and -11.5 to -13.1 % at 10 kHz.

## The synth path: what the code decided

Where the design left a choice, or the code (the self-test, measuring the JIT-compiled module) proved it wrong:

- **The OTA is the i-form**, as the design had it, and the measurement says why: a first build in the v-form (the
  OTA's control voltage the source, Phase 0's machinery unchanged) wrote otalp.cir's output as `u - 455.5 v`, and its
  T1 was -56 dB at 10 uA, a float floor that oversampling made no better. Its current comes from the law at the
  solved v (`g tanh(v / 2 Vt)`, one `tanhf` more a step), never from `(p - v) / kd`, the same cancellation again.
- **ADAA's tail for tanh** is the mean of the current, the midpoint's plus a correction, with `log cosh`'s difference
  computed from `v - v_P` itself (above). The design's `lc(v / 2 Vt) - lc_P`, stored and differenced, measured T1 -26
  dB; a tanh from `(1 - e^-2|x|) / (1 + e^-2|x|)` cancels where |x| is small too (tanhf's polynomial does not). The
  step stores `t_P`, `kgo_P` (`kd g`, not `g_P`) and `lc_P`, which only a step of `|x - x_P| >= 1` reads.
- **One carry a step**, on `kd g`: it covers the law input's move and a pot's move of `kd` (the design's two carries,
  one in `b<N>_set` for `kd`, one a step for `g`, sum to it), and keeps the residual exactly as the lead ruled.
- **The reference runs an OTA block in its i-form.** With the carry, the i-form's mean pairs the moved point with the
  midpoint `(p + p_P) / 2`, the v-form's with the averaged states; they differ by `(g - g_P) t_P / 2` a step while the
  curve moves, which the modulated unit measured at -51.6 dB (T1). The i-form's is the consistent one: the point moved
  onto this step's curve.
- **T3's H3 counts at -120 dB of the input sine or more** (and within 100 dB of H1, Phase 0's rule): below, an
  OTA low-pass's H3 is float's and the window's, not a response.
- **Prewarp** needs a one-state block with no pot in its network (its fit is made at generation), and fits the pole
  as `a + b g` (the design's), not proportional to `g`.
- **Law inputs** are clamped in `step` (each call) and in `tick` (once a host sample), and `tick` then calls the
  region's blocks itself, so the gains are computed once a host sample. The netlist keeps its signal, `input`, and
  lists its law inputs apart, `laws`; `Result::inputs` is the one vector, the signal first. An unused law input warns,
  as an unused pot does; a law input and a pot of one name (they meet in expressions, which fold case) are refused.

## How it computes

| Stage | File | What |
|---|---|---|
| parse | `src/circuit/Netlist.*` | the line scanner, numbers, expressions; `Messages` (clang's format) |
| plan | `src/circuit/Plan.*` | diode pairs and OTAs into ports, pots into variable branches, law inputs, the checks, the partition at op amps (a union-find over the nodes no source drives; each op amp in its inverting input's network; an OTA's four nodes in one block), the chain, the configuration, the region and the 1x check, the prewarp's fit |
| equations | `src/circuit/Mna.*` | each block's stamps (KCL rows, nullor rows, variable-branch rows, a diode pair's port row; sources: the input, the states, a diode pair's v or an OTA's i), the pivots, `solve<T>`, `derive<T>`, `holters()` (the v-form, and an OTA block's i-form too), `analog()` |
| laws | `src/circuit/Laws.*` | each law's text (the guess, the Newton steps in the bracket, ADAA's mean, what a step stores) and its double twin (phi, phi', the antiderivative, the steps, the converged solve) |
| text | `src/circuit/Emit.*` | `Val` and the code builder, then the module: fields, init, clear, set, `b<N>_set`, `b<N>`, the halfbands, step, tick (with the guard); the map from each line to the netlist's |
| halfbands | `src/circuit/Halfbands.*` | the S1-S3 FIR and IIR coefficients of `experiments/hb-coefs.txt`, their responses, their FMinus |
| generate | `src/circuit/Generate.cpp` | `generate()`: parse, plan, each block's derivation, emit; errors stop it before emission; `selfTestKey()` |
| search | `src/circuit/Search.*` | the self-test's arithmetic: the stress matrix, the 64x and T1 references, a unit's gates and their reduction, the candidates in the cost model's order, the ordered search, the no-configuration answer, the budget; `Choice`'s text |
| runners | `src/circuit/Runners.*` | `fm_set`, `fm_steps` and `fm_ticks`: FMinus that drives a module over buffers, a call a run |
| self-test | `src/api/CircuitSelfTest.cpp` (in `fminus_lib`) | `selfTest()`: each candidate generated, compiled through `fminus::Program` and run on the host's threads |
| reference | `src/circuit/Reference.*` | a block in Holters' form in double, converged (a diode pair's from the closed-form guess, an OTA's by bracketed Newton) or with the emitted steps, plain or ADAA (an OTA's in its i-form, with the carry); a chain, with a stream of gains |
| analysis | `src/circuit/Analysis.*` | the FFT, the windowed-sinc decimator, in-band error, inharmonic energy (and its excess over a reference's), H1 and H3, the stress signals (sines, the saw, the sweep, noise) |

The invariant: every emitted coefficient is `derive<Val>` of the derivation the reference runs as `derive<double>`.

## The checks, and the report's tables

`example-circuits --check` (ctest's `example.circuits`) runs the generated modules through the JIT, a call a run, and
prints each check's worst value, where it fell, and the report's figure beside it:

- **a.** every shipped netlist generates with no error or warning, and its methods are nonblocking;
- **b.** DC, as above;
- **c.** report §2's targets on both 1-port blocks, as shipped and on the provisional configuration (Phase 0's
  defaults), over the full stress set (sines at L/15, L/4.5, L/2.25, L and 2.2 L on odd bins at 105, 996 and 3012 Hz, a
  sweep and noise at L, each pot at 0, 0.5 and 1): **T1** the emitted step against the converged double of the same
  configuration, in band, at most -60 dB; **T2** the emitted tick's inharmonic energy at most -60 dB for sines up to L;
  **T3** H1 and H3 within 0.5 dB of a converged 64x reference; **T4** finite on every signal; the references converge
  on every sample;
- **d.** the TS chain: T3 against a converged 64x chain at drive x tone {0, 0.5, 1}, and tone 1 is 16.4 dB brighter at
  5 kHz than tone 0 (report §9.1's orientation);
- **e.** the report's decision: example 1 plain omega(2) at 4x passes; the TS plain omega(2) at 4x and ADAA omega(1) at
  2x fail T2, and pass at 8x; example 1 with ADAA at 4x fails T3 on the stress set;
- **f.** tick against step through each oversampler (the halfbands' indexing); **g.** `.channels 2`, lane for lane,
  bit for bit; **h.** calls of 64 and 61 samples against one, a pot gliding on the 16-sample grid, bit for bit; and a
  glide under ADAA (the TS block's drive, a triangle): each `set()` keeps the last point's Newton residual (to 1e-5 of
  `p_D1`, where the curve moves up to 1.6e-2), and over 512 ramps the output stays within 1.1x of the held error against
  a plain omega3 + 3 module;
- **i.** the self-test's ordered search on `diodetone.cir` and `screamer-clip.cir` without their configuration
  directives, on the full stress matrix: it chooses their shipped configurations, its notes list each candidate tried in
  the model's order with its verdict, and the answer's T1, T2 and T3 are section c's;
- **j.** the same answer and notes, byte for byte, on the calling thread alone and on 8 threads;
- **k.** the guard: a NaN written into `x_C1` is reset at the next multiple of 16 ticks, the fields then a fresh
  instance's, the fade monotonic to exactly 1 after 960 ticks, calls of 1, 61 and 64 samples alike, and a NaN in one
  lane of a `float2` resetting both;
- **l.** the provisional configuration: 8x IIR, plain omega(2), its note, nonblocking; otalp.cir's, 2x newton(2);
- **m.** the synth path, `otalp.cir`: T1-T4 at its configuration on the synth's stress matrix, the self-test's way
  through the JIT (T1 -89.0, T2 -73.2, T3 0.13 dB); without its directives, the self-test chooses what it ships (4x,
  newton(1), candidate 10 of 18) with the same figures; tick against step through its 4x IIR oversampler; the
  prewarp's -3 dB points ([above](#prewarp)); the carry under ADAA: each step keeps the stored point's Newton residual
  (to 6e-8 of `p_G1`, where a 37 Hz triangle moves the curve by up to 4e-4 of it), and over 512 ramps of a +-2-octave
  37 Hz triangle the ADAA module stays within 1.1x of its departure from a plain newton(3) module with iabc held
  (0.1502 V against 0.1501 V);
- **n.** law inputs: iabc NaN, 0 and 1 A for 200 samples of an 11 V sine give finite output; `.channels 2`'s lanes
  are the scalar module's, bit for bit, the law input broadcast to both; calls of 64 and 61 samples are one call's,
  bit for bit, with a per-sample iabc.

As built, the TS block at 4x with ADAA measures T1 -78.7, T2 -73.5 (converged -73.6; the report -75.1) and T3 0.14 dB;
example 1 as shipped T1 -93.1, T2 -79.0 (the report -79.8), T3 0.10 dB; the decision's failures -53.3 and -52.7 dB (the
report -53.6 and -52.6).

`example-circuits --tables` reprints report §5.2's worst-case table (on `diodetone.cir` and `screamer-clip.cir` at
N Vt 45.3 mV) and fminus.md's Tables 1-3 (on `clipper.cir` and `screamer-clip.cir` at drive 1, at 45.0 mV), the solver
columns from emitted code and the converged ones from the reference, each beside the source's numbers. It takes about
a minute and a half on an M5. Acceptance, by eye: §5.2's entries within 1 dB, and solver columns below -80 dB within
3 dB. As built: every alias, H1 and H3 entry is the report's to 0.1 dB; Tables 1-3 within 0.5 dB; the solver columns
within 1.1 dB down to about -95 dB, except example 1's ADAA omega K2 at 1x (-92.3 dB against the report's -83.5, which
is the floor of the report's float emulation: its K2 there is worse than its K1, -89.1). Deeper entries are float's
own floor (-100 to -150 dB), where the emitted code and the experiments' float emulation differ by up to 12 dB in
either direction (§5.2 at 4x and 8x, prev K3-K6; Table 3's omega + 2 at 4x and 8x), all far below the -60 dB gate.

`example-circuits` with no flags first prints `generate()`'s time on each shipped netlist (0.05 to 14 ms: the TS's three
linear blocks' 1x checks are most of it) and the self-test's wall time on 1, 2, 4 and 8 threads (diodetone.cir and
screamer-clip.cir without their directives: 3.3, 1.8, 1.1 and 0.8 s on an M5 at load 3), then the cost table in
`experiments/fmbench.cpp`'s method (subnormals flushed, one
thread, the best of 7 x 10^6 host samples; cycles and instructions from `proc_pid_rusage` on macOS), each row named as
`experiments/mkbench.py` names its bench and printed beside the self-test's cost model and `experiments/timing1.out`'s
measurement; `--all` runs
mkbench's whole matrix, `--only <regex>` some rows, `--math fast` the frozen copy's mode. Acceptance, by eye: within
10 % of timing1.out on an idle M-series Mac. The emitted shapes are mkbench's (the omega3, Newton and ADAA text, field
arrays in tick), in one module with methods, with fewer coefficients. As built (an M5 at load 2): every row within 7 %
of timing1.out, mostly a few percent faster; the TS clipping stage at 4x with ADAA 904 cycles a host sample (the report
903), the TS chain 905-906 whatever the region (the report 905-907: the tone block in the region costs nothing), example
1 as shipped 863 (899), and Phase 0's defaults on the TS stage, 8x plain omega3 + 2, 1,803 (1,865).

The synth path's rows (`--only OTA`) run otalp.cir with iabc from an expo converter each sample (one `expf`, `2^(2
sin)` around 141 uA at 37 Hz), against the model, which leaves out the converter and an OTA's last `tanhf` (its
current at the solved v): at load 4, 1x newton(2) 481 cycles (model 284), 2x newton(2), the provisional, 530 (592),
4x newton(1) as shipped 815 (728), with ADAA 1,108 (1,528), `.channels 2` at 1x 472 for both lanes, and 1x newton(2)
with `.prewarp on` 293. That last row is the unexplained one: the module with prewarp (790 instructions a sample)
runs at an IPC of 2.7, the one without (516) at 1.1, on the same recursion; the IR has not been compared yet. The
self-test on otalp.cir without its directives takes 1.6 s on 4 threads (5.1 s of CPU).

## Not built yet

Refused with "not supported yet" wherever a netlist can ask for them:

- multi-port blocks (an OTA SVF's two ports, ladders, resonance and self-oscillation's stress), BLT and tearing,
  decompositions, feedback across op-amp cuts (Phase 2);
- BJTs, JFETs, triodes (Phase 2-3); biased circuits: V and I sources, the DC operating point and `.op`, the offset
  formulation (Phase 1-2);
- inductors, E/F/H sources and a linear G, switches (Phase 1); open-loop OTAs (VCAs), diode and OTA ports in one
  region (per-family solvers), law inputs outside an OTA's gain (pot-as-port: in R, E or G), interpolated (up-chain)
  law inputs (Phase 2);
- single diodes, series strings, asymmetric or parallel pairs, RS, a model library;
- soft and GBW op amps (an OTA filter's rails), `.subckt`, `.param`, `.include`, `.temp`, `.method bdf2` and the
  ringing check; prewarp for a block of more than one state, or with pots in its network;
- the self-test's BDF2 twins;
- Woodbury pot updates, and pots as ports for audio-rate resistances;
- magnitude-matched linear blocks at 1x, oversampled linear-only netlists, several regions, several inputs or outputs;
- designing halfbands (the coefficients are gen.cpp's, embedded);
- `.channels 4` voice banks; FMinus additions (`#line`, `fmaf`, `expm1f`, `omegaf`, file-scope `const` arrays); a
  `latency()` in the host's contract;
- the playground (`.cir` in its Sources, its include resolver, `Lang.h`, `lint.layers`, a `fminus.circuit` probe, the
  circuits guide part, the self-test run asynchronously with its answers kept); `.cir` highlighting in `editors/vscode`;
  `Circuit.h` on the site's API page; ngspice goldens.
