// tests/unit/RecoveryTest.cpp: what the code generator reports once, or no longer (extending.md, recipe 32). An
// operand with an error of its own (an undeclared name, a call of no function) fails, and what takes it says nothing
// more, as clang's recovery says nothing more: an operator, a subscript, *, & and ->, ?:, a call, and a conversion
// where a value initialises, is assigned, passed or returned. A constant condition branches one way, so the end of a
// non-void function after a `while (1)` without a break, or after `if (1) return`, is reached by nothing. sizeof past
// an int is an error, not a wrapped int, and an enumeration constant of the file is not named as a builtin.
#include "Compile.h"

#include <catch2/catch_test_macros.hpp>

#include <string>
#include <utility>
#include <vector>

using namespace fminus::test;

TEST_CASE("an operand's own error is the only one: what takes it reports nothing more, as clang", "[codegen][recovery]")
{
    const std::vector<std::pair<const char*, const char*>> cases{
        // an operator, and what takes its result: *(p + y) is one error, not three
        {"float f(float* p) { return *(p + y); }", "test.fm:1:34: error: use of undeclared identifier 'y'\n"},
        {"bool f(float* p) { return y < p; }", "test.fm:1:27: error: use of undeclared identifier 'y'\n"},
        {"void f(float* q) { q += y; }", "test.fm:1:25: error: use of undeclared identifier 'y'\n"},
        // a subscript's index, a subscript's base, *, & of an rvalue, ->
        {"float f(float* p) { return p[y]; }", "test.fm:1:30: error: use of undeclared identifier 'y'\n"},
        {"float f(int i) { return y[i]; }", "test.fm:1:25: error: use of undeclared identifier 'y'\n"},
        {"float f() { return *y; }", "test.fm:1:21: error: use of undeclared identifier 'y'\n"},
        {"float* f(float a) { return &(a + y); }", "test.fm:1:34: error: use of undeclared identifier 'y'\n"},
        {"float f() { return y->x; }", "test.fm:1:20: error: use of undeclared identifier 'y'\n"},
        // a conversion: initialising, assigning, passing, returning, and the arms of ?:
        {"float* f() { float* q = y; return q; }", "test.fm:1:25: error: use of undeclared identifier 'y'\n"},
        {"void f(float* q) { q = y; }", "test.fm:1:24: error: use of undeclared identifier 'y'\n"},
        {"float g(float* p) { return p[0]; } float f() { return g(y); }",
         "test.fm:1:57: error: use of undeclared identifier 'y'\n"},
        {"float* f() { return y; }", "test.fm:1:21: error: use of undeclared identifier 'y'\n"},
        {"float* f(float* p, int c) { return c ? p : y; }", "test.fm:1:44: error: use of undeclared identifier 'y'\n"},
        {"float4 f(float4 v, int c) { return c ? v : y; }", "test.fm:1:44: error: use of undeclared identifier 'y'\n"},
        // a call of no function, a component of what it gives, and a unary operator of that
        {"float f() { return -nofn(1).x; }", "test.fm:1:21: error: call to undeclared function 'nofn'\n"},
        // a module's: initialised from one, a method through one, one's method
        {"module M { float a; float t() { return a; } };\n"
         "float f() { M m = y; return m.a + y->t() + y.t(); }",
         "test.fm:2:19: error: use of undeclared identifier 'y'\n"
         "test.fm:2:35: error: use of undeclared identifier 'y'\n"
         "test.fm:2:44: error: use of undeclared identifier 'y'\n"},
        // `this` outside a method is its own error, and -> of it says nothing more
        {"float f() { return this->y; }", "test.fm:1:20: error: invalid use of 'this' outside of a non-static member "
                                          "function\n"},
    };
    for (const auto& [source, expected] : cases)
    {
        INFO(source);
        CHECK(errorsOf(source) == expected);
    }

    // Operands without an error of their own still get the operator's.
    CHECK(errorsOf("float* f(float a) { return &(a + 1.0f); }")
          == "test.fm:1:28: error: cannot take the address of an rvalue of type 'float'\n");
    CHECK(errorsOf("float f(float* p, float* q) { float* r = p + q; return 0; }")
          == "test.fm:1:44: error: invalid operands to binary expression ('float *' and 'float *')\n");
    CHECK(errorsOf("float f(float* p) { float x = p; return x; }")
          == "test.fm:1:27: error: initializing 'float' with an expression of incompatible type 'float *'\n");
    // The error of an operator whose operands are fine fails it in turn: one error for the whole.
    CHECK(errorsOf("float f(float* p, float* q) { return *(p + q); }")
          == "test.fm:1:42: error: invalid operands to binary expression ('float *' and 'float *')\n");
}

