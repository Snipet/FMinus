// src/runtime/Json.cpp: the JSON parser, for FMinus's json_* builtins (Runtime.h), compiled only into a build with
// FMINUS_JSON. The engine is yyjson's reader (cmake/FMinusJson.cmake), and this is the one file that knows it: a
// document is yyjson's, every number in it left as its text (YYJSON_READ_NUMBER_AS_RAW), which fast_float reads when a
// builtin asks for it (Numbers.h), to a float or a double, each correctly rounded and in no locale. What the compiler
// gives the runtime, a prepared path and its ints (read through Tables.h, as Xml.cpp reads XML's), and what the
// builtins promise, are the runtime's own: another engine would change this file and nothing else.
//
// Parsing allocates, and is for the time before the audio. Everything else reads a finished document and writes only
// what a builtin is given to write: no allocation, no lock, no system call, and a number of steps the document's size
// bounds (a member is found by looking through its object's members; an element of an array of numbers is found at
// once, of any other array by stepping over the elements before it). The audio thread may call any of them.
#include "runtime/Runtime.h"

#if FMINUS_HAS_JSON
#include "runtime/JsonShapes.h"
#include "runtime/Numbers.h"
#include "runtime/Tables.h"

#include <algorithm>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <limits>

#if defined(__GNUC__)
#pragma GCC visibility push(hidden)
#endif
#include <yyjson.h>
#if defined(__GNUC__)
#pragma GCC visibility pop
#endif

namespace
{
    using fminus::runtime::NumberSyntax;

    // The array index a reference token's text writes, as RFC 6901 writes one: 0, or digits from 1 with no leading 0.
    // Saturated past any array's size, which then has no such element.
    bool arrayIndex(const Token& token, std::size_t& value)
    {
        std::size_t digits = 0;
        bool zero = false;
        value = 0;
        const bool written = token.each([&](char c) {
            if (c < '0' || c > '9' || zero)
                return false;
            zero = digits == 0 && c == '0';
            value = std::min<std::size_t>(value * 10 + static_cast<std::size_t>(c - '0'),
                                          std::numeric_limits<std::uint32_t>::max());
            ++digits;
            return true;
        });
        return written && digits > 0;
    }

    // The value at a prepared path in a document, or null: nothing is there, the document is null, or the path is not
    // well formed.
    yyjson_val* find(const void* document, const void* path, const std::int32_t* ints, std::int32_t count)
    {
        Table table;
        if (document == nullptr || !open(path, table))
            return nullptr;
        yyjson_val* value = yyjson_doc_get_root(static_cast<yyjson_doc*>(const_cast<void*>(document)));
        std::int32_t at = 0;
        while (at < table.words && value != nullptr)
        {
            const std::int32_t pieces = table.word(at++);
            if (pieces < 0 || pieces > (table.words - at) / 2)
                return nullptr;
            const Token token{table, at, pieces, ints, count};
            at += 2 * pieces;
            if (yyjson_is_obj(value))
            {
                yyjson_val* found = nullptr;
                std::size_t i = 0;
                std::size_t members = 0;
                yyjson_val* key = nullptr;
                yyjson_val* member = nullptr;
                yyjson_obj_foreach(value, i, members, key, member)
                {
                    if (token.names(yyjson_get_str(key), yyjson_get_len(key)))
                    {
                        found = member;
                        break;
                    }
                }
                value = found;
            }
            else if (std::size_t index = 0; yyjson_is_arr(value) && arrayIndex(token, index))
                value = yyjson_arr_get(value, index);
            else
                value = nullptr;
        }
        return value;
    }

    // A number's text as yyjson leaves it, or false for a value that is not a number.
    bool numberText(yyjson_val* value, const char*& text, std::int32_t& length)
    {
        if (!yyjson_is_raw(value))
            return false;
        text = yyjson_get_raw(value);
        length = static_cast<std::int32_t>(yyjson_get_len(value));   // within the document: at most 64 MiB
        return text != nullptr;
    }

    bool readFloat(yyjson_val* value, float& out)
    {
        const char* text = nullptr;
        std::int32_t length = 0;
        return numberText(value, text, length)
               && fminus::runtime::readFloat(text, length, NumberSyntax::Json, out) == length;
    }

    // How many of `count` values a call writes: none for a null `out` or a cap that is not positive.
    std::int32_t taken(const float* out, std::int32_t cap, std::int64_t count)
    {
        return out == nullptr || cap <= 0 ? 0 : static_cast<std::int32_t>(std::min<std::int64_t>(cap, count));
    }

