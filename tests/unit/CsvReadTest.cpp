// tests/unit/CsvReadTest.cpp: csv_read(doc, rows, cap), an array of a module filled from a CSV document, a row each,
// by its type's table of columns (ir/Csv.cpp, runtime/Csv.cpp), and the rest of the csv_* builtins through the JIT:
// each kind of field, the names csv::name gives and the ones a nested module's field, an array's element and a
// vector's lane get, a default, a range, a category, csv::skip, a parameter and a string left alone, a cell that is
// empty, no value of its kind or past the row's end, the count of complete rows, CSV_MISSING, a cap, a null document,
// many rows read on the pool the same as one by one, a delimiter; and the errors, each at its place.
#include "Compile.h"

#include <catch2/catch_test_macros.hpp>

#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <string>
#include <string_view>

using namespace fminus::test;

namespace
{
    constexpr const char* kProgram = R"(
enum Wave { SAW, SQUARE, NOISE };
enum Flag : bool { OFF, ON };
module Inner { float u; int v; };
module Row
{
    float f;
    double d;
    int i [[fminus::range(0, 100)]];
    bool b;
    enum Wave w;
    enum Flag on;
    char tag[4];
    float g [[fminus::default(7)]];
    float x [[csv::name("x0", "x1")]] [2];
    int y[2][2];
    Inner inner;
    float4 v [[csv::name("vx")]];
    int c [[csv::category]];
    float skipped [[csv::skip]];
    string s;
    int last [[csv::name("the last one")]] [[fminus::default(-1)]];
};

int rowSize() { return sizeof(Row); }
int read(const void* data, int bytes, Row* rows, int cap)
{
    csv_doc doc = csv_parse(data, bytes);
    defer csv_free(doc);
    return csv_read(doc, rows, cap);
}
float f(const Row* r, int k) { return r[k].f; }
double d(const Row* r, int k) { return r[k].d; }
int i(const Row* r, int k) { return r[k].i; }
bool b(const Row* r, int k) { return r[k].b; }
int w(const Row* r, int k) { return r[k].w; }
bool on(const Row* r, int k) { return r[k].on; }
int tag(const Row* r, int k, int j) { return r[k].tag[j]; }
float g(const Row* r, int k) { return r[k].g; }
float x(const Row* r, int k, int j) { return r[k].x[j]; }
int y(const Row* r, int k, int j, int l) { return r[k].y[j][l]; }
float u(const Row* r, int k) { return r[k].inner.u; }
int v(const Row* r, int k) { return r[k].inner.v; }
float lane(const Row* r, int k, int j) { float4 l = r[k].v; return j == 0 ? l.x : j == 1 ? l.y : j == 2 ? l.z : l.w; }
int c(const Row* r, int k) { return r[k].c; }
float skipped(const Row* r, int k) { return r[k].skipped; }
int last(const Row* r, int k) { return r[k].last; }
void mark(Row* r, int k) { r[k].f = 99.0f; r[k].skipped = 5.0f; r[k].i = 77; }
)";

    // Rows of the program's module, zeroed, as malloc's are: aligned to 64.
    struct Rows
    {
        std::size_t size = 0;
        void* memory = nullptr;
        Rows(std::size_t count, std::size_t rowSize)
            : size(count * rowSize)
            , memory(std::aligned_alloc(64, (size + 63) / 64 * 64))
        {
            std::memset(memory, 0, size);
        }
        ~Rows()
        {
            std::free(memory);
        }
        Rows(const Rows&) = delete;
        Rows& operator=(const Rows&) = delete;
    };
} // namespace

