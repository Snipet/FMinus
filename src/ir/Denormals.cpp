// src/ir/Denormals.cpp: Denormals::Flush (CodeGen.h), flush-to-zero, the mode a plugin host runs its audio thread in.
// C's floats underflow gradually, through the subnormals, and FMinus's do by default (Preserve). In audio that costs
// twice: a subnormal is slow on some CPUs, and a filter gone silent decays into the subnormals and can stay there, at
// 1e-43, for ever. So plugin hosts flush, FTZ and on x86 DAZ too, around each block (a ScopedFtz). LLVM has to be
// told, or it folds and simplifies as IEEE says and the hardware then does not compute: a frozen parameter's
// 1e-30 * 1e-10 would fold to 1e-40, where a knob's computes 0. Under Flush:
//
//   - Every function the program defines is marked denormal_fpenv(preservesign), LLVM 23's attribute for the mode a
//     function runs in (the successor of the "denormal-fp-math" string): a subnormal input reads as zero and a
//     subnormal result is zero, each keeping its sign. That is AArch64's FPCR.FZ (with FPCR.AH clear, as macOS and
//     Linux leave it) and x86's MXCSR.FTZ with MXCSR.DAZ. LLVM's constant folding then flushes as the hardware does.
//     The attribute sets no hardware.
//   - The code generator's own folds flush likewise (DenormalFolder, Denormals.h): IRBuilder folds constants as it
//     builds them, where no attribute reaches, and at -O0 nothing folds them again before the instruction selector,
//     which folds in IEEE arithmetic whatever the attribute says.
//   - Each entry point sets the mode itself: a [[clang::nonblocking]] function, what a host calls on its audio thread,
//     that no function of the program calls. Its definition becomes an internal function, `<name>.body`, and the
//     entry point `<name>` a wrapper that reads the control register and calls the body; when the caller does not
//     flush already, it sets flush-to-zero first and restores the register after: one return, after any of the
//     body's. Nothing in it can block: register moves, a compare and a call.
//
// Reading the register is free, and writing it is not: on the M5 an MSR to FPCR costs about 4 ns even when it changes
// nothing, and setting FZ and clearing it again about 18 ns a call. So a host that flushes already, around its whole
// block, pays a compare and a call, and one that doesn't pays the two writes, once a call. That is also why a function
// the program calls is not an entry point, nonblocking or not: `OnePole::tick`, called a sample at a time from
// `process`, runs in the mode `process` set. Its calls inside the program never meet a wrapper, but a host might call
// it too, a sample at a time, and the writes would then cost more than such a method does. A host that calls one, or
// anything else that is not an entry point (an `init`), sets the mode around the call itself.
//
// The wrapper is denormal_fpenv(dynamic): it starts in whatever mode its caller has, and does no arithmetic of its
// own. That is also what keeps the body out of it, with noinline on the body besides: LLVM's inliner does not inline a
// function that assumes a mode into one that does not know it. The body has to stay behind a call. In LLVM's default
// floating-point environment arithmetic depends on no register, so it moves freely, across a write of the control
// register too (llvm.aarch64.set.fpcr is ordered only against other side effects), and a body inlined into its
// wrapper could start computing before the mode is set.
#include "ir/Lowering.h"

#include <llvm/ADT/FloatingPointMode.h>
#include <llvm/IR/Constants.h>
#include <llvm/IR/IntrinsicsAArch64.h>
#include <llvm/IR/IntrinsicsX86.h>
#include <llvm/IR/Verifier.h>
#include <llvm/Support/raw_ostream.h>
#include <llvm/TargetParser/Host.h>
#include <llvm/TargetParser/Triple.h>

#include <cstdint>
#include <string>
#include <unordered_set>
#include <vector>

namespace fminus::ir::detail
{
    namespace
    {
        // The mode of every function the program defines under Flush, and of an entry point's wrapper.
        constexpr llvm::DenormalFPEnv kFlushing{llvm::DenormalMode::getPreserveSign()};
        constexpr llvm::DenormalFPEnv kCallersMode{llvm::DenormalMode::getDynamic()};

