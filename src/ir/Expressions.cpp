// src/ir/Expressions.cpp: expressions. Constants, variables, unary and binary operators (lane by lane on vectors; the
// bitwise ones on ints), && and || (short-circuiting), ?: (only the branch taken runs, unless the condition is a
// vector: Vectors.cpp), casts, assignment, ++ and --. Calls are Calls.cpp's, vector literals and swizzles
// Vectors.cpp's, methods and `this` Modules.cpp's, and what an assignment stores to Memory.cpp's.
#include "ir/Lowering.h"

#include <llvm/ADT/APFloat.h>
#include <llvm/IR/Constants.h>
#include <llvm/IR/GlobalVariable.h>
#include <llvm/Support/Error.h>

#include <charconv>
#include <cstdint>
#include <limits>
#include <optional>
#include <utility>
#include <variant>

namespace fminus::ir::detail
{
    // `expr` lowered, failed when it or a part of it reported an error (Value: what takes it then says nothing more).
    // Its arithmetic's floating constants are doubles when the arithmetic has a double (lowerAs): what the arithmetic
    // around it says (doubleArithmetic_, for this call only), or, at the top of its arithmetic, what hasDouble finds.
    Lowering::Value Lowering::lower(const ast::Expr& expr)
    {
        const std::size_t reported = diagnostics_.errors().size();
        const std::optional<bool> given = std::exchange(doubleArithmetic_, std::nullopt);
        const auto wide = [&] { return given ? *given : hasDouble(expr); };
        Value value = std::visit(
            Overloaded{
                [&](const ast::NumberExpr& number) { return lowerNumber(expr.location, number, wide()); },
                [&](const ast::VariableExpr& variable) {
                    if (const Binding* binding = named(variable.name))
                    {
                        if (binding->enumerator)   // Constants.cpp
                            return Value{builder_.getInt32(static_cast<std::uint32_t>(*binding->enumerator)),
                                         ast::Type::Int};
                        // A constant of the top level that is no array reads as its value, as clang reads a const
                        // object whose initialiser is a constant: what its memory holds, with no load.
                        auto* global = llvm::dyn_cast_if_present<llvm::GlobalVariable>(binding->slot);
                        if (global != nullptr && !ast::isArray(binding->type) && global->hasInitializer())
                            return Value{global->getInitializer(), binding->type};
                    }
                    return readLValue(expr);
                },
                [&](const ast::IndexExpr& index) {
                    if (const std::optional<unsigned> field = parameterOf(*index.base))
                        return lowerParameter(expr.location, *field, index);   // cutoff[i] (Modules.cpp)
                    return readLValue(expr);
                },
                [&](const ast::BoolExpr& constant) {
                    return Value{builder_.getInt1(constant.value), ast::Type::Bool};
                },
                [&](const ast::UnaryExpr& unary) {
                    if (unary.op == ast::UnaryOp::Dereference)
                        return readLValue(expr);   // *p: what p points to
                    if (unary.op == ast::UnaryOp::AddressOf)
                        return lowerAddressOf(expr.location, *unary.operand);
                    const bool sign = unary.op == ast::UnaryOp::Negate || unary.op == ast::UnaryOp::Plus;
                    const Value operand = sign ? lowerWith(*unary.operand, wide()) : lower(*unary.operand);
                    const bool complement = unary.op == ast::UnaryOp::Complement;
                    const ast::Type type = ast::arithmeticOf(operand.type);   // an int, a float, a float vector
                    if (operand.failed)   // its own error says why (Value): the type the operator would give
                        return recovery(complement                        ? ast::Type::Int
                                        : unary.op == ast::UnaryOp::Not   ? ast::Type::Bool
                                        : ast::isArithmetic(operand.type) ? type
                                                                          : ast::Type::Float);
                    const bool invalid = complement ? type != ast::Type::Int : !ast::isArithmetic(operand.type);
                    if (invalid && unary.op != ast::UnaryOp::Not)
                    {
                        error(expr.location,
                              "invalid argument type " + quoted(ast::typeName(operand.type)) + " to unary expression");
                        return poison(complement ? ast::Type::Int : ast::Type::Float);
                    }
                    switch (unary.op)
                    {
                    case ast::UnaryOp::Negate:
                    {
                        llvm::Value* value = convert(operand, type, expr.location, Use::Initializing);
                        if (type == ast::Type::Int)
                            return Value{builder_.CreateNSWNeg(value), type};   // 0 - x, as clang's
                        return Value{builder_.CreateFNeg(value), type};
                    }
                    case ast::UnaryOp::Plus:
                        return Value{convert(operand, type, expr.location, Use::Initializing), type};
                    case ast::UnaryOp::Not:
                    {
                        llvm::Value* test = convert(operand, ast::Type::Bool, expr.location, Use::Logical);
                        return Value{builder_.CreateNot(test, "lnot"), ast::Type::Bool};
                    }
                    case ast::UnaryOp::Complement:   // xor with -1, as clang's; a bool is an int first: ~true is -2
                    {
                        llvm::Value* value = convert(operand, type, expr.location, Use::Initializing);
                        return Value{builder_.CreateNot(value, "not"), type};
                    }
                    case ast::UnaryOp::Dereference:   // above
                    case ast::UnaryOp::AddressOf:
                        break;
                    }
                    return poison(ast::Type::Float);
                },
                [&](const ast::BinaryExpr& operation) {
                    // + - * / % pass their arithmetic's on; a comparison's two sides are one arithmetic of their own
                    // (d < 0.1 compares with C's 0.1); a bitwise operator's are each their own.
                    if (isComparison(operation.op) || isArithmetic(operation.op))
                    {
                        const bool doubles = isArithmetic(operation.op)
                                                 ? wide()
                                                 : hasDouble(*operation.lhs) || hasDouble(*operation.rhs);
                        const Value lhs = lowerWith(*operation.lhs, doubles);
                        const Value rhs = lowerWith(*operation.rhs, doubles);
                        return binary(expr.location, operation.op, lhs, rhs);
                    }
                    const Value lhs = lower(*operation.lhs);
                    const Value rhs = lower(*operation.rhs);
                    return binary(expr.location, operation.op, lhs, rhs);
                },
                [&](const ast::LogicalExpr& logical) {
                    return Value{lowerLogical(expr.location, logical), ast::Type::Bool};
                },
                [&](const ast::ConditionalExpr& conditional) {
                    return lowerConditional(expr.location, conditional, wide());
                },
                [&](const ast::ConstructExpr& construct) { return lowerConstruct(expr.location, construct); },
                [&](const ast::MemberExpr& member) {
                    return member.arrow ? readLValue(expr) : lowerMember(expr.location, member);
                },
                [&](const ast::MethodCallExpr& call) { return lowerMethodCall(expr.location, call); },
                [&](const ast::ThisExpr&) { return lowerThis(expr.location); },
                [&](const ast::CallExpr& call) { return lowerCall(expr.location, call); },
                [&](const ast::AssignExpr& assign) { return lowerAssign(expr.location, assign); },
                [&](const ast::IncrementExpr& increment) { return lowerIncrement(expr.location, increment); },
                [&](const ast::CastExpr& cast) { return lowerCast(expr.location, cast); },
                [&](const ast::SizeofExpr& size) { return lowerSizeof(expr.location, size); },
                [&](const ast::StringExpr&) { return notAValue(expr); },   // a builtin's argument only (Strings.cpp)
            },
            expr.node);
        value.failed = value.failed || diagnostics_.errors().size() != reported;
        return value;
    }

