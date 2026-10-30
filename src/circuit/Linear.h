// src/circuit/Linear.h: a linear block at the host rate in closed form (report §6.2's M = 0 row, §8.6, §9.2): the
// matched design. The trapezoidal rule warps a block's response near Nyquist (the TS tone block 13.4 dB off at 44.1
// kHz); a block of one or two states can instead run a biquad (or a one-pole) whose poles are the analog ones mapped by
// impulse invariance and whose numerator matches the analog magnitude at DC, at the poles' frequency and near the top
// of the band (Vicanek, "Matched Second Order Digital Filters", 2016, with his Nyquist point moved to 0.83 of Nyquist:
// kMatchTop). It is computed from the block's own trapezoidal state-space, in float, wherever its coefficients are
// (b<N>_set for a block with pots or switches, init otherwise): the inverse bilinear transform of that state-space is
// the block's analog response exactly, in the variable s / 2 fs, so the design needs no rate of its own. The same
// arithmetic runs on doubles (the gate at generation, the tests) and on Vals (the emitted text), as Mna.h's derivation
// does.
#pragma once

#include "circuit/Emit.h"
#include "circuit/Mna.h"

#include <cmath>
#include <complex>
#include <numbers>
#include <optional>
#include <string>
#include <vector>

namespace fminus::circuit
{
    // A block's trapezoidal state-space, of order 1 or 2: x(n) = A x(n-1) + B u(n), y(n) = Cy x(n-1) + Dy u(n).
    template <typename T>
    struct TrForm
    {
        int order = 2;
        T a11 = T(0.0), a12 = T(0.0), a21 = T(0.0), a22 = T(0.0);
        T b1 = T(0.0), b2 = T(0.0);
        T c1 = T(0.0), c2 = T(0.0);
        T d = T(0.0);
    };

    // The matched design, run as transposed direct form II: y = b0 u + s1, s1 <- b1 u - a1 y + s2, s2 <- b2 u - a2 y
    // (order 1: b2 = a2 = 0).
    template <typename T>
    struct Matched
    {
        T b0 = T(0.0), b1 = T(0.0), b2 = T(0.0), a1 = T(0.0), a2 = T(0.0);
    };

    inline double expOf(double x)
    {
        return std::exp(x);
    }

    inline double sqrtOf(double x)
    {
        return std::sqrt(x);
    }

    inline double cosOf(double x)
    {
        return std::cos(x);
    }

    inline double whereNegative(double c, double a, double b)
    {
        return c < 0 ? a : b;
    }

    // The top match point, as a fraction of Nyquist: 0.83, the audio band's top at 48 kHz (20 kHz). Matched at Nyquist
    // itself (Vicanek's), a block whose response falls as a one-pole's does errs by 0.96 dB near 0.37 of the rate (an
    // RC low-pass at any corner above 300 Hz, the wah's tank, the TS tone block); matched here, by 0.35 to 0.38 dB.
    inline constexpr double kMatchTop = 0.83;

