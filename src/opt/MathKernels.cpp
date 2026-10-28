// src/opt/MathKernels.cpp: the vector math mode's builtins, inline (MathKernels.h).
#include "opt/MathKernels.h"

#include "ir/Denormals.h"
#include "runtime/MathKernels.h"

#include <llvm/ADT/APFloat.h>
#include <llvm/IR/Constants.h>
#include <llvm/IR/DerivedTypes.h>
#include <llvm/IR/IRBuilder.h>
#include <llvm/IR/Instructions.h>
#include <llvm/IR/MDBuilder.h>
#include <llvm/IR/Module.h>
#include <llvm/Transforms/Scalar/EarlyCSE.h>
#include <llvm/Transforms/Scalar/InstSimplifyPass.h>
#include <llvm/Transforms/Scalar/SimplifyCFG.h>
#include <llvm/Transforms/Utils/BasicBlockUtils.h>
#include <llvm/Transforms/Utils/ModuleUtils.h>

#include <algorithm>
#include <array>
#include <charconv>
#include <cstdint>
#include <vector>

namespace fminus::opt
{
    namespace
    {
        constexpr std::array<std::string_view, 6> kStems{"sin", "cos", "tan", "exp", "tanh", "log"};

        // The runtime's function of a stem, `lanes` wide (0: the scalar one): fminus_rt_sinf, fminus_rt_sin_f4.
        std::string runtimeName(std::string_view stem, unsigned lanes)
        {
            return "fminus_rt_" + std::string(stem) + (lanes == 0 ? "f" : "_f" + std::to_string(lanes));
        }

        // The runtime's function `name`, of `type`, declared with what is true of its code: it reads and writes no
        // memory, returns, and cannot unwind. Not speculatable: what calls it is the branch for a large argument,
        // which nothing may hoist out of its condition (or every call would pay for the exact reduction).
        llvm::Function* runtimeFunction(llvm::Module& module, const std::string& name, llvm::FunctionType* type)
        {
            auto* function = llvm::cast<llvm::Function>(module.getOrInsertFunction(name, type).getCallee());
            function->setDoesNotAccessMemory();
            function->setDoesNotThrow();
            function->setWillReturn();
            function->setNoSync();
            function->setDoesNotFreeMemory();
            function->addFnAttr(llvm::Attribute::NoCallback);
            return function;
        }

        // What a large argument's branch calls for a lane: fminus.large.<stem>, an internal function of the
        // preserve_all calling convention that calls the runtime's scalar function and saves every register the call
        // may change. A kernel's branch is in its caller's loop, and a call of C's convention there would make every
        // value live across the loop (the kernels' constants) survive a call that may change 24 of the 32 vector
        // registers: the register allocator spilled them, and reloaded them every iteration. With this one the cost is
        // on the branch, which almost never runs. (FMinus's names have no dot, so no program has a function of this
        // name.)
        llvm::Function* largeFunction(llvm::Module& module, std::string_view stem)
        {
            const std::string name = "fminus.large." + std::string(stem);
            if (llvm::Function* existing = module.getFunction(name))
                return existing;
            llvm::LLVMContext& context = module.getContext();
            llvm::Type* single = llvm::Type::getFloatTy(context);
            llvm::FunctionType* type = llvm::FunctionType::get(single, {single}, false);
            llvm::Function* runtime = runtimeFunction(module, runtimeName(stem, 0), type);
            llvm::Function* function =
                llvm::Function::Create(type, llvm::GlobalValue::InternalLinkage, name, module);
            function->setCallingConv(llvm::CallingConv::PreserveAll);
            function->addFnAttr(llvm::Attribute::NoInline);
            function->addFnAttr(llvm::Attribute::Cold);
            function->setDoesNotAccessMemory();
            function->setDoesNotThrow();
            function->setWillReturn();
            function->setNoSync();
            function->setDoesNotFreeMemory();
            llvm::IRBuilder<> body(llvm::BasicBlock::Create(context, "entry", function));
            body.CreateRet(body.CreateCall(runtime, {function->getArg(0)}));
            return function;
        }

        // The kernels' operations (runtime/MathKernels.h, "The operations") as IR, before `builder`'s insertion point,
        // on a float or a vector of floats of `type`: the same IEEE operation in each lane as the runtime's Scalar and
        // Vector compute. No fast-math flags, so LLVM reassociates and contracts none of them, and an fma is
        // llvm.fma, fused on every target (or the C library's correctly rounded fmaf where there is no instruction).
        struct IROps
        {
            using F = llvm::Value*;
            using I = llvm::Value*;
            using M = llvm::Value*;

