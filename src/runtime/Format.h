// src/runtime/Format.h: the text a format and its ints make, for the runtime's functions that take a string literal
// with places for ints (ir/Strings.cpp checks the literal when the program is compiled): each %d is the next int in
// decimal (a minus before a negative one), and %% is a percent sign. It allocates nothing and calls nothing: the audio
// thread may use it. The code generator has checked the literal, but the runtime trusts no byte of it: a % that is
// neither, at the end or before any other byte, is itself, and a %d past the last int is nothing. The entry points
// that take strings as well (a URL's, a file's name) read %s too, the next text (expandPlaces): only they, so that a
// "%s" in a literal of the others is still its two bytes.
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

    // Whether the format, the `length` bytes at `text`, has a %s place (as expandPlaces reads it: a "%%" is a percent
    // sign, never the start of one).
    inline bool hasTextPlace(const char* text, std::int32_t length) noexcept
    {
        if (text == nullptr || length < 0)
            return false;
        for (std::int32_t i = 0; i + 1 < length; ++i)
        {
            if (text[i] != '%')
                continue;
            if (text[i + 1] == 's')
                return true;
            if (text[i + 1] == '%' || text[i + 1] == 'd')
                ++i;
        }
        return false;
    }

    // A format with places for ints and for texts, for the entry points that take strings (a URL's, a file's name):
    // as expandFormat, each %d the next of the `count` ints at `ints`, %% a percent sign, and besides each %s the next
    // of the `textCount` texts, the i-th the `lengths[i]` bytes at `texts[i]`. Calls byte(c) for each byte the format
    // and the ints make, and text(bytes, length) once for each %s, with its text whole (a %s past the last text: a null
    // address and 0, the empty text), in order. Returns false, at the first, when a text is not one: a negative
    // length, or a null address with bytes (the callbacks made until then are the caller's to discard). A null `texts`
    // or `lengths`, or a negative `textCount`, is no texts. It allocates nothing and calls nothing else.
    template <typename Byte, typename Text>
    bool expandPlaces(const char* format, std::int32_t length, const std::int32_t* ints, std::int32_t count,
                      const char* const* texts, const std::int32_t* lengths, std::int32_t textCount, Byte&& byte,
                      Text&& text) noexcept
    {
        if (format == nullptr || length < 0)
            return true;
        if (ints == nullptr || count < 0)
            count = 0;
        if (texts == nullptr || lengths == nullptr || textCount < 0)
            textCount = 0;
        std::int32_t nextInt = 0;
        std::int32_t nextText = 0;
        for (std::int32_t i = 0; i < length; ++i)
        {
            const char c = format[i];
            const char after = i + 1 < length ? format[i + 1] : '\0';
            if (c != '%' || (after != 'd' && after != 's' && after != '%'))
            {
                byte(c);
                continue;
            }
            ++i;
            if (after == '%')
            {
                byte('%');
                continue;
            }
            if (after == 's')
            {
                if (nextText == textCount)
                {
                    text(static_cast<const char*>(nullptr), std::int32_t{0});
                    continue;
                }
                const char* bytes = texts[nextText];
                const std::int32_t size = lengths[nextText++];
                if (size < 0 || (bytes == nullptr && size > 0))
                    return false;
                text(bytes, size);
                continue;
            }
            if (nextInt == count)
                continue;
            const std::int64_t value = ints[nextInt++];
            std::uint64_t magnitude = static_cast<std::uint64_t>(value < 0 ? -value : value);
            char digits[10];
            int n = 0;
            do
            {
                digits[n++] = static_cast<char>('0' + magnitude % 10);
                magnitude /= 10;
            } while (magnitude != 0);
            if (value < 0)
                byte('-');
            while (n > 0)
                byte(digits[--n]);
        }
        return true;
    }
} // namespace fminus::runtime
