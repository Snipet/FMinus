// src/support/HostTarget.h: this machine as a compilation target, shared by the JIT and ahead-of-time compilation: the
// host's TargetMachine, a module set up for it, and LLVM's default optimisation pipeline with its cost model. An
// integration piece that came with the project skeleton.
#pragma once

#include <llvm/Passes/OptimizationLevel.h>
#include <llvm/Support/CodeGen.h>
#include <llvm/Support/Error.h>

#include <memory>

namespace llvm
{
    class Module;
    class TargetMachine;
} // namespace llvm

namespace fminus
{
    // A TargetMachine for this machine's CPU and its features, which is what LLJIT targets (JITTargetMachineBuilder's
    // detectHost), generating code at `level`. Registers the host target with LLVM first (initializeLLVM).
    llvm::Expected<std::unique_ptr<llvm::TargetMachine>>
    hostTargetMachine(llvm::CodeGenOptLevel level = llvm::CodeGenOptLevel::Aggressive);

    // The code generation level that goes with an optimisation level (O0 None, O1 Less, O2 Default, O3 Aggressive).
    llvm::CodeGenOptLevel codeGenLevel(llvm::OptimizationLevel level);

    // Gives `module` the machine's triple and data layout. Do it before optimising: the passes read both.
    void prepareModule(llvm::Module& module, const llvm::TargetMachine& machine);

    // The module flag in which the code generator records how it lowered the math builtins (ir::Options::math): the
    // string "vector", "libm" or "fast".
    inline constexpr const char* kMathFlag = "fminus.math";

    // Runs LLVM's default pipeline at `level` (O0 to O3, as clang -O<n> would), with loop and SLP vectorisation, using
    // the machine's TargetTransformInfo: what lets the vectorisers see the host's vector unit. On Apple systems it also
    // gives the pipeline libsystem_m's vector math library, for a module whose kMathFlag is "fast" or that has none
    // (LLVM IR written by hand, as fminus-build takes): a module in the vector or the libm mode calls the runtime's
    // math functions instead, and the library would only make the loops of a prototype's libm function (atanf) give
    // one result in their vectorised body and another in their remainder.
    void optimiseModule(llvm::Module& module, llvm::TargetMachine& machine, llvm::OptimizationLevel level);

    // How many floats the machine's vector registers hold, as its cost model says (the width its vectorisers use): a
    // poly float's lanes (ir::Options). 4 with NEON or SSE, 8 with AVX2, 16 with AVX-512 (where LLVM prefers 512-bit
    // vectors: on AMD's Zen 4 and later, or with prefer-vector-width=512; on Intel's AVX-512 CPUs it prefers 256 bits,
    // so 8).
    unsigned polyLanes(const llvm::TargetMachine& machine);
} // namespace fminus
