// tests/unit/WhenTest.cpp: `when (condition) statement` (frontend/Ast.h, ir/Statements.cpp), which runs its statement
// each time its condition becomes true, its state a bool of the module's instance after the declared fields. Through
// the JIT: a rising edge fires once, and again after the condition was false; a condition that stays true does not;
// the first reach with a true condition fires; two whens, and two instances, keep their own state, which a copy of a
// module copies; a when in a loop over samples fires on each rising edge; leaving the statement does not arm it again;
// sizeof a module counts its whens; a nonblocking method may hold one. Then the refusals, each once.
#include "Compile.h"

#include <catch2/catch_test_macros.hpp>

#include <vector>

using namespace fminus::test;

namespace
{
    // A module with two whens, and functions that drive an instance a host holds (its address).
    constexpr const char* kEdges = "module Edges\n"
                                   "{\n"
                                   "    int fired;\n"
                                   "    int other;\n"
                                   "    int step(bool c)\n"
                                   "    {\n"
                                   "        when (c)\n"
                                   "            fired++;\n"
                                   "        return fired;\n"
                                   "    }\n"
                                   "    int both(bool a, bool b)\n"
                                   "    {\n"
                                   "        when (a) fired++;\n"
                                   "        when (b) other++;\n"
                                   "        return fired * 10 + other;\n"
                                   "    }\n"
                                   "};\n"
                                   "int step(Edges* e, bool c) { return e->step(c); }\n"
                                   "int both(Edges* e, bool a, bool b) { return e->both(a, b); }\n"
                                   "int size() { return sizeof(Edges); }\n";

    // An Edges as FMinus lays it out: its two ints, and its three whens' bools after them.
    struct Edges
    {
        int fired = 0;
        int other = 0;
        bool was[3] = {};
    };
} // namespace

TEST_CASE("a when fires once on a rising edge, and again after its condition was false", "[codegen][when]")
{
    auto jit = compile(kEdges);
    REQUIRE(jit);
    auto* step = lookup<int(Edges*, bool)>(*jit, "step");
    CHECK(lookup<int()>(*jit, "size")() == static_cast<int>(sizeof(Edges)));
    Edges e;
    CHECK(step(&e, false) == 0);
    CHECK(step(&e, true) == 1);   // rising
    CHECK(step(&e, true) == 1);   // stays true: nothing
    CHECK(step(&e, true) == 1);
    CHECK(step(&e, false) == 1);
    CHECK(step(&e, true) == 2);   // rising again
    CHECK(e.was[0]);              // the state, a bool after the fields
    Edges first;
    CHECK(step(&first, true) == 1);   // the first reach, true: it fires
    // Two instances keep their own state.
    Edges a;
    Edges b;
    CHECK(step(&a, true) == 1);
    CHECK(step(&b, false) == 0);
    CHECK(step(&b, true) == 1);
    CHECK(step(&a, true) == 1);
    // Two whens keep theirs.
    auto* both = lookup<int(Edges*, bool, bool)>(*jit, "both");
    Edges c;
    CHECK(both(&c, true, false) == 10);
    CHECK(both(&c, true, true) == 11);
    CHECK(both(&c, false, true) == 11);
    CHECK(both(&c, true, false) == 21);
    CHECK(both(&c, true, true) == 22);
}

