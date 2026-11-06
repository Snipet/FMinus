// src/opt/BoundsCheck.cpp: bounds checks (BoundsCheck.h).
//
// A check, as the code generator writes it (ir/Bounds.cpp), is a trap until this pass runs:
//
//     %bounds.in = icmp ult i64 %idxprom, 16
//     br i1 %bounds.in, label %bounds.ok, label %bounds.fail, !prof !likely
//   bounds.fail:
//     call void @fminus.bounds.stop(i32 1, i64 %idxprom)          ; noreturn
//     unreachable
//
// The pass makes each stop a branch to its function's one stop block, which records the check, unless the record holds
// one already, and returns:
//
//   bounds.fail:
//     br label %bounds.stop
//   bounds.stop:
//     %bounds.site = phi i32 [ 1, %bounds.fail ], ...
//     %bounds.index = phi i64 [ %idxprom, %bounds.fail ], ...
//     %bounds.first = cmpxchg ptr @fminus_rt_bounds, i32 0, i32 %bounds.site monotonic monotonic, align 8
//     %bounds.won = extractvalue { i32, i1 } %bounds.first, 1
//     br i1 %bounds.won, label %bounds.record, label %bounds.return
//   bounds.record:
//     store atomic i64 %bounds.index, ptr getelementptr inbounds (...) monotonic, align 8
//     br label %bounds.return
//   bounds.return:
//     ret float 0.000000e+00
//
// A function may stop when it has a check, or calls a function that may stop (directly, or by handing it to a runtime
// function: a parallel region's body, which fminus_rt_parallel_for runs). After each such call it reads the record,
// and returns at once if it holds a check:
//
//     %5 = call float @tap(ptr %this, i32 %4)
//     %bounds.recorded = load atomic i32, ptr @fminus_rt_bounds monotonic, align 8
//     %6 = icmp ne i32 %bounds.recorded, 0
//     br i1 %6, label %bounds.unwind, label %if.end.bounds, !prof !unlikely
//   bounds.unwind:                                    ; the callee stopped: return, and write nothing
//     ret float 0.000000e+00
//
// So a stopped call returns to its host through every function between, each returning 0, null or nothing, and none
// going on with a value that was never computed. A callee that always failed a check was noreturn to the optimiser,
// which left `unreachable` after its calls: the record is always set when such a callee returns, so the read after the
// call always unwinds. A function marked kBoundsCheckAttribute was changed by an earlier run of the pass: it may stop,
// and is left as it is. An indirect call is not followed (opt/EarlyReturn.h, reachedFunctions).
//
// Until this pass, a check costs what clang's -fsanitize-trap costs: a compare and a branch, and a loop with an exit
// that the vectoriser may not vectorise. The optimiser removes the checks it can prove (an index the loop's own
// condition keeps below the extent), and keeps the others where they are. The pass adds a load and a branch after each
// call of a function that may stop, which inlining makes few: what is left is the calls the inliner kept.
//
// The functions it changes get their attributes adjusted to what is now true of them (opt/EarlyReturn.h,
// mayReturnEarly), and so do the calls of them.
#include "opt/BoundsCheck.h"

#include "opt/EarlyReturn.h"

#include <llvm/ADT/DenseMap.h>
#include <llvm/ADT/STLExtras.h>
#include <llvm/ADT/SmallVector.h>
#include <llvm/IR/Constants.h>
#include <llvm/IR/DerivedTypes.h>
#include <llvm/IR/GlobalVariable.h>
#include <llvm/IR/IRBuilder.h>
#include <llvm/IR/Instructions.h>
#include <llvm/IR/MDBuilder.h>
#include <llvm/IR/Metadata.h>
#include <llvm/IR/Module.h>
#include <llvm/Support/ModRef.h>
#include <llvm/Transforms/Utils/BasicBlockUtils.h>
#include <llvm/Transforms/Utils/Local.h>

#include <iterator>
#include <vector>

namespace fminus::opt
{
    namespace
    {
        // The record's type, runtime/Runtime.h's fminus_rt_bounds_state: { int32_t site; int32_t reserved; int64_t
        // index; }.
        llvm::StructType* recordType(llvm::LLVMContext& context)
        {
            return llvm::StructType::get(llvm::Type::getInt32Ty(context), llvm::Type::getInt32Ty(context),
                                         llvm::Type::getInt64Ty(context));
        }

