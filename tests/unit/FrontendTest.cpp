// tests/unit/FrontendTest.cpp: FMinus v0's scanner and parser (src/frontend/), through the syntax tree's dump.
#include "frontend/Frontend.h"
#include "frontend/Parser.tab.hpp"

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <cctype>
#include <string>
#include <string_view>
#include <utility>
#include <variant>
#include <vector>

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
    CHECK(parsed("float half(float);\nfloat atanf(const float x);\nfloat half(float x) { return x / 2; }\n")
          == "(declare float half ((float)))\n"
             "(declare float atanf ((const float x)))\n"
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

TEST_CASE("the bitwise operators and shifts have C's precedence, and & is unary or binary as in C", "[frontend]")
{
    // Loosest first: ||, &&, |, ^, &, equality, relational, shifts, additive; ~ is unary.
    CHECK(parsed("int f(int a, int b, int c) { return a | b & c; }")
          == "(function int f ((int a) (int b) (int c))\n  (return (| a (& b c))))\n");
    CHECK(parsed("int f(int a) { return a << 1 + 2; }") == "(function int f ((int a))\n  (return (<< a (+ 1 2))))\n");
    CHECK(parsed("bool f(int a, int b) { return ~a == b; }")
          == "(function bool f ((int a) (int b))\n  (return (== (~ a) b)))\n");
    CHECK(parsed("int f(int a, int b, int c) { return a & b == c || a ^ b | c && a < b >> c; }")
          == "(function int f ((int a) (int b) (int c))\n"
             "  (return (|| (& a (== b c)) (&& (| (^ a b) c) (< a (>> b c))))))\n");
    CHECK(parsed("int f(int a) { return a >> 1 << 2 & ~-a; }")
          == "(function int f ((int a))\n  (return (& (<< (>> a 1) 2) (~ (- a)))))\n");
    // & before an operand is the address, after one the operator; && is one token, as C's maximal munch has it.
    CHECK(parsed("int f(int a, int* p) { p = &a; return a&*p & &a == p && a; }")
          == "(function int f ((int a) (int * p))\n"
             "  (= p (& a))\n"
             "  (return (&& (& (& a (* p)) (== (& a) p)) a)))\n");
    // The compound assignments bind as = does: loosest, right to left.
    CHECK(parsed("int f(int a, int b) { a <<= b >>= 1; a &= 1; a |= 2; a ^= b; return a; }")
          == "(function int f ((int a) (int b))\n"
             "  (<<= a (>>= b 1))\n  (&= a 1)\n  (|= a 2)\n  (^= a b)\n  (return a))\n");
}

TEST_CASE("switch parses as C's, and case and default as C23's labels: block items, or a statement's", "[frontend]")
{
    // In a block, a label is an item of its own, so a declaration may follow one and one may end the block; where a
    // statement is expected, a label labels the statement after it.
    CHECK(parsed("int f(int x)\n"
                 "{\n"
                 "    switch (x)\n"
                 "    {\n"
                 "    case 1:\n"
                 "    case 2 + 1: x = 0; break;\n"
                 "    default: int y = x; x = y;\n"
                 "    case 4:\n"
                 "    }\n"
                 "    switch (x) case 1: case 2: return 1;\n"
                 "    if (x) default: x = 3;\n"
                 "    switch (x) ;\n"
                 "    return x ? 1 : 2;\n"
                 "}\n")
          == "(function int f ((int x))\n"
             "  (switch x\n"
             "    (block\n"
             "      (case 1)\n"
             "      (case (+ 2 1))\n"
             "      (= x 0)\n"
             "      (break)\n"
             "      (default)\n"
             "      (decl int y x)\n"
             "      (= x y)\n"
             "      (case 4)))\n"
             "  (switch x\n"
             "    (case 1\n"
             "      (case 2\n"
             "        (return 1))))\n"
             "  (if x\n"
             "    (default\n"
             "      (= x 3)))\n"
             "  (switch x\n"
             "    (empty))\n"
             "  (return (?: x 1 2)))\n");
    // A case's value is an expression up to its colon, ?: included; `default` is a keyword, and an identifier in an
    // attribute, as C23 reads a keyword there.
    CHECK(parsed("int f(int x) { switch (x) { case x ? 1 : 2: break; } return 0; }")
          == "(function int f ((int x))\n"
             "  (switch x\n"
             "    (block\n"
             "      (case (?: x 1 2))\n"
             "      (break)))\n"
             "  (return 0))\n");
    CHECK(parsed("module M { param int n [[fminus::default(3)]]; };")
          == "(module M\n  (param int n [[fminus::default(3)]]))\n");
    CHECK(parsed("int f(int x) { switch (x) { case 1 break; } }")
          == "test.fm:1:36: error: syntax error, unexpected break\n");
    CHECK(parsed("int f(int default) { return 0; }")
          == "test.fm:1:11: error: syntax error, unexpected default, expecting * or ) or , or identifier\n");
}

TEST_CASE("defer and _Defer parse as TS 25755's defer statement: a statement after the keyword", "[frontend]")
{
    CHECK(parsed("void f(int* p, int k)\n"
                 "{\n"
                 "    defer *p = 1;\n"
                 "    _Defer { int x = k; defer *p += x; }\n"
                 "    if (k) defer if (k > 1) *p = 2; else *p = 3;\n"
                 "    for (;;) defer defer k++;\n"
                 "    switch (k) { default: defer break; }\n"
                 "}\n")
          == "(function void f ((int * p) (int k))\n"
             "  (defer\n"
             "    (= (* p) 1))\n"
             "  (defer\n"
             "    (block\n"
             "      (decl int x k)\n"
             "      (defer\n"
             "        (+= (* p) x))))\n"
             "  (if k\n"
             "    (defer\n"
             "      (if (> k 1)\n"
             "        (= (* p) 2)\n"
             "        (= (* p) 3))))\n"
             "  (for () () ()\n"
             "    (defer\n"
             "      (defer\n"
             "        (post++ k))))\n"
             "  (switch k\n"
             "    (block\n"
             "      (default)\n"
             "      (defer\n"
             "        (break)))))\n");
    // A label after it parses, as clang parses one (a switch's jump into it is the code generator's error); a
    // declaration does not, for the TS defers a statement.
    CHECK(parsed("void f(int k) { switch (k) { defer case 1: k = 2; } }")
          == "(function void f ((int k))\n"
             "  (switch k\n"
             "    (block\n"
             "      (defer\n"
             "        (case 1\n"
             "          (= k 2))))))\n");
    CHECK(parsed("void f() { defer int x = 1; }") == "test.fm:1:18: error: syntax error, unexpected int\n");
    CHECK(parsed("void f() { int defer = 1; }")
          == "test.fm:1:16: error: syntax error, unexpected defer, expecting * or identifier\n");
    CHECK(parsed("module M { float x [[_Defer::_Defer]] [[ui::_Defer]]; };")
          == "(module M\n  (field float x [[_Defer::_Defer]] [[ui::_Defer]]))\n");
    CHECK(parsed("#define _Defer 1\n")
          == "test.fm:1:9: error: '_Defer' cannot be used as a macro name: it is one of FMinus's keywords\n");
}

TEST_CASE("serve, route and reply parse as FMinus's statements, a JSON literal as an expression", "[frontend]")
{
    CHECK(parsed("void f(http_server s)\n"
                 "{\n"
                 "    serve (s)\n"
                 "    {\n"
                 "        route \"POST /note/{n}\" (const int n [[fminus::range(0, 127)]], float g) { reply 204; }\n"
                 "        route \"GET /\" \"state\" { if (s) reply 200, json { g, \"a\" \"b\": [1, (2), {}], }; }\n"
                 "        route \"PUT /x\" () [[x::y]] { reply 200, \"text/plain\", \"%d\", 1; }\n"
                 "    }\n"
                 "    serve (s, 10 * 2) { route \"GET /y\" { reply 200, json [g, [], { \"z\": true }]; } }\n"
                 "    return json { };\n"
                 "}\n")
          == "(function void f ((http_server s))\n"
             "  (serve s\n"
             "    (route \"POST /note/{n}\" ((const int n [[fminus::range(0, 127)]]) (float g))\n"
             "      (reply 204))\n"
             "    (route \"GET /state\"\n"
             "      (if s\n"
             "        (reply 200 (json {\"g\" g \"ab\" [1 2 {}]}))))\n"
             "    (route \"PUT /x\" () [[x::y]]\n"
             "      (reply 200 \"text/plain\" \"%d\" 1)))\n"
             "  (serve s (* 10 2)\n"
             "    (route \"GET /y\"\n"
             "      (reply 200 (json [g [] {\"z\" true}]))))\n"
             "  (return (json {})))\n");
    // Only a route stands in a serve's braces; a reply has a status; the words are keywords, and attributes' names.
    CHECK(parsed("void f(http_server s) { serve (s) { int x = 1; route \"GET /\" { reply 1; } } }")
          == "test.fm:1:36: error: only a route may stand in a serve's braces: route \"METHOD /path\" (parameters) "
             "{ ... }\n");
    CHECK(parsed("void f() { reply; }") == "test.fm:1:17: error: expected expression\n");
    CHECK(parsed("void f() { int json = 1; }")
          == "test.fm:1:16: error: syntax error, unexpected json, expecting * or identifier\n");
    CHECK(parsed("void serve() { }") == "test.fm:1:6: error: syntax error, unexpected serve, expecting * or identifier\n");
    CHECK(parsed("void f() { g(json { \"a\" 1 }); }")
          == "test.fm:1:25: error: syntax error, unexpected number, expecting : or string literal\n");
    for (const char* word : {"serve", "route", "reply", "json"})
        CHECK(parsed(std::string("#define ") + word + " 1\n")
              == std::string("test.fm:1:9: error: '") + word
                     + "' cannot be used as a macro name: it is one of FMinus's keywords\n");
}

TEST_CASE("if and switch parse C2y's declaration in their head, and its third form as the second", "[frontend]")
{
    CHECK(parsed("int f(int x)\n"
                 "{\n"
                 "    if (int n = g(x), *p; n > 0) return n; else return -n;\n"
                 "    if (const json_doc doc = parse(x)) x = 1;\n"
                 "    switch (int k = x; k) { default: break; }\n"
                 "    switch (float4 v = x) ;\n"
                 "    return x;\n"
                 "}\n")
          == "(function int f ((int x))\n"
             "  (if (decl int n (call g x)) (decl int * p) (> n 0)\n"
             "    (return n)\n"
             "    (return (- n)))\n"
             "  (if (decl const json_doc doc (call parse x)) doc\n"
             "    (= x 1))\n"
             "  (switch (decl int k x) k\n"
             "    (block\n"
             "      (default)\n"
             "      (break)))\n"
             "  (switch (decl float4 v x) v\n"
             "    (empty))\n"
             "  (return x))\n");
    // What C2y refuses, in clang's words: a second declarator in the third form, at its comma; none with no
    // initialiser; a second declaration where the condition goes; and a first clause that is no declaration.
    CHECK(parsed("void f(int x)\n"
                 "{\n"
                 "    if (int a = 1, b = 2) x = 1;\n"
                 "    if (int c) x = 2;\n"
                 "    if (int d = 1; int e = 2) x = 3;\n"
                 "    switch (x = 1; x) {}\n"
                 "    if (; x) x = 4;\n"
                 "}\n")
          == "test.fm:3:18: error: multiple declarations are not allowed\n"
             "test.fm:4:13: error: variable declaration in condition must have an initializer\n"
             "test.fm:5:20: error: expected expression\n"
             "test.fm:6:13: error: first clause in condition must be a declaration\n"
             "test.fm:7:9: error: first clause in condition must be a declaration\n");
}

TEST_CASE("enumerations parse as C's, where they are among the functions and modules, and `enum Mode` is int",
          "[frontend]")
{
    CHECK(parsed("enum Wave { SAW, SQUARE = 4, TRIANGLE = SQUARE + 1, };\n"
                 "module Osc\n"
                 "{\n"
                 "    float phase;\n"
                 "    enum Shape { SINE, PULSE = -1 };\n"
                 "    param enum Wave wave [[fminus::range(0, 5)]];\n"
                 "};\n"
                 "enum { N = 8 };\n"
                 "enum Wave pick(enum Wave w, int i)\n"
                 "{\n"
                 "    enum Local { A };\n"
                 "    enum Local a = (enum Local)i;\n"
                 "    return w + a + sizeof(enum Wave);\n"
                 "}\n"
                 "const enum { LAST };\n")
          == "(enum Wave (SAW) (SQUARE 4) (TRIANGLE (+ SQUARE 1)))\n"
             "(module Osc\n"   // its enumerations and fields in order: a field's array sees those before it
             "  (field float phase)\n"
             "  (enum Shape (SINE) (PULSE (- 1)))\n"
             "  (param int wave [[fminus::range(0, 5)]]))\n"
             "(enum (N 8))\n"
             "(function int pick ((int w) (int i))\n"
             "  (enum Local (A))\n"
             "  (decl int a (cast int i))\n"
             "  (return (+ (+ w a) (sizeof int))))\n"
             "(enum (LAST))\n");
    // A tag is a type from its enumeration's closing brace on, in its scope: a block's ends with it.
    CHECK(parsed("int f() { { enum E { A }; enum E e = A; } enum E x = 0; return x; }")
          == "test.fm:1:48: error: ISO C forbids forward references to 'enum' types\n");
    CHECK(parsed("enum E { A = sizeof(enum E) };")
          == "test.fm:1:26: error: ISO C forbids forward references to 'enum' types\n");
    CHECK(parsed("enum E { A };\nint f() { enum E { B }; return B; }\nenum E { C };")
          == "test.fm:3:6: error: redefinition of 'E'\n"
             "test.fm:1:6: note: previous definition is here\n");
    CHECK(parsed("enum E { };") == "test.fm:1:10: error: use of empty enum\n");
}

TEST_CASE("an enumeration declares names of its type after its list, and C23's fixed underlying type parses",
          "[frontend]")
{
    // C's `enum E { A } e;`: the enumeration, then the declaration of `e`, whose type is `enum E` (int); a constant at
    // the top level, a field in a module (after the module's enumeration), a local in a block. The tag is a type from
    // the closing brace on, so the names after it may use it.
    CHECK(parsed("const enum Size { SMALL = 2, LARGE } sizes[2] = {SMALL, LARGE}, one = (enum Size)1;\n"
                 "module Voice { enum Stage { IDLE, ON } stage, stages[ON + 1]; };\n"
                 "int f() { enum { A } a = A, *p = &a; enum { B } ; return a; }\n")
          == "(enum Size (SMALL 2) (LARGE))\n"
             "(decl const int[2] sizes {SMALL LARGE})\n"
             "(decl const int one (cast int 1))\n"
             "(module Voice\n"
             "  (enum Stage (IDLE) (ON))\n"
             "  (field int stage)\n"
             "  (field int[(+ ON 1)] stages))\n"
             "(function int f ()\n"
             "  (enum (A))\n"
             "  (decl int a A)\n"
             "  (decl int * p (& a))\n"
             "  (enum (B))\n"
             "  (return a))\n");
    // C23's fixed underlying type, int or bool, after the tag; a tag declared ahead of its list with one is a type
    // from there on, and adds nothing to the tree.
    CHECK(parsed("enum Mode : int;\n"
                 "enum Mode first(enum Mode m) { return m; }\n"
                 "enum Mode : int { A, B };\n"
                 "enum Flag : bool { OFF, ON } flag;\n"
                 "enum : int { C };\n")
          == "(function int first ((int m))\n"
             "  (return m))\n"
             "(enum Mode : int (A) (B))\n"
             "(enum Flag : bool (OFF) (ON))\n"
             "(decl bool flag)\n"
             "(enum : int (C))\n");
    // The tags' errors, clang's: a type no integer, redeclarations that disagree on the fixed type (a note at the one
    // before), a definition twice, a declaration ahead without a tag or without a fixed type.
    CHECK(parsed("enum E : float { A };") == "test.fm:1:10: error: non-integral type 'float' is an invalid underlying "
                                             "type\n");
    CHECK(parsed("enum F { X };\nenum E : enum F { A };")
          == "test.fm:2:10: error: an enumerated type is no underlying type: C23 takes an integer type that is not an "
             "enumeration\n");
    CHECK(parsed("enum E : int;\nenum E : bool;")
          == "test.fm:2:6: error: enumeration redeclared with different underlying type 'bool' (was 'int')\n"
             "test.fm:1:6: note: previous declaration is here\n");
    CHECK(parsed("enum E : int;\nenum E { A };")
          == "test.fm:2:6: error: enumeration previously declared with fixed underlying type\n"
             "test.fm:1:6: note: previous declaration is here\n");
    CHECK(parsed("enum E { A };\nenum E : int;")
          == "test.fm:2:6: error: enumeration previously declared with nonfixed underlying type\n"
             "test.fm:1:6: note: previous declaration is here\n");
    CHECK(parsed("enum E : int;\nenum E : int { A };\nenum E : int { B };")
          == "test.fm:3:6: error: redefinition of 'E'\n"
             "test.fm:2:6: note: previous definition is here\n");
    CHECK(parsed("enum : int;") == "test.fm:1:11: error: unnamed enumeration must be a definition\n");
    CHECK(parsed("enum E;") == "test.fm:1:6: error: ISO C forbids forward references to 'enum' types\n");
    // A declaration ahead in an inner scope declares a tag of its own there, as C's does; one of a tag already
    // defined there declares nothing (C11 6.7.2.3p3).
    CHECK(parsed("enum E { A };\nenum E;\nint f() { enum E : bool; enum E : bool { B }; return B; }")
          == "(enum E (A))\n"
             "(function int f ()\n"
             "  (enum E : bool (B))\n"
             "  (return B))\n");
}

TEST_CASE("vector types, literals and swizzles parse as OpenCL C's", "[frontend]")
{
    // A literal is a parenthesised type, then its elements; a swizzle binds tighter than any operator, as in C.
    CHECK(parsed("float4 f(float2 a, float8 b) { return (float4)(a, b.s0, 1) * -a.yx.xxyy; }")
          == "(function float4 f ((float2 a) (float8 b))\n"
             "  (return (* (float4 a (. b s0) 1) (- (. (. a yx) xxyy)))))\n");
    CHECK(parsed("float f(float4 v) { v.xy = v.zw; return v.x; }")
          == "(function float f ((float4 v))\n  (= (. v xy) (. v zw))\n  (return (. v x)))\n");
    // C's literal, in braces: a trailing comma, or none at all, as C allows. There is no poly form.
    CHECK(parsed("float4 f(float a) { (float4){a, 2,}; (float4){}; return (float4){a}; }")
          == "(function float4 f ((float a))\n  (float4 {a 2})\n  (float4 {})\n  (return (float4 {a})))\n");
    CHECK(parsed("poly float f() { return (poly float){1}; }")
          == "test.fm:1:37: error: syntax error, unexpected {, expecting (\n");
}

TEST_CASE("poly types parse as a qualifier and a scalar type", "[frontend]")
{
    CHECK(parsed("poly float f(poly float a, const poly bool m) { poly float b = (poly float)(1); return m ? a : b; }")
          == "(function poly float f ((poly float a) (const poly bool m))\n"
             "  (decl poly float b (poly float 1))\n"
             "  (return (?: m a b)))\n");
}

TEST_CASE("double parses as C's: a type, a pointer's and an array's, and a cast; poly double is an error",
          "[frontend]")
{
    CHECK(parsed("double f(double x, const double* p) { double t[2] = {0.1, x}; return (double)(float)x + p[0]; }")
          == "(function double f ((double x) (const double * p))\n"
             "  (decl double[2] t {0.1 x})\n"
             "  (return (+ (cast double (cast float x)) ([] p 0))))\n");
    CHECK(parsed("poly double f(poly double x) { return x; }")
          == "test.fm:1:6: error: FMinus has poly float and poly bool, not 'poly double': a double is no vector lane\n"
             "test.fm:1:20: error: FMinus has poly float and poly bool, not 'poly double': a double is no vector "
             "lane\n");
}

TEST_CASE("the handle types are type words, as float4 is: a declaration's, a parameter's, a field's, an array's",
          "[frontend]")
{
    CHECK(parsed("json_doc f(http_response r, const http_request q, xml_doc x[4]) { http_server s, t[2][3]; "
                 "file_content c = g(file_listing_n); fft_plan p; return x[0]; }\n"
                 "module M { file_listing l; http_response pending[16]; };\n")
          == "(function json_doc f ((http_response r) (const http_request q) (xml_doc * x))\n"
             "  (decl http_server s)\n"
             "  (decl http_server[2][3] t)\n"
             "  (decl file_content c (call g file_listing_n))\n"
             "  (decl fft_plan p)\n"
             "  (return ([] x 0)))\n"
             "(module M\n"
             "  (field file_listing l)\n"
             "  (field http_response[16] pending))\n");
    // A pointer to one parses, as C's would; the code generator refuses it (HandleTest.cpp). Each word is a keyword,
    // and so no name of a variable, nor of a macro, but an attribute's name, as C23 reads a keyword there.
    CHECK(parsed("void f() { json_doc* p; }") == "(function void f ()\n  (decl json_doc * p))\n");
    CHECK(parsed("void f() { int json_doc = 1; }")
          == "test.fm:1:16: error: syntax error, unexpected handle type, expecting * or identifier\n");
    for (const char* word : {"http_response", "http_request", "http_server", "json_doc", "xml_doc", "file_content",
                             "file_listing", "fft_plan"})
    {
        const std::string attributes = std::string("[[") + word + "::" + word + "]] [[ui::" + word + "]]";
        CHECK(parsed("module M { float x " + attributes + "; };")
              == "(module M\n  (field float x " + attributes + "))\n");
        CHECK(parsed(std::string("#define ") + word + " 1\n")
              == "test.fm:1:9: error: '" + std::string(word)
                     + "' cannot be used as a macro name: it is one of FMinus's keywords\n");
    }
}

TEST_CASE("constants keep their spelling: C's decimal, hex and integer forms", "[frontend]")
{
    CHECK(parsed("float f() { return fmaxf(fminf(1, .5), g(2e3, 1.e-2f, 3.25E+1F, 0x1.8p1, 0x1p-3f, 1.)); }")
          == "(function float f ()\n"
             "  (return (call fmaxf (call fminf 1 .5) (call g 2e3 1.e-2f 3.25E+1F 0x1.8p1 0x1p-3f 1.))))\n");
    CHECK(parsed("int f() { return g(0, 010, 0x1F, 0Xff); }")   // an F in hex is a digit, not a suffix
          == "(function int f ()\n  (return (call g 0 010 0x1F 0Xff)))\n");
}

TEST_CASE("int, void, casts, ++ and -- parse as C's", "[frontend]")
{
    // A postfix ++ binds tighter than a unary -, and a cast binds as a unary operator does.
    CHECK(parsed("int f(int i, float x) { ++i; i--; return -i++ + (int)x * 2 + 0x1F; }")
          == "(function int f ((int i) (float x))\n"
             "  (pre++ i)\n"
             "  (post-- i)\n"
             "  (return (+ (+ (- (post++ i)) (* (cast int x) 2)) 0x1F)))\n");
    // void, `return;`, and C's empty parameter list, (void).
    CHECK(parsed("void f(void) { return; }") == "(function void f ()\n  (return))\n");
    // ++ is one token, the longest that matches, as in C: i+++i is i++ + i.
    CHECK(parsed("int g(int i) { return i+++i; }") == "(function int g ((int i))\n  (return (+ (post++ i) i)))\n");
}

TEST_CASE("pointers parse as C's: declarators, *, & and []", "[frontend]")
{
    // The * belongs to the declarator, as in C: `float *a, b;` is a pointer and a float. A const before the type is
    // the pointee's, one after the * the pointer's own.
    CHECK(parsed("void f(const float* restrict in, float* const out, int n) { float *a = out, b; *a = in[n] + b; }")
          == "(function void f ((const float * restrict in) (float * const out) (int n))\n"
             "  (decl float * a out)\n"
             "  (decl float b)\n"
             "  (= (* a) (+ ([] in n) b)))\n");
    // [] and ++ after their operand bind tighter than * and & before it.
    CHECK(parsed("float* g(float* p, float x) { *p++ = -p[1]; &x; return &p[2]; }")
          == "(function float * g ((float * p) (float x))\n"
             "  (= (* (post++ p)) (- ([] p 1)))\n"
             "  (& x)\n"
             "  (return (& ([] p 2))))\n");
}

TEST_CASE("attributes follow a function's parameters, as C23 writes them", "[frontend]")
{
    CHECK(parsed("void f(float* p) [[clang::nonblocking]] { }\nfloat g(float) [[clang::nonallocating, other]];\n")
          == "(function void f ((float * p)) [[clang::nonblocking]])\n"
             "(declare float g ((float)) [[clang::nonallocating]] [[other]])\n");
}

TEST_CASE("sizeof takes a type, as C's sizeof(type) does", "[frontend]")
{
    CHECK(parsed("int f() { return sizeof(float) + sizeof(const float4*) + sizeof(poly float); }")
          == "(function int f ()\n  (return (+ (+ (sizeof float) (sizeof const float4 *)) (sizeof poly float))))\n");
}

TEST_CASE("arrays parse as C's: a size after the name, a list, and a parameter that is a pointer", "[frontend]")
{
    // A size is an expression, kept as written: the code generator works it out (an integer constant expression).
    CHECK(parsed("void f(const float x[16], int k[]) { float a[4] = {1, 2,}, b; const int c[0x10]; a[1] = x[2]; }")
          == "(function void f ((const float * x) (int * k))\n"
             "  (decl float[4] a {1 2})\n"
             "  (decl float b)\n"
             "  (decl const int[0x10] c)\n"
             "  (= ([] a 1) ([] x 2)))\n");
    CHECK(parsed("enum { ROWS = 17, COLS = 12 };\n"
                 "module Bank { float hist[ROWS << 1]; enum { TAPS = 4 }; float taps[TAPS]; };\n"
                 "void f(float x[ROWS * COLS]) { float buf[ROWS * COLS]; const int t[] = {1, 2, 3}; float n[]; }")
          == "(enum (ROWS 17) (COLS 12))\n"
             "(module Bank\n"
             "  (field float[(<< ROWS 1)] hist)\n"
             "  (enum (TAPS 4))\n"
             "  (field float[TAPS] taps))\n"
             "(function void f ((float * x))\n"
             "  (decl float[(* ROWS COLS)] buf)\n"
             "  (decl const int[] t {1 2 3})\n"
             "  (decl float[] n))\n");
}

TEST_CASE("2-D arrays parse as C's: two sizes, rows braced in a list, and sizeof of an array type", "[frontend]")
{
    // Each size as written; a braced list's values in their braces, as written (C's rules place them later).
    CHECK(parsed("enum { ROWS = 4 };\n"
                 "const float table[2][3] = {{1, 2}, {}, 3,};\n"
                 "module Bank { float gain[ROWS][ROWS * 4]; };\n"
                 "int f() { int g[][2] = {{1, {2}}, 3}; float h[2][2]; h[1][0] = g[0][1];"
                 " return sizeof(float[4][16]) + sizeof(int[ROWS]); }")
          == "(enum (ROWS 4))\n"
             "(decl const float[2][3] table {{1 2} {} 3})\n"
             "(module Bank\n"
             "  (field float[ROWS][(* ROWS 4)] gain))\n"
             "(function int f ()\n"
             "  (decl int[][2] g {{1 {2}} 3})\n"
             "  (decl float[2][2] h)\n"
             "  (= ([] ([] h 1) 0) ([] ([] g 0) 1))\n"
             "  (return (+ (sizeof float[4][16]) (sizeof int[ROWS]))))\n");
    // A third dimension, a parameter of a 2-D array (a pointer to rows, which FMinus has no parameter of), and an
    // array of pointers in sizeof, at their places.
    CHECK(parsed("float a[2][3][4];\nvoid f(float g[2][3]) { }\nint s() { return sizeof(float*[2]); }")
          == "test.fm:1:14: error: FMinus's arrays have at most two dimensions\n"
             "test.fm:2:14: error: FMinus has no parameters of 2-D arrays, which would be pointers to rows: pass a row "
             "(a float*), or the module that holds the array\n"
             "test.fm:3:30: error: FMinus has no arrays of pointers\n");
}

TEST_CASE("top-level constants parse as C's file-scope declarations, where they are among the others", "[frontend]")
{
    // Before and after functions, modules and enumerations, each where it is; and a declaration of several names.
    CHECK(parsed("const float gain = 0.5f;\n"
                 "enum { N = 4 };\n"
                 "const int sizes[N] = {1, 2}, count = N;\n"
                 "module M { float x; };\n"
                 "const float table[] = {1, 2, 3,};\n"
                 "enum { K = 2 };\n"
                 "float f() { return table[K] * gain; }\n"
                 "const float4 lanes = (float4)(1);\n"
                 "float state;\n")
          == "(decl const float gain 0.5f)\n"
             "(enum (N 4))\n"
             "(decl const int[N] sizes {1 2})\n"
             "(decl const int count N)\n"
             "(module M\n"
             "  (field float x))\n"
             "(decl const float[] table {1 2 3})\n"
             "(enum (K 2))\n"
             "(function float f ()\n"
             "  (return (* ([] table K) gain)))\n"
             "(decl const float4 lanes (float4 1))\n"
             "(decl float state)\n");   // FMinus's error, the code generator's: a top-level declaration is const
}

TEST_CASE("modules parse into fields and methods, and a module's name is a type from its declaration on",
          "[frontend]")
{
    // The methods follow their module, as functions named Osc::tick; the `this` the parser gives them is not shown.
    CHECK(parsed("module Osc\n"
                 "{\n"
                 "    float phase, step;\n"
                 "    Osc* next;\n"
                 "    float tick() [[clang::nonblocking]] { phase += step; return this->phase; }\n"
                 "};\n"
                 "module Voice { Osc oscs[2]; float tick() { return oscs[0].tick() + oscs[1].phase; } };\n"
                 "float f(Voice* v, Osc o)\n"
                 "{\n"
                 "    Osc* p = &o;\n"
                 "    return v->tick() + p->next->tick() + o.phase * sizeof(Osc);\n"
                 "}\n")
          == "(module Osc\n"
             "  (field float phase)\n"
             "  (field float step)\n"
             "  (field Osc * next))\n"
             "(method float Osc::tick () [[clang::nonblocking]]\n"
             "  (+= phase step)\n"
             "  (return (-> this phase)))\n"
             "(module Voice\n"
             "  (field Osc[2] oscs))\n"
             "(method float Voice::tick ()\n"
             "  (return (+ (call. ([] oscs 0) tick) (. ([] oscs 1) phase))))\n"
             "(function float f ((Voice * v) (Osc o))\n"
             "  (decl Osc * p (& o))\n"
             "  (return (+ (+ (call-> v tick) (call-> (-> p next) tick)) (* (. o phase) (sizeof Osc)))))\n");
    // Before its declaration, a module's name is not a type: there, as in C, `M m;` is two identifiers.
    CHECK(parsed("float f() { M m; }\nmodule M { };\n")
          == "test.fm:1:15: error: syntax error, unexpected identifier\n");
}

TEST_CASE("a module's parameters parse with their attributes, and only in a module", "[frontend]")
{
    CHECK(parsed("module Lowpass\n"
                 "{\n"
                 "    param float cutoff [[fminus::range(20, 2e4)]] [[fminus::default(1000)]];\n"
                 "    param int mode [[fminus::range(-1, 0x10)]];\n"
                 "    param bool on;\n"
                 "    float y;\n"
                 "    float tick(float x) { return y += cutoff[0] * (x - y); }\n"
                 "};\n")
          == "(module Lowpass\n"
             "  (param float cutoff [[fminus::range(20, 2e4)]] [[fminus::default(1000)]])\n"
             "  (param int mode [[fminus::range(-1, 0x10)]])\n"
             "  (param bool on)\n"
             "  (field float y))\n"
             "(method float Lowpass::tick ((float x))\n"
             "  (return (+= y (* ([] cutoff 0) (- x y)))))\n");
    // A parameter takes no initializer (its default is an attribute), and one is a module's.
    CHECK(parsed("module A { param float x = 1; };")
          == "test.fm:1:26: error: syntax error, unexpected =, expecting [ or ;\n");
    CHECK(parsed("param float x;") == "test.fm:1:1: error: syntax error, unexpected param\n");
}

TEST_CASE("attributes follow a field's name, as C23 writes them, and take numbers and strings", "[frontend]")
{
    // After the name, before an array's sizes, in each declarator of a declaration and after an enumeration's list;
    // a parameter's take strings too, and C's adjacent string literals are one.
    CHECK(parsed("module Meter\n"
                 "{\n"
                 "    float level [[ui::unit(\"dB\"), ui::label(\"Out\" \"put\")]] [[fminus::range(-60, 0)]];\n"
                 "    float taps [[ui::hidden]] [16], grid [[ui::shape(\"rows\", 2)]] [2][3];\n"
                 "    enum Mode { A, B } mode [[ui::label(\"Mode\")]];\n"
                 "    param float gain [[fminus::range(-24, 12)]] [[ui::unit(\"dB\")]];\n"
                 "};\n")
          == "(module Meter\n"
             "  (field float level [[ui::unit(\"dB\")]] [[ui::label(\"Output\")]] [[fminus::range(-60, 0)]])\n"
             "  (field float[16] taps [[ui::hidden]])\n"
             "  (field float[2][3] grid [[ui::shape(\"rows\", 2)]])\n"
             "  (enum Mode (A) (B))\n"
             "  (field int mode [[ui::label(\"Mode\")]])\n"
             "  (param float gain [[fminus::range(-24, 12)]] [[ui::unit(\"dB\")]]))\n");
    // A local's and a constant's parse, for the code generator to refuse.
    CHECK(parsed("const float k [[ui::unit(\"dB\")]] = 1;\nfloat f() { float x [[a]] = k, *p [[b]]; return x; }\n")
          == "(decl const float k [[ui::unit(\"dB\")]] 1)\n"
             "(function float f ()\n"
             "  (decl float x [[a]] k)\n"
             "  (decl float * p [[b]])\n"
             "  (return x))\n");
    // C's escapes, resolved, and written back as C writes them: a quote, a backslash, \n and \t, a control character
    // in octal; hex and octal bytes and universal character names (in UTF-8: B5, then F0 9F 98 80) as themselves.
    CHECK(parsed(R"fm(module M { float x [[ui::text("q\"b\\s\n\t\x41\101\u00B5\U0001F600\1\a\?\'")]]; };)fm")
          == "(module M\n"
             "  (field float x [[ui::text(\"q\\\"b\\\\s\\n\\tAA\xC2\xB5\xF0\x9F\x98\x80\\001\\007?'\")]]))\n");

    // An attribute's namespace and name are identifiers, and C23 reads a keyword there as one: each of the parser's
    // tokens that is spelt as an identifier (its keywords, a new one's too), then a vector type and a module's name,
    // which the scanner makes types.
    using fminus::frontend::Parser;
    for (int kind = 0; kind < Parser::YYNTOKENS; ++kind)
    {
        const std::string word = Parser::symbol_name(static_cast<Parser::symbol_kind_type>(kind));
        const auto wordChar = [](char c) { return std::isalnum(static_cast<unsigned char>(c)) != 0 || c == '_'; };
        if (std::isalpha(static_cast<unsigned char>(word.front())) == 0 || !std::ranges::all_of(word, wordChar))
            continue;
        const std::string attributes = "[[" + word + "::" + word + "(1)]] [[ui::" + word + "]] [[" + word + "]]";
        CHECK(parsed("module M { float x " + attributes + "; };")
              == "(module M\n  (field float x " + attributes + "))\n");
    }
    CHECK(parsed("module meter { float v; };\nmodule M { float x [[meter::label(\"x\")]] [[float4::meter]]; };\n")
          == "(module meter\n  (field float v))\n"
             "(module M\n  (field float x [[meter::label(\"x\")]] [[float4::meter]]))\n");
}

