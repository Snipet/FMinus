// tests/unit/ModuleTest.cpp: modules (extending.md, recipe 12), FMinus's classes: fields, methods that see them,
// `this`, modules inside modules and arrays of them, copies, memory a C++ host owns, the effects of methods; and the
// errors, run through the JIT.
#include "Compile.h"

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <cmath>
#include <cstddef>
#include <string>

using namespace fminus::test;

namespace
{
    // A one-pole low-pass, the module the tests share.
    constexpr const char* kOnePole = "module OnePole\n"
                                     "{\n"
                                     "    float y;\n"
                                     "    float k;\n"
                                     "\n"
                                     "    void init(float cutoff, float rate)\n"
                                     "    {\n"
                                     "        k = 1 - expf(-6.2831853f * cutoff / rate);\n"
                                     "    }\n"
                                     "\n"
                                     "    float tick(float x) [[clang::nonblocking]]\n"
                                     "    {\n"
                                     "        return y += k * (x - y);\n"
                                     "    }\n"
                                     "};\n";

    // What a C++ host declares to hold one: the same fields in the same order, laid out alike.
    struct OnePole
    {
        float y = 0.0f;
        float k = 0.0f;
    };

    // tick() in C++, one rounding at a time: clang would fuse `y + k * (x - y)` written as one expression into a
    // multiply-add (-ffp-contract=on is its default), which FMinus never does.
    float next(float& y, float k, float x)
    {
        const float step = k * (x - y);
        y = y + step;
        return y;
    }
} // namespace

TEST_CASE("a module's methods see its fields, and a C++ host runs them on its own memory", "[codegen][module]")
{
    std::string errors;
    auto jit = compile(std::string(kOnePole)
                           + "void run(OnePole* f, const float* restrict in, float* restrict out, int n)"
                             " [[clang::nonblocking]]\n"
                             "{\n"
                             "    for (int i = 0; i < n; ++i)\n"
                             "        out[i] = f->tick(in[i]);\n"
                             "}\n",
                       &errors);
    INFO(errors);
    REQUIRE(jit);
    OnePole filter;   // the host's memory, zeroed: what a module starts as
    lookup<void(OnePole*, float, float)>(*jit, "OnePole::init")(&filter, 1000.0f, 48000.0f);
    CHECK(filter.k == 1.0f - std::exp(-6.2831853f * 1000.0f / 48000.0f));
    CHECK(filter.y == 0.0f);

    // Two calls of tick, then a block through run: the state carries over in the host's struct.
    auto tick = lookup<float(OnePole*, float)>(*jit, "OnePole::tick");
    float y = 0.0f;
    for (int i = 0; i < 2; ++i)
        CHECK(tick(&filter, 1.0f) == next(y, filter.k, 1.0f));
    const std::array<float, 4> in{1.0f, 0.0f, -1.0f, 0.5f};
    std::array<float, 4> out{};
    lookup<void(OnePole*, const float*, float*, int)>(*jit, "run")(&filter, in.data(), out.data(), 4);
    for (std::size_t i = 0; i < 4; ++i)
        CHECK(out[i] == next(y, filter.k, in[i]));
    CHECK(filter.y == y);
}

