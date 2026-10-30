// src/circuit/Netlist.cpp: the netlist's line scanner and parser (Netlist.h). SPICE is line-oriented, with
// continuations, case-folding and unit suffixes, so it is read by hand, a logical line at a time: no grammar.
#include "circuit/Netlist.h"

#include <algorithm>
#include <array>
#include <charconv>
#include <cmath>
#include <cstddef>
#include <format>
#include <limits>
#include <locale>
#include <map>
#include <set>
#include <sstream>
#include <stdexcept>
#include <system_error>
#include <utility>

namespace fminus::circuit
{
    // ---- Messages ---------------------------------------------------------------------------------------------------

    void Messages::error(Loc loc, std::string text, std::vector<Message> notes)
    {
        messages_.push_back({Severity::Error, loc, std::move(text), std::move(notes)});
    }

    void Messages::warning(Loc loc, std::string text)
    {
        messages_.push_back({Severity::Warning, loc, std::move(text), {}});
    }

    void Messages::note(Loc loc, std::string text)
    {
        messages_.push_back({Severity::Note, loc, std::move(text), {}});
    }

    bool Messages::hasErrors() const
    {
        return std::any_of(messages_.begin(), messages_.end(),
                           [](const Message& m) { return m.severity == Severity::Error; });
    }

    namespace
    {
        const char* severityName(Severity severity)
        {
            switch (severity)
            {
            case Severity::Error:
                return "error";
            case Severity::Warning:
                return "warning";
            case Severity::Note:
                return "note";
            }
            return "note";
        }

        void formatOne(std::string& out, std::string_view fileName, const Message& m)
        {
            out += fileName;
            if (m.loc.line > 0)
                out += ":" + std::to_string(m.loc.line) + ":" + std::to_string(m.loc.column);
            out += ": ";
            out += severityName(m.severity);
            out += ": ";
            out += m.text;
            out += "\n";
        }
    } // namespace

    std::string Messages::format(std::string_view fileName, Severity severity) const
    {
        std::vector<const Message*> chosen;
        for (const Message& m : messages_)
        {
            if (m.severity == severity)
                chosen.push_back(&m);
        }
        // Source order; a message about the netlist as a whole (line 0) after the others. Stable: the order reported
        // among messages at one place.
        std::stable_sort(chosen.begin(), chosen.end(),
                         [](const Message* a, const Message* b)
                         {
                             const int la = a->loc.line == 0 ? 1 << 30 : a->loc.line;
                             const int lb = b->loc.line == 0 ? 1 << 30 : b->loc.line;
                             if (la != lb)
                                 return la < lb;
                             return a->loc.column < b->loc.column;
                         });
        std::string out;
        for (const Message* m : chosen)
        {
            formatOne(out, fileName, *m);
            for (const Message& n : m->notes)
                formatOne(out, fileName, n);
        }
        return out;
    }

    // ---- literals and names -----------------------------------------------------------------------------------------

    std::string literal(double value, bool smallInt)
    {
        const float f = static_cast<float>(value);
        if (!std::isfinite(f))
            throw NotAFloat(std::format("the netlist's values give a coefficient of {:g}, beyond float's range: check "
                                        "them",
                                        value));
        if (smallInt && f == std::floor(f) && std::fabs(f) <= 16.0f)
            return std::to_string(static_cast<int>(f));
        std::array<char, 64> buffer{};
        const std::to_chars_result r = std::to_chars(buffer.data(), buffer.data() + buffer.size(), f);
        if (r.ec != std::errc())
            throw std::runtime_error("std::to_chars failed");
        std::string text(buffer.data(), r.ptr);
        if (text == "-0")
            text = "0";
        if (text.find_first_of(".e") == std::string::npos)
            text += ".0";
        return text + "f";
    }

    std::string folded(std::string_view text)
    {
        std::string out(text);
        for (char& c : out)
        {
            if (c >= 'A' && c <= 'Z')
                c = static_cast<char>(c - 'A' + 'a');
        }
        return out;
    }

    namespace
    {
        bool isLetter(char c)
        {
            return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z');
        }

        bool isDigit(char c)
        {
            return c >= '0' && c <= '9';
        }
    } // namespace

    bool isPlainName(std::string_view name)
    {
        if (name.empty() || !isLetter(name.front()))
            return false;
        return std::all_of(name.begin(), name.end(), [](char c) { return isLetter(c) || isDigit(c); });
    }

    const char* taperName(Taper taper)
    {
        switch (taper)
        {
        case Taper::Lin:
            return "lin";
        case Taper::Audio:
            return "audio";
        case Taper::Rlog:
            return "rlog";
        }
        return "lin";
    }

    double taperValue(Taper taper, double x)
    {
        // (81^x - 1) / 80 as the emitted code computes it, exp(ln 81 x): ln 81 is a literal, so generation needs no
        // libm beyond what this reference evaluation does.
        constexpr double kLn81 = 4.394449154672439;
        switch (taper)
        {
        case Taper::Lin:
            return x;
        case Taper::Audio:
            return (std::exp(kLn81 * x) - 1) * 0.0125;
        case Taper::Rlog:
            return 1 - (std::exp(kLn81 * (1 - x)) - 1) * 0.0125;
        }
        return x;
    }

    std::vector<ModelCard> builtinModels()
    {
        // Yeh, DAFx-07 (as examples/circuits' netlists write them): both are the same small-signal silicon diode
        return {{"1N914", {}, 2.52e-9, 1.752, 0}, {"1N4148", {}, 2.52e-9, 1.752, 0}};
    }

    // ---- expressions ------------------------------------------------------------------------------------------------

    double evaluate(const Expr& expr, const std::function<double(const std::string&)>& pot)
    {
        switch (expr.op)
        {
        case Expr::Op::Number:
            return expr.value;
        case Expr::Op::Name:
            return pot(expr.pot);
        case Expr::Op::Neg:
            return -evaluate(expr.args[0], pot);
        case Expr::Op::Add:
            return evaluate(expr.args[0], pot) + evaluate(expr.args[1], pot);
        case Expr::Op::Sub:
            return evaluate(expr.args[0], pot) - evaluate(expr.args[1], pot);
        case Expr::Op::Mul:
            return evaluate(expr.args[0], pot) * evaluate(expr.args[1], pot);
        case Expr::Op::Div:
            return evaluate(expr.args[0], pot) / evaluate(expr.args[1], pot);
        case Expr::Op::Pow:
        {
            // ^ as repeated products, as it prints
            const double base = evaluate(expr.args[0], pot);
            double r = 1;
            for (int i = 0; i < expr.exponent; ++i)
                r = i == 0 ? base : r * base;
            return r;
        }
        }
        return 0;
    }

    namespace
    {
        void collectPots(const Expr& expr, std::vector<std::string>& out)
        {
            if (expr.op == Expr::Op::Name)
            {
                if (std::find(out.begin(), out.end(), expr.pot) == out.end())
                    out.push_back(expr.pot);
                return;
            }
            for (const Expr& a : expr.args)
                collectPots(a, out);
        }

        // Precedences for printing: 1 + -, 2 * /, 3 unary minus, 4 a primary.
        int precedence(const Expr& e)
        {
            switch (e.op)
            {
            case Expr::Op::Add:
            case Expr::Op::Sub:
                return 1;
            case Expr::Op::Mul:
            case Expr::Op::Div:
                return 2;
            case Expr::Op::Neg:
                return 3;
            case Expr::Op::Pow:   // printed as a product
                return e.exponent == 0 ? 4 : e.exponent == 1 ? precedence(e.args[0]) : 2;
            case Expr::Op::Number:
            case Expr::Op::Name:
                return 4;
            }
            return 4;
        }

        std::string print(const Expr& e, const std::function<std::string(const std::string&)>& pot);

        std::string printOperand(const Expr& e, int minimum, const std::function<std::string(const std::string&)>& pot)
        {
            const std::string text = print(e, pot);
            const int p = potsOf(e).empty() ? 4 : precedence(e);
            return p < minimum ? "(" + text + ")" : text;
        }