TEST_CASE("a string literal is an expression to the parser, which knows where each of its bytes is spelt",
          "[frontend]")
{
    // C's adjacent literals are one, written back as one; the code generator decides where a literal may be
    // (StringTest.cpp), so a literal anywhere an expression goes parses.
    CHECK(parsed("int f(int k) { return __fminus_string_hash(\"/osc/\" \"%d\\n\", k); }\n"
                 "float g() { return \"x\" * 2; }\n")
          == "(function int f ((int k))\n"
             "  (return (call __fminus_string_hash \"/osc/%d\\n\" k)))\n"
             "(function float g ()\n"
             "  (return (* \"x\" 2)))\n");

    // Each byte's place: its character's, or its escape's backslash, in the literal that holds it.
    fminus::Diagnostics diagnostics("test.fm");
    const auto program =
        fminus::frontend::parse("int f() { return h(\"a\\x41\\u00B5\"\n             \"\\0z\"); }\n", diagnostics);
    REQUIRE(program);
    const auto& ret = std::get<fminus::ast::ReturnStmt>(program->functions.at(0).body.at(0).node);
    const auto& call = std::get<fminus::ast::CallExpr>(ret.value->node);
    const auto& literal = std::get<fminus::ast::StringExpr>(call.args.at(0)->node);
    CHECK(literal.value == std::string("aA\xC2\xB5\0z", 6));
    REQUIRE(literal.at.size() == 6);
    const std::vector<std::pair<int, int>> places{{1, 21}, {1, 22}, {1, 26}, {1, 26}, {2, 15}, {2, 17}};
    for (std::size_t i = 0; i < places.size(); ++i)
    {
        INFO(i);
        CHECK(literal.at[i].line == places[i].first);
        CHECK(literal.at[i].column == places[i].second);
    }
}

