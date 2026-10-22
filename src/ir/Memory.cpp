// src/ir/Memory.cpp: memory. lvalues, what a program can assign or take the address of: a variable, an element a
// pointer reaches (`p[i]`, `*p`), a module's field (`m.y`, `p->y`, or `y` in a method), or components of any of them
// (`v.x`, `p[i].zw`, `v.xy.x`). lvalue() finds one, read() and write() load and store it; reading a variable, an
// assignment, ++ and & all go through them, so a new kind of lvalue joins every one at once. Also C's arrays (an
// array's name stands for a pointer to its first element), and C's arithmetic on pointers, on addresses.
//
// An element's address is a getelementptr, as clang's: inbounds (it stays within the object the pointer points
// into, or one past its end, as C requires), on an index widened to 64 bits with sext, since the index is a signed
// int.
#include "ir/Lowering.h"

#include <algorithm>
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
        if (const auto* member = std::get_if<ast::MemberExpr>(&expr.node))
            return member->arrow || isLValue(*member->base);   // p->y always is, as *p is
        return false;
    }

    // The lvalue `expr` names (isLValue said it names one), or nothing after an error. Errors about components are
    // reported at `location`, the operator that stores (clang's place for them).
    std::optional<Lowering::LValue> Lowering::lvalue(const ast::Expr& expr, SourceLocation location)
    {
        if (const auto* variable = std::get_if<ast::VariableExpr>(&expr.node))
        {
            const Binding* binding = find(variable->name);
            // In a method, a name no local or parameter has may be one of the module's: `y` is this->y, as in C++.
            const ast::Module* module = function_ != nullptr ? function_->module : nullptr;
            if (binding == nullptr && module != nullptr
                && (findField(*module, variable->name, nullptr) != nullptr
                    || std::ranges::find(module->methods, variable->name) != module->methods.end()))
                return field(expr.location, self(), *module, variable->name, false);
            if (binding == nullptr)
            {
                error(expr.location, "use of undeclared identifier " + quoted(variable->name));
                return std::nullopt;
            }
            return LValue{binding->slot, binding->type, binding->isConst, variable->name, binding->type, {}};
        }
        if (const auto* index = std::get_if<ast::IndexExpr>(&expr.node))
        {
            if (const std::optional<unsigned> field = parameterOf(*index->base))
            {
                error(expr.location,
                      "parameter " + quoted(function_->module->fields[*field].name)
                          + " is the host's: a method reads it, and cannot assign it or take its address");
                return std::nullopt;
            }
            const std::size_t reported = diagnostics_.errors().size();
            Value base = lower(*index->base);
            Value offset = lower(*index->index);
            if (!ast::isPointer(base.type) && ast::isPointer(offset.type))
                std::swap(base, offset);   // C's i[p] is p[i]
            if (!ast::isPointer(base.type))
            {
                if (diagnostics_.errors().size() == reported)   // else the operands' own errors say why
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

        // p->y, a field through a pointer to a module.
        const auto& member = std::get<ast::MemberExpr>(expr.node);
        if (member.arrow)
        {
            const Value pointer = lower(*member.base);
            if (!ast::isPointer(pointer.type))
            {
                error(expr.location, "member reference type " + quoted(ast::typeName(pointer.type))
                                         + " is not a pointer"
                                         + (ast::isModule(pointer.type) ? "; did you mean to use '.'?" : ""));
                return std::nullopt;
            }
            if (pointer.type.pointee != ast::Kind::Module)
            {
                error(expr.location, "member reference base type " + quoted(ast::typeName(ast::pointee(pointer.type)))
                                         + " is not a structure or union");
                return std::nullopt;
            }
            return field(expr.location, pointer.ir, *pointer.type.module, member.name, pointer.type.pointeeConst);
        }
        // m.y, a module's field; or components of an lvalue: of a variable, or of components of one (v.xy.x is v's
        // lane 0).
        std::optional<LValue> base = lvalue(*member.base, location);
        if (!base)
            return std::nullopt;
        if (ast::isModule(base->type))
            return field(expr.location, base->address, *base->type.module, member.name, base->isConst);
        if (ast::isPointer(base->type) && base->type.pointee == ast::Kind::Module)
        {
            error(expr.location, "member reference type " + quoted(ast::typeName(base->type))
                                     + " is a pointer; did you mean to use '->'?");
            return std::nullopt;
        }
        if (!hasComponents(location, base->type))
            return std::nullopt;
        const auto lanes = components(location, member.name, base->type);
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

    // Whether `target` may be stored to: not an array, not const, and with no component twice (`v.xx = e` would write
    // lane 0 twice). Reports clang's error when not.
    bool Lowering::writable(SourceLocation location, const LValue& target)
    {
        if (ast::isArray(target.type))
        {
            error(location, "array type " + quoted(ast::typeName(target.type)) + " is not assignable");
            return false;
        }
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
    // shufflevector for several). An array is no load: its name stands for a pointer to its first element, const
    // when the array is (C's decay). Nor is a module, whose value is its address. The load carries its type's TBAA
    // tag (Tbaa.cpp), as every load and store of a value does.
    Lowering::Value Lowering::read(const LValue& source)
    {
        if (ast::isArray(source.type))
            return {builder_.CreateConstInBoundsGEP2_64(typeOf(source.type), source.address, 0, 0, "arraydecay"),
                    ast::Type::pointerTo(ast::pointee(source.type), source.isConst)};
        if (ast::isModule(source.type))
            return {source.address, source.type};
        llvm::LoadInst* loaded = builder_.CreateLoad(typeOf(source.whole), source.address, source.name);
        tag(loaded, source.whole);
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
    // and the vector stored back; a module is copied whole. Returns what was stored, the value of an assignment (a
    // module's: its address).
    llvm::Value* Lowering::write(SourceLocation location, const LValue& target, Value value, Use use)
    {
        llvm::Value* stored = convert(value, target.type, location, use);
        if (ast::isModule(target.type))
        {
            if (value.type == target.type)
                copy(target.address, value.ir, target.type);
            return target.address;
        }
        if (target.components.empty())
        {
            tag(builder_.CreateStore(stored, target.address), target.type);
            return stored;
        }
        llvm::LoadInst* whole = builder_.CreateLoad(typeOf(target.whole), target.address, target.name);
        tag(whole, target.whole);
        llvm::Value* vector = whole;
        for (std::size_t i = 0; i < target.components.size(); ++i)
        {
            llvm::Value* element = target.components.size() == 1 ? stored : builder_.CreateExtractElement(stored, i);
            vector = builder_.CreateInsertElement(vector, element,
                                                  static_cast<std::uint64_t>(target.components[i]));
        }
        tag(builder_.CreateStore(vector, target.address), target.whole);
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
        return {target->address, ast::Type::pointerTo(target->type, target->isConst)};
    }

    // Every byte of what is at `address` 0, as a declaration of an array or a module starts.
    void Lowering::zero(llvm::Value* address, ast::Type type)
    {
        builder_.CreateMemSet(address, builder_.getInt8(0), *sizeOf(type), alignOf(type));
    }

    // `float c[4] = {1, 2};`: the list's elements, converted to the element type, in order; the rest stay 0. C only
    // warns about excess elements (and drops them); FMinus refuses them, as it does a vector literal's. An array of
    // modules takes no list: its modules start at 0, as every module does.
    void Lowering::initializeArray(SourceLocation location, llvm::Value* address, ast::Type type,
                                   const std::vector<ast::ExprPtr>& list)
    {
        const ast::Type element = ast::pointee(type);
        if (list.size() > static_cast<std::size_t>(type.count))
        {
            error(location, "excess elements in array initializer");
            return;
        }
        if (ast::isModule(element) && !list.empty())
        {
            error(location, "an array of modules takes no initializer list: its modules start at 0");
            return;
        }
        for (std::size_t i = 0; i < list.size(); ++i)
        {
            const Value value = lower(*list[i]);
            llvm::Value* at = builder_.CreateConstInBoundsGEP2_64(typeOf(type), address, 0, i, "arrayinit");
            tag(builder_.CreateStore(convert(value, element, list[i]->location, Use::Initializing), at), element);
        }
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
            const bool samePointee = ast::samePointee(lhs.type, rhs.type) || voidPointee;
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
