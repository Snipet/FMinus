// src/ir/Statements.cpp: statements. Blocks, declarations and return, and C's control flow: if, while, for, switch and
// its labels, break and continue, in blocks named as clang names them (if.then, while.cond, for.inc, sw.bb, ...). A
// loop whose condition is not a constant expression must make progress, as in C11 (llvm.loop.mustprogress).
#include "ir/Lowering.h"

#include <llvm/IR/Constants.h>
#include <llvm/IR/GlobalVariable.h>
#include <llvm/IR/LLVMContext.h>
#include <llvm/IR/Metadata.h>

#include <algorithm>
#include <tuple>

namespace fminus::ir::detail
{
    // Whether `expr` is a constant expression (C11 6.6): constants, enumeration constants, sizeof, and operators, casts
    // and ?: on constant expressions only, nothing the program computes as it runs. A variable is never one, const or
    // not, as in C (C++ differs), and neither is a parameter, whatever the host's binding: a frozen one folds to a
    // constant, but the program means the same as with a knob. `folding` also takes what reads a constant of the top
    // level (folds()).
    bool Lowering::isConstant(const ast::Expr& expr, bool folding) const
    {
        return std::visit(
            Overloaded{
                [](const ast::NumberExpr&) { return true; },
                [](const ast::BoolExpr&) { return true; },
                [](const ast::SizeofExpr&) { return true; },
                [&](const ast::UnaryExpr& unary) {
                    switch (unary.op)
                    {
                    case ast::UnaryOp::Negate:
                    case ast::UnaryOp::Plus:
                    case ast::UnaryOp::Not:
                    case ast::UnaryOp::Complement:
                        return isConstant(*unary.operand, folding);
                    case ast::UnaryOp::Dereference:
                    case ast::UnaryOp::AddressOf:
                        return false;
                    }
                    return false;
                },
                [&](const ast::BinaryExpr& binary) {
                    return isConstant(*binary.lhs, folding) && isConstant(*binary.rhs, folding);
                },
                [&](const ast::LogicalExpr& logical) {
                    return isConstant(*logical.lhs, folding) && isConstant(*logical.rhs, folding);
                },
                [&](const ast::ConditionalExpr& conditional) {
                    return isConstant(*conditional.condition, folding) && isConstant(*conditional.thenValue, folding)
                           && isConstant(*conditional.elseValue, folding);
                },
                [&](const ast::CastExpr& cast) { return isConstant(*cast.operand, folding); },
                [&](const ast::ConstructExpr& construct) {
                    for (const ast::ExprPtr& arg : construct.args)
                    {
                        if (!isConstant(*arg, folding))
                            return false;
                    }
                    return true;
                },
                [&](const ast::MemberExpr& member) {
                    return !member.arrow && isConstant(*member.base, folding);   // v.x
                },
                [&](const ast::VariableExpr& variable) {
                    if (enumerator(variable.name))
                        return true;
                    // A constant of the top level that is no array: lower() reads it as its initialiser's value.
                    const Binding* binding = folding ? named(variable.name) : nullptr;
                    const auto* global =
                        binding != nullptr ? llvm::dyn_cast_if_present<llvm::GlobalVariable>(binding->slot) : nullptr;
                    return global != nullptr && !ast::isArray(binding->type) && global->hasInitializer();
                },
                [](const ast::IndexExpr&) { return false; },
                [](const ast::CallExpr&) { return false; },
                [](const ast::MethodCallExpr&) { return false; },
                [](const ast::ThisExpr&) { return false; },
                [](const ast::AssignExpr&) { return false; },
                [](const ast::IncrementExpr&) { return false; },
            },
            expr.node);
    }

    // Whether `expr` folds to a constant, as clang folds a condition where it decides what its flow analysis reaches
    // and whether a switch's value is a constant: a constant expression (isConstant), or one that also reads a
    // constant of the top level, `const int RUNNING = 1;`, which is its value wherever it is read (Expressions.cpp). On
    // the tree, never on the IR: a parameter is none, whatever the host binds it to, so that a program compiles the
    // same, with the same errors and warnings, with a knob, a stream or a frozen value (whose read lowers to a
    // constant).
    bool Lowering::folds(const ast::Expr& expr) const
    {
        return isConstant(expr, true);
    }

