// Designs the 2x halfband stages (Kaiser FIR, and polyphase-allpass IIR by HIIR's elliptic design) for 48 -> 96 ->
// 192 -> 384 kHz, checks each by simulation (stopband, passband ripple to 20 kHz, group delay at 1 kHz), and writes
// them as FMinus modules (halfbands.fm) the way a circuit compiler would emit them: taps and coefficients as literals.
#include <cmath>
#include <complex>
#include <cstdio>
#include <string>
#include <vector>

static double bessel0(double x)
{
    double s = 1, t = 1;
    for (int k = 1; k < 60; ++k)
    {
        t *= (x / (2 * k)) * (x / (2 * k));
        s += t;
    }
    return s;
}
// halfband FIR, L = 4J - 1; returns the odd taps h[1], h[3], ..., h[2J-1] (h[0] = 0.5)
static std::vector<double> firHalfband(int L, double beta)
{
    const int c = L / 2;
    std::vector<double> h(L);
    double sum = 0;
    for (int n = 0; n < L; ++n)
    {
        const int k = n - c;
        const double sinc = k == 0 ? 0.5 : std::sin(M_PI * k / 2) / (M_PI * k);
        const double r = double(k) / c;
        h[n] = sinc * bessel0(beta * std::sqrt(std::fmax(0.0, 1 - r * r))) / bessel0(beta);
        sum += h[n];
    }
    std::vector<double> odd;
    for (int k = 1; k <= c; k += 2)
        odd.push_back(h[c + k] / sum);
    // renormalise so that the centre is exactly 0.5 and DC gain 1: centre/sum
    const double centre = h[c] / sum;
    for (double& v : odd)
        v *= 0.5 / centre;
    return odd;
}
static void firResponse(const std::vector<double>& odd, double fs, double pass, double stopFrom, double* stopDb, double* ripDb)
{
    double worstStop = 0, worstPass = 0;
    for (int i = 0; i <= 8000; ++i)
    {
        const double f = 0.5 * fs * i / 8000;
        double re = 0.5;
        for (size_t j = 0; j < odd.size(); ++j)
            re += 2 * odd[j] * std::cos(2 * M_PI * f / fs * double(2 * j + 1));
        const double mag = std::fabs(re);
        if (f >= stopFrom)
            worstStop = std::fmax(worstStop, mag);
        if (f <= pass)
            worstPass = std::fmax(worstPass, std::fabs(20 * std::log10(mag)));
    }
    *stopDb = 20 * std::log10(worstStop);
    *ripDb = worstPass;
}

// ---- HIIR (Laurent de Soras, WTFPL) elliptic polyphase halfband design ----
static void transitionParam(double& k, double& q, double transition)
{
    k = std::tan((1 - transition * 2) * M_PI / 4);
    k *= k;
    const double kksqrt = std::pow(1 - k * k, 0.25);
    const double e = 0.5 * (1 - kksqrt) / (1 + kksqrt);
    const double e2 = e * e, e4 = e2 * e2;
    q = e * (1 + e4 * (2 + e4 * (15 + 150 * e4)));
}
static double accNum(double q, int order, int c)
{
    double r = 0, t;
    int i = 0, j = 1;
    do
    {
        t = std::pow(q, i * (i + 1)) * std::sin((i * 2 + 1) * c * M_PI / order) * j;
        r += t;
        j = -j;
        ++i;
    } while (std::fabs(t) > 1e-100);
    return r;
}
static double accDen(double q, int order, int c)
{
    double r = 0, t;
    int i = 1, j = -1;
    do
    {
        t = std::pow(q, i * i) * std::cos(i * 2 * c * M_PI / order) * j;
        r += t;
        j = -j;
        ++i;
    } while (std::fabs(t) > 1e-100);
    return r;
}
static std::vector<double> iirHalfband(int nc, double transition)
{
    double k, q;
    transitionParam(k, q, transition);
    const int order = nc * 2 + 1;
    std::vector<double> a(nc);
    for (int idx = 0; idx < nc; ++idx)
    {
        const int c = idx + 1;
        const double num = accNum(q, order, c) * std::pow(q, 0.25);
        const double den = accDen(q, order, c) + 0.5;
        const double ww = num / den, wwsq = ww * ww;
        const double x = std::sqrt((1 - wwsq * k) * (1 - wwsq / k)) / (1 + wwsq);
        a[idx] = (1 - x) / (1 + x);
    }
    return a;
}
// response of the decimator 0.5 (A0(z^2) + z^-1 A1(z^2)) as HIIR's Downsampler2x builds it
static std::complex<double> iirH(const std::vector<double>& a, double w)
{
    const std::complex<double> z2 = std::polar(1.0, -2 * w);
    std::complex<double> A0 = 1, A1 = 1;
    for (size_t i = 0; i < a.size(); ++i)
    {
        const std::complex<double> s = (a[i] + z2) / (1.0 + a[i] * z2);
        if (i % 2 == 0)
            A0 *= s;
        else
            A1 *= s;
    }
    return 0.5 * (A0 + std::polar(1.0, -w) * A1);
}
static void iirResponse(const std::vector<double>& a, double fs, double pass, double stopFrom, double* stopDb, double* ripDb,
                        double* gd1k)
{
    double worstStop = 0, worstPass = 0;
    for (int i = 0; i <= 8000; ++i)
    {
        const double f = 0.5 * fs * i / 8000;
        const double mag = std::abs(iirH(a, 2 * M_PI * f / fs));
        if (f >= stopFrom)
            worstStop = std::fmax(worstStop, mag);
        if (f <= pass)
            worstPass = std::fmax(worstPass, std::fabs(20 * std::log10(mag)));
    }
    const double dw = 1e-4, w = 2 * M_PI * 1000 / fs;
    const double ph1 = std::arg(iirH(a, w - dw)), ph2 = std::arg(iirH(a, w + dw));
    double d = ph2 - ph1;
    while (d > M_PI)
        d -= 2 * M_PI;
    while (d < -M_PI)
        d += 2 * M_PI;
    *gd1k = -d / (2 * dw);   // in high-rate samples
    *stopDb = 20 * std::log10(worstStop);
    *ripDb = worstPass;
}

