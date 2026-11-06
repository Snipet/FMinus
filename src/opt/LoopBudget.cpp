// src/opt/LoopBudget.cpp: the loop budget (LoopBudget.h).
//
// Which functions change. A function counts when it has a cycle, or can call itself again:
//   - its cycles are CycleInfo's: every loop, and an irreducible cycle too (FMinus's structured code makes none, but a
//     pass might). A loop whose count ScalarEvolution knows is charged it in its preheader (plan(), below, says which
//     and why); any other cycle counts one at each branch back to its header. Every closed path through the code
//     passes through some cycle's header from inside that cycle, so no path can go round for ever uncounted;
//   - it can call itself again through its calls, and through the functions it hands a runtime function (a parallel
//     region's body, which fminus_rt_parallel_for runs). Its entry counts one, so a recursion that runs long without a
//     loop runs out too (and one that would run out of stack first, if the budget is the smaller).
// A function may stop when it counts, or calls a function that may stop (directly, or by handing it to a runtime
// function). Each one that may stop checks the flag after such a call, and returns at once if it is set. An indirect
// call is not followed (opt/EarlyReturn.h, reachedFunctions): FMinus has no function pointers, and the only indirect
// call it makes in the program is an entry point's call of its body under Denormals::Flush (ir/Denormals.cpp), whose
// wrapper restores the floating-point mode and returns what the body returned, which is what a stopped body's wrapper
// should do. A function marked kLoopBudgetHostAttribute is the host's and is left alone.
//
// In a function that counts, the count is a stack slot that mem2reg (PromoteMemToReg) makes a register once the code
// is in place, as the code generator's own variables become registers:
//
//   entry:
//     %budget.start = load atomic i64, ptr @fminus_rt_loop_budget monotonic, align 8
//     ...
//   vector.ph:                                       ; a loop with a count, charged it where it starts
//     %2 = lshr i64 %1, 2                            ; ScalarEvolution's count of its branches back, expanded
//     %budget.left = sub i64 %budget.start, %2
//     %budget.out = icmp slt i64 %budget.left, 0
//     br i1 %budget.out, label %budget.stop, label %vector.body, !prof !unlikely
//   ...
//   while.body.budget:                               ; any other: on each branch back to its header
//     %budget.left3 = sub i64 %budget.slot.0, 1
//     %budget.out4 = icmp slt i64 %budget.left3, 0
//     br i1 %budget.out4, label %budget.stop, label %while.body, !prof !unlikely
//   ...
//   budget.stop:                                     ; ran out: -1 for what is left, the flag set, and return
//     store atomic i64 -1, ptr @fminus_rt_loop_budget monotonic, align 8
//     store atomic i32 1, ptr getelementptr inbounds ({ i64, i32 }, ptr @fminus_rt_loop_budget, i32 0, i32 1)
//         monotonic, align 4
//     ret float 0.000000e+00
//
// Before each return, and before each call of a function that may stop, it writes the count back to the budget, and
// after such a call it checks the flag and reads the count again:
//
//     store atomic i64 %budget.slot.1, ptr @fminus_rt_loop_budget monotonic, align 8
//     %5 = call i32 @calls(i32 %4, float %x)
//     %budget.stopped = load atomic i32, ptr getelementptr inbounds (...) monotonic, align 4
//     %6 = icmp ne i32 %budget.stopped, 0
//     br i1 %6, label %budget.unwind, label %if.end.budget, !prof !unlikely
//   if.end.budget:
//     %budget.now = load atomic i64, ptr @fminus_rt_loop_budget monotonic, align 8
//   ...
//   budget.unwind:                                   ; the callee ran out: return, and write nothing
//     ret i32 0
//
// The branch to the stop is marked unlikely, so the code generator puts it out of the loop's way: on arm64 a loop's
// charge is a SUBS and a B.MI where it starts, and a branch back's a SUB and a TBZ. What runs out is reported once,
// by the function where the count went below zero; the functions that return after it write nothing, so `left` holds
// the -1 that function wrote.
//
// The count never wraps. `left` is never below -1: the host sets it to -1 or more (Program::setLoopBudget clamps it),
// a stop writes -1, and every other write is of a count that passed its check, 0 or more, or of what was read. So a
// function's count starts at -1 or more, a charge takes at most INT64_MAX (countOf charges no count that may be
// larger), and no subtraction goes below INT64_MIN: a negative `left` stays negative, however many calls a host makes
// without setting it again. (Were a stop to write its own count, two calls that ran out on a charge near 2^62 would
// take `left` near INT64_MIN, and a third's charge would wrap it positive and let its loop run.)
//
// A parallel region's threads share `left`, each part counting from what it read where it started. A function that
// may run on them (one handed to a runtime function, a region's body, and every function it reaches) writes its count
// back with an atomicrmw min, not a store, so that `left` never goes up while the program runs: a part that read it
// before another ran out cannot write a positive count over the -1, and every part that starts after a stop stops at
// its first check. Each thread then takes at most the budget, and the region its thread count times it. Elsewhere the
// write is a store, since no other thread writes `left` meanwhile, and the min would cost an atomic read-modify-write
// at each call and return (a compare-and-swap loop on x86-64).
//
// What a stopped call leaves in memory is whichever of the optimised code's stores had run. The optimiser keeps a
// module's field in a register through a loop and stores it after the loop (LICM), sinks and merges stores, so some of
// the call's writes may be missing, out of order with the source, or out of step with each other: the instance may be
// in a state the source never passes through. A host re-initialises it, or discards it, before running it again.
//
// The functions it changes get their attributes adjusted to what is now true of them: they read and write memory
// that is no argument's (the budget), they return (noreturn goes: a loop that never ended returns now), and their
// result may be 0 or null, so no attribute that says otherwise (nonnull, dereferenceable, a range, nofpclass, an
// argument it returns) stays on it or on a call of it (opt/EarlyReturn.h, mayReturnEarly).
#include "opt/LoopBudget.h"

