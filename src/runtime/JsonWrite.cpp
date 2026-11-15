// src/runtime/JsonWrite.cpp: the JSON writer, for FMinus's JSON literals (Runtime.h, fminus_rt_json_write), compiled
// only into a build with FMINUS_JSON. A literal's shape is a constant of the program, its keys and punctuation ready
// made and its values described (JsonShapes.h); only the values vary, and this writes the text they make straight
// into the caller's buffer: no tree, no allocation, no lock, no system call, numbers by std::to_chars (the shortest
// digits that read back the same, in no locale). It reads the description first, whole, and writes nothing for one
// that is not well formed; then it walks it once, recursing as deep as its shapes nest (at most kMostShapeDepth).
#include "runtime/Runtime.h"

#if FMINUS_HAS_JSON
#include "runtime/JsonShapes.h"
#include "runtime/Tables.h"

#include <algorithm>
#include <charconv>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <limits>
#include <type_traits>

namespace
{
    // The literal's operations (Runtime.h).
    enum : std::int32_t
    {
        WriteBytes = 0,   // bytes[a] to bytes[a + n - 1], as they are
        WriteValue = 1,   // the value of a shape at a slot
    };

    // The text written: every byte counted, those within the capacity stored.
    struct Out
    {
        char* buffer;
        std::int64_t capacity;
        std::int64_t size = 0;

        void put(char c)
        {
            if (size < capacity)
                buffer[size] = c;
            ++size;
        }

        void put(const char* bytes, std::size_t n)
        {
            if (n == 0)
                return;   // a null address too, which memcpy may not be given even for no byte
            if (size < capacity)
                std::memcpy(buffer + size, bytes, static_cast<std::size_t>(std::min<std::int64_t>(
                                                      capacity - size, static_cast<std::int64_t>(n))));
            size += static_cast<std::int64_t>(n);
        }
    };

    template <typename T>
    T load(const unsigned char* at)
    {
        T value;
        std::memcpy(&value, at, sizeof value);
        return value;
    }

    // A number, as std::to_chars writes it with no precision: the shortest digits that read back as the same value
    // (an int's in decimal). NaN and the infinities, which JSON has no number for, as null.
    template <typename T>
    void putNumber(Out& out, T value)
    {
        if constexpr (std::is_floating_point_v<T>)
        {
            if (!std::isfinite(value))
            {
                out.put("null", 4);
                return;
            }
        }
        char digits[64];
        const std::to_chars_result result = std::to_chars(digits, digits + sizeof digits, value);
        out.put(digits, static_cast<std::size_t>(result.ptr - digits));
    }

    // Bytes as a JSON string (RFC 8259): '"' and '\' escaped, U+0000 to U+001F as \b \f \n \r \t or \u00XX, every
    // other byte as it is (no UTF-8 check).
    void putString(Out& out, const char* bytes, std::size_t n)
    {
        static constexpr char kHex[] = "0123456789abcdef";
        out.put('"');
        std::size_t run = 0;   // bytes before `i` not yet put, that need no escape
        for (std::size_t i = 0; i < n; ++i)
        {
            const auto c = static_cast<unsigned char>(bytes[i]);
            if (c >= 0x20 && c != '"' && c != '\\')
            {
                ++run;
                continue;
            }
            out.put(bytes + i - run, run);
            run = 0;
            char escape[6] = {'\\', 0, 0, 0, 0, 0};
            std::size_t length = 2;
            switch (c)
            {
            case '"': escape[1] = '"'; break;
            case '\\': escape[1] = '\\'; break;
            case '\b': escape[1] = 'b'; break;
            case '\f': escape[1] = 'f'; break;
            case '\n': escape[1] = 'n'; break;
            case '\r': escape[1] = 'r'; break;
            case '\t': escape[1] = 't'; break;
            default:
                escape[1] = 'u';
                escape[2] = '0';
                escape[3] = '0';
                escape[4] = kHex[c >> 4];
                escape[5] = kHex[c & 15];
                length = 6;
            }
            out.put(escape, length);
        }
        out.put(bytes + n - run, run);
        out.put('"');
    }

