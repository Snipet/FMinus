// examples/circuits/tables.cpp: example-circuits --tables. The report's accuracy tables again, their solver columns
// from emitted code run in the JIT, each printed in its source's layout with the source's numbers beside it:
//   - report §5.2's worst case (experiments/adaa-worst.out), on diodetone.cir and screamer-clip.cir at N Vt 45.3 mV;
//   - fminus.md §6.1's Tables 1-3 (clip3.cpp, clip2.cpp, clip.cpp), on clipper.cir and screamer-clip.cir at drive 1,
//     at their 45.0 mV.
// The converged columns come from the double reference (src/circuit/Reference.h), as the report's did. Minutes.
#include "harness.h"

#include <cmath>
#include <cstdio>
#include <format>
#include <map>
#include <stdexcept>
#include <string>
#include <vector>

namespace circuits
{
    namespace
    {
        struct Case
        {
            const char* name;
            double amp;
            int bin;
        };

        // An emitted variant of a netlist, compiled once.
        class Variants
        {
        public:
            explicit Variants(std::string text)
                : text_(std::move(text))
            {
            }

            const Circuit& get(int m, bool adaa, fc::Solver solver)
            {
                const std::string key =
                    std::format("{} {} {} {}", m, adaa, solver.guess == fc::Solver::Guess::Omega, solver.steps);
                auto it = built_.find(key);
                if (it == built_.end())
                {
                    fc::Options o;
                    o.oversample = m;
                    o.adaa = adaa;
                    o.solver = solver;
                    std::string errors;
                    std::unique_ptr<Circuit> c = build(text_, "variant.cir", o, errors);
                    if (!c)
                        throw std::runtime_error("a variant does not build:\n" + errors);
                    it = built_.emplace(key, std::move(c)).first;
                }
                return *it->second;
            }

        private:
            std::string text_;
            std::map<std::string, std::unique_ptr<Circuit>> built_;
        };

        constexpr double kSkip = 1e9;

        std::string cell(double v)
        {
            return v >= kSkip ? std::string("      -") : std::format(" {:6.1f}", v);
        }

        // ---- report §5.2: the worst case over pots {0, 0.5, 1} x the three cases ----
        void adaaWorst(const char* title, const std::string& file, const std::string& pot, const Case (&cases)[3],
                       const char* const (&report)[4])
        {
            constexpr std::size_t n = 32768;
            const std::string text = withN(netlist(file), "1.75140653");
            Variants variants(text);
            const fc::Plan& plan = variants.get(1, false, {fc::Solver::Guess::Omega, 2}).plan;
            std::printf("%s\n", title);
            int row = 0;
            for (int m : {1, 2, 4, 8})
            {
                double aliasP = -999, aliasA = -999, h3P = 0, h3A = 0, h1A = 0;
                double wP[3] = {-999, -999, -999}, wA[3] = {-999, -999, -999};
                double pP[6] = {-999, -999, -999, -999, -999, -999};
                for (double p : {0.0, 0.5, 1.0})
                {
                    const std::vector<double> pots = potsWith(plan, pot, p);
                    const fc::Holters plainBlock = fc::holters(plan, 0, 48000.0 * m, pots);
                    const fc::Holters adaaBlock = fc::holters(plan, 0, 48000.0 * m * 2 / 3, pots);
                    const fc::Holters block64 = fc::holters(plan, 0, 48000.0 * 64, pots);
                    for (const Case& c : cases)
                    {
                        const std::vector<double> u = fc::sine(c.amp, c.bin, n, m, 4 * n * static_cast<std::size_t>(m));
                        auto window = [&](const std::vector<double>& y)
                        {
                            return fc::decimateWindow(y, m, 2 * n, n);
                        };
                        fc::RefOptions adaa;
                        adaa.adaa = true;
                        const std::vector<double> plain = window(fc::simulate(plainBlock, u, {}));
                        const std::vector<double> anti = window(fc::simulate(adaaBlock, u, adaa));
                        const std::vector<double> ref64 =
                            fc::simulate(block64, fc::sine(c.amp, c.bin, n, 64, 3 * n * 64), {});
                        const std::span<const double> w64 = std::span<const double>(ref64).subspan(2 * n * 64, n * 64);
                        const double r1 = fc::binMagnitude(w64, c.bin);
                        const double r3 = fc::binMagnitude(w64, 3 * c.bin);
                        aliasP = std::fmax(aliasP, fc::inharmonicDb(plain, c.bin));
                        aliasA = std::fmax(aliasA, fc::inharmonicDb(anti, c.bin));
                        h3P = std::fmin(h3P, dB(fc::binMagnitude(plain, 3 * c.bin) / r3));
                        h3A = std::fmin(h3A, dB(fc::binMagnitude(anti, 3 * c.bin) / r3));
                        h1A = std::fmin(h1A, dB(fc::binMagnitude(anti, c.bin) / r1));
                        for (int k = 0; k < 3; ++k)
                        {
                            const Circuit& vp = variants.get(m, false, {fc::Solver::Guess::Omega, k});
                            wP[k] = std::fmax(wP[k], fc::bandErrorDb(window(runSteps(vp, u, pots)), plain));
                            const Circuit& va = variants.get(m, true, {fc::Solver::Guess::Omega, k});
                            wA[k] = std::fmax(wA[k], fc::bandErrorDb(window(runSteps(va, u, pots)), anti));
                        }
                        for (int k = 1; k <= 6; ++k)
                        {
                            const Circuit& vn = variants.get(m, false, {fc::Solver::Guess::Previous, k});
                            pP[k - 1] = std::fmax(pP[k - 1], fc::bandErrorDb(window(runSteps(vn, u, pots)), plain));
                        }
                    }
                }
                std::printf(
                    " %dx | alias plain %6.1f ADAA %6.1f | H3 plain %+5.2f ADAA %+5.2f, H1 ADAA %+5.2f dB | solver "
                    "plain w K0/K1/K2 %6.1f %6.1f %6.1f | prev K1..K6 %6.1f %6.1f %6.1f %6.1f %6.1f %6.1f | ADAA w "
                    "K0/K1/K2 %6.1f %6.1f %6.1f\n",
                    m, aliasP, aliasA, h3P, h3A, h1A, wP[0], wP[1], wP[2], pP[0], pP[1], pP[2], pP[3], pP[4], pP[5],
                    wA[0], wA[1], wA[2]);
                std::printf("    report: %s\n", report[row++]);
                std::fflush(stdout);
            }
        }

