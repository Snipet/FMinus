// tests/unit/SwitchTest.cpp: C's switch, with case and default as C23's labels (extending.md, recipe 27), run through
// the JIT: fall-through, break and default, labels in any order, inside blocks and loops of the body, a switch in a
// loop (break leaves the switch, continue goes to the loop), nested switches, an empty one, a bool's, case values from
// constant arithmetic (C's integer constant expressions, worked out as FMinus computes), the declarations a jump
// passes zeroed, a switch in a parallel for, and clang's errors; and clang's -Wswitch, FMinus's warning, for a switch
// over a value of an enumerated type that misses some of its constants (recipe 32).
#include "Compile.h"

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <string>

using namespace fminus::test;

TEST_CASE("a switch jumps to its value's case, falls through to the next, and break leaves it", "[codegen][switch]")
{
    std::string errors;
    auto jit = compile("int pick(int x)\n"
                       "{\n"
                       "    int r = 0;\n"
                       "    switch (x)\n"
                       "    {\n"
                       "    case 0:\n"
                       "        r = 10;\n"
                       "        break;\n"
                       "    case 1:\n"                  // two labels, one statement
                       "    case 2:\n"
                       "        r = 20;\n"              // falls through
                       "    case 3:\n"
                       "        r += 1;\n"
                       "        break;\n"
                       "    default:\n"                 // default in the middle
                       "        r = -1;\n"
                       "    case -5:\n"
                       "        r -= 100;\n"
                       "    }\n"
                       "    return r;\n"
                       "}\n"
                       "int noDefault(int x) { int r = 7; switch (x) { case 1: r = 1; } return r; }\n"
                       "int empty(int x) { switch (x) { } switch (x); return x + 1; }\n"
                       "int last(int x) { int r = 0; switch (x) { case 1: r = 1; case 2: } return r; }\n"   // C23
                       "int returns(int x) { switch (x) { case 1: return 10; default: return 20; } }\n"
                       "int one(int x) { switch (x) case 4: return 1; return 0; }\n"   // a body that is a label
                       "int truth(bool b)\n"
                       "{\n"
                       "    switch (b) { case false: return 5; case true: return 6; }\n"
                       "    return 0;\n"
                       "}\n",
                       &errors);
    INFO(errors);
    REQUIRE(jit);
    auto pick = lookup<int(int)>(*jit, "pick");
    CHECK(pick(0) == 10);
    CHECK(pick(1) == 21);
    CHECK(pick(2) == 21);
    CHECK(pick(3) == 1);
    CHECK(pick(4) == -101);   // default, then case -5's statement
    CHECK(pick(-5) == -100);
    CHECK(lookup<int(int)>(*jit, "noDefault")(1) == 1);
    CHECK(lookup<int(int)>(*jit, "noDefault")(2) == 7);
    CHECK(lookup<int(int)>(*jit, "empty")(4) == 5);
    CHECK(lookup<int(int)>(*jit, "last")(1) == 1);
    CHECK(lookup<int(int)>(*jit, "last")(2) == 0);
    CHECK(lookup<int(int)>(*jit, "returns")(1) == 10);
    CHECK(lookup<int(int)>(*jit, "returns")(9) == 20);
    CHECK(lookup<int(int)>(*jit, "one")(4) == 1);
    CHECK(lookup<int(int)>(*jit, "one")(3) == 0);
    CHECK(lookup<int(bool)>(*jit, "truth")(false) == 5);
    CHECK(lookup<int(bool)>(*jit, "truth")(true) == 6);
}

