// src/ir/Denormals.h: the code generator's constant folder, for Denormals::Flush (CodeGen.h; Denormals.cpp has the
// rest). An IRBuilder folds an operation on two constants as it builds it, in IEEE arithmetic, and no function
// attribute reaches it: with `k` frozen at 1e-30, `k[i] * 1e-10f` would be built as the subnormal 1e-40, where the same
// code run flushing computes 0, and where LLVM, folding it later with the function's denormal_fpenv, would give 0 too.
// DenormalFolder is LLVM's ConstantFolder, except that under Flush it folds floating-point arithmetic and comparisons
// as the flushing hardware computes them: a subnormal operand reads as zero, and a subnormal result is zero, each with
// its sign (what LLVM's ConstantFolding does in a function marked denormal_fpenv(preservesign)). Under Preserve it is
// ConstantFolder, bit for bit.
#pragma once

#include <llvm/IR/ConstantFolder.h>
#include <llvm/IR/IRBuilderFolder.h>

namespace fminus::ir::detail
{
    class DenormalFolder final : public llvm::IRBuilderFolder
    {
    public:
        explicit DenormalFolder(bool flush) : flush_(flush) {}

        // The folds that see subnormals: fadd, fsub, fmul, fdiv and frem, and fcmp (Denormals.cpp).
        llvm::Value* FoldBinOp(llvm::Instruction::BinaryOps opcode, llvm::Value* lhs, llvm::Value* rhs) const override;
        llvm::Value* FoldBinOpFMF(llvm::Instruction::BinaryOps opcode, llvm::Value* lhs, llvm::Value* rhs,
                                  llvm::FastMathFlags flags) const override;
        llvm::Value* FoldCmp(llvm::CmpInst::Predicate predicate, llvm::Value* lhs, llvm::Value* rhs) const override;

        // The rest are ConstantFolder's. fneg only flips a sign bit, which flushing leaves alone (Arm's FNEG, SSE's
        // xorps), and FMinus's casts take no subnormal to anything that differs (a float below 1 is 0 as an int).
        llvm::Value* FoldExactBinOp(llvm::Instruction::BinaryOps opcode, llvm::Value* lhs, llvm::Value* rhs,
                                    bool isExact) const override
        {
            return folder_.FoldExactBinOp(opcode, lhs, rhs, isExact);
        }

        llvm::Value* FoldNoWrapBinOp(llvm::Instruction::BinaryOps opcode, llvm::Value* lhs, llvm::Value* rhs,
                                     bool hasNUW, bool hasNSW) const override
        {
            return folder_.FoldNoWrapBinOp(opcode, lhs, rhs, hasNUW, hasNSW);
        }

        llvm::Value* FoldUnOpFMF(llvm::Instruction::UnaryOps opcode, llvm::Value* value,
                                 llvm::FastMathFlags flags) const override
        {
            return folder_.FoldUnOpFMF(opcode, value, flags);
        }

        llvm::Value* FoldGEP(llvm::Type* type, llvm::Value* pointer, llvm::ArrayRef<llvm::Value*> indices,
                             llvm::GEPNoWrapFlags flags) const override
        {
            return folder_.FoldGEP(type, pointer, indices, flags);
        }

        llvm::Value* FoldSelect(llvm::Value* condition, llvm::Value* ifTrue, llvm::Value* ifFalse,
                                llvm::FastMathFlags flags) const override
        {
            return folder_.FoldSelect(condition, ifTrue, ifFalse, flags);
        }

        llvm::Value* FoldExtractValue(llvm::Value* aggregate, llvm::ArrayRef<unsigned> indices) const override
        {
            return folder_.FoldExtractValue(aggregate, indices);
        }

        llvm::Value* FoldInsertValue(llvm::Value* aggregate, llvm::Value* value,
                                     llvm::ArrayRef<unsigned> indices) const override
        {
            return folder_.FoldInsertValue(aggregate, value, indices);
        }

        llvm::Value* FoldExtractElement(llvm::Value* vector, llvm::Value* index) const override
        {
            return folder_.FoldExtractElement(vector, index);
        }

        llvm::Value* FoldInsertElement(llvm::Value* vector, llvm::Value* element, llvm::Value* index) const override
        {
            return folder_.FoldInsertElement(vector, element, index);
        }

        llvm::Value* FoldShuffleVector(llvm::Value* a, llvm::Value* b, llvm::ArrayRef<int> mask) const override
        {
            return folder_.FoldShuffleVector(a, b, mask);
        }

        llvm::Value* FoldCast(llvm::Instruction::CastOps opcode, llvm::Value* value, llvm::Type* type) const override
        {
            return folder_.FoldCast(opcode, value, type);
        }

        llvm::Value* FoldIntrinsic(llvm::Intrinsic::ID id, llvm::ArrayRef<llvm::Value*> operands, llvm::Type* type,
                                   llvm::FastMathFlags flags, llvm::Function* function) const override
        {
            return folder_.FoldIntrinsic(id, operands, type, flags, function);
        }

        llvm::Value* CreatePointerCast(llvm::Constant* constant, llvm::Type* type) const override
        {
            return folder_.CreatePointerCast(constant, type);
        }

        llvm::Value* CreatePointerBitCastOrAddrSpaceCast(llvm::Constant* constant, llvm::Type* type) const override
        {
            return folder_.CreatePointerBitCastOrAddrSpaceCast(constant, type);
        }

    private:
        llvm::ConstantFolder folder_;
        bool flush_;
    };
} // namespace fminus::ir::detail
