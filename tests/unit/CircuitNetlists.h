// tests/unit/CircuitNetlists.h: the netlists the circuit compiler's tests share (CircuitTest.cpp, CircuitEmitTest.cpp),
// as strings: the report's two examples (docs/research/analog-circuits/notes/fminus.md §4, with the report's §9.1
// tone-pot line), TS's clipping stage alone, and synthetic circuits with textbook responses. makeDT and makeTS are
// docs/research/analog-circuits/experiments/adaa.cpp's hand-derived Holters forms of the two 1-port blocks, copied
// from there, so that the generator's derivation is checked against an independent one.
#pragma once

#include <cmath>
#include <string>

namespace fminus::circuit::test
{
    // Example 1, as examples/circuits/diodetone.cir ships it.
    inline const std::string kDiodeTone = R"(* diodetone.cir: a passive diode clipper, then a tone pot into a low-pass
.title DiodeTone
.input  u    in   scale=1          ; 1.0 = 1 V into R1
.output vout b
.pot    tone 0.5 lin               ; 1: bright (0 ohm), 0: dark (25k)
.oversample 4 iir blocks=auto
.adaa off
.solver omega(2)
.target solver=-60dB alias=-60dB level=4.5V freq=3k
R1     in a  2.2k
C1     a  0  10n
D1     a  0  1N914
D2     0  a  1N914
R2     a  c  470
Ptone  c  b  25k {1 - tone}        ; a rheostat
C2     b  0  22n
.model 1N914 D(IS=2.52n N=1.752)
.end
)";

    // Example 2, as examples/circuits/screamer.cir ships it.
    inline const std::string kScreamer = R"(* screamer.cir: TS808 clipping stage, tone control and level
.title Screamer
.input  u    in   scale=1
.output vout out
.pot    drive 0.5 audio
.pot    tone  0.5 lin
.pot    level 0.7 audio
.oversample 4 iir blocks=auto
.adaa on
.solver omega(1)
.target solver=-60dB alias=-60dB level=0.45V freq=3k
C1     in  p1  1u
R1     p1  0   10k
XU1    p1  n1  v2  opamp
R4     n1  z1  4.7k
C3     z1  0   47n
R6     n1  f1  51k
Pdrive f1  v2  500k {drive} taper=audio
C4     n1  v2  51p
D1     n1  v2  1N914
D2     v2  n1  1N914
R7     v2  t1  1k
C5     t1  0   0.22u
R8     t1  0   10k
XU2    t1  tn  v3  opamp
R9     v3  tn  1k
Ptone  tn  tw  t1  20k {tone}
R10    tw  tz  220
C6     tz  0   0.22u
C7     v3  o1  1u
R11    o1  o2  1k
Plevel o2  out 0   100k {level} taper=audio
.model 1N914 D(IS=2.52n N=1.752)
.end
)";

    // Example 2's block 2 alone, driven at XU1's + input.
    inline const std::string kScreamerClip = R"(* screamer-clip.cir
.title ScreamerClip
.input  u    p1   scale=1
.output vout v2
.pot    drive 0.5 audio
.oversample 4 iir blocks=auto
.adaa on
.solver omega(1)
XU1    p1  n1  v2  opamp
R4     n1  z1  4.7k
C3     z1  0   47n
R6     n1  f1  51k
Pdrive f1  v2  500k {drive} taper=audio
C4     n1  v2  51p
D1     n1  v2  1N914
D2     v2  n1  1N914
.model 1N914 D(IS=2.52n N=1.752)
.end
)";

    // A netlist with its .model's N replaced (1.75140653 gives adaa.cpp's 45.3 mV).
    inline std::string withN(std::string netlist, const std::string& n)
    {
        const std::string from = "N=1.752)";
        netlist.replace(netlist.find(from), from.size(), "N=" + n + ")");
        return netlist;
    }

    // Synthetic circuits with textbook transfer functions.
    inline const std::string kRcLowpass = R"(.title RcLowpass