#include "opt/EarlyReturn.h"

#include <llvm/ADT/DenseMap.h>
#include <llvm/ADT/STLExtras.h>
#include <llvm/ADT/SmallPtrSet.h>
#include <llvm/ADT/SmallVector.h>
#include <llvm/Analysis/AssumptionCache.h>
#include <llvm/Analysis/CycleAnalysis.h>
#include <llvm/Analysis/LoopInfo.h>
#include <llvm/Analysis/ScalarEvolution.h>
#include <llvm/Analysis/ScalarEvolutionExpressions.h>
#include <llvm/Analysis/TargetLibraryInfo.h>
#include <llvm/IR/CFG.h>
#include <llvm/IR/Constants.h>
#include <llvm/IR/DerivedTypes.h>
#include <llvm/IR/Dominators.h>
#include <llvm/IR/GlobalVariable.h>
#include <llvm/IR/IRBuilder.h>
#include <llvm/IR/Instructions.h>
#include <llvm/IR/MDBuilder.h>
#include <llvm/IR/Module.h>
#include <llvm/Transforms/Utils/BasicBlockUtils.h>
#include <llvm/Transforms/Utils/LoopUtils.h>
#include <llvm/Transforms/Utils/PromoteMemToReg.h>
#include <llvm/Transforms/Utils/ScalarEvolutionExpander.h>

#include <cstdint>
#include <iterator>
#include <limits>
#include <utility>
#include <vector>

namespace fminus::opt
{
    namespace
    {
        // The budget's type, runtime/Runtime.h's fminus_rt_loop_budget_state: { int64_t left; int32_t stopped; }.
        llvm::StructType* budgetType(llvm::LLVMContext& context)
        {
            return llvm::StructType::get(llvm::Type::getInt64Ty(context), llvm::Type::getInt32Ty(context));
        }

        struct Facts
        {
            bool done = false;         // changed already, by an earlier run of the pass (kLoopBudgetAttribute)
            bool cycles = false;       // has a cycle
            bool recursive = false;    // can call itself again
            bool mayStop = false;      // counts, or calls a function that may stop
            bool concurrent = false;   // may run on a parallel region's threads: handed to a runtime function, or
                                       // reached from one that is
            llvm::SmallVector<llvm::Function*, 8> reaches;   // the functions its calls run, with repeats
            llvm::SmallVector<llvm::Function*, 2> hands;     // the functions it hands a runtime function
        };

