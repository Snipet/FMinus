// src/ir/Strings.cpp: string literals. A literal is the argument of a builtin's string parameter (Param::String or
// Param::Format, Calls.cpp), and is then a private constant of the program, its bytes and no NUL after them, passed to
// the runtime as its address and its length, so the runtime never looks for an end and a literal may hold a NUL
// ("a\0b" is three bytes). A program that passes one means what it would in C, where the literal is a char array and
// the builtin a function of a const char* and a length. Where a value of FMinus's type string is wanted, a literal is
// one (Text.cpp). Anywhere else a literal would have C's meaning, a char array, which FMinus does not have, so it is an
// error, in clang's words for the conversion and where clang reports it when there is one (passing it to an int
// parameter, initialising a float with it, ...), and at the literal otherwise.
//
// A format is a literal with places for ints, checked as clang checks printf's (scanFormat, formatArguments): %d is
// the next int argument, %% a percent sign, and anything else after a % an error at it; too few ints is an error at
// the first % left without one, too many at the first int left over, and an argument that is no int an error at it.
// The runtime gets the literal as written (its escapes resolved) and the ints, and makes the text itself
// (runtime/Format.h). Every family of builtins that takes a path, a key or a URL with numbers in it checks it here.
//
// An address, a URL or a file's name, may name its values where they go instead: `{name}`, a hole, a variable in
// scope (scanHoles finds them; Calls.cpp's lowerHoles fills them), `{{` and `}}` a brace. The library gets each hole
// as a %d, or a %s for a string's, and its %% as it was: the literal's own percent signs reach it as themselves.
#include "ir/Lowering.h"

#include <llvm/IR/Constants.h>
#include <llvm/IR/GlobalVariable.h>

#include <algorithm>

namespace fminus::ir::detail
{
    namespace
    {
        // A byte as clang shows one in a message: itself when it is printable ASCII, else a hex escape.
        std::string shown(char c)
        {
            const auto byte = static_cast<unsigned char>(c);
            if (byte >= 0x20 && byte < 0x7f)
                return std::string(1, c);
            constexpr std::string_view kHex = "0123456789abcdef";
            return std::string("\\x") + kHex[byte >> 4] + kHex[byte & 15];
        }

        // C's type of a literal of `bytes` bytes: an array of them and the NUL C puts after them.
        std::string charArray(std::size_t bytes)
        {
            return "'char[" + std::to_string(bytes + 1) + "]'";
        }
    } // namespace

    // A format's places, read as clang reads printf's (its ParsePrintfSpecifier): after a %, flags, a width, a
    // precision and a length modifier, then the conversion. A format that ends before its conversion is clang's
    // "incomplete format specifier", at the %; a conversion C has no such specifier for, clang's "invalid conversion
    // specifier", at it. One C has that is not a plain %d or %% (%f, %5d, %ld, %s) has no meaning here: the same
    // words, of all it says after the %, at the byte after the %, and the note says what a format takes.
    FormatScan scanFormat(std::string_view text)
    {
        constexpr std::string_view kDigits = "0123456789";
        constexpr std::string_view kConversions = "diouxXfFeEgGaAcsSpnmbBC%";   // clang's, for printf on Linux
        FormatScan scan;
        for (std::size_t i = 0; i < text.size(); ++i)
        {
            if (text[i] != '%')
                continue;
            std::size_t j = i + 1;
            const auto skip = [&](std::string_view set) {
                while (j < text.size() && set.find(text[j]) != std::string_view::npos)
                    ++j;
            };
            const auto next = [&](std::string_view part) { return text.substr(j).starts_with(part); };
            skip("-+ #0'");   // the flags
            if (next("*"))   // the width
                ++j;
            else
                skip(kDigits);
            if (next("."))   // the precision
            {
                ++j;
                if (next("*"))
                    ++j;
                else
                    skip(kDigits);
            }
            if (next("hh") || next("ll"))   // the length modifier
                j += 2;
            else if (next("I64") || next("I32"))
                j += 3;
            else if (j < text.size() && std::string_view("hlqjztLIw").find(text[j]) != std::string_view::npos)
                ++j;
            if (j == text.size())
                scan.errors.push_back({i, "incomplete format specifier"});
            else if (j == i + 1 && text[j] == 'd')
                scan.places.push_back(i);
            else if (j == i + 1 && text[j] == '%')
                ;   // a percent sign
            else if (kConversions.find(text[j]) == std::string_view::npos)
                scan.errors.push_back({j, "invalid conversion specifier '" + shown(text[j]) + "'"});
            else
            {
                std::string specifier;
                for (std::size_t k = i + 1; k <= j; ++k)
                    specifier += shown(text[k]);
                scan.errors.push_back({i + 1, "invalid conversion specifier '" + specifier + "'"});
            }
            i = j;
        }
        return scan;
    }

