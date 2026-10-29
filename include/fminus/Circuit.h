// include/fminus/Circuit.h: analog circuits as FMinus source. fminus::circuit::generate turns a SPICE-style netlist
// (a .cir file: resistors, capacitors, antiparallel diode pairs with their .model, ideal op amps, pots, and the
// directives docs/circuits.md lists) into the text of one FMinus module, which Program::compile compiles like any
// other source. It is the circuit compiler's first slice (Phase 0 of docs/research/analog-circuits/README.md, §11).
// No LLVM, C++17, as FMinus.h; the host links FMinus::FMinus.
//
//   const fminus::circuit::Result result = fminus::circuit::generate(netlist, "pedal.cir");
//   if (!result.errors.empty())
//       return report(result.errors);                     // "pedal.cir:7:1: error: ...", as clang words them
//   auto program = fminus::Program::compile(result.source, errors);
//
// The module (named by .title):
//   void init(float rate)        the host's rate; every state at the DC operating point (0 V: no bias in Phase 0),
//                                the pots at their .pot defaults
//   void set(float drive, ...)   the pots, 0 to 1, in .pot order (clamped): call it on the absolute 16-sample grid
//   float tick(float u)          one host-rate sample: .input's volts (1.0 = scale volts) to .output's
//   float step(float u)          one step of the oversampled region, at .oversample times the rate (absent if none)
//   void clear()                 back to the DC operating point
// Fields are named after the parts (x_C1: C1's state; v_D1: the voltage across the pair D1|D2). .channels 2 makes
// tick and step take and return float2s. The code is straight-line float code with fixed step counts: its output
// never depends on how a host splits blocks. generate() is pure and deterministic (the same text and options give
// the same Result, byte for byte, on any machine), safe on any thread, and allocates: call it off the audio thread.
#pragma once

#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace fminus
{
    // SPICE-style netlists to FMinus modules (above).
    namespace circuit
    {
        // How a nonlinear port's equation (v + 2 Is K sinh(v / N Vt) = p for a diode pair) is solved each step: a
        // first guess, then a fixed number of Newton steps, each kept in the bracket [min(0, p), max(0, p)].
        //   Omega     the closed form of the conducting diode alone (D'Angelo et al.'s omega3): .solver omega(K)
        //   Previous  the previous step's solution: .solver newton(K) (the report compares it; it fails at hard drive)
        struct Solver
        {
            // Where Newton starts.
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

        // Overrides of the netlist's directives: a member that is set wins over its directive, and one left unset
        // leaves the choice to the netlist, then to the default (docs/circuits.md).
        struct Options
        {
            std::string moduleName;             // the module's name; empty: the netlist's .title
            std::optional<int> oversample;      // 1, 2, 4 or 8 (.oversample; default 8)
            std::optional<Halfband> halfband;   // .oversample's iir or fir (default iir)
            std::optional<Region> region;       // .oversample's blocks= (default auto)
            std::optional<Solver> solver;       // .solver (default omega(2) without ADAA, omega(1) with)
            std::optional<bool> adaa;           // .adaa on|off (default off)
            std::optional<int> channels;        // .channels 1 or 2 (default 1)
        };

        // What generate() returns.
        struct Result
        {
            std::string source;              // the module's FMinus text; empty when there are errors
            std::string errors;              // "pedal.cir:7:1: error: ..." lines, each with its notes; empty on success
            std::string warnings;            // "pedal.cir:14:12: warning: ..." lines, the same format
            std::string notes;               // "pedal.cir: note: ..." lines: the blocks, rates, solvers, latency
            std::string module;              // the module's name, "DiodeTone"
            std::vector<std::string> pots;   // the pots in .pot order: set()'s arguments
        };

        // The FMinus module of `netlist`, the text of the file the messages call `fileName`.
        Result generate(std::string_view netlist, std::string_view fileName, const Options& options);

        // The same with default Options.
        Result generate(std::string_view netlist, std::string_view fileName);
    } // namespace circuit
} // namespace fminus
