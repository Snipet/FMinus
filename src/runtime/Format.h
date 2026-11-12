// src/runtime/Format.h: the text a format and its ints make, for the runtime's functions that take a string literal
// with places for ints (ir/Strings.cpp checks the literal when the program is compiled): each %d is the next int in
// decimal (a minus before a negative one), and %% is a percent sign. It allocates nothing and calls nothing: the audio
// thread may use it. The code generator has checked the literal, but the runtime trusts no byte of it: a % that is
// neither, at the end or before any other byte, is itself, and a %d past the last int is nothing.
#pragma once

#include <cstdint>

namespace fminus::runtime
{
    // Calls out(byte), a char, for each byte of the text `length` bytes at `text` and `count` ints at `ints` make, in
    // order. A null `text` or a negative `length` is the empty text, and a null `ints` or a negative `count` no ints.
    template <typename Out>
    void expandFormat(const char* text, std::int32_t length, const std::int32_t* ints, std::int32_t count,
                      Out&& out) noexcept
    {
        if (text == nullptr || length < 0)
            return;
        if (ints == nullptr || count < 0)
            count = 0;
        std::int32_t next = 0;
        for (std::int32_t i = 0; i < length; ++i)
        {
            const char c = text[i];
            if (c != '%' || i + 1 == length || (text[i + 1] != 'd' && text[i + 1] != '%'))
            {
                out(c);
                continue;
            }
            ++i;
            if (text[i] == '%')
            {
                out('%');
                continue;
            }
            if (next == count)
                continue;
            // The int's digits, least significant first, from its magnitude as an unsigned 64-bit number: INT32_MIN's
            // too.
            const std::int64_t value = ints[next++];
            std::uint64_t magnitude = static_cast<std::uint64_t>(value < 0 ? -value : value);
            char digits[10];
            int n = 0;
            do
            {
                digits[n++] = static_cast<char>('0' + magnitude % 10);
                magnitude /= 10;
            } while (magnitude != 0);
            if (value < 0)
                out('-');
            while (n > 0)
                out(digits[--n]);
        }
    }
} // namespace fminus::runtime
