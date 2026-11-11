// src/frontend/Preprocessor.cpp: an #if's line to its value. The line is lexed into C's preprocessing tokens (numbers,
// names, punctuators), its macros are expanded, and the tokens are parsed by precedence climbing over C's binary
// operators, with ?: and the unary operators by recursive descent. Arithmetic is on 64-bit integers, as C's intmax_t,
// and defined for every operand: + - * and unary - wrap, a quotient of the least integer by -1 is itself (its
// remainder 0), and a shift takes its count's low six bits (OpenCL C's rule, as FMinus's code shifts an int by its
// count's low five), the right one arithmetic.
#include "frontend/Preprocessor.h"

#include "fminus/Config.h"   // FMINUS_HAS_JSON, FMINUS_HAS_XML

#include <algorithm>
#include <array>
#include <limits>
#include <optional>
#include <utility>
#include <vector>

namespace fminus::frontend
{
    namespace
    {
        constexpr std::size_t kMaxTokens = 65536;   // an #if's tokens after expansion: `#define B A A` doubles them
        constexpr int kMaxDepth = 256;              // nested parentheses, operators or macros; clang's bracket depth

        // The work of expanding macros in a compilation's conditions, all of them: the bytes of the values expanded,
        // and one for each macro, so that one whose value is empty counts too. kMaxTokens bounds a line's tokens, but
        // `#define C0`, then C1 a thousand C0s, C2 a thousand C1s and C3 a thousand C2s, makes no token at all from
        // a billion expansions; and a line under both bounds can be repeated.
        constexpr std::size_t kMaxExpansion = 1 << 20;

        bool isSpace(char c)
        {
            return c == ' ' || c == '\t' || c == '\r' || c == '\f' || c == '\v';
        }

        bool isDigit(char c)
        {
            return c >= '0' && c <= '9';
        }

        bool isIdentifierStart(char c)
        {
            return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '_';
        }

        bool isIdentifierChar(char c)
        {
            return isIdentifierStart(c) || isDigit(c);
        }

        // A preprocessing token of an #if's line: a number (C's preprocessing number, which the parser reads as an
        // integer), a name, a punctuator (an operator, a parenthesis, or any other character, which no rule takes),
        // or the line's end. `offset` and `length` place it in the line: a token of a macro's value is at the
        // macro's name.
        struct Token
        {
            enum class Kind
            {
                Number,
                Name,
                Punctuator,
                End
            };
            Kind kind = Kind::End;
            std::string text;
            std::size_t offset = 0;
            std::size_t length = 1;
        };

        bool is(const Token& token, std::string_view punctuator)
        {
            return token.kind == Token::Kind::Punctuator && token.text == punctuator;
        }

        // The tokens of `text` from `from` on, appended to `out`; placed at `at` when it is given.
        void lex(std::string_view text, std::size_t from, std::vector<Token>& out, const Token* at)
        {
            constexpr std::string_view kPairs[] = {"<<", ">>", "<=", ">=", "==", "!=", "&&", "||"};
            std::size_t i = from;
            while (true)
            {
                while (i < text.size() && isSpace(text[i]))
                    ++i;
                if (i == text.size())
                    return;
                const std::size_t start = i;
                Token::Kind kind = Token::Kind::Punctuator;
                if (isDigit(text[i]) || (text[i] == '.' && i + 1 < text.size() && isDigit(text[i + 1])))
                {
                    // C's preprocessing number: digits, letters, _ and ., and a sign after an exponent's letter.
                    kind = Token::Kind::Number;
                    for (++i; i < text.size(); ++i)
                    {
                        const char c = text[i];
                        const char before = text[i - 1];
                        const bool sign = (c == '+' || c == '-')
                                          && (before == 'e' || before == 'E' || before == 'p' || before == 'P');
                        if (!isIdentifierChar(c) && c != '.' && !sign)
                            break;
                    }
                }
                else if (isIdentifierStart(text[i]))
                {
                    kind = Token::Kind::Name;
                    while (i < text.size() && isIdentifierChar(text[i]))
                        ++i;
                }
                else
                {
                    const std::string_view pair = text.substr(i, 2);
                    i += std::ranges::find(kPairs, pair) != std::end(kPairs) ? 2 : 1;
                }
                Token token{kind, std::string(text.substr(start, i - start)), start, i - start};
                if (at != nullptr)
                {
                    token.offset = at->offset;
                    token.length = at->length;
                }
                out.push_back(std::move(token));
            }
        }

