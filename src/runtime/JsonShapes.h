// src/runtime/JsonShapes.h: the shapes of values in memory that the JSON writer (JsonWrite.cpp, json_write) writes and
// the module reader (Json.cpp, json_module) fills, as the compiler describes them (Runtime.h has the layout), and the
// check every description passes before either reads a byte of the program's memory by it. A shape is a tree of int32
// words in a table laid out as a path's (Tables.h: `words` int32s, then `length` bytes): its kind, then the kind's
// words. The runtime trusts no word of it: every word and byte is read inside what the table's header says, every
// count is checked, and the trees nest at most kMostShapeDepth deep, so the functions that walk one recurse no deeper.
//
// Everything here is in an unnamed namespace, as Tables.h is: each file that includes it has its own copy.
#pragma once

#include "runtime/Tables.h"

#include <bit>
#include <cstdint>

namespace
{
    // A shape's kind, its first word (Runtime.h lists the words that follow each).
    enum ShapeKind : std::int32_t
    {
        ShapeFloat = 1,     // a float (4 bytes)
        ShapeDouble = 2,    // a double (8 bytes)
        ShapeInt = 3,       // an int (4 bytes)
        ShapeBool = 4,      // a bool (1 byte, 0 or 1)
        ShapeString = 5,    // a string: its address (8 bytes), then its length, an int, at 8
        ShapeChars = 6,     // a char array of n bytes
        ShapeEnum = 7,      // an enumeration's value (an int, or 1 byte for one whose type is bool), and its names
        ShapePointer = 8,   // an address (8 bytes) of a value of the shape that follows
        ShapeArray = 9,     // count elements of the shape that follows, `stride` bytes apart
        ShapeSpan = 10,     // an address and an int count at 8, at most `capacity`, of elements `stride` bytes apart
        ShapeVector = 11,   // a float vector of 2, 4 or 8 lanes
        ShapeModule = 12,   // a module: its fields, each a name, an offset, its range and default, and its shape
    };

    // A module field's flags.
    enum : std::int32_t
    {
        FieldRange = 1,     // a range follows: the least and the greatest value, each a double in two words
        FieldDefault = 2,   // a default follows: a double in two words
    };

    // How deep shapes nest: each pointer, array, span and module is one level.
    constexpr int kMostShapeDepth = 16;

    // The most fields a module's shape has (json_module keeps a bit for each, on its stack, at each level).
    constexpr std::int32_t kMostFields = 1024;

    // A double written as two words, its IEEE bits' low 32 first.
    double wordsDouble(const Table& table, std::int32_t at)
    {
        const auto low = static_cast<std::uint32_t>(table.word(at));
        const auto high = static_cast<std::uint32_t>(table.word(at + 1));
        return std::bit_cast<double>(static_cast<std::uint64_t>(high) << 32 | low);
    }

    // Whether bytes[a] to bytes[a + n - 1] are inside the table.
    bool inBytes(const Table& table, std::int32_t a, std::int32_t n)
    {
        return a >= 0 && n >= 0 && a <= table.length - n;
    }

    // A module field's words: `size` (the record's words, this one among them), the name's bytes (a, n), its offset,
    // its flags, then the range's four words and the default's two when the flags say so, then its shape.
    struct Field
    {
        std::int32_t size = 0;
        std::int32_t name = 0;
        std::int32_t nameLength = 0;
        std::int32_t offset = 0;
        std::int32_t flags = 0;
        double least = 0;
        double greatest = 0;
        double fallback = 0;
        std::int32_t shape = 0;   // the word its shape begins at
    };

    // The field whose record begins at `at`, in a table already checked (shapeEnd).
    Field fieldAt(const Table& table, std::int32_t at)
    {
        Field f;
        f.size = table.word(at);
        f.name = table.word(at + 1);
        f.nameLength = table.word(at + 2);
        f.offset = table.word(at + 3);
        f.flags = table.word(at + 4);
        std::int32_t next = at + 5;
        if ((f.flags & FieldRange) != 0)
        {
            f.least = wordsDouble(table, next);
            f.greatest = wordsDouble(table, next + 2);
            next += 4;
        }
        if ((f.flags & FieldDefault) != 0)
        {
            f.fallback = wordsDouble(table, next);
            next += 2;
        }
        f.shape = next;
        return f;
    }