        std::string print(const Expr& e, const std::function<std::string(const std::string&)>& pot)
        {
            if (potsOf(e).empty())
            {
                const double v = evaluate(e, [](const std::string&) { return 0.0; });
                return literal(v);
            }
            switch (e.op)
            {
            case Expr::Op::Number:
                return literal(e.value);
            case Expr::Op::Name:
                return pot(e.pot);
            case Expr::Op::Neg:
                return "-" + printOperand(e.args[0], 3, pot);
            case Expr::Op::Add:
                return printOperand(e.args[0], 1, pot) + " + " + printOperand(e.args[1], 2, pot);
            case Expr::Op::Sub:
                return printOperand(e.args[0], 1, pot) + " - " + printOperand(e.args[1], 2, pot);
            case Expr::Op::Mul:
                return printOperand(e.args[0], 2, pot) + " * " + printOperand(e.args[1], 3, pot);
            case Expr::Op::Div:
                return printOperand(e.args[0], 2, pot) + " / " + printOperand(e.args[1], 3, pot);
            case Expr::Op::Pow:
            {
                if (e.exponent == 0)
                    return "1.0f";
                const std::string base = printOperand(e.args[0], 3, pot);
                std::string s = e.exponent == 1 ? print(e.args[0], pot) : base;
                for (int i = 1; i < e.exponent; ++i)
                    s += " * " + base;
                return s;
            }
            }
            return "0.0f";
        }
    } // namespace

    std::vector<std::string> potsOf(const Expr& expr)
    {
        std::vector<std::string> out;
        collectPots(expr, out);
        return out;
    }

    std::string printExpr(const Expr& expr, const std::function<std::string(const std::string&)>& pot)
    {
        return print(expr, pot);
    }

    // ---- the scanner ------------------------------------------------------------------------------------------------

    namespace
    {
        struct Token
        {
            std::string text;
            Loc loc;
        };

        // A logical line: its tokens, the first at its start, continuations appended.
        struct Line
        {
            std::vector<Token> tokens;
        };

        std::vector<Line> scan(std::string_view text)
        {
            std::vector<Line> lines;
            int number = 0;
            std::size_t start = 0;
            while (start <= text.size())
            {
                std::size_t end = text.find('\n', start);
                if (end == std::string_view::npos)
                    end = text.size();
                std::string_view raw = text.substr(start, end - start);
                ++number;
                start = end + 1;
                if (!raw.empty() && raw.back() == '\r')
                    raw.remove_suffix(1);
                // ';' starts a comment anywhere
                const std::size_t semicolon = raw.find(';');
                if (semicolon != std::string_view::npos)
                    raw = raw.substr(0, semicolon);
                std::size_t i = 0;
                while (i < raw.size() && (raw[i] == ' ' || raw[i] == '\t'))
                    ++i;
                if (i == raw.size() || raw[i] == '*')
                {
                    if (end == text.size())
                        break;
                    continue;
                }
                bool continuation = false;
                if (raw[i] == '+')
                {
                    continuation = true;
                    ++i;
                }
                std::vector<Token> tokens;
                while (i < raw.size())
                {
                    while (i < raw.size() && (raw[i] == ' ' || raw[i] == '\t'))
                        ++i;
                    if (i == raw.size())
                        break;
                    const std::size_t first = i;
                    if (raw[i] == '{')
                    {
                        const std::size_t close = raw.find('}', i);
                        i = close == std::string_view::npos ? raw.size() : close + 1;
                    }
                    else
                    {
                        while (i < raw.size() && raw[i] != ' ' && raw[i] != '\t')
                        {
                            if (raw[i] == '{')
                            {
                                const std::size_t close = raw.find('}', i);
                                i = close == std::string_view::npos ? raw.size() : close + 1;
                            }
                            else
                                ++i;
                        }
                    }
                    tokens.push_back(
                        {std::string(raw.substr(first, i - first)), {number, static_cast<int>(first) + 1}});
                }
                if (continuation && !lines.empty())
                {
                    for (Token& t : tokens)
                        lines.back().tokens.push_back(std::move(t));
                }
                else if (!tokens.empty())
                    lines.push_back({std::move(tokens)});
                if (end == text.size())
                    break;
            }
            return lines;
        }

        // key = value written with spaces, joined into one key=value token at the key's place.
        std::vector<Token> joinAssignments(std::vector<Token> tokens)
        {
            std::vector<Token> out;
            for (std::size_t i = 0; i < tokens.size(); ++i)
            {
                Token t = tokens[i];
                if (t.text == "=" && !out.empty() && i + 1 < tokens.size())
                {
                    out.back().text += "=" + tokens[i + 1].text;
                    ++i;
                    continue;
                }
                if (!t.text.empty() && t.text.back() == '=' && i + 1 < tokens.size())
                {
                    t.text += tokens[i + 1].text;
                    ++i;
                }
                else if (!t.text.empty() && t.text.front() == '=' && !out.empty())
                {
                    out.back().text += t.text;
                    continue;
                }
                out.push_back(std::move(t));
            }
            return out;
        }

        // A .model line's card after the name, split at ( ) and , as SPICE allows (each piece keeping its column),
        // its key = value pieces joined: D(IS=2.52n N=1.752), D (IS=2.52n, N=1.752 ) and D IS=2.52n N=1.752 give the
        // same pieces.
        std::vector<Token> modelCard(const std::vector<Token>& t)
        {
            std::vector<Token> pieces;
            for (std::size_t i = 2; i < t.size(); ++i)
            {
                const std::string& s = t[i].text;
                std::size_t k = 0;
                while (k < s.size())
                {
                    while (k < s.size() && (s[k] == '(' || s[k] == ')' || s[k] == ','))
                        ++k;
                    const std::size_t first = k;
                    while (k < s.size() && s[k] != '(' && s[k] != ')' && s[k] != ',')
                        ++k;
                    if (k > first)
                        pieces.push_back({s.substr(first, k - first), {t[i].loc.line, t[i].loc.column + int(first)}});
                }
            }
            return joinAssignments(std::move(pieces));
        }

        // ---- numbers ------------------------------------------------------------------------------------------------

        struct Number
        {
            double value = 0;
            std::string mantissa;   // the digits, as written, before the suffix
            std::string suffix;     // as written: "k", "M", "meg"; empty if none
            bool milli = false;     // the suffix was m or M
        };

        // [+-]digits[.digits][e[+-]digits][suffix][letters]: SPICE's number. Suffixes t g meg k m u n p f (meg
        // before m, any case); trailing letters (10nF, 2.2kohm, -60dB) are ignored.
        std::optional<Number> parseNumber(std::string_view s)
        {
            std::size_t i = 0;
            if (i < s.size() && (s[i] == '+' || s[i] == '-'))
                ++i;
            const std::size_t digitsStart = i;
            bool any = false;
            while (i < s.size() && isDigit(s[i]))
            {
                ++i;
                any = true;
            }
            if (i < s.size() && s[i] == '.')
            {
                ++i;
                while (i < s.size() && isDigit(s[i]))
                {
                    ++i;
                    any = true;
                }
            }
            if (!any)
                return std::nullopt;
            if (i < s.size() && (s[i] == 'e' || s[i] == 'E'))
            {
                std::size_t j = i + 1;
                if (j < s.size() && (s[j] == '+' || s[j] == '-'))
                    ++j;
                if (j < s.size() && isDigit(s[j]))
                {
                    while (j < s.size() && isDigit(s[j]))
                        ++j;
                    i = j;
                }
            }
            Number n;
            n.mantissa = std::string(s.substr(0, i));
            (void)digitsStart;
            // C's reading of a decimal number, whatever the process's locale
            double v = 0;
            std::istringstream stream(std::string(s.substr(s[0] == '+' ? 1 : 0, i - (s[0] == '+' ? 1 : 0))));
            stream.imbue(std::locale::classic());
            stream >> v;
            if (stream.fail())
                return std::nullopt;
            const std::string rest = folded(s.substr(i));
            double scale = 1;
            std::size_t used = 0;
            if (rest.rfind("meg", 0) == 0)
            {
                scale = 1e6;
                used = 3;
            }
            else if (!rest.empty())
            {
                switch (rest[0])
                {
                case 't':
                    scale = 1e12;
                    used = 1;
                    break;
                case 'g':
                    scale = 1e9;
                    used = 1;
                    break;
                case 'k':
                    scale = 1e3;
                    used = 1;
                    break;
                case 'm':
                    scale = 1e-3;
                    used = 1;
                    n.milli = true;
                    break;
                case 'u':
                    scale = 1e-6;
                    used = 1;
                    break;
                case 'n':
                    scale = 1e-9;
                    used = 1;
                    break;
                case 'p':
                    scale = 1e-12;
                    used = 1;
                    break;
                case 'f':
                    scale = 1e-15;
                    used = 1;
                    break;
                default:
                    break;
                }
            }
            for (std::size_t k = used; k < rest.size(); ++k)
            {
                if (!isLetter(rest[k]))
                    return std::nullopt;
            }
            n.suffix = std::string(s.substr(i, used));
            n.value = v * scale;
            return n;
        }

