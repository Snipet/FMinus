// tests/unit/BitwiseTest.cpp: C's bitwise operators on int (extending.md, recipe 25): &, |, ^ and ~, the shifts << and
// >> (defined for every count and value: the count's low five bits, two's complement bits shifted out, the sign shifted
// in), the compound assignments, & still C's address-of where it is unary, and the errors, run through the JIT. Their
// precedence is FrontendTest.cpp's.
#include "Compile.h"

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <cstdint>
#include <limits>
#include <string>

using namespace fminus::test;

namespace
{
    constexpr int kMin = std::numeric_limits<int>::min();
    constexpr int kMax = std::numeric_limits<int>::max();

    // The shifts as FMinus defines them, in C++ (where << of a negative int is defined since C++20, and >> is
    // arithmetic): by the count's low five bits.
    int shiftLeft(int a, int n)
    {
        return static_cast<int>(static_cast<std::uint32_t>(a) << (n & 31));
    }

    int shiftRight(int a, int n)
    {
        return a >> (n & 31);
    }
} // namespace

TEST_CASE("&, |, ^ and ~ are C's on ints, and a bool takes part as an int", "[codegen][bitwise]")
{
    std::string errors;
    auto jit = compile("int bitand(int a, int b) { return a & b; }\n"
                       "int bitor(int a, int b) { return a | b; }\n"
                       "int bitxor(int a, int b) { return a ^ b; }\n"
                       "int complement(int a) { return ~a; }\n"
                       "int bools(bool a, bool b) { return (a & b) + (a | b) * 2 + (a ^ b) * 4 + ~a * 8; }\n"
                       "int both(int a, int b) { return (a < b) & (b < 10); }\n"
                       "int folded() { return (0x55 & 0xF0) + (0x50 | 0x05) * 1000 + (~0 ^ 0xFF) * 1000000; }\n",
                       &errors);
    INFO(errors);
    REQUIRE(jit);
    auto bitand_ = lookup<int(int, int)>(*jit, "bitand");
    auto bitor_ = lookup<int(int, int)>(*jit, "bitor");
    auto bitxor_ = lookup<int(int, int)>(*jit, "bitxor");
    auto complement = lookup<int(int)>(*jit, "complement");
    const std::array values{0, 1, -1, 2, -2, 0x55555555, 0x0F0F0F0F, 12345, -98765, kMax, kMin};
    for (const int a : values)
    {
        INFO("a = " << a);
        CHECK(complement(a) == ~a);
        for (const int b : values)
        {
            INFO("b = " << b);
            CHECK(bitand_(a, b) == (a & b));
            CHECK(bitor_(a, b) == (a | b));
            CHECK(bitxor_(a, b) == (a ^ b));
        }
    }
    CHECK(complement(kMin) == kMax);
    CHECK(complement(0) == -1);
    CHECK(bitand_(kMin, -1) == kMin);
    CHECK(bitxor_(kMin, kMax) == -1);
    auto bools = lookup<int(bool, bool)>(*jit, "bools");
    CHECK(bools(true, false) == 0 + 2 + 4 - 16);   // ~true is -2, as in C: the bool is the int 1 first
    CHECK(bools(true, true) == 1 + 2 + 0 - 16);
    CHECK(bools(false, false) == -8);
    CHECK(lookup<int(int, int)>(*jit, "both")(3, 4) == 1);
    CHECK(lookup<int(int, int)>(*jit, "both")(3, 12) == 0);
    CHECK(lookup<int()>(*jit, "folded")() == 0x50 + 0x55 * 1000 + ~0xFF * 1000000);
}

