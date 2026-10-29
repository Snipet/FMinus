// tests/unit/CircuitTest.cpp: the circuit compiler's front half (src/circuit/): SPICE numbers and {expressions}, the
// netlist subset and its errors, the plan (blocks cut at ideal op amps, the oversampled region), each block's equations
// against hand derivations, Yeh's eq. 24 and textbook responses, the 1x check, the pivots, and the halfband stages;
// and the double reference and the analysis that the checks of examples/circuits run on.
#include "fminus/Circuit.h"

#include "circuit/Analysis.h"
#include "circuit/Emit.h"
#include "circuit/Halfbands.h"
#include "circuit/Mna.h"
#include "circuit/Netlist.h"
#include "circuit/Plan.h"
#include "circuit/Reference.h"
#include "CircuitNetlists.h"

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include <cmath>
#include <complex>
#include <cstddef>
#include <numbers>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace
{
    namespace fc = fminus::circuit;
    using Complex = std::complex<double>;
    using Catch::Matchers::WithinAbs;
    using Catch::Matchers::WithinRel;

    struct Parsed
    {
        fc::Netlist netlist;
        std::string errors;
        std::string warnings;
    };

    Parsed parsed(std::string_view text)
    {
        fc::Messages messages;
        Parsed p{fc::parse(text, messages), {}, {}};
        p.errors = messages.format("t.cir", fc::Severity::Error);
        p.warnings = messages.format("t.cir", fc::Severity::Warning);
        return p;
    }

    // The netlist's plan; it must have one.
    fc::Plan planned(std::string_view text, const fc::Options& options = {})
    {
        fc::Messages messages;
        const fc::Netlist netlist = fc::parse(text, messages);
        std::optional<fc::Plan> p = fc::plan(netlist, options, "t.cir", messages);
        INFO(messages.format("t.cir", fc::Severity::Error));
        REQUIRE(p);
        return *p;
    }

    // generate()'s errors for a netlist that must fail.
    std::string errorsOf(std::string_view text)
    {
        const fc::Result r = fc::generate(text, "t.cir");
        CHECK(r.source.empty());
        return r.errors;
    }

    // The one value of a netlist's first element.
    double valueOf(const std::string& value)
    {
        const Parsed p = parsed(".input u a\n.output y a\nR1 a 0 " + value + "\n");
        REQUIRE(p.netlist.elements.size() == 1);
        return p.netlist.elements[0].value;
    }

    std::vector<double> potsAt(const fc::Plan& plan, const std::string& key, double value)
    {
        std::vector<double> point;
        for (const fc::PotInfo& p : plan.pots)
            point.push_back(p.key == key ? value : p.defaultValue);
        return point;
    }

    double dB(Complex h)
    {
        return 20 * std::log10(std::abs(h));
    }

    // D + C (zI - A)^-1 B for a small state space, by Gaussian elimination in complex.
    Complex stateSpace(const fc::Holters& h, Complex z)
    {
        const auto n = static_cast<std::size_t>(h.states);
        std::vector<std::vector<Complex>> m(n, std::vector<Complex>(n + 1));
        for (std::size_t i = 0; i < n; ++i)
        {
            for (std::size_t j = 0; j < n; ++j)
                m[i][j] = (i == j ? z : Complex(0)) - h.a[i][j];
            m[i][n] = h.b[i];
        }
        for (std::size_t k = 0; k < n; ++k)
        {
            std::size_t best = k;
            for (std::size_t r = k + 1; r < n; ++r)
            {
                if (std::abs(m[r][k]) > std::abs(m[best][k]))
                    best = r;
            }
            std::swap(m[k], m[best]);
            for (std::size_t r = 0; r < n; ++r)
            {
                if (r == k)
                    continue;
                const Complex f = m[r][k] / m[k][k];
                for (std::size_t c = k; c <= n; ++c)
                    m[r][c] -= f * m[k][c];
            }
        }
        Complex y = h.dy;
        for (std::size_t i = 0; i < n; ++i)
            y += h.cy[i] * m[i][n] / m[i][i];
        return y;
    }

    // Within 1e-12 of the larger, or of `scale`: the largest entry of the matrix compared, for an entry that is a
    // cancellation in adaa.cpp's derivation and an exact zero in the generator's (example 1's Cy[0], V(b) per x_C1).
    bool near(double a, double b, double scale)
    {
        return std::fabs(a - b) <= 1e-12 * std::fmax(std::fmax(std::fabs(a), std::fabs(b)), scale);
    }
} // namespace

TEST_CASE("SPICE numbers and units", "[circuit]")
{
    CHECK(valueOf("2.2k") == 2200);
    CHECK_THAT(valueOf("10n"), WithinRel(10e-9, 1e-15));
    CHECK(valueOf("1meg") == 1e6);
    CHECK(valueOf("1MEG") == 1e6);
    CHECK_THAT(valueOf("10nF"), WithinRel(10e-9, 1e-15));
    CHECK(valueOf("4.7K") == 4700);
    CHECK(valueOf("1e3") == 1000);
    CHECK_THAT(valueOf("51p"), WithinRel(51e-12, 1e-15));
    CHECK(valueOf("2.2kohm") == 2200);
    CHECK_THAT(valueOf(".5u"), WithinRel(0.5e-6, 1e-15));
    // 1M is milli in SPICE: a warning at the value, quoting its spelling
    const Parsed p = parsed(".input u a\n.output y a\nR14 a 0 1M\nR2 a 0 4.7m\n");
    REQUIRE(p.netlist.elements.size() == 2);
    CHECK(p.netlist.elements[0].value == 1e-3);
    CHECK(p.warnings == "t.cir:3:9: warning: '1M' is 1 milliohm in SPICE; write 1meg for a megohm\n"
                        "t.cir:4:8: warning: '4.7m' is 4.7 milliohm in SPICE; write 4.7meg for a megohm\n");
    CHECK(parsed(".input u a\n.output y a\nC1 a 0 1M\n").warnings.empty());   // a capacitor of 1 mF is no slip
    CHECK(parsed(".input u a\n.output y a\nR1 a 0 abc\n").errors ==
          "t.cir:3:8: error: 'abc' is not a number (R1's value)\n");
}

