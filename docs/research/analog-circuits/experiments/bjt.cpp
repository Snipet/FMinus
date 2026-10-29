// Biased BJT circuits through dk.hpp: (1) float against double on a 9 V common-emitter stage (the Rangemaster's
// topology, silicon and germanium-like devices), absolute states against offsets from the DC operating point;
// (2) the Fuzz Face's 4-port Newton: the K matrix's structure, iterations per sample, and fixed-K accuracy.
#include "dk.hpp"

#include <complex>
#include <cstring>

using namespace dk;
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

struct Built
{
    Netlist net;
    std::vector<double> u0;   // inputs at the operating point (signal 0, supplies on)
};

// Rangemaster topology as an NPN stage on +9 V [M: values from the common schematic, not checked against one]
static Built rangemaster(bool germanium)
{
    Built b;
    Netlist& n = b.net;
    const int in = n.node(), x = n.node(), base = n.node(), col = n.node(), em = n.node(), out = n.node(), vcc = n.node();
    n.inputs = 2;
    n.vs.push_back({ in, 0, 0 });
    n.vs.push_back({ vcc, 0, 1 });
    n.rs.push_back({ in, x, 1e3 });       // source
    n.cs.push_back({ x, base, 5e-9 });    // input cap
    n.rs.push_back({ vcc, base, 470e3 });
    n.rs.push_back({ base, 0, 68e3 });
    n.rs.push_back({ em, 0, 3.9e3 });
    n.cs.push_back({ em, 0, 47e-6 });
    n.rs.push_back({ vcc, col, 10e3 });
    n.cs.push_back({ col, out, 10e-9 });
    n.rs.push_back({ out, 0, 10e3 });
    n.ports.push_back({ base, em, base, em });   // B->E
    n.ports.push_back({ col, em, col, em });     // C->E
    Device q{ Dev::BJT, 0 };
    if (germanium)
    {
        q.is = 5e-6;   // germanium-like [L]: Vbe ~ 0.2 V at 1 mA
        q.bf = 90;
        q.br = 20;
    }
    else
    {
        q.is = 1e-14;
        q.bf = 200;
        q.br = 4;
    }
    n.devs.push_back(q);
    n.outputs.push_back(out);
    n.outputs.push_back(col);
    b.u0 = { 0, 9 };
    return b;
}

// Fuzz Face, NPN silicon version [M: topology from coda-effects' analysis; values from the common schematic]
static bool gJunctionCaps = false;
static Built fuzzface(double fuzz)
{
    Built b;
    Netlist& n = b.net;
    const int in = n.node(), x = n.node(), b1 = n.node(), c1 = n.node(), c2 = n.node(), m = n.node(), e2 = n.node(),
              w = n.node(), out = n.node(), vcc = n.node();
    n.inputs = 2;
    n.vs.push_back({ in, 0, 0 });
    n.vs.push_back({ vcc, 0, 1 });
    n.rs.push_back({ in, x, 1e3 });
    n.cs.push_back({ x, b1, 2.2e-6 });
    n.rs.push_back({ vcc, c1, 33e3 });
    n.rs.push_back({ c2, m, 330 });
    n.rs.push_back({ m, vcc, 8.2e3 });
    n.rs.push_back({ e2, b1, 100e3 });
    n.rs.push_back({ e2, w, std::fmax(1.0, (1 - fuzz) * 1e3) });
    n.rs.push_back({ w, 0, std::fmax(1.0, fuzz * 1e3) });
    n.cs.push_back({ w, 0, 20e-6 });
    n.cs.push_back({ m, out, 10e-9 });
    n.rs.push_back({ out, 0, 500e3 });
    if (gJunctionCaps)   // junction capacitances (Holmes, Holters & van Walstijn DAFx-17: they relieve the solver) [L values]
    {
        n.cs.push_back({ b1, 0, 20e-12 });
        n.cs.push_back({ b1, c1, 5e-12 });
        n.cs.push_back({ c1, e2, 20e-12 });
        n.cs.push_back({ c1, c2, 5e-12 });
    }
    n.ports.push_back({ b1, 0, b1, 0 });   // Q1 B->E
    n.ports.push_back({ c1, 0, c1, 0 });   // Q1 C->E
    n.ports.push_back({ c1, e2, c1, e2 }); // Q2 B->E
    n.ports.push_back({ c2, e2, c2, e2 }); // Q2 C->E
    Device q{ Dev::BJT, 0 };
    q.is = 1e-14;
    q.bf = 200;
    q.br = 4;
    n.devs.push_back(q);
    q.port0 = 2;
    n.devs.push_back(q);
    n.outputs.push_back(out);
    n.outputs.push_back(c1);
    b.u0 = { 0, 9 };
    return b;
}

