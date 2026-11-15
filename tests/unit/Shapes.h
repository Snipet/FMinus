// tests/unit/Shapes.h: the tables a compiler will emit for the JSON writer and the module reader (runtime/Runtime.h,
// "shapes"), built as words and bytes from C++, for the tests of fminus_rt_json_write and fminus_rt_json_module: a
// shape is a vector of words, whose names are offsets into the table's bytes, and a table lays them out as the
// program's constant is (its counts, its code, its bytes at 4-byte alignment).
#pragma once

#include <bit>
#include <cstdint>
#include <cstring>
#include <initializer_list>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace fminus::test::shapes
{
    using Words = std::vector<std::int32_t>;

    enum : std::int32_t
    {
        Float = 1,
        Double = 2,
        Int = 3,
        Bool = 4,
        String = 5,
        Chars = 6,
        Enum = 7,
        Pointer = 8,
        Array = 9,
        Span = 10,
        Vector = 11,
        Module = 12,
    };

    // A table being built: its code, and the bytes its names and literals are.
    class Table
    {
    public:
        // The offset of `text` among the bytes (added).
        std::int32_t text(std::string_view text)
        {
            const auto at = static_cast<std::int32_t>(bytes_.size());
            bytes_ += text;
            return at;
        }

        Words name(std::string_view name)
        {
            return {text(name), static_cast<std::int32_t>(name.size())};
        }

        // Shapes.
        static Words scalar(std::int32_t kind)
        {
            return {kind};
        }
        static Words chars(std::int32_t n)
        {
            return {Chars, n};
        }
        Words enumeration(std::int32_t size, const std::vector<std::pair<std::int32_t, std::string_view>>& names)
        {
            Words w{Enum, size, static_cast<std::int32_t>(names.size())};
            for (const auto& [value, name] : names)
            {
                w.push_back(value);
                const Words n = this->name(name);
                w.insert(w.end(), n.begin(), n.end());
            }
            return w;
        }
        static Words pointer(const Words& shape)
        {
            return join({Pointer}, shape);
        }
        static Words array(std::int32_t count, std::int32_t stride, const Words& shape)
        {
            return join({Array, count, stride}, shape);
        }
        static Words span(std::int32_t capacity, std::int32_t stride, const Words& shape)
        {
            return join({Span, capacity, stride}, shape);
        }
        static Words vector(std::int32_t lanes)
        {
            return {Vector, lanes};
        }

        struct FieldSpec
        {
            std::string_view name;
            std::int32_t offset;
            Words shape;
            std::optional<std::pair<double, double>> range = {};
            std::optional<double> fallback = {};
        };

        Words module(const std::vector<FieldSpec>& fields)
        {
            Words w{Module, static_cast<std::int32_t>(fields.size())};
            for (const FieldSpec& f : fields)
            {
                Words record{0};
                const Words n = name(f.name);
                record.insert(record.end(), n.begin(), n.end());
                record.push_back(f.offset);
                record.push_back((f.range ? 1 : 0) | (f.fallback ? 2 : 0));
                if (f.range)
                {
                    addDouble(record, f.range->first);
                    addDouble(record, f.range->second);
                }
                if (f.fallback)
                    addDouble(record, *f.fallback);
                record.insert(record.end(), f.shape.begin(), f.shape.end());
                record[0] = static_cast<std::int32_t>(record.size());
                w.insert(w.end(), record.begin(), record.end());
            }
            return w;
        }

        // The writer's operations.
        void literal(std::string_view bytes)
        {
            const Words n = name(bytes);
            code.push_back(0);
            code.insert(code.end(), n.begin(), n.end());
        }
        void value(std::int32_t slot, const Words& shape)
        {
            code.push_back(1);
            code.push_back(slot);
            code.insert(code.end(), shape.begin(), shape.end());
        }

        // The table laid out: words, length, code, bytes (padded to a word).
        Words layout() const
        {
            Words table{static_cast<std::int32_t>(code.size()), static_cast<std::int32_t>(bytes_.size())};
            table.insert(table.end(), code.begin(), code.end());
            const std::size_t at = table.size();
            table.resize(at + (bytes_.size() + 3) / 4);
            if (!bytes_.empty())
                std::memcpy(table.data() + at, bytes_.data(), bytes_.size());
            return table;
        }

        static void addDouble(Words& w, double value)
        {
            const auto bits = std::bit_cast<std::uint64_t>(value);
            w.push_back(static_cast<std::int32_t>(static_cast<std::uint32_t>(bits & 0xffffffffu)));
            w.push_back(static_cast<std::int32_t>(static_cast<std::uint32_t>(bits >> 32)));
        }

        static Words join(Words a, const Words& b)
        {
            a.insert(a.end(), b.begin(), b.end());
            return a;
        }

        Words code;

    private:
        std::string bytes_;
    };
} // namespace fminus::test::shapes
