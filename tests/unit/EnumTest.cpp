// tests/unit/EnumTest.cpp: C's enumerations (extending.md, recipe 27), run through the JIT: values implicit, explicit
// and negative, constants used as ints (and `enum Mode` as the type int), C's scopes (a block's, hidden by an inner
// variable; the file's for the top level and modules, seen by what follows and by a module's methods, a field hiding
// one in a method), case labels and constant tables of them, in a parallel for, and clang's errors.
#include "Compile.h"

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <string>

using namespace fminus::test;

TEST_CASE("an enumeration counts from 0, a constant may take a value of its own, and the next counts on from it",
          "[codegen][enum]")
{
    std::string errors;
    auto jit = compile("enum Wave { SAW, SQUARE = 4, TRIANGLE, NOISE = -3, AFTER, LAST = TRIANGLE * 10 + AFTER, };"
                       "\n"
                       "enum { LEAST = -2147483647 - 1, NEXT, GREATEST = 2147483647 };\n"
                       "int wave(int i)\n"
                       "{\n"
                       "    const int values[6] = {SAW, SQUARE, TRIANGLE, NOISE, AFTER, LAST};\n"   // a table
                       "    return values[i];\n"
                       "}\n"
                       "int extremes(int i) { return i == 0 ? LEAST : i == 1 ? NEXT : GREATEST; }\n"
                       "float asInt(float x)\n"   // an int in arithmetic, a comparison, an index and a call
                       "{\n"
                       "    float t[5] = {1, 2, 3, 4, 5};\n"
                       "    return x * SQUARE + t[TRIANGLE - 1] + (SAW < NOISE) + wave(SQUARE / 4)\n"
                       "           + (float)AFTER / 4;\n"
                       "}\n"
                       "enum Wave next(enum Wave w)\n"   // `enum Wave` is int: a parameter, a result, a cast
                       "{\n"
                       "    enum Wave n = (enum Wave)(w + 1);\n"
                       "    return n + sizeof(enum Wave);\n"
                       "}\n",
                       &errors);
    INFO(errors);
    REQUIRE(jit);
    auto wave = lookup<int(int)>(*jit, "wave");
    CHECK(wave(0) == 0);
    CHECK(wave(1) == 4);
    CHECK(wave(2) == 5);
    CHECK(wave(3) == -3);
    CHECK(wave(4) == -2);
    CHECK(wave(5) == 48);
    auto extremes = lookup<int(int)>(*jit, "extremes");
    CHECK(extremes(0) == -2147483647 - 1);
    CHECK(extremes(1) == -2147483647);
    CHECK(extremes(2) == 2147483647);
    CHECK(lookup<float(float)>(*jit, "asInt")(2.0f) == 8.0f + 5.0f + 0.0f + 4.0f - 0.5f);
    CHECK(lookup<int(int)>(*jit, "next")(4) == 9);
}

TEST_CASE("enumeration constants are named as C names them: from just after each one, in its scope", "[codegen][enum]")
{
    std::string errors;
    auto jit = compile("enum { N = 8 };\n"
                       "module Osc\n"
                       "{\n"
                       "    enum Shape { SINE = 100, PULSE };\n"   // the file's: seen by every method, and after
                       "    float N;\n"                           // a field hides the file's N in a method
                       "    float tick() { return SINE + early() + N; }\n"
                       "    float early() { return PULSE; }\n"
                       "    void set(float n) { N = n; }\n"
                       "};\n"
                       "float shapes(float x)\n"
                       "{\n"
                       "    Osc o;\n"
                       "    o.set(x);\n"
                       "    return o.tick() + PULSE + N;\n"
                       "}\n"
                       "int scopes(int x)\n"
                       "{\n"
                       "    enum { A = 1, B = A + 1 };\n"         // each from just after its own definition
                       "    int r = B;\n"
                       "    {\n"
                       "        int A = 10;\n"                    // a variable of an inner block hides a constant
                       "        enum { C = B * 100, B = 7 };\n"   // C is 200: its B is the outer one
                       "        r += A + C + B;\n"
                       "    }\n"
                       "    {\n"
                       "        enum { A = A + 50 };\n"           // its value's A is the outer one: 51
                       "        r += A;\n"
                       "    }\n"
                       "    return r + A + x;\n"
                       "}\n",
                       &errors);
    INFO(errors);
    REQUIRE(jit);
    CHECK(lookup<float(float)>(*jit, "shapes")(0.5f) == 100.0f + 101.0f + 0.5f + 101.0f + 8.0f);
    CHECK(lookup<int(int)>(*jit, "scopes")(1000) == 2 + 10 + 200 + 7 + 51 + 1 + 1000);
}

