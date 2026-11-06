// tests/unit/DoubleTest.cpp: double, C's IEEE double precision (extending.md, recipe 33), run through the JIT. Floating
// constants are floats first, as FMinus has always had them, and C's doubles in an arithmetic that has a double (an
// operand, the target its value goes to, a comparison's other side); C's usual arithmetic conversions and casts both
// ways; doubles in modules, arrays, pointers and constants of the top level; the double builtins (sqrt, fabs, floor,
// fmin, fmax, fma); libm's double functions by prototype; subnormals flushed; and clang's errors where a double meets a
// vector, a parameter or a bitwise operator.
#include "Compile.h"

#include <catch2/catch_test_macros.hpp>

#include <bit>
#include <cmath>
#include <cstdint>
#include <limits>
#include <string>

using namespace fminus::test;

TEST_CASE("a floating constant is a float first: arithmetic with no double computes as it always did", "[double]")
{
    std::string errors;
    auto jit = compile("float scaled(float x) { return x * 0.1; }\n"
                       "float third() { return 1.0 / 3.0; }\n"
                       "bool below(float x) { return x < 0.1; }\n"
                       "float narrowed(float x) { return (float)(x * 0.1 + 0.2); }\n",
                       &errors);
    INFO(errors);
    REQUIRE(jit);
    CHECK(lookup<float(float)>(*jit, "scaled")(3.0f) == 3.0f * 0.1f);
    CHECK(lookup<float()>(*jit, "third")() == 1.0f / 3.0f);
    CHECK_FALSE(lookup<bool(float)>(*jit, "below")(0.1f));   // C would compare 0.1f with 0.1, and find it above
    CHECK(lookup<float(float)>(*jit, "narrowed")(3.0f) == 3.0f * 0.1f + 0.2f);
}

TEST_CASE("a floating constant is C's double in an arithmetic that has a double", "[double]")
{
    std::string errors;
    auto jit = compile("double third() { return 1.0 / 3.0; }\n"                        // the result
                       "double tenth() { double d = 0.1; return d; }\n"                  // an initialiser
                       "double scaled(float x) { double y = x * 0.1; return y; }\n"      // a float operand, too
                       "double later(float x, double d) { return x * 0.1 * d; }\n"       // an operand after it
                       "double sum(double d) { return (0.1 + 0.2) + d; }\n"
                       "bool same(double d) { return d == 0.1; }\n"                      // a comparison's sides
                       "bool sameFloat(float f) { double d = f; return d == 0.1; }\n"
                       "double decay(double d) { d *= 0.999; return d; }\n"              // an assignment's target
                       "double pick(bool c, double d) { return c ? 0.1 : d; }\n"         // a ?:'s arm
                       "double cast() { return (double)0.1; }\n"
                       "double half(double x) { return x / 2; }\n"
                       "double passed() { return half(0.1); }\n"                          // a parameter
                       "double listed(int i) { const double t[2] = {0.1, 1.0 / 3.0}; return t[i]; }\n"
                       "double big() { return 1e300 * 10; }\n"
                       "double negative() { return -0.1; }\n",
                       &errors);
    INFO(errors);
    REQUIRE(jit);
    CHECK(lookup<double()>(*jit, "third")() == 1.0 / 3.0);
    CHECK(lookup<double()>(*jit, "tenth")() == 0.1);
    CHECK(lookup<double(float)>(*jit, "scaled")(3.0f) == 3.0 * 0.1);
    CHECK(lookup<double(float, double)>(*jit, "later")(3.0f, 2.0) == 3.0 * 0.1 * 2.0);
    CHECK(lookup<double(double)>(*jit, "sum")(1.0) == (0.1 + 0.2) + 1.0);
    CHECK(lookup<bool(double)>(*jit, "same")(0.1));
    CHECK_FALSE(lookup<bool(float)>(*jit, "sameFloat")(0.1f));
    CHECK(lookup<double(double)>(*jit, "decay")(2.0) == 2.0 * 0.999);
    CHECK(lookup<double(bool, double)>(*jit, "pick")(true, 2.0) == 0.1);
    CHECK(lookup<double()>(*jit, "cast")() == 0.1);
    CHECK(lookup<double()>(*jit, "passed")() == 0.05);
    CHECK(lookup<double(int)>(*jit, "listed")(0) == 0.1);
    CHECK(lookup<double(int)>(*jit, "listed")(1) == 1.0 / 3.0);
    CHECK(lookup<double()>(*jit, "big")() == 1e301);
    CHECK(lookup<double()>(*jit, "negative")() == -0.1);
    // A float's constant past a float's range is still an error; a double's is not.
    CHECK(errorsOf("float f() { return 1e39; }")
          == "test.fm:1:20: error: magnitude of floating-point constant too large for type 'float'\n");
}