TEST_CASE("csv_read: each kind of field, each name, and the complete rows", "[csv]")
{
    auto jit = compile(kProgram);
    REQUIRE(jit != nullptr);
    const auto size = static_cast<std::size_t>(lookup<int()>(*jit, "rowSize")());
    const auto read = lookup<int(const void*, int, void*, int)>(*jit, "read");
    const auto f = lookup<float(const void*, int)>(*jit, "f");
    const auto d = lookup<double(const void*, int)>(*jit, "d");
    const auto i = lookup<int(const void*, int)>(*jit, "i");
    const auto b = lookup<bool(const void*, int)>(*jit, "b");
    const auto w = lookup<int(const void*, int)>(*jit, "w");
    const auto on = lookup<bool(const void*, int)>(*jit, "on");
    const auto tag = lookup<int(const void*, int, int)>(*jit, "tag");
    const auto g = lookup<float(const void*, int)>(*jit, "g");
    const auto x = lookup<float(const void*, int, int)>(*jit, "x");
    const auto y = lookup<int(const void*, int, int, int)>(*jit, "y");
    const auto u = lookup<float(const void*, int)>(*jit, "u");
    const auto v = lookup<int(const void*, int)>(*jit, "v");
    const auto lane = lookup<float(const void*, int, int)>(*jit, "lane");
    const auto c = lookup<int(const void*, int)>(*jit, "c");
    const auto skipped = lookup<float(const void*, int)>(*jit, "skipped");
    const auto last = lookup<int(const void*, int)>(*jit, "last");
    const auto mark = lookup<void(void*, int)>(*jit, "mark");

    const std::string text =
        "f,d,i,b,w,on,tag,x0,x1,y[0][0],y[0][1],y[1][0],y[1][1],inner.u,inner.v,vx[0],vx[1],vx[2],vx[3],c,skipped,"
        "the last one,extra\n"
        // every field filled; on by its constant's name; the last by its renamed column
        "1.5,0.1,42,true,SQUARE,ON,ab,1,2,3,4,5,6,0.5,7,8,9,10,11,red,99,3,zzz\n"
        // f no number, i past its range, w by its number, on by 1, tag that does not fit, x0 empty, c and last empty
        "x,\"2\",101,0,2,1,abcd,,2,3,4,5,6,0.5,7,8,9,10,11,,99,,zzz\n"
        // a short row: only f
        "3\n"
        // an enumeration's name that no constant has, a bool that is no bool
        " 4 ,1e3,7,yes,TRIANGLE,MAYBE,\"q\"\"\",1,2,3,4,5,6,0.5,7,8,9,10,11,blue,99,4,zzz\n";
    Rows rows(5, size);
    mark(rows.memory, 1);
    mark(rows.memory, 4);   // past the rows the text has: left alone
    CHECK(read(text.data(), static_cast<int>(text.size()), rows.memory, 5) == 1);
    const void* r = rows.memory;

    // Row 0: everything.
    CHECK(f(r, 0) == 1.5f);
    CHECK(d(r, 0) == 0.1);
    CHECK(i(r, 0) == 42);
    CHECK(b(r, 0));
    CHECK(w(r, 0) == 1);
    CHECK(on(r, 0));
    CHECK((tag(r, 0, 0) == 'a' && tag(r, 0, 1) == 'b' && tag(r, 0, 2) == 0 && tag(r, 0, 3) == 0));
    CHECK(g(r, 0) == 7.0f);   // no column: its default
    CHECK((x(r, 0, 0) == 1.0f && x(r, 0, 1) == 2.0f));
    CHECK((y(r, 0, 0, 0) == 3 && y(r, 0, 0, 1) == 4 && y(r, 0, 1, 0) == 5 && y(r, 0, 1, 1) == 6));
    CHECK((u(r, 0) == 0.5f && v(r, 0) == 7));
    CHECK((lane(r, 0, 0) == 8.0f && lane(r, 0, 3) == 11.0f));
    CHECK(c(r, 0) == 0);   // red, the first category
    CHECK(skipped(r, 0) == 0.0f);   // csv::skip: never read
    CHECK(last(r, 0) == 3);

    // Row 1: what could not be read.
    CHECK(std::isnan(f(r, 1)));   // no number, no default: NaN
    CHECK(d(r, 1) == 2.0);        // a quoted number
    CHECK(i(r, 1) == 77);         // past its range, no default: as it was
    CHECK_FALSE(b(r, 1));
    CHECK(w(r, 1) == 2);          // an enumeration by its number
    CHECK(on(r, 1));
    CHECK(tag(r, 1, 0) == 0);     // "abcd" leaves no room for a 0: not read
    CHECK(std::isnan(x(r, 1, 0)));
    CHECK(c(r, 1) == 0);          // an empty category: as it was (0, as zeroed)
    CHECK(skipped(r, 1) == 5.0f);
    CHECK(last(r, 1) == -1);      // empty: its default

    // Row 2: a short row, its missing cells empty.
    CHECK(f(r, 2) == 3.0f);
    CHECK(std::isnan(d(r, 2)));
    CHECK(g(r, 2) == 7.0f);
    CHECK(last(r, 2) == -1);

    // Row 3: spaces around a number; a name no constant has; a bool that is none; "" in a char array.
    CHECK(f(r, 3) == 4.0f);
    CHECK(d(r, 3) == 1000.0);
    CHECK(w(r, 3) == 0);          // not read: as it was
    CHECK_FALSE(on(r, 3));
    CHECK((tag(r, 3, 0) == 'q' && tag(r, 3, 1) == '"' && tag(r, 3, 2) == 0));
    CHECK(c(r, 3) == 1);          // blue, the second category in row order (row 1's is empty)

    // Row 4: no such row; untouched.
    CHECK(f(r, 4) == 99.0f);
}