// Newton on g(v) = v - p - K i(v). Returns iterations used; K < 0: to convergence (tol), else exactly K steps.
template <class T> static int newton(const System& sd, const std::vector<T>& Kf, const Laws<T>& law, const T* p, const T* u, T* v,
                                     int K, double tol, bool limit, bool damped = false)
{
    const int M = sd.M;
    T i[8], J[64], g[8], a[64];
    const int maxIt = K < 0 ? 200 : K;
    int it = 0;
    for (; it < maxIt; ++it)
    {
        law.eval(v, u, i, J);
        for (int r = 0; r < M; ++r)
        {
            T s = v[r] - p[r];
            for (int c = 0; c < M; ++c)
                s -= Kf[static_cast<size_t>(r * M + c)] * i[c];
            g[r] = s;
            for (int c = 0; c < M; ++c)
            {
                T kj = 0;
                for (int q = 0; q < M; ++q)
                    kj += Kf[static_cast<size_t>(r * M + q)] * J[q * M + c];
                a[r * M + c] = (r == c ? T(1) : T(0)) - kj;
            }
        }
        solveSmall(M, a, g);
        T scale = 1;
        if (limit)   // keep every junction's forward step below 4 nVt (capped step, Holmes & van Walstijn)
            for (const Device& d : law.devs)
                if (d.kind == Dev::BJT)
                {
                    const int q = d.port0;
                    const T dbe = -g[q], dbc = -(g[q] - g[q + 1]);
                    const T cap = T(4 * d.nvt);
                    if (dbe > cap)
                        scale = std::fmin(scale, cap / dbe);
                    if (dbc > cap)
                        scale = std::fmin(scale, cap / dbc);
                }
        T mx = 0;
        if (damped)   // backtracking on the residual's norm (offline reference only: its cost is unbounded)
        {
            auto resid = [&](const T* vv) {
                T ii[8], JJ[64];
                law.eval(vv, u, ii, JJ);
                T nrm = 0;
                for (int r = 0; r < M; ++r)
                {
                    T s2 = vv[r] - p[r];
                    for (int c = 0; c < M; ++c)
                        s2 -= Kf[static_cast<size_t>(r * M + c)] * ii[c];
                    nrm += s2 * s2;
                }
                return nrm;
            };
            const T r0 = resid(v);
            T vt[8];
            for (int tries = 0; tries < 60; ++tries)
            {
                for (int r = 0; r < M; ++r)
                    vt[r] = v[r] - scale * g[r];
                const T r1 = resid(vt);
                if (std::isfinite(double(r1)) && r1 <= r0)
                    break;
                scale *= T(0.5);
            }
        }
        for (int r = 0; r < M; ++r)
        {
            v[r] -= scale * g[r];
            mx = std::fmax(mx, std::fabs(scale * g[r]));
        }
        if (K < 0 && double(mx) < tol)
            return it + 1;
    }
    return it;
}

struct Op
{
    std::vector<double> v0, i0, h0, y0;
};

