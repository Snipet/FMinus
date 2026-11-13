// tests/unit/TableTest.cpp: const arrays of constants, stored once (extending.md, recipe 26; src/ir/Memory.cpp,
// declareConstArray): each reads what the same array on the stack would hold, bit for bit (every element type and
// shape of list, subnormals preserved or flushed), wherever it is read (a loop, a pointer to it, a method, a knob's
// copies, a parallel for, nested calls); a const array whose list is not all constants is a local, its list
// computed once, in order; and the errors are an array's. The IR, a constant global and no stores, is
// tests/lit/fminus/tables.fm's.
#include "Compile.h"

#include <catch2/catch_test_macros.hpp>

#include <bit>
#include <cstdint>
#include <string>

using namespace fminus::test;

namespace
{
    // Each table twice: as a const array (a constant of the module) and as a plain one (the stack's), which must read
    // alike. `$` is replaced by "const " and by "".
    constexpr std::string_view kTables =
        "float floats$(int i)\n"
        "{\n"
        "    $float t[16] = {1, 2.5f, -0.0f, 0x1p-3f, 1e-40f, -1e-40f, 3.4e38f, 1 << 3,\n"
        "                    (float)(7 / 2), sizeof(float4), -(1.5f * 2), 0.0f / 0.0f, -2147483647 - 1};\n"
        "    return t[i];\n"
        "}\n"
        "int ints$(int i)\n"
        "{\n"
        "    $int k[20] = {1, -1, 1 << 31, ~0, 0x7FFFFFFF, hash(3), (int)2.9f, (int)-2.9f, true, 010, 0, 5};\n"
        "    return k[i];\n"
        "}\n"
        "float4 vectors$(int i)\n"
        "{\n"
        "    $float4 v[4] = {(float4)(1, 2, 3, 4), (float4){5}, (float4)(-0.0f)};\n"
        "    return v[i];\n"
        "}\n"
        "float zeros$(int i) { $float z[64]; return z[i]; }\n"
        "float nearlyFull$(int i) { $float n[10] = {9, 8, 7, 6, 5, 4, 3, 2, 1}; return n[i]; }\n"
        "float big$(int i) { $float b[100000] = {1, 2, 3}; return b[i]; }\n";

    std::string withConst(bool isConst)
    {
        std::string source(kTables);
        for (std::size_t at = source.find('$'); at != std::string::npos; at = source.find('$', at))
        {
            const bool type = at > 0 && source[at - 1] == ' ';   // "$float t": the qualifier; "floats$(": a suffix
            source.replace(at, 1, type ? (isConst ? "const " : "") : (isConst ? "Table" : "Stack"));
        }
        return source;
    }

    std::uint32_t bits(float x)
    {
        return std::bit_cast<std::uint32_t>(x);
    }
} // namespace

