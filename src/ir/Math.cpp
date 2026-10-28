// src/ir/Math.cpp: sinf, cosf, tanf, expf, tanhf and logf in the vector and libm math modes (ir::Options::math,
// CodeGen.h).
// Each is a call of a function LLVM does not know, never LLVM's intrinsic: LLVM knows llvm.sin, and folds it with the
// host's double sin when its argument is a constant, pairs it with llvm.cos into one sincos call at -O2, and hands it
// to the target's vector math library when it vectorises. Each of those gives the same argument another result, so
// what a sample met depended on where it was: in a loop's vectorised body or its remainder, at -O0 or -O2, in a poly
// float of one width or another, behind a knob or frozen. A function LLVM does not know it can only call. (The fast
// mode keeps the intrinsics: Calls.cpp.)
//
//   vector   a scalar call is FMinus's math intrinsic fminus.sin.f32, which names fminus.sin.v2f32 and
//            fminus.sin.v4f32 as its two- and four-lane variants, so a loop of them still vectorises, and the SLP
//            vectoriser can pair two (a stereo effect's left and right); a float2 is one call of fminus.sin.v2f32, a
//            float4 one of fminus.sin.v4f32, and wider vectors are cut into four lanes. No module defines them: at the
//            end of the pipeline every call becomes FMinus's kernel, inline (opt/MathKernels.cpp, from -O1), or a call
//            of the runtime's function, fminus_rt_sinf or fminus_rt_sin_f4, a call a lane for two lanes (-O0), which
//            computes the same (runtime/MathKernels.h).
//   libm     fminus_rt_libm_sinf, libm's sinf behind a name LLVM does not know, a lane at a time.
//
// The runtime's three names (fminus_rt_sinf, fminus_rt_sin_f4, fminus_rt_libm_sinf) are reserved: a program that calls
// sinf may not declare a function of any of them itself, in any mode (checkMathNames).
#include "ir/Lowering.h"
#include "opt/MathKernels.h"

#include <llvm/Analysis/VectorUtils.h>
#include <llvm/IR/Constants.h>
#include <llvm/IR/DerivedTypes.h>
#include <llvm/Transforms/Utils/ModuleUtils.h>

#include <cstdint>
#include <string>
#include <vector>

namespace fminus::ir::detail
{
    // `x`, a float or a float vector of `type`, through the builtin's function in the math mode, lane by lane.
    llvm::Value* Lowering::lowerMath(const Builtin& builtin, ast::Type type, llvm::Value* x)
    {
        const unsigned count = ast::isVector(type) ? lanes(type) : 0;   // 0: a float
        if (math_ == Math::Libm)
        {
            llvm::FunctionCallee libm = mathFunction(builtin, MathFunction::Libm);
            if (count == 0)
                return builder_.CreateCall(libm, {x});
            llvm::Value* result = llvm::PoisonValue::get(x->getType());
            for (unsigned i = 0; i < count; ++i)
            {
                llvm::Value* lane = builder_.CreateCall(libm, {builder_.CreateExtractElement(x, i)});
                result = builder_.CreateInsertElement(result, lane, i);
            }
            return result;
        }

        if (count == 0)
            return builder_.CreateCall(mathFunction(builtin, MathFunction::Scalar), {x});
        if (count == 1)   // a poly float of one lane: the float's
        {
            llvm::Value* one = builder_.CreateCall(mathFunction(builtin, MathFunction::Scalar),
                                                   {builder_.CreateExtractElement(x, std::uint64_t{0})});
            return builder_.CreateInsertElement(llvm::PoisonValue::get(x->getType()), one, std::uint64_t{0});
        }
        if (count == 2)   // a float2, or a poly float of two lanes
            return builder_.CreateCall(mathFunction(builtin, MathFunction::Two), {x});
        llvm::FunctionCallee four = mathFunction(builtin, MathFunction::Four);
        if (count == 4)
            return builder_.CreateCall(four, {x});
        // A float8, or a poly float of 8 to 64 lanes (a power of two): four lanes a call, put back together in order.
        std::vector<llvm::Value*> parts;
        for (unsigned first = 0; first < count; first += 4)
        {
            const std::vector<int> part{static_cast<int>(first), static_cast<int>(first + 1),
                                        static_cast<int>(first + 2), static_cast<int>(first + 3)};
            parts.push_back(builder_.CreateCall(four, {builder_.CreateShuffleVector(x, part)}));
        }
        return llvm::concatenateVectors(builder_, parts);
    }

    // The name of the builtin's runtime function `which` (not Two, which has none): fminus_rt_sinf, fminus_rt_sin_f4 or
    // fminus_rt_libm_sinf.
    std::string Lowering::mathName(const Builtin& builtin, MathFunction which)
    {
        const std::string stem(builtin.runtime);
        return which == MathFunction::Scalar ? "fminus_rt_" + stem + "f"
               : which == MathFunction::Four ? "fminus_rt_" + stem + "_f4"
                                             : "fminus_rt_libm_" + stem + "f";
    }

