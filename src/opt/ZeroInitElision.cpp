// src/opt/ZeroInitElision.cpp: zero-init elision (ZeroInitElision.h). For each local with a memset, the pass looks
// for a writer that covers what the loads read, and proves three things about the memset M, the writer and the loads:
// order (no path goes from M to a load without completing the writer, or skipping it under a condition the load
// rules out), stability (the values the writer's length and its guard are computed from do not change between M and
// a load) and range (each load reads below the writer's length). The proofs are about the function as it is, with
// every memset in place, so several memsets of one local may go at once.
//
// Why that is enough. Take an execution that reaches a load R, and the last M before it, which exists because M
// dominates R. Between the two, control completed the writer or took its guard's skipping edge: the search from M
// finds no path to R that does neither. Take the last time it did either before R. Each value the writer's length and
// the guard's operands are computed from (an instruction, or a loop's iteration, which its header sets) was last
// defined before that time. One whose definition dominates M is not defined again between M and R: if it were, a path
// from its definition to R would avoid M, and so would one from the function's entry through it, though M dominates R.
// For any other, the same search from its definition finds no path to R that does not complete or skip the writer, or
// pass M. So at R those values are the ones the writer, or its guard, last saw. Had the guard skipped it, its
// condition was false then, so false at R; but R is only reached where it holds. So the writer completed: a loop
// writer was entered through its preheader, with its induction variable at 0, and ran all its iterations, storing
// [0, end); a fill stored [0, end) in one call; and `end` is the same at R. Later stores only replace bytes with other
// values, and nothing else reads the local. So R reads bytes the writer stored after M, never one M stored, and M can
// go.
//
// The ranges are exact integers, not wrapped ones: an access outside the local is undefined behaviour, so in an
// execution without any, every byte the writer and the loads touch lies inside it, and the bounds computed below are
// checked to stay far from 2^64.
#include "opt/ZeroInitElision.h"

#include <llvm/ADT/APInt.h>
#include <llvm/ADT/SmallPtrSet.h>
#include <llvm/ADT/SmallVector.h>
#include <llvm/Analysis/LoopInfo.h>
#include <llvm/Analysis/ScalarEvolution.h>
#include <llvm/Analysis/ScalarEvolutionExpressions.h>
#include <llvm/IR/CFG.h>
#include <llvm/IR/CmpPredicate.h>
#include <llvm/IR/DataLayout.h>
#include <llvm/IR/Dominators.h>
#include <llvm/IR/Function.h>
#include <llvm/IR/Instructions.h>
#include <llvm/IR/IntrinsicInst.h>
#include <llvm/IR/Module.h>

#include <cstdint>
#include <optional>
#include <vector>

namespace fminus::opt
{
    namespace
    {
        // Every byte count the proofs compute stays below 2^62, so nothing in 64-bit arithmetic on them wraps.
        constexpr unsigned kBoundBits = 62;
        constexpr unsigned kWideBits = 128;   // the width the bounds are checked in

        // What a function does with one local: every use of its address, followed through getelementptrs. A local
        // used any other way (as a call's argument, through a phi or a select, stored as a value, cast to an integer)
        // escapes, and the pass leaves it alone.
        struct Accesses
        {
            std::vector<llvm::LoadInst*> loads;
            std::vector<llvm::StoreInst*> stores;
            std::vector<llvm::IntrinsicInst*> fills;   // memset, memset.pattern, memcpy and memmove into it
            std::vector<llvm::MemSetInst*> memsets;    // the memsets the pass may remove: those that are not volatile
        };

