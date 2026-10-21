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
#pragma once

#include "fminus/Config.h"   // IWYU pragma: export (FMINUS_VERSION_STRING, the LLVM this build links)

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <string_view>
#include <type_traits>
#include <vector>

namespace fminus
{
    class Program
    {
    public:
        struct Options
        {
            int optimisation = 2;                  // -O0 to -O3: LLVM's pipeline at that level, as fminus -O<n>
            unsigned polyLanes = 0;                // a poly float's lanes (1, 2, 4, ... 64); 0 is this machine's
            std::string fileName = "program.fm";   // what the error messages call the source
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

        // A module of the program (FMinus's class), as a host sees it. An instance is `size` bytes, aligned to
        // `alignment` and zeroed, since every field starts at 0 (std::aligned_alloc and std::memset, or a value-
        // initialised C++ struct with the same fields); a method then runs on its address (above). Nothing else is
        // needed to make one: FMinus runs no constructor.
        struct Module
        {
            struct Field
            {
                std::string name;
                std::string type;          // as FMinus writes it: "float", "float4", "float[16]", "Osc", "Osc *"
                std::size_t offset = 0;    // in bytes, from the instance's address
                std::size_t size = 0;
            };

            std::string name;
            std::size_t size = 0;
            std::size_t alignment = 1;
            std::vector<Field> fields;          // in declaration order
            std::vector<std::string> methods;   // as function() takes them: "OnePole::tick"
        };

        // The module `name`, or nullptr when the program has none.
        const Module* module(std::string_view name) const;

        // Every module of the program, in declaration order.
        const std::vector<Module>& modules() const;

    private:
        struct Impl;
        explicit Program(std::unique_ptr<Impl> impl);

        std::unique_ptr<Impl> impl_;
    };
} // namespace fminus
