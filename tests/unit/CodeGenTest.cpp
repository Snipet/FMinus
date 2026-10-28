// tests/unit/CodeGenTest.cpp: the code generator (src/ir/), run through the JIT (src/jit/Jit.h): functions, variables,
// prototypes, builtins, %, constants and the errors. ControlFlowTest.cpp, VectorTest.cpp and PolyTest.cpp have the
// rest; Compile.h has what they share.
#include "Compile.h"
#include "fminus/FMinus.h"

#include <catch2/catch_test_macros.hpp>

#include <cmath>
#include <string>

using namespace fminus::test;

TEST_CASE("functions compute what they say", "[codegen]")
{
    auto jit = compile("float softclip(float x, const float drive)\n"
                       "{\n"
                       "    const float y = x * drive;\n"
                       "    return y / (1.0f + fabsf(y));\n"
                       "}\n"
                       "float chain(float x) { return softclip(half(x), 4); }\n"   // calls before definitions
                       "float half(float x) { return x / 2; }\n"
                       "float constant() { return -(1 - 3) * 2.5f + +1; }\n");
    REQUIRE(jit);
    CHECK(lookup<float(float, float)>(*jit, "softclip")(1.0f, 1.0f) == 0.5f);
    CHECK(lookup<float(float)>(*jit, "chain")(1.0f) == 2.0f / 3.0f);
    CHECK(lookup<float()>(*jit, "constant")() == 6.0f);
}

TEST_CASE("variables have C's semantics: assignable, scoped, zeroed when uninitialised", "[codegen]")
{
    auto jit = compile("float f(float x)\n"
                       "{\n"
                       "    float a, b = x;\n"          // a starts at 0
                       "    a += b * 2;\n"              // compound assignment
                       "    x = a = a - 1;\n"           // assignment is an expression, right to left
                       "    { float a = 100; b = a; }\n" // a block's a shadows, and ends with it
                       "    return a + b + x;\n"
                       "}\n"
                       "float g(float x) { x *= x; x -= 1; x /= 2; return x; }\n"   // parameters are assignable
                       "float h(float x) { return x; x = 7; return x; }\n");        // code after a return is accepted
    REQUIRE(jit);
    CHECK(lookup<float(float)>(*jit, "f")(3.0f) == 5.0f + 100.0f + 5.0f);
    CHECK(lookup<float(float)>(*jit, "g")(3.0f) == 4.0f);
    CHECK(lookup<float(float)>(*jit, "h")(2.0f) == 2.0f);
}

TEST_CASE("prototypes declare functions defined later, or elsewhere (external, resolved at link time)", "[codegen]")
{
    auto jit = compile("float half(float);\n"
                       "float atanf(float x);\n"   // no definition: external, libm's (the JIT looks in the process)
                       "float sinf(float);\n"      // a builtin's prototype, as <math.h> has, changes nothing
                       "float f(float x) { return atanf(half(x)) + sinf(0); }\n"
                       "float half(const float x) { return x / 2; }\n");
    REQUIRE(jit);
    CHECK(lookup<float(float)>(*jit, "f")(2.0f) == std::atan(1.0f));
    CHECK(errorsOf("float f(float x);\n"
                   "float f(float x, float y) { return x; }\n"
                   "float sinf(float a, float b);\n"
                   "float g(float) { return 1; }\n"
                   "float k(float);\n"
                   "float k(float x) { return x; }\n"
                   "float k(float x) { return x; }\n")
          == "test.fm:2:7: error: conflicting types for 'f'\n"
             "test.fm:3:7: error: conflicting types for 'sinf'\n"
             "test.fm:4:9: error: parameter name omitted\n"
             "test.fm:7:7: error: redefinition of 'k'\n");
}

TEST_CASE("builtins are <math.h>'s float functions: LLVM's intrinsics, and the runtime's transcendentals", "[codegen]")
{
    auto jit = compile("float f(float x) { return sqrtf(x) + fabsf(-x) + floorf(x / 2) + fminf(x, 1) + fmaxf(x, 1); }\n"
                       "float g(float x) { return sinf(x) * sinf(x) + cosf(x) * cosf(x) + logf(expf(x)); }\n"
                       "float h(float x) { return tanhf(x); }\n"
                       "float t(float x) { return tanf(x); }\n");
    REQUIRE(jit);
    CHECK(lookup<float(float)>(*jit, "f")(9.0f) == 3.0f + 9.0f + 4.0f + 1.0f + 9.0f);
    CHECK(std::abs(lookup<float(float)>(*jit, "g")(0.25f) - 1.25f) < 1e-6f);
    CHECK(lookup<float(float)>(*jit, "h")(0.5f) == fminus::math::tanh(0.5f));   // the default math mode (MathTest.cpp)
    CHECK(lookup<float(float)>(*jit, "t")(0.5f) == fminus::math::tan(0.5f));
    CHECK(fminus::ir::builtins().size() == 11);
}