        std::optional<Accesses> accessesOf(llvm::AllocaInst& slot)
        {
            Accesses accesses;
            llvm::SmallVector<llvm::Value*, 8> pointers{&slot};
            while (!pointers.empty())
            {
                const llvm::Value* pointer = pointers.pop_back_val();
                for (const llvm::Use& use : pointer->uses())
                {
                    auto* user = llvm::dyn_cast<llvm::Instruction>(use.getUser());
                    if (user == nullptr)
                        return std::nullopt;
                    const bool first = use.getOperandNo() == 0;   // a getelementptr's base, an intrinsic's destination
                    if (auto* address = llvm::dyn_cast<llvm::GetElementPtrInst>(user); address != nullptr && first)
                        pointers.push_back(address);
                    else if (auto* load = llvm::dyn_cast<llvm::LoadInst>(user))
                        accesses.loads.push_back(load);
                    else if (auto* store = llvm::dyn_cast<llvm::StoreInst>(user);
                             store != nullptr && use.getOperandNo() == llvm::StoreInst::getPointerOperandIndex())
                        accesses.stores.push_back(store);
                    else if (llvm::isa<llvm::ICmpInst>(user) || user->isLifetimeStartOrEnd())
                        continue;   // comparing addresses reads nothing, and a lifetime marker stores nothing
                    else if (auto* memset = llvm::dyn_cast<llvm::MemSetInst>(user); memset != nullptr && first)
                    {
                        accesses.fills.push_back(memset);
                        if (!memset->isVolatile())
                            accesses.memsets.push_back(memset);
                    }
                    else if (llvm::isa<llvm::MemSetPatternInst, llvm::MemTransferInst>(user) && first)
                        accesses.fills.push_back(llvm::cast<llvm::IntrinsicInst>(user));
                    else
                        return std::nullopt;   // a memcpy out of it reads it too: as good as an escape here
                }
            }
            return accesses;
        }

        // What writes every byte of [0, end) of a local, and the point past which it has. A loop has, once control
        // enters its exit block: it has one exit, which only its latch leads to, and the store runs on every
        // iteration, so a loop that completes has stored at least one element. A fill (a memset or a copy) has once
        // control passes it, and may have stored nothing.
        struct Writer
        {
            const llvm::SCEV* end = nullptr;
            std::uint64_t least = 0;
            const llvm::Loop* loop = nullptr;     // a loop writer's loop, which must not hold the memset
            llvm::BasicBlock* exit = nullptr;     // a loop writer: entering this block completes it
            llvm::Instruction* call = nullptr;    // a fill: passing it completes it
            llvm::Instruction* entry = nullptr;   // what the memset must dominate: the preheader's branch, or the call
            // The branch that enters the writer or goes around it, if there is one (a loop's guard): its block, where
            // its other edge goes, and the comparison that holds when it enters.
            llvm::BasicBlock* guard = nullptr;
            llvm::BasicBlock* skip = nullptr;
            llvm::CmpPredicate predicate;
            const llvm::SCEV* lhs = nullptr;
            const llvm::SCEV* rhs = nullptr;
        };

        // The proofs for one local, its accesses and the writers among them.
        class Local
        {
        public:
            Local(llvm::AllocaInst& slot, Accesses accesses, llvm::DominatorTree& dominators, llvm::LoopInfo& loops,
                  llvm::ScalarEvolution& evolution)
                : slot_(slot)
                , accesses_(std::move(accesses))
                , dominators_(dominators)
                , loops_(loops)
                , evolution_(evolution)
                , layout_(slot.getModule()->getDataLayout())
                , loads_(accesses_.loads.begin(), accesses_.loads.end())
            {
                for (llvm::StoreInst* store : accesses_.stores)
                {
                    if (std::optional<Writer> writer = loopWriter(*store))
                        writers_.push_back(*writer);
                }
                for (llvm::IntrinsicInst* fill : accesses_.fills)
                {
                    if (std::optional<Writer> writer = fillWriter(*fill))
                        writers_.push_back(*writer);
                }
            }

            const Accesses& accesses() const
            {
                return accesses_;
            }

            // Whether no load can read a byte `memset` stores: some writer proves it, without its guard or with it.
            bool removable(llvm::MemSetInst& memset) const
            {
                if (accesses_.loads.empty())
                    return true;   // nothing reads the local at all (DSE removes such memsets before this pass runs)
                for (const Writer& writer : writers_)
                {
                    if (proves(memset, writer, false) || (writer.guard != nullptr && proves(memset, writer, true)))
                        return true;
                }
                return false;
            }

        private:
            // The bytes a type takes in memory, if fixed.
            std::optional<std::uint64_t> sizeOf(llvm::Type* type) const
            {
                const llvm::TypeSize size = layout_.getTypeStoreSize(type);
                if (size.isScalable())
                    return std::nullopt;
                return size.getFixedValue();
            }

