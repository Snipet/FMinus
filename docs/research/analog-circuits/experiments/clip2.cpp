// Second pass: in-band errors (bins under 20 kHz), integration methods (TR, BE, BDF2) against a 64x reference,
// the trapezoidal rule's ringing at Nyquist, and fixed Newton counts measured in band.
#include <cmath>
#include <complex>
#include <cstdio>
#include <vector>

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

enum class Method { TR, BE, BDF2 };
// A capacitor's companion: i = g v - h, h from the past.
template <class T> struct Cap
{
    T C = 0, fs = 0;
    Method m = Method::TR;
    T g = 0, h = 0, v1 = 0;
    void init(double c, double rate, Method method)
    {
        C = T(c);
        fs = T(rate);
        m = method;
        g = m == Method::TR ? T(2) * C * fs : m == Method::BE ? C * fs : T(1.5) * C * fs;
        h = 0;
        v1 = 0;
    }
    void update(T v)   // after the solve of this sample
    {
        if (m == Method::TR)
            h = T(2) * g * v - h;
        else if (m == Method::BE)
            h = g * v;
        else
        {
            h = C * fs * (T(2) * v - T(0.5) * v1);
            v1 = v;
        }
    }
};

template <class T> static T newton(T p, T k, T vt, T g, int iters)
{
    const T lo = std::fmin(T(0), p), hi = std::fmax(T(0), p);
    T v = std::fmin(std::fmax(g, lo), hi);
    for (int i = 0; i < iters; ++i)
    {
        const T e = std::exp(v / vt), ei = T(1) / e;
        const T f = v - p + k * T(0.5) * (e - ei);
        const T d = T(1) + (k / vt) * T(0.5) * (e + ei);
        v = std::fmin(std::fmax(v - f / d, lo), hi);
    }
    return v;
}
static double converged(double p, double k, double vt, double g)
{
    const double lo = std::fmin(0.0, p), hi = std::fmax(0.0, p);
    double v = std::fmin(std::fmax(g, lo), hi);
    for (int i = 0; i < 300; ++i)
    {
        const double e = std::exp(v / vt), ei = 1 / e;
        const double f = v - p + k * 0.5 * (e - ei);
        const double d = 1 + (k / vt) * 0.5 * (e + ei);
        double nv = std::fmin(std::fmax(v - f / d, lo), hi);
        if (std::fabs(nv - v) < 1e-16 + 1e-15 * std::fabs(nv))
            return nv;
        v = nv;
    }
    return v;
}

struct Opts
{
    Method m = Method::TR;
    int K = -1;               // -1: converged
    bool extrapolate = true;
};

template <class T> static std::vector<double> clipper(double fs, const std::vector<double>& in, Opts o)
{
    const double R = 2200, C = 10e-9, Is = 2.52e-9, vt = 0.045;
    Cap<T> c;
    c.init(C, fs, o.m);
    const T G = T(1 / R);
    const T inv = T(1) / (G + c.g), k = T(2 * Is) * inv, Vt = T(vt);
    T v = 0, pPrev = 0;
    std::vector<double> out(in.size());
    for (size_t i = 0; i < in.size(); ++i)
    {
        const T p = (G * T(in[i]) + c.h) * inv;
        T g = v;
        if (o.extrapolate)
        {
            const T e = std::exp(v / Vt);
            g = v + (p - pPrev) / (T(1) + (k / Vt) * T(0.5) * (e + T(1) / e));
        }
        v = o.K < 0 ? T(converged(double(p), double(k), vt, double(g))) : newton(p, k, Vt, g, o.K);
        c.update(v);
        pPrev = p;
        out[i] = double(v);
    }
    return out;
}

template <class T> static std::vector<double> screamer(double fs, const std::vector<double>& in, Opts o, double D = 1)
{
    const double R1 = 4700, Cz = 47e-9, Cc = 51e-12, Is = 2.52e-9, vt = 0.045;
    Cap<T> cz, cc;
    cz.init(Cz, fs, o.m);
    cc.init(Cc, fs, o.m);
    const T G1 = T(1 / R1), G2 = T(1 / (51e3 + D * 500e3));
    const T invA = T(1) / (G1 + cz.g), invB = T(1) / (G2 + cc.g), k = T(2 * Is) * invB, Vt = T(vt);
    T v = 0, pPrev = 0;
    std::vector<double> out(in.size());
    for (size_t i = 0; i < in.size(); ++i)
    {
        const T vi = T(in[i]);
        const T va = (vi * G1 + cz.h) * invA;
        const T In = (vi - va) * G1;
        const T p = (In + cc.h) * invB;
        T g = v;
        if (o.extrapolate)
        {
            const T e = std::exp(v / Vt);
            g = v + (p - pPrev) / (T(1) + (k / Vt) * T(0.5) * (e + T(1) / e));
        }
        v = o.K < 0 ? T(converged(double(p), double(k), vt, double(g))) : newton(p, k, Vt, g, o.K);
        cz.update(va);
        cc.update(v);
        pPrev = p;
        out[i] = double(vi + v);
    }
    return out;
}

