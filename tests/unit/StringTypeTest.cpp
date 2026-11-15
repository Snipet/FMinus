// tests/unit/StringTypeTest.cpp: FMinus's type string (frontend/Ast.h, ir/Text.cpp), a read-only view of bytes, and
// char as an array's element and a pointer's pointee. Through the JIT: a literal as a value where a string is wanted,
// == and != by content, a switch on a string, each string_* builtin at its edges (empty, out of range, crossing ends),
// a string in a local, a parameter, a result, a field, an array, a constant of the top level and a parallel region, and
// char arrays read as 0 to 255. Then the errors, each once, at its column; a nonblocking function that calls every
// builtin, and the runtime's functions, which allocate nothing.
#include "Compile.h"
#include "HeapCalls.h"

#include <catch2/catch_test_macros.hpp>

#include <cstdint>
#include <string>

using namespace fminus::test;

TEST_CASE("a string literal is a string where one is wanted", "[codegen][string-type]")
{
    auto jit = compile("const string GREETING = \"hello\";\n"
                       "const string WAVES[3] = {\"saw\", \"square\"};\n"
                       "int size(string s) { return string_size(s); }\n"
                       "string pick(bool on) { return on ? \"on\" : \"off\"; }\n"
                       "string named() { return \"named\"; }\n"
                       "int test(int k)\n"
                       "{\n"
                       "    string a = \"abc\";            // an initialiser\n"
                       "    string b;                      // the empty string\n"
                       "    b = \"de\" \"f\";                // an assignment; adjacent literals are one\n"
                       "    int r = size(a) + size(b) * 10 + size(\"\") * 100 + size(\"a\\0b\") * 1000;\n"
                       "    r += string_size(pick(k > 0)) * 10000 + string_size(named()) * 100000;\n"
                       "    r += string_size(GREETING) * 1000000 + string_size(WAVES[k]) * 10000000;\n"
                       "    return r;\n"
                       "}\n"
                       "const int LENGTH = string_size(\"four\");\n"
                       "int length() { return LENGTH; }\n"
                       "module Named { string name; int set(string s) { name = s; return string_size(name); } };\n"
                       "int method() { Named n; return n.set(\"method\") * 10 + (n.name == \"method\" ? 1 : 0); }\n");
    REQUIRE(jit);
    auto* test = lookup<int(int)>(*jit, "test");
    CHECK(test(0) == 3 + 30 + 0 + 3000 + 30000 + 500000 + 5000000 + 30000000);
    CHECK(test(1) == 3 + 30 + 0 + 3000 + 20000 + 500000 + 5000000 + 60000000);
    CHECK(test(2) == 3 + 30 + 0 + 3000 + 20000 + 500000 + 5000000);   // the third is the empty string
    CHECK(lookup<int()>(*jit, "length")() == 4);
    CHECK(lookup<int()>(*jit, "method")() == 61);   // a method's argument too
}

TEST_CASE("== and != compare two strings by content: the same length and the same bytes", "[codegen][string-type]")
{
    auto jit = compile("int same(string a, string b) { return (a == b ? 1 : 0) + (a != b ? 2 : 0); }\n"
                       "int test()\n"
                       "{\n"
                       "    string empty;\n"
                       "    char kept[4] = \"abc\";\n"
                       "    string view = string_of(kept, 3);\n"
                       "    int r = same(\"abc\", \"abc\");                        // 1\n"
                       "    r += same(\"abc\", \"abd\") * 10;                      // 2\n"
                       "    r += same(\"abc\", \"ab\") * 100;                      // 2: lengths differ\n"
                       "    r += same(empty, \"\") * 1000;                         // 1: null and an empty literal\n"
                       "    r += same(\"a\\0b\", \"a\") * 10000;                   // 2: a NUL is a byte\n"
                       "    r += (view == \"abc\" && \"abc\" == view && view != \"ab\" ? 1 : 0) * 100000;\n"
                       "    r += (\"x\" == \"x\" ? 1 : 0) * 1000000;\n"
                       "    return r;\n"
                       "}\n");
    REQUIRE(jit);
    CHECK(lookup<int()>(*jit, "test")() == 1 + 20 + 200 + 1000 + 20000 + 100000 + 1000000);
}