    // The value of a base64 digit (RFC 4648's alphabet), or -1.
    int sextet(char c)
    {
        if (c >= 'A' && c <= 'Z')
            return c - 'A';
        if (c >= 'a' && c <= 'z')
            return c - 'a' + 26;
        if (c >= '0' && c <= '9')
            return c - '0' + 52;
        if (c == '+')
            return 62;
        if (c == '/')
            return 63;
        return -1;
    }

    // The digits of the base64 text `length` bytes at `text`, without its padding, or -1 when it is not base64: digits
    // only, then one or two '=' that make its length a multiple of 4, or none and a length that leaves no lone digit
    // (4k + 1). The bits a last digit carries past the bytes are not looked at.
    std::int64_t base64Digits(const char* text, std::size_t length)
    {
        std::size_t padding = 0;
        while (padding < 2 && padding < length && text[length - 1 - padding] == '=')
            ++padding;
        const std::size_t digits = length - padding;
        if ((padding > 0 && length % 4 != 0) || digits % 4 == 1)
            return -1;
        for (std::size_t i = 0; i < digits; ++i)
        {
            if (sextet(text[i]) < 0)
                return -1;
        }
        return static_cast<std::int64_t>(digits);
    }

    // ---- a module filled from an object (fminus_rt_json_module) ------------------------------------------------------

    template <typename T>
    void store(unsigned char* at, T value)
    {
        std::memcpy(at, &value, sizeof value);
    }

    // An int from a default (a double the compiler wrote): saturated to an int's range, NaN 0.
    std::int32_t defaultInt(double value)
    {
        if (!(value == value))
            return 0;
        return static_cast<std::int32_t>(std::clamp(value, -2147483648.0, 2147483647.0));
    }

    // Writes a field's default into a value of its leaf kind (a checked shape: one of the numbers' or a bool's).
    void storeDefault(const Table& table, std::int32_t shape, const Field& field, unsigned char* at)
    {
        switch (table.word(shape))
        {
        case ShapeFloat:
            store(at, static_cast<float>(field.fallback));
            break;
        case ShapeDouble:
            store(at, field.fallback);
            break;
        case ShapeInt:
            store(at, defaultInt(field.fallback));
            break;
        case ShapeBool:
            store(at, static_cast<unsigned char>(field.fallback != 0.0 ? 1 : 0));
            break;
        case ShapeEnum:
            if (table.word(shape + 1) == 1)
                store(at, static_cast<unsigned char>(field.fallback != 0.0 ? 1 : 0));
            else
                store(at, defaultInt(field.fallback));
            break;
        case ShapeArray:
        case ShapeVector:
        {
            const bool vector = table.word(shape) == ShapeVector;
            const std::int64_t count = table.word(shape + 1);
            const std::int64_t stride = vector ? 4 : table.word(shape + 2);
            for (std::int64_t i = 0; i < count; ++i)
            {
                if (vector)
                    store(at + 4 * i, static_cast<float>(field.fallback));
                else
                    storeDefault(table, shape + 3, field, at + i * stride);
            }
            break;
        }
        default:
            break;   // never: checked
        }
    }

    // Whether a number is inside the field's range, when it has one.
    bool inRange(const Field& field, double value)
    {
        return (field.flags & FieldRange) == 0 || (value >= field.least && value <= field.greatest);
    }

    bool readModule(const Table& table, std::int32_t at, yyjson_val* object, unsigned char* base);

