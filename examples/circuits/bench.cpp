// examples/circuits/bench.cpp: example-circuits with no flags. First what generate() takes on each shipped netlist,
// and the self-test's wall time on diodetone.cir and screamer-clip.cir without their configuration directives, on 1,
// 2, 4 and 8 threads. Then the report's cost table (§3.2, §3.3, §4.1) from emitted code, in fmbench.cpp's method: each
// row is a module generate() writes, with mkbench.py's bench function appended (n host samples of a 1 kHz sine through
// tick, the squares summed), compiled with subnormals flushed, one thread and the row's math mode, and the best of
// `reps` runs is kept: wall time, and on macOS this process's cycles and instructions (proc_pid_rusage) at the
// user-interactive quality of service. A row is named as mkbench.py names its bench; the self-test's cost model
// (src/circuit/Search.h, report §3.2) and the report's measurement of that bench (experiments/timing1.out) are printed
// beside it. Before it, set()'s cycles and the probes' cost in a host's process loop (--only probes: that alone).
// Printed, never asserted: run it on a quiet machine. (--only skips the first two.)
#include "harness.h"

#include "circuit/Emit.h"
#include "circuit/Search.h"

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <format>
#include <fstream>
#include <map>
#include <regex>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

#if defined(__APPLE__)
#include <libproc.h>
#include <pthread.h>
#include <sys/qos.h>
#include <sys/resource.h>
#include <unistd.h>
#endif
#if !defined(_WIN32)
#include <stdlib.h>
#endif

namespace circuits
{
    namespace
    {
        struct Row
        {
            std::string circuit;   // "DT" (diodetone.cir), "TS" (screamer-clip.cir), "TSF" (screamer.cir, the chain),
                                   // "OTA" (otalp.cir, iabc moving each sample)
            fc::Solver solver;
            bool adaa = false;
            int m = 4;
            fc::Halfband kind = fc::Halfband::Iir;
            int channels = 1;
            fc::Region region = fc::Region::Auto;
            fminus::Program::Math math = fminus::Program::Math::Vector;
            std::string note;   // what the report says of it

            // mkbench.py's name for this bench, and timing1.out's section.
            std::string name() const
            {
                const std::string g = std::format("{}{}{}", solver.guess == fc::Solver::Guess::Omega ? "w" : "p",
                                                  solver.steps, adaa ? "_adaa" : "");
                const std::string k = kind == fc::Halfband::Iir ? "iir" : "fir";
                if (circuit == "OTA")
                    return std::format("b_OTA_{}_{}x_{}{}", g, m, k, prewarp ? "_prewarp" : "");
                if (circuit == "TSF")
                    return std::format("f_TS_{}_{}x_{}_{}", g, m, k,
                                       region == fc::Region::All         ? "all"
                                       : region == fc::Region::Nonlinear ? "part"
                                                                         : "auto");
                return std::format("b_{}_{}_{}x_{}", circuit, g, m, k);
            }

            bool prewarp = false;   // OTA: .prewarp on

            std::string section() const
            {
                const std::string mode = math == fminus::Program::Math::Fast ? "fast" : "vector";
                if (circuit == "OTA")
                    return "ota " + mode;
                if (circuit == "TSF")
                    return "tsfull " + mode;
                if (circuit == "TS")
                    return "ts " + mode;
                return (channels == 2 ? "dt2 " : "dt ") + mode;
            }
        };

        // timing1.out: (section, bench) -> cycles.
        std::map<std::pair<std::string, std::string>, double> reported()
        {
            std::map<std::pair<std::string, std::string>, double> out;
            std::ifstream file(std::string(FMINUS_EXAMPLE_DIR) +
                               "/../../docs/research/analog-circuits/experiments/timing1.out");
            std::string line;
            std::string section;
            const std::regex row(R"(^(\S+)\s+[0-9.]+ ns\s+([0-9.]+) cycles)");
            while (std::getline(file, line))
            {
                if (line.rfind("== ", 0) == 0)
                    section = line.substr(3);
                std::smatch m;
                if (std::regex_search(line, m, row))
                    out[{section, m[1].str()}] = std::stod(m[2].str());
            }
            return out;
        }