TEST_CASE("a switch on a string takes the case whose bytes equal its value's", "[codegen][string-type]")
{
    auto jit = compile("int wave(string s)\n"
                       "{\n"
                       "    int r = 0;\n"
                       "    switch (s)\n"
                       "    {\n"
                       "    case \"saw\":\n"
                       "        return 1;\n"
                       "    case \"squ\" \"are\":                  // adjacent literals are one\n"
                       "        r = 2;\n"
                       "        break;\n"
                       "    case \"\":\n"
                       "        r = 10;                          // falls through\n"
                       "    case \"sine\":\n"
                       "        r += 3;\n"
                       "        break;\n"
                       "    default:\n"
                       "        r = -1;\n"
                       "    }\n"
                       "    return r;\n"
                       "}\n"
                       "int none(string s) { int r = 7; switch (s) { case \"a\": r = 1; } return r; }\n"
                       "int head(int k) { switch (string s = k > 0 ? \"b\" : \"a\"; s) { case \"a\": return 1; "
                       "case \"b\": return string_size(s) + 1; } return 0; }\n"
                       "int test()\n"
                       "{\n"
                       "    string empty;\n"
                       "    return wave(\"saw\") + wave(\"square\") * 10 + wave(\"sine\") * 100 + wave(empty) * 1000\n"
                       "           + (wave(\"sa\") == -1 ? 10000 : 0) + none(\"b\") * 100000 + none(\"a\") * 1000000\n"
                       "           + head(1) * 10000000 + head(0) * 100000000;\n"
                       "}\n");
    REQUIRE(jit);
    CHECK(lookup<int()>(*jit, "test")() == 1 + 20 + 300 + 13000 + 10000 + 700000 + 1000000 + 20000000 + 100000000);
}

