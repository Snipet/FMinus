// dk.hpp: a small netlist -> nodal-DK compiler in double (the shape of the proposed fminus::circuit's stages 4-6), and
// a runtime for the resulting recursion in float or double. Elements: R, C (trapezoidal companions), voltage sources
// (inputs, supplies, ideal VCVS), ideal op-amps (nullors) and nonlinear ports. A port has a controlling node pair and an
// injection node pair (its current flows from i+ through the device to i-). Devices: diode pair, BJT (Ebers-Moll,
// two ports: B->E and C->E), OTA (tanh VCCS whose transconductance is a per-sample input).
#pragma once
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

namespace dk
{
using Vec = std::vector<double>;
using Mat = std::vector<Vec>;

static Mat zeros(int r, int c) { return Mat(static_cast<size_t>(r), Vec(static_cast<size_t>(c), 0.0)); }

struct LU
{
    Mat a;
    std::vector<int> piv;
    int n = 0;
    void factor(Mat m)
    {
        n = int(m.size());
        a = std::move(m);
        piv.resize(static_cast<size_t>(n));
        for (int i = 0; i < n; ++i)
            piv[static_cast<size_t>(i)] = i;
        for (int k = 0; k < n; ++k)
        {
            int p = k;
            for (int i = k + 1; i < n; ++i)
                if (std::fabs(a[static_cast<size_t>(i)][static_cast<size_t>(k)]) > std::fabs(a[static_cast<size_t>(p)][static_cast<size_t>(k)]))
                    p = i;
            if (std::fabs(a[static_cast<size_t>(p)][static_cast<size_t>(k)]) < 1e-300)
            {
                std::fprintf(stderr, "singular MNA at column %d\n", k);
                std::exit(1);
            }
            std::swap(a[static_cast<size_t>(p)], a[static_cast<size_t>(k)]);
            std::swap(piv[static_cast<size_t>(p)], piv[static_cast<size_t>(k)]);
            for (int i = k + 1; i < n; ++i)
            {
                const double f = a[static_cast<size_t>(i)][static_cast<size_t>(k)] / a[static_cast<size_t>(k)][static_cast<size_t>(k)];
                a[static_cast<size_t>(i)][static_cast<size_t>(k)] = f;
                for (int j = k + 1; j < n; ++j)
                    a[static_cast<size_t>(i)][static_cast<size_t>(j)] -= f * a[static_cast<size_t>(k)][static_cast<size_t>(j)];
            }
        }
    }
    Vec solve(const Vec& b) const
    {
        Vec x(static_cast<size_t>(n));
        for (int i = 0; i < n; ++i)
            x[static_cast<size_t>(i)] = b[static_cast<size_t>(piv[static_cast<size_t>(i)])];
        for (int i = 0; i < n; ++i)
            for (int j = 0; j < i; ++j)
                x[static_cast<size_t>(i)] -= a[static_cast<size_t>(i)][static_cast<size_t>(j)] * x[static_cast<size_t>(j)];
        for (int i = n - 1; i >= 0; --i)
        {
            for (int j = i + 1; j < n; ++j)
                x[static_cast<size_t>(i)] -= a[static_cast<size_t>(i)][static_cast<size_t>(j)] * x[static_cast<size_t>(j)];
            x[static_cast<size_t>(i)] /= a[static_cast<size_t>(i)][static_cast<size_t>(i)];
        }
        return x;
    }
};

struct R { int a, b; double r; };
struct C { int a, b; double c; };
struct V { int a, b; int input; };                 // V(a) - V(b) = u[input]
struct E { int a, b, cp, cn; double gain; };       // V(a) - V(b) = gain (V(cp) - V(cn))
struct OpAmp { int inp, inn, out; };
struct Port { int cp, cn, ip, in; };

enum class Dev { DiodePair, BJT, OTA };
struct Device
{
    Dev kind;
    int port0;          // BJT: port0 = B->E (controls vbe), port0 + 1 = C->E (controls vce)
    double is = 1e-14, nvt = 0.02585, bf = 100, br = 5;
    int gmInput = -1;   // OTA: which input carries Iabc
};

struct Netlist
{
    int nodes = 1;   // node 0 is ground
    std::vector<R> rs;
    std::vector<C> cs;
    std::vector<V> vs;
    std::vector<E> es;
    std::vector<OpAmp> ops;
    std::vector<Port> ports;
    std::vector<Device> devs;
    std::vector<int> outputs;   // node voltages
    int inputs = 0;
    int node() { return nodes++; }
};

// The DK system at one sample rate: p = Pu u + Ph h, v = p + K i, h' = A h + B u + Cm i, y = Yu u + Yh h + Yi i.
struct System
{
    int N = 0, M = 0, U = 0, Y = 0;
    Mat Pu, Ph, K, A, B, Cm, Yu, Yh, Yi;
    Vec gC;   // companion conductances
};

// Builds the MNA (caps as conductances gC, or open at DC with gmin) and the DK matrices from unit solves.
static System build(const Netlist& net, double fs, bool dc = false)
{
    const int nn = net.nodes - 1;
    const int nv = int(net.vs.size()), ne = int(net.es.size()), no = int(net.ops.size());
    const int D = nn + nv + ne + no;
    Mat m = zeros(D, D);
    auto at = [&](int node) { return node - 1; };
    auto stampG = [&](int a, int b, double g) {
        if (a)
            m[static_cast<size_t>(at(a))][static_cast<size_t>(at(a))] += g;
        if (b)
            m[static_cast<size_t>(at(b))][static_cast<size_t>(at(b))] += g;
        if (a && b)
        {
            m[static_cast<size_t>(at(a))][static_cast<size_t>(at(b))] -= g;
            m[static_cast<size_t>(at(b))][static_cast<size_t>(at(a))] -= g;
        }
    };
    for (const R& r : net.rs)
        stampG(r.a, r.b, 1 / r.r);
    System s;
    s.N = int(net.cs.size());
    s.M = int(net.ports.size());
    s.U = net.inputs;
    s.Y = int(net.outputs.size());
    s.gC.resize(static_cast<size_t>(s.N));
    for (int c = 0; c < s.N; ++c)
    {
        s.gC[static_cast<size_t>(c)] = dc ? 0 : 2 * net.cs[static_cast<size_t>(c)].c * fs;
        stampG(net.cs[static_cast<size_t>(c)].a, net.cs[static_cast<size_t>(c)].b, s.gC[static_cast<size_t>(c)]);
    }
    if (dc)
        for (int i = 1; i <= nn; ++i)
            stampG(i, 0, 1e-12);
    int row = nn;
    for (const V& v : net.vs)
    {
        if (v.a)
        {
            m[static_cast<size_t>(row)][static_cast<size_t>(at(v.a))] += 1;
            m[static_cast<size_t>(at(v.a))][static_cast<size_t>(row)] += 1;
        }
        if (v.b)
        {
            m[static_cast<size_t>(row)][static_cast<size_t>(at(v.b))] -= 1;
            m[static_cast<size_t>(at(v.b))][static_cast<size_t>(row)] -= 1;
        }
        ++row;
    }
    for (const E& e : net.es)
    {
        if (e.a)
        {
            m[static_cast<size_t>(row)][static_cast<size_t>(at(e.a))] += 1;
            m[static_cast<size_t>(at(e.a))][static_cast<size_t>(row)] += 1;
        }
        if (e.b)
        {
            m[static_cast<size_t>(row)][static_cast<size_t>(at(e.b))] -= 1;
            m[static_cast<size_t>(at(e.b))][static_cast<size_t>(row)] -= 1;
        }
        if (e.cp)
            m[static_cast<size_t>(row)][static_cast<size_t>(at(e.cp))] -= e.gain;
        if (e.cn)
            m[static_cast<size_t>(row)][static_cast<size_t>(at(e.cn))] += e.gain;
        ++row;
    }
    for (const OpAmp& o : net.ops)
    {
        if (o.inp)
            m[static_cast<size_t>(row)][static_cast<size_t>(at(o.inp))] += 1;
        if (o.inn)
            m[static_cast<size_t>(row)][static_cast<size_t>(at(o.inn))] -= 1;
        m[static_cast<size_t>(at(o.out))][static_cast<size_t>(row)] += 1;   // the output's current, free
        ++row;
    }
    LU lu;
    lu.factor(m);
    auto vdiff = [&](const Vec& z, int a, int b) { return (a ? z[static_cast<size_t>(at(a))] : 0) - (b ? z[static_cast<size_t>(at(b))] : 0); };
    auto respond = [&](const Vec& rhs, int col, Mat& P, Mat& Q, Mat& Yo) {
        const Vec z = lu.solve(rhs);
        for (int j = 0; j < s.M; ++j)
            P[static_cast<size_t>(j)][static_cast<size_t>(col)] = vdiff(z, net.ports[static_cast<size_t>(j)].cp, net.ports[static_cast<size_t>(j)].cn);
        for (int c = 0; c < s.N; ++c)
            Q[static_cast<size_t>(c)][static_cast<size_t>(col)] = vdiff(z, net.cs[static_cast<size_t>(c)].a, net.cs[static_cast<size_t>(c)].b);
        for (int y = 0; y < s.Y; ++y)
            Yo[static_cast<size_t>(y)][static_cast<size_t>(col)] = vdiff(z, net.outputs[static_cast<size_t>(y)], 0);
    };
    s.Pu = zeros(s.M, s.U);
    s.Ph = zeros(s.M, s.N);
    s.K = zeros(s.M, s.M);
    Mat Qu = zeros(s.N, s.U), Qh = zeros(s.N, s.N), Qi = zeros(s.N, s.M);
    s.Yu = zeros(s.Y, s.U);
    s.Yh = zeros(s.Y, s.N);
    s.Yi = zeros(s.Y, s.M);
    for (int k = 0; k < s.U; ++k)
    {
        Vec rhs(static_cast<size_t>(D), 0.0);
        int r2 = nn;
        for (const V& v : net.vs)
        {
            if (v.input == k)
                rhs[static_cast<size_t>(r2)] = 1;
            ++r2;
        }
        respond(rhs, k, s.Pu, Qu, s.Yu);
    }
    for (int c = 0; c < s.N; ++c)
    {
        Vec rhs(static_cast<size_t>(D), 0.0);
        if (net.cs[static_cast<size_t>(c)].a)
            rhs[static_cast<size_t>(at(net.cs[static_cast<size_t>(c)].a))] += 1;
        if (net.cs[static_cast<size_t>(c)].b)
            rhs[static_cast<size_t>(at(net.cs[static_cast<size_t>(c)].b))] -= 1;
        respond(rhs, c, s.Ph, Qh, s.Yh);
    }
    for (int j = 0; j < s.M; ++j)
    {
        Vec rhs(static_cast<size_t>(D), 0.0);
        if (net.ports[static_cast<size_t>(j)].ip)
            rhs[static_cast<size_t>(at(net.ports[static_cast<size_t>(j)].ip))] -= 1;
        if (net.ports[static_cast<size_t>(j)].in)
            rhs[static_cast<size_t>(at(net.ports[static_cast<size_t>(j)].in))] += 1;
        respond(rhs, j, s.K, Qi, s.Yi);
    }
    s.A = zeros(s.N, s.N);
    s.B = zeros(s.N, s.U);
    s.Cm = zeros(s.N, s.M);
    for (int c = 0; c < s.N; ++c)
    {
        const double g2 = 2 * s.gC[static_cast<size_t>(c)];
        for (int j = 0; j < s.N; ++j)
            s.A[static_cast<size_t>(c)][static_cast<size_t>(j)] = g2 * Qh[static_cast<size_t>(c)][static_cast<size_t>(j)] - (c == j ? 1 : 0);
        for (int k = 0; k < s.U; ++k)
            s.B[static_cast<size_t>(c)][static_cast<size_t>(k)] = g2 * Qu[static_cast<size_t>(c)][static_cast<size_t>(k)];
        for (int j = 0; j < s.M; ++j)
            s.Cm[static_cast<size_t>(c)][static_cast<size_t>(j)] = g2 * Qi[static_cast<size_t>(c)][static_cast<size_t>(j)];
    }
    // at DC the caps are open: report their voltages through Qu, Qh (h = 0), Qi in A, B, Cm's place
    if (dc)
    {
        s.A = Qh;
        s.B = Qu;
        s.Cm = Qi;
    }
    return s;
}

// Device laws: currents i(v) and the Jacobian di/dv over all ports. T is float or double. `gm` holds per-sample inputs.
template <class T> struct Laws
{
    std::vector<Device> devs;
    int M = 0;
    // offset form: i(v0 + dv) - i(v0), with e^{v0/nvt} precomputed per junction (expm1 by tanh, as FMinus could)
    bool offset = false;
    std::vector<T> v0, e0, x0;   // per junction: e^{v0/nvt} and v0/nvt
    static T expm1T(T x)
    {
        if (std::fabs(x) < T(10))
        {
            const T t = std::tanh(x * T(0.5));
            return T(2) * t / (T(1) - t);
        }
        return std::exp(x) - T(1);
    }
    void eval(const T* v, const T* u, T* i, T* J) const   // J: M x M row-major
    {
        for (int k = 0; k < M * M; ++k)
            J[k] = 0;
        for (const Device& d : devs)
        {
            const int p = d.port0;
            const T is = T(d.is), nvt = T(d.nvt);
            if (d.kind == Dev::DiodePair)
            {
                const T e = std::exp(v[p] / nvt), ei = T(1) / e;
                i[p] = is * (e - ei);
                J[p * M + p] = is / nvt * (e + ei);
            }
            else if (d.kind == Dev::OTA)
            {
                const T g = T(u[d.gmInput]);
                const T x = v[p] / (T(2) * nvt);
                const T t = std::tanh(x);
                i[p] = g * t;
                J[p * M + p] = g * (T(1) - t * t) / (T(2) * nvt);
            }
            else
            {
                // ports: p = B->E (v = vbe), p+1 = C->E (v = vce); vbc = vbe - vce
                const T bf = T(d.bf), br = T(d.br);
                T ef, er, fF, fR;   // fF = e^{vbe/nvt} - 1, fR = e^{vbc/nvt} - 1, ef/er their derivatives' exps
                if (offset)
                {
                    // a junction that conducts at the operating point: e0 (e^{x/nvt} - 1), exact for small x; one that
                    // is reverse biased there (e0 below 1e-12): e^{(v0 + x)/nvt} - e0 from the total voltage
                    const T dvbe = v[p], dvbc = v[p] - v[p + 1];
                    const T e0f = e0[static_cast<size_t>(p)], e0r = e0[static_cast<size_t>(p) + 1];
                    if (e0f > T(1e-12))
                    {
                        const T a = expm1T(dvbe / nvt);
                        ef = e0f * (a + T(1));
                        fF = e0f * a;
                    }
                    else
                    {
                        ef = std::exp(x0[static_cast<size_t>(p)] + dvbe / nvt);
                        fF = ef - e0f;
                    }
                    if (e0r > T(1e-12))
                    {
                        const T b = expm1T(dvbc / nvt);
                        er = e0r * (b + T(1));
                        fR = e0r * b;
                    }
                    else
                    {
                        er = std::exp(x0[static_cast<size_t>(p) + 1] + dvbc / nvt);
                        fR = er - e0r;
                    }
                }
                else
                {
                    const T vbe = v[p], vbc = v[p] - v[p + 1];
                    ef = std::exp(vbe / nvt);
                    er = std::exp(vbc / nvt);
                    fF = ef - T(1);
                    fR = er - T(1);
                }
                const T ib = is / bf * fF + is / br * fR;
                const T ic = is * (fF - fR) - is / br * fR;
                i[p] = ib;
                i[p + 1] = ic;
                const T dF = is / nvt * ef, dR = is / nvt * er;   // d/dvbe of e^{vbe}, d/dvbc of e^{vbc}
                // dvbc/dvbe = 1, dvbc/dvce = -1
                J[p * M + p] = dF / bf + dR / br;
                J[p * M + p + 1] = -dR / br;
                J[(p + 1) * M + p] = dF - dR - dR / br;
                J[(p + 1) * M + p + 1] = dR + dR / br;
            }
        }
    }
};

// Small dense solve for the Newton step (M <= 8), Gaussian elimination with partial pivoting.
template <class T> static void solveSmall(int M, T* a, T* b)
{
    for (int k = 0; k < M; ++k)
    {
        int p = k;
        for (int i = k + 1; i < M; ++i)
            if (std::fabs(a[i * M + k]) > std::fabs(a[p * M + k]))
                p = i;
        if (p != k)
        {
            for (int j = 0; j < M; ++j)
                std::swap(a[k * M + j], a[p * M + j]);
            std::swap(b[k], b[p]);
        }
        const T inv = T(1) / a[k * M + k];
        for (int i = k + 1; i < M; ++i)
        {
            const T f = a[i * M + k] * inv;
            for (int j = k; j < M; ++j)
                a[i * M + j] -= f * a[k * M + j];
            b[i] -= f * b[k];
        }
    }
    for (int i = M - 1; i >= 0; --i)
    {
        T s = b[i];
        for (int j = i + 1; j < M; ++j)
            s -= a[i * M + j] * b[j];
        b[i] = s / a[i * M + i];
    }
}

}   // namespace dk