TEST_CASE("expressions over pots", "[circuit]")
{
    const Parsed p = parsed(".input u a\n.output y a\n.pot tone\n.pot drive\n"
                            "R1 a b {1 - tone}\nR2 b c {(1 - drive) * 0.5}\nR3 c 0 {51k + 500k*drive^2}\n"
                            "R4 a 0 {-drive^2 + 3}\n");
    REQUIRE(p.errors.empty());
    REQUIRE(p.netlist.elements.size() == 4);
    auto at = [](double tone, double drive)
    {
        return [=](const std::string& key)
        {
            return key == "tone" ? tone : drive;
        };
    };
    const fc::Expr& e1 = *p.netlist.elements[0].expr;
    const fc::Expr& e2 = *p.netlist.elements[1].expr;
    const fc::Expr& e3 = *p.netlist.elements[2].expr;
    const fc::Expr& e4 = *p.netlist.elements[3].expr;
    for (double x : {0.0, 0.5, 1.0})
    {
        CHECK(fc::evaluate(e1, at(x, 0)) == 1 - x);
        CHECK(fc::evaluate(e2, at(0, x)) == (1 - x) * 0.5);
        CHECK(fc::evaluate(e3, at(0, x)) == 51e3 + 500e3 * (x * x));
        CHECK(fc::evaluate(e4, at(0, x)) == -(x * x) + 3);   // -x^2 is -(x^2)
    }
    auto name = [](const std::string& key)
    {
        return key;
    };
    CHECK(fc::printExpr(e1, name) == "1.0f - tone");
    CHECK(fc::printExpr(e2, name) == "(1.0f - drive) * 0.5f");
    CHECK(fc::printExpr(e3, name) == "51000.0f + 5e+05f * (drive * drive)");
    CHECK(fc::printExpr(e4, name) == "-(drive * drive) + 3.0f");
    CHECK(fc::potsOf(e3) == std::vector<std::string>{"drive"});
    // an expression without a pot is a constant resistor
    const Parsed c = parsed(".input u a\n.output y a\nR1 a 0 {2k * 2 + 200}\n");
    CHECK(c.netlist.elements[0].value == 4200);
    CHECK_FALSE(c.netlist.elements[0].expr);
    // the errors, at their columns
    CHECK(parsed(".input u a\n.output y a\n.pot tone\nR1 a 0 {1 - tonee}\n").errors ==
          "t.cir:4:13: error: unknown pot 'tonee' (the pots: tone)\n");
    CHECK(parsed(".input u a\n.output y a\n.pot drive\nR1 a 0 {drive^2.5}\n").errors ==
          "t.cir:4:15: error: the exponent of '^' is an integer from 0 to 8\n");
    CHECK(parsed(".input u a\n.output y a\n.pot drive\nR1 a 0 {drive^9}\n").errors ==
          "t.cir:4:15: error: the exponent of '^' is an integer from 0 to 8\n");
    CHECK(parsed(".input u a\n.output y a\n.pot drive\nR1 a 0 {(drive + 1}\n").errors ==
          "t.cir:4:19: error: expected ')'\n");
}

TEST_CASE("the netlist subset parses", "[circuit]")
{
    const Parsed p = parsed(fc::test::kDiodeTone);
    REQUIRE(p.errors.empty());
    CHECK(p.warnings.empty());
    const fc::Netlist& n = p.netlist;
    CHECK(n.title == "DiodeTone");
    CHECK(n.elements.size() == 7);
    REQUIRE(n.models.size() == 1);
    CHECK(n.models[0].name == "1N914");
    CHECK_THAT(n.models[0].is, WithinRel(2.52e-9, 1e-15));
    CHECK(n.models[0].n == 1.752);
    REQUIRE(n.pots.size() == 1);
    CHECK(n.pots[0].name == "tone");
    CHECK(n.pots[0].defaultValue == 0.5);
    CHECK(n.pots[0].taper == fc::Taper::Lin);
    REQUIRE(n.input);
    CHECK(n.input->name == "u");
    CHECK(n.input->node == "in");
    CHECK(n.input->scale == 1);
    REQUIRE(n.output);
    CHECK(n.output->node == "b");
    CHECK(n.oversample.value == 4);
    CHECK(n.halfband.value == fc::Halfband::Iir);
    CHECK(n.adaa.value == false);
    REQUIRE(n.solver.value);
    CHECK(n.solver.value->guess == fc::Solver::Guess::Omega);
    CHECK(n.solver.value->steps == 2);
    CHECK(n.target.solver == -60);
    CHECK(n.target.alias == -60);
    CHECK(n.target.level == 4.5);
    CHECK(n.target.freq == 3000);
    const fc::Element& pot = n.elements[5];
    CHECK(pot.kind == fc::Element::Kind::Pot);
    CHECK(pot.nodes == std::vector<std::string>{"c", "b"});
    CHECK(pot.value == 25000);

    // continuation, ';' and '*' comments, case, .end
    const Parsed q = parsed("* a comment\n.TITLE Cont\n.Input u IN\n.output y Out ; the output\n"
                            "r1 in out\n+ 1k\nC1 OUT 0 1n\n  * an indented comment\n.end\nR9 junk\n");
    REQUIRE(q.errors.empty());
    REQUIRE(q.netlist.elements.size() == 2);
    CHECK(q.netlist.elements[0].value == 1000);
    CHECK(q.netlist.elements[0].nodeLocs[0].line == 5);
    CHECK(q.netlist.elements[0].valueLoc.line == 6);
    CHECK(q.netlist.elements[0].valueLoc.column == 3);
    const fc::Plan plan = planned("* a comment\n.TITLE Cont\n.Input u IN\n.output y Out ; the output\n"
                                  "r1 in out\n+ 1k\nC1 OUT 0 1n\n.end\n");
    CHECK(plan.module == "Cont");
    CHECK(plan.nodes.size() == 3);   // ground, in, out: compared without case, named as first written
    CHECK(plan.nodes[1].name == "in");
    CHECK(plan.nodes[2].name == "out");
    // a model card with its parameters spaced, and the ones Phase 0 ignores
    const Parsed m = parsed(".model D1N4148 D ( IS = 4.352n N=1.906 CJO=4p )\n");
    REQUIRE(m.netlist.models.size() == 1);
    CHECK_THAT(m.netlist.models[0].is, WithinRel(4.352e-9, 1e-15));
    CHECK(m.netlist.models[0].n == 1.906);
    CHECK(m.warnings == "t.cir:1:40: warning: CJO is ignored (Phase 0's diode is Shockley's: IS and N)\n");
}