static std::vector<double> decimate(const std::vector<double>& x, int M)
{
    if (M == 1)
        return x;
    const int half = 48 * M;
    std::vector<double> h(2 * half + 1);
    const double fc = 0.5 / M * 0.92;
    double sum = 0;
    for (int i = -half; i <= half; ++i)
    {
        const double t = i;
        const double sinc = i == 0 ? 2 * fc : std::sin(2 * M_PI * fc * t) / (M_PI * t);
        const double n = double(i + half) / (2 * half);
        const double w = 0.35875 - 0.48829 * std::cos(2 * M_PI * n) + 0.14128 * std::cos(4 * M_PI * n) -
                         0.01168 * std::cos(6 * M_PI * n);
        h[size_t(i + half)] = sinc * w;
        sum += sinc * w;
    }
    for (double& v : h)
        v /= sum;
    std::vector<double> y(x.size() / size_t(M));
    for (size_t o = 0; o < y.size(); ++o)
    {
        const long c = long(o) * M;
        double acc = 0;
        for (int i = -half; i <= half; ++i)
        {
            const long j = c - i;
            if (j >= 0 && j < long(x.size()))
                acc += h[size_t(i + half)] * x[size_t(j)];
        }
        y[o] = acc;
    }
    return y;
}

static const double fsBase = 48000;
static const size_t N = 32768;

// In-band (<= 20 kHz) error of a against ref over the last N samples, dB re ref's in-band energy.
static double bandErrDb(const std::vector<double>& a, const std::vector<double>& ref)
{
    const size_t s = a.size() - N;
    std::vector<cd> e(N), r(N);
    for (size_t i = 0; i < N; ++i)
    {
        e[i] = a[s + i] - ref[s + i];
        r[i] = ref[s + i];
    }
    fft(e);
    fft(r);
    const size_t top = size_t(20000.0 / fsBase * double(N));
    double ee = 0, rr = 0;
    for (size_t b = 1; b <= top; ++b)
    {
        ee += std::norm(e[b]);
        rr += std::norm(r[b]);
    }
    return 10 * std::log10(ee / rr + 1e-40);
}
static double aboveBandDb(const std::vector<double>& a)   // energy above 20 kHz re all
{
    const size_t s = a.size() - N;
    std::vector<cd> r(N);
    for (size_t i = 0; i < N; ++i)
        r[i] = a[s + i];
    fft(r);
    const size_t top = size_t(20000.0 / fsBase * double(N));
    double hi = 0, all = 0;
    for (size_t b = 1; b <= N / 2; ++b)
    {
        all += std::norm(r[b]);
        if (b > top)
            hi += std::norm(r[b]);
    }
    return 10 * std::log10(hi / all + 1e-40);
}

int main()
{
    const int kLow = 683, kHigh = 2049;   // ~1000.5 Hz and ~3001.5 Hz on bins of N = 32768 at 48 kHz
    auto sine = [&](double amp, int k, int M, size_t n) {
        std::vector<double> s(n);
        const double f = double(k) * fsBase / double(N);
        for (size_t i = 0; i < n; ++i)
            s[i] = amp * std::sin(2 * M_PI * f * double(i) / (fsBase * M));
        return s;
    };
    struct Case { const char* name; double amp; int k; };
    const Case clip[] = { { "0.3 V 1 kHz", 0.3, kLow }, { "2 V 1 kHz", 2.0, kLow }, { "4.5 V 3 kHz", 4.5, kHigh } };
    const Case scr[] = { { "30 mV 1 kHz", 0.03, kLow }, { "200 mV 1 kHz", 0.2, kLow }, { "450 mV 3 kHz", 0.45, kHigh } };

    for (int circuit = 0; circuit < 2; ++circuit)
    {
        std::printf(circuit == 0 ? "== clipper ==\n" : "== Tube Screamer clipping stage, drive 1 ==\n");
        for (int ci = 0; ci < 3; ++ci)
        {
            const Case c = circuit == 0 ? clip[ci] : scr[ci];
            auto run = [&](auto tag, int M, Opts o) {
                using T = decltype(tag);
                const auto in = sine(c.amp, c.k, M, 4 * N * size_t(M));
                auto y = circuit == 0 ? clipper<T>(fsBase * M, in, o) : screamer<T>(fsBase * M, in, o);
                return decimate(y, M);
            };
            const auto ref = run(double{}, 64, Opts{ Method::TR, -1, true });
            std::printf(" %s\n", c.name);
            // methods vs oversampling, converged solve, in-band error re the 64x reference
            for (Method m : { Method::TR, Method::BE, Method::BDF2 })
            {
                std::printf("  %-4s", m == Method::TR ? "TR" : m == Method::BE ? "BE" : "BDF2");
                for (int M : { 1, 2, 4, 8 })
                    std::printf(" | %dx %7.1f", M, bandErrDb(run(double{}, M, Opts{ m, -1, true }), ref));
                std::printf("\n");
            }
            // TR's ringing: energy above 20 kHz at 1x, TR against BDF2
            std::printf("  above 20 kHz at 1x: TR %6.1f dB, BDF2 %6.1f dB, ref (64x, decimated) %6.1f dB\n",
                        aboveBandDb(run(double{}, 1, Opts{ Method::TR, -1, true })),
                        aboveBandDb(run(double{}, 1, Opts{ Method::BDF2, -1, true })), aboveBandDb(ref));
            // fixed Newton counts, in band, against the converged solve of the same method and rate
            for (int M : { 1, 4 })
            {
                for (Method m : { Method::TR, Method::BDF2 })
                {
                    const auto conv = run(double{}, M, Opts{ m, -1, true });
                    std::printf("  K at %dx %-4s:", M, m == Method::TR ? "TR" : "BDF2");
                    for (int K = 1; K <= 3; ++K)
                        std::printf(" K=%d prev %6.1f extr %6.1f |", K, bandErrDb(run(double{}, M, Opts{ m, K, false }), conv),
                                    bandErrDb(run(double{}, M, Opts{ m, K, true }), conv));
                    std::printf(" float K=2 extr %6.1f, float K=8 %6.1f\n", bandErrDb(run(float{}, M, Opts{ m, 2, true }), conv),
                                bandErrDb(run(float{}, M, Opts{ m, 8, true }), conv));
                }
            }
        }
    }
    return 0;
}