static std::string lit(double v)
{
    char b[64];
    std::snprintf(b, sizeof b, "%.9g", v);
    std::string s = b;
    if (s.find('.') == std::string::npos && s.find('e') == std::string::npos)
        s += ".0";
    return s + "f";
}

// FMinus FIR up/down modules. Up: one low-rate sample in, two high-rate out. Down: two in, one out.
static std::string emitFir(const std::string& name, const std::vector<double>& odd)
{
    const int J = int(odd.size()), W = 2 * J;
    std::string s;
    char b[256];
    // Up
    std::snprintf(b, sizeof b, "module %sUp\n{\n    float d[%d];\n    int pos;\n    void clear()\n    {\n        pos = 0;\n        for (int i = 0; i < %d; ++i)\n            d[i] = 0;\n    }\n",
                  name.c_str(), 2 * W, 2 * W);
    s += b;
    std::snprintf(b, sizeof b, "    void up(float x, float* restrict o)   // o[0], o[1]: the two high-rate samples; delay %d low-rate samples\n    {\n        pos = pos == 0 ? %d : pos - 1;\n        d[pos] = x;\n        d[pos + %d] = x;\n        const int p = pos;\n",
                  J, W - 1, W);
    s += b;
    // x[n-k] = d[p + k]; y_c[2n] = x[n-J]; y_c[2n+1] = 2 sum h_{2i+1} (x[n-J-i] + x[n-J+i+1])
    std::snprintf(b, sizeof b, "        o[0] = d[p + %d];\n        o[1] =", J);
    s += b;
    for (int i = 0; i < J; ++i)
    {
        std::snprintf(b, sizeof b, "%s%s * (d[p + %d] + d[p + %d])", i == 0 ? " " : "\n             + ", lit(2 * odd[size_t(i)]).c_str(), J + i,
                      J - i - 1);
        s += b;
    }
    s += ";\n    }\n};\n\n";
    // Down: E delayed by J, O window of 2J (O[n-2J .. n-1])
    std::snprintf(b, sizeof b, "module %sDown\n{\n    float e[%d];\n    float od[%d];\n    int pe, po;\n    void clear()\n    {\n        pe = 0;\n        po = 0;\n        for (int i = 0; i < %d; ++i)\n            e[i] = 0;\n        for (int i = 0; i < %d; ++i)\n            od[i] = 0;\n    }\n",
                  name.c_str(), 2 * (J + 1), 2 * W, 2 * (J + 1), 2 * W);
    s += b;
    std::snprintf(b, sizeof b, "    float down(float v0, float v1)   // v0 = v[2n], v1 = v[2n+1]\n    {\n        pe = pe == 0 ? %d : pe - 1;\n        e[pe] = v0;\n        e[pe + %d] = v0;\n",
                  J, J + 1);
    s += b;
    // O[n-1-k] = od[po + k] for k = 0..W-1 (od holds odd samples up to the previous pair)
    std::snprintf(b, sizeof b, "        const int q = po;\n        float y = 0.5f * e[pe + %d]", J);
    s += b;
    for (int i = 0; i < J; ++i)
    {
        // O[n-J-i-1] = od[q + J + i], O[n-J+i] = od[q + J - i - 1]
        std::snprintf(b, sizeof b, "\n                + %s * (od[q + %d] + od[q + %d])", lit(odd[size_t(i)]).c_str(), J + i, J - i - 1);
        s += b;
    }
    std::snprintf(b, sizeof b, ";\n        po = po == 0 ? %d : po - 1;\n        od[po] = v1;\n        od[po + %d] = v1;\n        return y;\n    }\n};\n\n",
                  W - 1, W);
    s += b;
    return s;
}

