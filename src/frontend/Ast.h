// src/frontend/Ast.h: the syntax tree of FMinus v0, a subset of C. The parser (Parser.y) builds it, dump() prints it,
// and the code generator (ir/CodeGen.cpp) lowers it to LLVM IR.
//
// Every kind of expression and statement is one alternative of a std::variant. The code that walks the tree does it
// with std::visit and no default case, so adding an alternative makes every walk that does not handle it a compile
// error: the compiler lists what to update.
#pragma once

#include "frontend/Diagnostics.h"

#include <memory>
#include <string>
#include <variant>
#include <vector>

namespace fminus::ast
{
    // The value types. float is IEEE single precision (LLVM's float). bool is C23's: 0 or 1, which converts to float
    // as 0 or 1, and to which a float converts as `x != 0` (a NaN is true).
    enum class Type
    {
        Float,
        Bool,
    };

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

    struct Expr
    {
        SourceLocation location;
        std::variant<NumberExpr, VariableExpr, BoolExpr, UnaryExpr, BinaryExpr, LogicalExpr, CallExpr, AssignExpr,
                     ConditionalExpr>
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