    // C11 6.8.5p6: a loop whose controlling expression is not a constant expression, and which does no I/O and
    // touches no volatile or atomic object, may be assumed to terminate. Clang tells LLVM so for C11 and later
    // (CodeGenFunction::checkIfLoopMustProgress), and so does FMinus, with the same metadata on the same branches:
    // every branch back to the loop's condition block, `header`, carries a loop ID that holds
    // llvm.loop.mustprogress. `entry`, the branch into the loop, does not. A loop that runs for ever without
    // effects is then undefined, and LLVM may delete a loop whose results go unused without proving that it
    // ends. (FMinus has no volatile objects; its atomics are knob reads, once a call, and I/O is in external
    // functions, which LLVM assumes may make progress.) A loop with a constant condition, `for (;;)` or
    // `while (1)`, may run for ever, as in C; and no function is marked mustprogress, which C does not imply.
    void mustProgress(llvm::BasicBlock* header, const llvm::Instruction* entry)
    {
        llvm::LLVMContext& context = header->getContext();
        llvm::Metadata* operands[] = {
            nullptr, llvm::MDNode::get(context, llvm::MDString::get(context, "llvm.loop.mustprogress"))};
        llvm::MDNode* loop = llvm::MDNode::getDistinct(context, operands);
        loop->replaceOperandWith(0, loop);   // a loop ID names itself, so that it is this loop's alone
        for (llvm::User* user : header->users())
        {
            auto* branch = llvm::dyn_cast<llvm::Instruction>(user);
            if (branch != nullptr && branch != entry && branch->isTerminator())
                branch->setMetadata(llvm::LLVMContext::MD_loop, loop);
        }
    }

    void Lowering::lowerStatements(const std::vector<ast::Stmt>& statements)
    {
        for (const ast::Stmt& stmt : statements)
            lowerStatement(stmt);
    }

    void Lowering::lowerStatement(const ast::Stmt& stmt)
    {
        // C accepts statements after a return, a break or a continue, and before a switch's first label. They still
        // lower, so their errors are found, into a block nothing branches to, which the optimiser deletes. A label
        // starts a block of its own, which the switch reaches, and a block's items each look for themselves. (Not
        // getTerminator(): since LLVM 23 it assumes a finished block.)
        const bool ownBlock = std::holds_alternative<ast::CaseStmt>(stmt.node)
                              || std::holds_alternative<ast::DefaultStmt>(stmt.node)
                              || std::holds_alternative<ast::BlockStmt>(stmt.node)
                              || std::holds_alternative<ast::EnumStmt>(stmt.node);   // no code
        if (builder_.GetInsertBlock()->hasTerminator() && !ownBlock)
        {
            llvm::Function* function = builder_.GetInsertBlock()->getParent();
            builder_.SetInsertPoint(llvm::BasicBlock::Create(module_.getContext(), "unreachable", function));
        }
        std::visit(Overloaded{
                       [&](const ast::DeclStmt& decl) { declareVariable(stmt.location, decl); },
                       [&](const ast::ExprStmt& statement) { lower(*statement.expr); },
                       [&](const ast::ReturnStmt& ret) { lowerReturn(stmt.location, ret); },
                       [&](const ast::BlockStmt& block) {
                           scopes_.emplace_back();
                           lowerStatements(block.items);
                           scopes_.pop_back();
                       },
                       [&](const ast::EmptyStmt&) {},
                       [&](const ast::IfStmt& branch) { lowerIf(branch); },
                       [&](const ast::WhileStmt& loop) { lowerWhile(loop); },
                       [&](const ast::ForStmt& loop) { lowerFor(loop); },
                       [&](const ast::BreakStmt&) {
                           if (breakables_.empty())
                               error(stmt.location, "'break' statement not in loop or switch statement");
                           else if (breakables_.back().kind == Breakable::Kind::Region)   // its body (Parallel.cpp)
                               error(stmt.location, regions_.back().loop
                                                        ? "'break' cannot leave a parallel for: its iterations run "
                                                          "at once, and none can end the others"
                                                        : "'break' cannot leave a statement of a parallel block");
                           else
                               builder_.CreateBr(breakables_.back().breakTarget);
                       },
                       [&](const ast::ContinueStmt&) {
                           // A switch is no loop: continue goes to the next iteration of the loop around it.
                           const auto loop = std::ranges::find_if(breakables_.rbegin(), breakables_.rend(),
                                                                  [](const Breakable& around) {
                                                                      return around.kind != Breakable::Kind::Switch;
                                                                  });
                           if (loop == breakables_.rend())
                               error(stmt.location, "'continue' statement not in loop statement");
                           else if (loop->continueTarget == nullptr)
                               error(stmt.location, "'continue' cannot leave a statement of a parallel block");
                           else
                               builder_.CreateBr(loop->continueTarget);
                       },
                       [&](const ast::SwitchStmt& branch) { lowerSwitch(branch); },
                       [&](const ast::CaseStmt& label) { lowerLabel(stmt.location, label.value.get(), label.body); },
                       [&](const ast::DefaultStmt& label) { lowerLabel(stmt.location, nullptr, label.body); },
                       [&](const ast::EnumStmt& statement) { declareEnum(statement.enumeration); },
                       [&](const ast::ParallelForStmt& loop) { lowerParallelFor(stmt.location, loop); },
                       [&](const ast::ParallelBlockStmt& block) { lowerParallelBlock(stmt.location, block); },
                   },
                   stmt.node);
    }

