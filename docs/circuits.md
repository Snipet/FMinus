# Circuits: SPICE netlists to FMinus modules (Phase 1, as built)

`fminus::circuit` turns a SPICE-style netlist (a `.cir` file) into the text of one FMinus module, which compiles like
any other source. It is the analog circuit compiler that [research/analog-circuits/](research/analog-circuits/README.md)
proposes (its §11). Phase 0 built the 1-port generator, both of the report's examples through `fminus --emit fm`, and
the report's accuracy and cost tables reprinted from emitted code. Phase 1 built the rest of the report's first slice
in four parts: the public API, the self-test that chooses a netlist's configuration on the report's stress matrix and
targets (asynchronously for a host, which keeps the answer), the run-time guard every module carries, and FMinus's
messages re-cited at the netlist's lines; the synth path (report §7): the OTA, law inputs that move a device's law
every sample, the tanh law's solve and ADAA with their carry, the cutoff prewarp, and the self-test's smooth-monotone
row; the rest of the netlist's parts and the DC operating point: inductors, DC voltage and current sources, E and
linear G sources, every diode form (one diode, a series string, an asymmetric pair, RS, the built-in 1N914 and 1N4148),
and the module running in offsets from the circuit's DC point (`.op` prints it); and the coefficients' and the
integrator's last pieces: the Woodbury form of a block's pot updates, ramped switches, matched designs for linear
blocks at 1x and oversampled regions for linear netlists, BDF2, and the method check that finds a block ringing at
Nyquist. This page says what it does, how to use it, what it decided where the design left a choice or the code proved
the design wrong, and what is not built yet.

## Status

