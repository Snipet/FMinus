// tests/unit/VectorTest.cpp: float2, float4 and float8 (simd.md, recipe 7) and the vector math library (recipe 8),
// called from C++ through clang's ext_vector_type.
#include "Compile.h"

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include <array>
#include <cmath>
#include <string>

using namespace fminus::test;

TEST_CASE("float2, float4 and float8 are OpenCL C's vector types: literals, lanes, swizzles", "[codegen][simd]")
{
    std::string errors;
    auto jit = compile("float4 literal(float a) { return (float4)(a, 2, (float2)(3, 4)); }\n"   // lanes fill in order
                       "float4 splat(float a) { return (float4)(a); }\n"                       // one scalar: every lane
                       "float first_last(float4 v) { return v.x + v.w; }\n"
                       "float4 reverse(float4 v) { return v.wzyx; }\n"
                       "float4 numbered(float4 v) { return v.s3210 + v.xxyy; }\n"
                       "float2 middle(float4 v) { return v.yz; }\n"
                       "float4 set(float4 v) { v.x = 10; v.zw *= 2; return v; }\n"
                       "float2 swap(float2 v) { v.yx = v; return v; }\n"
                       "float4 nested(float4 v) { v.wzyx.x = 10; v.zw.y += 1; return v; }\n"   // v.w twice
                       "float4 zero() { float4 v; return v; }\n",   // indeterminate in C; FMinus zeroes it
                       &errors);
    INFO(errors);
    REQUIRE(jit);
    const float4 v = {1.0f, 2.0f, 3.0f, 4.0f};
    CHECK(lanesOf(lookup<float4(float)>(*jit, "literal")(7.0f)) == std::array{7.0f, 2.0f, 3.0f, 4.0f});
    CHECK(lanesOf(lookup<float4(float)>(*jit, "splat")(5.0f)) == std::array{5.0f, 5.0f, 5.0f, 5.0f});
    CHECK(lookup<float(float4)>(*jit, "first_last")(v) == 5.0f);
    CHECK(lanesOf(lookup<float4(float4)>(*jit, "reverse")(v)) == std::array{4.0f, 3.0f, 2.0f, 1.0f});
    CHECK(lanesOf(lookup<float4(float4)>(*jit, "numbered")(v)) == std::array{5.0f, 4.0f, 4.0f, 3.0f});
    const float2 yz = lookup<float2(float4)>(*jit, "middle")(v);
    CHECK((yz.x == 2.0f && yz.y == 3.0f));
    CHECK(lanesOf(lookup<float4(float4)>(*jit, "set")(v)) == std::array{10.0f, 2.0f, 6.0f, 8.0f});
    const float2 swapped = lookup<float2(float2)>(*jit, "swap")(float2{1.0f, 2.0f});
    CHECK((swapped.x == 2.0f && swapped.y == 1.0f));
    CHECK(lanesOf(lookup<float4(float4)>(*jit, "nested")(v)) == std::array{1.0f, 2.0f, 3.0f, 11.0f});
    CHECK(lanesOf(lookup<float4()>(*jit, "zero")()) == std::array{0.0f, 0.0f, 0.0f, 0.0f});
}

TEST_CASE("C's vector literal, in braces, fills the lanes in order and zeroes the rest", "[codegen][simd]")
{
    std::string errors;
    auto jit = compile("float4 full(float a) { return (float4){a, 2, 3, 4}; }\n"
                       "float4 one(float a) { return (float4){a}; }\n"            // not a broadcast, as (float4)(a) is
                       "float4 two(float a) { return (float4){a, 2,}; }\n"        // C's trailing comma
                       "float4 none() { return (float4){}; }\n"
                       "float4 copy(float4 v) { return (float4){v}; }\n"          // one of its own type: the value
                       "float4 mask(float a) { return (float4){a > 0, 1}; }\n"    // a bool is 0 or 1
                       "float eight() { return reduce_add((float8){1, 2, 3, 4, 5, 6, 7, 8}); }\n",
                       &errors);
    INFO(errors);
    REQUIRE(jit);
    CHECK(lanesOf(lookup<float4(float)>(*jit, "full")(7.0f)) == std::array{7.0f, 2.0f, 3.0f, 4.0f});
    CHECK(lanesOf(lookup<float4(float)>(*jit, "one")(7.0f)) == std::array{7.0f, 0.0f, 0.0f, 0.0f});
    CHECK(lanesOf(lookup<float4(float)>(*jit, "two")(7.0f)) == std::array{7.0f, 2.0f, 0.0f, 0.0f});
    CHECK(lanesOf(lookup<float4()>(*jit, "none")()) == std::array{0.0f, 0.0f, 0.0f, 0.0f});
    CHECK(lanesOf(lookup<float4(float4)>(*jit, "copy")(float4{1.0f, 2.0f, 3.0f, 4.0f}))
          == std::array{1.0f, 2.0f, 3.0f, 4.0f});
    CHECK(lanesOf(lookup<float4(float)>(*jit, "mask")(-1.0f)) == std::array{0.0f, 1.0f, 0.0f, 0.0f});
    CHECK(lookup<float()>(*jit, "eight")() == 36.0f);
    // C takes scalars only: a vector initialises the whole literal, if it comes first, or one lane. C only warns about
    // excess elements; FMinus refuses them.
    CHECK(errorsOf("float f(float2 p)\n"
                   "{\n"
                   "    (float4){1, 2, 3, 4, 5};\n"
                   "    (float4){p, 1};\n"
                   "    (float4){1, p};\n"
                   "    return 0;\n"
                   "}\n")
          == "test.fm:3:5: error: excess elements in vector initializer\n"
             "test.fm:4:5: error: initializing 'float4' with an expression of incompatible type 'float2'\n"
             "test.fm:5:5: error: initializing 'float' with an expression of incompatible type 'float2'\n");
}

