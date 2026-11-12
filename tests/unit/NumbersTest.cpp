// tests/unit/NumbersTest.cpp: the runtime's reader of numbers (runtime/Numbers.h), on fast_float, which the JSON and
// XML parsers read their numbers with: correctly rounded straight to float, or to double, JSON's syntax, C's or XML's,
// the bytes it took, and nothing past the bytes it is given; and an int, exactly, only when the number is an integer
// an int holds. Only in a build with FMINUS_JSON or FMINUS_XML (fminus/Config.h).
#include "fminus/Config.h"
#include "runtime/Numbers.h"

#include <catch2/catch_test_macros.hpp>

#include <bit>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <limits>
#include <initializer_list>
#include <string_view>
#include <utility>

#if FMINUS_HAS_JSON || FMINUS_HAS_XML
namespace
{
    using fminus::runtime::NumberSyntax;

    // The bytes readFloat takes of `text`, and the float it reads (NaN when it takes none).
    std::int32_t read(std::string_view text, NumberSyntax syntax, float& value)
    {
        value = std::numeric_limits<float>::quiet_NaN();
        return fminus::runtime::readFloat(text.data(), static_cast<std::int32_t>(text.size()), syntax, value);
    }
} // namespace

TEST_CASE("a number is read straight to the nearest float, never through a double", "[runtime][numbers]")
{
    float value = 0;
    CHECK(read("0.1", NumberSyntax::Json, value) == 3);
    CHECK(value == 0.1f);
    CHECK(read("-1234.5e-2", NumberSyntax::Json, value) == 10);
    CHECK(value == -12.345f);
    // Just past halfway between 1 and the next float: the nearest float is the next one, but the nearest double is
    // halfway exactly, which rounds to even, to 1, when it is narrowed. Read straight, it is the next float.
    CHECK(read("1.000000059604644775390625000000000001", NumberSyntax::Json, value) == 38);
    CHECK(value == std::nextafter(1.0f, 2.0f));
    // Past a float's range, an infinity; nearer zero than its least subnormal, a zero; the least subnormal itself.
    CHECK(read("1e39", NumberSyntax::Json, value) == 4);
    CHECK(value == std::numeric_limits<float>::infinity());
    CHECK(read("-1e-50", NumberSyntax::Json, value) == 6);
    CHECK(value == 0.0f);
    CHECK(std::signbit(value));
    CHECK(read("1.401298464324817e-45", NumberSyntax::Json, value) == 21);
    CHECK(value == std::numeric_limits<float>::denorm_min());
}

TEST_CASE("JSON's numbers are JSON's, and the general syntax is C's", "[runtime][numbers]")
{
    float value = 0;
    // What follows a number is not read: the caller's syntax decides what it may be.
    CHECK(read("12,", NumberSyntax::Json, value) == 2);
    CHECK(value == 12.0f);
    // JSON has no '+', no leading zero, no bare '.', no infinity or NaN.
    CHECK(read("+1", NumberSyntax::Json, value) == 0);
    CHECK(std::isnan(value));
    CHECK(read(".5", NumberSyntax::Json, value) == 0);
    CHECK(read("01", NumberSyntax::Json, value) == 0);
    CHECK(read("inf", NumberSyntax::Json, value) == 0);
    CHECK(read("nan", NumberSyntax::Json, value) == 0);
    // The general syntax takes .5 and the special values, in any case.
    CHECK(read(".5", NumberSyntax::General, value) == 2);
    CHECK(value == 0.5f);
    CHECK(read("-INF", NumberSyntax::General, value) == 4);
    CHECK(value == -std::numeric_limits<float>::infinity());
    CHECK(read("NaN", NumberSyntax::General, value) == 3);
    CHECK(std::isnan(value));
    // A comma is no decimal point, in any locale.
    CHECK(read("0,5", NumberSyntax::General, value) == 1);
    CHECK(value == 0.0f);
}

TEST_CASE("a number is read from the bytes given and no further", "[runtime][numbers]")
{
    float value = 0;
    const char text[] = "123456";
    CHECK(fminus::runtime::readFloat(text, 3, NumberSyntax::Json, value) == 3);
    CHECK(value == 123.0f);
    value = 5;
    CHECK(fminus::runtime::readFloat(text, 0, NumberSyntax::Json, value) == 0);
    CHECK(fminus::runtime::readFloat(text, -1, NumberSyntax::Json, value) == 0);
    CHECK(fminus::runtime::readFloat(nullptr, 3, NumberSyntax::Json, value) == 0);
    CHECK(value == 5.0f);   // untouched where there is no number
}

TEST_CASE("readDouble is the nearest double, which is not the nearest float widened", "[runtime][numbers]")
{
    double value = 0;
    const auto readDouble = [&](std::string_view text) {
        value = std::numeric_limits<double>::quiet_NaN();
        return fminus::runtime::readDouble(text.data(), static_cast<std::int32_t>(text.size()), NumberSyntax::Json,
                                           value);
    };
    CHECK(readDouble("0.1") == 3);
    CHECK(value == 0.1);
    CHECK(value != static_cast<double>(0.1f));
    CHECK(readDouble("16777217.000000001") == 18);
    CHECK(value == 16777217.0);
    CHECK(readDouble("1e309") == 5);
    CHECK(value == std::numeric_limits<double>::infinity());
    CHECK(readDouble("4.9406564584124654e-324") == 23);
    CHECK(value == std::numeric_limits<double>::denorm_min());
    CHECK(readDouble("+1") == 0);
    CHECK(std::isnan(value));
}