// DC operating point: caps open (gmin 1e-12), source stepping, Newton to convergence in double.
static Op operatingPoint(const Built& b, const System& tr)
{
    const System dc = build(b.net, 1, true);
    Laws<double> law;
    law.devs = b.net.devs;
    law.M = dc.M;
    std::vector<double> Kf(static_cast<size_t>(dc.M * dc.M));
    for (int r = 0; r < dc.M; ++r)
        for (int c = 0; c < dc.M; ++c)
            Kf[static_cast<size_t>(r * dc.M + c)] = dc.K[static_cast<size_t>(r)][static_cast<size_t>(c)];
    std::vector<double> v(static_cast<size_t>(dc.M), 0.0), p(static_cast<size_t>(dc.M));
    for (int step = 1; step <= 50; ++step)
    {
        const double lam = step / 50.0;
        std::vector<double> u = b.u0;
        for (double& x : u)
            x *= lam;
        for (int r = 0; r < dc.M; ++r)
        {
            p[static_cast<size_t>(r)] = 0;
            for (int k = 0; k < dc.U; ++k)
                p[static_cast<size_t>(r)] += dc.Pu[static_cast<size_t>(r)][static_cast<size_t>(k)] * u[static_cast<size_t>(k)];
        }
        newton<double>(dc, Kf, law, p.data(), u.data(), v.data(), -1, 1e-13, true);
    }
    Op op;
    op.v0 = v;
    op.i0.resize(static_cast<size_t>(dc.M));
    std::vector<double> J(static_cast<size_t>(dc.M * dc.M));
    law.eval(v.data(), b.u0.data(), op.i0.data(), J.data());
    // capacitor voltages at DC (dc.B = Qu, dc.Cm = Qi there); TR steady state: h0 = gC vc0
    op.h0.resize(static_cast<size_t>(tr.N));
    for (int c = 0; c < tr.N; ++c)
    {
        double vc = 0;
        for (int k = 0; k < dc.U; ++k)
            vc += dc.B[static_cast<size_t>(c)][static_cast<size_t>(k)] * b.u0[static_cast<size_t>(k)];
        for (int j = 0; j < dc.M; ++j)
            vc += dc.Cm[static_cast<size_t>(c)][static_cast<size_t>(j)] * op.i0[static_cast<size_t>(j)];
        op.h0[static_cast<size_t>(c)] = tr.gC[static_cast<size_t>(c)] * vc;
    }
    op.y0.resize(static_cast<size_t>(tr.Y));
    for (int y = 0; y < tr.Y; ++y)
    {
        double s = 0;
        for (int k = 0; k < tr.U; ++k)
            s += tr.Yu[static_cast<size_t>(y)][static_cast<size_t>(k)] * b.u0[static_cast<size_t>(k)];
        for (int c = 0; c < tr.N; ++c)
            s += tr.Yh[static_cast<size_t>(y)][static_cast<size_t>(c)] * op.h0[static_cast<size_t>(c)];
        for (int j = 0; j < tr.M; ++j)
            s += tr.Yi[static_cast<size_t>(y)][static_cast<size_t>(j)] * op.i0[static_cast<size_t>(j)];
        op.y0[static_cast<size_t>(y)] = s;
    }
    // consistency: the transient model's port voltages at (u0, h0, i0) must equal v0
    double worst = 0;
    for (int r = 0; r < tr.M; ++r)
    {
        double s = 0;
        for (int k = 0; k < tr.U; ++k)
            s += tr.Pu[static_cast<size_t>(r)][static_cast<size_t>(k)] * b.u0[static_cast<size_t>(k)];
        for (int c = 0; c < tr.N; ++c)
            s += tr.Ph[static_cast<size_t>(r)][static_cast<size_t>(c)] * op.h0[static_cast<size_t>(c)];
        for (int j = 0; j < tr.M; ++j)
            s += tr.K[static_cast<size_t>(r)][static_cast<size_t>(j)] * op.i0[static_cast<size_t>(j)];
        worst = std::fmax(worst, std::fabs(s - v[static_cast<size_t>(r)]));
    }
    if (worst > 1e-6)
        std::printf("  warning: DC point inconsistent with the transient model by %.3g V\n", worst);
    return op;
}

struct RunOpts
{
    bool offset = false;
    int K = -1;          // -1: converged
    bool extrap = true;  // linearised extrapolation of the guess (Holters EUSIPCO-16)
    bool limit = false;
    bool damped = false;
};
struct Stats
{
    double meanIt = 0;
    int maxIt = 0;
    int nonfinite = 0;
};