        constexpr std::uint64_t kFpcrFz = std::uint64_t{1} << 24;   // AArch64 FPCR.FZ: inputs and results flushed
        constexpr std::uint64_t kMxcsrFtz = std::uint64_t{1} << 15;  // x86 MXCSR.FTZ: results flushed
        constexpr std::uint64_t kMxcsrDaz = std::uint64_t{1} << 6;   // x86 MXCSR.DAZ: inputs read as zero

        bool isFloatArithmetic(llvm::Instruction::BinaryOps opcode)
        {
            return opcode == llvm::Instruction::FAdd || opcode == llvm::Instruction::FSub
                   || opcode == llvm::Instruction::FMul || opcode == llvm::Instruction::FDiv
                   || opcode == llvm::Instruction::FRem;
        }

        // `constant` with each subnormal float in it a zero of its sign, as flushing hardware reads it and writes it:
        // a float, or a vector lane by lane. nullptr for a constant this cannot see into (a constant expression),
        // which is then not folded.
        llvm::Constant* flushed(llvm::Constant* constant)
        {
            if (auto* number = llvm::dyn_cast<llvm::ConstantFP>(constant))
            {
                const llvm::APFloat& value = number->getValueAPF();
                return value.isDenormal() ? llvm::ConstantFP::getZero(number->getType(), value.isNegative()) : number;
            }
            if (llvm::isa<llvm::ConstantAggregateZero, llvm::UndefValue>(constant))
                return constant;   // zeros, and undef or poison (a poison lane stays poison)
            auto* vector = llvm::dyn_cast<llvm::FixedVectorType>(constant->getType());
            if (vector == nullptr || llvm::isa<llvm::ConstantExpr>(constant))
                return nullptr;
            std::vector<llvm::Constant*> lanes;
            bool changed = false;
            for (unsigned i = 0; i < vector->getNumElements(); ++i)
            {
                llvm::Constant* lane = constant->getAggregateElement(i);
                llvm::Constant* flushedLane = lane != nullptr ? flushed(lane) : nullptr;
                if (flushedLane == nullptr)
                    return nullptr;
                changed = changed || flushedLane != lane;
                lanes.push_back(flushedLane);
            }
            return changed ? llvm::ConstantVector::get(lanes) : constant;
        }
    } // namespace

    // fadd, fsub, fmul, fdiv and frem of two constants, folded with their subnormal operands read as zero and a
    // subnormal result written as zero: LLVM's ConstantFoldFPInstOperands under preservesign, in the same order.
    llvm::Value* DenormalFolder::FoldBinOp(llvm::Instruction::BinaryOps opcode, llvm::Value* lhs,
                                           llvm::Value* rhs) const
    {
        if (!flush_ || !isFloatArithmetic(opcode))
            return folder_.FoldBinOp(opcode, lhs, rhs);
        auto* a = llvm::dyn_cast<llvm::Constant>(lhs);
        auto* b = llvm::dyn_cast<llvm::Constant>(rhs);
        if (a == nullptr || b == nullptr)
            return nullptr;
        a = flushed(a);
        b = flushed(b);
        if (a == nullptr || b == nullptr)
            return nullptr;
        auto* result = llvm::dyn_cast_or_null<llvm::Constant>(folder_.FoldBinOp(opcode, a, b));
        return result != nullptr ? flushed(result) : nullptr;
    }

    // As ConstantFolder does, it folds the same with fast-math flags as without.
    llvm::Value* DenormalFolder::FoldBinOpFMF(llvm::Instruction::BinaryOps opcode, llvm::Value* lhs, llvm::Value* rhs,
                                              llvm::FastMathFlags) const
    {
        return FoldBinOp(opcode, lhs, rhs);
    }

