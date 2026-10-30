// tests/unit/CircuitEmitTest.cpp: the circuit compiler's FMinus text, compiled through fminus::Program: its
// coefficients against the double derivation they print, every configuration compiling with its methods
// nonblocking, and generation deterministic with a layout that no pot value changes.
#include "circuit/Emit.h"
#include "circuit/Halfbands.h"
#include "circuit/Laws.h"
#include "circuit/Linear.h"
#include "circuit/Mna.h"
#include "circuit/Netlist.h"
#include "circuit/Plan.h"
#include "circuit/Reference.h"
#include "circuit/Runners.h"
#include "circuit/Analysis.h"
#include "CircuitNetlists.h"
#include "fminus/Circuit.h"
#include "fminus/FMinus.h"

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdlib>
#include <cstring>
#include <format>
#include <locale>
#include <map>
#include <memory>
#include <numbers>
#include <optional>
#include <regex>
#include <sstream>
#include <string>
#include <tuple>
#include <vector>

namespace
{
    namespace fc = fminus::circuit;
    using fminus::Program;

    struct Free
    {
        void operator()(void* p) const
        {
            std::free(p);
        }
    };

    // An instance of a module, as a host makes one: aligned and zeroed.
    std::unique_ptr<void, Free> instance(const Program::Module& module)
    {
        const std::size_t alignment = std::max(module.alignment, alignof(std::max_align_t));
        void* memory = std::aligned_alloc(alignment, (module.size + alignment - 1) / alignment * alignment);
        std::memset(memory, 0, module.size);
        return std::unique_ptr<void, Free>(memory);
    }

    std::unique_ptr<Program> compiled(const fc::Result& result)
    {
        INFO(result.errors);
        REQUIRE(result.errors.empty());
        std::string errors;
        auto program = Program::compile(result.source, errors);
        INFO(errors);
        REQUIRE(program);
        return program;
    }

    // E and G with gains pots move, an E's output driving the next block, a G stamped in its block.
    const std::string kGains = R"(.title Gains
.input u in
.output y o3
.pot g 0.5
.pot h 0.2
E1 m 0 in 0 {1 + 10 * g}
R1 m out 1k
C1 out 0 10n
G1 o2 0 out 0 {1m + 2m * h}
R3 o2 0 1k
X2 o2 o3 o3 opamp
)";

    // One diode whose K a pot moves (R1 from 100 ohm to 20k), and an asymmetric pair's (ADAA on: b1_set's carry).
    const std::string kSinglePot = R"(.title SinglePot
.input u in
.output y a
.pot drive 0.5
.oversample 4
.adaa on
.solver omega(1)
R1 in a {100 + 20k * drive}
C1 a 0 10n
D1 a 0 1N4148
)";

    // kSinglePot with no pot (K a number but for the capacitor): a port block BDF2 runs
    const std::string kSinglePot0 = R"(.title SinglePot
.input u in
.output y a
.oversample 4
.adaa on
.solver omega(1)
R1 in a 2.2k
C1 a 0 10n
D1 a 0 1N4148
)";

    const std::string kAsymPot = R"(.title AsymPot
.input u in
.output y a
.pot drive 0.5
.oversample 4
.adaa on
.solver omega(1)
R1 in a {100 + 20k * drive}
C1 a 0 10n
D1 a 0 1N914
D2 0 m 1N914
D3 m a 1N914
)";

    fc::Plan planned(const std::string& text, const fc::Options& options = {})
    {
        fc::Messages messages;
        const fc::Netlist netlist = fc::parse(text, messages);
        std::optional<fc::Plan> p = fc::plan(netlist, options, "t.cir", messages);
        REQUIRE(p);
        return *p;
    }

    std::string withLine(std::string text, const std::string& from, const std::string& to)
    {
        const std::size_t at = text.find(from);
        REQUIRE(at != std::string::npos);
        text.replace(at, from.size(), to);
        return text;
    }

    float readFloat(const void* base, const Program::Module& m, const std::string& name)
    {
        for (const Program::Module::Field& f : m.fields)
        {
            if (f.name == name)
            {
                float v = 0;
                std::memcpy(&v, static_cast<const char*>(base) + f.offset, sizeof v);
                return v;
            }
        }
        FAIL("no field " << name);
        return 0;
    }

    // Calls set() with the pots, whatever their count (0 to 3 in these netlists: none, no call), then the switches
    // (none, or one).
    void callSet(Program& program, const std::string& module, void* self, const std::vector<float>& pots,
                 const std::vector<int>& switches = {})
    {
        std::string errors;
        const std::string name = module + "::set";
        if (pots.empty() && switches.empty())
            return;
        if (switches.size() == 1)
        {
            if (pots.empty())
                program.function<void(void*, int)>(name, errors)(self, switches[0]);
            else if (pots.size() == 1)
                program.function<void(void*, float, int)>(name, errors)(self, pots[0], switches[0]);
            else
                FAIL("set() takes " << pots.size() << " pots and a switch");
            return;
        }
        if (!switches.empty())
            FAIL("set() takes " << switches.size() << " switches");
        if (pots.size() == 1)
            program.function<void(void*, float)>(name, errors)(self, pots[0]);
        else if (pots.size() == 2)
            program.function<void(void*, float, float)>(name, errors)(self, pots[0], pots[1]);
        else if (pots.size() == 3)
            program.function<void(void*, float, float, float)>(name, errors)(self, pots[0], pots[1], pots[2]);
        else
            FAIL("set() takes " << pots.size() << " pots");
    }
} // namespace

TEST_CASE("the emitted coefficients are the double derivation's, in float", "[circuit]")
{
    // Phase 0's, and Phase 1's elements: an inductor (the wah's tank, a pot damping it), a DC source and an E, a
    // pot's gain in an E and a G, one diode and an asymmetric pair whose K a pot moves, with and without ADAA, and
    // switches (one in a linear block, one moving a diode pair's K). Where b<N>_set is the Woodbury form (its rate-only
    // part init's), init and b<N>_set together give the all-in-set derivation's coefficients (Phase 0's pivot order,
    // in double), pots at exactly 0 and 1 included, at host rates from 44.1 to 384 kHz.
    const std::string netlists[] = {fc::test::kDiodeTone,
                                    ".title Sw\n.input u in\n.output y a\n.switch mode 3 1\nR1 in a 10k\nC1 a 0 10n\n"
                                    "S1 a b mode 1,2\nC2 b 0 47n\n",
                                    ".title SwClip\n.input u in\n.output y a\n.switch hard 2\n.oversample 4\n"
                                    ".adaa on\n.solver omega(1)\n.pot drive\nR1 in a {1k + 9k * drive}\n"
                                    "S1 in a hard 1 ron=100\nC1 a 0 10n\nD1 a 0 1N914\nD2 0 a 1N914\n",
                                    fc::test::kScreamer,
                                    withLine(fc::test::kDiodeTone, ".adaa off", ".adaa on"),
                                    withLine(fc::test::kScreamer, ".adaa on", ".adaa off"),
                                    fc::test::kWah,
                                    fc::test::kRlLowpass,
                                    fc::test::kDivider,
                                    kGains,
                                    kSinglePot,
                                    withLine(kSinglePot, ".adaa on", ".adaa off"),
                                    kAsymPot,
                                    withLine(kAsymPot, ".adaa on", ".adaa off")};
    for (const std::string& text : netlists)
    {
        INFO(text);
        const fc::Result result = fc::generate(text, "t.cir");
        auto program = compiled(result);
        const Program::Module* m = program->module(result.module);
        REQUIRE(m);
        const fc::Plan plan = planned(text);
        std::string errors;
        auto* init = program->function<void(void*, float)>(result.module + "::init", errors);
        REQUIRE(init);
        for (double rate : {44100.0, 48000.0, 96000.0, 192000.0, 384000.0})
        {
            for (std::size_t b = 0; b < plan.blocks.size(); ++b)
            {
                const fc::Block& block = plan.blocks[b];
                fc::Messages messages;
                const std::optional<fc::BlockCode> code = fc::deriveBlock(plan, static_cast<int>(b), messages);
                REQUIRE(code);
                const std::optional<fc::Pivots> phase0 =
                    fc::choosePivots(code->system, plan, static_cast<int>(b), messages);
                REQUIRE(phase0);
                const double designRate = rate * block.rateFactor * block.designFactor;
                for (const std::vector<double>& pots : fc::potGrid(plan, block))
                {
                    auto self = instance(*m);
                    init(self.get(), static_cast<float>(rate));
                    // the pots as floats, the switches as ints, set until a switch's ramp settles (16 / (0.02 rate)
                    // a call: 60 calls at 48 kHz, 480 at 384 kHz)
                    std::vector<float> args(pots.begin(), pots.begin() + static_cast<long>(plan.pots.size()));
                    std::vector<int> positions;
                    for (std::size_t k = plan.pots.size(); k < pots.size(); ++k)
                        positions.push_back(static_cast<int>(pots[k]));
                    for (int call = 0; call < (positions.empty() ? 1 : 512); ++call)
                        callSet(*program, result.module, self.get(), args, positions);
                    if (block.design == fc::Block::Design::Matched)
                    {
                        // a matched design: the float one's response against the double one's (from the block's
                        // trapezoidal form, Linear.h) over the 1x check's band, within 0.02 dB to 96 kHz and 0.1 dB
                        // above. Its coefficients are only as well conditioned as the trapezoidal form they come
                        // from: a pole near z = -1 (a stiff branch, the switch's 1 ohm into C2) or near z = 1 (a low
                        // pole at 384 kHz) costs float two to four digits of them (the matched designs' own test holds
                        // well-conditioned ones to 1e-5)
                        const std::optional<fc::TrForm<double>> tr =
                            fc::trForm(plan, static_cast<int>(b), designRate, pots);
                        REQUIRE(tr);
                        const fc::Matched<double> d = fc::matchedDesign(*tr, block.matchedPolarity);
                        const std::string n = "_b" + std::to_string(block.index);
                        auto field = [&](const char* name)
                        {
                            return static_cast<double>(readFloat(self.get(), *m, name + n));
                        };
                        fc::Matched<double> f;
                        f.b0 = field("mb0");
                        f.b1 = field("mb1");
                        f.a1 = field("ma1");
                        if (tr->order == 2)
                        {
                            f.b2 = field("mb2");
                            f.a2 = field("ma2");
                        }
                        // within 40 dB of the response's peak, as the 1x check counts
                        double peak = 0;
                        for (double hz = 20; hz <= 20000; hz *= 1.05)
                            peak = std::fmax(peak, std::abs(fc::matchedResponse(d, hz, designRate)));
                        double worst = 0;
                        for (double hz = 20; hz <= 20000; hz *= 1.05)
                        {
                            const double hd = std::abs(fc::matchedResponse(d, hz, designRate));
                            const double hf = std::abs(fc::matchedResponse(f, hz, designRate));
                            if (hd >= 0.01 * peak)
                                worst = std::fmax(worst, std::fabs(20 * std::log10(hf / hd)));
                        }
                        INFO(result.module << " block " << b + 1 << " at " << rate << ": " << worst << " dB");
                        CHECK(worst <= (rate <= 96000 ? 0.02 : 0.1));
                        continue;
                    }
                    const std::vector<double> symbols =
                        fc::symbolValues(code->system, plan, static_cast<int>(b), designRate, pots);
                    const fc::Linear<double> linear = fc::solve(code->system, *phase0, symbols);
                    const fc::Coefficients<double> doubles =
                        fc::derive(code->system, linear, block, plan.config.adaa && block.port.has_value());
                    std::map<std::string, double> expected(doubles.begin(), doubles.end());
                    // per readout (the text before _by_): max_s |f - d| S_s <= 1e-5 sum_s |d| S_s, S_s a source's scale
                    std::map<std::string, std::pair<double, double>> rows;   // readout -> (worst, sum)
                    for (const auto& [name, d] : doubles)
                    {
                        const std::size_t by = name.find("_by_");
                        if (by == std::string::npos)
                            continue;
                        const std::string source = name.substr(by + 4);
                        // a capacitor's state is a current (gc volts); an inductor's a voltage (rl i + v)
                        double scale = 1;
                        if (source.rfind("x_", 0) == 0)
                        {
                            const auto gc = std::find_if(code->system.symbols.begin(), code->system.symbols.end(),
                                                         [&](const fc::System::Symbol& s)
                                                         { return s.name == "gc_" + source.substr(2); });
                            if (gc != code->system.symbols.end())
                                scale = symbols[static_cast<std::size_t>(gc - code->system.symbols.begin())];
                        }
                        auto& row = rows[name.substr(0, by)];
                        row.second += std::fabs(d) * scale;
                        const auto field = std::find_if(code->coefficients.begin(), code->coefficients.end(),
                                                        [&](const auto& c) { return c.first == name; });
                        REQUIRE(field != code->coefficients.end());
                        if (field->second.kind != fc::Val::Kind::Var)
                            continue;   // a literal: exact
                        const double f = static_cast<double>(readFloat(self.get(), *m, name));
                        CHECK(std::isfinite(f));
                        row.first = std::fmax(row.first, std::fabs(f - d) * scale);
                    }
                    for (const auto& [readout, row] : rows)
                    {
                        INFO(result.module << " block " << b + 1 << " at " << rate << ", readout " << readout);
                        CHECK(row.first <= 1e-5 * row.second);
                    }
                    if (block.port)
                    {
                        for (const char* k : {"ka_", "kb_", "lna_", "kc_", "kr_", "ks_", "lnr_", "kcr_"})
                        {
                            const std::string name = k + block.port->name;
                            if (!expected.count(name))
                                continue;
                            const double d = expected[name];
                            const double f = static_cast<double>(readFloat(self.get(), *m, name));
                            INFO(name << ": " << f << " against " << d);
                            CHECK(std::fabs(f - d) <= 1e-5 * std::fabs(d));
                        }
                    }
                }
            }
        }
    }
}

