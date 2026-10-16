// src/jit/Jit.h: ORC's LLJIT set up for FMinus modules. It targets this machine's CPU, optimises every module with
// LLVM's default pipeline at a chosen level (support/HostTarget.h) and defines the FMinus runtime (runtime/Runtime.h).
//
// LLJIT compiles lazily, on the thread that looks a symbol up: look up from a background thread, never the audio
// thread, and hand the address over. Process symbols (libm's sinf for the sin builtin, ...) resolve too; a plugin may
// later want only the runtime's.
#pragma once

#include <llvm/ExecutionEngine/Orc/Shared/ExecutorAddress.h>
#include <llvm/Passes/OptimizationLevel.h>
#include <llvm/Support/Error.h>

#include <memory>

namespace llvm
{
    class LLVMContext;
    class Module;
    class TargetMachine;
    namespace orc
    {
        class LLJIT;
    } // namespace orc
} // namespace llvm

namespace fminus::jit
{
    class Jit
    {
    public:
        static llvm::Expected<std::unique_ptr<Jit>> create(llvm::OptimizationLevel level = llvm::OptimizationLevel::O3);
        ~Jit();
        Jit(const Jit&) = delete;
        Jit& operator=(const Jit&) = delete;

        // Adds `module` (and the context it lives in): set up for the JIT's target here, optimised and compiled when
        // one of its symbols is first looked up.
        llvm::Error add(std::unique_ptr<llvm::Module> module, std::unique_ptr<llvm::LLVMContext> context);

        // The address of `name` (an FMinus function's name), compiling what it needs on this thread.
        llvm::Expected<llvm::orc::ExecutorAddr> lookup(llvm::StringRef name);

    private:
        Jit() = default;

        llvm::OptimizationLevel level_ = llvm::OptimizationLevel::O3;
        std::unique_ptr<llvm::TargetMachine> machine_;   // the pipeline's cost model; outlives jit_
        std::unique_ptr<llvm::orc::LLJIT> jit_;
    };
} // namespace fminus::jit