            llvm::IRBuilderBase& b;
            llvm::Module& module;
            llvm::Type* floatType;   // float, or <N x float>
            llvm::Type* intType;     // i32, or <N x i32>
            unsigned lanes;          // 0 for a float
            std::string name;        // each instruction's, so the kernel reads as one in the IR: "sin", "sin1", ...

            F c(float v)
            {
                return llvm::ConstantFP::get(floatType, static_cast<double>(v));
            }
            I ci(std::uint32_t v)
            {
                return llvm::ConstantInt::get(intType, v);
            }
            F add(F x, F y)
            {
                return b.CreateFAdd(x, y, name);
            }
            F sub(F x, F y)
            {
                return b.CreateFSub(x, y, name);
            }
            F mul(F x, F y)
            {
                return b.CreateFMul(x, y, name);
            }
            F div(F x, F y)
            {
                return b.CreateFDiv(x, y, name);
            }
            F fma(F x, F y, F z)
            {
                return b.CreateIntrinsic(llvm::Intrinsic::fma, {floatType}, {x, y, z}, nullptr, name);
            }
            F abs(F x)
            {
                return b.CreateUnaryIntrinsic(llvm::Intrinsic::fabs, x, nullptr, name);
            }
            F neg(F x)
            {
                return b.CreateFNeg(x, name);
            }
            F min(F x, F y)
            {
                return b.CreateMinNum(x, y, nullptr, name);   // y is a constant, never NaN: the kernels' select
            }
            // A float's negation chosen, in its floating-point register (an exclusive or of its bits would take it to
            // an integer register and back, a few cycles of latency); a vector's sign bits flipped by exclusive or, two
            // operations where a select would be three. The same bits.
            F flipSign(F x, I sign)
            {
                if (lanes == 0)
                    return b.CreateSelect(signBit(sign), b.CreateFNeg(x, name), x, name);
                return fromBits(ixor(bits(x), iand(sign, ci(kernels::kSign))));
            }
            M lt(F x, F y)
            {
                return b.CreateFCmpOLT(x, y, name);
            }
            M gt(F x, F y)
            {
                return b.CreateFCmpOGT(x, y, name);
            }
            M eq(F x, F y)
            {
                return b.CreateFCmpOEQ(x, y, name);
            }
            M ge(F x, F y)
            {
                return b.CreateFCmpOGE(x, y, name);
            }
            M signBit(I x)
            {
                return b.CreateICmpSLT(x, llvm::Constant::getNullValue(intType), name);
            }
            M ult(I x, I y)
            {
                return b.CreateICmpULT(x, y, name);
            }
            F select(M m, F x, F y)
            {
                return b.CreateSelect(m, x, y, name);
            }
            I bits(F x)
            {
                return b.CreateBitCast(x, intType, name);
            }
            F fromBits(I x)
            {
                return b.CreateBitCast(x, floatType, name);
            }
            I iadd(I x, I y)
            {
                return b.CreateAdd(x, y, name);
            }
            I isub(I x, I y)
            {
                return b.CreateSub(x, y, name);
            }
            I iand(I x, I y)
            {
                return b.CreateAnd(x, y, name);
            }
            I ixor(I x, I y)
            {
                return b.CreateXor(x, y, name);
            }
            I shl(I x, int n)
            {
                return b.CreateShl(x, static_cast<std::uint64_t>(n), name);
            }
            I ashr(I x, int n)
            {
                return b.CreateAShr(x, static_cast<std::uint64_t>(n), name);
            }
            F toFloat(I x)
            {
                return b.CreateSIToFP(x, floatType, name);
            }

