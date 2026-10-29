// examples/circuits/check.cpp: example-circuits --check (what ctest runs). The generated modules, run through the JIT,
// against the double reference of the same configuration and against a converged 64x one, on the report's targets
// (docs/research/analog-circuits/README.md §2): T1 solver <= -60 dB in band, T2 alias <= -60 dB, T3 H1 and H3 within
// 0.5 dB, T4 finite; with the generation itself, the DC point, the TS chain, the report's decision, the halfbands'
// indexing, float2, and block-split invariance. Each line prints the worst value, where it fell, and the report's
// figure beside it; a failure exits 1. Nothing is timed.
#include "harness.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <format>
#include <limits>
#include <numeric>
#include <string>
#include <vector>

namespace circuits
{
    namespace
    {
        constexpr std::size_t kWindow = 4096;                 // N: the analysis window, host samples
        constexpr std::size_t kWarm = 1024;                   // host samples before it
        constexpr std::size_t kHost = kWarm + kWindow + 64;   // and 64 after it (the decimator reads 48)
        constexpr double kGate = -60;
        constexpr double kResponse = 0.5;

        struct Report
        {
            int failures = 0;

            void line(bool ok, const std::string& text)
            {
                std::printf("  %s %s\n", ok ? "ok  " : "FAIL", text.c_str());
                std::fflush(stdout);
                failures += ok ? 0 : 1;
            }

            static void note(const std::string& text)
            {
                std::printf("       %s\n", text.c_str());
            }
        };

        // A worst value and where it fell.
        struct Worst
        {
            double value = -std::numeric_limits<double>::infinity();
            std::string where;

            void max(double v, const std::string& at)
            {
                if (v > value)
                {
                    value = v;
                    where = at;
                }
            }
        };

        struct Signal
        {
            std::string name;
            enum class Kind
            {
                Sine,
                Sweep,
                Noise,
            } kind = Kind::Sine;
            double amp = 0;
            int bin = 0;
            bool overload = false;   // 2.2 x the target level: T4 only
        };

        std::size_t nextPow2(std::size_t n)
        {
            std::size_t p = 1;
            while (p < n)
                p <<= 1;
            return p;
        }

        std::vector<double> make(const Signal& s, int m, std::size_t n)
        {
            switch (s.kind)
            {
            case Signal::Kind::Sine:
                return fc::sine(s.amp, s.bin, kWindow, m, n);
            case Signal::Kind::Sweep:
                return fc::sweep(s.amp, m, n);
            case Signal::Kind::Noise:
            {
                std::vector<double> v = fc::noise(s.amp, m, nextPow2(n));
                v.resize(n);
                return v;
            }
            }
            return {};
        }

        std::string hz(int bin)
        {
            return std::format("{:.0f} Hz", bin * 48000.0 / kWindow);
        }

        // Report §2's stress set at level L: 5 levels x 3 odd bins, the sweep and the noise at L.
        std::vector<Signal> stressSet(double level)
        {
            std::vector<Signal> set;
            const double levels[] = {level / 15, level / 4.5, level / 2.25, level, 2.2 * level};
            for (double a : levels)
            {
                for (int bin : {9, 85, 257})
                    set.push_back(
                        {std::format("{:.3g} V {}", a, hz(bin)), Signal::Kind::Sine, a, bin, a > level * 1.01});
            }
            set.push_back({std::format("sweep at {:.3g} V", level), Signal::Kind::Sweep, level, 0, false});
            set.push_back({std::format("noise at {:.3g} V", level), Signal::Kind::Noise, level, 0, false});
            return set;
        }

        std::span<const double> windowOf(const std::vector<double>& y, std::size_t m = 1)
        {
            return std::span<const double>(y).subspan(kWarm * m, kWindow * m);
        }

        // The 64x converged plain reference's H1 and H3 for a sine, and the emitted tick's, in dB of difference.
        struct Harmonics
        {
            double h1 = 0;
            double h3 = 0;
            bool h3Counted = false;
        };

