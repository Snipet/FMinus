// src/frontend/Ast.cpp
#include "frontend/Ast.h"

#include <algorithm>
#include <array>
#include <cerrno>
#include <cstdlib>
#include <utility>

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
            case BinaryOp::BitAnd:
                return "&";
            case BinaryOp::BitOr:
                return "|";
            case BinaryOp::BitXor:
                return "^";
            case BinaryOp::ShiftLeft:
                return "<<";
            case BinaryOp::ShiftRight:
                return ">>";
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
            case AssignOp::BitAnd:
                return "&=";
            case AssignOp::BitOr:
                return "|=";
            case AssignOp::BitXor:
                return "^=";
            case AssignOp::ShiftLeft:
                return "<<=";
            case AssignOp::ShiftRight:
                return ">>=";
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
            case UnaryOp::Complement:
                return "~";
            }
            return "?";
        }

        std::string declared(Type type, bool isConst, bool isRestrict, const Expr* size = nullptr,
                             const Expr* rowSize = nullptr);
        std::string stringLiteral(const std::string& text);

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
                                  [](const SizeofExpr& size) {   // (sizeof float *), (sizeof float[4][(* 2 N)])
                                      if (isArray(size.type))
                                          return "(sizeof " + declared(size.type, false, false, size.size.get(),
                                                                       size.rowSize.get())
                                                 + ")";
                                      return "(sizeof " + typeName(size.type) + ")";
                                  },
                                  [](const IndexExpr& index) {   // ([] p i)
                                      return "([] " + dump(*index.base) + " " + dump(*index.index) + ")";
                                  },
                                  [](const CastExpr& cast) {   // (cast int x)
                                      return std::string("(cast ") + typeName(cast.type) + " " + dump(*cast.operand)
                                             + ")";
                                  },
                                  [](const StringExpr& string) {   // "a\0b", one literal however written
                                      return stringLiteral(string.value);
                                  },
                              },
                              expr.node);
        }

        // A declared name's type, as C spells it: `const float`, `const float * restrict`, `float * const`,
        // `const float[16]`. An array's size is as written, `float[(* ROWS COLS)]`, or nothing for `float[]`, and a
        // 2-D array's rows' after it, `float[4][16]`.
        std::string declared(Type type, bool isConst, bool isRestrict, const Expr* size, const Expr* rowSize)
        {
            if (isArray(type))
                return (isConst ? "const " : "") + typeName(elementOf(type)) + "["
                       + (size != nullptr ? dump(*size) : "") + "]"
                       + (hasRows(type) ? "[" + (rowSize != nullptr ? dump(*rowSize) : "") + "]" : "");
            if (!isPointer(type))
                return (isConst ? "const " : "") + typeName(type);
            return typeName(type) + (isConst ? " const" : "") + (isRestrict ? " restrict" : "");
        }

        // A string as a C string literal: "dB", a quote, a backslash and a control character escaped ("a\"b",
        // "\n", "\001"), any other byte as it is (UTF-8's too).
        std::string stringLiteral(const std::string& text)
        {
            std::string literal = "\"";
            for (const char c : text)
            {
                const auto byte = static_cast<unsigned char>(c);
                if (c == '"' || c == '\\')
                    literal += std::string("\\") + c;
                else if (c == '\n')
                    literal += "\\n";
                else if (c == '\t')
                    literal += "\\t";
                else if (byte < 0x20 || byte == 0x7f)
                {
                    literal += '\\';
                    literal += static_cast<char>('0' + (byte >> 6));
                    literal += static_cast<char>('0' + ((byte >> 3) & 7));
                    literal += static_cast<char>('0' + (byte & 7));
                }
                else
                    literal += c;
            }
            return literal + "\"";
        }

        // [[fminus::range(20, 20000)]], [[clang::nonblocking]], [[ui::unit("dB")]]
        std::string attributeText(const Attribute& attribute)
        {
            std::string args;
            for (const AttributeArg& arg : attribute.args)
                args += (args.empty() ? "" : ", ") + (arg.isString ? stringLiteral(arg.text) : arg.text);
            return "[[" + attribute.name + (attribute.args.empty() ? "" : "(" + args + ")") + "]]";
        }

        std::string attributesText(const std::vector<Attribute>& attributes)
        {
            std::string text;
            for (const Attribute& attribute : attributes)
                text += " " + attributeText(attribute);
            return text;
        }

        // The values of an initialiser list from `first` to `end`, the braces in them that are `depth` + 1 deep each
        // in its own, from `brace` on (the braces are in the order their `{`s are, so a brace's own come after it).
        std::string listed(const std::vector<ExprPtr>& elements, const std::vector<Brace>& braces, std::size_t first,
                           std::size_t end, int depth, std::size_t& brace)
        {
            std::string text;
            const auto add = [&](const std::string& piece) { text += (text.empty() ? "" : " ") + piece; };
            for (std::size_t i = first;;)
            {
                if (brace < braces.size() && braces[brace].depth == depth + 1 && braces[brace].first == i
                    && i + braces[brace].count <= end)
                {
                    const Brace& inner = braces[brace++];
                    add("{" + listed(elements, braces, i, i + inner.count, depth + 1, brace) + "}");
                    i += inner.count;
                    continue;
                }
                if (i >= end)
                    return text;
                add(dump(*elements[i++]));
            }
        }

        // An array's initialiser list: {1 2}, and a 2-D array's rows braced, {{1 2} {3}}.
        std::string list(const std::vector<ExprPtr>& elements, const std::vector<Brace>& braces)
        {
            std::size_t brace = 0;
            return "{" + listed(elements, braces, 0, elements.size(), 0, brace) + "}";
        }

        // (enum Mode (SAW) (SQUARE 4) (TRIANGLE)): each constant, with its value as written if it has one; C23's fixed
        // underlying type after the tag, (enum Mode : int (SAW)).
        std::string dump(const Enum& enumeration)
        {
            std::string text = "(enum" + (enumeration.tag.empty() ? "" : " " + enumeration.tag);
            if (enumeration.fixed)
                text += " : " + typeName(enumeration.underlying);
            for (const Enumerator& enumerator : enumeration.enumerators)
                text += " (" + enumerator.name + (enumerator.value ? " " + dump(*enumerator.value) : "") + ")";
            return text + ")";
        }

        // (decl const float[4] t {1 2}), a block's or the top level's.
        std::string dump(const DeclStmt& decl, const std::string& indent)
        {
            return indent + "(decl "
                   + declared(decl.type, decl.isConst, decl.isRestrict, decl.size.get(), decl.rowSize.get()) + " "
                   + decl.name + attributesText(decl.attributes) + (decl.init ? " " + dump(*decl.init) : "")
                   + (decl.hasList ? " " + list(decl.list, decl.braces) : "") + ")";
        }

        std::string dump(const Stmt& stmt, const std::string& indent);

        // A selection statement's declarations (C2y's), each followed by a space: `(decl int n 1) `.
        std::string declarations(const std::vector<Stmt>& init)
        {
            std::string text;
            for (const Stmt& item : init)
                text += dump(item, "") + " ";
            return text;
        }

        // One statement per line, `indent` spaces in; a block's items one level further in.
        std::string dump(const Stmt& stmt, const std::string& indent)
        {
            return std::visit(Overloaded{
                                  [&](const DeclStmt& decl) { return dump(decl, indent); },
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
                                  [&](const IfStmt& branch) {   // (if (decl int n 1) (> n 0)\n  s): C2y's head
                                      std::string text = indent + "(if " + declarations(branch.init)
                                                         + dump(*branch.condition) + "\n"
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
                                  [&](const SwitchStmt& branch) {   // (switch x\n  (block ...))
                                      return indent + "(switch " + declarations(branch.init) + dump(*branch.value)
                                             + "\n"
                                             + dump(*branch.body, indent + "  ") + ")";
                                  },
                                  [&](const CaseStmt& label) {   // (case 1), or (case 1\n  s) with a body
                                      return indent + "(case " + dump(*label.value)
                                             + (label.body ? "\n" + dump(*label.body, indent + "  ") : "") + ")";
                                  },
                                  [&](const DefaultStmt& label) {
                                      return indent + "(default"
                                             + (label.body ? "\n" + dump(*label.body, indent + "  ") : "") + ")";
                                  },
                                  [&](const EnumStmt& statement) { return indent + dump(statement.enumeration); },
                                  [&](const ParallelForStmt& loop) {
                                      std::string init;
                                      for (const Stmt& item : loop.init)
                                          init += (init.empty() ? "" : " ") + dump(item, "");
                                      return indent + "(parallel-for (" + init + ") "
                                             + (loop.condition ? dump(*loop.condition) : "()") + " "
                                             + (loop.step ? dump(*loop.step) : "()") + "\n"
                                             + dump(*loop.body, indent + "  ") + ")";
                                  },
                                  [&](const ParallelBlockStmt& block) {
                                      std::string text = indent + "(parallel";
                                      for (const Stmt& task : block.tasks)
                                          text += "\n" + dump(task, indent + "  ");
                                      return text + ")";
                                  },
                                  [&](const DeferStmt& deferred) {   // (defer\n  s)
                                      return indent + "(defer\n" + dump(*deferred.body, indent + "  ") + ")";
                                  },
                              },
                              stmt.node);
        }
    } // namespace

    std::optional<double> attributeNumber(std::string spelling)
    {
        const bool hex = spelling.find("0x") != std::string::npos || spelling.find("0X") != std::string::npos;
        // An f suffix: a hex float's follows its exponent, whose digits are decimal (0x1p-3f); in 0x1F, F is a digit.
        if ((!hex || spelling.find_first_of("pP") != std::string::npos) && !spelling.empty()
            && (spelling.back() == 'f' || spelling.back() == 'F'))
            spelling.pop_back();
        const bool floating = spelling.find_first_of(hex ? ".pP" : ".eE") != std::string::npos;
        const bool negative = spelling.starts_with('-');
        const char* text = spelling.c_str();
        if (spelling.size() == (negative ? 1u : 0u))
            return std::nullopt;
        char* end = nullptr;
        double value = 0;
        if (floating)
            value = std::strtod(text, &end);
        else
        {
            // An integer's digits, its minus apart, fit C's widest type, unsigned long long, or it is none: clang
            // refuses "integer literal is too large to be represented in any integer type", where strtoull saturates.
            errno = 0;
            const unsigned long long magnitude = std::strtoull(text + (negative ? 1 : 0), &end, 0);
            if (errno == ERANGE)
                return std::nullopt;
            value = negative ? 0.0 - static_cast<double>(magnitude) : static_cast<double>(magnitude);   // -0 is 0
        }
        if (end != text + spelling.size())
            return std::nullopt;
        return value;
    }

    bool isIgnoredAttribute(std::string_view name)
    {
        if (name.find("::") != std::string_view::npos)
            return false;
        if (name.size() > 4 && name.starts_with("__") && name.ends_with("__"))
            name = name.substr(2, name.size() - 4);   // [[__deprecated__]] is [[deprecated]]
        constexpr std::array<std::string_view, 7> kStandard{"deprecated", "fallthrough",  "maybe_unused", "nodiscard",
                                                            "noreturn",   "reproducible", "unsequenced"};
        return std::ranges::find(kStandard, name) == kStandard.end();
    }

    std::string typeName(Type type)
    {
        switch (type.kind)
        {
        case Kind::Void:
            return "void";
        case Kind::Float:
            return "float";
        case Kind::Double:
            return "double";
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
        case Kind::Pointer:   // a pointer to a row in clang's spelling: float (*)[16]
            if (hasRows(type))
                return (type.pointeeConst ? "const " : "") + typeName(elementOf(type)) + " (*)["
                       + std::to_string(type.rowCount) + "]";
            return (type.pointeeConst ? "const " : "") + typeName(pointee(type)) + " *";
        case Kind::Module:
            return type.module != nullptr ? type.module->name : "?";
        case Kind::Array:   // clang's spelling: float[16], float[4][16]
            if (hasRows(type))
                return typeName(elementOf(type)) + "[" + std::to_string(type.count) + "]["
                       + std::to_string(type.rowCount) + "]";
            return typeName(pointee(type)) + "[" + std::to_string(type.count) + "]";
        case Kind::Handle:   // its typedef's name, as clang writes one: http_response
            return handleName(type.handle);
        }
        return "?";
    }

    const char* handleName(Handle handle)
    {
        switch (handle)
        {
        case Handle::HttpResponse:
            return "http_response";
        case Handle::HttpRequest:
            return "http_request";
        case Handle::HttpServer:
            return "http_server";
        case Handle::JsonDoc:
            return "json_doc";
        case Handle::XmlDoc:
            return "xml_doc";
        case Handle::FileContent:
            return "file_content";
        case Handle::FileListing:
            return "file_listing";
        case Handle::FftPlan:
            return "fft_plan";
        case Handle::None:
            break;
        }
        return "?";
    }

    std::string handleStruct(Handle handle)
    {
        return std::string("struct fminus_") + handleName(handle);
    }

    std::string dump(const Program& program)
    {
        // A module as its enumerations and its fields, in order: (module OnePole (field float y) (field float k)).
        // Its methods follow it, as the functions they are: (method float OnePole::tick ((float x)) ...), without the
        // `this` the parser added. An enumeration or a constant at the top level comes before the modules declared
        // after it, and a constant before the enumerations declared after it.
        const auto declarationsAt = [&](std::size_t position, std::size_t modules) {
            std::vector<std::pair<std::size_t, std::string>> items;   // in source order, by 2 * the enumerations before
            for (std::size_t e = 0; e < program.enums.size(); ++e)
            {
                const Enum& enumeration = program.enums[e];
                if (enumeration.module == nullptr && enumeration.position == position && enumeration.modules == modules)
                    items.emplace_back(2 * e + 1, dump(enumeration) + "\n");
            }
            for (const Constant& constant : program.constants)
            {
                if (constant.position == position && constant.modules == modules)
                    items.emplace_back(2 * constant.enums, dump(constant.declaration, "") + "\n");
            }
            std::ranges::stable_sort(items, {}, [](const auto& item) { return item.first; });
            std::string text;
            for (const auto& item : items)
                text += item.second;
            return text;
        };
        const auto modulesAt = [&](std::size_t position) {
            std::string text;
            for (std::size_t m = 0; m <= program.modules.size(); ++m)
            {
                text += declarationsAt(position, m);   // the ones after m modules
                if (m == program.modules.size() || program.modules[m]->position != position)
                    continue;
                const Module* module = program.modules[m].get();
                text += "(module " + module->name;
                std::vector<const Enum*> enums;   // the module's
                for (const Enum& enumeration : program.enums)
                {
                    if (enumeration.module == module)
                        enums.push_back(&enumeration);
                }
                std::size_t shown = 0;
                for (const Field& field : module->fields)
                {
                    for (; shown < field.enums && shown < enums.size(); ++shown)
                        text += "\n  " + dump(*enums[shown]);
                    text += std::string(field.isParam ? "\n  (param " : "\n  (field ")
                            + declared(field.type, field.isConst, false, field.size.get(), field.rowSize.get()) + " "
                            + field.name + attributesText(field.attributes) + ")";
                }
                for (; shown < enums.size(); ++shown)
                    text += "\n  " + dump(*enums[shown]);
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
            text += ")" + attributesText(function.attributes);
            for (const Stmt& stmt : function.body)
                text += "\n" + dump(stmt, "  ");
            text += ")\n";
        }
        return text + modulesAt(program.functions.size());
    }
} // namespace fminus::ast
