// tests/unit/ArrayTest.cpp: arrays (extending.md, recipe 12): C's, of float, int, float vectors and modules, zeroed,
// indexed, initialised from a list, standing for a pointer to their first element; and the errors, run through the
// JIT.
#include "Compile.h"

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <string>

using namespace fminus::test;

TEST_CASE("arrays are C's: zeroed, indexed, initialised from a list, and a pointer to their first element",
          "[codegen][array]")
{
    std::string errors;
    auto jit = compile("float ramp(int n)\n"
                       "{\n"
                       "    float a[8];\n"   // zeroed: the elements past n stay 0
                       "    for (int i = 0; i < n; ++i)\n"
                       "        a[i] = i;\n"
                       "    float s = 0;\n"
                       "    for (int i = 0; i < 8; ++i)\n"
                       "        s += a[i];\n"
                       "    return s;\n"
                       "}\n"
                       "float listed() { float c[4] = {1, 2}; return c[0] + 10 * c[1] + 100 * c[2] + 1000 * c[3]; }\n"
                       "float at(const float* p, int i) { return p[i]; }\n"
                       "float decays()\n"
                       "{\n"
                       "    float c[3] = {5, 6, 7,};\n"
                       "    float* p = c;\n"                               // the array's name is a pointer
                       "    return at(c, 2) + *(p + 1) + (p == &c[0]) + (c + 3 - p);\n"
                       "}\n"
                       "int squares() { int k[3] = {1, 2, 3}; int t = 0; for (int i = 0; i < 3; ++i) t += k[i] * k[i];"
                       " return t; }\n"
                       "float vectors()\n"
                       "{\n"
                       "    float4 v[2];\n"
                       "    v[1].y = 3;\n"
                       "    v[0] = (float4)(1);\n"
                       "    return v[0].x + v[1].y + v[1].x;\n"
                       "}\n"
                       "float fresh()\n"
                       "{\n"
                       "    float s = 0;\n"
                       "    for (int r = 0; r < 3; ++r)\n"
                       "    {\n"
                       "        float a[2];\n"   // each time it is declared, it starts at 0 again
                       "        s += a[0];\n"
                       "        a[0] = 5;\n"
                       "    }\n"
                       "    return s;\n"
                       "}\n"
                       "void twice(float x[4], int n) { for (int i = 0; i < n; ++i) x[i] *= 2; }\n"   // a float*
                       "float readonly() { const float c[2] = {3, 4}; const float* p = c; return p[0] * c[1]; }\n",
                       &errors);
    INFO(errors);
    REQUIRE(jit);
    CHECK(lookup<float(int)>(*jit, "ramp")(5) == 10.0f);
    CHECK(lookup<float()>(*jit, "listed")() == 21.0f);
    CHECK(lookup<float()>(*jit, "decays")() == 17.0f);   // 7 + 6 + 1 + 3
    CHECK(lookup<int()>(*jit, "squares")() == 14);
    CHECK(lookup<float()>(*jit, "vectors")() == 4.0f);
    CHECK(lookup<float()>(*jit, "fresh")() == 0.0f);
    CHECK(lookup<float()>(*jit, "readonly")() == 12.0f);
    std::array<float, 3> buffer{1.0f, 2.0f, 3.0f};
    lookup<void(float*, int)>(*jit, "twice")(buffer.data(), 3);   // a C++ host's buffer: the parameter is a pointer
    CHECK(buffer == std::array{2.0f, 4.0f, 6.0f});
}

TEST_CASE("array errors are reported as clang reports them", "[codegen][array]")
{
    CHECK(errorsOf("float f(float x)\n"
                   "{\n"
                   "    float a[4];\n"
                   "    float b[4] = a;\n"
                   "    float c[2] = {1, 2, 3};\n"
                   "    a = b;\n"
                   "    a += 1;\n"
                   "    a++;\n"
                   "    bool flags[4];\n"
                   "    &a;\n"
                   "    float y = {1};\n"
                   "    return a;\n"
                   "}\n")
          == "test.fm:4:11: error: array initializer must be an initializer list\n"
             "test.fm:5:11: error: excess elements in array initializer\n"
             "test.fm:6:7: error: array type 'float[4]' is not assignable\n"
             "test.fm:7:7: error: array type 'float[4]' is not assignable\n"
             "test.fm:8:6: error: array type 'float[4]' is not assignable\n"
             "test.fm:9:10: error: FMinus has arrays of float, int, float2, float4, float8 and modules, not of 'bool'\n"
             "test.fm:10:5: error: FMinus has pointers to float, int, float2, float4, float8, void and modules, not to "
             "'float[4]'\n"
             "test.fm:11:11: error: FMinus takes an initializer list for an array only\n"
             "test.fm:12:5: error: returning 'float *' from a function with incompatible result type 'float'\n");

    // The size is the parser's to check: a positive integer constant.
    CHECK(errorsOf("void f() { float a[1.5]; }")
          == "test.fm:1:20: error: size of array has non-integer type 'float'\n");
    CHECK(errorsOf("void f() { float a[0]; }") == "test.fm:1:20: error: zero size arrays are not allowed\n");
    CHECK(errorsOf("void f() { float a[0x7fffffff]; }")
          == "test.fm:1:20: error: array is too large: FMinus's arrays have at most 2^30 elements\n");
    CHECK(errorsOf("void f() { float* a[2]; }") == "test.fm:1:19: error: FMinus has no arrays of pointers\n");
}
