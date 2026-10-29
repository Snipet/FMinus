// The synth example: a 2-pole OTA state-variable filter (two LM13700-style OTAs as tanh VCCS macros, Iout = Iabc
// tanh(Vd / 2Vt), 220R/100k input attenuators, 1 nF integrating caps with 10 Mohm leakage, an ideal summer
// hp = in - lp - kq bp). The cutoff is Iabc, a per-sample input that enters only the device law, so the DK matrices
// do not depend on it; kq (resonance) is a pot. DK form (derived by hand, checked against dk.hpp's MNA):
//   p1 = att (in - h2/G2 - kq h1/G1),  p2 = att h1 / G1
//   v1 = p1 + K11 i1 + K12 i2,  v2 = p2 + K21 i1   (K11 = -att kq / G1, K12 = -att / G2, K21 = att / G1, K22 = 0)
//   bp = (i1 + h1)/G1, lp = (i2 + h2)/G2, h' = 2 gC v - h
// Solvers: 2-D Newton, and the torn 1-D Newton on v1 (K22 = 0, so v2 follows from i1 explicitly).
#include "dk.hpp"

#include <complex>
#include <cstring>

using cd = std::complex<double>;
static void fft(std::vector<cd>& a)
{
    const size_t n = a.size();
    for (size_t i = 1, j = 0; i < n; ++i)
    {
        size_t bit = n >> 1;
        for (; j & bit; bit >>= 1)
            j ^= bit;
        j ^= bit;
        if (i < j)
            std::swap(a[i], a[j]);
    }
    for (size_t len = 2; len <= n; len <<= 1)
    {
        const double ang = -2 * M_PI / double(len);
        const cd wl(std::cos(ang), std::sin(ang));
        for (size_t i = 0; i < n; i += len)
        {
            cd w(1);
            for (size_t j = 0; j < len / 2; ++j)
            {
                cd u = a[i + j], v = a[i + j + len / 2] * w;
                a[i + j] = u + v;
                a[i + j + len / 2] = u - v;
                w *= wl;
            }
        }
    }
}

static const double C = 1e-9, Gl = 1e-7, att = 220.0 / 100220.0, Vt2 = 2 * 0.02585;
static double iabcFor(double fc) { return Vt2 * 2 * M_PI * fc * C / att; }   // small-signal cutoff

struct Opts
{
    int K = -1;         // -1: converged
    bool torn = true;
    bool extrap = false;
    bool linFinal = false;   // final currents from the last iterate's linearisation (no extra tanh)
};