    // FMinus's floating constants are floats first: a constant without a suffix is a float, as FMinus has always had
    // it (`x * 0.5` of a float x is a float product), unless the arithmetic it is part of has a double. Its arithmetic
    // is the expression around it as far as + - * / %, unary + and -, the arms of ?: and parentheses reach: the leaves
    // are its operands, and what takes its value is where it goes. It has a double when an operand is one (hasDouble),
    // when the two sides of a comparison it is in have one, or when its value goes to a double: `type`, the type it is
    // wanted as (a declaration's or an assignment's target, a parameter, a result, a cast). Then each floating constant
    // in it without a suffix is the double nearest its spelling, and the arithmetic computes as C's does, every step of
    // it the same: `double t = x * 0.1;` is C's double product, and `double third = 1.0 / 3.0;` C's third, not a float's
    // widened. An arithmetic with no double computes as it always did: a program without doubles means the same, bit
    // for bit. lower(expr) is lowerAs(expr, void): its arithmetic has a double only when hasDouble finds one.
    Lowering::Value Lowering::lowerAs(const ast::Expr& expr, ast::Type type)
    {
        return lowerWith(expr, type == ast::Type::Double || hasDouble(expr));
    }

    // `expr`, a part of an arithmetic whose floating constants are doubles or not: `doubles`, which the next lower()
    // takes (doubleArithmetic_), and passes on to its own arithmetic's parts.
    Lowering::Value Lowering::lowerWith(const ast::Expr& expr, bool doubles)
    {
        doubleArithmetic_ = doubles;
        return lower(expr);
    }

    // + - * / %: the operators an arithmetic is made of (lowerAs).
    bool Lowering::isArithmetic(ast::BinaryOp op)
    {
        return op == ast::BinaryOp::Add || op == ast::BinaryOp::Subtract || op == ast::BinaryOp::Multiply
               || op == ast::BinaryOp::Divide || op == ast::BinaryOp::Remainder;
    }

