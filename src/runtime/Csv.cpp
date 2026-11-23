// src/runtime/Csv.cpp: the CSV reader behind FMinus's csv_* builtins (Runtime.h; docs/tables.md), in every build: no
// dependency but fast_float's numbers (Numbers.h), and it reaches nothing by itself, its bytes a file's or a
// response's.
//
// A document is RFC 4180's text, read where it lies: the bytes it was parsed from, which must outlive it, and an index
// of where each row begins (4 bytes a row), the header's names (copied), and a column's category levels when a read
// asks for them. Parsing checks the whole text once: fields separated by the delimiter, records by LF or CRLF (a lone
// CR at the end too), a field in double quotes holding the delimiter, line ends and "" for a quote, and a quote that
// opens a field closed before its end; the first record is the header, a UTF-8 byte order mark before it skipped; an
// empty line is no record; a record with more fields than the header is malformed (no document), one with fewer is
// short, its missing cells empty. A quote inside a field that did not begin with one is a byte like any other.
//
// The reads walk a row's fields from its start. A number is XML Schema's syntax as Numbers.h reads it (a sign, '+'
// too, leading zeros, inf, infinity and nan in any case), correctly rounded, in no locale, with spaces and tabs around
// it allowed; an empty cell, or one that is no number, is none. csv_read fills an array of a module, row by row, from a
// table the compiler made of the module's fields, flattened to one record a column (JsonShapes.h's records;
// ir/Csv.cpp), in blocks of rows on the pool (Parallel.cpp): every row is one thread's, so the rows are the same on any
// number of threads, and a column's category codes are made first, in row order, so the same text has the same code on
// any.
#include "runtime/JsonShapes.h"
#include "runtime/Numbers.h"
#include "runtime/Runtime.h"