TEST_CASE("the string_* builtins at their edges", "[codegen][string-type]")
{
    auto jit = compile(
        "int size(string s) { return string_size(s); }\n"
        "int byte(string s, int i) { return string_byte(s, i); }\n"
        "int sliced(int from, int to)\n"
        "{\n"
        "    string s = string_slice(\"abcdef\", from, to);\n"
        "    return string_size(s) * 1000 + (string_size(s) > 0 ? string_byte(s, 0) : 0);\n"
        "}\n"
        "int find(string s, string what) { return string_find(s, what); }\n"
        "int starts(string s, string with) { return string_starts(s, with) ? 1 : 0; }\n"
        "int ends(string s, string with) { return string_ends(s, with) ? 1 : 0; }\n"
        "int tests()\n"
        "{\n"
        "    string empty;\n"
        "    int r = 0;\n"
        "    r += size(empty) + size(\"\") + size(\"abc\") * 10;\n"
        "    r += (byte(\"abc\", -1) == -1 && byte(\"abc\", 3) == -1 && byte(empty, 0) == -1 ? 100 : 0);\n"
        "    r += (byte(\"abc\", 0) == 97 && byte(\"abc\", 2) == 99 && byte(\"\\xff\\x80\", 0) == 255\n"
        "          && byte(\"\\xff\\x80\", 1) == 128 ? 1000 : 0);\n"
        "    r += (find(\"abcabc\", \"ca\") == 2 && find(\"abc\", \"\") == 0 && find(empty, \"\") == 0\n"
        "          && find(\"abc\", \"abcd\") == -1 && find(\"abc\", \"x\") == -1 && find(\"abc\", \"c\") == 2\n"
        "          && find(empty, \"a\") == -1 ? 10000 : 0);\n"
        "    r += (starts(\"abc\", \"ab\") + starts(\"abc\", \"\") + starts(\"abc\", \"abc\") + starts(empty, \"\")\n"
        "          == 4 && starts(\"abc\", \"bc\") + starts(\"ab\", \"abc\") + starts(empty, \"a\") == 0\n"
        "          ? 100000 : 0);\n"
        "    r += (ends(\"abc\", \"bc\") + ends(\"abc\", \"\") + ends(\"abc\", \"abc\") + ends(empty, \"\") == 4\n"
        "          && ends(\"abc\", \"ab\") + ends(\"bc\", \"abc\") + ends(empty, \"a\") == 0 ? 1000000 : 0);\n"
        "    return r;\n"
        "}\n"
        "int data()\n"
        "{\n"
        "    string s = \"abcdef\";\n"
        "    string t = string_slice(s, 2, 4);\n"
        "    const void* p = string_data(s);\n"
        "    string again = string_of(p, 6);\n"
        "    string none = string_of(p, -1);\n"
        "    void* nothing;\n"
        "    string null = string_of(nothing, 4);\n"
        "    return (again == s ? 1 : 0) + (string_size(none) == 0 && string_size(null) == 0 ? 10 : 0)\n"
        "           + (string_data(none) == nothing && string_data(null) == nothing ? 100 : 0)\n"
        "           + (t == \"cd\" ? 1000 : 0) + (string_slice(t, -5, 1) == \"c\" ? 10000 : 0);\n"
        "}\n"
        "int copied(int cap)\n"
        "{\n"
        "    char out[8];\n"
        "    int n = string_copy(out, cap, \"hello\");\n"
        "    int sum = 0;\n"
        "    for (int i = 0; i < 8; ++i)\n"
        "        sum = sum * 2 + (out[i] != 0 ? 1 : 0);\n"
        "    return n * 1000 + sum;\n"
        "}\n");
    REQUIRE(jit);
    CHECK(lookup<int()>(*jit, "tests")() == 30 + 100 + 1000 + 10000 + 100000 + 1000000);
    CHECK(lookup<int()>(*jit, "data")() == 11111);
    auto* sliced = lookup<int(int, int)>(*jit, "sliced");
    CHECK(sliced(1, 3) == 2000 + 'b');
    CHECK(sliced(-4, 2) == 2000 + 'a');    // from clamped to 0
    CHECK(sliced(4, 100) == 2000 + 'e');   // to clamped to the end
    CHECK(sliced(3, 1) == 0);              // crossing ends: empty
    CHECK(sliced(9, 12) == 0);             // both past the end: empty
    CHECK(sliced(0, 6) == 6000 + 'a');
    auto* copied = lookup<int(int)>(*jit, "copied");
    CHECK(copied(0) == 5000);   // a cap of 0 asks the length
    CHECK(copied(-3) == 5000);
    CHECK(copied(3) == 5000 + 0b11100000);   // the first three bytes
    CHECK(copied(8) == 5000 + 0b11111000);   // all five, the rest left as they were (0)
    // A string read as a number by the rules a query's has: the whole string, no '+', finite, an int that fits.
    auto numbers = compile("float f(int k)\n"
                           "{\n"
                           "    switch (k)\n"
                           "    {\n"
                           "    case 0: return string_float(\"1.5e3\", -1.0f);\n"
                           "    case 1: return string_float(\"-0.25\", -1.0f);\n"
                           "    case 2: return string_float(\"+1\", -1.0f);\n"
                           "    case 3: return string_float(\" 1\", -1.0f);\n"
                           "    case 4: return string_float(\"1e40\", -1.0f);\n"
                           "    case 5: return string_float(\"nan\", -1.0f);\n"
                           "    case 6: return string_float(\"\", -1.0f);\n"
                           "    case 7: return string_float(\"2x\", -1.0f);\n"
                           "    case 8: return string_float(\"0.1\", -1.0f);\n"
                           "    case 9: return string_float(\"inf\", -1.0f);\n"
                           "    }\n"
                           "    return 0;\n"
                           "}\n"
                           "int i(int k)\n"
                           "{\n"
                           "    switch (k)\n"
                           "    {\n"
                           "    case 0: return string_int(\"42\", -7);\n"
                           "    case 1: return string_int(\"-2147483648\", -7);\n"
                           "    case 2: return string_int(\"2147483648\", -7);\n"
                           "    case 3: return string_int(\"+1\", -7);\n"
                           "    case 4: return string_int(\"1.0\", -7);\n"
                           "    case 5: return string_int(\"\", -7);\n"
                           "    case 6: return string_int(\"007\", -7);\n"
                           "    case 7: return string_int(\" 1\", -7);\n"
                           "    }\n"
                           "    return 0;\n"
                           "}\n");
    REQUIRE(numbers);
    auto* f = lookup<float(int)>(*numbers, "f");
    CHECK(f(0) == 1500.0f);
    CHECK(f(1) == -0.25f);
    CHECK(f(2) == -1.0f);
    CHECK(f(3) == -1.0f);
    CHECK(f(4) == -1.0f);   // past a float's range: no finite number
    CHECK(f(5) == -1.0f);
    CHECK(f(6) == -1.0f);
    CHECK(f(7) == -1.0f);
    CHECK(f(8) == 0.1f);   // correctly rounded, straight to a float
    CHECK(f(9) == -1.0f);
    auto* i = lookup<int(int)>(*numbers, "i");
    CHECK(i(0) == 42);
    CHECK(i(1) == -2147483647 - 1);
    CHECK(i(2) == -7);
    CHECK(i(3) == -7);
    CHECK(i(4) == -7);
    CHECK(i(5) == -7);
    CHECK(i(6) == 7);
    CHECK(i(7) == -7);
}

