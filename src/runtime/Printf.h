// src/runtime/Printf.h: C's printf, the part of it FMinus has: the text a format and its values make, for the printf
// and eprintf builtins (Print.cpp) and the web server's replies (web/Server.cpp, http_reply_text). The code generator
// checks the literal when the program is compiled (ir/Print.cpp's scanPrintFormat for printf's, ir/WebServer.cpp's
// scanReplyFormat for a reply's, a part of it) and passes every number as a double (an int's exactly, a float's
// widened) and every %s's string in two arrays, the bytes' addresses and their lengths. This writes the text, every
// digit FMinus's own (std::to_chars, in no locale), so it is the same on every machine.
//
// A place is C's: '%', flags ('-' to the left of its width, '+' and ' ' before a number that is not negative, '0' to
// pad a number with zeros), a width, a precision ('.' and digits: an int's fewest digits, a float's digits after the
// point (%f, %e) or significant ones (%g), a string's most bytes), and a conversion: d or i, an int; f, e or g, a
// double (6 digits of precision unless it says, as C's); s, a string; % a percent sign. What C's printf prints, this
// prints: std::to_chars's fixed, scientific and general formats with a precision are printf's %f, %e and %g (the C++
// standard defines them so), and the rest is C's rules for the flags, the width and an int's precision.
//
// It allocates nothing and calls nothing but std::to_chars and `out`: the audio thread may use it (a reply does). The
// code generator has checked the literal, but this trusts no byte of it: a '%' that begins no place it reads is
// itself, a place past the last value writes nothing, a width or a precision is at most kMaxWidth or kMaxPrecision,
// and %s without `texts` (a reply's entry point without strings) is its own two bytes.
#pragma once

#include <charconv>
#include <cstdint>
#include <cstring>
#include <limits>

namespace fminus::runtime
{
    // A format's strings: the i-th the lengths[i] bytes at data[i]. A null or negative entry is the empty string.
    struct PrintTexts
    {
        const char* const* data = nullptr;
        const std::int32_t* lengths = nullptr;
        std::int32_t count = 0;
    };

    // The widest width and precision a place may have: the code generator refuses more (ir/Print.cpp), and this reads
    // no more than these.
    inline constexpr std::int32_t kMaxPrintWidth = 999;
    inline constexpr std::int32_t kMaxPrintPrecision = 99;

    namespace printf_detail
    {
        inline bool isDigit(char c)
        {
            return c >= '0' && c <= '9';
        }

        struct Place
        {
            bool left = false;    // '-'
            bool plus = false;    // '+'
            bool space = false;   // ' '
            bool zero = false;    // '0'
            std::int32_t width = 0;
            std::int32_t precision = -1;   // none
            char conversion = 0;
        };

        // The place that begins at text[i], a '%': its flags, width, precision and conversion, and the index of its
        // conversion in `end`; false when no place FMinus has begins there.
        inline bool read(const char* text, std::int32_t length, std::int32_t i, Place& place, std::int32_t& end)
        {
            std::int32_t j = i + 1;
            for (; j < length; ++j)
            {
                const char c = text[j];
                if (c == '-')
                    place.left = true;
                else if (c == '+')
                    place.plus = true;
                else if (c == ' ')
                    place.space = true;
                else if (c == '0')
                    place.zero = true;
                else
                    break;
            }
            std::int32_t digits = 0;
            for (; j < length && isDigit(text[j]); ++j)
            {
                if (++digits > 3)
                    return false;
                place.width = place.width * 10 + (text[j] - '0');
            }
            if (j < length && text[j] == '.')
            {
                ++j;
                place.precision = 0;
                digits = 0;
                for (; j < length && isDigit(text[j]); ++j)
                {
                    if (++digits > 2)
                        return false;
                    place.precision = place.precision * 10 + (text[j] - '0');
                }
            }
            if (j == length)
                return false;
            const char c = text[j];
            if (c != 'd' && c != 'i' && c != 'f' && c != 'e' && c != 'g' && c != 's')
                return false;
            place.conversion = c;
            end = j;
            return true;
        }

        template <typename Out>
        void repeat(Out& out, char c, std::int32_t n)
        {
            for (std::int32_t k = 0; k < n; ++k)
                out.put(c);
        }

        // A number's text, its sign apart: `sign` (0 for none), `zeros` of C's padding with '0' or an int's precision,
        // then the digits; padded to the width as the flags say.
        template <typename Out>
        void putPadded(Out& out, const Place& place, char sign, std::int32_t zeros, const char* digits,
                       std::int32_t count, bool padWithZeros)
        {
            const std::int32_t used = (sign != 0 ? 1 : 0) + zeros + count;
            const std::int32_t pad = place.width > used ? place.width - used : 0;
            if (!place.left && !padWithZeros)
                repeat(out, ' ', pad);
            if (sign != 0)
                out.put(sign);
            repeat(out, '0', zeros + (!place.left && padWithZeros ? pad : 0));
            out.put(digits, digits + count);
            if (place.left)
                repeat(out, ' ', pad);
        }