TEST_CASE("every configuration compiles, nonblocking", "[circuit]")
{
    struct Pair
    {
        bool adaa;
        fc::Solver solver;
    };
    const Pair solvers[] = {{true, {fc::Solver::Guess::Omega, 1}},    {false, {fc::Solver::Guess::Omega, 2}},
                            {true, {fc::Solver::Guess::Omega, 0}},    {false, {fc::Solver::Guess::Previous, 2}},
                            {true, {fc::Solver::Guess::Previous, 4}}, {false, {fc::Solver::Guess::Omega, 0}}};
    const std::pair<int, fc::Halfband> rates[] = {
        {1, fc::Halfband::Iir}, {2, fc::Halfband::Fir}, {4, fc::Halfband::Iir}, {8, fc::Halfband::Fir}};
    const fc::Region regions[] = {fc::Region::Auto, fc::Region::Nonlinear, fc::Region::All};
    int index = 0;
    int count = 0;
    for (const std::string& text : {fc::test::kDiodeTone, fc::test::kScreamer})
    {
        for (const Pair& s : solvers)
        {
            for (const auto& [m, kind] : rates)
            {
                fc::Options o;
                o.adaa = s.adaa;
                o.solver = s.solver;
                o.oversample = m;
                o.halfband = kind;
                o.channels = index % 3 == 0 ? 2 : 1;
                o.region = regions[index % 3];
                ++index;
                const fc::Result result = fc::generate(text, "t.cir", o);
                INFO("config " << index << ":\n" << result.source);
                auto program = compiled(result);
                for (const char* method : {"init", "set", "tick", "step", "clear"})
                {
                    const std::string name = result.module + "::" + method;
                    INFO(name);
                    CHECK(program->nonBlocking(name));
                }
                ++count;
            }
        }
    }
    CHECK(count == 48);
}

TEST_CASE("generation is deterministic and the layout is fixed", "[circuit]")
{
    for (const std::string& text : {fc::test::kDiodeTone, fc::test::kScreamer})
    {
        const fc::Result a = fc::generate(text, "dir/sub/pedal.cir");
        const fc::Result b = fc::generate(text, "dir/sub/pedal.cir");
        CHECK(a.source == b.source);
        CHECK(a.errors == b.errors);
        CHECK(a.warnings == b.warnings);
        CHECK(a.notes == b.notes);
        CHECK(a.module == b.module);
        CHECK(a.pots == b.pots);
        // the header names the netlist by its base name only
        CHECK(a.source.find("generated from pedal.cir by") != std::string::npos);
        CHECK(a.source.find("dir/sub") == std::string::npos);
    }
    const fc::Result dt = fc::generate(fc::test::kDiodeTone, "diodetone.cir");
    CHECK(dt.module == "DiodeTone");
    CHECK(dt.pots == std::vector<std::string>{"tone"});
    const fc::Result ts = fc::generate(fc::test::kScreamer, "screamer.cir");
    CHECK(ts.module == "Screamer");
    CHECK(ts.pots == (std::vector<std::string>{"drive", "tone", "level"}));
    // other .pot defaults: the same fields and methods (the frozen-dial rule)
    {
        const std::string moved =
            withLine(withLine(fc::test::kScreamer, ".pot    drive 0.5 audio", ".pot drive 1 audio"),
                     ".pot    level 0.7 audio", ".pot level 0 audio");
        auto p1 = compiled(ts);
        auto p2 = compiled(fc::generate(moved, "screamer.cir"));
        const Program::Module* m1 = p1->module("Screamer");
        const Program::Module* m2 = p2->module("Screamer");
        REQUIRE(m1);
        REQUIRE(m2);
        CHECK(m1->size == m2->size);
        CHECK(m1->methods == m2->methods);
        REQUIRE(m1->fields.size() == m2->fields.size());
        for (std::size_t i = 0; i < m1->fields.size(); ++i)
        {
            CHECK(m1->fields[i].name == m2->fields[i].name);
            CHECK(m1->fields[i].type == m2->fields[i].type);
            CHECK(m1->fields[i].offset == m2->fields[i].offset);
        }
    }
    // every float literal re-reads to the float it names, and prints back the same
    const std::regex literal(R"((^|[^A-Za-z0-9_.])([0-9]+\.[0-9]*(e[+-]?[0-9]+)?|[0-9]+e[+-]?[0-9]+)f)");
    int literals = 0;
    for (const fc::Result& r : {dt, ts})
    {
        for (auto it = std::sregex_iterator(r.source.begin(), r.source.end(), literal); it != std::sregex_iterator();
             ++it)
        {
            const std::string text = (*it)[2].str();
            std::istringstream stream(text);
            stream.imbue(std::locale::classic());
            double value = 0;
            stream >> value;
            const float f = static_cast<float>(value);
            INFO(text);
            CHECK(fc::literal(static_cast<double>(f)) == text + "f");
            ++literals;
        }
    }
    CHECK(literals > 50);
    // the constants the generator folds, as the floats of their doubles
    CHECK(dt.source.find("gc_C1 = " + fc::literal(2 * 10e-9 * 4) + " * rate;") != std::string::npos);
    CHECK(ts.source.find("gc_C3 = " + fc::literal(2 * 47e-9 * 4 * (2.0 / 3.0)) + " * rate;") != std::string::npos);
    const double nvt = 1.752 * 1.380649e-23 * 300.15 / 1.602176634e-19;
    CHECK(dt.source.find("expf(v * " + fc::literal(1 / nvt) + ")") != std::string::npos);
    CHECK(dt.source.find(" - " + fc::literal(nvt) + " * w;") != std::string::npos);
    CHECK(fc::literal(4.394449154672439) == "4.394449f");
    CHECK(fc::literal(25000) == "25000.0f");
    CHECK(fc::literal(0.5) == "0.5f");
    CHECK(fc::literal(2, true) == "2");
}

TEST_CASE("a port block whose K is a constant compiles and runs", "[circuit]")
{
    // No capacitor and no pot: K is a number, so the port's constants are literals, not fields (ka_D1 and the rest).
    // y is the port's v, from v + 2 Is K sinh(v / N Vt) = p.
    struct Case
    {
        std::string netlist;
        double k;      // the port's K
        double gain;   // p per volt of u
        double sign;   // y = u * offset + sign * v
        double offset;
    };
    const std::string model = ".model DD D(IS=2.52n N=1.752)\n";
    const Case cases[] = {
        // a resistor and the pair: y = v, p = u
        {".title Hard\n.input u in\n.output y a\nR1 in a 2.2k\nD1 a 0 DD\nD2 0 a DD\n" + model, 2200, 1, 1, 0},
        // an op amp with resistive feedback: V(n1) = u, v = V(n1) - V(v2) = u - y, p = -u R6 / R4
        {".title OpClip\n.input u p1\n.output y v2\nXU1 p1 n1 v2 opamp\nR4 n1 0 4.7k\nR6 n1 v2 51k\nD1 n1 v2 DD\n"
         "D2 v2 n1 DD\n" +
             model,
         51000, -51.0 / 4.7, -1, 1},
    };
    const double nvt = fc::thermalVoltage(1.752);
    for (const Case& c : cases)
    {
        for (bool adaa : {false, true})
        {
            fc::Options o;
            o.adaa = adaa;
            const fc::Result r = fc::generate(c.netlist, "t.cir", o);
            INFO(r.source);
            CHECK(r.source.find("ka_D1") == std::string::npos);
            auto program = compiled(r);
            for (const char* method : {"init", "tick", "step", "clear"})
                CHECK(program->nonBlocking(r.module + "::" + method));
            const Program::Module* m = program->module(r.module);
            REQUIRE(m);
            std::string errors;
            auto* init = program->function<void(void*, float)>(r.module + "::init", errors);
            auto* tick = program->function<float(void*, float)>(r.module + "::tick", errors);
            REQUIRE(init);
            REQUIRE(tick);
            for (double u : {0.1, 1.0})
            {
                // the exact root, by bisection
                const double p = c.gain * u;
                double lo = -std::fabs(p);
                double hi = std::fabs(p);
                for (int i = 0; i < 200; ++i)
                {
                    const double mid = 0.5 * (lo + hi);
                    (mid + 2 * 2.52e-9 * c.k * std::sinh(mid / nvt) < p ? lo : hi) = mid;
                }
                const double exact = c.offset * u + c.sign * 0.5 * (lo + hi);
                auto self = instance(*m);
                init(self.get(), 48000);
                float y = 0;
                for (int i = 0; i < 4800; ++i)
                    y = tick(self.get(), static_cast<float>(u));
                // omega + 2 lands on the root; ADAA's omega + 1 within F1's first-order DC error
                INFO("adaa " << adaa << ", u " << u << ": " << y << " against " << exact);
                CHECK(std::fabs(static_cast<double>(y) - exact) <= (adaa ? 1.5e-3 : 2e-5));
            }
        }
    }
}

