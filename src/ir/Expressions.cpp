// src/ir/Expressions.cpp: expressions. Constants, variables, unary and binary operators (lane by lane on vectors), &&
// and || (short-circuiting), ?: (only the branch taken runs, unless the condition is a vector: Vectors.cpp), and
// assignment. Calls are Calls.cpp's, vector literals and swizzles Vectors.cpp's.
#include "ir/Lowering.h"

#include <llvm/ADT/APFloat.h>
#include <llvm/IR/Constants.h>
#include <llvm/Support/Error.h>

namespace fminus::ir::detail
{
    Lowering::Value Lowering::lower(const ast::Expr& expr)
    {
        return std::visit(
            Overloaded{
                [&](const ast::NumberExpr& number) {
                    return Value{lowerNumber(expr.location, number), ast::Type::Float};
                },
                [&](const ast::VariableExpr& variable) {
                    const Binding* binding = find(variable.name);
                    if (binding == nullptr)
                    {
                        error(expr.location, "use of undeclared identifier " + quoted(variable.name));
                        return poison(ast::Type::Float);
                    }
                    return Value{builder_.CreateLoad(binding->slot->getAllocatedType(), binding->slot,
                                                     variable.name),
                                 binding->type};
                },
                [&](const ast::BoolExpr& constant) {
                    return Value{builder_.getInt1(constant.value), ast::Type::Bool};
                },
                [&](const ast::UnaryExpr& unary) {
                    const Value operand = lower(*unary.operand);
                    const ast::Type type = ast::arithmeticOf(operand.type);   // a float or a float vector
                    switch (unary.op)
                    {
                    case ast::UnaryOp::Negate:
                    {
                        llvm::Value* value = convert(operand, type, expr.location, Use::Initializing);
                        return Value{builder_.CreateFNeg(value), type};
                    }
                    case ast::UnaryOp::Plus:
                        return Value{convert(operand, type, expr.location, Use::Initializing), type};
                    case ast::UnaryOp::Not:
                    {
                        llvm::Value* test = convert(operand, ast::Type::Bool, expr.location, Use::Logical);
                        return Value{builder_.CreateNot(test, "lnot"), ast::Type::Bool};
                    }
                    }
                    return poison(ast::Type::Float);
                },
                [&](const ast::BinaryExpr& binary) {
                    // C's usual arithmetic conversions, and OpenCL C's: a bool is 0 or 1, and a scalar
                    // meeting a vector is broadcast to it. Vector operators work lane by lane; a comparison of
                    // vectors is a lane mask.
                    const Value lhs = lower(*binary.lhs);
                    const Value rhs = lower(*binary.rhs);
                    ast::Type type = ast::Type::Float;
                    if (!commonType(expr.location, lhs.type, rhs.type, type))
                        return poison(ast::Type::Float);
                    llvm::Value* result =
                        arithmetic(binary.op, convert(lhs, type, expr.location, Use::Initializing),
                                   convert(rhs, type, expr.location, Use::Initializing));
                    return Value{result, isComparison(binary.op) ? ast::maskOf(type) : type};
                },
                [&](const ast::LogicalExpr& logical) {
                    return Value{lowerLogical(expr.location, logical), ast::Type::Bool};
                },
                [&](const ast::ConditionalExpr& conditional) {
                    return lowerConditional(expr.location, conditional);
                },
                [&](const ast::ConstructExpr& construct) { return lowerConstruct(expr.location, construct); },
                [&](const ast::SwizzleExpr& swizzle) { return lowerSwizzle(expr.location, swizzle); },
                [&](const ast::CallExpr& call) { return lowerCall(expr.location, call); },
                [&](const ast::AssignExpr& assign) { return lowerAssign(expr.location, assign); },
            },
            expr.node);
    }

    bool Lowering::isComparison(ast::BinaryOp op)
    {
        return op == ast::BinaryOp::Less || op == ast::BinaryOp::LessEqual || op == ast::BinaryOp::Greater
               || op == ast::BinaryOp::GreaterEqual || op == ast::BinaryOp::Equal
               || op == ast::BinaryOp::NotEqual;
    }