#include <algorithm>
#include <atomic>
#include <cstdint>
#include <cstring>
#include <limits>
#include <memory>
#include <mutex>
#include <new>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace
{
    using fminus::runtime::NumberSyntax;

    constexpr std::int32_t kMissing = -1;   // CSV_MISSING: a column the header does not have
    constexpr std::int32_t kInvalid = -2;   // CSV_INVALID: a table or an argument the reader refuses

    // A column's categories: each distinct text a code, from 0, in the order the rows first have it; and each row's
    // code, -1 for an empty cell or a short row.
    struct Levels
    {
        std::vector<std::string> labels;
        std::vector<std::int32_t> codes;
    };

    struct Document
    {
        const char* text = nullptr;
        std::int32_t end = 0;
        char delimiter = ',';
        std::vector<std::int32_t> starts;   // each row's first byte, after the header
        std::vector<std::string> names;     // the header's, unescaped
        std::mutex lock;                    // levels
        std::vector<std::unique_ptr<Levels>> levels;
    };

    const Document* documentOf(const void* doc)
    {
        return static_cast<const Document*>(doc);
    }

    // A field of a row: its bytes (a quoted one's between its quotes), whether it was quoted, and whether its bytes
    // hold "", a quote, which text unescapes.
    struct Cell
    {
        const char* data = nullptr;
        std::int32_t size = 0;
        bool escaped = false;
    };

    // Whether a record ends at `at`: LF, CRLF, or a CR that is the text's last byte.
    bool lineEnd(const char* t, std::int32_t end, std::int32_t at)
    {
        return t[at] == '\n' || (t[at] == '\r' && (at + 1 == end || t[at + 1] == '\n'));
    }

    // Scans the record that begins at `at` (no empty line), moving `at` to the next one's first byte: its fields'
    // count, or -1 when it is malformed (a quote never closed, a byte after a closing quote that is no delimiter and no
    // line end).
    std::int32_t scanRecord(const char* t, std::int32_t end, char delimiter, std::int32_t& at)
    {
        std::int32_t fields = 1;
        for (;;)
        {
            if (at < end && t[at] == '"')
            {
                ++at;
                for (;;)
                {
                    if (at >= end)
                        return -1;
                    if (t[at] == '"')
                    {
                        if (at + 1 < end && t[at + 1] == '"')
                        {
                            at += 2;
                            continue;
                        }
                        ++at;
                        break;
                    }
                    ++at;
                }
            }
            else
            {
                while (at < end && t[at] != delimiter && !lineEnd(t, end, at))
                    ++at;
            }
            if (at == end)
                return fields;
            if (t[at] == delimiter)
            {
                ++at;
                ++fields;
                continue;
            }
            if (!lineEnd(t, end, at))
                return -1;
            at += t[at] == '\r' ? (at + 1 < end ? 2 : 1) : 1;
            return fields;
        }
    }

    // The field at `at` of a parsed document, moving `at` past it and its delimiter: whether another field follows on
    // its row.
    bool nextCell(const Document& d, std::int32_t& at, Cell& cell)
    {
        const char* t = d.text;
        if (at < d.end && t[at] == '"')
        {
            const std::int32_t begin = at + 1;
            std::int32_t i = begin;
            bool escaped = false;
            while (i < d.end && !(t[i] == '"' && (i + 1 == d.end || t[i + 1] != '"')))
            {
                if (t[i] == '"')   // "", a quote
                {
                    escaped = true;
                    ++i;
                }
                ++i;
            }
            cell = {t + begin, i - begin, escaped};
            at = i + 1;
        }
        else
        {
            const std::int32_t begin = at;
            while (at < d.end && t[at] != d.delimiter && !lineEnd(t, d.end, at))
                ++at;
            cell = {t + begin, at - begin, false};
        }
        if (at < d.end && t[at] == d.delimiter)
        {
            ++at;
            return true;
        }
        return false;
    }

    // The cell of `column` on row `row`: false when the row is short.
    bool cellAt(const Document& d, std::int32_t row, std::int32_t column, Cell& cell)
    {
        std::int32_t at = d.starts[static_cast<std::size_t>(row)];
        for (std::int32_t k = 0;; ++k)
        {
            const bool more = nextCell(d, at, cell);
            if (k == column)
                return true;
            if (!more)
                return false;
        }
    }

    // Every cell of a row into `cells` (sized to the header's columns), those past a short row's end empty.
    void cellsOf(const Document& d, std::int32_t row, std::vector<Cell>& cells)
    {
        std::int32_t at = d.starts[static_cast<std::size_t>(row)];
        std::size_t k = 0;
        bool more = true;
        for (; k < cells.size() && more; ++k)
            more = nextCell(d, at, cells[k]);
        for (; k < cells.size(); ++k)
            cells[k] = Cell{};
    }

    // A cell's text, its "" unescaped into `buffer` when it has any.
    std::string_view textOf(const Cell& cell, std::string& buffer)
    {
        if (!cell.escaped)
            return {cell.data, static_cast<std::size_t>(cell.size)};
        buffer.clear();
        for (std::int32_t i = 0; i < cell.size; ++i)
        {
            buffer += cell.data[i];
            if (cell.data[i] == '"')
                ++i;   // the second of ""
        }
        return buffer;
    }

    // A cell without the spaces and tabs around it.
    std::string_view trimmed(std::string_view text)
    {
        while (!text.empty() && (text.front() == ' ' || text.front() == '\t'))
            text.remove_prefix(1);
        while (!text.empty() && (text.back() == ' ' || text.back() == '\t'))
            text.remove_suffix(1);
        return text;
    }

    // A cell's number, all of it but the spaces around it: none for an empty cell, or one with "" (no number has a
    // quote).
    bool readFloat(const Cell& cell, float& value)
    {
        const std::string_view text = trimmed({cell.data, static_cast<std::size_t>(cell.size)});
        const auto n = static_cast<std::int32_t>(text.size());
        return n > 0 && !cell.escaped && fminus::runtime::readFloat(text.data(), n, NumberSyntax::Xml, value) == n;
    }

    bool readDouble(const Cell& cell, double& value)
    {
        const std::string_view text = trimmed({cell.data, static_cast<std::size_t>(cell.size)});
        const auto n = static_cast<std::int32_t>(text.size());
        return n > 0 && !cell.escaped && fminus::runtime::readDouble(text.data(), n, NumberSyntax::Xml, value) == n;
    }

    bool readInt(const Cell& cell, std::int32_t& value)
    {
        const std::string_view text = trimmed({cell.data, static_cast<std::size_t>(cell.size)});
        const auto n = static_cast<std::int32_t>(text.size());
        return n > 0 && !cell.escaped && fminus::runtime::readInt(text.data(), n, NumberSyntax::Xml, value);
    }

    // true, false (in any case), 1 or 0, with spaces around it.
    bool readBool(const Cell& cell, bool& value)
    {
        const std::string_view text = trimmed({cell.data, static_cast<std::size_t>(cell.size)});
        const auto is = [&](std::string_view word) {
            return text.size() == word.size() && std::equal(text.begin(), text.end(), word.begin(), [](char a, char b) {
                       return (a >= 'A' && a <= 'Z' ? static_cast<char>(a - 'A' + 'a') : a) == b;
                   });
        };
        if (is("true") || text == "1")
            value = true;
        else if (is("false") || text == "0")
            value = false;
        else
            return false;
        return true;
    }

    // The column a read names: `column` when `name` is null, else the header's first column of that name; -1 for
    // none.
    std::int32_t columnOf(const Document& d, std::int32_t column, const char* name, std::int32_t length)
    {
        if (name == nullptr)
            return column >= 0 && static_cast<std::size_t>(column) < d.names.size() ? column : -1;
        if (length < 0)
            return -1;
        const std::string_view wanted(name, static_cast<std::size_t>(length));
        for (std::size_t k = 0; k < d.names.size(); ++k)
        {
            if (d.names[k] == wanted)
                return static_cast<std::int32_t>(k);
        }
        return -1;
    }

    // A column's levels, made the first time a read asks for them: every row's cell, in order, its text a code.
    const Levels& levelsOf(Document& d, std::int32_t column)
    {
        std::lock_guard guard(d.lock);
        auto& slot = d.levels[static_cast<std::size_t>(column)];
        if (slot != nullptr)
            return *slot;
        auto made = std::make_unique<Levels>();
        std::unordered_map<std::string, std::int32_t> codes;
        made->codes.resize(d.starts.size(), -1);
        std::string buffer;
        for (std::size_t r = 0; r < d.starts.size(); ++r)
        {
            Cell cell;
            if (!cellAt(d, static_cast<std::int32_t>(r), column, cell) || cell.size == 0)
                continue;
            const std::string_view text = textOf(cell, buffer);
            const auto [found, added] = codes.try_emplace(std::string(text), static_cast<std::int32_t>(codes.size()));
            if (added)
                made->labels.emplace_back(text);
            made->codes[r] = found->second;
        }
        slot = std::move(made);
        return *slot;
    }

    // ---- csv_read: rows of a module ---------------------------------------------------------------------------------

    template <typename T>
    void store(unsigned char* at, T value)
    {
        std::memcpy(at, &value, sizeof value);
    }

    std::int32_t defaultInt(double value)
    {
        if (!(value == value))
            return 0;
        return static_cast<std::int32_t>(std::clamp(value, -2147483648.0, 2147483647.0));
    }

    bool inRange(const Field& field, double value)
    {
        return (field.flags & FieldRange) == 0 || (value >= field.least && value <= field.greatest);
    }

    // A column's record: its field, where its shape is, the header's column (or -1), and its levels (a category's).
    struct Column
    {
        Field field;
        std::int32_t kind = 0;
        std::int32_t column = -1;
        const Levels* levels = nullptr;
    };

    struct Reading
    {
        const Document* document = nullptr;
        const Table* table = nullptr;
        const std::vector<Column>* columns = nullptr;
        unsigned char* rows = nullptr;
        std::int64_t size = 0;
        std::int32_t count = 0;   // rows to read
        std::int32_t block = 0;   // rows an iteration reads
        std::atomic<std::int64_t> complete{0};
    };

    // A field's default, of its kind.
    void storeDefault(const Table& table, const Column& c, unsigned char* at)
    {
        const Field& field = c.field;
        switch (c.kind)
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
            if (table.word(field.shape + 1) == 1)
                store(at, static_cast<unsigned char>(field.fallback != 0.0 ? 1 : 0));
            else
                store(at, defaultInt(field.fallback));
            break;
        default:
            break;   // a char array has none: checked
        }
    }

    // Fills a column's value of row `row` at `at` from its cell: whether it was filled.
    bool fill(const Table& table, const Column& c, std::int32_t row, const Cell& cell, unsigned char* at,
              std::string& buffer)
    {
        const Field& field = c.field;
        switch (c.kind)
        {
        case ShapeFloat:
        {
            float number = 0.0f;
            if (!readFloat(cell, number) || !inRange(field, static_cast<double>(number)))
                return false;
            store(at, number);
            return true;
        }
        case ShapeDouble:
        {
            double number = 0.0;
            if (!readDouble(cell, number) || !inRange(field, number))
                return false;
            store(at, number);
            return true;
        }
        case ShapeInt:
        {
            std::int32_t number = 0;
            if (c.levels != nullptr)
                number = c.levels->codes[static_cast<std::size_t>(row)];
            if (c.levels != nullptr ? number < 0 : !readInt(cell, number) || !inRange(field, number))
                return false;
            store(at, number);
            return true;
        }
        case ShapeBool:
        {
            bool value = false;
            if (!readBool(cell, value))
                return false;
            store(at, static_cast<unsigned char>(value ? 1 : 0));
            return true;
        }
        case ShapeEnum:
        {
            const std::int32_t size = table.word(field.shape + 1);
            const std::int32_t names = table.word(field.shape + 2);
            const std::string_view text = trimmed(textOf(cell, buffer));
            std::int32_t number = 0;
            bool found = false;
            for (std::int32_t k = 0; k < names && !found; ++k)
            {
                const std::int32_t entry = field.shape + 3 + 3 * k;
                const auto n = static_cast<std::size_t>(table.word(entry + 2));
                if (n == text.size() && std::memcmp(table.bytes + table.word(entry + 1), text.data(), n) == 0)
                {
                    number = table.word(entry);
                    found = true;
                }
            }
            if (!found)
                found = readInt(cell, number);
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
            const auto size = static_cast<std::size_t>(table.word(field.shape + 1));
            const std::string_view text = textOf(cell, buffer);
            if (text.size() >= size)
                return false;
            std::memcpy(at, text.data(), text.size());
            std::memset(at + text.size(), 0, size - text.size());
            return true;
        }
        default:
            return false;
        }
    }

    // The rows of one iteration's block, each filled, column by column; the rows with every column that has no
    // default filled counted into `complete`.
    void readRows(void* context, std::int32_t begin, std::int32_t end)
    {
        Reading& r = *static_cast<Reading*>(context);
        const Document& d = *r.document;
        std::vector<Cell> cells(d.names.size());
        std::string buffer;
        std::int64_t complete = 0;
        for (std::int32_t b = begin; b < end; ++b)
        {
            const std::int32_t first = b * r.block;
            const std::int32_t last = std::min(r.count, first + r.block);
            for (std::int32_t row = first; row < last; ++row)
            {
                cellsOf(d, row, cells);
                unsigned char* base = r.rows + static_cast<std::int64_t>(row) * r.size;
                bool all = true;
                for (const Column& c : *r.columns)
                {
                    unsigned char* at = base + c.field.offset;
                    const bool has = c.column >= 0;
                    if (has && fill(*r.table, c, row, cells[static_cast<std::size_t>(c.column)], at, buffer))
                        continue;
                    if ((c.field.flags & FieldDefault) != 0)
                        storeDefault(*r.table, c, at);
                    else
                    {
                        all = false;
                        if (c.kind == ShapeFloat)
                            store(at, std::numeric_limits<float>::quiet_NaN());
                        else if (c.kind == ShapeDouble)
                            store(at, std::numeric_limits<double>::quiet_NaN());
                    }
                }
                complete += all ? 1 : 0;
            }
        }
        r.complete.fetch_add(complete, std::memory_order_relaxed);
    }

    template <typename T, typename Read>
    std::int32_t readColumn(const void* doc, std::int32_t column, const char* name, std::int32_t length, T* out,
                            std::int32_t cap, Read read)
    {
        const Document* d = documentOf(doc);
        if (d == nullptr)
            return kMissing;
        const std::int32_t k = columnOf(*d, column, name, length);
        if (k < 0)
            return kMissing;
        const auto rows = static_cast<std::int32_t>(d->starts.size());
        const std::int32_t written = out == nullptr || cap < 0 ? 0 : std::min(cap, rows);
        for (std::int32_t r = 0; r < written; ++r)
        {
            Cell cell;
            out[r] = read(cellAt(*d, r, k, cell) ? &cell : nullptr);
        }
        return rows;
    }
} // namespace