TEST_CASE("csv_read: a missing column, a cap, a null document", "[csv]")
{
    auto jit = compile(R"(
module A { float a; float b; };
module B { float a; float z [[fminus::default(2)]]; };
int readA(const void* data, int bytes, A* rows, int cap)
{
    csv_doc doc = csv_parse(data, bytes);
    defer csv_free(doc);
    return csv_read(doc, rows, cap);
}
int readB(const void* data, int bytes, B* rows, int cap)
{
    csv_doc doc = csv_parse(data, bytes);
    defer csv_free(doc);
    return csv_read(doc, rows, cap);
}
)");
    REQUIRE(jit != nullptr);
    const auto readA = lookup<int(const void*, int, void*, int)>(*jit, "readA");
    const auto readB = lookup<int(const void*, int, void*, int)>(*jit, "readB");
    const std::string text = "a,c\n1,2\n3,4\n5,6\n";
    float rows[6] = {-1, -1, -1, -1, -1, -1};
    CHECK(readA(text.data(), static_cast<int>(text.size()), rows, 3) == -1);   // CSV_MISSING: no column b
    CHECK(rows[0] == -1.0f);                                                   // and nothing written
    CHECK(readB(text.data(), static_cast<int>(text.size()), rows, 2) == 2);    // z's default; a cap of 2
    CHECK((rows[0] == 1.0f && rows[1] == 2.0f && rows[2] == 3.0f && rows[3] == 2.0f));
    CHECK((rows[4] == -1.0f && rows[5] == -1.0f));                              // the third row: past the cap
    const std::string notCsv = "a\n\"open\n";
    CHECK(readB(notCsv.data(), static_cast<int>(notCsv.size()), rows, 2) == 0);   // a null document: no rows
    CHECK(readB(text.data(), static_cast<int>(text.size()), nullptr, 0) == 0);
}

TEST_CASE("csv_read: many rows, read on the pool, are the rows read one by one", "[csv]")
{
    auto jit = compile(R"(
module R { int id; double a; int k [[csv::category]]; char name[8]; };
int read(const void* data, int bytes, R* rows, int cap)
{
    csv_doc doc = csv_parse(data, bytes);
    defer csv_free(doc);
    return csv_read(doc, rows, cap);
}
int sizeR() { return sizeof(R); }
int id(const R* r, int i) { return r[i].id; }
double a(const R* r, int i) { return r[i].a; }
int k(const R* r, int i) { return r[i].k; }
int first(const R* r, int i) { return r[i].name[0]; }
)");
    REQUIRE(jit != nullptr);
    const auto read = lookup<int(const void*, int, void*, int)>(*jit, "read");
    const auto id = lookup<int(const void*, int)>(*jit, "id");
    const auto a = lookup<double(const void*, int)>(*jit, "a");
    const auto k = lookup<int(const void*, int)>(*jit, "k");
    const auto first = lookup<int(const void*, int)>(*jit, "first");
    const auto size = static_cast<std::size_t>(lookup<int()>(*jit, "sizeR")());
    constexpr int kRows = 25000;   // 25 blocks of 1024 rows
    std::string text = "id,a,k,name\r\n";
    const char* words[] = {"pear", "fig", "plum"};
    for (int i = 0; i < kRows; ++i)
        text += std::to_string(i) + "," + std::to_string(i * 0.25) + ",\"" + words[(i * 7) % 3] + "\"," +
                words[i % 3] + "\r\n";
    Rows rows(kRows, size);
    CHECK(read(text.data(), static_cast<int>(text.size()), rows.memory, kRows) == kRows);
    int wrong = 0;
    for (int i = 0; i < kRows; ++i)
    {
        // pear (row 0), fig (row 1: 7 % 3 is 1), plum (row 2: 14 % 3 is 2): codes in the order rows first have them
        const int expected = (i * 7) % 3;
        if (id(rows.memory, i) != i || a(rows.memory, i) != i * 0.25 || k(rows.memory, i) != expected
            || first(rows.memory, i) != words[i % 3][0])
            ++wrong;
    }
    CHECK(wrong == 0);
}

