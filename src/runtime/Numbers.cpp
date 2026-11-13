// src/runtime/Numbers.cpp: numbers read from text, on fast_float (Numbers.h), compiled into every build (the parsers
// and string_float read numbers). fast_float is headers only, and this is the one file of the runtime that includes
// them: its inline functions are compiled here, hidden (a plugin that links FMinus exports none of them, so they
// cannot meet another copy of fast_float in the host's process), after the standard headers they use, which keep their
// own visibility.
#include "runtime/Numbers.h"

#include <algorithm>
#include <bit>
#include <cassert>
#include <cctype>
#include <cfloat>
#include <cinttypes>
#include <climits>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <iterator>
#include <limits>
#include <system_error>
#include <type_traits>
#include <version>

#if defined(__GNUC__)
#pragma GCC visibility push(hidden)
#endif
#include <fast_float/fast_float.h>
#if defined(__GNUC__)
#pragma GCC visibility pop
#endif

namespace fminus::runtime
{
    namespace
    {
        fast_float::chars_format formatOf(NumberSyntax syntax)
        {
            switch (syntax)
            {
            case NumberSyntax::Json:
                return fast_float::chars_format::json;
            case NumberSyntax::General:
                break;
            case NumberSyntax::Xml:
                return fast_float::chars_format::general | fast_float::chars_format::allow_leading_plus;
            }
            return fast_float::chars_format::general;
        }
    } // namespace

    std::int32_t readFloat(const char* text, std::int32_t length, NumberSyntax syntax, float& value) noexcept
    {
        if (text == nullptr || length <= 0)
            return 0;
        const fast_float::chars_format format = formatOf(syntax);
        float read = 0.0f;
        const fast_float::from_chars_result result = fast_float::from_chars(text, text + length, read, format);
        // Out of range is still a number, rounded as IEEE rounds it: an infinity, or a zero.
        if (result.ec != std::errc() && result.ec != std::errc::result_out_of_range)
            return 0;
        value = read;
        return static_cast<std::int32_t>(result.ptr - text);
    }

    std::int32_t readDouble(const char* text, std::int32_t length, NumberSyntax syntax, double& value) noexcept
    {
        if (text == nullptr || length <= 0)
            return 0;
        const fast_float::chars_format format = formatOf(syntax);
        double read = 0.0;
        const fast_float::from_chars_result result = fast_float::from_chars(text, text + length, read, format);
        if (result.ec != std::errc() && result.ec != std::errc::result_out_of_range)
            return 0;
        value = read;
        return static_cast<std::int32_t>(result.ptr - text);
    }

    bool readInt(const char* text, std::int32_t length, NumberSyntax syntax, std::int32_t& value) noexcept
    {
        if (text == nullptr || length <= 0)
            return false;
        // JSON's syntax: -?(0|[1-9][0-9]*)(.[0-9]+)?([eE][+-]?[0-9]+)?, the whole text. The others':
        // [+-]?([0-9]+(.[0-9]*)?|.[0-9]+)([eE][+-]?[0-9]+)?.
        const bool json = syntax == NumberSyntax::Json;
        const auto digit = [](char c) { return c >= '0' && c <= '9'; };
        std::int32_t at = 0;
        const bool negative = text[at] == '-';
        at += negative || (!json && text[at] == '+') ? 1 : 0;
        const std::int32_t whole = at;   // the integer part's digits: [whole, point)
        if (json && (at == length || !digit(text[at])))
            return false;
        if (json && text[at] == '0')
            ++at;
        else
        {
            while (at < length && digit(text[at]))
                ++at;
        }
        const std::int32_t point = at;
        std::int32_t fraction = at;   // the fraction's digits: [fraction, end)
        if (at < length && text[at] == '.')
        {
            fraction = ++at;
            if (json && (at == length || !digit(text[at])))
                return false;
            while (at < length && digit(text[at]))
                ++at;
        }
        const std::int32_t end = at;
        if (point == whole && end == fraction)
            return false;   // no digit on either side of the point (JSON's syntax has refused it already)
        std::int64_t exponent = 0;   // saturated far beyond any number of digits an int32 length allows
        if (at < length && (text[at] == 'e' || text[at] == 'E'))
        {
            ++at;
            const bool down = at < length && text[at] == '-';
            at += at < length && (text[at] == '-' || text[at] == '+') ? 1 : 0;
            if (at == length || !digit(text[at]))
                return false;
            while (at < length && digit(text[at]))
            {
                exponent = std::min<std::int64_t>(exponent * 10 + (text[at] - '0'), std::int64_t{1} << 40);
                ++at;
            }
            exponent = down ? -exponent : exponent;
        }
        if (at != length)
            return false;

        // The digits in order, the integer part's then the fraction's: digit k (from 0) weighs 10^(integers - 1 - k +
        // exponent). The number is an integer when its last nonzero digit weighs at least 1, and an int holds it only
        // when its first weighs at most 10^9.
        const std::int32_t integers = point - whole;
        const std::int32_t count = integers + (end - fraction);
        const auto digitAt = [&](std::int32_t k) {
            return text[k < integers ? whole + k : fraction + (k - integers)] - '0';
        };
        std::int32_t first = 0;
        while (first < count && digitAt(first) == 0)
            ++first;
        if (first == count)
        {
            value = 0;   // -0 too
            return true;
        }
        std::int32_t last = count - 1;
        while (digitAt(last) == 0)
            --last;
        const std::int64_t lastWeight = integers - 1 - last + exponent;
        const std::int64_t firstWeight = integers - 1 - first + exponent;
        if (lastWeight < 0 || firstWeight > 9)
            return false;
        std::int64_t magnitude = 0;   // at most ten digits: below 10^10
        for (std::int32_t k = first; k <= last; ++k)
            magnitude = magnitude * 10 + digitAt(k);
        for (std::int64_t k = 0; k < lastWeight; ++k)
            magnitude *= 10;
        const std::int64_t signedValue = negative ? -magnitude : magnitude;
        if (signedValue < std::numeric_limits<std::int32_t>::min()
            || signedValue > std::numeric_limits<std::int32_t>::max())
            return false;
        value = static_cast<std::int32_t>(signedValue);
        return true;
    }
} // namespace fminus::runtime