        // ---- the expression parser ----------------------------------------------------------------------------------

        class ExprParser
        {
        public:
            ExprParser(std::string_view text, Loc at, Messages& messages)
                : text_(text)
                , at_(at)
                , messages_(messages)
            {
            }

            std::optional<Expr> parse()
            {
                skip();
                if (pos_ >= text_.size())
                {
                    fail(pos_, "expected an expression");
                    return std::nullopt;
                }
                std::optional<Expr> e = expression();
                if (!e)
                    return std::nullopt;
                skip();
                if (pos_ < text_.size())
                {
                    fail(pos_, "unexpected '" + std::string(1, text_[pos_]) + "' in the expression");
                    return std::nullopt;
                }
                return e;
            }

        private:
            Loc locAt(std::size_t p) const
            {
                return {at_.line, at_.column + 1 + static_cast<int>(p)};
            }

            void fail(std::size_t p, std::string text)
            {
                if (!failed_)
                    messages_.error(locAt(p), std::move(text));
                failed_ = true;
            }

            void skip()
            {
                while (pos_ < text_.size() && (text_[pos_] == ' ' || text_[pos_] == '\t'))
                    ++pos_;
            }

            // One level deeper ('(' or a unary '-' at p), within kMaxDepth: the parser and every walk of the tree
            // (evaluate, print, the destructor) recurse, and generate() may run on a thread with a small stack.
            bool nested(std::size_t p)
            {
                if (++depth_ <= kMaxDepth)
                    return true;
                fail(p, "the expression is nested too deeply (at most " + std::to_string(kMaxDepth) + " levels)");
                return false;
            }

            // One operator more, within kMaxOperators: a long chain is a deep tree too.
            bool counted(Loc loc)
            {
                if (++operators_ <= kMaxOperators)
                    return true;
                if (!failed_)
                    messages_.error(loc, "the expression has too many operators (at most " +
                                             std::to_string(kMaxOperators) + ")");
                failed_ = true;
                return false;
            }

            bool accept(char c)
            {
                skip();
                if (pos_ < text_.size() && text_[pos_] == c)
                {
                    ++pos_;
                    return true;
                }
                return false;
            }

            std::optional<Expr> binary(std::optional<Expr> left, Expr::Op op, std::optional<Expr> right, Loc loc)
            {
                if (!left || !right || !counted(loc))
                    return std::nullopt;
                Expr e;
                e.op = op;
                e.loc = loc;
                e.args.push_back(std::move(*left));
                e.args.push_back(std::move(*right));
                return e;
            }

            std::optional<Expr> expression()
            {
                std::optional<Expr> left = term();
                for (;;)
                {
                    skip();
                    const std::size_t p = pos_;
                    if (accept('+'))
                        left = binary(std::move(left), Expr::Op::Add, term(), locAt(p));
                    else if (accept('-'))
                        left = binary(std::move(left), Expr::Op::Sub, term(), locAt(p));
                    else
                        return left;
                    if (!left)
                        return std::nullopt;
                }
            }

            std::optional<Expr> term()
            {
                std::optional<Expr> left = unary();
                for (;;)
                {
                    skip();
                    const std::size_t p = pos_;
                    if (accept('*'))
                        left = binary(std::move(left), Expr::Op::Mul, unary(), locAt(p));
                    else if (accept('/'))
                        left = binary(std::move(left), Expr::Op::Div, unary(), locAt(p));
                    else
                        return left;
                    if (!left)
                        return std::nullopt;
                }
            }

            std::optional<Expr> unary()
            {
                skip();
                const std::size_t p = pos_;
                if (accept('-'))
                {
                    if (!nested(p))
                        return std::nullopt;
                    std::optional<Expr> operand = unary();
                    --depth_;
                    if (!operand || !counted(locAt(p)))
                        return std::nullopt;
                    Expr e;
                    e.op = Expr::Op::Neg;
                    e.loc = locAt(p);
                    e.args.push_back(std::move(*operand));
                    return e;
                }
                return power();
            }

            std::optional<Expr> power()
            {
                std::optional<Expr> base = primary();
                if (!base)
                    return std::nullopt;
                skip();
                const std::size_t p = pos_;
                if (!accept('^'))
                    return base;
                skip();
                const std::size_t q = pos_;
                std::size_t end = q;
                while (end < text_.size() && isDigit(text_[end]))
                    ++end;
                int exponent = -1;
                if (end > q && (end == text_.size() || !isLetter(text_[end])) && end - q <= 2)
                    exponent = std::stoi(std::string(text_.substr(q, end - q)));
                if (exponent < 0 || exponent > 8 || (end < text_.size() && text_[end] == '.'))
                {
                    fail(q, "the exponent of '^' is an integer from 0 to 8");
                    return std::nullopt;
                }
                pos_ = end;
                if (!counted(locAt(p)))
                    return std::nullopt;
                Expr e;
                e.op = Expr::Op::Pow;
                e.loc = locAt(p);
                e.exponent = exponent;
                e.args.push_back(std::move(*base));
                return e;
            }

            std::optional<Expr> primary()
            {
                skip();
                if (pos_ >= text_.size())
                {
                    fail(pos_, "expected an expression");
                    return std::nullopt;
                }
                const std::size_t p = pos_;
                const char c = text_[pos_];
                if (c == '(')
                {
                    if (!nested(p))
                        return std::nullopt;
                    ++pos_;
                    std::optional<Expr> e = expression();
                    --depth_;
                    if (!e)
                        return std::nullopt;
                    if (!accept(')'))
                    {
                        fail(pos_, "expected ')'");
                        return std::nullopt;
                    }
                    return e;
                }
                if (isDigit(c) || c == '.')
                {
                    std::size_t end = pos_;
                    while (end < text_.size() && (isDigit(text_[end]) || text_[end] == '.'))
                        ++end;
                    if (end < text_.size() && (text_[end] == 'e' || text_[end] == 'E'))
                    {
                        std::size_t j = end + 1;
                        if (j < text_.size() && (text_[j] == '+' || text_[j] == '-'))
                            ++j;
                        if (j < text_.size() && isDigit(text_[j]))
                        {
                            while (j < text_.size() && isDigit(text_[j]))
                                ++j;
                            end = j;
                        }
                    }
                    while (end < text_.size() && isLetter(text_[end]))
                        ++end;
                    const std::optional<Number> n = parseNumber(text_.substr(pos_, end - pos_));
                    if (!n)
                    {
                        fail(p, "'" + std::string(text_.substr(pos_, end - pos_)) + "' is not a number");
                        return std::nullopt;
                    }
                    pos_ = end;
                    Expr e;
                    e.op = Expr::Op::Number;
                    e.value = n->value;
                    e.loc = locAt(p);
                    return e;
                }
                if (isLetter(c))
                {
                    std::size_t end = pos_;
                    while (end < text_.size() && (isLetter(text_[end]) || isDigit(text_[end]) || text_[end] == '_'))
                        ++end;
                    Expr e;
                    e.op = Expr::Op::Name;
                    e.pot = folded(text_.substr(pos_, end - pos_));
                    e.spelling = std::string(text_.substr(pos_, end - pos_));
                    e.loc = locAt(p);
                    pos_ = end;
                    return e;
                }
                fail(p, "unexpected '" + std::string(1, c) + "' in the expression");
                return std::nullopt;
            }

            static constexpr int kMaxDepth = 64;
            static constexpr int kMaxOperators = 256;