    // A sub-statement (a branch, a loop body) has a scope of its own, as in C.
    void Lowering::lowerScoped(const ast::Stmt& stmt)
    {
        scopes_.emplace_back();
        lowerStatement(stmt);
        scopes_.pop_back();
    }

    // `return value;` converts the value to the function's result type; a void function's is `return;`, and each
    // kind of function refuses the other's, as C does.
    void Lowering::lowerReturn(SourceLocation location, const ast::ReturnStmt& ret)
    {
        if (!regions_.empty())   // in a parallel region's body, a function of its own (Parallel.cpp)
        {
            error(location, regions_.back().loop
                                ? "cannot return from a parallel for: its iterations run at once, and none can end "
                                  "the function"
                                : "cannot return from a statement of a parallel block");
            if (ret.value != nullptr)
                lower(*ret.value);   // for its own errors
            builder_.CreateRetVoid();
            return;
        }
        const ast::Type result = function_->returnType;
        const bool returnsVoid = result == ast::Type::Void;
        if (ret.value == nullptr)
        {
            if (!returnsVoid)
                error(location, "non-void function " + quoted(function_->name) + " should return a value");
            if (returnsVoid)
                builder_.CreateRetVoid();
            else
                builder_.CreateRet(poison(result).ir);
            return;
        }
        const Value value = lowerAs(*ret.value, result);
        if (returnsVoid)
        {
            error(location, "void function " + quoted(function_->name) + " should not return a value");
            builder_.CreateRetVoid();
            return;
        }
        builder_.CreateRet(convert(value, result, location, Use::Returning));
    }

    llvm::BasicBlock* Lowering::newBlock(const char* name)
    {
        return llvm::BasicBlock::Create(module_.getContext(), name, builder_.GetInsertBlock()->getParent());
    }

    // Branches to `target` unless the block already ends (a return, a break, a continue in it).
    void Lowering::fallThrough(llvm::BasicBlock* target)
    {
        if (!builder_.GetInsertBlock()->hasTerminator())
            builder_.CreateBr(target);
    }