    HoleScan scanHoles(std::string_view text)
    {
        HoleScan scan;
        const auto fail = [&](std::size_t offset, std::string message) {
            scan.errors.push_back({offset, std::move(message)});
            return scan;
        };
        const auto put = [&](char c, std::size_t offset) {
            scan.library += c;
            scan.offsets.push_back(offset);
        };
        const auto identifierByte = [](char c) {
            return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_';
        };
        std::optional<std::size_t> firstPercent;   // the literal's first %d, for the error at a hole after it
        for (std::size_t i = 0; i < text.size(); ++i)
        {
            const char c = text[i];
            if (c == '%' && i + 1 < text.size() && (text[i + 1] == '%' || text[i + 1] == 'd'))
            {
                if (text[i + 1] == 'd' && !scan.holes.empty())
                    return fail(i, "a literal names its values by holes, {name}, or by %d after it, not both: this "
                                   "one has a hole before");
                if (text[i + 1] == 'd' && !firstPercent)
                    firstPercent = i;
                scan.percent = scan.percent || text[i + 1] == 'd';
                put(c, i);
                put(text[i + 1], i + 1);
                ++i;
                continue;
            }
            if ((c == '{' || c == '}') && i + 1 < text.size() && text[i + 1] == c)
            {
                put(c, i);   // {{ and }}: a brace
                ++i;
                continue;
            }
            if (c == '}')
                return fail(i, "a '}' ends a hole, {name}, and this one ends none: a brace is written '}}'");
            if (c != '{')
            {
                put(c, i);
                continue;
            }
            const std::size_t close = text.find('}', i + 1);
            if (close == std::string_view::npos)
                return fail(i, "a '{' begins a hole, {name}, and this one has no '}': a brace is written '{{'");
            const std::string_view name = text.substr(i + 1, close - i - 1);
            if (name.empty())
                return fail(close, "a hole names a variable, {name}, and this one names none");
            if (name.front() >= '0' && name.front() <= '9')
                return fail(i + 1, "a hole names a variable, {name}, and a name does not begin with a digit");
            if (const auto bad = std::ranges::find_if_not(name, identifierByte); bad != name.end())
                return fail(i + 1 + static_cast<std::size_t>(bad - name.begin()),
                            "a hole names a variable, {name}: '" + shown(*bad) + "' is no part of a name");
            if (firstPercent)
                return fail(i, "a literal names its values by holes, {name}, or by %d after it, not both: this one "
                               "has a %d before");
            scan.holes.push_back({std::string(name), i, scan.library.size()});
            put('%', i);
            put('d', i);
            i = close;
        }
        return scan;
    }

    bool Lowering::isString(const ast::Expr& expr)
    {
        return std::holds_alternative<ast::StringExpr>(expr.node);
    }

    // The literal's bytes, a private constant of the module, as clang makes one (.str, unnamed_addr) but with no NUL
    // after them: its address. An empty literal is an array of none, whose address is still an address.
    llvm::Value* Lowering::stringConstant(const ast::StringExpr& literal)
    {
        llvm::Constant* bytes = llvm::ConstantDataArray::getString(module_.getContext(), literal.value, false);
        auto* global = new llvm::GlobalVariable(module_, bytes->getType(), true, llvm::GlobalValue::PrivateLinkage,
                                                bytes, ".str");
        global->setUnnamedAddr(llvm::GlobalValue::UnnamedAddr::Global);
        global->setAlignment(llvm::Align(1));
        return global;
    }