template <class T> struct Svf
{
    T G1, G2, g2C1, g2C2, K11, K12, K21, kq, iG1, iG2;
    T h1 = 0, h2 = 0, v1 = 0, v2 = 0, p1o = 0, p2o = 0;
    T lastX1 = 0, lastI1 = 0, lastD1 = 0, lastI2 = 0, lastD2 = 0;
    void init(double fs, double kqv)
    {
        const double gC = 2 * C * fs;
        G1 = T(gC + Gl);
        G2 = T(gC + Gl);
        iG1 = T(1 / (gC + Gl));
        iG2 = T(1 / (gC + Gl));
        g2C1 = T(2 * gC);
        g2C2 = T(2 * gC);
        kq = T(kqv);
        K11 = T(-att * kqv / (gC + Gl));
        K12 = T(-att / (gC + Gl));
        K21 = T(att / (gC + Gl));
    }
    // returns lp; *bp out. g: Iabc this sample (both OTAs).
    T step(T in, T g, const Opts& o, T* bp = nullptr, int* iters = nullptr)
    {
        const T p1 = T(att) * (in - h2 * iG2 - kq * h1 * iG1);
        const T p2 = T(att) * h1 * iG1;
        const T ivt = T(1 / Vt2);
        T x1 = v1, x2 = v2;
        if (o.extrap)
        {
            const T t1 = std::tanh(v1 * ivt), t2 = std::tanh(v2 * ivt);
            const T d1 = g * (T(1) - t1 * t1) * ivt, d2 = g * (T(1) - t2 * t2) * ivt;
            // (I - K D) dv = dp
            const T a11 = T(1) - K11 * d1, a12 = -K12 * d2, a21 = -K21 * d1, a22 = T(1);
            const T b1 = p1 - p1o, b2 = p2 - p2o, det = a11 * a22 - a12 * a21;
            x1 = v1 + (b1 * a22 - a12 * b2) / det;
            x2 = v2 + (a11 * b2 - a21 * b1) / det;
        }
        int it = 0;
        const int maxIt = o.K < 0 ? 100 : o.K;
        for (; it < maxIt; ++it)
        {
            if (o.torn)
            {
                const T t1 = std::tanh(x1 * ivt);
                const T i1 = g * t1, d1 = g * (T(1) - t1 * t1) * ivt;
                x2 = p2 + K21 * i1;
                const T t2 = std::tanh(x2 * ivt);
                const T i2 = g * t2, d2 = g * (T(1) - t2 * t2) * ivt;
                const T r = x1 - p1 - K11 * i1 - K12 * i2;
                const T dr = T(1) - K11 * d1 - K12 * d2 * K21 * d1;
                const T step = r / dr;
                lastX1 = x1;
                lastI1 = i1;
                lastD1 = d1;
                lastI2 = i2;
                lastD2 = d2;
                x1 -= step;
                if (o.K < 0 && std::fabs(double(step)) < 1e-14)
                {
                    ++it;
                    break;
                }
            }
            else
            {
                const T t1 = std::tanh(x1 * ivt), t2 = std::tanh(x2 * ivt);
                const T i1 = g * t1, i2 = g * t2;
                const T d1 = g * (T(1) - t1 * t1) * ivt, d2 = g * (T(1) - t2 * t2) * ivt;
                const T r1 = x1 - p1 - K11 * i1 - K12 * i2, r2 = x2 - p2 - K21 * i1;
                const T a11 = T(1) - K11 * d1, a12 = -K12 * d2, a21 = -K21 * d1, a22 = T(1);
                const T det = a11 * a22 - a12 * a21;
                const T s1 = (r1 * a22 - a12 * r2) / det, s2 = (a11 * r2 - a21 * r1) / det;
                x1 -= s1;
                x2 -= s2;
                if (o.K < 0 && std::fabs(double(s1)) + std::fabs(double(s2)) < 1e-14)
                {
                    ++it;
                    break;
                }
            }
        }
        T i1, i2;
        if (o.linFinal && o.K >= 1 && o.torn)
        {
            // re-run the last iterate's evaluation at its start point, then move the currents along their slopes
            T xs = x1;   // x1 already stepped; recover the start from the stored step is not kept: recompute
            (void)xs;
            i1 = lastI1 + lastD1 * (x1 - lastX1);
            i2 = lastI2 + lastD2 * K21 * (i1 - lastI1);
            x2 = p2 + K21 * i1;
        }
        else
        {
            if (o.torn)   // v2 consistent with the final v1
                x2 = p2 + K21 * g * std::tanh(x1 * ivt);
            i1 = g * std::tanh(x1 * ivt);
            i2 = g * std::tanh(x2 * ivt);
        }
        if (iters)
            *iters = it;
        const T b = (i1 + h1) * iG1, l = (i2 + h2) * iG2;
        h1 = g2C1 * b - h1;
        h2 = g2C2 * l - h2;
        v1 = x1;
        v2 = x2;
        p1o = p1;
        p2o = p2;
        if (bp)
            *bp = b;
        return l;
    }
};

static std::vector<double> decimate(const std::vector<double>& x, int M)
{
    if (M == 1)
        return x;
    const int half = 48 * M;
    std::vector<double> h(static_cast<size_t>(2 * half + 1));
    const double fc = 0.5 / M * 0.92;
    double sum = 0;
    for (int i = -half; i <= half; ++i)
    {
        const double sinc = i == 0 ? 2 * fc : std::sin(2 * M_PI * fc * i) / (M_PI * i);
        const double n = double(i + half) / (2 * half);
        const double w = 0.35875 - 0.48829 * std::cos(2 * M_PI * n) + 0.14128 * std::cos(4 * M_PI * n) - 0.01168 * std::cos(6 * M_PI * n);
        h[static_cast<size_t>(i + half)] = sinc * w;
        sum += sinc * w;
    }
    for (double& v : h)
        v /= sum;
    std::vector<double> y(x.size() / static_cast<size_t>(M));
    for (size_t o = 0; o < y.size(); ++o)
    {
        const long c = long(o) * M;
        double acc = 0;
        for (int i = -half; i <= half; ++i)
        {
            const long j = c - i;
            if (j >= 0 && j < long(x.size()))
                acc += h[static_cast<size_t>(i + half)] * x[static_cast<size_t>(j)];
        }
        y[o] = acc;
    }
    return y;
}

