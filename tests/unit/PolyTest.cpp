// tests/unit/PolyTest.cpp: poly float (simd.md, recipe 9): one source at several widths, a poly float at the C
// boundary, and the errors that keep a program's meaning the same at every width.
#include "Compile.h"

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include <algorithm>
#include <array>
#include <limits>
#include <string>
#include <vector>

using namespace fminus::test;

namespace
{
    // basel(n) as a poly float of `lanes` lanes computes it: lane i sums the terms k = i + 1, i + 1 + lanes, ... in
    // order, then the lanes are added in order, from -0.
    float baselIn(unsigned lanes, int n)
    {
        std::vector<float> sums(lanes, 0.0f);
        for (int base = 1; base <= n; base += static_cast<int>(lanes))
        {
            for (unsigned i = 0; i < lanes; ++i)
            {
                const auto k = static_cast<float>(base + static_cast<int>(i));
                sums[i] += k <= static_cast<float>(n) ? 1 / (k * k) : 0.0f;
            }
        }
        float total = -0.0f;
        for (float sum : sums)
            total += sum;
        return total;
    }
} // namespace

TEST_CASE("a poly float has as many lanes as the target says, and means the same at any width", "[codegen][poly]")
{
    const char* source = "float lanes() { return poly_lanes(); }\n"
                         "float index_sum() { return reduce_add(poly_index()); }\n"
                         "float positive(float a) { poly bool up = poly_index() < a; return reduce_add(up ? 1 : 0); }\n"
                         "float all() { poly bool yes = true; return reduce_min((poly float)(2) * yes); }\n"
                         "float basel(float n)\n"
                         "{\n"
                         "    poly float sum = 0;\n"
                         "    for (float base = 1; base <= n; base += poly_lanes())\n"
                         "    {\n"
                         "        const poly float k = base + poly_index();\n"
                         "        sum += k <= n ? 1 / (k * k) : 0;\n"
                         "    }\n"
                         "    return reduce_add(sum);\n"
                         "}\n"
                         "float peak(float n)\n"   // a maximum does not depend on the order: the same at any width
                         "{\n"
                         "    poly float best = -1;\n"
                         "    for (float base = 0; base < n; base += poly_lanes())\n"
                         "    {\n"
                         "        const poly float k = base + poly_index();\n"
                         "        best = fmaxf(best, k < n ? k * (n - k) / (n + 0.5f) : -1);\n"
                         "    }\n"
                         "    return reduce_max(best);\n"
                         "}\n";
    float onePeak = 0.0f;
    for (const unsigned lanes : {1u, 2u, 4u, 8u, 16u, 64u})
    {
        INFO("poly float lanes: " << lanes);
        std::string errors;
        auto jit = compile(source, &errors, {.polyLanes = lanes});
        INFO(errors);
        REQUIRE(jit);
        const auto n = static_cast<float>(lanes);
        CHECK(lookup<float()>(*jit, "lanes")() == n);
        CHECK(lookup<float()>(*jit, "index_sum")() == n * (n - 1) / 2);
        CHECK(lookup<float(float)>(*jit, "positive")(2.5f) == std::min(n, 3.0f));
        CHECK(lookup<float()>(*jit, "all")() == 2.0f);
        CHECK(lookup<float(float)>(*jit, "basel")(1000.0f) == baselIn(lanes, 1000));   // its own order
        CHECK_THAT(lookup<float(float)>(*jit, "basel")(1000.0f), Catch::Matchers::WithinRel(1.6439345f, 1e-6f));
        const float peak = lookup<float(float)>(*jit, "peak")(501.0f);
        if (lanes == 1)
            onePeak = peak;
        CHECK(peak == onePeak);
        CHECK(peak == 250.0f * 251.0f / 501.5f);
    }
}

TEST_CASE("at the C boundary a poly float is the target's vector: a float4 on NEON", "[codegen][poly]")
{
    std::string errors;
    auto jit = compile("poly float twice(poly float x) { return x * 2; }\n", &errors, {.polyLanes = 4});
    INFO(errors);
    REQUIRE(jit);
    CHECK(lanesOf(lookup<float4(float4)>(*jit, "twice")(float4{1.0f, 2.0f, 3.0f, 4.0f}))
          == std::array{2.0f, 4.0f, 6.0f, 8.0f});
}

TEST_CASE("a poly float never mixes with a fixed-size vector, even with as many lanes", "[codegen][poly]")
{
    std::string errors;
    // At 4 lanes a poly float is <4 x float>, as a float4 is; the errors must not depend on that.
    CHECK(compile("float4 f(poly float p, float4 v)\n"
                  "{\n"
                  "    p + v;\n"
                  "    v = p;\n"
                  "    p.x;\n"
                  "    (poly float)(1, 2);\n"
                  "    (float4)(p);\n"
                  "    p > 0 ? v : v;\n"
                  "    if (p) p = 1;\n"
                  "    p && 1;\n"
                  "    poly_lanes(1);\n"
                  "    return p;\n"
                  "}\n"
                  "float poly_index() { return 0; }\n",
                  &errors, {.polyLanes = 4})
          == nullptr);
    CHECK(errors
          == "test.fm:3:7: error: cannot convert between 'poly float' and 'float4': a poly type has as many lanes as "
             "the target's vectors\n"
             "test.fm:4:7: error: assigning to 'float4' from incompatible type 'poly float'\n"
             "test.fm:5:6: error: a poly float has no components: its lanes are the target's\n"
             "test.fm:6:5: error: a poly float literal takes one scalar, which it broadcasts\n"
             "test.fm:7:5: error: cannot convert between 'poly float' and 'float4': a poly type has as many lanes as "
             "the target's vectors\n"
             "test.fm:8:11: error: cannot convert between 'poly bool' and 'float4': a poly type has as many lanes as "
             "the target's vectors\n"
             "test.fm:9:9: error: statement requires expression of scalar type ('poly float' invalid)\n"
             "test.fm:10:7: error: a logical operator needs scalar operands, not 'poly float'\n"
             "test.fm:11:5: error: too many arguments to function call, expected 0, have 1\n"
             "test.fm:12:5: error: returning 'poly float' from a function with incompatible result type 'float4'\n"
             "test.fm:14:7: error: 'poly_index' is a builtin and cannot be redefined\n");
}

