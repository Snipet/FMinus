// src/runtime/MathKernels.h: FMinus's own math kernels, sin, cos, tan, exp, tanh and log, for the vector math mode
// (runtime/Math.cpp). Each is one sequence of IEEE operations, written once as a template over the operations it uses
// (`O`, below), so that everything that computes it computes the same operations in the same order, and gives a float
// the same result: the scalar function, and each lane of a vector function, whatever the other lanes hold. Nothing in
// them depends on the target either:
//
//   - only IEEE 754's basic operations: add, subtract, multiply, divide, fused multiply-add, compare, the conversion of
//     an int to a float, and the bit operations of abs and copy-sign. No libm, no reciprocal or square-root estimate
//     (ARM's and x86's differ), no instruction with an approximate result. Every rounding is IEEE's to nearest, even
//     on a tie, so the result is the same on arm64 and x86-64, at any vector width.
//   - every fused multiply-add is an explicit O::fma, and no multiply and add are ever fused otherwise: each operation
//     is a call of its own, and the runtime builds with -ffp-contract=off. (An x86-64 without FMA, from before Haswell,
//     computes fma in the C library's fmaf, which C requires to round correctly: the same bits, slowly.)
//   - no branch, but to reduce an argument of sin, cos or tan past kLargeArgument (O::large): every special case is a
//     select, so a vector's lanes never take different paths, and a lane's result depends on that lane alone.
//   - under flush-to-zero (Program::Options::denormals), a subnormal argument gives what its signed zero gives and a
//     subnormal result is a zero, both when the processor computes and when a compiler folds a constant. LLVM folds an
//     add, subtract, multiply, divide or comparison flushing, and an fma not (ConstantFoldScalarCall). So a kernel
//     reads a subnormal argument only where either gives the same result, and an odd function's last operation, the
//     one that gives x for a tiny x, is an add (sin, tanh) or a divide (tan), never an fma.
//
// Each function's largest error over every float, against the exact result, and its special values are in
// docs/extending.md (recipe 23). MathTest.cpp checks both, and that each lane is the scalar function.
#pragma once

#include <bit>
#include <cstddef>
#include <cstdint>

namespace fminus::kernels
{
    // ---- The constants ----------------------------------------------------------------------------------------------
    // 1.5 * 2^23: a float of magnitude below 2^22 added to it is rounded to an integer (to even), which its low bits
    // then hold, and subtracting it again gives that integer as a float. Two adds instead of a rounding instruction,
    // the same on every target; and the integer's bits come with it, for the quadrant and for exp's scale.
    inline constexpr float kRound = 0x1.8p23f;

    // pi/2 in three floats whose sum is pi/2 to 72 bits: q pi/2 is subtracted in three fused multiply-adds (Cody and
    // Waite's reduction). The first is exact, so the argument loses nothing to cancellation.
    inline constexpr float kHalfPi1 = 0x1.921fb6p+0f;
    inline constexpr float kHalfPi2 = -0x1.777a5cp-25f;
    inline constexpr float kHalfPi3 = -0x1.ee59dap-50f;
    inline constexpr float kTwoOverPi = 0x1.45f306p-1f;  // 2 / pi

    // Past this magnitude sin, cos and tan reduce their argument exactly instead, out of line (largeKernel): three
    // floats of pi/2 no longer hold the reduction to a float's precision far beyond it. Audio's arguments, phases and
    // angular frequencies, are a few radians.
    inline constexpr float kLargeArgument = 0x1p+16f;

    // ln 2 in two floats: x - k ln2 is the first's fused multiply-add, exact, and the second's.
    inline constexpr float kLn2Hi = 0x1.62e43p-1f;
    inline constexpr float kLn2Lo = -0x1.05c61p-29f;
    inline constexpr float kLog2e = 0x1.715476p+0f;

    // exp's argument, clamped: below -104 the result rounds to 0 (half the least subnormal is exp(-103.97)), above 89
    // to infinity (exp(88.73) is past the greatest float).
    inline constexpr float kExpLow = -104.0f;
    inline constexpr float kExpHigh = 89.0f;

