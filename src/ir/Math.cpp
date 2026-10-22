// src/ir/Math.cpp: sinf, cosf, expf, tanhf and logf in the vector and libm math modes (ir::Options::math, CodeGen.h).
// Each is a call of a function of the runtime (runtime/Runtime.h, runtime/Math.cpp), never LLVM's intrinsic: LLVM
// knows llvm.sin, and folds it with the host's double sin when its argument is a constant, pairs it with llvm.cos into
// one sincos call at -O2, and hands it to the target's vector math library when it vectorises. Each of those gives the
// same argument another result, so what a sample met depended on where it was: in a loop's vectorised body or its
// remainder, at -O0 or -O2, in a poly float of one width or another, behind a knob or frozen. A function LLVM does not
// know it can only call. (The fast mode keeps the intrinsics: Calls.cpp.)
//
//   vector   a scalar call is fminus_rt_sinf, which names fminus_rt_sin_f4 as its four-lane variant, so a loop of
//            them still vectorises; a float4 is one call of fminus_rt_sin_f4, and other widths are cut or padded into
//            four lanes. The runtime makes the two agree: a lane of fminus_rt_sin_f4 is fminus_rt_sinf of that lane.
//   libm     fminus_rt_libm_sinf, libm's sinf behind a name LLVM does not know, a lane at a time.
//
// The three names are the runtime's: a program that calls sinf may not declare a function of any of them itself, in
// any mode (checkMathNames).
#include "ir/Lowering.h"

#include <llvm/Analysis/VectorUtils.h>
#include <llvm/IR/Constants.h>
#include <llvm/IR/DerivedTypes.h>
#include <llvm/Transforms/Utils/ModuleUtils.h>

#include <string>
#include <vector>

namespace fminus::ir::detail
{
    // `x`, a float or a float vector of `type`, through the builtin's runtime function, lane by lane.
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
        llvm::FunctionCallee four = mathFunction(builtin, MathFunction::Four);
        if (count == 4)
            return builder_.CreateCall(four, {x});
        if (count < 4)
        {
            // A float2, or a poly float of one or two lanes: padded to four with zeros, which keep sin and cos on the
            // runtime's fast path (runtime/Math.cpp), and its own lanes taken back.
            std::vector<int> padded;
            for (unsigned i = 0; i < 4; ++i)
                padded.push_back(static_cast<int>(i < count ? i : count));   // count: the zero vector's first lane
            llvm::Value* zeros = llvm::Constant::getNullValue(x->getType());
            llvm::Value* result = builder_.CreateCall(four, {builder_.CreateShuffleVector(x, zeros, padded)});
            std::vector<int> own;
            for (unsigned i = 0; i < count; ++i)
                own.push_back(static_cast<int>(i));
            return builder_.CreateShuffleVector(result, own);
        }
        // A float8, or a poly float of 8 to 64 lanes: four lanes a call, put back together in order.
        std::vector<llvm::Value*> parts;
        for (unsigned first = 0; first < count; first += 4)
        {
            const std::vector<int> part{static_cast<int>(first), static_cast<int>(first + 1),
                                        static_cast<int>(first + 2), static_cast<int>(first + 3)};
            parts.push_back(builder_.CreateCall(four, {builder_.CreateShuffleVector(x, part)}));
        }
        return llvm::concatenateVectors(builder_, parts);
    }

    // The name of the builtin's runtime function `which`: fminus_rt_sinf, fminus_rt_sin_f4 or fminus_rt_libm_sinf.
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
    // function the call reaches, or the one LLVM takes for the runtime's: it would get the runtime's attributes
    // (memory(none), willreturn, speculatable, ...), which are false of a program's code, and the four-lane name would
    // be the scalar function's vector variant, which the loop vectoriser calls with a <4 x float> whatever the
    // program's function takes. So no program's function is ever given a runtime function's attributes, nor named as a
    // vector variant. The names are refused in every math mode, the fast mode's too, which calls none of them, so that
    // whether a program compiles does not depend on the mode. A program that calls its own function of such a name,
    // and not the builtin, is left alone, as callRuntime leaves a program's own heap and FFT functions (Calls.cpp).
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

    // The runtime function `which` of the builtin, declared once a module. It has the attributes LLVM gives the
    // intrinsic it stands for (llvm.sin): it reads and writes no memory, always returns, cannot unwind, makes no
    // poison of an argument that isn't, and is defined for every argument, so LICM hoists a call out of a loop, GVN
    // merges two calls of one argument and a call of an unused result goes, but nothing evaluates one, since LLVM does
    // not know the function. The vector mode's scalar function names its four-lane one as its vector variant
    // ("vector-function-abi-variant", LLVM's vector function ABI: _ZGV, the LLVM ISA, unmasked, 4 lanes, one vector
    // parameter), which is what the loop and SLP vectorisers look for: a loop of fminus_rt_sinf calls vectorises into
    // calls of fminus_rt_sin_f4. The variant must be declared in the module when they look, and GlobalDCE removes a
    // declaration that nothing uses before they run, so it is kept in llvm.compiler.used, as LLVM's InjectTLIMappings
    // keeps a vector library's. The declaration is always the runtime's, of the runtime's type: the program has no
    // function of the name, since checkMathNames refuses a call of the builtin when it has.
    llvm::FunctionCallee Lowering::mathFunction(const Builtin& builtin, MathFunction which)
    {
        const std::string name = mathName(builtin, which);
        llvm::Type* type = which == MathFunction::Four ? llvm::FixedVectorType::get(builder_.getFloatTy(), 4)
                                                       : builder_.getFloatTy();
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
            auto* variant = llvm::cast<llvm::Function>(mathFunction(builtin, MathFunction::Four).getCallee());
            function->addFnAttr("vector-function-abi-variant",
                                "_ZGV_LLVM_N4v_" + name + "(" + variant->getName().str() + ")");
            llvm::appendToCompilerUsed(module_, {variant});
        }
        return callee;
    }
} // namespace fminus::ir::detail