TEST_CASE("netlist errors, as clang words them", "[circuit]")
{
    const std::string head = ".input u in\n.output y o\n";
    struct Case
    {
        std::string netlist;
        std::string errors;
    };
    const Case cases[] = {
        {head + "R1 in o 1k\nR6 o f1 51k\nR2 o 0 1k\n",
         "t.cir:4:1: error: node 'f1' has one connection (R6); a node needs two\n"},
        {head + "R1 in o 1k\nC3 o z2 1n\nC9 z2 0 1n\n",
         "t.cir:4:1: error: node 'z2' reaches ground only through capacitors (C3, C9): its DC voltage is undefined "
         "(add 10meg to ground)\n"},
        {head + "R1 in o 1k\nD1 o 0 1N4148X\nD2 0 o 1N4148X\n.model 1N914 D(IS=2.52n N=1.752)\n",
         "t.cir:4:8: error: unknown model '1N4148X' (the models: 1N914)\n"
         "t.cir:5:8: error: unknown model '1N4148X' (the models: 1N914)\n"},
        {head + "R1 in o 1k\nD3 o 0 1N914\n.model 1N914 D(IS=2.52n N=1.752)\n",
         "t.cir:4:1: error: D3 has no antiparallel partner: Phase 0 solves antiparallel diode pairs only\n"},
        {head + "R1 in o 1k\nD1 o 0 A\nD2 0 o B\n.model A D(IS=1n)\n.model B D(IS=2n)\n",
         "t.cir:5:1: error: antiparallel diodes D1, D2 have different models: not supported yet\n"},
        {head + "R1 in n 1k\nR2 n 0 1k\nXU1 n 0 o opamp\nR3 o 0 1k\n",
         "t.cir:5:1: error: op amp XU1 has no negative feedback: an ideal op amp needs one\n"},
        {head + "R1 in o 1k\nR2 o 0 1k\nD1 o 0 M\nD2 0 o M\nD3 o 0 M\nD4 0 o M\n.model M D(IS=1n)\n",
         "t.cir:3:1: error: block 1 has 2 nonlinear ports (D1|D2, D3|D4): not supported yet (Phase 1)\n"},
        {".input u in\nR1 in o 1k\nR2 o 0 1k\n", "t.cir: error: the netlist has no .output\n"},
        {".output y o\nR1 o 0 1k\nR2 o 0 2k\n", "t.cir: error: the netlist has no .input\n"},
        {head + "R1 in o 1k\nR2 o 0 1k\n.foo 3\n", "t.cir:5:1: error: unknown directive '.foo'\n"},
        {head + "R1 in o 1k\nL1 o 0 1m\n", "t.cir:4:1: error: the inductor L1 is not supported yet (Phase 1)\n"},
        {head + "R1 in o 1k\nQ1 o in 0 2N3904\n",
         "t.cir:4:1: error: the transistor Q1 is not supported yet (Phase 2)\n"},
        {head + "R1 in o 1k\nR2 o 0 1k\n.model M D(IS=1n RS=10)\n",
         "t.cir:5:18: error: RS is not supported yet (Phase 1)\n"},
        {head + "R1 in o 1k\nR2 o 0 1k\n.method bdf2\n",
         "t.cir:5:9: error: '.method bdf2' is not supported yet (Phase 1)\n"},
        {head + "R1 in o 1k\nR2 o 0 1k\n.subckt foo a b\n", "t.cir:5:1: error: '.subckt' is not supported yet\n"},
        {head + "R1 in o 1k\nR2 o 0 0\n", "t.cir:4:8: error: R2's value is 0: a resistor needs a positive value\n"},
        // a feedback loop that crosses blocks: X1 reads X2's output and X2 reads X1's
        {".input u in\n.output y m\nR1 in m 1k\nR2 m 0 1k\nX1 o2 n1 o1 opamp\nR3 o1 n1 1k\nR4 n1 0 1k\n"
         "X2 o1 n2 o2 opamp\nR5 o2 n2 1k\nR6 n2 0 1k\n",
         "t.cir:5:1: error: a feedback loop around X1 crosses blocks: not supported yet\n"},
        // two op amps whose nullors say the same thing: a singular block
        {".input u in\n.output y o1\nX1 in n o1 opamp\nX2 in n o2 opamp\nR1 o1 n 1k\nR2 o2 n 1k\nR3 n 0 1k\n",
         "t.cir:3:1: error: block 1 (X1, X2, R1, R2, ...): its equations are singular (nothing determines V(o2)): an "
         "op amp without negative feedback, or a node cut off from the rest\n"},
        {head + "R1 in o 1k\nR2 o 0 1k\n.pot set\n",
         "t.cir:5:6: error: pot 'set' is an FMinus keyword or builtin, or a "
         "name the module uses: rename it\n"},
        // the names the module uses: os1 (tick's field), and any name its text has, for the module (R11)
        {".input os1 in\n.output y o\nR1 in o 1k\nR2 o 0 1k\n",
         "t.cir:1:8: error: .input 'os1' is an FMinus keyword or builtin, or a name the module uses: rename it\n"},
        {".title step\n" + head + "R1 in o 1k\nR2 o 0 1k\n",
         "t.cir:1:8: error: the module's name 'step', from .title, is a name the module uses: rename it\n"},
        {".title t3\n" + head + "R1 in o 1k\nR2 o 0 1k\n",
         "t.cir:1:8: error: the module's name 't3', from .title, is a name the module uses: rename it\n"},
        {".title rate\n" + head + "R1 in o 1k\nR2 o 0 1k\n",
         "t.cir:1:8: error: the module's name 'rate', from .title, is a name the module uses: rename it\n"},
        // a negative impedance converter around the port: K < 0, which the bracket and log(Is K / N Vt) cannot take
        {".input u in\n.output y a\n.oversample 2\nR1 in a 1k\nC1 a 0 10n\nD1 a 0 DX\nD2 0 a DX\nX1 a n o opamp\n"
         "Rf o a 100\nR3 o n 1k\nR4 n 0 1k\nR5 a n 1meg\n.model DX D(IS=2.52n N=1.752)\n",
         "t.cir:4:1: error: port D1|D2 sees a negative resistance (K = -123.2 ohm at 22.05 kHz): positive feedback "
         "around X1 is not supported yet\n"},
        // values float coefficients cannot hold, and expressions too deep for a walk of their tree
        {head + "R1 in o 1e-45\nR2 o 0 1k\n",
         "t.cir:3:9: error: R1's value 1e-45 is out of range for float coefficients (1e-30 to 1e30)\n"},
        {head + "R1 in o 1k\nC1 o 0 2e30\n",
         "t.cir:4:8: error: C1's value 2e30 is out of range for float coefficients (1e-30 to 1e30)\n"},
        {head + ".pot tone\nR1 in o {1/0 + tone}\nR2 o 0 1k\n",
         "t.cir:4:11: error: a constant in the expression is inf, not a finite float\n"},
        {head + ".pot tone\nR1 in o 1k\nR2 o 0 {" + std::string(65, '(') + "tone" + std::string(65, ')') + "}\n",
         "t.cir:5:73: error: the expression is nested too deeply (at most 64 levels)\n"},
        {head + ".pot tone\nR1 in o 1k\nR2 o 0 {" + std::string(65, '-') + "tone}\n",
         "t.cir:5:73: error: the expression is nested too deeply (at most 64 levels)\n"},
        {head + ".pot tone\nR1 in o 1k\nR2 o 0 {1k + tone" + [] {
             std::string s;
             for (int i = 0; i < 300; ++i)
                 s += "+1";
             return s;
         }() + "}\n",
         "t.cir:5:528: error: the expression has too many operators (at most 256)\n"},
    };
    for (const Case& c : cases)
    {
        INFO(c.netlist);
        CHECK(errorsOf(c.netlist) == c.errors);
    }
}

