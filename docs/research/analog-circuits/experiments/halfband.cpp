// Kaiser-windowed halfband FIRs for 2x stages: taps, stopband attenuation, multiplies per base-rate sample.
#include <cmath>
#include <cstdio>
#include <vector>

static double bessel0(double x)
{
    double s = 1, t = 1;
    for (int k = 1; k < 50; ++k)
    {
        t *= (x / (2 * k)) * (x / (2 * k));
        s += t;
    }
    return s;
}

// Length L = 4J - 1 (odd, halfband: every other tap zero but the centre). Edges in cycles/sample at the high rate.
static void design(int L, double beta, double pass, double stop, const char* what, int stageFactor)
{
    const int c = L / 2;
    std::vector<double> h(L);
    for (int n = 0; n < L; ++n)
    {
        const int k = n - c;
        const double sinc = k == 0 ? 0.5 : std::sin(M_PI * k / 2) / (M_PI * k);
        const double r = double(k) / c;
        h[n] = sinc * bessel0(beta * std::sqrt(std::fmax(0.0, 1 - r * r))) / bessel0(beta);
    }
    double sum = 0;
    for (double v : h)
        sum += v;
    for (double& v : h)
        v /= sum;
    double worstStop = 0, worstPass = 0;
    for (int i = 0; i <= 4000; ++i)
    {
        const double f = 0.5 * i / 4000;
        double re = 0, im = 0;
        for (int n = 0; n < L; ++n)
        {
            re += h[n] * std::cos(2 * M_PI * f * (n - c));
            im -= h[n] * std::sin(2 * M_PI * f * (n - c));
        }
        const double mag = std::sqrt(re * re + im * im);
        if (f >= stop)
            worstStop = std::fmax(worstStop, mag);
        if (f <= pass)
            worstPass = std::fmax(worstPass, std::fabs(20 * std::log10(mag)));
    }
    int nonzero = 0;
    for (int n = 0; n < L; ++n)
        if (std::fabs(h[n]) > 1e-12)
            ++nonzero;
    // per output of a decimator (or per input of an interpolator): the odd phase's taps, halved by symmetry, + centre
    const int oddTaps = (L + 1) / 2;   // nonzero taps in the non-trivial phase
    const int mults = (oddTaps + 1) / 2;
    std::printf("%s: L=%d beta=%.1f nonzero=%d | stopband %.1f dB | passband ripple %.4f dB | %d mults per low-rate "
                "sample per direction | delay %.1f low-rate samples per direction (x%d)\n",
                what, L, beta, nonzero, 20 * std::log10(worstStop), worstPass, mults, double(c) / 2 / stageFactor * 1.0 * 1,
                stageFactor);
}

int main()
{
    // 48k -> 96k: pass to 20k (0.2083), stop from 28k (0.2917)
    design(63, 8.0, 20.0 / 96, 28.0 / 96, "stage 1 (48->96k)", 1);
    design(79, 9.5, 20.0 / 96, 28.0 / 96, "stage 1 (48->96k)", 1);
    design(95, 11.0, 20.0 / 96, 28.0 / 96, "stage 1 (48->96k)", 1);
    // 96k -> 192k: pass to 20k (0.104), stop from 76k (0.396)
    design(23, 8.0, 20.0 / 192, 76.0 / 192, "stage 2 (96->192k)", 2);
    design(27, 9.0, 20.0 / 192, 76.0 / 192, "stage 2 (96->192k)", 2);
    design(31, 10.0, 20.0 / 192, 76.0 / 192, "stage 2 (96->192k)", 2);
    design(11, 6.0, 20.0 / 192, 76.0 / 192, "stage 2 (96->192k)", 2);
    design(15, 8.0, 20.0 / 192, 76.0 / 192, "stage 2 (96->192k)", 2);
    design(19, 10.0, 20.0 / 192, 76.0 / 192, "stage 2 (96->192k)", 2);
    return 0;
}
