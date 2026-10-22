// src/opt/KnobVersioning.cpp: knob versioning (KnobVersioning.h). For each function not versioned already:
//   1. Its knobs: the instructions of its entry block tagged !fminus.domain whose value reaches, through casts,
//      comparisons and logic on bools, the condition of a branch, a switch or a select inside one of its loops (or, for
//      a stream's test, of a select whose value a loop uses: the stream's base and stride). A knob that steers no loop
//      is left alone: a copy would save one branch a call.
//   2. The knobs it versions on: the streams' tests for null first, then the knobs in the order they are declared in
//      (their slots' order in the module), each whose domain still fits the budget, multiplied by the copies already
//      chosen. So three ints over 0..3 under a budget of 8 give 4 copies, on the first, and not 64.
//   3. The copies. The entry block is split after the last chosen knob's read, and the rest of the function, its body,
//      is cloned once for each combination of the knobs' values, with the knobs replaced by those values (LLVM's
//      CloneBasicBlock and remapInstructionsInBlocks, block by block). The entry block then picks the copy: a switch on
//      each int, a branch on each bool, and an int outside its domain falls back to the original body, unchanged.
//      When every chosen knob is a bool, no value falls back, and the original body becomes the first combination's
//      copy instead (keeping its names), so there is no dead fallback to delete.
// Everything stays inside the one function, so it keeps its name, its attributes, its callers and its effects. What the
// entry block computes before the split (the knobs' reads, a stream's pointer, the streams' alias scope declaration) is
// shared by every copy, and dominates them all; what it computes from a knob moves into the body first, so each copy
// computes it with its value. Each copy has its own locals (the body's stack slots), with their lifetimes marked. The
// copies' memory accesses keep their !alias.scope and !noalias; a scope declared inside the body is cloned for each
// copy (cloneAndAdaptNoAliasScopes), and a loop's !llvm.loop is made anew for each, since no two loops may share one.
#include "opt/KnobVersioning.h"

#include <llvm/ADT/DepthFirstIterator.h>
#include <llvm/ADT/STLExtras.h>
#include <llvm/ADT/SmallPtrSet.h>
#include <llvm/ADT/SmallVector.h>
#include <llvm/ADT/StringExtras.h>
#include <llvm/Analysis/LoopInfo.h>
#include <llvm/IR/Constants.h>
#include <llvm/IR/DataLayout.h>
#include <llvm/IR/Function.h>
#include <llvm/IR/InlineAsm.h>
#include <llvm/IR/Instructions.h>
#include <llvm/IR/IRBuilder.h>
#include <llvm/IR/Metadata.h>
#include <llvm/IR/Module.h>
#include <llvm/Transforms/Utils/Cloning.h>
#include <llvm/Transforms/Utils/ValueMapper.h>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <iterator>
#include <limits>
#include <string>
#include <utility>
#include <vector>

namespace fminus::opt
{
    namespace
    {
        // A knob the function may be versioned on: the instruction of the entry block that reads it, the values its
        // domain lists (each once), and where its slot is in the module, the order of its declaration.
        struct Knob
        {
            llvm::Instruction* value = nullptr;
            std::vector<llvm::ConstantInt*> domain;
            std::int64_t order = 0;
        };

        // The module's budget, from its flag; the default when it has none.
        std::uint64_t budgetOf(const llvm::Module& module)
        {
            if (const auto* flag = llvm::mdconst::dyn_extract_or_null<llvm::ConstantInt>(
                    module.getModuleFlag(kVersionsFlag)))
                return flag->getZExtValue();
            return kDefaultVersions;
        }

        // The values a domain lists: constants of the tagged value's own type, each kept once. Nothing when the tag is
        // not such a list.
        std::vector<llvm::ConstantInt*> valuesOf(const llvm::Instruction& value, const llvm::MDNode& domain)
        {
            std::vector<llvm::ConstantInt*> values;
            if (!value.getType()->isIntegerTy())
                return values;
            for (const llvm::MDOperand& operand : domain.operands())
            {
                auto* constant = llvm::mdconst::dyn_extract_or_null<llvm::ConstantInt>(operand.get());
                if (constant == nullptr || constant->getType() != value.getType())
                    return {};
                if (std::ranges::find(values, constant) == values.end())   // constants are unique: same value, same one
                    values.push_back(constant);
            }
            return values;
        }

