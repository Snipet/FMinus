// src/runtime/Numbers.h: numbers read from text, for the runtime's parsers of JSON and XML and a string's
// string_float, on fast_float (cmake/FMinusFastFloat.cmake): correctly rounded straight to a float, never through a
// double (which rounds twice, and is one bit off now and then), and the same on every machine and in every locale (C's
// strtof reads "0,5" where the locale is German, and reads "0.5" as 0 there). In every build.
#pragma once

#include "fminus/Config.h"

#include <cstdint>

namespace fminus::runtime
{
    // Which numbers a text may write: JSON's (RFC 8259: -?int frac? exp?, no leading zero, no '+', no infinity or
    // NaN), or C's and XML Schema's decimal floats, which may also be inf, infinity and nan in any case; and XML's,
    // the general syntax with a '+' before it allowed too, as XML Schema's float, double, decimal and int write one
    // ("+1.5", "007", ".5", "1.", "INF", "-INF", "NaN").
    enum class NumberSyntax
    {
        Json,
        General,
        Xml,
    };

    // The number at the start of the `length` bytes at `text`, read into `value`: the bytes it takes, or 0 when they
    // begin with none (and `value` is as it was). A number past a float's range is an infinity of its sign, and one
    // nearer zero than the least subnormal a zero, as rounding to nearest makes them. It allocates nothing and reads
    // only the bytes it is given.
    std::int32_t readFloat(const char* text, std::int32_t length, NumberSyntax syntax, float& value) noexcept;

    // The same, read into a double: correctly rounded to a double, which is not the float readFloat gives widened.
    std::int32_t readDouble(const char* text, std::int32_t length, NumberSyntax syntax, double& value) noexcept;

    // Whether the `length` bytes at `text`, all of them, are a number of the syntax's whose value is an integer an int
    // holds, and that integer in `value` (as it was when not). Exactly, from the digits, through no float: "7", "-0",
    // "7.0", "700e-2" and "0.07e2" are 7 or 0; "1.5" is no integer, and "3e9" and "2147483648" are none an int holds,
    // so none is truncated or saturated. An exponent of any size is read (0e99999999999 is 0). JSON's syntax is
    // RFC 8259's; the others' (General, Xml) are decimal, with a sign, '+' or '-', leading zeros, and digits on either
    // side of the point or both ("+007", "7.", ".0e1"), and no infinity or NaN, which are no integers. It allocates
    // nothing and reads only the bytes it is given.
    bool readInt(const char* text, std::int32_t length, NumberSyntax syntax, std::int32_t& value) noexcept;
} // namespace fminus::runtime
