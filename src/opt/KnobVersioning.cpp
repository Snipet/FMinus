// src/opt/KnobVersioning.cpp: knob versioning (KnobVersioning.h). For each function:
//   1. Its knobs: the instructions of its entry block tagged !fminus.domain whose value reaches, through casts,
//      comparisons and logic on bools, the condition of a branch, a switch or a select inside one of its loops. A knob
//      that steers no loop is left alone: a copy would save one branch a call.
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
// shared by every copy, and dominates them all. The copies' memory accesses keep their !alias.scope and !noalias; a
// scope declared inside the body is cloned for each copy (cloneAndAdaptNoAliasScopes), and a loop's !llvm.loop is made
// anew for each, since no two loops may share one.
#include "opt/KnobVersioning.h"

#include <llvm/ADT/DepthFirstIterator.h>
#include <llvm/ADT/SmallPtrSet.h>
#include <llvm/ADT/SmallVector.h>
#include <llvm/ADT/StringExtras.h>
#include <llvm/Analysis/LoopInfo.h>
#include <llvm/IR/Constants.h>
#include <llvm/IR/DataLayout.h>
#include <llvm/IR/Function.h>
#include <llvm/IR/Instructions.h>
#include <llvm/IR/IRBuilder.h>
#include <llvm/IR/Metadata.h>
#include <llvm/IR/Module.h>
#include <llvm/Transforms/Utils/Cloning.h>
#include <llvm/Transforms/Utils/ValueMapper.h>

#include <algorithm>
#include <cstddef>
#include <cstdint>
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

        // Whether the knob is a stream's test for null (a pointer compared): chosen before the knobs. A stream's read
        // is a branch on it, which keeps its loop from vectorising where the stream's load alone would not have; a knob
        // left out costs only what it cost before there were copies.
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

        // Whether the knob decides something inside one of the function's loops: whether its value reaches, through
        // casts, comparisons and logic on bools, the condition of a conditional branch, a switch or a select in a
        // block of a loop. (Into a select's other operands, or a phi, it is data, not a decision.)
        bool steersLoop(const llvm::Instruction& knob, const llvm::LoopInfo& loops)
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
                        if (select->getCondition() == value && inLoop)
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

        // Versions `function` on `knobs`, in that order, into `copies` copies (the product of their domains' sizes).
        void version(llvm::Function& function, const std::vector<Knob>& knobs, std::size_t copies)
        {
            llvm::LLVMContext& context = function.getContext();
            llvm::BasicBlock& entry = function.getEntryBlock();

            // The split: after the last knob's read. The static allocas after it move up, so they stay in the entry
            // block, which is what keeps them static (and promotable); in their order, before its first instruction.
            llvm::Instruction* last = knobs.front().value;
            for (const Knob& knob : knobs)
            {
                if (last->comesBefore(knob.value))
                    last = knob.value;
            }
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
            }

            // The choice, where the entry block branched to the body.
            entry.getTerminator()->eraseFromParent();
            choose(entry, knobs, 0, 0, starts, original);
        }
    } // namespace

    llvm::PreservedAnalyses KnobVersioningPass::run(llvm::Function& function, llvm::FunctionAnalysisManager& analyses)
    {
        if (function.isDeclaration() || function.hasOptNone() || function.hasOptSize())
            return llvm::PreservedAnalyses::all();   // nothing to copy, or asked not to grow
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
        std::erase_if(knobs, [&](const Knob& knob) { return !steersLoop(*knob.value, loops); });
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