    // Writes the value of the shape at `at` (a checked one) in the memory at `value`, and returns the word after the
    // shape.
    std::int32_t writeShape(const Table& table, std::int32_t at, const unsigned char* value, Out& out)
    {
        const std::int32_t kind = table.word(at++);
        switch (kind)
        {
        case ShapeFloat:
            putNumber(out, load<float>(value));
            return at;
        case ShapeDouble:
            putNumber(out, load<double>(value));
            return at;
        case ShapeInt:
            putNumber(out, load<std::int32_t>(value));
            return at;
        case ShapeBool:
            if (load<unsigned char>(value) != 0)
                out.put("true", 4);
            else
                out.put("false", 5);
            return at;
        case ShapeString:
        {
            const auto* bytes = load<const char*>(value);
            const auto n = load<std::int32_t>(value + 8);
            if (bytes == nullptr || n <= 0)
                putString(out, "", 0);
            else
                putString(out, bytes, static_cast<std::size_t>(n));
            return at;
        }
        case ShapeChars:
        {
            const auto n = static_cast<std::size_t>(table.word(at));
            const void* zero = n == 0 ? nullptr : std::memchr(value, 0, n);
            putString(out, reinterpret_cast<const char*>(value),
                      zero == nullptr ? n : static_cast<std::size_t>(static_cast<const unsigned char*>(zero) - value));
            return at + 1;
        }
        case ShapeEnum:
        {
            const std::int32_t size = table.word(at);
            const std::int32_t names = table.word(at + 1);
            const std::int32_t number = size == 1 ? load<unsigned char>(value) : load<std::int32_t>(value);
            for (std::int32_t k = 0; k < names; ++k)
            {
                const std::int32_t entry = at + 2 + 3 * k;
                if (table.word(entry) == number)
                {
                    putString(out, table.bytes + table.word(entry + 1), static_cast<std::size_t>(table.word(entry + 2)));
                    return at + 2 + 3 * names;
                }
            }
            putNumber(out, number);
            return at + 2 + 3 * names;
        }
        case ShapePointer:
        {
            const auto* target = load<const unsigned char*>(value);
            if (target != nullptr)
                return writeShape(table, at, target, out);
            out.put("null", 4);
            return shapeEnd(table, at, 0, false);
        }
        case ShapeArray:
        case ShapeSpan:
        case ShapeVector:
        {
            std::int64_t count = 0;
            std::int64_t stride = 4;
            const unsigned char* first = value;
            std::int32_t element = -1;   // the elements' shape (none: a vector's floats)
            std::int32_t end = at + 1;
            if (kind == ShapeVector)
                count = table.word(at);
            else
            {
                stride = table.word(at + 1);
                element = at + 2;
                if (kind == ShapeArray)
                    count = table.word(at);
                else
                {
                    first = load<const unsigned char*>(value);
                    count = first == nullptr ? 0 : std::clamp<std::int64_t>(load<std::int32_t>(value + 8), 0,
                                                                            table.word(at));
                }
                end = -1;
            }
            out.put('[');
            for (std::int64_t i = 0; i < count; ++i)
            {
                if (i > 0)
                    out.put(',');
                const unsigned char* item = first + i * stride;
                if (element < 0)
                    putNumber(out, load<float>(item));
                else
                    end = writeShape(table, element, item, out);
            }
            out.put(']');
            return end >= 0 ? end : shapeEnd(table, element, 0, false);
        }
        case ShapeModule:
        {
            const std::int32_t fields = table.word(at++);
            out.put('{');
            for (std::int32_t k = 0; k < fields; ++k)
            {
                const Field field = fieldAt(table, at);
                if (k > 0)
                    out.put(',');
                putString(out, table.bytes + field.name, static_cast<std::size_t>(field.nameLength));
                out.put(':');
                writeShape(table, field.shape, value + field.offset, out);
                at += field.size;
            }
            out.put('}');
            return at;
        }
        default:
            return table.words;   // never: the description was checked
        }
    }
} // namespace

extern "C" std::int32_t fminus_rt_json_write(char* buffer, std::int32_t capacity, const void* description,
                                             const void* values) noexcept
{
    Table table;
    if (!open(description, table))
        return -1;
    // The description, whole, before a byte is written.
    for (std::int32_t at = 0; at < table.words;)
    {
        if (table.words - at < 2)
            return -1;
        const std::int32_t op = table.word(at);
        if (op == WriteBytes)
        {
            if (table.words - at < 3 || !inBytes(table, table.word(at + 1), table.word(at + 2)))
                return -1;
            at += 3;
        }
        else if (op == WriteValue)
        {
            if (values == nullptr || table.word(at + 1) < 0)
                return -1;
            at = shapeEnd(table, at + 2, 0, false);
            if (at < 0)
                return -1;
        }
        else
            return -1;
    }
    Out out{buffer, buffer == nullptr || capacity < 0 ? 0 : capacity};
    for (std::int32_t at = 0; at < table.words;)
    {
        if (table.word(at) == WriteBytes)
        {
            out.put(table.bytes + table.word(at + 1), static_cast<std::size_t>(table.word(at + 2)));
            at += 3;
        }
        else
        {
            const auto* slot = static_cast<const unsigned char*>(values) + 8 * static_cast<std::size_t>(table.word(at + 1));
            at = writeShape(table, at + 2, slot, out);
        }
    }
    return static_cast<std::int32_t>(std::min<std::int64_t>(out.size, std::numeric_limits<std::int32_t>::max()));
}
#endif
