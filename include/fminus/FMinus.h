// include/fminus/FMinus.h: FMinus as a C++ library. A Program compiles FMinus source while the host runs, for the CPU
// it runs on, and gives its functions as C function pointers. This is the one header a host includes: it has no LLVM
// in it and needs C++17, and the host links FMinus::FMinus (README.md, "Using FMinus as a library").
//
//   std::string errors;
//   auto program = fminus::Program::compile(source, errors);           // nullptr when it does not compile
//   auto* gain = program->function<void(const float*, float*, int, float)>("gain", errors);
//   gain(in, out, n, 0.5f);                                             // one call a block: the loop is FMinus's
//
// A function's C++ type is its FMinus declaration's C type, and FMinus checks nothing at this boundary: a Signature
// that does not match the declaration is undefined behaviour. Every function has C's calling convention and its own
// name, unmangled.
//
//   FMinus                         C++
//   float, int, bool, void         float, int, bool, void
//   float*, const int*, void*, ... the same pointer (restrict is a promise to FMinus's optimiser, not a type)
//   float2, float4                 clang's float __attribute__((ext_vector_type(2))), (4)
//   float8                         none: the C ABI passes it in memory, FMinus in registers (docs/simd.md); keep it
//                                  inside FMinus
//   poly float                     the vector of polyLanes() floats: a float4 at 4 lanes (NEON's), and at 8 or more
//                                  none, as float8; keep it inside FMinus too
//   OnePole* (a module's pointer)  void*, or a pointer to a C++ struct with the same fields in the same order (which
//                                  C++ lays out as FMinus does); module() says where each field is. A module itself
//                                  never crosses: FMinus passes and returns modules by pointer only
//
// A module's method is a function too, named as C++ qualifies it, whose first parameter is the instance it runs on:
// `float tick(float x)` in `module OnePole` is function<float(void*, float)>("OnePole::tick", errors).
//
// Threads: compile(), and the first look-up of each function, run LLVM's optimiser and code generator on the calling
// thread. They take milliseconds and allocate, so in a plugin they run off the audio thread, and the pointer is handed
// over (an std::atomic does). Calling a JIT'd function is a plain call; mark the ones the audio thread calls
// [[clang::nonblocking]], and FMinus refuses any that could reach malloc, calloc or free. Different Programs may
// compile and look up on different threads at once.
//
// Lifetime: a function pointer is valid while its Program lives. Destroy a Program only when no thread can still be
// running its code.
//
// Parameters: a module's `param float cutoff;` is a value the host sets, read in a method as cutoff[i], at sample i of
// the call. How it reads is the host's choice when it compiles (Options::parameters, Binding): a knob (the value, read
// once when the method starts, and safe to set from any thread), a stream (a buffer of samples, an LFO's say, or null
// for a call in which it holds still at its value), or frozen (a constant compiled in). Module::parameters describes
// each; setValue, setStream and setDefaults write them.
//
//   Program::Options options;
//   options.parameters["Lowpass::cutoff"] = Program::Binding::stream();   // modulated: a sample each
//   auto program = Program::compile(source, errors, options);
//   const Program::Parameter* cutoff = program->parameter("Lowpass::cutoff");
//   Program::setStream(lowpass, *cutoff, lfo);                            // before each call of Lowpass::process
#pragma once

#include "fminus/Config.h"   // IWYU pragma: export (FMINUS_VERSION_STRING, the LLVM this build links)

#include <cstddef>
#include <cstdint>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <type_traits>
#include <vector>

namespace fminus
{
    class Program
    {
    public:
        // How a module's parameter reads in its methods, chosen when the program compiles. Every binding has the same
        // slot in an instance (Parameter), so one instance can be run by programs that bind it differently.
        //   Knob    the slot's value, loaded once when the method starts, with an atomic load: the host may set it
        //           from any thread at any time (setValue), and the method sees one value it set, never a torn one.
        //           Held in a register from then on, as fast as a local. The default.
        //   Stream  a float parameter's buffer: cutoff[i] is samples[i], from the pointer the host sets before each
        //           call (setStream). Sample-accurate: an audio-rate signal, an envelope, a smoothed knob. A null
        //           pointer is a flat call: cutoff[i] is then the slot's value (setValue), read as a knob's, so a host
        //           whose smoother is idle passes no buffer, and the method runs its flat copy (Options::knobVersions),
        //           with what it computes from the parameter out of its loop.
        //   Frozen  `value`, compiled in as a constant: a knob that will not move while this program plays. The
        //           optimiser folds it as it folds a constant (a branch on a frozen bool disappears); the host
        //           compiles again to move it.
        struct Binding
        {
            enum class Kind
            {
                Knob,
                Stream,
                Frozen,
            };

