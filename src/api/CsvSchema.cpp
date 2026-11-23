// src/api/CsvSchema.cpp: a module from a CSV file's header and contents (CsvSchema.h). Every column is read through the
// runtime's reader, its distinct texts as its categories (fminus_rt_csv_levels and _label: what csv_read sees), and
// typed by them: numbers read as runtime/Numbers.h reads a cell, XML Schema's syntax.
#include "api/CsvSchema.h"

#include "circuit/Emit.h"   // isFMinusKeyword
#include "runtime/Numbers.h"
#include "runtime/Runtime.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <format>
#include <limits>
#include <set>
#include <string>
#include <vector>

namespace fminus
{
    namespace
    {
        using fminus::runtime::NumberSyntax;

        constexpr std::size_t kMostEnumerated = 16;   // distinct words an enumeration is made of, at most

        std::string_view trimmed(std::string_view text)
        {
            while (!text.empty() && (text.front() == ' ' || text.front() == '\t'))
                text.remove_prefix(1);
            while (!text.empty() && (text.back() == ' ' || text.back() == '\t'))
                text.remove_suffix(1);
            return text;
        }

        bool isLetter(char c)
        {
            return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '_';
        }

        bool isDigit(char c)
        {
            return c >= '0' && c <= '9';
        }

        bool isIdentifier(std::string_view text)
        {
            if (text.empty() || !isLetter(text.front()))
                return false;
            return std::all_of(text.begin(), text.end(), [](char c) { return isLetter(c) || isDigit(c); });
        }

        // Decimal digits with a sign or none: an integer's text, not "7.0" or "1e3".
        bool integerText(std::string_view text)
        {
            if (!text.empty() && (text.front() == '-' || text.front() == '+'))
                text.remove_prefix(1);
            return !text.empty() && std::all_of(text.begin(), text.end(), isDigit);
        }

        // The significant digits a number's text writes: its mantissa's, without leading zeros, nor trailing ones
        // after a point.
        std::size_t significant(std::string_view text)
        {
            const std::size_t exponent = text.find_first_of("eE");
            std::string_view mantissa = text.substr(0, exponent);
            if (!mantissa.empty() && (mantissa.front() == '-' || mantissa.front() == '+'))
                mantissa.remove_prefix(1);
            std::string digits;
            for (char c : mantissa)
            {
                if (isDigit(c))
                    digits += c;
            }
            const std::size_t first = digits.find_first_not_of('0');
            if (first == std::string::npos)
                return 1;
            digits.erase(0, first);
            if (mantissa.find('.') != std::string_view::npos)
            {
                const std::size_t last = digits.find_last_not_of('0');
                digits.erase(last + 1);
            }
            return digits.size();
        }

        bool caseless(std::string_view text, std::string_view word)
        {
            return text.size() == word.size() && std::equal(text.begin(), text.end(), word.begin(), [](char a, char b) {
                       return (a >= 'A' && a <= 'Z' ? static_cast<char>(a - 'A' + 'a') : a) == b;
                   });
        }

        // A string literal of `text`, C's escapes for a quote, a backslash and the control bytes.
        std::string literal(std::string_view text)
        {
            std::string out = "\"";
            for (const char c : text)
            {
                const auto byte = static_cast<unsigned char>(c);
                if (c == '"' || c == '\\')
                    out += std::string("\\") + c;
                else if (byte < 0x20 || byte == 0x7f)
                    out += std::format("\\x{:02x}", byte);
                else
                    out += c;
            }
            return out + "\"";
        }

        // A name for a field or a type from `text`: its letters, digits and underscores, every other byte an
        // underscore, an underscore before a first digit, one after a keyword.
        std::string identifierOf(std::string_view text)
        {
            std::string name;
            for (const char c : text)
                name += isLetter(c) || isDigit(c) ? c : '_';
            if (name.empty() || isDigit(name.front()))
                name.insert(name.begin(), '_');
            if (circuit::isFMinusKeyword(name))
                name += '_';
            return name;
        }