static const size_t N = 16384;
static const double fs0 = 48000;
static double errDb(const std::vector<double>& a, const std::vector<double>& ref)
{
    const size_t s = 2 * N;
    std::vector<cd> e(N), r(N);
    for (size_t k = 0; k < N; ++k)
    {
        e[k] = a[s + k] - ref[s + k];
        r[k] = ref[s + k];
    }
    fft(e);
    fft(r);
    const size_t top = size_t(20000.0 / fs0 * double(N));
    double ee = 0, rr = 0;
    for (size_t b = 1; b <= top; ++b)
    {
        ee += std::norm(e[b]);
        rr += std::norm(r[b]);
    }
    return 10 * std::log10(ee / rr + 1e-40);
}
static double inharmonicDb(const std::vector<double>& y, int k)
{
    const size_t s = 2 * N;
    std::vector<cd> a(N);
    for (size_t i = 0; i < N; ++i)
        a[i] = y[s + i];
    fft(a);
    double harm = 0, other = 0;
    const size_t top = size_t(20000.0 / fs0 * double(N));
    for (size_t b = 1; b <= top; ++b)
    {
        const double e = std::norm(a[b]);
        if (b % size_t(k) == 0)
            harm += e;
        else
            other += e;
    }
    return 10 * std::log10(other / (harm + other) + 1e-40);
}
static double peakHz(const std::vector<double>& y)
{
    const size_t s = 2 * N;
    std::vector<cd> a(N);
    for (size_t i = 0; i < N; ++i)
        a[i] = y[s + i] * (0.5 - 0.5 * std::cos(2 * M_PI * double(i) / double(N)));
    fft(a);
    size_t best = 1;
    for (size_t b = 1; b < N / 2; ++b)
        if (std::abs(a[b]) > std::abs(a[best]))
            best = b;
    // parabolic interpolation on log magnitude
    const double l = std::log(std::abs(a[best - 1])), c0 = std::log(std::abs(a[best])), r = std::log(std::abs(a[best + 1]));
    const double d = 0.5 * (l - r) / (l - 2 * c0 + r);
    return (double(best) + d) * fs0 / double(N);
}