            Kind kind = Kind::Knob;
            double value = 0.0;   // a frozen parameter's: an int's truncated toward 0, a bool's true unless 0

            static Binding knob()
            {
                return {};
            }

            static Binding stream()
            {
                return {Kind::Stream, 0.0};
            }

            static Binding frozen(double value)
            {
                return {Kind::Frozen, value};
            }
        };

        // What the program does with subnormal floats, chosen when it compiles.
        //   Preserve  IEEE's gradual underflow, as C has it: a subnormal is a number like any other. The default.
        //   Flush     flush-to-zero, the mode a plugin's audio thread runs in: a subnormal input reads as 0 and a
        //             subnormal result is 0, each with its sign. LLVM folds and optimises for that mode, and each entry
        //             point, a function marked [[clang::nonblocking]] that no function of the program calls, sets it
        //             when called and restores the caller's on return (FPCR.FZ on arm64, MXCSR's FTZ and DAZ on
        //             x86-64). Writing the register is slow (about 18 ns a call on an M5), so an entry point called
        //             in that mode already writes nothing (under 1 ns): set it around the block, as a host's ScopedFtz
        //             does, and call anything else in it too. The code assumes the mode: run in IEEE's, a subnormal's
        //             result may differ from what the compiler folded.
        enum class Denormals
        {
            Preserve,
            Flush,
        };

        struct Options
        {
            int optimisation = 2;                  // -O0 to -O3: LLVM's pipeline at that level, as fminus -O<n>
            unsigned polyLanes = 0;                // a poly float's lanes (1, 2, 4, ... 64); 0 is this machine's
            std::string fileName = "program.fm";   // what the error messages call the source
            // Each parameter's binding, by its qualified name ("Lowpass::cutoff"); one not named is a knob. A name
            // that is no parameter of the program, or a stream that is not a float, is a compile error.
            std::map<std::string, Binding> parameters;
            // C's strict-aliasing rule, which the optimiser relies on: the program reads and writes memory only as the
            // type it holds (a float vector as floats). Off, as clang's -fno-strict-aliasing, for a program that
            // reads memory as another type; its loops over a module's buffers may then run several times slower.
            bool strictAliasing = true;
            // Knob versioning, from -O1: a method whose loop branches on a bool knob, an int knob whose fminus::range
            // holds at most 8 integers, or whether a stream is flat (setStream) gets a copy of its code for each of
            // their values, picked when it starts, in which the value is a constant, folded as a frozen one is (an int
            // outside its range runs the method as written). At most this many copies a method; 0 turns it off. The copies cost code size and compile time, and compute what the method
            // does, bit for bit, but for math functions' last bits: a copy's loop may vectorise where the method's
            // did not, and a vectorised sinf, expf, ... is the vector library's, not libm's.
            unsigned knobVersions = 8;
            Denormals denormals = Denormals::Preserve;   // subnormals kept, or flushed to zero (above)
        };

        // Compiles `source`. Returns nullptr when it does not compile, with `errors` holding FMinus's messages, one a
        // line, as clang words them (`program.fm:3:12: error: use of undeclared identifier 'x'`), notes included.
        // `errors` is empty on success.
        static std::unique_ptr<Program> compile(std::string_view source, std::string& errors, const Options& options);
        static std::unique_ptr<Program> compile(std::string_view source, std::string& errors);

        ~Program();
        Program(const Program&) = delete;
        Program& operator=(const Program&) = delete;

        // The function `name`, as a pointer of type Signature*, which must be its declaration's C type (above). It is
        // compiled now, on this thread, if it has not been yet. nullptr, with `errors` set, when the program has no
        // such function.
        template <typename Signature>
        Signature* function(std::string_view name, std::string& errors)
        {
            static_assert(std::is_function_v<Signature>, "Signature is a function type, such as float(float)");
            return reinterpret_cast<Signature*>(address(name, errors));
        }

        // The function's address, or 0 (with `errors` set).
        std::uintptr_t address(std::string_view name, std::string& errors);