            // How many bytes past the local's start `pointer` is, as a SCEV of the index type, or null.
            const llvm::SCEV* offsetOf(llvm::Value* pointer) const
            {
                const llvm::SCEV* address = evolution_.getSCEV(pointer);
                if (evolution_.getPointerBase(address) != evolution_.getSCEV(&slot_))
                    return nullptr;
                const llvm::SCEV* offset = evolution_.removePointerBase(address);
                if (llvm::isa<llvm::SCEVCouldNotCompute>(offset) || !offset->getType()->isIntegerTy())
                    return nullptr;
                return offset;
            }

            // A loop that stores element i of the local on every iteration i, from 0: `for (i = 0; i < n; ++i)
            // tmp[i] = ...;` once LLVM has rotated it and given it its induction variable. It writes
            // [0, size * (count + 1)), count being how many times its backedge runs.
            std::optional<Writer> loopWriter(llvm::StoreInst& store) const
            {
                llvm::Loop* loop = loops_.getLoopFor(store.getParent());
                if (loop == nullptr)
                    return std::nullopt;
                llvm::BasicBlock* preheader = loop->getLoopPreheader();
                llvm::BasicBlock* latch = loop->getLoopLatch();
                llvm::BasicBlock* exit = loop->getUniqueExitBlock();
                if (preheader == nullptr || latch == nullptr || exit == nullptr || loop->getExitingBlock() != latch
                    || !loop->hasDedicatedExits() || !dominators_.dominates(store.getParent(), latch))
                    return std::nullopt;   // a loop with a break, or a store that some iteration may skip
                const std::optional<std::uint64_t> size = sizeOf(store.getValueOperand()->getType());
                const auto* offset = llvm::dyn_cast_or_null<llvm::SCEVAddRecExpr>(offsetOf(store.getPointerOperand()));
                if (!size || offset == nullptr || offset->getLoop() != loop || !offset->isAffine()
                    || !offset->getStart()->isZero())
                    return std::nullopt;
                const auto* step = llvm::dyn_cast<llvm::SCEVConstant>(offset->getStepRecurrence(evolution_));
                if (step == nullptr || step->getAPInt() != *size)
                    return std::nullopt;   // element after element, with no gaps
                llvm::Type* type = offset->getType();
                const llvm::SCEV* count = evolution_.getBackedgeTakenCount(loop);
                const auto* most =
                    llvm::dyn_cast<llvm::SCEVConstant>(evolution_.getConstantMaxBackedgeTakenCount(loop));
                if (llvm::isa<llvm::SCEVCouldNotCompute>(count) || most == nullptr
                    || count->getType()->getIntegerBitWidth() > type->getIntegerBitWidth())
                    return std::nullopt;
                bool overflow = false;
                const llvm::APInt bytes = (most->getAPInt().zext(kWideBits) + 1).umul_ov(llvm::APInt(kWideBits, *size),
                                                                                        overflow);
                if (overflow || bytes.uge(llvm::APInt::getOneBitSet(kWideBits, kBoundBits)))
                    return std::nullopt;
                Writer writer;
                writer.end = evolution_.getMulExpr(
                    evolution_.getConstant(type, *size),
                    evolution_.getAddExpr(evolution_.getNoopOrZeroExtend(count, type), evolution_.getOne(type)));
                writer.least = *size;
                writer.loop = loop;
                writer.exit = exit;
                writer.entry = preheader->getTerminator();
                guard(*preheader, writer);
                return writer;
            }