        Harmonics harmonics(std::span<const double> tickWindow, std::span<const double> ref64Window, int bin)
        {
            const double r1 = fc::binMagnitude(ref64Window, bin);
            const double r3 = fc::binMagnitude(ref64Window, 3 * bin);
            const double t1 = fc::binMagnitude(tickWindow, bin);
            const double t3 = fc::binMagnitude(tickWindow, 3 * bin);
            Harmonics h;
            h.h1 = std::fabs(dB(t1 / r1));
            h.h3Counted = r3 / r1 >= 1e-5;   // H3 more than -100 dB under H1
            h.h3 = h.h3Counted ? std::fabs(dB(t3 / r3)) : 0;
            return h;
        }

        std::string configText(const fc::Config& c)
        {
            return std::format("{}x {}, {}, {}({})", c.oversample, c.halfband == fc::Halfband::Iir ? "IIR" : "FIR",
                               c.adaa ? "ADAA" : "no ADAA",
                               c.solver.guess == fc::Solver::Guess::Omega ? "omega" : "newton", c.solver.steps);
        }

        std::unique_ptr<Circuit> buildOrReport(Report& r, const std::string& text, const std::string& name,
                                               const fc::Options& options = {})
        {
            std::string errors;
            std::unique_ptr<Circuit> c = build(text, name, options, errors);
            if (!c)
                r.line(false, name + " does not build:\n" + errors);
            return c;
        }

        // ---- a. the shipped netlists generate cleanly, and their methods are nonblocking ----
        void generation(Report& r)
        {
            std::printf(
                "a. the shipped netlists generate with no error or warning; init, set, tick, step and clear are "
                "nonblocking\n");
            for (const char* name : {"diodetone.cir", "screamer.cir", "screamer-clip.cir", "clipper.cir"})
            {
                const fc::Result result = fc::generate(netlist(name), name);
                if (!result.errors.empty() || !result.warnings.empty())
                {
                    r.line(false, std::string(name) + ":\n" + result.errors + result.warnings);
                    continue;
                }
                std::string errors;
                std::unique_ptr<Circuit> c = build(netlist(name), name, {}, errors);
                if (!c)
                {
                    r.line(false, std::string(name) + ": " + errors);
                    continue;
                }
                std::string bad;
                for (const char* method : {"init", "set", "tick", "step", "clear"})
                {
                    const std::string full = c->result.module + "::" + method;
                    const bool exists = std::find(c->module->methods.begin(), c->module->methods.end(), full) !=
                                        c->module->methods.end();
                    if (exists && !c->program->nonBlocking(full))
                        bad += " " + full;
                }
                r.line(bad.empty(), std::format("{}: module {}, {} block(s){}", name, c->result.module,
                                                c->plan.blocks.size(), bad.empty() ? "" : "; not nonblocking:" + bad));
            }
        }

        // ---- b. DC ----
        void dc(Report& r)
        {
            std::printf("b. DC: diodetone.cir's tick at 48 kHz, tone 1, 0.25 s (docs/circuits.md, \"DC\")\n");
            const std::string text = netlist("diodetone.cir");
            std::unique_ptr<Circuit> shipped = buildOrReport(r, text, "diodetone.cir");
            fc::Options adaa;
            adaa.adaa = true;
            adaa.solver = fc::Solver{fc::Solver::Guess::Omega, 1};
            std::unique_ptr<Circuit> withAdaa = buildOrReport(r, text, "diodetone.cir", adaa);
            if (!shipped || !withAdaa)
                return;
            const fc::Plan& plan = shipped->plan;
            const std::vector<double> pots = potsWith(plan, "tone", 1);
            const double nvt = plan.blocks[0].port->nvt;
            auto root = [&](double u)
            {
                double lo = 0;
                double hi = u;
                for (int i = 0; i < 200; ++i)
                {
                    const double m = 0.5 * (lo + hi);
                    (m + 2200 * 2 * 2.52e-9 * std::sinh(m / nvt) > u ? hi : lo) = m;
                }
                return lo;
            };
            auto mean = [](const std::vector<double>& y, std::size_t n)
            {
                return std::accumulate(y.end() - static_cast<long>(n), y.end(), 0.0) / static_cast<double>(n);
            };
            const fc::Holters plain = fc::holters(plan, 0, 4 * 48000.0, pots);
            const fc::Holters designed = fc::holters(plan, 0, 4 * 48000.0 * 2 / 3, pots);
            for (double u : {0.1, 1.0, 5.0})
            {
                const std::vector<double> in(12000, u);
                const double exact = root(u);
                // as shipped: plain omega3 + 2 steps, a fixed point the double reference shares
                const std::vector<double> y = runTicks(*shipped, in, pots);
                fc::RefOptions o;
                o.steps = 2;
                const double ref = fc::simulate(plain, std::vector<double>(48000, u), o).back();
                r.line(std::fabs(y.back() - ref) <= 1e-5 && std::fabs(y.back() - exact) <= 2e-5,
                       std::format("{} V, as shipped: {:.7f} V; the double reference of the same configuration {:.7f}, "
                                   "the exact root {:.7f}",
                                   u, y.back(), ref, exact));
                // with ADAA and omega3 + 1 step (the report's, Phase 0's first default): ADAA's mean switches to the
                // midpoint below |dp| = 1e-5 V, which carries the step's first-order error; at high DC it does not
                // settle, so float and double land apart (an observation, not a gate)
                const std::vector<double> ya = runTicks(*withAdaa, in, pots);
                fc::RefOptions oa;
                oa.adaa = true;
                oa.steps = 1;
                const std::vector<double> refa = fc::simulate(designed, std::vector<double>(48000, u), oa);
                const double tickMean = mean(ya, 960);
                double spread = 0;
                for (auto it = ya.end() - 960; it != ya.end(); ++it)
                    spread = std::max(spread, std::fabs(*it - tickMean));
                r.line(std::fabs(tickMean - exact) <= 1.5e-3,
                       std::format("{} V, ADAA omega(1): {:.7f} V, {:+.4f} mV from the exact root (within 1.5 mV); it "
                                   "wanders by {:.1e} V, and the double reference sits at {:.7f}",
                                   u, tickMean, 1e3 * (tickMean - exact), spread, mean(refa, 3840)));
            }
        }