    // A branch on `condition`, the test `test` lowered, to `whenTrue` or `whenFalse`; on a constant (`while (1)`,
    // `if (true)`), to the one it picks alone, as clang branches on one. Whether it is one is the tree's to say
    // (folds()), and its value the IR's, which folds a constant's test against 0 to a constant: what only the other
    // edge would reach is then reached by nothing, so after a `while (1)` without a break the end of a non-void
    // function needs no return (Functions.cpp), as clang's CFG finds. A frozen parameter's test is a constant in the
    // IR too, but no constant to the tree: the branch stays, and the program compiles as it does with a knob. The block
    // the branch does not take is still lowered, for its errors, and the optimiser deletes it.
    void Lowering::branchOn(const ast::Expr& test, llvm::Value* condition, llvm::BasicBlock* whenTrue,
                            llvm::BasicBlock* whenFalse)
    {
        const auto* constant = llvm::dyn_cast<llvm::ConstantInt>(condition);
        if (constant != nullptr && folds(test))
            builder_.CreateBr(constant->isOne() ? whenTrue : whenFalse);
        else
            builder_.CreateCondBr(condition, whenTrue, whenFalse);
    }

    // if.then and if.else join at if.end. A branch that returns never gets there; when neither does, if.end
    // is unreachable, and so is what follows. A constant condition takes one branch (branchOn()).
    void Lowering::lowerIf(const ast::IfStmt& branch)
    {
        llvm::Value* condition =
            convert(lower(*branch.condition), ast::Type::Bool, branch.condition->location, Use::Condition);
        llvm::BasicBlock* thenBlock = newBlock("if.then");
        llvm::BasicBlock* elseBlock = branch.elseBranch ? newBlock("if.else") : nullptr;
        llvm::BasicBlock* endBlock = newBlock("if.end");
        branchOn(*branch.condition, condition, thenBlock, elseBlock != nullptr ? elseBlock : endBlock);

        builder_.SetInsertPoint(thenBlock);
        lowerScoped(*branch.thenBranch);
        fallThrough(endBlock);
        if (elseBlock != nullptr)
        {
            builder_.SetInsertPoint(elseBlock);
            lowerScoped(*branch.elseBranch);
            fallThrough(endBlock);
        }
        builder_.SetInsertPoint(endBlock);
    }

    // while.cond tests and branches to while.body or while.end; the body goes back to while.cond. break goes
    // to while.end, continue to while.cond. The branches back carry llvm.loop.mustprogress unless the condition is a
    // constant expression (mustProgress). A constant condition branches to one of the two (branchOn()): `while (1)`
    // reaches while.end only by a break.
    void Lowering::lowerWhile(const ast::WhileStmt& loop)
    {
        llvm::BasicBlock* condBlock = newBlock("while.cond");
        llvm::BasicBlock* bodyBlock = newBlock("while.body");
        llvm::BasicBlock* endBlock = newBlock("while.end");
        const llvm::Instruction* entry = builder_.CreateBr(condBlock);

        builder_.SetInsertPoint(condBlock);
        branchOn(*loop.condition,
                 convert(lower(*loop.condition), ast::Type::Bool, loop.condition->location, Use::Condition), bodyBlock,
                 endBlock);

        builder_.SetInsertPoint(bodyBlock);
        breakables_.push_back({Breakable::Kind::Loop, endBlock, condBlock});
        lowerScoped(*loop.body);
        breakables_.pop_back();
        fallThrough(condBlock);
        if (!isConstant(*loop.condition))
            mustProgress(condBlock, entry);

        builder_.SetInsertPoint(endBlock);
    }

    // for (init; cond; step) body: init, then for.cond -> for.body -> for.inc (the step) -> for.cond, until
    // for.end. continue goes to for.inc, so the step still runs. The init's declarations are the loop's. for.inc's
    // branch back carries llvm.loop.mustprogress when there is a condition and it is not a constant expression. With
    // no condition, or a constant one (branchOn()), only a break reaches for.end.
    void Lowering::lowerFor(const ast::ForStmt& loop)
    {
        scopes_.emplace_back();
        lowerStatements(loop.init);
        llvm::BasicBlock* condBlock = newBlock("for.cond");
        llvm::BasicBlock* bodyBlock = newBlock("for.body");
        llvm::BasicBlock* incBlock = newBlock("for.inc");
        llvm::BasicBlock* endBlock = newBlock("for.end");
        const llvm::Instruction* entry = builder_.CreateBr(condBlock);

        builder_.SetInsertPoint(condBlock);
        if (loop.condition)
            branchOn(*loop.condition,
                     convert(lower(*loop.condition), ast::Type::Bool, loop.condition->location, Use::Condition),
                     bodyBlock, endBlock);
        else
            builder_.CreateBr(bodyBlock);   // no condition: until a break or a return

        builder_.SetInsertPoint(bodyBlock);
        breakables_.push_back({Breakable::Kind::Loop, endBlock, incBlock});
        lowerScoped(*loop.body);
        breakables_.pop_back();
        fallThrough(incBlock);

        builder_.SetInsertPoint(incBlock);
        if (loop.step)
            lower(*loop.step);
        builder_.CreateBr(condBlock);
        if (loop.condition && !isConstant(*loop.condition))
            mustProgress(condBlock, entry);

        builder_.SetInsertPoint(endBlock);
        scopes_.pop_back();
    }

