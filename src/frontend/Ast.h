// src/frontend/Ast.h: the syntax tree of FMinus v0, a subset of C. The parser (Parser.y) builds it, dump() prints it,
// and the code generator (ir/CodeGen.cpp) lowers it to LLVM IR.
//
// Every kind of expression and statement is one alternative of a std::variant. The code that walks the tree does it
// with std::visit and no default case, so adding an alternative makes every walk that does not handle it a compile
// error: the compiler lists what to update.
#pragma once

#include "frontend/Diagnostics.h"

#include <memory>
#include <optional>
#include <string>
#include <variant>
#include <vector>

namespace fminus::ast
{
    // The value types. float is IEEE single precision (LLVM's float). bool is C23's: 0 or 1, which converts to float
    // as 0 or 1, and to which a float converts as `x != 0` (a NaN is true).
    //
    // float2, float4 and float8 are OpenCL C's vector types: that many floats side by side, computed on together. A
    // float4 fills one 128-bit NEON register; a float8 is two. Comparing two of them gives a lane mask, a bool2, bool4
    // or bool8, which a program cannot declare (as in OpenCL C) but can select with or use as 0s and 1s.
    //
    // poly float is a float vector as wide as the target's vector registers: 4 lanes with NEON or SSE, 8 with AVX2,
    // 16 with AVX-512. A program is written without knowing how many (poly_lanes() is the number, a constant once the
    // target is known), so it means the same on every target. Comparing two gives a poly bool, which a program can
    // declare.
    enum class Type
    {
        Float,
        Bool,
        Float2,
        Float4,
        Float8,
        Bool2,
        Bool4,
        Bool8,
        PolyFloat,
        PolyBool,
    };

    inline bool isPoly(Type type)
    {
        return type == Type::PolyFloat || type == Type::PolyBool;
    }

    // A vector type's number of lanes; 1 for a scalar, and 0 for a poly type, whose lanes the target decides (the code
    // generator knows how many).
    inline unsigned lanes(Type type)
    {
        switch (type)
        {
        case Type::PolyFloat:
        case Type::PolyBool:
            return 0;
        case Type::Float:
        case Type::Bool:
            return 1;
        case Type::Float2:
        case Type::Bool2:
            return 2;
        case Type::Float4:
        case Type::Bool4:
            return 4;
        case Type::Float8:
        case Type::Bool8:
            return 8;
        }
        return 1;
    }

    inline bool isVector(Type type)
    {
        return lanes(type) > 1 || isPoly(type);
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

    // What a comparison of two values of `type` gives: a bool for floats, a lane mask for float vectors.
    inline Type maskOf(Type type)
    {
        switch (type)
        {
        case Type::Float2:
            return Type::Bool2;
        case Type::Float4:
            return Type::Bool4;
        case Type::Float8:
            return Type::Bool8;
        case Type::PolyFloat:
            return Type::PolyBool;
        default:
            return Type::Bool;
        }
    }

    // The type a value of `type` computes as: a bool as a float, a lane mask as the float vector of its size.
    inline Type arithmeticOf(Type type)
    {
        if (type == Type::Bool)
            return Type::Float;
        if (type == Type::PolyBool)
            return Type::PolyFloat;
        if (isMask(type))
            return *vectorOf(lanes(type));
        return type;
    }

    enum class UnaryOp
    {
        Negate,   // -x
        Plus,     // +x: the value itself
        Not,      // !x: a bool
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

    // A floating or integer constant as written, f suffix included. The code generator rounds it once, straight to
    // float: in FMinus every constant is a float (in C, an unsuffixed 1.5 would be a double).
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

    // `v.x`, `v.wzyx`, `v.s07`: OpenCL C's components. x, y, z, w name lanes 0 to 3, and s followed by hex digits names
    // any lane. One component is a float, two, four or eight a vector of them; a variable's components can be assigned.
    struct SwizzleExpr
    {
        ExprPtr base;
        std::string components;
    };

    struct Expr
    {
        SourceLocation location;
        std::variant<NumberExpr, VariableExpr, BoolExpr, UnaryExpr, BinaryExpr, LogicalExpr, CallExpr, AssignExpr,
                     ConditionalExpr, ConstructExpr, SwizzleExpr>
            node;
    };

    struct Stmt;

    // `[const] float name = init;`: a variable of the enclosing block, from its declaration to the block's end. One per
    // declarator: `float a = 1, b;` is two. Without an initialiser the variable starts at 0 (C leaves it
    // indeterminate, and reading it undefined).
    struct DeclStmt
    {
        bool isConst = false;
        Type type = Type::Float;
        std::string name;
        ExprPtr init;   // null without an initialiser
    };

    // `expr;`: evaluated for its effect (an assignment, a call).
    struct ExprStmt
    {
        ExprPtr expr;
    };

    struct ReturnStmt
    {
        ExprPtr value;
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
    // leave its name out (empty).
    struct Param
    {
        SourceLocation location;
        std::string name;
        Type type = Type::Float;
        bool isConst = false;
    };

    // A definition, or a prototype (`float f(float x);`, no body). As in C, a prototype declares a function defined
    // later or elsewhere: one with no definition in the program is external, resolved when the code is linked (the
    // JIT looks in the process, so `float tanhf(float x);` calls libm's).
    struct Function
    {
        SourceLocation location;   // of its name
        std::string name;
        std::vector<Param> params;
        Type returnType = Type::Float;
        std::vector<Stmt> body;   // the outermost block's items: the same scope as the parameters
        bool isDefinition = true;
    };

    struct Program
    {
        std::vector<Function> functions;
    };

    template <typename Node>
    ExprPtr makeExpr(SourceLocation location, Node node)
    {
        return std::make_unique<Expr>(Expr{location, std::move(node)});
    }

    // The type's name in FMinus source.
    const char* typeName(Type type);

    // The tree as s-expressions, one function after another, for `fminus --emit ast` and tests:
    //     (function float gain ((float x) (const float g))
    //       (decl float y (* x g))
    //       (*= y 2)
    //       (return y))
    std::string dump(const Program& program);
} // namespace fminus::ast