        // The report's figures for a configuration (§5.2's worst case over its three cases x pots).
        struct Figures
        {
            double solver;
            double alias;
            double h3;
        };

        // ---- c. the gates, on the two 1-port blocks ----
        void gates(Report& r, const std::string& label, const std::string& text, const std::string& name,
                   const std::string& pot, const Figures& report)
        {
            std::unique_ptr<Circuit> c = buildOrReport(r, text, name);
            if (!c)
                return;
            const fc::Plan& plan = c->plan;
            const int m = plan.config.oversample;
            const bool adaa = plan.config.adaa;
            const double level = *plan.netlist.target.level;
            std::printf("c. %s %s (%s), L = %g V; %s 0, 0.5 and 1; report §2's stress set\n", name.c_str(),
                        label.c_str(), configText(plan.config).c_str(), level, pot.c_str());
            Worst t1;
            Worst t2;
            Worst converged;
            Worst t3;
            bool finiteAll = true;
            std::string notFinite;
            fc::RefStats stats;
            for (double p : {0.0, 0.5, 1.0})
            {
                const std::vector<double> pots = potsWith(plan, pot, p);
                const fc::Holters h = fc::holters(plan, 0, 48000.0 * m * plan.blocks[0].designFactor, pots);
                const fc::Holters h64 = fc::holters(plan, 0, 48000.0 * 64, pots);
                for (const Signal& s : stressSet(level))
                {
                    const std::string at = std::format("{} {}, {}", pot, p, s.name);
                    const std::vector<double> u = make(s, m, m * kHost);
                    const std::vector<double> y = runSteps(*c, u, pots);
                    fc::RefOptions o;
                    o.adaa = adaa;
                    const std::vector<double> ref = fc::simulate(h, u, o, &stats);
                    const std::vector<double> yd = fc::decimateWindow(y, m, kWarm, kWindow);
                    const std::vector<double> rd = fc::decimateWindow(ref, m, kWarm, kWindow);
                    if (!s.overload)
                        t1.max(fc::bandErrorDb(yd, rd, 0, s.kind != Signal::Kind::Sine), at);
                    const std::vector<double> ticks = runTicks(*c, make(s, 1, kHost), pots);
                    if (!finite(y) || !finite(ticks))
                    {
                        finiteAll = false;
                        notFinite = at;
                    }
                    if (s.kind != Signal::Kind::Sine || s.overload)
                        continue;
                    t2.max(fc::inharmonicDb(windowOf(ticks), s.bin), at);
                    converged.max(fc::inharmonicDb(rd, s.bin), at);
                    if (s.bin == 9)
                        continue;
                    const std::vector<double> ref64 =
                        fc::simulate(h64, fc::sine(s.amp, s.bin, kWindow, 64, 64 * kHost), {}, &stats);
                    const Harmonics hm = harmonics(windowOf(ticks), windowOf(ref64, 64), s.bin);
                    t3.max(hm.h1, at + ", H1");
                    if (hm.h3Counted)
                        t3.max(hm.h3, at + ", H3");
                }
            }
            r.line(t1.value <= kGate, std::format("T1 solver {:6.1f} dB at {} (gate {}; report §5.2: {})", t1.value,
                                                  t1.where, kGate, report.solver));
            r.line(t2.value <= kGate,
                   std::format("T2 alias  {:6.1f} dB at {}, the emitted tick (gate {}); converged "
                               "{:.1f} at {} (report §5.2: {})",
                               t2.value, t2.where, kGate, converged.value, converged.where, report.alias));
            r.line(t3.value <= kResponse,
                   std::format("T3 H1, H3 {:.3f} dB at {}, against 64x (gate {}; report §5.2: H3 "
                               "{})",
                               t3.value, t3.where, kResponse, report.h3));
            r.line(finiteAll, finiteAll ? std::string("T4 finite on every signal, 2.2 L, the sweep and the noise too")
                                        : "T4 not finite at " + notFinite);
            r.line(stats.unconverged == 0, std::format("the references: {} unconverged of {} samples, at most {} "
                                                       "Newton steps",
                                                       stats.unconverged, stats.samples, stats.maxIterations));
        }