    // tanh: below atanh(1/2) a polynomial, above it (1 - e^-2|x|) / (1 + e^-2|x|), which loses nothing to cancellation
    // there; -2|x| is clamped at -87, where e^-2|x| is still a normal float.
    inline constexpr float kTanhSmall = 0x1.193ea8p-1f;   // 0.549306154
    inline constexpr float kTanhLow = -87.0f;

    inline constexpr std::uint32_t kSign = 0x80000000u;
    inline constexpr std::uint32_t kOne = 0x3f800000u;   // 1.0f's bits

    // The polynomials, lowest degree first: minimax fits (Remez's exchange) of each function's remainder, weighted to
    // its relative error, with float coefficients, as Scripts/fit-math-kernels.py prints them. Their own error is under
    // 0.1 ulp; the roundings of the operations make the rest (recipe 23).
    //
    // sin(r) = r + r^3 P(r^2) and cos(r) = 1 + r^2 Q(r^2) on [-pi/4, pi/4], for sin, cos and tan.
    inline constexpr float kSinP[] = {-0x1.555546p-3f, 0x1.110778p-7f, -0x1.995408p-13f};
    inline constexpr float kCosP[] = {-0x1p-1f, 0x1.55554ap-5f, -0x1.6c0c28p-10f, 0x1.99e80cp-16f};
    // e^r = 1 + r + r^2 Q(r) on [-ln2/2, ln2/2].
    inline constexpr float kExpQ[] = {0x1.fffffcp-2f, 0x1.55548cp-3f, 0x1.555858p-5f, 0x1.123dep-7f, 0x1.6ac74ep-10f};
    // log(1 + f) = f + f^2 P(f) on [sqrt(1/2) - 1, sqrt(2) - 1].
    inline constexpr float kLogP[] = {-0x1p-1f,       0x1.55554ep-2f, -0x1.ffffdap-3f, 0x1.99a126p-3f,
                                      -0x1.5564dep-3f, 0x1.2383dep-3f, -0x1.fb48ep-4f,  0x1.e1caf6p-4f,
                                      -0x1.e16c4cp-4f, 0x1.239ca4p-4f};
    // tanh(x) = x + x^3 T(x^2) on [0, atanh(1/2)].
    inline constexpr float kTanhT[] = {-0x1.55554ap-2f, 0x1.110ceep-3f, -0x1.b91e7ep-5f, 0x1.58e518p-6f,
                                       -0x1.9921a2p-8f};

    // Which out-of-line reduction O::large calls, for an argument past kLargeArgument.
    enum class Large
    {
        Sin,
        Cos,
        Tan,
    };

    // ---- The operations ---------------------------------------------------------------------------------------------
    // A kernel's `O` has the types F (floats, a lane each), I (32-bit integers, a lane each, wrapping) and M (a mask),
    // and these functions, each one IEEE operation (or an exact bit operation) a lane:
    //
    //   F c(float), I ci(uint32_t)                     a constant in every lane
    //   F add, sub, mul, div (F, F); F fma(F, F, F)    rounded once each; fma(a, b, c) is a * b + c
    //   F abs(F)                                       the sign bit cleared
    //   M lt, gt, eq, ge (F, F)                        ordered comparisons: false when either is NaN
    //   M signBit(I)                                   the lane's top bit is set
    //   F select(M, F, F)                              per lane: the first where the mask is set
    //   I bits(F), F fromBits(I)                       the same 32 bits, read as the other type
    //   I iadd, isub, iand, ixor (I, I); I shl, ashr (I, int)
    //   F toFloat(I)                                   the int (signed) converted, exact below 2^24
    //   F large(M, F x, F fast, Large)                 per lane: largeKernel(x) where the mask is set, else fast
    //
    // Scalar and Vector<N>, below, run them in C++.