            // Where any lane is past the limit, a branch the kernel marks unlikely to a block that calls the runtime's
            // scalar function for each lane (through largeFunction), whose result the lanes past the limit take; a phi
            // joins the two. The insertion point, the call this kernel replaces, ends up at the start of the block
            // after them.
            F large(M big, F x, F fast, kernels::Large which)
            {
                llvm::Instruction* at = &*b.GetInsertPoint();
                llvm::BasicBlock* head = at->getParent();
                llvm::Value* any = lanes == 0 ? big : b.CreateOrReduce(big);
                llvm::Instruction* branch = llvm::SplitBlockAndInsertIfThen(
                    any, at->getIterator(), false, llvm::MDBuilder(b.getContext()).createUnlikelyBranchWeights());
                branch->getParent()->setName(name + ".large");
                llvm::IRBuilder<> large(branch);
                const std::string_view stem = which == kernels::Large::Sin   ? "sin"
                                              : which == kernels::Large::Cos ? "cos"
                                                                             : "tan";
                llvm::Function* exactly = largeFunction(module, stem);
                const auto call = [&](llvm::Value* lane) {
                    llvm::CallInst* one = large.CreateCall(exactly, {lane}, name);
                    one->setCallingConv(llvm::CallingConv::PreserveAll);
                    return one;
                };
                llvm::Value* exact = nullptr;
                if (lanes == 0)
                    exact = call(x);
                else
                {
                    exact = fast;
                    for (unsigned lane = 0; lane < lanes; ++lane)
                        exact = large.CreateInsertElement(exact, call(large.CreateExtractElement(x, lane)), lane);
                    exact = large.CreateSelect(big, exact, fast, name);   // the other lanes keep the fast path's
                }
                at->getParent()->setName(name + ".join");
                b.SetInsertPoint(at);
                llvm::PHINode* joined = b.CreatePHI(floatType, 2, name);
                joined->addIncoming(fast, head);
                joined->addIncoming(exact, branch->getParent());
                return joined;
            }
        };

        // The kernel of `stem` on x, built before `ops.b`'s insertion point.
        llvm::Value* kernel(IROps& ops, std::string_view stem, llvm::Value* x)
        {
            if (stem == "sin")
                return kernels::sin(ops, x);
            if (stem == "cos")
                return kernels::cos(ops, x);
            if (stem == "tan")
                return kernels::tan(ops, x);
            if (stem == "exp")
                return kernels::exp(ops, x);
            if (stem == "tanh")
                return kernels::tanh(ops, x);
            return kernels::log(ops, x);
        }

        // Whether the function computes flushing subnormals (Denormals::Flush: denormal_fpenv(preservesign)), which
        // the builder's folds of constants must then do too (ir/Denormals.h).
        bool flushes(const llvm::Function& function)
        {
            return function.getDenormalMode(llvm::APFloat::IEEEsingle()) == llvm::DenormalMode::getPreserveSign();
        }

        // The call's kernel, inline, in its place.
        void expand(llvm::CallInst& call, std::string_view stem)
        {
            llvm::Function& function = *call.getFunction();
            llvm::IRBuilder<ir::detail::DenormalFolder> builder(call.getContext(),
                                                                ir::detail::DenormalFolder(flushes(function)));
            builder.SetInsertPoint(&call);
            builder.SetCurrentDebugLocation(call.getDebugLoc());
            llvm::Type* type = call.getType();
            const auto* vector = llvm::dyn_cast<llvm::FixedVectorType>(type);
            llvm::Type* intType = builder.getInt32Ty();
            if (vector != nullptr)
                intType = llvm::VectorType::get(intType, vector->getElementCount());
            const unsigned lanes = vector != nullptr ? vector->getNumElements() : 0;
            IROps ops{builder, *function.getParent(), type, intType, lanes, std::string(stem)};
            llvm::Value* result = kernel(ops, stem, call.getArgOperand(0));
            call.replaceAllUsesWith(result);
            call.eraseFromParent();
        }

        // The call, of the runtime's function of its width; of a width the runtime has none of, a call a lane.
        void callRuntime(llvm::CallInst& call, std::string_view stem)
        {
            llvm::Module& module = *call.getModule();
            llvm::Type* type = call.getType();
            const auto* vector = llvm::dyn_cast<llvm::FixedVectorType>(type);
            const unsigned lanes = vector != nullptr ? vector->getNumElements() : 0;
            if (lanes == 0 || lanes == 4)
            {
                call.setCalledFunction(
                    runtimeFunction(module, runtimeName(stem, lanes), llvm::FunctionType::get(type, {type}, false)));
                return;
            }
            llvm::IRBuilder<> builder(&call);
            llvm::Type* single = builder.getFloatTy();
            llvm::Function* scalar =
                runtimeFunction(module, runtimeName(stem, 0), llvm::FunctionType::get(single, {single}, false));
            llvm::Value* result = llvm::PoisonValue::get(type);
            for (unsigned lane = 0; lane < lanes; ++lane)
            {
                llvm::Value* argument = builder.CreateExtractElement(call.getArgOperand(0), lane);
                result = builder.CreateInsertElement(result, builder.CreateCall(scalar, {argument}), lane);
            }
            call.replaceAllUsesWith(result);
            call.eraseFromParent();
        }
    } // namespace

