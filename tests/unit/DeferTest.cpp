// tests/unit/DeferTest.cpp: TS 25755's defer (extending.md, recipe 44), through the JIT: a deferred statement runs when
// its block is left, at its end or by return, break or continue, the last reached first and the innermost block's
// first; a return's value is computed before; nothing is captured; a defer that is the whole body of an if or a loop
// runs at once, each time; one not reached does not run; one may hold another. Then the constraints, each once and in
// clang's words, and the effects of a deferred call.
#include "Compile.h"

#include "fminus/Config.h"

#include <catch2/catch_test_macros.hpp>

#include <string>
#include <vector>

using namespace fminus::test;

TEST_CASE("a deferred statement runs when its block ends, the last reached first, the innermost block's first",
          "[codegen][defer]")
{
    auto jit = compile("int order()\n"
                       "{\n"
                       "    int r = 0;\n"
                       "    {\n"
                       "        defer r = r * 10 + 1;\n"
                       "        _Defer r = r * 10 + 2;\n"
                       "        {\n"
                       "            defer r = r * 10 + 3;\n"
                       "        }\n"
                       "        r = r * 10 + 4;\n"
                       "    }\n"
                       "    return r;\n"
                       "}\n"
                       // TS 25755's example 5: a deferred statement's own defers run at its end
                       "int nested()\n"
                       "{\n"
                       "    int r = 0;\n"
                       "    {\n"
                       "        defer\n"
                       "        {\n"
                       "            defer r *= 4;\n"
                       "            r *= 2;\n"
                       "            defer\n"
                       "            {\n"
                       "                r += 3;\n"
                       "            }\n"
                       "        }\n"
                       "        defer r += 1;\n"
                       "    }\n"
                       "    return r;\n"
                       "}\n"
                       // a defer that is an if's or a loop's whole body is that body's, and runs when it ends
                       "int unbraced(bool c)\n"
                       "{\n"
                       "    int r = 0;\n"
                       "    if (c)\n"
                       "        defer r = r * 10 + 1;\n"
                       "    r = r * 10 + 2;\n"
                       "    for (int i = 3; i < 6; ++i)\n"
                       "        defer r = r * 10 + i;\n"
                       "    return r;\n"
                       "}\n"
                       // a deferred statement declares in braces, and sees what was in scope at its defer
                       "int scoped()\n"
                       "{\n"
                       "    int x = 1;\n"
                       "    int r = 0;\n"
                       "    {\n"
                       "        defer { int y = x * 100; r += y; }\n"
                       "        int x = 7;\n"
                       "        r += x;\n"
                       "    }\n"
                       "    return r;\n"
                       "}\n");
    REQUIRE(jit != nullptr);
    CHECK(lookup<int()>(*jit, "order")() == 3421);
    CHECK(lookup<int()>(*jit, "nested")() == 20);
    CHECK(lookup<int(bool)>(*jit, "unbraced")(true) == 12345);
    CHECK(lookup<int(bool)>(*jit, "unbraced")(false) == 2345);
    CHECK(lookup<int()>(*jit, "scoped")() == 107);
}