        // ---- fminus.md Table 1: solvers at 1x and 4x, in float, against the converged solve at the same rate ----
        void table1()
        {
            constexpr std::size_t n = 32768;
            std::printf("\nfminus.md Table 1: the solver, float, in band against the converged solve of the same rate "
                        "(clip3.cpp: N = 32768, the last N samples); emitted code\n");
            std::printf("  %-22s %4s | prev K1 / K2   | extr K1 / K2   | omega K0 / K1 / K2    | lim K1 / K2\n",
                        "circuit, input", "rate");
            struct Row
            {
                bool screamer;
                Case c;
                int m;
                const char* report;
            };
            const Row rows[] = {
                {false,
                 {"clipper 0.3 V, 1 kHz", 0.3, 683},
                 1,
                 "-73 / -137 | -137 / -137 | -42 / -114 / -137 | -73 / -137"},
                {false, {"clipper 2 V, 1 kHz", 2.0, 683}, 1, "-31 / -39 | -38 / -51 | -34 / -55 / -95 | -26 / -36"},
                {false, {"clipper 4.5 V, 3 kHz", 4.5, 2049}, 1, "-2 / -4 | -3 / -5 | -39 / -62 / -104 | -12 / -19"},
                {false, {"clipper 4.5 V, 3 kHz", 4.5, 2049}, 4, "-33 / -42 | -39 / -49 | -35 / -55 / -95 | -30 / -41"},
                {true, {"TS 30 mV, 1 kHz", 0.03, 683}, 1, "-24 / -30 | -30 / -38 | -31 / -52 / -92 | -22 / -33"},
                {true, {"TS 200 mV, 1 kHz", 0.2, 683}, 1, "-4 / -7 | -5 / -9 | -41 / -63 / -104 | -23 / -32"},
                {true, {"TS 450 mV, 3 kHz", 0.45, 2049}, 1, "+27 / +26 | +27 / +28 | -50 / -73 / -114 | -20 / -33"},
                {true, {"TS 450 mV, 3 kHz", 0.45, 2049}, 4, "-14 / -17 | -14 / -18 | -46 / -71 / -114 | -30 / -43"},
            };
            Variants clipper(withN(netlist("clipper.cir"), "1.73980781"));
            Variants screamer(withN(netlist("screamer-clip.cir"), "1.73980781"));
            for (const Row& r : rows)
            {
                Variants& v = r.screamer ? screamer : clipper;
                const Circuit& any = v.get(r.m, false, {fc::Solver::Guess::Omega, 2});
                const std::vector<double> pots = r.screamer ? potsWith(any.plan, "drive", 1) : std::vector<double>{};
                const std::vector<double> u = fc::sine(r.c.amp, r.c.bin, n, r.m, 4 * n * static_cast<std::size_t>(r.m));
                auto window = [&](const std::vector<double>& y)
                {
                    return fc::decimateWindow(y, r.m, 3 * n, n);
                };
                const std::vector<double> conv =
                    window(fc::simulate(fc::holters(any.plan, 0, 48000.0 * r.m, pots), u, {}));
                auto err = [&](fc::Solver::Guess g, int k)
                {
                    return fc::bandErrorDb(window(runSteps(v.get(r.m, false, {g, k}), u, pots)), conv);
                };
                std::printf("  %-22s %3dx | %s%s | %s%s | %s%s%s | %s%s\n", r.c.name, r.m,
                            cell(err(fc::Solver::Guess::Previous, 1)).c_str(),
                            cell(err(fc::Solver::Guess::Previous, 2)).c_str(), cell(kSkip).c_str(), cell(kSkip).c_str(),
                            cell(err(fc::Solver::Guess::Omega, 0)).c_str(),
                            cell(err(fc::Solver::Guess::Omega, 1)).c_str(),
                            cell(err(fc::Solver::Guess::Omega, 2)).c_str(), cell(kSkip).c_str(), cell(kSkip).c_str());
                std::printf("  %-22s       fminus.md: %s\n", "", r.report);
                std::fflush(stdout);
            }
            std::printf("  (extr and lim are guesses Phase 0 does not emit: -)\n");
        }