    llvm::Value* Lowering::arithmetic(ast::BinaryOp op, llvm::Value* lhs, llvm::Value* rhs)
    {
        switch (op)
        {
        case ast::BinaryOp::Add:
            return builder_.CreateFAdd(lhs, rhs);
        case ast::BinaryOp::Subtract:
            return builder_.CreateFSub(lhs, rhs);
        case ast::BinaryOp::Multiply:
            return builder_.CreateFMul(lhs, rhs);
        case ast::BinaryOp::Divide:
            return builder_.CreateFDiv(lhs, rhs);
        case ast::BinaryOp::Remainder:
            return builder_.CreateFRem(lhs, rhs);
        // Ordered comparisons: false when either side is NaN. != is unordered: true then.
        case ast::BinaryOp::Less:
            return builder_.CreateFCmpOLT(lhs, rhs, "cmp");
        case ast::BinaryOp::LessEqual:
            return builder_.CreateFCmpOLE(lhs, rhs, "cmp");
        case ast::BinaryOp::Greater:
            return builder_.CreateFCmpOGT(lhs, rhs, "cmp");
        case ast::BinaryOp::GreaterEqual:
            return builder_.CreateFCmpOGE(lhs, rhs, "cmp");
        case ast::BinaryOp::Equal:
            return builder_.CreateFCmpOEQ(lhs, rhs, "cmp");
        case ast::BinaryOp::NotEqual:
            return builder_.CreateFCmpUNE(lhs, rhs, "cmp");
        }
        return poison(ast::Type::Float).ir;
    }

    // a && b and a || b, as clang lowers them: b runs only when a does not decide (land.rhs, lor.rhs), and a
    // phi joins the two ways out.
    llvm::Value* Lowering::lowerLogical(SourceLocation location, const ast::LogicalExpr& logical)
    {
        const bool isAnd = logical.op == ast::LogicalOp::And;
        llvm::Value* lhs = convert(lower(*logical.lhs), ast::Type::Bool, location, Use::Logical);
        llvm::BasicBlock* lhsEnd = builder_.GetInsertBlock();
        llvm::Function* function = lhsEnd->getParent();
        llvm::LLVMContext& context = module_.getContext();
        llvm::BasicBlock* rhsBlock =
            llvm::BasicBlock::Create(context, isAnd ? "land.rhs" : "lor.rhs", function);
        llvm::BasicBlock* endBlock =
            llvm::BasicBlock::Create(context, isAnd ? "land.end" : "lor.end", function);
        if (isAnd)
            builder_.CreateCondBr(lhs, rhsBlock, endBlock);
        else
            builder_.CreateCondBr(lhs, endBlock, rhsBlock);

        builder_.SetInsertPoint(rhsBlock);
        llvm::Value* rhs = convert(lower(*logical.rhs), ast::Type::Bool, location, Use::Logical);
        llvm::BasicBlock* rhsEnd = builder_.GetInsertBlock();   // lowering b may have added blocks
        builder_.CreateBr(endBlock);

        builder_.SetInsertPoint(endBlock);
        llvm::PHINode* phi = builder_.CreatePHI(builder_.getInt1Ty(), 2, isAnd ? "land" : "lor");
        phi->addIncoming(builder_.getInt1(!isAnd), lhsEnd);   // && is false when a is; || is true when a is
        phi->addIncoming(rhs, rhsEnd);
        return phi;
    }