TEST_CASE("return, break and continue run the deferred statements of the blocks they leave, and no others",
          "[codegen][defer]")
{
    auto jit = compile("int value(int* out)\n"
                       "{\n"
                       "    int x = 1;\n"
                       "    defer *out = x;\n"   // x as it is when the function returns
                       "    defer x = 2;\n"
                       "    *out = 0;\n"
                       "    return x;\n"         // 1: computed before x = 2
                       "}\n"
                       // TS 25755's example 2
                       "int example()\n"
                       "{\n"
                       "    int r = 4;\n"
                       "    int* p = &r;\n"
                       "    defer { *p = 5; }\n"
                       "    return *p;\n"
                       "}\n"
                       "float converted(float* out)\n"
                       "{\n"
                       "    int k = 7;\n"
                       "    defer *out = (float)k;\n"
                       "    defer k = 9;\n"
                       "    return k / 2;\n"   // 3, converted to 3.0f, before either runs
                       "}\n"
                       "int loops(int* log)\n"
                       "{\n"
                       "    int n = 0;\n"
                       "    for (int i = 0; i < 5; ++i)\n"
                       "    {\n"
                       "        defer log[n++] = i;\n"
                       "        if (i == 1)\n"
                       "            continue;\n"
                       "        if (i == 3)\n"
                       "            break;\n"
                       "        log[n++] = 100 + i;\n"
                       "    }\n"
                       "    int k = 0;\n"
                       "    while (k < 10)\n"
                       "    {\n"
                       "        defer k += 1;\n"
                       "        switch (k)\n"
                       "        {\n"
                       "        case 0:\n"
                       "        {\n"
                       "            defer log[n++] = 50;\n"
                       "            break;\n"   // leaves the switch's case block: its defer, not the loop's
                       "        }\n"
                       "        case 1:\n"
                       "            continue;\n"
                       "        }\n"
                       "        if (k == 2)\n"
                       "            break;\n"
                       "        log[n++] = 60 + k;\n"
                       "    }\n"
                       "    log[n++] = k;\n"
                       "    return n;\n"
                       "}\n"
                       "int early(bool stop, int* out)\n"
                       "{\n"
                       "    *out = 0;\n"
                       "    {\n"
                       "        if (stop)\n"
                       "            return 1;\n"
                       "        defer *out = 7;\n"   // not reached when stop: does not run
                       "    }\n"
                       "    return 2;\n"
                       "}\n"
                       // every way out a return in a block with a defer: the end is not reached, and needs none
                       "int both(bool c, int* out)\n"
                       "{\n"
                       "    {\n"
                       "        defer *out += 1;\n"
                       "        if (c)\n"
                       "            return 1;\n"
                       "        return 2;\n"
                       "    }\n"
                       "}\n"
                       "int inner(int* out)\n"
                       "{\n"
                       "    *out = 0;\n"
                       "    defer *out = *out * 10 + 1;\n"
                       "    for (int i = 0; i < 3; ++i)\n"
                       "    {\n"
                       "        defer *out = *out * 10 + 2;\n"
                       "        {\n"
                       "            defer *out = *out * 10 + 3;\n"
                       "            if (i == 1)\n"
                       "                return *out;\n"   // 32: computed before 3, 2 and 1 run
                       "        }\n"
                       "    }\n"
                       "    return -1;\n"
                       "}\n");
    REQUIRE(jit != nullptr);
    int out = -1;
    CHECK(lookup<int(int*)>(*jit, "value")(&out) == 1);
    CHECK(out == 2);
    CHECK(lookup<int()>(*jit, "example")() == 4);
    float result = 0.0f;
    CHECK(lookup<float(float*)>(*jit, "converted")(&result) == 3.0f);
    CHECK(result == 9.0f);

    std::vector<int> log(32, -1);
    const int n = lookup<int(int*)>(*jit, "loops")(log.data());
    log.resize(static_cast<std::size_t>(n));
    CHECK(log == std::vector<int>{100, 0, 1, 102, 2, 3, 50, 60, 3});

    CHECK(lookup<int(bool, int*)>(*jit, "early")(true, &out) == 1);
    CHECK(out == 0);
    CHECK(lookup<int(bool, int*)>(*jit, "early")(false, &out) == 2);
    CHECK(out == 7);
    out = 0;
    CHECK(lookup<int(bool, int*)>(*jit, "both")(true, &out) == 1);
    CHECK(lookup<int(bool, int*)>(*jit, "both")(false, &out) == 2);
    CHECK(out == 2);
    CHECK(lookup<int(int*)>(*jit, "inner")(&out) == 32);
    CHECK(out == 32321);
}

#if FMINUS_HAS_JSON
TEST_CASE("defer closes a handle on every path of a function with three exits", "[codegen][defer][handles]")
{
    auto jit = compile("void close(json_doc doc, int* closed) { json_free(doc); *closed += 1; }\n"
                       "int gain(const void* data, int bytes, int* closed)\n"
                       "{\n"
                       "    json_doc doc = json_parse(data, bytes);\n"
                       "    defer close(doc, closed);\n"
                       "    if (!doc)\n"
                       "        return -1;\n"
                       "    const int g = json_int(doc, \"/gain\", -2);\n"
                       "    if (g < 0)\n"
                       "        return -2;\n"
                       "    return g;\n"
                       "}\n");
    REQUIRE(jit != nullptr);
    auto* gain = lookup<int(const void*, int, int*)>(*jit, "gain");
    int closed = 0;
    const std::string good = R"({"gain": 6})";
    const std::string none = R"({"level": 6})";
    const std::string bad = "{";
    CHECK(gain(good.data(), static_cast<int>(good.size()), &closed) == 6);
    CHECK(gain(none.data(), static_cast<int>(none.size()), &closed) == -2);
    CHECK(gain(bad.data(), static_cast<int>(bad.size()), &closed) == -1);
    CHECK(closed == 3);
}
#endif