TEST_CASE("C's usual arithmetic conversions: an int, a bool or a float meeting a double converts to it", "[double]")
{
    std::string errors;
    auto jit = compile("double add(float f, int i, double d) { return f + i + d; }\n"   // (f + i) is a float sum
                       "int truncated(double d) { int i = d; return i; }\n"
                       "float rounded(double d) { return d; }\n"
                       "double widened(float f) { return f; }\n"
                       "bool truth(double d) { return d; }\n"
                       "double fromBool(bool b) { return b + 0.5; }\n"
                       "double remainder(double a, double b) { return a % b; }\n"
                       "double counted(double d) { d++; ++d; d--; return -d; }\n"
                       "double mixed(double d, int n) { return n / 2 + d / n; }\n",   // n / 2 is an int quotient
                       &errors);
    INFO(errors);
    REQUIRE(jit);
    CHECK(lookup<double(float, int, double)>(*jit, "add")(0.1f, 3, 0.25)
          == static_cast<double>(0.1f + 3.0f) + 0.25);
    CHECK(lookup<int(double)>(*jit, "truncated")(-2.9) == -2);
    CHECK(lookup<float(double)>(*jit, "rounded")(0.1) == 0.1f);
    CHECK(lookup<float(double)>(*jit, "rounded")(1e300) == std::numeric_limits<float>::infinity());
    CHECK(lookup<double(float)>(*jit, "widened")(0.1f) == static_cast<double>(0.1f));
    CHECK(lookup<bool(double)>(*jit, "truth")(std::nan("")));
    CHECK_FALSE(lookup<bool(double)>(*jit, "truth")(0.0));
    CHECK(lookup<double(bool)>(*jit, "fromBool")(true) == 1.5);
    CHECK(lookup<double(double, double)>(*jit, "remainder")(-7.5, 2.0) == -1.5);   // fmod's: the dividend's sign
    CHECK(lookup<double(double)>(*jit, "counted")(0.25) == -1.25);
    CHECK(lookup<double(double, int)>(*jit, "mixed")(1.0, 3) == 1 + 1.0 / 3);
}

TEST_CASE("slow state: a double integrates steps a float rounds away", "[double]")
{
    // A temperature that moves a millionth of a degree a step from 90: as a float it never moves (half an ulp of 90 is
    // 3.8e-6), as a double it does, as C's would.
    std::string errors;
    auto jit = compile("float asFloat(int n) { float t = 90; for (int i = 0; i < n; ++i) t += 1e-6; return t; }\n"
                       "double asDouble(int n) { double t = 90; for (int i = 0; i < n; ++i) t += 1e-6; return t; }\n",
                       &errors);
    INFO(errors);
    REQUIRE(jit);
    CHECK(lookup<float(int)>(*jit, "asFloat")(100000) == 90.0f);
    double expected = 90;
    for (int i = 0; i < 100000; ++i)
        expected += 1e-6;
    CHECK(lookup<double(int)>(*jit, "asDouble")(100000) == expected);
}