    // A polynomial at z in Horner's form: the fewest operations, a fused multiply-add a coefficient.
    template <class O, std::size_t N>
    typename O::F horner(O& o, typename O::F z, const float (&c)[N])
    {
        typename O::F p = o.c(c[N - 1]);
        for (std::size_t i = N - 1; i-- > 0;)
            p = o.fma(p, z, o.c(c[i]));
        return p;
    }

    // The same polynomial in Estrin's form: pairs of coefficients first, joined by z^2, z^4, ...; one or two
    // operations more than Horner's, and a chain of fused multiply-adds about half as long, where the latency of a
    // recursion's math is what costs. `z2` is z * z, which the caller has already.
    template <class O, std::size_t N>
    typename O::F estrin(O& o, typename O::F z, typename O::F z2, const float (&c)[N])
    {
        using F = typename O::F;
        F terms[N];
        std::size_t count = 0;
        for (std::size_t i = 0; i < N; i += 2)
            terms[count++] = i + 1 < N ? o.fma(o.c(c[i + 1]), z, o.c(c[i])) : o.c(c[i]);
        F power = z2;
        while (count > 1)
        {
            std::size_t joined = 0;
            for (std::size_t i = 0; i < count; i += 2)
                terms[joined++] = i + 1 < count ? o.fma(terms[i + 1], power, terms[i]) : terms[i];
            count = joined;
            if (count > 1)
                power = o.mul(power, power);
        }
        return terms[0];
    }

    // x with its sign bit flipped where `sign`'s is set.
    template <class O>
    typename O::F flipSign(O& o, typename O::F x, typename O::I sign)
    {
        return o.fromBits(o.ixor(o.bits(x), o.iand(sign, o.ci(kSign))));
    }

    // sin(r) and cos(r) for r in [-pi/4, pi/4]. sin's last operation is the add of r, so a subnormal r gives its
    // signed zero under flush-to-zero, folded or not; cos(r) is near 1.
    template <class O>
    typename O::F sinPolynomial(O& o, typename O::F r, typename O::F z)
    {
        return o.add(r, o.mul(o.mul(r, z), horner(o, z, kSinP)));
    }

    template <class O>
    typename O::F cosPolynomial(O& o, typename O::F z)
    {
        return o.fma(z, horner(o, z, kCosP), o.c(1.0f));
    }

    // sin(x) and cos(x) of r in [-pi/4, pi/4] and the quadrant q: sin(r + q pi/2) is sin r, cos r, -sin r, -cos r for q
    // = 0 to 3 (mod 4), and cos(x) is sin(x + pi/2), the next quadrant's. `negate` is x's sign bit for sin, 0 for cos.
    template <class O>
    typename O::F sinOfQuadrant(O& o, typename O::F r, typename O::I q, typename O::I negate)
    {
        using F = typename O::F;
        const F z = o.mul(r, r);
        const F value = o.select(o.signBit(o.shl(q, 31)), cosPolynomial(o, z), sinPolynomial(o, r, z));
        return flipSign(o, value, o.ixor(o.shl(q, 30), negate));
    }

    // tan(r) for r in [-pi/4, pi/4] of a quadrant q: sin r / cos r when q is even, -cos r / sin r when it is odd (the
    // sign is the caller's). `odd` holds q's parity in its sign bit.
    template <class O>
    typename O::F tanPolynomial(O& o, typename O::F r, typename O::I odd)
    {
        using F = typename O::F;
        const F z = o.mul(r, r);
        const F s = sinPolynomial(o, r, z);
        const F c = cosPolynomial(o, z);
        const auto swap = o.signBit(odd);
        return o.div(o.select(swap, c, s), o.select(swap, s, c));
    }

    // |x| reduced by the nearest multiple q of pi/2, to r in [-pi/4, pi/4]: three fused multiply-adds, the first exact
    // (Cody and Waite). `t` is kRound + q, q in its low bits.
    template <class O>
    typename O::F reduce(O& o, typename O::F a, typename O::F& t)
    {
        using F = typename O::F;
        t = o.fma(a, o.c(kTwoOverPi), o.c(kRound));
        const F q = o.sub(t, o.c(kRound));
        F r = o.fma(q, o.c(-kHalfPi1), a);
        r = o.fma(q, o.c(-kHalfPi2), r);
        return o.fma(q, o.c(-kHalfPi3), r);
    }

