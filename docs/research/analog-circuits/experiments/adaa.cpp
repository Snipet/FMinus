// Stateful first-order ADAA (Holters, Appl. Sci. 10(1):20, 2020) on the two 1-port blocks of the report:
// example 1 (DiodeTone: R1-C1-diode pair, then R2 + tone rheostat into C2) and example 2's block 2 (the TS
// clipping stage). Both blocks are cast in Holters' form
//     p(n) = Cp x(n-1) + dp u(n),   v = G(p) solves v + k sinh(v/vt) = p,
//     x(n) = A x(n-1) + B u(n) + W v,   y(n) = Cy x(n-1) + Dy u(n) + wy v.
// ADAA replaces v by (F(p1) - F(p0)) / (p1 - p0), F' = G, averages the linear terms over two samples, and designs every
// coefficient for fs' = 2/3 fs (Holters eqs. 16-19). F needs no table: for the inverse of h(v) = v + k sinh(v/vt),
//     F(p) = p v - H(v),  H(v) = v^2/2 + k vt cosh(v/vt)   (integration by parts; dF/dv = p - h(v) = 0 at the root).
// Measures: in-band error against a converged 64x non-ADAA reference (ADAA's half-sample delay compensated), and
// the inharmonic (alias) energy below 20 kHz.
#include <chrono>
#include <cmath>
#include <complex>
#include <cstdio>
#include <cstring>
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

struct Block
{
    double Cp[2], dp;
    double A[2][2], B[2], W[2];
    double Cy[2], Dy, wy;
    double k, vt;
};

static const double Is = 2.52e-9, NVt = 0.0453;

// Example 2, block 2. Drive: the audio taper (81^d - 1)/80 of 500k, plus 51k.
static Block makeTS(double fs, double drive)
{
    const double g4 = 1 / 4700.0, gc3 = 2 * 47e-9 * fs, gc4 = 2 * 51e-12 * fs;
    const double inA = 1 / (g4 + gc3);
    const double rf = 51e3 + 500e3 * (std::pow(81.0, drive) - 1) / 80;
    const double invB = 1 / (1 / rf + gc4);
    Block b{};
    // va = (u g4 + x3) inA;  p = ((u - va) g4 + x4) invB
    b.Cp[0] = -g4 * inA * invB;
    b.Cp[1] = invB;
    b.dp = (g4 - g4 * g4 * inA) * invB;
    b.A[0][0] = 2 * gc3 * inA - 1;
    b.A[0][1] = 0;
    b.A[1][0] = 0;
    b.A[1][1] = -1;
    b.B[0] = 2 * gc3 * inA * g4;
    b.B[1] = 0;
    b.W[0] = 0;
    b.W[1] = 2 * gc4;
    b.Cy[0] = b.Cy[1] = 0;
    b.Dy = 1;
    b.wy = 1;
    b.k = 2 * Is * invB;
    b.vt = NVt;
    return b;
}