        // Whether the domain is every value of its type, a bool's false and true: then no value falls back.
        bool covers(const Knob& knob)
        {
            return knob.value->getType()->isIntegerTy(1) && knob.domain.size() == 2;
        }

        // Whether the knob is a stream's test for null (a pointer compared): chosen before the knobs. Unversioned, a
        // stream's read is a load at a stride the loop vectoriser checks is 1 before its vector loop, and a select of
        // the value, and a flat stream runs the scalar loop; in the copies the read is the load alone, or the value,
        // and a flat stream's work leaves the loop. A knob left out costs only what it cost before there were copies.
        bool testsStream(const Knob& knob)
        {
            const auto* compare = llvm::dyn_cast<llvm::ICmpInst>(knob.value);
            return compare != nullptr && compare->getOperand(0)->getType()->isPointerTy();
        }

        // Where the knob's slot is, in bytes from the function's first argument (a method's `this`): the order its
        // parameter is declared in. The read is the instruction itself (an int's load) or its first operand (a bool's
        // byte compared with 0, a stream's pointer compared with null). A knob read any other way sorts last.
        std::int64_t orderOf(const llvm::Instruction& value, const llvm::DataLayout& layout)
        {
            constexpr std::int64_t kLast = std::numeric_limits<std::int64_t>::max();
            const llvm::Value* read = &value;
            if (const auto* compare = llvm::dyn_cast<llvm::CmpInst>(read))
                read = compare->getOperand(0);
            const auto* load = llvm::dyn_cast<llvm::LoadInst>(read);
            if (load == nullptr)
                return kLast;
            llvm::APInt offset(layout.getIndexTypeSizeInBits(load->getPointerOperandType()), 0);
            const llvm::Value* base =
                load->getPointerOperand()->stripAndAccumulateConstantOffsets(layout, offset, /*AllowNonInbounds=*/true);
            return llvm::isa<llvm::Argument>(base) ? offset.getSExtValue() : kLast;
        }

        // The value of FMinus's opaque unit (src/ir/Modules.cpp, opaqueUnit): an empty asm statement that returns
        // the constant it is given, in the register it returns. Null for anything else.
        llvm::ConstantInt* unitOf(const llvm::Value* value)
        {
            const auto* call = llvm::dyn_cast<llvm::CallInst>(value);
            const auto* statement =
                call != nullptr ? llvm::dyn_cast<llvm::InlineAsm>(call->getCalledOperand()) : nullptr;
            if (statement == nullptr || !statement->getAsmString().empty()
                || statement->getConstraintString() != kUnitConstraints || call->arg_size() != 1)
                return nullptr;
            return llvm::dyn_cast<llvm::ConstantInt>(call->getArgOperand(0));
        }

        // Whether `select` picks where a stream's samples are: its base (a pointer) or its stride (0 or the opaque
        // unit). A stream's test selects these, and its reads (the value or the sample): only the first two make a
        // loop's loads differ.
        bool addressesStream(const llvm::SelectInst& select)
        {
            return select.getType()->isPointerTy() || unitOf(select.getTrueValue()) != nullptr
                   || unitOf(select.getFalseValue()) != nullptr;
        }

        // Whether one of the instructions that use `value` is in a loop.
        bool usedInLoop(const llvm::Value& value, const llvm::LoopInfo& loops)
        {
            return llvm::any_of(value.users(), [&](const llvm::User* user) {
                const auto* instruction = llvm::dyn_cast<llvm::Instruction>(user);
                return instruction != nullptr && loops.getLoopFor(instruction->getParent()) != nullptr;
            });
        }

