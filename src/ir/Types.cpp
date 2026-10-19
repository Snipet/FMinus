// src/ir/Types.cpp: FMinus's types as LLVM's, and the conversions between them: C's implicit ones (and its casts), and
// OpenCL C's for vectors. Also the errors' helpers (clang's wording) and poison values, which every other part uses.
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
        switch (type.kind)
        {
        case ast::Kind::Void:
            return builder_.getVoidTy();
        case ast::Kind::Float:
            return builder_.getFloatTy();
        case ast::Kind::Bool:
            return builder_.getInt1Ty();
        case ast::Kind::Int:
            return builder_.getInt32Ty();
        case ast::Kind::Float2:
        case ast::Kind::Float4:
        case ast::Kind::Float8:
        case ast::Kind::PolyFloat:
            return llvm::FixedVectorType::get(builder_.getFloatTy(), lanes(type));
        case ast::Kind::Bool2:
        case ast::Kind::Bool4:
        case ast::Kind::Bool8:
        case ast::Kind::PolyBool:
            return llvm::FixedVectorType::get(builder_.getInt1Ty(), lanes(type));
        case ast::Kind::Pointer:
            return builder_.getPtrTy();   // opaque: what it points to rides on each load, store and GEP
        }
        return builder_.getFloatTy();
    }

    // C's implicit conversions, and OpenCL C's for vectors. Between scalars: a bool is 0 or 1 as an int or a
    // float; an int is exact as a float up to 2^24 and rounded beyond; a float becomes an int truncated toward
    // zero (out of int's range the result is undefined, as in C); and a number is `x != 0` as a bool (so a NaN
    // is true). A scalar becomes a vector by being broadcast to every lane (a poly bool takes a bool), and a
    // lane mask is 0 or 1 per lane as the float vector of its size. Nothing else converts from a vector, and
    // nothing at all from void (a call of a void function has no value): those are errors, reported for `use`.
    llvm::Value* Lowering::convert(Value value, ast::Type type, SourceLocation location, Use use)
    {
        if (value.type == type)
            return value.ir;
        if (value.type == ast::Type::Void || type == ast::Type::Void)
        {
            incompatible(location, use, value.type, type);
            return poison(type).ir;
        }
        if (ast::isPointer(value.type) || ast::isPointer(type))
            return convertPointer(value, type, location, use);
        if (!ast::isVector(value.type))
        {
            if (ast::isVector(type))
            {
                const ast::Type scalarType = ast::isMask(type) ? ast::Type::Bool : ast::Type::Float;
                llvm::Value* scalar = convert(value, scalarType, location, use);
                return builder_.CreateVectorSplat(lanes(type), scalar, "splat");
            }
            const bool fromFloat = value.type == ast::Type::Float;
            if (type == ast::Type::Bool)
            {
                llvm::Value* zero = llvm::Constant::getNullValue(value.ir->getType());
                return fromFloat ? builder_.CreateFCmpUNE(value.ir, zero, "tobool")
                                 : builder_.CreateICmpNE(value.ir, zero, "tobool");
            }
            if (type == ast::Type::Int)
                return fromFloat ? builder_.CreateFPToSI(value.ir, typeOf(type), "conv")
                                 : builder_.CreateZExt(value.ir, typeOf(type), "conv");
            if (type == ast::Type::Float)
                return value.type == ast::Type::Int ? builder_.CreateSIToFP(value.ir, typeOf(type), "conv")
                                                    : builder_.CreateUIToFP(value.ir, typeOf(type), "conv");
        }
        else if (ast::isMask(value.type) && type == ast::arithmeticOf(value.type))
        {
            return builder_.CreateUIToFP(value.ir, typeOf(type), "conv");
        }
        incompatible(location, use, value.type, type);
        return poison(type).ir;
    }

    // A pointer converts to a bool (whether it is not null), and to a pointer to the same type with const added
    // (float* to const float*), as in C. Nothing else converts to or from a pointer: C would convert another pointer
    // or an integer, with a warning; FMinus refuses, in the warning's words.
    llvm::Value* Lowering::convertPointer(Value value, ast::Type type, SourceLocation location, Use use)
    {
        if (ast::isPointer(value.type) && type == ast::Type::Bool)
            return builder_.CreateIsNotNull(value.ir, "tobool");
        if (ast::isPointer(value.type) && ast::isPointer(type))
        {
            const bool samePointee = value.type.pointee == type.pointee;
            const bool discardsConst = value.type.pointeeConst && !type.pointeeConst;
            if (samePointee && !discardsConst)
                return value.ir;   // const is the type's, not the IR's: the same ptr
            pointerMismatch(location, use, value.type, type, samePointee);
            return poison(type).ir;
        }
        incompatible(location, use, value.type, type);
        return poison(type).ir;
    }

    // clang's words for a pointer that converts only with a warning in C: to a pointer to another type ("incompatible
    // pointer types ..."), or to one without the const it had ("... discards qualifiers").
    void Lowering::pointerMismatch(SourceLocation location, Use use, ast::Type from, ast::Type to, bool discardsConst)
    {
        const std::string source = quoted(ast::typeName(from));
        const std::string target = quoted(ast::typeName(to));
        std::string what;
        switch (use)
        {
        case Use::Initializing:
            what = "initializing " + target + " with an expression of type " + source;
            break;
        case Use::Assigning:
            what = "assigning to " + target + " from " + source;
            break;
        case Use::Returning:
            what = "returning " + source + " from a function with result type " + target;
            break;
        case Use::Passing:
            what = "passing " + source + " to parameter of type " + target;
            break;
        case Use::Condition:   // a pointer converts to a bool, and nothing converts to a pointer otherwise
        case Use::Logical:
        case Use::Casting:
            what = "converting " + source + " to " + target;
            break;
        }
        error(location, discardsConst ? what + " discards qualifiers" : "incompatible pointer types " + what);
    }

    // Whether a variable, a parameter or a result may have `type`: one with a pointer to what FMinus cannot point to
    // may not. Reports the error when not.
    bool Lowering::checkType(SourceLocation location, ast::Type type)
    {
        if (!ast::isPointer(type) || ast::canPointTo(type.pointee))
            return true;
        noPointerTo(location, ast::pointee(type));
        return false;
    }

    void Lowering::noPointerTo(SourceLocation location, ast::Type pointee)
    {
        error(location, "FMinus has pointers to float, int, float2, float4 and float8, not to "
                            + quoted(ast::typeName(pointee)));
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
        case Use::Casting:
            error(location, "cannot cast from " + source + " to " + target);
            return;
        }
    }

    // The type two operands meet in, as C's usual arithmetic conversions and OpenCL C's decide it: the vector
    // type when either one is a vector (the other is broadcast to it), otherwise float when either one is a
    // float, otherwise int. A bool takes part as an int, and a lane mask as the float vector of its size. Vectors
    // of two sizes do not meet: clang's error, and false.
    bool Lowering::commonType(SourceLocation location, ast::Type a, ast::Type b, ast::Type& type)
    {
        if (!ast::isArithmetic(a) || !ast::isArithmetic(b))
        {
            error(location, "invalid operands to binary expression (" + quoted(ast::typeName(a)) + " and "
                                + quoted(ast::typeName(b)) + ")");
            return false;
        }
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
        if (ast::isVector(ta) || ast::isVector(tb))
            type = ast::isVector(ta) ? ta : tb;
        else
            type = ta == ast::Type::Int && tb == ast::Type::Int ? ast::Type::Int : ast::Type::Float;
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

    // A value of `type` that stands in for one an error kept from being computed. void has no values: its stand-in
    // has no IR, which nothing reads, since every use of a void value is an error of its own.
    Lowering::Value Lowering::poison(ast::Type type)
    {
        if (type == ast::Type::Void)
            return {nullptr, type};
        return {llvm::PoisonValue::get(typeOf(type)), type};
    }

    void Lowering::error(SourceLocation location, std::string message)
    {
        diagnostics_.error(location, std::move(message));
    }
} // namespace fminus::ir::detail
