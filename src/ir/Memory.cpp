// src/ir/Memory.cpp: lvalues, what a program can assign: a variable, or components of one (`v.x`, `v.zw`, `v.xy.x`).
// lvalue() finds one, read() and write() load and store it. An assignment, and anything else that stores, goes
// through them, so a new kind of lvalue joins every one at once.
#include "ir/Lowering.h"

#include <cstdint>

namespace fminus::ir::detail
{
    // Whether `expr` names something a program could store to; lvalue() then checks that it does. C's rule, which
    // the grammar leaves to the code generator: `(a ? b : c) = 1` and `f().x = 1` are not assignable.
    bool Lowering::isLValue(const ast::Expr& expr)
    {
        if (std::holds_alternative<ast::VariableExpr>(expr.node))
            return true;
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

    // Whether `target` may be stored to: not const, and with no component twice (`v.xx = e` would write lane 0
    // twice). Reports clang's error when not.
    bool Lowering::writable(SourceLocation location, const LValue& target)
    {
        if (target.isConst)
        {
            error(location, "cannot assign to variable " + quoted(target.name) + " with const-qualified type "
                                + quoted(std::string("const ") + ast::typeName(target.whole)));
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
} // namespace fminus::ir::detail