TEST_CASE("netlist warnings and notes", "[circuit]")
{
    const std::string head = ".input u in\n.output y o\nR1 in o 1k\nC1 o 0 1n\n";
    struct Case
    {
        std::string netlist;
        std::string warnings;
    };
    const Case cases[] = {
        {head + ".pot drive\n", "t.cir:5:6: warning: pot 'drive' is never used\n"},
        {".input u in\n.output y o\n.pot tone 0.5 lin\nR1 in o 1k\nP1 o 0 10k {tone} taper=audio\n",
         "t.cir:5:19: warning: P1's taper=audio differs from pot 'tone''s lin: taper=audio is used\n"},
        {head + "R9 in 0 1k\n",
         "t.cir:5:1: warning: R9 joins 'in' and ground, which are both driven: it changes nothing\n"},
        {head + ".op\n", "t.cir:5:1: warning: '.op' is ignored (Phase 1: the DC operating point)\n"},
    };
    for (const Case& c : cases)
    {
        INFO(c.netlist);
        const fc::Result r = fc::generate(c.netlist, "t.cir");
        CHECK(r.errors.empty());
        CHECK(r.warnings.find(c.warnings) != std::string::npos);
    }
    // auto is Phase 0's default, with a note; simulator directives are ignored with one
    const fc::Result r = fc::generate(head + ".oversample auto\n.adaa auto\n.solver auto\n.tran 1u 1m\n", "t.cir");
    CHECK(r.notes.find("t.cir:5:1: note: '.oversample auto' is 8x, Phase 0's default") != std::string::npos);
    CHECK(r.notes.find("t.cir:6:1: note: '.adaa auto' is off") != std::string::npos);
    CHECK(r.notes.find("t.cir:7:1: note: '.solver auto' is omega(2)") != std::string::npos);
    CHECK(r.notes.find("t.cir:8:1: note: '.tran' is a simulator's directive: ignored") != std::string::npos);
    CHECK(r.notes.find("t.cir: note: block 1 (R1, C1): 1 state, linear, at the host rate") != std::string::npos);
    CHECK(errorsOf(head + ".model Q1 NPN(BF=100)\n") ==
          "t.cir:5:11: error: model type 'NPN' is not supported yet (Phase 2): Phase 0 has diodes (D)\n");
    CHECK(errorsOf(head + ".model D9 D(IS=1n XYZ=3)\n") == "t.cir:5:19: error: unknown diode parameter 'XYZ'\n");
}

TEST_CASE("the plan cuts at ideal op amps", "[circuit]")
{
    const fc::Plan ts = planned(fc::test::kScreamer);
    REQUIRE(ts.blocks.size() == 4);
    auto parts = [&](const fc::Block& b)
    {
        std::string out;
        for (int e : b.elements)
            out += (out.empty() ? "" : " ") + ts.netlist.elements[static_cast<std::size_t>(e)].name;
        return out;
    };
    CHECK(parts(ts.blocks[0]) == "C1 R1");
    CHECK(parts(ts.blocks[1]) == "XU1 R4 C3 R6 Pdrive C4 D1 D2");
    CHECK(parts(ts.blocks[2]) == "R7 C5 R8 XU2 R9 Ptone R10 C6");
    CHECK(parts(ts.blocks[3]) == "C7 R11 Plevel");
    int states = 0;
    int ports = 0;
    for (const fc::Block& b : ts.blocks)
    {
        for (const fc::Branch& br : b.branches)
            states += br.kind == fc::Branch::Kind::Capacitor ? 1 : 0;
        ports += b.port ? 1 : 0;
    }
    CHECK(states == 6);
    CHECK(ports == 1);
    CHECK(ts.blocks[0].pots.empty());
    CHECK(ts.blocks[1].pots == std::vector<std::string>{"drive"});
    CHECK(ts.blocks[2].pots == std::vector<std::string>{"tone"});
    CHECK(ts.blocks[3].pots == std::vector<std::string>{"level"});
    CHECK(ts.nodes[static_cast<std::size_t>(ts.blocks[1].input)].name == "p1");   // XU1's + input: an exact cut
    CHECK(ts.nodes[static_cast<std::size_t>(ts.blocks[2].input)].name == "v2");   // XU1's output
    CHECK(ts.nodes[static_cast<std::size_t>(ts.blocks[3].input)].name == "v3");   // XU2's output
    CHECK(ts.regionFirst == 1);   // blocks=auto: block 2, and block 3, which fails the 1x check
    CHECK(ts.regionLast == 2);
    CHECK(ts.blocks[1].rateFactor == 4);
    CHECK(ts.blocks[1].designFactor == 2.0 / 3.0);
    CHECK(ts.blocks[2].rateFactor == 4);
    CHECK(ts.blocks[2].designFactor == 1);
    CHECK(ts.blocks[0].rateFactor == 1);
    CHECK(ts.blocks[3].rateFactor == 1);

    fc::Options nonlinear;
    nonlinear.region = fc::Region::Nonlinear;
    const fc::Plan ts2 = planned(fc::test::kScreamer, nonlinear);
    CHECK(ts2.regionFirst == 1);
    CHECK(ts2.regionLast == 1);
    fc::Options all;
    all.region = fc::Region::All;
    const fc::Plan ts3 = planned(fc::test::kScreamer, all);
    CHECK(ts3.regionFirst == 0);
    CHECK(ts3.regionLast == 3);

    CHECK(planned(fc::test::kDiodeTone).blocks.size() == 1);
    CHECK(planned(fc::test::kScreamerClip).blocks.size() == 1);
    // a follower in a Sallen-Key belongs to its + input's network
    CHECK(planned(fc::test::kSallenKey).blocks.size() == 1);
}

