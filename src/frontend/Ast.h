// src/frontend/Ast.h: the syntax tree of FMinus v0, a subset of C. The parser (Parser.y) builds it, dump() prints it,
// and the code generator (ir/CodeGen.cpp) lowers it to LLVM IR.
//
// Every kind of expression and statement is one alternative of a std::variant. The code that walks the tree does it
// with std::visit and no default case, so adding an alternative makes every walk that does not handle it a compile
// error: the compiler lists what to update.
#pragma once

#include "frontend/Diagnostics.h"

#include <cstddef>
#include <memory>
#include <optional>
#include <string>
#include <variant>
#include <vector>

namespace fminus::ast
{
    // The value types. float is IEEE single precision (LLVM's float). int is C's on every target FMinus runs on: 32
    // bits, signed. Two ints compute as ints (`/` truncates toward zero), and an int meeting a float converts to it,
    // as C's usual arithmetic conversions say. bool is C23's: 0 or 1, which computes as an int, and to which a number
    // converts as `x != 0` (a NaN is true). void is C's: what a function returns when it returns nothing, and no
    // variable or parameter has it (`f(void)` is C's empty parameter list).
    //
    // float2, float4 and float8 are OpenCL C's vector types: that many floats side by side, computed on together. A
    // float4 fills one 128-bit NEON register; a float8 is two. Comparing two of them gives a lane mask, a bool2, bool4
    // or bool8, which a program cannot declare (as in OpenCL C) but can select with or use as 0s and 1s.
    //
    // poly float is a float vector as wide as the target's vector registers: 4 lanes with NEON or SSE, 8 with AVX2,
    // 16 with AVX-512. A program is written without knowing how many (poly_lanes() is the number, a constant once the
    // target is known), so it means the same on every target. Comparing two gives a poly bool, which a program can
    // declare.
    //
    // A pointer is C's: the address of a float, an int, a float2, a float4 or a float8 (FMinus has no pointers to
    // pointers, to bool or to poly types). `const float*` points to floats a program reads but cannot write through
    // it. p[i] is the i-th element from p, *p is p[0], and &x is x's address; p + i, p - i, p - q (how many elements
    // apart, an int) and comparisons of two pointers of one type are C's. A void* is an address with no type, what
    // malloc returns: it converts to and from any other pointer, as in C, and has no elements.
    //
    // A module is FMinus's class: fields, and methods that reach them (`module OnePole { float y; float tick(float
    // x) { ... } };`, Module below). A module type is a name the program declares; its value lives in memory (a local,
    // a field, an element, the heap or the host's), is copied by `=` and goes to a function through a pointer. A
    // pointer to a module is C's, and `p->y` its field.
    //
    // An array is C's: `float history[16];`, a fixed number of elements side by side, of a type FMinus can point to
    // (a float, an int, a float vector or a module). Its name stands for a pointer to its first element, which p[i]
    // indexes; it cannot be assigned, and a parameter declared as one is that pointer.
    //
    // A type is a kind, and for a pointer what it points to, for an array its elements and how many, for a module
    // which; two types are the same when every field is. Type::Float and the rest name the types that are only a
    // kind, and a kind converts to its type, so `type == Type::Float` reads as it says. A switch over a type is a
    // switch over `type.kind`, and -Wswitch lists the kinds it misses.
    struct Module;

    enum class Kind
    {
        Void,
        Float,
        Bool,
        Int,
        Float2,
        Float4,
        Float8,
        Bool2,
        Bool4,
        Bool8,
        PolyFloat,
        PolyBool,
        Pointer,
        Module,
        Array,
    };

    struct Type
    {
        Kind kind = Kind::Float;
        Kind pointee = Kind::Void;        // a pointer's: what it points to; an array's: what its elements are
        bool pointeeConst = false;        // a pointer's: what it points to is read-only through it (const float*)
        const Module* module = nullptr;   // a module type's, and a pointer's or an array's whose pointee is one
        int count = 0;                    // an array's: how many elements

        constexpr Type() = default;
        constexpr Type(Kind kind) : kind(kind) {}   // implicit: Kind::Float is the type float

        // A pointer to `pointee`, which is a kind or a module type (FMinus has no pointers to pointers or arrays).
        static constexpr Type pointerTo(Type pointee, bool isConst)
        {
            Type type(Kind::Pointer);
            type.pointee = pointee.kind;
            type.module = pointee.module;
            type.pointeeConst = isConst;
            return type;
        }

