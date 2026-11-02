// src/ir/Constants.cpp: C's integer constant expressions (C11 6.6p6), worked out where they are written, as a C
// compiler works them out: a case label's value, an enumerator's. Their operands are integer constants, enumeration
// constants, `true` and `false`, sizeof, and floating constants only as the immediate operands of casts (`(int)2.5f`),
// combined by the unary operators but * and &, the binary ones, &&, || and ?:, and casts to int or bool; their type is
// int or bool. A variable is none, const or not, as in C (isConstant, Statements.cpp, says the same of a loop's
// condition).
//
// Enumerations (declareEnum) are C's: each constant an int, named from just after its own definition on, in the scope
// the enumeration is declared in. In a block, that is the block's scope, where variables are too: C gives the two one
// name space, so a variable of an inner block hides a constant, and one of the same scope is a redefinition. At the
// top level, and in a module (as a C struct's), it is the file's, fileScope_, which the functions and modules after
// the enumeration see, and a module's methods (CodeGen.cpp declares each in its place among the definitions). A name is
// looked up as C++ looks it up in a member function (enumerator()): the blocks' scopes, then the module's fields and
// methods, then the file's.
//
// Each operator computes as FMinus computes it as the program runs (Expressions.cpp), so a constant means what the same
// expression means anywhere: a shift by its count's low five bits, / and % truncating toward zero. Where C makes the
// value an error, it is one: an int that overflows (6.6p4: a constant expression's value is in its type's range;
// clang warns, "overflow in expression"), and a division by zero or the least int by -1, after which the expression is
// no constant at all, as clang finds. Those two are errors only where the expression is evaluated: `0 && 1 / 0` is the
// constant 0, as in C (6.6p3), and so is the arm of a ?: not taken. What is no constant is reported wherever it is.
#include "ir/Lowering.h"

#include <llvm/ADT/APFloat.h>
#include <llvm/ADT/APSInt.h>
#include <llvm/IR/Constants.h>

#include <algorithm>
#include <cstdint>
#include <limits>

namespace fminus::ir::detail
{
    namespace
    {
        constexpr std::int64_t kLeast = std::numeric_limits<std::int32_t>::min();
        constexpr std::int64_t kGreatest = std::numeric_limits<std::int32_t>::max();

        // An int's 32 bits, as two's complement wraps `value`.
        std::int32_t wrapped(std::int64_t value)
        {
            return static_cast<std::int32_t>(static_cast<std::uint32_t>(static_cast<std::uint64_t>(value)));
        }
    } // namespace

    // The value of the enumeration constant `name` names where it is used, or nothing for a name that is not one: a
    // block's constant, unless a variable of an inner block hides it; then, in a method, nothing if the module has a
    // field or a method of the name; then the file's.
    std::optional<std::int32_t> Lowering::enumerator(const std::string& name) const
    {
        if (const Binding* binding = lookup(name))
            return binding->enumerator;
        const ast::Module* module = function_ != nullptr ? function_->module : nullptr;
        if (module != nullptr
            && (findField(*module, name, nullptr) != nullptr
                || std::ranges::find(module->methods, name) != module->methods.end()))
            return std::nullopt;
        const auto found = fileScope_.find(name);
        return found != fileScope_.end() ? found->second.enumerator : std::nullopt;
    }