    // switch (value) body, as clang lowers it: the value (an int, or a bool promoted to one) goes to LLVM's switch, to
    // which each label adds its value and its block as the body is lowered (lowerLabel), and whose default is the
    // end, sw.epilog, until a default label replaces it. break goes to sw.epilog, and continue to the loop around.
    // Nothing reaches what the body has before its first label (lowerStatement). An operand with an error of its own
    // gives the switch no second error (Value). A value of an enumerated type that does not fold to a constant
    // (folds(), as clang's check folds it: a constant of the top level is one), without a default, gets clang's
    // -Wswitch for the constants no case has (unhandled()).
    void Lowering::lowerSwitch(const ast::SwitchStmt& branch)
    {
        const Value value = lower(*branch.value);
        llvm::Value* condition = poison(ast::Type::Int).ir;
        if (ast::arithmeticOf(value.type) == ast::Type::Int)
            condition = convert(value, ast::Type::Int, branch.value->location, Use::Initializing);
        else if (!value.failed)
            error(branch.value->location, "statement requires expression of integer type ("
                                              + quoted(ast::typeName(value.type)) + " invalid)");
        llvm::BasicBlock* endBlock = newBlock("sw.epilog");
        llvm::SwitchInst* instruction = builder_.CreateSwitch(condition, endBlock);
        breakables_.push_back({Breakable::Kind::Switch, endBlock, nullptr});
        switches_.push_back({instruction, scopes_.size(), {}, std::nullopt});
        lowerScoped(*branch.body);
        const Switch lowered = std::move(switches_.back());
        switches_.pop_back();
        breakables_.pop_back();
        fallThrough(endBlock);
        if (llvm::BasicBlock* last = &endBlock->getParent()->back(); last != endBlock)
            endBlock->moveAfter(last);   // after the labels' blocks, as clang's
        builder_.SetInsertPoint(endBlock);
        if (!value.failed && ast::arithmeticOf(value.type) == ast::Type::Int && !folds(*branch.value))
            unhandled(begin(*branch.value), value.type.enumeration, lowered);
    }

    // Where `expr` begins: its opening parenthesis when it has one (`(e)`), and its first operand's beginning for an
    // operator written after it (`m->mode`, `e = A`, `a[i]`), as clang places a diagnostic about a whole expression;
    // the tree places those at the operator. The rest begin where the tree places them.
    SourceLocation Lowering::begin(const ast::Expr& expr)
    {
        if (expr.opening)
            return *expr.opening;
        const SourceLocation here = expr.location;
        return std::visit(Overloaded{
                              [&](const ast::BinaryExpr& binary) { return begin(*binary.lhs); },
                              [&](const ast::LogicalExpr& logical) { return begin(*logical.lhs); },
                              [&](const ast::AssignExpr& assign) { return begin(*assign.target); },
                              [&](const ast::ConditionalExpr& conditional) { return begin(*conditional.condition); },
                              [&](const ast::MemberExpr& member) { return begin(*member.base); },
                              [&](const ast::MethodCallExpr& call) { return begin(*call.object); },
                              [&](const ast::IndexExpr& index) { return begin(*index.base); },
                              [&](const ast::IncrementExpr& increment) {
                                  const bool postfix = increment.op == ast::IncrementOp::PostIncrement
                                                       || increment.op == ast::IncrementOp::PostDecrement;
                                  return postfix ? begin(*increment.target) : here;
                              },
                              [&](const ast::NumberExpr&) { return here; },
                              [&](const ast::VariableExpr&) { return here; },
                              [&](const ast::BoolExpr&) { return here; },
                              [&](const ast::UnaryExpr&) { return here; },
                              [&](const ast::CallExpr&) { return here; },
                              [&](const ast::ConstructExpr&) { return here; },
                              [&](const ast::ThisExpr&) { return here; },
                              [&](const ast::CastExpr&) { return here; },
                              [&](const ast::SizeofExpr&) { return here; },
                          },
                          expr.node);
    }