TEST_CASE("char is a byte in an array, read as an int from 0 to 255", "[codegen][string-type]")
{
    auto jit =
        compile("int bytes(int v)\n"
                "{\n"
                "    char a[4];\n"
                "    int stored = (a[0] = v);       // the low 8 bits, and the assignment's value is what a[0] holds\n"
                "    a[1] = a[0] + 1;\n"
                "    a[2]++;\n"
                "    a[3] -= 1;                     // 0 - 1 keeps 255\n"
                "    return stored * 1000000 + a[1] * 1000 + a[2] * 100 + (a[3] == 255 ? 1 : 0);\n"
                "}\n"
                "int fill(char* p, int n) { for (int i = 0; i < n; ++i) p[i] = 200 + i; return *p; }\n"
                "int sum(const char* p, int n) { int s = 0; for (int i = 0; i < n; ++i) s += p[i]; return s; }\n"
                "int test()\n"
                "{\n"
                "    char grid[2][3];\n"
                "    fill(grid[1], 3);\n"
                "    char word[] = \"ok\";               // C's: its bytes and a NUL\n"
                "    char padded[6] = \"abc\";           // the rest 0\n"
                "    char exact[3] = \"abc\";            // no room for C's NUL: none\n"
                "    const char table[3] = {1, 2, 300};\n"
                "    char* p = &padded[1];\n"
                "    void* v = p;\n"
                "    char* q = v;\n"
                "    *q = 66;\n"
                "    return sum(grid[1], 3) + (word[1] == 107 && word[2] == 0 ? 3000 : 0) + sizeof(char) * 10000 + "
                "padded[1] * 100000\n"
                "           + (padded[3] == 0 && padded[5] == 0 && exact[2] == 99 ? 100000000 : 0)\n"
                "           + table[2] * 0 + (table[2] == 44 ? 1000000000 : 0) + grid[0][2];\n"
                "}\n"
                "module Name\n"
                "{\n"
                "    char text[16];\n"
                "    int size;\n"
                "    void keep(string s) { size = string_copy(text, 16, s); }\n"
                "    string get() { return string_of(text, size < 16 ? size : 16); }\n"
                "};\n"
                "int kept()\n"
                "{\n"
                "    Name n;\n"
                "    char scratch[8] = \"xyz\";\n"
                "    n.keep(string_slice(string_of(scratch, 8), 0, 3));\n"
                "    scratch[0] = 0;   // the name is a copy\n"
                "    Name m = n;       // and copied with its module\n"
                "    return (m.get() == \"xyz\" ? 1 : 0) + (n.get() == \"xyz\" ? 10 : 0) + n.text[2] * 100;\n"
                "}\n");
    REQUIRE(jit);
    auto* bytes = lookup<int(int)>(*jit, "bytes");
    CHECK(bytes(300) == 44 * 1000000 + 45 * 1000 + 100 + 1);
    CHECK(bytes(-1) == 255 * 1000000 + 0 * 1000 + 100 + 1);   // 255 + 1 keeps 0
    CHECK(bytes(65) == 65 * 1000000 + 66 * 1000 + 100 + 1);
    CHECK(lookup<int()>(*jit, "test")() == 603 + 3000 + 10000 + 66 * 100000 + 100000000 + 1000000000);
    CHECK(lookup<int()>(*jit, "kept")() == 1 + 10 + 'z' * 100);
}