TEST_CASE("in a loop, break leaves the switch and continue goes to the loop's next iteration", "[codegen][switch]")
{
    std::string errors;
    auto jit = compile("int count(const int* codes, int n)\n"
                       "{\n"
                       "    int total = 0;\n"
                       "    for (int i = 0; i < n; ++i)\n"
                       "    {\n"
                       "        switch (codes[i])\n"
                       "        {\n"
                       "        case 0: total += 1; break;\n"           // leaves the switch, not the loop
                       "        case 1: total += 10; continue;\n"       // skips the += 1000 below
                       "        case 2:\n"
                       "            switch (codes[i] * 2)\n"            // a switch in a switch
                       "            {\n"
                       "            case 4: total += 100; break;\n"     // leaves the inner one
                       "            default: total += 7;\n"
                       "            }\n"
                       "            break;\n"
                       "        default: total += 10000;\n"
                       "        }\n"
                       "        total += 1000;\n"
                       "    }\n"
                       "    return total;\n"
                       "}\n"
                       "int until(int n)\n"
                       "{\n"
                       "    int i = 0;\n"
                       "    while (1)\n"
                       "    {\n"
                       "        switch (i) { case 3: return i * 100 + n; }\n"
                       "        ++i;\n"
                       "    }\n"   // no return: only a break would leave the while (1), as clang's CFG finds
                       "}\n",
                       &errors);
    INFO(errors);
    REQUIRE(jit);
    const std::array<int, 5> codes{0, 1, 2, 3, 0};
    // 0: 1 + 1000; 1: 10; 2: 100 + 1000; 3: 10000 + 1000; 0: 1 + 1000
    CHECK(lookup<int(const int*, int)>(*jit, "count")(codes.data(), 5) == 1001 + 10 + 1100 + 11000 + 1001);
    CHECK(lookup<int(int)>(*jit, "until")(5) == 305);
}

TEST_CASE("labels may sit in blocks and loops of the body, and a declaration a jump passes is 0", "[codegen][switch]")
{
    std::string errors;
    auto jit = compile("int into(int x)\n"
                       "{\n"
                       "    int r = 0;\n"
                       "    switch (x)\n"
                       "    {\n"
                       "    case 0:\n"
                       "        for (int i = 0; i < 3; ++i)\n"
                       "        {\n"
                       "            r += 1;\n"
                       "    case 1:\n"                            // into the loop: i is passed, so 0
                       "            r += 10;\n"
                       "        }\n"
                       "        break;\n"
                       "    case 2:\n"
                       "        if (x > 100)\n"
                       "        {\n"
                       "    case 3:\n"                            // into the if's block
                       "            r = 50;\n"
                       "        }\n"
                       "    }\n"
                       "    return r;\n"
                       "}\n"
                       "float passed(int x)\n"
                       "{\n"
                       "    float r = 1;\n"
                       "    switch (x)\n"
                       "    {\n"
                       "        float before = 9;\n"              // never runs: before any label
                       "    case 0:\n"
                       "        float t = 5;\n"                   // C23: a declaration after a label
                       "        float4 v = (float4)(1);\n"
                       "        int a[3] = {1, 2, 3};\n"
                       "        r = t + v.y + a[2];\n"
                       "        break;\n"
                       "    case 1:\n"                            // passes t, v and a: each 0
                       "        r = t + v.w + a[0] + a[2] + before;\n"
                       "        break;\n"
                       "    }\n"
                       "    return r;\n"
                       "}\n",
                       &errors);
    INFO(errors);
    REQUIRE(jit);
    auto into = lookup<int(int)>(*jit, "into");
    CHECK(into(0) == 33);   // three times 1 + 10
    CHECK(into(1) == 32);   // 10 with i at 0, then i is 1 and 2: twice more 1 + 10
    CHECK(into(2) == 0);
    CHECK(into(3) == 50);
    auto passed = lookup<float(int)>(*jit, "passed");
    CHECK(passed(0) == 9.0f);
    CHECK(passed(1) == 0.0f);
    CHECK(passed(2) == 1.0f);
}

