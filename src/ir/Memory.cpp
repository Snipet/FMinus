// src/ir/Memory.cpp: memory. lvalues, what a program can assign or take the address of: a variable, an element a
// pointer reaches (`p[i]`, `*p`), or components of either (`v.x`, `p[i].zw`, `v.xy.x`). lvalue() finds one, read()
// and write() load and store it; reading a variable, an assignment, ++ and & all go through them, so a new kind of
// lvalue joins every one at once. Also C's arithmetic on pointers, which is arithmetic on addresses.
//
// An element's address is a getelementptr, as clang's: inbounds (it stays within the object the pointer points
// into, or one past its end, as C requires), on an index widened to 64 bits with sext, since the index is a signed
// int.
#include "ir/Lowering.h"

#include <cstdint>

namespace fminus::ir::detail
{
    // Whether `expr` names something a program could store to; lvalue() then checks that it does. C's rule, which
    // the grammar leaves to the code generator: `(a ? b : c) = 1` and `f().x = 1` are not assignable.
    bool Lowering::isLValue(const ast::Expr& expr)
    {
        if (std::holds_alternative<ast::VariableExpr>(expr.node) || std::holds_alternative<ast::IndexExpr>(expr.node))
            return true;
        if (const auto* unary = std::get_if<ast::UnaryExpr>(&expr.node))
            return unary->op == ast::UnaryOp::Dereference;
        if (const auto* swizzle = std::get_if<ast::SwizzleExpr>(&expr.node))
            return isLValue(*swizzle->base);
        return false;
    }

    // The lvalue `expr` names (isLValue said it names one), or nothing after an error. Errors about components are
    // reported at `location`, the operator that stores (clang's place for them).
    std::optional<Lowering::LValue> Lowering::lvalue(const ast::Expr& expr, SourceLocation location)
    {
        if (const auto* variable = std::get_if<ast::VariableExpr>(&expr.node))
        {
            const Binding* binding = find(variable->name);
            if (binding == nullptr)
            {
                error(expr.location, "use of undeclared identifier " + quoted(variable->name));
                return std::nullopt;
            }
            return LValue{binding->slot, binding->type, binding->isConst, variable->name, binding->type, {}};
        }
        if (const auto* index = std::get_if<ast::IndexExpr>(&expr.node))
        {
            Value base = lower(*index->base);
            Value offset = lower(*index->index);
            if (!ast::isPointer(base.type) && ast::isPointer(offset.type))
                std::swap(base, offset);   // C's i[p] is p[i]
            if (!ast::isPointer(base.type))
            {
                error(expr.location, "subscripted value is not a pointer");
                return std::nullopt;
            }
            if (offset.type != ast::Type::Int && offset.type != ast::Type::Bool)
            {
                error(expr.location, "array subscript is not an integer");
                return std::nullopt;
            }
            return element(expr.location, base, offset);
        }
        if (const auto* unary = std::get_if<ast::UnaryExpr>(&expr.node))   // *p
        {
            const Value pointer = lower(*unary->operand);
            if (!ast::isPointer(pointer.type))
            {
                error(expr.location,
                      "indirection requires pointer operand (" + quoted(ast::typeName(pointer.type)) + " invalid)");
                return std::nullopt;
            }
            return element(expr.location, pointer, std::nullopt);
        }

        // Components of an lvalue: of a variable, or of components of one (v.xy.x is v's lane 0).
        const auto& swizzle = std::get<ast::SwizzleExpr>(expr.node);
        std::optional<LValue> base = lvalue(*swizzle.base, location);
        if (!base || !hasComponents(location, base->type))
            return std::nullopt;
        const auto lanes = components(location, swizzle.components, base->type);
        if (!lanes)
            return std::nullopt;
        const auto type = componentsType(location, lanes->size());
        if (!type)
            return std::nullopt;
        std::vector<int> whole;   // the lanes of the vector in memory
        for (const int lane : *lanes)
            whole.push_back(base->components.empty() ? lane : base->components[static_cast<std::size_t>(lane)]);
        base->type = *type;
        base->components = std::move(whole);
        return base;
    }