TEST_CASE("example 1's equations are the hand-derived ones", "[circuit]")
{
    const fc::Plan plan = planned(fc::test::kDiodeTone);
    for (double fs : {48000.0, 128000.0, 192000.0})
    {
        for (double tone : {0.0, 0.5, 1.0})
        {
            INFO("fs' " << fs << ", tone " << tone);
            const fc::Holters h = fc::holters(plan, 0, fs, potsAt(plan, "tone", tone));
            const fc::test::HandBlock d = fc::test::makeDT(fs, tone);
            REQUIRE(h.states == 2);
            REQUIRE(h.port);
            const double cpScale = std::fmax(std::fabs(d.cp[0]), std::fabs(d.cp[1]));
            const double cyScale = std::fmax(std::fabs(d.cy[0]), std::fabs(d.cy[1]));
            const double aScale = std::fmax(std::fmax(std::fabs(d.a[0][0]), std::fabs(d.a[0][1])),
                                            std::fmax(std::fabs(d.a[1][0]), std::fabs(d.a[1][1])));
            const double wScale = std::fmax(std::fabs(d.w[0]), std::fabs(d.w[1]));   // per volt, as B's entries
            for (std::size_t i = 0; i < 2; ++i)
            {
                CHECK(near(h.cp[i], d.cp[i], cpScale));
                CHECK(near(h.cy[i], d.cy[i], cyScale));
                CHECK(near(h.b[i], d.b[i], wScale));
                CHECK(near(h.w[i], d.w[i], 0));
                for (std::size_t j = 0; j < 2; ++j)
                    CHECK(near(h.a[i][j], d.a[i][j], aScale));
            }
            CHECK(near(h.dp, d.dp, 0));
            CHECK(near(h.dy, d.dy, 1));   // volts per volt: V(b) per u is a cancellation there, 0 here
            CHECK(near(h.wy, d.wy, 1));
            CHECK(near(h.k, d.k, 0));
        }
    }
}

TEST_CASE("the TS clipping stage's equations are the hand-derived ones", "[circuit]")
{
    // makeTS's v is V(v2) - V(n1), and its C4 runs the same way; the netlist's D1 and C4 run n1 -> v2, so x_C4, v and
    // p change sign: S = (+1, -1) on the states (C3, C4), -1 on v.
    const fc::Plan plan = planned(fc::test::kScreamer);
    const double s[2] = {1, -1};
    for (double drive : {0.0, 0.5, 1.0})
    {
        for (double fs : {128000.0, 192000.0})
        {
            INFO("fs' " << fs << ", drive " << drive);
            const fc::Holters h = fc::holters(plan, 1, fs, potsAt(plan, "drive", drive));
            const fc::test::HandBlock d = fc::test::makeTS(fs, drive);
            REQUIRE(h.states == 2);
            const double cpScale = std::fmax(std::fabs(d.cp[0]), std::fabs(d.cp[1]));
            for (std::size_t i = 0; i < 2; ++i)
            {
                CHECK(near(h.cp[i], -s[i] * d.cp[i], cpScale));
                CHECK(near(h.cy[i], s[i] * d.cy[i], 1));
                CHECK(near(h.b[i], s[i] * d.b[i], 0));
                CHECK(near(h.w[i], -s[i] * d.w[i], 0));
                for (std::size_t j = 0; j < 2; ++j)
                    CHECK(near(h.a[i][j], s[i] * s[j] * d.a[i][j], 1));
            }
            CHECK(near(h.dp, -d.dp, 0));
            CHECK(near(h.dy, d.dy, 0));
            CHECK(near(h.wy, -d.wy, 0));
            CHECK(near(h.k, d.k, 0));
        }
    }
}

TEST_CASE("the tone stage is Yeh's eq. 24", "[circuit]")
{
    // Yeh, Abel & Smith, DAFx-07, eq. 24, with R_l = T 20k on the + input's side (report §9.1).
    const double rs = 1e3, cs = 0.22e-6, ri = 10e3, rz = 220, cz = 0.22e-6, rf = 1e3, rp = 20e3;
    auto yeh = [&](Complex s, double t)
    {
        const double rl = t * rp, rr = (1 - t) * rp, par = rl * rr / (rl + rr);
        const double y = (rl + rr) * (rz + par);
        const double w = y / (rl * rf + y);
        const double x = (rr / (rl + rr)) / ((rz + par) * cz);
        const double wz = 1 / (cz * (rz + par)), wp = 1 / (cs * (rs * ri / (rs + ri)));
        return (rl * rf + y) / (y * rs * cs) * (s + w * wz) / ((s + wp) * (s + wz) + x * s);
    };
    const fc::Plan plan = planned(fc::test::kScreamer);
    std::string swappedText = fc::test::kScreamer;
    const std::string line = "Ptone  tn  tw  t1  20k {tone}";
    swappedText.replace(swappedText.find(line), line.size(), "Ptone  t1  tw  tn  20k {tone}");
    const fc::Plan swapped = planned(swappedText);
    for (double t : {0.0, 0.5, 1.0})
    {
        double worst = 0;
        double swappedWorst = 0;
        for (double f = 20; f <= 20000; f *= 1.01)
        {
            const Complex s(0, 2 * std::numbers::pi * f);
            const double ref = dB(yeh(s, t));
            worst = std::fmax(worst, std::fabs(dB(fc::analog(plan, 2, s, potsAt(plan, "tone", t))) - ref));
            swappedWorst =
                std::fmax(swappedWorst, std::fabs(dB(fc::analog(swapped, 2, s, potsAt(swapped, "tone", t))) - ref));
        }
        INFO("T = " << t);
        CHECK(worst < 1e-9);
        if (t != 0.5)
        {
            CHECK(swappedWorst > 16.3);   // the line as fminus.md wrote it reverses the knob: 16.4 dB at the ends
            CHECK(swappedWorst < 16.5);
        }
    }
}

TEST_CASE("the trapezoidal blocks are the bilinear map of their analog response", "[circuit]")
{
    const fc::Plan plan = planned(fc::test::kScreamer);
    const double fs = 48000;
    for (int b : {0, 2, 3})
    {
        const fc::Block& block = plan.blocks[static_cast<std::size_t>(b)];
        for (const std::vector<double>& pots : fc::potGrid(plan, block))
        {
            const fc::Holters h = fc::holters(plan, b, fs, pots);
            double worst = 0;
            for (double f = 20; f <= 20000; f *= 1.05)
            {
                const Complex z = std::polar(1.0, 2 * std::numbers::pi * f / fs);
                const Complex digital = stateSpace(h, z);
                const Complex analog =
                    fc::analog(plan, b, Complex(0, 2 * fs * std::tan(std::numbers::pi * f / fs)), pots);
                if (std::abs(analog) < 1e-9)
                    continue;   // the level pot at 0: silence
                worst = std::fmax(worst, std::fabs(dB(digital) - dB(analog)));
            }
            INFO("block " << b + 1);
            CHECK(worst < 1e-9);
        }
    }
}