    // Fills the value of the shape at `shape` (a checked one) in the memory at `at` from `value`, by the field's rules:
    // true when it was filled. `all` turns false when a module inside it left a field with no default unfilled.
    bool readShape(const Table& table, std::int32_t shape, const Field& field, yyjson_val* value, unsigned char* at,
                   bool& all)
    {
        switch (table.word(shape))
        {
        case ShapeFloat:
        {
            float number = 0.0f;
            if (!readFloat(value, number) || !inRange(field, static_cast<double>(number)))
                return false;
            store(at, number);
            return true;
        }
        case ShapeDouble:
        {
            const char* text = nullptr;
            std::int32_t length = 0;
            double number = 0.0;
            if (!numberText(value, text, length)
                || fminus::runtime::readDouble(text, length, NumberSyntax::Json, number) != length
                || !inRange(field, number))
                return false;
            store(at, number);
            return true;
        }
        case ShapeInt:
        {
            const char* text = nullptr;
            std::int32_t length = 0;
            std::int32_t number = 0;
            if (!numberText(value, text, length)
                || !fminus::runtime::readInt(text, length, NumberSyntax::Json, number) || !inRange(field, number))
                return false;
            store(at, number);
            return true;
        }
        case ShapeBool:
            if (!yyjson_is_bool(value))
                return false;
            store(at, static_cast<unsigned char>(yyjson_get_bool(value) ? 1 : 0));
            return true;
        case ShapeEnum:
        {
            const std::int32_t size = table.word(shape + 1);
            const std::int32_t names = table.word(shape + 2);
            std::int32_t number = 0;
            bool found = false;
            if (yyjson_is_str(value))
            {
                const char* text = yyjson_get_str(value);
                const std::size_t length = yyjson_get_len(value);
                for (std::int32_t k = 0; k < names && !found; ++k)
                {
                    const std::int32_t entry = shape + 3 + 3 * k;
                    const auto n = static_cast<std::size_t>(table.word(entry + 2));
                    if (n == length && std::memcmp(table.bytes + table.word(entry + 1), text, n) == 0)
                    {
                        number = table.word(entry);
                        found = true;
                    }
                }
            }
            else
            {
                const char* text = nullptr;
                std::int32_t length = 0;
                found = numberText(value, text, length)
                        && fminus::runtime::readInt(text, length, NumberSyntax::Json, number);
            }
            if (!found || !inRange(field, number) || (size == 1 && number != 0 && number != 1))
                return false;
            if (size == 1)
                store(at, static_cast<unsigned char>(number));
            else
                store(at, number);
            return true;
        }
        case ShapeChars:
        {
            const auto size = static_cast<std::size_t>(table.word(shape + 1));
            if (!yyjson_is_str(value) || yyjson_get_len(value) >= size)
                return false;
            const std::size_t length = yyjson_get_len(value);
            std::memcpy(at, yyjson_get_str(value), length);
            std::memset(at + length, 0, size - length);
            return true;
        }
        case ShapeArray:
        case ShapeVector:
        {
            if (!yyjson_is_arr(value))
                return false;
            const bool vector = table.word(shape) == ShapeVector;
            const std::int64_t count = table.word(shape + 1);
            const std::int64_t stride = vector ? 4 : table.word(shape + 2);
            std::int64_t i = 0;
            std::size_t k = 0;
            std::size_t elements = 0;
            yyjson_val* element = nullptr;
            yyjson_arr_foreach(value, k, elements, element)
            {
                if (i == count)
                    break;
                if (vector)
                {
                    float number = 0.0f;
                    if (readFloat(element, number) && inRange(field, static_cast<double>(number)))
                        store(at + 4 * i, number);
                }
                else
                    readShape(table, shape + 3, field, element, at + i * stride, all);
                ++i;
            }
            return true;
        }
        case ShapeModule:
            if (!yyjson_is_obj(value))
                return false;
            all = readModule(table, shape, value, at) && all;
            return true;
        default:
            return false;   // never: checked
        }
    }

    // Fills the module at `base` from `object` (null, or no object: none) by the module shape at `at` (a checked one),
    // in one walk of its members: each member's name looked for among the fields not yet named, the first member of a
    // name taking its field; then each field not filled set to its default, or a module to its own fields' defaults.
    // False when a field with no default, at any depth, was not filled.
    bool readModule(const Table& table, std::int32_t at, yyjson_val* object, unsigned char* base)
    {
        const std::int32_t fields = table.word(at + 1);
        const std::int32_t first = at + 2;
        std::uint64_t named[kMostFields / 64] = {};
        std::uint64_t filled[kMostFields / 64] = {};
        const auto bit = [](const std::uint64_t* set, std::int32_t k) { return (set[k / 64] >> (k % 64) & 1) != 0; };
        const auto mark = [](std::uint64_t* set, std::int32_t k) { set[k / 64] |= std::uint64_t{1} << (k % 64); };
        bool all = true;
        if (yyjson_is_obj(object))
        {
            std::int32_t left = fields;   // fields no member has named yet
            std::size_t i = 0;
            std::size_t members = 0;
            yyjson_val* key = nullptr;
            yyjson_val* member = nullptr;
            yyjson_obj_foreach(object, i, members, key, member)
            {
                if (left == 0)
                    break;
                const char* name = yyjson_get_str(key);
                const std::size_t length = yyjson_get_len(key);
                std::int32_t record = first;
                for (std::int32_t k = 0; k < fields; ++k)
                {
                    const std::int32_t size = table.word(record);
                    if (!bit(named, k) && static_cast<std::size_t>(table.word(record + 2)) == length
                        && std::memcmp(table.bytes + table.word(record + 1), name, length) == 0)
                    {
                        mark(named, k);
                        --left;
                        const Field field = fieldAt(table, record);
                        if (readShape(table, field.shape, field, member, base + field.offset, all))
                            mark(filled, k);
                        break;
                    }
                    record += size;
                }
            }
        }
        std::int32_t record = first;
        for (std::int32_t k = 0; k < fields; ++k)
        {
            const Field field = fieldAt(table, record);
            record += field.size;
            if (bit(filled, k))
                continue;
            if ((field.flags & FieldDefault) != 0)
                storeDefault(table, field.shape, field, base + field.offset);
            else if (table.word(field.shape) == ShapeModule)
                all = readModule(table, field.shape, nullptr, base + field.offset) && all;
            else
                all = false;
        }
        return all;
    }
} // namespace

