// src/opt/EarlyReturn.h: what the passes that make a call of the program return early share, the loop budget's
// (opt/LoopBudget.h) and the bounds checks' (opt/BoundsCheck.h): which of the program's functions a call runs, what a
// function that may now return early no longer promises, and what it returns then.
#pragma once

#include <llvm/ADT/SmallVector.h>

namespace llvm
{
    class CallBase;
    class Function;
    class IRBuilderBase;
} // namespace llvm

namespace fminus::opt
{
    // The program's functions that `call` runs: its callee, when the module defines it, or the functions the module
    // defines that it hands a declared function (a parallel region's body, to fminus_rt_parallel_for), which go in
    // `handed` too when it is given. Inline asm runs nothing, and an indirect call is not followed: FMinus has no
    // function pointers, and the only indirect calls it makes are an entry point's call of its body under
    // Denormals::Flush (ir/Denormals.cpp), whose wrapper restores the floating-point mode and returns what the body
    // returned, which is what a body that returned early should have its wrapper do, and an executable's C main's call
    // of main (support/Aot.cpp), which reads the passes' records itself.
    void reachedFunctions(llvm::CallBase& call, llvm::SmallVectorImpl<llvm::Function*>& functions,
                          llvm::SmallVectorImpl<llvm::Function*>* handed = nullptr);

    // Makes `function`, which may now return early, promise only what is still true of it: it reads and writes memory
    // that is no argument's (the pass's global), it returns (noreturn goes: a loop that never ended, or a function
    // that always failed a check, returns now), and its result may be 0 or null, so no attribute that says otherwise
    // (nonnull, dereferenceable, a range, nofpclass, an argument it returns) stays on it.
    void mayReturnEarly(llvm::Function& function);

    // The same of a call of such a function, wherever it is: the attributes the call carries itself.
    void mayReturnEarly(llvm::CallBase& call);

    // Returns, at `at`, what `function` returns when it stops: nothing, zero or null.
    void returnEarly(llvm::IRBuilderBase& at, const llvm::Function& function);
} // namespace fminus::opt