        // Whether the knob decides something inside one of the function's loops: whether its value reaches, through
        // casts, comparisons and logic on bools, the condition of a conditional branch, a switch or a select in a
        // block of a loop. (Into a select's other operands, or a phi, it is data, not a decision.) A stream's test
        // (`selects`) decides in a loop too through its base or its stride (addressesStream), selected where the
        // method starts, when a loop uses them: the loop reads the stream. A read before the loop selects on the test
        // too, outside every loop, and steers nothing.
        bool steersLoop(const llvm::Instruction& knob, const llvm::LoopInfo& loops, bool selects)
        {
            llvm::SmallVector<const llvm::Value*, 8> work{&knob};
            llvm::SmallPtrSet<const llvm::Value*, 16> seen{&knob};
            while (!work.empty())
            {
                const llvm::Value* value = work.pop_back_val();
                for (const llvm::User* user : value->users())
                {
                    const auto* instruction = llvm::dyn_cast<llvm::Instruction>(user);
                    if (instruction == nullptr)
                        continue;
                    const bool inLoop = loops.getLoopFor(instruction->getParent()) != nullptr;
                    if (const auto* branch = llvm::dyn_cast<llvm::CondBrInst>(instruction))
                    {
                        if (branch->getCondition() == value && inLoop)
                            return true;
                        continue;
                    }
                    if (const auto* choice = llvm::dyn_cast<llvm::SwitchInst>(instruction))
                    {
                        if (choice->getCondition() == value && inLoop)
                            return true;
                        continue;
                    }
                    if (const auto* select = llvm::dyn_cast<llvm::SelectInst>(instruction))
                    {
                        const bool addresses = selects && addressesStream(*select) && usedInLoop(*select, loops);
                        if (select->getCondition() == value && (inLoop || addresses))
                            return true;
                        continue;
                    }
                    const bool logic =
                        llvm::isa<llvm::BinaryOperator>(instruction) && instruction->getType()->isIntOrIntVectorTy(1);
                    if ((llvm::isa<llvm::CastInst, llvm::CmpInst, llvm::FreezeInst>(instruction) || logic)
                        && seen.insert(instruction).second)
                        work.push_back(instruction);
                }
            }
            return false;
        }

        // What a copy's blocks and values are suffixed with for one knob's value: ".mode2", ".on1", ".cutoff.flat1"
        // (a bool knob's value is named after its parameter, with ".on").
        std::string label(const Knob& knob, const llvm::ConstantInt& value)
        {
            std::string name = knob.value->getName().str();
            if (name.ends_with(".on"))
                name.resize(name.size() - 3);
            if (name.empty())
                name = "knob";
            const llvm::APInt& bits = value.getValue();
            return "." + name + llvm::toString(bits, 10, /*Signed=*/bits.getBitWidth() > 1);
        }

        // Gives each loop of `blocks` a loop ID of its own, where it has one (!llvm.loop: distinct, its first operand
        // itself), with the same properties: a copy's loops are new loops.
        void renameLoops(const std::vector<llvm::BasicBlock*>& blocks)
        {
            for (llvm::BasicBlock* block : blocks)
            {
                llvm::Instruction* terminator = block->getTerminatorOrNull();
                llvm::MDNode* loop =
                    terminator != nullptr ? terminator->getMetadata(llvm::LLVMContext::MD_loop) : nullptr;
                if (loop == nullptr || loop->getNumOperands() == 0 || loop->getOperand(0) != loop)
                    continue;
                llvm::SmallVector<llvm::Metadata*, 4> operands{nullptr};
                for (unsigned i = 1; i < loop->getNumOperands(); ++i)
                    operands.push_back(loop->getOperand(i));
                llvm::MDNode* fresh = llvm::MDNode::getDistinct(block->getContext(), operands);
                fresh->replaceOperandWith(0, fresh);
                terminator->setMetadata(llvm::LLVMContext::MD_loop, fresh);
            }
        }

        // Ends `block` with the choice of a copy by knobs[level] onwards, the earlier knobs' values having chosen
        // `index` among the copies, which are numbered with knob 0's values slowest. A bool is a branch; an int is a
        // switch whose default, a value outside its domain, is the original body.
        void choose(llvm::BasicBlock& block, const std::vector<Knob>& knobs, std::size_t level, std::size_t index,
                    const std::vector<llvm::BasicBlock*>& starts, llvm::BasicBlock* original)
        {
            const Knob& knob = knobs[level];
            std::vector<llvm::BasicBlock*> targets;
            for (std::size_t v = 0; v < knob.domain.size(); ++v)
            {
                const std::size_t next = index * knob.domain.size() + v;
                if (level + 1 == knobs.size())
                {
                    targets.push_back(starts[next]);
                    continue;
                }
                llvm::BasicBlock* inner = llvm::BasicBlock::Create(
                    block.getContext(), "versions" + label(knob, *knob.domain[v]), block.getParent(), original);
                choose(*inner, knobs, level + 1, next, starts, original);
                targets.push_back(inner);
            }
            llvm::IRBuilder<> at(&block);
            if (covers(knob))
            {
                const std::size_t whenTrue = knob.domain[0]->isOne() ? 0 : 1;
                at.CreateCondBr(knob.value, targets[whenTrue], targets[1 - whenTrue]);
                return;
            }
            llvm::SwitchInst* choice = at.CreateSwitch(knob.value, original, static_cast<unsigned>(targets.size()));
            for (std::size_t v = 0; v < targets.size(); ++v)
                choice->addCase(knob.domain[v], targets[v]);
        }