        // The longest count, in ScalarEvolution's terms, that a loop is charged before it starts: longer ones count a
        // branch back at a time. The usual are a few terms (n - 1, a vectorised loop's ((n - 16) / 16)).
        constexpr unsigned short kLongestCount = 12;

        bool counts(const Facts& facts)
        {
            return facts.cycles || facts.recursive;
        }

        // Whether `from` reaches itself again through the calls in `facts`.
        bool recursive(llvm::Function* from, const llvm::DenseMap<llvm::Function*, Facts>& facts)
        {
            llvm::SmallVector<llvm::Function*, 16> work(facts.at(from).reaches.begin(), facts.at(from).reaches.end());
            llvm::SmallPtrSet<llvm::Function*, 16> seen;
            while (!work.empty())
            {
                llvm::Function* next = work.pop_back_val();
                if (next == from)
                    return true;
                if (!seen.insert(next).second)
                    continue;
                const auto found = facts.find(next);
                if (found != facts.end())
                    work.append(found->second.reaches.begin(), found->second.reaches.end());
            }
            return false;
        }

        // One function's changes (the top of this file).
        class Instrument
        {
        public:
            Instrument(llvm::Function& function, const Facts& facts, const llvm::DenseMap<llvm::Function*, Facts>& all,
                       llvm::GlobalVariable& budget)
                : function_(function)
                , facts_(facts)
                , all_(all)
                , context_(function.getContext())
                , i64_(llvm::Type::getInt64Ty(context_))
                , i32_(llvm::Type::getInt32Ty(context_))
                , left_(&budget)
                , stopped_(llvm::ConstantExpr::getInBoundsGetElementPtr(
                      budgetType(context_), &budget,
                      llvm::ArrayRef<llvm::Constant*>{llvm::ConstantInt::get(i32_, 0), llvm::ConstantInt::get(i32_, 1)}))
                , unlikely_(llvm::MDBuilder(context_).createUnlikelyBranchWeights())
            {
            }

