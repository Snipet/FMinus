// src/ir/CodeGen.h: FMinus v0's syntax tree to LLVM IR.
#pragma once

#include "frontend/Ast.h"
#include "frontend/Diagnostics.h"

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
    struct Options
    {
        // The lanes of a poly float: the target's vector register width in floats (support/HostTarget.h: polyLanes
        // asks the TargetMachine). 4 is NEON's and SSE's; any power of two from 1 to 64 lowers, wider ones in parts.
        unsigned polyLanes = 4;

        // The target's data layout (TargetMachine::createDataLayout), which a module's layout and sizeof follow. Empty
        // is LLVM's default, which agrees with the 64-bit targets FMinus runs on for every type it has.
        std::string dataLayout;
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