TEST_CASE("a string in memory: fields, arrays of one and two dimensions, a copy, a parallel region",
          "[codegen][string-type]")
{
    auto jit =
        compile("module Presets\n"
                "{\n"
                "    string current;\n"
                "    string names[3];\n"
                "    string grid[2][2];\n"
                "    int count;\n"
                "    void add(string s) { names[count] = s; grid[count % 2][count / 2] = s; count++; current = s; }\n"
                "    int has(string s) { for (int i = 0; i < count; ++i) if (names[i] == s) return i; return -1; }\n"
                "};\n"
                "int test()\n"
                "{\n"
                "    Presets p;\n"
                "    int empty = string_size(p.current) + string_size(p.names[2]);\n"
                "    p.add(\"warm\");\n"
                "    p.add(\"bright\");\n"
                "    Presets q = p;\n"
                "    q.add(\"dark\");\n"
                "    string all[2] = {\"x\", p.current};\n"
                "    return empty + p.has(\"bright\") * 10 + p.has(\"dark\") * 100 + q.has(\"dark\") * 1000\n"
                "           + (q.grid[1][0] == \"bright\" && q.grid[0][1] == \"dark\" ? 10000 : 0)\n"
                "           + (all[1] == \"bright\" ? 100000 : 0) + sizeof(Presets) * 0;\n"
                "}\n"
                "int lengths()\n"
                "{\n"
                "    string words[4] = {\"a\", \"bb\", \"ccc\", \"dddd\"};\n"
                "    int out[4];\n"
                "    parallel for (int i = 0; i < 4; ++i)\n"
                "        out[i] = string_size(words[i]) * (i + 1);\n"
                "    return out[0] + out[1] + out[2] + out[3];\n"
                "}\n"
                "int size() { return sizeof(string) * 100 + sizeof(string[3]); }\n");
    REQUIRE(jit);
    CHECK(lookup<int()>(*jit, "test")() == 10 - 100 + 2000 + 10000 + 100000);
    CHECK(lookup<int()>(*jit, "lengths")() == 1 + 4 + 9 + 16);
    CHECK(lookup<int()>(*jit, "size")() == 1600 + 48);
}

TEST_CASE("every string builtin is wait-free: a nonblocking function calls each, and the runtime's allocate nothing",
          "[codegen][string-type][effects]")
{
    auto jit =
        compile("int all(int k) [[clang::nonblocking]]\n"
                "{\n"
                "    char buffer[16];\n"
                "    string s = k > 0 ? \"preset-12\" : \"other\";\n"
                "    int n = string_copy(buffer, 16, s);\n"
                "    string kept = string_of(buffer, n);\n"
                "    string number = string_slice(kept, string_find(kept, \"-\") + 1, n);\n"
                "    const void* p = string_data(kept);\n"
                "    int r = string_size(kept) + string_byte(kept, 0) + string_int(number, 0) * 1000;\n"
                "    r += (string_starts(kept, \"pre\") ? 100000 : 0) + (string_ends(kept, \"12\") ? 200000 : 0);\n"
                "    r += (int)string_float(number, 0.5f) * 0 + (p != buffer ? 1 : 0) * 0;\n"
                "    switch (kept) { case \"preset-12\": r += 1000000; break; default: break; }\n"
                "    return r + (kept == s ? 10000000 : 0);\n"
                "}\n");
    REQUIRE(jit);
    auto* all = lookup<int(int)>(*jit, "all");
    CHECK(all(1) == 9 + 'p' + 12000 + 300000 + 1000000 + 10000000);
    if (HeapCalls::counted())
    {
        int result = 0;
        long calls = 0;
        {
            const HeapCalls counting;
            for (int i = 0; i < 100; ++i)
                result += all(i % 2);
            calls = counting.count();
        }
        CHECK(result == 50 * (9 + 'p' + 12000 + 300000 + 1000000 + 10000000) + 50 * (5 + 'o' + 10000000));
        CHECK(calls == 0);
    }
}