// Example 1 (DiodeTone). Tone: Ptone = 25k (1 - tone).
static Block makeDT(double fs, double tone)
{
    const double g1 = 1 / 2200.0, gc1 = 2 * 10e-9 * fs, gc2 = 2 * 22e-9 * fs;
    const double gs = 1 / (470 + 25000 * (1 - tone));
    const double s11 = g1 + gc1 + gs, s22 = gs + gc2, det = s11 * s22 - gs * gs, inv = 1 / det;
    const double cin = s22 * g1 * inv, cx1 = s22 * inv, cx2 = gs * inv, kk = s22 * inv, gk = det / s22;
    const double din = gs * g1 * inv, dx1 = gs * inv, dx2 = s11 * inv, di = gs * inv;
    Block b{};
    b.Cp[0] = cx1;
    b.Cp[1] = cx2;
    b.dp = cin;
    // i = (p - v) gk;  y = din u + dx1 x1 + dx2 x2 - di i
    b.Cy[0] = dx1 - di * gk * cx1;
    b.Cy[1] = dx2 - di * gk * cx2;
    b.Dy = din - di * gk * cin;
    b.wy = di * gk;
    b.A[0][0] = -1;
    b.A[0][1] = 0;
    b.A[1][0] = 2 * gc2 * b.Cy[0];
    b.A[1][1] = 2 * gc2 * b.Cy[1] - 1;
    b.B[0] = 0;
    b.B[1] = 2 * gc2 * b.Dy;
    b.W[0] = 2 * gc1;
    b.W[1] = 2 * gc2 * b.wy;
    b.k = 2 * Is * kk;
    b.vt = NVt;
    return b;
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
template <class T> static T newtonSteps(T p, T k, T vt, T g, int K)
{
    const T lo = std::fmin(T(0), p), hi = std::fmax(T(0), p);
    T v = std::fmin(std::fmax(g, lo), hi);
    for (int i = 0; i < K; ++i)
    {
        const T e = std::exp(v / vt), ei = T(1) / e;
        const T f = v - p + k * T(0.5) * (e - ei);
        const T d = T(1) + (k / vt) * T(0.5) * (e + ei);
        v = std::fmin(std::fmax(v - f / d, lo), hi);
    }
    return v;
}
static long gNotConverged = 0;
template <class T> static T omegaGuess(T p, T k, T vt);
static double converged(double p, double k, double vt, double g)
{
    const double lo = std::fmin(0.0, p), hi = std::fmax(0.0, p);
    // start from the closed form (a previous-sample start can need hundreds of steps after a large jump in p)
    (void)g;
    double v = std::fmin(std::fmax(omegaGuess(p, k, vt), lo), hi);
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
    ++gNotConverged;
    return v;
}
template <class T> static T omegaGuess(T p, T k, T vt)
{
    const T a = T(0.5) * k, q = std::fabs(p);
    const T v = q + a - vt * omega3((q + a) / vt + std::log(a / vt));
    return p < 0 ? -v : v;
}

enum class Guess { omega, prev };
struct Opts
{
    int K = -1;          // -1: converged (double)
    Guess guess = Guess::omega;
    bool adaa = false;
    double eps = 1e-5;   // ADAA: |dp| below this (volts) uses the endpoint average
};

template <class T> struct Sim
{
    T Cp[2], dp, A[2][2], B[2], W[2], Cy[2], Dy, wy, k, vt;
    explicit Sim(const Block& b)
    {
        for (int i = 0; i < 2; ++i)
        {
            Cp[i] = T(b.Cp[i]);
            B[i] = T(b.B[i]);
            W[i] = T(b.W[i]);
            Cy[i] = T(b.Cy[i]);
            for (int j = 0; j < 2; ++j)
                A[i][j] = T(b.A[i][j]);
        }
        dp = T(b.dp);
        Dy = T(b.Dy);
        wy = T(b.wy);
        k = T(b.k);
        vt = T(b.vt);
    }
    T solve(T p, T vprev, const Opts& o) const
    {
        if (o.K < 0)
            return T(converged(double(p), double(k), double(vt), double(vprev)));
        const T g = o.guess == Guess::omega ? omegaGuess(p, k, vt) : vprev;
        return newtonSteps(p, k, vt, g, o.K);
    }
};

template <class T> static std::vector<double> run(const Block& blk, const std::vector<double>& in, const Opts& o)
{
    Sim<T> s(blk);
    T x1[2] = { 0, 0 }, x2[2] = { 0, 0 };   // x(n-1), x(n-2)
    T u1 = 0, p1 = 0, v1 = 0, c1 = 1;        // u(n-1), p(n-1), v(n-1), cosh(v(n-1)/vt)
    std::vector<double> out(in.size());
    for (size_t n = 0; n < in.size(); ++n)
    {
        const T u = T(in[n]);
        const T p = s.Cp[0] * x1[0] + s.Cp[1] * x1[1] + s.dp * u;
        const T v = s.solve(p, v1, o);
        if (!o.adaa)
        {
            out[n] = double(s.Cy[0] * x1[0] + s.Cy[1] * x1[1] + s.Dy * u + s.wy * v);
            const T n0 = s.A[0][0] * x1[0] + s.A[0][1] * x1[1] + s.B[0] * u + s.W[0] * v;
            const T n1 = s.A[1][0] * x1[0] + s.A[1][1] * x1[1] + s.B[1] * u + s.W[1] * v;
            x1[0] = n0;
            x1[1] = n1;
            v1 = v;
            continue;
        }
        // first-order ADAA of the solved map, in the cancellation-free form
        //   vbar = vm + [dv (pm - vm) - k vt (c - c1)] / dp
        const T e = std::exp(v / s.vt);
        const T c = T(0.5) * (e + T(1) / e);
        const T dpv = p - p1, dv = v - v1, vm = T(0.5) * (v + v1), pm = T(0.5) * (p + p1);
        T vbar = vm;
        if (std::fabs(dpv) > T(o.eps))
            vbar = vm + (dv * (pm - vm) - s.k * s.vt * (c - c1)) / dpv;
        const T xa0 = T(0.5) * (x1[0] + x2[0]), xa1 = T(0.5) * (x1[1] + x2[1]), ua = T(0.5) * (u + u1);
        out[n] = double(s.Cy[0] * xa0 + s.Cy[1] * xa1 + s.Dy * ua + s.wy * vbar);
        const T n0 = s.A[0][0] * xa0 + s.A[0][1] * xa1 + s.B[0] * ua + s.W[0] * vbar;
        const T n1 = s.A[1][0] * xa0 + s.A[1][1] * xa1 + s.B[1] * ua + s.W[1] * vbar;
        x2[0] = x1[0];
        x2[1] = x1[1];
        x1[0] = n0;
        x1[1] = n1;
        u1 = u;
        p1 = p;
        v1 = v;
        c1 = c;
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
static size_t N = 32768;

// In-band error of a against ref delayed by `delay` host samples (periodic signals: exact as a phase shift).
static double bandErrDb(const std::vector<double>& a, const std::vector<double>& ref, double delay)
{
    const size_t s = 2 * N;   // away from both ends (the decimator's edges)
    std::vector<cd> A(N), R(N);
    for (size_t i = 0; i < N; ++i)
    {
        A[i] = a[s + i];
        R[i] = ref[s + i];
    }
    fft(A);
    fft(R);
    const size_t top = size_t(20000.0 / fsBase * double(N));
    double ee = 0, rr = 0;
    for (size_t b = 1; b <= top; ++b)
    {
        const cd sh = std::polar(1.0, -2 * M_PI * double(b) * delay / double(N));
        ee += std::norm(A[b] - R[b] * sh);
        rr += std::norm(R[b]);
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
    const size_t top = size_t(20000.0 / fsBase * double(N));
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
// Gain of the fundamental and of H3 against the reference, dB (ADAA's lowpass and warping show here).
static void harmonicDiff(const std::vector<double>& y, const std::vector<double>& ref, int k, double* h1, double* h3)
{
    const size_t s = 2 * N;
    std::vector<cd> a(N), r(N);
    for (size_t i = 0; i < N; ++i)
    {
        a[i] = y[s + i];
        r[i] = ref[s + i];
    }
    fft(a);
    fft(r);
    *h1 = 20 * std::log10(std::abs(a[size_t(k)]) / std::abs(r[size_t(k)]));
    *h3 = 20 * std::log10(std::abs(a[size_t(3 * k)]) / std::abs(r[size_t(3 * k)]) + 1e-30);
}

int main(int argc, char** argv)
{
    const bool quick = argc > 1 && std::strcmp(argv[1], "quick") == 0;
    const int kLow = 683, kHigh = 2049;   // ~1000.5 Hz and ~3001.5 Hz on bins of N = 32768 at 48 kHz
    auto sine = [&](double amp, int k, int M, size_t n) {
        std::vector<double> s(n);
        const double f = double(k) * fsBase / double(N);
        for (size_t i = 0; i < n; ++i)
            s[i] = amp * std::sin(2 * M_PI * f * double(i) / (fsBase * M));
        return s;
    };
    struct Case { const char* name; double amp; int k; };
    const Case dtCases[] = { { "0.3 V 1 kHz", 0.3, kLow }, { "2 V 1 kHz", 2.0, kLow }, { "4.5 V 3 kHz", 4.5, kHigh } };
    const Case tsCases[] = { { "30 mV 1 kHz", 0.03, kLow }, { "200 mV 1 kHz", 0.2, kLow }, { "450 mV 3 kHz", 0.45, kHigh } };
    const double pots[] = { 0.0, 0.5, 1.0 };

    if (argc > 1 && std::strcmp(argv[1], "worst") == 0)
    {
        // Worst case over pots {0, .5, 1} x the three cases, per circuit and rate, of every candidate.
        for (int circuit = 0; circuit < 2; ++circuit)
        {
            std::printf(circuit == 0 ? "== Example 1 (DiodeTone) worst case ==\n" : "== Example 2 block 2 (TS clip) worst case ==\n");
            for (int M : { 1, 2, 4, 8 })
            {
                double aliasP = -999, aliasA = -999, h3P = 0, h3A = 0, h1A = 0;
                double wP[3] = { -999, -999, -999 }, pP[6] = { -999, -999, -999, -999, -999, -999 }, wA[3] = { -999, -999, -999 };
                for (double pot : pots)
                    for (int ci = 0; ci < 3; ++ci)
                    {
                        const Case c = circuit == 0 ? dtCases[ci] : tsCases[ci];
                        auto blockAt = [&](double rate, bool adaa) {
                            const double design = adaa ? rate * 2.0 / 3.0 : rate;
                            return circuit == 0 ? makeDT(design, pot) : makeTS(design, pot);
                        };
                        auto runIt = [&](auto tag, int MM, Opts o) {
                            using T = decltype(tag);
                            const auto in = sine(c.amp, c.k, MM, 4 * N * size_t(MM));
                            return decimate(run<T>(blockAt(fsBase * MM, o.adaa), in, o), MM);
                        };
                        const auto ref = runIt(double{}, 64, Opts{});
                        const auto plain = runIt(double{}, M, Opts{});
                        Opts oa;
                        oa.adaa = true;
                        const auto adaa = runIt(double{}, M, oa);
                        aliasP = std::fmax(aliasP, inharmonicDb(plain, c.k));
                        aliasA = std::fmax(aliasA, inharmonicDb(adaa, c.k));
                        double h1, h3;
                        harmonicDiff(plain, ref, c.k, &h1, &h3);
                        h3P = std::fmin(h3P, h3);
                        harmonicDiff(adaa, ref, c.k, &h1, &h3);
                        h3A = std::fmin(h3A, h3);
                        h1A = std::fmin(h1A, h1);
                        for (int K = 0; K < 3; ++K)
                        {
                            Opts f;
                            f.K = K;
                            wP[K] = std::fmax(wP[K], bandErrDb(runIt(float{}, M, f), plain, 0));
                            f.adaa = true;
                            wA[K] = std::fmax(wA[K], bandErrDb(runIt(float{}, M, f), adaa, 0));
                        }
                        for (int K = 1; K <= 6; ++K)
                        {
                            Opts f;
                            f.K = K;
                            f.guess = Guess::prev;
                            pP[K - 1] = std::fmax(pP[K - 1], bandErrDb(runIt(float{}, M, f), plain, 0));
                        }
                    }
                std::printf(" %dx | alias plain %6.1f ADAA %6.1f | H3 plain %+5.2f ADAA %+5.2f, H1 ADAA %+5.2f dB | solver plain w K0/K1/K2 %6.1f %6.1f %6.1f"
                            " | prev K1..K6 %6.1f %6.1f %6.1f %6.1f %6.1f %6.1f | ADAA w K0/K1/K2 %6.1f %6.1f %6.1f\n",
                            M, aliasP, aliasA, h3P, h3A, h1A, wP[0], wP[1], wP[2], pP[0], pP[1], pP[2], pP[3], pP[4], pP[5], wA[0], wA[1], wA[2]);
                std::fflush(stdout);
            }
        }
        return 0;
    }
    for (int circuit = 0; circuit < 2; ++circuit)
    {
        std::printf(circuit == 0 ? "== Example 1 (DiodeTone), pot = tone ==\n" : "== Example 2 block 2 (TS clip), pot = drive ==\n");
        for (double pot : pots)
        {
            if (quick && pot != 1.0)
                continue;
            for (int ci = 0; ci < 3; ++ci)
            {
                const Case c = circuit == 0 ? dtCases[ci] : tsCases[ci];
                auto blockAt = [&](double rate, bool adaa) {
                    const double design = adaa ? rate * 2.0 / 3.0 : rate;
                    return circuit == 0 ? makeDT(design, pot) : makeTS(design, pot);
                };
                auto runIt = [&](auto tag, int M, Opts o) {
                    using T = decltype(tag);
                    const auto in = sine(c.amp, c.k, M, 4 * N * size_t(M));
                    return decimate(run<T>(blockAt(fsBase * M, o.adaa), in, o), M);
                };
                const auto ref = runIt(double{}, 64, Opts{});
                std::printf(" pot %.1f, %s\n", pot, c.name);
                for (int M : { 1, 2, 4, 8 })
                {
                    const auto plain = runIt(double{}, M, Opts{});
                    Opts oa;
                    oa.adaa = true;
                    const auto adaa = runIt(double{}, M, oa);
                    Opts fa = oa;
                    fa.K = 1;
                    const auto adaaF = runIt(float{}, M, fa);
                    Opts fp;
                    fp.K = 1;
                    const auto plainF = runIt(float{}, M, fp);
                    double h1a, h3a;
                    harmonicDiff(adaa, ref, c.k, &h1a, &h3a);
                    const double d = 0.5 / M;
                    std::printf("  %dx | plain: alias %6.1f null %6.1f | ADAA: alias %6.1f null %6.1f H1 %+5.2f H3 %+5.2f dB"
                                " | float w+K1 vs same-rate double: plain %6.1f ADAA %6.1f\n",
                                M, inharmonicDb(plain, c.k), bandErrDb(plain, ref, 0), inharmonicDb(adaa, c.k),
                                bandErrDb(adaa, ref, d), h1a, h3a, bandErrDb(plainF, plain, 0), bandErrDb(adaaF, adaa, 0));
                }
            }
        }
    }
    return 0;
}
