// Example 2, block 3: the netlist's small-signal response (nodal analysis, ideal op-amp) against Yeh, Abel & Smith,
// DAFx-07, eq. 24, at T = 0 (ends kept 1e-4 off), 0.5 and 1, both pot orientations. Also the discretised block (TR at
// 48 kHz, as TsB3 in mkbench.py emits it) against the analog response.
#include <cmath>
#include <complex>
#include <cstdio>

using cd = std::complex<double>;
static const double Rs = 1e3, Cs = 0.22e-6, Ri = 10e3, Rz = 220, Cz = 0.22e-6, Rf = 1e3, Rp = 20e3;

// netlist: Vi -Rs- t1; Cs, Ri: t1-0; pot: t1 -Ra- tw -Rb- tn; tw -Rz- -Cz- 0; Rf: out-tn; op-amp: + = t1, - = tn
static cd netlist(cd s, double Ra, double Rb)
{
    const cd Yz = s * Cz / (1.0 + s * Cz * Rz);
    const double Gab = 1 / Ra + 1 / Rb;
    const cd frac = Yz / (Gab + Yz);                 // (Vp - Vw) / Vp
    const cd Vp = (1 / Rs) / (1 / Rs + s * Cs + 1 / Ri + frac / Ra);
    return Vp * (1.0 + Rf * frac / Rb);
}
static cd yeh(cd s, double T)
{
    const double Rl = T * Rp, Rr = (1 - T) * Rp, par = Rl * Rr / (Rl + Rr);
    const double Y = (Rl + Rr) * (Rz + par);
    const double W = Y / (Rl * Rf + Y);
    const double X = (Rr / (Rl + Rr)) / ((Rz + par) * Cz);
    const double wz = 1 / (Cz * (Rz + par)), wp = 1 / (Cs * (Rs * Ri / (Rs + Ri)));
    return (Rl * Rf + Y) / (Y * Rs * Cs) * (s + W * wz) / ((s + wp) * (s + wz) + X * s);
}
// the emitted block (TsB3) run as a filter: its frequency response from the per-sample recursion's coefficients
static cd discrete(double f, double fs, double T)
{
    const double g5 = 2 * Cs * fs, g6 = 2 * Cz * fs, gz = 1 / Rz;
    const double ga = 1 / (T * Rp), gb = 1 / ((1 - T) * Rp);
    const double m = gz / (gz + g6), iz = 1 / (gz + g6);
    const double d2 = ga + gb + gz * (1 - m), al = (ga + gb) / d2, be = m / d2;
    const double gs = 1 / Rs, den = 1 / (gs + 1 / Ri + g5 + ga - ga * al);
    const double c1 = gs * den, c2 = den, c3 = ga * be * den, o1 = 1 + Rf * gb, o2 = Rf * gb;
    // states h5, h6: h' = 2 g vp - h etc. In z: H = output / input for e^{jwn}
    const cd z = std::polar(1.0, 2 * M_PI * f / fs), zi = 1.0 / z;
    // vp = c1 u + c2 h5 + c3 h6; vw = al vp + be h6; vz = m vw + iz h6; h5 z = 2 g5 vp - h5; h6 z = 2 g6 vz - h6
    // h5 = 2 g5 vp / (z + 1); h6 = 2 g6 vz / (z + 1) -> solve linear 2x2 for vp, h6 given u = 1
    // vp = c1 + c2 * 2 g5 vp / (z + 1) + c3 h6;  vz = m (al vp + be h6) + iz h6;  h6 (z + 1) = 2 g6 vz
    const cd a5 = 2 * g5 / (z + 1.0), a6 = 2 * g6 / (z + 1.0);
    // h6 = a6 (m al vp + (m be + iz) h6) -> h6 (1 - a6 (m be + iz)) = a6 m al vp
    const cd r = a6 * m * al / (1.0 - a6 * (m * be + iz));
    const cd vp = c1 / (1.0 - c2 * a5 - c3 * r);
    const cd h6 = r * vp;
    const cd vw = al * vp + be * h6;
    (void)zi;
    return o1 * vp - o2 * vw;
}

int main()
{
    const double fsx[] = { 20, 50, 100, 200, 500, 1000, 2000, 5000, 10000, 15000, 20000 };
    for (double T : { 1e-4, 0.5, 1 - 1e-4 })
    {
        double worstA = 0, worstB = 0, worstD = 0;
        std::printf("T = %.4f\n  f(Hz)   eq24(dB)  netlist Ra=T (dB)  netlist Ra=1-T (dB)  TR@48k (dB)\n", T);
        for (double f : fsx)
        {
            const cd s(0, 2 * M_PI * f);
            const double e = 20 * std::log10(std::abs(yeh(s, T)));
            const double a = 20 * std::log10(std::abs(netlist(s, T * Rp, (1 - T) * Rp)));
            const double b = 20 * std::log10(std::abs(netlist(s, (1 - T) * Rp, T * Rp)));
            const double d = 20 * std::log10(std::abs(discrete(f, 48000, T)));
            worstA = std::fmax(worstA, std::fabs(a - e));
            worstB = std::fmax(worstB, std::fabs(b - e));
            if (f <= 10000)
                worstD = std::fmax(worstD, std::fabs(d - a));
            std::printf("  %6.0f  %8.3f  %8.3f           %8.3f             %8.3f\n", f, e, a, b, d);
        }
        std::printf("  worst |netlist(Ra = T 20k) - eq24| = %.2e dB, |netlist(Ra = (1-T) 20k) - eq24| = %.2f dB, |TR - analog| to 10k = %.3f dB\n",
                    worstA, worstB, worstD);
    }
    return 0;
}
