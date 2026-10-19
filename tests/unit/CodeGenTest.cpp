// tests/unit/CodeGenTest.cpp: FMinus v0's code generator (src/ir/CodeGen.cpp), run through the JIT (src/jit/Jit.h).
#include "frontend/Frontend.h"
#include "ir/CodeGen.h"
#include "jit/Jit.h"

#include <catch2/catch_test_macros.hpp>

#include <llvm/IR/LLVMContext.h>
#include <llvm/IR/Module.h>
#include <llvm/Support/Error.h>

#include <array>
#include <cmath>
#include <memory>
#include <string>
#include <string_view>

namespace
{
    // The program JIT-compiled at -O2, or nothing with its errors in `errors`.
    std::unique_ptr<fminus::jit::Jit> compile(std::string_view source, std::string* errors = nullptr)
    {
        fminus::Diagnostics diagnostics("test.fm");
        const auto program = fminus::frontend::parse(source, diagnostics);
        auto context = std::make_unique<llvm::LLVMContext>();
        std::unique_ptr<llvm::Module> module = program ? fminus::ir::lower(*program, *context, diagnostics) : nullptr;
        if (errors != nullptr)
            *errors = diagnostics.format();
        if (!module)
            return nullptr;
        auto jit = llvm::cantFail(fminus::jit::Jit::create(llvm::OptimizationLevel::O2));
        llvm::cantFail(jit->add(std::move(module), std::move(context)));
        return jit;
    }

    template <typename Function>
    Function* lookup(fminus::jit::Jit& jit, std::string_view name)
    {
        return llvm::cantFail(jit.lookup(name)).toPtr<Function*>();
    }

    std::string errorsOf(std::string_view source)
    {
        std::string errors;
        CHECK(compile(source, &errors) == nullptr);
        return errors;
    }
} // namespace

TEST_CASE("functions compute what they say", "[codegen]")
{
    auto jit = compile("float softclip(float x, const float drive)\n"
                       "{\n"
                       "    const float y = x * drive;\n"
                       "    return y / (1.0f + fabsf(y));\n"
                       "}\n"
                       "float chain(float x) { return softclip(half(x), 4); }\n"   // calls before definitions
                       "float half(float x) { return x / 2; }\n"
                       "float constant() { return -(1 - 3) * 2.5f + +1; }\n");
    REQUIRE(jit);
    CHECK(lookup<float(float, float)>(*jit, "softclip")(1.0f, 1.0f) == 0.5f);
    CHECK(lookup<float(float)>(*jit, "chain")(1.0f) == 2.0f / 3.0f);
    CHECK(lookup<float()>(*jit, "constant")() == 6.0f);
}

TEST_CASE("variables have C's semantics: assignable, scoped, zeroed when uninitialised", "[codegen]")
{
    auto jit = compile("float f(float x)\n"
                       "{\n"
                       "    float a, b = x;\n"          // a starts at 0
                       "    a += b * 2;\n"              // compound assignment
                       "    x = a = a - 1;\n"           // assignment is an expression, right to left
                       "    { float a = 100; b = a; }\n" // a block's a shadows, and ends with it
                       "    return a + b + x;\n"
                       "}\n"
                       "float g(float x) { x *= x; x -= 1; x /= 2; return x; }\n"   // parameters are assignable
                       "float h(float x) { return x; x = 7; return x; }\n");        // code after a return is accepted
    REQUIRE(jit);
    CHECK(lookup<float(float)>(*jit, "f")(3.0f) == 5.0f + 100.0f + 5.0f);
    CHECK(lookup<float(float)>(*jit, "g")(3.0f) == 4.0f);
    CHECK(lookup<float(float)>(*jit, "h")(2.0f) == 2.0f);
}

TEST_CASE("prototypes declare functions defined later, or elsewhere (external, resolved at link time)", "[codegen]")
{
    auto jit = compile("float half(float);\n"
                       "float tanhf(float x);\n"   // no definition: external, libm's (the JIT looks in the process)
                       "float sinf(float);\n"      // a builtin's prototype, as <math.h> has, changes nothing
                       "float f(float x) { return tanhf(half(x)) + sinf(0); }\n"
                       "float half(const float x) { return x / 2; }\n");
    REQUIRE(jit);
    CHECK(lookup<float(float)>(*jit, "f")(2.0f) == std::tanh(1.0f));
    CHECK(errorsOf("float f(float x);\n"
                   "float f(float x, float y) { return x; }\n"
                   "float sinf(float a, float b);\n"
                   "float g(float) { return 1; }\n"
                   "float k(float);\n"
                   "float k(float x) { return x; }\n"
                   "float k(float x) { return x; }\n")
          == "test.fm:2:7: error: conflicting types for 'f'\n"
             "test.fm:3:7: error: conflicting types for 'sinf'\n"
             "test.fm:4:9: error: parameter name omitted\n"
             "test.fm:7:7: error: redefinition of 'k'\n");
}

