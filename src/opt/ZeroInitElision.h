// src/opt/ZeroInitElision.h: a function pass that removes the memset which zeroes a local array, when the program
// overwrites every element it reads before reading it. FMinus starts every variable at 0 (a deviation from C, where a
// local is indeterminate), so the code generator zeroes each array at its declaration, every time the declaration runs.
// LLVM keeps that memset whenever the writes after it cover a length only known at run time (`for (i = 0; i < n; ++i)
// tmp[i] = ...;`), since it cannot tell that the reads stay below n. This pass can, for the common shapes, with
// ScalarEvolution; anything it cannot prove keeps its memset.
#pragma once

#include <llvm/IR/PassManager.h>

namespace fminus::opt
{
    // Removes a memset of a local (an entry-block alloca) when no load can read a byte of it that the memset wrote:
    //   - the local never escapes: its address only reaches loads, stores, getelementptrs, comparisons, lifetime
    //     markers, memsets and copies into it (any other use, a call's argument above all, keeps every memset);
    //   - a writer runs after the memset and writes every byte of [0, end): a loop that stores its element i on every
    //     iteration, i counting from 0 by one element, or a memset, memset.pattern, memcpy or memmove into the local's
    //     start (what LLVM's loop idiom pass makes of such a loop);
    //   - every load comes after the memset (it dominates them), and no path reaches one from the memset without
    //     completing the writer, or skipping it through a guard whose condition the load is known to need;
    //   - and every load reads within [0, end), as ScalarEvolution proves: the end of the last element a loop over it
    //     reads is at most the writer's end. The values both are computed from do not change between the memset and
    //     the load.
    // It runs at -O1 and above, at the end of the function simplification pipeline (support/HostTarget.cpp).
    struct ZeroInitElisionPass : llvm::PassInfoMixin<ZeroInitElisionPass>
    {
        llvm::PreservedAnalyses run(llvm::Function& function, llvm::FunctionAnalysisManager& analyses);
    };
} // namespace fminus::opt