        // `tokens` into `out` with their macros expanded: a macro's name becomes its value's tokens, expanded in turn,
        // except inside its own value (`expanding`, C's rule), and defined's operand stays the name it is. Each
        // expansion adds its work to `expanded` (kMaxExpansion).
        void expand(const std::vector<Token>& tokens, const Macros& macros, std::vector<std::string>& expanding,
                    std::size_t& expanded, std::vector<Token>& out)
        {
            const auto push = [&out](const Token& token) {
                if (out.size() == kMaxTokens)
                    throw PreprocessorError{token.offset, token.length,
                                            "macro expansion is too long: more than 65536 tokens"};
                out.push_back(token);
            };
            for (std::size_t k = 0; k < tokens.size(); ++k)
            {
                const Token& token = tokens[k];
                if (token.kind != Token::Kind::Name)
                {
                    push(token);
                    continue;
                }
                if (token.text == "defined")
                {
                    push(token);
                    if (k + 1 < tokens.size() && is(tokens[k + 1], "("))
                        push(tokens[++k]);
                    if (k + 1 < tokens.size() && tokens[k + 1].kind == Token::Kind::Name)
                        push(tokens[++k]);
                    continue;
                }
                const auto macro = macros.find(token.text);
                if (macro == macros.end() || std::ranges::find(expanding, token.text) != expanding.end())
                {
                    push(token);
                    continue;
                }
                if (expanding.size() == kMaxDepth)
                    throw PreprocessorError{token.offset, token.length,
                                            "macro expansion nested too deeply: more than 256 macros"};
                expanded += 1 + macro->second.size();
                if (expanded > kMaxExpansion)
                    throw PreprocessorError{token.offset, token.length,
                                            "macro expansion is too long: the program's conditions expand more than "
                                            "1048576 bytes of macros"};
                std::vector<Token> value;
                lex(macro->second, 0, value, &token);
                expanding.push_back(token.text);
                expand(value, macros, expanding, expanded, out);
                expanding.pop_back();
            }
        }

        // The parser of the expanded tokens, which computes as it parses. `live` is false in an operand C does not
        // compute (the right of && after a 0, of || after a 1, the branch of ?: not taken): its division by zero is
        // no error there.
        class Evaluator
        {
        public:
            Evaluator(std::vector<Token> tokens, std::size_t end, const Macros& macros)
                : tokens_(std::move(tokens)), end_{Token::Kind::End, "", end, 1}, macros_(macros)
            {
            }

            std::int64_t run()
            {
                const std::int64_t value = conditional(true);
                const Token& after = peek();
                if (after.kind != Token::Kind::End)
                    fail(after, is(after, ")") || is(after, ":") || is(after, ",")
                                    ? "expected end of line in preprocessor expression"
                                    : "token is not a valid binary operator in a preprocessor subexpression");
                return value;
            }

        private:
            // One level deeper: a parenthesis, a unary operator, a ?:.
            class Nested
            {
            public:
                Nested(Evaluator& evaluator, const Token& at) : evaluator_(evaluator)
                {
                    if (++evaluator_.depth_ > kMaxDepth)
                        fail(at, "preprocessor expression nested too deeply: more than 256 levels");
                }
                ~Nested()
                {
                    --evaluator_.depth_;
                }
                Nested(const Nested&) = delete;
                Nested& operator=(const Nested&) = delete;

            private:
                Evaluator& evaluator_;
            };

            [[noreturn]] static void fail(const Token& at, std::string message)
            {
                throw PreprocessorError{at.offset, at.length, std::move(message)};
            }

            const Token& peek() const
            {
                return position_ < tokens_.size() ? tokens_[position_] : end_;
            }

            Token next()
            {
                Token token = peek();
                if (position_ < tokens_.size())
                    ++position_;
                return token;
            }

            static std::int64_t wrap(std::uint64_t bits)
            {
                return static_cast<std::int64_t>(bits);   // modular, as C++20 converts
            }

            // C's binary operators, from the loosest (||, 1) to the tightest (* / %, 10); -1 for any other token.
            static int precedence(const Token& token)
            {
                if (token.kind != Token::Kind::Punctuator)
                    return -1;
                constexpr std::pair<std::string_view, int> kLevels[] = {
                    {"||", 1}, {"&&", 2}, {"|", 3},  {"^", 4},  {"&", 5},  {"==", 6}, {"!=", 6},
                    {"<", 7},  {">", 7},  {"<=", 7}, {">=", 7}, {"<<", 8}, {">>", 8}, {"+", 9},
                    {"-", 9},  {"*", 10}, {"/", 10}, {"%", 10}};
                for (const auto& [spelling, level] : kLevels)
                    if (token.text == spelling)
                        return level;
                return -1;
            }

            // condition ? a : b, right to left, or a binary expression.
            std::int64_t conditional(bool live)
            {
                const std::int64_t condition = binary(1, live);
                if (!is(peek(), "?"))
                    return condition;
                const Nested nested(*this, next());
                const std::int64_t taken = conditional(live && condition != 0);
                if (!is(peek(), ":"))
                    fail(peek(), "expected ':'");
                next();
                const std::int64_t other = conditional(live && condition == 0);
                return condition != 0 ? taken : other;
            }

