// src/ir/Types.cpp: FMinus's types as LLVM's, and the conversions between them: C's implicit ones, and OpenCL C's for
// vectors. Also the errors' helpers (clang's wording) and poison values, which every other part uses.
#include "ir/Lowering.h"

#include <llvm/IR/Constants.h>

namespace fminus::ir::detail
{
    // A type's lanes: a poly type has the target's.
    unsigned Lowering::lanes(ast::Type type) const
    {
        return ast::isPoly(type) ? polyLanes_ : ast::lanes(type);
    }

    llvm::Type* Lowering::typeOf(ast::Type type)
    {
        switch (type)
        {
        case ast::Type::Float:
            return builder_.getFloatTy();
        case ast::Type::Bool:
            return builder_.getInt1Ty();
        case ast::Type::Float2:
        case ast::Type::Float4:
        case ast::Type::Float8:
        case ast::Type::PolyFloat:
            return llvm::FixedVectorType::get(builder_.getFloatTy(), lanes(type));
        case ast::Type::Bool2:
        case ast::Type::Bool4:
        case ast::Type::Bool8:
        case ast::Type::PolyBool:
            return llvm::FixedVectorType::get(builder_.getInt1Ty(), lanes(type));
        }
        return builder_.getFloatTy();
    }

    // C's implicit conversions, and OpenCL C's for vectors. A bool is 0 or 1 as a float, and a float is
    // `x != 0` as a bool (so a NaN is true). A scalar becomes a vector by being broadcast to every lane (a
    // poly bool takes a bool), and a lane mask is 0 or 1 per lane as the float vector of its size. Nothing
    // else converts from a vector: that is an error, reported for `use`.
    llvm::Value* Lowering::convert(Value value, ast::Type type, SourceLocation location, Use use)
    {
        if (value.type == type)
            return value.ir;
        if (!ast::isVector(value.type))
        {
            if (ast::isVector(type))
            {
                const ast::Type scalarType = ast::isMask(type) ? ast::Type::Bool : ast::Type::Float;
                llvm::Value* scalar = convert(value, scalarType, location, use);
                return builder_.CreateVectorSplat(lanes(type), scalar, "splat");
            }
            if (type == ast::Type::Bool)
                return builder_.CreateFCmpUNE(value.ir, llvm::ConstantFP::get(value.ir->getType(), 0.0),
                                              "tobool");
            if (type == ast::Type::Float)
                return builder_.CreateUIToFP(value.ir, typeOf(type), "conv");
        }
        else if (ast::isMask(value.type) && type == ast::arithmeticOf(value.type))
        {
            return builder_.CreateUIToFP(value.ir, typeOf(type), "conv");
        }
        incompatible(location, use, value.type, type);
        return poison(type).ir;
    }

    void Lowering::incompatible(SourceLocation location, Use use, ast::Type from, ast::Type to)
    {
        const std::string source = quoted(ast::typeName(from));
        const std::string target = quoted(ast::typeName(to));
        switch (use)
        {
        case Use::Initializing:
            error(location, "initializing " + target + " with an expression of incompatible type " + source);
            return;
        case Use::Assigning:
            error(location, "assigning to " + target + " from incompatible type " + source);
            return;
        case Use::Returning:
            error(location, "returning " + source + " from a function with incompatible result type " + target);
            return;
        case Use::Passing:
            error(location, "passing " + source + " to parameter of incompatible type " + target);
            return;
        case Use::Condition:
            error(location, "statement requires expression of scalar type (" + source + " invalid)");
            return;
        case Use::Logical:
            error(location, "a logical operator needs scalar operands, not " + source);
            return;
        }
    }

    // The type two operands meet in, as OpenCL C's usual arithmetic conversions decide it: float, or the
    // vector type when either one is a vector (the other is broadcast to it); a bool and a lane mask take part
    // as floats. Vectors of two sizes do not meet: clang's error, and false.
    bool Lowering::commonType(SourceLocation location, ast::Type a, ast::Type b, ast::Type& type)
    {
        const ast::Type ta = ast::arithmeticOf(a);
        const ast::Type tb = ast::arithmeticOf(b);
        if (ast::isVector(ta) && ast::isVector(tb) && ast::isPoly(ta) != ast::isPoly(tb))
        {
            polyMismatch(location, a, b);
            return false;
        }
        if (ast::isVector(ta) && ast::isVector(tb) && ta != tb)
        {
            error(location, "cannot convert between vector values of different size ("
                                + quoted(ast::typeName(a)) + " and " + quoted(ast::typeName(b)) + ")");
            return false;
        }
        type = ast::isVector(ta) ? ta : tb;
        return true;
    }

    // A poly type never meets a fixed-size vector, even where the two have as many lanes: on another target
    // they would not, and a program means the same on every target.
    void Lowering::polyMismatch(SourceLocation location, ast::Type a, ast::Type b)
    {
        error(location, "cannot convert between " + quoted(ast::typeName(a)) + " and "
                            + quoted(ast::typeName(b))
                            + ": a poly type has as many lanes as the target's vectors");
    }

    Lowering::Value Lowering::poison(ast::Type type)
    {
        return {llvm::PoisonValue::get(typeOf(type)), type};
    }

    void Lowering::error(SourceLocation location, std::string message)
    {
        diagnostics_.error(location, std::move(message));
    }
} // namespace fminus::ir::detail