TEST_CASE("errors print in source order, each with its notes after it", "[frontend]")
{
    fminus::Diagnostics diagnostics("x.fm");
    diagnostics.error({2, 3}, "second");
    diagnostics.error({1, 1}, "first", {{{5, 6}, "why", {}}, {{1, 9}, "and why", {}}});
    CHECK(diagnostics.format()
          == "x.fm:1:1: error: first\nx.fm:5:6: note: why\nx.fm:1:9: note: and why\nx.fm:2:3: error: second\n");
}

TEST_CASE("syntax and scanner errors carry their locations", "[frontend]")
{
    CHECK(parsed("float f(float x) {\n    return x + ;\n}") == "test.fm:2:16: error: syntax error, unexpected ;\n");
    CHECK(parsed("float f(float x) float { return x; }")   // [ starts an attribute: [[clang::nonblocking]]
          == "test.fm:1:18: error: syntax error, unexpected float, expecting [ or { or ;\n");
    CHECK(parsed("float f(float x) {\n  return x $ 2;\n}") == "test.fm:2:12: error: unexpected character '$'\n");
    CHECK(parsed("float f(float x) { return 1f; }") == "test.fm:1:27: error: invalid suffix 'f' on integer constant\n");
    CHECK(parsed("float f(float x) /* never\nclosed { return x; }")
          == "test.fm:1:18: error: unterminated /* comment\n");
    // A string literal is C's: an attribute's argument, and an expression to the parser, which the code generator takes
    // only as a builtin's argument (StringTest.cpp).
    CHECK(parsed("module M { float x [[ui::label(\"never closed)]]; };")
          == "test.fm:1:32: error: missing terminating '\"' character\n");
    CHECK(parsed(R"fm(module M { float x [[ui::label("a\qb")]]; };)fm")
          == "test.fm:1:34: error: unknown escape sequence '\\q'\n");
    CHECK(parsed(R"fm(module M { float x [[ui::label("a\x100")]]; };)fm")
          == "test.fm:1:34: error: hex escape sequence out of range\n");
    CHECK(parsed(R"fm(module M { float x [[ui::label("a\xg")]]; };)fm")
          == "test.fm:1:34: error: \\x used with no following hex digits\n");
    CHECK(parsed(R"fm(module M { float x [[ui::label("\400")]]; };)fm")
          == "test.fm:1:33: error: octal escape sequence out of range\n");
    CHECK(parsed(R"fm(module M { float x [[ui::label("\u12")]]; };)fm")
          == "test.fm:1:33: error: incomplete universal character name\n");
    CHECK(parsed(R"fm(module M { float x [[ui::label("\uD800")]]; };)fm")
          == "test.fm:1:33: error: invalid universal character\n");
    // An attribute after an array's size would be its type's (C23), and a string is no attribute's name.
    CHECK(parsed("module M { float taps[16] [[ui::label(\"Taps\")]] [[ui::hidden]]; };")
          == "test.fm:1:29: error: 'ui::label' attribute cannot be applied to types: the attributes of 'taps' go after "
             "its name, before its size\n");
    CHECK(parsed("module M { float x [[\"dB\"]]; };")
          == "test.fm:1:22: error: syntax error, unexpected string literal\n");
    // Bison lists what it expected only when that is at most four tokens, and a function can start in eight ways.
    CHECK(parsed("fn f(x: float) -> float { return x; }")
          == "test.fm:1:1: error: syntax error, unexpected identifier\n");
}