TEST_CASE("a pot's move keeps ADAA's last point on the port's curve", "[circuit]")
{
    // b<N>_set moves p_D1 with ka, so that p_D1 - v_D1 - 2 ka sinh(v_D1 / N Vt), the last step's Newton residual,
    // stays what it was: otherwise ADAA's closed-form mean pairs a point of the old curve with the new curve's.
    const fc::Result r = fc::generate(fc::test::kScreamerClip, "screamer-clip.cir");
    auto program = compiled(r);
    const Program::Module* m = program->module(r.module);
    REQUIRE(m);
    std::string errors;
    auto* init = program->function<void(void*, float)>(r.module + "::init", errors);
    auto* set = program->function<void(void*, float)>(r.module + "::set", errors);
    auto* tick = program->function<float(void*, float)>(r.module + "::tick", errors);
    REQUIRE(init);
    REQUIRE(set);
    REQUIRE(tick);
    auto self = instance(*m);
    init(self.get(), 48000);
    set(self.get(), 0);
    // a 0.45 V, 996 Hz sine, to a sample where the diodes conduct
    const double nvt = fc::thermalVoltage(1.752);
    bool conducting = false;
    for (int i = 0; i < 9600 && !conducting; ++i)
    {
        tick(self.get(), static_cast<float>(0.45 * std::sin(2 * 3.141592653589793 * 996 * i / 48000)));
        conducting = i >= 2400 && std::fabs(readFloat(self.get(), *m, "v_D1")) > 0.4f;
    }
    REQUIRE(conducting);
    struct Point
    {
        double residual;
        double p;
        double curve;   // 2 ka sinh(v / N Vt)
    };
    auto point = [&]
    {
        const auto v = static_cast<double>(readFloat(self.get(), *m, "v_D1"));
        const auto p = static_cast<double>(readFloat(self.get(), *m, "p_D1"));
        const double curve = static_cast<double>(readFloat(self.get(), *m, "ka_D1")) * 2 * std::sinh(v / nvt);
        return Point{p - v - curve, p, curve};
    };
    const Point before = point();
    set(self.get(), 1);
    const Point after = point();
    INFO("residual " << before.residual << " -> " << after.residual << ", p " << before.p << " -> " << after.p);
    CHECK(std::fabs(after.curve - before.curve) > 0.1 * std::fabs(before.p));   // the move is large
    CHECK(std::fabs(after.residual - before.residual) <= 1e-5 * std::fmax(std::fabs(before.p), std::fabs(after.p)));
    // a pot held changes nothing, bit for bit
    const float p = readFloat(self.get(), *m, "p_D1");
    set(self.get(), 1);
    CHECK(readFloat(self.get(), *m, "p_D1") == p);
}

TEST_CASE("a BDF2 module is the double BDF2 reference's, nonblocking in both channel counts", "[circuit]")
{
    // .method bdf2: the emitted step (its companions, its second histories) against the double reference running
    // the block's BDF2 Holters form with the module's guess and Newton steps, in band: a diode pair across C1 (and a
    // pot into C2), an OTA stage in its i-form, one diode
    struct Case
    {
        std::string text;
        std::vector<double> laws;   // the law inputs, held
    };
    const Case cases[] = {{withLine(fc::test::kDiodeTone, ".adaa off", ".method bdf2"), {}},
                          {fc::test::otaStage(".oversample 2\n.adaa off\n.solver newton(2)\n.method bdf2\n"), {1e-4}},
                          {withLine(kSinglePot0, ".adaa on", ".method bdf2"), {}}};
    for (const Case& c : cases)
    {
        const fc::Result r = fc::generate(c.text, "t.cir");
        INFO(r.errors << r.warnings);
        REQUIRE(r.errors.empty());
        CHECK(r.source.find("xo_") != std::string::npos);
        auto program = compiled(r);
        for (const char* method : {"init", "set", "tick", "step", "clear"})
        {
            const std::string name = r.module + "::" + method;
            if (std::find(program->module(r.module)->methods.begin(), program->module(r.module)->methods.end(),
                          name) != program->module(r.module)->methods.end())
                CHECK(program->nonBlocking(name));
        }
        fc::Options stereo;
        stereo.channels = 2;
        const fc::Result r2 = fc::generate(c.text, "t.cir", stereo);
        auto program2 = compiled(r2);
        CHECK(program2->nonBlocking(r2.module + "::tick"));
        CHECK(program2->nonBlocking(r2.module + "::step"));

        const fc::Plan plan = planned(c.text);
        REQUIRE(plan.regionFirst == 0);
        REQUIRE(plan.regionFirst == plan.regionLast);
        const Program::Module* m = program->module(r.module);
        REQUIRE(m);
        std::string errors;
        auto* init = program->function<void(void*, float)>(r.module + "::init", errors);
        REQUIRE(init);
        const int os = plan.config.oversample;
        const double rate = 48000.0 * os;
        std::vector<double> pots;
        for (const fc::PotInfo& pot : plan.pots)
            pots.push_back(pot.defaultValue);
        const fc::Holters h = fc::holters(plan, 0, rate, pots, c.laws);
        fc::RefOptions o;
        o.steps = plan.config.solver.steps;
        o.guess = plan.config.solver.guess;
        const std::size_t n = static_cast<std::size_t>(os) * (1024 + 4096 + 64);
        const std::vector<double> u = fc::sine(plan.netlist.target.level.value_or(1), 85, 4096, os, n);
        const std::vector<double> ref = fc::simulate(h, u, o);
        auto self = instance(*m);
        init(self.get(), 48000);
        std::vector<double> y(n);
        if (c.laws.empty())
        {
            auto* step = program->function<float(void*, float)>(r.module + "::step", errors);
            REQUIRE(step);
            for (std::size_t i = 0; i < n; ++i)
                y[i] = static_cast<double>(step(self.get(), static_cast<float>(u[i])));
        }
        else
        {
            auto* step = program->function<float(void*, float, float)>(r.module + "::step", errors);
            REQUIRE(step);
            for (std::size_t i = 0; i < n; ++i)
                y[i] = static_cast<double>(step(self.get(), static_cast<float>(u[i]), static_cast<float>(c.laws[0])));
        }
        const std::vector<double> yd = fc::decimateWindow(y, os, 1024, 4096);
        const std::vector<double> rd = fc::decimateWindow(ref, os, 1024, 4096);
        const double error = fc::bandErrorDb(yd, rd);
        INFO(r.module << ": " << error << " dB");
        CHECK(error <= -100);
    }
}

TEST_CASE("a switch ramps over 20 ms of set() calls, as a pot gliding there does", "[circuit]")
{
    // S1, open (10meg) at mode 0 and closed (1 ohm) at mode 1: sw_S1 moves 16 / (0.02 rate) a call, so at 48 kHz it
    // reaches 1 on the 60th call exactly, r falling by (roff / ron)^(1/60) a call; at 44.1 kHz on the 56th
    const std::string text = ".title Sw\n.input u in\n.output y a\n.switch mode 2\nR1 in a 10k\nC1 a 0 10n\n"
                             "S1 a b mode 1\nC2 b 0 47n\n";
    const fc::Result r = fc::generate(text, "t.cir");
    auto program = compiled(r);
    const Program::Module* m = program->module(r.module);
    REQUIRE(m);
    std::string errors;
    auto* init = program->function<void(void*, float)>(r.module + "::init", errors);
    auto* set = program->function<void(void*, int)>(r.module + "::set", errors);
    auto* tick = program->function<float(void*, float)>(r.module + "::tick", errors);
    REQUIRE(init);
    REQUIRE(set);
    REQUIRE(tick);
    CHECK(program->nonBlocking(r.module + "::set"));
    const double a = std::log(10e6);
    const double b = std::log(1.0) - a;
    for (double rate : {48000.0, 44100.0})
    {
        const double perCall = std::pow(10e6, 800 / rate);   // (roff / ron)^(1/60) at 48 kHz
        auto self = instance(*m);
        init(self.get(), static_cast<float>(rate));
        CHECK(readFloat(self.get(), *m, "sw_S1") == 0);
        double before = std::exp(a);
        int landed = 0;
        double worst = 0;
        for (int call = 1; call <= 80; ++call)
        {
            set(self.get(), 1);
            const auto sw = static_cast<double>(readFloat(self.get(), *m, "sw_S1"));
            const double now = std::exp(a + b * sw);
            worst = std::fmax(worst, before / now);
            before = now;
            if (sw == 1 && landed == 0)
                landed = call;
        }
        INFO("at " << rate);
        CHECK(landed == (rate == 48000 ? 60 : 56));
        CHECK(worst <= perCall * 1.001);
        // and back, as fast
        for (int call = 1; call <= 60; ++call)
            set(self.get(), 0);
        CHECK(readFloat(self.get(), *m, "sw_S1") == 0);
    }

    // under a 1 kHz sine the switch closing makes no larger a step than a pot that moves the same resistance the same
    // way on the same grid ticks: R2 {10meg * glide}, glide 10^(-7 s) at each set()
    const std::string glide = ".title Glide\n.input u in\n.output y a\n.pot glide 1\nR1 in a 10k\nC1 a 0 10n\n"
                              "R2 a b {10meg * glide}\nC2 b 0 47n\n";
    const fc::Result g = fc::generate(glide, "t.cir");
    auto gp = compiled(g);
    const Program::Module* gm = gp->module(g.module);
    auto* ginit = gp->function<void(void*, float)>(g.module + "::init", errors);
    auto* gset = gp->function<void(void*, float)>(g.module + "::set", errors);
    auto* gtick = gp->function<float(void*, float)>(g.module + "::tick", errors);
    REQUIRE(ginit);
    REQUIRE(gset);
    REQUIRE(gtick);
    auto self = instance(*m);
    auto other = instance(*gm);
    init(self.get(), 48000);
    ginit(other.get(), 48000);
    set(self.get(), 0);
    gset(other.get(), 1);
    double stepSwitch = 0;
    double stepGlide = 0;
    float lastS = 0;
    float lastG = 0;
    int calls = 0;
    for (int i = 0; i < 9600; ++i)
    {
        if (i % 16 == 0)
        {
            const bool on = i >= 2400;
            set(self.get(), on ? 1 : 0);
            calls += on ? 1 : 0;
            gset(other.get(), static_cast<float>(std::pow(10.0, -7.0 * std::fmin(calls / 60.0, 1.0))));
        }
        const auto u = static_cast<float>(std::sin(2 * std::numbers::pi * 1000 * i / 48000));
        const float ys = tick(self.get(), u);
        const float yg = gtick(other.get(), u);
        if (i > 0)
        {
            stepSwitch = std::fmax(stepSwitch, std::fabs(static_cast<double>(ys - lastS)));
            stepGlide = std::fmax(stepGlide, std::fabs(static_cast<double>(yg - lastG)));
        }
        lastS = ys;
        lastG = yg;
    }
    INFO("largest step: the switch's " << stepSwitch << ", the glide's " << stepGlide);
    CHECK(stepSwitch <= 1.1 * stepGlide);

    // clear() keeps each ramp where set() left it, as the coefficients are: two instances at rest with S1 closed away
    // from its default (open), one cleared, then the same sine and the same set() calls: the same bits (a clear that
    // put the ramp back at its default would open S1 and ramp it shut again over 20 ms, the coefficients stale until
    // the next set()); init alone starts the ramp at its default
    auto kept = instance(*m);
    auto cleared = instance(*m);
    auto* clear = program->function<void(void*)>(r.module + "::clear", errors);
    REQUIRE(clear);
    for (void* p : {kept.get(), cleared.get()})
    {
        init(p, 48000);
        for (int i = 0; i < 4800; ++i)
        {
            if (i % 16 == 0)
                set(p, 1);
            tick(p, 0);
        }
    }
    clear(cleared.get());
    CHECK(readFloat(cleared.get(), *m, "sw_S1") == 1);
    int differ = 0;
    for (int i = 4800; i < 9600; ++i)
    {
        if (i % 16 == 0)
        {
            set(kept.get(), 1);
            set(cleared.get(), 1);
        }
        const auto u = static_cast<float>(4.5 * std::sin(2 * std::numbers::pi * 1000 * i / 48000));
        const float yk = tick(kept.get(), u);
        const float yc = tick(cleared.get(), u);
        differ += std::memcmp(&yk, &yc, sizeof yk) != 0 ? 1 : 0;
    }
    CHECK(differ == 0);
    init(cleared.get(), 48000);
    CHECK(readFloat(cleared.get(), *m, "sw_S1") == 0);
}

