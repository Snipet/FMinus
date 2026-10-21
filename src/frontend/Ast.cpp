// src/frontend/Ast.cpp
#include "frontend/Ast.h"

#include <cstdlib>

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

        const char* opName(IncrementOp op)
        {
            switch (op)
            {
            case IncrementOp::PreIncrement:
                return "pre++";
            case IncrementOp::PreDecrement:
                return "pre--";
            case IncrementOp::PostIncrement:
                return "post++";
            case IncrementOp::PostDecrement:
                return "post--";
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
            case UnaryOp::Dereference:
                return "*";
            case UnaryOp::AddressOf:
                return "&";
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
                                  [](const MemberExpr& member) {   // (. v xy), (-> p y)
                                      return std::string(member.arrow ? "(-> " : "(. ") + dump(*member.base) + " "
                                             + member.name + ")";
                                  },
                                  [](const MethodCallExpr& call) {   // (call. lp tick x), (call-> p tick x)
                                      std::string text = std::string(call.arrow ? "(call-> " : "(call. ")
                                                         + dump(*call.object) + " " + call.method;
                                      for (const ExprPtr& arg : call.args)
                                          text += " " + dump(*arg);
                                      return text + ")";
                                  },
                                  [](const ThisExpr&) { return std::string("this"); },
                                  [](const AssignExpr& assign) {
                                      return std::string("(") + opName(assign.op) + " " + dump(*assign.target) + " "
                                             + dump(*assign.value) + ")";
                                  },
                                  [](const IncrementExpr& increment) {   // (pre++ i), (post-- i)
                                      return std::string("(") + opName(increment.op) + " " + dump(*increment.target)
                                             + ")";
                                  },
                                  [](const SizeofExpr& size) {   // (sizeof float *)
                                      return "(sizeof " + typeName(size.type) + ")";
                                  },
                                  [](const IndexExpr& index) {   // ([] p i)
                                      return "([] " + dump(*index.base) + " " + dump(*index.index) + ")";
                                  },
                                  [](const CastExpr& cast) {   // (cast int x)
                                      return std::string("(cast ") + typeName(cast.type) + " " + dump(*cast.operand)
                                             + ")";
                                  },
                              },
                              expr.node);
        }

        // A declared name's type, as C spells it: `const float`, `const float * restrict`, `float * const`,
        // `const float[16]`.
        std::string declared(Type type, bool isConst, bool isRestrict)
        {
            if (!isPointer(type))
                return (isConst ? "const " : "") + typeName(type);
            return typeName(type) + (isConst ? " const" : "") + (isRestrict ? " restrict" : "");
        }

        // [[fminus::range(20, 20000)]], [[clang::nonblocking]]
        std::string attributeText(const Attribute& attribute)
        {
            std::string args;
            for (const std::string& arg : attribute.args)
                args += (args.empty() ? "" : ", ") + arg;
            return "[[" + attribute.name + (attribute.args.empty() ? "" : "(" + args + ")") + "]]";
        }

        // An array's initialiser list: {1 2}.
        std::string list(const std::vector<ExprPtr>& elements)
        {
            std::string text;
            for (const ExprPtr& element : elements)
                text += (text.empty() ? "" : " ") + dump(*element);
            return "{" + text + "}";
        }

        // One statement per line, `indent` spaces in; a block's items one level further in.
        std::string dump(const Stmt& stmt, const std::string& indent)
        {
            return std::visit(Overloaded{
                                  [&](const DeclStmt& decl) {
                                      return indent + "(decl " + declared(decl.type, decl.isConst, decl.isRestrict)
                                             + " " + decl.name + (decl.init ? " " + dump(*decl.init) : "")
                                             + (decl.hasList ? " " + list(decl.list) : "") + ")";
                                  },
                                  [&](const ExprStmt& statement) { return indent + dump(*statement.expr); },
                                  [&](const ReturnStmt& ret) {
                                      return indent + "(return" + (ret.value ? " " + dump(*ret.value) : "") + ")";
                                  },
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

    std::optional<double> attributeNumber(std::string spelling)
    {
        const bool hex = spelling.find("0x") != std::string::npos || spelling.find("0X") != std::string::npos;
        if (!hex && !spelling.empty() && (spelling.back() == 'f' || spelling.back() == 'F'))
            spelling.pop_back();
        const bool floating = spelling.find_first_of(hex ? ".pP" : ".eE") != std::string::npos;
        char* end = nullptr;
        const double value = floating ? std::strtod(spelling.c_str(), &end)
                                      : static_cast<double>(std::strtoll(spelling.c_str(), &end, 0));
        if (spelling.empty() || end != spelling.c_str() + spelling.size())
            return std::nullopt;
        return value;
    }

    std::string typeName(Type type)
    {
        switch (type.kind)
        {
        case Kind::Void:
            return "void";
        case Kind::Float:
            return "float";
        case Kind::Bool:
            return "bool";
        case Kind::Int:
            return "int";
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
        case Kind::Pointer:
            return (type.pointeeConst ? "const " : "") + typeName(pointee(type)) + " *";
        case Kind::Module:
            return type.module != nullptr ? type.module->name : "?";
        case Kind::Array:   // clang's spelling: float[16]
            return typeName(pointee(type)) + "[" + std::to_string(type.count) + "]";
        }
        return "?";
    }

    std::string dump(const Program& program)
    {
        // A module as its fields: (module OnePole (field float y) (field float k)). Its methods follow it, as the
        // functions they are: (method float OnePole::tick ((float x)) ...), without the `this` the parser added.
        const auto modulesAt = [&](std::size_t position) {
            std::string text;
            for (const std::unique_ptr<Module>& module : program.modules)
            {
                if (module->position != position)
                    continue;
                text += "(module " + module->name;
                for (const Field& field : module->fields)
                {
                    text += std::string(field.isParam ? "\n  (param " : "\n  (field ")
                            + declared(field.type, field.isConst, false) + " " + field.name;
                    for (const Attribute& attribute : field.attributes)
                        text += " " + attributeText(attribute);
                    text += ")";
                }
                text += ")\n";
            }
            return text;
        };
        std::string text;
        for (std::size_t f = 0; f < program.functions.size(); ++f)
        {
            text += modulesAt(f);
            const Function& function = program.functions[f];
            const char* kind = function.module != nullptr ? "(method "
                               : function.isDefinition    ? "(function "
                                                          : "(declare ";
            text += kind + typeName(function.returnType) + " " + function.name + " (";
            const std::size_t first = function.module != nullptr ? 1 : 0;   // `this`
            for (std::size_t i = first; i < function.params.size(); ++i)
            {
                const Param& param = function.params[i];
                text += std::string(i == first ? "(" : " (") + declared(param.type, param.isConst, param.isRestrict)
                        + (param.name.empty() ? "" : " " + param.name) + ")";
            }
            text += ")";
            for (const Attribute& attribute : function.attributes)
                text += " " + attributeText(attribute);
            for (const Stmt& stmt : function.body)
                text += "\n" + dump(stmt, "  ");
            text += ")\n";
        }
        return text + modulesAt(program.functions.size());
    }
} // namespace fminus::ast