    // The element `pointer` points to (*p), or the index-th from it (p[i]): its address, and its type, const when
    // the pointer's is a const T*. Nothing for a pointer to what FMinus cannot point to (its declaration reported
    // that).
    std::optional<Lowering::LValue> Lowering::element(SourceLocation location, Value pointer,
                                                      std::optional<Value> index)
    {
        const ast::Type type = ast::pointee(pointer.type);
        if (type == ast::Type::Void)   // an address with no type: it has no elements
        {
            error(location, index ? "subscript of pointer to incomplete type 'void'"
                                  : "indirection not permitted on operand of type " + quoted(ast::typeName(pointer.type)));
            return std::nullopt;
        }
        if (!ast::canPointTo(type.kind))
            return std::nullopt;
        llvm::Value* address = pointer.ir;
        if (index)
        {
            llvm::Value* offset = convert(*index, ast::Type::Int, location, Use::Initializing);   // a bool: 0 or 1
            offset = builder_.CreateSExt(offset, builder_.getInt64Ty(), "idxprom");
            address = builder_.CreateInBoundsGEP(typeOf(type), pointer.ir, offset, "arrayidx");
        }
        return LValue{address, type, pointer.type.pointeeConst, "", type, {}};
    }

    // Whether `target` may be stored to: not const, and with no component twice (`v.xx = e` would write lane 0
    // twice). Reports clang's error when not.
    bool Lowering::writable(SourceLocation location, const LValue& target)
    {
        if (target.isConst && target.name.empty())
        {
            error(location, "read-only variable is not assignable");   // an element through a const T*
            return false;
        }
        if (target.isConst)
        {
            // clang writes a const pointer's type with the const last: 'float *const'
            const std::string type = ast::isPointer(target.whole) ? ast::typeName(target.whole) + "const"
                                                                  : "const " + ast::typeName(target.whole);
            error(location, "cannot assign to variable " + quoted(target.name) + " with const-qualified type "
                                + quoted(type));
            return false;
        }
        for (std::size_t i = 0; i < target.components.size(); ++i)
        {
            for (std::size_t j = i + 1; j < target.components.size(); ++j)
            {
                if (target.components[i] == target.components[j])
                {
                    error(location, "vector is not assignable (contains duplicate components)");
                    return false;
                }
            }
        }
        return true;
    }

    // A load; for components, a load of the vector and the lanes taken out of it (an extractelement for one, a
    // shufflevector for several).
    Lowering::Value Lowering::read(const LValue& source)
    {
        llvm::Value* loaded = builder_.CreateLoad(typeOf(source.whole), source.address, source.name);
        if (source.components.empty())
            return {loaded, source.type};
        if (source.components.size() == 1)
            return {builder_.CreateExtractElement(loaded, static_cast<std::uint64_t>(source.components[0])),
                    source.type};
        return {builder_.CreateShuffleVector(loaded, source.components), source.type};
    }

    // An lvalue expression's value: a variable's, an element's (p[i], *p). Poison after an error.
    Lowering::Value Lowering::readLValue(const ast::Expr& expr)
    {
        const std::optional<LValue> source = lvalue(expr, expr.location);
        return source ? read(*source) : poison(ast::Type::Float);
    }

    // `value` converted to the target's type and stored: for components, the vector is loaded, the lanes inserted
    // and the vector stored back. Returns what was stored, the value of an assignment.
    llvm::Value* Lowering::write(SourceLocation location, const LValue& target, Value value, Use use)
    {
        llvm::Value* stored = convert(value, target.type, location, use);
        if (target.components.empty())
        {
            builder_.CreateStore(stored, target.address);
            return stored;
        }
        llvm::Value* vector = builder_.CreateLoad(typeOf(target.whole), target.address, target.name);
        for (std::size_t i = 0; i < target.components.size(); ++i)
        {
            llvm::Value* element = target.components.size() == 1 ? stored : builder_.CreateExtractElement(stored, i);
            vector = builder_.CreateInsertElement(vector, element,
                                                  static_cast<std::uint64_t>(target.components[i]));
        }
        builder_.CreateStore(vector, target.address);
        return stored;
    }

    // &x: the address of an lvalue, a pointer to its type (const when it is). A variable whose address is taken stays
    // in memory, since mem2reg cannot promote a slot that escapes; after inlining, SROA often can.
    Lowering::Value Lowering::lowerAddressOf(SourceLocation location, const ast::Expr& operand)
    {
        const Value failed = poison(ast::Type::pointerTo(ast::Kind::Float, false));
        if (!isLValue(operand))
        {
            const ast::Type type = lower(operand).type;
            error(location, "cannot take the address of an rvalue of type " + quoted(ast::typeName(type)));
            return failed;
        }
        const std::optional<LValue> target = lvalue(operand, location);
        if (!target)
            return failed;
        if (!target->components.empty())
        {
            error(location, "address of vector element requested");
            return failed;
        }
        if (!ast::canPointTo(target->type.kind))
        {
            noPointerTo(location, target->type);
            return failed;
        }
        return {target->address, ast::Type::pointerTo(target->type.kind, target->isConst)};
    }