TEST_CASE("a switch's ramp: block splits give the same bits, and ADAA's point stays on its curve", "[circuit]")
{
    // S1 in parallel with R1 moves the pair's K from 10k to 99 ohm; with ADAA, b1_set carries the stored point onto
    // each new curve (its Newton residual kept), call by call through the ramp
    const std::string text = ".title SwClip\n.input u in\n.output y a\n.switch hard 2\n.oversample 4\n.adaa on\n"
                             ".solver omega(1)\nR1 in a 10k\nS1 in a hard 1 ron=100\nC1 a 0 10n\nD1 a 0 1N914\n"
                             "D2 0 a 1N914\n";
    const fc::Result r = fc::generate(text, "t.cir");
    INFO(r.errors);
    REQUIRE(r.errors.empty());
    fc::RunnerShape shape;
    shape.module = r.module;
    shape.switches = 1;
    std::string errors;
    auto program = Program::compile(r.source + fc::runnerSource(shape), errors);
    INFO(errors);
    REQUIRE(program);
    const Program::Module* m = program->module(r.module);
    REQUIRE(m);
    auto* init = program->function<void(void*, float)>(r.module + "::init", errors);
    auto* set = program->function<void(void*, int)>(r.module + "::set", errors);
    auto* tick = program->function<float(void*, float)>(r.module + "::tick", errors);
    using Ticks = void(void*, const float*, const float*, const float*, const int*, float*, int, int);
    auto* ticks = program->function<Ticks>("fm_ticks", errors);
    REQUIRE(init);
    REQUIRE(set);
    REQUIRE(tick);
    REQUIRE(ticks);
    // calls of 1, 61 and 64 samples against one call, the switch closing at 1000 and opening at 3000
    constexpr int n = 4800;
    std::vector<float> u(n);
    std::vector<int> positions(n);
    for (int i = 0; i < n; ++i)
    {
        u[static_cast<std::size_t>(i)] = static_cast<float>(4.5 * std::sin(2 * std::numbers::pi * 996 * i / 48000));
        positions[static_cast<std::size_t>(i)] = i >= 1000 && i < 3000 ? 1 : 0;
    }
    auto run = [&](int call)
    {
        auto self = instance(*m);
        init(self.get(), 48000);
        std::vector<float> y(n);
        for (int at = 0; at < n; at += call)
        {
            const int count = std::min(call, n - at);
            ticks(self.get(), u.data() + at, nullptr, nullptr, positions.data() + at, y.data() + at, count, at);
        }
        return y;
    };
    const std::vector<float> one = run(n);
    for (int call : {1, 61, 64})
    {
        const std::vector<float> split = run(call);
        INFO("calls of " << call);
        CHECK(std::memcmp(one.data(), split.data(), sizeof(float) * n) == 0);
    }

    // the carry: drive to conduction, then close the switch call by call
    auto self = instance(*m);
    init(self.get(), 48000);
    set(self.get(), 0);
    const double nvt = fc::thermalVoltage(1.752);
    bool conducting = false;
    for (int i = 0; i < 9600 && !conducting; ++i)
    {
        tick(self.get(), static_cast<float>(4.5 * std::sin(2 * std::numbers::pi * 996 * i / 48000)));
        conducting = i >= 2400 && std::fabs(readFloat(self.get(), *m, "v_D1")) > 0.5f;
    }
    REQUIRE(conducting);
    auto residual = [&]
    {
        const auto v = static_cast<double>(readFloat(self.get(), *m, "v_D1"));
        const auto p = static_cast<double>(readFloat(self.get(), *m, "p_D1"));
        const auto ka = static_cast<double>(readFloat(self.get(), *m, "ka_D1"));
        return std::pair{p - v - ka * 2 * std::sinh(v / nvt), std::fabs(p)};
    };
    const auto [first, size] = residual();
    double moved = 0;
    for (int call = 0; call < 60; ++call)
    {
        const double kaBefore = static_cast<double>(readFloat(self.get(), *m, "ka_D1"));
        set(self.get(), 1);
        moved = std::fmax(moved, std::fabs(static_cast<double>(readFloat(self.get(), *m, "ka_D1")) - kaBefore));
        const auto [now, p] = residual();
        INFO("call " << call + 1 << ": residual " << first << " -> " << now);
        CHECK(std::fabs(now - first) <= 1e-5 * std::fmax(size, p));
    }
    CHECK(moved > 0);
}

TEST_CASE("a matched block's float coefficients are the double design's, and its tick its response", "[circuit]")
{
    // rc8k.cir's one-pole and sallen-key.cir's biquad, through the JIT at 44.1, 48 and 96 kHz: init computes the design
    // from the block's trapezoidal form in float, within 1e-5 of the double design at 44.1 and 48 kHz (of the largest
    // numerator coefficient for b0, b1 and b2; 1e-4 at 96 kHz, where its poles sit nearer z = 1), and tick, a sine at
    // 105, 996 and 3012 Hz, is the design's response there within 0.001 dB; both channel counts compile nonblocking
    for (const std::string& text : {fc::test::kRc8k, fc::test::kSallenKey2k})
    {
        const fc::Result r = fc::generate(text, "t.cir");
        INFO(r.errors << r.warnings);
        REQUIRE(r.errors.empty());
        CHECK(r.warnings.empty());
        auto program = compiled(r);
        for (const char* method : {"init", "tick", "clear"})
            CHECK(program->nonBlocking(r.module + "::" + method));
        fc::Options stereo;
        stereo.channels = 2;
        auto program2 = compiled(fc::generate(text, "t.cir", stereo));
        CHECK(program2->nonBlocking(r.module + "::tick"));
        const Program::Module* m = program->module(r.module);
        REQUIRE(m);
        std::string errors;
        auto* init = program->function<void(void*, float)>(r.module + "::init", errors);
        auto* tick = program->function<float(void*, float)>(r.module + "::tick", errors);
        REQUIRE(init);
        REQUIRE(tick);
        const fc::Plan plan = planned(text);
        const fc::Block& block = plan.blocks[0];
        REQUIRE(block.design == fc::Block::Design::Matched);
        for (double rate : {44100.0, 48000.0, 96000.0})
        {
            const fc::Matched<double> d = fc::matchedDesign(*fc::trForm(plan, 0, rate, {}), block.matchedPolarity);
            auto self = instance(*m);
            init(self.get(), static_cast<float>(rate));
            const double top = std::fmax(std::fabs(d.b0), std::fmax(std::fabs(d.b1), std::fabs(d.b2)));
            const bool two = std::any_of(m->fields.begin(), m->fields.end(),
                                         [](const Program::Module::Field& f) { return f.name == "mb2_b1"; });
            std::vector<std::tuple<std::string, double, double>> fields{
                {"mb0_b1", d.b0, top}, {"mb1_b1", d.b1, top}, {"ma1_b1", d.a1, 1.0}};
            if (two)
            {
                fields.emplace_back("mb2_b1", d.b2, top);
                fields.emplace_back("ma2_b1", d.a2, 1.0);
            }
            for (const auto& [name, value, scale] : fields)
            {
                const double f = static_cast<double>(readFloat(self.get(), *m, name));
                INFO(r.module << " at " << rate << ": " << name << " " << f << " against " << value);
                CHECK(std::fabs(f - value) <= (rate <= 48000 ? 1e-5 : 1e-4) * scale);
            }
            for (int bin : {9, 85, 257})
            {
                const double hz = bin * rate / 4096;
                auto run = instance(*m);
                init(run.get(), static_cast<float>(rate));
                std::vector<double> y(8192);
                for (std::size_t i = 0; i < y.size(); ++i)
                {
                    const double phase = 2 * std::numbers::pi * bin * static_cast<double>(i) / 4096;
                    y[i] = static_cast<double>(tick(run.get(), static_cast<float>(std::sin(phase))));
                }
                const double measured = fc::binMagnitude(std::span<const double>(y).subspan(4096, 4096), bin);
                const double expected = std::abs(fc::matchedResponse(d, hz, rate));
                INFO(r.module << " at " << rate << ", " << hz << " Hz: " << measured << " against " << expected);
                CHECK(std::fabs(20 * std::log10(measured / expected)) <= 1e-3);
            }
        }
    }

    // the matched step's output is ym_, a name no input can take: an input named y compiles, and runs as u does
    CHECK(fc::isModuleWord("ym_"));
    for (const std::string& text : {fc::test::kRc8k, fc::test::kSallenKey2k})
    {
        const fc::Result u = fc::generate(text, "t.cir");
        const fc::Result y = fc::generate(withLine(text, ".input u in", ".input y in"), "t.cir");
        CHECK(y.source.find("const float ym_ = mb0_b1 * y + s1_b1;") != std::string::npos);
        auto pu = compiled(u);
        auto py = compiled(y);
        std::string errors;
        auto* initU = pu->function<void(void*, float)>(u.module + "::init", errors);
        auto* tickU = pu->function<float(void*, float)>(u.module + "::tick", errors);
        auto* initY = py->function<void(void*, float)>(y.module + "::init", errors);
        auto* tickY = py->function<float(void*, float)>(y.module + "::tick", errors);
        REQUIRE(initU);
        REQUIRE(tickU);
        REQUIRE(initY);
        REQUIRE(tickY);
        const Program::Module* mu = pu->module(u.module);
        const Program::Module* my = py->module(y.module);
        REQUIRE(mu);
        REQUIRE(my);
        auto su = instance(*mu);
        auto sy = instance(*my);
        initU(su.get(), 48000.0f);
        initY(sy.get(), 48000.0f);
        for (int i = 0; i < 256; ++i)
        {
            const auto x = static_cast<float>(std::sin(0.13 * i));
            const float a = tickU(su.get(), x);
            const float b = tickY(sy.get(), x);
            REQUIRE(std::memcmp(&a, &b, sizeof a) == 0);
        }
    }
}

