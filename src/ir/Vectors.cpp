// src/ir/Vectors.cpp: vector expressions, OpenCL C's: literals, swizzles read and assigned, and ?: with a vector
// condition, lane by lane. The types, and how scalars broadcast, are Types.cpp's.
#include "ir/Lowering.h"

#include <llvm/IR/Constants.h>

#include <cctype>
#include <cstdint>

namespace fminus::ir::detail
{
    // c ? a : b with a vector c chooses lane by lane, as in OpenCL C: there are no branches, so both sides run,
    // and a select picks each lane from one or the other. A float vector condition is `!= 0` per lane, as a
    // scalar one is. The result is a float vector of the condition's size; scalar branches are broadcast.
    Lowering::Value Lowering::lowerSelect(SourceLocation location, Value test, const ast::ConditionalExpr& conditional)
    {
        const ast::Type mask = ast::maskOf(ast::arithmeticOf(test.type));
        llvm::Value* condition = test.ir;
        if (!ast::isMask(test.type))
            condition = builder_.CreateFCmpUNE(condition, llvm::Constant::getNullValue(typeOf(test.type)),
                                               "tobool");
        const Value thenValue = lower(*conditional.thenValue);
        const Value elseValue = lower(*conditional.elseValue);
        const ast::Type type = ast::arithmeticOf(mask);
        ast::Type branches = type;
        if (!commonType(location, thenValue.type, elseValue.type, branches))
            return poison(type);
        if (ast::isVector(branches) && ast::isPoly(branches) != ast::isPoly(type))
        {
            polyMismatch(location, mask, branches);
            return poison(type);
        }
        if (ast::isVector(branches) && branches != type)
        {
            error(location, "vector condition type " + quoted(ast::typeName(mask)) + " and result type "
                                + quoted(ast::typeName(branches)) + " do not have the same number of elements");
            return poison(type);
        }
        return {builder_.CreateSelect(condition, convert(thenValue, type, location, Use::Initializing),
                                      convert(elseValue, type, location, Use::Initializing), "select"),
                type};
    }

    // (float4)(a, b, c, d), OpenCL C's vector literal: one scalar is broadcast; otherwise the arguments' lanes,
    // in order, must fill the vector exactly. (poly float)(x) is a broadcast only: a program does not know how
    // many lanes it would have to fill, and a poly argument cannot fill a fixed number.
    Lowering::Value Lowering::lowerConstruct(SourceLocation location, const ast::ConstructExpr& construct)
    {
        const unsigned count = lanes(construct.type);
        std::vector<Value> args;
        for (const ast::ExprPtr& arg : construct.args)
            args.push_back(lower(*arg));
        if (args.size() == 1 && !ast::isVector(args[0].type))
            return {convert(args[0], construct.type, location, Use::Initializing), construct.type};
        if (ast::isPoly(construct.type))
        {
            error(location, "a poly float literal takes one scalar, which it broadcasts");
            return poison(construct.type);
        }

        std::vector<llvm::Value*> elements;
        for (const Value& arg : args)
        {
            const ast::Type type = ast::arithmeticOf(arg.type);
            if (ast::isPoly(type))
            {
                polyMismatch(location, arg.type, construct.type);
                return poison(construct.type);
            }
            llvm::Value* floats = convert(arg, type, location, Use::Initializing);
            if (!ast::isVector(type))
                elements.push_back(floats);
            for (unsigned i = 0; ast::isVector(type) && i < lanes(type); ++i)
                elements.push_back(builder_.CreateExtractElement(floats, i));
        }
        if (elements.size() != count)
        {
            if (elements.size() < count)
                error(location, "too few elements in vector initialization (expected " + std::to_string(count)
                                    + " elements, have " + std::to_string(elements.size()) + ")");
            else
                error(location, "excess elements in vector initializer");
            return poison(construct.type);
        }
        llvm::Value* vector = llvm::PoisonValue::get(typeOf(construct.type));
        for (unsigned i = 0; i < count; ++i)
            vector = builder_.CreateInsertElement(vector, elements[i], i);
        return {vector, construct.type};
    }

    // The lanes a component name names, as in OpenCL C: x, y, z and w for 0 to 3, or s followed by hex digits
    // (s0 ... s7) for any lane; the two do not mix. Reports clang's errors, and returns nothing, for a name
    // that is neither, or that names a lane `vector` does not have.
    std::optional<std::vector<int>> Lowering::components(SourceLocation location, const std::string& name,
                                                         ast::Type vector)
    {
        std::vector<int> indices;
        const bool numbered = name.size() > 1 && (name[0] == 's' || name[0] == 'S');
        for (std::size_t i = numbered ? 1 : 0; i < name.size(); ++i)
        {
            const auto c = static_cast<unsigned char>(name[i]);
            int lane = -1;
            if (numbered && std::isxdigit(c) != 0)
                lane = std::isdigit(c) != 0 ? c - '0' : std::tolower(c) - 'a' + 10;
            else if (!numbered && std::string_view("xyzw").find(static_cast<char>(c)) != std::string_view::npos)
                lane = static_cast<int>(std::string_view("xyzw").find(static_cast<char>(c)));
            if (lane < 0)
            {
                error(location,
                      "illegal vector component name " + quoted(std::string(1, static_cast<char>(c))));
                return std::nullopt;
            }
            if (lane >= static_cast<int>(lanes(vector)))
            {
                error(location, "vector component access exceeds type " + quoted(ast::typeName(vector)));
                return std::nullopt;
            }
            indices.push_back(lane);
        }
        return indices;
    }

