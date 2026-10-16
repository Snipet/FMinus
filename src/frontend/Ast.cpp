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
            }
            return "?";
        }

        std::string dump(const Expr& expr)
        {
            return std::visit(Overloaded{
                                  [](const NumberExpr& number) { return number.spelling; },
                                  [](const VariableExpr& variable) { return variable.name; },
                                  [](const UnaryExpr& unary) {
                                      switch (unary.op)
                                      {
                                      case UnaryOp::Negate:
                                          return "(- " + dump(*unary.operand) + ")";
                                      }
                                      return std::string("(?)");
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
                              },
                              expr.node);
        }

        std::string dump(const Stmt& stmt)
        {
            return std::visit(Overloaded{
                                  [](const LetStmt& let) { return "(let " + let.name + " " + dump(*let.value) + ")"; },
                                  [](const ReturnStmt& ret) { return "(return " + dump(*ret.value) + ")"; },
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
            text += "(fn " + function.name + " (";
            for (std::size_t i = 0; i < function.params.size(); ++i)
            {
                const Param& param = function.params[i];
                text += (i == 0 ? "(" : " (") + param.name + " " + typeName(param.type) + ")";
            }
            text += std::string(") ") + typeName(function.returnType);
            for (const Stmt& stmt : function.body)
                text += "\n  " + dump(stmt);
            text += ")\n";
        }
        return text;
    }
} // namespace fminus::ast