TEST_CASE("vector arithmetic works lane by lane, broadcasting scalars, with builtins and reductions", "[codegen][simd]")
{
    std::string errors;
    auto jit = compile("float4 softclip4(float4 x, float drive)\n"
                       "{\n"
                       "    const float4 y = x * drive;\n"   // drive is broadcast to every lane
                       "    return y / (1.0f + fabsf(y));\n" // fabsf on a float4 is llvm.fabs.v4f32
                       "}\n"
                       "float4 wrap(float4 phase) { phase %= 1; return phase; }\n"
                       "float4 clip(float4 x) { return fminf(fmaxf(x, -1), 1); }\n"
                       "float sum(float4 v) { return reduce_add(v); }\n"
                       "float least(float4 v) { return reduce_min(v); }\n"
                       "float most(float4 v) { return reduce_max(v); }\n"
                       "float4 rectify(float4 x) { return x > 0 ? x : 0; }\n"   // a vector condition: per lane
                       "float4 halfwave(float4 x) { return x * (x > 0); }\n"    // a lane mask is 0 or 1
                       "float4 sines(float4 x) { return sinf(x); }\n"
                       "float wide(float a)\n"                                // float8: two NEON registers
                       "{\n"
                       "    const float8 v = (float8)(a) * (float8)(1, 2, 3, 4, 5, 6, 7, 8);\n"
                       "    return reduce_add(v + v.s76543210);\n"
                       "}\n",
                       &errors);
    INFO(errors);
    REQUIRE(jit);
    const float4 x = {-2.0f, -0.5f, 0.5f, 2.0f};
    std::array<float, 4> clipped{};
    for (int i = 0; i < 4; ++i)
        clipped[static_cast<std::size_t>(i)] = x[i] / (1.0f + std::fabs(x[i]));
    CHECK(lanesOf(lookup<float4(float4, float)>(*jit, "softclip4")(x, 1.0f)) == clipped);
    CHECK(lanesOf(lookup<float4(float4)>(*jit, "wrap")(float4{2.25f, -2.25f, 0.5f, 3.0f}))
          == std::array{0.25f, -0.25f, 0.5f, 0.0f});
    CHECK(lanesOf(lookup<float4(float4)>(*jit, "clip")(float4{-3.0f, -0.5f, 0.5f, 3.0f}))
          == std::array{-1.0f, -0.5f, 0.5f, 1.0f});
    const float4 v = {0.1f, 0.2f, 0.3f, 0.4f};
    CHECK(lookup<float(float4)>(*jit, "sum")(v) == ((0.1f + 0.2f) + 0.3f) + 0.4f);   // in lane order, exactly
    CHECK(lookup<float(float4)>(*jit, "least")(float4{3.0f, std::nanf(""), -1.0f, 2.0f}) == -1.0f);   // NaN ignored
    CHECK(lookup<float(float4)>(*jit, "most")(float4{3.0f, std::nanf(""), -1.0f, 2.0f}) == 3.0f);
    CHECK(lanesOf(lookup<float4(float4)>(*jit, "rectify")(x)) == std::array{0.0f, 0.0f, 0.5f, 2.0f});
    CHECK(lanesOf(lookup<float4(float4)>(*jit, "halfwave")(x)) == std::array{-0.0f, -0.0f, 0.5f, 2.0f});
    const float4 angles = {0.0f, 0.5f, 1.0f, 2.0f};
    const float4 sines = lookup<float4(float4)>(*jit, "sines")(angles);
    for (int i = 0; i < 4; ++i)   // one call to the vector math library (recipe 8): within 3 ulp of sinf's
        CHECK_THAT(sines[i], Catch::Matchers::WithinULP(std::sin(angles[i]), 3));
    CHECK(lookup<float(float)>(*jit, "wide")(1.0f) == 72.0f);
}