        static constexpr Type moduleType(const Module* module)
        {
            Type type(Kind::Module);
            type.module = module;
            return type;
        }

        // An array of `count` elements of `element`, a kind or a module type.
        static constexpr Type arrayOf(Type element, int count)
        {
            Type type(Kind::Array);
            type.pointee = element.kind;
            type.module = element.module;
            type.count = count;
            return type;
        }

        friend constexpr bool operator==(Type, Type) = default;

        static const Type Void, Float, Bool, Int, Float2, Float4, Float8, Bool2, Bool4, Bool8, PolyFloat, PolyBool;
    };

    inline constexpr Type Type::Void{Kind::Void};
    inline constexpr Type Type::Float{Kind::Float};
    inline constexpr Type Type::Bool{Kind::Bool};
    inline constexpr Type Type::Int{Kind::Int};
    inline constexpr Type Type::Float2{Kind::Float2};
    inline constexpr Type Type::Float4{Kind::Float4};
    inline constexpr Type Type::Float8{Kind::Float8};
    inline constexpr Type Type::Bool2{Kind::Bool2};
    inline constexpr Type Type::Bool4{Kind::Bool4};
    inline constexpr Type Type::Bool8{Kind::Bool8};
    inline constexpr Type Type::PolyFloat{Kind::PolyFloat};
    inline constexpr Type Type::PolyBool{Kind::PolyBool};

    inline bool isPoly(Type type)
    {
        return type == Type::PolyFloat || type == Type::PolyBool;
    }

    // A vector type's number of lanes; 1 for a scalar, and 0 for a poly type, whose lanes the target decides (the code
    // generator knows how many).
    inline unsigned lanes(Type type)
    {
        switch (type.kind)
        {
        case Kind::PolyFloat:
        case Kind::PolyBool:
            return 0;
        case Kind::Void:
        case Kind::Float:
        case Kind::Bool:
        case Kind::Int:
        case Kind::Pointer:
        case Kind::Module:
        case Kind::Array:
            return 1;
        case Kind::Float2:
        case Kind::Bool2:
            return 2;
        case Kind::Float4:
        case Kind::Bool4:
            return 4;
        case Kind::Float8:
        case Kind::Bool8:
            return 8;
        }
        return 1;
    }

    inline bool isVector(Type type)
    {
        return lanes(type) > 1 || isPoly(type);
    }

    inline bool isPointer(Type type)
    {
        return type.kind == Kind::Pointer;
    }

    inline bool isModule(Type type)
    {
        return type.kind == Kind::Module;
    }

    inline bool isArray(Type type)
    {
        return type.kind == Kind::Array;
    }

    // What a pointer of `type` points to, or what the elements of an array of `type` are.
    inline Type pointee(Type type)
    {
        Type element(type.pointee);
        if (type.pointee == Kind::Module)
            element.module = type.module;
        return element;
    }

    // Whether FMinus has pointers to `kind`: to float, int, float2, float4, float8, void and modules.
    inline bool canPointTo(Kind kind)
    {
        return kind == Kind::Float || kind == Kind::Int || kind == Kind::Float2 || kind == Kind::Float4
               || kind == Kind::Float8 || kind == Kind::Void || kind == Kind::Module;
    }

    // Whether two pointers point to the same type (const aside): the same kind, and the same module for modules.
    inline bool samePointee(Type a, Type b)
    {
        return a.pointee == b.pointee && a.module == b.module;
    }

    // Whether a value of `type` takes part in arithmetic: every type's but void's, a pointer's (whose arithmetic is
    // its own), a module's and an array's.
    inline bool isArithmetic(Type type)
    {
        return type != Type::Void && !isPointer(type) && !isModule(type) && !isArray(type);
    }

    inline bool isMask(Type type)
    {
        return type == Type::Bool2 || type == Type::Bool4 || type == Type::Bool8 || type == Type::PolyBool;
    }

    // The float vector type of `count` lanes, if FMinus has one.
    inline std::optional<Type> vectorOf(unsigned count)
    {
        switch (count)
        {
        case 2:
            return Type::Float2;
        case 4:
            return Type::Float4;
        case 8:
            return Type::Float8;
        default:
            return std::nullopt;
        }
    }