TEST_CASE("names keep their case, and a pot's sections have names of their own", "[circuit]")
{
    // FMinus's names keep their case: a pot tone in module Tone, a pot u beside .input U
    for (const std::string& text : {".title Tone\n.input u in\n.output y a\n.pot tone\nR1 in a 1k\nP1 a 0 10k {tone}\n",
                                    ".title Up\n.input U in\n.output y a\n.pot u\nR1 in a 1k\nP1 a 0 10k {u}\n"})
    {
        INFO(text);
        compiled(fc::generate(text, "t.cir"));
    }
    // a part named as another's section: Ptone's sections are Ptone_a_2 and Ptone_b, beside the rheostat Ptone_a
    const fc::Result r = fc::generate(".title Sections\n.input u in\n.output y b\n.pot tone\nR1 in a 1k\n"
                                      "Ptone a w b 25k {tone}\nPtone_a w 0 10k {tone}\nC1 b 0 10n\nR2 b 0 10k\n",
                                      "t.cir");
    CHECK(r.source.find("const float r_Ptone_a_2 = 25000.0f * (1 - t_Ptone);") != std::string::npos);
    CHECK(r.source.find("const float r_Ptone_a = 10000.0f * fminf(fmaxf(tone, 0), 1);") != std::string::npos);
    compiled(r);
}

TEST_CASE("Result's fields: the inputs, the latency, and each line's netlist line", "[circuit]")
{
    const fc::Result r = fc::generate(fc::test::kScreamer, "screamer.cir");
    REQUIRE(r.errors.empty());
    REQUIRE(r.inputs.size() == 1);
    CHECK(r.inputs[0].name == "u");
    CHECK(r.inputs[0].kind == fc::Input::Kind::Voltage);
    CHECK(r.inputs[0].scale == 1);
    CHECK(r.switches.empty());
    CHECK(!r.provisional);
    CHECK(r.selfTestKey.empty());   // the netlist names its whole configuration
    // the IIR halfbands' round trip at 4x (3.5 host samples at 1 kHz), and ADAA's eighth of a sample
    CHECK(r.latency == fc::roundTripDelay(4, fc::Halfband::Iir, 1000) + 0.125);
    CHECK(r.notes.find(std::format("latency: about {:.1f} host samples", r.latency - 0.125)) != std::string::npos);
    // a line of the netlist for each line of the module: the frame at .title, set at the first .pot, a block's
    // methods at its first part, tick and step at .input, a part's fields at its line
    std::vector<std::string> lines;
    std::istringstream text(r.source);
    for (std::string line; std::getline(text, line);)
        lines.push_back(line);
    REQUIRE(r.lines.size() == lines.size());
    auto lineOf = [&](const std::string& start)
    {
        for (std::size_t i = 0; i < lines.size(); ++i)
        {
            if (lines[i].find(start) != std::string::npos)
                return r.lines[i];
        }
        FAIL("no line " << start);
        return -1;
    };
    auto netlistLine = [&](const std::string& start)
    {
        std::istringstream netlist(fc::test::kScreamer);
        int n = 0;
        for (std::string line; std::getline(netlist, line);)
        {
            ++n;
            if (line.rfind(start, 0) == 0)
                return n;
        }
        FAIL("no netlist line " << start);
        return -1;
    };
    CHECK(lineOf("module Screamer") == netlistLine(".title"));
    CHECK(r.lines.front() == netlistLine(".title"));   // the header
    CHECK(lineOf("void init(float rate)") == netlistLine(".title"));
    CHECK(lineOf("void set(float drive, float tone, float level)") == netlistLine(".pot    drive"));
    CHECK(lineOf("void b2_set(float drive)") == netlistLine("XU1"));
    CHECK(lineOf("float b2(float u)") == netlistLine("XU1"));
    CHECK(lineOf("float b3(float u)") == netlistLine("R7"));
    CHECK(lineOf("float tick(float u)") == netlistLine(".input"));
    CHECK(lineOf("float step(float u)") == netlistLine(".input"));
    CHECK(lineOf("    float x_C4;") == netlistLine("C4"));
    CHECK(lineOf("    float v_D1;") == netlistLine("D1"));
    CHECK(lineOf("    float gc_C6;") == netlistLine("C6"));
    CHECK(lineOf("    int ck_;") == netlistLine(".title"));
    // a netlist without .title: its frame is line 0
    const fc::Result untitled = fc::generate(".input u in\n.output y o\nR1 in o 1k\nC1 o 0 1n\n", "t.cir");
    REQUIRE(untitled.errors.empty());
    CHECK(untitled.lines.front() == 0);
    CHECK(*std::max_element(untitled.lines.begin(), untitled.lines.end()) == 4);
}

TEST_CASE("the OTA's laws compile nonblocking, with and without ADAA, in both channel counts", "[circuit]")
{
    // one stage and the two-stage low-pass, a gain of a law input (each sample's), of a pot and a law input (tick's,
    // with the pot stored by set), of a pot alone (set's) and a constant, and the low-pass with both OTAs' gains
    // reading one pot and the law input (one pv_ field for the two); ADAA, .channels 2 and .prewarp on and off
    const std::string withPot = fc::test::otaStage(".pot res 0.5\n", "{iabc * (0.5 + res)}");
    const std::string potOnly = fc::test::otaStage(".pot res 0.5\n", "{1m * (0.1 + res)}");
    const std::string constant = fc::test::otaStage("", "1m");
    std::string shared = fc::test::kOtaLp;
    shared = withLine(shared, "G1   0    y1   OTA a1 m1 {iabc}", "G1 0 y1 OTA a1 m1 {iabc * amt}");
    shared = withLine(shared, "G2   0    y2   OTA a2 m2 {iabc}", "G2 0 y2 OTA a2 m2 {iabc * amt}");
    shared = withLine(shared, ".output vout y2b\n", ".output vout y2b\n.pot amt 0.5\n");
    int count = 0;
    int index = 0;
    for (const std::string& text : {fc::test::otaStage(), fc::test::kOtaLp, withPot, potOnly, constant, shared})
    {
        for (bool adaa : {false, true})
        {
            for (int channels : {1, 2})
            {
                for (bool prewarp : {false, true})
                {
                    fc::Options o;
                    o.adaa = adaa;
                    o.channels = channels;
                    o.prewarp = prewarp;
                    o.oversample = index % 3 == 0 ? 1 : index % 3 == 1 ? 2 : 4;
                    o.solver = fc::Solver{fc::Solver::Guess::Previous, 1 + index % 3};
                    ++index;
                    const fc::Result result = fc::generate(text, "t.cir", o);
                    INFO("config " << index << ":\n" << result.errors << result.source);
                    auto program = compiled(result);
                    for (const char* method : {"init", "set", "tick", "step", "clear", "pw1_"})
                    {
                        const std::string name = result.module + "::" + method;
                        const auto& methods = program->module(result.module)->methods;
                        if (std::find(methods.begin(), methods.end(), name) == methods.end())
                            continue;
                        INFO(name);
                        CHECK(program->nonBlocking(name));
                    }
                    ++count;
                }
            }
        }
    }
    CHECK(count == 48);
    // the shared pot's module: both gains are iabc amt, from the one pv_amt that set stores
    const fc::Result r = fc::generate(shared, "t.cir");
    CHECK(r.source.find("    float pv_amt;") == r.source.rfind("    float pv_amt;"));
    auto program = compiled(r);
    std::string errors;
    const Program::Module& m = *program->module(r.module);
    auto self = instance(m);
    program->function<void(void*, float)>(r.module + "::init", errors)(self.get(), 48000);
    callSet(*program, r.module, self.get(), {0.25f});
    program->function<float(void*, float, float)>(r.module + "::tick", errors)(self.get(), 0.1f, 1e-3f);
    CHECK(readFloat(self.get(), m, "g_G1") == 1e-3f * 0.25f);
    CHECK(readFloat(self.get(), m, "g_G2") == 1e-3f * 0.25f);
}

TEST_CASE("the bracket keeps every law's iterates finite in float, through the JIT", "[circuit]")
{
    // one step from rest at 1x: p is the input's (the states are 0), and v, read from the instance, is finite and in
    // [min(0, p), max(0, p)] for p from 1e-6 to 1e6 V either way, with 0 to 3 Newton steps (an OTA's from 1: its guess
    // is the previous step's v, newton(K) with K at least 1). A pair: diodetone.cir at tone 1 (its p is the input's
    // times p_D1_by_u); an OTA: one stage at 2 mA (p = att u).
    struct Law
    {
        std::string text;
        std::string port;
        std::vector<fc::Solver> solvers;
    };
    const std::vector<Law> laws{
        {fc::test::kDiodeTone,
         "D1",
         {{fc::Solver::Guess::Omega, 0},
          {fc::Solver::Guess::Omega, 1},
          {fc::Solver::Guess::Omega, 2},
          {fc::Solver::Guess::Omega, 3}}},
        {fc::test::otaStage("", "2m"),
         "G1",
         {{fc::Solver::Guess::Previous, 1}, {fc::Solver::Guess::Previous, 2}, {fc::Solver::Guess::Previous, 3}}},
        // one diode and an asymmetric pair (halfwave.cir, asym.cir: p is the input's times p_D1_by_u)
        {fc::test::kHalfWave,
         "D1",
         {{fc::Solver::Guess::Omega, 0},
          {fc::Solver::Guess::Omega, 1},
          {fc::Solver::Guess::Omega, 3},
          {fc::Solver::Guess::Previous, 2}}},
        {fc::test::kAsym,
         "D1",
         {{fc::Solver::Guess::Omega, 0},
          {fc::Solver::Guess::Omega, 1},
          {fc::Solver::Guess::Omega, 3},
          {fc::Solver::Guess::Previous, 2}}}};
    for (const Law& l : laws)
    {
        for (const fc::Solver& solver : l.solvers)
        {
            fc::Options o;
            o.oversample = 1;
            o.adaa = false;
            o.solver = solver;
            const fc::Result r = fc::generate(l.text, "t.cir", o);
            auto program = compiled(r);
            std::string errors;
            const Program::Module& m = *program->module(r.module);
            auto init = program->function<void(void*, float)>(r.module + "::init", errors);
            const bool ota = l.port == "G1";
            for (double mag = 1e-6; mag <= 1e6; mag *= 10)
            {
                for (double sign : {1.0, -1.0})
                {
                    auto self = instance(m);
                    init(self.get(), 48000);
                    float p = 0;
                    if (ota)
                    {
                        const float u = static_cast<float>(sign * mag / (220.0 / 100220.0));
                        program->function<float(void*, float)>(r.module + "::tick", errors)(self.get(), u);
                        p = 0.0021951706f * u;   // p_G1_by_u, a literal: the stage's att
                    }
                    else
                    {
                        if (!r.pots.empty())
                            callSet(*program, r.module, self.get(), {1.0f});
                        const float k = readFloat(self.get(), m, "p_D1_by_u");
                        const float u = static_cast<float>(sign * mag) / k;
                        program->function<float(void*, float)>(r.module + "::tick", errors)(self.get(), u);
                        p = k * u;
                    }
                    const float v = readFloat(self.get(), m, "v_" + l.port);
                    INFO(l.port << " steps " << solver.steps << " p " << p);
                    CHECK(std::isfinite(v));
                    CHECK(v >= std::fmin(0.0f, p));
                    CHECK(v <= std::fmax(0.0f, p));
                }
            }
        }
    }
}