        // ---- fminus.md Table 2: the trapezoidal rule against 64x, converged ----
        void table2()
        {
            constexpr std::size_t n = 32768;
            std::printf(
                "\nfminus.md Table 2: the trapezoidal rule, converged, in band against a 64x reference (clip2.cpp: "
                "N = 32768, the last N samples)\n");
            struct Row
            {
                bool screamer;
                Case c;
                const char* report;
            };
            const Row rows[] = {
                {false, {"clipper 2 V, 1 kHz", 2.0, 683}, "-50 / -62 / -74 / -86"},
                {false, {"clipper 4.5 V, 3 kHz", 4.5, 2049}, "-27 / -45 / -58 / -70"},
                {true, {"TS 200 mV, 1 kHz", 0.2, 683}, "-31 / -54 / -67 / -79"},
                {true, {"TS 450 mV, 3 kHz", 0.45, 2049}, "-22 / -30 / -53 / -68"},
            };
            for (const Row& r : rows)
            {
                const std::string text = withN(netlist(r.screamer ? "screamer-clip.cir" : "clipper.cir"), "1.73980781");
                std::string errors;
                const std::unique_ptr<Circuit> c = build(text, "t.cir", {}, errors);
                if (!c)
                    throw std::runtime_error(errors);
                const std::vector<double> pots = r.screamer ? potsWith(c->plan, "drive", 1) : std::vector<double>{};
                const std::vector<double> ref =
                    fc::decimateWindow(fc::simulate(fc::holters(c->plan, 0, 48000.0 * 64, pots),
                                                    fc::sine(r.c.amp, r.c.bin, n, 64, 4 * n * 64), {}),
                                       64, 3 * n, n);
                std::string line;
                for (int m : {1, 2, 4, 8})
                {
                    const std::vector<double> y = fc::decimateWindow(
                        fc::simulate(fc::holters(c->plan, 0, 48000.0 * m, pots),
                                     fc::sine(r.c.amp, r.c.bin, n, m, 4 * n * static_cast<std::size_t>(m)), {}),
                        m, 3 * n, n);
                    line += std::format(" | {}x {:6.1f}", m, fc::bandErrorDb(y, ref));
                }
                std::printf("  %-22s TR%s   (fminus.md: %s)\n", r.c.name, line.c_str(), r.report);
                std::fflush(stdout);
            }
        }

        // ---- fminus.md Table 3: aliasing against the rate ----
        void table3()
        {
            constexpr std::size_t n = 65536;
            std::printf(
                "\nfminus.md Table 3: inharmonic energy below 20 kHz (clip.cpp: N = 65536, bins 1365 and 4097); "
                "converged, and emitted omega + 2\n");
            struct Row
            {
                bool screamer;
                Case c;
                const char* report;
            };
            const Row rows[] = {
                {false, {"clipper 2 V, 1 kHz", 2.0, 1365}, "-62 / -105 / -178 / -188"},
                {false, {"clipper 4.5 V, 3 kHz", 4.5, 4097}, "-29 / -48 / -75 / -108"},
                {true, {"TS 30 mV, 1 kHz", 0.03, 1365}, "-54 / -93 / -157 / -181"},
                {true, {"TS 200 mV, 1 kHz", 0.2, 1365}, "-31 / -58 / -81 / -118"},
                {true, {"TS 450 mV, 3 kHz", 0.45, 4097}, "-23 / -31 / -53 / -73"},
            };
            Variants clipper(withN(netlist("clipper.cir"), "1.73980781"));
            Variants screamer(withN(netlist("screamer-clip.cir"), "1.73980781"));
            for (const Row& r : rows)
            {
                Variants& v = r.screamer ? screamer : clipper;
                std::string converged;
                std::string emitted;
                for (int m : {1, 2, 4, 8})
                {
                    const Circuit& c = v.get(m, false, {fc::Solver::Guess::Omega, 2});
                    const std::vector<double> pots = r.screamer ? potsWith(c.plan, "drive", 1) : std::vector<double>{};
                    const std::vector<double> u = fc::sine(r.c.amp, r.c.bin, n, m, 3 * n * static_cast<std::size_t>(m));
                    const std::vector<double> conv =
                        fc::decimateWindow(fc::simulate(fc::holters(c.plan, 0, 48000.0 * m, pots), u, {}), m, n, n);
                    const std::vector<double> mine = fc::decimateWindow(runSteps(c, u, pots), m, n, n);
                    converged += std::format(" | {}x {:6.1f}", m, fc::inharmonicDb(conv, r.c.bin));
                    emitted += std::format(" | {}x {:6.1f}", m, fc::inharmonicDb(mine, r.c.bin));
                }
                std::printf("  %-22s converged%s   (fminus.md: %s)\n", r.c.name, converged.c_str(), r.report);
                std::printf("  %-22s omega + 2%s\n", "", emitted.c_str());
                std::fflush(stdout);
            }
        }
    } // namespace

