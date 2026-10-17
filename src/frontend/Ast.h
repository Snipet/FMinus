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
    // The value types. float is IEEE single precision (LLVM's float); it is the only type so far.
    enum class Type
    {
        Float,
    };

    enum class UnaryOp
    {
        Negate,   // -x
        Plus,     // +x: the value itself
    };

    enum class BinaryOp
    {
        Add,
        Subtract,
        Multiply,
        Divide,
        Remainder,
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
        std::variant<NumberExpr, VariableExpr, UnaryExpr, BinaryExpr, CallExpr, AssignExpr> node;
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

    struct Stmt
    {
        SourceLocation location;
        std::variant<DeclStmt, ExprStmt, ReturnStmt, BlockStmt, EmptyStmt> node;
    };

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
