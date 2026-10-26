// src/ir/Statements.cpp: statements. Blocks, declarations and return, and C's control flow: if, while, for, break and
// continue, in blocks named as clang names them (if.then, while.cond, for.inc, ...). A loop whose condition is not a
// constant expression must make progress, as in C11 (llvm.loop.mustprogress).
#include "ir/Lowering.h"

#include <llvm/IR/Constants.h>
#include <llvm/IR/LLVMContext.h>
#include <llvm/IR/Metadata.h>

namespace fminus::ir::detail
{
    namespace
    {
        // Whether `expr` is a constant expression (C11 6.6): constants, sizeof, and operators, casts and ?: on
        // constant expressions only, nothing the program computes as it runs. A variable is never one, const or not,
        // as in C (C++ differs), and neither is a parameter, whatever the host's binding: a frozen one folds to a
        // constant, but the program means the same as with a knob.
        bool isConstant(const ast::Expr& expr)
        {
            return std::visit(
                Overloaded{
                    [](const ast::NumberExpr&) { return true; },
                    [](const ast::BoolExpr&) { return true; },
                    [](const ast::SizeofExpr&) { return true; },
                    [](const ast::UnaryExpr& unary) {
                        switch (unary.op)
                        {
                        case ast::UnaryOp::Negate:
                        case ast::UnaryOp::Plus:
                        case ast::UnaryOp::Not:
                            return isConstant(*unary.operand);
                        case ast::UnaryOp::Dereference:
                        case ast::UnaryOp::AddressOf:
                            return false;
                        }
                        return false;
                    },
                    [](const ast::BinaryExpr& binary) { return isConstant(*binary.lhs) && isConstant(*binary.rhs); },
                    [](const ast::LogicalExpr& logical) {
                        return isConstant(*logical.lhs) && isConstant(*logical.rhs);
                    },
                    [](const ast::ConditionalExpr& conditional) {
                        return isConstant(*conditional.condition) && isConstant(*conditional.thenValue)
                               && isConstant(*conditional.elseValue);
                    },
                    [](const ast::CastExpr& cast) { return isConstant(*cast.operand); },
                    [](const ast::ConstructExpr& construct) {
                        for (const ast::ExprPtr& arg : construct.args)
                        {
                            if (!isConstant(*arg))
                                return false;
                        }
                        return true;
                    },
                    [](const ast::MemberExpr& member) { return !member.arrow && isConstant(*member.base); },   // v.x
                    [](const ast::VariableExpr&) { return false; },
                    [](const ast::IndexExpr&) { return false; },
                    [](const ast::CallExpr&) { return false; },
                    [](const ast::MethodCallExpr&) { return false; },
                    [](const ast::ThisExpr&) { return false; },
                    [](const ast::AssignExpr&) { return false; },
                    [](const ast::IncrementExpr&) { return false; },
                },
                expr.node);
        }
    } // namespace

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
        {
            // C accepts statements after a return (and, once the language has them, a break or a continue).
            // They still lower, so their errors are found, into a block nothing branches to, which the
            // optimiser deletes. (Not getTerminator(): since LLVM 23 it assumes a finished block.)
            if (builder_.GetInsertBlock()->hasTerminator())
            {
                llvm::Function* function = builder_.GetInsertBlock()->getParent();
                builder_.SetInsertPoint(
                    llvm::BasicBlock::Create(module_.getContext(), "unreachable", function));
            }
            lowerStatement(stmt);
        }
    }

    void Lowering::lowerStatement(const ast::Stmt& stmt)
    {
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
                           if (loops_.empty())
                               error(stmt.location, "'break' statement not in loop or switch statement");
                           else if (loops_.back().breakTarget == nullptr)   // a parallel region's body (Parallel.cpp)
                               error(stmt.location, regions_.back().loop
                                                        ? "'break' cannot leave a parallel for: its iterations run "
                                                          "at once, and none can end the others"
                                                        : "'break' cannot leave a statement of a parallel block");
                           else
                               builder_.CreateBr(loops_.back().breakTarget);
                       },
                       [&](const ast::ContinueStmt&) {
                           if (loops_.empty())
                               error(stmt.location, "'continue' statement not in loop statement");
                           else if (loops_.back().continueTarget == nullptr)
                               error(stmt.location, "'continue' cannot leave a statement of a parallel block");
                           else
                               builder_.CreateBr(loops_.back().continueTarget);
                       },
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
        const Value value = lower(*ret.value);
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

    // if.then and if.else join at if.end. A branch that returns never gets there; when neither does, if.end
    // is unreachable, and so is what follows.
    void Lowering::lowerIf(const ast::IfStmt& branch)
    {
        llvm::Value* condition =
            convert(lower(*branch.condition), ast::Type::Bool, branch.condition->location, Use::Condition);
        llvm::BasicBlock* thenBlock = newBlock("if.then");
        llvm::BasicBlock* elseBlock = branch.elseBranch ? newBlock("if.else") : nullptr;
        llvm::BasicBlock* endBlock = newBlock("if.end");
        builder_.CreateCondBr(condition, thenBlock, elseBlock != nullptr ? elseBlock : endBlock);

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
    // constant expression (mustProgress).
    void Lowering::lowerWhile(const ast::WhileStmt& loop)
    {
        llvm::BasicBlock* condBlock = newBlock("while.cond");
        llvm::BasicBlock* bodyBlock = newBlock("while.body");
        llvm::BasicBlock* endBlock = newBlock("while.end");
        const llvm::Instruction* entry = builder_.CreateBr(condBlock);

        builder_.SetInsertPoint(condBlock);
        builder_.CreateCondBr(
            convert(lower(*loop.condition), ast::Type::Bool, loop.condition->location, Use::Condition),
            bodyBlock, endBlock);

        builder_.SetInsertPoint(bodyBlock);
        loops_.push_back({endBlock, condBlock});
        lowerScoped(*loop.body);
        loops_.pop_back();
        fallThrough(condBlock);
        if (!isConstant(*loop.condition))
            mustProgress(condBlock, entry);

        builder_.SetInsertPoint(endBlock);
    }

    // for (init; cond; step) body: init, then for.cond -> for.body -> for.inc (the step) -> for.cond, until
    // for.end. continue goes to for.inc, so the step still runs. The init's declarations are the loop's. for.inc's
    // branch back carries llvm.loop.mustprogress when there is a condition and it is not a constant expression.
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
            builder_.CreateCondBr(
                convert(lower(*loop.condition), ast::Type::Bool, loop.condition->location, Use::Condition),
                bodyBlock, endBlock);
        else
            builder_.CreateBr(bodyBlock);   // no condition: until a break or a return

        builder_.SetInsertPoint(bodyBlock);
        loops_.push_back({endBlock, incBlock});
        lowerScoped(*loop.body);
        loops_.pop_back();
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

    // The initialiser is lowered before the name is bound, so `float x = x;` reads an outer x. Without one,
    // the variable starts at 0. An array or a module starts at 0 too (a memset), and then takes its initialiser: an
    // array its list's elements, a module a copy of another.
    void Lowering::declareVariable(SourceLocation location, const ast::DeclStmt& decl)
    {
        ast::Type type = decl.type;
        if (type == ast::Type::Void)
        {
            error(location, "variable has incomplete type 'void'");
            type = ast::Type::Float;   // so its uses report no more errors
        }
        if (!checkType(location, type))   // likewise
            type = ast::isArray(type) ? ast::Type::arrayOf(ast::Type::Float, type.count)
                                      : ast::Type::pointerTo(ast::Kind::Float, type.pointeeConst);
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
                else
                    incompatible(location, Use::Initializing, value.type, type);
            }
            if (decl.hasList && ast::isArray(type))
                initializeArray(location, slot, type, decl.list);
            else if (decl.hasList)
                error(location, "a module takes no initializer list: its fields start at 0 (set them in a method, "
                                "such as init)");
            if (!bind(decl.name, Binding{slot, decl.isConst, type}))
                error(location, "redefinition of " + quoted(decl.name));
            return;
        }
        if (decl.hasList)
            error(location, "FMinus takes an initializer list for an array only");
        llvm::Value* value = decl.init ? convert(lower(*decl.init), type, location, Use::Initializing)
                                       : llvm::Constant::getNullValue(typeOf(type));
        llvm::AllocaInst* slot = allocate(typeOf(type), decl.name);
        builder_.CreateStore(value, slot);
        if (!bind(decl.name, Binding{slot, decl.isConst, type}))
            error(location, "redefinition of " + quoted(decl.name));
    }
} // namespace fminus::ir::detail