TEST_CASE("closed forms", "[circuit]")
{
    const fc::Plan ts = planned(fc::test::kScreamer);
    for (double f = 20; f <= 20000; f *= 1.1)
    {
        const Complex s(0, 2 * std::numbers::pi * f);
        // block 1: C1 1u into R1 10k
        const Complex h1 = s * 10e3 * 1e-6 / (1.0 + s * 10e3 * 1e-6);
        CHECK(std::fabs(dB(fc::analog(ts, 0, s, potsAt(ts, "drive", 0.5))) - dB(h1)) < 1e-9);
        // block 4: C7 1u, R11 1k, the level pot 100k to ground, read at its wiper
        for (double level : {0.3, 0.7, 1.0})
        {
            const double t = fc::taperValue(fc::Taper::Audio, level);
            const Complex h4 = t * 100e3 * s * 1e-6 / (1.0 + s * 1e-6 * 101e3);
            CHECK(std::fabs(dB(fc::analog(ts, 3, s, potsAt(ts, "level", level))) - dB(h4)) < 1e-9);
        }
        // an RC low-pass
        const fc::Plan rc = planned(fc::test::kRcLowpass);
        CHECK(std::fabs(dB(fc::analog(rc, 0, s, {})) - dB(1.0 / (1.0 + s * 1e3 * 100e-9))) < 1e-9);
        // a non-inverting amplifier with a capacitor across its feedback resistor
        const fc::Plan amp = planned(fc::test::kNonInverting);
        const Complex zf = 47e3 / (1.0 + s * 47e3 * 1e-9);
        CHECK(std::fabs(dB(fc::analog(amp, 0, s, {})) - dB(1.0 + zf / 4.7e3)) < 1e-9);
        // a unity-gain Sallen-Key low-pass
        const fc::Plan sk = planned(fc::test::kSallenKey);
        const Complex hsk = 1.0 / (1.0 + s * 10e-9 * (10e3 + 10e3) + s * s * 10e3 * 10e3 * 22e-9 * 10e-9);
        CHECK(std::fabs(dB(fc::analog(sk, 0, s, {})) - dB(hsk)) < 1e-9);
    }
    // a 3-terminal pot as a divider, at each taper: exactly t, ends included
    for (const char* taper : {"lin", "audio", "rlog"})
    {
        const fc::Plan divider = planned(fc::test::potDivider(taper));
        const fc::Taper kind = std::string(taper) == "lin"     ? fc::Taper::Lin
                               : std::string(taper) == "audio" ? fc::Taper::Audio
                                                               : fc::Taper::Rlog;
        for (double x : {0.0, 0.25, 0.5, 0.75, 1.0})
        {
            INFO(taper << " at " << x);
            CHECK_THAT(std::abs(fc::analog(divider, 0, {0, 1000}, {x})), WithinAbs(fc::taperValue(kind, x), 1e-12));
        }
    }
}

TEST_CASE("§9.2 reproduced, and the 1x check's verdicts", "[circuit]")
{
    const fc::Plan plan = planned(fc::test::kScreamer);
    const fc::System system = fc::assemble(plan, 2);
    fc::Messages messages;
    const std::optional<fc::Pivots> pivots = fc::choosePivots(system, plan, 2, messages);
    REQUIRE(pivots);
    // report §9.2: the trapezoidal tone block's worst error to 5, 10 and 20 kHz, each T within the report's range
    struct Row
    {
        double fs;
        double lo[3];
        double hi[3];
    };
    const Row rows[] = {{48000, {0.20, 1.08, 8.7}, {0.30, 1.36, 9.0}}, {96000, {0.05, 0.24, 1.28}, {0.07, 0.31, 1.37}}};
    for (const Row& row : rows)
    {
        for (double t : {0.0, 0.5, 1.0})
        {
            double w[3] = {0, 0, 0};
            for (double f = 20; f <= 20000; f *= 1.01)
            {
                const std::vector<double> pots = potsAt(plan, "tone", t);
                const double a = dB(fc::analog(system, *pivots, plan, 2, {0, 2 * std::numbers::pi * f}, pots));
                const double d = dB(fc::analog(system, *pivots, plan, 2,
                                               {0, 2 * row.fs * std::tan(std::numbers::pi * f / row.fs)}, pots));
                const double e = std::fabs(d - a);
                if (f <= 5000)
                    w[0] = std::fmax(w[0], e);
                if (f <= 10000)
                    w[1] = std::fmax(w[1], e);
                w[2] = std::fmax(w[2], e);
            }
            for (int k = 0; k < 3; ++k)
            {
                INFO("fs " << row.fs << ", T " << t << ", band " << k << ": " << w[k] << " dB");
                CHECK(w[k] >= row.lo[k] - 0.02);
                CHECK(w[k] <= row.hi[k] + 0.02);
            }
        }
    }
    // the verdicts: blocks 1 and 4 pass at 44.1 kHz, block 3 fails there and passes at 4 x 44.1 kHz
    CHECK(plan.blocks[0].oneX < 0.5);
    CHECK(plan.blocks[3].oneX < 0.5);
    CHECK(plan.blocks[2].oneX > 0.5);
    CHECK(plan.blocks[2].oneXRegion < 0.5);
    CHECK(fc::oneXError(system, *pivots, plan, 2, 176400) < 0.5);
}