        struct Facts
        {
            bool done = false;      // changed already, by an earlier run of the pass (kBoundsCheckAttribute)
            bool mayStop = false;   // has a check, or calls a function that may stop
            llvm::SmallVector<llvm::Function*, 8> reaches;   // the functions its calls run, with repeats
        };

        // One function's changes (the top of this file).
        class Instrument
        {
        public:
            Instrument(llvm::Function& function, const llvm::DenseMap<llvm::Function*, Facts>& all,
                       llvm::GlobalVariable& record, llvm::Function* stop)
                : function_(function)
                , all_(all)
                , context_(function.getContext())
                , i32_(llvm::Type::getInt32Ty(context_))
                , i64_(llvm::Type::getInt64Ty(context_))
                , site_(&record)
                , index_(llvm::ConstantExpr::getInBoundsGetElementPtr(
                      recordType(context_), &record,
                      llvm::ArrayRef<llvm::Constant*>{llvm::ConstantInt::get(i32_, 0),
                                                      llvm::ConstantInt::get(i32_, 2)}))
                , stop_(stop)
                , unlikely_(llvm::MDBuilder(context_).createUnlikelyBranchWeights())
            {
            }

            void run()
            {
                // What is there before anything changes: the stops, and the calls that may stop.
                llvm::SmallVector<llvm::CallInst*, 8> stops;
                llvm::SmallVector<llvm::CallInst*, 8> calls;
                for (llvm::BasicBlock& block : function_)
                {
                    for (llvm::Instruction& instruction : block)
                    {
                        auto* call = llvm::dyn_cast<llvm::CallInst>(&instruction);
                        if (call == nullptr)
                            continue;
                        if (stop_ != nullptr && call->getCalledFunction() == stop_)
                            stops.push_back(call);
                        else if (!call->isMustTailCall() && mayStop(*call))
                            calls.push_back(call);
                    }
                }

                // Each stop a branch to the stop block, with its check's number and index. What follows the call
                // never ran (it did not return): nothing, or what the optimiser left, `unreachable`.
                for (llvm::CallInst* stop : stops)
                {
                    llvm::BasicBlock* block = stop->getParent();
                    llvm::changeToUnreachable(stop->getNextNode());
                    block->getTerminator()->eraseFromParent();
                    llvm::UncondBrInst::Create(stopBlock(), block);
                    sitePhi_->addIncoming(stop->getArgOperand(0), block);
                    indexPhi_->addIncoming(stop->getArgOperand(1), block);
                    stop->eraseFromParent();
                }

                // After a call that may stop: return if it did.
                for (llvm::CallInst* call : calls)
                {
                    llvm::BasicBlock* block = call->getParent();
                    llvm::BasicBlock* next = llvm::SplitBlock(block, std::next(call->getIterator()),
                                                              static_cast<llvm::DomTreeUpdater*>(nullptr), nullptr,
                                                              nullptr, block->getName() + ".bounds");
                    llvm::Instruction* branch = block->getTerminator();
                    llvm::IRBuilder<> at(branch);
                    llvm::LoadInst* recorded = at.CreateAlignedLoad(i32_, site_, llvm::Align(8), "bounds.recorded");
                    recorded->setAtomic(llvm::AtomicOrdering::Monotonic);
                    at.CreateCondBr(at.CreateICmpNE(recorded, llvm::ConstantInt::get(i32_, 0)), unwindBlock(), next,
                                    unlikely_);
                    branch->eraseFromParent();
                }

                mayReturnEarly(function_);   // one that always failed a check returns now (opt/EarlyReturn.h)
                function_.addFnAttr(kBoundsCheckAttribute);
            }

        private:
            // Whether `call` runs a function that may stop.
            bool mayStop(llvm::CallInst& call) const
            {
                llvm::SmallVector<llvm::Function*, 2> functions;
                reachedFunctions(call, functions);
                return llvm::any_of(functions, [&](llvm::Function* function) {
                    const auto found = all_.find(function);
                    return found != all_.end() && found->second.mayStop;
                });
            }