    // Whether the arithmetic `expr` is (lowerAs) has an operand of type double: one of its leaves, as staticType knows
    // it before anything is lowered (a variable, a field, an element, a call's result, a cast, an assignment's value).
    // A constant is no double here, nor is a comparison, whose sides are an arithmetic of their own.
    bool Lowering::hasDouble(const ast::Expr& expr) const
    {
        if (std::holds_alternative<ast::NumberExpr>(expr.node))
            return false;
        if (const auto* unary = std::get_if<ast::UnaryExpr>(&expr.node);
            unary != nullptr && (unary->op == ast::UnaryOp::Negate || unary->op == ast::UnaryOp::Plus))
            return hasDouble(*unary->operand);
        if (const auto* binary = std::get_if<ast::BinaryExpr>(&expr.node); binary != nullptr && isArithmetic(binary->op))
            return hasDouble(*binary->lhs) || hasDouble(*binary->rhs);
        if (const auto* conditional = std::get_if<ast::ConditionalExpr>(&expr.node))
            return hasDouble(*conditional->thenValue) || hasDouble(*conditional->elseValue);
        return staticType(expr) == std::optional<ast::Type>(ast::Type::Double);
    }

    // `expr`'s type, worked out from the declarations alone, as lowering it would give it, or nothing where that takes
    // more: a variable's, a module's field's (m.y, p->y, a field by name in a method), an element's (a[i], *p, a
    // parameter's cutoff[i]), a call's result (a function's, a method's, a double builtin's), a cast's, an assignment's
    // or an increment's target's, a pointer's after p + i, and a ?:'s whose arms have one. What hasDouble asks of an arithmetic's operands, which it must know before they are
    // lowered: a constant beside them, lowered first, takes its type from what they are.
    std::optional<ast::Type> Lowering::staticType(const ast::Expr& expr) const
    {
        const ast::Module* module = function_ != nullptr ? function_->module : nullptr;
        // The module a value of `type` is, or a pointer of `type` points to.
        const auto moduleOf = [](std::optional<ast::Type> type, bool pointer) -> const ast::Module* {
            if (!type)
                return nullptr;
            if (pointer && ast::isPointer(*type) && type->pointee == ast::Kind::Module)
                return type->module;
            return !pointer && ast::isModule(*type) ? type->module : nullptr;
        };
        // A field's type, if `owner` has a field `name` that is no parameter.
        const auto fieldOf = [&](const ast::Module* owner, const std::string& name) -> std::optional<ast::Type> {
            const ast::Field* field = owner != nullptr ? findField(*owner, name, nullptr) : nullptr;
            if (field == nullptr || field->isParam)
                return std::nullopt;
            return fieldType(*field);
        };
        // A function's result, if the program declares one of the name.
        const auto resultOf = [&](const std::string& name) -> std::optional<ast::Type> {
            const auto found = functions_.find(name);
            return found != functions_.end() ? std::optional(found->second->returnType) : std::nullopt;
        };
        // What a pointer or an array of `type` reaches.
        const auto elementOf = [](std::optional<ast::Type> type) -> std::optional<ast::Type> {
            if (type && (ast::isPointer(*type) || ast::isArray(*type)))
                return ast::pointee(*type);
            return std::nullopt;
        };
        return std::visit(
            Overloaded{
                [&](const ast::VariableExpr& variable) -> std::optional<ast::Type> {
                    if (const Binding* binding = named(variable.name))
                        return binding->enumerator ? ast::Type::Int : binding->type;
                    return fieldOf(module, variable.name);
                },
                [&](const ast::CastExpr& cast) -> std::optional<ast::Type> { return cast.type; },
                [&](const ast::CallExpr& call) -> std::optional<ast::Type> {
                    if (module != nullptr && functions_.contains(module->name + "::" + call.callee))
                        return resultOf(module->name + "::" + call.callee);
                    if (const Builtin* builtin = findBuiltin(call.callee))
                        return builtin->type == ast::Kind::Double ? std::optional(ast::Type::Double) : std::nullopt;
                    return resultOf(call.callee);
                },
                [&](const ast::MethodCallExpr& call) -> std::optional<ast::Type> {
                    const ast::Module* owner = moduleOf(staticType(*call.object), call.arrow);
                    return owner != nullptr ? resultOf(owner->name + "::" + call.method) : std::nullopt;
                },
                [&](const ast::MemberExpr& member) -> std::optional<ast::Type> {
                    return fieldOf(moduleOf(staticType(*member.base), member.arrow), member.name);
                },
                [&](const ast::IndexExpr& index) -> std::optional<ast::Type> {
                    if (const std::optional<unsigned> field = parameterOf(*index.base))
                        return module->fields[*field].type;   // cutoff[i]
                    const std::optional<ast::Type> base = elementOf(staticType(*index.base));
                    return base ? base : elementOf(staticType(*index.index));   // C's i[p]
                },
                [&](const ast::UnaryExpr& unary) -> std::optional<ast::Type> {
                    if (unary.op == ast::UnaryOp::Dereference)
                        return elementOf(staticType(*unary.operand));
                    return std::nullopt;
                },
                [&](const ast::BinaryExpr& binary) -> std::optional<ast::Type> {
                    // p + i and p - i: the pointer's type, a base for p[i] and p->y
                    if (binary.op != ast::BinaryOp::Add && binary.op != ast::BinaryOp::Subtract)
                        return std::nullopt;
                    const std::optional<ast::Type> lhs = staticType(*binary.lhs);
                    if (lhs && ast::isPointer(*lhs))
                        return lhs;
                    const std::optional<ast::Type> rhs = staticType(*binary.rhs);
                    return binary.op == ast::BinaryOp::Add && rhs && ast::isPointer(*rhs) ? rhs : std::nullopt;
                },
                [&](const ast::ConditionalExpr& conditional) -> std::optional<ast::Type> {
                    // two arms of one type: (c ? p : q)->y
                    const std::optional<ast::Type> thenType = staticType(*conditional.thenValue);
                    return thenType == staticType(*conditional.elseValue) ? thenType : std::nullopt;
                },
                [&](const ast::AssignExpr& assign) { return staticType(*assign.target); },
                [&](const ast::IncrementExpr& increment) { return staticType(*increment.target); },
                [&](const ast::ThisExpr&) -> std::optional<ast::Type> {
                    if (module == nullptr)
                        return std::nullopt;
                    return ast::Type::pointerTo(ast::Type::moduleType(module), false);
                },
                [](const auto&) -> std::optional<ast::Type> { return std::nullopt; },
            },
            expr.node);
    }