    std::string mathIntrinsicName(std::string_view stem, unsigned lanes)
    {
        return "fminus." + std::string(stem) + (lanes == 0 ? ".f32" : ".v" + std::to_string(lanes) + "f32");
    }

    std::optional<std::string> mathIntrinsicStem(std::string_view name)
    {
        constexpr std::string_view kPrefix = "fminus.";
        if (!name.starts_with(kPrefix))
            return std::nullopt;
        name.remove_prefix(kPrefix.size());
        const std::size_t dot = name.find('.');
        if (dot == std::string_view::npos)
            return std::nullopt;
        const std::string_view stem = name.substr(0, dot);
        bool known = false;
        for (const std::string_view candidate : kStems)
            known = known || candidate == stem;
        if (!known)
            return std::nullopt;
        std::string_view type = name.substr(dot + 1);
        if (type == "f32")
            return std::string(stem);
        unsigned lanes = 0;
        if (!type.starts_with("v") || !type.ends_with("f32"))
            return std::nullopt;
        type = type.substr(1, type.size() - 4);
        const auto [end, error] = std::from_chars(type.data(), type.data() + type.size(), lanes);
        if (error != std::errc() || end != type.data() + type.size() || lanes == 0)
            return std::nullopt;
        return std::string(stem);
    }

    llvm::PreservedAnalyses MathKernelsPass::run(llvm::Module& module, llvm::ModuleAnalysisManager& analyses)
    {
        std::vector<std::pair<llvm::Function*, std::string>> intrinsics;
        for (llvm::Function& function : module)
        {
            if (!function.isDeclaration())
                continue;
            if (std::optional<std::string> stem = mathIntrinsicStem(function.getName()))
                intrinsics.emplace_back(&function, std::move(*stem));
        }
        if (intrinsics.empty())
            return llvm::PreservedAnalyses::all();
        std::vector<llvm::Function*> changed;
        for (const auto& [intrinsic, stem] : intrinsics)
        {
            std::vector<llvm::CallInst*> calls;
            for (llvm::User* user : intrinsic->users())
            {
                auto* call = llvm::dyn_cast<llvm::CallInst>(user);
                if (call != nullptr && call->getCalledFunction() == intrinsic)
                    calls.push_back(call);
            }
            for (llvm::CallInst* call : calls)
            {
                changed.push_back(call->getFunction());
                if (inlineKernels_)
                    expand(*call, stem);
                else
                    callRuntime(*call, stem);
            }
        }
        // The four-lane intrinsics were kept for the vectorisers (llvm.compiler.used); with their calls gone, every
        // intrinsic goes.
        llvm::removeFromUsedLists(module, [&](llvm::Constant* constant) {
            for (const auto& entry : intrinsics)
            {
                if (constant->stripPointerCasts() == entry.first)
                    return true;
            }
            return false;
        });
        for (const auto& entry : intrinsics)
        {
            entry.first->removeDeadConstantUsers();   // the used list's old array
            if (entry.first->use_empty())
                entry.first->eraseFromParent();
        }
        // The kernels inline, tidied in the functions that have them: EarlyCSE merges what two share (sinf and cosf of
        // one angle compute one reduction, and the same polynomials), InstSimplify folds a constant's kernel, as the
        // function computes (denormal_fpenv), and SimplifyCFG takes out a large argument's branch that folded to
        // false. (It cannot hoist the runtime's call out of that branch: the call is not speculatable.)
        if (inlineKernels_)
        {
            llvm::FunctionAnalysisManager& functions =
                analyses.getResult<llvm::FunctionAnalysisManagerModuleProxy>(module).getManager();
            std::sort(changed.begin(), changed.end());
            changed.erase(std::unique(changed.begin(), changed.end()), changed.end());
            for (llvm::Function* function : changed)
            {
                functions.invalidate(*function, llvm::PreservedAnalyses::none());
                llvm::FunctionPassManager cleanup;
                cleanup.addPass(llvm::EarlyCSEPass());
                cleanup.addPass(llvm::InstSimplifyPass());
                cleanup.addPass(llvm::SimplifyCFGPass());
                cleanup.run(*function, functions);
            }
        }
        return llvm::PreservedAnalyses::none();
    }
} // namespace fminus::opt