extern "C" void* fminus_rt_json_parse(const void* data, std::int32_t bytes) noexcept
{
    if (data == nullptr || bytes <= 0 || bytes > FMINUS_JSON_MAX_BYTES)
        return nullptr;
    // yyjson copies the text (without YYJSON_READ_INSITU it never writes to it): the cast only fits its signature.
    yyjson_read_err error;
    return yyjson_read_opts(const_cast<char*>(static_cast<const char*>(data)), static_cast<std::size_t>(bytes),
                            YYJSON_READ_NUMBER_AS_RAW, nullptr, &error);
}

extern "C" void fminus_rt_json_free(void* document) noexcept
{
    yyjson_doc_free(static_cast<yyjson_doc*>(document));
}

extern "C" float fminus_rt_json_float(const void* document, const void* path, const std::int32_t* ints,
                                      std::int32_t count, float fallback) noexcept
{
    float value = 0.0f;
    return readFloat(find(document, path, ints, count), value) ? value : fallback;
}

extern "C" double fminus_rt_json_double(const void* document, const void* path, const std::int32_t* ints,
                                        std::int32_t count, double fallback) noexcept
{
    const char* text = nullptr;
    std::int32_t length = 0;
    double value = 0.0;
    return numberText(find(document, path, ints, count), text, length)
                   && fminus::runtime::readDouble(text, length, NumberSyntax::Json, value) == length
               ? value
               : fallback;
}

extern "C" std::int32_t fminus_rt_json_int(const void* document, const void* path, const std::int32_t* ints,
                                           std::int32_t count, std::int32_t fallback) noexcept
{
    const char* text = nullptr;
    std::int32_t length = 0;
    std::int32_t value = 0;
    return numberText(find(document, path, ints, count), text, length)
                   && fminus::runtime::readInt(text, length, NumberSyntax::Json, value)
               ? value
               : fallback;
}

extern "C" std::int32_t fminus_rt_json_bool(const void* document, const void* path, const std::int32_t* ints,
                                            std::int32_t count, std::int32_t fallback) noexcept
{
    yyjson_val* value = find(document, path, ints, count);
    if (!yyjson_is_bool(value))
        return fallback;
    return yyjson_get_bool(value) ? 1 : 0;
}

extern "C" std::int32_t fminus_rt_json_count(const void* document, const void* path, const std::int32_t* ints,
                                             std::int32_t count) noexcept
{
    yyjson_val* value = find(document, path, ints, count);
    if (yyjson_is_arr(value))
        return static_cast<std::int32_t>(yyjson_arr_size(value));   // the document's size bounds it
    if (yyjson_is_obj(value))
        return static_cast<std::int32_t>(yyjson_obj_size(value));
    return 0;
}