    bool Lowering::isComparison(ast::BinaryOp op)
    {
        return op == ast::BinaryOp::Less || op == ast::BinaryOp::LessEqual || op == ast::BinaryOp::Greater
               || op == ast::BinaryOp::GreaterEqual || op == ast::BinaryOp::Equal
               || op == ast::BinaryOp::NotEqual;
    }

    bool Lowering::isBitwise(ast::BinaryOp op)
    {
        return op == ast::BinaryOp::BitAnd || op == ast::BinaryOp::BitOr || op == ast::BinaryOp::BitXor
               || op == ast::BinaryOp::ShiftLeft || op == ast::BinaryOp::ShiftRight;
    }

    // A binary operator, as C's usual arithmetic conversions and OpenCL C's say: the operands meet in their common
    // type (an int meeting a float is a float; a scalar meeting a vector is broadcast to it), the operator works
    // lane by lane on vectors, and a comparison gives a bool, or a lane mask for vectors. The bitwise operators take
    // ints (a bool is one), as C's take integers, and give an int; for a shift, C promotes each operand on its own,
    // which with int the only integer type is the same. An operand that reported an error of its own (an undeclared
    // name, say: it lowers to a poison float) fails (Value), and the operator then says nothing more, so that error is
    // the only one, as clang's: `p + y`, `y & 1` and `x |= y` give a recovery() of the type they would likely have had
    // (recovered), and report no "invalid operands". (A poison value alone is no test for this: (int)3e10f folds to
    // one with no error.)
    Lowering::Value Lowering::binary(SourceLocation location, ast::BinaryOp op, Value lhs, Value rhs)
    {
        if (lhs.failed || rhs.failed)
            return recovery(recovered(op, lhs, rhs));
        if (isBitwise(op) && (ast::arithmeticOf(lhs.type) != ast::Type::Int
                              || ast::arithmeticOf(rhs.type) != ast::Type::Int))
        {
            error(location, "invalid operands to binary expression (" + quoted(ast::typeName(lhs.type)) + " and "
                                + quoted(ast::typeName(rhs.type)) + ")");
            return poison(ast::Type::Int);
        }
        if (ast::isPointer(lhs.type) || ast::isPointer(rhs.type))
            return pointerArithmetic(location, op, lhs, rhs);   // Memory.cpp
        ast::Type type = ast::Type::Float;
        if (!commonType(location, lhs.type, rhs.type, type))
            return poison(ast::Type::Float);
        llvm::Value* result = arithmetic(op, type, convert(lhs, type, location, Use::Initializing),
                                         convert(rhs, type, location, Use::Initializing));
        return {result, isComparison(op) ? ast::maskOf(type) : type};
    }