        std::vector<Row> defaultRows()
        {
            using G = fc::Solver::Guess;
            const auto fast = fminus::Program::Math::Fast;
            std::vector<Row> rows;
            auto add = [&](Row r)
            {
                rows.push_back(r);
            };
            // §3.2: one step a host sample (1x)
            for (const char* c : {"DT", "TS"})
            {
                add({c, {G::Omega, 1}, false, 1, fc::Halfband::Iir, 1, fc::Region::Auto, {}, "§3.2: omega + 1"});
                add({c, {G::Omega, 2}, false, 1, fc::Halfband::Iir, 1, fc::Region::Auto, {}, "§3.2: omega + 2"});
                add({c, {G::Previous, 4}, false, 1, fc::Halfband::Fir, 1, fc::Region::Auto, {}, "§3.2: prev + 4"});
                add({c, {G::Omega, 1}, true, 1, fc::Halfband::Iir, 1, fc::Region::Auto, {}, "§3.2: ADAA omega + 1"});
            }
            add({"DT", {G::Previous, 2}, false, 1, fc::Halfband::Fir, 1, fc::Region::Auto, {}, "§3.2: prev + 2"});
            // §3.3, example 1
            add({"DT",
                 {G::Omega, 2},
                 false,
                 4,
                 fc::Halfband::Fir,
                 1,
                 fc::Region::Auto,
                 {},
                 "§3.3: 912 (as shipped, but FIR)"});
            add({"DT", {G::Omega, 2}, false, 4, fc::Halfband::Iir, 1, fc::Region::Auto, {}, "as shipped"});
            add({"DT", {G::Omega, 1}, true, 4, fc::Halfband::Fir, 1, fc::Region::Auto, {}, "§3.3: 866"});
            add({"DT", {G::Omega, 2}, false, 4, fc::Halfband::Fir, 2, fc::Region::Auto, {}, "§3.3: float2, 965"});
            add({"DT", {G::Omega, 1}, true, 4, fc::Halfband::Fir, 2, fc::Region::Auto, {}, "§3.3: float2, 957"});
            // §3.3, the TS chain
            add({"TSF", {G::Omega, 1}, true, 4, fc::Halfband::Iir, 1, fc::Region::Nonlinear, {}, "§3.3: 907"});
            add({"TSF", {G::Omega, 1}, true, 4, fc::Halfband::Iir, 1, fc::Region::All, {}, "§3.3: 905-918"});
            add({"TSF",
                 {G::Omega, 1},
                 true,
                 4,
                 fc::Halfband::Iir,
                 1,
                 fc::Region::Auto,
                 {},
                 "blocks=auto as shipped: the tone block matched at 1x"});
            // §4.1
            add({"TS", {G::Omega, 2}, false, 8, fc::Halfband::Fir, 1, fc::Region::Auto, {}, "§4.1 row 1: 1,861"});
            add({"TS", {G::Omega, 1}, true, 4, fc::Halfband::Fir, 1, fc::Region::Auto, {}, "§4.1 row 1: 903"});
            add({"TS", {G::Omega, 1}, true, 4, fc::Halfband::Iir, 1, fc::Region::Auto, {}, "as shipped"});
            add({"TS", {G::Omega, 2}, false, 8, fc::Halfband::Iir, 1, fc::Region::Auto, {}, "Phase 0's defaults"});
            add({"TS", {G::Previous, 6}, false, 8, fc::Halfband::Fir, 1, fc::Region::Auto, {}, "§4.1 row 2: 3,551"});
            add({"DT", {G::Previous, 4}, false, 4, fc::Halfband::Fir, 1, fc::Region::Auto, {}, "§4.1 row 2: 1,257"});
            add({"DT", {G::Omega, 1}, true, 2, fc::Halfband::Iir, 2, fc::Region::Auto, {}, "§4.1 row 3: 452-479"});
            add({"DT", {G::Omega, 1}, true, 2, fc::Halfband::Fir, 2, fc::Region::Auto, {}, "§4.1 row 3: 452-479"});
            add({"DT", {G::Omega, 2}, false, 4, fc::Halfband::Fir, 1, fc::Region::Auto, fast, "§4.1 row 4: 808"});
            add({"TS", {G::Omega, 2}, false, 8, fc::Halfband::Fir, 1, fc::Region::Auto, fast, "§4.1 row 4: 1,660"});
            add({"TS", {G::Omega, 1}, true, 4, fc::Halfband::Fir, 1, fc::Region::Auto, fast, "§4.1 row 4: 796"});
            for (int m : {4, 8})
            {
                add({"DT", {G::Omega, 1}, false, m, fc::Halfband::Fir, 1, fc::Region::Auto, {}, "§4.1 row 5"});
                add({"DT", {G::Omega, 1}, false, m, fc::Halfband::Iir, 1, fc::Region::Auto, {}, "§4.1 row 5"});
            }
            add({"TSF", {G::Omega, 2}, false, 8, fc::Halfband::Fir, 1, fc::Region::Nonlinear, {}, "§4.1 row 6: 1,867"});
            add({"TSF", {G::Omega, 2}, false, 8, fc::Halfband::Fir, 1, fc::Region::All, {}, "§4.1 row 6: 1,865"});
            add({"DT", {G::Omega, 1}, false, 4, fc::Halfband::Iir, 1, fc::Region::Auto, {}, "§4.1 row 7: omega + 1"});
            add({"DT", {G::Omega, 2}, false, 4, fc::Halfband::Iir, 1, fc::Region::Auto, {}, "§4.1 row 7: omega + 2"});
            // the synth path: otalp.cir's two OTA one-poles, iabc moving each sample (an expf, the expo converter);
            // report §7.4 measured its 2-port SVF, 248 cycles a voice at 1x with 2 steps, not comparable
            add({"OTA", {G::Previous, 2}, false, 1, fc::Halfband::Iir, 1, fc::Region::Auto, {}, "1x, newton(2)"});
            add({"OTA", {G::Previous, 2}, false, 2, fc::Halfband::Iir, 1, fc::Region::Auto, {}, "the provisional"});
            add({"OTA", {G::Previous, 1}, false, 4, fc::Halfband::Iir, 1, fc::Region::Auto, {}, "as shipped"});
            add({"OTA", {G::Previous, 1}, true, 4, fc::Halfband::Iir, 1, fc::Region::Auto, {}, "ADAA"});
            add({"OTA", {G::Previous, 2}, false, 1, fc::Halfband::Iir, 2, fc::Region::Auto, {}, "float2"});
            {
                Row warped{"OTA", {G::Previous, 2}, false, 1, fc::Halfband::Iir, 1, fc::Region::Auto, {},
                           "prewarp (one tanf a sample)"};
                warped.prewarp = true;
                add(warped);
            }
            return rows;
        }