            std::string_view text_;
            Loc at_;   // the '{' (the text starts after it)
            Messages& messages_;
            std::size_t pos_ = 0;
            bool failed_ = false;
            int depth_ = 0;
            int operators_ = 0;
        };

        // ---- the line parser ----------------------------------------------------------------------------------------

        class Parser
        {
        public:
            Parser(Netlist& netlist, Messages& messages)
                : n_(netlist)
                , messages_(messages)
            {
            }

            // Returns false at .end.
            bool line(std::vector<Token> tokens)
            {
                const Token& head = tokens.front();
                if (head.text.front() == '.')
                    return directive(joinAssignments(std::move(tokens)));
                element(joinAssignments(std::move(tokens)));
                return true;
            }

            void finish();

        private:
            void error(Loc loc, std::string text)
            {
                messages_.error(loc, std::move(text));
            }

            std::optional<double> number(const Token& t, const std::string& what)
            {
                const std::optional<Number> n = parseNumber(t.text);
                if (!n)
                {
                    error(t.loc, "'" + t.text + "' is not a number (" + what + ")");
                    return std::nullopt;
                }
                return n->value;
            }

            std::optional<Expr> expression(const Token& t)
            {
                if (t.text.size() < 2 || t.text.back() != '}')
                {
                    error(t.loc, "'" + t.text + "': an expression ends with '}'");
                    return std::nullopt;
                }
                ExprParser p(std::string_view(t.text).substr(1, t.text.size() - 2), t.loc, messages_);
                std::optional<Expr> e = p.parse();
                if (e && !finiteConstants(*e))
                    return std::nullopt;
                return e;
            }

            // Whether each part of `e` without a pot (which the module prints folded, as a float literal) is a finite
            // float; an error at the first that is not.
            bool finiteConstants(const Expr& e)
            {
                if (potsOf(e).empty())
                {
                    const double v = evaluate(e, [](const std::string&) { return 0.0; });
                    if (std::isfinite(v) && std::fabs(v) <= static_cast<double>(std::numeric_limits<float>::max()))
                        return true;
                    error(e.loc, std::format("a constant in the expression is {:g}, not a finite float", v));
                    return false;
                }
                return std::all_of(e.args.begin(), e.args.end(), [&](const Expr& a) { return finiteConstants(a); });
            }

            // Whether a part's value is one Phase 0's float coefficients hold (1/R, R and 2 C fs' are float literals,
            // with headroom for their products): 1e-30 to 1e30, ohms or farads. An error otherwise.
            bool inRange(double value, const Token& t, const std::string& part)
            {
                if (value >= 1e-30 && value <= 1e30)
                    return true;
                error(t.loc, part + "'s value " + t.text + " is out of range for float coefficients (1e-30 to 1e30)");
                return false;
            }

            // An E's or a linear G's gain: a number (not 0, at most 1e30 and at least 1e-30 in size: a float
            // coefficient) or an {expression} of pots.
            bool gain(Element& e, const Token& t, const std::string& part)
            {
                e.valueLoc = t.loc;
                if (t.text.front() == '{')
                {
                    e.expr = expression(t);
                    if (!e.expr)
                        return false;
                    if (!potsOf(*e.expr).empty())
                        return true;
                    e.value = evaluate(*e.expr, [](const std::string&) { return 0.0; });
                    e.expr.reset();
                }
                else
                {
                    const std::optional<Number> v = parseNumber(t.text);
                    if (!v)
                    {
                        error(t.loc, "'" + t.text + "' is not a number (" + part + "'s gain)");
                        return false;
                    }
                    e.value = v->value;
                }
                if (std::fabs(e.value) >= 1e-30 && std::fabs(e.value) <= 1e30)
                    return true;
                error(t.loc, part + "'s gain " + t.text +
                                 " is out of range for float coefficients (1e-30 to 1e30 in size; not 0)");
                return false;
            }

            bool defined(const std::string& name, Loc loc)
            {
                const std::string key = folded(name);
                const auto it = names_.find(key);
                if (it != names_.end())
                {
                    error(loc, name + " is defined twice (first at line " + std::to_string(it->second.line) + ")");
                    return true;
                }
                names_.emplace(key, loc);
                return false;
            }

            void element(std::vector<Token> t);
            bool directive(std::vector<Token> t);
            void model(const std::vector<Token>& t);
            template <typename T>
            bool once(Setting<T>& s, const Token& head)
            {
                if (s.loc.line > 0)
                {
                    error(head.loc, "'" + folded(head.text) + "' is given twice (first at line " +
                                        std::to_string(s.loc.line) + ")");
                    return false;
                }
                s.loc = head.loc;
                return true;
            }

            Netlist& n_;
            Messages& messages_;
            std::map<std::string, Loc> names_;   // element names, folded
        };

        // The element kinds refused, with what to say.
        const char* unsupported(char kind)
        {
            switch (kind)
            {
            case 'f':
            case 'h':
                return "the current-controlled source %s is not supported yet (Phase 2)";
            case 'q':
                return "the transistor %s is not supported yet (Phase 2)";
            case 'j':
                return "the JFET %s is not supported yet (Phase 2)";
            case 'm':
                return "the MOSFET %s is not supported yet (Phase 2)";
            case 's':
                return "the switch %s is not supported yet (Phase 1)";
            case 'k':
                return "the coupling %s is not supported yet (Phase 2)";
            case 't':
                return "the transmission line %s is not supported yet";
            case 'b':
                return "the behavioural source %s is not supported yet";
            default:
                return nullptr;
            }
        }

        std::string replaced(const char* format, const std::string& name)
        {
            std::string s(format);
            const std::size_t at = s.find("%s");
            s.replace(at, 2, name);
            return s;
        }

        bool isKeyValue(const Token& t, std::string_view key, std::string& value)
        {
            const std::size_t eq = t.text.find('=');
            if (eq == std::string::npos || folded(std::string_view(t.text).substr(0, eq)) != key)
                return false;
            value = t.text.substr(eq + 1);
            return true;
        }