    // A format's ints, after it in a call: checked against its places (scanFormat), each converted to an int. Each
    // error is reported where clang reports printf's, and then there are none.
    std::optional<std::vector<llvm::Value*>> Lowering::formatArguments(const ast::StringExpr& literal,
                                                                       const std::vector<const ast::Expr*>& ints)
    {
        const FormatScan scan = scanFormat(literal.value);
        bool fine = scan.errors.empty();
        for (const FormatScan::Error& wrong : scan.errors)
            diagnostics_.error(literal.at[wrong.offset], wrong.message,
                               {{literal.at[wrong.offset], "a format here takes %d, an int, and %%, a percent sign",
                                 {}}});
        std::vector<llvm::Value*> values;
        for (const ast::Expr* arg : ints)
        {
            // C passes a variadic argument as it is, promoted: an int (an enumeration's, a bool's 0 or 1) is one, and
            // anything else is not what %d reads, clang's -Wformat, which FMinus refuses.
            if (isString(*arg))   // C's char array, passed as a char*
            {
                error(begin(*arg), "format specifies type 'int' but the argument has type 'char *'");
                fine = false;
                values.push_back(poison(ast::Type::Int).ir);
                continue;
            }
            const Value value = lower(*arg);
            const bool integer = value.type.kind == ast::Kind::Int || value.type == ast::Type::Bool;
            if (!value.failed && !integer)
            {
                error(begin(*arg), "format specifies type 'int' but the argument has type "
                                       + quoted(ast::typeName(value.type)));
                fine = false;
            }
            fine = fine && !value.failed;
            values.push_back(integer ? convert(value, ast::Type::Int, arg->location, Use::Passing)
                                     : poison(ast::Type::Int).ir);
        }
        if (ints.size() < scan.places.size() && scan.errors.empty())
        {
            error(literal.at[scan.places[ints.size()] + 1], "more '%' conversions than data arguments");   // at d
            fine = false;
        }
        else if (ints.size() > scan.places.size() && scan.errors.empty())   // clang counts one an error was about
        {
            error(begin(*ints[scan.places.size()]), "data argument not used by format string");
            fine = false;
        }
        if (!fine)
            return std::nullopt;
        return values;
    }

    // `expr`, a string literal, where a value of `type` goes (none: a place that says no type), as `use` takes it: the
    // error, in clang's words for its conversion where C would warn of it (to an int, to a pointer to another type:
    // errors in FMinus, as its own pointer conversions are) or refuse it (to a float), at `where`, where clang reports
    // the conversion (the declarator, the assignment's operator, the literal passed or returned), and its stand-in.
    // Where C would convert it without a word, to a void* or a bool, clang has no words to borrow, and the error, at
    // the literal, says what FMinus lacks.
    Lowering::Value Lowering::refuseString(const ast::Expr& expr, std::optional<ast::Type> type, Use use,
                                           SourceLocation where)
    {
        const auto& literal = std::get<ast::StringExpr>(expr.node);
        if (type == ast::Type::String)   // FMinus's string: no refusal at all (Text.cpp)
            return stringValue(literal);
        const bool phrased =
            use == Use::Initializing || use == Use::Assigning || use == Use::Returning || use == Use::Passing;
        if (!type || !phrased || *type == ast::Type::Void || *type == ast::Type::Bool
            || (ast::isPointer(*type) && type->pointee == ast::Kind::Void))
            return notAValue(expr);
        const std::string source = charArray(literal.value.size());
        const std::string target = quoted(ast::typeName(*type));
        if (type->kind == ast::Kind::Int || ast::isPointer(*type))
        {
            // clang's -Wint-conversion and -Wincompatible-pointer-types, errors here as FMinus's own pointer
            // conversions are (Types.cpp, pointerMismatch)
            std::string what;
            switch (use)
            {
            case Use::Initializing:
                what = "initializing " + target + " with an expression of type " + source;
                break;
            case Use::Assigning:
                what = "assigning to " + target + " from " + source;
                break;
            case Use::Returning:
                what = "returning " + source + " from a function with result type " + target;
                break;
            case Use::Passing:
                what = "passing " + source + " to parameter of type " + target;
                break;
            case Use::Condition:   // not phrased: above
            case Use::Logical:
            case Use::Casting:
                break;
            }
            error(where, (ast::isPointer(*type) ? "incompatible pointer types " : "incompatible pointer to "
                                                                                           "integer conversion ")
                                     + what);
            return recovery(*type);
        }
        switch (use)
        {
        case Use::Initializing:
            error(where, "initializing " + target + " with an expression of incompatible type " + source);
            break;
        case Use::Assigning:
            error(where, "assigning to " + target + " from incompatible type " + source);
            break;
        case Use::Returning:
            error(where, "returning " + source + " from a function with incompatible result type " + target);
            break;
        case Use::Passing:
            error(where, "passing " + source + " to parameter of incompatible type " + target);
            break;
        case Use::Condition:
        case Use::Logical:
        case Use::Casting:
            break;
        }
        return recovery(*type);
    }

    // A string literal where FMinus has nothing to make of it: an operand, a condition, a subscript, an argument of a
    // builtin that takes no string there, a void* or a bool. C would have a char array; FMinus has none.
    Lowering::Value Lowering::notAValue(const ast::Expr& expr)
    {
        error(expr.location, "FMinus has no char arrays here: a string literal is a 'string' where one is wanted, or "
                             "the argument of a builtin's string parameter");
        return recovery(ast::Type::Float);
    }
} // namespace fminus::ir::detail