int main()
{
    // cross-check the hand-derived DK form against dk.hpp's MNA (E sources for the summer and the buffer)
    {
        dk::Netlist n;
        const int in = n.node(), na = n.node(), nb = n.node(), hp = n.node(), a1 = n.node(), b = n.node(), bb = n.node(), a2 = n.node(),
                  l = n.node();
        n.inputs = 2;
        n.vs.push_back({ in, 0, 0 });
        const double kq = 0.3;
        n.es.push_back({ na, 0, in, l, 1.0 });
        n.es.push_back({ nb, 0, b, 0, kq });
        n.es.push_back({ hp, 0, na, nb, 1.0 });
        n.rs.push_back({ hp, a1, 100e3 });
        n.rs.push_back({ a1, 0, 220 });
        n.es.push_back({ bb, 0, b, 0, 1.0 });
        n.rs.push_back({ bb, a2, 100e3 });
        n.rs.push_back({ a2, 0, 220 });
        n.cs.push_back({ b, 0, C });
        n.rs.push_back({ b, 0, 1 / Gl });
        n.cs.push_back({ l, 0, C });
        n.rs.push_back({ l, 0, 1 / Gl });
        n.ports.push_back({ a1, 0, 0, b });
        n.ports.push_back({ a2, 0, 0, l });
        n.outputs.push_back(l);
        const dk::System s = dk::build(n, 96000);
        Svf<double> f;
        f.init(96000, kq);
        std::printf("DK cross-check (MNA vs hand form): K11 %.6g/%.6g K12 %.6g/%.6g K21 %.6g/%.6g K22 %.3g; Pu1 %.6g/%.6g\n", s.K[0][0], f.K11,
                    s.K[0][1], f.K12, s.K[1][0], f.K21, s.K[1][1], s.Pu[0][0], att);
    }
    // a bandlimited saw (harmonics to 20 kHz) on bin 37 (~108 Hz) of N at 48 kHz, level `amp` peak-ish
    auto saw = [&](double amp, int k, int M, size_t n) {
        std::vector<double> x(n);
        const double f0 = double(k) * fs0 / double(N);
        const int H = int(20000 / f0);
        for (int h = 1; h <= H; ++h)
        {
            const double a = amp * 2 / M_PI * (h % 2 ? 1 : -1) / h;
            const double w = 2 * M_PI * f0 * h / (fs0 * M);
            const cd rot = std::polar(1.0, w);
            cd z(1, 0);
            for (size_t i = 0; i < n; ++i)
            {
                x[i] += a * z.imag();
                z *= rot;
                if ((i & 1023) == 0)
                    z /= std::abs(z);
            }
        }
        return x;
    };
    const int kSaw = 37;
    struct Case { const char* name; double fc, kq, amp; };
    const Case cases[] = {
        { "fc 1 kHz, Q 0.7, 1 V saw", 1000, 1.414, 1.0 },
        { "fc 1 kHz, Q 10, 1 V saw", 1000, 0.1, 1.0 },
        { "fc 1 kHz, Q 10, 10 V saw", 1000, 0.1, 10.0 },
        { "fc 4 kHz, Q 25, 5 V saw", 4000, 0.04, 5.0 },
        { "fc 4 kHz, self-osc (kq -0.02), 1 V saw", 4000, -0.02, 1.0 },
    };
    for (const Case& c : cases)
    {
        std::printf("== %s ==\n", c.name);
        auto run = [&](auto tag, int M, Opts o, double* meanIt = nullptr, int* maxIt = nullptr, bool modulate = false) {
            using T = decltype(tag);
            const auto x = saw(c.amp, kSaw, M, 4 * N * size_t(M));
            Svf<T> f;
            f.init(fs0 * M, c.kq);
            std::vector<double> y(x.size());
            double s = 0;
            int mx = 0;
            for (size_t i = 0; i < x.size(); ++i)
            {
                double fc = c.fc;
                if (modulate)   // audio-rate cutoff modulation: +-2 octaves at 37 Hz, through an expo law, per sample
                    fc = c.fc * std::exp2(2 * std::sin(2 * M_PI * 37.0 * double(i) / (fs0 * M)));
                int it = 0;
                y[i] = double(f.step(T(x[i]), T(iabcFor(fc)), o, nullptr, &it));
                s += it;
                mx = std::max(mx, it);
            }
            if (meanIt)
                *meanIt = s / double(x.size());
            if (maxIt)
                *maxIt = mx;
            return decimate(y, M);
        };
        const auto ref = run(double{}, 64, Opts{});
        std::printf("  64x reference: peak at %.1f Hz\n", peakHz(ref));
        for (int M : { 1, 2, 4 })
        {
            double m2 = 0, mt = 0;
            int x2 = 0, xt = 0;
            Opts o2;
            o2.torn = false;
            const auto conv = run(double{}, M, o2, &m2, &x2);
            Opts ot;
            run(double{}, M, ot, &mt, &xt);
            std::printf("  %dx: alias %6.1f dB, vs 64x %6.1f dB, peak %.1f Hz | converged iterations: 2-D %.2f/%d, torn %.2f/%d\n", M,
                        inharmonicDb(conv, kSaw), errDb(conv, ref), peakHz(conv), m2, x2, mt, xt);
            std::printf("      float, fixed K vs converged double (prev guess):");
            for (int torn = 1; torn >= 0; --torn)
                for (int K = 1; K <= 3; ++K)
                {
                    Opts f;
                    f.K = K;
                    f.torn = torn == 1;
                    std::printf(" %s K%d %6.1f", torn ? "torn" : "2-D", K, errDb(run(float{}, M, f), conv));
                }
            for (int K = 1; K <= 2; ++K)
            {
                Opts f;
                f.K = K;
                f.linFinal = true;
                std::printf(" torn-lin K%d %6.1f", K, errDb(run(float{}, M, f), conv));
            }
            if (c.kq < 0)
            {
                Opts f;
                f.K = 2;
                const auto fl = run(float{}, M, f);
                double r1 = 0, r2 = 0;
                for (size_t k = 2 * N; k < 3 * N; ++k)
                {
                    r1 += conv[k] * conv[k];
                    r2 += fl[k] * fl[k];
                }
                std::printf(" | self-osc: double %.1f Hz %.3f V rms, float torn K2 %.1f Hz %.3f V rms", peakHz(conv), std::sqrt(r1 / N), peakHz(fl),
                            std::sqrt(r2 / N));
            }
            std::printf("\n");
            // modulated cutoff
            const auto convMod = run(double{}, M, o2, nullptr, nullptr, true);
            std::printf("      cutoff modulated per sample (+-2 oct, 37 Hz):");
            for (int torn = 1; torn >= 0; --torn)
                for (int K = 1; K <= 2; ++K)
                {
                    Opts f;
                    f.K = K;
                    f.torn = torn == 1;
                    std::printf(" %s K%d %6.1f", torn ? "torn" : "2-D", K, errDb(run(float{}, M, f, nullptr, nullptr, true), convMod));
                }
            std::printf("\n");
        }
    }
    return 0;
}