.input u in
.output y o
R1 in o 1k
C1 o 0 100n
)";

    // A non-inverting amplifier, 1 + Zf / Rg with Zf = Rf || Cf.
    inline const std::string kNonInverting = R"(.title NonInverting
.input u in
.output y out
X1 in n out opamp
Rf out n 47k
Cf out n 1n
Rg n 0 4.7k
)";

    // A 3-terminal pot as a divider: V(w) / V(in) = t.
    inline std::string potDivider(const std::string& taper)
    {
        return ".title Divider\n.input u in\n.output y w\n.pot x 0.5\nP1 in w 0 10k {x} taper=" + taper + "\n";
    }

    // A unity-gain Sallen-Key low-pass: 1 / (1 + s C2 (R1 + R2) + s^2 R1 R2 C1 C2).
    inline const std::string kSallenKey = R"(.title SallenKey
.input u in
.output y out
R1 in a 10k
R2 a b 10k
C1 a out 22n
C2 b 0 10n
X1 b out out opamp
)";

    // ---- adaa.cpp's hand-derived blocks (Holters' form) ----

    struct HandBlock
    {
        double cp[2], dp;
        double a[2][2], b[2], w[2];
        double cy[2], dy, wy;
        double k;
    };

    inline constexpr double kIs = 2.52e-9;

    // Example 2, block 2. Drive: the audio taper (81^d - 1)/80 of 500k, plus 51k. v = V(v2) - V(n1) here.
    inline HandBlock makeTS(double fs, double drive)
    {
        const double g4 = 1 / 4700.0, gc3 = 2 * 47e-9 * fs, gc4 = 2 * 51e-12 * fs;
        const double inA = 1 / (g4 + gc3);
        const double rf = 51e3 + 500e3 * (std::pow(81.0, drive) - 1) / 80;
        const double invB = 1 / (1 / rf + gc4);
        HandBlock b{};
        b.cp[0] = -g4 * inA * invB;
        b.cp[1] = invB;
        b.dp = (g4 - g4 * g4 * inA) * invB;
        b.a[0][0] = 2 * gc3 * inA - 1;
        b.a[0][1] = 0;
        b.a[1][0] = 0;
        b.a[1][1] = -1;
        b.b[0] = 2 * gc3 * inA * g4;
        b.b[1] = 0;
        b.w[0] = 0;
        b.w[1] = 2 * gc4;
        b.cy[0] = b.cy[1] = 0;
        b.dy = 1;
        b.wy = 1;
        b.k = 2 * kIs * invB;
        return b;
    }

    // Example 1 (DiodeTone). Tone: Ptone = 25k (1 - tone).
    inline HandBlock makeDT(double fs, double tone)
    {
        const double g1 = 1 / 2200.0, gc1 = 2 * 10e-9 * fs, gc2 = 2 * 22e-9 * fs;
        const double gs = 1 / (470 + 25000 * (1 - tone));
        const double s11 = g1 + gc1 + gs, s22 = gs + gc2, det = s11 * s22 - gs * gs, inv = 1 / det;
        const double cin = s22 * g1 * inv, cx1 = s22 * inv, cx2 = gs * inv, kk = s22 * inv, gk = det / s22;
        const double din = gs * g1 * inv, dx1 = gs * inv, dx2 = s11 * inv, di = gs * inv;
        HandBlock b{};
        b.cp[0] = cx1;
        b.cp[1] = cx2;
        b.dp = cin;
        b.cy[0] = dx1 - di * gk * cx1;
        b.cy[1] = dx2 - di * gk * cx2;
        b.dy = din - di * gk * cin;
        b.wy = di * gk;
        b.a[0][0] = -1;
        b.a[0][1] = 0;
        b.a[1][0] = 2 * gc2 * b.cy[0];
        b.a[1][1] = 2 * gc2 * b.cy[1] - 1;
        b.b[0] = 0;
        b.b[1] = 2 * gc2 * b.dy;
        b.w[0] = 2 * gc1;
        b.w[1] = 2 * gc2 * b.wy;
        b.k = 2 * kIs * kk;
        return b;
    }
} // namespace fminus::circuit::test