        void Parser::element(std::vector<Token> t)
        {
            const Token& head = t.front();
            const char kind = folded(head.text.substr(0, 1))[0];
            Element e;
            e.name = head.text;
            e.loc = head.loc;
            for (const Token& token : t)
                e.text += (e.text.empty() ? "" : " ") + token.text;
            if (const char* what = unsupported(kind); what != nullptr)
            {
                error(head.loc, replaced(what, head.text));
                return;
            }
            auto nodesFrom = [&](std::size_t first, std::size_t count)
            {
                for (std::size_t i = first; i < first + count; ++i)
                {
                    e.nodes.push_back(t[i].text);
                    e.nodeLocs.push_back(t[i].loc);
                }
            };
            auto nodeTokensOk = [&](std::size_t first, std::size_t count)
            {
                for (std::size_t i = first; i < first + count; ++i)
                {
                    if (t[i].text.find_first_of("{}=()") != std::string::npos)
                    {
                        error(t[i].loc, "'" + t[i].text + "' is not a node name");
                        return false;
                    }
                }
                return true;
            };
            switch (kind)
            {
            case 'r':
            case 'c':
            {
                const bool resistor = kind == 'r';
                if (t.size() != 4 || !nodeTokensOk(1, 2))
                {
                    error(head.loc, head.text + std::string(resistor ? ": expected 'R<name> n+ n- value'"
                                                                     : ": expected 'C<name> n+ n- value'"));
                    return;
                }
                e.kind = resistor ? Element::Kind::Resistor : Element::Kind::Capacitor;
                nodesFrom(1, 2);
                e.valueLoc = t[3].loc;
                if (resistor && t[3].text.front() == '{')
                {
                    e.expr = expression(t[3]);
                    if (!e.expr)
                        return;
                    if (potsOf(*e.expr).empty())
                    {
                        e.value = evaluate(*e.expr, [](const std::string&) { return 0.0; });
                        e.expr.reset();
                    }
                }
                else
                {
                    const std::optional<Number> v = parseNumber(t[3].text);
                    if (!v)
                    {
                        error(t[3].loc, "'" + t[3].text + "' is not a number (" + head.text + "'s value)");
                        return;
                    }
                    e.value = v->value;
                    if (resistor && v->milli && v->value < 1)
                        messages_.warning(t[3].loc, "'" + t[3].text + "' is " + v->mantissa +
                                                        " milliohm in SPICE; write " + v->mantissa +
                                                        "meg for a megohm");
                }
                if (!e.expr && !(e.value > 0))
                {
                    error(t[3].loc, head.text + "'s value is " + t[3].text + ": a " +
                                        (resistor ? "resistor" : "capacitor") + " needs a positive value");
                    return;
                }
                if (!e.expr && !inRange(e.value, t[3], head.text))
                    return;
                break;
            }
            case 'l':
            {
                if (t.size() != 4 || !nodeTokensOk(1, 2))
                {
                    error(head.loc, head.text + ": expected 'L<name> n+ n- value'");
                    return;
                }
                e.kind = Element::Kind::Inductor;
                nodesFrom(1, 2);
                e.valueLoc = t[3].loc;
                const std::optional<Number> v = parseNumber(t[3].text);
                if (!v)
                {
                    error(t[3].loc, "'" + t[3].text + "' is not a number (" + head.text + "'s value)");
                    return;
                }
                e.value = v->value;
                if (!(e.value > 0))
                {
                    error(t[3].loc, head.text + "'s value is " + t[3].text + ": an inductor needs a positive value");
                    return;
                }
                if (!inRange(e.value, t[3], head.text))
                    return;
                break;
            }
            case 'v':
            case 'i':
            {
                // V<name> n+ n- [DC] value, I<name> n+ n- [DC] value: DC only (the signal is .input's)
                const bool voltage = kind == 'v';
                for (std::size_t i = 3; i < t.size(); ++i)
                {
                    const std::string w = folded(t[i].text);
                    static constexpr std::array<std::string_view, 7> kSpecs{"ac",  "sin", "pulse", "pwl",
                                                                            "exp(", "sffm", "am("};
                    if (std::any_of(kSpecs.begin(), kSpecs.end(), [&](std::string_view k)
                                    { return w == k || (k != "ac" && w.rfind(k, 0) == 0); }))
                    {
                        std::string spec;   // the rest of the line
                        for (std::size_t j = i; j < t.size(); ++j)
                            spec += (spec.empty() ? "" : " ") + t[j].text;
                        error(t[i].loc, "'" + spec + "': a" + std::string(voltage ? " V" : "n I") +
                                            " source is DC: the signal is .input's");
                        return;
                    }
                }
                const std::size_t at = t.size() == 5 && folded(t[3].text) == "dc" ? 4 : 3;
                if (t.size() != at + 1 || !nodeTokensOk(1, 2))
                {
                    error(head.loc, head.text + (voltage ? ": expected 'V<name> n+ n- [DC] value'"
                                                         : ": expected 'I<name> n+ n- [DC] value'"));
                    return;
                }
                e.kind = voltage ? Element::Kind::Voltage : Element::Kind::Current;
                nodesFrom(1, 2);
                e.valueLoc = t[at].loc;
                const std::optional<Number> v = parseNumber(t[at].text);
                if (!v)
                {
                    error(t[at].loc, "'" + t[at].text + "' is not a number (" + head.text + "'s value)");
                    return;
                }
                e.value = v->value;
                // a DC volt or amp value: 0 or negative too, finite, and within float's reach
                if (!std::isfinite(e.value) || std::fabs(e.value) > 1e30)
                {
                    error(t[at].loc, head.text + "'s value " + t[at].text +
                                         " is out of range for float coefficients (at most 1e30 in size)");
                    return;
                }
                break;
            }
            case 'e':
            {
                // E<name> n+ n- nc+ nc- gain|{expr}: a VCVS
                if (t.size() != 6 || !nodeTokensOk(1, 4))
                {
                    error(head.loc, head.text + ": expected 'E<name> n+ n- nc+ nc- gain' (gain in V/V, or {expr} of "
                                                "pots)");
                    return;
                }
                e.kind = Element::Kind::Vcvs;
                nodesFrom(1, 4);
                if (!gain(e, t[5], head.text))
                    return;
                break;
            }
            case 'd':
                if (t.size() != 4 || !nodeTokensOk(1, 2))
                {
                    error(head.loc, head.text + ": expected 'D<name> anode cathode model'");
                    return;
                }
                e.kind = Element::Kind::Diode;
                nodesFrom(1, 2);
                e.model = t[3].text;
                e.modelLoc = t[3].loc;
                break;
            case 'p':
            {
                // P<name> e1 w e2 value {expr} [taper=...], or P<name> a b value {expr} [taper=...]
                std::size_t brace = 0;
                for (std::size_t i = 1; i < t.size(); ++i)
                {
                    if (t[i].text.front() == '{')
                    {
                        brace = i;
                        break;
                    }
                }
                if (brace != 4 && brace != 5)
                {
                    error(head.loc, head.text + ": expected 'P<name> e1 w e2 value {expr}' or 'P<name> a b value "
                                                "{expr}', with [taper=lin|audio|rlog]");
                    return;
                }
                const std::size_t nodeCount = brace - 2;
                if (!nodeTokensOk(1, nodeCount))
                    return;
                e.kind = Element::Kind::Pot;
                nodesFrom(1, nodeCount);
                const std::optional<Number> v = parseNumber(t[brace - 1].text);
                e.valueLoc = t[brace - 1].loc;
                if (!v)
                {
                    error(t[brace - 1].loc, "'" + t[brace - 1].text + "' is not a number (" + head.text + "'s value)");
                    return;
                }
                e.value = v->value;
                if (v->milli && v->value < 1)
                    messages_.warning(t[brace - 1].loc, "'" + t[brace - 1].text + "' is " + v->mantissa +
                                                            " milliohm in SPICE; write " + v->mantissa +
                                                            "meg for a megohm");
                if (!(e.value > 0))
                {
                    error(t[brace - 1].loc,
                          head.text + "'s value is " + t[brace - 1].text + ": a pot needs a positive value");
                    return;
                }
                if (!inRange(e.value, t[brace - 1], head.text))
                    return;
                e.expr = expression(t[brace]);
                if (!e.expr)
                    return;
                for (std::size_t i = brace + 1; i < t.size(); ++i)
                {
                    std::string value;
                    if (!isKeyValue(t[i], "taper", value))
                    {
                        error(t[i].loc, "'" + t[i].text + "': a pot takes taper=lin|audio|rlog after its {expr}");
                        return;
                    }
                    const std::string v2 = folded(value);
                    e.taperLoc = t[i].loc;
                    if (v2 == "lin")
                        e.taper = Taper::Lin;
                    else if (v2 == "audio")
                        e.taper = Taper::Audio;
                    else if (v2 == "rlog")
                        e.taper = Taper::Rlog;
                    else
                    {
                        error(t[i].loc, "unknown taper '" + value + "' (lin, audio or rlog)");
                        return;
                    }
                }
                break;
            }
            case 'g':
            {
                // G<name> n+ n- nc+ nc- gain|{expr}: a linear VCCS, its current leaving n+ and entering n- (SPICE's G
                // direction)
                if (t.size() == 6 && folded(t[3].text) != "ota")
                {
                    if (!nodeTokensOk(1, 4))
                        return;
                    e.kind = Element::Kind::Vccs;
                    nodesFrom(1, 4);
                    if (!gain(e, t[5], head.text))
                        return;
                    break;
                }
                // G<name> n+ n- OTA nc+ nc- gain|{expr}: the OTA macro (report §7.1), SPICE's G direction (its current
                // leaves n+ and enters n-)
                if (t.size() != 7 || folded(t[3].text) != "ota" || !nodeTokensOk(1, 2) || !nodeTokensOk(4, 2))
                {
                    error(head.loc, head.text + ": expected 'G<name> n+ n- nc+ nc- gain' (a VCCS, gain in A/V, or "
                                                "{expr} of pots) or 'G<name> n+ n- OTA nc+ nc- gain' (gain in amps, "
                                                "or {expr} of pots and law inputs)");
                    return;
                }
                e.kind = Element::Kind::Ota;
                nodesFrom(1, 2);
                nodesFrom(4, 2);
                e.valueLoc = t[6].loc;
                if (t[6].text.front() == '{')
                {
                    e.expr = expression(t[6]);
                    if (!e.expr)
                        return;
                    if (potsOf(*e.expr).empty())
                    {
                        e.value = evaluate(*e.expr, [](const std::string&) { return 0.0; });
                        e.expr.reset();
                    }
                }
                else
                {
                    const std::optional<Number> v = parseNumber(t[6].text);
                    if (!v)
                    {
                        error(t[6].loc, "'" + t[6].text + "' is not a number (" + head.text + "'s gain)");
                        return;
                    }
                    e.value = v->value;
                }
                if (!e.expr && !(e.value > 0))
                {
                    error(t[6].loc, head.text + "'s gain is " + t[6].text + ": an OTA's gain (its bias current) is "
                                                                             "positive");
                    return;
                }
                if (!e.expr && !inRange(e.value, t[6], head.text))
                    return;
                break;
            }
            case 'x':
            {
                if (t.size() != 5 || !nodeTokensOk(1, 3))
                {
                    error(head.loc, head.text + ": expected 'X<name> inp inn out opamp' (an ideal op amp)");
                    return;
                }
                const std::string sub = folded(t[4].text);
                if (sub.rfind("opamp(", 0) == 0)
                {
                    error(t[4].loc, "op amp " + head.text + "'s parameters (" + t[4].text +
                                        ") are not supported yet (Phase 2): Phase 0's op amps are ideal");
                    return;
                }
                if (sub != "opamp")
                {
                    error(t[4].loc, "subcircuit '" + t[4].text +
                                        "' is not supported yet: an X line is an ideal op amp, 'X<name> inp inn out "
                                        "opamp'");
                    return;
                }
                e.kind = Element::Kind::OpAmp;
                nodesFrom(1, 3);
                break;
            }
            default:
                error(head.loc, "unknown element '" + head.text + "'");
                return;
            }
            if (defined(e.name, e.loc))
                return;
            n_.elements.push_back(std::move(e));
        }