        // The module's name: the file's own, without its folder and extension, its words capitalised, then "Row".
        std::string moduleName(std::string_view fileName)
        {
            std::string_view stem = fileName;
            if (const std::size_t slash = stem.find_last_of("/\\"); slash != std::string_view::npos)
                stem.remove_prefix(slash + 1);
            if (const std::size_t dot = stem.find_last_of('.'); dot != std::string_view::npos && dot > 0)
                stem = stem.substr(0, dot);
            std::string name;
            bool upper = true;
            for (const char c : stem)
            {
                if (!isLetter(c) && !isDigit(c))
                {
                    upper = true;
                    continue;
                }
                name += upper && c >= 'a' && c <= 'z' ? static_cast<char>(c - 'a' + 'A') : c;
                upper = c == '_';
            }
            std::erase(name, '_');
            if (name.empty() || isDigit(name.front()))
                name.insert(0, "Table");
            return name + "Row";
        }

        struct Column
        {
            std::string name;        // the header's
            std::string field;       // its field's
            std::string type;        // its declaration's type
            std::string attributes;  // [[csv::name(...)]], [[csv::category]]
            std::string comment;
            std::vector<std::string> constants;   // an enumeration's, when it is one
        };
    } // namespace

    bool csvSchema(std::string_view text, std::string_view fileName, char delimiter, std::string& out,
                   std::string& error)
    {
        if (text.size() > static_cast<std::size_t>(std::numeric_limits<std::int32_t>::max()))
        {
            error = std::string(fileName) + ": past 2 GB, more than a table holds";
            return false;
        }
        void* doc = fminus_rt_csv_parse(text.data(), static_cast<std::int32_t>(text.size()), delimiter);
        if (doc == nullptr)
        {
            error = std::string(fileName) + ": no CSV table: no header, a quote never closed, a byte after a closing "
                                            "quote, or a row of more fields than the header";
            return false;
        }
        const std::int32_t rows = fminus_rt_csv_rows(doc);
        const std::int32_t columns = fminus_rt_csv_columns(doc);
        std::vector<Column> fields;
        std::set<std::string> taken;      // the fields' names
        std::set<std::string> headers;    // the header's names, each csv_read's for its first column
        std::set<std::string> constants;  // every enumeration's constants, which share the file's scope
        std::vector<double> values(static_cast<std::size_t>(rows));
        for (std::int32_t j = 0; j < columns; ++j)
        {
            Column c;
            std::int32_t length = 0;
            const char* bytes = fminus_rt_csv_name(doc, j, &length);
            c.name.assign(bytes == nullptr ? "" : bytes, static_cast<std::size_t>(length));
            c.field = identifierOf(c.name);
            for (int k = 2; !taken.insert(c.field).second; ++k)
                c.field = identifierOf(c.name) + "_" + std::to_string(k);
            // Its distinct texts, in the order the rows first have them.
            std::vector<std::string> labels;
            const std::int32_t levels = fminus_rt_csv_levels(doc, j, nullptr, 0);
            for (std::int32_t code = 0; code < levels; ++code)
            {
                std::int32_t size = 0;
                const char* label = fminus_rt_csv_label(doc, j, nullptr, 0, code, &size);
                labels.emplace_back(label == nullptr ? "" : label, static_cast<std::size_t>(size));
            }
            bool integers = !labels.empty();
            bool numbers = !labels.empty();
            bool wide = false;   // a value past a float's digits or range
            double least = std::numeric_limits<double>::infinity();
            double greatest = -least;
            for (const std::string& label : labels)
            {
                const std::string_view t = trimmed(label);
                double value = 0.0;
                std::int32_t whole = 0;
                const auto n = static_cast<std::int32_t>(t.size());
                if (n == 0 || fminus::runtime::readDouble(t.data(), n, NumberSyntax::Xml, value) != n)
                {
                    integers = numbers = false;
                    break;
                }
                integers = integers && integerText(t)
                           && fminus::runtime::readInt(t.data(), n, NumberSyntax::Xml, whole);
                if (value == value)
                {
                    least = std::min(least, value);
                    greatest = std::max(greatest, value);
                    wide = wide || significant(t) > 7 || std::fabs(value) > 3.4028234663852886e38
                           || (value != 0.0 && std::fabs(value) < 1.1754943508222875e-38);
                }
            }
            const std::int32_t read = fminus_rt_csv_doubles(doc, j, nullptr, 0, values.data(), rows);
            const auto empty = static_cast<std::int32_t>(
                std::count_if(values.begin(), values.begin() + std::max(0, std::min(read, rows)),
                              [](double v) { return !(v == v); }));
            // Each end the shortest digits that read back as it: an int's exactly.
            const std::string range = least <= greatest ? std::format("{} to {}", least, greatest) : "";
            if (labels.empty())
            {
                c.type = "float";
                c.comment = "empty in every row";
            }
            else if (integers)
            {
                c.type = "int";
                c.comment = range;
            }
            else if (numbers)
            {
                c.type = wide ? "double" : "float";
                c.comment = range.empty() ? "no number but NaN" : range;
            }
            else if (std::all_of(labels.begin(), labels.end(), [](const std::string& label) {
                         return caseless(trimmed(label), "true") || caseless(trimmed(label), "false");
                     }))
            {
                c.type = "bool";
                c.comment = "true or false";
            }
            else
            {
                const bool words =
                    labels.size() <= kMostEnumerated
                    && std::all_of(labels.begin(), labels.end(), [&](const std::string& label) {
                           return isIdentifier(label) && !circuit::isFMinusKeyword(label) && !constants.contains(label);
                       });
                if (words)
                {
                    c.constants = labels;
                    constants.insert(labels.begin(), labels.end());
                    c.type = "enum " + c.field + "_value";
                    c.comment = std::format("{} values", labels.size());
                }
                else
                {
                    c.type = "int";
                    c.attributes = " [[csv::category]]";
                    c.comment = std::format("{} distinct text{}: a code each, csv_label(doc, {}, code) the text",
                                            labels.size(), labels.size() == 1 ? "" : "s", literal(c.name));
                }
            }
            if (empty > 0 && !labels.empty() && (numbers || integers))
                c.comment += std::format(", {} empty", empty);
            if (c.field != c.name)
                c.attributes = " [[csv::name(" + literal(c.name) + ")]]" + c.attributes;
            if (!headers.insert(c.name).second)   // a second column of the name: csv_read reads the first
            {
                c.attributes += " [[csv::skip]]";
                c.comment = std::format("a second column {}, which csv_read cannot tell from the first: read it by its "
                                        "index, {}",
                                        literal(c.name), j);
            }
            fields.push_back(std::move(c));
        }
        fminus_rt_csv_free(doc);

        std::string_view base = fileName;
        if (const std::size_t slash = base.find_last_of("/\\"); slash != std::string_view::npos)
            base.remove_prefix(slash + 1);
        out = std::format("// {}: {} row{}, {} column{}. Written by fminus --emit fm: check it, and keep it in the "
                          "program.\n",
                          base, rows, rows == 1 ? "" : "s", columns, columns == 1 ? "" : "s");
        for (const Column& c : fields)
        {
            if (c.constants.empty())
                continue;
            out += "enum " + c.field + "_value {";
            for (std::size_t k = 0; k < c.constants.size(); ++k)
                out += (k == 0 ? " " : ", ") + c.constants[k];
            out += " };\n";
        }
        out += "\nmodule " + moduleName(fileName) + "\n{\n";
        for (const Column& c : fields)
        {
            std::string line = "    " + c.type + " " + c.field + c.attributes + ";";
            if (!c.comment.empty())
                line += std::string(line.size() < 40 ? 40 - line.size() : 1, ' ') + "// " + c.comment;
            out += line + "\n";
        }
        out += "};\n";
        return true;
    }
} // namespace fminus
