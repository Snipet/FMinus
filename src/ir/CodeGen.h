// src/ir/CodeGen.h: FMinus v0's syntax tree to LLVM IR.
#pragma once

#include "frontend/Ast.h"
#include "frontend/Diagnostics.h"

#include <memory>
#include <string_view>
#include <vector>

namespace llvm
{
    class LLVMContext;
    class Module;
} // namespace llvm

namespace fminus::ir
{
    // Lowers `program` into a new module in `context`, named after the diagnostics' file. Every FMinus function becomes
    // an external LLVM function of the same name with the C calling convention (float is f32), so C++ calls the JIT'd
    // or linked code as float(float, ...). Returns nullptr when it reported errors into `diagnostics`; it reports every
    // error it finds, not only the first.
    std::unique_ptr<llvm::Module> lower(const ast::Program& program, llvm::LLVMContext& context,
                                        Diagnostics& diagnostics);

    // The builtins a program can call (LLVM intrinsics: abs, sqrt, sin, ...), with their parameter counts.
    struct BuiltinInfo
    {
        std::string_view name;
        unsigned arity;
    };
    std::vector<BuiltinInfo> builtins();
} // namespace fminus::ir