- **Built:** netlists of resistors, capacitors, inductors, pots, ramped switches, DC voltage and current sources, E
  and G (linear) controlled sources, ideal op amps, diodes (one, a series string, an antiparallel pair, an asymmetric
  pair, with RS; a `.model` card or the built-in 1N914 and 1N4148) and OTAs, cut into blocks at the op amps' and
  grounded E sources' outputs; each block's equations (modified nodal analysis, trapezoidal or BDF2 companions, an
  inductor, a variable resistor and a switch in the impedance form, its one nonlinear port in Holters' form: a diode's
  voltage, an OTA's current) eliminated with a pivot order fixed at generation and printed as straight-line float code,
  in the Woodbury form where it saves a pot's update work (what the rate alone moves computed once in init); the
  closed-form omega3 guess with fixed Newton steps (a diode's), the previous step's v with fixed Newton steps (an
  OTA's); Holters' antiderivative antialiasing (ADAA) for every law; IIR or FIR halfband oversampling of the region
  that needs it; matched designs at 1x for linear blocks of one or two states; `.channels 2`; the DC operating point,
  which the module runs in offsets from (`.op` prints it). The self-test (the stress matrix, the converged references,
  T1-T4, the exp-affine and smooth-monotone candidates in the report's cost-model order, BDF2 twins where a block
  rings, the ordered search, the no-configuration answer), the provisional configuration a netlist plays until it
  answers, the method check (stage 7), the run-time guard, the map from each generated line to the netlist's (`fminus
  --self-test`, `--notes`); law inputs (`.input NAME - kind=cv|current`) in an OTA's gain, each sample's, with ADAA's
  carry; `.prewarp on`. Probes: `Options::probes`, the fields through which a host watches up to four node voltages
  as the module plays, with the same output bits, and `Result::nodes` ([below](#probes); `fminus --probes --nodes`).
- **Checked:** the parser, the plan and the equations against hand derivations, Yeh's eq. 24, report §7.2's OTA stage
  and textbook responses (an RL low-pass and an RLC band-pass among them), the stamps of an inductor, an E and a G, DC
  points by hand, the offset form, the diode forms against their hand laws, each law's antiderivative and bracket, the
  carry's residual (`tests/unit/CircuitTest.cpp`); the emitted coefficients against the double derivation, every
  configuration compiling nonblocking, the OTA's in 40 combinations and Phase 1's elements and diode laws in 40, the
  bracket through the JIT, the carry of a pot's move for every diode law (`tests/unit/CircuitEmitTest.cpp`); the
  self-test's order (both rows), search, gates, key and answers, the synth's stress, T1 on the region's own input, and
  its determinism, cancel, budget and errors (`tests/unit/CircuitSearchTest.cpp`); the text, the driver and the
  equivalence with the research's hand-written modules (`tests/lit/circuit/`); the report's targets on its full stress
  set, the self-test's choices, the guard, the synth path's gates, prewarp, carry, lanes and block splits, the offset
  form against the unbiased module, the new diode forms' self-tests and an inductor's response, through the JIT
  (`example-circuits --check`); the DC point's notes and errors (`tests/lit/circuit/op.test`, `errors.test`); BDF2's
  blocks against the BDF2 map of their analog response, switches' parsing, planning and errors, the matched designs'
  poles and magnitudes and their float run, the linear-only region, the eigenvalues (`CircuitTest.cpp`); the Woodbury
  form's coefficients against the double derivation all in set at 44.1 to 384 kHz, a switch's ramp, block splits and
  ADAA residual, BDF2 against its reference, the matched designs' float coefficients and response
  (`CircuitEmitTest.cpp`); switch positions as stress axes, the method check's flag, measurement, warning and twins
  (`CircuitSearchTest.cpp`); every shipped netlist's coefficients, `switch.cir`, `rc8k.cir`, `sallen-key.cir`,
  `lowpass-sweep.cir`, `tonestack.cir`, BDF2 and `ringing.cir` through the JIT (`example-circuits --check`, p); the
  probes' nodes, their order, DC volts and readability (`CircuitTest.cpp`), their fields' layout, init, clear, the
  rings and the slots, every method nonblocking in one and two channels, an ADAA input only a probe reads
  (`CircuitEmitTest.cpp`), the self-test unchanged by them (`CircuitSearchTest.cpp`), the node list and the text
  (`tests/lit/circuit/probes.test`), and on every shipped netlist the same output bits probed or not, each ring against
  the double reference of its node, the stats and the DC volts (`example-circuits --check`, q).
- **Not built:** Phase 2 and later, at the end of this page.

## Use

```bash
build-dev/bin/fminus examples/circuits/diodetone.cir --emit fm        # the generated module
build-dev/bin/fminus examples/circuits/diodetone.cir                  # compiled, as that text would be (--emit ll)
build-dev/bin/fminus play.fm -I examples/circuits --run dc 1          # a program that does #include "diodetone.cir"
build-dev/bin/fminus examples/circuits/diodetone.cir --probes --nodes --emit fm   # with its probe fields, and its nodes
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

`Result` also holds `warnings`, `notes` (the plan: each block, its rate, its solver or its linear form, the latency),
`module` (its name), `pots` and `switches` (`set()`'s arguments, in order) and the fields [below](#result). `Options`
override the netlist's directives. `generate()` is pure and deterministic (the same text and options give the same
`Result`, byte for byte, on any machine), safe on any thread, and allocates: call it off the audio thread. A host that
serves `#include` itself generates in its resolver, as the playground would:

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

`tests/library/CircuitTest.cpp` does this; `examples/circuits/` has the report's netlists, `otalp.cir`, the synth
path's, Phase 1's elements': `screamer-biased.cir` (a DC reference), `halfwave.cir` (one diode), `asym.cir` (an
asymmetric pair) and `wah.cir` (an inductor), and its linear blocks' and switches': `rc8k.cir` and `sallen-key.cir`
(matched at 1x), `tonestack.cir` (three states: an oversampled linear-only region), `switch.cir` (a clipping switch)
and `ringing.cir` (a block the method check flags).

A netlist with switches (`.switch clip 2 1`, `S1 a d clip 1`) makes `set()` take each switch's position, an `int`,
after the pots, in `.switch` order. A switch's resistance ramps between its `roff` and `ron` over 20 ms of `set()`
calls on the 16-sample grid (60 calls at 48 kHz), so a flip never steps; while one ramps, the host calls `set()` every
grid tick (with the same arguments), as it does while a pot glides:

```c
filter.set(tone, clip);     // .pot tone; .switch clip: 0 (the RC alone) or 1 (the diodes in)
```

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
.adaa on, .solver omega(1)`), its warnings (a block that rings at Nyquist: `ringing.cir:12:1: warning: at 1x block 1
rings at Nyquist (13 dB more above 20 kHz than the 64x reference): .oversample 2 or .method bdf2`; the host shows
`SelfTestResult::warnings`), and generates with the answer; `--notes` prints `Result::notes`. FMinus's own messages on
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
  (ohms, farads or henries; an E's or a G's gain in size, not 0), which float coefficients hold; a source's (volts or
  amps) is finite and at most 1e30 in size, and may be 0 or negative (the lead's ruling: the float range, no tighter
  physical one). Should the values still fold to a coefficient beyond float's range, that is an error of the netlist
  ("the netlist's values give a coefficient of 1e+45, beyond float's range").
- **Expressions** `{...}` over the pots: numbers, pot names, `+ - * /`, unary minus, `( )`, and `^` with an integer
  exponent 0 to 8 (`-x^2` is `-(x^2)`; `^` prints as repeated products). They evaluate in double and print as FMinus
  over the clamped pots. One that names no pot is a constant. A part without a pot must be a finite float (`{1/0 +
  tone}` is an error), and an expression has at most 64 levels of nesting and 256 operators: every walk of its tree
  recurses, and `generate()` may run on a thread with a small stack.

| Element | Meaning |
|---|---|
| `R<name> n+ n- value` | a resistor (positive); `R<name> n+ n- {expr}` one whose resistance is the expression of pots |
| `C<name> n+ n- value` | a capacitor: a trapezoidal companion, `gc = 2 C fs'`, its history injected into n+ and drawn from n- (BDF2's: `gc = 3 C fs' / 2`, [below](#bdf2)) |
| `L<name> n+ n- value` | an inductor, in the impedance form: its current an unknown, the row `V(n+) - V(n-) - rl i = -x`, `rl = 2 L fs'`, its state `x <- 2 rl i - x`; at DC (`rl = 0`) an exact short (BDF2's: `rl = 3 L fs' / 2`) |
| `V<name> n+ n- [DC] value` | a DC voltage source, `V(n+) - V(n-) = value`. With a terminal grounded, its other node is a constant (ground in the offsets the module runs in); otherwise a row of its block with its current an unknown (a short in the offsets). AC, SIN, PULSE and the other transient specs are errors ("'AC 1': a V source is DC: the signal is .input's") |
| `I<name> n+ n- [DC] value` | a DC current source, `value` amps from n+ through it to n- (SPICE's direction): open in the offsets, seen by the DC point alone |
| `E<name> n+ n- nc+ nc- gain\|{expr}` | a VCVS, `V(n+) - V(n-) = gain (V(nc+) - V(nc-))`. With an output terminal grounded, its other is driven: a cut, as an op amp's output, the E in its controls' block (a buffer block of its own when both are driven or ground); otherwise a row of its block, its current an unknown, its four nodes in one block. A gain of pots is the local `k_<E>` of `b<N>_set` |
| `G<name> n+ n- nc+ nc- gain\|{expr}` | a linear VCCS, `gain (V(nc+) - V(nc-))` amps from n+ through it to n-: stamped in its block, which its four nodes join; a control on the block's input is a right-hand side |
| `D<name> anode cathode model` | a diode. Antiparallel ones are a pair, `i = 2 IS sinh(v / N Vt)`; one alone, `i = IS (e^(v / N Vt) - 1)`; a pair of two laws (models, or strings of other lengths), the asymmetric law. Diodes in series through nodes nothing else touches (and not .input's or .output's), one model and one direction, are a string, `D1+D2`: one diode of n N. A model's RS is a resistor `RS_<D>` to a new node `rs_<D>` at the port's anode (one for a pair, whose RS must be equal). The model is a `.model` card's, else a built-in's: `1N914` and `1N4148`, IS 2.52 nA, N 1.752 (Yeh, DAFx-07) |
| `P<name> e1 w e2 value {expr} [taper=...]` | a pot: `R(e1-w) = (1 - t) value`, `R(w-e2) = t value` |
| `P<name> a b value {expr} [taper=...]` | a rheostat: `R = t value` |
| `S<name> a b SWITCH pos[,pos...] [ron=1] [roff=10meg]` | a switch: a resistance that ramps between `roff` (open) and `ron` (closed), closed at the positions listed of the `.switch` SWITCH (and open at the others); in the impedance form, as a pot's section is, `r = e^(ln roff + s (ln ron - ln roff))` with the ramp `s` from 0 to 1 ([below](#the-module)). `ron` and `roff` lie in 1e-30 to 1e30 ohms |
| `X<name> inp inn out opamp` | an ideal op amp (a nullor): `V(inp) = V(inn)`, any output current, no input current |
| `G<name> n+ n- OTA nc+ nc- gain` | an OTA (report §7.1's macro): `i = g tanh((V(nc+) - V(nc-)) / 2 Vt)`, leaving n+ and entering n- (SPICE's G direction), 2 Vt = 2 k 300.15 / q = 51.73 mV; `g`, amps, a positive number or a `{expr}` of pots and law inputs, not negative anywhere on their grid (each at its low end, middle and high end: the full grid for three law inputs or fewer, otherwise each alone and all at either end, as the pots' grid) |

`t = taper(clamp01(expr))`: `lin` x; `audio` (81^x - 1) / 80, emitted as `(expf(4.394449f * x) - 1) * 0.0125f`; `rlog` 1
- audio(1 - x). The taper is the P line's `taper=`, else the `.pot`'s word when `{expr}` is that pot alone, else lin (a
warning when both are given and differ). F, H, Q, J, M, K, T and B elements and `opamp(...)` parameters are refused at
their phase ("the current-controlled source F1 is not supported yet (Phase 2)"). A law input may appear only in an OTA's
gain: "law input 'cv' in R1's value: a law input enters an OTA's gain only (pot-as-port is Phase 2)", an E's or a G's
too.

| Directive | Meaning |
|---|---|
| `.title NAME` | the module's name (default: the file's stem, its first letter capitalised) |
| `.input NAME node [kind=voltage] [scale=x]` | exactly one, the signal: a voltage source from node to ground; sample 1.0 is `scale` volts; tick's first argument |
| `.input NAME - kind=cv\|current [scale=x] [range=lo:hi]` | a law input: no node (`-`); tick's and step's float after the signal, in `.input` order; its value is the sample times `scale`, clamped to `range` (`fmaxf` first, so a NaN is the low end) where it is used, and held over a host sample's oversampled steps; `current` (amps) needs `range=`, `cv` is 0:1 unless it says; an unused one warns |
| `.output NAME node [scale=x]` | exactly one: tick returns V(node) / scale; the last block must compute it |
| `.pot NAME [default] [lin\|audio\|rlog]` | `set()`'s arguments, in this order (default 0.5); an unused pot warns |
| `.switch NAME positions [default]` | a switch of 2 to 8 positions, 0 to positions - 1 (default 0): `set()`'s `int` arguments after the pots, in this order; its S elements are closed at the positions they list. An unused switch warns |
| `.model NAME D(IS=x N=x RS=x ...)` | a diode model: IS (1e-14), N (1) and RS (0: none); CJO, TT, BV, IBV, M, VJ, EG, XTI, FC, KF and AF warn "ignored". A card of a built-in's name replaces it |
| `.oversample 1\|2\|4\|8\|auto [iir\|fir] [blocks=auto\|nonlinear\|all]` | the oversampling factor, the halfbands, and which blocks run oversampled; `auto`, or no `.oversample`, leaves the factor to the self-test |
| `.adaa on\|off\|auto` | Holters' first-order ADAA on the port; `auto` or none: the self-test's |
| `.solver omega\|omega(K)\|newton(K)\|auto` | the port's solver: the omega3 guess or the previous step's solution, then K Newton steps; `auto`, none, or bare `omega` (an exp-affine port's candidates are all omega's): the self-test's. An OTA's is `newton(K)`: omega3 is the diode's closed form, and is refused for it |
| `.channels 1\|2` | 2: tick and step take and return `float2`s |
| `.method tr\|bdf2\|auto` | the integrator: the trapezoidal rule, or BDF2 ([below](#bdf2)), which ADAA (Holters' trapezoidal form) refuses; `auto`, or none while another searched dimension is open: the self-test's (the trapezoidal rule, and BDF2 twins where a block rings); none, the rest pinned: TR. A linear netlist ignores `bdf2`, with a warning: its blocks run the trapezoidal rule or matched designs |
| `.prewarp on\|off` | off (the default, the user's ruling): an OTA's cutoff law is the circuit's; on: prewarped ([below](#prewarp)) |
| `.target [solver=xdB] [alias=xdB] [level=xV] [freq=xHz]` | echoed in the header; `level`, a positive voltage, is the self-test's stress level L (else `.input`'s scale); `examples/circuits` checks against it |
| `.op` | the DC operating point as notes: each node's voltage, each V source's current (SPICE's `i(V1)`, from n+ through the source to n-), each port's voltage ([below](#dc-and-the-offset-form)) |

SPICE's simulator directives (`.tran`, `.ac`,
`.print`, ...) are ignored with a note; `.subckt`, `.param`, `.include`, `.lib`, `.options`, `.temp`, `.ic`, `.nodeset`
and `.func` are errors, as is any other directive.

**Checks**, with the report's wording: a node with one connection; a node that reaches ground only through capacitors
and current sources (I, G and OTA outputs: "its DC voltage is undefined (add 10meg to ground)"); a loop of voltage
sources, E outputs and inductors, `.input`'s node and op amps' outputs counting as sources to ground ("a loop of voltage
sources, E outputs and inductors (V1, L1): its DC current is undefined"; an inductor from `.input`'s node to a V
source's, "(.input, V1, L1)"; from an op amp's output to ground, "(X1, L1)"); an unknown model (with the list, and the
built-ins: "unknown model '1N4148X' (the models: 1N914; built in: 1N914, 1N4148)"); a series string of two models
("diodes D3 and D4 in series through node 'k' have different models (1N914, 1N4148): a series string is of one model");
a pair's unequal RS ("antiparallel D1|D2 have unequal RS (10 and 5 ohm): a pair shares one series resistor"); a diode
whose anode and cathode are one node; an S naming no `.switch` ("unknown switch 'sel' (the switches: mode)", or "(there
are no .switch lines)"), closed at a position its switch lacks ("S2 is closed at position 3 of switch 'mode', whose
positions are 0 to 2"), or whose `ron` or `roff` float cannot hold ("'ron=0': S3's ron is out of range for float
coefficients (1e-30 to 1e30 ohms)"); a `.switch` of fewer than 2 or more than 8 positions ("'1': a switch has 2 to 8
positions"), a default outside them ("switch 'mode' defaults to 5; its positions are 0 to 2"), one defined twice, or
named as a pot or an input is; a switch position that moves the DC point ("switch 'mode' at position 1 moves the DC
operating point: ..."); `.method bdf2` with `.adaa on` ("'.method bdf2' with '.adaa on': ADAA is Holters' trapezoidal
form", or the Options' members it came from); a V source on `.input`'s node ("the V source V1 is across .input's node
'in': the signal is .input's"); a node driven twice (by op amps, grounded E and V sources); an E whose controls are in
two blocks ("E1's controls, 'a' and 'b', are in two blocks: an E reads one block's nodes (or its input)"); an op amp
without negative feedback, or whose output is driven twice or is `.input`'s node; two nonlinear ports in a block
("multi-port Newton is Phase 2"); a feedback loop that crosses blocks (Phase 2); at the DC point, a biased port, a block
whose DC equations DC sources make inconsistent, a pot that moves the DC point ([below](#dc-and-the-offset-form)); a
block that reads two inputs or none, or that is off the chain from `.input` to `.output`; a singular block ("its
equations are singular at drive=0, 44.1 kHz: an op amp without negative feedback, or a node cut off from the rest"); a
port whose self-impedance K is not positive somewhere on that grid of rates and pots ("port D1|D2 sees a negative
resistance (K = -123.2 ohm at 22.05 kHz): positive feedback around X1 is not supported yet"); an OTA with no feedback
around it ("OTA G1 has no feedback around it (an open-loop OTA, a VCA): Phase 2") or with positive feedback ("OTA G1
sees positive feedback (kd = -24.86 ohm at 22.05 kHz): its output drives its + input; not supported yet"); diode and OTA
ports in one region ("one solver serves a region (per-family solvers are Phase 2)"); an OTA's gain negative somewhere on
its grid; pot, switch, input and law input names that are FMinus keywords, builtins or names the module uses (`set`,
`tick`, `b2`, `p`, `v`, `ua`, `os1`, `swstep_`, ...), and a module named by any word its text uses (`step`, `rate`,
`t3`, `x`, ...: FMinus reads a module's name as a type wherever it appears). Names compare with their case, as FMinus's
do: `.title Tone` with `.pot tone` is fine.

## Ports and laws

A block holds at most one nonlinear port (two are Phase 2's multi-port Newton). Reduced, its equation is
`v + k phi(v) = p` (report §6.1), `p` linear in the block's states and input, `k > 0` everywhere on the block's grid of
rates and pots (`checkPort`):

| Law | Family | Port | phi | k | Solve | ADAA's antiderivative |
|---|---|---|---|---|---|---|
| a diode pair | exp-affine (EA) | the v-form: its voltage `v` the source, its current `j` an unknown | `sinh(v / N Vt)` | `2 Is K` | omega3, or the previous v, then K Newton steps | `F = p v - v^2 / 2 - k N Vt cosh(v / N Vt)` |
| one diode | EA | the v-form | `e^(v / N Vt) - 1` | `Is K` | omega3 for p > 0 (exact there: `v + k e^(v / N Vt) = p + k`), `p + k` for p < 0 (the diode off), or the previous v; then K Newton steps | `F = p v - v^2 / 2 - k (N Vt e^(v / N Vt) - v)` |
| an asymmetric pair | EA | the v-form | `Is (e^(v / N Vt) - 1) - Is2 (e^(-v / N2 Vt) - 1)`, over Is | `K` (its `ka = Is K`, `kr = Is2 K`) | omega3 for the side that conducts (p's sign's), or the previous v; then K Newton steps with both exponentials (110 model cycles a step) | the two diodes' terms, `ka (N Vt e^(v / N Vt) - v) + kr (N2 Vt e^(-v / N2 Vt) + v)` |
| an OTA | smooth monotone (SM) | the i-form: its current `i` the source, its control voltage `v = p - kd i` a readout | `tanh(v / 2 Vt)` | `kd g`, `kd = -dv/di` | the previous v, then K Newton steps (their derivative is at least 1) | `F = p v - v^2 / 2 - k 2 Vt lc(v / 2 Vt)`, `lc = log cosh` |

Every Newton step stays in the bracket `[min(0, p), max(0, p)]`, where the root is (`h(v) = v + k phi(v)` rises
through 0), so every iterate is finite for any `p`. A series string is one diode of n N (`D1+D2`), so its law is one
of these: a string of one model each way is a pair, of another length or model an asymmetric pair. With ADAA one
diode's mean is `vm + ((v - v_P) (pm - vm + ka) - kc (c - c_P)) / dp`, `c = e^(v / N Vt)` stored as `c_P` (`1` at
clear); an asymmetric pair's adds the other diode's `- kr` and `- kcr (cr - cr_P)`, `cr = e^(-v / N2 Vt)`.

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
  When `k` moves (a pot or a switch's ramp moving a diode's K, an OTA's law input or a pot moving `kd g`), the point
  moves onto the new curve with `v_P` held, keeping its Newton residual (the lead's ruling): a diode's in `b<N>_set`,
  `p_P += (K' - K) f(v_P)` (a pair's Phase 0 move, `(ka - kprev) (e - 1 / e)` from `v_P`'s exponential computed again;
  one diode's `(ka - kprev) (c_P - 1)` from the stored value; an asymmetric pair's that less `(kr - krprev) (cr_P -
  1)`), an OTA's at the start of each step, `p_P += (kg - kgo_P) t_P`, which covers a pot's move of `kd` and a law
  input's of `g` alike (`kgo_P` is the last step's `kd g`, `t_P` its `tanh(v_P / 2 Vt)`).

## The module

```c
module DiodeTone                     // .title
{
    void init(float rate)            // the host's rate; every state at the DC point (0 in offsets); the pots' defaults
    void set(float tone)             // the pots, 0 to 1, in .pot order (clamped), then the switches' int positions
                                     // (set(float tone, int clip)): on the absolute 16-sample grid
    float tick(float u)              // one host sample: .input's volts to .output's, its DC point added (then the
                                     // law inputs, if any: float tick(float u, float iabc))
    float step(float u)              // one step of the oversampled region (absent if there is none)
    void clear()                     // back to the DC point (0 in offsets)
    void b1_set(float tone)          // block 1's coefficients, from its pots and its switches' ramps
    float b1(float u)                // one step of block 1 at its rate (a matched block's biquad, at the host's)
    void hb1_up(float x, float* restrict o)   float hb1_down(float v0, float v1)   // the halfbands
};
```

The header's comment says where each choice of the configuration came from (`configuration: .oversample 4, .adaa on,
.solver omega(1) (the self-test 087ad67714f6249a); iir blocks=auto, .method tr (default)`), as `Result::configuration`
does.

- **Names.** States `x_<C>` (C's trapezoidal state) and `x_<L>` (L's); with ADAA `xo_<C>` and `xo_<L>`,
  `uo_<input>`, `p_<D>` and `c_<D>` (an asymmetric pair's `cr_<D>` too); with BDF2 `xo_<C>` and `xo_<L>` are the
  state's second part (h / 4 a step earlier; BDF2 never runs with ADAA) and `h_<C>`, `h_<L>` the step's locals; the
  port's last voltage `v_<D>` (always stored: the previous-solution guess, ADAA's v(n-1), what `trace` reads); rates
  `gc_<C>` = 2 C fs', `g2_<C>` = 2 gc, `rl_<L>` = 2 L fs' and `r2_<L>` = 2 rl (BDF2's 3 C fs' / 2, 3 L fs' / 2 and
  4/3 of them); a switch's ramp `sw_<S>` (0 open to 1 closed), the ramp's step `swstep_` = 16 / (0.02 rate) and
  `set`'s target `to_<S>`; the Woodbury form's rate-only coefficients `w<N>_<k>` (init's); a matched block's
  coefficients `mb0_b<N>`, `mb1_b<N>`, `mb2_b<N>`, `ma1_b<N>` and `ma2_b<N>` and its states `s1_b<N>` and
  `s2_b<N>`; coefficients `<readout>_by_<source>` (`p_D1_by_x_C1`, `n_b_by_v_D1`,
  `vc_C3_by_n_p1`, `il_L1_by_x_L1`: an inductor's current) and `ka_`, `kb_`, `lna_`, `kc_<D>` (an asymmetric pair's
  other diode's `kr_`, `ks_`, `lnr_` and `kcr_<D>`; literals instead when K is a constant: a port block with no
  capacitor and no pot); a gain of pots, `b<N>_set`'s local `k_<E>`; an
  OTA's `kd_<G>` (its loop), `g_<G>` (its gain, when it moves or is prewarped: each host sample's, set's or init's),
  `<readout>_by_i_<G>` (its current's), with ADAA `t_<G>`, `lc_<G>` and `kgo_<G>`, with `.prewarp on` `pws_<G>` and
  `pwk_<G>` (2 fs' and its inverse) and the method `pw<N>_`, and `pv_<pot>` for a pot a law input's gain reads (tick
  computes it; one field however many OTAs' gains read the pot);
  tick's and step's law inputs, clamped, `l_<name>`;
  halfband state `hb<s>u_*` and `hb<s>d_*`; the region's samples `os1`, `os2_0`, ...; the guard's `ck_`, `fd_` and
  `fdstep_`, and tick's locals `sum_` and `gain_` ([the guard](#the-guard)); with probes, `kProbeFields`' (`prb_`,
  `prs_`, `prn_`, `pr_`, `prp_`, `pra_`, `prq_`, `prv_`), a node's coefficients `pn_<node>_by_<source>`, ADAA's input
  history `pru_b<N>` where only a probe reads it, and the methods `b<N>_pr`, `step_pr` and `tick_pr`
  ([probes](#probes)). Characters outside
  `[A-Za-z0-9_]` in part and node names become `_`, and a name taken already gets `_2` (a pot's sections `Ptone_a`
  and `Ptone_b` included, and RS's resistor `RS_<D>` and node `rs_<D>`). Every field and local holds an offset from the
  DC operating point, so `trace` reads offsets.
- **Layout.** It depends on the netlist and the options only, never on a pot's value or `.pot` default, so a frozen copy
  of a program keeps its layout.
- **Rates.** Region blocks run at oversample x the rate, the others at the host's; an ADAA port block is designed at
  2/3 of its rate (Holters 2020). Rate factors (`gc_`, `g2_`, `rl_`, `r2_`, `swstep_`) are init's; a block's
  coefficients are its `b<N>_set`'s, or init's when it has no pots and no switches, and in the Woodbury form what the
  rate alone moves is init's too ([linear blocks](#linear-blocks)).
- **The region** is one run of the block chain, through one up and one down halfband chain: `blocks=nonlinear` the
  first to last nonlinear block, `all` every block, `auto` (the default) those widened to every linear block that fails
  the 1x check and has no matched design: its trapezoidal response `H(j 2 fs tan(pi f / fs))` against the analog
  `H(j 2 pi f)`, at 44.1 kHz, over 20 Hz-20 kHz in 1.01x steps at each point of its pot and switch grid, counting only
  within 40 dB of the maximum, fails above 0.5 dB (T3's tolerance). The Tube Screamer's tone block (13.4 dB off at 44.1
  kHz) runs a matched design at 1x (0.35 dB), and stays out of the region with its two 15.9 Hz and 1.6 Hz high-passes;
  `blocks=all` puts it in, in its trapezoidal form. A linear-only netlist's blocks run at 1x, matched where they fail
  the check, and its region is the run of those that can be neither, at the smallest of 2, 4 and 8 that passes
  ([linear blocks](#linear-blocks)).
- **Latency** of the IIR halfbands' round trip at 1 kHz: 2.7, 3.5 and 3.8 host samples at 2x, 4x and 8x (the phase is
  not linear); of the FIR's, 40, 47 and 49.5; ADAA adds 0.5 / oversample for each port block it runs in. The header
  prints it. (The report's 3.2 / 4.3
  / 4.7 for the IIR count each decimator's group delay without the half high-rate sample its polyphase structure
  advances by: `tests/unit/CircuitTest.cpp` measures both.)
- **Determinism.** Straight-line float code with fixed step counts: the output never depends on how a host splits its
  blocks, and `set()` belongs on the absolute 16-sample grid (`example-circuits --check`, h).
- **Switches ramp.** `set()` moves each switch's `sw_<S>` toward its position's target, 1 where the S is closed and 0
  where it is open, by `swstep_` a call (snapping onto it within 1.001 steps), and `b<N>_set` computes its resistance
  `r_<S> = expf(ln roff + sw (ln ron - ln roff))`: geometric, so each call moves it by the same ratio, (roff / ron)^(1 /
  60) at 48 kHz, and the block sees a pot's glide, never a step. `init` starts each `sw_<S>` at its switch's default
  (then `set(defaults)` holds it there); `clear` leaves it where `set()` put it, as it leaves the coefficients, so a
  reset (the host's, or the guard's) keeps the host's position and nothing ramps; the ramp is a field, kept across
  calls (`example-circuits --check`, p).
- **Pots under ADAA.** ADAA's closed-form mean pairs the last step's point (`v_D1`, `p_D1`) with this step's on one
  curve, `p = v + 2 ka sinh(v / N Vt)`. A pot (or a switch's ramp) that moves K moves the curve, so `b<N>_set` moves
  `p_D1` onto the new one with `v_D1` held (its Newton residual kept); without it the mean spikes when the signal's move
  happens to cancel the curve's (on the TS's drive glides, about one glide in 60 to 200 clicks by 0.1 to 0.4 V, against
  an error of 0.07 V with the pot held). A held pot gives the same `ka`, and changes nothing.

### DC and the offset form

The circuit's DC operating point (`src/circuit/Dc.*`, the pipeline's stage 4) is solved at generation: each block in
chain order, in double, at rate 0 (a capacitor's `gc = 0` opens it, an inductor's `rl = 0` shorts it), its DC sources
(Constant sources of `assemble(plan, block, true)`: the V and I sources' values) on its right-hand side and its input
the block before's output (`.input`'s is 0 V), at the default pots, by a dense LU with partial pivoting. A diode's port
is solved open (its current 0, its row and current dropped): its `p` at DC is the voltage the network puts across it,
so an inductor across it, which shorts it at DC, leaves nothing undefined; only where the open network leaves that
voltage free (the diode a node's only DC path) is the port shorted instead (Holters' v-form, `v = 0`). Branches in
parallel at DC can share a current the equations leave free (a pot's 0-ohm end across an inductor): the elimination
then takes the pots' sections' currents last, so such a section carries none, as at every other position of its pot,
and checks the residual; only an inconsistent block (a DC current with nowhere to go), or one that leaves its port's
value free, is an error. Without a DC source every value is 0 (Phase 0's circuits: nothing to solve).

The module runs in **offsets** from that point: its states, ports, inputs and readouts are differences from it, so
`init` and `clear` set 0, a V source is a 0 V source (a short: its row is kept, its value's column dropped), a grounded
one's node is ground, and an I source is open. That is exact for a linear network, and for a port with 0 V across it
at the DC point (`f(0) = 0`, so its law in the offsets is its law). The output adds its node's DC voltage back as a
literal, after the guard's fade (a reset returns the circuit to its DC point), with a note: `return (gain_ < 1 ? z0 *
gain_ : z0) + 4.5f;` (`divider.cir: note: the output's DC operating point, V(out) = 4.5 V, is tick's constant`). `step`
and the fields are offsets, and so is what `trace` reads; the self-test measures tick less the constant. A port with a
DC voltage across it (more than 1 nV of its open-circuit `p` at DC) is an error at its element, `port D1 is biased at DC
(0.573 V): biased ports need the offset formulation (Phase 2)` (or, a current only the diode can take, `(0.001 A
through it)`), as is a pot that moves the DC point (each pot at 0, 0.5 and 1, the others at their defaults, the
defaults' point must still solve every block's equations: `pot 'x' moves the DC operating point (V(a) is 4.5 at x 0.5,
0 at 0): the module runs in offsets from one DC point (one a pot moves is Phase 2's)`), as is a switch's position that
moves it (each position, the others at their defaults: `switch 'mode' at position 1 moves the DC operating point: the
module runs in offsets from one DC point (one a switch moves is Phase 2's)`), and a block whose DC equations
are inconsistent while DC sources drive it (an op amp with only capacitors around it and a DC current into it: an
integrator with no DC source has no DC point to need, and starts at 0).

`examples/circuits/screamer-biased.cir` is `screamer.cir` with its 4.5 V reference drawn (V1, and R1 and R8 to it): its
DC point puts 4.5 V on the op amps' inputs and outputs and 0 V across the diodes and at the output (after C7), and it
generates screamer.cir's module but for its comments, so its output is screamer.cir's, bit for bit
(`example-circuits --check`, o; `tests/lit/circuit/op.test`). `.op` prints the point:

```
screamer-biased.cir: note: .op: V(vb) = 4.5 V
screamer-biased.cir: note: .op: V(in) = 0 V
screamer-biased.cir: note: .op: V(p1) = 4.5 V
...
screamer-biased.cir: note: .op: V(out) = 0 V
screamer-biased.cir: note: .op: i(V1) = 0 A
screamer-biased.cir: note: .op: port D1|D2: v = 0 V
```

A V source's current is SPICE's `i(V1)`, from n+ through the source to n-: a supply that delivers 450 uA reads
`-450 uA`. A grounded source's is what its node's other elements draw; a floating one's is its unknown.

**A DC input.** A circuit without bias starts and clears to 0 V, and its DC output, under a DC input, is where the
solver's fixed point lands, which is not exactly the circuit's DC point:

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
`tick` sums every field that carries from one sample to the next (the capacitors' and inductors' states, BDF2's second
parts, a matched block's `s1_` and `s2_`, the ports' last voltages, ADAA's last points and inputs, an OTA's stored tanh
and kd g, the halfbands' delay lines; a switch's ramp is `set()`'s, bounded, and not summed), and when the sum is not
finite (`!(sum_ - sum_ == 0)`: a NaN or an infinity anywhere in it), it calls `clear()` and sets the fade `fd_` to 0.
The output is `gain_ < 1 ? y * gain_ : y`, with `gain_` the fade before it rises by `fdstep_ = 1 / (0.02 rate)` a tick,
to exactly 1 after 960 ticks at 48 kHz. Either lane of a `float2` resets both. Settled, the gain is 1 and the output is
the circuit's, bit for bit (Phase 0's equivalence tests stay exact); the check is on the tick count, so how a host
splits its blocks never changes it. It costs a sum every 16 ticks, the halfbands' arrays in loops: about 7 cycles a host
sample on the shipped modules. The solver's bracket stays (it keeps `v` finite: a NaN written into `x_C1` leaves
diodetone's output finite until the guard resets it), and the playground's ±4 output guard is the last line
(`example-circuits --check`, k).

### Result

| Field | What |
|---|---|
| `source` | the module's FMinus text; empty when there are errors |
| `errors`, `warnings`, `notes` | `pedal.cir:7:1: error: ...` lines, as clang words them; the notes are the plan (each block, its rate and solver or linear form, each `b<N>_set`'s form, the method check's flags, the latency) and the provisional note |
| `module`, `pots` | the module's name; `set()`'s float arguments, in `.pot` order |
| `inputs` | tick's and step's arguments: the signal (`Input::Kind::Voltage`, its name and scale), then the law inputs (`Cv` or `Current`, their scales and ranges), in `.input` order |
| `switches` | the switches' names, in `.switch` order: `set()`'s int arguments, after the pots |
| `configuration` | one line: each choice, grouped by where it came from (the netlist, Options, the self-test and its key, provisional, default) |
| `provisional` | a dimension is open and no `Options::chosen` for this netlist answered it |
| `selfTestKey` | the key the self-test's answer goes by, when a dimension is open; empty when the netlist names its whole configuration |
| `latency` | host samples of delay at 1 kHz: the halfbands' round trip, and ADAA's half step for each port block it runs in |
| `lines` | for each line of `source`, the netlist's line it comes from (0: none): the module's frame at `.title`, `set` at the first `.pot`, a block's `b<N>_set` and `b<N>` at its first part, `tick` and `step` at `.input`, a part's fields at its line. `fminus` re-cites FMinus's messages with it |
| `nodes` | every node of the netlist but ground, in first-mention order, with its rate, DC volts and whether a probe can read it ([below](#probes)); `prs_` takes the index |

## Probes

A host that draws the circuit (the playground's schematic) watches its node voltages as it plays: `Options::probes`
gives the module the fields `kProbeFields` names (`include/fminus/Circuit.h`), through which a host arms up to
`kProbeSlots` (4) slots, each on a node of `Result::nodes`, and reads each one's last `kProbeRing` (1024) samples and
its peak, sum and sum of squares. The output's bits are the same with probes off, on and disarmed, and armed on any
node (`example-circuits --check`, q); so are the messages, the self-test's key and its answer.

```cpp
fminus::circuit::Options options;
options.probes = true;
const fminus::circuit::Result r = fminus::circuit::generate(netlist, "pedal.cir", options);
for (std::size_t k = 0; k < r.nodes.size(); ++k)     // prs_ takes k: "n1", line 14, 4x, 0 V, readable
    show(r.nodes[k].name, r.nodes[k].readable ? "" : r.nodes[k].why);
// between two calls (the fields' offsets from Program::Module::fields): prs_[0] = k, then prb_ = 1, slot 0 armed
// after each call: prn_'s advance since the last call is how many of slot 0's ring samples are new, the newest at
// pr_[(prn_ - 1) % 1024]; prp_[0], pra_[0] and prq_[0] read, then zeroed
```

`fminus pedal.cir --probes --nodes --emit fm` prints the module with its probe fields and, on stderr, the nodes
(`pedal.cir:14:12: node 3 'n1': rate 4, dc 4.5 V, readable`; `tests/lit/circuit/probes.test` pins diodetone.cir's).

**The nodes.** `Result::nodes` lists every node of the netlist but ground, in the order the netlist first mentions
them (an element's node token, or `.input`'s and `.output`'s node, whichever comes first: `.input u in` on line 3 makes
`in` node 0), with that mention's line and column, its spelling there, the DC point's volts (`.op`'s, the pots and
switches at their defaults), the rate of the block that computes it (steps a host sample) and whether a probe can read
it. It is filled with or without `Options::probes`. A probe reads:

- a node a block solves for or drives (an op amp's output), from that block's step: one value a host sample, the
  region's at its last step of the host sample (`rate` is the region's factor), a `.channels 2` module's lane 0;
- `.output`'s node: tick's output in volts, `.output`'s scale times it, through the halfbands when the region computes
  it (the oversampled node itself is not what plays), before the guard's fade;
- `.input`'s node: tick's sample times `.input`'s scale; a DC source's node: its volts.

It cannot read the inside of a matched design at 1x, whose node voltages the module never computes, only its input and
its output (`why`: "inside a filter computed as a whole at 1x (block 3)": the TS tone block's t1, tn, tw and tz;
`rc8k.cir`'s one block has no inside, its nodes its input and its output), nor a series string's inner node ("inside
the series string D1|D2+D3, computed as one diode (block 1)"), nor a node no block computes.

**The fields** come after every other field of the module, so the module's own keep their names, types and offsets:

| Field | Type | Who writes it | What |
|---|---|---|---|
| `prb_` | `int` | the host; init sets 0 | 0: off; else armed |
| `prs_` | `int[4]` | the host; init sets -1 | each slot's node, an index into `Result::nodes`; a slot on a node out of range or not readable writes nothing |
| `prn_` | `int` | tick; init sets 0 | the samples written while armed, modulo 2^30: the sample written at `prn_ = i` is at `pr_[1024 k + i % 1024]` |
| `pr_` | `float[4096]` | tick | slot k's ring at `[1024 k, 1024 k + 1024)`: absolute volts (the DC point added back) |
| `prp_`, `pra_`, `prq_` | `float[4]` each | tick; the host zeroes them when it has read them; init sets 0 | each slot's peak \|v\|, sum of v and sum of v², since zeroed |
| `prv_` | `float[nodes]` | tick | its scratch: each readable node's volts at the last armed sample |

`clear()` leaves them all (they are the host's, and the guard's reset is the circuit's), the guard sums none of them,
and `init` leaves the rings: a host allocates them zeroed, nothing reads a ring beyond what `prn_` says is written,
and a ring is memory (16 KB an instance) that stays untouched until a probe is armed. Every name ends in `_`, which no
pot, input or module name has; `isModuleWord` refuses them anyway, with the methods' (`tick_pr`, `step_pr`) and the
locals'.

**The code.** tick's first statement is `if (prb_ != 0) return tick_pr(u);` (with its law inputs): disarmed, a probe
costs tick a load, a compare and a branch, and the rest of tick is the module's without probes. `tick_pr` is tick
again (its guard, its law inputs, the same calls in the same order, so the same output bit for bit) with each block's
step replaced by `b<N>_pr`, which is `b<N>` with, before its states move on, a store of each of its nodes' volts into
`prv_`; in the region, only the host sample's last step calls the probing step (`step_pr`). Then it writes
`.input`'s, the DC nodes' and `.output`'s volts, each armed slot's sample, peak, sum and sum of squares, and advances
`prn_`. A node's volts are a readout the block already computes (its output, a grounded capacitor's voltage, the
port's v), or its own: `pn_<node>_by_<source>` on the step's sources (with ADAA, their means, as every readout's),
derived after the block's own coefficients in the same code with the same pivots. The elimination's statements are
the same text, so each is the temporary it was, the module's own coefficients are computed by the same operations as
without probes, and the Woodbury form and its counts are chosen without the probes' readouts; the probes'
coefficients are computed where the block's are (its `b<N>_set`, or init, with the Woodbury form's rate-only terms in
init), so a pot's move costs `set()` what they take. An ADAA block whose readouts never read its input but a node does
keeps that input a step earlier in `pru_b<N>`, which both steps store and the guard does not sum.

As built, in a host module's process loop as the playground's template calls a circuit (two instances, blocks of 64,
the signal from a table; `example-circuits --only probes`, an M5 at load 4): screamer.cir 1196 cycles a host sample
without probes, 1200 disarmed (+0.3 %), 1222 armed on one slot (+2 %), 1244 on four; otalp.cir 1232, 1229, 1303 and
1371; screamer.cir's `set()` with its three pots moving each call, 193 cycles without probes and 192 with them. The
disarmed figures are within the bench's noise of each other (+-1 % from run to run); a first build, with the branch
after the guard and `tick_pr` a call that was not a tail call, cost otalp.cir 2 %.

**The self-test** runs without probes (`selfTest()` generates its candidates with `Options::probes` off), and its key
leaves `probes` out (`tests/unit/CircuitSearchTest.cpp`).

## Configuration and the self-test

The configuration has six dimensions: the oversampling factor, the halfbands (IIR or FIR), the region (`blocks=`), ADAA,
the solver and the integrator (`.method`), with `.channels` beside them. Four are searched: oversampling, ADAA, the
solver and the integrator. Each choice comes, in this order, from:

1. `Options` (a member that is set);
2. the netlist's directive;
3. the self-test's answer, `Options::chosen`, for a searched dimension that neither names (or the netlist writes
   `auto`): an **open** dimension. The answer counts only when its key is the netlist's; otherwise it is ignored with
   the note `Options::chosen is another netlist's or other Options' (key ...; this one's is ...): it is ignored`. An
   answer with the netlist's key that the netlist cannot take (a damaged or hand-edited one: the self-test gives
   neither) is ignored the same way, all of it, with the note `Options::chosen (key ...) does not fit this netlist:
   ...: it is ignored`: an omega solver for an OTA's port, or ADAA with BDF2 where the answer gave either (the
   netlist's and the Options' own say stays an error);
4. the provisional configuration, for an open dimension nothing answered: `Result::provisional` is then set, with one
   note, `pedal.cir: note: the self-test chooses this circuit's configuration; provisional: 8x IIR, omega(2)`;
5. the default, for the rest: IIR halfbands, `blocks=auto`, TR, one channel.

A netlist without a port has nothing to choose: its dimensions are the defaults, never open. The integrator is open
when the netlist writes `.method auto`, or leaves `.method` out while another searched dimension is open; a netlist
that pins the rest and says nothing of the integrator plays TR, with nothing left to choose (were a missing `.method`
open, every netlist would be). Its candidates are TR's, and a BDF2 twin of each candidate at an oversampling where a
block rings ([the method check](#the-method-check)); the paste line names `.method` only when the answer is BDF2 or the
netlist wrote `auto`. The provisional integrator is TR. The header and `Result::configuration` say where each choice
came from: the netlist, Options, the self-test and its key, provisional, or default.

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

- points: the axes are the pots the region and the blocks before it read, each at 0, 0.5 and 1, the switches they read,
  each at every position, and the law inputs, each at its low end, its middle (geometric; the arithmetic one when its
  range starts at 0) and its high end: the full grid for three axes or fewer, otherwise each axis alone (the others at
  their defaults, a law input at its middle), then all low and all high;
- signals: sines at L/15, L/4.5, L/2.25, L and 2.2 L on odd bins 9, 85 and 257 of a 4096-sample window at 48 kHz (105,
  996 and 3012 Hz), after 1024 samples of warm-up; a 20 Hz-20 kHz log sweep and band-limited noise at L. L is `.target
  level`, else `.input`'s scale (full scale). With law inputs, a synth's too (report §2): a band-limited saw at L on bin
  9 (its harmonics below 20 kHz; T1, T2 and T4), and for each law input a 996 Hz sine at L with that input moving +-2
  octaves around its middle at 37 Hz (linearly over its range when it starts at 0; T1 and T4), run at the points where
  that input is at its middle. A law input's value is held over a host sample's region steps, in the module and in the
  references alike. Its values are in its range's units (amps for `kind=current`), which the references read as the
  gain; the candidate module takes each over the input's `scale` (`lawArguments`), which it multiplies back.

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
Newton steps; all with TR, and BDF2 twins where a block rings ([below](#the-method-check)). A dimension the netlist or
the Options pin drops out, and one configuration serves every port block of the region (so a region's ports are of one
family). They run **cheapest first** by the report's §3.2 model, in cycles a host sample: each region block times its
rate (a port block: 10 + 2 a state for p, 65 for omega3's guess or 5 for the previous step's, 70 a Newton step of a
diode pair or 60 of an OTA's tanh, 57 for ADAA's tail on an exp-affine law or 100 on tanh's, 5 for the state update, and
2 a state for BDF2's second part; a linear block: 5 + 3 a state), each block outside the region once, and 12 for each
sample a halfband stage runs at its high rate, up and down together. Candidates within 5 % of the cheapest of their
group go ADAA first, then fewer steps. The **first** to pass T1-T4 wins (the user's ruling: not an exhaustive search for
the cheapest); its notes list every candidate tried, in order, with its model cost and verdict. On the shipped netlists
without their configuration directives:

| Netlist | The self-test's answer | Candidate | Its T1, T2, T3 | Before it |
|---|---|---|---|---|
| `diodetone.cir` | 4x, omega(2) (968 model cycles) | 11 of 16 | -93.1, -79.0, 0.10 dB | 4x omega(1) fails T1 (-54.2 dB); 4x ADAA omega(1) fails T3 (0.57 dB); every 2x fails T2 or T3 |
| `screamer-clip.cir` | 4x, ADAA, omega(1) (916) | 10 of 16 | -78.7, -73.5, 0.14 dB | 4x omega(1) fails T1 (-49.8 dB); 2x ADAA fails T2 (-52.7 dB) |
| `screamer.cir` (the chain) | 4x, ADAA, omega(1) (976) | 10 of 16 | -81.1, -66.6, 0.21 dB | the same |
| `clipper.cir` | 4x, ADAA, omega(1) (908) | 10 of 16 | -80.6, -84.1, 0.44 dB | |
| `otalp.cir` | 4x, newton(1) (728) | 10 of 18 | -89.0, -73.2, 0.13 dB | 1x newton(1) fails T1 at 2 mA (-61.0 dB); every 1x fails T3 (2.3 dB: the trapezoidal rule's warp of H3 at 9 kHz, through two poles), every 2x by 0.03-0.04 dB; ADAA adds its droop |
| `halfwave.cir` (one diode) | 4x, omega(1) (680) | 9 of 16 | -70.3, -79.6, 0.04 dB | every 2x fails T2 (-58.6 dB) or T3 (1.28 dB with ADAA); omega3 is one diode's closed form where it conducts, so one step is enough |
| `asym.cir` (an asymmetric pair) | 4x, ADAA, omega(1) (1068) | 10 of 16 | -82.7, -86.4, 0.44 dB | 4x omega(1) fails T1 (-57.3 dB at 4.5 V, 3 kHz: omega3's guess for the two diodes' side); every 2x fails T2 or T3 |

These are the configurations the report and Phase 0 chose by hand, which the shipped netlists name (`example-circuits
--check`, i):

| Netlist | Configuration | Why |
|---|---|---|
| `diodetone.cir` | 4x, plain omega3 + 2 | the report's example-1 row (912 cycles); passes T1-T4; the self-test's answer |
| `screamer.cir`, `screamer-clip.cir` | 4x, ADAA, omega3 + 1 | the report's choice for the TS (903 / 907 cycles); passes T1-T4; the self-test's answer |
| `clipper.cir` | none: the self-test's | |
| `otalp.cir` | 4x, the previous v + 1 | the self-test's answer, pinned (no hand choice: the report measured an SVF, §7.3) |
| `halfwave.cir`, `asym.cir` | 4x, omega3 + 1; 4x, ADAA, omega3 + 1 | the self-test's answers, pinned (`example-circuits --check`, o) |

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

- **R1 Rate and pots at run time.** Each block's equations are eliminated in float with the pivot order fixed at
  generation: in the Woodbury form where it makes `b<N>_set` shorter (the variable branches pivoted last, what the rate
  alone moves eliminated once in init into the fields `w<N>_<k>`, the rest in `b<N>_set`), otherwise all in its
  `b<N>_set` (or init), as in Phase 0 ([linear blocks](#the-woodbury-form)).
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
  accepted when every pivot is at least 1e-9 of its column on the grid and the double residual is under 1e-9. The
  Woodbury form's order (the variable branches last) is tried first under the same rules.
- **The code builder** shares identical subexpressions and folds a temporary into the field assignment that uses it
  once, so `b<N>_set` reads like the hand-written modules (`p_D1_by_x_C1 = t16;`, `kb_D1 = 22.067577f * ka_D1;`).

## Prewarp

`.prewarp on` (or `Options::prewarp`; off by default, the user's ruling) moves an OTA one-pole's cutoff law from the
circuit's to the trapezoidal rule's prewarped one, so its -3 dB point is the analog circuit's at every gain, without
ADAA (with it, below, only nearer). At generation the block's small-signal pole is fitted as affine in the gain, `pole =
a + b g` (rad/s), from the ends of the gain's range on its grid, and checked at the middle (a residual of 1e-6 at most,
else a warning that prewarp is ignored). It applies to a block with one state (a capacitor or an inductor) whose network
no pot moves (no pot, no `{expr}` resistor, no E or G gain of pots); any other OTA warns that prewarp is ignored for it.
A law input's range makes a gain's, and the fit covers the OTA's own 10 MEG leak (otalp.cir's stage: `a = -1 / (10 MEG 1
nF) = -100`, `b = -att / (2 Vt 1 nF) = -4.2435e7`; at 10 uA the leak is a fifth of the pole, which a fit proportional to
`g` alone would miss). Where `g` is computed (each host sample for a law input, `b<N>_set` for pots, `init` for a
constant), it becomes `g' = (sign(a + b g) 2 fs' tan(w) - a) / b`, `w = |a + b g| / 2 fs'` below 1.5, with `fs'` the
block's design rate (2/3 of its rate with ADAA): the method `pw<N>_`, one `tanf` a host sample, off the recursion's
chain. The notes print `a` and `b`. It applies to an OTA in a one-state block whose network has no pot (the fit is made
at generation) on the trapezoidal rule; elsewhere it warns that it is ignored, as it does in a netlist without an OTA
and on a BDF2 block (it places the trapezoidal rule's pole). The self-test's T1
reference prewarps as the module does; the 64x reference, the circuit's truth, does not.

At 1x, 44.1 and 48 kHz, cutoffs of 1, 4 and 10 kHz, a 10 mV signal (`example-circuits --check`, m): with `.prewarp on`
the -3 dB point is the analog's within 0.0001 %; off, the trapezoidal rule's warp, -0.14 to -0.17 % at 1 kHz, -2.2 to
-2.6 % at 4 kHz and -11.5 to -13.1 % at 10 kHz.

**Only without ADAA.** ADAA's mean adds its own droop (a first-order average over each step, at 2/3 of the rate), which
the tan warp at `fs'` does not place: at 1x with ADAA the -3 dB point misses the analog's by -1.5 to -1.7 % at 1 kHz,
-9.2 to -10.2 % at 4 kHz and -23.9 to -25.3 % at 10 kHz with `.prewarp on` (off: -1.8 to -2.0, -12.9 to -14.5 and
-36.5 to -39.3 %), so prewarp narrows the error there but does not remove it (check m prints both); oversampling
shrinks both terms. Placing the -3 dB point under ADAA (a prewarp for the averaged recursion) is not built.

**A cutoff law, not an accuracy gain.** Prewarp places the small-signal -3 dB point by warping the OTA's gain `g`, and
`g` is also how hard the tanh is driven: near the cutoff the warped `g'` (38 % above `g` for a 13.5 kHz cutoff at 1x, 48
kHz) shrinks the OTA's differential input for the same output, so its distortion moves away from the circuit's (H3 goes
roughly with the cube of that input). On otalp.cir with its three configuration directives removed (`fminus
--self-test`), T3 against the 64x reference (which does not prewarp), without and with `.prewarp on`: 1x 2.27 to 2.99 dB
against 6.95 to 11.18 dB, 2x 0.53 to 0.54 against 1.43 to 1.85, 4x (the answer both times) 0.13 against 0.39, with
prewarp the worst case H3 at iabc 2 mA. So turning it on "for accuracy" gives a less accurate circuit at the same cost;
it is for a synth that wants the digital cutoff where the analog -3 dB point is, and the self-test's T3 still gates it.

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

## The elements and the DC point: what the code decided

Where the design left a choice, or the code proved it wrong:

- **RS is written into the netlist.** A model's RS becomes an element `RS_<D>` from the port's anode to a new node
  `rs_<D>` before the nodes are numbered (one for a pair, whose two RS must be equal; a string's is n RS), so the module
  is the one the explicit resistor gives, text for text (`tests/unit/CircuitTest.cpp`). A pair's shared resistor is
  the usual approximation (each diode's RS where it conducts), said in a note.
- **Strings and pairs on names.** The diodes are elaborated on the netlist's node names (a string's inner node is one
  two diodes alone touch, one's cathode and the other's anode, and not `.input`'s or `.output`'s); a pair of one law
  each way (the same Is and n N) is the sinh law, Phase 0's text unchanged, any other pair the asymmetric law.
- **One diode's guess** is omega3 for p > 0, where it is exact (`v + ka e^(v / N Vt) = p + ka`), and `p + ka` for p < 0
  (the diode off): halfwave.cir's self-test takes one Newton step at 4x. An asymmetric pair's omega3 is the conducting
  side's, selected per lane (`p < 0 ? kr : ka`, ...); its Newton step costs 110 model cycles (two exponentials).
- **The carry** of one diode and of an asymmetric pair reads ADAA's stored `c_P = e^(v_P / N Vt)` (and `cr_P`); the
  pair's recomputes its exponential from `v_P`, as Phase 0's text does, so its modules are unchanged.
- **A grounded V source's node is a constant**, not a row: in the offsets it is ground, so a biased netlist cuts and
  stamps as its unbiased twin does (screamer-biased.cir's module is screamer.cir's). A floating V source is a row and a
  current; on `.input`'s node, either kind is refused (the signal is `.input`'s).
- **E and G sources.** A G, and an E whose output floats, join their four nodes into one block, as an OTA does; an E
  with an output terminal grounded drives the other, a cut as an op amp's output, in its controls' block (whose regular
  nodes must be one block's: "controls in two blocks" is that check). A gain is a float coefficient's (1e-30 to 1e30 in
  size, negative for an inverting E, not 0) or an expression of pots, `b<N>_set`'s `k_<E>`.
- **The DC point is checked over the pots.** The offsets are exact only from one DC point, so at each pot at 0, 0.5
  and 1 (the others at their defaults) the defaults' point must still be a DC point: every block's equations there hold
  at it, each row to 1e-9 of its terms (and 1e-12 of the block's largest row). A residual, not a comparison of
  solutions, so a position that leaves a current's share free (a pot's 0-ohm end across an inductor) passes; one that
  moves the point is an error (a biasing trim is Phase 2's, with the offset formulation). A block singular at DC is an
  error only when its DC equations are inconsistent, or leave its port's value free, while DC sources drive it; with
  none, its point is 0 (an integrator starts at 0 V, as in Phase 0).
- **A diode's port is solved open at DC**, its `p` the voltage across it with its current 0, and shorted (Holters'
  v-form) only when the open network leaves that voltage free; a DC current only the diode can take is a bias too,
  given in amps.
- **The output's DC is added after the fade**, so a reset returns the module to its DC point, not to 0 V, and the
  self-test measures tick less it (its references run in offsets).
- **`clear()`'s comment** now says what it sets, "0 in the offsets the module runs in", on every module (Phase 0's said
  "no bias, 0 V everywhere"): the one change to the shipped modules' text.

## Linear blocks

A linear block (no port) runs the trapezoidal rule's form, or one of two others where that warps its response too far:
a matched design at 1x, or an oversampled region. Every block's coefficients, linear or not, may take the Woodbury
form.

### Matched designs at 1x

The trapezoidal rule maps the analog frequency axis onto the digital one by `tan`, so near 20 kHz at 44.1 kHz a
block's response is far from the circuit's: the TS tone block 13.4 dB off, an 8 kHz RC low-pass 12.8 dB, a 2 kHz
Sallen-Key 26.8 dB. A linear block of one or two states (`src/circuit/Linear.*`) can instead run a one-pole or a
biquad whose poles are the analog ones mapped by impulse invariance (`z = e^(s T)`) and whose numerator matches the
analog magnitude at DC, at the poles' frequency (clamped to 0.1 radians a sample below, 764 Hz at 48 kHz, and to a
quarter of the rate above) and at 0.83 of Nyquist (Vicanek, "Matched Second Order Digital Filters", 2016; a one-pole's
at DC and 0.83 of Nyquist), as transposed direct form II:

```c
const float ym_ = mb0_b3 * u + s1_b3;                  // ym_: a name no input can take
s1_b3 = mb1_b3 * u - ma1_b3 * ym_ + s2_b3;
s2_b3 = mb2_b3 * u - ma2_b3 * ym_;
```

- **From the trapezoidal form.** The design takes the block's own trapezoidal state-space (the coefficients its
  derivation gives, in float, wherever they are computed: `b<N>_set` for a block with pots or switches, init
  otherwise), whose inverse bilinear transform, in `s / 2 fs`, is the block's analog response exactly: so the design
  needs no rate of its own, and a pot's move reaches it through the same code. `matchedDesign<T>` runs on doubles (the
  gate, the tests), on floats (the gate's float run) and on `Val`s (the emitted text), as `derive<T>` does; the text
  uses `expf`, `sqrtf` and `sinf`, and selects (`q < 0 ? a : b`) where the poles turn from real to complex and where a
  square root's argument is held at 0 or above.
- **Poles near z = 1.** A low pole at a high rate puts the poles near `z = 1`, where float keeps few of the design's
  digits. Two places lost them all: Vicanek's `|A|^2 = A0 phi0 + A1 phi1 + A2 phi2` from `a1` near -2 and `a2` near 1
  (so `1 + a1 + a2`, a pole pair's distance from DC, came from their last digits), and the numerator's `B1` and `B2`
  from `|H|^2 |A|^2 - B0 phi0` at a match point so low that the response has barely moved from its DC value there. A
  Sallen-Key swept down to 99 Hz (`lowpass-sweep.cir`) came out with NaN coefficients at 48 kHz from a cutoff of about
  200 Hz down, and played NaN with no warning. Now `|A|^2`'s terms come from the poles in factors free of that
  cancellation (`1 + a1 + a2 = e^2 - rs`, `A1 - 16 a2 = (e^2 + rs)((1 + r)^2 + 4 r + rs)`, with `e = 1 - r` and `rs`
  from `sinh^2` or `sin^2` of the poles' spread), the middle match point has a floor (`kMatchFloor`: the numerator's
  magnitude is smooth, so a higher point serves as well, and in double no shipped figure moves), and every square
  root's argument is held at 0 or above and `b2`'s divisor away from 0, so the coefficients are finite wherever the
  poles sit.
- **The top match point is 0.83 of Nyquist**, the audio band's top at 48 kHz, not Vicanek's Nyquist: matched there, a
  block whose response falls as a one-pole's does (an RC low-pass above 300 Hz, the wah's tank, the TS tone block)
  errs by 0.96 dB near 0.37 of the rate; at 0.83, by 0.32 to 0.45 dB on the shipped blocks.
- **The gate.** At generation, at 44.1 and 48 kHz over the block's pot and switch grid, the matched response must pass
  the 1x check's rule (0.5 dB, within 40 dB of the maximum, 20 Hz-20 kHz): that is the note's figure. Then the float
  run: the design as the module computes it, `matchedDesign<float>` on the trapezoidal form rounded to float, at
  every host rate from 44.1 to 192 kHz (44.1, 48, 88.2, 96, 176.4 and 192) over a finer grid (17 positions a pot,
  every switch position), must give finite coefficients, poles inside the unit circle and a response that passes the
  same rule against the analog one. A design that cannot serve says why in a note: more than two states ("3 states: a
  matched design is of order 1 or 2"), zeros that are not minimum-phase (a magnitude match would change the phase),
  coefficients that are not finite, a polarity that changes with the pots (a magnitude match loses the sign; it is
  fixed at generation, -1 for an inverting block), or the float run's figure ("in float, as the module computes it,
  its response errs by 2.76 dB at 192 kHz, cut 0.0625": the same Sallen-Key at Q 5, its poles 1e-4 from `z = 1` there,
  where a direct form's float coefficients cannot place them). A block that fails either way joins the oversampled
  region, with a note. Above 192 kHz the module runs the same design, finite, unchecked.
- **Under `blocks=auto` only**, and only at 1x: `blocks=all` puts every block in the region in its trapezoidal form,
  and `blocks=nonlinear` leaves a failing block at 1x with a warning that says what `blocks=auto` would do with it
  ("blocks=auto would run a matched design at 1x (0.35 dB)", or "would oversample it").
- **Float.** The coefficients come out of float arithmetic on the block's own float coefficients, and a stiff block's
  are ill-conditioned (its poles near `z = 1`): the emitted responses are within 0.0010 dB of the double design's up
  to 96 kHz and within 0.016 dB at 192 and 384 kHz on the shipped netlists (`example-circuits --check`, p);
  lowpass-sweep.cir's, through the JIT at 33 positions of its pot from 44.1 to 384 kHz, finite with their poles inside
  the unit circle, and within 0.34 dB of the analog response to 192 kHz (0.22 at 384); rc8k.cir's and
  sallen-key.cir's coefficients themselves within 1e-5 at 44.1 and 48 kHz (`CircuitEmitTest.cpp`).

As built: `rc8k.cir` 0.32 dB (the trapezoidal rule's 12.79), `sallen-key.cir` 0.45 dB (26.82), `wah.cir` 0.35 dB
(13.42), the TS tone block 0.35 dB (13.40), each at 44.1 and 48 kHz; the header says it (`block 3 (...): ...; linear;
matched at 1x (0.35 dB off at 44.1 and 48 kHz; the trapezoidal rule's 13.40 at 44.1)`). The TS chain's region is now
block 2 alone: T3 against the 64x chain 0.26 dB (check d), tone 1 still 16.4 dB brighter at 5 kHz than tone 0, and a
20 ms tone glide's largest step 0.061 V against 0.078 V with the tone block trapezoidal at 4x. Matched designs and
their tests are pinned by `CircuitTest.cpp` (the poles at `e^(p T)` to 1e-12, |H| at DC, the poles' frequency and the
top point to 1e-9) and `CircuitEmitTest.cpp`.

### Linear-only regions

A netlist with no port has nothing for the self-test to choose. Its blocks run at 1x, matched where they fail the 1x
check; those that can be neither form its oversampled region, at `.oversample`'s factor when the netlist gives one,
else at the smallest of 2, 4 and 8 at which their trapezoidal responses pass the 1x check (the configuration line says
`(the 1x check)`; nothing is provisional). `tonestack.cir` (three states: no matched design) runs in a 4x region (13.37
dB at 1x, 0.37 at 4x): through the 4x IIR halfbands its H1 is within 0.006 dB of the analog response (check p). A
linear netlist ignores `.method bdf2`, with a warning.

### The Woodbury form

Phase 0 eliminated a block's whole system in `b<N>_set` each call, the rate's terms with the pots'. Most of a block
does not move with its pots: a pot's section, an `{expr}` resistor and a switch are variable branches in the impedance
form, their currents unknowns with a row each. `choosePivots` first tries an order with those currents pivoted last,
on their own rows (under the same threshold and residual rules); then everything the elimination computes before
them depends on the rate alone. `Val` carries a dependency tag (rate, pots: `gc_` and `rl_` are the rate's, `r_<P>`
and the switches' resistances the pots'), the code builder propagates it through each temporary, and the rate-only
temporaries `b<N>_set` reads become fields `w<N>_<k>`, computed once in init:

```c
// init, the TS clipping stage's rate-only part:
w2_0 = t6 + gc_C4;                     // j(D1|D2) per unit of n_p1
w2_1 = 0.00021276595f * t8;            // j(D1|D2) per unit of x_C3
vc_C3_by_n_p1 = t3;                    // a coefficient no pot moves: init's outright
// b2_set(drive), what the pot moves:
const float r_Pdrive = 5e+05f * t_Pdrive;
const float t2 = r_Pdrive + 51000.0f;
...
const float t14 = 1 / t13;             // K, the port's self-impedance
p_D1_by_n_p1 = w2_0 * t14;
```

It is the Woodbury identity's split: the fixed system's elimination once, and the k variable branches' small solve each
call. The form is kept only when it makes `b<N>_set`'s operation count strictly smaller; a note says which form each
block took (`block 3's b3_set: the Woodbury form (its variable branches pivoted last), 200 operations, and 11 in init;
all in set, 236`). ADAA's carry stays in `b<N>_set`. As built: the TS clipping stage 12 operations against 23, its tone
block 200 against 236 (the matched design is most of them), diodetone.cir 19 against 26, wah.cir 23 against 29; the tone
stack 182 either way (all in set), and the TS's level block finds no order that pivots its pot last (37). Every shipped
block's coefficients, through the JIT, are within 1e-7 of the double derivation all in set, row-normalised, from 44.1 to
384 kHz with the pots at 0 and 1 (the gate 1e-5), and those all in set within 1.9e-7 (the gate 1e-5) but one: the tone
stack's, whose region designs at 176 kHz to 1.5 MHz put its 8 Hz pole a hair from `z = 1`, where float holds its
coefficients to 8.0e-6 at 44.1 kHz, 1.4e-5 at 48 and 9.8e-5 at 384 (the gate 1e-4). The TS tone block's `b3_set`, its
pot moving every call, costs 126 cycles a call in the Woodbury form against 134 all in set (the bench prints both; an M5
at load 3): a pot's update is off the per-sample path either way.

## BDF2 and the method check

### BDF2

`.method bdf2` (or `Options::method`) runs the region's blocks with the second-order backward difference instead of
the trapezoidal rule: a capacitor's companion `gc = 3 C fs' / 2` with the history `x <- h - xo`, `xo <- h / 4`, `h =
4/3 gc v` (`g2_<C>` = 4/3 gc = 2 C fs'), an inductor's the same with `rl = 3 L fs' / 2`. Its state is `[x; xo]`, which
Holters' form, the references and the guard carry; `clear` sets both to 0 (the DC point). BDF2 is L-stable: a stiff
pole lands near `z = 0`, where the trapezoidal rule puts it near `z = -1` and lets it ring at Nyquist; its price is a
response that droops near Nyquist (no `tan` map makes it exact), so it is for a block that rings, not a default. Its
blocks are checked against their own map, `s = fs' (3 - 4 z^-1 + z^-2) / 2`, to 1e-9 (`CircuitTest.cpp`), and
diodetone.cir with `.method bdf2` follows the double BDF2 reference at -102.6 dB in band (check p). ADAA is Holters'
trapezoidal form, so `.method bdf2` with `.adaa on` is an error; `.prewarp on` places the trapezoidal rule's pole, so
it is ignored with a warning on a BDF2 block; a linear netlist ignores BDF2, with a warning, and its configuration
line says so (`.method tr (the netlist's bdf2 ignored: linear)`). The model costs BDF2 2 cycles more a state.

### The method check

The pipeline's stage 7 (report §6.2; fminus.md §5): a port block's trapezoidal recursion rings at Nyquist when its
linearised state matrix has an eigenvalue near -1 (a stiff pole, `z = (1 - a T / 2) / (1 + a T / 2)` for a large
`a`). At generation, for each region port block on the trapezoidal rule, `ringingCheck` (`src/circuit/Linear.*`)
forms `A + W Cp^T / (1 + kd G)` (Holters' form with the port replaced by its small-signal conductance G) at the
block's design rate and its pots, at the port's smallest conductance (v = 0) and its largest on the stress range (a
diode's at its short-circuit current at the stress level L over 20 Hz-20 kHz, the blocks before it at their analog
gain; an OTA's at v = 0 and its largest gain), finds its eigenvalues (Householder reduction to Hessenberg form, then
the shifted QR algorithm in complex double) and flags the block when one lies within 0.1 of -1, with a note:
`ringing.cir: note: block 1 (...): an eigenvalue of its linearised trapezoidal A at 1x, -0.9998, is within 0.1 of -1
(the port at 0.993 S): it may ring at Nyquist (the self-test measures it)`.

A flag is a suspicion: the self-test measures it. At each oversampling a candidate needs, it runs the flagged block's
region trapezoidal and converged (no ADAA) on the sines up to L, and compares the energy it puts between 20 and 24 kHz
with the 64x reference's there, summed over those units. More than 6 dB above it rings: the self-test warns once every
factor is measured, naming the smallest higher factor that does not ring (`ringing.cir:12:1: warning: at 1x block 1
rings at Nyquist (13 dB more above 20 kHz than the 64x reference): .oversample 2 or .method bdf2`; with `.adaa on`
named, in the netlist or Options, `.oversample 2, or .adaa off and .method bdf2`, since BDF2 has no ADAA), and puts each
candidate at that oversampling's BDF2 twin (no ADAA, its solver) right after it in the order, when the integrator is
open and ADAA is not named on. A flag without the excess is a note and no warning. The band is the host's top, 20 to 24
kHz at every factor: up to the region's Nyquist it grew with the oversampling and measured the halfbands' own stopband.

As built: `ringing.cir` (an inductor's kick ringing with a capacitor at 159 kHz) is flagged at every factor and rings
at 1x (12.8 dB; 1.7, 0.2 and 0.1 at 2, 4 and 8x); its 1x BDF2 twins fail too (T2 -34.5 dB, T3 1.62 dB: BDF2's droop),
and the answer is 2x, ADAA, omega(1). diodetone.cir's block is flagged at 1x (-0.96) and not at 4x (-0.85); the TS
clipping stage is flagged at every factor and measures 2.4, 0.1, -0.1 and 0.0 dB (no warning, no twin), so the
shipped netlists' answers are unchanged.

## Switches and the coefficients: what the code decided

- **A switch is a resistor that ramps.** Its resistance moves geometrically, `ln r` linear in the ramp, so every call
  moves it by the same ratio ((roff / ron)^(1/60) at 48 kHz, 1.308 for 1 ohm and 10 MEG), and the block sees a
  pot's glide: closing switch.cir's clipping diodes under a 4.5 V sine steps the output by 0.51 V at most, against
  0.59 V with a pot moving R the same way. A linear ramp of R would leave the switch all but open until its last
  calls, and close it in one.
- **The ramp is `set()`'s**, a field moved a step a call on the 16-sample grid, so the output never depends on how a
  host splits its blocks (calls of 1, 61 and 64 samples alike with a switch flipping) and a switch costs nothing a
  sample. It snaps onto its target within 1.001 steps, so 60 calls at 48 kHz land exactly. `init` puts each ramp at
  its switch's default; `clear` leaves it, as it leaves the coefficients `set()` computed from it. (As first built,
  `clear` put it back at the default too, with the coefficients still the host's position's: after a reset away from
  the default, the next `set()` stepped the switch to about its default and ramped it back over 20 ms, 4 V off for a
  clipping switch: the plan's "init and clear take the default position", which a final review found audible.)
- **Positions are the stress matrix's axes** (each of them), and the DC point is checked at each, as a pot's are.
- **The Woodbury form only where it saves**, by the code builder's own operation count (strictly fewer): the tone
  stack's is as long either way, and keeps Phase 0's text.
- **BDF2 is a twin, not a candidate of its own**: it costs a droop near Nyquist, so it is tried only where the
  trapezoidal rule was measured ringing, right after the candidate it replaces.

## How it computes

| Stage | File | What |
|---|---|---|
| parse | `src/circuit/Netlist.*` | the line scanner, numbers, expressions; `Messages` (clang's format) |
| plan | `src/circuit/Plan.*` | the diodes on the netlist's node names (each model, a card's or a built-in's; series strings; pairs, of one law or two; RS written in as a resistor to a new node), then diodes and OTAs into ports, pots and switches into variable branches, V sources into driven nodes or branches, E and G sources, I sources, law inputs, the checks (loops of sources and inductors, cut-sets of capacitors and current sources among them), the partition at op amps and grounded E sources (a union-find over the nodes no source drives; each op amp in its inverting input's network; an OTA's, a G's and a floating E's four nodes in one block; a grounded E in its controls' block), the chain, the DC point, the configuration (the integrator's rule too), the 1x check, the matched designs' gate and the region (a linear-only one's factor), the prewarp's fit, the method check's note |
| DC point | `src/circuit/Dc.*` | the DC operating point at the default pots, each block in chain order at rate 0 by a dense LU; the biased-port, pot and singular-at-DC errors; `.op`'s notes; the output's DC, tick's constant |
| equations | `src/circuit/Mna.*` | each block's stamps (KCL rows, nullor rows, a grounded E's row, the impedance form's rows of variable resistors, switches, inductors and floating V sources, a floating E's row, a diode's port row; a G's transconductances; sources: the input, the states, a diode's v or an OTA's i, and at DC the V and I sources' constants; trapezoidal or BDF2 companions), the pivots (the variable branches last first, for the Woodbury form), `solve<T>`, `derive<T>`, `holters()` (the v-form, and an OTA block's i-form too; BDF2's `[x; xo]`), `analog()`, the 1x check |
| laws | `src/circuit/Laws.*` | each law's text (the guess, the Newton steps in the bracket, ADAA's mean, what a step stores) and its double twin (phi, phi', the antiderivative, the steps, the converged solve) |
| linear | `src/circuit/Linear.*` | a linear block's trapezoidal form (order 1 or 2), the matched design on doubles and `Val`s, its gate; the eigenvalues (Hessenberg, shifted QR) and the method check's linearised A |
| text | `src/circuit/Emit.*` | `Val` (with its rate and pots tag) and the code builder (its operation count, the init/set split), then the module: fields, init (the Woodbury form's `w<N>_<k>`), clear, set (the switches' ramps), `b<N>_set`, `b<N>` (a matched block's TDF2), the halfbands, step, tick (with the guard); the map from each line to the netlist's; the probes' nodes (`probeNodes`), readouts, fields and probing methods |
| halfbands | `src/circuit/Halfbands.*` | the S1-S3 FIR and IIR coefficients of `experiments/hb-coefs.txt`, their responses, their FMinus |
| generate | `src/circuit/Generate.cpp` | `generate()`: parse, plan, each block's derivation, emit; errors stop it before emission; `selfTestKey()` |
| search | `src/circuit/Search.*` | the self-test's arithmetic: the stress matrix (switch positions among its axes), the 64x and T1 references (TR or BDF2), a unit's gates and their reduction, the candidates in the cost model's order, the method check's measure, warning and BDF2 twins, the ordered search, the no-configuration answer, the budget; `Choice`'s text |
| runners | `src/circuit/Runners.*` | `fm_set`, `fm_steps` and `fm_ticks`: FMinus that drives a module over buffers, a call a run |
| self-test | `src/api/CircuitSelfTest.cpp` (in `fminus_lib`) | `selfTest()`: each candidate generated, compiled through `fminus::Program` and run on the host's threads |
| reference | `src/circuit/Reference.*` | a block in Holters' form in double, converged (a diode pair's from the closed-form guess, an OTA's by bracketed Newton) or with the emitted steps, plain or ADAA (an OTA's in its i-form, with the carry, which `RefOptions::carry` turns off for a check's control); a chain, with a stream of gains |
| analysis | `src/circuit/Analysis.*` | the FFT, the windowed-sinc decimator, in-band error, inharmonic energy (and its excess over a reference's), H1 and H3, the stress signals (sines, the saw, the sweep, noise) |

The invariant: every emitted coefficient is `derive<Val>` of the derivation the reference runs as `derive<double>`.

## The checks, and the report's tables

`example-circuits --check` (ctest's `example.circuits`) runs the generated modules through the JIT, a call a run, and
prints each check's worst value, where it fell, and the report's figure beside it:

- **a.** every shipped netlist generates with no error or warning, and its methods are nonblocking (the report's
  four, `otalp.cir`, `rc8k.cir`, `sallen-key.cir`, `tonestack.cir`, `switch.cir` and `ringing.cir`; o has the rest,
  and p `lowpass-sweep.cir`);
- **b.** DC, as above;
- **c.** report §2's targets on both 1-port blocks, as shipped and on the provisional configuration (Phase 0's
  defaults), over the full stress set (sines at L/15, L/4.5, L/2.25, L and 2.2 L on odd bins at 105, 996 and 3012 Hz, a
  sweep and noise at L, each pot at 0, 0.5 and 1): **T1** the emitted step against the converged double of the same
  configuration, in band, at most -60 dB; **T2** the emitted tick's inharmonic energy at most -60 dB for sines up to L;
  **T3** H1 and H3 within 0.5 dB of a converged 64x reference; **T4** finite on every signal; the references converge
  on every sample;
- **d.** the TS chain: T3 against a converged 64x chain at drive x tone {0, 0.5, 1} (0.26 dB, the tone block matched
  at 1x), and tone 1 is 16.4 dB brighter at 5 kHz than tone 0 (report §9.1's orientation); a 20 ms tone glide's
  largest step, the tone block matched, within 1.1x of the step with it trapezoidal at 4x (`blocks=all`; 0.061 V
  against 0.078 V);
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
  newton(1), candidate 10 of 18) with the same figures, and so it does with iabc's argument in mA (`scale=1m`, the
  same range in amps); tick against step through its 4x IIR oversampler; the
  prewarp's -3 dB points ([above](#prewarp)); the carry under ADAA: one OTA stage driven into its law's bend (10k / 1k
  attenuators, tanh 0.7 at the step), iabc stepping from 1 uA to 200 uA in one sample and back at 1x, newton(2): the
  module follows the double reference with the same steps, ADAA, gain stream and carry to 1e-5 of the signal (2e-5
  the gate), where the reference without the carry departs by 18 times the signal, and a module without it by 0.18 of
  it (the mean pairs points on curves 200x apart); and over 512 ramps of a +-2-octave 37 Hz triangle on otalp.cir the
  ADAA module stays within 1.1x of its departure from a plain newton(3) module with iabc held (0.1502 V against 0.1501
  V), a bound on the output that the carry itself, a few uV there, barely moves;
- **n.** law inputs: iabc NaN, 0 and 1 A for 200 samples of an 11 V sine give finite output; `.channels 2`'s lanes
  are the scalar module's, bit for bit, the law input broadcast to both; calls of 64 and 61 samples are one call's,
  bit for bit, with a per-sample iabc;
- **o.** Phase 1's elements: `screamer-biased.cir`, `halfwave.cir`, `asym.cir` and `wah.cir` generate (wah.cir, linear,
  matched at 1x: its tank's trapezoidal response near 20 kHz warps by 13.4 dB at 1x) and are nonblocking;
  screamer-biased.cir's module is screamer.cir's but for its comment lines, its output screamer.cir's bit for bit under
  a drive glide, its DC point 4.5 V at the op amps' inputs and outputs (to 1e-9) and 0 V across the port; halfwave.cir
  and asym.cir without their directives get the exp-affine answers they ship, which meet T1-T4 (the figures above);
  wah.cir's tick follows its matched design's response within 0.0006 dB (gate 0.01 dB) at 105, 996 and 3012 Hz,
  `res` 0, 0.5 and 1;
- **p.** the coefficients and the linear blocks: every shipped netlist's coefficients, init's and `b<N>_set`'s through
  the JIT, against the double derivation all in set, 44.1 to 384 kHz, on each block's control grid with the pots at 0
  and 1 (the Woodbury blocks within 1e-7, gate 1e-5; those all in set within 1.9e-7, gate 1e-5, but the tone stack's,
  within 9.8e-5 at 384 kHz, gate 1e-4), the matched designs' float responses against the double design's (0.0010 dB to
  96 kHz, gate 0.02; 0.016 above, gate 0.1), the TS tone block's operation counts (200 against 236); `switch.cir`: the
  ramp from roff to ron in 60 `set()` calls at 48 kHz, each call's ratio within 0.1 % of (roff / ron)^(1/60), closing
  the diodes under a 4.5 V sine within 1.1x of a pot moving R the same way (0.51 V against 0.59), calls of 1, 61 and 64
  samples with S1 flipping bit for bit one call's, the ADAA point's Newton residual kept call by call (4.5e-7 of `p_D1`,
  gate 1e-5), each position among the stress matrix's points; `rc8k.cir` and `sallen-key.cir` matched at 1x with the
  header's figure, their ticks their designs' responses (gate 0.01 dB), the TS tone block matched and the region block 2
  alone, `tonestack.cir` falling back with its note, `lowpass-sweep.cir`'s design through the JIT (finite, stable and
  within 0.5 dB of the analog response at 33 positions, 44.1 to 192 kHz); the tone stack's linear-only 4x region (tick
  against step through its halfbands, H1 against the analog response 0.006 dB off); BDF2 on diodetone.cir (the step
  against the double BDF2 reference, -102.6 dB in band, gate -100; nonblocking in one and two channels); the method
  check (`ringing.cir` at 1x flagged, 12.8 dB of excess, the warning's text and its BDF2 twins; diodetone.cir at 4x not
  flagged; screamer-clip.cir flagged without excess, no warning, no twin);
- **q.** the probes, on every netlist here (the playground library's three, diodetone.cir, otalp.cir and screamer.cir,
  among them but for their comments): in one and two channels, the stress set's signals one after another with the
  pots gliding (the switches stepping, the law inputs moving), in calls of 64 and 61 samples, the output bits the
  module's without probes, with probes on and disarmed, and armed on each readable node in turn (four slots, each on
  another) and on each unreadable one (with nodes out of range: those slots write nothing); then each readable node's
  ring, a sine at L / 2.25 with the pots at their defaults, against the double reference of its voltage in the same
  configuration (the chain's Holters forms with the emitted solver's steps, the node's block reading that node, the
  matched designs as their double TDF2, the halfbands in double), within -60 dB of the node's RMS or 1e-4 V (the worst
  -79.9 dB, switch.cir's d), `.output`'s ring tick's output times its scale, `prp_`, `pra_` and `prq_` the ring's
  samples' peak, sum and sum of squares in float, bit for bit, and each node's `dc` the `.op` note's to 1e-9.

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

`example-circuits` with no flags first prints `generate()`'s time on each shipped netlist (0.1 to 16 ms: the TS's linear
blocks' 1x checks and matched gates are most of it; the tone stack's linear-only region, not in the list, about 220 ms,
its 1x checks at three factors), the self-test's wall time on 1, 2, 4 and 8 threads (diodetone.cir and screamer-clip.cir
without their directives: 4.5 to 4.9, 2.4 to 2.6, 1.6 to 1.7 and 1.2 s on an M5 at load 3; the method check's
measurements added a second on one thread) and `b3_set`'s cycles a call in both forms ([above](#the-woodbury-form)),
then the cost table in `experiments/fmbench.cpp`'s method (subnormals flushed, one thread, the best of 7 x 10^6 host
samples; cycles and instructions from `proc_pid_rusage` on macOS), each row named as `experiments/mkbench.py` names its
bench and printed beside the self-test's cost model and `experiments/timing1.out`'s measurement; `--all` runs mkbench's
whole matrix, `--only <regex>` some rows, `--math fast` the frozen copy's mode. Acceptance, by eye: within 10 % of
timing1.out on an idle M-series Mac. The emitted shapes are mkbench's (the omega3, Newton and ADAA text, field arrays in
tick), in one module with methods, with fewer coefficients. As built (an M5 at load 2): every row within 7 % of
timing1.out, mostly a few percent faster; the TS clipping stage at 4x with ADAA 904 cycles a host sample (the report
903), the TS chain 905-906 whatever the region (the report 905-907: the tone block in the region costs nothing), example
1 as shipped 863 (899), and Phase 0's defaults on the TS stage, 8x plain omega3 + 2, 1,803 (1,865).

The TS chain as shipped, its tone block matched at 1x (`f_TS_w1_adaa_4x_iir_auto`), costs 932 cycles a host sample
(907 with the block trapezoidal at 1x, `_part`; 927 with it in the region, `_all`; the model 943).

The synth path's rows (`--only OTA`) run otalp.cir with iabc from an expo converter each sample (one `expf`, `2^(2
sin)` around 141 uA at 37 Hz), against the model, which leaves out the converter and an OTA's last `tanhf` (its
current at the solved v): at load 4, 1x newton(2) 481 cycles (model 284), 2x newton(2), the provisional, 530 (592),
4x newton(1) as shipped 815 (728), with ADAA 1,108 (1,528), `.channels 2` at 1x 472 for both lanes, and 1x newton(2)
with `.prewarp on` 293. That last row is the unexplained one: the module with prewarp (790 instructions a sample)
runs at an IPC of 2.7, the one without (516) at 1.1, on the same recursion; the IR has not been compared yet. The
self-test on otalp.cir without its directives takes 1.6 s on 4 threads (5.1 s of CPU).

## Not built yet

Phase 2 and later (the design's §17); a netlist that asks for one is refused with "not supported yet" where it can ask:

- **Devices and solvers:** BJTs, JFETs and junction capacitances (CJO and the rest warn "ignored"); op amps beyond
  ideal (soft rails, GBW); multi-port Newton (m > 1), with BLT, tearing and Holters' decomposition, the OTA SVF,
  ladders, resonance and self-oscillation stress; feedback across cuts; biased ports (the `expm1` offset formulation,
  and a DC point a pot or a switch moves); open-loop OTAs (VCAs); mixed diode and OTA regions (per-family solvers);
  pot-as-port (law inputs in R, E or G); tubes and tables (the K-method, file-scope `const` arrays); second-order
  ADAA; halfband design (the coefficients are gen.cpp's, embedded); MZT and symbolic closed-form linear designs (a
  matched design of more than two states); interpolated (up-chain) law inputs.
- **Netlists:** F, H, K, T and B elements; `.subckt`, `.param`, `.include` and `.lib`; several inputs, outputs or
  regions; per-block configurations; more built-in models than 1N914 and 1N4148.
- **Probes:** a `.channels 2` module's second lane, differential and current probes, the inside of a matched design or
  of a series string of diodes; a ring that init clears.
- **FMinus and tooling:** `#line`, `fmaf`, `expm1f`, `omegaf`; ngspice goldens; `.cir` in `editors/vscode`;
  `Circuit.h` on the site's API page.
- **The playground:** voice banks (`.channels 4`, VoiceBank); the circuit block sugar; `latency()` in the contract;
  `alias_db`, the sine scenario and `reference_null_db`; a disk cache of answers; language features inside `.cir`
  tabs; SPICE highlighting in chat code blocks; a "Refining" indicator and `get_status` circuit lines; a view of the
  generated module.
