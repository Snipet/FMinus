// src/opt/KnobVersioning.h: knob versioning, an LLVM pass of FMinus's own. A method whose loops branch on an int or a
// bool knob, or on whether a stream is flat, gets one copy of its body for each value the knob can take, picked where
// the method starts. In each copy the knob is a constant, so the rest of the pipeline folds it as it folds a frozen
// parameter: the branch leaves the loop, and so does the work only the other values needed.
//
// It rests on what a knob means (docs/extending.md, recipe 14): one value a call, read once where the method starts.
// A copy chosen there by that value runs the whole call with it, which is what the method would have done anyway.
//
// The code generator says which values a knob can take (src/ir/Modules.cpp, atEntry). The instruction that reads it
// where the method starts carries !fminus.domain, a list of constants of its type: !{i1 false, i1 true} for a bool
// and for a stream's test for null, !{i32 0, i32 1, i32 2} for an int with [[fminus::range(0, 2)]]. The list is a
// hint, never a promise: a value outside it runs the method's original body, so a copy only ever runs with the value
// it was made for. The module flag "fminus.knob-versions" (ir::Options::knobVersions, which ir::lower writes) caps how
// many copies one method gets: 8 when the module has none, and 0 or 1 turns the pass off.
//
// Each copy computes what the method does with its value, and the rest of the pipeline treats it as any other code.
// In the vector and libm math modes (ir::Math) that is bit for bit: a math call is the runtime's function, which gives
// a float the same result in a vectorised loop as in a scalar one, and which LLVM cannot fold. Only the fast mode can
// move a math call's last bits: where a copy's loop vectorises and the method's did not, a math call in it becomes the
// vector library's (_simd_sin_f4), and where the knob's constant reaches one, LLVM folds it with the host's double
// functions (a frozen parameter's pitfall).
#pragma once

#include <llvm/IR/PassManager.h>

namespace fminus::opt
{
    // The metadata kind that tags a knob's value with its domain.
    inline constexpr const char* kDomainMetadata = "fminus.domain";

    // The module flag that holds the budget: at most this many copies of a method's body.
    inline constexpr const char* kVersionsFlag = "fminus.knob-versions";

    // The budget when the module states none (IR that FMinus's code generator did not write).
    inline constexpr unsigned kDefaultVersions = 8;

    // The string attribute a versioned function gets: how many copies of its body it has, "3" for a mode over 0..2.
    // The pass leaves a function that has it alone: FMinus's optimised IR, optimised again (fminus-build at -O3, say),
    // is not versioned twice.
    inline constexpr const char* kVersionsAttribute = "fminus-knob-versions";

    // The constraints of FMinus's opaque unit (src/ir/Modules.cpp, opaqueUnit), an empty asm statement whose result
    // is its operand, in the same register: the 1 of a stream's stride when the stream is not flat, which LLVM cannot
    // see is 1. In a copy for a stream with samples, the pass makes the stride 1 again.
    inline constexpr const char* kUnitConstraints = "=r,0";

    // Versions one function on the knobs of its entry block that steer one of its loops (KnobVersioning.cpp). Run it
    // before the inliner, so that each method is versioned on its own knobs.
    struct KnobVersioningPass : llvm::PassInfoMixin<KnobVersioningPass>
    {
        llvm::PreservedAnalyses run(llvm::Function& function, llvm::FunctionAnalysisManager& analyses);
    };
} // namespace fminus::opt
