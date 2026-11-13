// src/ir/Types.cpp: FMinus's types as LLVM's, and the conversions between them: C's implicit ones (and its casts), and
// OpenCL C's for vectors. Also the errors' helpers (clang's wording) and poison values, which every other part uses.
#include "ir/Lowering.h"

#include <llvm/IR/Constants.h>

#include <cstdint>

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
        case ast::Kind::Double:
            return builder_.getDoubleTy();
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
        case ast::Kind::Handle:   // C's pointer to a struct the program never sees
            return builder_.getPtrTy();   // opaque: what it points to rides on each load, store and GEP
        case ast::Kind::Module:
            return structs_.at(type.module);   // layOut() made one for every module (Modules.cpp)
        case ast::Kind::Array:
            return llvm::ArrayType::get(typeOf(ast::pointee(type)), static_cast<std::uint64_t>(type.count));
        case ast::Kind::Char:
            return builder_.getInt8Ty();
        case ast::Kind::String:   // C's struct { const char* data; int size; }: 16 bytes, aligned to 8
            return llvm::StructType::get(module_.getContext(), {builder_.getPtrTy(), builder_.getInt32Ty()});
        }
        return builder_.getFloatTy();
    }

    // The LLVM type of a Value of `type`: typeOf's, but a ptr for a module, whose value is its address (and for an
    // array, which no Value has: its name stands for a pointer).
    llvm::Type* Lowering::valueTypeOf(ast::Type type)
    {
        return ast::isModule(type) || ast::isArray(type) ? builder_.getPtrTy() : typeOf(type);
    }

    // C's implicit conversions, and OpenCL C's for vectors. Between scalars: a bool is 0 or 1 as an int, a float or a
    // double; an int is exact as a float up to 2^24 and rounded beyond, and exact as a double; a float is exact as a
    // double, and a double rounds to the nearest float (to infinity past the greatest); a float or a double becomes an
    // int truncated toward zero (out of int's range the result is undefined, as in C); and a number is `x != 0` as a
    // bool (so a NaN is true). A scalar becomes a vector by being broadcast to every lane (a poly bool takes a bool),
    // and a lane mask is 0 or 1 per lane as the float vector of its size. A double does not: a vector's lanes are
    // floats, and it would be rounded to one silently (clang's error; a vector literal's element converts, as a cast
    // does: Vectors.cpp). Nothing else converts from a vector, and nothing at all from void (a call of a void function
    // has no value): those are errors, reported for `use`. A value that failed converts silently. A string converts to
    // nothing and nothing to it (a literal is one where one is wanted: lowerAs), and is no condition. A char, which no
    // value is, is what a number converts to on its way into a char array's element: its low 8 bits.
    llvm::Value* Lowering::convert(Value value, ast::Type type, SourceLocation location, Use use)
    {
        if (value.type == type)
            return value.ir;
        if (value.failed)   // its own error says why: it converts to anything, as a poison of the type (Value)
            return poison(type).ir;
        if (value.type == ast::Type::String || type == ast::Type::String)
        {
            if (value.type == ast::Type::String && (use == Use::Condition || use == Use::Logical))
                diagnostics_.error(location,
                                   use == Use::Condition
                                       ? "statement requires expression of scalar type ('string' invalid)"
                                       : "a logical operator needs scalar operands, not 'string'",
                                   {Diagnostic{location, "a string is no condition: test whether it is empty with "
                                                         "string_size(s) > 0",
                                               {}}});
            else
                incompatible(location, use, value.type, type);
            return poison(type).ir;
        }
        if (type == ast::Type::Char && ast::isArithmetic(value.type) && !ast::isVector(value.type))
            return builder_.CreateTrunc(convert(value, ast::Type::Int, location, use), builder_.getInt8Ty(), "conv");
        const bool aggregate = ast::isModule(value.type) || ast::isModule(type) || ast::isArray(value.type)
                               || ast::isArray(type);
        if (value.type == ast::Type::Void || type == ast::Type::Void || aggregate)   // a module converts to nothing
        {
            incompatible(location, use, value.type, type);
            return poison(type).ir;
        }
        if (ast::isPointer(value.type) || ast::isPointer(type) || ast::isHandle(value.type) || ast::isHandle(type))
            return convertPointer(value, type, location, use);
        if (!ast::isVector(value.type))
        {
            if (ast::isVector(type) && value.type == ast::Type::Double)
            {
                incompatible(location, use, value.type, type);
                return poison(type).ir;
            }
            if (ast::isVector(type))
            {
                const ast::Type scalarType = ast::isMask(type) ? ast::Type::Bool : ast::Type::Float;
                llvm::Value* scalar = convert(value, scalarType, location, use);
                return builder_.CreateVectorSplat(lanes(type), scalar, "splat");
            }
            const bool fromFloating = ast::isFloating(value.type);
            if (type == ast::Type::Bool)
            {
                llvm::Value* zero = llvm::Constant::getNullValue(value.ir->getType());
                return fromFloating ? builder_.CreateFCmpUNE(value.ir, zero, "tobool")
                                    : builder_.CreateICmpNE(value.ir, zero, "tobool");
            }
            if (type == ast::Type::Int)
                return fromFloating ? builder_.CreateFPToSI(value.ir, typeOf(type), "conv")
                                    : builder_.CreateZExt(value.ir, typeOf(type), "conv");
            if (ast::isFloating(type) && fromFloating)   // float to double, exact, or double to float, rounded
                return type == ast::Type::Double ? builder_.CreateFPExt(value.ir, typeOf(type), "conv")
                                                 : builder_.CreateFPTrunc(value.ir, typeOf(type), "conv");
            if (ast::isFloating(type))
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
    // or an integer, with a warning; FMinus refuses, in the warning's words. A handle is C's pointer to a struct the
    // program never sees: it converts to a bool, and to a void* (const or not); a void* converts to it, but a const
    // void* only with the const discarded, an error; and another pointer, or a handle of another kind, does not.
    llvm::Value* Lowering::convertPointer(Value value, ast::Type type, SourceLocation location, Use use)
    {
        if ((ast::isPointer(value.type) || ast::isHandle(value.type)) && type == ast::Type::Bool)
            return builder_.CreateIsNotNull(value.ir, "tobool");
        if (ast::isHandle(value.type) || ast::isHandle(type))
        {
            const bool fromVoid = ast::isPointer(value.type) && value.type.pointee == ast::Kind::Void;
            const bool toVoid = ast::isPointer(type) && type.pointee == ast::Kind::Void;
            if ((ast::isHandle(value.type) && toVoid) || (fromVoid && ast::isHandle(type) && !value.type.pointeeConst))
                return value.ir;
            if ((ast::isHandle(value.type) || ast::isPointer(value.type))
                && (ast::isHandle(type) || ast::isPointer(type)))
                pointerMismatch(location, use, value.type, type, fromVoid && ast::isHandle(type));
            else
                incompatible(location, use, value.type, type);
            return poison(type).ir;
        }
        if (ast::isPointer(value.type) && ast::isPointer(type))
        {
            // C converts any pointer to a void* and back (what malloc's result needs), without a cast.
            const bool compatible = ast::samePointee(value.type, type) || value.type.pointee == ast::Kind::Void
                                    || type.pointee == ast::Kind::Void;
            const bool discardsConst = value.type.pointeeConst && !type.pointeeConst;
            if (compatible && !discardsConst)
                return value.ir;   // const and the pointee are the type's, not the IR's: the same ptr
            pointerMismatch(location, use, value.type, type, compatible);
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

    // Whether a variable, a parameter, a result or a field may have `type`: a pointer to what FMinus cannot point to
    // may not, nor an array of what it cannot (an array's name stands for a pointer to its first element), nor a 2-D
    // array of it (a row's name stands for a pointer to the row's first element). Reports the error when not.
    bool Lowering::checkType(SourceLocation location, ast::Type type)
    {
        const ast::Type element = ast::elementOf(type);
        if (ast::isArray(type) && !ast::canBeElement(element.kind))
        {
            error(location, "FMinus has arrays of float, double, int, float2, float4, float8, char, string, modules "
                            "and handles, not of " + quoted(ast::typeName(element)));
            return false;
        }
        if (type == ast::Type::Char)   // C's char, which FMinus keeps only in arrays
        {
            error(location, "FMinus has no 'char' variables, parameters or results: a char is an array's element or a "
                            "pointer's pointee, and an element reads as an int from 0 to 255");
            return false;
        }
        if (!ast::isPointer(type) || ast::canPointTo(type.pointee))
            return true;
        noPointerTo(location, ast::pointee(type));
        return false;
    }

    void Lowering::noPointerTo(SourceLocation location, ast::Type pointee)
    {
        error(location, "FMinus has pointers to float, double, int, float2, float4, float8, char, void and modules, "
                        "not to " + quoted(ast::typeName(pointee)));
    }

    // A type's size in bytes, as C and clang's ext_vector_type have it on the 64-bit targets FMinus runs on: a double
    // is 8, a float4 16, a pointer 8, a poly float as many floats as the target's vectors hold (16 bytes with NEON, 64 with
    // AVX-512). A lane mask has a byte a lane. void has no size. A module's and an array's are the data layout's: a C
    // struct's and array's, padding included, which may pass 4 GB (an array of 2^30 float4s is 16 GB): 64 bits, as
    // C's size_t, for a copy's memcpy and a declaration's memset.
    std::optional<std::uint64_t> Lowering::sizeOf(ast::Type type)
    {
        switch (type.kind)
        {
        case ast::Kind::Void:
            return std::nullopt;
        case ast::Kind::Bool:
            return 1;
        case ast::Kind::Int:
        case ast::Kind::Float:
            return 4;
        case ast::Kind::Double:
            return 8;
        case ast::Kind::Float2:
        case ast::Kind::Float4:
        case ast::Kind::Float8:
        case ast::Kind::PolyFloat:
            return 4 * lanes(type);
        case ast::Kind::Bool2:
        case ast::Kind::Bool4:
        case ast::Kind::Bool8:
        case ast::Kind::PolyBool:
            return lanes(type);
        case ast::Kind::Pointer:
        case ast::Kind::Handle:
            return 8;
        case ast::Kind::Char:
            return 1;
        case ast::Kind::Module:
        case ast::Kind::Array:
        case ast::Kind::String:
            return module_.getDataLayout().getTypeAllocSize(typeOf(type)).getFixedValue();
        }
        return std::nullopt;
    }

    // A type's alignment in memory, as the data layout has it (C's: a float4's is 16, a module's its largest field's).
    llvm::Align Lowering::alignOf(ast::Type type)
    {
        return module_.getDataLayout().getABITypeAlign(typeOf(type));
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
    // type when either one is a vector (the other is broadcast to it), otherwise double when either one is a double,
    // float when either one is a float, otherwise int. A bool takes part as an int, and a lane mask as the float vector
    // of its size. Vectors of two sizes do not meet, nor a vector and a double, whose broadcast would round it to the
    // vector's floats: clang's errors, and false.
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
        if (ast::isVector(ta) != ast::isVector(tb) && (ta == ast::Type::Double || tb == ast::Type::Double))
        {
            error(location, "cannot convert between scalar type 'double' and vector type "
                                + quoted(ast::typeName(ast::isVector(ta) ? a : b))
                                + " as implicit conversion would cause truncation");
            return false;
        }
        if (ast::isVector(ta) || ast::isVector(tb))
            type = ast::isVector(ta) ? ta : tb;
        else if (ta == ast::Type::Int && tb == ast::Type::Int)
            type = ast::Type::Int;
        else
            type = ta == ast::Type::Double || tb == ast::Type::Double ? ast::Type::Double : ast::Type::Float;
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

    // A value of `type` that stands in for one an error kept from being computed. It is not failed by itself (Value):
    // lower() marks it failed when the expression reported the error, so a stand-in that no error is behind is checked
    // as any value is, and its uses report what is wrong with them. void has no values: its stand-in has no IR, which
    // nothing reads, since every use of a void value is an error of its own.
    Lowering::Value Lowering::poison(ast::Type type)
    {
        if (type == ast::Type::Void)
            return {nullptr, type};
        return {llvm::PoisonValue::get(valueTypeOf(type)), type};
    }

    // A poison of `type` that is failed (Value), as clang's RecoveryExpr contains errors, for where an error this
    // expression did not report is behind it: an operand's own (failed), or a declaration's that left a type FMinus
    // cannot use (a function's `bool *` result, refused where it is declared). Only there: a failed value with no
    // error behind it would let every use of it go unreported, and the program compile on a poison.
    Lowering::Value Lowering::recovery(ast::Type type)
    {
        Value value = poison(type);
        value.failed = true;
        return value;
    }

    void Lowering::error(SourceLocation location, std::string message)
    {
        diagnostics_.error(location, std::move(message));
    }
} // namespace fminus::ir::detail