    // clang's -Wswitch, FMinus's one warning: a switch without a default over a value of an enumerated type (one that
    // does not fold to a constant), whose cases miss some of the enumeration's constants, names them, where the value
    // begins, in clang's words and with its note, each value by its first constant and in order of value, as clang
    // lists them: "enumeration value 'C' not handled in switch [-Wswitch]". Nothing when a case is in error, or when
    // the enumeration has no constants yet (C23 declares one ahead of them, `enum Mode : int;`, and a switch before
    // its list knows none). clang's other -Wswitch, a case value that is none of the enumeration's, is not FMinus's;
    // nor is its warning for a constant value that no case has, "no case matching constant switch condition '1'".
    void Lowering::unhandled(SourceLocation location, int enumeration, const Switch& lowered)
    {
        if (enumeration == 0 || lowered.defaultLabel || lowered.erroneous)
            return;
        const auto found = enumerations_.find(enumeration);
        if (found == enumerations_.end())
            return;
        std::vector<std::pair<std::int32_t, std::string>> constants = found->second;
        std::ranges::stable_sort(constants, {}, [](const auto& constant) { return constant.first; });
        std::vector<std::string> missing;
        for (std::size_t i = 0; i < constants.size(); ++i)
        {
            if (i > 0 && constants[i].first == constants[i - 1].first)
                continue;   // a value's first name stands for it
            if (!lowered.cases.contains(constants[i].first))
                missing.push_back(quoted(constants[i].second));
        }
        std::string message;
        switch (missing.size())
        {
        case 0:
            return;
        case 1:
            message = "enumeration value " + missing[0] + " not handled in switch";
            break;
        case 2:
            message = "enumeration values " + missing[0] + " and " + missing[1] + " not handled in switch";
            break;
        case 3:
            message = "enumeration values " + missing[0] + ", " + missing[1] + ", and " + missing[2]
                      + " not handled in switch";
            break;
        default:
            message = std::to_string(missing.size()) + " enumeration values not handled in switch: " + missing[0]
                      + ", " + missing[1] + ", " + missing[2] + "...";
            break;
        }
        diagnostics_.warning(location, message + " [-Wswitch]", {Diagnostic{location, "add missing switch cases", {}}});
    }

    namespace
    {
        // The name a case label's value is written as, `A` in `case A:` and `case (int)A:`, or none: clang names it so
        // in a duplicate's message.
        std::string labelName(const ast::Expr* value)
        {
            while (const auto* cast = std::get_if<ast::CastExpr>(&value->node))
                value = cast->operand.get();
            const auto* variable = std::get_if<ast::VariableExpr>(&value->node);
            return variable != nullptr ? variable->name : "";
        }
    } // namespace