extern "C" void* fminus_rt_csv_parse(const void* data, std::int32_t bytes, std::int32_t delimiter) noexcept
{
    if (data == nullptr || bytes <= 0 || delimiter < 1 || delimiter > 255 || delimiter == '"' || delimiter == '\n'
        || delimiter == '\r')
        return nullptr;
    try
    {
        auto d = std::make_unique<Document>();
        d->text = static_cast<const char*>(data);
        d->end = bytes;
        d->delimiter = static_cast<char>(delimiter);
        const char* t = d->text;
        std::int32_t at = bytes >= 3 && std::memcmp(t, "\xEF\xBB\xBF", 3) == 0 ? 3 : 0;   // a UTF-8 byte order mark
        if (at == bytes || lineEnd(t, bytes, at))
            return nullptr;   // no header
        const std::int32_t header = at;
        const std::int32_t columns = scanRecord(t, bytes, d->delimiter, at);
        if (columns < 0)
            return nullptr;
        d->starts.push_back(header);   // the header's record, for its cells; taken off below
        std::vector<Cell> cells(static_cast<std::size_t>(columns));
        cellsOf(*d, 0, cells);
        std::string buffer;
        for (const Cell& cell : cells)
            d->names.emplace_back(textOf(cell, buffer));
        d->starts.clear();
        while (at < bytes)
        {
            if (lineEnd(t, bytes, at))   // an empty line: no record
            {
                at += t[at] == '\r' ? (at + 1 < bytes ? 2 : 1) : 1;
                continue;
            }
            const std::int32_t start = at;
            const std::int32_t fields = scanRecord(t, bytes, d->delimiter, at);
            if (fields < 0 || fields > columns)
                return nullptr;
            d->starts.push_back(start);
        }
        d->levels.resize(d->names.size());
        return d.release();
    }
    catch (const std::bad_alloc&)
    {
        return nullptr;
    }
}