// Runs output 0 (AC part: output minus its DC value) for the input u[0] = sig, supply on.
template <class T> static std::vector<double> runCircuit(const Built& b, const System& s, const Op& op, const std::vector<double>& sig,
                                                       const RunOpts& o, Stats* st = nullptr)
{
    const int N = s.N, M = s.M, U = s.U;
    auto cast = [](const Mat& m) {
        std::vector<T> f;
        for (const Vec& r : m)
            for (double x : r)
                f.push_back(T(x));
        return f;
    };
    const std::vector<T> Pu = cast(s.Pu), Ph = cast(s.Ph), Kf = cast(s.K), A = cast(s.A), B = cast(s.B), Cm = cast(s.Cm), Yu = cast(s.Yu),
                         Yh = cast(s.Yh), Yi = cast(s.Yi);
    Laws<T> law;
    law.devs = b.net.devs;
    law.M = M;
    law.offset = o.offset;
    if (o.offset)
    {
        law.e0.resize(static_cast<size_t>(M));
        law.x0.resize(static_cast<size_t>(M));
        for (const Device& d : b.net.devs)
            if (d.kind == Dev::BJT)
            {
                const double xbe = op.v0[static_cast<size_t>(d.port0)] / d.nvt;
                const double xbc = (op.v0[static_cast<size_t>(d.port0)] - op.v0[static_cast<size_t>(d.port0) + 1]) / d.nvt;
                law.x0[static_cast<size_t>(d.port0)] = T(xbe);
                law.x0[static_cast<size_t>(d.port0) + 1] = T(xbc);
                law.e0[static_cast<size_t>(d.port0)] = T(std::exp(xbe));
                law.e0[static_cast<size_t>(d.port0) + 1] = T(std::exp(xbc));
            }
    }
    std::vector<T> h(static_cast<size_t>(N)), v(static_cast<size_t>(M)), vprev(static_cast<size_t>(M)), pprev(static_cast<size_t>(M)), u(static_cast<size_t>(U)), p(static_cast<size_t>(M)), i(static_cast<size_t>(M)), J(static_cast<size_t>(M * M));
    for (int c = 0; c < N; ++c)
        h[static_cast<size_t>(c)] = o.offset ? T(0) : T(op.h0[static_cast<size_t>(c)]);
    for (int r = 0; r < M; ++r)
    {
        v[static_cast<size_t>(r)] = o.offset ? T(0) : T(op.v0[static_cast<size_t>(r)]);
        vprev[static_cast<size_t>(r)] = v[static_cast<size_t>(r)];
    }
    // p at the operating point (for the first extrapolation)
    for (int r = 0; r < M; ++r)
    {
        double s0 = 0;
        if (!o.offset)
        {
            for (int k = 0; k < U; ++k)
                s0 += s.Pu[static_cast<size_t>(r)][static_cast<size_t>(k)] * b.u0[static_cast<size_t>(k)];
            for (int c = 0; c < N; ++c)
                s0 += s.Ph[static_cast<size_t>(r)][static_cast<size_t>(c)] * op.h0[static_cast<size_t>(c)];
        }
        pprev[static_cast<size_t>(r)] = T(s0);
    }
    std::vector<double> y(sig.size());
    double itSum = 0;
    int itMax = 0, bad = 0;
    for (size_t n = 0; n < sig.size(); ++n)
    {
        for (int k = 0; k < U; ++k)
            u[static_cast<size_t>(k)] = o.offset ? T(k == 0 ? sig[n] : 0.0) : T(k == 0 ? sig[n] : b.u0[static_cast<size_t>(k)]);
        for (int r = 0; r < M; ++r)
        {
            T acc = 0;
            for (int k = 0; k < U; ++k)
                acc += Pu[static_cast<size_t>(r * U + k)] * u[static_cast<size_t>(k)];
            for (int c = 0; c < N; ++c)
                acc += Ph[static_cast<size_t>(r * N + c)] * h[static_cast<size_t>(c)];
            p[static_cast<size_t>(r)] = acc;
        }
        // guess
        if (o.extrap)
        {
            law.eval(vprev.data(), u.data(), i.data(), J.data());
            T a[64], dp[8];
            for (int r = 0; r < M; ++r)
            {
                dp[r] = p[static_cast<size_t>(r)] - pprev[static_cast<size_t>(r)];
                for (int c = 0; c < M; ++c)
                {
                    T kj = 0;
                    for (int q = 0; q < M; ++q)
                        kj += Kf[static_cast<size_t>(r * M + q)] * J[static_cast<size_t>(q * M + c)];
                    a[r * M + c] = (r == c ? T(1) : T(0)) - kj;
                }
            }
            solveSmall(M, a, dp);
            for (int r = 0; r < M; ++r)
                v[static_cast<size_t>(r)] = vprev[static_cast<size_t>(r)] + dp[r];
        }
        else
            v = vprev;
        const int it = newton<T>(s, Kf, law, p.data(), u.data(), v.data(), o.K, sizeof(T) == 8 ? 1e-10 : 1e-6, o.limit, o.damped);
        itSum += it;
        itMax = std::max(itMax, it);
        law.eval(v.data(), u.data(), i.data(), J.data());
        T out = 0;
        for (int k = 0; k < U; ++k)
            out += Yu[static_cast<size_t>(k)] * u[static_cast<size_t>(k)];
        for (int c = 0; c < N; ++c)
            out += Yh[static_cast<size_t>(c)] * h[static_cast<size_t>(c)];
        for (int j = 0; j < M; ++j)
            out += Yi[static_cast<size_t>(j)] * i[static_cast<size_t>(j)];
        std::vector<T> hn(static_cast<size_t>(N));
        for (int c = 0; c < N; ++c)
        {
            T acc = 0;
            for (int q = 0; q < N; ++q)
                acc += A[static_cast<size_t>(c * N + q)] * h[static_cast<size_t>(q)];
            for (int k = 0; k < U; ++k)
                acc += B[static_cast<size_t>(c * U + k)] * u[static_cast<size_t>(k)];
            for (int j = 0; j < M; ++j)
                acc += Cm[static_cast<size_t>(c * M + j)] * i[static_cast<size_t>(j)];
            hn[static_cast<size_t>(c)] = acc;
        }
        h = hn;
        vprev = v;
        pprev = p;
        if (!std::isfinite(double(out)))
            ++bad;
        y[n] = o.offset ? double(out) : double(out) - op.y0[0];
    }
    if (st)
    {
        st->meanIt = itSum / double(sig.size());
        st->maxIt = itMax;
        st->nonfinite = bad;
    }
    return y;
}