    // Whether the program leaves the names of the builtin's three runtime functions to the runtime, as a call of the
    // builtin at `location` needs; if it does not, the error says so there, with a note at the program's function.
    // A program's own function of one of those names, a definition or a prototype of any type, would otherwise be the
    // function a call reaches (the libm mode's, the vector mode's at -O0, and sin's, cos's and tan's for a large
    // argument; opt/MathKernels.cpp), or the one LLVM takes for the runtime's: it would get the runtime's attributes
    // (memory(none), willreturn, ...), which are false of a program's code, and a four-lane name would be called with
    // a <4 x float> whatever the program's function takes (LLVM asserted on one that took an int, when the runtime's
    // names were the vector variants). So no program's function is ever given a runtime function's attributes. The
    // names are refused in every math mode, the fast mode's too, which calls none of them, so that whether a program
    // compiles does not depend on the mode. A program that calls its own function of such a name, and not the
    // builtin, is left alone, as callRuntime leaves a program's own heap and FFT functions (Calls.cpp).
    bool Lowering::checkMathNames(SourceLocation location, const Builtin& builtin)
    {
        for (const MathFunction which : {MathFunction::Scalar, MathFunction::Four, MathFunction::Libm})
        {
            const std::string name = mathName(builtin, which);
            const auto found = functions_.find(name);
            if (found == functions_.end())
                continue;
            diagnostics_.error(location,
                               quoted(builtin.name) + " reserves " + quoted(name)
                                   + " for the runtime's function, and this program declares a function of that name: "
                                     "rename it",
                               {Diagnostic{found->second->location, quoted(name) + " declared here", {}}});
            return false;
        }
        return true;
    }

    // The function a call of the builtin calls, `which` of them, declared once a module: the vector mode's math
    // intrinsics (fminus.sin.f32, fminus.sin.v2f32 and fminus.sin.v4f32, opt/MathKernels.h), or the libm mode's runtime
    // function. Each has the attributes LLVM gives the intrinsic it stands for (llvm.sin): it reads and writes no
    // memory, always returns, cannot unwind, makes no poison of an argument that isn't, and is defined for every
    // argument, so LICM hoists a call out of a loop, GVN merges two calls of one argument and a call of an unused
    // result goes, but nothing evaluates one, since LLVM does not know the function. The vector mode's scalar intrinsic
    // names its two- and four-lane ones as its vector variants ("vector-function-abi-variant", LLVM's vector function
    // ABI: _ZGV, the LLVM ISA, unmasked, 2 or 4 lanes, one vector parameter), which is what the loop and SLP
    // vectorisers look for: a loop of fminus.sin.f32 calls vectorises into calls of fminus.sin.v4f32, and SLP can pair
    // two independent calls into one of fminus.sin.v2f32. The variants must be declared in the module when they look,
    // and GlobalDCE removes a declaration that nothing uses before they run, so they are kept in llvm.compiler.used, as
    // LLVM's InjectTLIMappings keeps a vector library's (until opt/MathKernels.cpp, which replaces the calls, takes
    // them out). A program can declare no function of an intrinsic's name (FMinus's names have no dot), and none of the
    // libm function's, since checkMathNames refuses a call of the builtin when it has.
    llvm::FunctionCallee Lowering::mathFunction(const Builtin& builtin, MathFunction which)
    {
        const unsigned count = which == MathFunction::Two ? 2 : which == MathFunction::Four ? 4 : 0;
        const std::string name =
            which == MathFunction::Libm ? mathName(builtin, which) : opt::mathIntrinsicName(builtin.runtime, count);
        llvm::Type* type =
            count == 0 ? builder_.getFloatTy() : llvm::FixedVectorType::get(builder_.getFloatTy(), count);
        llvm::FunctionType* signature = llvm::FunctionType::get(type, {type}, false);
        llvm::FunctionCallee callee = module_.getOrInsertFunction(name, signature);
        auto* function = llvm::cast<llvm::Function>(callee.getCallee());
        if (function->doesNotAccessMemory())   // set up by an earlier call
            return callee;
        function->setDoesNotAccessMemory();
        function->setDoesNotThrow();
        function->setWillReturn();
        function->setNoSync();
        function->setDoesNotFreeMemory();
        function->addFnAttr(llvm::Attribute::NoCallback);
        function->addFnAttr(llvm::Attribute::NoCreateUndefOrPoison);
        function->addFnAttr(llvm::Attribute::Speculatable);
        if (which == MathFunction::Scalar)
        {
            auto* two = llvm::cast<llvm::Function>(mathFunction(builtin, MathFunction::Two).getCallee());
            auto* four = llvm::cast<llvm::Function>(mathFunction(builtin, MathFunction::Four).getCallee());
            const auto variant = [&](unsigned width, const llvm::Function* vector) {
                return "_ZGV_LLVM_N" + std::to_string(width) + "v_" + name + "(" + vector->getName().str() + ")";
            };
            function->addFnAttr("vector-function-abi-variant", variant(2, two) + "," + variant(4, four));
            llvm::appendToCompilerUsed(module_, {two, four});
        }
        return callee;
    }
} // namespace fminus::ir::detail