    // The type `lhs op rhs` gives when an operand failed (Value): an int for a bitwise operator; a bool for a
    // comparison, or the lane mask of a vector operand that did not fail; otherwise the type of the operand that did
    // not fail, as it computes (a pointer stays one: p + y is a pointer), or a float. A guess, which keeps what uses
    // the result from reporting more.
    ast::Type Lowering::recovered(ast::BinaryOp op, const Value& lhs, const Value& rhs)
    {
        const Value* other = !lhs.failed ? &lhs : !rhs.failed ? &rhs : nullptr;
        if (isBitwise(op))
            return ast::Type::Int;
        if (isComparison(op))
            return other != nullptr && ast::isVector(other->type) ? ast::maskOf(ast::arithmeticOf(other->type))
                                                                  : ast::Type::Bool;
        if (other != nullptr && ast::isPointer(other->type))
            return other->type;
        if (other != nullptr && ast::isArithmetic(other->type))
            return ast::arithmeticOf(other->type);
        return ast::Type::Float;
    }

    // The operator's instruction in `type`. For int, C's: + - * carry nsw, as clang's do, since signed overflow is
    // undefined (it is what lets LLVM widen a loop counter to 64 bits); / and % truncate toward zero, and dividing by
    // 0 is undefined; the comparisons are signed. & | ^ are and, or and xor. A shift is defined for every operand, as
    // OpenCL C defines it (where C leaves a count below 0 or from 32 on, and a left shift of a negative value or into
    // the sign, undefined, and which LLVM would make poison): the count is its low five bits (n & 31, clang's
    // shl.mask, which the shift instructions of arm64 and x86-64 apply themselves, so it costs nothing on a scalar),
    // << is a shl without nsw or nuw, the bits shifted out lost, and >> is an ashr, the sign shifted in, as clang's
    // is (C makes a negative value's implementation-defined). For floats and float vectors, IEEE's.
    llvm::Value* Lowering::arithmetic(ast::BinaryOp op, ast::Type type, llvm::Value* lhs, llvm::Value* rhs)
    {
        if (type == ast::Type::Int)
        {
            switch (op)
            {
            case ast::BinaryOp::Add:
                return builder_.CreateNSWAdd(lhs, rhs);
            case ast::BinaryOp::Subtract:
                return builder_.CreateNSWSub(lhs, rhs);
            case ast::BinaryOp::Multiply:
                return builder_.CreateNSWMul(lhs, rhs);
            case ast::BinaryOp::Divide:
                return builder_.CreateSDiv(lhs, rhs);
            case ast::BinaryOp::Remainder:
                return builder_.CreateSRem(lhs, rhs);
            case ast::BinaryOp::Less:
                return builder_.CreateICmpSLT(lhs, rhs, "cmp");
            case ast::BinaryOp::LessEqual:
                return builder_.CreateICmpSLE(lhs, rhs, "cmp");
            case ast::BinaryOp::Greater:
                return builder_.CreateICmpSGT(lhs, rhs, "cmp");
            case ast::BinaryOp::GreaterEqual:
                return builder_.CreateICmpSGE(lhs, rhs, "cmp");
            case ast::BinaryOp::Equal:
                return builder_.CreateICmpEQ(lhs, rhs, "cmp");
            case ast::BinaryOp::NotEqual:
                return builder_.CreateICmpNE(lhs, rhs, "cmp");
            case ast::BinaryOp::BitAnd:
                return builder_.CreateAnd(lhs, rhs, "and");
            case ast::BinaryOp::BitOr:
                return builder_.CreateOr(lhs, rhs, "or");
            case ast::BinaryOp::BitXor:
                return builder_.CreateXor(lhs, rhs, "xor");
            case ast::BinaryOp::ShiftLeft:
                return builder_.CreateShl(lhs, builder_.CreateAnd(rhs, 31, "shl.mask"), "shl");
            case ast::BinaryOp::ShiftRight:
                return builder_.CreateAShr(lhs, builder_.CreateAnd(rhs, 31, "shr.mask"), "shr");
            }
        }
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
        case ast::BinaryOp::BitAnd:   // ints only: binary() refused the operands
        case ast::BinaryOp::BitOr:
        case ast::BinaryOp::BitXor:
        case ast::BinaryOp::ShiftLeft:
        case ast::BinaryOp::ShiftRight:
            break;
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
        // A top-level constant's initialiser runs nowhere: a constant a is the value it decides (Constants.cpp).
        if (const auto* decided = llvm::dyn_cast<llvm::ConstantInt>(lhs); decided != nullptr && atTopLevel())
            return decided->isOne() == isAnd ? rhs : builder_.getInt1(!isAnd);
        return phi;
    }