TEST_CASE("<< and >> shift by the count's low five bits: << loses the bits shifted out, >> shifts the sign in",
          "[codegen][bitwise]")
{
    std::string errors;
    auto jit = compile("int shl(int a, int n) { return a << n; }\n"
                       "int shr(int a, int n) { return a >> n; }\n"
                       // The same shifts of constants, which the code generator folds: the same results.
                       "int one31() { return 1 << 31; }\n"
                       "int one32() { return 1 << 32; }\n"
                       "int oneMinus1() { return 1 << -1; }\n"
                       "int minusOne1() { return -1 << 1; }\n"
                       "int intoSign() { return 0x40000000 << 1; }\n"
                       "int minRight31() { return (-2147483647 - 1) >> 31; }\n"
                       "int minusEight() { return -8 >> 1; }\n"
                       "int minus1Right40() { return -1 >> 40; }\n"
                       "int byZero(int a) { return (a << 0) + (a >> 0); }\n",
                       &errors);
    INFO(errors);
    REQUIRE(jit);
    auto shl = lookup<int(int, int)>(*jit, "shl");
    auto shr = lookup<int(int, int)>(*jit, "shr");
    for (const int a : {0, 1, -1, 5, -8, 0x12345678, 0x40000000, kMax, kMin})
    {
        for (const int n : {0, 1, 4, 30, 31, 32, 33, 63, -1, -31, -32, kMax, kMin})
        {
            INFO(a << " shifted by " << n);
            CHECK(shl(a, n) == shiftLeft(a, n));
            CHECK(shr(a, n) == shiftRight(a, n));
        }
    }
    CHECK(shl(1, 31) == kMin);   // into the sign: defined, as two's complement has it
    CHECK(shl(1, 32) == 1);      // 32 & 31 is 0
    CHECK(shl(1, -1) == kMin);   // -1 & 31 is 31
    CHECK(shl(-1, 1) == -2);     // a negative value shifted left: defined
    CHECK(shl(kMax, 1) == -2);
    CHECK(shr(kMin, 31) == -1);  // arithmetic: the sign shifted in
    CHECK(shr(-8, 1) == -4);
    CHECK(shr(-1, 40) == -1);
    CHECK(shr(kMin, -1) == -1);
    CHECK(shr(kMax, 30) == 1);
    CHECK(lookup<int()>(*jit, "one31")() == kMin);
    CHECK(lookup<int()>(*jit, "one32")() == 1);
    CHECK(lookup<int()>(*jit, "oneMinus1")() == kMin);
    CHECK(lookup<int()>(*jit, "minusOne1")() == -2);
    CHECK(lookup<int()>(*jit, "intoSign")() == kMin);
    CHECK(lookup<int()>(*jit, "minRight31")() == -1);
    CHECK(lookup<int()>(*jit, "minusEight")() == -4);
    CHECK(lookup<int()>(*jit, "minus1Right40")() == -1);
    CHECK(lookup<int(int)>(*jit, "byZero")(-7) == -14);
}

TEST_CASE("&= |= ^= <<= and >>= are x = x op v, on any lvalue, and give the value stored", "[codegen][bitwise]")
{
    std::string errors;
    auto jit = compile("int steps(int a) { a |= 1; a <<= 2; a ^= 3; a &= 0xFF; a >>= 1; return a; }\n"
                       "int elements() { int k[2] = {6, 3}; k[0] &= k[1]; k[1] <<= 4; return k[0] * 100 + k[1]; }\n"
                       "int value(int a) { int b = (a <<= 1); return a * 10 + b; }\n"
                       "bool flag(bool a, bool b) { a &= b; return a; }\n"
                       "bool any(bool a) { a |= 2; return a; }\n"   // 1 | 2 is 3, which converts to true
                       "void low(int* p, int n) { for (int i = 0; i < n; ++i) p[i] &= 0xF; }\n"
                       "module Bits\n"
                       "{\n"
                       "    int set;\n"
                       "    int mark(int b) { set |= 1 << b; return set; }\n"
                       "    int clear(int b) { set &= ~(1 << b); return set; }\n"
                       "};\n"
                       "int marks() { Bits m; m.mark(0); m.mark(3); m.mark(31); return m.clear(0); }\n"
                       "int chained(int a, int b) { a &= b |= 0xF0; return a * 1000 + b; }\n",   // right to left
                       &errors);
    INFO(errors);
    REQUIRE(jit);
    CHECK(lookup<int(int)>(*jit, "steps")(5) == 11);   // 5 | 1 = 5, << 2 = 20, ^ 3 = 23, & 0xFF = 23, >> 1 = 11
    CHECK(lookup<int()>(*jit, "elements")() == 248);
    CHECK(lookup<int(int)>(*jit, "value")(3) == 66);
    CHECK(lookup<bool(bool, bool)>(*jit, "flag")(true, false) == false);
    CHECK(lookup<bool(bool, bool)>(*jit, "flag")(true, true) == true);
    CHECK(lookup<bool(bool)>(*jit, "any")(false) == true);
    std::array<int, 4> buffer{0x12, -1, 0x7F, 0x10};
    lookup<void(int*, int)>(*jit, "low")(buffer.data(), 4);
    CHECK(buffer == std::array{0x2, 0xF, 0xF, 0x0});
    CHECK(lookup<int()>(*jit, "marks")() == static_cast<int>((1u << 3) | (1u << 31)));
    CHECK(lookup<int(int, int)>(*jit, "chained")(0x3C, 0x0F) == 60255);   // b = 0xFF, then a = 0x3C & 0xFF
}

TEST_CASE("& is an address where it is unary, as in C, and && and & stay apart", "[codegen][bitwise]")
{
    std::string errors;
    auto jit = compile("int low(int a) { int* p = &a; return *p & 1; }\n"
                       "float second(float* p) { const float* q = &p[1]; return *q; }\n"
                       "int tight(int a, int b) { return a&b; }\n"
                       "int logical(int a, int b) { return a&&b; }\n"
                       "int masked(int a, int b) { return a&~b; }\n"
                       "int through(int a, int* p) { return a & *p; }\n"
                       "int negated(int a, int b) { return a & -b; }\n",
                       &errors);
    INFO(errors);
    REQUIRE(jit);
    CHECK(lookup<int(int)>(*jit, "low")(7) == 1);
    std::array<float, 2> two{1.0f, 2.5f};
    CHECK(lookup<float(float*)>(*jit, "second")(two.data()) == 2.5f);
    CHECK(lookup<int(int, int)>(*jit, "tight")(6, 3) == 2);
    CHECK(lookup<int(int, int)>(*jit, "logical")(6, 3) == 1);
    CHECK(lookup<int(int, int)>(*jit, "masked")(7, 2) == 5);
    int three = 3;
    CHECK(lookup<int(int, int*)>(*jit, "through")(6, &three) == 2);
    CHECK(lookup<int(int, int)>(*jit, "negated")(12, 8) == 8);   // -8 is ~7: 12 & -8 clears the low three bits
}