TEST_CASE("case values are integer constant expressions, computed as FMinus computes them", "[codegen][switch]")
{
    std::string errors;
    auto jit = compile("int which(int x)\n"
                       "{\n"
                       "    switch (x)\n"
                       "    {\n"
                       "    case 2 + 3 * 4: return 1;\n"            // 14
                       "    case -(1 << 4): return 2;\n"            // -16
                       "    case 1 << 33: return 3;\n"              // the count's low five bits: 2
                       "    case -8 >> 1: return 4;\n"              // the sign shifted in: -4
                       "    case 0x7FFFFFFF: return 5;\n"
                       "    case -2147483647 - 1: return 6;\n"     // the least int
                       "    case 7 / 2 + 7 % -4: return 7;\n"      // 3 + 3
                       "    case (int)2.9f * 1000: return 8;\n"   // a floating constant as a cast's operand
                       "    case 1 < 2 ? 100 : 1 / 0: return 9;\n"     // the arm not taken is not evaluated
                       "    case (0 && 1 / 0) + 101: return 10;\n"
                       "    case (true || 1 / 0) + 101 * 2: return 11;\n"
                       "    case ~0 & 0xF0 ^ 0x0F: return 12;\n"   // 0xF0 ^ 0x0F
                       "    case sizeof(float4) + sizeof(int*): return 13;\n"
                       "    case (bool)0.5f + (bool)7 + (int)true + !0 + 50: return 14;\n"
                       "    }\n"
                       "    return 0;\n"
                       "}\n",
                       &errors);
    INFO(errors);
    REQUIRE(jit);
    auto which = lookup<int(int)>(*jit, "which");
    CHECK(which(14) == 1);
    CHECK(which(-16) == 2);
    CHECK(which(2) == 3);
    CHECK(which(-4) == 4);
    CHECK(which(2147483647) == 5);
    CHECK(which(-2147483647 - 1) == 6);
    CHECK(which(6) == 7);
    CHECK(which(2000) == 8);
    CHECK(which(100) == 9);
    CHECK(which(101) == 10);
    CHECK(which(203) == 11);
    CHECK(which(0xFF) == 12);
    CHECK(which(24) == 13);
    CHECK(which(54) == 14);
    CHECK(which(1) == 0);
}

TEST_CASE("a switch in a parallel for is each iteration's own", "[codegen][switch][parallel]")
{
    std::string errors;
    auto jit = compile("void shape(float* out, const int* modes, int n)\n"
                       "{\n"
                       "    parallel for (int i = 0; i < n; ++i)\n"
                       "    {\n"
                       "        switch (modes[i])\n"
                       "        {\n"
                       "        case 0: out[i] = 1; break;\n"
                       "        case 1: out[i] = 2; continue;\n"   // the next iteration
                       "        default: out[i] = 3;\n"
                       "        }\n"
                       "        out[i] += 10;\n"
                       "    }\n"
                       "}\n",
                       &errors);
    INFO(errors);
    REQUIRE(jit);
    std::array<float, 64> out{};
    std::array<int, 64> modes{};
    for (std::size_t i = 0; i < modes.size(); ++i)
        modes[i] = static_cast<int>(i % 3);
    lookup<void(float*, const int*, int)>(*jit, "shape")(out.data(), modes.data(), 64);
    for (std::size_t i = 0; i < out.size(); ++i)
    {
        INFO(i);
        CHECK(out[i] == (i % 3 == 0 ? 11.0f : i % 3 == 1 ? 2.0f : 13.0f));
    }
}