    int tables()
    {
        const Case dt[3] = {{"0.3 V 1 kHz", 0.3, 683}, {"2 V 1 kHz", 2.0, 683}, {"4.5 V 3 kHz", 4.5, 2049}};
        const Case ts[3] = {{"30 mV 1 kHz", 0.03, 683}, {"200 mV 1 kHz", 0.2, 683}, {"450 mV 3 kHz", 0.45, 2049}};
        // adaa-worst.out, as the report's §5.2 quotes it
        const char* const dtReport[4] = {
            "alias -42.0 / -58.0 | H3 -1.23 / -3.74, H1 -0.40 | w -33.8 -54.0 -93.2 | prev -2.7 -5.0 -7.7 -9.7 -11.5 "
            "-13.2 | ADAA w -51.3 -89.1 -83.5",
            "alias -52.4 / -81.9 | H3 -0.33 / -0.91, H1 -0.12 | w -34.0 -54.2 -93.9 | prev -18.0 -24.4 -29.1 -33.2 "
            "-36.5 -40.2 | ADAA w -45.9 -83.2 -99.6",
            "alias -79.8 / -107.6 | H3 -0.08 / -0.24, H1 -0.04 | w -34.5 -54.5 -93.7 | prev -38.4 -46.6 -55.0 -69.5 "
            "-97.0 -127.0 | ADAA w -45.8 -84.2 -99.5",
            "alias -113.9 / -140.5 | H3 -0.02 / -0.08, H1 -0.01 | w -34.6 -55.9 -97.0 | prev -53.7 -72.2 -106.2 -118.1 "
            "-118.1 -118.1 | ADAA w -47.2 -84.9 -98.9"};
        const char* const tsReport[4] = {
            "alias -22.7 / -35.9 | H3 -0.03 / -0.58, H1 -0.12 | w -30.8 -51.7 -92.2 | prev 27.3 26.4 27.6 26.3 27.6 "
            "25.3 "
            "| ADAA w -43.4 -83.4 -89.2",
            "alias -31.1 / -52.6 | H3 -0.01 / -0.17, H1 -0.04 | w -29.5 -49.8 -89.4 | prev 7.5 5.1 2.6 0.3 -0.7 -1.4 | "
            "ADAA w -46.3 -83.7 -88.0",
            "alias -53.6 / -75.1 | H3 -0.02 / -0.06, H1 -0.01 | w -29.8 -51.1 -91.8 | prev -13.7 -17.1 -19.9 -22.4 "
            "-24.4 "
            "-26.4 | ADAA w -44.3 -81.2 -89.4",
            "alias -72.9 / -89.7 | H3 -0.00 / -0.03, H1 -0.00 | w -34.0 -54.4 -94.4 | prev -32.2 -38.2 -43.2 -47.1 "
            "-51.0 "
            "-56.7 | ADAA w -42.3 -82.6 -92.6"};
        std::printf("Report §5.2: the worst case over pots {0, 0.5, 1} x the three cases (adaa.cpp worst: N = 32768, "
                    "bins 683 and 2049; N Vt 45.3 mV); the solver columns from emitted code\n");
        adaaWorst("== Example 1 (diodetone.cir) worst case ==", "diodetone.cir", "tone", dt, dtReport);
        adaaWorst("== Example 2 block 2 (screamer-clip.cir) worst case ==", "screamer-clip.cir", "drive", ts, tsReport);
        table1();
        table2();
        table3();
        return 0;
    }
} // namespace circuits
