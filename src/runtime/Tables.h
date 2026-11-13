// src/runtime/Tables.h: the tables the compiler prepares for the parsers' builtins (ir/Paths.cpp; Runtime.h has their
// layout), read as Json.cpp and Xml.cpp read them: a path's words and bytes, the pieces of a name or a token in it,
// the literal's bytes or an int in decimal, and json_choice's and xml_choice's literals. The runtime trusts no word of
// a table: its header is checked once, and every word and byte after it is read inside what the header says.
//
// Everything here is in an unnamed namespace: each file that includes it has its own copy, and exports none of it.
#pragma once

#include <cstddef>
#include <cstdint>
#include <cstring>

namespace
{
    // A table the compiler prepared: `words` int32s, then `length` bytes.
    struct Table
    {
        const unsigned char* code = nullptr;
        std::int32_t words = 0;
        const char* bytes = nullptr;
        std::int32_t length = 0;

        std::int32_t word(std::int32_t i) const
        {
            std::int32_t value = 0;
            std::memcpy(&value, code + 4 * static_cast<std::size_t>(i), sizeof value);
            return value;
        }
    };

    bool open(const void* table, Table& out)
    {
        if (table == nullptr)
            return false;
        const auto* base = static_cast<const unsigned char*>(table);
        std::memcpy(&out.words, base, sizeof out.words);
        std::memcpy(&out.length, base + 4, sizeof out.length);
        if (out.words < 0 || out.length < 0)
            return false;
        out.code = base + 8;
        out.bytes = reinterpret_cast<const char*>(base + 8 + 4 * static_cast<std::size_t>(out.words));
        return true;
    }

    // An int's decimal digits, a minus before a negative one, into `text` (at most 11 bytes): their count.
    int decimal(std::int32_t value, char* text)
    {
        std::uint32_t magnitude =
            value < 0 ? 0u - static_cast<std::uint32_t>(value) : static_cast<std::uint32_t>(value);
        char digits[10];
        int n = 0;
        do
        {
            digits[n++] = static_cast<char>('0' + magnitude % 10);
            magnitude /= 10;
        } while (magnitude != 0);
        int length = 0;
        if (value < 0)
            text[length++] = '-';
        while (n > 0)
            text[length++] = digits[--n];
        return length;
    }

    // A name in a prepared path (a JSON Pointer's reference token, an XML path's element or attribute name): its
    // `pieces` pairs from word `at`, and the ints its places take. each(byte) calls byte(c) for each byte of its text
    // in order, until one returns false: true when every one returned true, false when one did not or the name is not
    // well formed.
    struct Token
    {
        const Table& path;
        std::int32_t at;
        std::int32_t pieces;
        const std::int32_t* ints;
        std::int32_t count;

        template <typename Byte>
        bool each(Byte&& byte) const
        {
            for (std::int32_t p = 0; p < pieces; ++p)
            {
                const std::int32_t a = path.word(at + 2 * p);
                const std::int32_t b = path.word(at + 2 * p + 1);
                if (b >= 0)
                {
                    if (a < 0 || a > path.length - b)
                        return false;
                    for (std::int32_t i = a; i < a + b; ++i)
                    {
                        if (!byte(path.bytes[i]))
                            return false;
                    }
                }
                else if (b == -1)
                {
                    if (ints == nullptr || a < 0 || a >= count)
                        return false;
                    char text[11];
                    const int n = decimal(ints[a], text);
                    for (int i = 0; i < n; ++i)
                    {
                        if (!byte(text[i]))
                            return false;
                    }
                }
                else
                    return false;
            }
            return true;
        }

        // Whether its text is the `length` bytes at `name`.
        bool names(const char* name, std::size_t length) const
        {
            std::size_t matched = 0;
            return each([&](char c) { return matched < length && name[matched++] == c; }) && matched == length;
        }
    };

    // Which of a choice table's literals (`words` lengths, then their bytes one after another) the `length` bytes at
    // `text` are: the index of the first, or -1 for none, or for a table that is not well formed.
    [[maybe_unused]] std::int32_t choose(const void* choices, const char* text, std::size_t length)
    {
        Table table;
        if (!open(choices, table))
            return -1;
        std::int32_t at = 0;
        for (std::int32_t k = 0; k < table.words; ++k)
        {
            const std::int32_t size = table.word(k);
            if (size < 0 || size > table.length - at)
                return -1;
            if (static_cast<std::size_t>(size) == length && std::memcmp(table.bytes + at, text, length) == 0)
                return k;
            at += size;
        }
        return -1;
    }
} // namespace
