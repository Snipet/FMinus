// tests/unit/IntTest.cpp: int (extending.md, recipe 10): C's integer constants, arithmetic and conversions, casts, ++
// and --, run through the JIT.
#include "Compile.h"

#include <catch2/catch_test_macros.hpp>

#include <string>

using namespace fminus::test;

TEST_CASE("int is C's: integer constants, arithmetic, and the usual arithmetic conversions", "[codegen][int]")
{
    std::string errors;
    auto jit = compile("int divide(int a, int b) { return a / b; }\n"      // truncates toward zero
                       "int remainder(int a, int b) { return a % b; }\n"   // with the dividend's sign
                       "int constants() { return 010 + 0x1F + 0XA + 2147483647 - 2147483647; }\n"   // 8 + 31 + 10
                       "float half() { return 1 / 2; }\n"                  // two ints divide as ints: 0
                       "float halved() { return 1 / 2.0f; }\n"             // an int meeting a float is a float
                       "int count(bool a, bool b) { return a + b; }\n"     // a bool is 0 or 1, as an int
                       "bool less(int a, int b) { return a < b; }\n"       // signed
                       "int negate(int a) { return -a; }\n"
                       "float scale(int i, float x) { return i * x; }\n"
                       "int truncate(float x) { return x; }\n"             // toward zero
                       "bool truthy(int x) { return x; }\n"                // x != 0
                       "int sum(int n) { int s = 0; for (int i = 1; i <= n; i += 1) s += i; return s; }\n"
                       "int scaled(int i) { i *= 1.5f; return i; }\n",     // a float product, truncated to int
                       &errors);
    INFO(errors);
    REQUIRE(jit);
    auto divide = lookup<int(int, int)>(*jit, "divide");
    CHECK(divide(7, 2) == 3);
    CHECK(divide(-7, 2) == -3);
    CHECK(lookup<int(int, int)>(*jit, "remainder")(-7, 2) == -1);
    CHECK(lookup<int()>(*jit, "constants")() == 49);
    CHECK(lookup<float()>(*jit, "half")() == 0.0f);
    CHECK(lookup<float()>(*jit, "halved")() == 0.5f);
    CHECK(lookup<int(bool, bool)>(*jit, "count")(true, true) == 2);
    CHECK(lookup<bool(int, int)>(*jit, "less")(-1, 1));
    CHECK(lookup<int(int)>(*jit, "negate")(5) == -5);
    CHECK(lookup<float(int, float)>(*jit, "scale")(3, 0.5f) == 1.5f);
    auto truncate = lookup<int(float)>(*jit, "truncate");
    CHECK(truncate(2.7f) == 2);
    CHECK(truncate(-2.7f) == -2);
    CHECK(lookup<bool(int)>(*jit, "truthy")(-3));
    CHECK_FALSE(lookup<bool(int)>(*jit, "truthy")(0));
    CHECK(lookup<int(int)>(*jit, "sum")(100) == 5050);
    CHECK(lookup<int(int)>(*jit, "scaled")(3) == 4);
}

TEST_CASE("casts convert between scalars, as C's implicit conversions do", "[codegen][int]")
{
    std::string errors;
    auto jit = compile("float ratio(int a, int b) { return (float)a / b; }\n"   // a float division
                       "int whole(float x) { return (int)floorf(x); }\n"
                       "bool nonzero(float x) { return (bool)x; }\n"
                       "float fraction(float x) { return x - (int)x; }\n",
                       &errors);
    INFO(errors);
    REQUIRE(jit);
    CHECK(lookup<float(int, int)>(*jit, "ratio")(7, 2) == 3.5f);
    CHECK(lookup<int(float)>(*jit, "whole")(-1.5f) == -2);
    CHECK(lookup<bool(float)>(*jit, "nonzero")(0.25f));
    CHECK(lookup<float(float)>(*jit, "fraction")(2.75f) == 0.75f);
}

TEST_CASE("++ and -- are C's, prefix and postfix, on any lvalue", "[codegen][int]")
{
    std::string errors;
    auto jit = compile("int loop(int n) { int s = 0; for (int i = 0; i < n; ++i) s += i; return s; }\n"
                       "int post() { int i = 5; int j = i++; return j * 10 + i; }\n"   // 5, then 6
                       "int pre() { int i = 5; int j = --i; return j * 10 + i; }\n"    // 4 and 4
                       "float twice(float x) { x++; x++; --x; return x; }\n"           // floats too
                       "float4 lanes(float4 v) { v++; v.x--; return v; }\n"            // every lane, or one
                       "bool set() { bool b = false; b++; return b; }\n",              // ++ makes a bool true
                       &errors);
    INFO(errors);
    REQUIRE(jit);
    CHECK(lookup<int(int)>(*jit, "loop")(10) == 45);
    CHECK(lookup<int()>(*jit, "post")() == 56);
    CHECK(lookup<int()>(*jit, "pre")() == 44);
    CHECK(lookup<float(float)>(*jit, "twice")(1.5f) == 2.5f);
    CHECK(lanesOf(lookup<float4(float4)>(*jit, "lanes")(float4{1.0f, 2.0f, 3.0f, 4.0f}))
          == std::array{1.0f, 3.0f, 4.0f, 5.0f});
    CHECK(lookup<bool()>(*jit, "set")());
}

TEST_CASE("int errors are reported as clang reports them", "[codegen][int]")
{
    CHECK(errorsOf("int f(int a, float4 v)\n"
                   "{\n"
                   "    int x = 08;\n"
                   "    int y = 2147483648;\n"
                   "    ++(a + 1);\n"
                   "    const int k = 1;\n"
                   "    k++;\n"
                   "    (int)v;\n"
                   "    return a;\n"
                   "}\n")
          == "test.fm:3:13: error: invalid digit '8' in octal constant\n"
             "test.fm:4:13: error: integer constant is too large for type 'int'\n"
             "test.fm:5:5: error: expression is not assignable\n"
             "test.fm:7:6: error: cannot assign to variable 'k' with const-qualified type 'const int'\n"
             "test.fm:8:5: error: cannot cast from 'float4' to 'int'\n");
}
