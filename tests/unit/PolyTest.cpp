// tests/unit/PolyTest.cpp: poly float (simd.md, recipe 9): one source at several widths, a poly float at the C
// boundary, and the errors that keep a program's meaning the same at every width.
#include "Compile.h"

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include <algorithm>
#include <array>
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