extern "C" std::int32_t fminus_rt_json_floats(const void* document, const void* path, const std::int32_t* ints,
                                              std::int32_t count, float* out, std::int32_t cap) noexcept
{
    yyjson_val* array = find(document, path, ints, count);
    if (!yyjson_is_arr(array))
        return 0;
    // First what it holds: numbers, or rows of numbers, never both, and how many.
    std::int64_t numbers = 0;
    bool flat = false;
    bool rows = false;
    std::size_t i = 0;
    std::size_t elements = 0;
    yyjson_val* element = nullptr;
    yyjson_arr_foreach(array, i, elements, element)
    {
        if (yyjson_is_raw(element))
        {
            flat = true;
            ++numbers;
            continue;
        }
        if (!yyjson_is_arr(element))
            return 0;
        rows = true;
        std::size_t j = 0;
        std::size_t columns = 0;
        yyjson_val* number = nullptr;
        yyjson_arr_foreach(element, j, columns, number)
        {
            if (!yyjson_is_raw(number))
                return 0;
            ++numbers;
        }
    }
    if (flat && rows)
        return 0;
    // Then the first `cap` of them, in order.
    const std::int32_t n = taken(out, cap, numbers);
    std::int32_t written = 0;
    const auto write = [&](yyjson_val* number) {
        float value = 0.0f;
        readFloat(number, value);   // a number yyjson read is one fast_float reads, whole
        out[written++] = value;
    };
    yyjson_arr_foreach(array, i, elements, element)
    {
        if (written == n)
            break;
        if (flat)
        {
            write(element);
            continue;
        }
        std::size_t j = 0;
        std::size_t columns = 0;
        yyjson_val* number = nullptr;
        yyjson_arr_foreach(element, j, columns, number)
        {
            if (written == n)
                break;
            write(number);
        }
    }
    return static_cast<std::int32_t>(numbers);   // the document's size bounds it
}

extern "C" std::int32_t fminus_rt_json_choice(const void* document, const void* path, const std::int32_t* ints,
                                              std::int32_t count, const void* choices) noexcept
{
    yyjson_val* value = find(document, path, ints, count);
    if (!yyjson_is_str(value))
        return -1;
    return choose(choices, yyjson_get_str(value), yyjson_get_len(value));
}

extern "C" std::int32_t fminus_rt_json_base64_floats(const void* document, const void* path, const std::int32_t* ints,
                                                     std::int32_t count, float* out, std::int32_t cap) noexcept
{
    yyjson_val* value = find(document, path, ints, count);
    if (!yyjson_is_str(value))
        return 0;
    const char* text = yyjson_get_str(value);
    const std::int64_t digits = base64Digits(text, yyjson_get_len(value));
    if (digits < 0)
        return 0;
    const std::int64_t bytes = digits / 4 * 3 + (digits % 4 == 0 ? 0 : digits % 4 - 1);
    const std::int64_t numbers = bytes / 4;
    const std::int32_t n = taken(out, cap, numbers);
    // Four digits are three bytes, and four bytes a float: little-endian, its bits as they are (bytes_floats').
    std::uint32_t bits = 0;
    int have = 0;   // bytes in `bits`
    std::int32_t written = 0;
    std::uint32_t buffer = 0;
    int buffered = 0;   // bits in `buffer`
    for (std::int64_t i = 0; i < digits && written < n; ++i)
    {
        buffer = buffer << 6 | static_cast<std::uint32_t>(sextet(text[i]));
        buffered += 6;
        if (buffered < 8)
            continue;
        buffered -= 8;
        bits |= (buffer >> buffered & 0xffu) << (8 * have);
        buffer &= (1u << buffered) - 1;
        if (++have == 4)
        {
            out[written++] = std::bit_cast<float>(bits);
            bits = 0;
            have = 0;
        }
    }
    return static_cast<std::int32_t>(numbers);
}

extern "C" const char* fminus_rt_json_text(const void* document, const void* path, const std::int32_t* ints,
                                           std::int32_t count, std::int32_t* length) noexcept
{
    yyjson_val* value = find(document, path, ints, count);
    const bool text = yyjson_is_str(value);
    if (length != nullptr)
        *length = text ? static_cast<std::int32_t>(yyjson_get_len(value)) : 0;   // within the document: 64 MiB
    return text ? yyjson_get_str(value) : nullptr;
}

extern "C" std::int32_t fminus_rt_json_module(const void* document, const void* path, const std::int32_t* ints,
                                              std::int32_t count, const void* shape, void* module) noexcept
{
    Table table;
    if (module == nullptr || !open(shape, table) || table.words < 1 || table.word(0) != ShapeModule
        || shapeEnd(table, 0, 0, true) != table.words)
        return 0;
    yyjson_val* object = find(document, path, ints, count);
    return readModule(table, 0, yyjson_is_obj(object) ? object : nullptr, static_cast<unsigned char*>(module)) ? 1 : 0;
}
#endif