TEST_CASE("a constant condition branches one way: after while (1) without a break, a function needs no return",
          "[codegen][recovery]")
{
    std::string errors;
    auto jit = compile("enum { RUNNING = 1 };\n"
                       "int spin(int n) { while (1) { if (n > 3) return n; ++n; } }\n"
                       "int count(int n) { for (;;) { if (n > 3) return n * 2; ++n; } }\n"
                       "int running(int n) { while (RUNNING) { if (n > 3) return n * 3; ++n; } }\n"
                       "float half(float x) { while (1.5f) { if (x < 1) return x; x *= 0.5f; } }\n"
                       "int taken(int n) { if (1) return n; }\n"
                       "int other(int n) { if (0) { } else return -n; }\n"
                       "int both(int n) { if (true) { while (true) { if (n > 0) return n; ++n; } } }\n",
                       &errors);
    INFO(errors);
    REQUIRE(jit);
    CHECK(errors.empty());
    CHECK(lookup<int(int)>(*jit, "spin")(0) == 4);
    CHECK(lookup<int(int)>(*jit, "count")(1) == 8);
    CHECK(lookup<int(int)>(*jit, "running")(2) == 12);
    CHECK(lookup<float(float)>(*jit, "half")(6.0f) == 0.75f);
    CHECK(lookup<int(int)>(*jit, "taken")(5) == 5);
    CHECK(lookup<int(int)>(*jit, "other")(5) == -5);
    CHECK(lookup<int(int)>(*jit, "both")(-2) == 1);

    // A break, a condition that is no constant, or a constant that skips the return: the end is reached.
    CHECK(errorsOf("int f(int n) { while (1) { if (n) break; } }")
          == "test.fm:1:5: error: non-void function 'f' does not return a value\n");
    CHECK(errorsOf("int f(int n) { while (n) { return 1; } }")
          == "test.fm:1:5: error: non-void function 'f' does not return a value\n");
    CHECK(errorsOf("int f(int n) { if (0) return n; }")
          == "test.fm:1:5: error: non-void function 'f' does not return a value\n");
    CHECK(errorsOf("int f(int n) { for (; 0;) return n; }")
          == "test.fm:1:5: error: non-void function 'f' does not return a value\n");
    // The branch not taken is still lowered, for its errors.
    CHECK(errorsOf("int f(int n) { if (0) return nope; return n; }")
          == "test.fm:1:30: error: use of undeclared identifier 'nope'\n");
}

TEST_CASE("sizeof past the greatest int is an error, not a wrapped int; a module's size is 64 bits",
          "[codegen][recovery]")
{
    // 2^29 floats and 4 more bytes: 2 GiB + 4, past an int (C's sizeof is a size_t, FMinus's an int).
    CHECK(errorsOf("module Big { float a[536870912]; float b; };\nint f() { return sizeof(Big); }")
          == "test.fm:2:18: error: 'Big' is too large for sizeof: 2147483652 bytes, and FMinus's sizeof is an "
             "'int'\n");
    // 16 GiB, which a 32-bit size would have made 0.
    CHECK(errorsOf("module Huge { float4 a[1073741824]; };\nint f() { return sizeof(Huge); }")
          == "test.fm:2:18: error: 'Huge' is too large for sizeof: 17179869184 bytes, and FMinus's sizeof is an "
             "'int'\n");
    std::string errors;
    auto jit = compile("module Most { float a[536870911]; };\n"   // 2^31 - 4 bytes: an int
                       "int most() { return sizeof(Most); }\n",
                       &errors);
    INFO(errors);
    REQUIRE(jit);
    CHECK(lookup<int()>(*jit, "most")() == 2147483644);
}

TEST_CASE("an enumeration constant of the file is not named as a builtin; a block's may be", "[codegen][recovery]")
{
    // C's file scope has <math.h>'s functions: clang's words after its #include, as for a function of the file.
    CHECK(errorsOf("enum { SAW, sinf, hash };\nint f() { return SAW; }")
          == "test.fm:1:13: error: redefinition of 'sinf'\n"
             "test.fm:1:19: error: redefinition of 'hash'\n");
    CHECK(errorsOf("module M { enum { fabsf }; };") == "test.fm:1:19: error: redefinition of 'fabsf'\n");
    std::string errors;
    auto jit = compile("int f() { enum { sinf = 3 }; return sinf; }", &errors);
    INFO(errors);
    REQUIRE(jit);
    CHECK(lookup<int()>(*jit, "f")() == 3);
}
