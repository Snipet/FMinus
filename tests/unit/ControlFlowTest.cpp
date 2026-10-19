// tests/unit/ControlFlowTest.cpp: bool, comparisons, !, && and ||, ?: (extending.md, recipe 3) and if, while, for,
// break and continue (recipe 4), run through the JIT.
#include "Compile.h"

#include <catch2/catch_test_macros.hpp>

#include <cmath>
#include <string>

using namespace fminus::test;

TEST_CASE("bool, comparisons, !, && and ||, and ?: follow C", "[codegen]")
{
    std::string errors;
    auto jit = compile("float magnitude(float x) { return x < 0 ? -x : x; }\n"
                       "float sign(float x) { return x > 0 ? 1 : x < 0 ? -1 : 0; }\n"   // ?: nests to the right
                       "bool positive(float x) { return x > 0; }\n"
                       "bool truthy(float x) { return x; }\n"                           // C: x != 0, so NaN is true
                       "float count(bool a, bool b) { return a + b + !a; }\n"           // C: a bool is 0 or 1
                       "float both(float x)\n"                                          // && may skip its right side
                       "{\n"
                       "    float ran = 0;\n"
                       "    x > 0 && (ran = 1);\n"
                       "    return ran;\n"
                       "}\n"
                       "float either(float x) { float ran = 0; x > 0 || (ran = 1); return ran; }\n"
                       "float pick(float x) { float t = 0; x > 0 ? (t = 1) : (t = 2); return t; }\n"   // one runs
                       "bool range(float x) { return x >= 0 && x <= 1 || x == 10 || !(x != 20); }\n",
                       &errors);
    INFO(errors);
    REQUIRE(jit);
    auto magnitude = lookup<float(float)>(*jit, "magnitude");
    auto sign = lookup<float(float)>(*jit, "sign");
    auto positive = lookup<bool(float)>(*jit, "positive");
    auto truthy = lookup<bool(float)>(*jit, "truthy");
    auto count = lookup<float(bool, bool)>(*jit, "count");
    auto both = lookup<float(float)>(*jit, "both");
    auto either = lookup<float(float)>(*jit, "either");
    auto pick = lookup<float(float)>(*jit, "pick");
    auto range = lookup<bool(float)>(*jit, "range");
    CHECK(magnitude(-2.5f) == 2.5f);
    CHECK(sign(-4.0f) == -1.0f);
    CHECK(sign(0.0f) == 0.0f);
    CHECK(sign(7.0f) == 1.0f);
    CHECK(positive(2.0f));
    CHECK_FALSE(positive(std::nanf("")));
    CHECK(truthy(std::nanf("")));
    CHECK_FALSE(truthy(0.0f));
    CHECK(count(true, true) == 2.0f);
    CHECK(count(false, false) == 1.0f);
    CHECK(both(1.0f) == 1.0f);
    CHECK(both(-1.0f) == 0.0f);
    CHECK(either(1.0f) == 0.0f);
    CHECK(either(-1.0f) == 1.0f);
    CHECK(pick(1.0f) == 1.0f);
    CHECK(pick(-1.0f) == 2.0f);
    CHECK(range(0.5f));
    CHECK(range(10.0f));
    CHECK(range(20.0f));
    CHECK_FALSE(range(5.0f));

    CHECK(errorsOf("float f(float a, float b, float c) { (a ? b : c) = 1; return a; }")
          == "test.fm:1:50: error: expression is not assignable\n");
    CHECK(errorsOf("float k() { const bool b = true; b = false; return 0; }")
          == "test.fm:1:36: error: cannot assign to variable 'b' with const-qualified type 'const bool'\n");
}

TEST_CASE("if, else, while, for, break and continue follow C", "[codegen]")
{
    std::string errors;
    auto jit = compile("float sum_to(float n)\n"
                       "{\n"
                       "    float total = 0;\n"
                       "    for (float i = 1; i <= n; i += 1)\n"
                       "        total += i;\n"
                       "    return total;\n"
                       "}\n"
                       "float classify(float x)\n"                 // the else binds to the nearest if
                       "{\n"
                       "    if (x > 0) if (x > 10) return 2; else return 1;\n"
                       "    return 0;\n"
                       "}\n"
                       "float odd_sum(float n)\n"                  // continue runs the step
                       "{\n"
                       "    float total = 0;\n"
                       "    for (float i = 0; i < n; i += 1) {\n"
                       "        if (i % 2 == 0) continue;\n"
                       "        total += i;\n"
                       "    }\n"
                       "    return total;\n"
                       "}\n"
                       "float first_square_above(float limit)\n"   // for (;;), break leaves the inner loop only
                       "{\n"
                       "    float found = 0;\n"
                       "    for (float i = 1; ; i += 1) {\n"
                       "        float j = 0;\n"
                       "        while (1) { j += 1; if (j >= i) break; }\n"
                       "        if (i * j > limit) { found = i * j; break; }\n"
                       "    }\n"
                       "    return found;\n"
                       "}\n"
                       "float halve_until(float x) { while (x > 1) x /= 2; return x; }\n"
                       "float pick(bool c) { if (c) return 1; else return 2; }\n"   // both branches return: no end
                       "float forever() { for (;;) { } }\n"                          // the end is unreachable
                       "float dead(float x) { return x; if (x < 0) x = 1; }\n",     // an if in dead code
                       &errors);
    INFO(errors);
    REQUIRE(jit);
    CHECK(lookup<float(float)>(*jit, "sum_to")(4.0f) == 10.0f);
    CHECK(lookup<float(float)>(*jit, "sum_to")(0.0f) == 0.0f);
    CHECK(lookup<float(float)>(*jit, "classify")(20.0f) == 2.0f);
    CHECK(lookup<float(float)>(*jit, "classify")(5.0f) == 1.0f);
    CHECK(lookup<float(float)>(*jit, "classify")(-5.0f) == 0.0f);
    CHECK(lookup<float(float)>(*jit, "odd_sum")(6.0f) == 9.0f);
    CHECK(lookup<float(float)>(*jit, "first_square_above")(10.0f) == 16.0f);
    CHECK(lookup<float(float)>(*jit, "halve_until")(10.0f) == 0.625f);
    CHECK(lookup<float(bool)>(*jit, "pick")(false) == 2.0f);
    CHECK(lookup<float(float)>(*jit, "dead")(3.0f) == 3.0f);

    CHECK(errorsOf("float f(float x)\n"
                   "{\n"
                   "    break;\n"
                   "    continue;\n"
                   "    for (float i = 0; i < 1; i += 1) { }\n"
                   "    return i;\n"
                   "}\n"
                   "float g(float x) { if (x > 0) return 1; }\n")
          == "test.fm:3:5: error: 'break' statement not in loop or switch statement\n"
             "test.fm:4:5: error: 'continue' statement not in loop statement\n"
             "test.fm:6:12: error: use of undeclared identifier 'i'\n"
             "test.fm:8:7: error: non-void function 'g' does not return a value\n");
}
