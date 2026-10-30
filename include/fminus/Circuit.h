// include/fminus/Circuit.h: analog circuits as FMinus source. fminus::circuit::generate turns a SPICE-style netlist
// (a .cir file: resistors, capacitors, inductors, DC voltage and current sources, E and G controlled sources, diodes
// (one, a series string, a pair, an asymmetric pair, with RS) with their .model or a built-in one (1N914, 1N4148),
// OTAs, ideal op amps, pots, ramped switches, law inputs, and the directives docs/circuits.md lists) into the text of
// one FMinus module, which Program::compile compiles like any other source. It is the circuit compiler of
// docs/research/analog-circuits/README.md (Phase 1 of its §11, in slices). No LLVM, C++17, as FMinus.h; the host links
// FMinus::FMinus.
//
//   const fminus::circuit::Result result = fminus::circuit::generate(netlist, "pedal.cir");
//   if (!result.errors.empty())
//       return report(result.errors);                     // "pedal.cir:7:1: error: ...", as clang words them
//   auto program = fminus::Program::compile(result.source, errors);
//
// The module (named by .title):
//   void init(float rate)        the host's rate; every state at the DC operating point (0: the module runs in
//                                offsets from it), the pots and switches at their defaults
//   void set(float drive, ...)   the pots, 0 to 1, in .pot order (clamped), then the switches' int positions
//                                (Result::switches): call it on the absolute 16-sample grid, and every grid tick while
//                                a switch ramps
//   float tick(float u, ...)     one host-rate sample: .input's volts (1.0 = scale volts) to .output's (its DC
//                                operating point added back, a constant: step and the fields are offsets); the law
//                                inputs (Result::inputs after the first: floats, in .input order, broadcast to float2
//                                with .channels 2) follow the signal, each clamped to its range (a NaN is its low
//                                end) and held over the host sample's oversampled steps
//   float step(float u, ...)     one step of the oversampled region, at .oversample times the rate (absent if none)
//   void clear()                 back to the DC operating point (the pots and switches where set() left them)
// Fields are named after the parts (x_C1: C1's state; v_D1: the voltage across the pair D1|D2) and hold offsets from
// the DC operating point. .channels 2 makes tick and step take and return float2s. The code is straight-line float
// code with fixed step counts: its output never depends on how a host splits blocks. generate() is pure and
// deterministic (the same text and options give the same Result, byte for byte, on any machine), safe on any thread,
// and allocates: call it off the audio thread.
//
// The guard: every module checks, once every 16 ticks (counted from init and clear), that the fields it carries from
// sample to sample are finite. If one is not (a NaN or an infinity from a host's input, say), tick calls clear() and
// fades its output back in over 20 ms. Settled, the output is the circuit's, bit for bit.
//
// The configuration (the oversampling factor, ADAA, the solver, the integrator) is the netlist's when its directives
// name it. A dimension the netlist leaves out, or writes `auto`, is open: the self-test chooses it, on the report's
// targets and stress matrix. The self-test takes seconds, so it never runs inside generate(). A host:
//   1. generates; Result::provisional says a dimension is open and nothing answered it, Result::selfTestKey names it;
//   2. plays that provisional module (a configuration that is safe, if costly);
//   3. runs selfTest() on its own threads (SelfTestOptions::parallel), off the audio thread;
//   4. stores the answer, Choice::text(), under the key (in memory, in a saved session);
//   5. generates again with Options::chosen = Choice::parse(stored), and compiles that.
// FMinus owns no threads and no cache for this: selfTest() blocks, is pure and deterministic, and allocates.
#pragma once