TEST_CASE("csv: column reads from a program, by index and by name, and a delimiter", "[csv]")
{
    auto jit = compile(R"(
int sum(const void* data, int bytes)
{
    csv_doc doc = csv_parse(data, bytes, "\t");
    defer csv_free(doc);
    float a[4];
    int b[4];
    const int n = csv_floats(doc, 0, a, 4);
    csv_ints(doc, "b", b, 4, -100);
    int total = 0;
    for (int i = 0; i < n; ++i)
        total += (int)a[i] + b[i];
    return total + csv_find(doc, "b") * 1000 + (csv_name(doc, 1) == "b" ? 10000 : 0);
}
int missing(const void* data, int bytes)
{
    csv_doc doc = csv_parse(data, bytes, "\t");
    defer csv_free(doc);
    float a[1];
    return csv_floats(doc, "nope", a, 1) == CSV_MISSING ? 1 : 0;
}
)");
    REQUIRE(jit != nullptr);
    const std::string text = "a\tb\n1\t10\n2\tx\n3\t30\n";
    const auto sum = lookup<int(const void*, int)>(*jit, "sum");
    CHECK(sum(text.data(), static_cast<int>(text.size())) == 1 + 2 + 3 + 10 - 100 + 30 + 1000 + 10000);
    CHECK(lookup<int(const void*, int)>(*jit, "missing")(text.data(), static_cast<int>(text.size())) == 1);
}

TEST_CASE("csv: the errors", "[csv]")
{
    const auto has = [](const std::string& errors, std::string_view what) {
        INFO(errors);
        CHECK(errors.find(what) != std::string::npos);
    };
    has(errorsOf("int f(csv_doc d, float* p) { return csv_read(d, p, 1); }"),
        "'csv_read' fills an array of a module, a row each: its rows must be a pointer to one, not 'float *'");
    has(errorsOf("module M { string s; };\nint f(csv_doc d, M* m) { return csv_read(d, m, 1); }"),
        "'csv_read' cannot fill 'M': it has no field csv_read reads");
    has(errorsOf("module M { float a; float b [[csv::name(\"a\")]]; };\n"
                 "int f(csv_doc d, M* m) { return csv_read(d, m, 1); }"),
        "duplicate CSV column 'a' in a module that csv_read fills");
    has(errorsOf("module M { float a [[csv::name(\"p\", \"q\")]] [3]; };"),
        "'csv::name' of 'a' takes one name, or one for each of its 3 elements, not 2");
    has(errorsOf("module M { float a [[csv::name(\"p\", \"q\")]]; };"), "'csv::name' of 'a' takes one name");
    has(errorsOf("module M { float a [[csv::name(\"\")]]; };"), "'csv::name' takes names, each a string");
    has(errorsOf("module M { float a [[csv::category]]; };"), "'csv::category' applies to a field of ints");
    has(errorsOf("enum E { P, Q };\nmodule M { enum E e [[csv::category]]; };"),
        "an enumeration reads its constants' names without it");
    has(errorsOf("module M { float a [[csv::skip(1)]]; };"), "'csv::skip' attribute takes no arguments");
    has(errorsOf("module M { float a [[csv::sep(\";\")]]; };"),
        "unknown attribute 'csv::sep': a field takes csv::name(\"name\"), csv::skip and csv::category");
    has(errorsOf("csv_doc f(const void* p, int n) { return csv_parse(p, n, \";;\"); }"),
        "a delimiter is one byte, not a quote, a line end or NUL");
    has(errorsOf("csv_doc f(const void* p, int n, string s) { return csv_parse(p, n, s); }"),
        "argument to 'csv_parse' must be a string literal");
    has(errorsOf("int f(csv_doc d, float* p) { return csv_floats(d, 1.5f, p, 1); }"),
        "a column is an int, its index from 0, or a string, its name, not 'float'");
    has(errorsOf("module M { float a; };\nint f(csv_doc d, M* m) [[clang::nonblocking]] { return csv_read(d, m, 1); }"),
        "function with 'nonblocking' attribute must not call non-'nonblocking' function 'csv_read'");
    has(errorsOf("int f(json_doc d) { return csv_rows(d); }"), "csv_doc");
    // A nonblocking function may read a column: wait-free.
    CHECK(compile("int f(csv_doc d, float* p) [[clang::nonblocking]] { return csv_floats(d, \"a\", p, 4); }")
          != nullptr);
}
