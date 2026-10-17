// src/frontend/Ast.cpp
#include "frontend/Ast.h"

namespace fminus::ast
{
    namespace
    {
        template <typename... Fs>
        struct Overloaded : Fs...
        {
            using Fs::operator()...;
        };

        const char* opName(BinaryOp op)
        {
            switch (op)
            {
            case BinaryOp::Add:
                return "+";
            case BinaryOp::Subtract:
                return "-";
            case BinaryOp::Multiply:
                return "*";
            case BinaryOp::Divide:
                return "/";
            case BinaryOp::Remainder:
                return "%";
            }
            return "?";
        }

        const char* opName(AssignOp op)
        {
            switch (op)
            {
            case AssignOp::Assign:
                return "=";
            case AssignOp::Add:
                return "+=";
            case AssignOp::Subtract:
                return "-=";
            case AssignOp::Multiply:
                return "*=";
            case AssignOp::Divide:
                return "/=";
            case AssignOp::Remainder:
                return "%=";
            }
            return "?";
        }

        const char* opName(UnaryOp op)
        {
            switch (op)
            {
            case UnaryOp::Negate:
                return "-";
            case UnaryOp::Plus:
                return "+";
            }
            return "?";
        }

        std::string dump(const Expr& expr)
        {
            return std::visit(Overloaded{
                                  [](const NumberExpr& number) { return number.spelling; },
                                  [](const VariableExpr& variable) { return variable.name; },
                                  [](const UnaryExpr& unary) {
                                      return std::string("(") + opName(unary.op) + " " + dump(*unary.operand) + ")";
                                  },
                                  [](const BinaryExpr& binary) {
                                      return std::string("(") + opName(binary.op) + " " + dump(*binary.lhs) + " "
                                             + dump(*binary.rhs) + ")";
                                  },
                                  [](const CallExpr& call) {
                                      std::string text = "(call " + call.callee;
                                      for (const ExprPtr& arg : call.args)
                                          text += " " + dump(*arg);
                                      return text + ")";
                                  },
                                  [](const AssignExpr& assign) {
                                      return std::string("(") + opName(assign.op) + " " + dump(*assign.target) + " "
                                             + dump(*assign.value) + ")";
                                  },
                              },
                              expr.node);
        }

        // One statement per line, `indent` spaces in; a block's items one level further in.
        std::string dump(const Stmt& stmt, const std::string& indent)
        {
            return std::visit(Overloaded{
                                  [&](const DeclStmt& decl) {
                                      return indent + "(decl " + (decl.isConst ? "const " : "") + typeName(decl.type)
                                             + " " + decl.name + (decl.init ? " " + dump(*decl.init) : "") + ")";
                                  },
                                  [&](const ExprStmt& statement) { return indent + dump(*statement.expr); },
                                  [&](const ReturnStmt& ret) { return indent + "(return " + dump(*ret.value) + ")"; },
                                  [&](const BlockStmt& block) {
                                      std::string text = indent + "(block";
                                      for (const Stmt& item : block.items)
                                          text += "\n" + dump(item, indent + "  ");
                                      return text + ")";
                                  },
                                  [&](const EmptyStmt&) { return indent + "(empty)"; },
                              },
                              stmt.node);
        }
    } // namespace

    const char* typeName(Type type)
    {
        switch (type)
        {
        case Type::Float:
            return "float";
        }
        return "?";
    }

    std::string dump(const Program& program)
    {
        std::string text;
        for (const Function& function : program.functions)
        {
            text += std::string(function.isDefinition ? "(function " : "(declare ") + typeName(function.returnType)
                    + " " + function.name + " (";
            for (std::size_t i = 0; i < function.params.size(); ++i)
            {
                const Param& param = function.params[i];
                text += std::string(i == 0 ? "(" : " (") + (param.isConst ? "const " : "") + typeName(param.type)
                        + (param.name.empty() ? "" : " " + param.name) + ")";
            }
            text += ")";
            for (const Stmt& stmt : function.body)
                text += "\n" + dump(stmt, "  ");
            text += ")\n";
        }
        return text;
    }
} // namespace fminus::ast