        // mkbench.py's matrix (bench_dt, bench_ts, bench_tsfull, bench_dt2).
        std::vector<Row> allRows()
        {
            using G = fc::Solver::Guess;
            std::vector<Row> rows;
            struct V
            {
                G g;
                int k;
                bool adaa;
            };
            const V dt[] = {{G::Omega, 1, false},
                            {G::Omega, 2, false},
                            {G::Previous, 2, false},
                            {G::Previous, 4, false},
                            {G::Omega, 1, true}};
            const V ts[] = {{G::Omega, 1, false},
                            {G::Omega, 2, false},
                            {G::Previous, 4, false},
                            {G::Previous, 6, false},
                            {G::Omega, 1, true}};
            for (int channels : {1, 2})
            {
                for (const V& v : dt)
                {
                    for (int m : {1, 2, 4, 8})
                    {
                        for (fc::Halfband kind : {fc::Halfband::Fir, fc::Halfband::Iir})
                        {
                            if (kind == fc::Halfband::Iir && v.g == G::Previous)
                                continue;   // mkbench.py runs the previous-sample variants through FIRs only
                            rows.push_back({"DT", {v.g, v.k}, v.adaa, m, kind, channels, fc::Region::Auto, {}, {}});
                        }
                    }
                }
            }
            for (const V& v : ts)
            {
                for (int m : {1, 2, 4, 8})
                {
                    for (fc::Halfband kind : {fc::Halfband::Fir, fc::Halfband::Iir})
                        rows.push_back({"TS", {v.g, v.k}, v.adaa, m, kind, 1, fc::Region::Auto, {}, {}});
                }
            }
            for (const V& v : {V{G::Omega, 2, false}, V{G::Omega, 1, true}})
            {
                for (int m : v.adaa ? std::vector<int>{4, 2} : std::vector<int>{8, 4})
                {
                    for (fc::Halfband kind : {fc::Halfband::Fir, fc::Halfband::Iir})
                    {
                        for (fc::Region region : {fc::Region::Nonlinear, fc::Region::All})
                            rows.push_back({"TSF", {v.g, v.k}, v.adaa, m, kind, 1, region, {}, {}});
                    }
                }
            }
            return rows;
        }