    // sin(x) and cos(x), of |x| (sin's sign is x's).
    template <class O>
    typename O::F sin(O& o, typename O::F x)
    {
        using F = typename O::F;
        const F a = o.abs(x);
        F t;
        const F r = reduce(o, a, t);
        const F result = sinOfQuadrant(o, r, o.bits(t), o.bits(x));
        return o.large(o.gt(a, o.c(kLargeArgument)), x, result, Large::Sin);
    }

    template <class O>
    typename O::F cos(O& o, typename O::F x)
    {
        using F = typename O::F;
        const F a = o.abs(x);
        F t;
        const F r = reduce(o, a, t);
        const F result = sinOfQuadrant(o, r, o.iadd(o.bits(t), o.ci(1)), o.ci(0));
        return o.large(o.gt(a, o.c(kLargeArgument)), x, result, Large::Cos);
    }

    // tan(x): of |x|, with x's sign, and -1 / tan in the odd quadrants. Its divide is its last operation, which flushes
    // a subnormal x's zero.
    template <class O>
    typename O::F tan(O& o, typename O::F x)
    {
        using F = typename O::F;
        const F a = o.abs(x);
        F t;
        const F r = reduce(o, a, t);
        const typename O::I odd = o.shl(o.bits(t), 31);
        const F result = flipSign(o, tanPolynomial(o, r, odd), o.ixor(odd, o.bits(x)));
        return o.large(o.gt(a, o.c(kLargeArgument)), x, result, Large::Tan);
    }

    // e^x for x already within [kExpLow, kExpHigh] (or NaN): x = k ln2 + r, e^r by its polynomial, scaled by 2^k in
    // two steps, so that a result among the subnormals is rounded once, as IEEE rounds it (and is a zero under
    // flush-to-zero). Both scales are normal floats for every k from -150 to 128, and a NaN stays a NaN through both.
    template <class O>
    typename O::F expClamped(O& o, typename O::F x)
    {
        using F = typename O::F;
        using I = typename O::I;
        const F t = o.fma(x, o.c(kLog2e), o.c(kRound));
        const F k = o.sub(t, o.c(kRound));
        F r = o.fma(k, o.c(-kLn2Hi), x);
        r = o.fma(k, o.c(-kLn2Lo), r);
        const F r2 = o.mul(r, r);
        const F e = o.add(o.c(1.0f), o.fma(estrin(o, r, r2, kExpQ), r2, r));
        const I ki = o.isub(o.bits(t), o.bits(o.c(kRound)));
        const I k1 = o.ashr(ki, 1);
        const I k2 = o.isub(ki, k1);
        const F s1 = o.fromBits(o.iadd(o.shl(k1, 23), o.ci(kOne)));
        const F s2 = o.fromBits(o.iadd(o.shl(k2, 23), o.ci(kOne)));
        return o.mul(o.mul(e, s1), s2);
    }

    // e^x. The clamps keep a NaN (a comparison with it is false).
    template <class O>
    typename O::F exp(O& o, typename O::F x)
    {
        typename O::F clamped = o.select(o.gt(x, o.c(kExpHigh)), o.c(kExpHigh), x);
        clamped = o.select(o.lt(clamped, o.c(kExpLow)), o.c(kExpLow), clamped);
        return expClamped(o, clamped);
    }