    // An enumeration, its constants in order: each one's value its own, or the one before it plus 1 (the first's 0),
    // bound from there on in the innermost scope, or the file's at the top level and in a module (scopes_ is empty
    // then). A value in error makes its constant the next one's after all, as clang counts it, so the rest still count
    // and its uses report nothing more. Clang's errors: a constant redefined, one past the greatest int, and, at the
    // top level, one named as a function or a module.
    void Lowering::declareEnum(const ast::Enum& enumeration)
    {
        const bool file = scopes_.empty();
        std::unordered_map<std::string, Binding>& scope = file ? fileScope_ : scopes_.back();
        std::int64_t next = 0;
        for (const ast::Enumerator& constant : enumeration.enumerators)
        {
            std::optional<std::int32_t> value = constant.value ? integerConstant(*constant.value) : std::nullopt;
            if (!constant.value && next > kGreatest)
                error(constant.location, "ISO C restricts enumerator values to range of 'int' (" + std::to_string(next)
                                             + " is too large)");
            const std::int32_t bound = value ? *value : wrapped(next);
            next = static_cast<std::int64_t>(bound) + 1;
            const auto [previous, added] =
                scope.try_emplace(constant.name, Binding{nullptr, true, ast::Type::Int, constant.location, bound});
            if (!added)
            {
                redefinition(constant.location, constant.name, true, previous->second);
                continue;
            }
            if (!file)
                continue;
            // C's file scope holds the functions' names too, and a module's name is a type's (a typedef's, to C).
            const std::vector<Diagnostic> here{Diagnostic{constant.location, "previous definition is here", {}}};
            for (std::size_t f = 0; f < program_->functions.size(); ++f)
            {
                const ast::Function& function = program_->functions[f];
                if (function.name != constant.name)
                    continue;
                if (f < enumeration.position)
                    redefinition(constant.location, constant.name, true, Binding{nullptr, false, ast::Type::Void,
                                                                                 function.location});
                else
                    diagnostics_.error(function.location,
                                       "redefinition of " + quoted(function.name) + " as different kind of symbol",
                                       here);
                break;
            }
            for (const std::unique_ptr<ast::Module>& module : program_->modules)
            {
                if (module->name == constant.name)   // declared after: before, its name is no identifier
                    diagnostics_.error(module->location,
                                       "redefinition of " + quoted(module->name) + " as different kind of symbol",
                                       here);
            }
        }
    }

    // The value of the integer constant expression `expr`, or nothing after an error that says why, in clang's words.
    std::optional<std::int32_t> Lowering::integerConstant(const ast::Expr& expr)
    {
        const std::optional<Folded> folded = fold(expr, true);
        if (!folded)
            return std::nullopt;
        if (folded->type == ast::Type::Float)
        {
            error(expr.location, "integer constant expression must have integer type, not 'float'");
            return std::nullopt;
        }
        if (folded->floating)   // an int that a floating constant took part in, as 1.5f < 2 or !1.5f
        {
            notConstant(*folded->floating);
            return std::nullopt;
        }
        return folded->value;
    }

    // "expression is not an integer constant expression", at `location`: what a constant expression cannot hold.
    std::optional<Lowering::Folded> Lowering::notConstant(SourceLocation location)
    {
        error(location, "expression is not an integer constant expression");
        return std::nullopt;
    }