        // The entry block's instructions before `last`, the last knob's read, that are computed from a knob (a
        // stream's base and stride, which its test selects, when another knob's read follows them) belong to the
        // body: each copy must compute them with its own values. They move to just after `last`, in their order, but
        // for one that touches memory, or that something else before `last` needs where it is.
        void sinkDerived(llvm::BasicBlock& entry, const std::vector<Knob>& knobs, llvm::Instruction& last)
        {
            llvm::SmallPtrSet<const llvm::Value*, 16> from;
            for (const Knob& knob : knobs)
                from.insert(knob.value);
            std::vector<llvm::Instruction*> derived;
            for (llvm::Instruction& instruction : llvm::make_range(entry.begin(), last.getIterator()))
            {
                const auto fromKnob = [&](const llvm::Use& operand) { return from.contains(operand.get()); };
                if (llvm::any_of(instruction.operands(), fromKnob) && !from.contains(&instruction))
                {
                    from.insert(&instruction);
                    derived.push_back(&instruction);
                }
            }
            llvm::SmallPtrSet<const llvm::Instruction*, 16> moving;   // decided from the last back, users first
            for (auto at = derived.rbegin(); at != derived.rend(); ++at)
            {
                llvm::Instruction* instruction = *at;
                bool movable = !instruction->mayReadOrWriteMemory() && !instruction->mayHaveSideEffects()
                            && !llvm::isa<llvm::AllocaInst>(instruction);
                for (const llvm::User* user : instruction->users())
                {
                    const auto* use = llvm::dyn_cast<llvm::Instruction>(user);
                    if (use != nullptr && use->getParent() == &entry && (use == &last || use->comesBefore(&last))
                        && !moving.contains(use))
                        movable = false;
                }
                if (movable)
                    moving.insert(instruction);
            }
            llvm::Instruction* after = &last;
            for (llvm::Instruction* instruction : derived)
            {
                if (!moving.contains(instruction))
                    continue;
                instruction->moveBefore(std::next(after->getIterator()));
                after = instruction;
            }
        }


        // In a copy, a stream's stride is a constant: 0 where the stream is flat, and 1 where it is not, which LLVM
        // cannot see in the stride's unit. Each select of `blocks` that a knob's value decides, between a unit and
        // something else, becomes what it picks, a unit becoming its value: so the copy for a stream's samples reads
        // them as the method did before it had flat streams, a sample after another.
        void foldUnits(const std::vector<llvm::BasicBlock*>& blocks)
        {
            std::vector<llvm::SelectInst*> selects;
            for (llvm::BasicBlock* block : blocks)
            {
                for (llvm::Instruction& instruction : *block)
                {
                    auto* select = llvm::dyn_cast<llvm::SelectInst>(&instruction);
                    if (select != nullptr && llvm::isa<llvm::ConstantInt>(select->getCondition())
                        && (unitOf(select->getTrueValue()) != nullptr || unitOf(select->getFalseValue()) != nullptr))
                        selects.push_back(select);
                }
            }
            for (llvm::SelectInst* select : selects)
            {
                llvm::Value* chosen = llvm::cast<llvm::ConstantInt>(select->getCondition())->isOne()
                                          ? select->getTrueValue()
                                          : select->getFalseValue();
                llvm::ConstantInt* unit = unitOf(chosen);
                select->replaceAllUsesWith(unit != nullptr ? unit : chosen);
                select->eraseFromParent();
            }
        }

        // Whether some instruction marks where `local` lives (llvm.lifetime.start or end).
        bool hasLifetime(const llvm::AllocaInst& local)
        {
            return llvm::any_of(local.users(), [](const llvm::User* user) {
                const auto* instruction = llvm::dyn_cast<llvm::Instruction>(user);
                return instruction != nullptr && instruction->isLifetimeStartOrEnd();
            });
        }

