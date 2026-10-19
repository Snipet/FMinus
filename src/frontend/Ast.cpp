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
            case BinaryOp::Less:
                return "<";
            case BinaryOp::Greater:
                return ">";
            case BinaryOp::LessEqual:
                return "<=";
            case BinaryOp::GreaterEqual:
                return ">=";
            case BinaryOp::Equal:
                return "==";
            case BinaryOp::NotEqual:
                return "!=";
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
            case UnaryOp::Not:
                return "!";
            }
            return "?";
        }

        std::string dump(const Expr& expr)
        {
            return std::visit(Overloaded{
                                  [](const NumberExpr& number) { return number.spelling; },
                                  [](const VariableExpr& variable) { return variable.name; },
                                  [](const BoolExpr& constant) {
                                      return std::string(constant.value ? "true" : "false");
                                  },
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
                                  [](const LogicalExpr& logical) {
                                      return std::string(logical.op == LogicalOp::And ? "(&& " : "(|| ")
                                             + dump(*logical.lhs) + " " + dump(*logical.rhs) + ")";
                                  },
                                  [](const ConditionalExpr& conditional) {
                                      return "(?: " + dump(*conditional.condition) + " " + dump(*conditional.thenValue)
                                             + " " + dump(*conditional.elseValue) + ")";
                                  },
                                  [](const ConstructExpr& construct) {   // (float4 a b), or (float4 {a b}) for C's
                                      std::string elements;
                                      for (const ExprPtr& arg : construct.args)
                                          elements += (elements.empty() ? "" : " ") + dump(*arg);
                                      if (construct.braces)
                                          return std::string("(") + typeName(construct.type) + " {" + elements + "})";
                                      return std::string("(") + typeName(construct.type)
                                             + (elements.empty() ? "" : " " + elements) + ")";
                                  },
                                  [](const SwizzleExpr& swizzle) {
                                      return "(. " + dump(*swizzle.base) + " " + swizzle.components + ")";
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
                                  [&](const IfStmt& branch) {
                                      std::string text = indent + "(if " + dump(*branch.condition) + "\n"
                                                         + dump(*branch.thenBranch, indent + "  ");
                                      if (branch.elseBranch)
                                          text += "\n" + dump(*branch.elseBranch, indent + "  ");
                                      return text + ")";
                                  },
                                  [&](const WhileStmt& loop) {
                                      return indent + "(while " + dump(*loop.condition) + "\n"
                                             + dump(*loop.body, indent + "  ") + ")";
                                  },
                                  [&](const ForStmt& loop) {
                                      std::string init;
                                      for (const Stmt& item : loop.init)
                                          init += (init.empty() ? "" : " ") + dump(item, "");
                                      return indent + "(for (" + init + ") "
                                             + (loop.condition ? dump(*loop.condition) : "()") + " "
                                             + (loop.step ? dump(*loop.step) : "()") + "\n"
                                             + dump(*loop.body, indent + "  ") + ")";
                                  },
                                  [&](const BreakStmt&) { return indent + "(break)"; },
                                  [&](const ContinueStmt&) { return indent + "(continue)"; },
                              },
                              stmt.node);
        }
    } // namespace

    const char* typeName(Type type)
    {
        switch (type.kind)
        {
        case Kind::Float:
            return "float";
        case Kind::Bool:
            return "bool";
        case Kind::Float2:
            return "float2";
        case Kind::Float4:
            return "float4";
        case Kind::Float8:
            return "float8";
        case Kind::Bool2:
            return "bool2";
        case Kind::Bool4:
            return "bool4";
        case Kind::Bool8:
            return "bool8";
        case Kind::PolyFloat:
            return "poly float";
        case Kind::PolyBool:
            return "poly bool";
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