    // c ? a : b, as clang lowers it: only the branch taken runs, so it takes blocks and a phi (a select would
    // run both). SimplifyCFG turns a small one without side effects into a select at -O1 and above. Its arms are part
    // of the arithmetic around it, whose floating constants are doubles or not (`doubles`, lowerAs); its condition is
    // an arithmetic of its own.
    Lowering::Value Lowering::lowerConditional(SourceLocation location, const ast::ConditionalExpr& conditional,
                                               bool doubles)
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
        const Value thenValue = lowerWith(*conditional.thenValue, doubles);
        llvm::BasicBlock* thenEnd = builder_.GetInsertBlock();
        builder_.SetInsertPoint(falseBlock);
        const Value elseValue = lowerWith(*conditional.elseValue, doubles);
        llvm::BasicBlock* elseEnd = builder_.GetInsertBlock();
        if (thenValue.failed || elseValue.failed)
        {
            // An arm's own error says why (Value): a recovery() of the other arm's type, which nothing reports on.
            const Value& other = thenValue.failed ? elseValue : thenValue;
            const bool usable = !other.failed && !ast::isModule(other.type) && other.type != ast::Type::Void;
            builder_.SetInsertPoint(thenEnd);
            builder_.CreateBr(endBlock);
            builder_.SetInsertPoint(elseEnd);
            builder_.CreateBr(endBlock);
            builder_.SetInsertPoint(endBlock);
            return recovery(usable ? other.type : ast::Type::Float);
        }

        // Two of one type give that type (two bools give a bool, two voids nothing); otherwise the usual arithmetic
        // conversions decide (a scalar and a vector give the vector). A void meets only a void, and a pointer only
        // a pointer to the same type (const if either is).
        const bool thenVoid = thenValue.type == ast::Type::Void;
        const bool elseVoid = elseValue.type == ast::Type::Void;
        if (ast::isModule(thenValue.type) || ast::isModule(elseValue.type))
        {
            // A module's value is its address, and ?: would have to copy one: choose between pointers instead.
            error(location, "FMinus's ?: does not choose between modules: choose between pointers, c ? &a : &b");
            builder_.SetInsertPoint(thenEnd);
            builder_.CreateBr(endBlock);
            builder_.SetInsertPoint(elseEnd);
            builder_.CreateBr(endBlock);
            builder_.SetInsertPoint(endBlock);
            return poison(ast::isModule(thenValue.type) ? thenValue.type : elseValue.type);   // its fields still read
        }
        const bool pointers = ast::isPointer(thenValue.type) || ast::isPointer(elseValue.type);
        ast::Type type = thenValue.type;
        bool meet = thenValue.type == elseValue.type;
        if (thenVoid != elseVoid)
            error(location, std::string(thenVoid ? "left" : "right") + " operand to ? is void, but "
                                + (thenVoid ? "right" : "left") + " operand is of type "
                                + quoted(ast::typeName(thenVoid ? elseValue.type : thenValue.type)));
        else if (pointers && !meet)
        {
            // Two pointers to one type, or one a void*, which the other converts to (C's); const if either is.
            const bool both = ast::isPointer(thenValue.type) && ast::isPointer(elseValue.type);
            const bool same = both && ast::samePointee(thenValue.type, elseValue.type);
            meet = same || (both && (thenValue.type.pointee == ast::Kind::Void
                                     || elseValue.type.pointee == ast::Kind::Void));
            if (meet)
                type = ast::Type::pointerTo(same ? ast::pointee(thenValue.type) : ast::Type::Void,
                                            thenValue.type.pointeeConst || elseValue.type.pointeeConst);
            else
                error(location, "incompatible operand types (" + quoted(ast::typeName(thenValue.type)) + " and "
                                    + quoted(ast::typeName(elseValue.type)) + ")");
        }
        else if (!meet && !thenVoid)
            meet = commonType(location, thenValue.type, elseValue.type, type);
        if (!meet)
            type = ast::Type::Float;
        if (!ast::isPointer(type))
            type.enumeration = 0;   // two of an enumerated type give its integer type, as C's conversions do
        builder_.SetInsertPoint(thenEnd);
        llvm::Value* thenIR = meet ? convert(thenValue, type, location, Use::Initializing) : poison(type).ir;
        builder_.CreateBr(endBlock);
        builder_.SetInsertPoint(elseEnd);
        llvm::Value* elseIR = meet ? convert(elseValue, type, location, Use::Initializing) : poison(type).ir;
        builder_.CreateBr(endBlock);