TEST_CASE("poly_load and poly_store move poly floats through a buffer, and a mask keeps the last step inside it",
          "[codegen][poly]")
{
    // [[clang::nonblocking]]: neither builtin allocates, so an audio thread's kernel may use them.
    const char* source = "void scale(const float* restrict input, float* restrict output, int n, float g)\n"
                         "    [[clang::nonblocking]]\n"
                         "{\n"
                         "    int i = 0;\n"
                         "    for (; i + poly_lanes() <= n; i += poly_lanes())\n"
                         "        poly_store(output + i, g * poly_load(input + i));\n"
                         "    const poly bool rest = i + poly_index() < n;\n"   // the lanes still in the buffer
                         "    poly_store(output + i, g * poly_load(input + i, rest), rest);\n"
                         "}\n"
                         "float first(const float* p, int k) { return reduce_add(poly_load(p, poly_index() < k)); }\n"
                         "void fill(float* p, float x) { poly_store(p, x); }\n";   // a scalar, broadcast
    for (const unsigned lanes : {1u, 2u, 4u, 8u, 16u, 64u})
    {
        INFO("poly float lanes: " << lanes);
        std::string errors;
        auto jit = compile(source, &errors, {.polyLanes = lanes});
        INFO(errors);
        REQUIRE(jit);
        auto scale = lookup<void(const float*, float*, int, float)>(*jit, "scale");
        const int width = static_cast<int>(lanes);
        for (const int n : {0, 1, width - 1, width, width + 1, 3 * width + 2, 100})
        {
            INFO("n = " << n);
            std::vector<float> input(static_cast<std::size_t>(n) + 64);
            for (std::size_t i = 0; i < input.size(); ++i)
                input[i] = static_cast<float>(i) + 0.5f;
            std::vector<float> output(input.size(), -7.0f);   // -7 past n: what the masked store must not overwrite
            scale(input.data(), output.data(), n, 2.0f);
            bool scaled = true;
            bool untouched = true;
            for (std::size_t i = 0; i < output.size(); ++i)
            {
                if (static_cast<int>(i) < n)
                    scaled = scaled && output[i] == 2.0f * input[i];
                else
                    untouched = untouched && output[i] == -7.0f;
            }
            CHECK(scaled);
            CHECK(untouched);
        }

        // A masked load reads only its lanes: the NaNs after the first k never reach the sum.
        std::vector<float> values(lanes, std::numeric_limits<float>::quiet_NaN());
        const int k = std::max(1, width / 2);
        for (int i = 0; i < k; ++i)
            values[static_cast<std::size_t>(i)] = static_cast<float>(i + 1);
        CHECK(lookup<float(const float*, int)>(*jit, "first")(values.data(), k) == static_cast<float>(k * (k + 1) / 2));

        std::vector<float> filled(lanes + 1, 0.0f);
        lookup<void(float*, float)>(*jit, "fill")(filled.data(), 3.0f);
        CHECK(std::count(filled.begin(), filled.end(), 3.0f) == width);
        CHECK(filled.back() == 0.0f);
    }
}

TEST_CASE("poly_load reads a const float*, poly_store writes a float*, and a mask is a poly bool", "[codegen][poly]")
{
    CHECK(errorsOf("void a(int* p) { poly float x = poly_load(p); }\n"
                   "void b(const float* p) { poly_store(p, 1); }\n"
                   "void c(float* p, float4 v) { poly_store(p, v); }\n"
                   "void d(float* p) { poly float x = poly_load(); }\n"
                   "void e(float* p, poly bool m) { poly float x = poly_load(p, m, m); }\n"
                   "void f(float* p) { float x = poly_load(p); }\n"
                   "void g(float* p) { float x = poly_store(p, 0); }\n"
                   "void h(float4* p) { poly_store(p, 0, poly_index() < 2); }\n"
                   "void poly_store(float* p) { }\n")
          == "test.fm:1:33: error: incompatible pointer types passing 'int *' to parameter of type 'const float *'\n"
             "test.fm:2:26: error: passing 'const float *' to parameter of type 'float *' discards qualifiers\n"
             "test.fm:3:30: error: passing 'float4' to parameter of incompatible type 'poly float'\n"
             "test.fm:4:35: error: too few arguments to function call, expected 1, have 0\n"
             "test.fm:5:48: error: too many arguments to function call, expected 2, have 3\n"
             "test.fm:6:26: error: initializing 'float' with an expression of incompatible type 'poly float'\n"
             "test.fm:7:26: error: initializing 'float' with an expression of incompatible type 'void'\n"
             "test.fm:8:21: error: incompatible pointer types passing 'float4 *' to parameter of type 'float *'\n"
             "test.fm:9:6: error: 'poly_store' is a builtin and cannot be redefined\n");
}