    // `expr`'s value as a constant expression, or nothing after its error; `evaluated` is false in the operand of &&,
    // || or ?: that C does not evaluate, where an overflow or a division by zero is no error.
    std::optional<Lowering::Folded> Lowering::fold(const ast::Expr& expr, bool evaluated)
    {
        // An int operator's exact result, `wide`: the int, or an error when it is out of an int's range.
        const auto checked = [&](SourceLocation location, std::int64_t wide) -> std::optional<Folded> {
            if (evaluated && (wide < kLeast || wide > kGreatest))
            {
                error(location, "overflow in expression; result is " + std::to_string(wrapped(wide))
                                    + " with type 'int'");
                return std::nullopt;
            }
            return Folded{wrapped(wide), ast::Type::Int, std::nullopt};
        };
        // A constant that lowers to no instruction (a number, sizeof): its value, or nothing after its own error.
        const auto lowered = [&](auto lowerIt) -> std::optional<Value> {
            const std::size_t reported = diagnostics_.errors().size();
            const Value value = lowerIt();
            if (diagnostics_.errors().size() != reported)
                return std::nullopt;
            return value;
        };
        return std::visit(
            Overloaded{
                [&](const ast::NumberExpr& number) -> std::optional<Folded> {
                    const std::optional<Value> value = lowered([&] { return lowerNumber(expr.location, number); });
                    if (!value)
                        return std::nullopt;
                    if (value->type == ast::Type::Float)
                        return Folded{0, ast::Type::Float, expr.location};
                    return Folded{static_cast<std::int32_t>(llvm::cast<llvm::ConstantInt>(value->ir)->getSExtValue()),
                                  ast::Type::Int, std::nullopt};
                },
                [&](const ast::BoolExpr& constant) -> std::optional<Folded> {
                    return Folded{constant.value ? 1 : 0, ast::Type::Bool, std::nullopt};
                },
                [&](const ast::SizeofExpr& size) -> std::optional<Folded> {
                    const std::optional<Value> value = lowered([&] { return lowerSizeof(expr.location, size); });
                    if (!value)
                        return std::nullopt;
                    return Folded{static_cast<std::int32_t>(llvm::cast<llvm::ConstantInt>(value->ir)->getSExtValue()),
                                  ast::Type::Int, std::nullopt};
                },
                [&](const ast::VariableExpr& variable) -> std::optional<Folded> {
                    // An enumeration constant is one; a variable, a parameter or a field (in a method) is none; any
                    // other name is undeclared.
                    if (const std::optional<std::int32_t> value = enumerator(variable.name))
                        return Folded{*value, ast::Type::Int, std::nullopt};
                    const ast::Module* module = function_ != nullptr ? function_->module : nullptr;
                    if (lookup(variable.name) != nullptr
                        || (module != nullptr
                            && (findField(*module, variable.name, nullptr) != nullptr
                                || std::ranges::find(module->methods, variable.name) != module->methods.end())))
                        return notConstant(expr.location);
                    error(expr.location, "use of undeclared identifier " + quoted(variable.name));
                    return std::nullopt;
                },
                [&](const ast::UnaryExpr& unary) -> std::optional<Folded> {
                    if (unary.op == ast::UnaryOp::Dereference || unary.op == ast::UnaryOp::AddressOf)
                        return notConstant(expr.location);
                    const std::optional<Folded> operand = fold(*unary.operand, evaluated);
                    if (!operand)
                        return std::nullopt;
                    if (operand->type == ast::Type::Float && unary.op == ast::UnaryOp::Complement)
                    {
                        error(expr.location, "invalid argument type 'float' to unary expression");
                        return std::nullopt;
                    }
                    const ast::Type type = unary.op == ast::UnaryOp::Not ? ast::Type::Bool
                                           : operand->type == ast::Type::Float ? ast::Type::Float
                                                                               : ast::Type::Int;
                    if (operand->floating)
                        return Folded{0, type, operand->floating};
                    const std::int64_t x = operand->value;   // a bool's is 0 or 1, an int's own
                    switch (unary.op)
                    {
                    case ast::UnaryOp::Negate:
                        return checked(expr.location, -x);
                    case ast::UnaryOp::Plus:
                        return Folded{wrapped(x), type, std::nullopt};
                    case ast::UnaryOp::Not:
                        return Folded{x == 0 ? 1 : 0, type, std::nullopt};
                    case ast::UnaryOp::Complement:
                        return Folded{~wrapped(x), type, std::nullopt};
                    case ast::UnaryOp::Dereference:   // above
                    case ast::UnaryOp::AddressOf:
                        break;
                    }
                    return std::nullopt;
                },
                [&](const ast::BinaryExpr& binary) -> std::optional<Folded> {
                    const std::optional<Folded> lhs = fold(*binary.lhs, evaluated);
                    const std::optional<Folded> rhs = fold(*binary.rhs, evaluated);
                    if (!lhs || !rhs)
                        return std::nullopt;
                    return foldBinary(expr.location, binary.op, *lhs, *rhs, evaluated);
                },
                [&](const ast::LogicalExpr& logical) -> std::optional<Folded> {
                    // C's: the right operand is not evaluated when the left decides, && when it is 0, || when not
                    const bool isAnd = logical.op == ast::LogicalOp::And;
                    const std::optional<Folded> lhs = fold(*logical.lhs, evaluated);
                    const bool decides = lhs && !lhs->floating && (lhs->value != 0) != isAnd;
                    const std::optional<Folded> rhs = fold(*logical.rhs, evaluated && lhs && !decides);
                    if (!lhs || !rhs)
                        return std::nullopt;
                    if (lhs->floating || rhs->floating)
                        return Folded{0, ast::Type::Bool, lhs->floating ? lhs->floating : rhs->floating};
                    if (decides)
                        return Folded{isAnd ? 0 : 1, ast::Type::Bool, std::nullopt};
                    return Folded{rhs->value != 0 ? 1 : 0, ast::Type::Bool, std::nullopt};
                },
                [&](const ast::ConditionalExpr& conditional) -> std::optional<Folded> {
                    // C's: only the arm the condition picks is evaluated
                    const std::optional<Folded> condition = fold(*conditional.condition, evaluated);
                    const bool known = condition && !condition->floating;
                    const bool truth = known && condition->value != 0;
                    const std::optional<Folded> thenValue = fold(*conditional.thenValue, evaluated && known && truth);
                    const std::optional<Folded> elseValue =
                        fold(*conditional.elseValue, evaluated && known && !truth);
                    if (!condition || !thenValue || !elseValue)
                        return std::nullopt;
                    // C's usual arithmetic conversions: two bools give a bool, a float anywhere a float
                    const ast::Type type =
                        thenValue->type == ast::Type::Float || elseValue->type == ast::Type::Float ? ast::Type::Float
                        : thenValue->type == ast::Type::Bool && elseValue->type == ast::Type::Bool ? ast::Type::Bool
                                                                                                   : ast::Type::Int;
                    for (const std::optional<SourceLocation>& floating :
                         {condition->floating, thenValue->floating, elseValue->floating})
                    {
                        if (floating)
                            return Folded{0, type, floating};
                    }
                    return Folded{truth ? thenValue->value : elseValue->value, type, std::nullopt};
                },
                [&](const ast::CastExpr& cast) -> std::optional<Folded> {
                    if (cast.type == ast::Type::Float)
                    {
                        const std::optional<Folded> operand = fold(*cast.operand, evaluated);
                        if (!operand)
                            return std::nullopt;
                        return Folded{0, ast::Type::Float, operand->floating ? operand->floating : expr.location};
                    }
                    // A floating constant as the cast's own operand is C's one place for one: (int) truncates toward
                    // zero, and a value out of an int's range makes no constant; (bool) is != 0.
                    if (const auto* number = std::get_if<ast::NumberExpr>(&cast.operand->node))
                    {
                        const std::optional<Value> value =
                            lowered([&] { return lowerNumber(cast.operand->location, *number); });
                        if (!value)
                            return std::nullopt;
                        if (value->type == ast::Type::Float)
                        {
                            const llvm::APFloat& floating = llvm::cast<llvm::ConstantFP>(value->ir)->getValueAPF();
                            if (cast.type == ast::Type::Bool)
                                return Folded{floating.isZero() ? 0 : 1, ast::Type::Bool, std::nullopt};
                            llvm::APSInt integer(32, false);
                            bool exact = false;
                            if ((floating.convertToInteger(integer, llvm::APFloat::rmTowardZero, &exact)
                                 & llvm::APFloat::opInvalidOp)
                                != 0)
                                return notConstant(expr.location);
                            return Folded{static_cast<std::int32_t>(integer.getExtValue()), ast::Type::Int,
                                          std::nullopt};
                        }
                    }
                    const std::optional<Folded> operand = fold(*cast.operand, evaluated);
                    if (!operand)
                        return std::nullopt;
                    if (operand->floating)
                        return Folded{0, cast.type, operand->floating};
                    if (cast.type == ast::Type::Bool)
                        return Folded{operand->value != 0 ? 1 : 0, ast::Type::Bool, std::nullopt};
                    return Folded{operand->value, ast::Type::Int, std::nullopt};
                },
                [&](const ast::CallExpr&) { return notConstant(expr.location); },
                [&](const ast::AssignExpr&) { return notConstant(expr.location); },
                [&](const ast::IncrementExpr&) { return notConstant(expr.location); },
                [&](const ast::IndexExpr&) { return notConstant(expr.location); },
                [&](const ast::MemberExpr&) { return notConstant(expr.location); },
                [&](const ast::MethodCallExpr&) { return notConstant(expr.location); },
                [&](const ast::ThisExpr&) { return notConstant(expr.location); },
                [&](const ast::ConstructExpr&) { return notConstant(expr.location); },
            },
            expr.node);
    }