            void run()
            {
                // What is there before anything changes: what each cycle counts, the calls that may stop, the
                // returns.
                if (facts_.cycles)
                    plan();
                llvm::SmallVector<llvm::CallInst*, 8> calls;
                llvm::SmallVector<llvm::ReturnInst*, 4> returns;
                for (llvm::BasicBlock& block : function_)
                {
                    for (llvm::Instruction& instruction : block)
                    {
                        if (auto* ret = llvm::dyn_cast<llvm::ReturnInst>(&instruction))
                            returns.push_back(ret);
                        else if (auto* call = llvm::dyn_cast<llvm::CallInst>(&instruction);
                                 call != nullptr && !call->isMustTailCall() && stops(*call))
                            calls.push_back(call);
                    }
                }

                const bool counting = counts(facts_);
                llvm::Instruction* started = nullptr;   // the store of the count where the function starts
                if (counting)
                {
                    llvm::BasicBlock& entry = function_.getEntryBlock();
                    llvm::IRBuilder<> at(&entry, entry.begin());
                    slot_ = at.CreateAlloca(i64_, nullptr, "budget.slot");
                    at.SetInsertPoint(&entry, entry.getFirstNonPHIOrDbgOrAlloca());
                    llvm::LoadInst* start = at.CreateAlignedLoad(i64_, left_, llvm::Align(8), "budget.start");
                    start->setAtomic(llvm::AtomicOrdering::Monotonic);
                    started = at.CreateStore(start, slot_);
                    for (llvm::ReturnInst* ret : returns)
                    {
                        llvm::IRBuilder<> before(ret);
                        publish(before);
                    }
                }

                // A loop with a count counts it before it starts; any other cycle counts one at each branch back.
                for (const auto& [preheader, header, count] : charges_)
                {
                    const auto* constant = llvm::dyn_cast<llvm::ConstantInt>(count);
                    if (constant != nullptr && constant->isZero())
                        continue;   // it never branches back
                    countDown(preheader->getTerminator(), header, count);
                }
                for (const auto& [header, latches] : backs_)
                {
                    for (llvm::BasicBlock* latch : latches)
                    {
                        if (auto* jump = llvm::dyn_cast<llvm::UncondBrInst>(latch->getTerminator()))
                        {
                            countDown(jump, header, llvm::ConstantInt::get(i64_, 1));
                            continue;
                        }
                        llvm::BasicBlock* back = llvm::SplitBlockPredecessors(
                            header, {latch}, ".budget", static_cast<llvm::DomTreeUpdater*>(nullptr));
                        countDown(back->getTerminator(), header, llvm::ConstantInt::get(i64_, 1));
                    }
                }

                // After a call that may stop: return if it did, and read the count again if it did not.
                for (llvm::CallInst* call : calls)
                {
                    if (counting)
                    {
                        llvm::IRBuilder<> before(call);
                        publish(before);
                    }
                    llvm::BasicBlock* block = call->getParent();
                    llvm::BasicBlock* next = llvm::SplitBlock(block, std::next(call->getIterator()),
                                                              static_cast<llvm::DomTreeUpdater*>(nullptr), nullptr,
                                                              nullptr, block->getName() + ".budget");
                    llvm::Instruction* branch = block->getTerminator();
                    llvm::IRBuilder<> at(branch);
                    llvm::LoadInst* flag = at.CreateAlignedLoad(i32_, stopped_, llvm::Align(4), "budget.stopped");
                    flag->setAtomic(llvm::AtomicOrdering::Monotonic);
                    at.CreateCondBr(at.CreateICmpNE(flag, llvm::ConstantInt::get(i32_, 0)), unwindBlock(), next,
                                    unlikely_);
                    branch->eraseFromParent();
                    if (counting)
                    {
                        llvm::IRBuilder<> after(next, next->getFirstInsertionPt());
                        llvm::LoadInst* now = after.CreateAlignedLoad(i64_, left_, llvm::Align(8), "budget.now");
                        now->setAtomic(llvm::AtomicOrdering::Monotonic);
                        after.CreateStore(now, slot_);
                    }
                }

                // A function that can call itself again counts one where it starts.
                if (facts_.recursive)
                {
                    llvm::BasicBlock* entry = started->getParent();
                    llvm::BasicBlock* rest = llvm::SplitBlock(entry, std::next(started->getIterator()),
                                                              static_cast<llvm::DomTreeUpdater*>(nullptr), nullptr,
                                                              nullptr, "budget.entered");
                    countDown(entry->getTerminator(), rest, llvm::ConstantInt::get(i64_, 1));
                }

                if (slot_ != nullptr)
                {
                    llvm::DominatorTree tree(function_);
                    llvm::PromoteMemToReg({slot_}, tree);
                }

                mayReturnEarly(function_);   // a loop that never ended returns now (opt/EarlyReturn.h)
                function_.addFnAttr(kLoopBudgetAttribute, counting ? "counts" : "returns");
            }

        private:
            // Whether `call` runs a function that may stop.
            bool stops(llvm::CallInst& call) const
            {
                llvm::SmallVector<llvm::Function*, 2> functions;
                reachedFunctions(call, functions);
                return llvm::any_of(functions, [&](llvm::Function* function) {
                    const auto found = all_.find(function);
                    return found != all_.end() && found->second.mayStop;
                });
            }

            // Writes the count back to the budget, at `at`: a store, or in a function a parallel region's threads may
            // run, an atomicrmw min, so that `left` never goes up while the program runs (the top of this file).
            void publish(llvm::IRBuilder<>& at)
            {
                llvm::Value* count = at.CreateLoad(i64_, slot_);
                if (facts_.concurrent)
                {
                    at.CreateAtomicRMW(llvm::AtomicRMWInst::Min, left_, count, llvm::MaybeAlign(8),
                                       llvm::AtomicOrdering::Monotonic);
                    return;
                }
                llvm::StoreInst* store = at.CreateAlignedStore(count, left_, llvm::Align(8));
                store->setAtomic(llvm::AtomicOrdering::Monotonic);
            }