TEST_CASE("case labels name enumeration constants, and a duplicate is named as clang names it", "[codegen][enum]")
{
    std::string errors;
    auto jit = compile("enum Mode { LOW, BAND, HIGH, NOTCH = 10 };\n"
                       "float response(enum Mode mode, float x)\n"
                       "{\n"
                       "    switch (mode)\n"
                       "    {\n"
                       "    case LOW: return x;\n"
                       "    case BAND: return x * 2;\n"
                       "    case HIGH:\n"
                       "    case NOTCH: return -x;\n"
                       "    case NOTCH + 1: return 100;\n"
                       "    default: return 0;\n"
                       "    }\n"
                       "}\n",
                       &errors);
    INFO(errors);
    REQUIRE(jit);
    auto response = lookup<float(int, float)>(*jit, "response");
    CHECK(response(0, 3.0f) == 3.0f);
    CHECK(response(1, 3.0f) == 6.0f);
    CHECK(response(2, 3.0f) == -3.0f);
    CHECK(response(10, 3.0f) == -3.0f);
    CHECK(response(11, 3.0f) == 100.0f);
    CHECK(response(5, 3.0f) == 0.0f);

    CHECK(errorsOf("enum { A = 1, B = 1 };\n"
                   "int f(int x)\n"
                   "{\n"
                   "    switch (x)\n"
                   "    {\n"
                   "    case A: break;\n"
                   "    case B: break;\n"
                   "    case (int)A: break;\n"
                   "    case 1: break;\n"
                   "    }\n"
                   "    return 0;\n"
                   "}\n")
          == "test.fm:7:10: error: duplicate case value: 'A' and 'B' both equal '1'\n"
             "test.fm:6:10: note: previous case defined here\n"
             "test.fm:8:10: error: duplicate case value: 'B' and 'A' both equal '1'\n"
             "test.fm:7:10: note: previous case defined here\n"
             "test.fm:9:10: error: duplicate case value: 'A' and '1' both equal '1'\n"
             "test.fm:8:10: note: previous case defined here\n");
}

TEST_CASE("a parallel for reads enumeration constants where it is, as constants", "[codegen][enum][parallel]")
{
    std::string errors;
    auto jit = compile("enum { SCALE = 3 };\n"
                       "void fill(float* out, int n)\n"
                       "{\n"
                       "    enum { OFFSET = 10 };\n"
                       "    parallel for (int i = 0; i < n; ++i)\n"
                       "    {\n"
                       "        enum { ONE = 1 };\n"
                       "        out[i] = i * SCALE + OFFSET + ONE;\n"
                       "    }\n"
                       "}\n",
                       &errors);
    INFO(errors);
    REQUIRE(jit);
    std::array<float, 40> out{};
    lookup<void(float*, int)>(*jit, "fill")(out.data(), 40);
    for (std::size_t i = 0; i < out.size(); ++i)
    {
        INFO(i);
        CHECK(out[i] == static_cast<float>(i * 3 + 11));
    }
}

TEST_CASE("enumerations' errors are clang's: redefinitions, values, and a constant used as a variable",
          "[codegen][enum]")
{
    CHECK(errorsOf("float early(float x) { return x + LATE; }\n"
                   "enum { A, B };\n"
                   "enum { A = 3 };\n"
                   "enum { BIG = 2147483647, OVER };\n"
                   "enum { f, g };\n"
                   "float f(float x) { return x; }\n"
                   "float h(float x);\n"
                   "enum { h, V = 1.5f, W = nope, U, X = U + 1 / 0 };\n"
                   "enum { Q };\n"
                   "module Q { float y; };\n"
                   "enum { LATE = 4 };\n"
                   "float g(float x) { return x; }\n")
          == "test.fm:1:35: error: use of undeclared identifier 'LATE'\n"
             "test.fm:3:8: error: redefinition of enumerator 'A'\n"
             "test.fm:2:8: note: previous definition is here\n"
             "test.fm:4:26: error: ISO C restricts enumerator values to range of 'int' (2147483648 is too large)\n"
             "test.fm:6:7: error: redefinition of 'f' as different kind of symbol\n"
             "test.fm:5:8: note: previous definition is here\n"
             "test.fm:8:8: error: redefinition of 'h'\n"
             "test.fm:7:7: note: previous definition is here\n"
             "test.fm:8:15: error: integer constant expression must have integer type, not 'float'\n"
             "test.fm:8:25: error: use of undeclared identifier 'nope'\n"
             "test.fm:8:44: error: expression is not an integer constant expression\n"
             "test.fm:8:44: note: division by zero is undefined\n"
             "test.fm:10:8: error: redefinition of 'Q' as different kind of symbol\n"
             "test.fm:9:8: note: previous definition is here\n"
             "test.fm:12:7: error: redefinition of 'g' as different kind of symbol\n"
             "test.fm:5:11: note: previous definition is here\n");
    CHECK(errorsOf("enum { X };\n"
                   "float f(int A, int k)\n"
                   "{\n"
                   "    enum { A = 1 };\n"     // the parameters' scope is the body's outermost one
                   "    int B = 2;\n"
                   "    enum { B, C = k };\n"
                   "    float C = 2;\n"
                   "    X = 2;\n"
                   "    X++;\n"
                   "    --X;\n"
                   "    X += 1;\n"
                   "    float* p = &X;\n"
                   "    float y = X.x;\n"
                   "    return B + C;\n"
                   "}\n")
          == "test.fm:4:12: error: redefinition of 'A'\n"
             "test.fm:2:13: note: previous definition is here\n"
             "test.fm:6:12: error: redefinition of 'B'\n"
             "test.fm:5:9: note: previous definition is here\n"
             "test.fm:6:19: error: expression is not an integer constant expression\n"
             "test.fm:7:11: error: redefinition of 'C' as different kind of symbol\n"
             "test.fm:6:15: note: previous definition is here\n"
             "test.fm:8:7: error: expression is not assignable\n"
             "test.fm:9:6: error: expression is not assignable\n"
             "test.fm:10:5: error: expression is not assignable\n"
             "test.fm:11:7: error: expression is not assignable\n"
             "test.fm:12:16: error: cannot take the address of an rvalue of type 'int'\n"
             "test.fm:13:16: error: member reference base type 'int' is not a structure or union\n");
    CHECK(errorsOf("void f(float* out)\n"
                   "{\n"
                   "    parallel { enum { T }; out[0] = 1; }\n"
                   "}\n")
          == "test.fm:3:16: error: a statement of a parallel block cannot declare an enumeration (its constants would "
             "end with it): declare it before the block\n");
}