            // A memset, memset.pattern, memcpy or memmove whose destination is the local's start: what LLVM's loop
            // idiom pass makes of a loop that fills it or copies into it. It writes [0, length).
            std::optional<Writer> fillWriter(llvm::IntrinsicInst& fill) const
            {
                const llvm::SCEV* offset = offsetOf(fill.getArgOperand(0));
                if (offset == nullptr || !offset->isZero())
                    return std::nullopt;
                llvm::Type* type = offset->getType();
                const llvm::SCEV* length = evolution_.getSCEV(fill.getArgOperand(2));   // a memset.pattern's: copies
                if (length->getType()->getIntegerBitWidth() > type->getIntegerBitWidth())
                    return std::nullopt;
                length = evolution_.getNoopOrZeroExtend(length, type);
                if (const auto* pattern = llvm::dyn_cast<llvm::MemSetPatternInst>(&fill))
                {
                    llvm::Type* element = pattern->getArgOperand(1)->getType();
                    const std::optional<std::uint64_t> size = sizeOf(element);
                    if (!size || layout_.getTypeAllocSize(element) != *size)
                        return std::nullopt;   // its copies sit an allocation size apart: no gaps only when equal
                    length = evolution_.getMulExpr(length, evolution_.getConstant(type, *size));
                }
                Writer writer;
                writer.end = length;
                writer.call = &fill;
                writer.entry = &fill;
                guard(*fill.getParent(), writer);
                return writer;
            }

            // The branch that enters `block` or goes around it, when `block` is reached only through it and it tests a
            // comparison of integers or pointers (a loop's guard, `n > 0`).
            void guard(llvm::BasicBlock& block, Writer& writer) const
            {
                llvm::BasicBlock* guard = block.getUniquePredecessor();
                auto* branch = guard != nullptr ? llvm::dyn_cast<llvm::CondBrInst>(guard->getTerminator()) : nullptr;
                auto* compare = branch != nullptr ? llvm::dyn_cast<llvm::ICmpInst>(branch->getCondition()) : nullptr;
                if (compare == nullptr || !evolution_.isSCEVable(compare->getOperand(0)->getType()))
                    return;
                const bool entersOnTrue = branch->getSuccessor(0) == &block;
                llvm::BasicBlock* skip = branch->getSuccessor(entersOnTrue ? 1 : 0);
                if (skip == &block)
                    return;
                writer.guard = guard;
                writer.skip = skip;
                writer.predicate = entersOnTrue ? compare->getCmpPredicate() : compare->getInverseCmpPredicate();
                writer.lhs = evolution_.getSCEV(compare->getOperand(0));
                writer.rhs = evolution_.getSCEV(compare->getOperand(1));
            }

            // Whether `expression`, the writer's length or a guard's operand, has at every load the value it had when
            // the writer ran (see the top of the file). Each instruction it reads is defined before the memset, or is
            // behind the writer as the loads are: no load is reached from it without completing the writer first (a
            // bound read from a module's field after the memset). Each loop it counts holds every load, and the
            // memset, or its header is behind the writer in the same way; so one iteration of it runs throughout.
            bool stable(const llvm::SCEV* expression, const llvm::MemSetInst& memset, const Writer& writer,
                        bool guarded) const
            {
                return !llvm::SCEVExprContains(expression, [&](const llvm::SCEV* node) {
                    if (const auto* unknown = llvm::dyn_cast<llvm::SCEVUnknown>(node))
                    {
                        auto* definition = llvm::dyn_cast<llvm::Instruction>(unknown->getValue());
                        return definition != nullptr && !dominators_.dominates(definition, &memset)
                               && !reached(*definition->getParent(), std::next(definition->getIterator()), memset,
                                           writer, guarded)
                                       .empty();
                    }
                    if (const auto* recurrence = llvm::dyn_cast<llvm::SCEVAddRecExpr>(node))
                    {
                        llvm::BasicBlock* header = recurrence->getLoop()->getHeader();
                        return !holdsAll(*recurrence->getLoop())
                               || (!recurrence->getLoop()->contains(&memset)
                                   && !reached(*header, header->begin(), memset, writer, guarded).empty());
                    }
                    return false;
                });
            }

            bool holdsAll(const llvm::Loop& loop) const
            {
                for (const llvm::LoadInst* load : accesses_.loads)
                {
                    if (!loop.contains(load))
                        return false;
                }
                return true;
            }