    // The type of `count` components: one is a float, 2, 4 or 8 a float vector; anything else is an error.
    std::optional<ast::Type> Lowering::componentsType(SourceLocation location, std::size_t count)
    {
        if (count == 1)
            return ast::Type::Float;
        const auto type = ast::vectorOf(static_cast<unsigned>(count));
        if (!type)
            error(location,
                  std::to_string(count) + " components have no type: FMinus has float2, float4 and float8");
        return type;
    }

    // Whether `type` has components: a float vector does; a scalar, a lane mask or a poly float (whose lanes
    // a program cannot name, not knowing how many there are) does not.
    bool Lowering::hasComponents(SourceLocation location, ast::Type type)
    {
        if (type == ast::Type::PolyFloat)
        {
            error(location, "a poly float has no components: its lanes are the target's");
            return false;
        }
        if (ast::isVector(type) && !ast::isMask(type))
            return true;
        error(location,
              "member reference base type " + quoted(ast::typeName(type)) + " is not a structure or union");
        return false;
    }

    // v.x is one lane (extractelement); v.wzyx, v.xxyy, v.s0123 are several, in any order, repeated or not (a
    // shufflevector).
    Lowering::Value Lowering::lowerSwizzle(SourceLocation location, const ast::SwizzleExpr& swizzle)
    {
        const Value base = lower(*swizzle.base);
        if (!hasComponents(location, base.type))
            return poison(ast::Type::Float);
        const auto lanes = components(location, swizzle.components, base.type);
        if (!lanes)
            return poison(ast::Type::Float);
        const auto type = componentsType(location, lanes->size());
        if (!type)
            return poison(ast::Type::Float);
        if (lanes->size() == 1)
            return {builder_.CreateExtractElement(base.ir, static_cast<std::uint64_t>((*lanes)[0]),
                                                  swizzle.components),
                    *type};
        return {builder_.CreateShuffleVector(base.ir, *lanes, swizzle.components), *type};
    }

    // v.x = e, v.zw += e: components of a vector variable, assigned. They must be distinct, and the value has
    // their type (a float for one, a vector of their number otherwise; a scalar is broadcast to it).
    Lowering::Value Lowering::assignComponents(SourceLocation location, const ast::AssignExpr& assign,
                                               const ast::SwizzleExpr& swizzle, Value value)
    {
        const Binding* binding = assignable(location, *swizzle.base);
        if (binding == nullptr || !hasComponents(location, binding->type))
            return poison(ast::Type::Float);
        const auto lanes = components(location, swizzle.components, binding->type);
        if (!lanes)
            return poison(ast::Type::Float);
        for (std::size_t i = 0; i < lanes->size(); ++i)
        {
            for (std::size_t j = i + 1; j < lanes->size(); ++j)
            {
                if ((*lanes)[i] == (*lanes)[j])
                {
                    error(location, "vector is not assignable (contains duplicate components)");
                    return poison(ast::Type::Float);
                }
            }
        }
        const auto type = componentsType(location, lanes->size());
        if (!type)
            return poison(ast::Type::Float);

        llvm::AllocaInst* slot = binding->slot;
        llvm::Value* vector = builder_.CreateLoad(slot->getAllocatedType(), slot, slot->getName());
        if (assign.op != ast::AssignOp::Assign)
        {
            llvm::Value* lanesNow =
                lanes->size() == 1
                    ? builder_.CreateExtractElement(vector, static_cast<std::uint64_t>((*lanes)[0]))
                    : builder_.CreateShuffleVector(vector, *lanes);
            const Value current{lanesNow, *type};
            value = compound(location, assign.op, current, value);
            if (value.ir == nullptr)
                return poison(*type);
        }
        llvm::Value* stored = convert(value, *type, location, Use::Assigning);
        for (std::size_t i = 0; i < lanes->size(); ++i)
        {
            llvm::Value* element = lanes->size() == 1 ? stored : builder_.CreateExtractElement(stored, i);
            vector = builder_.CreateInsertElement(vector, element, static_cast<std::uint64_t>((*lanes)[i]));
        }
        builder_.CreateStore(vector, slot);
        return {stored, *type};
    }
} // namespace fminus::ir::detail