        // ---- d. the Tube Screamer chain ----
        void chain(Report& r)
        {
            const std::string name = "screamer.cir";
            std::unique_ptr<Circuit> c = buildOrReport(r, netlist(name), name);
            if (!c)
                return;
            const fc::Plan& plan = c->plan;
            std::printf("d. %s as shipped (%s; blocks %d to %d in the region): T3 against the 64x chain, drive x tone "
                        "{0, 0.5, 1}, 0.45 V\n",
                        name.c_str(), configText(plan.config).c_str(), plan.regionFirst + 1, plan.regionLast + 1);
            constexpr std::size_t warm = 4096;   // block 1's 15.9 Hz high-pass settles
            const std::size_t host = warm + kWindow + 64;
            Worst t3;
            fc::RefStats stats;
            for (double drive : {0.0, 0.5, 1.0})
            {
                for (double tone : {0.0, 0.5, 1.0})
                {
                    std::vector<double> pots = potsWith(plan, "drive", drive);
                    pots[1] = tone;
                    const std::vector<fc::Holters> blocks = chainAt(plan, pots, 48000.0 * 64, false);
                    for (int bin : {85, 257})
                    {
                        const std::vector<double> ticks = runTicks(*c, fc::sine(0.45, bin, kWindow, 1, host), pots);
                        const std::vector<double> ref =
                            fc::simulateChain(blocks, fc::sine(0.45, bin, kWindow, 64, 64 * host), {}, &stats);
                        const Harmonics hm =
                            harmonics(std::span<const double>(ticks).subspan(warm, kWindow),
                                      std::span<const double>(ref).subspan(64 * warm, 64 * kWindow), bin);
                        const std::string at = std::format("drive {}, tone {}, {}", drive, tone, hz(bin));
                        t3.max(hm.h1, at + ", H1");
                        if (hm.h3Counted)
                            t3.max(hm.h3, at + ", H3");
                    }
                }
            }
            r.line(t3.value <= kResponse,
                   std::format("T3 H1, H3 {:.3f} dB at {} (gate {}: the tone block in the region, report §9.2)",
                               t3.value, t3.where, kResponse));
            r.line(stats.unconverged == 0,
                   std::format("the chain reference: {} unconverged of {} samples", stats.unconverged, stats.samples));
            const std::vector<double> defaults = potsWith(plan, "", 0);
            const std::vector<double> second = runTicks(*c, fc::sine(0.99, 85, kWindow, 1, 48000), defaults);
            const std::vector<double> swept = runTicks(*c, fc::sweep(0.45, 1, 48000), defaults);
            r.line(finite(second) && finite(swept), "T4 finite: 1 s at 1 kHz and 2.2 L, and the sweep");
            // report §9.1's orientation, end to end: tone 1 is bright
            const std::vector<double> u = fc::sine(0.005, 427, kWindow, 1, kHost);
            std::vector<double> bright = potsWith(plan, "tone", 1);
            std::vector<double> dark = potsWith(plan, "tone", 0);
            const double b = fc::binMagnitude(windowOf(runTicks(*c, u, bright)), 427);
            const double d = fc::binMagnitude(windowOf(runTicks(*c, u, dark)), 427);
            r.line(dB(b / d) >= 10,
                   std::format("a 5 mV, {} tone is {:.1f} dB louder at tone 1 than at tone 0 (at least "
                               "10; eq. 24 gives 16.4)",
                               hz(427), dB(b / d)));
        }

