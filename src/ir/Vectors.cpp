// src/ir/Vectors.cpp: vector expressions, OpenCL C's: literals, swizzles, and ?: with a vector condition, lane by
// lane. The types, and how scalars broadcast, are Types.cpp's; assigning components is Memory.cpp's. A swizzle is
// written as a module's field is, so it also reads a field of a module value.
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
        if (construct.braces)
            return lowerBraces(location, construct, args);
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
            // a scalar is one float lane; a vector (a mask as 0s and 1s) gives its lanes
            const ast::Type type = ast::isVector(arg.type) ? ast::arithmeticOf(arg.type) : ast::Type::Float;
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

    // (float4){a, b, c, d}, C's compound literal for a vector: scalars fill the lanes in order, and the rest are 0, so
    // (float4){x} is x and three zeros, not a broadcast, and (float4){} is all zeros. A vector element is an error, as
    // in C, except a single one of the literal's own type, which is the value itself. C only warns about excess
    // elements (and drops them); FMinus refuses them. The grammar has no poly form: a program can't know how many
    // lanes the zeros would fill.
    Lowering::Value Lowering::lowerBraces(SourceLocation location, const ast::ConstructExpr& construct,
                                          const std::vector<Value>& args)
    {
        if (args.size() > lanes(construct.type))
        {
            error(location, "excess elements in vector initializer");
            return poison(construct.type);
        }
        if (args.size() == 1 && args[0].type == construct.type)
            return args[0];
        llvm::Value* vector = llvm::Constant::getNullValue(typeOf(construct.type));
        for (std::size_t i = 0; i < args.size(); ++i)
        {
            if (ast::isVector(args[i].type))
            {
                // clang's: the first element could initialise the whole vector, any other only a lane
                incompatible(location, Use::Initializing, args[i].type, i == 0 ? construct.type : ast::Type::Float);
                return poison(construct.type);
            }
            llvm::Value* element = convert(args[i], ast::Type::Float, location, Use::Initializing);
            vector = builder_.CreateInsertElement(vector, element, static_cast<std::uint64_t>(i));
        }
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
    // shufflevector). m.y, where m is a module, is its field y, read (Modules.cpp).
    Lowering::Value Lowering::lowerMember(SourceLocation location, const ast::MemberExpr& swizzle)
    {
        const Value base = lower(*swizzle.base);
        if (ast::isModule(base.type))
        {
            const std::optional<LValue> field = this->field(location, base.ir, *base.type.module, swizzle.name, false);
            return field ? read(*field) : poison(ast::Type::Float);
        }
        if (ast::isPointer(base.type) && base.type.pointee == ast::Kind::Module)
        {
            error(location, "member reference type " + quoted(ast::typeName(base.type))
                                + " is a pointer; did you mean to use '->'?");
            return poison(ast::Type::Float);
        }
        if (!hasComponents(location, base.type))
            return poison(ast::Type::Float);
        const auto lanes = components(location, swizzle.name, base.type);
        if (!lanes)
            return poison(ast::Type::Float);
        const auto type = componentsType(location, lanes->size());
        if (!type)
            return poison(ast::Type::Float);
        if (lanes->size() == 1)
            return {builder_.CreateExtractElement(base.ir, static_cast<std::uint64_t>((*lanes)[0]), swizzle.name),
                    *type};
        return {builder_.CreateShuffleVector(base.ir, *lanes, swizzle.name), *type};
    }
} // namespace fminus::ir::detail
