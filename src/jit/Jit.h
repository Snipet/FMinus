// src/jit/Jit.h: ORC's LLJIT set up for FMinus modules. It targets this machine's CPU, optimises every module with
// LLVM's default pipeline at a chosen level (support/HostTarget.h), or takes them optimised already, and defines the
// FMinus runtime (runtime/Runtime.h).
//
// LLJIT compiles lazily, on the thread that looks a symbol up: look up from a background thread, never the audio
// thread, and hand the address over. Process symbols (libm's sinf for the sin builtin, ...) resolve too; a plugin may
// later want only the runtime's.
//
// It also keeps where each function's machine code is (code()), for a host's sampling profiler: as each object links,
// a plugin of JITLink's (or, with RuntimeDyld, a JITEventListener) writes down its functions' addresses and sizes.
#pragma once

#include <llvm/ExecutionEngine/Orc/Shared/ExecutorAddress.h>
#include <llvm/Passes/OptimizationLevel.h>
#include <llvm/Support/Error.h>

#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <string>
#include <vector>

namespace llvm
{
    class JITEventListener;
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
    struct CodeLog;   // where each function's code is, as the linker's plugin or listener writes it down (Jit.cpp)

    class Jit
    {
    public:
        // How the modules add() takes arrive.
        //   Unoptimised  as the code generator wrote them: the JIT optimises each (optimiseModule, at its level) when
        //                one of its symbols is first looked up, in LLJIT's IR transform layer. The default.
        //   Optimised    optimised already, at the JIT's level (prepareModule and optimiseModule, on a machine made as
        //                the JIT's is), as fminus::Program optimises a module whose IR the host keeps as text
        //                (Program::Options::keepIr): the JIT generates the code from each as it is.
        enum class Modules
        {
            Unoptimised,
            Optimised,
        };

        // Which linker places the code in memory and resolves its references.
        //   Default      LLJIT's for the target: JITLink on macOS and Linux, RuntimeDyld on Windows.
        //   RuntimeDyld  RuntimeDyld on any target, as on Windows: for the tests, which check code() through both.
        enum class Linker
        {
            Default,
            RuntimeDyld,
        };

        static llvm::Expected<std::unique_ptr<Jit>> create(llvm::OptimizationLevel level = llvm::OptimizationLevel::O3,
                                                           Modules modules = Modules::Unoptimised,
                                                           Linker linker = Linker::Default);
        ~Jit();
        Jit(const Jit&) = delete;
        Jit& operator=(const Jit&) = delete;

        // Adds `module` (and the context it lives in): set up for the JIT's target here, optimised (unless it came
        // optimised: Modules) and compiled when one of its symbols is first looked up.
        llvm::Error add(std::unique_ptr<llvm::Module> module, std::unique_ptr<llvm::LLVMContext> context);

        // The address of `name` (an FMinus function's name), compiling what it needs on this thread.
        llvm::Expected<llvm::orc::ExecutorAddr> lookup(llvm::StringRef name);

        // One function's machine code, as the JIT linked it.
        struct Code
        {
            std::string name;                   // the function's name in its module: "Osc::tick", "run.parallel.1"
            std::uint64_t address = 0;          // its first byte, in this process
            std::uint64_t size = 0;             // its bytes, from there
            std::vector<std::string> inlined;   // the module's functions the optimiser inlined into it (inlinedCalls)
        };

        // The machine code the JIT has linked so far, a function at a time, sorted by address: each symbol with a name
        // in an executable section of each object it linked, its name as the module has it (the object's has the
        // target's global prefix, macOS's '_'; an assembler's labels, ltmp0, are none of the module's). JITLink sizes a
        // symbol from its graph (a Mach-O object's symbols have no size: each runs to the next, its padding included),
        // RuntimeDyld from the object's symbol table (object::computeSymbolSizes). A function's code is where it was
        // linked, and stays there while the Jit lives: the JIT neither moves nor frees it. Any thread may call this.
        std::vector<Code> code() const;

        // The direct calls between a module's functions: for each function the module defines, the functions it calls
        // by name that the module defines too, itself not among them, sorted.
        using Calls = std::map<std::string, std::vector<std::string>, std::less<>>;
        static Calls directCalls(const llvm::Module& module);

        // The calls the optimiser inlined, from a module's direct calls before and after it optimised the module: G
        // is inlined into F when F called G before and does not after, and so, through G, is each function G called
        // that F does not call after either. A call the optimiser deleted as dead looks the same. Each function of
        // `after` that inlined any, with what it inlined, sorted.
        static Calls inlinedCalls(const Calls& before, const Calls& after);

        // Writes down a module's inlinedCalls(), for code(). The JIT does this for each module it optimises
        // (Modules::Unoptimised); for one that comes optimised, its optimiser's caller does.
        void recordInlining(const Calls& inlined);

    private:
        Jit() = default;

        llvm::OptimizationLevel level_ = llvm::OptimizationLevel::O3;
        std::unique_ptr<llvm::TargetMachine> machine_;       // the pipeline's cost model; outlives jit_
        std::shared_ptr<CodeLog> log_;                       // shared with the linker's plugin; outlives jit_
        std::unique_ptr<llvm::JITEventListener> listener_;   // RuntimeDyld's, which its layer holds by reference
        std::unique_ptr<llvm::orc::LLJIT> jit_;
    };
} // namespace fminus::jit