        // ---- e. the report's decision, on its three cases ----
        struct Gates
        {
            Worst t1;
            Worst alias;
            Worst converged;
            Worst t3;
        };

        Gates measure(const Circuit& c, const std::string& pot, const std::vector<Signal>& cases)
        {
            Gates g;
            const fc::Plan& plan = c.plan;
            const int m = plan.config.oversample;
            for (double p : {0.0, 0.5, 1.0})
            {
                const std::vector<double> pots = potsWith(plan, pot, p);
                const fc::Holters h = fc::holters(plan, 0, 48000.0 * m * plan.blocks[0].designFactor, pots);
                const fc::Holters h64 = fc::holters(plan, 0, 48000.0 * 64, pots);
                for (const Signal& s : cases)
                {
                    const std::string at = std::format("{} {}, {}", pot, p, s.name);
                    const std::vector<double> u = make(s, m, m * kHost);
                    fc::RefOptions o;
                    o.adaa = plan.config.adaa;
                    const std::vector<double> rd = fc::decimateWindow(fc::simulate(h, u, o), m, kWarm, kWindow);
                    const std::vector<double> yd = fc::decimateWindow(runSteps(c, u, pots), m, kWarm, kWindow);
                    g.t1.max(fc::bandErrorDb(yd, rd), at);
                    const std::vector<double> ticks = runTicks(c, make(s, 1, kHost), pots);
                    g.alias.max(fc::inharmonicDb(windowOf(ticks), s.bin), at);
                    g.converged.max(fc::inharmonicDb(rd, s.bin), at);
                    const std::vector<double> ref64 =
                        fc::simulate(h64, fc::sine(s.amp, s.bin, kWindow, 64, 64 * kHost), {});
                    const Harmonics hm = harmonics(windowOf(ticks), windowOf(ref64, 64), s.bin);
                    g.t3.max(std::max(hm.h1, hm.h3), at);
                }
            }
            return g;
        }