        void Parser::model(const std::vector<Token>& t)
        {
            const Token& head = t.front();
            if (t.size() < 3)
            {
                error(head.loc, "expected '.model NAME D(IS=... N=...)'");
                return;
            }
            std::vector<Token> pieces = modelCard(t);
            if (pieces.empty())
            {
                error(head.loc, "expected '.model NAME D(IS=... N=...)'");
                return;
            }
            if (folded(pieces.front().text) != "d")
            {
                const std::string type = folded(pieces.front().text);
                const bool transistor = type == "npn" || type == "pnp" || type == "njf" || type == "pjf" ||
                                        type == "nmos" || type == "pmos";
                error(pieces.front().loc, "model type '" + pieces.front().text + "' is not supported yet" +
                                              std::string(transistor ? " (Phase 2)" : "") + ": Phase 0 has diodes (D)");
                return;
            }
            ModelCard card;
            card.name = t[1].text;
            card.loc = t[1].loc;
            for (const ModelCard& m : n_.models)
            {
                if (folded(m.name) == folded(card.name))
                {
                    error(t[1].loc, "model '" + card.name + "' is defined twice (first at line " +
                                        std::to_string(m.loc.line) + ")");
                    return;
                }
            }
            static constexpr std::array<std::string_view, 11> kIgnored{"cjo", "tt",  "bv", "ibv", "m", "vj",
                                                                       "eg",  "xti", "fc", "kf",  "af"};
            for (std::size_t i = 1; i < pieces.size(); ++i)
            {
                const Token& p = pieces[i];
                const std::size_t eq = p.text.find('=');
                if (eq == std::string::npos)
                {
                    error(p.loc, "'" + p.text + "': a model parameter is NAME=value");
                    return;
                }
                const std::string key = folded(p.text.substr(0, eq));
                const std::string valueText = p.text.substr(eq + 1);
                const std::optional<Number> v = parseNumber(valueText);
                if (!v)
                {
                    error(p.loc, "'" + valueText + "' is not a number (" + p.text.substr(0, eq) + ")");
                    return;
                }
                if (key == "is")
                    card.is = v->value;
                else if (key == "n")
                    card.n = v->value;
                else if (key == "rs")
                {
                    // a series resistance: an internal node and a resistor (0: none)
                    if (!(v->value == 0 || (v->value >= 1e-30 && v->value <= 1e30)))
                    {
                        error(p.loc, "'" + p.text + "': RS is 0 or a resistance from 1e-30 to 1e30 ohms");
                        return;
                    }
                    card.rs = v->value;
                }
                else if (std::find(kIgnored.begin(), kIgnored.end(), key) != kIgnored.end())
                    messages_.warning(p.loc, p.text.substr(0, eq) + " is ignored (the diode model is Shockley's: IS, "
                                                                    "N and RS)");
                else
                {
                    error(p.loc, "unknown diode parameter '" + p.text.substr(0, eq) + "'");
                    return;
                }
            }
            if (!(card.is > 0) || !(card.n > 0))
            {
                error(t[1].loc, "model '" + card.name + "': IS and N are positive");
                return;
            }
            n_.models.push_back(card);
        }