    // What a comparison of two values of `type` gives: a bool for scalars, a lane mask for float vectors.
    inline Type maskOf(Type type)
    {
        switch (type.kind)
        {
        case Kind::Float2:
            return Type::Bool2;
        case Kind::Float4:
            return Type::Bool4;
        case Kind::Float8:
            return Type::Bool8;
        case Kind::PolyFloat:
            return Type::PolyBool;
        default:
            return Type::Bool;
        }
    }

    // The type a value of `type` computes as: a bool as an int (C's integer promotion), a lane mask as the float vector
    // of its size.
    inline Type arithmeticOf(Type type)
    {
        if (type == Type::Bool)
            return Type::Int;
        if (type == Type::PolyBool)
            return Type::PolyFloat;
        if (isMask(type))
            return *vectorOf(lanes(type));
        return type;
    }

    enum class UnaryOp
    {
        Negate,        // -x
        Plus,          // +x: the value itself
        Not,           // !x: a bool
        Dereference,   // *p: what p points to, an lvalue
        AddressOf,     // &x: the address of an lvalue
    };

    enum class BinaryOp
    {
        Add,
        Subtract,
        Multiply,
        Divide,
        Remainder,
        Less,        // the comparisons give bool; with a NaN, all but != are false
        LessEqual,
        Greater,
        GreaterEqual,
        Equal,
        NotEqual,
    };

    // && and ||: C's, so the right operand runs only when the left does not decide.
    enum class LogicalOp
    {
        And,
        Or,
    };

    // =, and the compound assignments: x op= v is x = x op v.
    enum class AssignOp
    {
        Assign,
        Add,
        Subtract,
        Multiply,
        Divide,
        Remainder,
    };

    struct Expr;
    using ExprPtr = std::unique_ptr<Expr>;

    // A constant as written, f suffix included. An integer constant (2, 010 in octal, 0x1F in hex: no point and no
    // exponent) is an int, as in C. Any other is a float, which the code generator rounds once, straight to float (in
    // C, an unsuffixed 1.5 would be a double).
    struct NumberExpr
    {
        std::string spelling;
    };

    // `true` or `false`.
    struct BoolExpr
    {
        bool value = false;
    };

    struct LogicalExpr
    {
        LogicalOp op = LogicalOp::And;
        ExprPtr lhs;
        ExprPtr rhs;
    };

    // `condition ? thenValue : elseValue`: C's, so only one branch runs. Two floats or two bools give that type;
    // a float and a bool give float.
    struct ConditionalExpr
    {
        ExprPtr condition;
        ExprPtr thenValue;
        ExprPtr elseValue;
    };

    struct VariableExpr
    {
        std::string name;
    };

    struct UnaryExpr
    {
        UnaryOp op = UnaryOp::Negate;
        ExprPtr operand;
    };

    struct BinaryExpr
    {
        BinaryOp op = BinaryOp::Add;
        ExprPtr lhs;
        ExprPtr rhs;
    };

    // A call of a function of the program, or of a builtin (ir/CodeGen.cpp lists them).
    struct CallExpr
    {
        std::string callee;
        std::vector<ExprPtr> args;
    };

    // `target op value`. As in C, an expression: its value is the one stored, so `a = b = 0` works. The parser takes
    // any expression as the target; the code generator accepts only a variable that is not const.
    struct AssignExpr
    {
        AssignOp op = AssignOp::Assign;
        ExprPtr target;
        ExprPtr value;
    };

    // `(float4)(a, b, c, d)`: OpenCL C's vector literal. One scalar is broadcast to every lane; otherwise the
    // arguments' lanes (one for a scalar, all of a vector's) fill the vector in order, and must fill it exactly.
    // `(poly float)(x)` takes one scalar, broadcast.
    //
    // `(float4){a, b, c, d}`: C's, a compound literal (braces). Scalars fill the lanes in order and the rest are 0, so
    // `(float4){x}` is x and three zeros.
    struct ConstructExpr
    {
        Type type = Type::Float4;
        std::vector<ExprPtr> args;
        bool braces = false;   // C's form
    };