    // c ? a : b, as clang lowers it: only the branch taken runs, so it takes blocks and a phi (a select would
    // run both). SimplifyCFG turns a small one without side effects into a select at -O1 and above.
    Lowering::Value Lowering::lowerConditional(SourceLocation location, const ast::ConditionalExpr& conditional)
    {
        const Value test = lower(*conditional.condition);
        if (ast::isVector(test.type))
            return lowerSelect(location, test, conditional);
        llvm::Value* condition = convert(test, ast::Type::Bool, location, Use::Condition);
        llvm::Function* function = builder_.GetInsertBlock()->getParent();
        llvm::LLVMContext& context = module_.getContext();
        llvm::BasicBlock* trueBlock = llvm::BasicBlock::Create(context, "cond.true", function);
        llvm::BasicBlock* falseBlock = llvm::BasicBlock::Create(context, "cond.false", function);
        llvm::BasicBlock* endBlock = llvm::BasicBlock::Create(context, "cond.end", function);
        builder_.CreateCondBr(condition, trueBlock, falseBlock);

        // Each branch may end in another block than it started in (a nested ?:): the phi needs the last. Both
        // are lowered before their type is known, then converted to it at their ends.
        builder_.SetInsertPoint(trueBlock);
        const Value thenValue = lower(*conditional.thenValue);
        llvm::BasicBlock* thenEnd = builder_.GetInsertBlock();
        builder_.SetInsertPoint(falseBlock);
        const Value elseValue = lower(*conditional.elseValue);
        llvm::BasicBlock* elseEnd = builder_.GetInsertBlock();

        // Two of one type give that type (two bools give a bool); otherwise the usual arithmetic conversions
        // decide (a scalar and a vector give the vector).
        ast::Type type = thenValue.type;
        const bool meet = thenValue.type == elseValue.type
                          || commonType(location, thenValue.type, elseValue.type, type);
        if (!meet)
            type = ast::Type::Float;
        builder_.SetInsertPoint(thenEnd);
        llvm::Value* thenIR = meet ? convert(thenValue, type, location, Use::Initializing) : poison(type).ir;
        builder_.CreateBr(endBlock);
        builder_.SetInsertPoint(elseEnd);
        llvm::Value* elseIR = meet ? convert(elseValue, type, location, Use::Initializing) : poison(type).ir;
        builder_.CreateBr(endBlock);

        builder_.SetInsertPoint(endBlock);
        llvm::PHINode* phi = builder_.CreatePHI(typeOf(type), 2, "cond");
        phi->addIncoming(thenIR, thenEnd);
        phi->addIncoming(elseIR, elseEnd);
        return {phi, type};
    }

    // The constant rounded once, from its spelling straight to float (no detour through double). APFloat
    // reads decimal and hex floats alike; the f suffix is C's and goes.
    llvm::Value* Lowering::lowerNumber(SourceLocation location, const ast::NumberExpr& number)
    {
        std::string spelling = number.spelling;
        if (!spelling.empty() && (spelling.back() == 'f' || spelling.back() == 'F'))
            spelling.pop_back();
        llvm::APFloat value(llvm::APFloat::IEEEsingle());
        auto status = value.convertFromString(spelling, llvm::APFloat::rmNearestTiesToEven);
        if (!status)
        {
            llvm::consumeError(status.takeError());
            error(location, "invalid floating-point constant " + quoted(number.spelling));
            return poison(ast::Type::Float).ir;
        }
        if ((*status & llvm::APFloat::opOverflow) != 0)
            error(location, "magnitude of floating-point constant too large for type 'float'");
        return llvm::ConstantFP::get(module_.getContext(), value);
    }

    // `target op value`: the target must be an lvalue that is not const (Memory.cpp). The value of the expression
    // is the one stored.
    Lowering::Value Lowering::lowerAssign(SourceLocation location, const ast::AssignExpr& assign)
    {
        Value value = lower(*assign.value);
        if (!isLValue(*assign.target))
        {
            error(location, "expression is not assignable");
            return poison(ast::Type::Float);
        }
        const std::optional<LValue> target = lvalue(*assign.target, location);
        if (!target || !writable(location, *target))
            return poison(ast::Type::Float);
        if (assign.op != ast::AssignOp::Assign)
        {
            value = compound(location, assign.op, read(*target), value);
            if (value.ir == nullptr)
                return poison(target->type);
        }
        return {write(location, *target, value, Use::Assigning), target->type};
    }

    // What `current op= value` stores: the operator, in the type both convert to (a scalar broadcasts to a
    // vector). A null IR after an error.
    Lowering::Value Lowering::compound(SourceLocation location, ast::AssignOp op, Value current, Value value)
    {
        ast::Type common = ast::Type::Float;
        if (!commonType(location, current.type, value.type, common))
            return {nullptr, common};
        return {arithmetic(compoundOperator(op), convert(current, common, location, Use::Initializing),
                           convert(value, common, location, Use::Initializing)),
                common};
    }

    // The operator of a compound assignment: += is +, ...
    ast::BinaryOp Lowering::compoundOperator(ast::AssignOp op)
    {
        switch (op)
        {
        case ast::AssignOp::Assign:
        case ast::AssignOp::Add:
            return ast::BinaryOp::Add;
        case ast::AssignOp::Subtract:
            return ast::BinaryOp::Subtract;
        case ast::AssignOp::Multiply:
            return ast::BinaryOp::Multiply;
        case ast::AssignOp::Divide:
            return ast::BinaryOp::Divide;
        case ast::AssignOp::Remainder:
            return ast::BinaryOp::Remainder;
        }
        return ast::BinaryOp::Add;
    }
} // namespace fminus::ir::detail