    // The kind of a shape's values once its arrays are looked through (a vector's lanes are floats): what a range and
    // a default apply to. In a table already checked.
    std::int32_t leafKind(const Table& table, std::int32_t at)
    {
        for (;;)
        {
            const std::int32_t kind = table.word(at);
            if (kind == ShapeArray)
                at += 3;
            else if (kind == ShapeVector)
                return ShapeFloat;
            else
                return kind;
        }
    }

    // The word after the shape that begins at `at`, or -1 when it is not well formed: a word or a byte outside the
    // table, an unknown kind, a negative count, size or stride, a vector of other than 2, 4 or 8 lanes, an enumeration
    // of other than 1 or 4 bytes, a module of more than kMostFields fields or a field whose size is not its record's,
    // a range or a default on a field whose values are not numbers (a range: float, double, int or an enumeration's; a
    // default: those or a bool), or a depth past kMostShapeDepth. `reading`: also a kind json_module does not fill (a
    // string, a pointer, a span) and a char array of no byte.
    std::int32_t shapeEnd(const Table& table, std::int32_t at, int depth, bool reading)
    {
        if (depth > kMostShapeDepth || at < 0 || at >= table.words)
            return -1;
        const std::int32_t kind = table.word(at++);
        const std::int32_t left = table.words - at;   // words after the kind
        switch (kind)
        {
        case ShapeFloat:
        case ShapeDouble:
        case ShapeInt:
        case ShapeBool:
            return at;
        case ShapeString:
            return reading ? -1 : at;
        case ShapeChars:
            if (left < 1 || table.word(at) < (reading ? 1 : 0))
                return -1;
            return at + 1;
        case ShapeEnum:
        {
            if (left < 2)
                return -1;
            const std::int32_t size = table.word(at);
            const std::int32_t names = table.word(at + 1);
            if ((size != 1 && size != 4) || names < 0 || names > (left - 2) / 3)
                return -1;
            for (std::int32_t k = 0; k < names; ++k)
            {
                if (!inBytes(table, table.word(at + 2 + 3 * k + 1), table.word(at + 2 + 3 * k + 2)))
                    return -1;
            }
            return at + 2 + 3 * names;
        }
        case ShapePointer:
            return reading ? -1 : shapeEnd(table, at, depth + 1, reading);
        case ShapeArray:
        case ShapeSpan:
            if ((reading && kind == ShapeSpan) || left < 2 || table.word(at) < 0 || table.word(at + 1) < 0)
                return -1;
            return shapeEnd(table, at + 2, depth + 1, reading);
        case ShapeVector:
            if (left < 1 || (table.word(at) != 2 && table.word(at) != 4 && table.word(at) != 8))
                return -1;
            return at + 1;
        case ShapeModule:
        {
            if (left < 1)
                return -1;
            const std::int32_t fields = table.word(at++);
            if (fields < 0 || fields > kMostFields)
                return -1;
            for (std::int32_t k = 0; k < fields; ++k)
            {
                if (table.words - at < 5)
                    return -1;
                const std::int32_t flags = table.word(at + 4);
                const std::int32_t extra = ((flags & FieldRange) != 0 ? 4 : 0) + ((flags & FieldDefault) != 0 ? 2 : 0);
                if (flags < 0 || flags > (FieldRange | FieldDefault) || table.word(at + 3) < 0
                    || !inBytes(table, table.word(at + 1), table.word(at + 2)) || table.words - at - 5 < extra)
                    return -1;
                const std::int32_t shape = at + 5 + extra;
                const std::int32_t end = shapeEnd(table, shape, depth + 1, reading);
                if (end < 0 || table.word(at) != end - at)
                    return -1;
                const std::int32_t leaf = leafKind(table, shape);
                const bool number = leaf == ShapeFloat || leaf == ShapeDouble || leaf == ShapeInt || leaf == ShapeEnum;
                if (((flags & FieldRange) != 0 && !number)
                    || ((flags & FieldDefault) != 0 && !number && leaf != ShapeBool))
                    return -1;
                at = end;
            }
            return at;
        }
        default:
            return -1;
        }
    }
} // namespace