            // Each cycle's count, as plan() works it out: a loop whose count ScalarEvolution knows when it starts is
            // charged it there, in its preheader, and any other cycle a branch back at a time. The preheader's charge
            // is computed there now, while the analyses are true of the code; the branches come later.
            //
            // A loop with a count is most of a DSP program's: a for over a block, a vectorised loop and its
            // remainder, a loop over a module's voices or taps. Charged once a start, it runs as it would without the
            // budget, where a check at every branch back costs its instructions every iteration (a loop of eight
            // voices, a sample at a time, took 9% longer) and splits a vectorised loop's one block in two, which the
            // code generator schedules worse (a loop of tanhf, 9% longer). The count is what the loop will take: its
            // exits all have counts (one that leaves on a condition of the data, a `break`, has none), and the
            // optimiser's loops follow it (the vectoriser's trip count, IndVarSimplify's exit test). What it cannot
            // be is a loop that runs for ever only through undefined behaviour, an int that overflows past its end,
            // since C lets the optimiser assume that never happens: such a loop is charged the count it was assumed
            // to have. A count that is not cheap to compute where the loop starts (one that changes with an outer
            // loop's iteration, a triangle's inner loop, or a long expression) counts a branch at a time instead.
            void plan()
            {
                llvm::DominatorTree tree(function_);
                llvm::LoopInfo loops(tree);
                for (llvm::Loop* loop : loops.getLoopsInPreorder())
                {
                    if (loop->getLoopPreheader() == nullptr)
                        llvm::InsertPreheaderForLoop(loop, &tree, &loops, nullptr, false);
                }
                llvm::CycleInfo cycles;
                cycles.compute(function_);
                llvm::AssumptionCache assumptions(function_);
                llvm::TargetLibraryInfoImpl libraryInfo(function_.getParent()->getTargetTriple());
                llvm::TargetLibraryInfo library(libraryInfo, &function_);
                llvm::ScalarEvolution evolution(function_, library, assumptions, tree, loops);
                llvm::SCEVExpander expander(evolution, "budget.count", false);

                llvm::SmallVector<const llvm::CycleInfo::CycleT*, 8> work(cycles.toplevel_cycles());
                while (!work.empty())
                {
                    const llvm::CycleInfo::CycleT* cycle = work.pop_back_val();
                    work.append(cycle->children().begin(), cycle->children().end());
                    llvm::BasicBlock* header = cycle->getHeader();
                    llvm::Loop* loop = cycle->isReducible() ? loops.getLoopFor(header) : nullptr;
                    if (loop != nullptr && loop->getHeader() == header)
                    {
                        if (llvm::Value* count = countOf(*loop, evolution, expander))
                        {
                            charges_.push_back({loop->getLoopPreheader(), header, count});
                            continue;
                        }
                    }
                    llvm::SmallVector<llvm::BasicBlock*, 4> latches;
                    for (llvm::BasicBlock* pred : llvm::predecessors(header))
                    {
                        if (cycle->contains(pred) && !llvm::is_contained(latches, pred))
                            latches.push_back(pred);
                    }
                    backs_.emplace_back(header, std::move(latches));
                }
            }

            // How many times `loop` branches back, computed in its preheader, or null when it has no count cheap to
            // compute there (plan()).
            llvm::Value* countOf(llvm::Loop& loop, llvm::ScalarEvolution& evolution, llvm::SCEVExpander& expander)
            {
                llvm::BasicBlock* preheader = loop.getLoopPreheader();
                if (preheader == nullptr || !llvm::isa<llvm::UncondBrInst>(preheader->getTerminator())
                    || !evolution.hasLoopInvariantBackedgeTakenCount(&loop))
                    return nullptr;
                const llvm::SCEV* taken = evolution.getBackedgeTakenCount(&loop);
                if (evolution.getTypeSizeInBits(taken->getType()) > 64 || taken->getExpressionSize() > kLongestCount
                    || llvm::SCEVExprContains(taken, [](const llvm::SCEV* part) {
                           return llvm::isa<llvm::SCEVAddRecExpr>(part);
                       }))
                    return nullptr;
                // A count that may not fit an int64_t would be a negative charge, which adds to the budget: a loop
                // over bytes, `p != e` with e below p, takes 2^64 - 1 less what lies between. FMinus's pointers are
                // to 4 bytes or more, whose loops' counts fit; IR written by hand may have such a loop.
                if (evolution.getUnsignedRangeMax(taken).ugt(std::numeric_limits<std::int64_t>::max()))
                    return nullptr;
                const llvm::SCEV* count = evolution.getNoopOrZeroExtend(taken, i64_);
                llvm::Instruction* at = preheader->getTerminator();
                if (!expander.isSafeToExpandAt(count, at))
                    return nullptr;
                return expander.expandCodeFor(count, i64_, at->getIterator());
            }

