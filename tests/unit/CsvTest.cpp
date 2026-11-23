// tests/unit/CsvTest.cpp: the runtime's CSV reader (runtime/Csv.cpp), through its C functions: what is a document and
// what is not (quotes, line ends, a byte order mark, empty lines, short rows and long ones), the header's names, a
// column's numbers, ints and texts as categories, by index and by name, and what a null document reads as. csv_read,
// which needs a module's table, is tested through the JIT (CsvReadTest.cpp).
#include "runtime/Runtime.h"

#include <catch2/catch_test_macros.hpp>

#include <cmath>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace
{
    // A document of `text` and its delimiter, freed at the end of the test.
    struct Doc
    {
        std::string text;
        void* doc = nullptr;

        explicit Doc(std::string source, char delimiter = ',')
            : text(std::move(source))
            , doc(fminus_rt_csv_parse(text.data(), static_cast<std::int32_t>(text.size()), delimiter))
        {
        }
        ~Doc()
        {
            fminus_rt_csv_free(doc);
        }
        Doc(const Doc&) = delete;
        Doc& operator=(const Doc&) = delete;

        std::string name(std::int32_t column) const
        {
            std::int32_t length = -1;
            const char* bytes = fminus_rt_csv_name(doc, column, &length);
            return bytes == nullptr ? std::string() : std::string(bytes, static_cast<std::size_t>(length));
        }

        std::vector<float> floats(std::int32_t column) const
        {
            std::vector<float> out(64, -7.0f);
            const std::int32_t n =
                fminus_rt_csv_floats(doc, column, nullptr, 0, out.data(), static_cast<std::int32_t>(out.size()));
            out.resize(n < 0 ? 0 : static_cast<std::size_t>(n));
            return out;
        }

        std::vector<std::int32_t> ints(std::string_view column, std::int32_t fallback) const
        {
            std::vector<std::int32_t> out(64, -7);
            const std::int32_t n =
                fminus_rt_csv_ints(doc, -1, column.data(), static_cast<std::int32_t>(column.size()), out.data(),
                                   static_cast<std::int32_t>(out.size()), fallback);
            out.resize(n < 0 ? 0 : static_cast<std::size_t>(n));
            return out;
        }

        std::string label(std::int32_t column, std::int32_t code) const
        {
            std::int32_t size = -1;
            const char* bytes = fminus_rt_csv_label(doc, column, nullptr, 0, code, &size);
            return bytes == nullptr ? std::string("<none>") : std::string(bytes, static_cast<std::size_t>(size));
        }
    };

    bool isNan(float x)
    {
        return std::isnan(x);
    }
} // namespace

TEST_CASE("csv: a header, rows, and the numbers of a column", "[csv]")
{
    const Doc d("x,y,name\n1,2.5,a\n-3,1e3,b\n+4,inf,c\n");
    REQUIRE(d.doc != nullptr);
    CHECK(fminus_rt_csv_rows(d.doc) == 3);
    CHECK(fminus_rt_csv_columns(d.doc) == 3);
    CHECK(d.name(0) == "x");
    CHECK(d.name(2) == "name");
    CHECK(d.name(3).empty());
    CHECK(d.name(-1).empty());
    CHECK(d.floats(0) == std::vector<float>{1.0f, -3.0f, 4.0f});
    const std::vector<float> y = d.floats(1);
    REQUIRE(y.size() == 3);
    CHECK(y[0] == 2.5f);
    CHECK(y[1] == 1000.0f);
    CHECK(std::isinf(y[2]));
    const std::vector<float> names = d.floats(2);
    REQUIRE(names.size() == 3);
    CHECK((isNan(names[0]) && isNan(names[1]) && isNan(names[2])));   // no number: NaN
    CHECK(d.ints("x", 0) == std::vector<std::int32_t>{1, -3, 4});
    CHECK(d.ints("y", 99) == std::vector<std::int32_t>{99, 1000, 99});   // 2.5 is no integer; 1e3 is one; inf none
    CHECK(fminus_rt_csv_find(d.doc, "y", 1) == 1);
    CHECK(fminus_rt_csv_find(d.doc, "z", 1) == -1);
    std::vector<float> out(1);
    CHECK(fminus_rt_csv_floats(d.doc, -1, "nope", 4, out.data(), 1) == -1);   // CSV_MISSING
    CHECK(fminus_rt_csv_floats(d.doc, 7, nullptr, 0, out.data(), 1) == -1);
}

TEST_CASE("csv: the cap, a null out, and doubles correctly rounded", "[csv]")
{
    const Doc d("v\n0.1\n0.2\n0.3\n");
    REQUIRE(d.doc != nullptr);
    double out[2] = {-1.0, -1.0};
    CHECK(fminus_rt_csv_doubles(d.doc, 0, nullptr, 0, out, 2) == 3);   // the rows there are, the first 2 written
    CHECK(out[0] == 0.1);
    CHECK(out[1] == 0.2);
    CHECK(fminus_rt_csv_doubles(d.doc, 0, nullptr, 0, nullptr, 5) == 3);   // nothing written
    float f = 0.0f;
    CHECK(fminus_rt_csv_floats(d.doc, 0, nullptr, 0, &f, 1) == 3);
    CHECK(f == 0.1f);   // the float nearest the digits, not a double narrowed
}