TEST_CASE("a defer's constraints are clang's errors, each once, and its deferred calls are its function's",
          "[codegen][defer]")
{
    CHECK(errorsOf("int f(int k)\n"
                   "{\n"
                   "    defer return;\n"
                   "    for (;;)\n"
                   "    {\n"
                   "        defer { break; }\n"
                   "        defer continue;\n"
                   "        defer for (int i = 0; i < k; ++i) { if (i == 2) break; else continue; }\n"
                   "    }\n"
                   "    defer break;\n"
                   "    return 0;\n"
                   "}\n")
          == "test.fm:3:11: error: cannot return from a defer statement\n"
             "test.fm:6:17: error: cannot break out of a defer statement\n"
             "test.fm:7:15: error: cannot continue loop outside of enclosing defer statement\n"
             "test.fm:10:11: error: 'break' statement not in loop or switch statement\n");
    // A switch may not jump past a defer of a block it jumps into, nor into a deferred statement (TS 25755's example
    // 6): clang's error at the label, its notes at the defers.
    CHECK(errorsOf("void f(int k, int* p)\n"
                   "{\n"
                   "    switch (k)\n"
                   "    {\n"
                   "    case 0:\n"
                   "        defer *p = 1;\n"
                   "        {\n"
                   "            defer *p = 2;\n"
                   "        case 1:\n"
                   "            break;\n"
                   "        }\n"
                   "    default:\n"
                   "        defer { case 2: *p = 3; }\n"
                   "    }\n"
                   "    switch (k)\n"
                   "    {\n"
                   "    case 0: { defer *p = 4; }\n"   // its block has ended: no jump passes it
                   "    case 1: break;\n"
                   "    }\n"
                   "}\n")
          == "test.fm:9:9: error: cannot jump from switch statement to this case label\n"
             "test.fm:8:13: note: jump bypasses defer statement\n"
             "test.fm:6:9: note: jump bypasses defer statement\n"
             "test.fm:12:5: error: cannot jump from switch statement to this case label\n"
             "test.fm:6:9: note: jump bypasses defer statement\n"
             "test.fm:13:17: error: cannot jump from switch statement to this case label\n"
             "test.fm:13:9: note: jump enters a defer statement\n");
    // An error in a deferred statement is reported once, however many exits lower it; and a deferred call is its
    // function's, as `p = malloc(4);` would be where it is written.
    CHECK(errorsOf("int f(int k)\n"
                   "{\n"
                   "    defer k = y;\n"
                   "    if (k > 1)\n"
                   "        return 1;\n"
                   "    if (k > 2)\n"
                   "        return 2;\n"
                   "    return 3;\n"
                   "}\n"
                   "void g(float* buffer, int n) [[clang::nonblocking]]\n"
                   "{\n"
                   "    float* scratch = buffer;\n"
                   "    defer free(scratch);\n"
                   "    for (int i = 0; i < n; ++i)\n"
                   "    {\n"
                   "        if (buffer[i] > 1.0f)\n"
                   "            return;\n"
                   "    }\n"
                   "}\n"
                   "int h(int k) { { defer k += 1; } }\n")
          == "test.fm:3:15: error: use of undeclared identifier 'y'\n"
             "test.fm:13:11: error: function with 'nonblocking' attribute must not call non-'nonblocking' function "
             "'free'\n"
             "test.fm:20:5: error: non-void function 'h' does not return a value\n");
    // A defer is no statement of a parallel block, each of which is a task; inside one's braces it is the task's.
    CHECK(errorsOf("void f(float* a, float* b)\n"
                   "{\n"
                   "    parallel\n"
                   "    {\n"
                   "        defer a[0] = 1.0f;\n"
                   "        { defer b[0] = 2.0f; b[1] = 0.0f; }\n"
                   "    }\n"
                   "}\n")
          == "test.fm:5:9: error: a statement of a parallel block cannot be a defer statement (each statement is a "
             "task, which would run it as soon as it began): defer it before the block, or inside a statement's "
             "braces\n");
}

TEST_CASE("a defer in a parallel region's iteration is the iteration's, and runs at its end", "[codegen][defer]")
{
    auto jit = compile("void scale(float* x, int n)\n"
                       "{\n"
                       "    parallel for (int i = 0; i < n; ++i)\n"
                       "    {\n"
                       "        defer x[i] *= 2.0f;\n"
                       "        if (x[i] < 0.0f)\n"
                       "            continue;\n"
                       "        x[i] += 1.0f;\n"
                       "    }\n"
                       "}\n");
    REQUIRE(jit != nullptr);
    std::vector<float> x{1.0f, -1.0f, 2.0f, 0.5f};
    lookup<void(float*, int)>(*jit, "scale")(x.data(), 4);
    CHECK(x == std::vector<float>{4.0f, -2.0f, 6.0f, 3.0f});
}
