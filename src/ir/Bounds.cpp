// src/ir/Bounds.cpp: bounds checks (Options::boundsCheck; opt/BoundsCheck.h). Where the code generator can see the
// array an access reaches, it checks the access against the array's extent:
//   - an array's name stands for a pointer to its first element (read(), Memory.cpp), which carries the array's extent:
//     its count, its name as the source has it and its type (arrayExtent). A local's, a module's field's (m.taps,
//     p->taps, or taps in a method), an element's of an array of modules (voices[v].taps), a constant of the top
//     level's, an array a parallel region uses of the function around it;
//   - p + i and p - i (pointerArithmetic, Memory.cpp) and &p[i] (lowerAddressOf) carry it on, i elements on
//     (movedExtent), and are checked as pointers formed, which may point to any of its elements or one past the last,
//     as C allows (width 0);
//   - p[i], *p and p->y (element() and lvalue(), Memory.cpp) are checked as elements, one of its own (width 1);
//   - poly_load(p) and poly_store(p, v) (lowerPolyMemory, Calls.cpp) as every lane they reach (width: the poly lanes),
//     or, with a mask, as each lane the mask takes (checkLanes).
// Anything else carries none, and its accesses are not checked: a pointer a variable, a field or a parameter holds
// (`float* p = buf;` is unchecked through p), what a call returns or a ?: chooses, a void*.
//
// A check compares the element's index, an i64 from the array's first (a negative one is a huge unsigned number),
// against the extent, unsigned, and branches, likely in bounds, to the code after it; the other side calls
// opt::kBoundsStopFunction with the check's number and the index, and is unreachable after the call, which the pass at
// the end of the pipeline makes a return (opt/BoundsCheck.cpp). Each check's description, opt::BoundsSite, goes into
// the module's named metadata (opt::addBoundsSite), numbered from 1 in the order they are lowered. An index that is a
// constant within the array is no check at all, so `a[3]` costs nothing; one that is a constant outside it fails
// whenever it runs, and is a branch too (on false), so that what follows stays reachable to the code generator's own
// rules (a non-void function that can reach its end is an error, with or without the checks). The initialiser of a
// constant of the top level runs nowhere (Constants.cpp lowers it into a function it deletes): it has no checks.
//
//   %idxprom = sext i32 %i to i64
//   %bounds.in = icmp ult i64 %idxprom, 16
//   br i1 %bounds.in, label %bounds.ok, label %bounds.fail, !prof !likely
// bounds.ok:
//   %arrayidx = getelementptr inbounds float, ptr %arraydecay, i64 %idxprom
//   ...
// bounds.fail:
//   call void @fminus.bounds.stop(i32 1, i64 %idxprom)
//   unreachable
#include "ir/Lowering.h"
#include "opt/BoundsCheck.h"

#include <llvm/IR/Constants.h>
#include <llvm/IR/MDBuilder.h>

#include <cstdint>
#include <string>
#include <utility>
#include <vector>

namespace fminus::ir::detail
{
    // The extent of the pointer an array of `array`'s type stands for, at its first element, named `name`; null when
    // the checks are off.
    const Lowering::Extent* Lowering::arrayExtent(ast::Type array, std::string name)
    {
        if (!boundsCheck_ || !ast::isArray(array))
            return nullptr;
        return &extents_.emplace_back(Extent{nullptr, array.count, std::move(name), array});
    }

    // The extent of a pointer `index` (an i64) elements on from `extent`'s.
    const Lowering::Extent* Lowering::movedExtent(const Extent& extent, llvm::Value* index)
    {
        return &extents_.emplace_back(Extent{elementOf(extent, index), extent.count, extent.name, extent.type});
    }

    // The index from the array's first of the element `index` (an i64, or null for 0) on from `extent`'s pointer. The
    // indexes are ints widened, and their sums do not overflow an i64.
    llvm::Value* Lowering::elementOf(const Extent& extent, llvm::Value* index)
    {
        if (extent.offset == nullptr)
            return index != nullptr ? index : builder_.getInt64(0);
        if (index == nullptr)
            return extent.offset;
        return builder_.CreateAdd(extent.offset, index, "bounds.element");
    }

    // Checks that the `width` elements from `index` (an i64, from the array's first) are the array's: width 1 an
    // element, a poly float's lanes a span of them, 0 a pointer formed, which may also be one past the last.
    void Lowering::checkBounds(SourceLocation location, const Extent& extent, llvm::Value* index, unsigned width)
    {
        const auto count = static_cast<std::uint64_t>(extent.count);
        llvm::Value* inBounds = nullptr;
        if (width == 1)
            inBounds = builder_.CreateICmpULT(index, builder_.getInt64(count), "bounds.in");
        else if (width <= count)
            inBounds = builder_.CreateICmpULE(index, builder_.getInt64(count - width), "bounds.in");
        else
            inBounds = builder_.getFalse();   // more lanes than the array has elements
        failBounds(location, extent, width, inBounds, index);
    }