        void decision(Report& r)
        {
            std::printf("e. the decision (report §5.2, §6), on its three cases x pots\n");
            const std::vector<Signal> dt{{"0.3 V 996 Hz", Signal::Kind::Sine, 0.3, 85, false},
                                         {"2 V 996 Hz", Signal::Kind::Sine, 2.0, 85, false},
                                         {"4.5 V 3012 Hz", Signal::Kind::Sine, 4.5, 257, false}};
            const std::vector<Signal> ts{{"30 mV 996 Hz", Signal::Kind::Sine, 0.03, 85, false},
                                         {"200 mV 996 Hz", Signal::Kind::Sine, 0.2, 85, false},
                                         {"450 mV 3012 Hz", Signal::Kind::Sine, 0.45, 257, false}};
            fc::Options plain;
            plain.adaa = false;
            plain.solver = fc::Solver{fc::Solver::Guess::Omega, 2};
            if (auto c = buildOrReport(r, netlist("diodetone.cir"), "diodetone.cir", plain))
            {
                const Gates g = measure(*c, "tone", dt);
                r.line(g.t1.value <= kGate && g.alias.value <= kGate && g.t3.value <= kResponse,
                       std::format("diodetone.cir, plain omega(2) at 4x passes: T1 {:.1f}, alias {:.1f}, T3 {:.2f} dB "
                                   "(report: -93.7, -79.8, H3 -0.08)",
                                   g.t1.value, g.alias.value, g.t3.value));
            }
            if (auto c = buildOrReport(r, netlist("screamer-clip.cir"), "screamer-clip.cir", plain))
            {
                const Gates g = measure(*c, "drive", ts);
                r.line(g.alias.value > kGate,
                       std::format("screamer-clip.cir, plain omega(2) at 4x fails T2: alias {:.1f} dB at {} (converged "
                                   "{:.1f}; report §5.2: -53.6)",
                                   g.alias.value, g.alias.where, g.converged.value));
            }
            fc::Options adaa;
            adaa.adaa = true;
            adaa.solver = fc::Solver{fc::Solver::Guess::Omega, 1};
            if (auto c = buildOrReport(r, netlist("diodetone.cir"), "diodetone.cir", adaa))
            {
                // T3 on §2's stress set: the sines up to L at 996 and 3012 Hz (the report measured three cases)
                std::vector<Signal> sines;
                for (const Signal& sig : stressSet(4.5))
                {
                    if (sig.kind == Signal::Kind::Sine && !sig.overload && sig.bin != 9)
                        sines.push_back(sig);
                }
                const Gates g = measure(*c, "tone", sines);
                r.line(g.t3.value > kResponse,
                       std::format("diodetone.cir, ADAA omega(1) at 4x fails T3 on the stress set: {:.3f} dB at {} "
                                   "(report §5.2, on its three cases: H3 -0.24); so Phase 0's default is 8x plain "
                                   "omega(2) (docs/circuits.md)",
                                   g.t3.value, g.t3.where));
            }
            fc::Options eight = plain;
            eight.oversample = 8;
            if (auto c = buildOrReport(r, netlist("screamer-clip.cir"), "screamer-clip.cir", eight))
            {
                const Gates g = measure(*c, "drive", ts);
                r.line(
                    g.t1.value <= kGate && g.alias.value <= kGate && g.t3.value <= kResponse,
                    std::format("screamer-clip.cir, plain omega(2) at 8x passes: T1 {:.1f}, alias {:.1f}, T3 {:.2f} dB "
                                "(report: -94.4, -72.9, H3 -0.00)",
                                g.t1.value, g.alias.value, g.t3.value));
            }
            fc::Options two;
            two.oversample = 2;
            if (auto c = buildOrReport(r, netlist("screamer-clip.cir"), "screamer-clip.cir", two))
            {
                const Gates g = measure(*c, "drive", ts);
                r.line(g.alias.value > kGate,
                       std::format("screamer-clip.cir, ADAA omega(1) at 2x fails T2: alias {:.1f} dB at {} (converged "
                                   "{:.1f}; report §5.2: -52.6)",
                                   g.alias.value, g.alias.where, g.converged.value));
            }
        }

        // ---- f. tick against step: the halfbands' indexing ----
        void halfbands(Report& r)
        {
            std::printf("f. tick against step, through each oversampler: 0.1 V, 996 Hz, diodetone.cir at tone 0.5\n");
            const std::string text = netlist("diodetone.cir");
            for (fc::Halfband kind : {fc::Halfband::Iir, fc::Halfband::Fir})
            {
                for (int m : {2, 4, 8})
                {
                    fc::Options o;
                    o.oversample = m;
                    o.halfband = kind;
                    auto c = buildOrReport(r, text, "diodetone.cir", o);
                    if (!c)
                        continue;
                    const std::vector<double> pots = potsWith(c->plan, "tone", 0.5);
                    const std::vector<double> stepped = fc::decimateWindow(
                        runSteps(*c, fc::sine(0.1, 85, kWindow, m, m * kHost), pots), m, kWarm, kWindow);
                    const std::vector<double> ticks = runTicks(*c, fc::sine(0.1, 85, kWindow, 1, kHost), pots);
                    const double gain = dB(fc::binMagnitude(windowOf(ticks), 85) / fc::binMagnitude(stepped, 85));
                    const double alias = fc::inharmonicDb(windowOf(ticks), 85);
                    r.line(
                        std::fabs(gain) <= 0.01 && alias <= -85,
                        std::format("{}x {}: the fundamental {:+.4f} dB against the steps ideally decimated, the rest "
                                    "{:.1f} dB",
                                    m, kind == fc::Halfband::Iir ? "IIR" : "FIR", gain, alias));
                }
            }
        }

