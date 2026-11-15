// src/opt/EarlyReturn.h: what the passes that make a call of the program return early share, the loop budget's
// (opt/LoopBudget.h) and the bounds checks' (opt/BoundsCheck.h): which of the program's functions a call runs, what a
// function that may now return early no longer promises, and what it returns then, deadline scopes abandoned first.
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

    // A within block's deadline scope (ir/Within.cpp) is its thread's, which the web library finds by the thread's id
    // (web/Web.h): one begun and never ended would stay the thread's, bounding its later waits by a deadline long past
    // and holding its later requests, and a later thread given the same id would find it. A stopped call reaches no
    // block's end, so every way out a pass makes in a function that begins one (a call of kWithinBeginSymbol) first
    // abandons the thread's scopes under the program's context (kWithinAbandonSymbol, which neither waits nor locks).
    // That is enough: a scope's function either stops itself or returns after the call that stopped, through one of
    // those ways out, on the scope's own thread; and every function between it and the host returns too, so every
    // scope its thread has open is dead. A module without such a call is left as it was.
    inline constexpr const char* kWithinBeginSymbol = "fminus_web_http_within_begin";
    inline constexpr const char* kWithinAbandonSymbol = "fminus_web_http_within_abandon";
    inline constexpr const char* kWebContextGlobal = "fminus_web_context";   // ir::kWebContextSymbol

    // Returns, at `at`, what `function` returns when it stops: nothing, zero or null; in a function that begins a
    // deadline scope, after abandoning the thread's scopes (above).
    void returnEarly(llvm::IRBuilderBase& at, llvm::Function& function);
} // namespace fminus::opt