    // `m.y` and `p->y`: a module's field, through the module or a pointer to it, an lvalue. Also `v.x`, `v.wzyx`,
    // `v.s07`: OpenCL C's components of a vector. x, y, z, w name lanes 0 to 3, and s followed by hex digits names any
    // lane. One component is a float, two, four or eight a vector of them; an lvalue's components can be assigned.
    // The code generator tells the two apart by the base's type.
    struct MemberExpr
    {
        ExprPtr base;
        std::string name;    // the field, or the components
        bool arrow = false;  // p->y
    };

    // `m.tick(x)` and `p->tick(x)`: a method of the module, called on it. Inside a method, `tick(x)` calls one on
    // `this` (a CallExpr: the code generator looks for a method first, as C++ does).
    struct MethodCallExpr
    {
        ExprPtr object;
        std::string method;
        std::vector<ExprPtr> args;
        bool arrow = false;
    };

    // `this`, inside a method: a pointer to the module it was called on.
    struct ThisExpr
    {
    };

    enum class IncrementOp
    {
        PreIncrement,    // ++x
        PreDecrement,    // --x
        PostIncrement,   // x++
        PostDecrement,   // x--
    };

    // ++x, --x, x++ and x--: C's. The target is an lvalue that is not const, as an assignment's is; ++x is x += 1, and
    // so is x++, whose value is the target's before.
    struct IncrementExpr
    {
        IncrementOp op = IncrementOp::PreIncrement;
        ExprPtr target;
    };

    // `p[i]`: the i-th element from the pointer p, an lvalue; i is an int (or a bool), and may be negative. As in C,
    // i[p] is the same element.
    struct IndexExpr
    {
        ExprPtr base;
        ExprPtr index;
    };

    // `sizeof(float)`, `sizeof(float4*)`: a type's size in bytes, an int (C's is a size_t). FMinus has no sizeof of an
    // expression.
    struct SizeofExpr
    {
        Type type = Type::Float;
    };

    // `(int)x`, `(float)x`, `(bool)x`: C's casts between scalar types, the implicit conversions written out. (int)
    // truncates toward zero.
    struct CastExpr
    {
        Type type = Type::Float;
        ExprPtr operand;
    };

    struct Expr
    {
        SourceLocation location;
        std::variant<NumberExpr, VariableExpr, BoolExpr, UnaryExpr, BinaryExpr, LogicalExpr, CallExpr, AssignExpr,
                     ConditionalExpr, ConstructExpr, MemberExpr, MethodCallExpr, ThisExpr, IncrementExpr, CastExpr,
                     IndexExpr, SizeofExpr>
            node;
    };

    struct Stmt;

    // `[const] float name = init;`: a variable of the enclosing block, from its declaration to the block's end. One per
    // declarator: `float a = 1, b;` is two, and `float *p, x;` a pointer and a float, as in C. Without an initialiser
    // the variable starts at 0, and a pointer at null (C leaves them indeterminate, and reading them undefined). An
    // array's initialiser is a list, `float c[4] = {1, 2};`, whose missing elements are 0; a module is initialised
    // from another of its type, or starts at 0.
    struct DeclStmt
    {
        bool isConst = false;      // the variable's: `const float x`, or `float* const p` (`const float* p` is not)
        Type type = Type::Float;
        std::string name;
        ExprPtr init;              // null without an initialiser
        bool isRestrict = false;   // `float* restrict p`: accepted, and changes nothing for a local
        bool hasList = false;      // `= {...}`
        std::vector<ExprPtr> list;
    };

    // `expr;`: evaluated for its effect (an assignment, a call).
    struct ExprStmt
    {
        ExprPtr expr;
    };

    struct ReturnStmt
    {
        ExprPtr value;   // null for `return;`, a void function's
    };

    // `{ ... }`: a nested scope. Its declarations end with it, and may shadow outer ones.
    struct BlockStmt
    {
        std::vector<Stmt> items;
    };

    // `;`
    struct EmptyStmt
    {
    };

    using StmtPtr = std::unique_ptr<Stmt>;

    // `if (condition) thenBranch [else elseBranch]`. A float condition is `!= 0`, as in C.
    struct IfStmt
    {
        ExprPtr condition;
        StmtPtr thenBranch;
        StmtPtr elseBranch;   // null without an else
    };

    struct WhileStmt
    {
        ExprPtr condition;
        StmtPtr body;
    };