TEST_CASE("a string's errors, each once, at its column", "[codegen][string-type][errors]")
{
    CHECK(errorsOf("int f(string s, int k, float x)\n"
                   "{\n"
                   "    if (s) k++;\n"
                   "    k += s < s;\n"
                   "    string t = s + \"a\";\n"
                   "    k += s[0];\n"
                   "    k += (int)s;\n"
                   "    string u = 5;\n"
                   "    k += !s;\n"
                   "    k += x == \"a\";\n"
                   "    k += s == 1;\n"
                   "    s++;\n"
                   "    k += -s;\n"
                   "    return k;\n"
                   "}\n") ==
          "test.fm:3:9: error: statement requires expression of scalar type ('string' invalid)\n"
          "test.fm:3:9: note: a string is no condition: test whether it is empty with string_size(s) > 0\n"
          "test.fm:4:12: error: invalid operands to binary expression ('string' and 'string')\n"
          "test.fm:5:18: error: invalid operands to binary expression ('string' and 'string')\n"
          "test.fm:6:11: error: subscripted value is not a pointer\n"
          "test.fm:7:10: error: cannot cast from 'string' to 'int'\n"
          "test.fm:8:12: error: initializing 'string' with an expression of incompatible type 'int'\n"
          "test.fm:9:10: error: a logical operator needs scalar operands, not 'string'\n"
          "test.fm:9:10: note: a string is no condition: test whether it is empty with string_size(s) > 0\n"
          "test.fm:10:15: error: FMinus has no char arrays here: a string literal is a 'string' where one is "
          "wanted, or the argument of a builtin's string parameter\n"
          "test.fm:11:12: error: invalid operands to binary expression ('string' and 'int')\n"
          "test.fm:12:6: error: invalid operands to binary expression ('string' and 'int')\n"
          "test.fm:13:10: error: invalid argument type 'string' to unary expression\n");
    // Where a string is declared: no parameter of a module, no pointer, no external function's parameter or result.
    CHECK(errorsOf("string external(string s);\n"
                   "module M { param string p; };\n"
                   "void f() { string* p; }\n"
                   "void g(string list[4]) {}\n") ==
          "test.fm:1:8: error: an external function cannot take or return a 'string' yet: C passes a struct by "
          "each platform's own rules; define 'external' in the program, or pass string_data(s) and "
          "string_size(s)\n"
          "test.fm:2:25: error: a parameter is a float, an int or a bool, not 'string'\n"
          "test.fm:3:20: error: FMinus has pointers to float, double, int, float2, float4, float8, char, void and "
          "modules, not to 'string'\n"
          "test.fm:4:15: error: FMinus has pointers to float, double, int, float2, float4, float8, char, void and "
          "modules, not to 'string'\n");
    // A prototype before the definition is no external function, and a builtin's prototype is the builtin's.
    std::string errors;
    CHECK(compile("int twice(string s);\nint twice(string s) { return 2 * string_size(s); }\n"
                  "int string_size(string s);\nstring string_slice(string s, int from, int to);\n",
                  &errors));
    CHECK(errors.empty());
    CHECK(errorsOf("int string_size(char* s);\nstring string_of(char* data, int size);\n") ==
          "test.fm:1:5: error: conflicting types for 'string_size'\n"
          "test.fm:2:8: error: conflicting types for 'string_of'\n");
    // A switch on a string: literal cases only, each once.
    CHECK(errorsOf("int f(string s, int k)\n"
                   "{\n"
                   "    switch (s) { case \"a\": break; case \"b\" \"\": break; case \"a\": break; case k: break; }\n"
                   "    switch (k) { case \"a\": break; }\n"
                   "    switch (\"a\") { default: break; }\n"
                   "    return 0;\n"
                   "}\n") ==
          "test.fm:3:60: error: duplicate case value '\"a\"'\n"
          "test.fm:3:23: note: previous case defined here\n"
          "test.fm:3:77: error: case value of a switch on a 'string' must be a string literal\n"
          "test.fm:4:23: error: expression is not an integer constant expression\n"
          "test.fm:5:13: error: FMinus has no char arrays here: a string literal is a 'string' where one is "
          "wanted, or the argument of a builtin's string parameter\n");
    // char: an element and a pointee only.
    CHECK(errorsOf("char f(char c)\n"
                   "{\n"
                   "    char d = 1;\n"
                   "    char a[2] = \"abc\";\n"
                   "    char b[2][2] = \"ab\";\n"
                   "    return d;\n"
                   "}\n"
                   "module M { char c; };\n") ==
          "test.fm:1:6: error: FMinus has no 'char' variables, parameters or results: a char is an array's "
          "element or a pointer's pointee, and an element reads as an int from 0 to 255\n"
          "test.fm:1:13: error: FMinus has no 'char' variables, parameters or results: a char is an array's "
          "element or a pointer's pointee, and an element reads as an int from 0 to 255\n"
          "test.fm:3:10: error: FMinus has no 'char' variables, parameters or results: a char is an array's "
          "element or a pointer's pointee, and an element reads as an int from 0 to 255\n"
          "test.fm:4:17: error: initializer-string for char array is too long, array size is 2 but initializer "
          "has size 4 (including the null terminating character)\n"
          "test.fm:5:10: error: array initializer must be an initializer list\n"   // and the literal's own, as a
          "test.fm:5:20: error: FMinus has no char arrays here: a string literal is a 'string' where one is "
          "wanted, or the argument of a builtin's string parameter\n"   // float array's has always had
          "test.fm:8:17: error: FMinus has no 'char' variables, parameters or results: a char is an array's "
          "element or a pointer's pointee, and an element reads as an int from 0 to 255\n");
    // A builtin's literal parameter still takes a literal only.
    CHECK(errorsOf("int f(string s) { return __fminus_string_hash(s); }\n") ==
          "test.fm:1:47: error: argument to '__fminus_string_hash' must be a string literal\n");
}
