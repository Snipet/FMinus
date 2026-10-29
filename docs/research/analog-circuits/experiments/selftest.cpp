// The compiler's self-test (pipeline stage 8), timed on example 2's block 2: a converged double reference at 64x on a
// stress matrix (drive at 0, 0.5, 1 x 5 levels x 3 frequencies, plus a log sweep and a noise burst at the target level,
// per drive), against float emulations of the candidate configurations. Reports the wall time of the reference, of
// one candidate, of the exhaustive search and of the ordered search (cheapest first, stop at the first that passes).
#define main adaa_main
#include "adaa.cpp"
#undef main

#include <algorithm>
#include <chrono>
#include <random>

struct Cand
{
    int M;
    bool adaa;
    Guess guess;
    int K;
    double cost;   // cycles per host sample, from the FMinus measurements (bench_ts), for ordering
};

int main()
{
    N = 8192;
    using clk = std::chrono::steady_clock;
    const double drives[] = { 0, 0.5, 1 };
    const double levels[] = { 0.03, 0.1, 0.2, 0.45, 1.0 };
    const int bins[] = { 17, 171, 513 };   // ~100 Hz, ~1 kHz, ~3 kHz on N = 8192 at 48 kHz; odd: no alias lands on a harmonic
    auto sine = [&](double amp, int k, int M, size_t n) {
        std::vector<double> s(n);
        const double f = double(k) * fsBase / double(N);
        for (size_t i = 0; i < n; ++i)
            s[i] = amp * std::sin(2 * M_PI * f * double(i) / (fsBase * M));
        return s;
    };
    auto sweep = [&](double amp, int M, size_t n) {
        std::vector<double> s(n);
        const double T = double(n) / (fsBase * M), f1 = 20, f2 = 20000, L = T / std::log(f2 / f1);
        for (size_t i = 0; i < n; ++i)
        {
            const double t = double(i) / (fsBase * M);
            s[i] = amp * std::sin(2 * M_PI * f1 * L * (std::exp(t / L) - 1));
        }
        return s;
    };
    // a noise burst, band-limited to 20 kHz at every rate: the same 48 kHz white noise, ideally interpolated (sinc)
    std::mt19937 rng(7);
    std::normal_distribution<double> nd(0, 1);
    std::vector<double> noise48(4 * N);
    for (double& v : noise48)
        v = nd(rng) * 0.3;
    auto noise = [&](double amp, int M, size_t n) {
        std::vector<double> s(n, 0.0);
        for (size_t i = 0; i < n; ++i)
        {
            const double t = double(i) / M;
            const long c = long(t);
            double acc = 0;
            for (long j = c - 16; j <= c + 16; ++j)
                if (j >= 0 && j < long(noise48.size()))
                {
                    const double x = t - double(j);
                    const double sinc = std::fabs(x) < 1e-12 ? 1 : std::sin(M_PI * x * 0.83) / (M_PI * x * 0.83) * 0.83;
                    acc += noise48[size_t(j)] * sinc * (0.5 + 0.5 * std::cos(M_PI * x / 17));
                }
            s[i] = amp * acc;
        }
        return s;
    };
    struct Sig { int kind; double amp; int k; };   // kind 0 sine, 1 sweep, 2 noise
    std::vector<Sig> sigs;
    for (double a : levels)
        for (int k : bins)
            sigs.push_back({ 0, a, k });
    sigs.push_back({ 1, 0.45, 0 });
    sigs.push_back({ 2, 0.45, 0 });
    auto make = [&](const Sig& s, int M) {
        const size_t n = 4 * N * size_t(M);
        return s.kind == 0 ? sine(s.amp, s.k, M, n) : s.kind == 1 ? sweep(s.amp, M, n) : noise(s.amp, M, n);
    };
    // candidates, with FMinus cycles per host sample (FIR halfbands) from timing1.out
    std::vector<Cand> cands;
    const double cyc[2][4][3] = { { { 168, 235, 0 }, { 337, 472, 0 }, { 669, 937, 0 }, { 1333, 1861, 0 } },
                                  { { 227, 0, 0 }, { 455, 0, 0 }, { 903, 0, 0 }, { 1800, 0, 0 } } };
    for (int mi = 0; mi < 4; ++mi)
    {
        const int M = 1 << mi;
        for (int K = 1; K <= 2; ++K)
            cands.push_back({ M, false, Guess::omega, K, cyc[0][mi][K - 1] });
        cands.push_back({ M, true, Guess::omega, 1, cyc[1][mi][0] });
        for (int K = 2; K <= 6; K += 2)
            cands.push_back({ M, false, Guess::prev, K, 10.0 + 72.0 * M * K });   // ordering only
    }
    std::sort(cands.begin(), cands.end(), [](const Cand& a, const Cand& b) { return a.cost < b.cost; });

    // 1. references
    const auto t0 = clk::now();
    std::vector<std::vector<std::vector<double>>> refs(3);
    for (int d = 0; d < 3; ++d)
        for (const Sig& s : sigs)
        {
            const auto in = make(s, 64);
            refs[size_t(d)].push_back(decimate(run<double>(makeTS(fsBase * 64, drives[d]), in, Opts{}), 64));
        }
    const auto t1 = clk::now();
    // 2. every candidate on every signal (exhaustive), recording pass/fail; 3. the ordered search's time to the first pass
    double exhaustive = 0, ordered = -1;
    std::string chosen;
    for (const Cand& c : cands)
    {
        const auto c0 = clk::now();
        bool pass = true;
        double worstAlias = -999, worstSolver = -999, worstNull = -999;
        for (int d = 0; d < 3; ++d)
            for (size_t si = 0; si < sigs.size(); ++si)
            {
                const Sig& s = sigs[si];
                const auto in = make(s, c.M);
                Opts o;
                o.adaa = c.adaa;
                o.K = c.K;
                o.guess = c.guess;
                const double design = c.adaa ? fsBase * c.M * 2.0 / 3.0 : fsBase * c.M;
                const auto y = decimate(run<float>(makeTS(design, drives[d]), in, o), c.M);
                Opts od = o;
                od.K = -1;
                const auto yd = decimate(run<double>(makeTS(design, drives[d]), in, od), c.M);
                const double solver = bandErrDb(y, yd, 0);
                worstSolver = std::fmax(worstSolver, solver);
                if (s.kind == 0)
                    worstAlias = std::fmax(worstAlias, inharmonicDb(yd, s.k));
                else
                    worstNull = std::fmax(worstNull, bandErrDb(yd, refs[size_t(d)][si], c.adaa ? 0.5 / c.M : 0));
                if (solver > -60 || (s.kind == 0 && s.amp <= 0.45 && inharmonicDb(yd, s.k) > -60))
                    pass = false;
            }
        const double secs = std::chrono::duration<double>(clk::now() - c0).count();
        exhaustive += secs;
        std::printf("  %dx %s %s K%d: %s  worst alias (<= 0.45 V) %6.1f, solver %6.1f, sweep/noise null %6.1f dB  (%.2f s)\n", c.M,
                    c.adaa ? "ADAA" : "    ", c.guess == Guess::omega ? "omega" : "prev ", c.K, pass ? "PASS" : "fail", worstAlias, worstSolver,
                    worstNull, secs);
        std::fflush(stdout);
        if (pass && ordered < 0)
        {
            ordered = std::chrono::duration<double>(clk::now() - t1).count();
            char b[128];
            std::snprintf(b, sizeof b, "%dx %s %s K%d", c.M, c.adaa ? "ADAA" : "plain", c.guess == Guess::omega ? "omega" : "prev", c.K);
            chosen = b;
        }
    }
    std::printf("reference (3 drives x %zu signals, 64x, converged double): %.2f s\n", sigs.size(),
                std::chrono::duration<double>(t1 - t0).count());
    std::printf("exhaustive search over %zu candidates: %.2f s; ordered search to the first pass (%s): %.2f s\n", cands.size(), exhaustive,
                chosen.c_str(), ordered);
    return 0;
}