        bool Parser::directive(std::vector<Token> t)
        {
            const Token& head = t.front();
            const std::string name = folded(head.text);
            auto arity = [&](std::size_t lo, std::size_t hi, const char* usage)
            {
                if (t.size() - 1 < lo || t.size() - 1 > hi)
                {
                    error(head.loc, std::string("expected '") + usage + "'");
                    return false;
                }
                return true;
            };
            if (name == ".end")
                return false;
            if (name == ".title")
            {
                if (!arity(1, 1, ".title NAME"))
                    return true;
                if (!n_.title.empty())
                {
                    error(head.loc, "'.title' is given twice (first at line " + std::to_string(n_.titleLoc.line) + ")");
                    return true;
                }
                n_.title = t[1].text;
                n_.titleLoc = t[1].loc;
                return true;
            }
            if (name == ".input" || name == ".output")
            {
                const bool input = name == ".input";
                if (!arity(2, 5, input ? ".input NAME node [kind=voltage] [scale=x]' or '.input NAME - kind=cv|current "
                                         "[scale=x] [range=lo:hi]"
                                       : ".output NAME node [scale=x]"))
                    return true;
                if (!input && t.size() > 4)
                {
                    error(head.loc, "expected '.output NAME node [scale=x]'");
                    return true;
                }
                IoLine io;
                io.name = t[1].text;
                io.loc = t[1].loc;
                io.node = t[2].text;
                io.nodeLoc = t[2].loc;
                bool ranged = false;
                Loc rangeLoc;
                std::string rangeText;
                for (std::size_t i = 3; i < t.size(); ++i)
                {
                    std::string value;
                    if (isKeyValue(t[i], "scale", value))
                    {
                        const std::optional<Number> v = parseNumber(value);
                        if (!v || !(v->value != 0) || !std::isfinite(v->value))
                        {
                            error(t[i].loc, "'" + value + "' is not a nonzero number (scale)");
                            return true;
                        }
                        io.scale = v->value;
                    }
                    else if (input && isKeyValue(t[i], "kind", value))
                    {
                        const std::string k = folded(value);
                        io.kindLoc = t[i].loc;
                        if (k == "cv")
                            io.kind = Input::Kind::Cv;
                        else if (k == "current")
                            io.kind = Input::Kind::Current;
                        else if (k != "voltage")
                        {
                            error(t[i].loc, "'kind=" + value + "': voltage, cv or current");
                            return true;
                        }
                    }
                    else if (input && isKeyValue(t[i], "range", value))
                    {
                        // lo:hi, two numbers with SPICE's suffixes, lo below hi, each a float
                        const std::size_t colon = value.find(':');
                        const std::optional<Number> lo =
                            colon == std::string::npos ? std::nullopt : parseNumber(value.substr(0, colon));
                        const std::optional<Number> hi =
                            colon == std::string::npos ? std::nullopt : parseNumber(value.substr(colon + 1));
                        auto isFloat = [](double x)
                        {
                            return std::isfinite(x) && std::fabs(x) <= 1e30;
                        };
                        if (!lo || !hi || !isFloat(lo->value) || !isFloat(hi->value) || !(lo->value < hi->value))
                        {
                            error(t[i].loc, "'" + t[i].text + "': range=lo:hi, two numbers with lo below hi");
                            return true;
                        }
                        io.low = lo->value;
                        io.high = hi->value;
                        ranged = true;
                        rangeLoc = t[i].loc;
                        rangeText = t[i].text;
                    }
                    else
                    {
                        error(t[i].loc, "'" + t[i].text + "': expected " +
                                            (input ? std::string("kind=voltage|cv|current, scale=x or range=lo:hi")
                                                   : std::string("scale=x")));
                        return true;
                    }
                }
                if (!isPlainName(io.name))
                {
                    error(io.loc, std::string(input ? ".input" : ".output") + " '" + io.name +
                                      "': a name is a letter, then letters and digits");
                    return true;
                }
                const bool law = io.kind != Input::Kind::Voltage;
                if (law && io.node != "-")
                {
                    error(io.nodeLoc, "'" + io.node + "': a law input (kind=" +
                                          (io.kind == Input::Kind::Cv ? "cv" : "current") +
                                          ") has no node; write '-' (it enters an OTA's gain, not the netlist)");
                    return true;
                }
                if (!law && io.node == "-")
                {
                    error(io.nodeLoc, "'-' is a law input's node: a law input says kind=cv or kind=current");
                    return true;
                }
                if (!law && ranged)
                {
                    error(rangeLoc, "'" + rangeText + "': range= is a law input's (kind=cv|current)");
                    return true;
                }
                if (io.kind == Input::Kind::Current && !ranged)
                {
                    error(io.kindLoc, "law input '" + io.name + "' is a current: it needs its range in amps, "
                                                                "range=lo:hi (as range=10u:2m)");
                    return true;
                }
                if (io.kind == Input::Kind::Cv && !ranged)
                {
                    io.low = 0;
                    io.high = 1;
                }
                if (input)
                {
                    auto taken = [&](const IoLine& other)
                    {
                        if (folded(other.name) != folded(io.name))
                            return false;
                        error(io.loc, "input '" + io.name + "' is defined twice (first at line " +
                                          std::to_string(other.loc.line) + ")");
                        return true;
                    };
                    if ((n_.input && taken(*n_.input)) ||
                        std::any_of(n_.laws.begin(), n_.laws.end(), [&](const IoLine& l) { return taken(l); }))
                        return true;
                }
                if (law)
                {
                    n_.laws.push_back(io);
                    return true;
                }
                std::optional<IoLine>& slot = input ? n_.input : n_.output;
                if (slot)
                {
                    error(head.loc, std::string(input ? "a second voltage .input" : "a second .output") +
                                        " (the first at line " + std::to_string(slot->loc.line) + "): " +
                                        (input ? "a netlist has one signal (law inputs are kind=cv|current)"
                                               : "a netlist has one output"));
                    return true;
                }
                slot = io;
                return true;
            }
            if (name == ".pot")
            {
                if (!arity(1, 3, ".pot NAME [default] [lin|audio|rlog]"))
                    return true;
                PotLine p;
                p.name = t[1].text;
                p.loc = t[1].loc;
                for (std::size_t i = 2; i < t.size(); ++i)
                {
                    const std::string w = folded(t[i].text);
                    if (w == "lin")
                        p.taper = Taper::Lin;
                    else if (w == "audio")
                        p.taper = Taper::Audio;
                    else if (w == "rlog")
                        p.taper = Taper::Rlog;
                    else if (i == 2)
                    {
                        const std::optional<double> v = number(t[i], "the pot's default");
                        if (!v)
                            return true;
                        if (*v < 0 || *v > 1)
                        {
                            error(t[i].loc, "pot '" + p.name + "' defaults to " + t[i].text + "; a pot is 0 to 1");
                            return true;
                        }
                        p.defaultValue = *v;
                    }
                    else
                    {
                        error(t[i].loc, "unknown taper '" + t[i].text + "' (lin, audio or rlog)");
                        return true;
                    }
                }
                if (!isPlainName(p.name))
                {
                    error(p.loc, "pot '" + p.name + "': a name is a letter, then letters and digits");
                    return true;
                }
                for (const PotLine& q : n_.pots)
                {
                    if (folded(q.name) == folded(p.name))
                    {
                        error(p.loc, "pot '" + p.name + "' is defined twice (first at line " +
                                         std::to_string(q.loc.line) + ")");
                        return true;
                    }
                }
                n_.pots.push_back(p);
                return true;
            }
            if (name == ".model")
            {
                model(t);
                return true;
            }
            if (name == ".oversample")
            {
                if (!once(n_.oversample, head) ||
                    !arity(1, 3, ".oversample 1|2|4|8|auto [iir|fir] [blocks=auto|nonlinear|all]"))
                    return true;
                const std::string m = folded(t[1].text);
                if (m == "auto")
                    n_.oversample.isAuto = true;
                else if (m == "1" || m == "2" || m == "4" || m == "8")
                    n_.oversample.value = std::stoi(m);
                else
                {
                    error(t[1].loc, "'.oversample " + t[1].text + "': 1, 2, 4, 8 or auto");
                    return true;
                }
                for (std::size_t i = 2; i < t.size(); ++i)
                {
                    const std::string w = folded(t[i].text);
                    std::string value;
                    if (w == "iir" || w == "fir")
                    {
                        n_.halfband.value = w == "iir" ? Halfband::Iir : Halfband::Fir;
                        n_.halfband.loc = t[i].loc;
                    }
                    else if (isKeyValue(t[i], "blocks", value))
                    {
                        const std::string v = folded(value);
                        n_.region.loc = t[i].loc;
                        if (v == "auto")
                            n_.region.value = Region::Auto;
                        else if (v == "nonlinear")
                            n_.region.value = Region::Nonlinear;
                        else if (v == "all")
                            n_.region.value = Region::All;
                        else
                        {
                            error(t[i].loc, "'blocks=" + value + "': auto, nonlinear or all");
                            return true;
                        }
                    }
                    else
                    {
                        error(t[i].loc, "'" + t[i].text + "': expected iir, fir or blocks=auto|nonlinear|all");
                        return true;
                    }
                }
                return true;
            }
            if (name == ".adaa")
            {
                if (!once(n_.adaa, head) || !arity(1, 1, ".adaa on|off|auto"))
                    return true;
                const std::string w = folded(t[1].text);
                if (w == "on" || w == "off")
                    n_.adaa.value = w == "on";
                else if (w == "auto")
                    n_.adaa.isAuto = true;
                else
                    error(t[1].loc, "'.adaa " + t[1].text + "': on, off or auto");
                return true;
            }
            if (name == ".solver")
            {
                if (!once(n_.solver, head) || !arity(1, 1, ".solver omega|omega(K)|newton(K)|auto"))
                    return true;
                const std::string w = folded(t[1].text);
                if (w == "auto")
                {
                    n_.solver.isAuto = true;
                    return true;
                }
                if (w == "omega")
                {
                    // the guess named, its steps left to the self-test (an EA port's candidates are all omega's)
                    n_.solver.isAuto = true;
                    n_.solverBare = true;
                    return true;
                }
                const std::size_t open = w.find('(');
                const std::string base = w.substr(0, open);
                if ((base == "omega" || base == "newton") && open != std::string::npos && w.back() == ')')
                {
                    const std::string k = w.substr(open + 1, w.size() - open - 2);
                    const bool digits = !k.empty() && k.size() <= 2 &&
                                        std::all_of(k.begin(), k.end(), [](char c) { return isDigit(c); });
                    const int steps = digits ? std::stoi(k) : -1;
                    const int lo = base == "omega" ? 0 : 1;
                    if (steps < lo || steps > 8)
                    {
                        error(t[1].loc, "'.solver " + t[1].text + "': " + base + "(K) takes " +
                                            (base == "omega" ? "0" : "1") + " to 8 Newton steps");
                        return true;
                    }
                    n_.solver.value = Solver{base == "omega" ? Solver::Guess::Omega : Solver::Guess::Previous, steps};
                    return true;
                }
                if (base == "newton")
                {
                    error(t[1].loc, "'.solver " + t[1].text + "': newton takes its step count, newton(K)");
                    return true;
                }
                if (base == "table")
                {
                    error(t[1].loc, "'.solver table' is not supported yet (Phase 3)");
                    return true;
                }
                error(t[1].loc, "'.solver " + t[1].text + "': omega, omega(K), newton(K) or auto");
                return true;
            }
            if (name == ".channels")
            {
                if (!once(n_.channels, head) || !arity(1, 1, ".channels 1|2"))
                    return true;
                const std::string w = folded(t[1].text);
                if (w == "1" || w == "2")
                    n_.channels.value = std::stoi(w);
                else if (w == "4")
                    error(t[1].loc, "'.channels 4' (a voice bank) is not supported yet (Phase 4)");
                else
                    error(t[1].loc, "'.channels " + t[1].text + "': 1 or 2");
                return true;
            }
            if (name == ".method")
            {
                if (!once(n_.method, head) || !arity(1, 1, ".method tr|bdf2|auto"))
                    return true;
                const std::string w = folded(t[1].text);
                if (w == "tr")
                    n_.method.value = Method::Tr;
                else if (w == "bdf2")
                    n_.method.value = Method::Bdf2;
                else if (w == "auto")
                    n_.method.isAuto = true;
                else
                    error(t[1].loc, "'.method " + t[1].text + "': tr, bdf2 or auto");
                return true;
            }
            if (name == ".prewarp")
            {
                if (!once(n_.prewarp, head) || !arity(1, 1, ".prewarp on|off"))
                    return true;
                const std::string w = folded(t[1].text);
                if (w == "off" || w == "on")
                    n_.prewarp.value = w == "on";
                else
                    error(t[1].loc, "'.prewarp " + t[1].text + "': on or off");
                return true;
            }
            if (name == ".target")
            {
                if (n_.target.loc.line > 0)
                {
                    error(head.loc,
                          "'.target' is given twice (first at line " + std::to_string(n_.target.loc.line) + ")");
                    return true;
                }
                n_.target.loc = head.loc;
                for (std::size_t i = 1; i < t.size(); ++i)
                {
                    const std::size_t eq = t[i].text.find('=');
                    const std::string key = folded(t[i].text.substr(0, eq));
                    std::optional<double>* slot = key == "solver"  ? &n_.target.solver
                                                  : key == "alias" ? &n_.target.alias
                                                  : key == "level" ? &n_.target.level
                                                  : key == "freq"  ? &n_.target.freq
                                                                   : nullptr;
                    if (eq == std::string::npos || slot == nullptr)
                    {
                        error(t[i].loc, "'" + t[i].text +
                                            "': .target takes solver=xdB, alias=xdB, level=xV and "
                                            "freq=xHz");
                        return true;
                    }
                    const std::optional<Number> v = parseNumber(t[i].text.substr(eq + 1));
                    if (!v)
                    {
                        error(t[i].loc, "'" + t[i].text.substr(eq + 1) + "' is not a number (" + key + ")");
                        return true;
                    }
                    // the self-test's stress level L: its sines run at fractions and multiples of it
                    if (key == "level" && !(std::isfinite(v->value) && v->value > 0))
                    {
                        error(t[i].loc, "'" + t[i].text + "': the level is a positive voltage");
                        return true;
                    }
                    *slot = v->value;
                }
                return true;
            }
            if (name == ".op")
            {
                // the DC operating point, as notes (Dc.h)
                if (!arity(0, 0, ".op"))
                    return true;
                n_.op = head.loc;
                return true;
            }
            static constexpr std::array<std::string_view, 11> kSimulator{
                ".tran", ".ac", ".dc", ".print", ".plot", ".probe", ".save", ".meas", ".measure", ".control", ".endc"};
            if (std::find(kSimulator.begin(), kSimulator.end(), name) != kSimulator.end())
            {
                messages_.note(head.loc, "'" + name + "' is a simulator's directive: ignored");
                return true;
            }
            static constexpr std::array<std::string_view, 10> kLater{
                ".subckt", ".ends", ".param", ".include", ".lib", ".options", ".temp", ".ic", ".nodeset", ".func"};
            if (std::find(kLater.begin(), kLater.end(), name) != kLater.end())
            {
                error(head.loc, "'" + name + "' is not supported yet");
                return true;
            }
            error(head.loc, "unknown directive '" + head.text + "'");
            return true;
        }