    // A binary operator on two constants, as arithmetic() computes it on ints. The bitwise operators take ints, as
    // binary()'s do; a float anywhere else makes a float, or a bool that is no constant (a comparison).
    std::optional<Lowering::Folded> Lowering::foldBinary(SourceLocation location, ast::BinaryOp op, Folded lhs,
                                                         Folded rhs, bool evaluated)
    {
        const bool floats = lhs.type == ast::Type::Float || rhs.type == ast::Type::Float;
        if (isBitwise(op) && floats)
        {
            error(location, "invalid operands to binary expression (" + quoted(ast::typeName(lhs.type)) + " and "
                                + quoted(ast::typeName(rhs.type)) + ")");
            return std::nullopt;
        }
        const ast::Type type = isComparison(op) ? ast::Type::Bool : floats ? ast::Type::Float : ast::Type::Int;
        if (lhs.floating || rhs.floating)   // a Float always has one
            return Folded{0, type, lhs.floating ? lhs.floating : rhs.floating};
        const std::int64_t a = lhs.value;
        const std::int64_t b = rhs.value;
        const auto exact = [&](std::int64_t wide) -> std::optional<Folded> {
            if (evaluated && (wide < kLeast || wide > kGreatest))
            {
                error(location, "overflow in expression; result is " + std::to_string(wrapped(wide))
                                    + " with type 'int'");
                return std::nullopt;
            }
            return Folded{wrapped(wide), type, std::nullopt};
        };
        const auto truth = [&](bool value) { return Folded{value ? 1 : 0, type, std::nullopt}; };
        switch (op)
        {
        case ast::BinaryOp::Add:
            return exact(a + b);
        case ast::BinaryOp::Subtract:
            return exact(a - b);
        case ast::BinaryOp::Multiply:
            return exact(a * b);
        case ast::BinaryOp::Divide:
        case ast::BinaryOp::Remainder:
            if (b == 0 || (a == kLeast && b == -1))
            {
                if (!evaluated)
                    return Folded{0, type, std::nullopt};
                diagnostics_.error(location, "expression is not an integer constant expression",
                                   {Diagnostic{location,
                                               b == 0 ? "division by zero is undefined"
                                                      : "value 2147483648 is outside the range of representable "
                                                        "values of type 'int'",
                                               {}}});
                return std::nullopt;
            }
            return Folded{wrapped(op == ast::BinaryOp::Divide ? a / b : a % b), type, std::nullopt};
        case ast::BinaryOp::Less:
            return truth(a < b);
        case ast::BinaryOp::LessEqual:
            return truth(a <= b);
        case ast::BinaryOp::Greater:
            return truth(a > b);
        case ast::BinaryOp::GreaterEqual:
            return truth(a >= b);
        case ast::BinaryOp::Equal:
            return truth(a == b);
        case ast::BinaryOp::NotEqual:
            return truth(a != b);
        case ast::BinaryOp::BitAnd:
            return Folded{wrapped(a & b), type, std::nullopt};
        case ast::BinaryOp::BitOr:
            return Folded{wrapped(a | b), type, std::nullopt};
        case ast::BinaryOp::BitXor:
            return Folded{wrapped(a ^ b), type, std::nullopt};
        case ast::BinaryOp::ShiftLeft:   // FMinus's: by the count's low five bits, the bits shifted out lost
            return Folded{wrapped(static_cast<std::int64_t>(static_cast<std::uint32_t>(wrapped(a)) << (b & 31))), type,
                          std::nullopt};
        case ast::BinaryOp::ShiftRight:   // the sign shifted in
            return Folded{wrapped(a) >> (b & 31), type, std::nullopt};
        }
        return std::nullopt;
    }
} // namespace fminus::ir::detail
