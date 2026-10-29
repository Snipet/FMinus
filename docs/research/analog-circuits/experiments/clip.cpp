// Scratch experiment for the circuit-compiler research note: two DK-style scalar circuits
//  (1) Yeh's RC diode clipper (2.2k, 10 nF, 1N914 pair; Is = 2.52e-9, nVt = 45 mV)
//  (2) the Tube Screamer clipping stage (Yeh/Abel/Smith DAFx-07, fig. 14): R1 4.7k + Cz 47n to ground,
//      feedback R2 = 51k + D*500k || Cc 51p || 1N914 pair, ideal op amp; Vo = Vi + V.
// Trapezoidal companions (Holters/Zoelzer DAFx-11 form). Measures:
//  A. solver error of a fixed Newton count (two initial guesses) against a converged solve, same rate
//  B. float32 against double, same algorithm
//  C. non-harmonic energy (aliasing) vs oversampling, decimated by a long windowed-sinc FIR in double
//  D. closed-form Wright-omega solutions (D'Angelo et al. DAFx-19, omega3/omega4) for the clipper
//  E. rough cost per sample (scalar, -O2, -ffp-contract=off)
#include <chrono>
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

// ---- the scalar nonlinear equation both circuits reduce to: v = p - k sinh(v / vt) ----------------------------
template <class T> struct Solve
{
    T k, vt;
    // Newton, `iters` steps, from guess g; each iterate kept inside the bracket [min(0,p), max(0,p)] (f monotone).
    T newton(T p, T g, int iters) const
    {
        const T lo = std::fmin(T(0), p), hi = std::fmax(T(0), p);
        T v = std::fmin(std::fmax(g, lo), hi);
        for (int i = 0; i < iters; ++i)
        {
            const T e = std::exp(v / vt), ei = T(1) / e;
            const T s = T(0.5) * (e - ei), c = T(0.5) * (e + ei);
            const T f = v - p + k * s;
            const T d = T(1) + (k / vt) * c;
            v = std::fmin(std::fmax(v - f / d, lo), hi);
        }
        return v;
    }
    double converged(double p, double g) const
    {
        double v = g;
        const double lo = std::fmin(0.0, p), hi = std::fmax(0.0, p);
        v = std::fmin(std::fmax(v, lo), hi);
        for (int i = 0; i < 200; ++i)
        {
            const double e = std::exp(v / double(vt)), ei = 1 / e;
            const double f = v - p + double(k) * 0.5 * (e - ei);
            const double d = 1 + (double(k) / double(vt)) * 0.5 * (e + ei);
            double nv = v - f / d;
            nv = std::fmin(std::fmax(nv, lo), hi);
            if (std::fabs(nv - v) < 1e-15)
                return nv;
            v = nv;
        }
        return v;
    }
};

// Wright omega: exact by Newton in double; approximations omega3, omega4 (D'Angelo, Gabrielli, Turchet DAFx-19).
static double omegaExact(double x)
{
    double w = x < 1 ? std::exp(x) : x - std::log(x);   // start
    for (int i = 0; i < 60; ++i)
    {
        const double f = w + std::log(w) - x, d = 1 + 1 / w;
        const double nw = w - f / d;
        if (std::fabs(nw - w) < 1e-15 * std::fmax(1.0, nw))
            return nw;
        w = nw > 0 ? nw : w * 0.5;
    }
    return w;
}
template <class T> static T omega3(T x)
{
    const T x1 = T(-3.341459552768620), x2 = T(8.0);
    const T a = T(-1.314293149877800e-3), b = T(4.775931364975583e-2), c = T(3.631952663804445e-1),
            d = T(6.313183464296682e-1);
    if (x <= x1)
        return T(0);
    if (x < x2)
        return ((a * x + b) * x + c) * x + d;
    return x - std::log(x);
}
template <class T> static T omega4(T x)
{
    const T w = omega3(x);
    return w - (w - std::exp(x - w)) / (w + T(1));
}