    // A case label (`value`) or default (none), and the statement it labels, if any: a block of its own, sw.bb or
    // sw.default, into which the code before it falls, and which the innermost switch jumps to for its value. The
    // value is an integer constant expression (Constants.cpp). A label outside a switch, two of one value and two
    // defaults are clang's errors, the duplicates with a note at the label before. A switch cannot jump into a
    // parallel region inside it: the region's body is a function of its own (Parallel.cpp).
    void Lowering::lowerLabel(SourceLocation location, const ast::Expr* value, const ast::StmtPtr& body)
    {
        const std::string keyword = value != nullptr ? "'case'" : "'default'";
        const auto around = std::ranges::find_if(breakables_.rbegin(), breakables_.rend(), [](const Breakable& b) {
            return b.kind != Breakable::Kind::Loop;
        });
        const bool inSwitch = std::ranges::any_of(breakables_, [](const Breakable& b) {
            return b.kind == Breakable::Kind::Switch;
        });
        Switch* branch = nullptr;
        if (!inSwitch)
            error(location, keyword + " statement not in switch statement");
        else if (around->kind == Breakable::Kind::Region)
            error(location, keyword + (regions_.back().loop ? " cannot jump into a parallel for"
                                                             : " cannot jump into a statement of a parallel block"));
        else
            branch = &switches_.back();

        llvm::BasicBlock* block = newBlock(value != nullptr ? "sw.bb" : "sw.default");
        fallThrough(block);
        if (branch != nullptr && value != nullptr)
        {
            const std::optional<std::int32_t> constant = integerConstant(*value);
            if (!constant)
                branch->erroneous = true;
            else
            {
                const CaseLabel label{value->location, labelName(value)};
                const auto [previous, added] = branch->cases.try_emplace(*constant, label);
                if (added)
                    branch->instruction->addCase(builder_.getInt32(static_cast<std::uint32_t>(*constant)),
                                                 bypass(*branch, block));
                else
                {
                    branch->erroneous = true;
                    // clang's: the value's name when both are written as one, else both names (or the number)
                    const std::string number = std::to_string(*constant);
                    const std::string before = previous->second.name.empty() ? number : previous->second.name;
                    const std::string now = label.name.empty() ? number : label.name;
                    diagnostics_.error(label.location,
                                       before == now ? "duplicate case value " + quoted(now)
                                                     : "duplicate case value: " + quoted(before) + " and "
                                                           + quoted(now) + " both equal " + quoted(number),
                                       {Diagnostic{previous->second.location, "previous case defined here", {}}});
                    previous->second = label;   // clang compares each label with the one of its value before it
                }
            }
        }
        else if (branch != nullptr)
        {
            if (branch->defaultLabel)
                diagnostics_.error(location, "multiple default labels in one switch",
                                   {Diagnostic{*branch->defaultLabel, "previous case defined here", {}}});
            else
            {
                branch->defaultLabel = location;
                branch->instruction->setDefaultDest(bypass(*branch, block));
            }
        }
        builder_.SetInsertPoint(block);
        if (body)
            lowerStatement(*body);
    }

    // Where the switch's jump to `label` lands: the label's block, or, when declarations of the switch's body are in
    // scope there (before the label in its block, or in a block or a loop around it), a block, sw.zero, that zeroes
    // each of their variables and goes on to the label's. C allows the jump and leaves such a variable indeterminate;
    // FMinus starts every variable at 0, so it is 0 there too, as if its declaration had run without an initialiser.
    // (An enumerator holds no memory, and a const table is a constant of the module, Memory.cpp: neither is zeroed.)
    llvm::BasicBlock* Lowering::bypass(const Switch& branch, llvm::BasicBlock* label)
    {
        std::vector<std::tuple<std::string, std::size_t, Binding>> passed;
        for (std::size_t scope = branch.base; scope < scopes_.size(); ++scope)
        {
            for (const auto& [name, binding] : scopes_[scope])
            {
                if (binding.slot != nullptr && !llvm::isa<llvm::GlobalVariable>(binding.slot))
                    passed.emplace_back(name, scope, binding);
            }
        }
        if (passed.empty())
            return label;
        std::ranges::sort(passed, {}, [](const auto& variable) {   // by name: the same IR every time
            return std::pair(std::get<0>(variable), std::get<1>(variable));
        });
        const llvm::IRBuilderBase::InsertPointGuard resume(builder_);
        llvm::BasicBlock* zeroes = newBlock("sw.zero");
        builder_.SetInsertPoint(zeroes);
        for (const auto& [name, scope, binding] : passed)
        {
            if (ast::isArray(binding.type) || ast::isModule(binding.type))
                zero(binding.slot, binding.type);
            else
                builder_.CreateStore(llvm::Constant::getNullValue(typeOf(binding.type)), binding.slot);
        }
        builder_.CreateBr(label);
        return zeroes;
    }