TEST_CASE("vector errors are reported as clang reports them", "[codegen][simd]")
{
    CHECK(errorsOf("float4 f(float4 a, float8 b, float x)\n"
                   "{\n"
                   "    float s = a;\n"
                   "    x = a;\n"
                   "    a + b;\n"
                   "    a.q;\n"
                   "    (float2)(1, 2).z;\n"
                   "    x.y;\n"
                   "    a.xxy;\n"
                   "    a.xx = (float2)(1);\n"
                   "    (float4)(1, 2, 3);\n"
                   "    (float4)(1, 2, 3, 4, 5);\n"
                   "    reduce_add(x);\n"
                   "    if (a) x = 1;\n"
                   "    a && x;\n"
                   "    b > 0 ? a : a;\n"
                   "    return b;\n"
                   "}\n"
                   "float g(float x) { return x; }\n"
                   "float h(float4 v) { return g(v.x) + g(v); }\n"   // a scalar broadcasts; a vector never narrows
                   "float r(float4 v) { return v; }\n")
          == "test.fm:3:11: error: initializing 'float' with an expression of incompatible type 'float4'\n"
             "test.fm:4:7: error: assigning to 'float' from incompatible type 'float4'\n"
             "test.fm:5:7: error: cannot convert between vector values of different size ('float4' and 'float8')\n"
             "test.fm:6:6: error: illegal vector component name 'q'\n"
             "test.fm:7:19: error: vector component access exceeds type 'float2'\n"
             "test.fm:8:6: error: member reference base type 'float' is not a structure or union\n"
             "test.fm:9:6: error: 3 components have no type: FMinus has float2, float4 and float8\n"
             "test.fm:10:10: error: vector is not assignable (contains duplicate components)\n"
             "test.fm:11:5: error: too few elements in vector initialization (expected 4 elements, have 3)\n"
             "test.fm:12:5: error: excess elements in vector initializer\n"
             "test.fm:13:5: error: the argument of 'reduce_add' must be a float vector, not 'float'\n"
             "test.fm:14:9: error: statement requires expression of scalar type ('float4' invalid)\n"
             "test.fm:15:7: error: a logical operator needs scalar operands, not 'float4'\n"
             "test.fm:16:11: error: vector condition type 'bool8' and result type 'float4' do not have the same number "
             "of elements\n"
             "test.fm:17:5: error: returning 'float8' from a function with incompatible result type 'float4'\n"
             "test.fm:20:37: error: passing 'float4' to parameter of incompatible type 'float'\n"
             "test.fm:21:21: error: returning 'float4' from a function with incompatible result type 'float'\n");
}

TEST_CASE("float4 math calls the vector math library; other sizes call sinf for each lane", "[codegen][simd]")
{
    std::string errors;
    auto jit = compile("float4 sines(float4 x) { return sinf(x); }\n"
                       "float4 tanhs(float4 x) { return tanhf(x); }\n"
                       "float wide(float a)\n"
                       "{\n"
                       "    return reduce_add(sinf((float8)(a) * (float8)(1, 2, 3, 4, 5, 6, 7, 8)));\n"
                       "}\n",
                       &errors);
    INFO(errors);
    REQUIRE(jit);
    for (float x = -100.0f; x <= 100.0f; x += 0.37f)
    {
        const float4 v = {x, x * 0.5f, x * 0.25f, -x};
        const float4 s = lookup<float4(float4)>(*jit, "sines")(v);
        const float4 t = lookup<float4(float4)>(*jit, "tanhs")(v * 0.05f);
        for (int i = 0; i < 4; ++i)
        {
            CHECK_THAT(s[i], Catch::Matchers::WithinULP(std::sin(v[i]), 3));
            CHECK_THAT(t[i], Catch::Matchers::WithinULP(std::tanh(v[i] * 0.05f), 3));
        }
    }
    float exact = 0.0f;   // float8 has no vector form: eight calls to sinf, in lane order, so exactly this
    for (int i = 1; i <= 8; ++i)
        exact += std::sin(0.5f * static_cast<float>(i));
    CHECK(lookup<float(float)>(*jit, "wide")(0.5f) == exact);
}