            // Operands joined by operators of `least` precedence or tighter, each left to right.
            std::int64_t binary(int least, bool live)
            {
                std::int64_t left = unary(live);
                while (precedence(peek()) >= least)
                {
                    const Token op = next();
                    const bool right = live && !(op.text == "&&" && left == 0) && !(op.text == "||" && left != 0);
                    left = apply(op, left, binary(precedence(op) + 1, right), live);
                }
                return left;
            }

            std::int64_t apply(const Token& op, std::int64_t a, std::int64_t b, bool live) const
            {
                const auto ua = static_cast<std::uint64_t>(a);
                const auto ub = static_cast<std::uint64_t>(b);
                const std::string& o = op.text;
                if (o == "/" || o == "%")
                {
                    if (b == 0)
                    {
                        if (live)
                            fail(op, o == "/" ? "division by zero in preprocessor expression"
                                              : "remainder by zero in preprocessor expression");
                        return 0;
                    }
                    if (b == -1)   // the least integer's quotient overflows: it wraps to itself
                        return o == "/" ? wrap(0 - ua) : 0;
                    return o == "/" ? a / b : a % b;
                }
                if (o == "*")
                    return wrap(ua * ub);
                if (o == "+")
                    return wrap(ua + ub);
                if (o == "-")
                    return wrap(ua - ub);
                if (o == "<<")
                    return wrap(ua << (ub & 63));
                if (o == ">>")
                    return a >> (ub & 63);   // arithmetic, as C++20 shifts a negative value
                if (o == "<")
                    return a < b;
                if (o == ">")
                    return a > b;
                if (o == "<=")
                    return a <= b;
                if (o == ">=")
                    return a >= b;
                if (o == "==")
                    return a == b;
                if (o == "!=")
                    return a != b;
                if (o == "&")
                    return a & b;
                if (o == "^")
                    return a ^ b;
                if (o == "|")
                    return a | b;
                if (o == "&&")
                    return a != 0 && b != 0;
                return a != 0 || b != 0;   // ||
            }

            std::int64_t unary(bool live)
            {
                const Token& token = peek();
                const Nested nested(*this, token);
                if (is(token, "!") || is(token, "~") || is(token, "-") || is(token, "+"))
                {
                    const std::string op = next().text;
                    const std::int64_t value = unary(live);
                    if (op == "!")
                        return value == 0;
                    if (op == "~")
                        return ~value;
                    if (op == "-")
                        return wrap(0 - static_cast<std::uint64_t>(value));
                    return value;
                }
                return primary(live);
            }

            std::int64_t primary(bool live)
            {
                const Token token = next();
                switch (token.kind)
                {
                case Token::Kind::Number:
                    return number(token);
                case Token::Kind::Name:
                    if (token.text == "defined")
                        return defined();
                    return token.text == "true";   // C23's true is 1, and false, like any other name, 0
                case Token::Kind::Punctuator:
                    if (is(token, "("))
                    {
                        const std::int64_t value = conditional(live);
                        const Token& close = peek();
                        if (!is(close, ")"))
                            fail(close, close.kind == Token::Kind::End || is(close, ":") || is(close, ",")
                                            ? "expected ')' in preprocessor expression"
                                            : "token is not a valid binary operator in a preprocessor subexpression");
                        next();
                        return value;
                    }
                    if (is(token, ")") || is(token, ":"))
                        fail(token, "expected value in expression");
                    fail(token, "invalid token at start of a preprocessor expression");
                case Token::Kind::End:
                    break;
                }
                fail(token, "expected value in expression");
            }

            // defined NAME, or defined ( NAME ): 1 when NAME is a macro.
            std::int64_t defined()
            {
                Token name = next();
                const bool parenthesised = is(name, "(");
                if (parenthesised)
                    name = next();
                if (name.kind != Token::Kind::Name)
                    fail(name, name.kind == Token::Kind::End || is(name, ")") ? "macro name missing"
                                                                              : "macro name must be an identifier");
                if (parenthesised)
                {
                    if (!is(peek(), ")"))
                        fail(peek(), "missing ')' after 'defined'");
                    next();
                }
                return macros_.contains(name.text);
            }