TEST_CASE("an OTA's ADAA mean is the double one wherever its control voltage goes, through the JIT", "[circuit]")
{
    // The emitted tail (Laws.cpp's text: the carry, the mean, what the step stores) in a module of its own, kd = 1 (the
    // mean of the current is then kg tanh's): a step from a point (p_P, v_P) to (p, v) on the curve p = v + kg tanh(x),
    // x = v / 2 Vt, with x_P and x over [-40, 40] (a 5 V control voltage is x = 97; tanhf is +-1 from |x| = 9), on a
    // grid and from 1e-4 to 5 either way from each x_P. The mean is finite and within 1e-5 kg of the double one
    // (Reference.cpp's, log cosh differenced in double; measured 2.9e-6 kg, float's rounding of x near 33). The log1p
    // form alone gave NaN, or 0 for -39 in log cosh's difference, on a long step back from saturation.
    const fc::Plan plan = planned(fc::test::otaStage("", "1m"));
    const fc::PortDef& port = *plan.blocks.front().port;
    fc::LawOperands o;
    o.type = "float";
    o.port = port.name;
    o.kd = "1";
    o.g = "1";
    o.solver = fc::Solver{fc::Solver::Guess::Previous, 1};
    o.adaa = true;
    const std::string& P = port.name;
    std::string text = "module Tail\n{\n    float v_" + P + ";\n    float p_" + P + ";\n";
    for (const fc::LawField& f : fc::adaaFields(port))
        text += "    float " + f.name + ";\n";
    text += "    float mean(float p, float v, float kg) [[clang::nonblocking]]\n    {\n";
    text += "        const float kgv = kg * " + fc::literal(port.nvt) + ";\n";
    for (const fc::LawLine& l : fc::adaaText(port, o))
        text += "        " + l.text + "\n";
    text += "        v_" + P + " = v;\n";
    for (const fc::LawLine& l : fc::storeText(port, o))
        text += "        " + l.text + "\n";
    text += "        return ib;\n    }\n};\n";
    INFO(text);
    std::string errors;
    auto program = Program::compile(text, errors);
    INFO(errors);
    REQUIRE(program);
    const Program::Module& m = *program->module("Tail");
    auto mean = program->function<float(void*, float, float, float)>("Tail::mean", errors);
    const double s = port.nvt;
    for (const double kg : {0.05, 0.5, 5.0})
    {
        double worst = 0;
        std::string where;
        std::size_t pairs = 0;
        auto pair = [&](double xp, double x)
        {
            if (std::fabs(x) > 40)
                return;
            const auto vpf = static_cast<float>(xp * s);
            const auto vf = static_cast<float>(x * s);
            const auto ppf =
                static_cast<float>(static_cast<double>(vpf) + kg * std::tanh(static_cast<double>(vpf) / s));
            const auto pf = static_cast<float>(static_cast<double>(vf) + kg * std::tanh(static_cast<double>(vf) / s));
            auto self = instance(m);
            mean(self.get(), ppf, vpf, static_cast<float>(kg));
            const auto got = static_cast<double>(mean(self.get(), pf, vf, static_cast<float>(kg)));
            // the reference's mean (Reference.cpp's stepTanh), kd = 1, on the same floats
            const auto vp = static_cast<double>(vpf);
            const auto v = static_cast<double>(vf);
            const auto pp = static_cast<double>(ppf);
            const auto p = static_cast<double>(pf);
            const double vm = 0.5 * (v + vp);
            const double pm = 0.5 * (p + pp);
            const double dlc = fc::logCosh(v / s) - fc::logCosh(vp / s);
            const double vbar = std::fabs(p - pp) > 1e-5 ? vm + ((v - vp) * (pm - vm) - kg * s * dlc) / (p - pp) : vm;
            const double error = std::isfinite(got) ? std::fabs(got - (pm - vbar)) / kg : 1e30;
            ++pairs;
            if (error > worst)
            {
                worst = error;
                where = std::format("x_P {:g}, x {:g}: {:g}, not {:g}", xp, x, got, pm - vbar);
            }
        };
        for (int i = -80; i <= 80; ++i)
        {
            for (int j = -80; j <= 80; ++j)
                pair(0.5 * i, 0.5 * j);
            for (const double d : {1e-4, 1e-3, 0.01, 0.1, 0.49, 0.999, 1.0, 1.001, 2.0, 5.0})
            {
                pair(0.5 * i, 0.5 * i + d);
                pair(0.5 * i, 0.5 * i - d);
            }
        }
        INFO("kg " << kg << ", " << pairs << " steps; worst " << worst << " kg at " << where);
        CHECK(worst <= 1e-5);
    }
}

TEST_CASE("an OTA driven far past its linear range runs finite with ADAA, and as the double reference does",
          "[circuit]")
{
    // otaStage with 10k / 10k attenuators (att = 1/2): a 5 V sine drives the control voltage to 2.5 V (x = 48), and a
    // step of its ADAA mean from saturation back toward 0 crosses tanhf's +-1. At 1x, newton(3), 10 uA and 2 mA, a 3
    // kHz sine: every output finite, and within 2e-5 V of the double reference run with the same steps and ADAA
    // (Reference.cpp's simulate, the block's Holters form at its design rate, 2/3 of 48 kHz; measured 3.9e-6 V). At 4x
    // (a region, its halfbands) a 10 kHz sine: finite. The log1p form alone gave 7500 outputs of 12000 non-finite.
    std::string hot = fc::test::otaStage();
    for (const char* from : {"R1 in a1 100k\nR2 a1 0 220\nR3 yb a2 100k\nR4 a2 0 220\n"})
        hot = withLine(hot, from, "R1 in a1 10k\nR2 a1 0 10k\nR3 yb a2 10k\nR4 a2 0 10k\n");
    for (const int oversample : {1, 4})
    {
        fc::Options o;
        o.oversample = oversample;
        o.adaa = true;
        o.solver = fc::Solver{fc::Solver::Guess::Previous, 3};
        const fc::Result r = fc::generate(hot, "t.cir", o);
        auto program = compiled(r);
        std::string errors;
        const Program::Module& m = *program->module(r.module);
        auto init = program->function<void(void*, float)>(r.module + "::init", errors);
        auto tick = program->function<float(void*, float, float)>(r.module + "::tick", errors);
        const fc::Plan plan = planned(hot, o);
        for (const double iabc : {1e-5, 2e-3})
        {
            const double f = oversample == 1 ? 3000 : 10000;
            std::vector<double> u(4800);
            for (std::size_t i = 0; i < u.size(); ++i)
                u[i] = 5 * std::sin(2 * std::numbers::pi * f * static_cast<double>(i) / 48000);
            auto self = instance(m);
            init(self.get(), 48000);
            std::vector<double> y(u.size());
            std::size_t notFinite = 0;
            for (std::size_t i = 0; i < u.size(); ++i)
            {
                y[i] = static_cast<double>(tick(self.get(), static_cast<float>(u[i]), static_cast<float>(iabc)));
                notFinite += std::isfinite(y[i]) ? 0 : 1;
            }
            INFO(oversample << "x, iabc " << iabc);
            CHECK(notFinite == 0);
            if (oversample != 1)
                continue;
            const std::vector<double> laws{iabc};
            const fc::Holters h = fc::holters(plan, 0, 48000.0 * 2 / 3, {}, laws);
            fc::RefOptions ro;
            ro.adaa = true;
            ro.steps = 3;
            ro.guess = fc::Solver::Guess::Previous;
            std::vector<double> uf(u.size());
            for (std::size_t i = 0; i < u.size(); ++i)
                uf[i] = static_cast<double>(static_cast<float>(u[i]));
            const std::vector<double> ref = fc::simulate(h, uf, ro);
            double worst = 0;
            for (std::size_t i = 0; i < u.size(); ++i)
                worst = std::max(worst, std::fabs(y[i] - ref[i]));
            INFO("worst " << worst << " V");
            CHECK(worst <= 2e-5);
        }
    }
}

TEST_CASE("Phase 1's elements and diode laws compile nonblocking, in both channel counts", "[circuit]")
{
    // an inductor, DC sources (V, I, a floating V), E and G sources with constant and pot gains, one diode, a string,
    // an asymmetric pair, RS: each with ADAA off and on, each solver's guess, .channels 1 and 2
    const std::string sources = ".title Sources\n.input u in\n.output y out\n.pot x 0.3\nC0 in b 1u\nV1 vcc 0 9\n"
                                "R1 vcc b 10k\nR2 b 0 10k\nI1 0 b 10u\nV2 c b 0\nR3 c 0 1meg\nL1 c d 10m\nR4 d 0 1k\n"
                                "C4 d e0 1u\nR7 e0 0 10k\nE1 e 0 e0 0 {2 + x}\nR5 e f 1k\nC5 f 0 10n\nD1 f 0 1N4148\n"
                                "X1 f out out opamp\n";
    const std::string rs = ".title Rs\n.input u in\n.output y c\nR1 in a 2.2k\nC1 a 0 10n\nD1 a m DR\nD2 m 0 DR\n"
                           "X1 a b b opamp\nR2 b c 1k\nC2 c 0 10n\nD3 c 0 DR\nD4 0 c DR\n"
                           ".model DR D(IS=2.52n N=1.752 RS=5)\n";
    int count = 0;
    int index = 0;
    for (const std::string& text : {fc::test::kWah, fc::test::kRlLowpass, fc::test::kDivider, kGains,
                                    fc::test::kHalfWave, fc::test::kAsym, kSinglePot, kAsymPot, sources, rs})
    {
        for (bool adaa : {false, true})
        {
            for (int channels : {1, 2})
            {
                fc::Options o;
                o.adaa = adaa;
                o.channels = channels;
                o.oversample = index % 3 == 0 ? 1 : index % 3 == 1 ? 2 : 4;
                o.solver = index % 2 == 0 ? fc::Solver{fc::Solver::Guess::Omega, 1 + index % 3}
                                          : fc::Solver{fc::Solver::Guess::Previous, 1 + index % 3};
                ++index;
                const fc::Result result = fc::generate(text, "t.cir", o);
                INFO("config " << index << ":\n" << result.errors << result.source);
                auto program = compiled(result);
                for (const char* method : {"init", "set", "tick", "step", "clear"})
                {
                    const std::string name = result.module + "::" + method;
                    const auto& methods = program->module(result.module)->methods;
                    if (std::find(methods.begin(), methods.end(), name) == methods.end())
                        continue;
                    INFO(name);
                    CHECK(program->nonBlocking(name));
                }
                ++count;
            }
        }
    }
    CHECK(count == 40);
}