TEST_CASE("modules hold modules and arrays of them, and methods call methods", "[codegen][module]")
{
    std::string errors;
    auto jit = compile("module Counter\n"
                       "{\n"
                       "    int count;\n"
                       "    int step(int by) { return count += by; }\n"
                       "    int twice() { step(1); return this->step(1); }\n"   // on this, implicitly or not
                       "    int* where() { return &count; }\n"
                       "};\n"
                       "int bump(Counter* c) { return c->step(10); }\n"
                       "module Bank\n"
                       "{\n"
                       "    Counter counters[4];\n"
                       "    Counter total;\n"
                       "    float history[3];\n"
                       "    int add(int i, int by)\n"
                       "    {\n"
                       "        total.step(by);\n"
                       "        history[i % 3] = by;\n"
                       "        return counters[i].step(by);\n"
                       "    }\n"
                       "    int sum()\n"
                       "    {\n"
                       "        int s = 0;\n"
                       "        for (Counter* c = counters; c < counters + 4; ++c)\n"   // the array's name: a pointer
                       "            s += c->count;\n"
                       "        return s;\n"
                       "    }\n"
                       "    int passThis() { return bump(&total) + bump(this->counters + 1); }\n"
                       "};\n"
                       "int run()\n"
                       "{\n"
                       "    Bank bank;\n"
                       "    bank.add(0, 1);\n"
                       "    bank.add(1, 2);\n"
                       "    bank.add(3, 4);\n"
                       "    bank.counters[2].twice();\n"
                       "    *bank.counters[0].where() += 100;\n"
                       "    return bank.sum() * 1000 + bank.total.count * 10 + (int)bank.history[1];\n"
                       "}\n"
                       "int passed() { Bank b; return b.passThis() + b.total.count + b.counters[1].count; }\n",
                       &errors);
    INFO(errors);
    REQUIRE(jit);
    CHECK(lookup<int()>(*jit, "run")() == 109 * 1000 + 7 * 10 + 2);   // counters 101, 2, 2, 4; total 7
    CHECK(lookup<int()>(*jit, "passed")() == 10 + 10 + 10 + 10);
}

TEST_CASE("a module starts at 0 wherever it lives, is copied by =, and goes to functions by pointer",
          "[codegen][module]")
{
    std::string errors;
    auto jit = compile(std::string(kOnePole)
                           + "module Frame\n"
                             "{\n"
                             "    float gain;\n"
                             "    float4 samples;\n"   // aligned to 16 bytes, so the module is 32
                             "    Frame* next;\n"      // a pointer to its own kind
                             "};\n"
                             "int sizes() { return sizeof(OnePole) * 100 + sizeof(Frame); }\n"
                             "float copies()\n"
                             "{\n"
                             "    OnePole a;\n"
                             "    a.init(1000, 48000);\n"
                             "    a.tick(1);\n"
                             "    OnePole b = a;\n"   // a copy, which changes on its own from here
                             "    b.tick(1);\n"
                             "    OnePole c;\n"
                             "    c = b;\n"
                             "    c.y = 10;\n"
                             "    return (a.y < b.y) + 10 * (b.y < c.y) + 100 * (a.k == c.k);\n"
                             "}\n"
                             "float heap()\n"
                             "{\n"
                             "    Frame* frames = calloc(3, sizeof(Frame));\n"   // zeroed, like every module
                             "    frames[0].next = frames + 1;\n"
                             "    frames[1].next = &frames[2];\n"
                             "    frames[2].gain = 0.5f;\n"
                             "    frames[2].samples.y = 3;\n"
                             "    float r = frames[0].next->next->gain * frames->next->next->samples.y;\n"
                             "    r += frames[1].samples.x + (frames[2].next == frames[2].next);\n"
                             "    free(frames);\n"
                             "    return r;\n"
                             "}\n"
                             "float chooses(OnePole* a, OnePole* b, bool first) { return (first ? a : b)->y; }\n",
                       &errors);
    INFO(errors);
    REQUIRE(jit);
    CHECK(lookup<int()>(*jit, "sizes")() == 8 * 100 + 48);   // 4, padding to 16, 16, 8, padding to a multiple of 16
    CHECK(lookup<float()>(*jit, "copies")() == 111.0f);
    CHECK(lookup<float()>(*jit, "heap")() == 2.5f);
    OnePole a{1.0f, 0.0f};
    OnePole b{2.0f, 0.0f};
    auto chooses = lookup<float(OnePole*, OnePole*, bool)>(*jit, "chooses");
    CHECK(chooses(&a, &b, true) == 1.0f);
    CHECK(chooses(&a, &b, false) == 2.0f);
}

TEST_CASE("a nonblocking method calls only what cannot allocate, its module's methods too", "[codegen][module]")
{
    std::string errors;
    REQUIRE(compile(std::string(kOnePole)
                        + "module Chain\n"
                          "{\n"
                          "    OnePole a;\n"
                          "    OnePole b;\n"
                          "    float tick(float x) [[clang::nonblocking]] { return b.tick(through(x)); }\n"
                          "    float through(float x) { return a.tick(x); }\n"   // unmarked, inferred
                          "};\n",
                    &errors));
    CHECK(errorsOf("module Line\n"
                   "{\n"
                   "    float* data;\n"
                   "    void grow(int n) { data = calloc(n, sizeof(float)); }\n"
                   "    void process(int n) [[clang::nonblocking]] { grow(n); }\n"
                   "};\n")
          == "test.fm:5:50: error: function with 'nonblocking' attribute must not call non-'nonblocking' function "
             "'Line::grow'\n"
             "test.fm:4:31: note: function cannot be inferred 'nonblocking' because it calls non-'nonblocking' "
             "function 'calloc'\n");
}