    // The initialiser is lowered before the name is bound, so `float x = x;` reads an outer x. Without one,
    // the variable starts at 0. An array's size is worked out first (sized, Constants.cpp: an integer constant
    // expression, or its list's length). An array or a module starts at 0 too (a memset), and then takes its
    // initialiser: an array its list's elements, a module a copy of another. A const array of constants is stored
    // once instead (declareConstArray, Memory.cpp).
    void Lowering::declareVariable(SourceLocation location, const ast::DeclStmt& decl)
    {
        const auto declare = [&](llvm::Value* slot, bool isConst, ast::Type type) {
            if (!bind(decl.name, Binding{slot, isConst, type, location}))
                redefinition(location, decl.name, false, scopes_.back().at(decl.name));
        };
        ast::Type type = decl.type;
        if (ast::isArray(type) && decl.size == nullptr && decl.init)
            type.count = 1;   // its initialiser's error says why
        else if (ast::isArray(type))
            type = sized(location, decl.name, type, decl.size.get(), decl.hasList ? &decl.list : nullptr);
        if (type == ast::Type::Void)
        {
            error(location, "variable has incomplete type 'void'");
            type = ast::Type::Float;   // so its uses report no more errors
        }
        if (!checkType(location, type))   // likewise
            type = ast::isArray(type) ? ast::Type::arrayOf(ast::Type::Float, type.count)
                                      : ast::Type::pointerTo(ast::Kind::Float, type.pointeeConst);
        if (decl.isConst && ast::isArray(type) && !decl.init && !ast::isModule(ast::pointee(type)))
        {
            // A table: a constant of the module when its list is constants, a local otherwise (Memory.cpp).
            declare(declareConstArray(location, decl, type), true, type);
            return;
        }
        if (ast::isArray(type) || ast::isModule(type))
        {
            llvm::AllocaInst* slot = allocate(typeOf(type), decl.name);
            zero(slot, type);
            if (decl.init)
            {
                const Value value = lower(*decl.init);
                if (ast::isArray(type))
                    error(location, "array initializer must be an initializer list");
                else if (value.type == type)
                    copy(slot, value.ir, type);
                else if (!value.failed)   // else its own error says why (Value)
                    incompatible(location, Use::Initializing, value.type, type);
            }
            if (decl.hasList && ast::isArray(type))
                initializeArray(location, slot, type, decl.list);
            else if (decl.hasList)
                error(location, "a module takes no initializer list: its fields start at 0 (set them in a method, "
                                "such as init)");
            declare(slot, decl.isConst, type);
            return;
        }
        if (decl.hasList)
            error(location, "FMinus takes an initializer list for an array only");
        llvm::Value* value = decl.init ? convert(lowerAs(*decl.init, type), type, location, Use::Initializing)
                                       : llvm::Constant::getNullValue(typeOf(type));
        llvm::AllocaInst* slot = allocate(typeOf(type), decl.name);
        builder_.CreateStore(value, slot);
        declare(slot, decl.isConst, type);
    }

    // `name`, declared at `location` as an enumeration constant or a variable, where `previous` already has it in the
    // same scope: clang's words for each pair (two variables' without the note, as FMinus has always reported them).
    // Clang says "as different kind of symbol" only when the enumeration constant came first; a constant after a
    // variable, a parameter or a constant of the top level is a plain redefinition, with its note.
    void Lowering::redefinition(SourceLocation location, const std::string& name, bool enumerator,
                                const Binding& previous)
    {
        const std::vector<Diagnostic> before{Diagnostic{previous.location, "previous definition is here", {}}};
        if (enumerator && previous.enumerator)
            diagnostics_.error(location, "redefinition of enumerator " + quoted(name), before);
        else if (enumerator)
            diagnostics_.error(location, "redefinition of " + quoted(name), before);
        else if (previous.enumerator)
            diagnostics_.error(location, "redefinition of " + quoted(name) + " as different kind of symbol", before);
        else
            error(location, "redefinition of " + quoted(name));
    }
} // namespace fminus::ir::detail