TEST_CASE("operators of mixed precedence compute as C groups them", "[codegen][bitwise]")
{
    std::string errors;
    auto jit = compile("int orAnd(int a, int b, int c) { return a | b & c; }\n"          // a | (b & c)
                       "int shiftSum(int a) { return a << 1 + 2; }\n"                      // a << 3
                       "bool notEquals(int a, int b) { return ~a == b; }\n"                // (~a) == b
                       "int andEquals(int a, int b, int c) { return a & b == c; }\n"       // a & (b == c)
                       "int xorOr(int a, int b, int c) { return a ^ b | c; }\n"            // (a ^ b) | c
                       "int orXor(int a, int b, int c) { return a | b ^ c; }\n"            // a | (b ^ c)
                       "int andXor(int a, int b, int c) { return a & b ^ c; }\n"           // (a & b) ^ c
                       "int shifts(int a) { return a >> 1 << 1; }\n"                       // (a >> 1) << 1
                       "bool below(int a, int b, int c) { return a < b << c; }\n"          // a < (b << c)
                       "int negShift(int a) { return -a >> 1; }\n",                        // (-a) >> 1
                       &errors);
    INFO(errors);
    REQUIRE(jit);
    CHECK(lookup<int(int, int, int)>(*jit, "orAnd")(4, 2, 1) == 4);
    CHECK(lookup<int(int)>(*jit, "shiftSum")(1) == 8);
    CHECK_FALSE(lookup<bool(int, int)>(*jit, "notEquals")(0, 0));
    CHECK(lookup<bool(int, int)>(*jit, "notEquals")(0, -1));
    CHECK(lookup<int(int, int, int)>(*jit, "andEquals")(1, 3, 3) == 1);
    CHECK(lookup<int(int, int, int)>(*jit, "xorOr")(1, 1, 1) == 1);
    CHECK(lookup<int(int, int, int)>(*jit, "orXor")(1, 1, 1) == 1);
    CHECK(lookup<int(int, int, int)>(*jit, "orXor")(0, 1, 1) == 0);
    CHECK(lookup<int(int, int, int)>(*jit, "andXor")(3, 1, 1) == 0);
    CHECK(lookup<int(int)>(*jit, "shifts")(7) == 6);
    CHECK(lookup<bool(int, int, int)>(*jit, "below")(3, 1, 2));
    CHECK(lookup<int(int)>(*jit, "negShift")(5) == -3);
}

TEST_CASE("the bitwise operators take ints: floats, vectors and pointers are clang's errors", "[codegen][bitwise]")
{
    CHECK(errorsOf("int f(float x, int i, float4 v, float* p, bool b)\n"
                   "{\n"
                   "    int a = x | 1;\n"
                   "    int c = i << 1.5f;\n"
                   "    int d = ~x;\n"
                   "    x &= 1;\n"
                   "    i ^= 2.0f;\n"
                   "    x <<= 1;\n"
                   "    int e = ~v;\n"
                   "    int g = v & 1;\n"
                   "    int h = p | 1;\n"
                   "    int k = ~p;\n"
                   "    p >>= 1;\n"
                   "    int m = ~b + (b ^ true);\n"   // a bool is an int here: no error
                   "    return a + c + d + e + g + h + k + m;\n"
                   "}\n")
          == "test.fm:3:15: error: invalid operands to binary expression ('float' and 'int')\n"
             "test.fm:4:15: error: invalid operands to binary expression ('int' and 'float')\n"
             "test.fm:5:13: error: invalid argument type 'float' to unary expression\n"
             "test.fm:6:7: error: invalid operands to binary expression ('float' and 'int')\n"
             "test.fm:7:7: error: invalid operands to binary expression ('int' and 'float')\n"
             "test.fm:8:7: error: invalid operands to binary expression ('float' and 'int')\n"
             "test.fm:9:13: error: invalid argument type 'float4' to unary expression\n"
             "test.fm:10:15: error: invalid operands to binary expression ('float4' and 'int')\n"
             "test.fm:11:15: error: invalid operands to binary expression ('float *' and 'int')\n"
             "test.fm:12:13: error: invalid argument type 'float *' to unary expression\n"
             "test.fm:13:7: error: invalid operands to binary expression ('float *' and 'int')\n");
}
