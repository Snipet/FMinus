// tests/unit/EffectTest.cpp: [[clang::nonallocating]] and [[clang::nonblocking]] (extending.md, recipe 11): a function
// marked with either may not reach malloc, calloc or free, nor (nonblocking) an external function that does not say
// it is nonblocking; FMinus checks it through every call, as clang's -Wfunction-effects does, and reports in its words.
#include "Compile.h"

#include <catch2/catch_test_macros.hpp>

#include <string>

using namespace fminus::test;

TEST_CASE("a nonblocking kernel may call math builtins, marked prototypes, and functions that do not allocate",
          "[codegen][effects]")
{
    std::string errors;
    auto jit = compile("float tanh_ok(float x) [[clang::nonblocking]];\n"     // marked: trusted
                       "float soft(float x) { return x / (1 + fabsf(x)); }\n"  // unmarked: its body decides
                       "float twice(float x, int n) { return n > 0 ? twice(x, n - 1) * 2 : soft(x); }\n"   // recursion
                       "void process(const float* restrict in, float* restrict out, int n) [[clang::nonblocking]]\n"
                       "{\n"
                       "    for (int i = 0; i < n; ++i)\n"
                       "        out[i] = twice(sinf(in[i]), 2);\n"
                       "}\n"
                       "float* create(int n) { return calloc(n, sizeof(float)); }\n"   // unmarked: may allocate
                       "float tanh_ok(float x) { return x; }\n",
                       &errors);
    INFO(errors);
    REQUIRE(jit);
    const float in[2] = {0.0f, 0.5f};
    float out[2] = {1.0f, 1.0f};
    lookup<void(const float*, float*, int)>(*jit, "process")(in, out, 2);
    CHECK(out[0] == 0.0f);
    CHECK(out[1] > 0.0f);
}

TEST_CASE("effect errors name the call, and notes follow it down, as clang's do", "[codegen][effects]")
{
    CHECK(errorsOf("float ext(float x);\n"
                   "float ext_ok(float x) [[clang::nonallocating]];\n"
                   "float* grow(int n) { return calloc(n, sizeof(float)); }\n"
                   "float helper(float x) { grow(4); return x; }\n"
                   "void a(float* p) [[clang::nonallocating]]\n"
                   "{\n"
                   "    p[0] = helper(p[1]) + ext(p[2]) + ext_ok(p[3]);\n"
                   "    free(p);\n"
                   "}\n"
                   "void b(float* p) [[clang::nonblocking]] { p[0] = ext_ok(p[1]); }\n"
                   "void c() [[clang::fast]] { }\n")
          == "test.fm:7:12: error: function with 'nonallocating' attribute must not call non-'nonallocating' function "
             "'helper'\n"
             "test.fm:4:25: note: function cannot be inferred 'nonallocating' because it calls non-'nonallocating' "
             "function 'grow'\n"
             "test.fm:3:29: note: function cannot be inferred 'nonallocating' because it calls non-'nonallocating' "
             "function 'calloc'\n"
             "test.fm:7:27: error: function with 'nonallocating' attribute must not call non-'nonallocating' function "
             "'ext'\n"
             "test.fm:1:7: note: declaration cannot be inferred 'nonallocating' because it has no definition in this "
             "program\n"
             "test.fm:8:5: error: function with 'nonallocating' attribute must not call non-'nonallocating' function "
             "'free'\n"
             "test.fm:10:50: error: function with 'nonblocking' attribute must not call non-'nonblocking' function "
             "'ext_ok'\n"
             "test.fm:2:7: note: declaration cannot be inferred 'nonblocking' because it has no definition in this "
             "program\n"
             "test.fm:11:12: error: unknown attribute 'clang::fast': FMinus has clang::nonallocating and "
             "clang::nonblocking\n");
}