            // Where a check fails: its number and index recorded, unless the record holds a check already (the first
            // is kept, which a compare-and-swap of the site from 0 decides among threads), and return.
            llvm::BasicBlock* stopBlock()
            {
                if (stopBlock_ != nullptr)
                    return stopBlock_;
                stopBlock_ = llvm::BasicBlock::Create(context_, "bounds.stop", &function_);
                llvm::BasicBlock* record = llvm::BasicBlock::Create(context_, "bounds.record", &function_);
                llvm::BasicBlock* done = llvm::BasicBlock::Create(context_, "bounds.return", &function_);
                llvm::IRBuilder<> at(stopBlock_);
                sitePhi_ = at.CreatePHI(i32_, 2, "bounds.site");
                indexPhi_ = at.CreatePHI(i64_, 2, "bounds.index");
                llvm::AtomicCmpXchgInst* first =
                    at.CreateAtomicCmpXchg(site_, llvm::ConstantInt::get(i32_, 0), sitePhi_, llvm::MaybeAlign(8),
                                           llvm::AtomicOrdering::Monotonic, llvm::AtomicOrdering::Monotonic);
                first->setName("bounds.first");
                at.CreateCondBr(at.CreateExtractValue(first, 1, "bounds.won"), record, done);
                at.SetInsertPoint(record);
                llvm::StoreInst* index = at.CreateAlignedStore(indexPhi_, index_, llvm::Align(8));
                index->setAtomic(llvm::AtomicOrdering::Monotonic);
                at.CreateBr(done);
                at.SetInsertPoint(done);
                returnEarly(at, function_);
                return stopBlock_;
            }

            // Where a call that stopped returns to: return, and write nothing (the function that stopped did).
            llvm::BasicBlock* unwindBlock()
            {
                if (unwind_ != nullptr)
                    return unwind_;
                unwind_ = llvm::BasicBlock::Create(context_, "bounds.unwind", &function_);
                llvm::IRBuilder<> at(unwind_);
                returnEarly(at, function_);
                return unwind_;
            }

            llvm::Function& function_;
            const llvm::DenseMap<llvm::Function*, Facts>& all_;
            llvm::LLVMContext& context_;
            llvm::Type* i32_;
            llvm::Type* i64_;
            llvm::Constant* site_;    // the record's site, at its address
            llvm::Constant* index_;   // and its index
            llvm::Function* stop_;    // kBoundsStopFunction, or null when the module calls it nowhere
            llvm::MDNode* unlikely_;
            llvm::BasicBlock* stopBlock_ = nullptr;
            llvm::PHINode* sitePhi_ = nullptr;
            llvm::PHINode* indexPhi_ = nullptr;
            llvm::BasicBlock* unwind_ = nullptr;
        };

        std::string text(const llvm::MDNode& node, unsigned operand)
        {
            const auto* string = operand < node.getNumOperands()
                                     ? llvm::dyn_cast_or_null<llvm::MDString>(node.getOperand(operand).get())
                                     : nullptr;
            return string != nullptr ? string->getString().str() : std::string();
        }

        std::int64_t number(const llvm::MDNode& node, unsigned operand)
        {
            const auto* value = operand < node.getNumOperands()
                                    ? llvm::mdconst::dyn_extract_or_null<llvm::ConstantInt>(node.getOperand(operand))
                                    : nullptr;
            return value != nullptr ? value->getSExtValue() : 0;
        }
    } // namespace

    llvm::GlobalVariable* boundsGlobal(llvm::Module& module)
    {
        if (llvm::GlobalVariable* existing = module.getNamedGlobal(kBoundsSymbol))
            return existing;
        llvm::LLVMContext& context = module.getContext();
        llvm::StructType* type = recordType(context);
        auto* record = new llvm::GlobalVariable(module, type, false, llvm::GlobalValue::ExternalLinkage,
                                                llvm::ConstantAggregateZero::get(type), kBoundsSymbol);
        record->setAlignment(llvm::Align(8));
        record->setDSOLocal(true);   // the program's own: reached directly, not through a GOT
        return record;
    }

    llvm::Function* boundsStopFunction(llvm::Module& module)
    {
        if (llvm::Function* existing = module.getFunction(kBoundsStopFunction))
            return existing;
        llvm::LLVMContext& context = module.getContext();
        auto* type = llvm::FunctionType::get(llvm::Type::getVoidTy(context),
                                             {llvm::Type::getInt32Ty(context), llvm::Type::getInt64Ty(context)}, false);
        llvm::Function* stop =
            llvm::Function::Create(type, llvm::GlobalValue::ExternalLinkage, kBoundsStopFunction, module);
        stop->setDoesNotReturn();
        stop->setDoesNotThrow();
        stop->addFnAttr(llvm::Attribute::Cold);
        // It writes the record, which nothing else the program does reads or writes until this pass: to the
        // optimiser, memory the program cannot reach, so that a loop's loads of a module's fields still move out of it
        // past a check in it.
        stop->setMemoryEffects(llvm::MemoryEffects::inaccessibleMemOnly(llvm::ModRefInfo::Mod));
        return stop;
    }