TEST_CASE("builtins are <math.h>'s float functions, lowered to LLVM's intrinsics", "[codegen]")
{
    auto jit = compile("float f(float x) { return sqrtf(x) + fabsf(-x) + floorf(x / 2) + fminf(x, 1) + fmaxf(x, 1); }\n"
                       "float g(float x) { return sinf(x) * sinf(x) + cosf(x) * cosf(x) + logf(expf(x)); }\n"
                        "float h(float x) { return tanhf(x); }\n");
    REQUIRE(jit);
    CHECK(lookup<float(float)>(*jit, "f")(9.0f) == 3.0f + 9.0f + 4.0f + 1.0f + 9.0f);
    CHECK(std::abs(lookup<float(float)>(*jit, "g")(0.25f) - 1.25f) < 1e-6f);
    CHECK(lookup<float(float)>(*jit, "h")(0.5f) == std::tanh(0.5f));
    CHECK(fminus::ir::builtins().size() == 10);
}

TEST_CASE("% is fmodf's remainder: the sign of the dividend", "[codegen]")
{
    auto jit = compile("float f(float x, float y) { return x % y * 2; }\n"
                       "float wrap(float phase) { phase %= 1; return phase; }\n");
    REQUIRE(jit);
    CHECK(lookup<float(float, float)>(*jit, "f")(7.5f, 2.0f) == 3.0f);    // (7.5 % 2) * 2: % binds as * and /
    CHECK(lookup<float(float, float)>(*jit, "f")(-7.5f, 2.0f) == -3.0f);  // the sign of the dividend
    CHECK(lookup<float(float)>(*jit, "wrap")(2.25f) == 0.25f);
}

TEST_CASE("a constant is rounded once, straight to float", "[codegen]")
{
    // Just above the midpoint of 1 and the next float: through double it would land on the midpoint and round down.
    auto jit = compile("float f() { return 1.0000000596046447753906250000000001; }\n"
                       "float g() { return 0x1.000002p0f + 0x1p-149; }\n");   // hex floats are exact
    REQUIRE(jit);
    CHECK(lookup<float()>(*jit, "f")() == std::nextafter(1.0f, 2.0f));
    CHECK(lookup<float()>(*jit, "g")() == std::nextafter(1.0f, 2.0f));
}

TEST_CASE("every semantic error is reported, in source order, as clang words it", "[codegen]")
{
    CHECK(errorsOf("float f(float x, float x)\n"
                   "{\n"
                   "    float y = x + z;\n"
                   "    float y = nope(x);\n"
                   "    const float k = 1;\n"
                   "    k = 2;\n"
                   "    x + 1 = 3;\n"
                   "    return sqrtf(x, y);\n"
                   "}\n"
                   "float f(float a) { return a; }\n"
                   "float sinf(float a) { return a; }\n"
                   "float g() { float big = 1e60f; }\n"
                   "float h() { return g(1) + q(); }\n")
          == "test.fm:1:24: error: redefinition of parameter 'x'\n"
             "test.fm:3:19: error: use of undeclared identifier 'z'\n"
             "test.fm:4:11: error: redefinition of 'y'\n"
             "test.fm:4:15: error: call to undeclared function 'nope'\n"
             "test.fm:6:7: error: cannot assign to variable 'k' with const-qualified type 'const float'\n"
             "test.fm:7:11: error: expression is not assignable\n"
             "test.fm:8:12: error: too many arguments to function call, expected 1, have 2\n"
             "test.fm:10:7: error: conflicting types for 'f'\n"
             "test.fm:11:7: error: 'sinf' is a builtin and cannot be redefined\n"
             "test.fm:12:7: error: non-void function 'g' does not return a value\n"
             "test.fm:12:25: error: magnitude of floating-point constant too large for type 'float'\n"
             "test.fm:13:20: error: too many arguments to function call, expected 0, have 1\n"
             "test.fm:13:27: error: call to undeclared function 'q'\n");
}

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

namespace
{
    // FMinus's float2 and float4 are clang's ext_vector_type at the C boundary: <2 x float> and <4 x float>, in one
    // register. (float8 is not: clang passes it through memory, so these tests keep float8 inside FMinus.)
    using float2 = float __attribute__((ext_vector_type(2)));
    using float4 = float __attribute__((ext_vector_type(4)));