// ---- circuits ---------------------------------------------------------------------------------------------------
struct Clipper   // Yeh's RC diode clipper
{
    double R = 2200, C = 10e-9, Is = 2.52e-9, vt = 0.045;
};
struct Screamer  // TS808/TS9 clipping stage, drive D
{
    double R1 = 4700, Cz = 47e-9, Cc = 51e-12, Is = 2.52e-9, vt = 0.045, D = 1.0;
};

enum class Guess { previous, extrapolated };

// Runs n samples of the clipper at rate fs on input `in`; K < 0: converged solve.
template <class T> static std::vector<double> runClipper(const Clipper& c, double fs, const std::vector<double>& in,
                                                         int K, Guess guess)
{
    const T G = T(1 / c.R), GC = T(2 * c.C * fs);
    const T inv = T(1) / (G + GC);
    Solve<T> s{ T(2 * c.Is) * inv, T(c.vt) };
    T x = 0, v = 0, pPrev = 0;
    std::vector<double> out(in.size());
    for (size_t i = 0; i < in.size(); ++i)
    {
        const T p = (G * T(in[i]) + x) * inv;
        T g = v;
        if (guess == Guess::extrapolated)
        {
            const T e = std::exp(v / s.vt);
            const T d = T(1) + (s.k / s.vt) * T(0.5) * (e + T(1) / e);
            g = v + (p - pPrev) / d;   // linearised at the last solution (Holters & Zoelzer EUSIPCO-16, eq. 13)
        }
        v = K < 0 ? T(s.converged(double(p), double(g))) : s.newton(p, g, K);
        x = T(2) * GC * v - x;
        pPrev = p;
        out[i] = double(v);
    }
    return out;
}

// The same clipper solved in closed form by omega (one exponential per diode branch, the other diode ignored).
template <class T> static std::vector<double> runClipperOmega(const Clipper& c, double fs, const std::vector<double>& in,
                                                              int which)
{
    const T G = T(1 / c.R), GC = T(2 * c.C * fs);
    const T inv = T(1) / (G + GC);
    const T a = T(c.Is) * inv;   // v + a (e^{v/vt} - 1) = p for p >= 0 (k/2 with k = 2 Is / (G + GC))
    const T vt = T(c.vt);
    const T lg = std::log(a / vt);
    T x = 0;
    std::vector<double> out(in.size());
    for (size_t i = 0; i < in.size(); ++i)
    {
        const T p = (G * T(in[i]) + x) * inv;
        const T q = std::fabs(p);
        const T arg = (q + a) / vt + lg;
        T w;
        if (which == 0)
            w = T(omegaExact(double(arg)));
        else if (which == 3)
            w = omega3(arg);
        else
            w = omega4(arg);
        T v = q + a - vt * w;
        v = p < 0 ? -v : v;
        x = T(2) * GC * v - x;
        out[i] = double(v);
    }
    return out;
}

template <class T> static std::vector<double> runScreamer(const Screamer& c, double fs, const std::vector<double>& in,
                                                          int K, Guess guess)
{
    const T GCz = T(2 * c.Cz * fs), GCc = T(2 * c.Cc * fs), G1 = T(1 / c.R1);
    const T G2 = T(1 / (51e3 + c.D * 500e3));
    const T invA = T(1) / (G1 + GCz), invB = T(1) / (G2 + GCc);
    Solve<T> s{ T(2 * c.Is) * invB, T(c.vt) };
    T xz = 0, xc = 0, v = 0, pPrev = 0;
    std::vector<double> out(in.size());
    for (size_t i = 0; i < in.size(); ++i)
    {
        const T vi = T(in[i]);
        const T va = (vi * G1 + xz) * invA;      // node between R1 and Cz
        const T In = (vi - va) * G1;             // current through Zs
        const T p = (In + xc) * invB;
        T g = v;
        if (guess == Guess::extrapolated)
        {
            const T e = std::exp(v / s.vt);
            const T d = T(1) + (s.k / s.vt) * T(0.5) * (e + T(1) / e);
            g = v + (p - pPrev) / d;
        }
        v = K < 0 ? T(s.converged(double(p), double(g))) : s.newton(p, g, K);
        xz = T(2) * GCz * va - xz;
        xc = T(2) * GCc * v - xc;
        pPrev = p;
        out[i] = double(vi + v);   // Vo = Vi + V
    }
    return out;
}