TEST_CASE("switch's errors are clang's: the value's type, duplicates, constants, and labels out of place",
          "[codegen][switch]")
{
    CHECK(errorsOf("int f(float y, float* p, float4 v)\n"
                   "{\n"
                   "    switch (y) { case 1: break; }\n"
                   "    switch (p) { }\n"
                   "    switch (v) { }\n"
                   "    switch (nope) { }\n"   // its own error only
                   "    return 0;\n"
                   "}\n")
          == "test.fm:3:13: error: statement requires expression of integer type ('float' invalid)\n"
             "test.fm:4:13: error: statement requires expression of integer type ('float *' invalid)\n"
             "test.fm:5:13: error: statement requires expression of integer type ('float4' invalid)\n"
             "test.fm:6:13: error: use of undeclared identifier 'nope'\n");
    CHECK(errorsOf("int f(int x)\n"
                   "{\n"
                   "    switch (x)\n"
                   "    {\n"
                   "    case 1: break;\n"
                   "    case 3 - 2: break;\n"
                   "    case 1: break;\n"
                   "    default: break;\n"
                   "    default: break;\n"
                   "    }\n"
                   "    return x;\n"
                   "}\n")
          == "test.fm:6:12: error: duplicate case value '1'\n"
             "test.fm:5:10: note: previous case defined here\n"
             "test.fm:7:10: error: duplicate case value '1'\n"
             "test.fm:6:12: note: previous case defined here\n"
             "test.fm:9:5: error: multiple default labels in one switch\n"
             "test.fm:8:5: note: previous case defined here\n");
    CHECK(errorsOf("int f(int x, int k)\n"
                   "{\n"
                   "    const int c = 2;\n"
                   "    switch (x)\n"
                   "    {\n"
                   "    case k: break;\n"            // a variable, const or not, is no constant, as in C
                   "    case c: break;\n"
                   "    case k + 1: break;\n"
                   "    case f(1, 2): break;\n"
                   "    case x = 4: break;\n"
                   "    case 1.5f: break;\n"
                   "    case 2 * 0.5f: break;\n"
                   "    case 1.5f < 2: break;\n"     // a floating constant only as a cast's operand
                   "    case (int)(0.5f + 1): break;\n"
                   "    case (int)3e10f: break;\n"   // out of an int's range
                   "    case 1 / 0: break;\n"
                   "    case 5 % (2 - 2): break;\n"
                   "    case (-2147483647 - 1) / -1: break;\n"
                   "    case 2147483647 + 1: break;\n"
                   "    case -(-2147483647 - 1): break;\n"
                   "    case 65536 * 65536: break;\n"
                   "    case 1.5f & 1: break;\n"
                   "    case ~1.5f: break;\n"
                   "    case undeclared: break;\n"
                   "    }\n"
                   "    return x;\n"
                   "}\n")
          == "test.fm:6:10: error: expression is not an integer constant expression\n"
             "test.fm:7:10: error: expression is not an integer constant expression\n"
             "test.fm:8:10: error: expression is not an integer constant expression\n"
             "test.fm:9:10: error: expression is not an integer constant expression\n"
             "test.fm:10:12: error: expression is not an integer constant expression\n"
             "test.fm:11:10: error: integer constant expression must have integer type, not 'float'\n"
             "test.fm:12:12: error: integer constant expression must have integer type, not 'float'\n"
             "test.fm:13:10: error: expression is not an integer constant expression\n"
             "test.fm:14:16: error: expression is not an integer constant expression\n"
             "test.fm:15:10: error: expression is not an integer constant expression\n"
             "test.fm:16:12: error: expression is not an integer constant expression\n"
             "test.fm:16:12: note: division by zero is undefined\n"
             "test.fm:17:12: error: expression is not an integer constant expression\n"
             "test.fm:17:12: note: division by zero is undefined\n"
             "test.fm:18:28: error: expression is not an integer constant expression\n"
             "test.fm:18:28: note: value 2147483648 is outside the range of representable values of type 'int'\n"
             "test.fm:19:21: error: overflow in expression; result is -2147483648 with type 'int'\n"
             "test.fm:20:10: error: overflow in expression; result is -2147483648 with type 'int'\n"
             "test.fm:21:16: error: overflow in expression; result is 0 with type 'int'\n"
             "test.fm:22:15: error: invalid operands to binary expression ('float' and 'int')\n"
             "test.fm:23:10: error: invalid argument type 'float' to unary expression\n"
             "test.fm:24:10: error: use of undeclared identifier 'undeclared'\n");
    CHECK(errorsOf("void f(int x, float* out, int n)\n"
                   "{\n"
                   "    case 1: x = 2;\n"
                   "    default: break;\n"
                   "    while (x) { case 2: x = 0; }\n"
                   "    switch (x) { case 1: continue; }\n"
                   "    switch (x)\n"
                   "    {\n"
                   "    case 0:\n"
                   "        parallel for (int i = 0; i < n; ++i) { case 1: out[i] = 0; }\n"
                   "        parallel { out[0] = 1; default: out[1] = 2; }\n"
                   "    }\n"
                   "}\n")
          == "test.fm:3:5: error: 'case' statement not in switch statement\n"
             "test.fm:4:5: error: 'default' statement not in switch statement\n"
             "test.fm:4:14: error: 'break' statement not in loop or switch statement\n"
             "test.fm:5:17: error: 'case' statement not in switch statement\n"
             "test.fm:6:26: error: 'continue' statement not in loop statement\n"
             "test.fm:10:48: error: 'case' cannot jump into a parallel for\n"
             "test.fm:11:32: error: 'default' cannot jump into a statement of a parallel block\n");
}

