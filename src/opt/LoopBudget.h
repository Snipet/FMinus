// src/opt/LoopBudget.h: the loop budget, runaway-loop protection, an LLVM pass of FMinus's own. A program that loops
// for ever, or for far longer than it should (a while whose condition never turns false, a for to a count read from
// the wrong place), would hold its caller's thread: in a plugin, the audio thread, and with it the host. Compiled with
// the budget (ir::Options::loopBudget, which ir::lower records as the module flag below), the program counts down a
// budget its host sets before each call, and when the count goes below zero the call returns early, with a flag raised
// that the host reads (Program::setLoopBudget and Program::loopBudgetExceeded, include/fminus/FMinus.h). A loop whose
// count ScalarEvolution knows where it starts is charged it there, before it runs; any other cycle one at each branch
// back to its header; and a function that can call itself again one where it starts.
//
// The pass runs last, after every other pass of the pipeline (support/HostTarget.cpp), so it changes no decision the
// optimiser makes: a loop vectorises, unrolls or disappears as it would without the budget, and the code it adds only
// counts and branches, so a call that stays within its budget computes what it would without it, bit for bit. What it
// counts is the optimised code's loops: a vectorised loop's branches back, one for each vector of samples, and its
// remainder's; nothing for a loop the optimiser unrolled or removed.
//
// The budget is the symbol fminus_rt_loop_budget, a { i64 left; i32 stopped } (runtime/Runtime.h), which the module
// defines (ir::lower, loopBudgetGlobal below), unlimited until the host sets it: in the JIT, each program's own. A
// function that counts reads `left` once where it starts and keeps it in a register, writes it back before each call
// that counts too and before it returns, and reads it again after such a call. When it runs out it writes `left`, sets
// `stopped` and returns zero, null or nothing, and each function that called it, seeing `stopped` after the call,
// returns at once too, so the state the program leaves is the one it had where it stopped: as if it had been paused
// there. Every access to the symbol is a relaxed (monotonic) atomic, so a parallel region's threads may share it (each
// counts its own copy of `left` from where its part starts, and `stopped` is shared).
#pragma once

#include <llvm/IR/PassManager.h>

namespace llvm
{
    class GlobalVariable;
    class Module;
} // namespace llvm

namespace fminus::opt
{
    // The module flag that turns the pass on: 1 (ir::lower writes it when ir::Options::loopBudget is set). A module
    // without it, or with 0, is left as it is.
    inline constexpr const char* kLoopBudgetFlag = "fminus.loop-budget";

    // The budget's symbol, which the generated code reads and writes (runtime/Runtime.h).
    inline constexpr const char* kLoopBudgetSymbol = "fminus_rt_loop_budget";

    // The string attribute of a function the pass changed, so that IR optimised once already (fminus-build at -O3, say)
    // is not counted twice.
    inline constexpr const char* kLoopBudgetAttribute = "fminus-loop-budget";

    // The string attribute of a function the pass leaves alone: the host's own, which sets the budget and reads the
    // flag (an executable's C main, support/Aot.h). A call of it is no call of the program's.
    inline constexpr const char* kLoopBudgetHostAttribute = "fminus-loop-budget-host";

    // The budget in `module`: a global of the type { i64, i32 }, defined there with an unlimited budget
    // ({ INT64_MAX, 0 }), and made if it is not there yet.
    llvm::GlobalVariable* loopBudgetGlobal(llvm::Module& module);

    // Counts down the budget in every function of a module that has the flag (LoopBudget.cpp says how), at every
    // level: -O0's code loops too, and ScalarEvolution counts none of its loops there (their counters are in memory),
    // so each counts a branch back at a time.
    struct LoopBudgetPass : llvm::PassInfoMixin<LoopBudgetPass>
    {
        llvm::PreservedAnalyses run(llvm::Module& module, llvm::ModuleAnalysisManager& analyses);

        static bool isRequired()
        {
            return true;   // at -O0 too, and in a function marked optnone
        }
    };
} // namespace fminus::opt
