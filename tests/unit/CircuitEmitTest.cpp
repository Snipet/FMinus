// tests/unit/CircuitEmitTest.cpp: the circuit compiler's FMinus text, compiled through fminus::Program: its
// coefficients against the double derivation they print, every configuration compiling with its methods
// nonblocking, and generation deterministic with a layout that no pot value changes.
#include "circuit/Emit.h"
#include "circuit/Halfbands.h"
#include "circuit/Laws.h"
#include "circuit/Mna.h"
#include "circuit/Netlist.h"
#include "circuit/Plan.h"
#include "circuit/Reference.h"
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

    // Calls set() with the pots, whatever their count (1 to 3 in these netlists).
    void callSet(Program& program, const std::string& module, void* self, const std::vector<float>& pots)
    {
        std::string errors;
        const std::string name = module + "::set";
        if (pots.size() == 1)
            program.function<void(void*, float)>(name, errors)(self, pots[0]);
        else if (pots.size() == 3)
            program.function<void(void*, float, float, float)>(name, errors)(self, pots[0], pots[1], pots[2]);
        else
            FAIL("set() takes " << pots.size() << " pots");
    }
} // namespace

TEST_CASE("the emitted coefficients are the double derivation's, in float", "[circuit]")
{
    const std::string netlists[] = {fc::test::kDiodeTone, fc::test::kScreamer,
                                    withLine(fc::test::kDiodeTone, ".adaa off", ".adaa on"),
                                    withLine(fc::test::kScreamer, ".adaa on", ".adaa off")};
    for (const std::string& text : netlists)
    {
        const fc::Result result = fc::generate(text, "t.cir");
        auto program = compiled(result);
        const Program::Module* m = program->module(result.module);
        REQUIRE(m);
        const fc::Plan plan = planned(text);
        std::string errors;
        auto* init = program->function<void(void*, float)>(result.module + "::init", errors);
        REQUIRE(init);
        for (double rate : {44100.0, 48000.0, 96000.0})
        {
            for (std::size_t b = 0; b < plan.blocks.size(); ++b)
            {
                const fc::Block& block = plan.blocks[b];
                fc::Messages messages;
                const std::optional<fc::BlockCode> code = fc::deriveBlock(plan, static_cast<int>(b), messages);
                REQUIRE(code);
                const double designRate = rate * block.rateFactor * block.designFactor;
                for (const std::vector<double>& pots : fc::potGrid(plan, block))
                {
                    auto self = instance(*m);
                    init(self.get(), static_cast<float>(rate));
                    std::vector<float> args;
                    for (double v : pots)
                        args.push_back(static_cast<float>(v));
                    callSet(*program, result.module, self.get(), args);
                    const std::vector<double> symbols =
                        fc::symbolValues(code->system, plan, static_cast<int>(b), designRate, pots);
                    const fc::Linear<double> linear = fc::solve(code->system, code->pivots, symbols);
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
                        double scale = 1;
                        if (source.rfind("x_", 0) == 0)
                            scale = symbols[static_cast<std::size_t>(
                                std::find_if(code->system.symbols.begin(), code->system.symbols.end(),
                                             [&](const fc::System::Symbol& s)
                                             { return s.name == "gc_" + source.substr(2); }) -
                                code->system.symbols.begin())];
                        auto& row = rows[name.substr(0, by)];
                        row.second += std::fabs(d) * scale;
                        const auto field = std::find_if(code->coefficients.begin(), code->coefficients.end(),
                                                        [&](const auto& c) { return c.first == name; });
                        REQUIRE(field != code->coefficients.end());
                        if (field->second.kind != fc::Val::Kind::Var)
                            continue;   // a literal: exact
                        const double f = static_cast<double>(readFloat(self.get(), *m, name));
                        row.first = std::fmax(row.first, std::fabs(f - d) * scale);
                    }
                    for (const auto& [readout, row] : rows)
                    {
                        INFO(result.module << " block " << b + 1 << " at " << rate << ", readout " << readout);
                        CHECK(row.first <= 1e-5 * row.second);
                    }
                    if (block.port)
                    {
                        for (const char* k : {"ka_", "kb_", "lna_", "kc_"})
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
         {{fc::Solver::Guess::Previous, 1}, {fc::Solver::Guess::Previous, 2}, {fc::Solver::Guess::Previous, 3}}}};
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
