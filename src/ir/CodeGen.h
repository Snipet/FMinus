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

    struct Options
    {
        // The lanes of a poly float: the target's vector register width in floats (support/HostTarget.h: polyLanes
        // asks the TargetMachine). 4 is NEON's and SSE's; any power of two from 1 to 64 lowers, wider ones in parts.
        unsigned polyLanes = 4;

        // The target's data layout (TargetMachine::createDataLayout), which a module's layout and sizeof follow. Empty
        // is LLVM's default, which agrees with the 64-bit targets FMinus runs on for every type it has.
        std::string dataLayout;

        // Each parameter's binding, by its qualified name ("Lowpass::cutoff"); a parameter not listed is a knob.
        std::map<std::string, ParamBinding> parameters;

        // C's strict-aliasing rule, told to LLVM: every load and store of memory carries its type's TBAA tag
        // (Tbaa.cpp), so a store of a float cannot change an int or a pointer, and a loop over a module's buffer loads
        // the module's fields once. Off, clang's -fno-strict-aliasing, nothing is tagged, and LLVM assumes any store
        // may change anything: for a program that reads memory as a type other than the one it holds.
        bool strictAliasing = true;
        // Knob versioning's budget (opt/KnobVersioning.h), from -O1: at most this many copies of a method's body, one
        // for each combination of the values of the int and bool knobs (and the streams' flat tests) its loops branch
        // on, so that each copy folds them as constants. 0 turns it off. It travels with the module, as the module flag
        // "fminus.knob-versions", to wherever the module is optimised. The copies compute what the method does, but
        // for math calls' last bits where a copy vectorises a loop the method did not (opt/KnobVersioning.h).
        unsigned knobVersions = 8;
    };

    // Lowers `program` into a new module in `context`, named after the diagnostics' file. Every FMinus function becomes
    // an external LLVM function of the same name with the C calling convention (float is f32), so C++ calls the JIT'd
    // or linked code as float(float, ...). Returns nullptr when it reported errors into `diagnostics`; it reports every
    // error it finds, not only the first.
    std::unique_ptr<llvm::Module> lower(const ast::Program& program, llvm::LLVMContext& context,
                                        Diagnostics& diagnostics, const Options& options = {});

    // The builtins a program can call, with their parameter counts: <math.h>'s float functions (fabsf, sqrtf,
    // sinf, ...), lowered to LLVM intrinsics.
    struct BuiltinInfo
    {
        std::string_view name;
        unsigned arity;
    };
    std::vector<BuiltinInfo> builtins();
} // namespace fminus::ir