        // mkbench.py's bench function, for a module named `module` (amp the sine's peak, pots set after init).
        std::string benchSource(const std::string& module, double amp, int channels, const std::string& setCall)
        {
            const std::string a = fc::literal(amp);
            if (channels == 2)
                return std::format(R"(
float fm_bench(float n)
{{
    {0} w;
    w.init(48000);
    {2}
    float sum = 0;
    float ph = 0;
    for (int i = 0; i < (int)n; ++i)
    {{
        const float2 x = (float2)({1} * sinf(ph), {1} * cosf(ph));
        ph += 0.130899694f;
        ph = ph > 6.28318531f ? ph - 6.28318531f : ph;
        const float2 y = w.tick(x);
        sum += y.x * y.x + y.y * y.y;
    }}
    return sum;
}}
float fm_baseline(float n)
{{
    float sum = 0;
    float ph = 0;
    for (int i = 0; i < (int)n; ++i)
    {{
        const float2 x = (float2)(4.5f * sinf(ph), 4.5f * cosf(ph));
        ph += 0.130899694f;
        ph = ph > 6.28318531f ? ph - 6.28318531f : ph;
        sum += x.x * x.x + x.y * x.y;
    }}
    return sum;
}}
)",
                                   module, a, setCall);
            return std::format(R"(
float fm_bench(float n)
{{
    {0} w;
    w.init(48000);
    {2}
    float sum = 0;
    float ph = 0;
    for (int i = 0; i < (int)n; ++i)
    {{
        const float x = {1} * sinf(ph);
        ph += 0.130899694f;
        ph = ph > 6.28318531f ? ph - 6.28318531f : ph;
        const float y = w.tick(x);
        sum += y * y;
    }}
    return sum;
}}
float fm_baseline(float n)
{{
    float sum = 0;
    float ph = 0;
    for (int i = 0; i < (int)n; ++i)
    {{
        const float x = 4.5f * sinf(ph);
        ph += 0.130899694f;
        ph = ph > 6.28318531f ? ph - 6.28318531f : ph;
        sum += x * x;
    }}
    return sum;
}}
)",
                               module, a, setCall);
        }

        // The OTA's bench: tick with a 1 kHz sine at amp and iabc from an expo converter, 2^(2 sin) around 141 uA at
        // 37 Hz (one expf a sample, as a synth's voice computes it).
        std::string otaBenchSource(const std::string& module, double amp, int channels)
        {
            const std::string a = fc::literal(amp);
            const std::string x =
                channels == 2 ? "(float2)(" + a + " * sinf(ph), " + a + " * cosf(ph))" : a + " * sinf(ph)";
            const std::string type = channels == 2 ? "float2" : "float";
            const std::string sq = channels == 2 ? "y.x * y.x + y.y * y.y" : "y * y";
            return std::format(R"(
float fm_bench(float n)
{{
    {0} w;
    w.init(48000);
    float sum = 0;
    float ph = 0;
    float lph = 0;
    for (int i = 0; i < (int)n; ++i)
    {{
        const {2} x = {1};
        ph += 0.130899694f;
        ph = ph > 6.28318531f ? ph - 6.28318531f : ph;
        lph += 0.00484346f;
        lph = lph > 6.28318531f ? lph - 6.28318531f : lph;
        const float iabc = 0.000141421f * expf(1.38629436f * sinf(lph));
        const {2} y = w.tick(x, iabc);
        sum += {3};
    }}
    return sum;
}}
float fm_baseline(float n)
{{
    float sum = 0;
    float ph = 0;
    for (int i = 0; i < (int)n; ++i)
    {{
        const float x = 4.5f * sinf(ph);
        ph += 0.130899694f;
        ph = ph > 6.28318531f ? ph - 6.28318531f : ph;
        sum += x * x;
    }}
    return sum;
}}
)",
                               module, x, type, sq);
        }

        struct Timing
        {
            double ns = 0;
            double cycles = 0;
            double instructions = 0;
        };

        void counters(std::uint64_t& cycles, std::uint64_t& instructions)
        {
#if defined(__APPLE__)
            rusage_info_v4 ri{};
            proc_pid_rusage(getpid(), RUSAGE_INFO_V4, reinterpret_cast<rusage_info_t*>(&ri));
            cycles = ri.ri_cycles;
            instructions = ri.ri_instructions;
#else
            cycles = 0;
            instructions = 0;
#endif
        }

        Timing best(float (*fn)(float), int samples, int reps)
        {
            volatile float sink = fn(1000);   // warm
            Timing t{1e30, 1e30, 1e30};
            const auto n = static_cast<double>(samples);
            for (int r = 0; r < reps; ++r)
            {
                std::uint64_t c0 = 0, i0 = 0, c1 = 0, i1 = 0;
                counters(c0, i0);
                const auto w0 = std::chrono::steady_clock::now();
                sink = fn(static_cast<float>(samples));
                const auto w1 = std::chrono::steady_clock::now();
                counters(c1, i1);
                t.ns = std::min(t.ns, std::chrono::duration<double, std::nano>(w1 - w0).count() / n);
                t.cycles = std::min(t.cycles, static_cast<double>(c1 - c0) / n);
                t.instructions = std::min(t.instructions, static_cast<double>(i1 - i0) / n);
            }
            (void)sink;
            return t;
        }
    } // namespace

    namespace
    {
        double millisecondsSince(std::chrono::steady_clock::time_point start)
        {
            return std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
        }

        // generate()'s time on each shipped netlist, and without its configuration directives: the best of 20.
        void generateTimes()
        {
            std::printf("generate(): ms a netlist, the best of 20\n");
            for (const char* name : {"diodetone.cir", "screamer.cir", "screamer-clip.cir", "clipper.cir", "otalp.cir"})
            {
                const std::string shipped = netlist(name);
                std::string open = shipped;
                for (const char* d : {".oversample", ".adaa", ".solver"})
                    open = withDirective(open, d, "");
                for (const std::string* text : {&shipped, static_cast<const std::string*>(&open)})
                {
                    double best = 1e30;
                    for (int k = 0; k < 20; ++k)
                    {
                        const auto t0 = std::chrono::steady_clock::now();
                        const fc::Result r = fc::generate(*text, name);
                        best = std::min(best, millisecondsSince(t0));
                        if (!r.errors.empty())
                            std::printf("%s", r.errors.c_str());
                    }
                    std::printf("  %-18s %-24s %7.2f ms\n", name, text == &open ? "(directives removed)" : "", best);
                }
            }
        }

        // The self-test's wall time on `threads` std::threads (SelfTestOptions::parallel).
        void selfTestTimes()
        {
            std::printf("selfTest(): wall seconds on 1, 2, 4 and 8 threads (the netlists without their configuration "
                        "directives; %u cores)\n",
                        std::thread::hardware_concurrency());
            for (const char* name : {"diodetone.cir", "screamer-clip.cir", "otalp.cir"})
            {
                std::string text = netlist(name);
                for (const char* d : {".oversample", ".adaa", ".solver"})
                    text = withDirective(text, d, "");
                std::printf("  %-18s", name);
                for (int threads : {1, 2, 4, 8})
                {
                    fc::SelfTestOptions test;
                    test.parallel = [threads](int count, const std::function<void(int)>& job)
                    {
                        std::atomic<int> next{0};
                        auto work = [&]
                        {
                            for (int i = next++; i < count; i = next++)
                                job(i);
                        };
                        std::vector<std::thread> pool;
                        for (int k = 1; k < threads; ++k)
                            pool.emplace_back(work);
                        work();
                        for (std::thread& t : pool)
                            t.join();
                    };
                    const auto t0 = std::chrono::steady_clock::now();
                    const fc::SelfTestResult r = fc::selfTest(text, name, {}, test);
                    std::printf(" %d: %6.2f s%s", threads, millisecondsSince(t0) / 1000,
                                r.choice ? "" : " (no answer)");
                    std::fflush(stdout);
                }
                std::printf("\n");
            }
        }
    } // namespace

    namespace
    {
        // b3_set's cycles a call on screamer.cir's tone block, its pot moving every call: in the Woodbury form (the
        // rate-only part in init) and all in set, both derived by deriveBlock and emitted whole.
        void setCycles(const BenchOptions& options)
        {
            std::printf("set() cycles: screamer.cir's b3_set (the tone block, its pot moving each call), best of %d "
                        "x %d\n",
                        options.reps, options.samples);
            fc::Messages messages;
            const std::optional<fc::Plan> plan =
                fc::plan(fc::parse(netlist("screamer.cir"), messages), {}, "screamer.cir", messages);
            if (!plan)
                return;
            for (bool woodbury : {true, false})
            {
                std::vector<fc::BlockCode> codes;
                for (std::size_t b = 0; b < plan->blocks.size(); ++b)
                    codes.push_back(*fc::deriveBlock(*plan, static_cast<int>(b), messages, woodbury));
                const std::string source = fc::emit(*plan, codes) + R"(
float fm_bench(float n)
{
    Screamer w;
    w.init(48000);
    float sum = 0;
    for (int i = 0; i < (int)n; ++i)
    {
        w.b3_set((float)(i % 256) * 0.00390625f);
        sum += w.mb0_b3;
    }
    return sum;
}
)";
                fminus::Program::Options po;
                po.denormals = fminus::Program::Denormals::Flush;
                po.threads = 1;
                std::string errors;
                auto program = fminus::Program::compile(source, errors, po);
                if (!program)
                {
                    std::printf("  does not compile:\n%s", errors.c_str());
                    continue;
                }
                const Timing t = best(program->function<float(float)>("fm_bench", errors), options.samples / 10,
                                      options.reps);
                std::printf("  %-26s %8.2f ns %8.1f cycles %8.1f instr a call | %s\n",
                            woodbury ? "the Woodbury form" : "all in set (Phase 0's)", t.ns, t.cycles,
                            t.instructions, codes[2].form.empty() ? "(no note)" : codes[2].form.c_str());
            }
        }

        // The probes' cost (docs/circuits.md, "Probes"): screamer.cir and otalp.cir as the playground's template calls
        // a circuit, two instances (left and right) ticked in a host module's process loop over blocks of 64 samples,
        // the signal (and otalp.cir's iabc) read from tables the host fills in init, so that the loop costs little but
        // the circuit's work; generated without probes, with them disarmed (tick's one branch), and armed on one slot
        // and on four (the left instance's, as the playground arms one). Printed, never asserted.
        void probeCosts(const BenchOptions& options)
        {
            const int samples = std::max(options.samples / 4, 4096);
            std::printf("the probes: ns a host sample of two instances in a host module's process loop (blocks of 64), "
                        "best of %d x %d; the left instance armed\n",
                        options.reps, samples);
            struct Case
            {
                const char* file;
                double amp;
                const char* node;   // the node one slot arms
            };
            for (const Case& k : {Case{"screamer.cir", 0.45, "v2"}, Case{"otalp.cir", 5, "y1"}})
            {
                std::vector<Timing> times;
                std::vector<std::string> names;
                std::string setLine;
                for (int variant = 0; variant < 4; ++variant)
                {
                    fc::Options o;
                    o.probes = variant > 0;
                    const fc::Result r = fc::generate(netlist(k.file), k.file, o);
                    if (!r.errors.empty())
                    {
                        std::printf("  %s: %s", k.file, r.errors.c_str());
                        break;
                    }
                    int index = 0;
                    std::vector<int> readable;
                    for (std::size_t n = 0; n < r.nodes.size(); ++n)
                    {
                        if (r.nodes[n].name == k.node)
                            index = static_cast<int>(n);
                        if (r.nodes[n].readable)
                            readable.push_back(static_cast<int>(n));
                    }
                    std::string arm;
                    if (variant >= 2)
                    {
                        arm = "        amp[0].prb_ = 1;\n";
                        for (int slot = 0; slot < (variant == 2 ? 1 : 4); ++slot)
                            arm += std::format("        amp[0].prs_[{}] = {};\n", slot,
                                               slot == 0 ? index
                                                         : readable[static_cast<std::size_t>(slot) % readable.size()]);
                    }
                    const bool ota = !r.inputs.empty() && r.inputs.size() > 1;
                    const std::string source = r.source + std::format(R"(
module Host
{{
    {0} amp[2];
    float sig[1024];
    float law[1024];
    void init(float rate)
    {{
        amp[0].init(rate);
        amp[1].init(rate);
        for (int i = 0; i < 1024; ++i)
        {{
            sig[i] = {1} * sinf(0.130899694f * (float)i);
            law[i] = 0.000141421f * expf(1.38629436f * sinf(0.00613592f * (float)i));
        }}
{2}    }}
    void process(float* restrict left, float* restrict right, int n) [[clang::nonblocking]]
    {{
        for (int i = 0; i < n; ++i)
        {{
            left[i] = amp[0].tick(left[i]{3});
            right[i] = amp[1].tick(right[i]{4});
        }}
    }}
}};
float fm_bench(float n)
{{
    Host h;
    h.init(48000);
    float left[64];
    float right[64];
    float sum = 0;
    int at = 0;
    for (int b = 0; b < (int)n; b += 64)
    {{
        for (int i = 0; i < 64; ++i)
        {{
            left[i] = h.sig[(at + i) % 1024];
            right[i] = h.sig[(at + i + 256) % 1024];
        }}
        h.process(left, right, 64);
        at = (at + 64) % 1024;
        sum += left[0] + right[63];
    }}
    return sum;
}}
)",
                                                                      r.module, fc::literal(k.amp), arm,
                                                                      ota ? ", law[i % 1024]" : "",
                                                                      ota ? ", law[(i + 512) % 1024]" : "");
                    fminus::Program::Options po;
                    po.math = options.math;
                    po.denormals = fminus::Program::Denormals::Flush;
                    po.threads = 1;
                    std::string errors;
                    auto program = fminus::Program::compile(source, errors, po);
                    if (!program)
                    {
                        std::printf("  %s does not compile:\n%s", k.file, errors.c_str());
                        break;
                    }
                    times.push_back(best(program->function<float(float)>("fm_bench", errors), samples, options.reps));
                    names.push_back(variant == 0   ? "no probes"
                                    : variant == 1 ? "disarmed"
                                    : variant == 2 ? std::format("armed, 1 slot ({})", k.node)
                                                   : std::string("armed, 4 slots"));
                }
                if (times.size() != 4)
                    continue;
                std::printf("  %-14s", k.file);
                for (std::size_t v = 0; v < times.size(); ++v)
                {
                    std::printf(" | %s %.1f ns", names[v].c_str(), times[v].ns);
                    if (v > 0)
                        std::printf(" (%+.2f %%)", 100 * (times[v].ns / times[0].ns - 1));
                }
                std::printf("\n  %-14s", "");
                for (std::size_t v = 0; v < times.size(); ++v)
                    std::printf(" | %.0f cycles, %.0f instr", times[v].cycles, times[v].instructions);
                std::printf("  (idle overhead %+.2f %% of the cycles)\n",
                            100 * (times[1].cycles / times[0].cycles - 1));
                std::fflush(stdout);
            }
            // set(): the probes' coefficients are computed where the module's are, so a pot's move costs more
            std::printf("  set() on screamer.cir, its three pots moving each call: ");
            for (bool probes : {false, true})
            {
                fc::Options o;
                o.probes = probes;
                const fc::Result r = fc::generate(netlist("screamer.cir"), "screamer.cir", o);
                const std::string source = r.source + R"(
float fm_bench(float n)
{
    Screamer w;
    w.init(48000);
    float sum = 0;
    for (int i = 0; i < (int)n; ++i)
    {
        const float x = (float)(i % 256) * 0.00390625f;
        w.set(x, 1 - x, x);
        sum += w.mb0_b3;
    }
    return sum;
}
)";
                fminus::Program::Options po;
                po.denormals = fminus::Program::Denormals::Flush;
                po.threads = 1;
                std::string errors;
                auto program = fminus::Program::compile(source, errors, po);
                if (!program)
                {
                    std::printf("does not compile:\n%s", errors.c_str());
                    return;
                }
                const Timing t =
                    best(program->function<float(float)>("fm_bench", errors), options.samples / 10, options.reps);
                std::printf("%s %.1f ns, %.0f cycles a call%s", probes ? "with probes" : "without", t.ns, t.cycles,
                            probes ? "\n" : "; ");
            }
        }
    } // namespace

    int bench(const BenchOptions& options)
    {
#if defined(__APPLE__)
        pthread_set_qos_class_self_np(QOS_CLASS_USER_INTERACTIVE, 0);   // prefer a performance core
#endif
#if defined(_WIN32)
        std::printf("load average: n/a\n");
#else
        double load[3] = {0, 0, 0};
        if (getloadavg(load, 3) == 3)
            std::printf("load average: %.2f %.2f %.2f\n", load[0], load[1], load[2]);
#endif
        if (options.only == "probes")
        {
            probeCosts(options);
            return 0;
        }
        if (options.only.empty())
        {
            generateTimes();
            selfTestTimes();
            setCycles(options);
            probeCosts(options);
        }
        std::printf("cycles a host sample at 48 kHz, best of %d x %d (wall ns; cycles and instructions from "
                    "proc_pid_rusage on macOS); the self-test's cost model and the report's (timing1.out) beside\n",
                    options.reps, options.samples);
        const auto report = reported();
        std::vector<Row> rows = options.all ? allRows() : defaultRows();
        const std::regex only(options.only.empty() ? std::string(".*") : options.only);
        std::map<std::string, Timing> baselines;
        for (Row row : rows)
        {
            if (options.math != fminus::Program::Math::Vector && row.math == fminus::Program::Math::Vector)
                row.math = options.math;
            const std::string name = row.name();
            const char* mode = row.math == fminus::Program::Math::Fast   ? " fast"
                               : row.math == fminus::Program::Math::Libm ? " libm"
                                                                         : "";
            if (!std::regex_search(name + mode + (row.channels == 2 ? " float2 " : " ") + row.note, only))
                continue;
            const std::string file = row.circuit == "DT"    ? "diodetone.cir"
                                     : row.circuit == "TS"  ? "screamer-clip.cir"
                                     : row.circuit == "OTA" ? "otalp.cir"
                                                            : "screamer.cir";
            fc::Options o;
            o.oversample = row.m;
            o.halfband = row.kind;
            o.adaa = row.adaa;
            o.solver = row.solver;
            o.channels = row.channels;
            o.region = row.region;
            if (row.prewarp)
                o.prewarp = true;
            const double amp = row.circuit == "DT" ? 4.5 : row.circuit == "OTA" ? 5 : 0.45;
            // mkbench.py's settings: tone 0.5 (DiodeTone), drive 1 (the TS)
            const std::string setCall = row.circuit == "DT"   ? "w.set(0.5f);"
                                        : row.circuit == "TS" ? "w.set(1);"
                                                              : "w.set(1, 0.5f, 0.7f);";
            const fc::Result r = fc::generate(netlist(file), file, o);
            if (!r.errors.empty())
            {
                std::printf("%-30s %s", name.c_str(), r.errors.c_str());
                continue;
            }
            fminus::Program::Options po;
            po.math = row.math;
            po.denormals = fminus::Program::Denormals::Flush;
            po.threads = 1;
            std::string errors;
            const std::string bench = row.circuit == "OTA" ? otaBenchSource(r.module, amp, row.channels)
                                                           : benchSource(r.module, amp, row.channels, setCall);
            auto program = fminus::Program::compile(r.source + bench, errors, po);
            if (!program)
            {
                std::printf("%-30s does not compile:\n%s", name.c_str(), errors.c_str());
                continue;
            }
            auto* fn = program->function<float(float)>("fm_bench", errors);
            const std::string baselineKey = row.section();
            if (!baselines.count(baselineKey))
                baselines[baselineKey] =
                    best(program->function<float(float)>("fm_baseline", errors), options.samples, options.reps);
            const Timing t = best(fn, options.samples, options.reps);
            const auto it = report.find({row.section(), name});
            const std::string theirs = it == report.end() ? std::string("      -") : std::format("{:7.1f}", it->second);
            fc::Messages messages;
            const std::optional<fc::Plan> plan = fc::plan(fc::parse(netlist(file), messages), o, file, messages);
            const double model = plan ? fc::modelCost(*plan, row.m, row.adaa, row.solver) * row.channels : 0;
            std::printf("%-30s %-11s %8.2f ns %8.1f cycles %8.1f instr | model %6.0f | report %s | %s\n", name.c_str(),
                        row.math == fminus::Program::Math::Fast ? "Math::Fast"
                        : row.channels == 2                     ? "float2"
                                                                : "",
                        t.ns, t.cycles, t.instructions, model, theirs.c_str(), row.note.c_str());
            std::fflush(stdout);
        }
        for (const auto& [section, t] : baselines)
            std::printf("baseline (%s): %.2f ns %.1f cycles %.1f instr (the report's: 10.1 cycles)\n", section.c_str(),
                        t.ns, t.cycles, t.instructions);
        return 0;
    }
} // namespace circuits