    // tanh(x): of |x|, x's sign copied on. Below kTanhSmall its polynomial, ending with an add for a subnormal x;
    // above, (1 - u) / (1 + u) with u = e^-2|x|, which goes to 0, and the result to 1, as |x| grows. u needs no scale
    // below 2^-126: from e^-87 on (|x| over 43.5, where tanh is 1 to a float's precision) it stays there, so it is one
    // multiply. A NaN takes the polynomial's side of the select, and stays a NaN.
    template <class O>
    typename O::F tanh(O& o, typename O::F x)
    {
        using F = typename O::F;
        using I = typename O::I;
        const F a = o.abs(x);
        const F z = o.mul(a, a);
        const F small = o.add(a, o.mul(o.mul(a, z), horner(o, z, kTanhT)));
        F minus = o.mul(a, o.c(-2.0f));
        minus = o.select(o.lt(minus, o.c(kTanhLow)), o.c(kTanhLow), minus);
        const F t = o.fma(minus, o.c(kLog2e), o.c(kRound));
        const F k = o.sub(t, o.c(kRound));
        F r = o.fma(k, o.c(-kLn2Hi), minus);
        r = o.fma(k, o.c(-kLn2Lo), r);
        const F r2 = o.mul(r, r);
        const F e = o.add(o.c(1.0f), o.fma(estrin(o, r, r2, kExpQ), r2, r));
        const I scale = o.iadd(o.shl(o.isub(o.bits(t), o.bits(o.c(kRound))), 23), o.ci(kOne));
        const F u = o.mul(e, o.fromBits(scale));
        const F large = o.div(o.sub(o.c(1.0f), u), o.add(o.c(1.0f), u));
        const F result = o.select(o.ge(a, o.c(kTanhSmall)), large, small);
        return o.fromBits(o.ixor(o.bits(result), o.iand(o.bits(x), o.ci(kSign))));
    }

    // log(x): x = 2^e m with m in [sqrt(1/2), sqrt(2)), log(x) = e ln2 + log(1 + f) with f = m - 1. A subnormal x is
    // scaled by 2^23 first, a multiply, which also makes it a zero under flush-to-zero. Then the special cases: +inf
    // for +inf, -inf for a zero of either sign, and NaN for a NaN or anything negative.
    template <class O>
    typename O::F log(O& o, typename O::F x)
    {
        using F = typename O::F;
        using I = typename O::I;
        const auto tiny = o.lt(x, o.c(0x1p-126f));
        const F xs = o.select(tiny, o.mul(x, o.c(0x1p23f)), x);
        const I offset = o.isub(o.bits(xs), o.ci(0x3f3504f3u));   // sqrt(1/2)'s bits: m from sqrt(1/2) up
        const I e = o.ashr(offset, 23);
        const F m = o.fromBits(o.iadd(o.iand(offset, o.ci(0x007fffffu)), o.ci(0x3f3504f3u)));
        const F f = o.sub(m, o.c(1.0f));
        const F ef = o.sub(o.toFloat(e), o.select(tiny, o.c(23.0f), o.c(0.0f)));
        const F f2 = o.mul(f, f);
        const F lp = o.fma(f2, estrin(o, f, f2, kLogP), f);
        F result = o.fma(ef, o.c(kLn2Hi), o.fma(ef, o.c(kLn2Lo), lp));
        const F infinity = o.c(std::bit_cast<float>(0x7f800000u));
        result = o.select(o.eq(xs, infinity), infinity, result);
        result = o.select(o.eq(xs, o.c(0.0f)), o.c(-std::bit_cast<float>(0x7f800000u)), result);
        return o.select(o.ge(xs, o.c(0.0f)), result, o.c(std::bit_cast<float>(0x7fc00000u)));
    }

    // ---- The reduction of a large argument --------------------------------------------------------------------------
    // 2/pi's first 256 bits after the point, behind a word of zeros: x times them, from the bit of 2/pi that meets x's
    // lowest, is x 2/pi to 64 bits past the point, for any float (Payne and Hanek's reduction).
    inline constexpr std::uint32_t kTwoOverPiBits[] = {0x00000000u, 0xa2f9836eu, 0x4e441529u, 0xfc2757d1u,
                                                       0xf534ddc0u, 0xdb629599u, 0x3c439041u, 0xfe5163abu,
                                                       0xdebbc561u};

