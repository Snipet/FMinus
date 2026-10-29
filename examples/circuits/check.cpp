// examples/circuits/check.cpp: example-circuits --check (what ctest runs). The generated modules, run through the JIT,
// against the double reference of the same configuration and against a converged 64x one, on the report's targets
// (docs/research/analog-circuits/README.md §2): T1 solver <= -60 dB in band, T2 alias <= -60 dB, T3 H1 and H3 within
// 0.5 dB, T4 finite; with the generation itself, the DC point, the TS chain, the report's decision, the halfbands'
// indexing, float2, block-split invariance and a pot's glide under ADAA (Phase 0's sections a to h); then Phase 1's:
// the self-test's ordered search (i), the same answer on 1 and 8 threads (j), the run-time guard (k), the provisional
// configuration (l), and the synth path's otalp.cir (m: T1-T4 on the synth's stress matrix, the self-test's choice,
// tick against step, the prewarp's -3 dB points, the carry under ADAA; n: a per-sample law input's lanes and block
// splits). Each line prints the worst value, where it fell, and the report's figure beside it; a failure exits 1.
// Nothing is timed.
#include "harness.h"

#include "circuit/Search.h"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <format>
#include <functional>
#include <limits>
#include <map>
#include <span>
#include <numbers>
#include <numeric>
#include <optional>
#include <stdexcept>
#include <string>
#include <thread>
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
            for (const char* name : {"diodetone.cir", "screamer.cir", "screamer-clip.cir", "clipper.cir", "otalp.cir"})
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
        // Returns the worst T1, T2 and T3 it measured (section i compares the self-test's with them).
        Figures gates(Report& r, const std::string& label, const std::string& text, const std::string& name,
                      const std::string& pot, const Figures& report)
        {
            std::unique_ptr<Circuit> c = buildOrReport(r, text, name);
            if (!c)
                return {0, 0, 0};
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
            return {t1.value, t2.value, t3.value};
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
            pair->ticks2(self.get(), fa.data(), fb.data(), nullptr, potBuffer.data(), nullptr, pa.data(), pb.data(),
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

        // A pot that moves K moves the ADAA port's curve, p = v + 2 ka sinh(v / N Vt), and b<N>_set moves the last
        // point (v_D1, p_D1) onto the new one, keeping its Newton residual p_D1 - v_D1 - 2 ka sinh(v_D1 / N Vt);
        // otherwise ADAA's closed-form mean pairs a point of the old curve with the new curve's, and spikes when the
        // signal's move cancels the curve's, leaving |p - p_D1| small (rare, and bound to the stimulus: about one in 60
        // to 200 such glides of a 996 Hz sine computed in float clicks by 0.1 to 0.4 V, against 0.07 V held; none of
        // these does).
        // screamer-clip.cir as shipped, on a 0.45 V, 996 Hz sine, drive a triangle 0 -> 1 -> 0 of 960 samples a ramp
        // after 0.1 s at 0: (1) each set() keeps the residual, the gate on the cause; (2) end to end, against itself
        // without ADAA (omega(3)), the worst departure over 512 ramps against the worst with drive held at 0 and at 1
        // (what ADAA's eighth of a sample's delay, and its smaller alias, give).
        void glide(Report& r)
        {
            std::printf("h. a glide under ADAA: screamer-clip.cir, a 0.45 V, 996 Hz sine, drive a triangle 0 -> 1 -> 0 "
                        "of 960-sample ramps\n");
            auto adaa = buildOrReport(r, netlist("screamer-clip.cir"), "screamer-clip.cir");
            fc::Options plainOptions;
            plainOptions.adaa = false;
            plainOptions.solver = fc::Solver{fc::Solver::Guess::Omega, 3};
            auto plain = buildOrReport(r, netlist("screamer-clip.cir"), "screamer-clip.cir", plainOptions);
            if (!adaa || !plain)
                return;
            constexpr std::size_t warm = 4800;
            constexpr std::size_t ramp = 960;
            auto triangle = [](std::size_t i)
            {
                if (i < warm)
                    return 0.0;
                const std::size_t k = (i - warm) % (2 * ramp);
                return static_cast<double>(k < ramp ? k : 2 * ramp - k) / ramp;
            };
            // (1) the residual across each set(), read from the instance's fields
            {
                const fc::PortDef& port = *adaa->plan.blocks.front().port;
                auto offsetOf = [&](const std::string& name)
                {
                    for (const fminus::Program::Module::Field& f : adaa->module->fields)
                    {
                        if (f.name == name)
                            return f.offset;
                    }
                    throw std::runtime_error("no field " + name);
                };
                const std::size_t v = offsetOf("v_" + port.name);
                const std::size_t p = offsetOf("p_" + port.name);
                const std::size_t ka = offsetOf("ka_" + port.name);
                Instance self(*adaa->module);
                adaa->init(self.get(), 48000);
                auto read = [&](std::size_t offset)
                {
                    float f = 0;
                    std::memcpy(&f, static_cast<const char*>(self.get()) + offset, sizeof f);
                    return static_cast<double>(f);
                };
                auto curve = [&] { return read(ka) * 2 * std::sinh(read(v) / port.nvt); };
                const std::size_t n = warm + 4 * ramp;
                const std::vector<double> u = fc::sine(0.45, 85, kWindow, 1, n);
                double worst = 0;
                double moved = 0;
                for (std::size_t i = 0; i < n; ++i)
                {
                    if (i % 16 == 0)
                    {
                        const double pBefore = read(p);
                        const double cBefore = curve();
                        const double drive = triangle(i);
                        adaa->callSet(self.get(), std::span<const double>(&drive, 1));
                        const double scale = std::max({std::fabs(pBefore), std::fabs(read(p)), 1e-3});
                        const double residualChange = (read(p) - curve()) - (pBefore - cBefore);   // v is held
                        worst = std::max(worst, std::fabs(residualChange) / scale);
                        moved = std::max(moved, std::fabs(curve() - cBefore) / scale);
                    }
                    const float x = static_cast<float>(u[i]);
                    const float pot = 0;
                    float y = 0;
                    adaa->ticks(self.get(), &x, nullptr, &pot, nullptr, &y, 1, 1);   // start 1: no set() inside
                }
                r.line(worst <= 1e-5 && moved > 1e-3,
                       std::format("each set() of 4 ramps keeps the last point's Newton residual: it moves {:.1e} of "
                                   "|p_D1| at worst (gate 1e-5), where the curve moves up to {:.1e}",
                                   worst, moved));
            }
            // (2) end to end, against the plain module
            auto departure = [&](std::size_t ramps, auto drive)
            {
                const std::size_t n = warm + ramps * ramp;
                const std::vector<double> u = fc::sine(0.45, 85, kWindow, 1, n);
                std::vector<float> pots(n);
                for (std::size_t i = 0; i < n; ++i)
                    pots[i] = static_cast<float>(drive(i));
                const std::vector<double> a = runTicksWith(*adaa, u, pots);
                const std::vector<double> b = runTicksWith(*plain, u, pots);
                double worst = 0;
                for (std::size_t i = warm; i < n; ++i)
                    worst = std::max(worst, std::fabs(a[i] - b[i]));
                return worst;
            };
            const double held = std::max(departure(64, [](std::size_t) { return 0.0; }),
                                         departure(64, [](std::size_t i) { return i < warm ? 0.0 : 1.0; }));
            const double moving = departure(512, triangle);
            r.line(moving <= 1.1 * held,
                   std::format("against the plain omega(3) module over 512 ramps: {:.4f} V at worst, {:.4f} V with "
                               "drive held: within 1.1x",
                               moving, held));
        }

        // ---- i, j. the self-test ----
        // SelfTestOptions::parallel on `threads` std::threads.
        std::function<void(int, const std::function<void(int)>&)> pool(int threads)
        {
            return [threads](int count, const std::function<void(int)>& job)
            {
                std::atomic<int> next{0};
                auto work = [&]
                {
                    for (int i = next++; i < count; i = next++)
                        job(i);
                };
                std::vector<std::thread> ts;
                for (int k = 1; k < threads; ++k)
                    ts.emplace_back(work);
                work();
                for (std::thread& t : ts)
                    t.join();
            };
        }

        // The netlist's plan (it must have one).
        fc::Plan planned(const std::string& text, const std::string& name)
        {
            fc::Messages messages;
            const std::optional<fc::Plan> p = fc::plan(fc::parse(text, messages), {}, name, messages);
            if (!p)
                throw std::runtime_error(name + " does not plan:\n" + messages.format(name, fc::Severity::Error));
            return *p;
        }

        // The netlist with its configuration directives removed: every searched dimension open.
        std::string opened(std::string text)
        {
            for (const char* d : {".oversample", ".adaa", ".solver"})
                text = withDirective(text, d, "");
            return text;
        }

        // A choice as the directives that pin it: ".oversample 4, .adaa off, .solver newton(1)".
        std::string directivesOf(const fc::Choice& c)
        {
            return std::format(".oversample {}, .adaa {}, .solver {}({})", c.oversample, c.adaa ? "on" : "off",
                               c.solver.guess == fc::Solver::Guess::Omega ? "omega" : "newton", c.solver.steps);
        }

        // The self-test of `name` without its configuration directives, on 8 threads: it chooses `expected` (the
        // shipped directives, which the report and Phase 0's measurements chose by hand), its notes list each
        // candidate tried in the cost model's order with its verdict, and the answer's figures are section c's.
        std::optional<fc::SelfTestResult> search(Report& r, const std::string& name, const std::string& expected,
                                                 const Figures& c)
        {
            const std::string text = opened(netlist(name));
            fc::SelfTestOptions test;
            test.parallel = pool(8);
            const fc::SelfTestResult t = fc::selfTest(text, name, {}, test);
            if (!t.errors.empty() || !t.choice)
            {
                r.line(false, name + ": the self-test gave no answer:\n" + t.errors + t.notes);
                return std::nullopt;
            }
            // the candidates tried, in order: "self-test candidate k: <configuration>, <cost> model cycles: ..."
            std::vector<double> costs;
            int verdicts = 0;
            for (std::size_t at = t.notes.find("self-test candidate "); at != std::string::npos;
                 at = t.notes.find("self-test candidate ", at + 1))
            {
                const std::size_t end = t.notes.find('\n', at);
                const std::string line = t.notes.substr(at, end - at);
                const std::size_t cycles = line.find(" model cycles");
                const std::size_t comma = line.rfind(", ", cycles);
                costs.push_back(std::stod(line.substr(comma + 2, cycles - comma - 2)));
                verdicts += line.ends_with(": passes") || line.find(": fails (") != std::string::npos ? 1 : 0;
            }
            // the model's order: candidates()'s (by cost, ADAA first within 5 % of a group's cheapest)
            const std::vector<fc::Candidate> order = fc::candidates(planned(text, name));
            bool ordered = costs.size() <= order.size();
            for (std::size_t k = 0; ordered && k < costs.size(); ++k)
                ordered = std::fabs(costs[k] - order[k].cost) < 0.5;
            const std::string chose = fc::Choice::parse(t.choice->text()) ? t.choice->text() : std::string();
            const std::string got = directivesOf(*t.choice);
            const std::size_t all = order.size();
            r.line(got == expected && t.choice->met && !chose.empty(),
                   std::format("{} without its directives: the self-test chose {} (the shipped {}), candidate {} of "
                               "{}",
                               name, got, expected, t.candidates, all));
            r.line(ordered && verdicts == t.candidates && static_cast<int>(costs.size()) == t.candidates,
                   std::format("its notes list the {} candidates tried, each with its verdict, in the model's order "
                               "({:g} to {:g} cycles)",
                               t.candidates, costs.empty() ? 0.0 : costs.front(), costs.empty() ? 0.0 : costs.back()));
            const double d1 = std::fabs(t.choice->t1 - c.solver);
            const double d2 = std::fabs(t.choice->t2 - c.alias);
            const double d3 = std::fabs(t.choice->t3 - c.h3);
            r.line(d1 <= 1 && d2 <= 1 && d3 <= 1,
                   std::format("the answer's T1 {:.1f}, T2 {:.1f}, T3 {:.2f} dB; section c measured {:.1f}, {:.1f}, "
                               "{:.2f} (within 1 dB)",
                               t.choice->t1, t.choice->t2, t.choice->t3, c.solver, c.alias, c.h3));
            return t;
        }

        void selfTests(Report& r, const Figures& diodeTone, const Figures& screamerClip)
        {
            std::printf("i. the self-test's ordered search, on diodetone.cir and screamer-clip.cir without their "
                        "configuration directives (the full stress matrix, 8 threads)\n");
            const std::optional<fc::SelfTestResult> dt =
                search(r, "diodetone.cir", ".oversample 4, .adaa off, .solver omega(2)", diodeTone);
            search(r, "screamer-clip.cir", ".oversample 4, .adaa on, .solver omega(1)", screamerClip);
            std::printf("j. diodetone.cir's self-test on the calling thread alone, against the 8 threads of i\n");
            if (!dt)
                return;
            const fc::SelfTestResult serial = fc::selfTest(opened(netlist("diodetone.cir")), "diodetone.cir", {});
            r.line(serial.choice && serial.choice->text() == dt->choice->text() && serial.notes == dt->notes,
                   std::format("the answer ({}) and the notes ({} bytes) are the same, byte for byte",
                               serial.choice ? serial.choice->text() : std::string("none"), serial.notes.size()));
        }

        // ---- k. the guard ----
        // diodetone.cir as shipped, a 1 V, 996 Hz sine, tone 0.5: a NaN written into x_C1 between ticks 100 and 101
        // (one channel), or into the second lane of x_C1 (.channels 2). From the next multiple of 16 ticks the output
        // is finite, the fields are a fresh instance's after the same tick (clear()'s, a tick on), but fd_, the fade,
        // which rises monotonically to exactly 1 within 960 samples (+-1); calls of 1, 61 and 64 samples agree, bit
        // for bit.
        void guard(Report& r)
        {
            std::printf("k. the run-time guard: diodetone.cir, a NaN written into x_C1 after tick 100\n");
            auto c = buildOrReport(r, netlist("diodetone.cir"), "diodetone.cir");
            fc::Options two;
            two.channels = 2;
            auto stereo = buildOrReport(r, netlist("diodetone.cir"), "diodetone.cir", two);
            if (!c || !stereo)
                return;
            auto offsetOf = [](const Circuit& circuit, const std::string& name)
            {
                for (const fminus::Program::Module::Field& f : circuit.module->fields)
                {
                    if (f.name == name)
                        return f.offset;
                }
                throw std::runtime_error("no field " + name);
            };
            constexpr std::size_t n = 2000;
            constexpr std::size_t poisoned = 101;   // the NaN is written before this tick
            const std::vector<double> u = fc::sine(1.0, 85, kWindow, 1, n);
            const std::vector<float> in(u.begin(), u.end());
            const float tone = 0.5f;
            const std::vector<float> pots(n, tone);
            const std::size_t x = offsetOf(*c, "x_C1");
            const std::size_t fd = offsetOf(*c, "fd_");
            auto readFloat = [](const void* base, std::size_t offset)
            {
                float f = 0;
                std::memcpy(&f, static_cast<const char*>(base) + offset, sizeof f);
                return f;
            };
            // the run, in calls of `call` samples, the NaN written between two calls; fd_ after each call
            auto run = [&](int call, std::vector<float>* fade, Instance& self)
            {
                c->init(self.get(), 48000);
                std::vector<float> out(n);
                auto ticks = [&](std::size_t from, std::size_t until)
                {
                    for (std::size_t at = from; at < until;)
                    {
                        const std::size_t count = std::min(static_cast<std::size_t>(call), until - at);
                        c->ticks(self.get(), in.data() + at, nullptr, pots.data() + at, nullptr, out.data() + at,
                                 static_cast<int>(count), static_cast<int>(at));
                        if (fade != nullptr)
                            fade->push_back(readFloat(self.get(), fd));
                        at += count;
                    }
                };
                ticks(0, poisoned);
                const float nan = std::numeric_limits<float>::quiet_NaN();
                std::memcpy(static_cast<char*>(self.get()) + x, &nan, sizeof nan);
                ticks(poisoned, n);
                return out;
            };
            std::vector<float> fade;
            Instance whole(*c->module);
            const std::vector<float> one = run(1, &fade, whole);
            const std::size_t reset = (poisoned + 15) / 16 * 16;   // 112: the next multiple of 16 ticks
            bool finiteAfter = true;
            for (std::size_t i = reset; i < n; ++i)
                finiteAfter = finiteAfter && std::isfinite(one[i]);
            r.line(finiteAfter && one[reset] == 0,
                   std::format("the output is finite from tick {}, where the guard resets (0 there: the fade starts at "
                               "0; before it, the bracket kept v_D1, and so the output, finite)",
                               reset));
            // the fields at the reset: a fresh instance's after one tick of the same input, but the fade's
            {
                Instance fresh(*c->module);
                c->init(fresh.get(), 48000);
                float y = 0;
                c->ticks(fresh.get(), in.data() + reset, nullptr, pots.data() + reset, nullptr, &y, 1, 0);
                Instance again(*c->module);
                // the run again, stopped right after the reset's tick; x_C1 is NaN until it
                c->init(again.get(), 48000);
                std::vector<float> out(n);
                bool poisonedUntil = true;
                for (std::size_t i = 0; i <= reset; ++i)
                {
                    if (i == poisoned)
                    {
                        const float nan = std::numeric_limits<float>::quiet_NaN();
                        std::memcpy(static_cast<char*>(again.get()) + x, &nan, sizeof nan);
                    }
                    if (i > poisoned && i <= reset)
                        poisonedUntil = poisonedUntil && std::isnan(readFloat(again.get(), x));
                    c->ticks(again.get(), in.data() + i, nullptr, pots.data() + i, nullptr, out.data() + i, 1,
                             static_cast<int>(i));
                }
                r.line(poisonedUntil && std::isfinite(readFloat(again.get(), x)),
                       std::format("x_C1 stays NaN from tick {} to the guard's check at tick {}, and is finite "
                                   "after it",
                                   poisoned, reset));
                std::string differ;
                for (const fminus::Program::Module::Field& f : c->module->fields)
                {
                    if (f.name == "fd_")
                        continue;
                    if (std::memcmp(static_cast<const char*>(fresh.get()) + f.offset,
                                    static_cast<const char*>(again.get()) + f.offset, f.size) != 0)
                        differ += " " + f.name;
                }
                r.line(differ.empty(), "the fields after the reset's tick are a fresh instance's after the same tick, "
                                       "but fd_" +
                                           (differ.empty() ? std::string() : "; these differ:" + differ));
            }
            // the fade (fd_ after each tick, the reset's the first): monotonic, exactly 1 within 960 samples (+-1)
            {
                bool monotonic = true;
                std::size_t settled = 0;
                for (std::size_t i = reset; i < fade.size(); ++i)
                {
                    monotonic = monotonic && (i == reset || fade[i] >= fade[i - 1]);
                    if (settled == 0 && fade[i] == 1.0f)
                        settled = i - reset + 1;
                }
                r.line(monotonic && settled >= 959 && settled <= 961 && fade[reset] < 1.0f,
                       std::format("fd_ rises monotonically from {:g} after the reset's tick to exactly 1 after {} "
                                   "ticks (960 +- 1 at 48 kHz)",
                                   fade[reset], settled));
            }
            for (int call : {61, 64})
            {
                Instance self(*c->module);
                const std::vector<float> parts = run(call, nullptr, self);
                std::size_t differ = 0;
                for (std::size_t i = 0; i < n; ++i)
                    differ += std::memcmp(&parts[i], &one[i], sizeof(float)) != 0 ? 1 : 0;
                r.line(differ == 0,
                       std::format("calls of {}: {} of {} samples differ from calls of 1", call, differ, n));
            }
            // .channels 2: a NaN in one lane resets both
            {
                Instance self(*stereo->module);
                stereo->init(self.get(), 48000);
                const std::vector<float> b(in.begin(), in.end());
                std::vector<float> ya(n);
                std::vector<float> yb(n);
                const std::size_t x2 = offsetOf(*stereo, "x_C1");
                stereo->ticks2(self.get(), in.data(), b.data(), nullptr, pots.data(), nullptr, ya.data(), yb.data(),
                               static_cast<int>(poisoned), 0);
                const float nan = std::numeric_limits<float>::quiet_NaN();
                std::memcpy(static_cast<char*>(self.get()) + x2 + sizeof(float), &nan, sizeof nan);
                stereo->ticks2(self.get(), in.data() + poisoned, b.data() + poisoned, nullptr, pots.data() + poisoned,
                               nullptr, ya.data() + poisoned, yb.data() + poisoned, static_cast<int>(n - poisoned),
                               static_cast<int>(poisoned));
                bool same = true;
                for (std::size_t i = reset; i < n; ++i)
                    same = same && ya[i] == one[i] && yb[i] == one[i];
                r.line(same, "float2: a NaN in x_C1's second lane resets both lanes at the same tick: from it, each "
                             "lane is the scalar module's after its reset, bit for bit");
            }
        }

        // ---- l. the provisional configuration ----
        void provisional(Report& r)
        {
            std::printf("l. the provisional configuration: diodetone.cir without its configuration directives\n");
            const fc::Result result = fc::generate(opened(netlist("diodetone.cir")), "diodetone.cir");
            const bool note = result.notes.find("diodetone.cir: note: the self-test chooses this circuit's "
                                                "configuration; provisional: 8x IIR, omega(2)\n") != std::string::npos;
            r.line(result.errors.empty() && result.provisional && note && result.selfTestKey.size() == 16,
                   "Result::provisional, its key, and the note 'the self-test chooses this circuit's configuration; "
                   "provisional: 8x IIR, omega(2)'");
            auto c = buildOrReport(r, opened(netlist("diodetone.cir")), "diodetone.cir");
            if (!c)
                return;
            const fc::Config& cfg = c->plan.config;
            std::string bad;
            for (const char* method : {"init", "set", "tick", "step", "clear"})
            {
                const std::string full = c->result.module + "::" + method;
                if (!c->program->nonBlocking(full))
                    bad += " " + full;
            }
            r.line(cfg.oversample == 8 && cfg.halfband == fc::Halfband::Iir && !cfg.adaa &&
                       cfg.solver.guess == fc::Solver::Guess::Omega && cfg.solver.steps == 2 && bad.empty(),
                   "its module is 8x IIR, plain omega(2), and its methods are nonblocking" +
                       (bad.empty() ? std::string() : "; not:" + bad));
            // an OTA's (the SM row's): 2x, the previous v + 2 Newton steps
            const fc::Result ota = fc::generate(opened(netlist("otalp.cir")), "otalp.cir");
            r.line(ota.errors.empty() && ota.provisional &&
                       ota.notes.find("otalp.cir: note: the self-test chooses this circuit's configuration; "
                                      "provisional: 2x IIR, newton(2)\n") != std::string::npos,
                   "otalp.cir without its directives: provisional, 2x IIR, newton(2) (the SM row's)");
        }
        // ---- m. the synth path: otalp.cir ----
        std::vector<float> floats(const std::vector<double>& v)
        {
            return {v.begin(), v.end()};
        }

        // The module's tick over u at `rate`, a law value a sample, in calls of `call` samples (0: one call).
        std::vector<double> ticksAt(const Circuit& c, double rate, std::span<const double> u,
                                    std::span<const float> law, int call = 0)
        {
            Instance self(*c.module);
            c.init(self.get(), static_cast<float>(rate));
            const auto n = static_cast<int>(u.size());
            const std::vector<float> in(u.begin(), u.end());
            std::vector<float> out(u.size());
            const auto l = static_cast<std::size_t>(c.laws());
            const int size = call <= 0 ? n : call;
            for (int start = 0; start < n; start += size)
            {
                const auto s = static_cast<std::size_t>(start);
                c.ticks(self.get(), in.data() + s, law.data() + s * l, nullptr, nullptr, out.data() + s,
                        std::min(size, n - start), start);
            }
            return {out.begin(), out.end()};
        }

        // T1-T4 at the netlist's configuration, the self-test's way (its stress matrix, a synth's: the law input at
        // its three points, the saw and the modulation; its converged references; measureUnit and reduce), through
        // the JIT here.
        Figures synthGates(Report& r, const std::string& name)
        {
            auto c = buildOrReport(r, netlist(name), name);
            if (!c)
                return {0, 0, 0};
            const fc::Plan& p = c->plan;
            const int m = p.config.oversample;
            const fc::StressMatrix mx = fc::stressMatrix(p);
            std::printf("m. %s as shipped (%s), L = %g V; %s at %zu points; the synth's stress matrix (%d units)\n",
                        name.c_str(), configText(p.config).c_str(), mx.level, p.laws.front().name.c_str(),
                        mx.points.size(), mx.units());
            std::vector<fc::UnitVerdict> units(static_cast<std::size_t>(mx.units()));
            long unconverged = 0;
            const std::size_t host = mx.shape.host();
            for (int u = 0; u < mx.units(); ++u)
            {
                if (!mx.runs(u))
                    continue;
                const fc::StressSignal& s = mx.signal(u);
                std::optional<fc::ReferenceT1> t1;
                std::optional<fc::Reference64> t64;
                if (s.t1)
                {
                    t1 = fc::referenceT1(p, mx, u, m, p.config.adaa, fc::Method::Tr, {});
                    unconverged += t1->unconverged;
                }
                if (s.t2)
                {
                    t64 = fc::reference64(p, mx, u, {});
                    unconverged += t64->unconverged;
                }
                const std::vector<double> steps = runSteps(*c, fc::regionInput(p, mx, u, m, {}), {},
                                                           floats(fc::lawArguments(p, mx, u, m, m * host)));
                const std::vector<double> ticks = runTicks(*c, fc::signalSamples(mx, s, 1, host), {}, 0,
                                                           floats(fc::lawArguments(p, mx, u, 1, host)));
                units[static_cast<std::size_t>(u)] =
                    fc::measureUnit(mx, u, m, steps, ticks, t1 ? &*t1 : nullptr, t64 ? &*t64 : nullptr);
            }
            const fc::Verdict v = fc::reduce(mx, units, unconverged);
            r.line(v.t1.value <= kGate, std::format("T1 solver  {:6.1f} dB at {} (gate {})", v.t1.value, v.t1.where,
                                                     kGate));
            r.line(v.t2.value <= kGate, std::format("T2 alias   {:6.1f} dB at {}, in excess of the 64x reference's "
                                                     "(gate {})",
                                                     v.t2.value, v.t2.where, kGate));
            r.line(v.t3.value <= kResponse, std::format("T3 H1, H3 {:.3f} dB at {}, against 64x (gate {})", v.t3.value,
                                                        v.t3.where, kResponse));
            r.line(v.finite, v.finite ? std::string("T4 finite on every signal, 2.2 L, the saw and the modulation too")
                                      : "T4 not finite at " + v.notFinite);
            r.line(unconverged == 0, std::format("the references: {} unconverged samples", unconverged));
            return {v.t1.value, v.t2.value, v.t3.value};
        }

        // An amplitude at f: the least-squares fit of a sin + b cos + c over y (exact for a sinusoid at f whatever
        // the window's length).
        double amplitudeAt(std::span<const double> y, double f, double rate, std::size_t first)
        {
            double s[3][4] = {};
            for (std::size_t i = first; i < y.size(); ++i)
            {
                const double w = 2 * std::numbers::pi * f * static_cast<double>(i) / rate;
                const double b[3] = {std::sin(w), std::cos(w), 1};
                for (int j = 0; j < 3; ++j)
                {
                    for (int k = 0; k < 3; ++k)
                        s[j][k] += b[j] * b[k];
                    s[j][3] += b[j] * y[i];
                }
            }
            for (int k = 0; k < 3; ++k)
            {
                for (int j = 0; j < 3; ++j)
                {
                    if (j == k)
                        continue;
                    const double f2 = s[j][k] / s[k][k];
                    for (int c = k; c < 4; ++c)
                        s[j][c] -= f2 * s[k][c];
                }
            }
            const double a = s[0][3] / s[0][0];
            const double b = s[1][3] / s[1][1];
            return std::hypot(a, b);
        }

        // The -3 dB frequency of a one-pole OTA module at `rate` with its gain held at g, at a small signal (10 mV):
        // its DC gain from a step, then a bisection on f.
        double minus3dB(const Circuit& c, double rate, double g, double guess)
        {
            const std::size_t n = 24000;
            const std::vector<float> law(n, static_cast<float>(g));
            const std::vector<double> dcIn(n, 0.01);
            const double dc = ticksAt(c, rate, dcIn, law).back() / 0.01;
            auto gain = [&](double f)
            {
                std::vector<double> u(n);
                for (std::size_t i = 0; i < n; ++i)
                    u[i] = 0.01 * std::sin(2 * std::numbers::pi * f * static_cast<double>(i) / rate);
                return amplitudeAt(ticksAt(c, rate, u, law), f, rate, 4000) / 0.01 / dc;
            };
            double lo = guess / 3;
            double hi = std::min(guess * 3, 0.45 * rate);
            for (int k = 0; k < 40; ++k)
            {
                const double mid = std::sqrt(lo * hi);
                (gain(mid) > std::sqrt(0.5) ? lo : hi) = mid;
            }
            return std::sqrt(lo * hi);
        }

        void synth(Report& r)
        {
            const Figures shipped = synthGates(r, "otalp.cir");

            // the self-test, without the netlist's configuration lines, chooses what it ships
            const std::optional<fc::SelfTestResult> amps =
                search(r, "otalp.cir", ".oversample 4, .adaa off, .solver newton(1)", shipped);

            // and with iabc's argument in mA (scale=1m), the same range in amps: the self-test feeds the module its
            // law samples over their scale, so it measures the same circuit and chooses as it does in amps (fed the
            // amps raw, the module ran at a thousandth of its references' gains: no candidate met, T3 65.65 dB)
            if (amps)
            {
                const std::string milliamps =
                    opened(withDirective(netlist("otalp.cir"), ".input  iabc",
                                         ".input  iabc -    kind=current scale=1m range=10u:2m"));
                fc::SelfTestOptions test;
                test.parallel = pool(8);
                const fc::SelfTestResult t = fc::selfTest(milliamps, "otalp.cir", {}, test);
                const fc::Choice& a = *amps->choice;
                if (!t.choice)
                    r.line(false, "iabc in mA (scale=1m): the self-test gave no answer:\n" + t.errors + t.notes);
                else
                    r.line(t.choice->met && directivesOf(*t.choice) == directivesOf(a) &&
                               t.candidates == amps->candidates && std::fabs(t.choice->t1 - a.t1) <= 3 &&
                               std::fabs(t.choice->t2 - a.t2) <= 3 && std::fabs(t.choice->t3 - a.t3) <= 0.01,
                           std::format("iabc in mA (scale=1m): the self-test chose {}, candidate {} ({}); T1 {:.1f}, "
                                       "T2 {:.1f}, T3 {:.3f} dB (in amps {:.1f}, {:.1f}, {:.3f}; T1 and T2 within 3 "
                                       "dB, float's floor, T3 0.01)",
                                       directivesOf(*t.choice), t.candidates, t.choice->met ? "met" : "not met",
                                       t.choice->t1, t.choice->t2, t.choice->t3, a.t1, a.t2, a.t3));
            }

            // tick against step through the 4x IIR oversampler, iabc held at its middle
            {
                auto c = buildOrReport(r, netlist("otalp.cir"), "otalp.cir");
                if (!c)
                    return;
                const int m = c->plan.config.oversample;
                const double mid = c->plan.laws.front().middle();
                const std::vector<double> stepped = fc::decimateWindow(
                    runSteps(*c, fc::sine(1.11, 85, kWindow, m, m * kHost), {}, heldLaws({&mid, 1}, m * kHost)), m,
                    kWarm, kWindow);
                const std::vector<double> ticks =
                    runTicks(*c, fc::sine(1.11, 85, kWindow, 1, kHost), {}, 0, heldLaws({&mid, 1}, kHost));
                const double gain = dB(fc::binMagnitude(windowOf(ticks), 85) / fc::binMagnitude(stepped, 85));
                const double rest = fc::inharmonicDb(windowOf(ticks), 85);
                r.line(
                    std::fabs(gain) <= 0.01 && rest <= -85,
                    std::format("tick against step at 4x IIR, 1.11 V, 996 Hz, iabc {:.3g}: the fundamental {:+.4f} dB "
                                "against the steps ideally decimated, the rest {:.1f} dB",
                                mid, gain, rest));
            }
        }

        // The prewarp: one OTA stage at 1x, its -3 dB point at 1, 4 and 10 kHz cutoffs (the gain g with a + b g = -2
        // pi fc), at 44.1 and 48 kHz, at a small signal: with .prewarp on within 0.5 % of the analog's; off (the
        // default), the trapezoidal rule's warp, printed.
        void prewarp(Report& r)
        {
            std::printf("m. the prewarp: one OTA stage (otalp.cir's first) at 1x, its -3 dB point at a 10 mV signal\n");
            const std::string stage = ".title Stage\n.input u in\n.input iabc - kind=current range=10u:2m\n"
                                      ".output y yb\n.oversample 1\n.adaa off\n.solver newton(2)\n"
                                      "R1 in a1 100k\nR2 a1 0 220\nR3 yb a2 100k\nR4 a2 0 220\n"
                                      "G1 0 y OTA a1 a2 {iabc}\nC1 y 0 1n\nR5 y 0 10meg\nX1 y yb yb opamp\n";
            fc::Options on;
            on.prewarp = true;
            auto warped = buildOrReport(r, stage, "stage.cir", on);
            auto plain = buildOrReport(r, stage, "stage.cir");
            if (!warped || !plain)
                return;
            const fc::PortDef& port = *warped->plan.blocks.front().port;
            double worst = 0;
            std::string where;
            std::string off;
            for (double rate : {44100.0, 48000.0})
            {
                for (double fc0 : {1000.0, 4000.0, 10000.0})
                {
                    const double g = (-2 * std::numbers::pi * fc0 - port.poleA) / port.poleB;
                    const double f = minus3dB(*warped, rate, g, fc0);
                    const double error = std::fabs(f / fc0 - 1);
                    if (error > worst)
                    {
                        worst = error;
                        where = std::format("{:g} kHz at {:g} kHz", fc0 / 1000, rate / 1000);
                    }
                    off += std::format("{}{:g} kHz at {:g}: {:.1f} Hz ({:+.2f} %)", off.empty() ? "" : "; ", fc0 / 1000,
                                       rate / 1000, minus3dB(*plain, rate, g, fc0),
                                       100 * (minus3dB(*plain, rate, g, fc0) / fc0 - 1));
                }
            }
            r.line(worst <= 0.005, std::format(".prewarp on: the -3 dB point within {:.4f} % of the analog's at worst "
                                               "({}; gate 0.5 %)",
                                               100 * worst, where));
            Report::note(".prewarp off (the default), the trapezoidal rule's warp: " + off);
            r.line(plain->plan.config.prewarpFrom == "default" && !plain->plan.config.prewarp,
                   "off is the default: the cutoff law is the circuit's");
        }

        // The carry under ADAA. (1) What it does: one OTA stage driven into its law's bend (10k / 1k attenuators, so a
        // 0.5 V, 1 kHz sine puts tanh(v / 2 Vt) near 0.7), 10 nF, at 1x with ADAA and newton(2), iabc stepping from 1
        // uA to 200 uA in one sample, at the sine's crest, and back. Its tick follows the double reference run with
        // the same steps, ADAA and gain stream (Reference.cpp's carry the emitted one) to 2e-5 of the signal (float's
        // rounding through the step, and in the slow pole after the step back); the same reference without the carry
        // departs from it by more than 1e-3 of the signal (by 18 times it: the mean of the step pairs points on two
        // curves 200x apart), so a module without the carry, or with a wrong one, fails (one without: 0.18). (2) otalp.cir with ADAA (4x, newton(1)), a 5 V, 996 Hz sine, iabc a triangle +-2 octaves
        // around its middle at 37 Hz, against the plain newton(3) module over 512 ramps: within 1.1x of the departure
        // with iabc held at the triangle's ends (a bound on the output, which the carry, a few uV here, barely moves).
        void carry(Report& r)
        {
            std::printf("m. the carry under ADAA: an OTA stage in its law's bend, iabc stepping 1 uA to 200 uA and "
                        "back, against the double reference; otalp.cir, iabc a triangle +-2 octaves at 37 Hz\n");
            // (1) a gain step, through the JIT, against the reference with and without the carry
            {
                const std::string stage = ".title Bend\n.input u in\n.input iabc - kind=current range=1u:200u\n"
                                          ".output y yb\n.oversample 1\n.adaa on\n.solver newton(2)\n"
                                          "R1 in a1 10k\nR2 a1 0 1k\nR3 yb a2 10k\nR4 a2 0 1k\n"
                                          "G1 0 y OTA a1 a2 {iabc}\nC1 y 0 10n\nR5 y 0 10meg\nX1 y yb yb opamp\n";
                auto c = buildOrReport(r, stage, "bend.cir");
                if (!c)
                    return;
                constexpr std::size_t n = 4800;
                constexpr std::size_t up = 2412;     // 1 uA -> 200 uA, at the sine's crest
                constexpr std::size_t down = 3612;   // and back
                std::vector<double> u(n);
                std::vector<float> law(n);
                std::vector<double> gains(n);
                for (std::size_t i = 0; i < n; ++i)
                {
                    const double w = 2 * std::numbers::pi * 1000 * static_cast<double>(i) / 48000;
                    u[i] = static_cast<double>(static_cast<float>(0.5 * std::sin(w)));
                    law[i] = i >= up && i < down ? 200e-6f : 1e-6f;
                    gains[i] = static_cast<double>(law[i]);
                }
                // the module, and tanh(v / 2 Vt) at the step
                Instance self(*c->module);
                c->init(self.get(), 48000);
                std::vector<double> y(n);
                double bend = 0;
                std::size_t tFieldOffset = 0;
                for (const fminus::Program::Module::Field& f : c->module->fields)
                {
                    if (f.name == "t_G1")
                        tFieldOffset = f.offset;
                }
                for (std::size_t i = 0; i < n; ++i)
                {
                    const auto x = static_cast<float>(u[i]);
                    float out = 0;
                    c->ticks(self.get(), &x, &law[i], nullptr, nullptr, &out, 1, static_cast<int>(i));
                    y[i] = static_cast<double>(out);
                    if (i + 1 == up || i + 1 == down)
                    {
                        float t = 0;
                        std::memcpy(&t, static_cast<const char*>(self.get()) + tFieldOffset, sizeof t);
                        bend = std::max(bend, std::fabs(static_cast<double>(t)));
                    }
                }
                const std::vector<fc::Holters> h = chainAt(c->plan, {}, 48000, true, {});
                fc::RefOptions ro;
                ro.adaa = true;
                ro.steps = 2;
                ro.guess = fc::Solver::Guess::Previous;
                const std::vector<double> carried = fc::simulate(h.front(), u, ro, nullptr, gains);
                ro.carry = false;
                const std::vector<double> held = fc::simulate(h.front(), u, ro, nullptr, gains);
                double follows = 0;
                double departs = 0;
                for (std::size_t i = 0; i < n; ++i)
                {
                    follows = std::max(follows, std::fabs(y[i] - carried[i]) / 0.5);
                    departs = std::max(departs, std::fabs(held[i] - carried[i]) / 0.5);
                }
                r.line(follows <= 2e-5 && departs >= 1e-3 && bend >= 0.5,
                       std::format("a gain step (tanh at the step {:.2f}): the module follows the reference to {:.1e} "
                                   "of the signal (gate 2e-5); without the carry the reference departs by {:.1e} "
                                   "(gate: at least 1e-3)",
                                   bend, follows, departs));
            }
            fc::Options adaaOptions;
            adaaOptions.adaa = true;
            auto adaa = buildOrReport(r, netlist("otalp.cir"), "otalp.cir", adaaOptions);
            fc::Options plainOptions;
            plainOptions.solver = fc::Solver{fc::Solver::Guess::Previous, 3};
            auto plain = buildOrReport(r, netlist("otalp.cir"), "otalp.cir", plainOptions);
            if (!adaa || !plain)
                return;
            const double mid = adaa->plan.laws.front().middle();
            const double period = 48000.0 / 37;
            auto triangle = [&](std::size_t i)
            {
                const double x = std::fmod(static_cast<double>(i) / period, 1.0);
                const double t = x < 0.5 ? 4 * x - 1 : 3 - 4 * x;   // -1 to 1 and back
                return mid * std::exp2(2 * t);
            };
            // (2) end to end
            constexpr std::size_t warm = 4800;
            const std::size_t ramp = static_cast<std::size_t>(period / 2);
            auto departure = [&](std::size_t ramps, auto law)
            {
                const std::size_t n = warm + ramps * ramp;
                const std::vector<double> u = fc::sine(5, 85, kWindow, 1, n);
                std::vector<float> laws(n);
                for (std::size_t i = 0; i < n; ++i)
                    laws[i] = static_cast<float>(law(i));
                const std::vector<double> a = runTicks(*adaa, u, {}, 0, laws);
                const std::vector<double> b = runTicks(*plain, u, {}, 0, laws);
                double worst = 0;
                for (std::size_t i = warm; i < n; ++i)
                    worst = std::max(worst, std::fabs(a[i] - b[i]));
                return worst;
            };
            const double held = std::max(departure(64, [&](std::size_t) { return mid / 4; }),
                                         departure(64, [&](std::size_t) { return mid * 4; }));
            const double moving = departure(512, triangle);
            r.line(moving <= 1.1 * held,
                   std::format("against the plain newton(3) module over 512 ramps: {:.4f} V at worst, {:.4f} V with "
                               "iabc held: within 1.1x",
                               moving, held));
        }

        // ---- n. law inputs: lanes and block splits ----
        // otalp.cir with a per-sample iabc (its triangle, and NaN, 0 and 1 A, which the clamp takes to its range):
        // finite; .channels 2's lanes are the scalar module's, bit for bit (iabc broadcast to both); calls of 64 and 61
        // samples are one call's, bit for bit.
        void synthLanes(Report& r)
        {
            std::printf("n. law inputs: otalp.cir with a per-sample iabc, its lanes and its block splits\n");
            auto c = buildOrReport(r, netlist("otalp.cir"), "otalp.cir");
            fc::Options two;
            two.channels = 2;
            auto pair = buildOrReport(r, netlist("otalp.cir"), "otalp.cir", two);
            if (!c || !pair)
                return;
            const double mid = c->plan.laws.front().middle();
            constexpr std::size_t n = 4800;
            std::vector<float> law(n);
            for (std::size_t i = 0; i < n; ++i)
                law[i] = static_cast<float>(mid * std::exp2(2 * std::sin(2 * std::numbers::pi * 37 * i / 48000.0)));
            // NaN, 0 and 1 A
            {
                bool finiteAll = true;
                for (float x : {std::numeric_limits<float>::quiet_NaN(), 0.0f, 1.0f})
                {
                    std::vector<float> bad = law;
                    for (std::size_t i = 1000; i < 1200; ++i)
                        bad[i] = x;
                    finiteAll = finiteAll && finite(runTicks(*c, fc::sine(11, 85, kWindow, 1, n), {}, 0, bad));
                }
                r.line(finiteAll, "iabc NaN, 0 and 1 A for 200 samples of an 11 V sine: the output is finite");
            }
            // the lanes
            {
                const std::vector<double> a = fc::sine(5, 85, kWindow, 1, n);
                std::vector<double> b = fc::noise(5, 1, 8192);
                b.resize(n);
                const std::vector<double> ya = runTicks(*c, a, {}, 0, law);
                const std::vector<double> yb = runTicks(*c, b, {}, 0, law);
                Instance self(*pair->module);
                pair->init(self.get(), 48000);
                const std::vector<float> fa = floats(a);
                const std::vector<float> fb = floats(b);
                std::vector<float> pa(n);
                std::vector<float> pb(n);
                pair->ticks2(self.get(), fa.data(), fb.data(), law.data(), nullptr, nullptr, pa.data(), pb.data(),
                             static_cast<int>(n), 0);
                std::size_t differ = 0;
                for (std::size_t i = 0; i < n; ++i)
                    differ += (static_cast<double>(pa[i]) != ya[i] ? 1 : 0) +
                              (static_cast<double>(pb[i]) != yb[i] ? 1 : 0);
                r.line(differ == 0, std::format(".channels 2: {} of {} samples differ from the scalar module's (a 5 V "
                                                "sine on one lane, 5 V noise on the other)",
                                                differ, 2 * n));
            }
            // the block splits
            {
                const std::vector<double> u = fc::sine(5, 85, kWindow, 1, n);
                const std::vector<double> whole = runTicks(*c, u, {}, 0, law);
                for (int call : {64, 61})
                {
                    const std::vector<double> parts = runTicks(*c, u, {}, call, law);
                    std::size_t differ = 0;
                    for (std::size_t i = 0; i < n; ++i)
                        differ += parts[i] != whole[i] ? 1 : 0;
                    r.line(differ == 0, std::format("calls of {}: {} of {} samples differ from one call", call, differ,
                                                    n));
                }
            }
        }
    } // namespace

    int check()
    {
        Report r;
        generation(r);
        dc(r);
        const Figures dt =
            gates(r, "as shipped", netlist("diodetone.cir"), "diodetone.cir", "tone", {-93.7, -79.8, -0.08});
        const Figures ts =
            gates(r, "as shipped", netlist("screamer-clip.cir"), "screamer-clip.cir", "drive", {-81.2, -75.1, -0.06});
        // Phase 0's defaults, now the provisional configuration: the netlists without their configuration lines
        auto defaults = [](std::string text)
        {
            for (const char* d : {".oversample", ".adaa", ".solver"})
                text = withDirective(text, d, "");
            return text;
        };
        gates(r, "provisional (Phase 0's defaults)", defaults(netlist("diodetone.cir")), "diodetone.cir", "tone",
              {-97.0, -113.9, -0.02});
        gates(r, "provisional (Phase 0's defaults)", defaults(netlist("screamer-clip.cir")), "screamer-clip.cir",
              "drive", {-94.4, -72.9, -0.00});
        chain(r);
        decision(r);
        halfbands(r);
        stereo(r);
        blocks(r);
        glide(r);
        selfTests(r, dt, ts);
        guard(r);
        provisional(r);
        synth(r);
        prewarp(r);
        carry(r);
        synthLanes(r);
        std::printf("%s: %d check(s) failed\n", r.failures == 0 ? "passed" : "FAILED", r.failures);
        return r.failures == 0 ? 0 : 1;
    }
} // namespace circuits