// ---- measures ---------------------------------------------------------------------------------------------------
static double errDb(const std::vector<double>& a, const std::vector<double>& ref, size_t from)
{
    double e = 0, s = 0;
    for (size_t i = from; i < a.size(); ++i)
    {
        e += (a[i] - ref[i]) * (a[i] - ref[i]);
        s += ref[i] * ref[i];
    }
    return e == 0 ? -400 : 10 * std::log10(e / s);
}

// Windowed-sinc decimator (Kaiser-ish via Blackman-Harris; 48 zero crossings a side), double.
static std::vector<double> decimate(const std::vector<double>& x, int M)
{
    if (M == 1)
        return x;
    const int half = 48 * M;
    std::vector<double> h(2 * half + 1);
    const double fc = 0.5 / M * 0.92;   // cutoff a little under the base Nyquist (22.1 kHz at 48 k)
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

// Energy below 20 kHz that is not on a harmonic of bin k, relative to all energy below 20 kHz (dB).
static double inharmonicDb(const std::vector<double>& y, size_t start, size_t N, int k, double fs)
{
    std::vector<cd> a(N);
    for (size_t i = 0; i < N; ++i)
        a[i] = y[start + i];
    fft(a);
    double harm = 0, other = 0;
    const size_t top = size_t(20000.0 / fs * double(N));
    for (size_t b = 1; b <= top; ++b)
    {
        const double e = std::norm(a[b]);
        if (b % size_t(k) == 0)
            harm += e;
        else
            other += e;
    }
    return 10 * std::log10(other / (harm + other));
}

int main()
{
    const double fs = 48000;
    const size_t N = 65536;
    const int kLow = 1365, kHigh = 4097;   // 999.8 Hz and 3000.7 Hz, on bins
    Clipper cl;
    Screamer ts;

    auto sine = [&](double amp, int k, double rate, size_t n) {
        std::vector<double> s(n);
        const double f = double(k) * fs / double(N);
        for (size_t i = 0; i < n; ++i)
            s[i] = amp * std::sin(2 * M_PI * f * double(i) / rate);
        return s;
    };

    // ---- A and B: solver error and precision, at 1x ----
    std::printf("A/B. Newton count and precision, error against a converged double solve at the same rate (dB re signal)\n");
    struct Case { const char* name; double amp; int k; };
    const Case cases[] = { { "0.3 V, 1 kHz", 0.3, kLow }, { "2 V, 1 kHz", 2.0, kLow }, { "4.5 V, 3 kHz", 4.5, kHigh } };
    for (int circuit = 0; circuit < 2; ++circuit)
    {
        std::printf(circuit == 0 ? " clipper (2.2k, 10n, 1N914 pair)\n" : " Tube Screamer clipping stage, drive 1 (551k)\n");
        for (const Case& c : cases)
        {
            const double amp = circuit == 0 ? c.amp : c.amp * 0.1;   // TS input: the guitar, 30 mV .. 450 mV
            const auto in = sine(amp, c.k, fs, 3 * N);
            auto run = [&](auto tag, int K, Guess g) {
                using T = decltype(tag);
                return circuit == 0 ? runClipper<T>(cl, fs, in, K, g) : runScreamer<T>(ts, fs, in, K, g);
            };
            const auto ref = run(double{}, -1, Guess::previous);
            std::printf("  %-14s", circuit == 0 ? c.name : (c.amp == 0.3 ? "30 mV, 1 kHz" : c.amp == 2 ? "200 mV, 1 kHz" : "450 mV, 3 kHz"));
            for (int K = 1; K <= 3; ++K)
                std::printf(" | K=%d prev %7.1f extr %7.1f", K, errDb(run(double{}, K, Guess::previous), ref, N),
                            errDb(run(double{}, K, Guess::extrapolated), ref, N));
            std::printf(" | float K=2 extr %7.1f, float converged %7.1f\n",
                        errDb(run(float{}, 2, Guess::extrapolated), ref, N), errDb(run(float{}, 8, Guess::extrapolated), ref, N));
        }
    }

    // ---- D: omega closed forms for the clipper ----
    std::printf("\nD. clipper in closed form (one diode at a time), error against the converged sinh model (dB re signal)\n");
    for (const Case& c : cases)
    {
        const auto in = sine(c.amp, c.k, fs, 3 * N);
        const auto ref = runClipper<double>(cl, fs, in, -1, Guess::previous);
        std::printf("  %-14s exact omega %7.1f | omega3 (float) %7.1f | omega4 (float) %7.1f\n", c.name,
                    errDb(runClipperOmega<double>(cl, fs, in, 0), ref, N), errDb(runClipperOmega<float>(cl, fs, in, 3), ref, N),
                    errDb(runClipperOmega<float>(cl, fs, in, 4), ref, N));
    }

    // ---- C: aliasing vs oversampling ----
    std::printf("\nC. inharmonic energy below 20 kHz (aliasing + numerical noise), dB re all energy below 20 kHz; converged solve\n");
    for (int circuit = 0; circuit < 2; ++circuit)
    {
        std::printf(circuit == 0 ? " clipper\n" : " Tube Screamer stage, drive 1\n");
        for (const Case& c : cases)
        {
            const double amp = circuit == 0 ? c.amp : c.amp * 0.1;
            std::printf("  %-14s", circuit == 0 ? c.name : (c.amp == 0.3 ? "30 mV, 1 kHz" : c.amp == 2 ? "200 mV, 1 kHz" : "450 mV, 3 kHz"));
            for (int M : { 1, 2, 4, 8 })
            {
                const double rate = fs * M;
                const auto in = sine(amp, c.k, rate, 3 * N * size_t(M));
                const auto y = circuit == 0 ? runClipper<double>(cl, rate, in, -1, Guess::previous)
                                            : runScreamer<double>(ts, rate, in, -1, Guess::previous);
                const auto d = decimate(y, M);
                std::printf(" | %dx %7.1f", M, inharmonicDb(d, N, N, c.k, fs));
            }
            std::printf("\n");
        }
    }

    // ---- E: cost ----
    std::printf("\nE. cost, float, scalar loop, ns/sample (best of 5; the machine is shared)\n");
    {
        const size_t n = 1 << 21;
        const auto in = sine(2.0, kLow, fs, n);
        auto time = [&](auto&& f) {
            double best = 1e9;
            for (int r = 0; r < 5; ++r)
            {
                const auto t0 = std::chrono::steady_clock::now();
                volatile double sink = f()[n / 2];
                (void)sink;
                const auto t1 = std::chrono::steady_clock::now();
                best = std::fmin(best, std::chrono::duration<double, std::nano>(t1 - t0).count() / double(n));
            }
            return best;
        };
        std::printf("  clipper Newton K=1 extr %.1f, K=2 extr %.1f, K=3 prev %.1f, omega3 %.1f, omega4 %.1f\n",
                    time([&] { return runClipper<float>(cl, fs, in, 1, Guess::extrapolated); }),
                    time([&] { return runClipper<float>(cl, fs, in, 2, Guess::extrapolated); }),
                    time([&] { return runClipper<float>(cl, fs, in, 3, Guess::previous); }),
                    time([&] { return runClipperOmega<float>(cl, fs, in, 3); }),
                    time([&] { return runClipperOmega<float>(cl, fs, in, 4); }));
        const auto in2 = sine(0.2, kLow, fs, n);
        std::printf("  screamer Newton K=2 extr %.1f\n", time([&] { return runScreamer<float>(ts, fs, in2, 2, Guess::extrapolated); }));
    }
    return 0;
}