static std::string emitIir(const std::string& name, const std::vector<double>& a)
{
    const int n = int(a.size());
    std::string s;
    char b[256];
    for (int dir = 0; dir < 2; ++dir)
    {
        std::snprintf(b, sizeof b, "module %s%s\n{\n    float sx[%d];\n    float sy[%d];\n    void clear()\n    {\n        for (int i = 0; i < %d; ++i)\n        {\n            sx[i] = 0;\n            sy[i] = 0;\n        }\n    }\n",
                      name.c_str(), dir == 0 ? "Up" : "Down", n, n, n);
        s += b;
        if (dir == 0)
            s += "    void up(float x, float* restrict o)\n    {\n        float s0 = x;\n        float s1 = x;\n";
        else
            s += "    float down(float v0, float v1)\n    {\n        float s0 = v1;\n        float s1 = v0;\n";
        for (int i = 0; i < n; ++i)
        {
            const char* sv = i % 2 == 0 ? "s0" : "s1";
            std::snprintf(b, sizeof b, "        {\n            const float t = %s;\n            %s = (%s - sy[%d]) * %s + sx[%d];\n            sx[%d] = t;\n            sy[%d] = %s;\n        }\n",
                          sv, sv, sv, i, lit(a[size_t(i)]).c_str(), i, i, i, sv);
            s += b;
        }
        if (dir == 0)
            s += "        o[0] = s0;\n        o[1] = s1;\n    }\n};\n\n";
        else
            s += "        return 0.5f * (s0 + s1);\n    }\n};\n\n";
    }
    return s;
}

int main()
{
    struct Stage { const char* name; double fsHigh, pass, stopFrom; int L; double beta; };
    const Stage stages[] = {
        { "S1", 96000, 20000, 28000, 79, 9.5 },     // 48 -> 96
        { "S2", 192000, 20000, 76000, 27, 9.0 },    // 96 -> 192 (images of 0..20k at 96 +- 20)
        { "S3", 384000, 20000, 172000, 19, 9.0 },   // 192 -> 384
    };
    std::string coefs;
    std::string out = "// halfbands.fm: generated by gen.cpp. 2x halfband stages, FIR (Kaiser) and IIR (polyphase allpass, HIIR's design).\n\n";
    for (const Stage& st : stages)
    {
        const auto odd = firHalfband(st.L, st.beta);
        double sdb, rip;
        firResponse(odd, st.fsHigh, st.pass, st.stopFrom, &sdb, &rip);
        std::printf("FIR %s: L=%d, %zu mults/low-rate sample/direction, stopband %.1f dB, ripple %.5f dB, delay %d low-rate samples/direction\n",
                    st.name, st.L, odd.size() + 0, sdb, rip, int(odd.size()));
        out += emitFir(std::string("Fir") + st.name, odd);
        coefs += std::string("FIR ") + st.name;
        for (double v : odd) { char b[40]; std::snprintf(b, sizeof b, " %.9g", 2 * v); coefs += b; }   // 2h (interpolator gain)
        coefs += "\n";
        // IIR: the least number of coefficients reaching -95 dB (transition: HIIR's normalised width, found by search)
        // HIIR's transition is (fs_high/4 - pass) / fs_high * 2? search the value that puts the stopband edge at stopFrom.
        for (int nc = 2; nc <= 14; ++nc)
        {
            // HIIR transition parameter tbw = (fs/2_low - pass)/fs_high*2 = (fsHigh/4 - pass)*2/fsHigh
            const double tbw = (st.fsHigh / 4 - st.pass) * 2 / st.fsHigh;
            const auto a = iirHalfband(nc, tbw);
            double sdbI, ripI, gd;
            iirResponse(a, st.fsHigh, st.pass, st.stopFrom, &sdbI, &ripI, &gd);
            if (sdbI < -95 || nc == 14)
            {
                std::printf("IIR %s: %d coefs (tbw %.4f), %d mults/low-rate sample/direction, stopband %.1f dB, ripple %.5f dB, group delay at 1 kHz %.2f high-rate samples\n",
                            st.name, nc, tbw, nc, sdbI, ripI, gd);
                out += emitIir(std::string("Iir") + st.name, a);
                coefs += std::string("IIR ") + st.name;
                for (double v : a) { char b[40]; std::snprintf(b, sizeof b, " %.9g", v); coefs += b; }
                coefs += "\n";
                break;
            }
        }
    }
    FILE* g = std::fopen("hb-coefs.txt", "w");
    std::fputs(coefs.c_str(), g);
    std::fclose(g);
    FILE* f = std::fopen("halfbands.fm", "w");
    std::fputs(out.c_str(), f);
    std::fclose(f);
    return 0;
}