        // %d and %i: C's conversion of the value to an int, saturated (NaN is 0); a precision is its fewest digits
        // (and 0 of 0 is none), and with one, '0' is ignored, as C ignores it.
        template <typename Out>
        void putInt(Out& out, const Place& place, double value)
        {
            std::int64_t n = 0;
            if (value >= 2147483647.0)
                n = std::numeric_limits<std::int32_t>::max();
            else if (value <= -2147483648.0)
                n = std::numeric_limits<std::int32_t>::min();
            else if (value == value)
                n = static_cast<std::int32_t>(value);
            const char sign = n < 0 ? '-' : place.plus ? '+' : place.space ? ' ' : 0;
            char digits[16];
            std::int32_t count = 0;
            if (n != 0 || place.precision != 0)
                count = static_cast<std::int32_t>(std::to_chars(digits, digits + sizeof digits, n < 0 ? -n : n).ptr
                                                  - digits);
            const std::int32_t zeros = place.precision > count ? place.precision - count : 0;
            putPadded(out, place, sign, zeros, digits, count, place.zero && place.precision < 0 && !place.left);
        }

        // %f, %e and %g: printf's, through std::to_chars; an infinity or a NaN is padded with spaces, never zeros, as
        // C pads one.
        template <typename Out>
        void putReal(Out& out, const Place& place, double value)
        {
            const std::int32_t precision = place.precision < 0 ? 6 : place.precision;
            const std::chars_format format = place.conversion == 'f'   ? std::chars_format::fixed
                                             : place.conversion == 'e' ? std::chars_format::scientific
                                                                       : std::chars_format::general;
            char text[420];   // %.99f of the largest double: 309 digits, a point, 99 more and a sign
            const std::to_chars_result written = std::to_chars(text, text + sizeof text, value, format, precision);
            if (written.ec != std::errc())
                return;
            const char* digits = text;
            char sign = place.plus ? '+' : place.space ? ' ' : 0;
            if (*digits == '-')
            {
                sign = '-';
                ++digits;
            }
            const bool finite = value - value == 0;   // false for an infinity and a NaN
            putPadded(out, place, sign, 0, digits, static_cast<std::int32_t>(written.ptr - digits),
                      place.zero && finite && !place.left);
        }

        // %s: the string's bytes, at most the precision's.
        template <typename Out>
        void putText(Out& out, const Place& place, const char* bytes, std::int32_t size)
        {
            if (bytes == nullptr || size < 0)
                size = 0;
            if (place.precision >= 0 && size > place.precision)
                size = place.precision;
            const std::int32_t pad = place.width > size ? place.width - size : 0;
            if (!place.left)
                repeat(out, ' ', pad);
            if (size > 0)
                out.put(bytes, bytes + size);
            if (place.left)
                repeat(out, ' ', pad);
        }
    } // namespace printf_detail

    // Calls out.put(c) for each byte, and out.put(first, last) for each run of bytes, of the text the format (`length`
    // bytes at `text`) makes with `count` values at `values` and, when `texts` is not null, its strings. A null `text`
    // or a negative `length` is the empty text, and a null `values` or a negative `count` no values. `out` may stop
    // taking bytes (a full buffer): this goes on calling it, and `out` decides what that means.
    template <typename Out>
    void formatPrintf(Out& out, const char* text, std::int32_t length, const double* values, std::int32_t count,
                      const PrintTexts* texts = nullptr) noexcept
    {
        using namespace printf_detail;
        if (text == nullptr || length < 0)
            return;
        if (values == nullptr || count < 0)
            count = 0;
        std::int32_t next = 0;
        std::int32_t nextText = 0;
        std::int32_t run = 0;   // the bytes since the last place, written together
        for (std::int32_t i = 0; i < length; ++i)
        {
            if (text[i] != '%')
                continue;
            if (i > run)
                out.put(text + run, text + i);
            run = i;
            if (i + 1 < length && text[i + 1] == '%')
            {
                out.put('%');
                run = ++i + 1;
                continue;
            }
            Place place;
            std::int32_t end = 0;
            if (!read(text, length, i, place, end) || (place.conversion == 's' && texts == nullptr))
                continue;   // a '%' that begins no place: itself, with the run after it
            if (place.conversion == 's')
            {
                if (nextText < texts->count && texts->data != nullptr && texts->lengths != nullptr)
                    putText(out, place, texts->data[nextText], texts->lengths[nextText]);
                ++nextText;
            }
            else if (next < count)
            {
                if (place.conversion == 'd' || place.conversion == 'i')
                    putInt(out, place, values[next]);
                else
                    putReal(out, place, values[next]);
                ++next;
            }
            else
                ++next;
            i = end;
            run = end + 1;
        }
        if (length > run)
            out.put(text + run, text + length);
    }
} // namespace fminus::runtime
