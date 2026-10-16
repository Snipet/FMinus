// src/frontend/Ast.h: the syntax tree of FMinus v0. The parser (Parser.y) builds it, dump() prints it, and the code
// generator (ir/CodeGen.cpp) lowers it to LLVM IR.
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
        Negate,
    };

    enum class BinaryOp
    {
        Add,
        Subtract,
        Multiply,
        Divide,
    };

    struct Expr;
    using ExprPtr = std::unique_ptr<Expr>;

    // A literal as written. The code generator rounds it once, straight to the target type.
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

    struct Expr
    {
        SourceLocation location;
        std::variant<NumberExpr, VariableExpr, UnaryExpr, BinaryExpr, CallExpr> node;
    };

    // `let name = value;` binds a name for the rest of the function. Bindings never change, so a binding is the SSA
    // value itself. Assignment and loops would need storage (allocas, which mem2reg turns back into SSA).
    struct LetStmt
    {
        std::string name;
        ExprPtr value;
    };

    struct ReturnStmt
    {
        ExprPtr value;
    };

    struct Stmt
    {
        SourceLocation location;
        std::variant<LetStmt, ReturnStmt> node;
    };

    struct Param
    {
        SourceLocation location;
        std::string name;
        Type type = Type::Float;
    };

    struct Function
    {
        SourceLocation location;
        std::string name;
        std::vector<Param> params;
        Type returnType = Type::Float;
        std::vector<Stmt> body;
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
    //     (fn gain ((x float) (g float)) float
    //       (return (* x g)))
    std::string dump(const Program& program);
} // namespace fminus::ast
