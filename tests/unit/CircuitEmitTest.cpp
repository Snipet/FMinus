// tests/unit/CircuitEmitTest.cpp: the circuit compiler's FMinus text, compiled through fminus::Program: its
// coefficients against the double derivation they print, every configuration compiling with its methods
// nonblocking, and generation deterministic with a layout that no pot value changes.
#include "circuit/Emit.h"
#include "circuit/Mna.h"
#include "circuit/Netlist.h"
#include "circuit/Plan.h"
#include "CircuitNetlists.h"
#include "fminus/Circuit.h"
#include "fminus/FMinus.h"

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdlib>
#include <cstring>
#include <locale>
#include <map>
#include <memory>
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