        // How many lanes a poly float has in this program: Options::polyLanes, or this machine's vector width in
        // floats (4 with NEON or SSE, 8 with AVX2).
        unsigned polyLanes() const;

        // Whether a declaration of function `name` is marked [[clang::nonblocking]]. FMinus has then checked that it
        // cannot reach malloc, calloc, free or an external function not marked nonblocking itself, so it may run on
        // the audio thread.
        bool nonBlocking(std::string_view name) const;

        // Whether a declaration of function `name` is marked [[clang::nonallocating]] or [[clang::nonblocking]]
        // (which includes it). FMinus has then checked that it cannot reach malloc, calloc or free.
        bool nonAllocating(std::string_view name) const;

        // A module's parameter, `param float cutoff [[fminus::range(20, 20000)]] [[fminus::default(1000)]];`. Its
        // slot in an instance is the C struct { const float* stream; T value; }, T a float, an int32_t or a bool (a
        // byte), whatever its binding: the stream's pointer at streamOffset, the value at valueOffset.
        struct Parameter
        {
            std::string module;                   // "Lowpass"
            std::string name;                     // "cutoff"
            std::string type;                     // "float", "int" or "bool"
            Binding binding;                      // as this program reads it
            std::size_t streamOffset = 0;         // in bytes, from the instance's address
            std::size_t valueOffset = 0;
            std::optional<double> minimum;        // fminus::range(minimum, maximum): what the host's control spans;
            std::optional<double> maximum;        // FMinus neither clamps to it nor checks it
            std::optional<double> defaultValue;   // fminus::default(value): what setDefaults sets
        };

        // A module of the program (FMinus's class), as a host sees it. An instance is `size` bytes, aligned to
        // `alignment` and zeroed, since every field starts at 0 (std::aligned_alloc and std::memset, or a value-
        // initialised C++ struct with the same fields); a method then runs on its address (above). Nothing else is
        // needed to make one: FMinus runs no constructor. A module with parameters wants them set too (setDefaults).
        struct Module
        {
            struct Field
            {
                std::string name;
                std::string type;          // as FMinus writes it: "float", "float4", "float[16]", "Osc", "Osc *";
                                           // a parameter's "param float" (its slot)
                std::size_t offset = 0;    // in bytes, from the instance's address
                std::size_t size = 0;
            };

            std::string name;
            std::size_t size = 0;
            std::size_t alignment = 1;
            std::vector<Field> fields;              // in declaration order
            std::vector<std::string> methods;       // as function() takes them: "OnePole::tick"
            std::vector<Parameter> parameters;      // the fields that are parameters, in declaration order
        };

        // The module `name`, or nullptr when the program has none.
        const Module* module(std::string_view name) const;

        // Every module of the program, in declaration order.
        const std::vector<Module>& modules() const;

        // The parameter `name`, qualified ("Lowpass::cutoff"), or nullptr when the program has none.
        const Parameter* parameter(std::string_view name) const;

        // Sets a parameter's value in `instance`, an instance of its module: a relaxed atomic store, from any thread
        // at any time, which a method reads when it next starts. A float's rounded, an int's truncated toward 0 (and
        // clamped to an int's range), a bool's true unless 0. A frozen parameter's methods do not read it, and a
        // stream's read it only in a call whose stream is null (setStream). No allocation, no lock: the audio thread
        // may call it.
        static void setValue(void* instance, const Parameter& parameter, double value) noexcept;

        // The value setValue last set (a relaxed atomic load), as a double.
        static double value(const void* instance, const Parameter& parameter) noexcept;

        // Points a stream parameter at its samples for the next call: as many as the call reads, which a method that
        // processes n samples, reading cutoff[i] for i below n, is n. The thread that calls the method sets it,
        // before the call; the method reads it when it starts. nullptr makes the call flat: every cutoff[i] is then
        // the slot's value, as setValue last set it, and no sample is read. A knob's or a frozen parameter's methods
        // do not read it.
        static void setStream(void* instance, const Parameter& parameter, const float* samples) noexcept;

        // Sets each of the module's parameters that has an fminus::default to it (setValue), in `instance`.
        static void setDefaults(void* instance, const Module& module) noexcept;

    private:
        struct Impl;
        explicit Program(std::unique_ptr<Impl> impl);

        std::unique_ptr<Impl> impl_;
    };
} // namespace fminus