    // A comparison of two floats reads a subnormal operand as zero, as FCMP and UCOMISS do flushing: 1e-40f > 0 is
    // false.
    llvm::Value* DenormalFolder::FoldCmp(llvm::CmpInst::Predicate predicate, llvm::Value* lhs, llvm::Value* rhs) const
    {
        if (!flush_ || !llvm::CmpInst::isFPPredicate(predicate))
            return folder_.FoldCmp(predicate, lhs, rhs);
        auto* a = llvm::dyn_cast<llvm::Constant>(lhs);
        auto* b = llvm::dyn_cast<llvm::Constant>(rhs);
        if (a == nullptr || b == nullptr)
            return nullptr;
        a = flushed(a);
        b = flushed(b);
        if (a == nullptr || b == nullptr)
            return nullptr;
        return folder_.FoldCmp(predicate, a, b);
    }

    // Every definition marked flushing, then each entry point wrapped. A function is called when any definition's
    // body calls it, by the calls lowerCall and callMethod record for Effects.cpp (calls_): one in dead code counts
    // too. A target whose mode FMinus cannot set is an error at 1:1, as a binding that names nothing is: the option
    // promises what the code cannot keep there.
    void Lowering::flushDenormals(const ast::Program& program)
    {
        const llvm::Triple triple(targetTriple_.empty() ? llvm::sys::getProcessTriple() : targetTriple_);
        const bool arm = triple.isAArch64();
        if (!arm && triple.getArch() != llvm::Triple::x86_64)
        {
            error({1, 1}, "cannot flush subnormals to zero on " + triple.getArchName().str()
                              + ": FMinus sets the floating-point mode on arm64 and x86-64");
            return;
        }
        std::unordered_set<std::string> called;
        for (const auto& caller : calls_)
        {
            for (const CallSite& call : caller.second)
                called.insert(call.callee);
        }
        for (const ast::Function& function : program.functions)
        {
            const auto found = functions_.find(function.name);
            if (!function.isDefinition || found == functions_.end() || found->second != &function)
                continue;   // a prototype, or a definition declare() refused
            llvm::Function* definition = module_.getFunction(function.name);
            definition->addFnAttrs(llvm::AttrBuilder(module_.getContext()).addDenormalFPEnvAttr(kFlushing));
            if (effectOf(function.name) == Effect::NonBlocking && !called.contains(function.name))
                wrapEntry(*definition, function.location, arm);
        }
    }