extern "C" void fminus_rt_csv_free(void* doc) noexcept
{
    delete static_cast<Document*>(doc);
}

extern "C" std::int32_t fminus_rt_csv_rows(const void* doc) noexcept
{
    const Document* d = documentOf(doc);
    return d == nullptr ? 0 : static_cast<std::int32_t>(d->starts.size());
}

extern "C" std::int32_t fminus_rt_csv_columns(const void* doc) noexcept
{
    const Document* d = documentOf(doc);
    return d == nullptr ? 0 : static_cast<std::int32_t>(d->names.size());
}

extern "C" const char* fminus_rt_csv_name(const void* doc, std::int32_t column, std::int32_t* length) noexcept
{
    const Document* d = documentOf(doc);
    const bool inside = d != nullptr && column >= 0 && static_cast<std::size_t>(column) < d->names.size();
    if (length != nullptr)
        *length = inside ? static_cast<std::int32_t>(d->names[static_cast<std::size_t>(column)].size()) : 0;
    return inside ? d->names[static_cast<std::size_t>(column)].data() : nullptr;
}

extern "C" std::int32_t fminus_rt_csv_find(const void* doc, const char* name, std::int32_t length) noexcept
{
    const Document* d = documentOf(doc);
    if (d == nullptr || (name == nullptr && length > 0))
        return -1;
    return columnOf(*d, -1, name == nullptr ? "" : name, length);
}