TEST_CASE("structural exactness and safe pivots", "[circuit]")
{
    // Example 1: V(b) reads neither u nor x_C1, and C1's voltage is v itself.
    {
        const fc::Plan plan = planned(fc::test::kDiodeTone);
        fc::Messages messages;
        const std::optional<fc::BlockCode> code = fc::deriveBlock(plan, 0, messages);
        REQUIRE(code);
        const fc::System& s = code->system;
        fc::Code text;
        std::vector<fc::Val> symbols;
        for (const fc::System::Symbol& sym : s.symbols)
            symbols.push_back(fc::Val::var(sym.name, &text));
        const fc::Linear<fc::Val> l = fc::solve(s, code->pivots, symbols);
        const auto out = static_cast<std::size_t>(s.output);
        CHECK(fc::isZero(l.coef[out][0]));   // u
        CHECK(fc::isZero(l.coef[out][1]));   // x_C1
        CHECK(l.coef[out][2].kind == fc::Val::Kind::Var);
        const auto [c1, sign] = s.capacitor[0];
        const auto c1r = static_cast<std::size_t>(c1);
        CHECK(sign == 1);
        CHECK(fc::isZero(l.coef[c1r][0]));
        CHECK(fc::isZero(l.coef[c1r][1]));
        CHECK(fc::isZero(l.coef[c1r][2]));
        CHECK(l.coef[c1r][3].kind == fc::Val::Kind::Const);   // v_D1
        CHECK(l.coef[c1r][3].c == 1);
    }
    // The TS block: V(n1) = u, V(v2) = u - v, C4's voltage = v, each exactly.
    {
        const fc::Plan plan = planned(fc::test::kScreamer);
        fc::System s = fc::assemble(plan, 1);
        fc::Messages messages;
        const std::optional<fc::Pivots> pivots = fc::choosePivots(s, plan, 1, messages);
        REQUIRE(pivots);
        fc::System::Readout n1;
        n1.name = "n_n1";
        n1.unknowns.emplace_back(0, 1.0);   // V(n1), the first node
        REQUIRE(s.unknownNames[0] == "V(n1)");
        s.readouts.push_back(n1);
        fc::Code text;
        std::vector<fc::Val> symbols;
        for (const fc::System::Symbol& sym : s.symbols)
            symbols.push_back(fc::Val::var(sym.name, &text));
        const fc::Linear<fc::Val> l = fc::solve(s, *pivots, symbols);
        const std::vector<fc::Val>& vn1 = l.coef.back();
        const std::vector<fc::Val>& vv2 = l.coef[static_cast<std::size_t>(s.output)];
        const auto v = static_cast<std::size_t>(s.port);
        CHECK(vn1[0].kind == fc::Val::Kind::Const);
        CHECK(vn1[0].c == 1);
        CHECK(vv2[0].kind == fc::Val::Kind::Const);
        CHECK(vv2[0].c == 1);
        CHECK(vv2[v].kind == fc::Val::Kind::Const);
        CHECK(vv2[v].c == -1);
        for (std::size_t k = 1; k < v; ++k)
        {
            CHECK(fc::isZero(vn1[k]));
            CHECK(fc::isZero(vv2[k]));
        }
        CHECK(fc::isZero(vn1[v]));
        const auto [c4, sign] = s.capacitor[1];
        const std::vector<fc::Val>& vc4 = l.coef[static_cast<std::size_t>(c4)];
        CHECK(sign * (vc4[v].kind == fc::Val::Kind::Const ? vc4[v].c : 0) == 1);
    }
    // Every block's pivots are at least 1e-6 of their column, and K > 0, over the grid of rates and pots.
    for (const std::string& netlist : {fc::test::kDiodeTone, fc::test::kScreamer, fc::test::kScreamerClip})
    {
        const fc::Plan plan = planned(netlist);
        for (std::size_t b = 0; b < plan.blocks.size(); ++b)
        {
            const fc::System s = fc::assemble(plan, static_cast<int>(b));
            fc::Messages messages;
            const std::optional<fc::Pivots> pivots = fc::choosePivots(s, plan, static_cast<int>(b), messages);
            REQUIRE(pivots);
            INFO(plan.module << " block " << b + 1);
            CHECK(fc::pivotMargin(s, *pivots, plan, static_cast<int>(b)) >= 1e-6);
            if (!plan.blocks[b].port)
                continue;
            for (double rate : {22050.0, 44100.0, 48000.0, 96000.0, 192000.0, 384000.0})
            {
                for (const std::vector<double>& pots : fc::potGrid(plan, plan.blocks[b]))
                    CHECK(fc::holters(plan, static_cast<int>(b), rate * 8, pots).k > 0);
            }
        }
    }
}

TEST_CASE("the halfband stages meet their design", "[circuit]")
{
    const double stopFrom[] = {28000, 76000, 172000};
    for (int s = 1; s <= 3; ++s)
    {
        const double fs = 48000 * std::pow(2.0, s);
        double firStop = 0, iirStop = 0, firRipple = 0, iirRipple = 0;
        for (int i = 0; i <= 8000; ++i)
        {
            const double f = 0.5 * fs * i / 8000;
            const double w = 2 * std::numbers::pi * f / fs;
            const double fir = std::fabs(fc::firResponse(s, w));
            const double iir = std::abs(fc::iirResponse(s, w));
            if (f >= stopFrom[s - 1])
            {
                firStop = std::fmax(firStop, fir);
                iirStop = std::fmax(iirStop, iir);
            }
            if (f <= 20000)
            {
                firRipple = std::fmax(firRipple, std::fabs(20 * std::log10(fir)));
                iirRipple = std::fmax(iirRipple, std::fabs(20 * std::log10(iir)));
            }
        }
        INFO("stage " << s);
        // gen.cpp's FIRs keep their centre tap at 0.5 (the halfband property), so their DC gain is the windowed sinc's
        // sum, 1 +- 1.5e-5 (0.00013 dB), and S3 reaches 91.9 dB: the report's ">= 92 dB" rounds it
        CHECK(20 * std::log10(firStop) <= -91.8);
        CHECK(20 * std::log10(iirStop) <= -92);
        CHECK(firRipple <= 0.001);
        CHECK(iirRipple <= 0.001);
        CHECK_THAT(fc::firResponse(s, 0), WithinAbs(1, 2e-5));
        CHECK_THAT(std::abs(fc::iirResponse(s, 0)), WithinAbs(1, 1e-12));
    }
    // Delays at 1 kHz, host samples. The FIR round trip is the report's (§4.1 row 5): 40 / 47 / 49.5.
    CHECK(fc::roundTripDelay(2, fc::Halfband::Fir, 1000) == 40);
    CHECK(fc::roundTripDelay(4, fc::Halfband::Fir, 1000) == 47);
    CHECK(fc::roundTripDelay(8, fc::Halfband::Fir, 1000) == 49.5);
    // The report's IIR figures, 3.2 / 4.3 / 4.7, sum the decimator's group delay twice a stage (gen.cpp's measure).
    double sum = 0;
    const double report[] = {3.2, 4.3, 4.7};
    for (int s = 1; s <= 3; ++s)
    {
        const double fs = 48000 * std::pow(2.0, s);
        const double w = 2 * std::numbers::pi * 1000 / fs;
        const double dw = 1e-6;
        const double tau = -(std::arg(fc::iirResponse(s, w + dw)) - std::arg(fc::iirResponse(s, w - dw))) / (2 * dw);
        sum += 2 * tau / std::pow(2.0, s);
        CHECK_THAT(sum, WithinAbs(report[s - 1], 0.1));
    }
    // The emitted chain's own round trip is half a high-rate sample a stage less: the polyphase decimator's output is
    // the filter's at the pair's odd sample. Measured by running the up and down sections (mkbench.py's iir()) on a
    // 1 kHz sine: 2.69 / 3.52 / 3.79 host samples.
    auto measured = [](int stages)
    {
        struct Section
        {
            std::vector<double> x, y;
            std::vector<double> a;
        };
        auto run = [](Section& sec, double s0, double s1, double& o0, double& o1)
        {
            for (std::size_t i = 0; i < sec.a.size(); ++i)
            {
                double& s = i % 2 == 0 ? s0 : s1;
                const double t = s;
                s = (s - sec.y[i]) * sec.a[i] + sec.x[i];
                sec.x[i] = t;
                sec.y[i] = s;
            }
            o0 = s0;
            o1 = s1;
        };
        std::vector<Section> ups(static_cast<std::size_t>(stages));
        std::vector<Section> downs(static_cast<std::size_t>(stages));
        for (int s = 0; s < stages; ++s)
        {
            const std::span<const double> a = fc::iirCoefficients(s + 1);
            for (Section* sec : {&ups[static_cast<std::size_t>(s)], &downs[static_cast<std::size_t>(s)]})
            {
                sec->a.assign(a.begin(), a.end());
                sec->x.assign(a.size(), 0);
                sec->y.assign(a.size(), 0);
            }
        }
        const int n = 48000;
        Complex in = 0, out = 0;
        for (int i = 0; i < n; ++i)
        {
            const double x = std::sin(2 * std::numbers::pi * 1000 * i / 48000.0);
            std::vector<double> values{x};
            for (int s = 0; s < stages; ++s)
            {
                std::vector<double> next;
                for (double v : values)
                {
                    double o0 = 0, o1 = 0;
                    run(ups[static_cast<std::size_t>(s)], v, v, o0, o1);
                    next.push_back(o0);
                    next.push_back(o1);
                }
                values = next;
            }
            for (int s = stages - 1; s >= 0; --s)
            {
                std::vector<double> next;
                for (std::size_t j = 0; j + 1 < values.size(); j += 2)
                {
                    double o0 = 0, o1 = 0;
                    run(downs[static_cast<std::size_t>(s)], values[j + 1], values[j], o0, o1);
                    next.push_back(0.5 * (o0 + o1));
                }
                values = next;
            }
            if (i >= n - 4800)
            {
                const Complex e = std::polar(1.0, -2 * std::numbers::pi * 1000 * i / 48000.0);
                in += x * e;
                out += values[0] * e;
            }
        }
        return -std::arg(out / in) / (2 * std::numbers::pi * 1000 / 48000.0);
    };
    for (int m : {2, 4, 8})
    {
        const int stages = m == 2 ? 1 : m == 4 ? 2 : 3;
        INFO(m << "x");
        CHECK_THAT(fc::roundTripDelay(m, fc::Halfband::Iir, 1000), WithinAbs(measured(stages), 0.01));
    }
    CHECK_THAT(fc::roundTripDelay(4, fc::Halfband::Iir, 1000), WithinAbs(3.52, 0.01));
}