    std::array<float, 4> lanesOf(float4 v)
    {
        return {v.x, v.y, v.z, v.w};
    }
} // namespace

TEST_CASE("float2, float4 and float8 are OpenCL C's vector types: literals, lanes, swizzles", "[codegen][simd]")
{
    std::string errors;
    auto jit = compile("float4 literal(float a) { return (float4)(a, 2, (float2)(3, 4)); }\n"   // lanes fill in order
                       "float4 splat(float a) { return (float4)(a); }\n"                       // one scalar: every lane
                       "float first_last(float4 v) { return v.x + v.w; }\n"
                       "float4 reverse(float4 v) { return v.wzyx; }\n"
                       "float4 numbered(float4 v) { return v.s3210 + v.xxyy; }\n"
                       "float2 middle(float4 v) { return v.yz; }\n"
                       "float4 set(float4 v) { v.x = 10; v.zw *= 2; return v; }\n"
                       "float2 swap(float2 v) { v.yx = v; return v; }\n"
                       "float4 zero() { float4 v; return v; }\n",   // indeterminate in C; FMinus zeroes it
                       &errors);
    INFO(errors);
    REQUIRE(jit);
    const float4 v = {1.0f, 2.0f, 3.0f, 4.0f};
    CHECK(lanesOf(lookup<float4(float)>(*jit, "literal")(7.0f)) == std::array{7.0f, 2.0f, 3.0f, 4.0f});
    CHECK(lanesOf(lookup<float4(float)>(*jit, "splat")(5.0f)) == std::array{5.0f, 5.0f, 5.0f, 5.0f});
    CHECK(lookup<float(float4)>(*jit, "first_last")(v) == 5.0f);
    CHECK(lanesOf(lookup<float4(float4)>(*jit, "reverse")(v)) == std::array{4.0f, 3.0f, 2.0f, 1.0f});
    CHECK(lanesOf(lookup<float4(float4)>(*jit, "numbered")(v)) == std::array{5.0f, 4.0f, 4.0f, 3.0f});
    const float2 yz = lookup<float2(float4)>(*jit, "middle")(v);
    CHECK((yz.x == 2.0f && yz.y == 3.0f));
    CHECK(lanesOf(lookup<float4(float4)>(*jit, "set")(v)) == std::array{10.0f, 2.0f, 6.0f, 8.0f});
    const float2 swapped = lookup<float2(float2)>(*jit, "swap")(float2{1.0f, 2.0f});
    CHECK((swapped.x == 2.0f && swapped.y == 1.0f));
    CHECK(lanesOf(lookup<float4()>(*jit, "zero")()) == std::array{0.0f, 0.0f, 0.0f, 0.0f});
}