TEST_CASE("a const array of constants reads as the same array on the stack, bit for bit", "[codegen][array][table]")
{
    for (const fminus::ir::Denormals denormals : {fminus::ir::Denormals::Preserve, fminus::ir::Denormals::Flush})
    {
        INFO((denormals == fminus::ir::Denormals::Flush ? "flushed" : "preserved"));
        fminus::ir::Options options;
        options.denormals = denormals;
        std::string errors;
        auto jit = compile(withConst(true) + withConst(false), &errors, options);
        INFO(errors);
        REQUIRE(jit);
        for (int i = 0; i < 16; ++i)
        {
            INFO("element " << i);
            const float table = lookup<float(int)>(*jit, "floatsTable")(i);
            CHECK(bits(table) == bits(lookup<float(int)>(*jit, "floatsStack")(i)));
        }
        for (int i = 0; i < 20; ++i)
            CHECK(lookup<int(int)>(*jit, "intsTable")(i) == lookup<int(int)>(*jit, "intsStack")(i));
        for (int i = 0; i < 4; ++i)
        {
            const auto table = lanesOf(lookup<float4(int)>(*jit, "vectorsTable")(i));
            const auto stack = lanesOf(lookup<float4(int)>(*jit, "vectorsStack")(i));
            for (std::size_t lane = 0; lane < 4; ++lane)
                CHECK(bits(table[lane]) == bits(stack[lane]));
        }
        for (const int i : {0, 31, 63})
            CHECK(bits(lookup<float(int)>(*jit, "zerosTable")(i)) == 0u);
        for (int i = 0; i < 10; ++i)
            CHECK(lookup<float(int)>(*jit, "nearlyFullTable")(i) == lookup<float(int)>(*jit, "nearlyFullStack")(i));
        for (const int i : {0, 1, 2, 3, 50000, 99999})
            CHECK(lookup<float(int)>(*jit, "bigTable")(i) == lookup<float(int)>(*jit, "bigStack")(i));
    }
    // What the stack's array holds, to be sure the comparison compares something.
    std::string errors;
    auto jit = compile(withConst(true), &errors);
    REQUIRE(jit);
    auto floats = lookup<float(int)>(*jit, "floatsTable");
    CHECK(floats(1) == 2.5f);
    CHECK(bits(floats(2)) == 0x80000000u);
    CHECK(floats(7) == 8.0f);
    CHECK(floats(8) == 3.0f);
    CHECK(floats(9) == 16.0f);
    CHECK(floats(11) != floats(11));   // NaN
    CHECK(floats(12) == -2147483648.0f);
    CHECK(floats(15) == 0.0f);
    auto ints = lookup<int(int)>(*jit, "intsTable");
    CHECK(ints(2) == static_cast<int>(0x80000000u));
    CHECK(ints(5) == 1408362973);   // hash(3)
    CHECK(ints(7) == -2);
    CHECK(ints(9) == 8);
    CHECK(ints(11) == 5);
    CHECK(ints(19) == 0);
}

TEST_CASE("a table reads the same wherever it is read: loops, pointers, methods, a knob's copies, parallel for",
          "[codegen][array][table]")
{
    std::string errors;
    auto jit = compile("float sum(const float* p, int n)\n"
                       "{\n"
                       "    float s = 0;\n"
                       "    for (int i = 0; i < n; ++i)\n"
                       "        s += p[i];\n"
                       "    return s;\n"
                       "}\n"
                       "float weights()\n"
                       "{\n"
                       "    const float w[5] = {0.5f, 0.25f, 0.125f, 0.0625f, 0.03125f};\n"
                       "    float s = 0;\n"
                       "    for (int i = 0; i < 5; ++i)\n"
                       "        s += w[i] * (i + 1);\n"
                       "    return s + sum(w, 5);\n"   // its name is a const float* to it
                       "}\n"
                       "int depth(int n)\n"   // nested calls share the table, and read it alike
                       "{\n"
                       "    const int t[2] = {10, 20};\n"
                       "    return n == 0 ? t[1] : depth(n - 1) + t[0];\n"
                       "}\n"
                       "int shadows(int i)\n"   // two tables of one name, in two scopes
                       "{\n"
                       "    const int t[2] = {1, 2};\n"
                       "    int s = t[i];\n"
                       "    {\n"
                       "        const int t[2] = {30, 40};\n"
                       "        s += t[i];\n"
                       "    }\n"
                       "    return s + t[i];\n"
                       "}\n"
                       "void spread(float* restrict out, int n)\n"
                       "{\n"
                       "    const float gains[4] = {1, -1, 0.5f, -0.5f};\n"
                       "    parallel for (int i = 0; i < n; ++i)\n"
                       "        out[i] = gains[i & 3] * i;\n"
                       "}\n"
                       "module Shaper\n"
                       "{\n"
                       "    param bool soft [[fminus::default(1)]];\n"
                       "    void run(float* restrict out, int n)\n"
                       "    {\n"
                       "        const float curve[4] = {0, 0.25f, 0.75f, 1};\n"
                       "        for (int i = 0; i < n; ++i)\n"
                       "            out[i] = soft[i] ? curve[i & 3] : curve[3 - (i & 3)];\n"
                       "    }\n"
                       "};\n",
                       &errors);
    INFO(errors);
    REQUIRE(jit);
    CHECK(lookup<float()>(*jit, "weights")() == 0.5f + 0.5f + 0.375f + 0.25f + 0.15625f + 0.96875f);
    CHECK(lookup<int(int)>(*jit, "depth")(3) == 50);
    CHECK(lookup<int(int)>(*jit, "shadows")(0) == 32);
    CHECK(lookup<int(int)>(*jit, "shadows")(1) == 44);
    std::vector<float> out(103);
    lookup<void(float*, int)>(*jit, "spread")(out.data(), 103);
    const std::array gains{1.0f, -1.0f, 0.5f, -0.5f};
    for (int i = 0; i < 103; ++i)
        CHECK(out[static_cast<std::size_t>(i)] == gains[static_cast<std::size_t>(i & 3)] * static_cast<float>(i));
    // Shaper's slot for `soft`: { const float* stream; bool value; }, 16 bytes, its knob's value at 8.
    struct alignas(8) Shaper
    {
        const float* stream = nullptr;
        bool value = false;
    };
    const std::array curve{0.0f, 0.25f, 0.75f, 1.0f};
    auto run = lookup<void(void*, float*, int)>(*jit, "Shaper::run");
    for (const bool soft : {false, true})
    {
        Shaper shaper;
        shaper.value = soft;
        std::vector<float> shaped(37);
        run(&shaper, shaped.data(), 37);
        for (int i = 0; i < 37; ++i)
            CHECK(shaped[static_cast<std::size_t>(i)] == curve[static_cast<std::size_t>(soft ? i & 3 : 3 - (i & 3))]);
    }
}