extern "C" std::int32_t fminus_rt_csv_floats(const void* doc, std::int32_t column, const char* name,
                                             std::int32_t length, float* out, std::int32_t cap) noexcept
{
    return readColumn(doc, column, name, length, out, cap, [](const Cell* cell) {
        float value = std::numeric_limits<float>::quiet_NaN();
        return cell != nullptr && readFloat(*cell, value) ? value : std::numeric_limits<float>::quiet_NaN();
    });
}

extern "C" std::int32_t fminus_rt_csv_doubles(const void* doc, std::int32_t column, const char* name,
                                              std::int32_t length, double* out, std::int32_t cap) noexcept
{
    return readColumn(doc, column, name, length, out, cap, [](const Cell* cell) {
        double value = std::numeric_limits<double>::quiet_NaN();
        return cell != nullptr && readDouble(*cell, value) ? value : std::numeric_limits<double>::quiet_NaN();
    });
}

extern "C" std::int32_t fminus_rt_csv_ints(const void* doc, std::int32_t column, const char* name,
                                           std::int32_t length, std::int32_t* out, std::int32_t cap,
                                           std::int32_t fallback) noexcept
{
    return readColumn(doc, column, name, length, out, cap, [fallback](const Cell* cell) {
        std::int32_t value = fallback;
        return cell != nullptr && readInt(*cell, value) ? value : fallback;
    });
}