static const size_t NF = 16384;
static double errDb(const std::vector<double>& a, const std::vector<double>& ref)   // in band (< 20 kHz at 48 kHz), last NF
{
    const size_t s = a.size() - NF;
    std::vector<cd> e(NF), r(NF);
    for (size_t k = 0; k < NF; ++k)
    {
        e[k] = a[s + k] - ref[s + k];
        r[k] = ref[s + k];
    }
    fft(e);
    fft(r);
    const size_t top = static_cast<size_t>(20000.0 / 48000.0 * double(NF));
    double ee = 0, rr = 0;
    for (size_t b = 1; b <= top; ++b)
    {
        ee += std::norm(e[b]);
        rr += std::norm(r[b]);
    }
    return 10 * std::log10(ee / rr + 1e-40);
}
static std::vector<double> dec(const std::vector<double>& x, int M)
{
    if (M == 1)
        return x;
    std::vector<double> y(x.size() / static_cast<size_t>(M));
    // the circuits here are compared against themselves at the same rate: plain decimation keeps the comparison fair
    for (size_t k = 0; k < y.size(); ++k)
        y[k] = x[k * static_cast<size_t>(M)];
    return y;
}

int main(int argc, char** argv)
{
    const bool rangeOnly = argc > 1 && std::strcmp(argv[1], "range") == 0;
    const bool fuzzOnly = argc > 1 && std::strncmp(argv[1], "fuzz", 4) == 0;
    gJunctionCaps = argc > 1 && std::strcmp(argv[1], "fuzzcaps") == 0;
    auto sine = [&](double amp, int M) {
        std::vector<double> s(6 * NF * static_cast<size_t>(M));
        const double f = 341.0 * 48000.0 / double(NF);   // ~999 Hz, bin-aligned
        for (size_t k = 0; k < s.size(); ++k)
            s[k] = amp * std::sin(2 * M_PI * f * double(k) / (48000.0 * M));
        return s;
    };
    if (!fuzzOnly)
        for (int ge = 0; ge < 2; ++ge)
        {
            const Built b = rangemaster(ge == 1);
            std::printf("== Rangemaster topology, %s ==\n", ge ? "germanium-like (Is 5 uA)" : "silicon (Is 1e-14)");
            for (int M : { 1, 4, 8 })
            {
                const System s = build(b.net, 48000.0 * M);
                const Op op = operatingPoint(b, s);
                if (M == 1)
                    std::printf("  DC: Vbe %.4f V, Vce %.4f V, Ic %.3f mA, collector %.3f V\n", op.v0[0], op.v0[1], op.i0[1] * 1e3, op.y0[1]);
                for (double amp : { 0.01, 0.1, 1.0 })
                {
                    const auto sig = sine(amp, M);
                    RunOpts conv;
                    conv.extrap = false;
                    conv.damped = true;
                    conv.limit = true;
                    const auto ref = dec(runCircuit<double>(b, s, op, sig, conv), M);
                    RunOpts offd = conv;
                    offd.offset = true;
                    const auto refOff = dec(runCircuit<double>(b, s, op, sig, offd), M);
                    RunOpts fc = conv;   // float, converged (damped, capped, to 1e-6 V)
                    const auto fa = dec(runCircuit<float>(b, s, op, sig, fc), M);
                    RunOpts fco = fc;
                    fco.offset = true;
                    const auto fb = dec(runCircuit<float>(b, s, op, sig, fco), M);
                    double fk[2][4];
                    for (int off = 0; off < 2; ++off)
                        for (int K = 1; K <= 4; ++K)
                        {
                            RunOpts f;
                            f.K = K;
                            f.limit = true;
                            f.extrap = false;
                            f.offset = off == 1;
                            fk[off][K - 1] = errDb(dec(runCircuit<float>(b, s, op, sig, f), M), ref);
                        }
                    std::printf("  %dx, %5.0f mV: double offset vs absolute %7.1f | float converged: absolute %7.1f, offset %7.1f | float K1..K4 capped:"
                                " absolute %6.1f %6.1f %6.1f %6.1f, offset %6.1f %6.1f %6.1f %6.1f dB\n", M, amp * 1e3, errDb(refOff, ref), errDb(fa, ref),
                                errDb(fb, ref), fk[0][0], fk[0][1], fk[0][2], fk[0][3], fk[1][0], fk[1][1], fk[1][2], fk[1][3]);
                }
            }
        }
    // ---- Fuzz Face ----
    if (!rangeOnly)
    {
        const Built b = fuzzface(0.9);
        const System s = build(b.net, 48000.0);
        std::printf(gJunctionCaps ? "== Fuzz Face with junction capacitances ==\n" : "");
        std::printf("== Fuzz Face (NPN silicon), fuzz 0.9 ==\n  K (V/A), ports Q1:BE, Q1:CE, Q2:BE, Q2:CE\n");
        for (int r = 0; r < s.M; ++r)
        {
            std::printf("   ");
            for (int c = 0; c < s.M; ++c)
                std::printf(" %12.4g", s.K[static_cast<size_t>(r)][static_cast<size_t>(c)]);
            std::printf("\n");
        }
        for (int M : { 1, 4, 8 })
        {
            const System sm = build(b.net, 48000.0 * M);
            const Op op = operatingPoint(b, sm);
            if (M == 1)
                std::printf("  DC: Q1 Vbe %.3f Vce %.3f, Q2 Vbe %.3f Vce %.3f; Ic1 %.3f mA, Ic2 %.3f mA\n", op.v0[0], op.v0[1], op.v0[2], op.v0[3],
                            op.i0[1] * 1e3, op.i0[3] * 1e3);
            for (double amp : { 0.01, 0.1, 1.0 })
            {
                const auto sig = sine(amp, M);
                Stats sp, se;
                RunOpts cp;
                cp.extrap = false;
                cp.damped = true;
                cp.limit = true;
                const auto ref = dec(runCircuit<double>(b, sm, op, sig, cp, &sp), M);
                RunOpts ce;
                ce.damped = true;
                ce.extrap = true;
                ce.limit = true;
                runCircuit<double>(b, sm, op, sig, ce, &se);
                std::printf("  %dx, %5.0f mV: iterations to 1e-12 V: previous %.2f mean / %d max, extrapolated %.2f / %d\n", M, amp * 1e3, sp.meanIt,
                            sp.maxIt, se.meanIt, se.maxIt);
                double rms = 0;
                for (size_t k = ref.size() / 2; k < ref.size(); ++k)
                    rms += ref[k] * ref[k];
                std::printf("      output RMS %.3f V\n", std::sqrt(rms / double(ref.size() / 2)));
                const char* names[] = { "prev, plain   ", "prev, capped  ", "extrap, plain ", "extrap, capped" };
                for (int mode = 0; mode < 4; ++mode)
                {
                    std::printf("      float offset, %s, fixed K vs converged double:", names[mode]);
                    for (int K = 1; K <= 8; ++K)
                    {
                        RunOpts fk;
                        fk.K = K;
                        fk.offset = true;
                        fk.extrap = mode >= 2;
                        fk.limit = mode % 2 == 1;
                        Stats st;
                        const double e = errDb(dec(runCircuit<float>(b, sm, op, sig, fk, &st), M), ref);
                        std::printf(" K%d %6.1f%s", K, e, st.nonfinite ? "!" : "");
                    }
                    std::printf("\n");
                }
                std::printf("\n");
            }
        }
    }
    return 0;
}