TEST_CASE("doubles live in modules, arrays, pointers and the top level's constants", "[double]")
{
    std::string errors;
    auto jit = compile("const double kStep = 1.0 / 48000.0;\n"
                       "const double kTable[3] = {0.1, 0.2, 1.0 / 3.0};\n"
                       "module Clock\n"
                       "{\n"
                       "    double seconds;\n"
                       "    double history[4];\n"
                       "    float tick() { seconds += kStep; history[1] = seconds; return (float)seconds; }\n"
                       "};\n"
                       "double run(int n) { Clock c; for (int i = 0; i < n; ++i) c.tick(); return c.history[1]; }\n"
                       "double sum(const double* x, int n) { double s = 0; for (int i = 0; i < n; ++i) s += x[i];\n"
                       "                                     return s; }\n"
                       "double table() { return sum(kTable, 3); }\n"
                       "double through(double* p) { *p *= 2; p[1] = 0.5; return *p; }\n"
                       "int sizes() { return sizeof(double) * 1000 + sizeof(Clock); }\n",
                       &errors);
    INFO(errors);
    REQUIRE(jit);
    double clock = 0;
    for (int i = 0; i < 48000; ++i)
        clock += 1.0 / 48000.0;
    CHECK(lookup<double(int)>(*jit, "run")(48000) == clock);
    const double values[4] = {1.5, 2.5, 3.5, 4.5};
    CHECK(lookup<double(const double*, int)>(*jit, "sum")(values, 4) == 12.0);
    CHECK(lookup<double()>(*jit, "table")() == 0.1 + 0.2 + 1.0 / 3.0);
    double pair[2] = {1.25, 0};
    CHECK(lookup<double(double*)>(*jit, "through")(pair) == 2.5);
    CHECK(pair[1] == 0.5);
    CHECK(lookup<int()>(*jit, "sizes")() == 8000 + 40);   // 8 + 4 * 8, aligned to 8
}

TEST_CASE("the double builtins are C's, exact: sqrt, fabs, floor, fmin, fmax and fma", "[double]")
{
    std::string errors;
    auto jit = compile("double root(double x) { return sqrt(x); }\n"
                       "double magnitude(double x) { return fabs(x); }\n"
                       "double down(double x) { return floor(x); }\n"
                       "double least(double a, double b) { return fmin(a, b); }\n"
                       "double greatest(double a, float b) { return fmax(a, b); }\n"   // a float converts
                       "double fused(double a, double b, double c) { return fma(a, b, c); }\n"
                       "double plain(double a, double b, double c) { return a * b + c; }\n"   // no contraction
                       "float rootf(double x) { return sqrtf(x); }\n"   // a double rounds to sqrtf's float
                       "double sin(double x);\n"                        // no builtin: the C library's, by prototype
                       "double viaLibm(double x) { return sin(x); }\n",
                       &errors);
    INFO(errors);
    REQUIRE(jit);
    CHECK(lookup<double(double)>(*jit, "root")(2.0) == std::sqrt(2.0));
    CHECK(lookup<double(double)>(*jit, "magnitude")(-0.1) == 0.1);
    CHECK(lookup<double(double)>(*jit, "down")(-1.5) == -2.0);
    CHECK(lookup<double(double, double)>(*jit, "least")(0.1, std::nan("")) == 0.1);   // a NaN is ignored
    CHECK(lookup<double(double, float)>(*jit, "greatest")(0.1, 0.5f) == 0.5);
    // 1 + 2^-30 squared, less 1 + 2^-29: 2^-60 rounded once, where the product alone rounds it away.
    const double a = 1.0 + std::ldexp(1.0, -30);
    const double c = -(1.0 + std::ldexp(1.0, -29));
    CHECK(lookup<double(double, double, double)>(*jit, "fused")(a, a, c) == std::fma(a, a, c));
    CHECK(lookup<double(double, double, double)>(*jit, "fused")(a, a, c) == std::ldexp(1.0, -60));
    CHECK(lookup<double(double, double, double)>(*jit, "plain")(a, a, c) == 0.0);
    CHECK(lookup<float(double)>(*jit, "rootf")(2.0) == std::sqrt(2.0f));
    CHECK(lookup<double(double)>(*jit, "viaLibm")(0.5) == std::sin(0.5));
}