TEST_CASE("% is fmodf's remainder: the sign of the dividend", "[codegen]")
{
    auto jit = compile("float f(float x, float y) { return x % y * 2; }\n"
                       "float wrap(float phase) { phase %= 1; return phase; }\n");
    REQUIRE(jit);
    CHECK(lookup<float(float, float)>(*jit, "f")(7.5f, 2.0f) == 3.0f);    // (7.5 % 2) * 2: % binds as * and /
    CHECK(lookup<float(float, float)>(*jit, "f")(-7.5f, 2.0f) == -3.0f);  // the sign of the dividend
    CHECK(lookup<float(float)>(*jit, "wrap")(2.25f) == 0.25f);
}

TEST_CASE("a constant is rounded once, straight to float", "[codegen]")
{
    // Just above the midpoint of 1 and the next float: through double it would land on the midpoint and round down.
    auto jit = compile("float f() { return 1.0000000596046447753906250000000001; }\n"
                       "float g() { return 0x1.000002p0f + 0x1p-149; }\n");   // hex floats are exact
    REQUIRE(jit);
    CHECK(lookup<float()>(*jit, "f")() == std::nextafter(1.0f, 2.0f));
    CHECK(lookup<float()>(*jit, "g")() == std::nextafter(1.0f, 2.0f));
}

TEST_CASE("void functions return nothing, as C's do", "[codegen]")
{
    std::string errors;
    auto jit = compile("void nothing() {}\n"                                     // falls off its end
                       "void early(float x) { if (x > 0) return; x = 1; }\n"
                       "float f(void) { nothing(); early(1); return 2; }\n",    // (void): C's empty list
                       &errors);
    INFO(errors);
    REQUIRE(jit);
    lookup<void()>(*jit, "nothing")();
    CHECK(lookup<float()>(*jit, "f")() == 2.0f);
    CHECK(errorsOf("void v() {}\n"
                   "float f(float x, void y)\n"
                   "{\n"
                   "    void z;\n"
                   "    float a = v();\n"
                   "    x = v() + 1;\n"
                   "    -v();\n"
                   "    x > 0 ? v() : 1;\n"
                   "    return;\n"
                   "}\n"
                   "void g() { return 1; }\n")
          == "test.fm:2:23: error: argument may not have 'void' type\n"
             "test.fm:4:10: error: variable has incomplete type 'void'\n"
             "test.fm:5:11: error: initializing 'float' with an expression of incompatible type 'void'\n"
             "test.fm:6:13: error: invalid operands to binary expression ('void' and 'int')\n"
             "test.fm:7:5: error: invalid argument type 'void' to unary expression\n"
             "test.fm:8:11: error: left operand to ? is void, but right operand is of type 'int'\n"
             "test.fm:9:5: error: non-void function 'f' should return a value\n"
             "test.fm:11:12: error: void function 'g' should not return a value\n");
}

TEST_CASE("every semantic error is reported, in source order, as clang words it", "[codegen]")
{
    CHECK(errorsOf("float f(float x, float x)\n"
                   "{\n"
                   "    float y = x + z;\n"
                   "    float y = nope(x);\n"
                   "    const float k = 1;\n"
                   "    k = 2;\n"
                   "    x + 1 = 3;\n"
                   "    return sqrtf(x, y);\n"
                   "}\n"
                   "float f(float a) { return a; }\n"
                   "float sinf(float a) { return a; }\n"
                   "float g() { float big = 1e60f; }\n"
                   "float h() { return g(1) + q(); }\n")
          == "test.fm:1:24: error: redefinition of parameter 'x'\n"
             "test.fm:3:19: error: use of undeclared identifier 'z'\n"
             "test.fm:4:11: error: redefinition of 'y'\n"
             "test.fm:4:15: error: call to undeclared function 'nope'\n"
             "test.fm:6:7: error: cannot assign to variable 'k' with const-qualified type 'const float'\n"
             "test.fm:7:11: error: expression is not assignable\n"
             "test.fm:8:12: error: too many arguments to function call, expected 1, have 2\n"
             "test.fm:10:7: error: conflicting types for 'f'\n"
             "test.fm:11:7: error: 'sinf' is a builtin and cannot be redefined\n"
             "test.fm:12:7: error: non-void function 'g' does not return a value\n"
             "test.fm:12:25: error: magnitude of floating-point constant too large for type 'float'\n"
             "test.fm:13:20: error: too many arguments to function call, expected 0, have 1\n"
             "test.fm:13:27: error: call to undeclared function 'q'\n");
}
