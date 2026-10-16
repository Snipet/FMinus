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

    // Gives `module` the machine's triple and data layout. Do it before optimising: the passes read both.
    void prepareModule(llvm::Module& module, const llvm::TargetMachine& machine);

    // Runs LLVM's default pipeline at `level` (O0 to O3, as clang -O<n> would), with loop and SLP vectorisation, using
    // the machine's TargetTransformInfo: what lets the vectorisers see the host's vector unit.
    void optimiseModule(llvm::Module& module, llvm::TargetMachine& machine, llvm::OptimizationLevel level);
} // namespace fminus