    // The design from the trapezoidal form, `polarity` (+1 or -1) the sign its numerator takes (a magnitude match
    // loses it: an inverting block's is -1). With s^ = s / 2 fs, the trapezoidal z^-1 = (1 - s^) / (1 + s^): the
    // analog response is (N0 + N1 s^ + N2 s^2) / (A0 + A1 s^ + A2 s^2), and a frequency w (radians a sample) is s^ = j
    // w / 2. The poles, by impulse invariance: s^ = -sigma +- sqrt(sigma^2 - w0^2) maps to e^(s T) = e^(2 s^). The
    // numerator's |B|^2 = B0 phi0 + B1 phi1 + B2 phi2 (Vicanek's: phi1 = sin^2(w / 2), phi0 = 1 - phi1, phi2 = 4 phi0
    // phi1) is |H|^2 |A|^2 at DC, at the poles' frequency (2 w0, or pi / 2 above a quarter of the rate) and at
    // kMatchTop pi: B0 directly, B1 and B2 by Cramer's rule; then Vicanek's factorisation. Order 1: the one-pole's
    // numerator, b0 + b1 z^-1, at DC and at kMatchTop pi.
    template <typename T>
    Matched<T> matchedDesign(const TrForm<T>& tr, double polarity)
    {
        const T one(1.0);
        const T two(2.0);
        // the top point's phi1 and phi0, and its s^ = j x, x^2 = top2
        const double top = kMatchTop * std::numbers::pi;
        const T topPhi1(std::sin(top / 2) * std::sin(top / 2));
        const T topPhi0(std::cos(top / 2) * std::cos(top / 2));
        const T top2(top * top / 4);
        Matched<T> m;
        if (tr.order == 1)
        {
            // the trapezoidal one-pole: (d + (c b - a d) z^-1) / (1 - a z^-1)
            const T beta1 = tr.c1 * tr.b1 - tr.a11 * tr.d;
            const T alpha1 = -tr.a11;
            const T a0 = one + alpha1;   // analog: A0 + A1 s^, N0 + N1 s^
            const T a1 = one - alpha1;
            const T n0 = tr.d + beta1;
            const T n1 = tr.d - beta1;
            m.a1 = -expOf(-(two * a0) / a1);   // the pole -A0 / A1, at e^(2 s^)
            const T h02 = (n0 * n0) / (a0 * a0);
            const T ht2 = (n0 * n0 + n1 * n1 * top2) / (a0 * a0 + a1 * a1 * top2);
            const T p0 = one + m.a1;
            const T p1 = one - m.a1;
            const T bc0 = h02 * (p0 * p0);
            const T bc1 = (ht2 * (p0 * p0 * topPhi0 + p1 * p1 * topPhi1) - bc0 * topPhi0) / topPhi1;
            const T r0 = sqrtOf(bc0);
            const T r1 = sqrtOf(bc1);
            m.b0 = T(0.5 * polarity) * (r0 + r1);
            m.b1 = T(0.5 * polarity) * (r0 - r1);
            return m;
        }
        // the trapezoidal biquad: tr = a11 + a22, det = a11 a22 - a12 a21; its numerator d + (Cy B - tr d) z^-1 + (det
        // d - tr Cy B + Cy A B) z^-2 over 1 - tr z^-1 + det z^-2
        const T trace = tr.a11 + tr.a22;
        const T det = tr.a11 * tr.a22 - tr.a12 * tr.a21;
        const T cb = tr.c1 * tr.b1 + tr.c2 * tr.b2;
        const T ab1 = tr.a11 * tr.b1 + tr.a12 * tr.b2;
        const T ab2 = tr.a21 * tr.b1 + tr.a22 * tr.b2;
        const T cab = tr.c1 * ab1 + tr.c2 * ab2;
        const T beta1 = cb - trace * tr.d;
        const T beta2 = det * tr.d - trace * cb + cab;
        // the analog response in s^ (the inverse bilinear transform)
        const T a0 = one - trace + det;
        const T a1 = two * (one - det);
        const T a2 = one + trace + det;
        const T n0 = tr.d + beta1 + beta2;
        const T n1 = two * (tr.d - beta2);
        const T n2 = tr.d - beta1 + beta2;
        // its poles, by impulse invariance: s^ = -sigma +- sqrt(q), q = sigma^2 - w0^2; z = e^(2 s^)
        const T sigma = a1 / (two * a2);
        const T w2 = a0 / a2;
        const T q = sigma * sigma - w2;
        const T root = sqrtOf(whereNegative(q, -q, q));
        const T decay = expOf(-(two * sigma));
        // underdamped: -2 e^(-2 sigma) cos(2 sqrt(-q)); overdamped: -(e^(-2 sigma + 2 sqrt(q)) + e^(-2 sigma - 2
        // sqrt(q)))
        const T under = -(two * decay * cosOf(two * root));
        const T over = -(expOf(two * root - two * sigma) + expOf(-(two * sigma) - two * root));
        m.a1 = whereNegative(q, under, over);
        m.a2 = decay * decay;
        // |H|^2 at s^ = j x, x^2 = s2
        auto magnitude2 = [&](const T& s2)
        {
            const T nr = n0 - n2 * s2;
            const T dr = a0 - a2 * s2;
            return (nr * nr + n1 * n1 * s2) / (dr * dr + a1 * a1 * s2);
        };
        // Vicanek's |A(e^jw)|^2 = A0 phi0 + A1 phi1 + A2 phi2
        const T p0 = one + m.a1 + m.a2;
        const T p1 = one - m.a1 + m.a2;
        const T ac0 = p0 * p0;
        const T ac1 = p1 * p1;
        const T ac2 = T(-4.0) * m.a2;
        // the poles' frequency: w0 (s^ = j w0), or pi / 4 above it (a quarter of the rate)
        const T quarter = T(std::numbers::pi * std::numbers::pi / 16);
        const T sm2 = whereNegative(w2 - quarter, w2, quarter);
        const T mPhi1 = T(0.5) * (one - cosOf(two * sqrtOf(sm2)));   // sin^2(x) = (1 - cos 2x) / 2
        const T mPhi0 = one - mPhi1;
        const T mPhi2 = T(4.0) * mPhi0 * mPhi1;
        const T tPhi2 = T(4.0) * topPhi0 * topPhi1;
        // B0 at DC; B1 and B2 from the other two: B1 phi1 + B2 phi2 = |H|^2 |A|^2 - B0 phi0, by Cramer's rule
        const T bc0 = magnitude2(T(0.0)) * ac0;
        const T rm = magnitude2(sm2) * (ac0 * mPhi0 + ac1 * mPhi1 + ac2 * mPhi2) - bc0 * mPhi0;
        const T rt = magnitude2(top2) * (ac0 * topPhi0 + ac1 * topPhi1 + ac2 * tPhi2) - bc0 * topPhi0;
        const T cramer = mPhi1 * tPhi2 - mPhi2 * topPhi1;
        const T bc1 = (rm * tPhi2 - mPhi2 * rt) / cramer;
        const T bc2 = (mPhi1 * rt - rm * topPhi1) / cramer;
        // the factorisation: sqrt(B0) = b0 + b1 + b2, sqrt(B1) = b0 - b1 + b2, B2 = -4 b0 b2
        const T r0 = sqrtOf(bc0);
        const T r1 = sqrtOf(bc1);
        const T w = T(0.5) * (r0 + r1);
        const T b0 = T(0.5) * (w + sqrtOf(w * w + bc2));
        m.b0 = T(polarity) * b0;
        m.b1 = T(0.5 * polarity) * (r0 - r1);
        m.b2 = T(-0.25 * polarity) * (bc2 / b0);
        return m;
    }