            // Replaces `branch`, an unconditional branch to `next`, by a count of `count` (an i64), and a branch to
            // the stop when the count goes below zero.
            void countDown(llvm::Instruction* branch, llvm::BasicBlock* next, llvm::Value* count)
            {
                llvm::IRBuilder<> at(branch);
                llvm::Value* left = at.CreateSub(at.CreateLoad(i64_, slot_), count, "budget.left");
                at.CreateStore(left, slot_);
                llvm::Value* out = at.CreateICmpSLT(left, llvm::ConstantInt::get(i64_, 0), "budget.out");
                at.CreateCondBr(out, stopBlock(), next, unlikely_);
                branch->eraseFromParent();
            }

            // Where the count goes below zero: -1 for what is left, the flag, and return. -1, not the count, which a
            // charge may have taken far below zero: `left` stays where no later subtraction can wrap it (the top of
            // this file). It is the least `left` can hold, so a store is a min here too.
            llvm::BasicBlock* stopBlock()
            {
                if (stop_ != nullptr)
                    return stop_;
                stop_ = llvm::BasicBlock::Create(context_, "budget.stop", &function_);
                llvm::IRBuilder<> at(stop_);
                llvm::StoreInst* out =
                    at.CreateAlignedStore(llvm::ConstantInt::getSigned(i64_, -1), left_, llvm::Align(8));
                out->setAtomic(llvm::AtomicOrdering::Monotonic);
                llvm::StoreInst* raise =
                    at.CreateAlignedStore(llvm::ConstantInt::get(i32_, 1), stopped_, llvm::Align(4));
                raise->setAtomic(llvm::AtomicOrdering::Monotonic);
                returnEarly(at, function_);
                return stop_;
            }

            // Where a call that stopped returns to: return, and write nothing (the function that stopped did).
            llvm::BasicBlock* unwindBlock()
            {
                if (unwind_ != nullptr)
                    return unwind_;
                unwind_ = llvm::BasicBlock::Create(context_, "budget.unwind", &function_);
                llvm::IRBuilder<> at(unwind_);
                returnEarly(at, function_);
                return unwind_;
            }

            llvm::Function& function_;
            const Facts& facts_;
            const llvm::DenseMap<llvm::Function*, Facts>& all_;
            llvm::LLVMContext& context_;
            llvm::Type* i64_;
            llvm::Type* i32_;
            llvm::Constant* left_;      // the budget's `left`, at its address
            llvm::Constant* stopped_;   // and its `stopped`
            llvm::MDNode* unlikely_;
            llvm::AllocaInst* slot_ = nullptr;
            llvm::BasicBlock* stop_ = nullptr;
            llvm::BasicBlock* unwind_ = nullptr;
            // What plan() found: the loops charged their count before they start (the preheader, the header and the
            // count), and the cycles counted a branch back at a time (the header, and the blocks that branch back).
            struct Charge
            {
                llvm::BasicBlock* preheader;
                llvm::BasicBlock* header;
                llvm::Value* count;
            };
            std::vector<Charge> charges_;
            std::vector<std::pair<llvm::BasicBlock*, llvm::SmallVector<llvm::BasicBlock*, 4>>> backs_;
        };
    } // namespace

    llvm::GlobalVariable* loopBudgetGlobal(llvm::Module& module)
    {
        if (llvm::GlobalVariable* existing = module.getNamedGlobal(kLoopBudgetSymbol))
            return existing;
        llvm::LLVMContext& context = module.getContext();
        // Unlimited until the host sets it (Program::setLoopBudget), so that a program runs as it would without the
        // budget until then.
        llvm::Constant* unlimited = llvm::ConstantStruct::get(
            budgetType(context),
            {llvm::ConstantInt::get(llvm::Type::getInt64Ty(context), std::numeric_limits<std::int64_t>::max()),
             llvm::ConstantInt::get(llvm::Type::getInt32Ty(context), 0)});
        auto* budget = new llvm::GlobalVariable(module, budgetType(context), false, llvm::GlobalValue::ExternalLinkage,
                                                unlimited, kLoopBudgetSymbol);
        budget->setAlignment(llvm::Align(8));
        budget->setDSOLocal(true);   // the program's own: reached directly, not through a GOT
        return budget;
    }