TEST_CASE("clang's -Wswitch: a switch over an enumerated type, without a default, names the constants it misses",
          "[codegen][switch][enum]")
{
    std::string errors;
    // The warnings are the program's, which compiles and runs: one each for a missing constant, two, three, and more
    // (the first three, then ...), each at the switch's value, with clang's note.
    auto jit = compile("enum Wave { SAW, SQUARE, TRIANGLE, NOISE, PULSE };\n"
                       "int one(enum Wave w) { switch (w) { case SAW: case SQUARE: case TRIANGLE: case NOISE: "
                       "return 1; } return 0; }\n"
                       "int two(enum Wave w) { switch (w) { case SAW: case SQUARE: case TRIANGLE: return 1; } "
                       "return 0; }\n"
                       "int three(enum Wave w) { switch (w) { case SAW: case SQUARE: return 1; } return 0; }\n"
                       "int four(enum Wave w) { switch (w) { case SAW: return 1; } return 0; }\n",
                       &errors);
    REQUIRE(jit);
    CHECK(errors == "test.fm:2:32: warning: enumeration value 'PULSE' not handled in switch [-Wswitch]\n"
                    "test.fm:2:32: note: add missing switch cases\n"
                    "test.fm:3:32: warning: enumeration values 'NOISE' and 'PULSE' not handled in switch [-Wswitch]\n"
                    "test.fm:3:32: note: add missing switch cases\n"
                    "test.fm:4:34: warning: enumeration values 'TRIANGLE', 'NOISE', and 'PULSE' not handled in switch "
                    "[-Wswitch]\n"
                    "test.fm:4:34: note: add missing switch cases\n"
                    "test.fm:5:33: warning: 4 enumeration values not handled in switch: 'SQUARE', 'TRIANGLE', "
                    "'NOISE'... [-Wswitch]\n"
                    "test.fm:5:33: note: add missing switch cases\n");
    CHECK(lookup<int(int)>(*jit, "four")(0) == 1);

    // Each value once, by its first constant, in order of value, as clang lists them.
    jit = compile("enum E { C2 = 2, A = 0, B = 1, A2 = 0 };\n"
                  "int f(enum E e) { switch (e) { case B: return 1; } return 0; }",
                  &errors);
    REQUIRE(jit);
    CHECK(errors == "test.fm:2:27: warning: enumeration values 'A' and 'C2' not handled in switch [-Wswitch]\n"
                    "test.fm:2:27: note: add missing switch cases\n");

    // Every value of the enumerated type: a variable, a parameter, a field (in a method, through a pointer: where the
    // value begins, as clang places it), a result, a cast, an assignment, an element, *p, a parameter of the host's,
    // C23's bool, an enumeration without a tag declaring its variable.
    jit = compile("enum E { A, B };\n"
                  "enum F : bool { OFF, ON };\n"
                  "enum E get();\n"
                  "module M\n"
                  "{\n"
                  "    enum E e;\n"
                  "    param enum E knob;\n"
                  "    int g() { switch (e) { case A: return 1; } switch (knob[0]) { case A: return 2; } return 0; }\n"
                  "};\n"
                  "int f(M* m, enum E* p, int x, enum F flag)\n"
                  "{\n"
                  "    enum E local = B;\n"
                  "    enum E all[2];\n"
                  "    enum { X, Y } anonymous = X;\n"
                  "    switch (m->e) { case A: return 1; }\n"
                  "    switch (get()) { case A: return 2; }\n"
                  "    switch ((enum E)x) { case A: return 3; }\n"
                  "    switch (local = A) { case A: return 4; }\n"
                  "    switch (all[x]) { case A: return 5; }\n"
                  "    switch (*p) { case A: return 6; }\n"
                  "    switch (flag) { case ON: return 7; }\n"
                  "    switch (anonymous) { case X: return 8; }\n"
                  "    return 0;\n"
                  "}\n"
                  "enum E get() { return B; }\n",
                  &errors);
    REQUIRE(jit);
    CHECK(errors == "test.fm:8:23: warning: enumeration value 'B' not handled in switch [-Wswitch]\n"
                    "test.fm:8:23: note: add missing switch cases\n"
                    "test.fm:8:56: warning: enumeration value 'B' not handled in switch [-Wswitch]\n"
                    "test.fm:8:56: note: add missing switch cases\n"
                    "test.fm:15:13: warning: enumeration value 'B' not handled in switch [-Wswitch]\n"
                    "test.fm:15:13: note: add missing switch cases\n"
                    "test.fm:16:13: warning: enumeration value 'B' not handled in switch [-Wswitch]\n"
                    "test.fm:16:13: note: add missing switch cases\n"
                    "test.fm:17:13: warning: enumeration value 'B' not handled in switch [-Wswitch]\n"
                    "test.fm:17:13: note: add missing switch cases\n"
                    "test.fm:18:13: warning: enumeration value 'B' not handled in switch [-Wswitch]\n"
                    "test.fm:18:13: note: add missing switch cases\n"
                    "test.fm:19:13: warning: enumeration value 'B' not handled in switch [-Wswitch]\n"
                    "test.fm:19:13: note: add missing switch cases\n"
                    "test.fm:20:13: warning: enumeration value 'B' not handled in switch [-Wswitch]\n"
                    "test.fm:20:13: note: add missing switch cases\n"
                    "test.fm:21:13: warning: enumeration value 'OFF' not handled in switch [-Wswitch]\n"
                    "test.fm:21:13: note: add missing switch cases\n"
                    "test.fm:22:13: warning: enumeration value 'Y' not handled in switch [-Wswitch]\n"
                    "test.fm:22:13: note: add missing switch cases\n");

    // A parenthesised value begins at its outermost opening parenthesis, as clang places it, also as an operand.
    jit = compile("enum E { A, B };\n"
                  "int f(enum E e) { switch ((e)) { case A: return 1; } switch (((e)) = A) { case A: return 2; } "
                  "return 0; }",
                  &errors);
    REQUIRE(jit);
    CHECK(errors == "test.fm:2:27: warning: enumeration value 'B' not handled in switch [-Wswitch]\n"
                    "test.fm:2:27: note: add missing switch cases\n"
                    "test.fm:2:62: warning: enumeration value 'B' not handled in switch [-Wswitch]\n"
                    "test.fm:2:62: note: add missing switch cases\n");

    // None: a default, every constant, what C promotes (an int's cast, arithmetic, -e, ?:), a constant, an int, an
    // enumeration declared ahead whose constants come after the switch, and a switch whose case is in error.
    jit = compile("enum E { A, B };\n"
                  "enum Later : int;\n"
                  "int f(enum E e, int c, enum Later later)\n"
                  "{\n"
                  "    switch (e) { case A: return 1; default: return 2; }\n"
                  "    switch (e) { case A: case B: return 3; }\n"
                  "    switch ((int)e) { case A: return 4; }\n"
                  "    switch (e + 0) { case A: return 5; }\n"
                  "    switch (-e) { case A: return 6; }\n"
                  "    switch (c ? e : B) { case A: return 7; }\n"
                  "    switch ((enum E)1) { case A: return 8; }\n"
                  "    switch (c) { case A: return 9; }\n"
                  "    switch (later) { case 0: return 10; }\n"
                  "    return 0;\n"
                  "}\n"
                  "enum Later : int { L0, L1 };\n",
                  &errors);
    REQUIRE(jit);
    CHECK(errors.empty());
    // Nor a value that folds to a constant, as clang folds a const object's (it says "no case matching constant switch
    // condition '1'" instead, a warning FMinus does not give): a constant of the top level of the enumerated type.
    jit = compile("const enum E { A, B, C } SHAPE = B;\n"
                  "int f() { switch (SHAPE) { case A: return 1; } switch ((SHAPE)) { case C: return 2; } return 0; }",
                  &errors);
    REQUIRE(jit);
    CHECK(errors.empty());
    CHECK(lookup<int()>(*jit, "f")() == 0);
    CHECK(errorsOf("enum E { A, B };\nint f(enum E e) { switch (e) { case A: return 1; case nope: return 2; } }")
          == "test.fm:2:5: error: non-void function 'f' does not return a value\n"
             "test.fm:2:55: error: use of undeclared identifier 'nope'\n");
}