#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace fminus
{
    // SPICE-style netlists to FMinus modules (above).
    namespace circuit
    {
        // The self-test's version: part of every key (selfTestKey), so an answer stored by another version of the
        // gates, the candidates or the emitted arithmetic is never taken for this one's.
        inline constexpr int kSelfTestVersion = 1;

        // How a nonlinear port's equation (v + 2 Is K sinh(v / N Vt) = p for a diode pair, v + kd g tanh(v / 2 Vt) = p
        // for an OTA) is solved each step: a first guess, then a fixed number of Newton steps, each kept in the
        // bracket [min(0, p), max(0, p)].
        //   Omega     the closed form of the conducting diode alone (D'Angelo et al.'s omega3): .solver omega(K); a
        //             diode's only
        //   Previous  the previous step's solution: .solver newton(K) (an OTA's; a diode's fails at hard drive)
        struct Solver
        {
            // Where Newton starts.
            //   Omega     omega3's closed form: .solver omega(K)
            //   Previous  the previous step's solution: .solver newton(K)
            enum class Guess
            {
                Omega,
                Previous,
            };

            Guess guess = Guess::Omega;   // where Newton starts
            int steps = 1;                // Newton steps after the guess: 0 to 8 (Previous: 1 to 8)
        };

        // The oversampler's 2x halfband stages, designed for a 48 kHz host (passband to 20 kHz).
        //   Iir  polyphase allpass (HIIR's design): about 3.5 host samples of delay at 1 kHz at 4x; the default
        //   Fir  Kaiser-windowed, linear phase: about 47 host samples of delay at 4x
        enum class Halfband
        {
            Iir,
            Fir,
        };

        // Which blocks run oversampled (.oversample's blocks=).
        //   Auto       the nonlinear blocks' span, and every linear block whose 1x response errs by more than 0.5 dB
        //   Nonlinear  from the first nonlinear block to the last
        //   All        every block
        enum class Region
        {
            Auto,
            Nonlinear,
            All,
        };

        // The integrator of the oversampled region's blocks (.method); the blocks outside it are trapezoidal (or a
        // matched design, docs/circuits.md).
        //   Tr    the trapezoidal rule: the default
        //   Bdf2  the second-order backward difference (L-stable: it does not ring at Nyquist where a stiff block
        //         makes the trapezoidal rule ring); never with ADAA, which is Holters' trapezoidal form
        enum class Method
        {
            Tr,
            Bdf2,
        };

        // An argument of tick and step: the signal (the first), or a law input (.input NAME - kind=cv|current), a
        // control that enters a device's law, such as an OTA's bias current.
        struct Input
        {
            // What the input is.
            //   Voltage  the signal: a voltage source from its node to ground, sample 1.0 = scale volts
            //   Cv       a law input from 0 to 1 (or its range), times scale
            //   Current  a law input in amps, times scale, within its range
            enum class Kind
            {
                Voltage,
                Cv,
                Current,
            };

            std::string name;            // as written: the argument's name
            Kind kind = Kind::Voltage;   // the signal, or a law input's kind
            double scale = 1;            // sample 1.0 is this many volts (or amps, or units of the law input)
            double low = 0;              // a law input's range (range=lo:hi), which tick clamps it to; 0 for the signal
            double high = 0;             // its top; 0 for the signal
        };

        // A self-test's answer: the configuration it chose for one netlist (its key), and how it measured. text() is
        // what a host stores; parse() reads it back. A choice passed to generate (Options::chosen) fills only the
        // dimensions the netlist and the Options leave open.
        struct Choice
        {
            std::string key;              // the netlist's selfTestKey: 16 hex digits
            int oversample = 8;           // 1, 2, 4 or 8
            bool adaa = false;            // Holters' first-order ADAA
            Solver solver;                // the ports' solver
            Method method = Method::Tr;   // the integrator
            bool met = true;              // whether it meets the targets T1-T4 (false: the most accurate under the cap)
            double t1 = 0;                // its worst T1 (solver error against its converged reference), dB
            double t2 = 0;                // its worst T2 (inharmonic energy, the alias), dB
            double t3 = 0;                // its worst T3 (|H1| or |H3| against the 64x reference), dB
            std::string worst;            // when not met: the worst gate and where it fell; empty otherwise

            // One line: "fminus-circuit-choice 1 key=0123456789abcdef oversample=4 adaa=on solver=omega(1)
            // method=tr met=yes t1=-78.7 t2=-73.5 t3=0.14", then " worst=..." when not met (the rest of the line).
            std::string text() const;

            // text()'s line read back, or nothing, with the reason in `why`, when it is not one (or is another
            // version's).
            static std::optional<Choice> parse(std::string_view text, std::string* why = nullptr);
        };

        // Overrides of the netlist's directives: a member that is set wins over its directive, and one left unset
        // leaves the choice to the netlist, then to the self-test's answer (chosen), then to the provisional
        // configuration (docs/circuits.md).
        struct Options
        {
            std::string moduleName;             // the module's name; empty: the netlist's .title
            std::optional<int> oversample;      // 1, 2, 4 or 8 (.oversample)
            std::optional<Halfband> halfband;   // .oversample's iir or fir (default iir)
            std::optional<Region> region;       // .oversample's blocks= (default auto)
            std::optional<Solver> solver;       // .solver
            std::optional<bool> adaa;           // .adaa on|off
            std::optional<int> channels;        // .channels 1 or 2 (default 1)
            std::optional<Method> method;       // .method tr|bdf2 (default tr; bdf2 not with ADAA)
            std::optional<bool> prewarp;        // .prewarp on|off: an OTA one-pole's cutoff law prewarped (off);
                                                // its -3 dB point the analog's without ADAA (with it, nearer only)
            std::optional<Choice> chosen;       // the self-test's answer: fills the open dimensions when its key is
                                                // the netlist's (otherwise a note says why it is ignored)
        };

        // What generate() returns.
        struct Result
        {
            std::string source;                  // the module's FMinus text; empty when there are errors
            std::string errors;                  // "pedal.cir:7:1: error: ..." lines, each with its notes
            std::string warnings;                // "pedal.cir:14:12: warning: ..." lines, the same format
            std::string notes;                   // "pedal.cir: note: ..." lines: the blocks, rates, solvers, latency
            std::string module;                  // the module's name, "DiodeTone"
            std::vector<std::string> pots;       // the pots in .pot order: set()'s float arguments
            std::vector<Input> inputs;           // tick's and step's arguments, in order: the signal, then law inputs
            std::vector<std::string> switches;   // the switches in .switch order: set()'s int arguments, after the pots
            std::string configuration;           // one line: each choice, and where it came from (the netlist,
                                                 // Options, the self-test <key>, provisional or default)
            bool provisional = false;            // a dimension is open, and no Options::chosen for this netlist
            std::string selfTestKey;             // the key the self-test's answer goes by, when a dimension is open;
                                                 // empty when the netlist names its whole configuration
            double latency = 0;                  // host samples of delay at 1 kHz: the halfbands' and ADAA's
            std::vector<int> lines;              // for each line of source, the netlist's line it comes from:
                                                 // lines[k] is line k + 1's (0: none, as the module's frame)
        };

        // The FMinus module of `netlist`, the text of the file the messages call `fileName`, with `options` (default
        // Options when there are none).
        Result generate(std::string_view netlist, std::string_view fileName, const Options& options);
        Result generate(std::string_view netlist, std::string_view fileName);

        // The key a self-test's answer for `netlist` with `options` goes by: 16 hex digits of FNV-1a 64 over
        // kSelfTestVersion, the netlist's lines as SPICE reads them (continuations joined, comments and blank lines
        // dropped, tokens lowercased and single-spaced; .title, .channels, .op, .end and simulator directives left
        // out) and the Options that change the answer (all but moduleName, channels and chosen). Empty when the
        // netlist does not parse and plan. It parses and plans only: cheap, and any thread may call it.
        std::string selfTestKey(std::string_view netlist, std::string_view fileName, const Options& options);

        // How selfTest() runs.
        struct SelfTestOptions
        {
            // Runs job(0) to job(count - 1), each once, in any order and on any threads, and returns when all have
            // returned. Empty: in order, on the calling thread. The jobs write only their own results, so the answer
            // is the same, byte for byte, for any number of threads. A job never throws: what one would throw is
            // carried back to selfTest()'s caller as an internal error in SelfTestResult::errors.
            std::function<void(int count, const std::function<void(int)>& job)> parallel;
            // Polled between jobs and every 4096 steps of a reference, on the calling thread and on parallel's: true
            // stops the test (no answer).
            std::function<bool()> cancelled;
            // The most steps (the references' and the candidates') the test may take; 0: the default (4e9, more than
            // any netlist needs yet). A test that would go over stops before it, with no answer and a note.
            std::int64_t budget = 0;
            // Measure every candidate, not only up to the first that passes (for tables and benches); the answer is
            // still the first that passes.
            bool everyCandidate = false;
        };

        // What selfTest() returns.
        struct SelfTestResult
        {
            std::string key;                // the netlist's selfTestKey
            std::optional<Choice> choice;   // the answer; none when cancelled, over budget, with nothing to choose,
                                            // or when the netlist has errors
            bool cancelled = false;         // SelfTestOptions::cancelled stopped it
            std::string errors;             // the netlist's errors, as generate() gives them
            std::string warnings;           // and its warnings, then the self-test's own: a block that rings
                                            // at Nyquist ("at 1x block 2 rings at Nyquist (...): .method bdf2")
            std::string notes;              // "pedal.cir: note: ..." lines: the stress matrix, each candidate tried
                                            // in order with its model cost and verdict, and the answer
            int candidates = 0;             // how many candidates it measured
        };

        // The self-test of `netlist` with `options`: the open dimensions' candidates, cheapest first by the report's
        // cost model (§3.2), each generated, compiled (fminus::Program, on the calling thread) and run on the stress
        // matrix (on SelfTestOptions::parallel's threads) against the converged double references, until one meets
        // the targets T1-T4 (report §2); when none does, the most accurate under 3,500 model cycles a host sample,
        // with met = false. Blocking, pure and deterministic; seconds to minutes: never on the audio thread.
        SelfTestResult selfTest(std::string_view netlist, std::string_view fileName, const Options& options,
                                const SelfTestOptions& test = {});
    } // namespace circuit
} // namespace fminus