    llvm::PreservedAnalyses LoopBudgetPass::run(llvm::Module& module, llvm::ModuleAnalysisManager&)
    {
        const auto* flag = llvm::mdconst::extract_or_null<llvm::ConstantInt>(module.getModuleFlag(kLoopBudgetFlag));
        if (flag == nullptr || flag->isZero())
            return llvm::PreservedAnalyses::all();
        bool changed = module.getNamedGlobal(kLoopBudgetSymbol) == nullptr;   // IR the code generator did not write
        llvm::GlobalVariable* budget = loopBudgetGlobal(module);

        // Each function's facts, in the module's order.
        llvm::DenseMap<llvm::Function*, Facts> facts;
        std::vector<llvm::Function*> order;
        for (llvm::Function& function : module)
        {
            if (function.isDeclaration() || function.hasFnAttribute(kLoopBudgetHostAttribute))
                continue;
            Facts& these = facts[&function];
            order.push_back(&function);
            if (function.hasFnAttribute(kLoopBudgetAttribute))
            {
                these.done = true;   // it may stop, and counts already
                these.mayStop = true;
                continue;
            }
            llvm::CycleInfo cycles;
            cycles.compute(function);
            these.cycles = !cycles.toplevel_cycles().empty();
            for (llvm::BasicBlock& block : function)
            {
                for (llvm::Instruction& instruction : block)
                {
                    if (auto* call = llvm::dyn_cast<llvm::CallBase>(&instruction))
                        reachedFunctions(*call, these.reaches, &these.hands);
                }
            }
        }
        for (llvm::Function* function : order)
        {
            Facts& these = facts[function];
            if (these.done)
                continue;
            these.recursive = recursive(function, facts);
            these.mayStop = counts(these);
        }
        // The functions a parallel region's threads may run: those handed to a runtime function, and every function
        // they reach.
        llvm::SmallVector<llvm::Function*, 8> work;
        for (llvm::Function* function : order)
        {
            for (llvm::Function* handed : facts[function].hands)
                work.push_back(handed);
        }
        while (!work.empty())
        {
            const auto found = facts.find(work.pop_back_val());
            if (found == facts.end() || found->second.concurrent)
                continue;
            found->second.concurrent = true;
            work.append(found->second.reaches.begin(), found->second.reaches.end());
        }
        for (bool grew = true; grew;)
        {
            grew = false;
            for (llvm::Function* function : order)
            {
                Facts& these = facts[function];
                if (these.mayStop)
                    continue;
                these.mayStop = llvm::any_of(these.reaches, [&](llvm::Function* callee) {
                    const auto found = facts.find(callee);
                    return found != facts.end() && found->second.mayStop;
                });
                grew = grew || these.mayStop;
            }
        }

        // Every call of a function that may stop, wherever it is (the host's own functions' too), promises no more of
        // its result or its memory than the function now does.
        for (llvm::Function& function : module)
        {
            for (llvm::BasicBlock& block : function)
            {
                for (llvm::Instruction& instruction : block)
                {
                    auto* call = llvm::dyn_cast<llvm::CallBase>(&instruction);
                    llvm::Function* callee = call != nullptr ? call->getCalledFunction() : nullptr;
                    const auto found = callee != nullptr ? facts.find(callee) : facts.end();
                    if (found == facts.end() || !found->second.mayStop)
                        continue;
                    mayReturnEarly(*call);
                    changed = true;
                }
            }
        }
        for (llvm::Function* function : order)
        {
            const Facts& these = facts[function];
            if (!these.mayStop || these.done)
                continue;
            Instrument(*function, these, facts, *budget).run();
            changed = true;
        }
        return changed ? llvm::PreservedAnalyses::none() : llvm::PreservedAnalyses::all();
    }
} // namespace fminus::opt