    // The reduction of a finite magnitude a past kLargeArgument, as reduce() does below it: a = q pi/2 + r, r in
    // [-pi/4, pi/4], q the nearest whole number of quarter turns (mod 4). In integers and one conversion of a double,
    // the same on every target.
    struct Reduced
    {
        float r;          // in radians
        std::uint32_t q;  // mod 4
    };

    inline Reduced reduceLarge(float a)
    {
        const std::uint32_t bits = std::bit_cast<std::uint32_t>(a);
        const std::uint32_t m = (bits & 0x007fffffu) | 0x00800000u;   // a = m 2^s
        const int s = static_cast<int>(bits >> 23) - 150;
        // The 128 bits of 2/pi from the one of weight 2^-(s - 1): a 2/pi = m W 2^-126 + (multiples of 4) + (< 2^-102).
        const int first = s - 1 + 31;   // its index in kTwoOverPiBits, where 32, word 1's top bit, is 2^-1's
        std::uint32_t w[4];
        for (int i = 0; i < 4; ++i)
        {
            const int word = first / 32 + i;
            const std::uint64_t pair =
                (static_cast<std::uint64_t>(kTwoOverPiBits[word]) << 32) | kTwoOverPiBits[word + 1];
            w[i] = static_cast<std::uint32_t>((pair << (first % 32)) >> 32);
        }
        // m W mod 2^128, in two 64-bit halves: bits 126 and 127 are q, the 64 below them a quarter turn's fraction.
        const std::uint64_t p0 = m * static_cast<std::uint64_t>(w[0]);
        const std::uint64_t p1 = m * static_cast<std::uint64_t>(w[1]);
        const std::uint64_t p2 = m * static_cast<std::uint64_t>(w[2]);
        const std::uint64_t p3 = m * static_cast<std::uint64_t>(w[3]);
        const std::uint64_t low = p3 + (p2 << 32);
        const std::uint64_t high = (p0 << 32) + p1 + (p2 >> 32) + (low < p3 ? 1u : 0u);
        const std::uint64_t fraction = (high << 2) | (low >> 62);
        // The nearest whole number: a fraction of a half or more rounds q up, and leaves r = fraction - 1.
        const std::uint32_t q = static_cast<std::uint32_t>(high >> 62) + static_cast<std::uint32_t>(fraction >> 63);
        const double r = static_cast<double>(static_cast<std::int64_t>(fraction)) * 0x1p-64 * 0x1.921fb54442d18p+0;
        return {static_cast<float>(r), q};
    }

    // ---- The operations in C++, for the runtime ---------------------------------------------------------------------
    float largeKernel(Large which, float x);

    // One float.
    struct Scalar
    {
        using F = float;
        using I = std::uint32_t;
        using M = bool;

        static F c(float v)
        {
            return v;
        }
        static I ci(std::uint32_t v)
        {
            return v;
        }
        static F add(F a, F b)
        {
            return a + b;
        }
        static F sub(F a, F b)
        {
            return a - b;
        }
        static F mul(F a, F b)
        {
            return a * b;
        }
        static F div(F a, F b)
        {
            return a / b;
        }
        static F fma(F a, F b, F c)
        {
            return __builtin_fmaf(a, b, c);
        }
        static F abs(F a)
        {
            return std::bit_cast<F>(std::bit_cast<I>(a) & ~kSign);
        }
        static M lt(F a, F b)
        {
            return a < b;
        }
        static M gt(F a, F b)
        {
            return a > b;
        }
        static M eq(F a, F b)
        {
            return a == b;
        }
        static M ge(F a, F b)
        {
            return a >= b;
        }
        static F select(M m, F a, F b)
        {
            return m ? a : b;
        }
        static I bits(F a)
        {
            return std::bit_cast<I>(a);
        }
        static F fromBits(I a)
        {
            return std::bit_cast<F>(a);
        }
        static I iadd(I a, I b)
        {
            return a + b;
        }
        static I isub(I a, I b)
        {
            return a - b;
        }
        static I iand(I a, I b)
        {
            return a & b;
        }
        static I ixor(I a, I b)
        {
            return a ^ b;
        }
        static I shl(I a, int n)
        {
            return a << n;
        }
        static I ashr(I a, int n)
        {
            return static_cast<I>(static_cast<std::int32_t>(a) >> n);
        }
        static M signBit(I a)
        {
            return static_cast<std::int32_t>(a) < 0;
        }
        static F toFloat(I a)
        {
            return static_cast<F>(static_cast<std::int32_t>(a));
        }
        static F large(M big, F x, F fast, Large which)
        {
            return big ? largeKernel(which, x) : fast;
        }
    };