        // Marks where each of a body's locals lives: from the body's start, `start`, to each of its returns. Each
        // copy has locals of its own, and no two copies run at once, so the code generator gives their locals one
        // stack slot (stack colouring): the method's frame does not grow with its copies. A local whose lifetime is
        // marked already (by hand-written IR) is left as it is.
        void markLifetimes(const std::vector<llvm::AllocaInst*>& locals, llvm::BasicBlock& start,
                           const std::vector<llvm::BasicBlock*>& blocks)
        {
            std::vector<llvm::AllocaInst*> unmarked;
            std::ranges::copy_if(locals, std::back_inserter(unmarked),
                                 [](const llvm::AllocaInst* local) { return !hasLifetime(*local); });
            if (unmarked.empty())
                return;
            llvm::IRBuilder<> begin(&start, start.getFirstInsertionPt());
            for (llvm::AllocaInst* local : unmarked)
                begin.CreateLifetimeStart(local);
            for (llvm::BasicBlock* block : blocks)
            {
                if (!llvm::isa<llvm::ReturnInst>(block->getTerminator()))
                    continue;
                llvm::IRBuilder<> end(block->getTerminator());
                for (llvm::AllocaInst* local : unmarked)
                    end.CreateLifetimeEnd(local);
            }
        }

        // Versions `function` on `knobs`, in that order, into `copies` copies (the product of their domains' sizes).
        void version(llvm::Function& function, const std::vector<Knob>& knobs, std::size_t copies)
        {
            llvm::LLVMContext& context = function.getContext();
            llvm::BasicBlock& entry = function.getEntryBlock();

            // The split: after the last knob's read, with what is computed from the knobs before it moved after it.
            // The static allocas after it move up, so they stay in the entry block, which is what keeps them static
            // (and promotable); in their order, before its first instruction.
            llvm::Instruction* last = knobs.front().value;
            for (const Knob& knob : knobs)
            {
                if (last->comesBefore(knob.value))
                    last = knob.value;
            }
            sinkDerived(entry, knobs, *last);
            llvm::Instruction* front = &*entry.getFirstInsertionPt();
            std::vector<llvm::AllocaInst*> allocas;
            for (auto at = std::next(last->getIterator()); at != entry.end(); ++at)
            {
                auto* alloca = llvm::dyn_cast<llvm::AllocaInst>(&*at);
                if (alloca != nullptr && alloca->isStaticAlloca())
                    allocas.push_back(alloca);
            }
            for (llvm::AllocaInst* alloca : allocas)
                alloca->moveBefore(front->getIterator());
            llvm::BasicBlock* original = entry.splitBasicBlock(std::next(last->getIterator()), "body");

            // The body: every block the original body's start reaches, in the function's order.
            const llvm::SmallPtrSet<llvm::BasicBlock*, 32> reached(llvm::df_begin(original), llvm::df_end(original));
            std::vector<llvm::BasicBlock*> body;
            for (llvm::BasicBlock& block : function)
            {
                if (reached.contains(&block))
                    body.push_back(&block);
            }
            llvm::SmallVector<llvm::MDNode*, 2> scopes;   // declared inside the body: each copy gets its own
            llvm::identifyNoAliasScopesToClone(body, scopes);

            // The body's locals: the entry block's static allocas that only the body uses. Each copy gets its own, in
            // the entry block: a local shared by the copies would have its stores in one copy and its loads in all,
            // so that no copy's store dominates its loads (zero-init elision, which needs a local's memset to
            // dominate its loads, would then remove none).
            std::vector<llvm::AllocaInst*> locals;
            for (llvm::Instruction& instruction : entry)
            {
                auto* local = llvm::dyn_cast<llvm::AllocaInst>(&instruction);
                const auto inBody = [&](const llvm::User* user) {
                    const auto* use = llvm::dyn_cast<llvm::Instruction>(user);
                    return use != nullptr && reached.contains(use->getParent());
                };
                if (local != nullptr && local->isStaticAlloca() && llvm::all_of(local->users(), inBody))
                    locals.push_back(local);
            }

            // Each combination's values, knob 0's slowest.
            const auto combination = [&](std::size_t index) {
                std::vector<llvm::ConstantInt*> values(knobs.size());
                for (std::size_t k = knobs.size(); k-- > 0;)
                {
                    values[k] = knobs[k].domain[index % knobs[k].domain.size()];
                    index /= knobs[k].domain.size();
                }
                return values;
            };

            // The copies. With no fallback, the original body is copy 0, specialised in place once the others are
            // cloned from it.
            const bool fallback = !std::ranges::all_of(knobs, covers);
            std::vector<llvm::BasicBlock*> starts(copies, nullptr);
            for (std::size_t c = fallback ? 0 : 1; c < copies; ++c)
            {
                const std::vector<llvm::ConstantInt*> values = combination(c);
                llvm::ValueToValueMapTy map;
                std::string suffix;
                for (std::size_t k = 0; k < knobs.size(); ++k)
                {
                    map[knobs[k].value] = values[k];
                    suffix += label(knobs[k], *values[k]);
                }
                std::vector<llvm::AllocaInst*> copyLocals;
                for (llvm::AllocaInst* local : locals)
                {
                    auto* clone = llvm::cast<llvm::AllocaInst>(local->clone());
                    clone->insertAfter(local->getIterator());
                    clone->setName(local->getName() + suffix);
                    map[local] = clone;
                    copyLocals.push_back(clone);
                }
                std::vector<llvm::BasicBlock*> copy;
                for (llvm::BasicBlock* block : body)
                {
                    llvm::BasicBlock* clone = llvm::CloneBasicBlock(block, map, suffix, &function);
                    map[block] = clone;
                    copy.push_back(clone);
                }
                llvm::remapInstructionsInBlocks(copy, map);
                renameLoops(copy);
                if (!scopes.empty())
                    llvm::cloneAndAdaptNoAliasScopes(scopes, copy, context, suffix);
                starts[c] = llvm::cast<llvm::BasicBlock>(static_cast<llvm::Value*>(map[original]));
                markLifetimes(copyLocals, *starts[c], copy);
                foldUnits(copy);
            }
            if (!fallback)
            {
                const std::vector<llvm::ConstantInt*> values = combination(0);
                for (std::size_t k = 0; k < knobs.size(); ++k)
                {
                    knobs[k].value->replaceUsesWithIf(values[k], [&](llvm::Use& use) {
                        const auto* user = llvm::dyn_cast<llvm::Instruction>(use.getUser());
                        return user != nullptr && reached.contains(user->getParent());
                    });
                }
                starts[0] = original;
                foldUnits(body);
            }
            markLifetimes(locals, *original, body);   // the original body: copy 0, or the fallback

            // The choice, where the entry block branched to the body.
            entry.getTerminator()->eraseFromParent();
            choose(entry, knobs, 0, 0, starts, original);
        }
    } // namespace