    // C's arithmetic on pointers. p + i and i + p are the element i after p (i may be negative), and p - i the one i
    // before; p - q is how many elements apart two pointers of one type are, an int (C's is a ptrdiff_t, which
    // FMinus does not have); and two pointers of one type compare as addresses. const does not matter to any of
    // them. Nothing else takes a pointer.
    Lowering::Value Lowering::pointerArithmetic(SourceLocation location, ast::BinaryOp op, Value lhs, Value rhs)
    {
        const std::string operands =
            "(" + quoted(ast::typeName(lhs.type)) + " and " + quoted(ast::typeName(rhs.type)) + ")";
        const auto integer = [](Value value) { return value.type == ast::Type::Int || value.type == ast::Type::Bool; };
        const bool voidPointee = (ast::isPointer(lhs.type) && lhs.type.pointee == ast::Kind::Void)
                                 || (ast::isPointer(rhs.type) && rhs.type.pointee == ast::Kind::Void);
        if (voidPointee && !isComparison(op) && (op == ast::BinaryOp::Add || op == ast::BinaryOp::Subtract))
        {
            error(location, "arithmetic on a pointer to void");   // a void* has no element size
            return poison(op == ast::BinaryOp::Subtract && ast::isPointer(rhs.type) ? ast::Type::Int : lhs.type);
        }
        if (ast::isPointer(lhs.type) && ast::isPointer(rhs.type))
        {
            const bool comparison = isComparison(op);
            // A void* compares with any pointer, as in C: it converts to the void* first.
            const bool samePointee = lhs.type.pointee == rhs.type.pointee || voidPointee;
            if (samePointee && op == ast::BinaryOp::Subtract && ast::canPointTo(lhs.type.pointee))
            {
                llvm::Value* difference =
                    builder_.CreatePtrDiff(typeOf(ast::pointee(lhs.type)), lhs.ir, rhs.ir, "sub.ptr.div");
                return {builder_.CreateTrunc(difference, builder_.getInt32Ty(), "conv"), ast::Type::Int};
            }
            if (samePointee && comparison)
            {
                const auto predicate = [&] {
                    switch (op)
                    {
                    case ast::BinaryOp::Less:
                        return llvm::CmpInst::ICMP_ULT;
                    case ast::BinaryOp::LessEqual:
                        return llvm::CmpInst::ICMP_ULE;
                    case ast::BinaryOp::Greater:
                        return llvm::CmpInst::ICMP_UGT;
                    case ast::BinaryOp::GreaterEqual:
                        return llvm::CmpInst::ICMP_UGE;
                    case ast::BinaryOp::NotEqual:
                        return llvm::CmpInst::ICMP_NE;
                    default:
                        return llvm::CmpInst::ICMP_EQ;
                    }
                }();
                return {builder_.CreateICmp(predicate, lhs.ir, rhs.ir, "cmp"), ast::Type::Bool};
            }
            if (!samePointee && op == ast::BinaryOp::Subtract)
                error(location, quoted(ast::typeName(lhs.type)) + " and " + quoted(ast::typeName(rhs.type))
                                    + " are not pointers to compatible types");
            else if (!samePointee && comparison)
                error(location, "comparison of distinct pointer types " + operands);
            else
                error(location, "invalid operands to binary expression " + operands);
            return poison(comparison ? ast::Type::Bool : ast::Type::Int);
        }

        // A pointer and an integer: p + i, i + p, p - i.
        const bool pointerFirst = ast::isPointer(lhs.type);
        const Value& pointer = pointerFirst ? lhs : rhs;
        const Value& offset = pointerFirst ? rhs : lhs;
        const bool valid = integer(offset)
                           && (op == ast::BinaryOp::Add || (op == ast::BinaryOp::Subtract && pointerFirst));
        if (!valid)
        {
            error(location, "invalid operands to binary expression " + operands);
            return poison(isComparison(op) ? ast::Type::Bool : pointer.type);
        }
        if (!ast::canPointTo(pointer.type.pointee))
            return poison(pointer.type);   // its declaration reported it
        llvm::Value* index = builder_.CreateSExt(convert(offset, ast::Type::Int, location, Use::Initializing),
                                                 builder_.getInt64Ty(), "idx.ext");
        if (op == ast::BinaryOp::Subtract)
            index = builder_.CreateNeg(index, "idx.neg");
        return {builder_.CreateInBoundsGEP(typeOf(ast::pointee(pointer.type)), pointer.ir, index, "add.ptr"),
                pointer.type};
    }
} // namespace fminus::ir::detail
