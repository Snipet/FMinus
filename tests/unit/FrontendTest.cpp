// tests/unit/FrontendTest.cpp: FMinus v0's scanner and parser (src/frontend/), through the syntax tree's dump.
#include "frontend/Frontend.h"

#include <catch2/catch_test_macros.hpp>

#include <string>
#include <string_view>

namespace
{
    // The program's dump, or its errors.
    std::string parsed(std::string_view source)
    {
        fminus::Diagnostics diagnostics("test.fm");
        const auto program = fminus::frontend::parse(source, diagnostics);
        return program ? fminus::ast::dump(*program) : diagnostics.format();
    }
} // namespace

TEST_CASE("the parser builds functions, declarations, statements and blocks", "[frontend]")
{
    CHECK(parsed("float gain(float x, const float g)\n"
                 "{\n"
                 "    float y = x * g; // a comment\n"
                 "    /* a block\n"
                 "       comment */\n"
                 "    y *= 2;\n"
                 "    { float y = 1; ; }\n"
                 "    return y;\n"
                 "}\n"
                 "float one() { const float a = 1, b; return a; }\n")
          == "(function float gain ((float x) (const float g))\n"
             "  (decl float y (* x g))\n"
             "  (*= y 2)\n"
             "  (block\n"
             "    (decl float y 1)\n"
             "    (empty))\n"
             "  (return y))\n"
             "(function float one ()\n"
             "  (decl const float a 1)\n"
             "  (decl const float b)\n"
             "  (return a))\n");
    CHECK(parsed("") == "");
}

TEST_CASE("prototypes declare, and may leave parameter names out", "[frontend]")
{
    CHECK(parsed("float half(float);\nfloat tanhf(const float x);\nfloat half(float x) { return x / 2; }\n")
          == "(declare float half ((float)))\n"
             "(declare float tanhf ((const float x)))\n"
             "(function float half ((float x))\n"
             "  (return (/ x 2)))\n");
}

TEST_CASE("operators have C's precedence and associativity", "[frontend]")
{
    CHECK(parsed("float f(float a, float b, float c) { return a - b - c * a / b; }")
          == "(function float f ((float a) (float b) (float c))\n  (return (- (- a b) (/ (* c a) b))))\n");
    CHECK(parsed("float f(float a, float b) { return -a * -(a + b) + +b; }")
          == "(function float f ((float a) (float b))\n  (return (+ (* (- a) (- (+ a b))) (+ b))))\n");
    // Assignment binds loosest and right to left; any expression parses as a target (the code generator decides).
    CHECK(parsed("float f(float a, float b) { a = b += 1 + 2; a + b = 3; return a; }")
          == "(function float f ((float a) (float b))\n  (= a (+= b (+ 1 2)))\n  (= (+ a b) 3)\n  (return a))\n");
    CHECK(parsed("float f(float a, float b) { a %= b; return a + b % a * b; }")
          == "(function float f ((float a) (float b))\n  (%= a b)\n  (return (+ a (* (% b a) b))))\n");
}

TEST_CASE("comparisons, !, && and ||, and ?: have C's precedence", "[frontend]")
{
    // Loosest first: assignment, ?:, ||, &&, equality, relational, additive, multiplicative, unary.
    CHECK(parsed("bool f(float a, float b) { return a + 1 < b * 2 == a >= b && !(a != b) || a <= b; }")
          == "(function bool f ((float a) (float b))\n"
             "  (return (|| (&& (== (< (+ a 1) (* b 2)) (>= a b)) (! (!= a b))) (<= a b))))\n");
    // ?: nests to the right, and an assignment takes the whole conditional.
    CHECK(parsed("float g(float a, float x) { x = a > 0 ? 1 : a < 0 ? -1 : 0; return x; }")
          == "(function float g ((float a) (float x))\n  (= x (?: (> a 0) 1 (?: (< a 0) (- 1) 0)))\n  (return x))\n");
    CHECK(parsed("bool h() { return !true || false; }") == "(function bool h ()\n  (return (|| (! true) false)))\n");
}

TEST_CASE("vector types, literals and swizzles parse as OpenCL C's", "[frontend]")
{
    // A literal is a parenthesised type, then its elements; a swizzle binds tighter than any operator, as in C.
    CHECK(parsed("float4 f(float2 a, float8 b) { return (float4)(a, b.s0, 1) * -a.yx.xxyy; }")
          == "(function float4 f ((float2 a) (float8 b))\n"
             "  (return (* (float4 a (. b s0) 1) (- (. (. a yx) xxyy)))))\n");
    CHECK(parsed("float f(float4 v) { v.xy = v.zw; return v.x; }")
          == "(function float f ((float4 v))\n  (= (. v xy) (. v zw))\n  (return (. v x)))\n");
}

TEST_CASE("poly types parse as a qualifier and a scalar type", "[frontend]")
{
    CHECK(parsed("poly float f(poly float a, const poly bool m) { poly float b = (poly float)(1); return m ? a : b; }")
          == "(function poly float f ((poly float a) (const poly bool m))\n"
             "  (decl poly float b (poly float 1))\n"
             "  (return (?: m a b)))\n");
}

TEST_CASE("constants keep their spelling: C's decimal, hex and integer forms", "[frontend]")
{
    CHECK(parsed("float f() { return fmaxf(fminf(1, .5), g(2e3, 1.e-2f, 3.25E+1F, 0x1.8p1, 0x1p-3f, 1.)); }")
          == "(function float f ()\n"
             "  (return (call fmaxf (call fminf 1 .5) (call g 2e3 1.e-2f 3.25E+1F 0x1.8p1 0x1p-3f 1.))))\n");
}

TEST_CASE("syntax and scanner errors carry their locations", "[frontend]")
{
    CHECK(parsed("float f(float x) {\n    return x + ;\n}") == "test.fm:2:16: error: syntax error, unexpected ;\n");
    CHECK(parsed("float f(float x) float { return x; }")
          == "test.fm:1:18: error: syntax error, unexpected float, expecting { or ;\n");
    CHECK(parsed("float f(float x) {\n  return x $ 2;\n}") == "test.fm:2:12: error: unexpected character '$'\n");
    CHECK(parsed("float f(float x) { return 1f; }") == "test.fm:1:27: error: invalid suffix 'f' on integer constant\n");
    CHECK(parsed("float f(float x) /* never\nclosed { return x; }")
          == "test.fm:1:18: error: unterminated /* comment\n");
    // Bison lists what it expected only when that is at most four tokens; a function can start five ways now.
    CHECK(parsed("fn f(x: float) -> float { return x; }")
          == "test.fm:1:1: error: syntax error, unexpected identifier\n");
}