            // An integer constant as FMinus writes one: decimal, octal (a leading 0) or hex (0x), with no suffix.
            static std::int64_t number(const Token& token)
            {
                const std::string& text = token.text;
                const bool hex = text.size() >= 2 && text[0] == '0' && (text[1] == 'x' || text[1] == 'X');
                const auto digit = [hex](char c) -> std::optional<unsigned> {
                    if (isDigit(c))
                        return static_cast<unsigned>(c - '0');
                    if (hex && c >= 'a' && c <= 'f')
                        return static_cast<unsigned>(c - 'a' + 10);
                    if (hex && c >= 'A' && c <= 'F')
                        return static_cast<unsigned>(c - 'A' + 10);
                    return std::nullopt;
                };
                const std::size_t first = hex ? 2 : 0;
                std::size_t end = first;
                while (end < text.size() && digit(text[end]))
                    ++end;
                const char stop = end < text.size() ? text[end] : '\0';
                if (stop == '.' || (hex ? stop == 'p' || stop == 'P' : stop == 'e' || stop == 'E'))
                    fail(token, "floating point literal in preprocessor expression");
                if (end == first || end < text.size())   // 0x with no digit: its x is the suffix, as clang reads it
                    fail(token, "invalid suffix '" + text.substr(end == first ? 1 : end) + "' on integer constant");
                const unsigned base = hex ? 16 : text[0] == '0' ? 8 : 10;
                constexpr auto kMax = static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max());
                std::uint64_t value = 0;
                bool tooLarge = false;
                for (std::size_t i = first; i < end; ++i)
                {
                    const unsigned d = *digit(text[i]);
                    if (d >= base)   // an 8 or a 9 in an octal constant
                        fail(token, "invalid digit '" + std::string(1, text[i]) + "' in octal constant");
                    tooLarge = tooLarge || value > (kMax - d) / base;
                    value = value * base + d;
                }
                if (tooLarge)
                    fail(token, "integer literal is too large to be represented in a signed integer type");
                return static_cast<std::int64_t>(value);
            }

            std::vector<Token> tokens_;
            Token end_;
            const Macros& macros_;
            std::size_t position_ = 0;
            int depth_ = 0;
        };
    } // namespace

    std::string withoutComments(std::string_view line, std::size_t from)
    {
        std::string text(line);
        for (std::size_t i = from; i < text.size();)
        {
            if (const char quote = text[i]; quote == '"' || quote == '\'')
            {
                std::size_t end = i + 1;
                while (end < text.size() && text[end] != quote)
                    end += text[end] == '\\' ? 2 : 1;
                i = std::min(end + 1, text.size());
                continue;
            }
            if (text.compare(i, 2, "//") == 0)
            {
                std::fill(text.begin() + static_cast<std::ptrdiff_t>(i), text.end(), ' ');
                break;
            }
            if (text.compare(i, 2, "/*") != 0)
            {
                ++i;
                continue;
            }
            const std::size_t end = text.find("*/", i + 2);
            if (end == std::string::npos)
                throw PreprocessorError{i, 2, "a directive's /* comment must end on its line"};
            const auto at = [&text](std::size_t offset) { return text.begin() + static_cast<std::ptrdiff_t>(offset); };
            std::fill(at(i), at(end + 2), ' ');
            i = end + 2;
        }
        return text;
    }

    std::int64_t evaluate(std::string_view line, std::size_t from, const Macros& macros, std::size_t& expanded)
    {
        std::vector<Token> tokens;
        lex(line, from, tokens, nullptr);
        std::vector<Token> out;
        std::vector<std::string> expanding;
        expand(tokens, macros, expanding, expanded, out);
        return Evaluator(std::move(out), line.size(), macros).run();
    }

    std::span<const PredefinedMacro> predefinedMacros()
    {
        static constexpr std::array kPredefined{
            PredefinedMacro{"__FMINUS_JSON__", "the JSON parser", "FMINUS_JSON", FMINUS_HAS_JSON != 0},
            PredefinedMacro{"__FMINUS_XML__", "the XML parser", "FMINUS_XML", FMINUS_HAS_XML != 0},
            PredefinedMacro{"__FMINUS_WEB__", "the web library", "FMINUS_WEB", FMINUS_HAS_WEB != 0},
        };
        return kPredefined;
    }

    std::optional<std::string> predefinedMacroError(std::string_view name)
    {
        for (const PredefinedMacro& macro : predefinedMacros())
        {
            if (macro.name == name)
                return "'" + std::string(name) + "' cannot be used as a macro name: FMinus defines it in a build with "
                       + std::string(macro.feature) + " (" + std::string(macro.option) + ")";
        }
        return std::nullopt;
    }

    std::string normalisedValue(std::string_view value)
    {
        std::string text;
        for (std::size_t i = 0; i < value.size();)
        {
            if (!isSpace(value[i]))
            {
                text += value[i++];
                continue;
            }
            while (i < value.size() && isSpace(value[i]))
                ++i;
            if (!text.empty() && i < value.size())
                text += ' ';
        }
        return text;
    }
} // namespace fminus::frontend
