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

    // The design's builtins on numbers: doubles (the gate at generation, the tests) and floats (the gate's run of the
    // emitted arithmetic); Emit.h's overloads print them for Vals.
    inline double expOf(double x)
    {
        return std::exp(x);
    }

    inline float expOf(float x)
    {
        return std::exp(x);
    }

    inline double sqrtOf(double x)
    {
        return std::sqrt(x);
    }

    inline float sqrtOf(float x)
    {
        return std::sqrt(x);
    }

    inline double cosOf(double x)
    {
        return std::cos(x);
    }

    inline float cosOf(float x)
    {
        return std::cos(x);
    }

    inline double sinOf(double x)
    {
        return std::sin(x);
    }

    inline float sinOf(float x)
    {
        return std::sin(x);
    }

    inline double whereNegative(double c, double a, double b)
    {
        return c < 0 ? a : b;
    }

    inline float whereNegative(float c, float a, float b)
    {
        return c < 0 ? a : b;
    }

    // The top match point, as a fraction of Nyquist: 0.83, the audio band's top at 48 kHz (20 kHz). Matched at Nyquist
    // itself (Vicanek's), a block whose response falls as a one-pole's does errs by 0.96 dB near 0.37 of the rate (an
    // RC low-pass at any corner above 300 Hz, the wah's tank, the TS tone block); matched here, by 0.35 to 0.38 dB.
    inline constexpr double kMatchTop = 0.83;

    // The middle match point's floor, x = w / 2 in radians (w = 0.1 radians a sample: 764 Hz at 48 kHz, 3 kHz at 192
    // kHz): a block whose poles sit lower is matched there, not at its poles' frequency. Matched at a low w, the
    // numerator's B1 and B2 are |H|^2 |A|^2 - B0 phi0 over phi1 = sin^2(w / 2), a difference of two nearly equal
    // numbers (the response barely moves from its DC value there), which float cannot hold below about w = 0.05 (a
    // Sallen-Key's coefficients at 99 Hz and 48 kHz came out NaN); |B|^2 is smooth (the zeros sit far from the
    // poles), so a higher point serves as well, and in double the gate's figures do not move.
    inline constexpr double kMatchFloor = 0.05;

    // The design from the trapezoidal form, `polarity` (+1 or -1) the sign its numerator takes (a magnitude match
    // loses it: an inverting block's is -1). With s^ = s / 2 fs, the trapezoidal z^-1 = (1 - s^) / (1 + s^): the
    // analog response is (N0 + N1 s^ + N2 s^2) / (A0 + A1 s^ + A2 s^2), and a frequency w (radians a sample) is s^ = j
    // w / 2. The poles, by impulse invariance: s^ = -sigma +- sqrt(sigma^2 - w0^2) maps to e^(s T) = e^(2 s^). The
    // numerator's |B|^2 = B0 phi0 + B1 phi1 + B2 phi2 (Vicanek's: phi1 = sin^2(w / 2), phi0 = 1 - phi1, phi2 = 4 phi0
    // phi1) is |H|^2 |A|^2 at DC, at the poles' frequency (2 w0, clamped to [2 kMatchFloor, pi / 2]) and at kMatchTop
    // pi: B0 directly, B1 and B2 by Cramer's rule; then Vicanek's factorisation. Order 1: the one-pole's numerator, b0
    // + b1 z^-1, at DC and at kMatchTop pi. Float: |A|^2 comes from the poles in factors that keep their digits near z
    // = 1 (below), every square root's argument is held at 0 or above and b2's divisor away from 0, so the
    // coefficients are finite wherever the poles sit; how near they come to the analog response is the gate's to say
    // (matchedCheck runs this on floats too).
    template <typename T>
    Matched<T> matchedDesign(const TrForm<T>& tr, double polarity)
    {
        const T zero(0.0);
        const T one(1.0);
        const T two(2.0);
        const T four(4.0);
        // the top point's phi1 and phi0, and its s^ = j x, x^2 = top2
        const double top = kMatchTop * std::numbers::pi;
        const T topPhi1(std::sin(top / 2) * std::sin(top / 2));
        const T topPhi0(std::cos(top / 2) * std::cos(top / 2));
        const T top2(top * top / 4);
        // x^2 held at 0 or above: a square root's argument, which float can push a hair below 0
        auto atLeast0 = [&](const T& x)
        {
            return whereNegative(x, zero, x);
        };
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
            const T r1 = sqrtOf(atLeast0(bc1));
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
        // its poles, by impulse invariance: s^ = -sigma +- sqrt(q), q = sigma^2 - w0^2; z = e^(2 s^), |z| = r =
        // e^(-2 sigma). Underdamped (q < 0): z = r e^(+-j theta), theta = 2 sqrt(-q), a1 = -2 r cos theta = -2 r (1 - 2
        // sin^2(theta / 2)). Overdamped: z = zp, zm = sp^2, sm^2, with sp, sm = e^(-sigma +- sqrt(q)), a1 = -(zp + zm)
        const T sigma = a1 / (two * a2);
        const T w2 = a0 / a2;
        const T q = sigma * sigma - w2;
        const T root = sqrtOf(whereNegative(q, -q, q));
        const T decay = expOf(-(two * sigma));
        const T sine = sinOf(root);
        const T sine2 = sine * sine;
        const T sp = expOf(root - sigma);
        const T sm = expOf(-(sigma + root));
        const T zp = sp * sp;
        const T zm = sm * sm;
        m.a1 = whereNegative(q, -(two * decay * (one - two * sine2)), -(zp + zm));
        m.a2 = decay * decay;
        // |H|^2 at s^ = j x, x^2 = s2
        auto magnitude2 = [&](const T& s2)
        {
            const T nr = n0 - n2 * s2;
            const T dr = a0 - a2 * s2;
            return (nr * nr + n1 * n1 * s2) / (dr * dr + a1 * a1 * s2);
        };
        // Vicanek's |A(e^jw)|^2 = A0 phi0 + A1 phi1 + A2 phi2 (A0 = (1 + a1 + a2)^2, A1 = (1 - a1 + a2)^2, A2 = -4 a2)
        // = A0 phi0 + phi1 (M + 16 a2 phi1), M = A1 - 16 a2, from the poles: near z = 1, a1 is near -2 and a2 near 1,
        // and 1 + a1 + a2 from them keeps only their last digits. With e = 1 - r and rs = 4 r sinh^2(sqrt q) = (sp -
        // sm)^2 (overdamped) or -4 r sin^2(theta / 2) (underdamped): 1 + a1 + a2 = (1 - z)(1 - z') = e^2 - rs ((1 -
        // zp)(1 - zm) overdamped), and M = (e^2 + rs) ((1 + r)^2 + 4 r + rs), each a product of sums of positive
        // terms but for the underdamped e^2 + rs (which a high Q makes small: the poles' own difference)
        const T e = one - decay;
        const T rs = whereNegative(q, -(four * decay * sine2), (sp - sm) * (sp - sm));
        const T sum = whereNegative(q, e * e - rs, (one - zp) * (one - zm));
        const T mm = (e * e + rs) * ((one + decay) * (one + decay) + four * decay + rs);
        const T ac0 = sum * sum;
        auto poles2 = [&](const T& phi0, const T& phi1)
        {
            return ac0 * phi0 + phi1 * (mm + T(16.0) * m.a2 * phi1);
        };
        // the poles' frequency: w0 (s^ = j w0), clamped to [kMatchFloor, pi / 4] (a quarter of the rate)
        const T quarter = T(std::numbers::pi * std::numbers::pi / 16);
        const T floor2 = T(kMatchFloor * kMatchFloor);
        const T upTo = whereNegative(w2 - quarter, w2, quarter);
        const T sm2 = whereNegative(upTo - floor2, floor2, upTo);
        const T match = sinOf(sqrtOf(sm2));
        const T mPhi1 = match * match;   // sin^2(w / 2), w / 2 = x
        const T mPhi0 = one - mPhi1;
        const T mPhi2 = four * mPhi0 * mPhi1;
        const T tPhi2 = four * topPhi0 * topPhi1;
        // B0 at DC; B1 and B2 from the other two: B1 phi1 + B2 phi2 = |H|^2 |A|^2 - B0 phi0, by Cramer's rule
        const T bc0 = magnitude2(zero) * ac0;
        const T rm = magnitude2(sm2) * poles2(mPhi0, mPhi1) - bc0 * mPhi0;
        const T rt = magnitude2(top2) * poles2(topPhi0, topPhi1) - bc0 * topPhi0;
        const T cramer = mPhi1 * tPhi2 - mPhi2 * topPhi1;
        const T bc1 = (rm * tPhi2 - mPhi2 * rt) / cramer;
        const T bc2 = (mPhi1 * rt - rm * topPhi1) / cramer;
        // the factorisation: sqrt(B0) = b0 + b1 + b2, sqrt(B1) = b0 - b1 + b2, B2 = -4 b0 b2; (b0 - b2)^2 = w^2 + B2,
        // w = b0 + b2, at 0 or above (a magnitude no numerator meets is met as nearly as the gate allows); b0 = 0 only
        // when the whole numerator is (then b2 is too)
        const T r0 = sqrtOf(bc0);
        const T r1 = sqrtOf(atLeast0(bc1));
        const T w = T(0.5) * (r0 + r1);
        const T b0 = T(0.5) * (w + sqrtOf(atLeast0(w * w + bc2)));
        m.b0 = T(polarity) * b0;
        m.b1 = T(0.5 * polarity) * (r0 - r1);
        m.b2 = whereNegative(-b0, T(-0.25 * polarity) * (bc2 / b0), zero);
        return m;
    }

    // A matched design's response at f Hz and `rate`.
    std::complex<double> matchedResponse(const Matched<double>& m, double f, double rate);

    // Block `block`'s trapezoidal form at `rate` and `pots` (in double, from its Holters form): of order 1 or 2, or
    // nothing.
    std::optional<TrForm<double>> trForm(const Plan& plan, int block, double rate, const std::vector<double>& pots);

    // The host rates the matched design's float run covers.
    inline constexpr double kMatchedRates[] = {44100.0, 48000.0, 88200.0, 96000.0, 176400.0, 192000.0};

    // The matched design's check at generation: in double, at 44.1 and 48 kHz over the block's pot grid, the 1x
    // check's rule (oneXError's) on the matched response; then in float, as the module computes it, at kMatchedRates
    // over a finer grid (17 positions a pot), the same rule, with the coefficients finite and the poles inside the
    // unit circle. The polarity it takes (the same everywhere on the grid), and why it cannot be used when it cannot
    // (its order, coefficients that are not finite, zeros that are not minimum-phase, a polarity that changes, the
    // float run's figure).
    struct MatchedCheck
    {
        bool usable = false;
        double worst = 0;        // dB, the gate's figure in double at 44.1 and 48 kHz (at most 0.5 to be used)
        double floatWorst = 0;   // dB, the float run's (at most 0.5 too)
        double polarity = 1;
        std::string why;         // "3 states: the matched design is of order 1 or 2"
    };

    MatchedCheck matchedCheck(const Plan& plan, int block);

    // ---- the method check (the pipeline's stage 7) ------------------------------------------------------------------

    // The eigenvalues of a real square matrix: Householder reduction to Hessenberg form, then the shifted QR algorithm
    // (Wilkinson's shift, deflating from the bottom), in complex double; in no particular order. Nothing when it does
    // not converge (it does, in a few iterations an eigenvalue, for the small matrices here).
    std::optional<std::vector<std::complex<double>>> eigenvalues(std::vector<std::vector<double>> a);

    // A port block's linearised trapezoidal state matrix at a design rate and pots: its Holters form with the port
    // replaced by its small-signal conductance G, v = p / (1 + kd G), so A + W Cp^T / (1 + kd G); and its eigenvalue
    // nearest -1, where the trapezoidal rule rings (a stiff pole, z = (1 - a T / 2) / (1 + a T / 2) near -1 for a
    // large a), at the port's smallest conductance (v = 0) and its largest on the stress range: a diode's at its
    // short-circuit current at the stress level L, over 20 Hz to 20 kHz (the blocks before it at their analog gain),
    // an OTA's at v = 0 and its largest gain.
    struct RingingCheck
    {
        bool flagged = false;                // an eigenvalue within 0.1 of -1
        std::complex<double> eigenvalue;     // the one nearest -1
        double conductance = 0;              // where it is: the port's conductance, siemens
    };

    RingingCheck ringingCheck(const Plan& plan, int block, double designRate);

    // The emitted design: the block's trapezoidal form from its derivation in Vals (`linear`, its readouts on its
    // sources), the design, and its coefficients bound to fields mb0_b<N>, mb1_b<N>, mb2_b<N>, ma1_b<N> and ma2_b<N>
    // (order 1: mb0, mb1 and ma1), in `code`.
    Coefficients<Val> matchedCoefficients(const Plan& plan, int block, const System& system, const Linear<Val>& linear,
                                          Code& code);
} // namespace fminus::circuit