TEST_CASE("vector arithmetic works lane by lane, broadcasting scalars, with builtins and reductions", "[codegen][simd]")
{
    std::string errors;
    auto jit = compile("float4 softclip4(float4 x, float drive)\n"
                       "{\n"
                       "    const float4 y = x * drive;\n"   // drive is broadcast to every lane
                       "    return y / (1.0f + fabsf(y));\n" // fabsf on a float4 is llvm.fabs.v4f32
                       "}\n"
                       "float4 wrap(float4 phase) { phase %= 1; return phase; }\n"
                       "float4 clip(float4 x) { return fminf(fmaxf(x, -1), 1); }\n"
                       "float sum(float4 v) { return reduce_add(v); }\n"
                       "float least(float4 v) { return reduce_min(v); }\n"
                       "float most(float4 v) { return reduce_max(v); }\n"
                       "float4 rectify(float4 x) { return x > 0 ? x : 0; }\n"   // a vector condition: per lane
                       "float4 halfwave(float4 x) { return x * (x > 0); }\n"    // a lane mask is 0 or 1
                       "float4 sines(float4 x) { return sinf(x); }\n"
                       "float wide(float a)\n"                                // float8: two NEON registers
                       "{\n"
                       "    const float8 v = (float8)(a) * (float8)(1, 2, 3, 4, 5, 6, 7, 8);\n"
                       "    return reduce_add(v + v.s76543210);\n"
                       "}\n",
                       &errors);
    INFO(errors);
    REQUIRE(jit);
    const float4 x = {-2.0f, -0.5f, 0.5f, 2.0f};
    std::array<float, 4> clipped{};
    for (int i = 0; i < 4; ++i)
        clipped[static_cast<std::size_t>(i)] = x[i] / (1.0f + std::fabs(x[i]));
    CHECK(lanesOf(lookup<float4(float4, float)>(*jit, "softclip4")(x, 1.0f)) == clipped);
    CHECK(lanesOf(lookup<float4(float4)>(*jit, "wrap")(float4{2.25f, -2.25f, 0.5f, 3.0f}))
          == std::array{0.25f, -0.25f, 0.5f, 0.0f});
    CHECK(lanesOf(lookup<float4(float4)>(*jit, "clip")(float4{-3.0f, -0.5f, 0.5f, 3.0f}))
          == std::array{-1.0f, -0.5f, 0.5f, 1.0f});
    const float4 v = {0.1f, 0.2f, 0.3f, 0.4f};
    CHECK(lookup<float(float4)>(*jit, "sum")(v) == ((0.1f + 0.2f) + 0.3f) + 0.4f);   // in lane order, exactly
    CHECK(lookup<float(float4)>(*jit, "least")(float4{3.0f, std::nanf(""), -1.0f, 2.0f}) == -1.0f);   // NaN ignored
    CHECK(lookup<float(float4)>(*jit, "most")(float4{3.0f, std::nanf(""), -1.0f, 2.0f}) == 3.0f);
    CHECK(lanesOf(lookup<float4(float4)>(*jit, "rectify")(x)) == std::array{0.0f, 0.0f, 0.5f, 2.0f});
    CHECK(lanesOf(lookup<float4(float4)>(*jit, "halfwave")(x)) == std::array{-0.0f, -0.0f, 0.5f, 2.0f});
    const float4 angles = {0.0f, 0.5f, 1.0f, 2.0f};
    CHECK(lanesOf(lookup<float4(float4)>(*jit, "sines")(angles))
          == std::array{std::sin(0.0f), std::sin(0.5f), std::sin(1.0f), std::sin(2.0f)});   // four calls to sinf
    CHECK(lookup<float(float)>(*jit, "wide")(1.0f) == 72.0f);
}

TEST_CASE("vector errors are reported as clang reports them", "[codegen][simd]")
{
    CHECK(errorsOf("float4 f(float4 a, float8 b, float x)\n"
                   "{\n"
                   "    float s = a;\n"
                   "    x = a;\n"
                   "    a + b;\n"
                   "    a.q;\n"
                   "    (float2)(1, 2).z;\n"
                   "    x.y;\n"
                   "    a.xxy;\n"
                   "    a.xx = (float2)(1);\n"
                   "    (float4)(1, 2, 3);\n"
                   "    (float4)(1, 2, 3, 4, 5);\n"
                   "    reduce_add(x);\n"
                   "    if (a) x = 1;\n"
                   "    a && x;\n"
                   "    b > 0 ? a : a;\n"
                   "    return b;\n"
                   "}\n"
                   "float g(float x) { return x; }\n"
                   "float h(float4 v) { return g(v.x) + g(v); }\n"   // a scalar broadcasts; a vector never narrows
                   "float r(float4 v) { return v; }\n")
          == "test.fm:3:11: error: initializing 'float' with an expression of incompatible type 'float4'\n"
             "test.fm:4:7: error: assigning to 'float' from incompatible type 'float4'\n"
             "test.fm:5:7: error: cannot convert between vector values of different size ('float4' and 'float8')\n"
             "test.fm:6:6: error: illegal vector component name 'q'\n"
             "test.fm:7:19: error: vector component access exceeds type 'float2'\n"
             "test.fm:8:6: error: member reference base type 'float' is not a structure or union\n"
             "test.fm:9:6: error: 3 components have no type: FMinus has float2, float4 and float8\n"
             "test.fm:10:10: error: vector is not assignable (contains duplicate components)\n"
             "test.fm:11:5: error: too few elements in vector initialization (expected 4 elements, have 3)\n"
             "test.fm:12:5: error: excess elements in vector initializer\n"
             "test.fm:13:5: error: the argument of 'reduce_add' must be a float vector, not 'float'\n"
             "test.fm:14:9: error: statement requires expression of scalar type ('float4' invalid)\n"
             "test.fm:15:7: error: a logical operator needs scalar operands, not 'float4'\n"
             "test.fm:16:11: error: vector condition type 'bool8' and result type 'float4' do not have the same number "
             "of elements\n"
             "test.fm:17:5: error: returning 'float8' from a function with incompatible result type 'float4'\n"
             "test.fm:20:37: error: passing 'float4' to parameter of incompatible type 'float'\n"
             "test.fm:21:21: error: returning 'float4' from a function with incompatible result type 'float'\n");
}