TEST_CASE("a pot's move keeps one diode's and an asymmetric pair's ADAA point on the curve", "[circuit]")
{
    // b<N>_set's carry, p_D1 += (ka' - ka) (c_D1 - 1) (and the other diode's - (kr' - kr) (cr_D1 - 1)): the last
    // step's Newton residual, p_D1 - v_D1 - K f(v_D1), is kept to 1e-5 of p_D1 as the pot glides 0 -> 1 (R1 100 ohm
    // to 20.1k: K moves by 4.8 times), as check h's for the pair
    for (const std::string& text : {kSinglePot, kAsymPot})
    {
        const fc::Result r = fc::generate(text, "t.cir");
        auto program = compiled(r);
        const Program::Module* m = program->module(r.module);
        REQUIRE(m);
        std::string errors;
        auto* init = program->function<void(void*, float)>(r.module + "::init", errors);
        auto* set = program->function<void(void*, float)>(r.module + "::set", errors);
        auto* tick = program->function<float(void*, float)>(r.module + "::tick", errors);
        REQUIRE(init);
        auto self = instance(*m);
        init(self.get(), 48000);
        set(self.get(), 0);
        const bool asym = r.module == "AsymPot";
        const double s = fc::thermalVoltage(1.752);
        const double s2 = fc::thermalVoltage(2 * 1.752);
        struct Point
        {
            double residual;
            double p;
            double curve;
        };
        auto point = [&]
        {
            const auto v = static_cast<double>(readFloat(self.get(), *m, "v_D1"));
            const auto p = static_cast<double>(readFloat(self.get(), *m, "p_D1"));
            double curve = static_cast<double>(readFloat(self.get(), *m, "ka_D1")) * (std::exp(v / s) - 1);
            if (asym)
                curve -= static_cast<double>(readFloat(self.get(), *m, "kr_D1")) * (std::exp(-v / s2) - 1);
            return Point{p - v - curve, p, curve};
        };
        // a 2 V, 996 Hz sine, to where the diode conducts (its forward side, and the asymmetric pair's other side)
        for (double side : {1.0, -1.0})
        {
            set(self.get(), 0);
            bool conducting = false;
            for (int i = 0; i < 9600 && !conducting; ++i)
            {
                tick(self.get(), static_cast<float>(2 * std::sin(2 * std::numbers::pi * 996 * i / 48000)));
                const float v = readFloat(self.get(), *m, "v_D1");
                conducting = i >= 2400 && side * static_cast<double>(v) > (side > 0 ? 0.5 : 0.95);
            }
            if (!asym && side < 0)
                continue;   // one diode: no reverse conduction
            INFO(r.module << " side " << side);
            REQUIRE(conducting);
            const Point before = point();
            set(self.get(), 1);
            const Point after = point();
            INFO("residual " << before.residual << " -> " << after.residual << ", p " << before.p << " -> " << after.p);
            CHECK(std::fabs(after.curve - before.curve) > 0.1 * std::fabs(before.p));   // the move is large
            CHECK(std::fabs(after.residual - before.residual) <=
                  1e-5 * std::fmax(std::fabs(before.p), std::fabs(after.p)));
            // a pot held changes nothing, bit for bit
            const float p = readFloat(self.get(), *m, "p_D1");
            set(self.get(), 1);
            CHECK(readFloat(self.get(), *m, "p_D1") == p);
        }
    }
}

namespace
{
    const Program::Module::Field* fieldNamed(const Program::Module& m, const std::string& name)
    {
        for (const Program::Module::Field& f : m.fields)
        {
            if (f.name == name)
                return &f;
        }
        return nullptr;
    }

    template <typename T>
    T readAt(const void* base, const Program::Module& m, const std::string& name, std::size_t index = 0)
    {
        const Program::Module::Field* f = fieldNamed(m, name);
        REQUIRE(f);
        REQUIRE((index + 1) * sizeof(T) <= f->size);
        T v{};
        std::memcpy(&v, static_cast<const char*>(base) + f->offset + index * sizeof(T), sizeof v);
        return v;
    }

    template <typename T>
    void writeAt(void* base, const Program::Module& m, const std::string& name, T v, std::size_t index = 0)
    {
        const Program::Module::Field* f = fieldNamed(m, name);
        REQUIRE(f);
        REQUIRE((index + 1) * sizeof(T) <= f->size);
        std::memcpy(static_cast<char*>(base) + f->offset + index * sizeof(T), &v, sizeof v);
    }

    fc::Options withProbes(fc::Options o = {})
    {
        o.probes = true;
        return o;
    }
} // namespace

TEST_CASE("a module's probe fields follow its others, and its methods are nonblocking in both channel counts",
          "[circuit]")
{
    int modules = 0;
    for (const std::string& text : {fc::test::kDiodeTone, fc::test::kScreamer, fc::test::kOtaLp, fc::test::kAsym,
                                    fc::test::kWah, fc::test::kToneStack, fc::test::screamerBiased()})
    {
        for (int channels : {1, 2})
        {
            fc::Options o;
            o.channels = channels;
            const fc::Result off = fc::generate(text, "t.cir", o);
            const fc::Result on = fc::generate(text, "t.cir", withProbes(o));
            INFO(on.source);
            auto p0 = compiled(off);
            auto p1 = compiled(on);
            const Program::Module* m0 = p0->module(off.module);
            const Program::Module* m1 = p1->module(on.module);
            REQUIRE(m0);
            REQUIRE(m1);
            // the module's own fields keep their names, types and offsets; the probes' come after them
            REQUIRE(m1->fields.size() > m0->fields.size());
            for (std::size_t i = 0; i < m0->fields.size(); ++i)
            {
                CHECK(m1->fields[i].name == m0->fields[i].name);
                CHECK(m1->fields[i].type == m0->fields[i].type);
                CHECK(m1->fields[i].offset == m0->fields[i].offset);
            }
            const std::map<std::string, std::string> types{
                {"prb_", "int"},      {"prs_", "int[4]"},   {"prn_", "int"},      {"pr_", "float[4096]"},
                {"prp_", "float[4]"}, {"pra_", "float[4]"}, {"prq_", "float[4]"},
                {"prv_", std::format("float[{}]", on.nodes.size())}};
            for (const char* name : fc::kProbeFields)
            {
                INFO(name);
                CHECK(!fieldNamed(*m0, name));
                const Program::Module::Field* f = fieldNamed(*m1, name);
                REQUIRE(f);
                CHECK(f->type == types.at(name));
                CHECK(f->offset >= m0->fields.back().offset + m0->fields.back().size);
            }
            CHECK(static_cast<int>(std::size(fc::kProbeFields)) == 8);
            CHECK(fc::kProbeSlots * fc::kProbeRing == 4096);
            // every method nonblocking: the module's, and the probing tick's, step's and blocks'
            std::vector<std::string> methods{"init", "set", "tick", "step", "clear", "tick_pr", "step_pr"};
            for (std::size_t b = 1; b <= 4; ++b)
                methods.push_back(std::format("b{}_pr", b));
            CHECK(std::find(m1->methods.begin(), m1->methods.end(), on.module + "::tick_pr") != m1->methods.end());
            for (const std::string& method : methods)
            {
                const std::string name = on.module + "::" + method;
                if (std::find(m1->methods.begin(), m1->methods.end(), name) == m1->methods.end())
                    continue;
                INFO(name);
                CHECK(p1->nonBlocking(name));
            }
            ++modules;
        }
    }
    CHECK(modules == 14);
}

TEST_CASE("init, clear, the ring and the slots of a module's probes", "[circuit]")
{
    const fc::Result off = fc::generate(fc::test::kScreamer, "screamer.cir");
    const fc::Result on = fc::generate(fc::test::kScreamer, "screamer.cir", withProbes());
    auto p0 = compiled(off);
    auto p1 = compiled(on);
    const Program::Module* m0 = p0->module("Screamer");
    const Program::Module* m = p1->module("Screamer");
    REQUIRE(m0);
    REQUIRE(m);
    std::string errors;
    auto* init0 = p0->function<void(void*, float)>("Screamer::init", errors);
    auto* tick0 = p0->function<float(void*, float)>("Screamer::tick", errors);
    auto* init = p1->function<void(void*, float)>("Screamer::init", errors);
    auto* tick = p1->function<float(void*, float)>("Screamer::tick", errors);
    auto* clear = p1->function<void(void*)>("Screamer::clear", errors);
    REQUIRE(init0);
    REQUIRE(tick0);
    REQUIRE(init);
    REQUIRE(tick);
    REQUIRE(clear);
    const std::size_t nodes = on.nodes.size();
    REQUIRE(nodes == 14);
    REQUIRE(on.nodes[1].name == "out");
    REQUIRE(on.nodes[3].name == "n1");
    REQUIRE(!on.nodes[7].readable);   // t1, inside the matched tone block

    // init: off, each slot on no node, nothing counted; it leaves the rings, the host's memory (here not zeroed)
    auto self = instance(*m);
    std::memset(self.get(), 0x5a, m->size);
    init(self.get(), 48000);
    CHECK(readAt<int>(self.get(), *m, "prb_") == 0);
    CHECK(readAt<int>(self.get(), *m, "prn_") == 0);
    for (std::size_t k = 0; k < 4; ++k)
    {
        CHECK(readAt<int>(self.get(), *m, "prs_", k) == -1);
        CHECK(readAt<float>(self.get(), *m, "prp_", k) == 0);
        CHECK(readAt<float>(self.get(), *m, "pra_", k) == 0);
        CHECK(readAt<float>(self.get(), *m, "prq_", k) == 0);
    }
    for (std::size_t k = 0; k < nodes; ++k)
        CHECK(readAt<float>(self.get(), *m, "prv_", k) == 0);
    {
        const Program::Module::Field* ring = fieldNamed(*m, "pr_");
        REQUIRE(ring);
        const auto* bytes = static_cast<const unsigned char*>(self.get()) + ring->offset;
        CHECK(std::all_of(bytes, bytes + ring->size, [](unsigned char b) { return b == 0x5a; }));
        std::memset(static_cast<char*>(self.get()) + ring->offset, 0, ring->size);
    }

    // armed: slot 0 on n1 (the clipping stage's, at its last step of four), slot 1 on t1 (not readable), slot 2 on
    // a node out of range, slot 3 on .output's node: the output bits are the module's without probes
    writeAt<int>(self.get(), *m, "prb_", 1);
    for (const auto& [slot, node] : {std::pair{0, 3}, std::pair{1, 7}, std::pair{2, 99}, std::pair{3, 1}})
        writeAt<int>(self.get(), *m, "prs_", node, static_cast<std::size_t>(slot));
    auto plain = instance(*m0);
    init0(plain.get(), 48000);
    constexpr int n = 3000;
    int differ = 0;
    float peak = 0;
    float sum = 0;
    float squares = 0;
    std::vector<float> ys;
    for (int i = 0; i < n; ++i)
    {
        const auto u = static_cast<float>(0.45 * std::sin(2 * std::numbers::pi * 996 * i / 48000));
        const float y0 = tick0(plain.get(), u);
        const float y = tick(self.get(), u);
        differ += std::memcmp(&y0, &y, sizeof y) != 0 ? 1 : 0;
        ys.push_back(y);
        // .output's node: tick's output times its scale (1), its DC point 0
        peak = std::fmax(peak, std::fabs(y));
        sum = sum + y;
        squares = squares + y * y;
    }
    CHECK(differ == 0);
    CHECK(readAt<int>(self.get(), *m, "prn_") == n);
    int written = 0;
    int unwritten = 0;
    int outputRing = 0;
    for (std::size_t j = 0; j < 1024; ++j)
    {
        written += readAt<float>(self.get(), *m, "pr_", j) != 0 ? 1 : 0;
        unwritten += readAt<float>(self.get(), *m, "pr_", 1024 + j) != 0 ? 1 : 0;
        unwritten += readAt<float>(self.get(), *m, "pr_", 2048 + j) != 0 ? 1 : 0;
    }
    // the sample written at prn_ = i is at i % 1024: slot 3's ring holds the last 1024 outputs
    for (int i = n - 1024; i < n; ++i)
    {
        const float r = readAt<float>(self.get(), *m, "pr_", static_cast<std::size_t>(3 * 1024 + i % 1024));
        outputRing += std::memcmp(&r, &ys[static_cast<std::size_t>(i)], sizeof r) == 0 ? 1 : 0;
    }
    CHECK(written > 1000);
    CHECK(unwritten == 0);
    CHECK(outputRing == 1024);
    for (std::size_t k : {1u, 2u})
    {
        CHECK(readAt<float>(self.get(), *m, "prp_", k) == 0);
        CHECK(readAt<float>(self.get(), *m, "pra_", k) == 0);
        CHECK(readAt<float>(self.get(), *m, "prq_", k) == 0);
    }
    // the stats: the peak, the sum and the sum of squares of every sample written since init, in float
    CHECK(readAt<float>(self.get(), *m, "prp_", 3) == peak);
    CHECK(readAt<float>(self.get(), *m, "pra_", 3) == sum);
    CHECK(readAt<float>(self.get(), *m, "prq_", 3) == squares);
    CHECK(readAt<float>(self.get(), *m, "prp_", 0) > 0);

    // clear leaves every probe field (they are the host's), whatever it resets
    std::vector<unsigned char> before(m->size);
    std::memcpy(before.data(), self.get(), m->size);
    clear(self.get());
    for (const char* name : fc::kProbeFields)
    {
        INFO(name);
        const Program::Module::Field* f = fieldNamed(*m, name);
        REQUIRE(f);
        CHECK(std::memcmp(before.data() + f->offset, static_cast<const char*>(self.get()) + f->offset, f->size) == 0);
    }

    // prn_ wraps at 2^30: the sample written at 2^30 - 1 is the ring's last
    writeAt<int>(self.get(), *m, "prn_", (1 << 30) - 1);
    const float y = tick(self.get(), 0.1f);
    CHECK(readAt<int>(self.get(), *m, "prn_") == 0);
    CHECK(readAt<float>(self.get(), *m, "pr_", 3 * 1024 + 1023) == y);

    // disarmed, nothing is written, and prn_ stays where it is
    writeAt<int>(self.get(), *m, "prb_", 0);
    writeAt<float>(self.get(), *m, "pra_", 0.0f, 3);
    for (int i = 0; i < 100; ++i)
        tick(self.get(), 0.2f);
    CHECK(readAt<int>(self.get(), *m, "prn_") == 0);
    CHECK(readAt<float>(self.get(), *m, "pra_", 3) == 0);
}