    // `for (init; condition; step) body`. The init's declarations are scoped to the loop; a missing condition is
    // true, so the loop runs until a break or a return.
    struct ForStmt
    {
        std::vector<Stmt> init;   // declarations, an expression statement, or nothing
        ExprPtr condition;        // null when omitted
        ExprPtr step;             // null when omitted
        StmtPtr body;
    };

    struct BreakStmt
    {
    };

    struct ContinueStmt
    {
    };

    struct Stmt
    {
        SourceLocation location;
        std::variant<DeclStmt, ExprStmt, ReturnStmt, BlockStmt, EmptyStmt, IfStmt, WhileStmt, ForStmt, BreakStmt,
                     ContinueStmt>
            node;
    };

    inline StmtPtr own(Stmt stmt)
    {
        return std::make_unique<Stmt>(std::move(stmt));
    }

    // A parameter is a variable of the function's outermost block, as in C: assignable unless const. A prototype may
    // leave its name out (empty). A restrict pointer is C99's promise that, while the function runs, what it reaches
    // is reached through no other pointer, so the optimiser may keep it in registers and vectorise across it (LLVM's
    // noalias).
    struct Param
    {
        SourceLocation location;
        std::string name;
        Type type = Type::Float;
        bool isConst = false;      // the parameter's own, as DeclStmt's
        bool isRestrict = false;
    };

    // A definition, or a prototype (`float f(float x);`, no body). As in C, a prototype declares a function defined
    // later or elsewhere: one with no definition in the program is external, resolved when the code is linked (the
    // JIT looks in the process, so `float atanf(float x);` calls libm's).
    // `[[clang::nonblocking]]` after a function's parameters: C23's attribute syntax, with clang's names. The tree
    // keeps an attribute as written; the code generator gives it its meaning (ir/Effects.cpp).
    struct Attribute
    {
        SourceLocation location;
        std::string name;   // "clang::nonblocking", or a name without its namespace
    };

    // A method is a function too: named `OnePole::tick`, with `module` set, and a first parameter the parser adds,
    // `this`, a pointer to the module (what C++ passes, and what a C++ host passes when it calls the method).
    struct Function
    {
        SourceLocation location;   // of its name
        std::string name;
        std::vector<Param> params;
        Type returnType = Type::Float;
        std::vector<Stmt> body;   // the outermost block's items: the same scope as the parameters
        bool isDefinition = true;
        std::vector<Attribute> attributes;
        const Module* module = nullptr;   // a method's
    };

    // A field of a module: a name and its type. It starts at 0, wherever the module is; a field cannot have an
    // initialiser or be const (the code generator says so), since nothing would run either.
    struct Field
    {
        SourceLocation location;
        std::string name;
        Type type = Type::Float;
        bool isConst = false;
        bool hasInitializer = false;
    };

    // `module OnePole { float y; float k; void init(float cutoff, float rate) { ... } float tick(float x) { ... } };`:
    // FMinus's class, C++'s struct with member functions. Its fields are laid out as a C struct's (the C++ host's
    // layout, too); its methods are Functions of the program, `OnePole::init` and `OnePole::tick`, which see the
    // fields by name (through `this`). Nothing runs when one is made: its fields start at 0, and a method such as
    // init sets them. The name is a type from the declaration on, as in C, so a module uses only modules declared
    // before it (and pointers to itself).
    struct Module
    {
        SourceLocation location;
        std::string name;
        std::vector<Field> fields;
        std::vector<std::string> methods;   // their names inside the module: tick for OnePole::tick
        std::size_t position = 0;           // how many of the program's functions come before it, for dump()
    };

    // The modules are kept by pointer, which a Type holds: they stay where they are while the program is built.
    struct Program
    {
        std::vector<Function> functions;   // with the modules' methods, in source order
        std::vector<std::unique_ptr<Module>> modules;
    };

    template <typename Node>
    ExprPtr makeExpr(SourceLocation location, Node node)
    {
        return std::make_unique<Expr>(Expr{location, std::move(node)});
    }

    // The type's name, as clang writes it in its messages: `float`, `poly float`, `const float *`.
    std::string typeName(Type type);

    // The tree as s-expressions, one function after another, for `fminus --emit ast` and tests:
    //     (function float gain ((float x) (const float g))
    //       (decl float y (* x g))
    //       (*= y 2)
    //       (return y))
    std::string dump(const Program& program);
} // namespace fminus::ast
