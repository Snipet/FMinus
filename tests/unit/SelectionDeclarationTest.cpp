// tests/unit/SelectionDeclarationTest.cpp: C2y's if and switch with a declaration in their head (N3356, N3580),
// through the JIT: a declaration and a condition, or one declarator with an initialiser whose value is the condition;
// its names in scope to the end of the whole statement, the else branch included, shadowing an outer name, and gone
// after it; a handle as the declared variable; a defer in a branch run before the statement ends. Then clang's errors
// for what C2y refuses.
#include "Compile.h"

#include "fminus/Config.h"

#include <catch2/catch_test_macros.hpp>

#include <string>

using namespace fminus::test;

TEST_CASE("if and switch take C2y's declaration in their head, in scope for the whole statement",
          "[codegen][selection]")
{
    auto jit = compile("int twice(int x) { return 2 * x; }\n"
                       "int both(int x)\n"
                       "{\n"
                       "    if (int n = twice(x), m = n + 1; n > 4)\n"
                       "        return n + m;\n"
                       "    else if (const int k = n - m)\n"   // n and m in the else branch: -1, true
                       "        return 100 * k;\n"
                       "    return -1;\n"
                       "}\n"
                       "int value(int x)\n"
                       "{\n"
                       "    if (int n = x - 3)\n"
                       "        return n;\n"
                       "    else\n"
                       "        return 50 + n;\n"   // 0 here
                       "}\n"
                       "int shadow(int x)\n"
                       "{\n"
                       "    int n = 7;\n"
                       "    if (float n = x * 0.5f; n > 1.0f)\n"
                       "        x = (int)(n * 4.0f);\n"
                       "    return x + n;\n"   // the outer n again
                       "}\n"
                       "int choose(int x)\n"
                       "{\n"
                       "    switch (int k = x % 3; k)\n"
                       "    {\n"
                       "    case 0:\n"
                       "        return 10;\n"
                       "    case 1:\n"
                       "        return 10 + k;\n"
                       "    }\n"
                       "    switch (int k = x * 2)\n"
                       "    {\n"
                       "    default:\n"
                       "        return k;\n"
                       "    }\n"
                       "}\n"
                       "int ended(int* log)\n"
                       "{\n"
                       "    int n = 0;\n"
                       "    if (int k = 5)\n"
                       "    {\n"
                       "        defer log[n++] = k;\n"
                       "        log[n++] = 1;\n"
                       "    }\n"
                       "    log[n++] = 2;\n"
                       "    return n;\n"
                       "}\n"
                       "int plan(int n)\n"
                       "{\n"
                       "    if (fft_plan p = fft_create(n))\n"
                       "    {\n"
                       "        defer fft_destroy(p);\n"
                       "        return 1;\n"
                       "    }\n"
                       "    else\n"
                       "        return p ? 2 : 0;\n"
                       "}\n");
    REQUIRE(jit != nullptr);
    auto* both = lookup<int(int)>(*jit, "both");
    CHECK(both(3) == 13);     // n = 6, m = 7
    CHECK(both(1) == -100);   // n = 2, m = 3: the else's k = -1
    auto* value = lookup<int(int)>(*jit, "value");
    CHECK(value(5) == 2);
    CHECK(value(3) == 50);
    auto* shadow = lookup<int(int)>(*jit, "shadow");
    CHECK(shadow(4) == 15);   // the inner n = 2.0f: x = 8, plus the outer n
    CHECK(shadow(1) == 8);
    auto* choose = lookup<int(int)>(*jit, "choose");
    CHECK(choose(3) == 10);
    CHECK(choose(4) == 11);
    CHECK(choose(5) == 10);   // k = 2 matches no case: the second switch's k = 10
    int log[4] = {};
    CHECK(lookup<int(int*)>(*jit, "ended")(log) == 3);
    CHECK(log[0] == 1);
    CHECK(log[1] == 5);
    CHECK(log[2] == 2);
    CHECK(lookup<int(int)>(*jit, "plan")(32) == 1);
    CHECK(lookup<int(int)>(*jit, "plan")(7) == 0);   // no plan of 7 points: null
}

TEST_CASE("an if's or a switch's declaration ends with it, and C2y's refusals are clang's errors",
          "[codegen][selection]")
{
    CHECK(errorsOf("int f(int x)\n"
                   "{\n"
                   "    if (int n = x; n > 0)\n"
                   "        x = n;\n"
                   "    switch (int k = x)\n"
                   "    {\n"
                   "    default:\n"
                   "        break;\n"
                   "    }\n"
                   "    return n + k;\n"
                   "}\n"
                   "int g(int x) { if (int n = x) { int n = 2; return n; } return 0; }\n")   // a block of its own
          == "test.fm:10:12: error: use of undeclared identifier 'n'\n"
             "test.fm:10:16: error: use of undeclared identifier 'k'\n");
}
