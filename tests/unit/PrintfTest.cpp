// tests/unit/PrintfTest.cpp: printf's formatter (runtime/Printf.h), which printf, eprintf and the web server's replies
// write their text with, against the C library's own snprintf: every flag, width, precision and conversion FMinus's
// printf has, of ints and doubles, at the edges (0, the least int, a double's largest and smallest, an infinity, a
// NaN). And what it does with what the code generator never passes: a '%' that begins no place, a place past the last
// value, a %s without strings.
#include "runtime/Printf.h"

#include <catch2/catch_test_macros.hpp>

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <initializer_list>
#include <limits>
#include <string>
#include <string_view>
#include <vector>

namespace
{
    struct Collect
    {
        std::string text;

        void put(char c)
        {
            text += c;
        }

        void put(const char* first, const char* last)
        {
            text.append(first, last);
        }
    };

    std::string format(std::string_view literal, std::initializer_list<double> values = {},
                       const fminus::runtime::PrintTexts* texts = nullptr)
    {
        Collect out;
        const std::vector<double> numbers(values);
        fminus::runtime::formatPrintf(out, literal.data(), static_cast<std::int32_t>(literal.size()), numbers.data(),
                                      static_cast<std::int32_t>(numbers.size()), texts);
        return out.text;
    }

    // What the C library's own snprintf writes: the reference. Its format is a variable here on purpose, so the
    // project's -Wformat=2 (-Wformat-nonliteral) is turned off for this one call.
    template <typename... Values>
    std::string c(const char* literal, Values... values)
    {
        char text[1024];
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wformat-nonliteral"
        const int n = std::snprintf(text, sizeof text, literal, values...);
#pragma GCC diagnostic pop
        REQUIRE(n >= 0);
        REQUIRE(static_cast<std::size_t>(n) < sizeof text);
        return std::string(text, static_cast<std::size_t>(n));
    }
} // namespace

TEST_CASE("printf's %d and %i are C's, every flag, width and precision", "[printf]")
{
    const std::initializer_list<int> ints{0, 1, -1, 7, -42, 12345, 2147483647, -2147483647 - 1};
    const std::initializer_list<const char*> formats{
        "%d",   "%i",   "%5d",   "%-5d",  "%05d",  "%+d",  "% d",   "%+5d",  "%-+6d|", "% 05d", "%.3d",
        "%.0d", "%8.3d", "%-8.3d|", "%08.3d", "%+.2d", "%012d", "%-012d|", "%1d",  "%+-08d|"};
    for (const char* literal : formats)
    {
        for (const int n : ints)
        {
            INFO(literal << " of " << n);
            CHECK(format(literal, {static_cast<double>(n)}) == c(literal, n));
        }
    }
}

TEST_CASE("an int's place converts its double as C converts one to an int, saturated", "[printf]")
{
    CHECK(format("%d", {2.9}) == "2");
    CHECK(format("%d", {-2.9}) == "-2");
    CHECK(format("%d", {1e12}) == "2147483647");
    CHECK(format("%d", {-1e12}) == "-2147483648");
    CHECK(format("%d", {std::numeric_limits<double>::quiet_NaN()}) == "0");
}

TEST_CASE("printf's %f, %e and %g are C's, every flag, width and precision", "[printf]")
{
    const double inf = std::numeric_limits<double>::infinity();
    const double nan = std::numeric_limits<double>::quiet_NaN();
    const std::initializer_list<double> values{0.0,       -0.0,     1.0,     -1.0,    0.1,
                                               2.5,       -2.5,     3.14159265358979, 1e-7,    123456789.125,
                                               1e300,     -1e-300,  5e-324,  1.7976931348623157e308,
                                               0.5,       1.5,      1e21,    1e-5,    100000.0,
                                               999999.5,  inf,      -inf,    nan};
    const std::initializer_list<const char*> formats{
        "%f",     "%e",     "%g",     "%.0f",   "%.0e",   "%.0g",    "%.1f",   "%.3e",   "%.10g",  "%.17g",
        "%12.4f", "%-12.4f|", "%012.4f", "%+f",   "% e",    "%+012.3e", "%-+12g|", "%010g",  "%.99f",  "%.99e",
        "%.99g",  "%999.2f", "% 08.2f", "%+.0f",  "%.2g",   "%7.0e"};
    for (const char* literal : formats)
    {
        for (const double x : values)
        {
            if (std::isnan(x) && std::signbit(x))
                continue;   // a NaN's sign: C libraries differ
            INFO(literal << " of " << x);
            CHECK(format(literal, {x}) == c(literal, x));
        }
    }
}

TEST_CASE("printf's %s is the string's bytes, at most its precision's, padded to its width", "[printf]")
{
    const char* data[] = {"hello", "", "a\0b"};
    const std::int32_t lengths[] = {5, 0, 3};
    const fminus::runtime::PrintTexts texts{data, lengths, 3};
    CHECK(format("[%s][%s]", {}, &texts) == "[hello][]");
    // "hello", "" padded, two of "a\0b"'s bytes, and a place with no string left, which writes nothing.
    CHECK(format("[%8s][%-8s][%.2s][%8.3s]", {}, &texts) == std::string("[   hello][        ][a\0][]", 26));
    const fminus::runtime::PrintTexts one{data, lengths, 1};
    CHECK(format("%8s|%-8s|%.2s|%8.3s", {}, &one) == c("%8s|||", "hello"));   // one string: the rest write nothing
    const fminus::runtime::PrintTexts nul{data + 2, lengths + 2, 1};
    CHECK(format("<%s>", {}, &nul) == std::string("<a\0b>", 5));   // bytes, a NUL among them
    for (const char* literal : {"%s", "%10s", "%-10s|", "%.3s", "%10.3s", "%-10.2s|", "%.0s", "%1s"})
    {
        INFO(literal);
        CHECK(format(literal, {}, &one) == c(literal, "hello"));
    }
}

TEST_CASE("values, numbers and strings, fill their places in order, each kind from its own list", "[printf]")
{
    const char* data[] = {"saw", "x"};
    const std::int32_t lengths[] = {3, 1};
    const fminus::runtime::PrintTexts texts{data, lengths, 2};
    CHECK(format("%s %d %.2f %s %%", {7, 0.125}, &texts) == "saw 7 0.12 x %");
    CHECK(format("100%% of %d", {3}) == "100% of 3");
}

TEST_CASE("what the code generator never passes writes no surprise", "[printf]")
{
    CHECK(format("%d and %d", {1}) == "1 and ");                  // a place past the last value: nothing
    CHECK(format("%s", {}) == "%s");                              // %s without strings: its own bytes
    CHECK(format("50% off") == "50% off");                        // a '%' that begins no place: itself
    CHECK(format("%q %5.3x %") == "%q %5.3x %");
    CHECK(format("%1000d", {1}) == "%1000d");                     // past the widest width and precision
    CHECK(format("%.100f", {1}) == "%.100f");
    CHECK(format("tail %", {}) == "tail %");
    CHECK(format("", {1}).empty());
    Collect out;
    fminus::runtime::formatPrintf(out, nullptr, 3, nullptr, 0);   // a null text, a negative length: nothing
    fminus::runtime::formatPrintf(out, "abc", -1, nullptr, 0);
    CHECK(out.text.empty());
}