        void Parser::finish()
        {
            // Every name an {expression} reads must be a .pot, or (in an OTA's gain only) a law input; they may come
            // in any order.
            std::string list;
            for (const PotLine& p : n_.pots)
                list += (list.empty() ? "" : ", ") + p.name;
            std::string laws;
            for (const IoLine& l : n_.laws)
                laws += (laws.empty() ? "" : ", ") + l.name;
            std::set<std::string> known;
            for (const PotLine& p : n_.pots)
                known.insert(folded(p.name));
            std::set<std::string> lawNames;
            for (const IoLine& l : n_.laws)
                lawNames.insert(folded(l.name));
            const Element* in = nullptr;
            std::function<void(const Expr&)> check = [&](const Expr& e)
            {
                if (e.op == Expr::Op::Name && !known.count(e.pot))
                {
                    // the name as written: the loc's column points at it in the line
                    if (lawNames.count(e.pot))
                    {
                        if (in->kind != Element::Kind::Ota)
                            error(e.loc, "law input '" + e.spelling + "' in " + in->name +
                                             "'s value: a law input enters an OTA's gain only (pot-as-port is Phase "
                                             "2)");
                    }
                    else if (laws.empty())
                        error(e.loc, "unknown pot '" + e.spelling + "' (" +
                                         (list.empty() ? std::string("there are no pots") : "the pots: " + list) +
                                         ")");
                    else
                        error(e.loc, "unknown pot or law input '" + e.spelling + "' (" +
                                         (list.empty() ? std::string("there are no pots") : "the pots: " + list) +
                                         "; the law inputs: " + laws + ")");
                }
                for (const Expr& a : e.args)
                    check(a);
            };
            for (const Element& e : n_.elements)
            {
                in = &e;
                if (e.expr)
                    check(*e.expr);
            }
            // a pot and a law input of one name
            for (const PotLine& p : n_.pots)
            {
                for (const IoLine& l : n_.laws)
                {
                    if (folded(p.name) == folded(l.name))
                        error(l.loc, "law input '" + l.name + "' has pot '" + p.name + "''s name (line " +
                                         std::to_string(p.loc.line) + ")");
                }
            }
        }
    } // namespace

    namespace
    {
        // A logical line as a self-test's key reads it (Netlist::canonical), or nothing for a line it leaves out.
        std::optional<std::string> canonicalLine(const std::vector<Token>& tokens)
        {
            const std::string head = folded(tokens.front().text);
            static constexpr std::array<std::string_view, 15> kLeftOut{
                ".title", ".channels", ".op",     ".end",  ".tran",     ".ac",      ".dc",   ".print",
                ".plot",  ".probe",    ".save",   ".meas", ".measure", ".control", ".endc"};
            if (std::find(kLeftOut.begin(), kLeftOut.end(), head) != kLeftOut.end())
                return std::nullopt;
            auto word = [](const Token& t)
            {
                std::string w;
                for (char c : folded(t.text))
                {
                    if (c != ' ' && c != '\t')
                        w += c;
                }
                return w;
            };
            std::string out;
            // a .model card as the parser reads it (its parentheses and commas are separators, so the spacing next
            // to them is spacing), written in one form: ".model 1n914 d(is=2.52n n=1.752)"
            if (head == ".model" && tokens.size() > 2)
            {
                const std::vector<Token> card = modelCard(tokens);
                out = word(tokens[0]) + " " + word(tokens[1]);
                for (std::size_t i = 0; i < card.size(); ++i)
                    out += (i == 0 ? " " : i == 1 ? "(" : " ") + word(card[i]);
                return card.size() > 1 ? out + ")" : out;
            }
            for (const Token& t : joinAssignments(tokens))
                out += (out.empty() ? "" : " ") + word(t);
            return out;
        }
    } // namespace

    Netlist parse(std::string_view text, Messages& messages)
    {
        Netlist netlist;
        Parser parser(netlist, messages);
        for (Line& line : scan(text))
        {
            const bool end = folded(line.tokens.front().text) == ".end";
            if (std::optional<std::string> c = canonicalLine(line.tokens))
                netlist.canonical.push_back(std::move(*c));
            if (end || !parser.line(std::move(line.tokens)))
                break;
        }
        parser.finish();
        return netlist;
    }
} // namespace fminus::circuit
