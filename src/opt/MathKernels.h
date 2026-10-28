// src/opt/MathKernels.h: the vector math mode's builtins, inline. The code generator lowers sinf, cosf, tanf, expf,
// tanhf and logf to calls of FMinus's math intrinsics, functions no module defines: fminus.sin.f32 for a float and
// fminus.sin.v4f32 for four lanes (ir/Math.cpp), which LLVM treats as it treats llvm.sin's calls (they read and write
// no memory, and a scalar one names its four-lane one as its vector variant), so it hoists them out of loops, merges
// two of one argument, and vectorises loops of them, but evaluates none. At the end of the pipeline this pass replaces
// every call of one: from -O1 by the kernel itself (runtime/MathKernels.h), built here as IR from the same template the
// runtime's functions instantiate, so that it computes the same operations in the same order, and gives every float
// the runtime's result; at -O0 by a call of the runtime's function of that width (fminus_rt_sinf, fminus_rt_sin_f4).
// An argument of sin, cos or tan past kernels::kLargeArgument takes a branch, which the kernel marks unlikely, to a
// call of the runtime's scalar function for each lane. The intrinsics' declarations go with their calls.
#pragma once

#include <llvm/IR/PassManager.h>

#include <optional>
#include <string>
#include <string_view>

namespace fminus::opt
{
    // A math intrinsic's name: fminus.<stem>.f32 for a float (lanes 0), fminus.<stem>.v<lanes>f32 for a vector. The
    // stem is the builtin's (ir/Lowering.h's Builtin::runtime): "sin" for sinf.
    std::string mathIntrinsicName(std::string_view stem, unsigned lanes);

    // The stem of a math intrinsic's name, or nothing for any other name.
    std::optional<std::string> mathIntrinsicStem(std::string_view name);

    // Replaces every call of a math intrinsic in the module (above): inline (from -O1), or by a call of the runtime
    // (-O0). Registered at the end of the pipeline (support/HostTarget.cpp), after the vectorisers have widened the
    // calls they could.
    struct MathKernelsPass : llvm::PassInfoMixin<MathKernelsPass>
    {
        explicit MathKernelsPass(bool inlineKernels) : inlineKernels_(inlineKernels) {}

        llvm::PreservedAnalyses run(llvm::Module& module, llvm::ModuleAnalysisManager& analyses);

        static bool isRequired()
        {
            return true;   // at -O0 too: the intrinsics have no definition, and a call of one must not reach the linker
        }

    private:
        bool inlineKernels_;
    };
} // namespace fminus::opt