    llvm::PreservedAnalyses KnobVersioningPass::run(llvm::Function& function, llvm::FunctionAnalysisManager& analyses)
    {
        if (function.isDeclaration() || function.hasOptNone() || function.hasOptSize())
            return llvm::PreservedAnalyses::all();   // nothing to copy, or asked not to grow
        if (function.hasFnAttribute(kVersionsAttribute))
            return llvm::PreservedAnalyses::all();   // versioned already: FMinus's optimised IR, optimised again
        const std::uint64_t budget = budgetOf(*function.getParent());
        if (budget < 2)
            return llvm::PreservedAnalyses::all();   // one copy is the function as it is
        const unsigned kind = function.getContext().getMDKindID(kDomainMetadata);
        const llvm::DataLayout& layout = function.getParent()->getDataLayout();
        std::vector<Knob> knobs;
        for (llvm::Instruction& instruction : function.getEntryBlock())
        {
            const llvm::MDNode* domain = instruction.getMetadata(kind);
            if (domain == nullptr)
                continue;
            Knob knob{&instruction, valuesOf(instruction, *domain), orderOf(instruction, layout)};
            if (!knob.domain.empty())
                knobs.push_back(std::move(knob));
        }
        if (knobs.empty())
            return llvm::PreservedAnalyses::all();

        const llvm::LoopInfo& loops = analyses.getResult<llvm::LoopAnalysis>(function);
        std::erase_if(knobs, [&](const Knob& knob) { return !steersLoop(*knob.value, loops, testsStream(knob)); });
        std::ranges::stable_sort(knobs, {}, [](const Knob& knob) { return std::pair(!testsStream(knob), knob.order); });
        std::vector<Knob> chosen;
        std::uint64_t copies = 1;
        for (const Knob& knob : knobs)
        {
            if (copies * knob.domain.size() > budget)
                continue;
            copies *= knob.domain.size();
            chosen.push_back(knob);
        }
        if (copies < 2)
            return llvm::PreservedAnalyses::all();

        version(function, chosen, static_cast<std::size_t>(copies));
        function.addFnAttr(kVersionsAttribute, std::to_string(copies));
        return llvm::PreservedAnalyses::none();
    }
} // namespace fminus::opt