TEST_CASE("the analysis measures what it says", "[circuit]")
{
    const std::size_t n = 4096;
    // an odd-bin sine has nothing off its harmonics
    const std::vector<double> s = fc::sine(1.0, 85, n, 1, n);
    CHECK(fc::inharmonicDb(s, 85) <= -250);
    // a tone 60 dB down, off the harmonics, reads -60 dB (of the total)
    std::vector<double> t = s;
    const std::vector<double> off = fc::sine(1e-3, 101, n, 1, n);
    for (std::size_t i = 0; i < n; ++i)
        t[i] += off[i];
    CHECK_THAT(fc::inharmonicDb(t, 85), WithinAbs(-60, 0.01));
    // the same harmonics: 0 dB apart
    double h1 = 1;
    double h3 = 1;
    std::vector<double> sq(n);
    for (std::size_t i = 0; i < n; ++i)
        sq[i] = std::tanh(3 * s[i]);
    fc::harmonicDiffDb(sq, sq, 85, &h1, &h3);
    CHECK(h1 == 0);
    CHECK(h3 == 0);
    // a known fractional delay, as a phase shift: the band error is rounding
    const double delay = 0.37;
    std::vector<double> late(n);
    for (std::size_t i = 0; i < n; ++i)
        late[i] = std::sin(2 * std::numbers::pi * 85 * (static_cast<double>(i) - delay) / static_cast<double>(n));
    CHECK(fc::bandErrorDb(late, s, delay) <= -200);
    CHECK(fc::bandErrorDb(late, s, 0) > -40);
    // the decimator passes the band and keeps the sine where it was
    const std::vector<double> fast = fc::sine(1.0, 85, n, 4, 4 * (n + 512));   // the filter reads 48 past the window
    const std::vector<double> slow = fc::decimateWindow(fast, 4, 256, n);
    const std::vector<double> want = fc::sine(1.0, 85, n, 1, n + 256);
    CHECK(fc::bandErrorDb(slow, std::span<const double>(want).subspan(256, n), 0) <= -100);
    CHECK_THAT(fc::binMagnitude(s, 85), WithinAbs(1, 1e-12));
}

TEST_CASE("the reference converges from the closed form", "[circuit]")
{
    // report §9.7: started from omega3, the converged solve never runs out of steps, even after a 13 V jump
    const fc::Plan plan = planned(fc::test::kDiodeTone);
    const fc::Holters h = fc::holters(plan, 0, 4 * 48000.0, potsAt(plan, "tone", 1));
    fc::RefStats stats;
    std::vector<double> step(48000, 13.0);
    fc::simulate(h, step, {}, &stats);
    fc::simulate(h, fc::sine(13.0, 257, 4096, 4, 48000), {}, &stats);
    fc::RefOptions adaa;
    adaa.adaa = true;
    fc::simulate(fc::holters(plan, 0, 4 * 48000.0 * 2 / 3, potsAt(plan, "tone", 1)), step, adaa, &stats);
    CHECK(stats.unconverged == 0);
    CHECK(stats.samples == 3 * 48000);
    CHECK(stats.maxIterations < 40);
    // its DC point is the exact root of v + R1 2 Is sinh(v / N Vt) = u
    const double nvt = fc::thermalVoltage(1.752);
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
    const double expected[] = {0.0999503, 0.515596, 0.615423};
    int k = 0;
    for (double u : {0.1, 1.0, 5.0})
    {
        const std::vector<double> y = fc::simulate(h, std::vector<double>(48000, u), {});
        INFO(u << " V");
        CHECK_THAT(y.back(), WithinAbs(root(u), 1e-9));
        CHECK_THAT(root(u), WithinAbs(expected[k++], 5e-7));
        // the converged ADAA reference lands there too
        const std::vector<double> ya = fc::simulate(fc::holters(plan, 0, 4 * 48000.0 * 2 / 3, potsAt(plan, "tone", 1)),
                                                    std::vector<double>(48000, u), adaa);
        CHECK_THAT(ya.back(), WithinAbs(root(u), 1e-9));
    }
}