        // ---- g. .channels 2: each lane is the scalar module's ----
        void stereo(Report& r)
        {
            std::printf("g. .channels 2: each lane against the scalar module, bit for bit\n");
            const std::string text = netlist("diodetone.cir");
            fc::Options two;
            two.channels = 2;
            auto pair = buildOrReport(r, text, "diodetone.cir", two);
            auto single = buildOrReport(r, text, "diodetone.cir");
            if (!pair || !single)
                return;
            const std::vector<double> a = fc::sine(1.0, 85, kWindow, 1, kHost);
            std::vector<double> b = fc::noise(2.0, 1, 8192);
            b.resize(kHost);
            const std::vector<double> pots = potsWith(single->plan, "tone", 0.3);
            const std::vector<double> ya = runTicks(*single, a, pots);
            const std::vector<double> yb = runTicks(*single, b, pots);
            Instance self(*pair->module);
            pair->init(self.get(), 48000);
            std::vector<float> fa(a.begin(), a.end());
            std::vector<float> fb(b.begin(), b.end());
            std::vector<float> pa(kHost);
            std::vector<float> pb(kHost);
            const std::vector<float> potBuffer(kHost, 0.3f);
            pair->ticks2(self.get(), fa.data(), fb.data(), potBuffer.data(), pa.data(), pb.data(),
                         static_cast<int>(kHost), 0);
            std::size_t differ = 0;
            for (std::size_t i = 0; i < kHost; ++i)
                differ += (static_cast<double>(pa[i]) != ya[i] ? 1 : 0) + (static_cast<double>(pb[i]) != yb[i] ? 1 : 0);
            r.line(differ == 0, std::format("{} of {} samples differ (a 1 V sine on one lane, 2 V noise on the other)",
                                            differ, 2 * kHost));
        }

        // ---- h. block-split invariance ----
        void blocks(Report& r)
        {
            std::printf(
                "h. screamer.cir in calls of 64 and 61 samples against one call, drive gliding 0 -> 1 over 20 ms, "
                "set on the absolute 16-sample grid\n");
            auto c = buildOrReport(r, netlist("screamer.cir"), "screamer.cir");
            if (!c)
                return;
            constexpr std::size_t n = 4800;
            const std::vector<double> u = fc::sine(0.45, 85, kWindow, 1, n);
            std::vector<float> pots;
            for (std::size_t i = 0; i < n; ++i)
            {
                const double drive = std::clamp((static_cast<double>(i) - 1000) / 960, 0.0, 1.0);
                pots.push_back(static_cast<float>(drive));
                pots.push_back(0.5f);
                pots.push_back(0.7f);
            }
            const std::vector<double> whole = runTicksWith(*c, u, pots);
            for (int call : {64, 61})
            {
                const std::vector<double> parts = runTicksWith(*c, u, pots, call);
                std::size_t differ = 0;
                for (std::size_t i = 0; i < n; ++i)
                    differ += parts[i] != whole[i] ? 1 : 0;
                r.line(differ == 0 && finite(parts),
                       std::format("calls of {}: {} of {} samples differ from one call", call, differ, n));
            }
        }
    } // namespace

    int check()
    {
        Report r;
        generation(r);
        dc(r);
        gates(r, "as shipped", netlist("diodetone.cir"), "diodetone.cir", "tone", {-93.7, -79.8, -0.08});
        gates(r, "as shipped", netlist("screamer-clip.cir"), "screamer-clip.cir", "drive", {-81.2, -75.1, -0.06});
        // Phase 0's defaults: the netlists without their configuration lines
        auto defaults = [](std::string text)
        {
            for (const char* d : {".oversample", ".adaa", ".solver"})
                text = withDirective(text, d, "");
            return text;
        };
        gates(r, "on Phase 0's defaults", defaults(netlist("diodetone.cir")), "diodetone.cir", "tone",
              {-97.0, -113.9, -0.02});
        gates(r, "on Phase 0's defaults", defaults(netlist("screamer-clip.cir")), "screamer-clip.cir", "drive",
              {-94.4, -72.9, -0.00});
        chain(r);
        decision(r);
        halfbands(r);
        stereo(r);
        blocks(r);
        std::printf("%s: %d check(s) failed\n", r.failures == 0 ? "passed" : "FAILED", r.failures);
        return r.failures == 0 ? 0 : 1;
    }
} // namespace circuits