TEST_CASE("a when in a loop over samples fires on each rising edge; a copy of a module copies its state",
          "[codegen][when]")
{
    auto jit = compile("module Trigger\n"
                       "{\n"
                       "    float level;\n"
                       "    int count;\n"
                       "    void process(const float* gate, float* out, int n) [[clang::nonblocking]]\n"
                       "    {\n"
                       "        for (int i = 0; i < n; ++i)\n"
                       "        {\n"
                       "            when (gate[i] > 0.5f)\n"
                       "            {\n"
                       "                level = 1;\n"
                       "                count++;\n"
                       "            }\n"
                       "            out[i] = level;\n"
                       "            level *= 0.5f;\n"
                       "        }\n"
                       "    }\n"
                       "};\n"
                       "int run(const float* gate, float* out, int n)\n"
                       "{\n"
                       "    Trigger t;\n"
                       "    t.process(gate, out, n);\n"
                       "    Trigger copy = t;              // high at the end: a copy is high too\n"
                       "    float again[1] = {1};\n"
                       "    float sink[1];\n"
                       "    copy.process(again, sink, 1);  // no edge for the copy\n"
                       "    t.process(again, sink, 1);     // nor for the original\n"
                       "    return t.count * 10 + copy.count;\n"
                       "}\n");
    REQUIRE(jit);
    auto* run = lookup<int(const float*, float*, int)>(*jit, "run");
    const std::vector<float> gate{0, 1, 1, 0, 1, 0.2f, 0.9f, 1};
    std::vector<float> out(gate.size());
    CHECK(run(gate.data(), out.data(), static_cast<int>(gate.size())) == 33);
    CHECK(out == std::vector<float>{0, 1, 0.5f, 0.25f, 1, 0.5f, 1, 0.5f});
}

TEST_CASE("a when's state is stored before its statement runs: leaving the statement does not arm it again",
          "[codegen][when]")
{
    auto jit = compile("module M\n"
                       "{\n"
                       "    int fired;\n"
                       "    int early(bool c) { when (c) return -1; return fired; }\n"
                       "    int loop(int n)\n"
                       "    {\n"
                       "        int r = 0;\n"
                       "        for (int i = 0; i < n; ++i)\n"
                       "        {\n"
                       "            when (i % 3 != 0)     // true at 1 and 2, 4 and 5, ...: one edge in three\n"
                       "            {\n"
                       "                r++;\n"
                       "                continue;\n"
                       "            }\n"
                       "            when (i == 7) break;\n"
                       "        }\n"
                       "        return r;\n"
                       "    }\n"
                       "};\n"
                       "int test()\n"
                       "{\n"
                       "    M m;\n"
                       "    int a = m.early(true);    // -1: it fired, and returned\n"
                       "    int b = m.early(true);    // still true: no edge, and the end\n"
                       "    int c = m.loop(9);        // edges at 1 and 4; 7 is no multiple of 3, so the first when\n"
                       "    return a * 1000 + b * 100 + c;   // takes it and the break is never reached\n"
                       "}\n");
    REQUIRE(jit);
    CHECK(lookup<int()>(*jit, "test")() == -1000 + 0 + 3);
}

TEST_CASE("a when's refusals: outside a method, in a parallel region, on a poly bool", "[codegen][when][errors]")
{
    CHECK(errorsOf("int f(bool c) { when (c) return 1; return 0; }\n") ==
          "test.fm:1:17: error: a 'when' keeps its state, one bool, in the instance of the module whose method it "
          "is in, and 'f' is no method: move it into a method of a module\n");
    CHECK(errorsOf("module M\n"
                   "{\n"
                   "    int n;\n"
                   "    void tick(bool c) { when (c) n++; }\n"
                   "    void all(const float* x, int k)\n"
                   "    {\n"
                   "        parallel for (int i = 0; i < k; ++i)\n"
                   "            when (x[i] > 0) {}\n"
                   "        parallel\n"
                   "        {\n"
                   "            when (k > 0) {}\n"
                   "            k + 1;\n"
                   "        }\n"
                   "        parallel for (int i = 0; i < k; ++i)\n"
                   "            tick(x[i] > 0);\n"
                   "        poly bool lanes = poly_index() > 1;\n"
                   "        when (lanes) {}\n"
                   "    }\n"
                   "};\n") ==
          "test.fm:8:13: error: every iteration of the parallel for runs this 'when', whose state is the module's "
          "('this'), and the iterations run at once\n"
          "test.fm:11:13: error: a statement of a parallel block cannot run a 'when': its state is the module's "
          "('this'), and the block's statements run at once\n"
          "test.fm:15:13: error: every iteration of the parallel for calls 'M::tick', which changes the module "
          "it is called on ('this'), and the iterations run at once\n"
          "test.fm:17:15: error: statement requires expression of scalar type ('poly bool' invalid)\n");
}