    // `function`, an entry point, becomes `<name>.body`, internal and noinline, and `<name>` a wrapper of the same
    // type and the same C ABI attributes (zeroext, noalias, this's nonnull) that runs it flushing. On arm64 (`arm`):
    //
    //   define float @"Lowpass::tick"(ptr %this, float %x) #1 {   ; denormal_fpenv(dynamic)
    //   entry:
    //     %fpcr = call i64 @llvm.aarch64.get.fpcr()
    //     %fpcr.fz = or i64 %fpcr, 16777216                        ; FZ, bit 24
    //     %flushing = icmp eq i64 %fpcr, %fpcr.fz                  ; the caller's mode flushes already
    //     br i1 %flushing, label %run, label %flush
    //   flush:
    //     call void @llvm.aarch64.set.fpcr(i64 %fpcr.fz)
    //     br label %run
    //   run:
    //     %result = call float @"Lowpass::tick.body"(ptr %this, float %x)
    //     br i1 %flushing, label %done, label %restore
    //   restore:
    //     call void @llvm.aarch64.set.fpcr(i64 %fpcr)
    //     br label %done
    //   done:
    //     ret float %result
    //   }
    //
    // On x86-64 the same through two stack slots, since STMXCSR and LDMXCSR go through memory: MXCSR with FTZ (bit 15)
    // and DAZ (bit 6) set, both. Either way the register comes back as it was.
    void Lowering::wrapEntry(llvm::Function& function, SourceLocation location, bool arm)
    {
        const std::string name = function.getName().str();
        llvm::LLVMContext& context = module_.getContext();
        function.setName(name + ".body");
        function.setLinkage(llvm::GlobalValue::InternalLinkage);
        function.addFnAttr(llvm::Attribute::NoInline);
        llvm::Function* entry =
            llvm::Function::Create(function.getFunctionType(), llvm::GlobalValue::ExternalLinkage, name, module_);
        entry->setAttributes(function.getAttributes());
        entry->removeFnAttr(llvm::Attribute::NoInline);
        entry->removeFnAttr(llvm::Attribute::DenormalFPEnv);
        entry->addFnAttrs(llvm::AttrBuilder(context).addDenormalFPEnvAttr(kCallersMode));

        llvm::BasicBlock* start = llvm::BasicBlock::Create(context, "entry", entry);
        llvm::BasicBlock* flush = llvm::BasicBlock::Create(context, "flush", entry);
        llvm::BasicBlock* run = llvm::BasicBlock::Create(context, "run", entry);
        llvm::BasicBlock* restore = llvm::BasicBlock::Create(context, "restore", entry);
        llvm::BasicBlock* done = llvm::BasicBlock::Create(context, "done", entry);
        llvm::IRBuilder<> at(start);

        // The caller's mode and the flushing one, and what the register's write takes for each: the value itself for
        // FPCR, a stack slot holding it for MXCSR.
        llvm::Function* set = nullptr;
        llvm::Value* mode = nullptr;
        llvm::Value* flushingMode = nullptr;
        llvm::Value* toFlush = nullptr;
        llvm::Value* toRestore = nullptr;
        if (arm)
        {
            llvm::Function* get = llvm::Intrinsic::getOrInsertDeclaration(&module_, llvm::Intrinsic::aarch64_get_fpcr);
            set = llvm::Intrinsic::getOrInsertDeclaration(&module_, llvm::Intrinsic::aarch64_set_fpcr);
            mode = at.CreateCall(get, {}, "fpcr");
            flushingMode = at.CreateOr(mode, kFpcrFz, "fpcr.fz");
            toFlush = flushingMode;
            toRestore = mode;
        }
        else
        {
            llvm::Function* get = llvm::Intrinsic::getOrInsertDeclaration(&module_, llvm::Intrinsic::x86_sse_stmxcsr);
            set = llvm::Intrinsic::getOrInsertDeclaration(&module_, llvm::Intrinsic::x86_sse_ldmxcsr);
            llvm::AllocaInst* saved = at.CreateAlloca(at.getInt32Ty(), nullptr, "mxcsr");
            llvm::AllocaInst* wanted = at.CreateAlloca(at.getInt32Ty(), nullptr, "mxcsr.ftz");
            at.CreateCall(get, {saved});
            mode = at.CreateLoad(at.getInt32Ty(), saved, "mode");
            flushingMode = at.CreateOr(mode, kMxcsrFtz | kMxcsrDaz, "mode.ftz");
            at.CreateStore(flushingMode, wanted);
            toFlush = wanted;
            toRestore = saved;
        }
        llvm::Value* flushing = at.CreateICmpEQ(mode, flushingMode, "flushing");
        at.CreateCondBr(flushing, run, flush);

        at.SetInsertPoint(flush);
        at.CreateCall(set, {toFlush});
        at.CreateBr(run);

        at.SetInsertPoint(run);
        std::vector<llvm::Value*> args;
        for (llvm::Argument& arg : entry->args())
        {
            arg.setName(function.getArg(arg.getArgNo())->getName());
            args.push_back(&arg);
        }
        const bool returns = !entry->getReturnType()->isVoidTy();
        llvm::CallInst* result = at.CreateCall(&function, args, returns ? "result" : "");
        at.CreateCondBr(flushing, done, restore);

        at.SetInsertPoint(restore);
        at.CreateCall(set, {toRestore});
        at.CreateBr(done);

        at.SetInsertPoint(done);
        if (returns)
            at.CreateRet(result);
        else
            at.CreateRetVoid();

        std::string problem;
        llvm::raw_string_ostream os(problem);
        if (llvm::verifyFunction(*entry, &os))
            error(location, "internal error: the IR of " + quoted(name) + "'s entry point is invalid: " + problem);
    }
} // namespace fminus::ir::detail
