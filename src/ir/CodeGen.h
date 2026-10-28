// src/ir/CodeGen.h: FMinus v0's syntax tree to LLVM IR.
#pragma once

#include "frontend/Ast.h"
#include "frontend/Diagnostics.h"

#include <map>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace llvm
{
    class LLVMContext;
    class Module;
} // namespace llvm

namespace fminus::ir
{
    // How a program reads a module's parameter (`param float cutoff;`), the host's choice when it compiles:
    //   Knob     its value slot, read once a call with an atomic load where a method starts: the host writes it
    //            whenever it likes, from any thread (the default)
    //   Stream   the samples its stream slot points to, cutoff[i] the i-th: the host sets the pointer before each call
    //   Frozen   `value`, a constant compiled in, so the code around it folds (a frozen `mode` picks one branch)
    // Whatever the binding, the parameter's slot in the module is the same: { const float* stream; T value; }, T the
    // parameter's type (bool a byte). So a program compiled with other bindings runs on the same instances.
    struct ParamBinding
    {
        enum class Kind
        {
            Knob,
            Stream,
            Frozen,
        };
        Kind kind = Kind::Knob;
        double value = 0.0;   // a frozen parameter's, converted to its type as C converts
    };

    // What a program does with subnormal floats (Denormals.cpp), the host's choice when it compiles:
    //   Preserve  IEEE's gradual underflow, as C has it: a subnormal is a number like any other (the default)
    //   Flush     flush-to-zero, as a plugin host usually runs: a subnormal input reads as 0, a subnormal result is 0,
    //             each with its sign. Every function the program defines says so to LLVM (denormal_fpenv), so its
    //             folding agrees with the hardware, and each entry point (a [[clang::nonblocking]] function that no
    //             function of the program calls) sets the floating-point mode on entry and restores it on return.
    //             Whatever else a host calls, it calls with the mode set itself.
    enum class Denormals
    {
        Preserve,
        Flush,
    };

    // How sinf, cosf, tanf, expf, tanhf and logf compute, scalar or vector (ir/Math.cpp lowers them, runtime/Math.cpp
    // computes them). fabsf, sqrtf, floorf, fminf and fmaxf are exact, so they are LLVM's intrinsics in every mode.
    //   Vector   FMinus's own kernels (runtime/MathKernels.h), which give a float the same result wherever it is: in
    //            a scalar loop or its vectorised body, a float4 or a poly float of any width, at any optimisation
    //            level, a constant or not, on any target. Each is one sequence of IEEE operations, which every lane
    //            computes alike: inline from -O1 (opt/MathKernels.h), and the runtime's functions at -O0
    //            (runtime/Runtime.h, fminus_rt_sinf and fminus_rt_sin_f4). Loops of them still vectorise. The
    //            default.
    //   Libm     libm's sinf, cosf, ... (through the runtime, so that nothing folds them), a lane at a time: C++'s
    //            std::sin of a float, bit for bit, everywhere. Loops of them don't vectorise.
    //   Fast     LLVM's intrinsics (llvm.sin, ...), as clang has them: LLVM folds constants with the host's double
    //            functions, pairs sinf and cosf into one sincos call, and the vectorisers and ReplaceWithVeclib use the
    //            target's vector math library (support/HostTarget.h), so a result can depend on where a block ends,
    //            on the optimisation level, on a poly float's width and on whether a parameter is frozen.
    // The module records the mode in its kMathFlag (support/HostTarget.h), which tells optimiseModule whether to give
    // the pipeline the vector math library: only Fast has it.
    enum class Math
    {
        Vector,
        Libm,
        Fast,
    };

    struct Options
    {
        // The lanes of a poly float: the target's vector register width in floats (support/HostTarget.h: polyLanes
        // asks the TargetMachine). 4 is NEON's and SSE's; any power of two from 1 to 64 lowers, wider ones in parts.
        unsigned polyLanes = 4;

        // How the transcendental builtins compute (above).
        Math math = Math::Vector;

        // The target's data layout (TargetMachine::createDataLayout), which a module's layout and sizeof follow. Empty
        // is LLVM's default, which agrees with the 64-bit targets FMinus runs on for every type it has.
        std::string dataLayout{};

        // Each parameter's binding, by its qualified name ("Lowpass::cutoff"); a parameter not listed is a knob.
        std::map<std::string, ParamBinding> parameters{};

        // C's strict-aliasing rule, told to LLVM: every load and store of memory carries its type's TBAA tag
        // (Tbaa.cpp), so a store of a float cannot change an int or a pointer, and a loop over a module's buffer loads
        // the module's fields once. Off, clang's -fno-strict-aliasing, nothing is tagged, and LLVM assumes any store
        // may change anything: for a program that reads memory as a type other than the one it holds.
        bool strictAliasing = true;
        // Knob versioning's budget (opt/KnobVersioning.h), from -O1: at most this many copies of a method's body, one
        // for each combination of the values of the int and bool knobs (and the streams' flat tests) its loops branch
        // on, so that each copy folds them as constants. 0 turns it off. It travels with the module, as the module flag
        // "fminus.knob-versions", to wherever the module is optimised. The copies compute what the method does, bit
        // for bit, in the Vector and Libm math modes; only Math::Fast can move a math call's last bits, where a copy
        // vectorises a loop the method did not, or folds a knob's constant into a call (opt/KnobVersioning.h).
        unsigned knobVersions = 8;
        // Subnormals preserved or flushed to zero (Denormals, above).
        Denormals denormals = Denormals::Preserve;

        // The target's triple (TargetMachine::getTargetTriple), which says how an entry point sets the floating-point
        // mode under Denormals::Flush: FPCR on arm64, MXCSR on x86-64. Empty is this process's, which is what the JIT
        // and an executable built here run on. Only Flush reads it.
        std::string targetTriple{};

        // The most threads a parallel for or parallel { } runs on, the calling thread's among them (Parallel.cpp):
        // 0 is as many as the runtime's pool has, 1 runs each region on the calling thread alone. Each region hands
        // it to fminus_rt_parallel_for (runtime/Runtime.h), which also runs every region alone until the pool starts.
        unsigned threads = 0;
    };

    // Lowers `program` into a new module in `context`, named after the diagnostics' file. Every FMinus function becomes
    // an external LLVM function of the same name with the C calling convention (float is f32), so C++ calls the JIT'd
    // or linked code as float(float, ...). Returns nullptr when it reported errors into `diagnostics`; it reports every
    // error it finds, not only the first.
    std::unique_ptr<llvm::Module> lower(const ast::Program& program, llvm::LLVMContext& context,
                                        Diagnostics& diagnostics, const Options& options = {});

    // The builtins a program can call, with their parameter counts: <math.h>'s float functions (fabsf, sqrtf,
    // sinf, ...), lowered to LLVM intrinsics, or the transcendental ones to the runtime's functions (Math).
    struct BuiltinInfo
    {
        std::string_view name;
        unsigned arity;
    };
    std::vector<BuiltinInfo> builtins();
} // namespace fminus::ir