    // A matched design's response at f Hz and `rate`.
    std::complex<double> matchedResponse(const Matched<double>& m, double f, double rate);

    // Block `block`'s trapezoidal form at `rate` and `pots` (in double, from its Holters form): of order 1 or 2, or
    // nothing.
    std::optional<TrForm<double>> trForm(const Plan& plan, int block, double rate, const std::vector<double>& pots);

    // The matched design's check at generation, in double: at 44.1 and 48 kHz over the block's pot grid, the 1x
    // check's rule (oneXError's) on the matched response; the polarity it takes (the same everywhere on the grid), and
    // why it cannot be used when it cannot (its order, a factorisation that fails, zeros that are not minimum-phase,
    // a polarity that changes).
    struct MatchedCheck
    {
        bool usable = false;
        double worst = 0;     // dB, the gate's figure (at most 0.5 to be used)
        double polarity = 1;
        std::string why;      // "3 states: the matched design is of order 1 or 2"
    };

    MatchedCheck matchedCheck(const Plan& plan, int block);

    // The emitted design: the block's trapezoidal form from its derivation in Vals (`linear`, its readouts on its
    // sources), the design, and its coefficients bound to fields mb0_b<N>, mb1_b<N>, mb2_b<N>, ma1_b<N> and ma2_b<N>
    // (order 1: mb0, mb1 and ma1), in `code`.
    Coefficients<Val> matchedCoefficients(const Plan& plan, int block, const System& system, const Linear<Val>& linear,
                                          Code& code);
} // namespace fminus::circuit