TEST_CASE("a const array whose list is not all constants is a local, its list computed once, in order",
          "[codegen][array][table]")
{
    std::string errors;
    auto jit = compile("float mixed(float x, int i) { const float m[3] = {x, 1}; return m[i]; }\n"
                       "int order(int* c) { const int k[3] = {(*c)++, (*c)++ * 10, 7}; return k[0] + k[1] + k[2]; }\n"
                       "float chosen(int i) { const float c[2] = {1 > 0 ? 2 : 3, 4}; return c[i]; }\n",   // ?: branches
                       &errors);
    INFO(errors);
    REQUIRE(jit);
    CHECK(lookup<float(float, int)>(*jit, "mixed")(3.5f, 0) == 3.5f);
    CHECK(lookup<float(float, int)>(*jit, "mixed")(3.5f, 1) == 1.0f);
    CHECK(lookup<float(float, int)>(*jit, "mixed")(3.5f, 2) == 0.0f);
    int count = 0;
    CHECK(lookup<int(int*)>(*jit, "order")(&count) == 17);   // 0, then 1 * 10, then 7
    CHECK(count == 2);
    CHECK(lookup<float(int)>(*jit, "chosen")(0) == 2.0f);
    CHECK(lookup<float(int)>(*jit, "chosen")(1) == 4.0f);

    // The errors are an array's, a const one's.
    CHECK(errorsOf("float f(float x, int i)\n"
                   "{\n"
                   "    const float a[2] = {1, 2, 3};\n"
                   "    const float b[2] = x;\n"
                   "    const float c[2] = {1, 2};\n"
                   "    c[i] = 3;\n"
                   "    float* p = c;\n"
                   "    c = c;\n"
                   "    const bool d[2] = {true};\n"
                   "    return a[0] + b[0] + c[i];\n"
                   "}\n")
          == "test.fm:3:31: error: excess elements in array initializer\n"
             "test.fm:4:17: error: array initializer must be an initializer list\n"
             "test.fm:6:10: error: cannot assign to variable 'c' with const-qualified type 'const float[2]'\n"
             "test.fm:7:12: error: initializing 'float *' with an expression of type 'const float *' discards "
             "qualifiers\n"
             "test.fm:8:7: error: array type 'float[2]' is not assignable\n"
             "test.fm:9:16: error: FMinus has arrays of float, double, int, float2, float4, float8, char, string, "
             "modules and handles, not of 'bool'\n");
}
