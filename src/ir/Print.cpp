// src/ir/Print.cpp: printf and eprintf, C's printf to stdout and to stderr (the table is Calls.cpp's; the runtime's
// functions runtime/Print.cpp, the text runtime/Printf.h's). Their format is a string literal, C's printf's in part,
// checked where it is written as clang checks printf's (clang's -Wformat warnings are errors here, as they are for
// every format of FMinus's):
//
//   %[flags][width][.precision]conversion
//     flags        '-' (to the left of the width), '+' and ' ' (a sign before a number that is not negative: '+'
//                  wins), '0' (a number padded with zeros: '-' wins). C's '#' and '\'' are not FMinus's.
//     width        the fewest bytes the place writes, at most 999 (runtime/Printf.h's kMaxPrintWidth); no '*'.
//     precision    '.' and at most 99 (kMaxPrintPrecision): an int's fewest digits, %f's and %e's digits after the
//                  point, %g's significant digits, a string's most bytes; no '*'.
//     conversion   d or i, an int (a bool, an enumeration's value: C promotes either to one); f, e or g, a double (a
//                  float, which C promotes to one); s, a string (a literal among them); and %% alone, a percent sign.
//                  No length modifier: FMinus has no long or short, and a float is a double here as in C.
//
// The values are lowered as a reply's are (WebServer.cpp, placedArguments): every number a double (an int's exactly, a
// float's widened), every string to the _s functions in two arrays, each value checked against its place in clang's
// words ("format specifies type 'double' but the argument has type 'int'").
#include "ir/Lowering.h"

#include <string>

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

        std::string shownText(std::string_view text)
        {
            std::string out;
            for (char c : text)
                out += shown(c);
            return out;
        }

        bool isDigit(char c)
        {
            return c >= '0' && c <= '9';
        }

        // The digits of `text`, as a number: at most `most`, or more than it when there are more digits than its own.
        std::size_t number(std::string_view text, std::size_t most)
        {
            std::size_t value = 0;
            for (char c : text)
            {
                value = value * 10 + static_cast<std::size_t>(c - '0');
                if (value > most)
                    return most + 1;
            }
            return value;
        }

        constexpr std::size_t kMostWidth = 999;      // runtime/Printf.h's kMaxPrintWidth
        constexpr std::size_t kMostPrecision = 99;   // and kMaxPrintPrecision
    } // namespace

    ReplyFormatScan scanPrintFormat(std::string_view text)
    {
        constexpr std::string_view kConversions = "diouxXfFeEgGaAcsSpnmbBC%";   // clang's, for printf on Linux
        ReplyFormatScan scan;
        for (std::size_t i = 0; i < text.size(); ++i)
        {
            if (text[i] != '%')
                continue;
            std::size_t j = i + 1;
            std::size_t left = 0, plus = 0, space = 0, zero = 0, other = 0;   // where each flag is, after the '%'
            for (; j < text.size() && std::string_view("-+ #0'").find(text[j]) != std::string_view::npos; ++j)
            {
                std::size_t& flag = text[j] == '-'   ? left
                                    : text[j] == '+' ? plus
                                    : text[j] == ' ' ? space
                                    : text[j] == '0' ? zero
                                                     : other;
                if (flag == 0)
                    flag = j;
            }
            std::size_t star = 0;   // where a '*' is
            const std::size_t widthAt = j;
            if (j < text.size() && text[j] == '*')
                star = j++;
            while (j < text.size() && isDigit(text[j]))
                ++j;
            const std::string_view width = text.substr(widthAt, j - widthAt);
            std::size_t precisionAt = 0;
            std::string_view precision;
            if (j < text.size() && text[j] == '.')
            {
                precisionAt = ++j;
                if (j < text.size() && text[j] == '*')
                {
                    if (star == 0)
                        star = j;
                    ++j;
                }
                while (j < text.size() && isDigit(text[j]))
                    ++j;
                precision = text.substr(precisionAt, j - precisionAt);
            }
            const std::size_t lengthAt = j;
            const auto next = [&](std::string_view part) { return text.substr(j).starts_with(part); };
            if (next("hh") || next("ll"))
                j += 2;
            else if (next("I64") || next("I32"))
                j += 3;
            else if (j < text.size() && std::string_view("hlqjztLIw").find(text[j]) != std::string_view::npos)
                ++j;
            const auto fail = [&](std::size_t offset, std::string message) {
                scan.errors.push_back({offset, std::move(message)});
            };
            if (j == text.size())
            {
                fail(i, "incomplete format specifier");
                break;
            }
            const char c = text[j];
            const std::string_view spec = text.substr(i + 1, j - i);
            if (kConversions.find(c) == std::string_view::npos)
                fail(j, "invalid conversion specifier '" + shown(c) + "'");
            else if (c == '%')
            {
                if (j != i + 1)
                    fail(i + 1, "a percent sign is %% alone, with no flag, width or precision between");
            }
            else if (std::string_view("difegs").find(c) == std::string_view::npos)
                fail(i + 1, "invalid conversion specifier '" + shownText(spec) + "'");
            else if (lengthAt != j)
                fail(lengthAt, "length modifier '" + shownText(text.substr(lengthAt, j - lengthAt))
                                   + "' is not printf's in FMinus: %d takes an int, and %f, %e and %g a float or a "
                                     "double");
            else if (star != 0)
                fail(star, "a width or a precision of '*' is not printf's in FMinus: write the number in the format");
            else if (other != 0)
                fail(other, "flag '" + shown(text[other]) + "' is not printf's in FMinus: it takes '-', '+', ' ' and "
                                                            "'0'");
            else if (number(width, kMostWidth) > kMostWidth)
                fail(widthAt, "a width of printf's is at most 999");
            else if (precisionAt != 0 && number(precision, kMostPrecision) > kMostPrecision)
                fail(precisionAt, "a precision of printf's is at most 99");
            else if (c == 's' && (zero != 0 || plus != 0 || space != 0))
            {
                const std::size_t flag = zero != 0 ? zero : plus != 0 ? plus : space;
                fail(flag, "flag '" + shown(text[flag]) + "' results in undefined behavior with 's' conversion "
                                                          "specifier");
            }
            else if (left != 0 && zero != 0)
                fail(zero, "flag '0' is ignored when flag '-' is present");
            else if (plus != 0 && space != 0)
                fail(space, "flag ' ' is ignored when flag '+' is present");
            else if (c == 's')
                scan.places.push_back({i, false, true});
            else
                scan.places.push_back({i, c == 'f' || c == 'e' || c == 'g'});
            i = j;
        }
        return scan;
    }

    // printf's values, after its format: checked against its places (scanPrintFormat), each lowered and converted as a
    // reply's are (placedArguments).
    std::optional<std::vector<llvm::Value*>> Lowering::printArguments(const ast::StringExpr& literal,
                                                                     const std::vector<const ast::Expr*>& values,
                                                                     std::vector<llvm::Value*>& texts)
    {
        return placedArguments(literal, scanPrintFormat(literal.value),
                               "printf takes %d and %i, an int; %f, %e and %g, a float or a double; %s, a string; "
                               "each with C's flags '-', '+', ' ' and '0', a width and a precision; and %%, a percent "
                               "sign",
                               values, texts);
    }
} // namespace fminus::ir::detail