TEST_CASE("readInt is a JSON number that is an integer an int holds, exactly, and nothing else", "[runtime][numbers]")
{
    const auto readInt = [](std::string_view text, std::int32_t& value) {
        value = -99;
        return fminus::runtime::readInt(text.data(), static_cast<std::int32_t>(text.size()), NumberSyntax::Json, value);
    };
    std::int32_t value = 0;
    for (const auto& [text, expected] : std::initializer_list<std::pair<std::string_view, std::int32_t>>{
             {"0", 0}, {"-0", 0}, {"7", 7}, {"7.0", 7}, {"7.000", 7}, {"700e-2", 7}, {"0.07e2", 7}, {"0.07E+2", 7},
             {"1e9", 1000000000}, {"2147483647", 2147483647}, {"-2147483648", -2147483647 - 1},
             {"21474836.47e2", 2147483647}, {"0e99999999999", 0}, {"0.0000001e7", 1}, {"-12", -12},
             {"10000000000e-1", 1000000000}})
    {
        INFO(text);
        CHECK(readInt(text, value));
        CHECK(value == expected);
    }
    // Not an integer, not one an int holds, or not JSON's: false, and the value as it was.
    for (std::string_view text : {"1.5", "0.1", "1e-1", "3e9", "2147483648", "-2147483649", "1e10",
                                  "2147483647.0000000001", "1e99999999999", "-1e99999999999", "01", "+1", ".5", "1.",
                                  "1e", "-", "", "1 ", " 1", "0x10", "1,0", "NaN"})
    {
        INFO(text);
        CHECK(!readInt(text, value));
        CHECK(value == -99);
    }
    CHECK(!fminus::runtime::readInt(nullptr, 3, NumberSyntax::Json, value));
    CHECK(fminus::runtime::readInt("123", 2, NumberSyntax::Json, value));   // the bytes given, and no further
    CHECK(value == 12);
}

TEST_CASE("XML's numbers: a '+', leading zeros and a bare point too, in readFloat and readInt", "[runtime][numbers]")
{
    float value = 0;
    for (const auto& [text, expected] : std::initializer_list<std::pair<std::string_view, float>>{
             {"+1.5", 1.5f}, {"007", 7.0f}, {".5", 0.5f}, {"1.", 1.0f}, {"-1E-1", -0.1f}, {"+.25e1", 2.5f}})
    {
        INFO(text);
        CHECK(read(text, NumberSyntax::Xml, value) == static_cast<std::int32_t>(text.size()));
        CHECK(value == expected);
    }
    CHECK(read("INF", NumberSyntax::Xml, value) == 3);
    CHECK(value == std::numeric_limits<float>::infinity());
    CHECK(read("-INF", NumberSyntax::Xml, value) == 4);
    CHECK(value == -std::numeric_limits<float>::infinity());
    CHECK(read("NaN", NumberSyntax::Xml, value) == 3);
    CHECK(std::isnan(value));
    CHECK(read("+1", NumberSyntax::General, value) == 0);   // C's general syntax, as fast_float reads it, takes no '+'
    for (std::string_view text : {"", "+", "-", ".", "e1", " 1", ",5", "++1"})
    {
        INFO(text);
        CHECK(read(text, NumberSyntax::Xml, value) == 0);
    }
    CHECK(read("1,5", NumberSyntax::Xml, value) == 1);   // a comma is never a decimal point
    CHECK(read("0x10", NumberSyntax::Xml, value) == 1);  // nor is hexadecimal read: the 0, and no further

    const auto readInt = [](std::string_view text, std::int32_t& value) {
        value = -99;
        return fminus::runtime::readInt(text.data(), static_cast<std::int32_t>(text.size()), NumberSyntax::Xml, value);
    };
    std::int32_t integer = 0;
    for (const auto& [text, expected] : std::initializer_list<std::pair<std::string_view, std::int32_t>>{
             {"7", 7}, {"+7", 7}, {"-7", -7}, {"007", 7}, {"-0", 0}, {"+0", 0}, {"7.", 7}, {"7.0", 7}, {".7e1", 7},
             {"0000000000002147483647", 2147483647}, {"-2147483648", -2147483647 - 1}, {"1E9", 1000000000}})
    {
        INFO(text);
        CHECK(readInt(text, integer));
        CHECK(integer == expected);
    }
    for (std::string_view text : {"", "+", "-", ".", "+.", "e1", ".e1", "1.5", ".5", "2147483648", "3e9", "INF", "NaN",
                                  "1 ", " 1", "++1", "+-1", "0x10", "1,0", "true"})
    {
        INFO(text);
        CHECK(!readInt(text, integer));
        CHECK(integer == -99);
    }
}
#endif
