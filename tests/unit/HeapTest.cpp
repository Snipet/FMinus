// tests/unit/HeapTest.cpp: the heap (extending.md, recipe 11): void*, sizeof, malloc, calloc and free, and memory a
// C++ host and FMinus pass to each other, run through the JIT.
#include "Compile.h"

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <cstdint>
#include <cstdlib>
#include <string>

using namespace fminus::test;

TEST_CASE("malloc, calloc and free are C's, on memory aligned to 64 bytes and zeroed", "[codegen][heap]")
{
    std::string errors;
    auto jit = compile("float* ramp(int n)\n"
                       "{\n"
                       "    float* p = calloc(n, sizeof(float));\n"   // a void*, which converts to a float*
                       "    for (int i = 0; i < n; ++i)\n"
                       "        p[i] = i;\n"
                       "    return p;\n"
                       "}\n"
                       "float4* frames(int n) { return malloc(n * sizeof(float4)); }\n"   // zeroed too
                       "void release(void* p) { free(p); }\n"                               // any pointer, or null
                       "bool refused() { return !malloc(0) && !calloc(-1, 4) && !calloc(1073741824, 64); }\n"   // 64 GiB
                       "bool same(float* a, void* b) { return a == b; }\n",                // a void* compares
                       &errors);
    INFO(errors);
    REQUIRE(jit);
    float* p = lookup<float*(int)>(*jit, "ramp")(100);
    REQUIRE(p != nullptr);
    CHECK(reinterpret_cast<std::uintptr_t>(p) % 64 == 0);
    CHECK((p[0] == 0.0f && p[1] == 1.0f && p[99] == 99.0f));
    CHECK(lookup<bool(float*, void*)>(*jit, "same")(p, p));
    std::free(p);   // FMinus's memory is C's: the host may free it

    float4* f = lookup<float4*(int)>(*jit, "frames")(3);
    REQUIRE(f != nullptr);
    CHECK(reinterpret_cast<std::uintptr_t>(f) % 64 == 0);
    CHECK((lanesOf(f[0]) == std::array{0.0f, 0.0f, 0.0f, 0.0f} && lanesOf(f[2]) == std::array{0.0f, 0.0f, 0.0f, 0.0f}));
    auto release = lookup<void(void*)>(*jit, "release");
    release(f);
    release(nullptr);
    release(std::malloc(16));   // and FMinus may free the host's
    CHECK(lookup<bool()>(*jit, "refused")());
}

TEST_CASE("sizeof gives a type's size in bytes, an int", "[codegen][heap]")
{
    const char* source = "int sizes()\n"
                         "{\n"
                         "    return sizeof(float) + 10 * sizeof(int) + 100 * sizeof(bool) + 1000 * sizeof(float4)\n"
                         "           + 100000 * sizeof(const float*) + 1000000 * sizeof(float8);\n"
                         "}\n"
                         "int vector_bytes() { return sizeof(poly float); }\n";
    std::string errors;
    auto jit = compile(source, &errors, {.polyLanes = 4});
    INFO(errors);
    REQUIRE(jit);
    CHECK(lookup<int()>(*jit, "sizes")() == 4 + 40 + 100 + 16000 + 800000 + 32000000);
    CHECK(lookup<int()>(*jit, "vector_bytes")() == 16);
    auto wide = compile(source, &errors, {.polyLanes = 16});
    REQUIRE(wide);
    CHECK(lookup<int()>(*wide, "vector_bytes")() == 64);   // the target's width decides it
}

TEST_CASE("a kernel's state lives on the heap: allocated once, used every block, freed once", "[codegen][heap]")
{
    std::string errors;
    auto jit = compile("// A delay of `length` samples: the line and its write position, allocated off the audio thread.\n"
                       "float* line_create(int length) { return calloc(length, sizeof(float)); }\n"
                       "int* position_create() { return calloc(1, sizeof(int)); }\n"
                       "void delay(float* restrict line, int* restrict position, int length,\n"
                       "           const float* restrict in, float* restrict out, int n)\n"
                       "{\n"
                       "    int w = *position;\n"
                       "    for (int i = 0; i < n; ++i)\n"
                       "    {\n"
                       "        out[i] = line[w];\n"
                       "        line[w] = in[i];\n"
                       "        if (++w == length)\n"
                       "            w = 0;\n"
                       "    }\n"
                       "    *position = w;\n"
                       "}\n"
                       "void destroy(float* line, int* position) { free(line); free(position); }\n",
                       &errors);
    INFO(errors);
    REQUIRE(jit);
    constexpr int length = 3;
    float* line = lookup<float*(int)>(*jit, "line_create")(length);
    int* position = lookup<int*()>(*jit, "position_create")();
    auto delay = lookup<void(float*, int*, int, const float*, float*, int)>(*jit, "delay");
    const std::array<float, 8> in{1, 2, 3, 4, 5, 6, 7, 8};
    std::array<float, 8> out{};
    delay(line, position, length, in.data(), out.data(), 5);   // two blocks, of 5 and 3
    delay(line, position, length, in.data() + 5, out.data() + 5, 3);
    CHECK(out == std::array<float, 8>{0, 0, 0, 1, 2, 3, 4, 5});   // three samples late, from silence
    lookup<void(float*, int*)>(*jit, "destroy")(line, position);
}

TEST_CASE("heap errors are reported as clang reports them", "[codegen][heap]")
{
    CHECK(errorsOf("float malloc(float x) { return x; }\n"
                   "void f(void* v, const float* c)\n"
                   "{\n"
                   "    v[0];\n"
                   "    *v;\n"
                   "    v + 1;\n"
                   "    sizeof(void);\n"
                   "    free(c);\n"
                   "    malloc(1, 2);\n"
                   "    float* p = v;\n"
                   "    int* q = p;\n"
                   "}\n")
          == "test.fm:1:7: error: 'malloc' is a builtin and cannot be redefined\n"
             "test.fm:4:6: error: subscript of pointer to incomplete type 'void'\n"
             "test.fm:5:5: error: indirection not permitted on operand of type 'void *'\n"
             "test.fm:6:7: error: arithmetic on a pointer to void\n"
             "test.fm:7:5: error: invalid application of 'sizeof' to an incomplete type 'void'\n"
             "test.fm:8:5: error: passing 'const float *' to parameter of type 'void *' discards qualifiers\n"
             "test.fm:9:5: error: too many arguments to function call, expected 1, have 2\n"
             "test.fm:11:10: error: incompatible pointer types initializing 'int *' with an expression of type "
             "'float *'\n");
}