TEST_CASE("module errors are reported as clang reports them", "[codegen][module]")
{
    CHECK(errorsOf("module M\n"
                   "{\n"
                   "    float y = 1;\n"
                   "    const float c;\n"
                   "    M self;\n"
                   "    float y;\n"
                   "    float tick(float x) { return y += x; }\n"
                   "    float tick;\n"
                   "    float calls() { return tick(1, 2) + z; }\n"
                   "    float self2() { return this->q; }\n"
                   "};\n"
                   "module M { float a; };\n")
          == "test.fm:3:11: error: a module's fields start at 0 and take no initializer: set 'y' in a method, such "
             "as init\n"
             "test.fm:4:17: error: a module's fields cannot be const: they start at 0, and a method sets them\n"
             "test.fm:5:7: error: field has incomplete type 'M': a module holds only modules declared before it (and "
             "pointers)\n"
             "test.fm:6:11: error: duplicate member 'y'\n"
             "test.fm:7:11: error: duplicate member 'tick'\n"
             "test.fm:9:28: error: too many arguments to function call, expected 1, have 2\n"
             "test.fm:9:41: error: use of undeclared identifier 'z'\n"
             "test.fm:10:32: error: no member named 'q' in 'M'\n"
             "test.fm:12:8: error: redefinition of 'M'\n");

    CHECK(errorsOf("module M { float y; float tick(float x) { return x; } };\n"
                   "M make() { M m; return m; }\n"
                   "void take(M m) { }\n"
                   "float f(M* p, const M* q, float v)\n"
                   "{\n"
                   "    M m;\n"
                   "    m = 3;\n"
                   "    p.y = 1;\n"
                   "    v->y = 1;\n"
                   "    m->y = 1;\n"
                   "    q->y = 1;\n"
                   "    q->tick(1);\n"
                   "    m.nope = 1;\n"
                   "    m.tick;\n"
                   "    m.y();\n"
                   "    M copy = {1};\n"
                   "    float s = m ? 1 : 0;\n"
                   "    float t = (v > 0 ? m : m).y;\n"
                   "    float w = this->y;\n"
                   "    return m + 1;\n"
                   "}\n")
          == "test.fm:2:3: error: FMinus returns no modules: write the result through a pointer, 'M *'\n"
             "test.fm:3:13: error: FMinus passes a module by pointer: make the parameter 'M *'\n"
             "test.fm:7:7: error: assigning to 'M' from incompatible type 'int'\n"
             "test.fm:8:6: error: member reference type 'M *' is a pointer; did you mean to use '->'?\n"
             "test.fm:9:6: error: member reference type 'float' is not a pointer\n"
             "test.fm:10:6: error: member reference type 'M' is not a pointer; did you mean to use '.'?\n"
             "test.fm:11:10: error: read-only variable is not assignable\n"
             "test.fm:12:8: error: 'this' argument to member function 'tick' has type 'const M', but function is not "
             "marked const\n"
             "test.fm:13:6: error: no member named 'nope' in 'M'\n"
             "test.fm:14:6: error: reference to non-static member function must be called\n"
             "test.fm:15:7: error: called object type 'float' is not a function or function pointer\n"
             "test.fm:16:7: error: a module takes no initializer list: its fields start at 0 (set them in a method, "
             "such as init)\n"
             "test.fm:17:17: error: statement requires expression of scalar type ('M' invalid)\n"
             "test.fm:18:22: error: FMinus's ?: does not choose between modules: choose between pointers, c ? &a : &b\n"
             "test.fm:19:15: error: invalid use of 'this' outside of a non-static member function\n"
             "test.fm:19:19: error: member reference base type 'void' is not a structure or union\n"
             "test.fm:20:14: error: invalid operands to binary expression ('M' and 'int')\n");
}