extern "C" std::int32_t fminus_rt_csv_read(void* doc, const void* shape, void* rows, std::int32_t size,
                                           std::int32_t cap) noexcept
{
    Table table;
    if (!open(shape, table) || table.words < 2 || table.word(0) != ShapeModule
        || shapeEnd(table, 0, 0, true, true) != table.words || size <= 0 || cap < 0 || (rows == nullptr && cap > 0))
        return kInvalid;
    auto* d = static_cast<Document*>(doc);
    if (d == nullptr)
        return 0;   // a null document has no rows
    try
    {
        // Each field a column: a leaf of a number, a bool, an enumeration or a char array (the compiler flattened the
        // module), inside the module's `size` bytes.
        std::vector<Column> columns;
        const std::int32_t fields = table.word(1);
        std::int32_t record = 2;
        for (std::int32_t k = 0; k < fields; ++k)
        {
            Column c;
            c.field = fieldAt(table, record);
            record += c.field.size;
            c.kind = table.word(c.field.shape);
            const std::int64_t bytes = c.kind == ShapeDouble ? 8
                                       : c.kind == ShapeBool  ? 1
                                       : c.kind == ShapeChars ? table.word(c.field.shape + 1)
                                       : c.kind == ShapeEnum  ? table.word(c.field.shape + 1)
                                                              : 4;
            if ((c.kind != ShapeFloat && c.kind != ShapeDouble && c.kind != ShapeInt && c.kind != ShapeBool
                 && c.kind != ShapeEnum && c.kind != ShapeChars)
                || std::int64_t{c.field.offset} + bytes > size)
                return kInvalid;
            c.column = columnOf(*d, -1, table.bytes + c.field.name, c.field.nameLength);
            if (c.column < 0 && (c.field.flags & FieldDefault) == 0)
                return kMissing;
            if ((c.field.flags & FieldCategory) != 0 && c.column >= 0)
                c.levels = &levelsOf(*d, c.column);
            columns.push_back(c);
        }
        Reading r;
        r.document = d;
        r.table = &table;
        r.columns = &columns;
        r.rows = static_cast<unsigned char*>(rows);
        r.size = size;
        r.count = std::min(cap, static_cast<std::int32_t>(d->starts.size()));
        r.block = 1024;
        const std::int32_t blocks = (r.count + r.block - 1) / r.block;
        if (blocks > 1)   // on the pool, every block a thread's
        {
            fminus_rt_parallel_start();
            fminus_rt_parallel_for(readRows, &r, 0, blocks, 0);
            fminus_rt_parallel_stop();
        }
        else
            readRows(&r, 0, blocks);
        return static_cast<std::int32_t>(r.complete.load(std::memory_order_relaxed));
    }
    catch (const std::bad_alloc&)
    {
        return kInvalid;
    }
}

extern "C" std::int32_t fminus_rt_csv_levels(void* doc, std::int32_t column, const char* name,
                                             std::int32_t length) noexcept
{
    auto* d = static_cast<Document*>(doc);
    if (d == nullptr)
        return kMissing;
    const std::int32_t k = columnOf(*d, column, name, length);
    if (k < 0)
        return kMissing;
    try
    {
        return static_cast<std::int32_t>(levelsOf(*d, k).labels.size());
    }
    catch (const std::bad_alloc&)
    {
        return kInvalid;
    }
}

extern "C" const char* fminus_rt_csv_label(void* doc, std::int32_t column, const char* name, std::int32_t length,
                                           std::int32_t code, std::int32_t* size) noexcept
{
    if (size != nullptr)
        *size = 0;
    auto* d = static_cast<Document*>(doc);
    if (d == nullptr)
        return nullptr;
    const std::int32_t k = columnOf(*d, column, name, length);
    if (k < 0)
        return nullptr;
    try
    {
        const Levels& levels = levelsOf(*d, k);
        if (code < 0 || static_cast<std::size_t>(code) >= levels.labels.size())
            return nullptr;
        const std::string& label = levels.labels[static_cast<std::size_t>(code)];
        if (size != nullptr)
            *size = static_cast<std::int32_t>(label.size());
        return label.data();
    }
    catch (const std::bad_alloc&)
    {
        return nullptr;
    }
}