TEST_CASE("a probe's node that reads an ADAA block's input, which no readout of it reads, keeps that input's history",
          "[circuit]")
{
    // R0 and R1 halve the way from the input to the port: V(m) = (u + v) / 2, ADAA's means (ua and vb) in the step;
    // the block's readouts read v alone, so only the probe needs u a step earlier, pru_b1 (not a field the guard sums)
    const std::string text = ".title Halves\n.input u in\n.output y a\n.oversample 1\n.adaa on\n.solver omega(1)\n"
                             "R0 in m 1k\nR1 m a 1k\nC1 a 0 10n\nD1 a 0 1N914\nD2 0 a 1N914\n";
    const fc::Result off = fc::generate(text, "t.cir");
    const fc::Result on = fc::generate(text, "t.cir", withProbes());
    INFO(on.source);
    CHECK(off.source.find("pru_") == std::string::npos);
    CHECK(off.source.find("uo_") == std::string::npos);
    CHECK(on.source.find("    float pru_b1;") != std::string::npos);
    CHECK(on.source.find("        pru_b1 = 0;") != std::string::npos);   // clear's: the DC point
    CHECK(on.source.find("const float ua = 0.5f * (u + pru_b1);") != std::string::npos);
    {
        const std::size_t sum = on.source.find("float sum_ = ");
        REQUIRE(sum != std::string::npos);
        CHECK(on.source.substr(sum, on.source.find('\n', sum) - sum).find("pru_") == std::string::npos);
    }
    REQUIRE(on.nodes.size() == 3);
    REQUIRE(on.nodes[2].name == "m");
    auto p0 = compiled(off);
    auto p1 = compiled(on);
    const Program::Module* m0 = p0->module("Halves");
    const Program::Module* m = p1->module("Halves");
    REQUIRE(m0);
    REQUIRE(m);
    std::string errors;
    auto* init0 = p0->function<void(void*, float)>("Halves::init", errors);
    auto* tick0 = p0->function<float(void*, float)>("Halves::tick", errors);
    auto* init = p1->function<void(void*, float)>("Halves::init", errors);
    auto* tick = p1->function<float(void*, float)>("Halves::tick", errors);
    REQUIRE(init0);
    REQUIRE(tick0);
    REQUIRE(init);
    REQUIRE(tick);
    CHECK(p1->nonBlocking("Halves::tick_pr"));
    CHECK(p1->nonBlocking("Halves::b1_pr"));
    auto plain = instance(*m0);
    auto self = instance(*m);
    init0(plain.get(), 48000);
    init(self.get(), 48000);
    // armed after 500 samples: the history is kept while disarmed too, so the first armed sample is right
    int differ = 0;
    double worst = 0;
    float before = 0;
    for (int i = 0; i < 2000; ++i)
    {
        if (i == 500)
        {
            writeAt<int>(self.get(), *m, "prb_", 1);
            writeAt<int>(self.get(), *m, "prs_", 2, 0);
        }
        const auto u = static_cast<float>(2 * std::sin(2 * std::numbers::pi * 1000 * i / 48000));
        const float y0 = tick0(plain.get(), u);
        const float y = tick(self.get(), u);
        differ += std::memcmp(&y0, &y, sizeof y) != 0 ? 1 : 0;
        if (i >= 500)
        {
            const float ring = readAt<float>(self.get(), *m, "pr_", static_cast<std::size_t>(i - 500) % 1024);
            const double ua = 0.5 * (static_cast<double>(u) + static_cast<double>(before));
            const double expected = 0.5 * ua + 0.5 * static_cast<double>(y);
            worst = std::fmax(worst, std::fabs(static_cast<double>(ring) - expected));
        }
        before = u;
    }
    CHECK(differ == 0);
    INFO("V(m) against (ua + vb) / 2: " << worst);
    CHECK(worst <= 1e-6);
}

TEST_CASE("probes armed on every node leave the output's bits in every configuration", "[circuit]")
{
    // the configurations example-circuits' check q does not ship: BDF2, the FIR halfbands, 2x and 8x, every block in
    // the region, one channel and two; each readable node in turn on slot 0, the others on the next ones
    struct Config
    {
        const std::string* text;
        int oversample;
        fc::Halfband halfband;
        fc::Region region;
        bool adaa;
        std::optional<fc::Method> method;
        int channels;
    };
    const std::vector<Config> configs{
        {&fc::test::kDiodeTone, 4, fc::Halfband::Iir, fc::Region::Auto, false, fc::Method::Bdf2, 1},
        {&fc::test::kDiodeTone, 2, fc::Halfband::Fir, fc::Region::Auto, true, std::nullopt, 2},
        {&fc::test::kScreamer, 8, fc::Halfband::Fir, fc::Region::All, true, std::nullopt, 1},
        {&fc::test::kScreamer, 2, fc::Halfband::Iir, fc::Region::Nonlinear, false, fc::Method::Bdf2, 2},
        {&kSinglePot, 1, fc::Halfband::Iir, fc::Region::Auto, true, std::nullopt, 1},
    };
    for (const Config& c : configs)
    {
        fc::Options o;
        o.oversample = c.oversample;
        o.halfband = c.halfband;
        o.region = c.region;
        o.adaa = c.adaa;
        o.method = c.method;
        o.channels = c.channels;
        const fc::Result off = fc::generate(*c.text, "t.cir", o);
        const fc::Result on = fc::generate(*c.text, "t.cir", withProbes(o));
        INFO(on.configuration << ", " << c.channels << " channel(s)");
        auto p0 = compiled(off);
        auto p1 = compiled(on);
        const Program::Module* m0 = p0->module(off.module);
        const Program::Module* m = p1->module(on.module);
        REQUIRE(m0);
        REQUIRE(m);
        std::string errors;
        auto* init0 = p0->function<void(void*, float)>(off.module + "::init", errors);
        auto* init = p1->function<void(void*, float)>(on.module + "::init", errors);
        REQUIRE(init0);
        REQUIRE(init);
        std::vector<int> readable;
        for (std::size_t k = 0; k < on.nodes.size(); ++k)
        {
            if (on.nodes[k].readable)
                readable.push_back(static_cast<int>(k));
        }
        REQUIRE(!readable.empty());
        for (std::size_t first = 0; first < readable.size(); ++first)
        {
            auto plain = instance(*m0);
            auto self = instance(*m);
            init0(plain.get(), 48000);
            init(self.get(), 48000);
            writeAt<int>(self.get(), *m, "prb_", 1);
            for (std::size_t k = 0; k < 4; ++k)
                writeAt<int>(self.get(), *m, "prs_", readable[(first + k) % readable.size()], k);
            int differ = 0;
            for (int i = 0; i < 1500; ++i)
            {
                const auto x = static_cast<float>(1.5 * std::sin(2 * std::numbers::pi * 997 * i / 48000));
                if (i % 16 == 0)
                {
                    const float pot = static_cast<float>(i % 960) / 960;
                    std::vector<float> pots(off.pots.size(), pot);
                    callSet(*p0, off.module, plain.get(), pots);
                    callSet(*p1, on.module, self.get(), pots);
                }
                if (c.channels == 2)
                {
                    using float2 = float __attribute__((ext_vector_type(2)));
                    auto* tick0 = p0->function<float2(void*, float2)>(off.module + "::tick", errors);
                    auto* tick = p1->function<float2(void*, float2)>(on.module + "::tick", errors);
                    REQUIRE(tick0);
                    REQUIRE(tick);
                    const float2 u{x, -0.5f * x};
                    const float2 y0 = tick0(plain.get(), u);
                    const float2 y = tick(self.get(), u);
                    differ += std::memcmp(&y0, &y, sizeof y) != 0 ? 1 : 0;
                    continue;
                }
                auto* tick0 = p0->function<float(void*, float)>(off.module + "::tick", errors);
                auto* tick = p1->function<float(void*, float)>(on.module + "::tick", errors);
                REQUIRE(tick0);
                REQUIRE(tick);
                const float y0 = tick0(plain.get(), x);
                const float y = tick(self.get(), x);
                differ += std::memcmp(&y0, &y, sizeof y) != 0 ? 1 : 0;
            }
            INFO("slot 0 on " << on.nodes[static_cast<std::size_t>(readable[first])].name);
            CHECK(differ == 0);
            CHECK(readAt<int>(self.get(), *m, "prn_") == 1500);
        }
    }
}