            // The end of what `load` reads, as a byte offset from the local's start: a bound on it for every time it
            // runs. A load in a loop entered after the memset, whose offset grows by a constant each iteration
            // (`tmp[i]`), reads furthest on its last iteration.
            std::optional<const llvm::SCEV*> endOfRead(llvm::LoadInst& load, const llvm::Instruction& memset) const
            {
                const llvm::SCEV* offset = offsetOf(load.getPointerOperand());
                const std::optional<std::uint64_t> size = sizeOf(load.getType());
                if (offset == nullptr || !size)
                    return std::nullopt;
                if (const auto* recurrence = llvm::dyn_cast<llvm::SCEVAddRecExpr>(offset);
                    recurrence != nullptr && !recurrence->getLoop()->contains(&memset))
                {
                    const llvm::Loop* loop = recurrence->getLoop();
                    const auto* step = llvm::dyn_cast<llvm::SCEVConstant>(recurrence->getStepRecurrence(evolution_));
                    const llvm::SCEV* count = evolution_.getBackedgeTakenCount(loop);
                    const auto* most =
                        llvm::dyn_cast<llvm::SCEVConstant>(evolution_.getConstantMaxBackedgeTakenCount(loop));
                    if (!recurrence->isAffine() || !loop->contains(&load) || step == nullptr
                        || step->getAPInt().isNegative() || llvm::isa<llvm::SCEVCouldNotCompute>(count)
                        || most == nullptr
                        || count->getType()->getIntegerBitWidth() > offset->getType()->getIntegerBitWidth())
                        return std::nullopt;
                    // start + step * count + size, at its largest, must stay below the bound: then the offsets the
                    // load reads grow with each iteration, and none is past the last one's.
                    bool overflow = false;
                    bool more = false;
                    const llvm::APInt furthest =
                        evolution_.getSignedRangeMax(recurrence->getStart())
                            .sext(kWideBits)
                            .sadd_ov(step->getAPInt().sext(kWideBits).smul_ov(most->getAPInt().zext(kWideBits), more),
                                     overflow);
                    if (overflow || more || furthest.sge(llvm::APInt::getOneBitSet(kWideBits, kBoundBits) - *size))
                        return std::nullopt;
                    offset = recurrence->evaluateAtIteration(evolution_.getNoopOrZeroExtend(count, offset->getType()),
                                                             evolution_);
                }
                const llvm::SCEV* end = evolution_.getAddExpr(offset, evolution_.getConstant(offset->getType(), *size));
                const bool left = llvm::SCEVExprContains(end, [&](const llvm::SCEV* node) {
                    const auto* recurrence = llvm::dyn_cast<llvm::SCEVAddRecExpr>(node);
                    return recurrence != nullptr && !recurrence->getLoop()->contains(&load);
                });
                if (left)
                    return std::nullopt;   // a value from a loop the load is not in: which iteration's is unclear
                return end;
            }

            // The loads control can reach from `at` in `block` before it reaches the memset again (from which the
            // search for it starts over) or completes the writer; and, when `skipping` is set, without taking the
            // edge that goes around the writer.
            std::vector<llvm::LoadInst*> reached(llvm::BasicBlock& block, llvm::BasicBlock::iterator at,
                                                 const llvm::MemSetInst& memset, const Writer& writer,
                                                 bool skipping) const
            {
                std::vector<llvm::LoadInst*> found;
                llvm::SmallPtrSet<llvm::BasicBlock*, 32> queued;
                llvm::SmallVector<llvm::BasicBlock*, 32> work;
                const auto scan = [&](llvm::BasicBlock& current, llvm::BasicBlock::iterator from) {
                    for (; from != current.end(); ++from)
                    {
                        llvm::Instruction& instruction = *from;
                        if (&instruction == &memset || &instruction == writer.call)
                            return;
                        if (auto* load = llvm::dyn_cast<llvm::LoadInst>(&instruction); load != nullptr
                                                                                      && loads_.contains(load))
                            found.push_back(load);
                    }
                    for (llvm::BasicBlock* next : llvm::successors(&current))
                    {
                        if (next == writer.exit || (skipping && &current == writer.guard && next == writer.skip))
                            continue;
                        if (queued.insert(next).second)
                            work.push_back(next);
                    }
                };
                scan(block, at);
                while (!work.empty())
                {
                    llvm::BasicBlock* next = work.pop_back_val();
                    scan(*next, next->begin());
                }
                return found;
            }