TEST_CASE("a flushing program folds and computes double subnormals as zero", "[double]")
{
    fminus::ir::Options options;
    options.denormals = fminus::ir::Denormals::Flush;
    std::string errors;
    auto jit = compile("double folded() [[clang::nonblocking]] { return 1e-300 * 1e-10; }\n"
                       "double computed(double x) [[clang::nonblocking]] { return x * 1e-10; }\n"
                       "float narrowed() [[clang::nonblocking]] { return (float)(double)1e-40; }\n"
                       "float rounded(double x) [[clang::nonblocking]] { return x; }\n",
                       &errors, options);
    INFO(errors);
    REQUIRE(jit);
    CHECK(lookup<double()>(*jit, "folded")() == 0.0);
    CHECK(lookup<double(double)>(*jit, "computed")(1e-300) == 0.0);
    CHECK(lookup<float()>(*jit, "narrowed")() == 0.0f);
    CHECK(lookup<float(double)>(*jit, "rounded")(1e-40) == 0.0f);
    // Preserved, IEEE's gradual underflow.
    auto ieee = compile("double folded() { return 1e-300 * 1e-10; }\n");
    REQUIRE(ieee);
    CHECK(lookup<double()>(*ieee, "folded")() == 1e-300 * 1e-10);
}

TEST_CASE("a double is no vector lane, parameter, bitwise operand, switch value or index: clang's errors",
          "[double]")
{
    CHECK(errorsOf("float4 a(float4 v, double d) { return v * d; }\n"
                   "float4 b(double d) { float4 v = d; return v; }\n"
                   "module M { param double k; };\n"
                   "int c(double d) { return d & 1; }\n"
                   "int s(double d) { switch (d) { default: return 1; } }\n"
                   "double g(double d) { return ~d; }\n"
                   "int h(double d) { float t[4]; return (int)t[d]; }\n"
                   "float k(double d) { return fminf((float4)(1.0f), d).x; }\n"
                   "float4 m(float4 v) { return sqrt(v); }\n"
                   "float n(double d) { return sin(d); }\n"
                   "float sqrt(float x);\n"
                   "double fabs(double x) { return x; }\n")
          == "test.fm:1:41: error: cannot convert between scalar type 'double' and vector type 'float4' as implicit "
             "conversion would cause truncation\n"
             "test.fm:2:29: error: initializing 'float4' with an expression of incompatible type 'double'\n"
             "test.fm:3:25: error: a parameter is a float, an int or a bool, not 'double'\n"
             "test.fm:4:28: error: invalid operands to binary expression ('double' and 'int')\n"
             "test.fm:5:27: error: statement requires expression of integer type ('double' invalid)\n"
             "test.fm:6:29: error: invalid argument type 'double' to unary expression\n"
             "test.fm:7:44: error: array subscript is not an integer\n"
             "test.fm:8:28: error: passing 'double' to parameter of incompatible type 'float4'\n"
             "test.fm:9:29: error: passing 'float4' to parameter of incompatible type 'double'\n"
             "test.fm:10:28: error: call to undeclared function 'sin'\n"
             "test.fm:10:28: note: FMinus's sinf computes in float, and rounds a double argument to one; a prototype, "
             "'double sin(double x);', calls the C library's on doubles\n"
             "test.fm:11:7: error: conflicting types for 'sqrt'\n"
             "test.fm:12:8: error: 'fabs' is a builtin and cannot be redefined\n");
    // A vector literal names its lanes' type: a double rounds to a float there, as a cast would.
    std::string errors;
    auto jit = compile("float4 f(double d) { return (float4)(d) + (float4){d, d, d, d}; }\n", &errors);
    INFO(errors);
    REQUIRE(jit);
    CHECK(lanesOf(lookup<float4(double)>(*jit, "f")(0.1)) == std::array{0.2f, 0.2f, 0.2f, 0.2f});
}