    std::int32_t addBoundsSite(llvm::Module& module, const BoundsSite& site)
    {
        llvm::LLVMContext& context = module.getContext();
        const auto integer = [&](llvm::Type* type, std::int64_t value) -> llvm::Metadata* {
            return llvm::ConstantAsMetadata::get(llvm::ConstantInt::getSigned(type, value));
        };
        llvm::Type* i32 = llvm::Type::getInt32Ty(context);
        llvm::Type* i64 = llvm::Type::getInt64Ty(context);
        llvm::NamedMDNode* sites = module.getOrInsertNamedMetadata(kBoundsSitesMetadata);
        sites->addOperand(llvm::MDTuple::get(
            context, {llvm::MDString::get(context, site.file), integer(i32, site.line), integer(i32, site.column),
                      llvm::MDString::get(context, site.array), llvm::MDString::get(context, site.type),
                      integer(i64, site.extent), integer(i32, site.width)}));
        return static_cast<std::int32_t>(sites->getNumOperands());
    }

    std::vector<BoundsSite> boundsSites(const llvm::Module& module)
    {
        std::vector<BoundsSite> sites;
        const llvm::NamedMDNode* named = module.getNamedMetadata(kBoundsSitesMetadata);
        if (named == nullptr)
            return sites;
        for (const llvm::MDNode* node : named->operands())
        {
            sites.push_back({text(*node, 0), static_cast<int>(number(*node, 1)), static_cast<int>(number(*node, 2)),
                             text(*node, 3), text(*node, 4), number(*node, 5), static_cast<int>(number(*node, 6))});
        }
        return sites;
    }

    std::string boundsMessageBefore(const BoundsSite& site)
    {
        std::string where = site.file + ":" + std::to_string(site.line) + ":" + std::to_string(site.column) + ": ";
        if (site.width == 0)
            return where + "pointer offset ";
        if (site.width == 1)
            return where + "index ";
        return where + std::to_string(site.width) + " elements from index ";
    }

    std::string boundsMessageAfter(const BoundsSite& site)
    {
        const std::string type = "type '" + site.type + "'";
        return " out of bounds for " + (site.array.empty() ? type : "'" + site.array + "' of " + type);
    }

    std::string boundsMessage(const BoundsSite& site, std::int64_t index)
    {
        return boundsMessageBefore(site) + std::to_string(index) + boundsMessageAfter(site);
    }

    llvm::PreservedAnalyses BoundsCheckPass::run(llvm::Module& module, llvm::ModuleAnalysisManager&)
    {
        const auto* flag = llvm::mdconst::extract_or_null<llvm::ConstantInt>(module.getModuleFlag(kBoundsCheckFlag));
        if (flag == nullptr || flag->isZero())
            return llvm::PreservedAnalyses::all();
        bool changed = module.getNamedGlobal(kBoundsSymbol) == nullptr;   // IR the code generator did not write
        llvm::GlobalVariable* record = boundsGlobal(module);
        llvm::Function* stop = module.getFunction(kBoundsStopFunction);

        // Each function's facts, in the module's order.
        llvm::DenseMap<llvm::Function*, Facts> facts;
        std::vector<llvm::Function*> order;
        for (llvm::Function& function : module)
        {
            if (function.isDeclaration())
                continue;
            Facts& these = facts[&function];
            order.push_back(&function);
            if (function.hasFnAttribute(kBoundsCheckAttribute))
            {
                these.done = true;   // it may stop, and returns early already
                these.mayStop = true;
                continue;
            }
            for (llvm::BasicBlock& block : function)
            {
                for (llvm::Instruction& instruction : block)
                {
                    auto* call = llvm::dyn_cast<llvm::CallBase>(&instruction);
                    if (call == nullptr)
                        continue;
                    if (stop != nullptr && call->getCalledFunction() == stop)
                        these.mayStop = true;
                    else
                        reachedFunctions(*call, these.reaches);
                }
            }
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

        // Every call of a function that may stop, wherever it is, promises no more of its result or its memory than
        // the function now does.
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
            Instrument(*function, facts, *record, stop).run();
            changed = true;
        }
        if (stop != nullptr && stop->use_empty())
            stop->eraseFromParent();
        return changed ? llvm::PreservedAnalyses::none() : llvm::PreservedAnalyses::all();
    }
} // namespace fminus::opt