            // The three proofs (the top of the file) for one memset and one writer, through its guard or not.
            bool proves(llvm::MemSetInst& memset, const Writer& writer, bool guarded) const
            {
                if (writer.call == &memset || (writer.loop != nullptr && writer.loop->contains(&memset)))
                    return false;
                const llvm::Instruction* entry = guarded ? writer.guard->getTerminator() : writer.entry;
                if (!dominators_.dominates(&memset, entry))
                    return false;
                for (const llvm::LoadInst* load : accesses_.loads)
                {
                    if (!dominators_.dominates(&memset, load))
                        return false;
                }

                // Stability.
                const auto fixed = [&](const llvm::SCEV* expression) {
                    return stable(expression, memset, writer, guarded);
                };
                if (!fixed(writer.end) || (guarded && (!fixed(writer.lhs) || !fixed(writer.rhs))))
                    return false;

                // Order: from the memset, every load is behind the writer; from the guard's skipping edge, behind
                // the writer or a condition the load rules out.
                if (!reached(*memset.getParent(), std::next(memset.getIterator()), memset, writer, guarded).empty())
                    return false;
                if (guarded)
                {
                    for (llvm::LoadInst* load : reached(*writer.skip, writer.skip->begin(), memset, writer, false))
                    {
                        if (!evolution_.isKnownPredicateAt(writer.predicate, writer.lhs, writer.rhs, load))
                            return false;
                    }
                }

                // Range.
                llvm::Type* type = writer.end->getType();
                const llvm::SCEV* least = evolution_.getConstant(type, writer.least);
                for (llvm::LoadInst* load : accesses_.loads)
                {
                    const std::optional<const llvm::SCEV*> end = endOfRead(*load, memset);
                    if (!end || (*end)->getType() != type)
                        return false;
                    if (!evolution_.isKnownPredicate(llvm::ICmpInst::ICMP_ULE, *end, least)
                        && !evolution_.isKnownPredicateAt(llvm::ICmpInst::ICMP_ULE, *end, writer.end, load))
                        return false;
                }
                return true;
            }

            llvm::AllocaInst& slot_;
            Accesses accesses_;
            llvm::DominatorTree& dominators_;
            llvm::LoopInfo& loops_;
            llvm::ScalarEvolution& evolution_;
            const llvm::DataLayout& layout_;
            llvm::SmallPtrSet<llvm::LoadInst*, 16> loads_;
            std::vector<Writer> writers_;
        };
    } // namespace

    llvm::PreservedAnalyses ZeroInitElisionPass::run(llvm::Function& function, llvm::FunctionAnalysisManager& analyses)
    {
        // The locals worth a look, before any analysis is asked for: the entry block's static allocas (where the
        // code generator and the inliner put every local) that a memset writes and nothing lets escape.
        std::vector<std::pair<llvm::AllocaInst*, Accesses>> candidates;
        for (llvm::Instruction& instruction : function.getEntryBlock())
        {
            auto* slot = llvm::dyn_cast<llvm::AllocaInst>(&instruction);
            if (slot == nullptr || !slot->isStaticAlloca())
                continue;
            if (std::optional<Accesses> accesses = accessesOf(*slot); accesses && !accesses->memsets.empty())
                candidates.emplace_back(slot, std::move(*accesses));
        }
        if (candidates.empty())
            return llvm::PreservedAnalyses::all();

        auto& dominators = analyses.getResult<llvm::DominatorTreeAnalysis>(function);
        auto& loops = analyses.getResult<llvm::LoopAnalysis>(function);
        auto& evolution = analyses.getResult<llvm::ScalarEvolutionAnalysis>(function);
        std::vector<llvm::MemSetInst*> dead;
        for (auto& [slot, accesses] : candidates)
        {
            const Local local(*slot, std::move(accesses), dominators, loops, evolution);
            for (llvm::MemSetInst* memset : local.accesses().memsets)
            {
                if (local.removable(*memset))
                    dead.push_back(memset);   // removed once every proof is done: they are about the function as it is
            }
        }
        if (dead.empty())
            return llvm::PreservedAnalyses::all();
        for (llvm::MemSetInst* memset : dead)
            memset->eraseFromParent();
        llvm::PreservedAnalyses preserved;
        preserved.preserveSet<llvm::CFGAnalyses>();   // a call is gone; no block or branch changed
        return preserved;
    }
} // namespace fminus::opt
