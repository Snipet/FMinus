// tests/unit/PointerTest.cpp: pointers (extending.md, recipe 10): buffers a C++ host passes, p[i], *p, &x, pointer
// arithmetic, const and restrict, and the errors, run through the JIT.
#include "Compile.h"

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <string>

using namespace fminus::test;

TEST_CASE("a function processes a C++ host's buffers through pointers", "[codegen][pointer]")
{
    std::string errors;
    auto jit = compile("void gain(const float* restrict in, float* restrict out, int n, float g)\n"
                       "{\n"
                       "    for (int i = 0; i < n; ++i)\n"
                       "        out[i] = in[i] * g;\n"
                       "}\n"
                       // state the host owns: read at the start of a block, written back at its end
                       "void smooth(const float* in, float* out, int n, float k, float* state)\n"
                       "{\n"
                       "    float y = *state;\n"
                       "    for (int i = 0; i < n; ++i)\n"
                       "        out[i] = y += k * (in[i] - y);\n"
                       "    *state = y;\n"
                       "}\n"
                       "int positives(const int* p, int n) { int c = 0; while (n--) c += *p++ > 0; return c; }\n"
                       "void scale(float4* frames, int n) { for (int i = 0; i < n; ++i) frames[i].yz *= 2; }\n",
                       &errors);
    INFO(errors);
    REQUIRE(jit);
    const std::array<float, 4> in{1.0f, -2.0f, 3.0f, 0.5f};
    std::array<float, 4> out{};
    lookup<void(const float*, float*, int, float)>(*jit, "gain")(in.data(), out.data(), 4, 0.5f);
    CHECK(out == std::array{0.5f, -1.0f, 1.5f, 0.25f});

    // Two blocks of two give what one block of four does: the state carries over.
    auto smooth = lookup<void(const float*, float*, int, float, float*)>(*jit, "smooth");
    float state = 0.0f;
    std::array<float, 4> whole{};
    smooth(in.data(), whole.data(), 4, 0.5f, &state);
    float split = 0.0f;
    std::array<float, 4> halves{};
    smooth(in.data(), halves.data(), 2, 0.5f, &split);
    smooth(in.data() + 2, halves.data() + 2, 2, 0.5f, &split);
    CHECK(halves == whole);
    CHECK(split == state);
    CHECK(whole == std::array{0.5f, -0.75f, 1.125f, 0.8125f});

    const std::array<int, 5> numbers{3, -1, 0, 7, 2};
    CHECK(lookup<int(const int*, int)>(*jit, "positives")(numbers.data(), 5) == 3);

    std::array<float4, 2> frames{float4{1.0f, 2.0f, 3.0f, 4.0f}, float4{5.0f, 6.0f, 7.0f, 8.0f}};
    lookup<void(float4*, int)>(*jit, "scale")(frames.data(), 2);
    CHECK(lanesOf(frames[0]) == std::array{1.0f, 4.0f, 6.0f, 4.0f});
    CHECK(lanesOf(frames[1]) == std::array{5.0f, 12.0f, 14.0f, 8.0f});
}

TEST_CASE("pointers are C's: indexing, arithmetic, comparisons, and addresses", "[codegen][pointer]")
{
    std::string errors;
    auto jit = compile("float sum(const float* p, int n)\n"
                       "{\n"
                       "    float s = 0;\n"
                       "    for (const float* end = p + n; p < end; ++p)\n"   // a pointer for a counter
                       "        s += *p;\n"
                       "    return s;\n"
                       "}\n"
                       "int span(const float* a, const float* b) { return b - a; }\n"   // elements, not bytes
                       "float around(const float* p) { return p[-1] + *(p + 1) + 1[p]; }\n"   // C's i[p] is p[i]
                       "void swap(float* a, float* b) { const float t = *a; *a = *b; *b = t; }\n"
                       "float local(float x)\n"
                       "{\n"
                       "    float y = 10;\n"
                       "    float* p = &x;\n"   // a variable's address: the variable, through a pointer
                       "    *p += 1;\n"
                       "    swap(p, &y);\n"
                       "    return x * 100 + y;\n"
                       "}\n"
                       "bool none(const float* p) { return !p; }\n"   // a pointer is true when not null
                       "bool null() { float* p; return none(p); }\n", // an uninitialised pointer is null
                       &errors);
    INFO(errors);
    REQUIRE(jit);
    const std::array<float, 5> v{1.0f, 2.0f, 3.0f, 4.0f, 5.0f};
    CHECK(lookup<float(const float*, int)>(*jit, "sum")(v.data(), 5) == 15.0f);
    CHECK(lookup<int(const float*, const float*)>(*jit, "span")(v.data() + 1, v.data() + 4) == 3);
    CHECK(lookup<float(const float*)>(*jit, "around")(v.data() + 2) == 2.0f + 4.0f + 4.0f);
    CHECK(lookup<float(float)>(*jit, "local")(2.0f) == 1003.0f);   // x = 3 and y = 10, swapped
    CHECK(lookup<bool(const float*)>(*jit, "none")(nullptr));
    CHECK_FALSE(lookup<bool(const float*)>(*jit, "none")(v.data()));
    CHECK(lookup<bool()>(*jit, "null")());
}

TEST_CASE("pointer errors are reported as clang reports them", "[codegen][pointer]")
{
    CHECK(errorsOf("float f(float* p, const float* c, int* q, float x, float4 v)\n"
                   "{\n"
                   "    c[0] = 1;\n"
                   "    p = c;\n"
                   "    p = q;\n"
                   "    x[0];\n"
                   "    p[x];\n"
                   "    *x;\n"
                   "    &(x + 1);\n"
                   "    &v.x;\n"
                   "    &p;\n"
                   "    p + p;\n"
                   "    p - q;\n"
                   "    p < q;\n"
                   "    p * 2;\n"
                   "    -p;\n"
                   "    float* const k = p;\n"
                   "    k = p;\n"
                   "    bool* b;\n"
                   "    x > 0 ? p : q;\n"
                   "    return p;\n"
                   "}\n")
          == "test.fm:3:10: error: read-only variable is not assignable\n"
             "test.fm:4:7: error: assigning to 'float *' from 'const float *' discards qualifiers\n"
             "test.fm:5:7: error: incompatible pointer types assigning to 'float *' from 'int *'\n"
             "test.fm:6:6: error: subscripted value is not a pointer\n"
             "test.fm:7:6: error: array subscript is not an integer\n"
             "test.fm:8:5: error: indirection requires pointer operand ('float' invalid)\n"
             "test.fm:9:5: error: cannot take the address of an rvalue of type 'float'\n"
             "test.fm:10:5: error: address of vector element requested\n"
             "test.fm:11:5: error: FMinus has pointers to float, int, float2, float4 and float8, not to 'float *'\n"
             "test.fm:12:7: error: invalid operands to binary expression ('float *' and 'float *')\n"
             "test.fm:13:7: error: 'float *' and 'int *' are not pointers to compatible types\n"
             "test.fm:14:7: error: comparison of distinct pointer types ('float *' and 'int *')\n"
             "test.fm:15:7: error: invalid operands to binary expression ('float *' and 'int')\n"
             "test.fm:16:5: error: invalid argument type 'float *' to unary expression\n"
             "test.fm:18:7: error: cannot assign to variable 'k' with const-qualified type 'float *const'\n"
             "test.fm:19:11: error: FMinus has pointers to float, int, float2, float4 and float8, not to 'bool'\n"
             "test.fm:20:11: error: incompatible operand types ('float *' and 'int *')\n"
             "test.fm:21:5: error: returning 'float *' from a function with incompatible result type 'float'\n");
}
