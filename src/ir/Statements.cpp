// src/ir/Statements.cpp: statements. Blocks, declarations and return, and C's control flow: if, while, for, break and
// continue, in blocks named as clang names them (if.then, while.cond, for.inc, ...).
#include "ir/Lowering.h"

#include <llvm/IR/Constants.h>

namespace fminus::ir::detail
{
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
                           else
                               builder_.CreateBr(loops_.back().breakTarget);
                       },
                       [&](const ast::ContinueStmt&) {
                           if (loops_.empty())
                               error(stmt.location, "'continue' statement not in loop statement");
                           else
                               builder_.CreateBr(loops_.back().continueTarget);
                       },
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
    // to while.end, continue to while.cond.
    void Lowering::lowerWhile(const ast::WhileStmt& loop)
    {
        llvm::BasicBlock* condBlock = newBlock("while.cond");
        llvm::BasicBlock* bodyBlock = newBlock("while.body");
        llvm::BasicBlock* endBlock = newBlock("while.end");
        builder_.CreateBr(condBlock);

        builder_.SetInsertPoint(condBlock);
        builder_.CreateCondBr(
            convert(lower(*loop.condition), ast::Type::Bool, loop.condition->location, Use::Condition),
            bodyBlock, endBlock);

        builder_.SetInsertPoint(bodyBlock);
        loops_.push_back({endBlock, condBlock});
        lowerScoped(*loop.body);
        loops_.pop_back();
        fallThrough(condBlock);

        builder_.SetInsertPoint(endBlock);
    }

    // for (init; cond; step) body: init, then for.cond -> for.body -> for.inc (the step) -> for.cond, until
    // for.end. continue goes to for.inc, so the step still runs. The init's declarations are the loop's.
    void Lowering::lowerFor(const ast::ForStmt& loop)
    {
        scopes_.emplace_back();
        lowerStatements(loop.init);
        llvm::BasicBlock* condBlock = newBlock("for.cond");
        llvm::BasicBlock* bodyBlock = newBlock("for.body");
        llvm::BasicBlock* incBlock = newBlock("for.inc");
        llvm::BasicBlock* endBlock = newBlock("for.end");
        builder_.CreateBr(condBlock);

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

        builder_.SetInsertPoint(endBlock);
        scopes_.pop_back();
    }

    // The initialiser is lowered before the name is bound, so `float x = x;` reads an outer x. Without one,
    // the variable starts at 0.
    void Lowering::declareVariable(SourceLocation location, const ast::DeclStmt& decl)
    {
        ast::Type type = decl.type;
        if (type == ast::Type::Void)
        {
            error(location, "variable has incomplete type 'void'");
            type = ast::Type::Float;   // so its uses report no more errors
        }
        if (!checkType(location, type))
            type = ast::Type::pointerTo(ast::Kind::Float, type.pointeeConst);   // likewise
        llvm::Value* value = decl.init ? convert(lower(*decl.init), type, location, Use::Initializing)
                                       : llvm::Constant::getNullValue(typeOf(type));
        llvm::AllocaInst* slot = allocate(typeOf(type), decl.name);
        builder_.CreateStore(value, slot);
        if (!bind(decl.name, Binding{slot, decl.isConst, type}))
            error(location, "redefinition of " + quoted(decl.name));
    }
} // namespace fminus::ir::detail