    // N floats in a GCC and clang vector, lane by lane (N is 2 or 4: fminus_rt_float2, fminus_rt_float4).
    template <int N>
    struct Vector
    {
        using F = float __attribute__((vector_size(4 * N)));
        using I = std::uint32_t __attribute__((vector_size(4 * N)));
        using S = std::int32_t __attribute__((vector_size(4 * N)));
        using M = S;   // all ones in a lane where it holds

        static F c(float v)
        {
            return F{} + v;
        }
        static I ci(std::uint32_t v)
        {
            return I{} + v;
        }
        static F add(F a, F b)
        {
            return a + b;
        }
        static F sub(F a, F b)
        {
            return a - b;
        }
        static F mul(F a, F b)
        {
            return a * b;
        }
        static F div(F a, F b)
        {
            return a / b;
        }
        static F fma(F a, F b, F c)
        {
            return __builtin_elementwise_fma(a, b, c);
        }
        static F abs(F a)
        {
            return fromBits(bits(a) & ~kSign);
        }
        static M lt(F a, F b)
        {
            return a < b;
        }
        static M gt(F a, F b)
        {
            return a > b;
        }
        static M eq(F a, F b)
        {
            return a == b;
        }
        static M ge(F a, F b)
        {
            return a >= b;
        }
        static F select(M m, F a, F b)
        {
            const I mask = std::bit_cast<I>(m);
            return fromBits((bits(a) & mask) | (bits(b) & ~mask));
        }
        static I bits(F a)
        {
            return std::bit_cast<I>(a);
        }
        static F fromBits(I a)
        {
            return std::bit_cast<F>(a);
        }
        static I iadd(I a, I b)
        {
            return a + b;
        }
        static I isub(I a, I b)
        {
            return a - b;
        }
        static I iand(I a, I b)
        {
            return a & b;
        }
        static I ixor(I a, I b)
        {
            return a ^ b;
        }
        static I shl(I a, int n)
        {
            return a << n;
        }
        static I ashr(I a, int n)
        {
            return std::bit_cast<I>(std::bit_cast<S>(a) >> n);
        }
        static M signBit(I a)
        {
            return std::bit_cast<S>(a) < 0;
        }
        static F toFloat(I a)
        {
            return __builtin_convertvector(std::bit_cast<S>(a), F);
        }
        static F large(M big, F x, F fast, Large which)
        {
            for (int i = 0; i < N; ++i)
            {
                if (big[i] != 0)
                    fast[i] = largeKernel(which, x[i]);
            }
            return fast;
        }
    };

    // sin, cos or tan of x past kLargeArgument (or infinite: NaN), reduced exactly, then by the fast path's own
    // polynomials and signs.
    inline float largeKernel(Large which, float x)
    {
        Scalar o;
        const float a = Scalar::abs(x);
        if (!(a < std::bit_cast<float>(0x7f800000u)))
            return x - x;   // NaN for an infinity (and a NaN, which never comes here)
        const Reduced reduced = reduceLarge(a);
        switch (which)
        {
        case Large::Sin:
            return sinOfQuadrant(o, reduced.r, reduced.q, Scalar::bits(x));
        case Large::Cos:
            return sinOfQuadrant(o, reduced.r, reduced.q + 1, 0u);
        case Large::Tan:
        {
            const std::uint32_t odd = reduced.q << 31;
            return flipSign(o, tanPolynomial(o, reduced.r, odd), odd ^ Scalar::bits(x));
        }
        }
        return x;
    }
} // namespace fminus::kernels