        builder_.SetInsertPoint(endBlock);
        if (type == ast::Type::Void)
            return {nullptr, type};   // nothing to join, and no value, as a void call has none: its uses are errors
        llvm::PHINode* phi = builder_.CreatePHI(typeOf(type), 2, "cond");
        phi->addIncoming(thenIR, thenEnd);
        phi->addIncoming(elseIR, elseEnd);
        // A top-level constant's initialiser runs nowhere: a constant condition is the arm it picks (Constants.cpp).
        if (const auto* picked = llvm::dyn_cast<llvm::ConstantInt>(condition); picked != nullptr && atTopLevel())
            return {picked->isOne() ? thenIR : elseIR, type};
        return {phi, type};
    }

    // A constant, from its spelling. One with no point and no exponent is an integer constant, an int, as in C
    // (lowerInteger). Any other is a float, rounded once, from its spelling straight to float (no detour through
    // double): APFloat reads decimal and hex floats alike, and the f suffix is C's and goes. One without the suffix
    // in an arithmetic that has a double (`doubles`, lowerAs) is a double instead, C's, rounded once from its spelling
    // to double.
    Lowering::Value Lowering::lowerNumber(SourceLocation location, const ast::NumberExpr& number, bool doubles)
    {
        const bool hex = number.spelling.size() > 1 && number.spelling[0] == '0'
                         && (number.spelling[1] == 'x' || number.spelling[1] == 'X');
        if (number.spelling.find_first_of(hex ? ".pP" : ".eEfF") == std::string::npos)
            return {lowerInteger(location, number.spelling, hex), ast::Type::Int};

        std::string spelling = number.spelling;
        const bool suffixed = !spelling.empty() && (spelling.back() == 'f' || spelling.back() == 'F');
        if (suffixed)
            spelling.pop_back();
        const ast::Type type = doubles && !suffixed ? ast::Type::Double : ast::Type::Float;
        llvm::APFloat value(type == ast::Type::Double ? llvm::APFloat::IEEEdouble() : llvm::APFloat::IEEEsingle());
        auto status = value.convertFromString(spelling, llvm::APFloat::rmNearestTiesToEven);
        if (!status)
        {
            llvm::consumeError(status.takeError());
            error(location, "invalid floating-point constant " + quoted(number.spelling));
            return poison(type);
        }
        if ((*status & llvm::APFloat::opOverflow) != 0)
            error(location, "magnitude of floating-point constant too large for type " + quoted(ast::typeName(type)));
        return {llvm::ConstantFP::get(module_.getContext(), value), type};
    }

    // An integer constant: hex after 0x, octal after a leading 0 (010 is 8), decimal otherwise. It must fit in an
    // int: C would make a larger one a long, which FMinus does not have.
    llvm::Value* Lowering::lowerInteger(SourceLocation location, const std::string& spelling, bool hex)
    {
        const int base = hex ? 16 : spelling.size() > 1 && spelling[0] == '0' ? 8 : 10;
        const char* first = spelling.data() + (hex ? 2 : 0);
        const char* last = spelling.data() + spelling.size();
        std::uint64_t value = 0;
        const auto [end, failure] = std::from_chars(first, last, value, base);
        if (end != last)   // only an octal constant can hold a digit its base does not have: an 8 or a 9
        {
            error(location, "invalid digit " + quoted(std::string(1, *end)) + " in octal constant");
            return poison(ast::Type::Int).ir;
        }
        if (failure == std::errc::result_out_of_range || value > std::numeric_limits<std::int32_t>::max())
        {
            error(location, "integer constant is too large for type 'int'");
            return poison(ast::Type::Int).ir;
        }
        return builder_.getInt32(static_cast<std::uint32_t>(value));
    }

    // sizeof(type): the type's size in bytes (Types.cpp), an int constant; a module's, padding included, is what to
    // allocate for one, and an array's, sizeof(float[4][16]), its elements' (its sizes worked out as a declaration's:
    // sized, Constants.cpp; `[]` has none). C's sizeof is a size_t, and FMinus's an int: a size past the greatest int (a
    // module of an array of 2^30 floats, 4 GB) is an error, not a wrapped or negative int.
    Lowering::Value Lowering::lowerSizeof(SourceLocation location, const ast::SizeofExpr& size)
    {
        ast::Type type = size.type;
        if (type == ast::Type::Void)
        {
            error(location, "invalid application of 'sizeof' to an incomplete type 'void'");
            return poison(ast::Type::Int);
        }
        if (ast::hasRows(type) && size.rowSize == nullptr)   // C has no array of float[] (sized's error)
        {
            error(location, "array has incomplete element type " + quoted(ast::typeName(ast::elementOf(type)) + "[]"));
            return poison(ast::Type::Int);
        }
        if (ast::isArray(type) && size.size == nullptr)   // float[], float[][16]
        {
            std::string rows;
            if (ast::hasRows(type))
            {
                const std::optional<std::int32_t> count = arraySize("", *size.rowSize);
                if (!count)
                    return poison(ast::Type::Int);
                rows = "[" + std::to_string(*count) + "]";
            }
            error(location, "invalid application of 'sizeof' to an incomplete type "
                                + quoted(ast::typeName(ast::elementOf(type)) + "[]" + rows));
            return poison(ast::Type::Int);
        }
        const std::size_t reported = diagnostics_.errors().size();
        if (ast::isArray(type))
            type = sized(location, "", type, size.size.get(), size.rowSize.get(), nullptr);
        if (diagnostics_.errors().size() != reported || !checkType(location, type))
            return poison(ast::Type::Int);
        const ast::Type module = ast::isArray(type) ? ast::elementOf(type) : type;
        if (ast::isModule(module) && structs_.at(module.module)->isOpaque())   // in its own fields' sizes
        {
            error(location,
                  "invalid application of 'sizeof' to an incomplete type " + quoted(ast::typeName(type)));
            return poison(ast::Type::Int);
        }
        const std::uint64_t bytes = *sizeOf(type);
        if (bytes > static_cast<std::uint64_t>(std::numeric_limits<std::int32_t>::max()))
        {
            error(location, quoted(ast::typeName(type)) + " is too large for sizeof: " + std::to_string(bytes)
                                + " bytes, and FMinus's sizeof is an 'int'");
            return poison(ast::Type::Int);
        }
        return {builder_.getInt32(static_cast<std::uint32_t>(bytes)), ast::Type::Int};
    }

    // (float)x, (double)x, (int)x, (bool)x: C's casts between scalars. Each is the implicit conversion, written out;
    // (double)0.1 is C's double 0.1 (lowerAs).
    Lowering::Value Lowering::lowerCast(SourceLocation location, const ast::CastExpr& cast)
    {
        return {convert(lowerAs(*cast.operand, cast.type), cast.type, location, Use::Casting), cast.type};
    }

    // `target op value`: the target must be an lvalue that is not const (Memory.cpp). The value of the expression
    // is the one stored. The value goes to the target, an operand of the operator too: `d *= 0.999` of a double d
    // multiplies by C's 0.999 (lowerAs, with the target's type as staticType knows it).
    Lowering::Value Lowering::lowerAssign(SourceLocation location, const ast::AssignExpr& assign)
    {
        const std::optional<ast::Type> targetType = staticType(*assign.target);
        Value value = isString(*assign.value) ? refuseString(*assign.value, targetType, Use::Assigning, location)
                                              : lowerAs(*assign.value, targetType.value_or(ast::Type::Void));
        if (!isLValue(*assign.target))
        {
            error(location, "expression is not assignable");
            return poison(ast::Type::Float);
        }
        const std::optional<LValue> target = lvalue(*assign.target, location);
        if (!target || !writable(location, *target))
            return recovery(ast::Type::Float);   // lvalue()'s error, or writable()'s
        if (assign.op != ast::AssignOp::Assign)
        {
            if (value.failed)
                return recovery(target->type);   // the value's own error says why (Value)
            const std::size_t reported = diagnostics_.errors().size();
            value = compound(location, assign.op, read(*target), value);
            if (value.ir == nullptr || diagnostics_.errors().size() != reported)
                return poison(target->type);   // the operator's error says why; storing would only add another
        }
        return {write(location, *target, value, Use::Assigning), target->type};
    }

    // What `current op= value` stores, before it converts to the target's type: the operator's result, as
    // binary() gives it (i *= 0.5f is a float, then truncated to the int i).
    Lowering::Value Lowering::compound(SourceLocation location, ast::AssignOp op, Value current, Value value)
    {
        return binary(location, compoundOperator(op), current, value);
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
        case ast::AssignOp::BitAnd:
            return ast::BinaryOp::BitAnd;
        case ast::AssignOp::BitOr:
            return ast::BinaryOp::BitOr;
        case ast::AssignOp::BitXor:
            return ast::BinaryOp::BitXor;
        case ast::AssignOp::ShiftLeft:
            return ast::BinaryOp::ShiftLeft;
        case ast::AssignOp::ShiftRight:
            return ast::BinaryOp::ShiftRight;
        }
        return ast::BinaryOp::Add;
    }

    // ++x and --x are x += 1 and x -= 1, and so is x++ (x--), whose value is the target's before. The target is
    // any lvalue that is not const: an int, a float, a vector (every lane), or a bool, which ++ makes true, as in C.
    Lowering::Value Lowering::lowerIncrement(SourceLocation location, const ast::IncrementExpr& increment)
    {
        const bool decrement = increment.op == ast::IncrementOp::PreDecrement
                               || increment.op == ast::IncrementOp::PostDecrement;
        const bool prefix = increment.op == ast::IncrementOp::PreIncrement
                            || increment.op == ast::IncrementOp::PreDecrement;
        if (!isLValue(*increment.target))
        {
            error(location, "expression is not assignable");
            return poison(ast::Type::Float);
        }
        const std::optional<LValue> target = lvalue(*increment.target, location);
        if (!target || !writable(location, *target))
            return recovery(ast::Type::Float);   // lvalue()'s error, or writable()'s
        const Value before = read(*target);
        const Value one{builder_.getInt32(1), ast::Type::Int};
        const Value after =
            binary(location, decrement ? ast::BinaryOp::Subtract : ast::BinaryOp::Add, before, one);
        llvm::Value* stored = write(location, *target, after, Use::Assigning);
        return prefix ? Value{stored, target->type} : before;
    }
} // namespace fminus::ir::detail