TEST_CASE("csv: quotes, the delimiter and line ends inside them, and \"\"", "[csv]")
{
    const Doc d("\"a, b\",\"say \"\"hi\"\"\",n\r\n\"1,5\",\"two\nlines\",\" 7 \"\r\n\"\",x,\"8\"\r\n");
    REQUIRE(d.doc != nullptr);
    CHECK(fminus_rt_csv_columns(d.doc) == 3);
    CHECK(d.name(0) == "a, b");
    CHECK(d.name(1) == "say \"hi\"");
    CHECK(fminus_rt_csv_rows(d.doc) == 2);
    CHECK(d.ints("n", -1) == std::vector<std::int32_t>{7, 8});   // a quoted number, spaces around it
    const std::vector<float> first = d.floats(0);
    REQUIRE(first.size() == 2);
    CHECK(isNan(first[0]));   // "1,5" is no number: a decimal comma is not read
    CHECK(isNan(first[1]));   // an empty cell is none
}

TEST_CASE("csv: a byte order mark, empty lines, a last row without a line end, and short rows", "[csv]")
{
    const Doc d("\xEF\xBB\xBF" "a,b,c\n\n1,2,3\n\r\n4\n5,6");
    REQUIRE(d.doc != nullptr);
    CHECK(d.name(0) == "a");
    CHECK(fminus_rt_csv_rows(d.doc) == 3);
    CHECK(d.floats(0) == std::vector<float>{1.0f, 4.0f, 5.0f});
    const std::vector<float> c = d.floats(2);
    REQUIRE(c.size() == 3);
    CHECK(c[0] == 3.0f);
    CHECK(isNan(c[1]));   // a short row's missing cells are empty
    CHECK(isNan(c[2]));
    const Doc cr("a\n1\r");   // a lone CR that ends the text ends its record
    REQUIRE(cr.doc != nullptr);
    CHECK(cr.floats(0) == std::vector<float>{1.0f});
}

TEST_CASE("csv: what is no document", "[csv]")
{
    CHECK(Doc("").doc == nullptr);                  // no header
    CHECK(Doc("\nx\n1").doc == nullptr);            // an empty first line: no header
    CHECK(Doc("a,b\n1,2,3\n").doc == nullptr);      // more fields than the header
    CHECK(Doc("a\n\"open\n").doc == nullptr);       // a quote never closed
    CHECK(Doc("a\n\"x\"y\n").doc == nullptr);       // a byte after a closing quote
    CHECK(Doc("a\n1\n", '"').doc == nullptr);       // a quote is no delimiter
    CHECK(Doc("a\n1\n", '\n').doc == nullptr);
    CHECK(fminus_rt_csv_parse(nullptr, 3, ',') == nullptr);
    const Doc header("a,b\n");   // a header and no rows is a table of none
    REQUIRE(header.doc != nullptr);
    CHECK(fminus_rt_csv_rows(header.doc) == 0);
    const Doc quotes("a\nx\"y\n");   // a quote inside a field that did not begin with one is a byte
    REQUIRE(quotes.doc != nullptr);
    CHECK(fminus_rt_csv_rows(quotes.doc) == 1);
}

TEST_CASE("csv: another delimiter", "[csv]")
{
    const Doc tabs("a\tb\n1,5\t2\n", '\t');
    REQUIRE(tabs.doc != nullptr);
    CHECK(tabs.name(1) == "b");
    CHECK(tabs.floats(1) == std::vector<float>{2.0f});
    const Doc semicolons("a;b\n1;2\n", ';');
    REQUIRE(semicolons.doc != nullptr);
    CHECK(semicolons.floats(0) == std::vector<float>{1.0f});
}

TEST_CASE("csv: a column's categories, in the order the rows first have them", "[csv]")
{
    const Doc d("city,n\nOslo,1\nRome,2\nOslo,3\n,4\n\"Rome\",5\n\"New \"\"York\"\"\",6\n");
    REQUIRE(d.doc != nullptr);
    CHECK(fminus_rt_csv_levels(d.doc, 0, nullptr, 0) == 3);   // the empty cell is none
    CHECK(d.label(0, 0) == "Oslo");
    CHECK(d.label(0, 1) == "Rome");
    CHECK(d.label(0, 2) == "New \"York\"");
    CHECK(d.label(0, 3) == "<none>");
    CHECK(d.label(0, -1) == "<none>");
    CHECK(fminus_rt_csv_levels(d.doc, -1, "city", 4) == 3);
    CHECK(fminus_rt_csv_levels(d.doc, -1, "nope", 4) == -1);
    CHECK(fminus_rt_csv_levels(d.doc, 1, nullptr, 0) == 6);   // numbers are texts too
}

TEST_CASE("csv: a null document is an empty table", "[csv]")
{
    CHECK(fminus_rt_csv_rows(nullptr) == 0);
    CHECK(fminus_rt_csv_columns(nullptr) == 0);
    std::int32_t length = 7;
    CHECK(fminus_rt_csv_name(nullptr, 0, &length) == nullptr);
    CHECK(length == 0);
    CHECK(fminus_rt_csv_find(nullptr, "a", 1) == -1);
    float out = 0.0f;
    CHECK(fminus_rt_csv_floats(nullptr, 0, nullptr, 0, &out, 1) == -1);
    CHECK(fminus_rt_csv_levels(nullptr, 0, nullptr, 0) == -1);
    CHECK(fminus_rt_csv_label(nullptr, 0, nullptr, 0, 0, &length) == nullptr);
    fminus_rt_csv_free(nullptr);
}