    // Checks a masked poly_load or poly_store from `extent`'s pointer: each lane `mask` takes must be one of the
    // array's elements. The index a failed check records is the first lane's that is not.
    void Lowering::checkLanes(SourceLocation location, const Extent& extent, llvm::Value* mask)
    {
        const unsigned width = polyLanes_;
        llvm::Type* indexes = llvm::FixedVectorType::get(builder_.getInt64Ty(), width);
        llvm::Value* first = elementOf(extent, nullptr);
        std::vector<llvm::Constant*> steps;
        for (unsigned lane = 0; lane < width; ++lane)
            steps.push_back(builder_.getInt64(lane));
        llvm::Value* each = builder_.CreateAdd(builder_.CreateVectorSplat(width, first),
                                               llvm::ConstantVector::get(steps), "bounds.lanes");
        llvm::Value* outside = builder_.CreateICmpUGE(
            each, llvm::ConstantInt::get(indexes, static_cast<std::uint64_t>(extent.count)), "bounds.outside");
        llvm::Value* taken = builder_.CreateBitCast(builder_.CreateAnd(outside, mask), builder_.getIntNTy(width));
        llvm::Value* inBounds = builder_.CreateICmpEQ(taken, builder_.getIntN(width, 0), "bounds.in");
        llvm::Value* lane = builder_.CreateBinaryIntrinsic(llvm::Intrinsic::cttz, taken, builder_.getFalse());
        llvm::Value* index = builder_.CreateAdd(first, builder_.CreateZExt(lane, builder_.getInt64Ty()), "bounds.lane");
        failBounds(location, extent, 1, inBounds, index);
    }

    // The branch of a check, `inBounds` true when it passes, and its failure: opt::kBoundsStopFunction with the check's
    // number and `index`. Nothing when it is known to pass, or where nothing runs (a constant of the top level).
    void Lowering::failBounds(SourceLocation location, const Extent& extent, unsigned width, llvm::Value* inBounds,
                              llvm::Value* index)
    {
        if (const auto* known = llvm::dyn_cast<llvm::ConstantInt>(inBounds); known != nullptr && known->isOne())
            return;
        if (atTopLevel())
            return;
        const opt::BoundsSite site{diagnostics_.file(location.file).name,
                                   location.line,
                                   location.column,
                                   extent.name,
                                   ast::typeName(extent.type),
                                   extent.count,
                                   static_cast<int>(width)};
        const std::int32_t number = opt::addBoundsSite(module_, site);
        llvm::BasicBlock* here = builder_.GetInsertBlock();
        llvm::Function* function = here->getParent();
        llvm::LLVMContext& context = module_.getContext();
        llvm::BasicBlock* ok = llvm::BasicBlock::Create(context, "bounds.ok", function, here->getNextNode());
        llvm::BasicBlock* fail = llvm::BasicBlock::Create(context, "bounds.fail", function);
        builder_.CreateCondBr(inBounds, ok, fail, llvm::MDBuilder(context).createLikelyBranchWeights());
        builder_.SetInsertPoint(fail);
        builder_.CreateCall(opt::boundsStopFunction(module_), {builder_.getInt32(static_cast<std::uint32_t>(number)),
                                                               index});
        builder_.CreateUnreachable();
        builder_.SetInsertPoint(ok);
    }

    // How the source names the array `expr` stands for, for a failed check's message: a variable's name, a field's
    // (m.taps, p->taps, voices[v].taps, this->taps), an element's index as written when it is a name or a number
    // (voices[...] otherwise); empty when there is no name (what a call returns).
    std::string Lowering::spelled(const ast::Expr& expr)
    {
        if (const auto* variable = std::get_if<ast::VariableExpr>(&expr.node))
            return variable->name;
        if (const auto* number = std::get_if<ast::NumberExpr>(&expr.node))
            return number->spelling;
        if (std::holds_alternative<ast::ThisExpr>(expr.node))
            return "this";
        if (const auto* member = std::get_if<ast::MemberExpr>(&expr.node))
        {
            const std::string base = spelled(*member->base);
            return base.empty() ? member->name : base + (member->arrow ? "->" : ".") + member->name;
        }
        if (const auto* index = std::get_if<ast::IndexExpr>(&expr.node))
        {
            const std::string base = spelled(*index->base);
            if (base.empty())
                return base;
            const bool simple = std::holds_alternative<ast::VariableExpr>(index->index->node)
                                || std::holds_alternative<ast::NumberExpr>(index->index->node);
            return base + "[" + (simple ? spelled(*index->index) : "...") + "]";
        }
        return {};
    }
} // namespace fminus::ir::detail
