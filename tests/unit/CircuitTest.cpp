// tests/unit/CircuitTest.cpp: the circuit compiler's front half (src/circuit/): SPICE numbers and {expressions}, the
// netlist subset and its errors, the plan (blocks cut at ideal op amps, the oversampled region), each block's equations
// against hand derivations, Yeh's eq. 24 and textbook responses, the 1x check, the pivots, and the halfband stages;
// and the double reference and the analysis that the checks of examples/circuits run on.
#include "fminus/Circuit.h"

#include "circuit/Analysis.h"
#include "circuit/Dc.h"
#include "circuit/Emit.h"
#include "circuit/Halfbands.h"
#include "circuit/Laws.h"
#include "circuit/Linear.h"
#include "circuit/Mna.h"
#include "circuit/Netlist.h"
#include "circuit/Plan.h"
#include "circuit/Reference.h"
#include "CircuitNetlists.h"

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include <algorithm>
#include <cmath>
#include <complex>
#include <map>
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

    // `text` with `lines` appended.
    std::string withLineAdded(const std::string& text, const std::string& lines)
    {
        return text + lines;
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
    // a model card with its parameters spaced, and the ones the diode ignores
    const Parsed m = parsed(".model D1N4148 D ( IS = 4.352n N=1.906 RS=0.5 CJO=4p )\n");
    REQUIRE(m.netlist.models.size() == 1);
    CHECK_THAT(m.netlist.models[0].is, WithinRel(4.352e-9, 1e-15));
    CHECK(m.netlist.models[0].n == 1.906);
    CHECK(m.netlist.models[0].rs == 0.5);
    CHECK(m.warnings == "t.cir:1:47: warning: CJO is ignored (the diode model is Shockley's: IS, N and RS)\n");
    // Phase 1's elements: L, V and I (DC), E and a linear G, each with its value
    const Parsed e = parsed(".input u in\n.output y o\n.pot g\nL1 in o 10m\nV1 b 0 DC -4.5\nI1 b 0 2m\n"
                            "E1 o2 0 o 0 11\nG1 o3 0 o 0 {1m * g}\nV2 c 0 0\n");
    REQUIRE(e.errors.empty());
    REQUIRE(e.netlist.elements.size() == 6);
    CHECK(e.netlist.elements[0].kind == fc::Element::Kind::Inductor);
    CHECK(e.netlist.elements[0].value == 0.01);
    CHECK(e.netlist.elements[1].kind == fc::Element::Kind::Voltage);
    CHECK(e.netlist.elements[1].value == -4.5);
    CHECK(e.netlist.elements[2].kind == fc::Element::Kind::Current);
    CHECK(e.netlist.elements[2].value == 0.002);
    CHECK(e.netlist.elements[3].kind == fc::Element::Kind::Vcvs);
    CHECK(e.netlist.elements[3].nodes == (std::vector<std::string>{"o2", "0", "o", "0"}));
    CHECK(e.netlist.elements[3].value == 11);
    CHECK(e.netlist.elements[4].kind == fc::Element::Kind::Vccs);
    CHECK(e.netlist.elements[4].expr);
    CHECK(e.netlist.elements[5].value == 0);   // a 0 V source: a short
    // the built-in models, when no card names them
    const std::vector<fc::ModelCard> builtin = fc::builtinModels();
    REQUIRE(builtin.size() == 2);
    CHECK(builtin[0].name == "1N914");
    CHECK(builtin[1].name == "1N4148");
    CHECK(builtin[1].is == 2.52e-9);
    CHECK(builtin[1].n == 1.752);
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
         "t.cir:4:8: error: unknown model '1N4148X' (the models: 1N914; built in: 1N914, 1N4148)\n"
         "t.cir:5:8: error: unknown model '1N4148X' (the models: 1N914; built in: 1N914, 1N4148)\n"},
        {head + "R1 in o 1k\nD3 o 0 BAT41\n",
         "t.cir:4:8: error: unknown model 'BAT41' (no .model cards; built in: 1N914, 1N4148)\n"},
        // a series string is of one model, and a pair's two RS must be equal (one resistor serves the pair)
        {head + "R1 in o 1k\nD1 o m 1N914\nD2 m 0 1N4148\n",
         "t.cir:5:1: error: diodes D1 and D2 in series through node 'm' have different models (1N914, 1N4148): a "
         "series string is of one model\n"},
        {head + "R1 in o 1k\nD1 o 0 A\nD2 0 o B\n.model A D(IS=1n RS=10)\n.model B D(IS=2n RS=5)\n",
         "t.cir:5:1: error: antiparallel D1|D2 have unequal RS (10 and 5 ohm): a pair shares one series resistor\n"},
        {head + "R1 in o 1k\nD1 o o 1N914\nR2 o 0 1k\n",
         "t.cir:4:1: error: D1's anode and cathode are one node, 'o'\n"},
        {head + "R1 in n 1k\nR2 n 0 1k\nXU1 n 0 o opamp\nR3 o 0 1k\n",
         "t.cir:5:1: error: op amp XU1 has no negative feedback: an ideal op amp needs one\n"},
        {head + "R1 in o 1k\nR2 o 0 1k\nD1 o 0 M\nD2 0 o M\nD3 o 0 M\nD4 0 o M\n.model M D(IS=1n)\n",
         "t.cir:3:1: error: block 1 has 2 nonlinear ports (D1|D2, D3|D4): multi-port Newton is Phase 2\n"},
        {".input u in\nR1 in o 1k\nR2 o 0 1k\n", "t.cir: error: the netlist has no .output\n"},
        {".output y o\nR1 o 0 1k\nR2 o 0 2k\n", "t.cir: error: the netlist has no .input\n"},
        {head + "R1 in o 1k\nR2 o 0 1k\n.foo 3\n", "t.cir:5:1: error: unknown directive '.foo'\n"},
        {head + "R1 in o 1k\nF1 o 0 V1 2\n",
         "t.cir:4:1: error: the current-controlled source F1 is not supported yet (Phase 2)\n"},
        {head + "R1 in o 1k\nQ1 o in 0 2N3904\n",
         "t.cir:4:1: error: the transistor Q1 is not supported yet (Phase 2)\n"},
        {head + "R1 in o 1k\nR2 o 0 1k\n.model M D(IS=1n RS=-10)\n",
         "t.cir:5:18: error: 'RS=-10': RS is 0 or a resistance from 1e-30 to 1e30 ohms\n"},
        // BDF2 is an integrator ADAA's mean does not hold for (Holters' is the trapezoidal rule's)
        {head + "R1 in o 1k\nC1 o 0 10n\nD1 o 0 1N914\nD2 0 o 1N914\n.adaa on\n.method bdf2\n",
         "t.cir:8:1: error: '.method bdf2' with '.adaa on': ADAA is Holters' trapezoidal form\n"},
        {head + "R1 in o 1k\nR2 o 0 1k\n.method be\n", "t.cir:5:9: error: '.method be': tr, bdf2 or auto\n"},
        {".input u in kind=cv\n.output y o\nR1 in o 1k\nR2 o 0 1k\n",
         "t.cir:1:10: error: 'in': a law input (kind=cv) has no node; write '-' (it enters an OTA's gain, not the "
         "netlist)\n"},
        {".input u in range=0:1\n.output y o\nR1 in o 1k\nR2 o 0 1k\n",
         "t.cir:1:13: error: 'range=0:1': range= is a law input's (kind=cv|current)\n"},
        {head + "R1 in o 1k\nR2 o 0 1k\n.subckt foo a b\n", "t.cir:5:1: error: '.subckt' is not supported yet\n"},
        {head + "R1 in o 1k\nR2 o 0 0\n", "t.cir:4:8: error: R2's value is 0: a resistor needs a positive value\n"},
        // a feedback loop that crosses blocks: X1 reads X2's output and X2 reads X1's
        {".input u in\n.output y m\nR1 in m 1k\nR2 m 0 1k\nX1 o2 n1 o1 opamp\nR3 o1 n1 1k\nR4 n1 0 1k\n"
         "X2 o1 n2 o2 opamp\nR5 o2 n2 1k\nR6 n2 0 1k\n",
         "t.cir:5:1: error: a feedback loop around X1 crosses blocks: not supported yet (Phase 2)\n"},
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

    };
    for (const Case& c : cases)
    {
        INFO(c.netlist);
        const fc::Result r = fc::generate(c.netlist, "t.cir");
        CHECK(r.errors.empty());
        CHECK(r.warnings.find(c.warnings) != std::string::npos);
    }
    // auto leaves a dimension to the self-test, but a linear netlist has nothing to choose; simulator directives are
    // ignored with a note
    const fc::Result r = fc::generate(head + ".oversample auto\n.adaa auto\n.solver auto\n.tran 1u 1m\n", "t.cir");
    CHECK(!r.provisional);
    CHECK(r.selfTestKey.empty());
    CHECK(r.notes.find("provisional") == std::string::npos);
    CHECK(r.notes.find("t.cir:8:1: note: '.tran' is a simulator's directive: ignored") != std::string::npos);
    // with a port, they (or no directive) are open, and a provisional configuration plays until the self-test answers
    const std::string port = ".input u in\n.output y o\nR1 in o 1k\nC1 o 0 1n\nD1 o 0 DD\nD2 0 o DD\n"
                             ".model DD D(IS=2.52n N=1.752)\n";
    for (const std::string& open : {std::string(), std::string(".oversample auto\n.adaa auto\n.solver auto\n"),
                                     std::string(".solver omega\n")})
    {
        INFO(open);
        const fc::Result p = fc::generate(port + open, "t.cir");
        CHECK(p.provisional);
        CHECK(p.selfTestKey.size() == 16);
        CHECK(p.notes.find("t.cir: note: the self-test chooses this circuit's configuration; provisional: 8x IIR, "
                           "omega(2)\n") != std::string::npos);
    }
    CHECK(r.notes.find("t.cir: note: block 1 (R1, C1): 1 state, linear, at the host rate") != std::string::npos);
    CHECK(errorsOf(head + ".model Q1 NPN(BF=100)\n") ==
          "t.cir:5:11: error: model type 'NPN' is not supported yet (Phase 2): Phase 0 has diodes (D)\n");
    CHECK(errorsOf(head + ".model D9 D(IS=1n XYZ=3)\n") == "t.cir:5:19: error: unknown diode parameter 'XYZ'\n");
    // ADAA delays each port block it runs in by half a step: two clipping stages at 4x, a quarter of a host sample
    const fc::Result two = fc::generate(".input u in\n.output y b\n.oversample 4\n.adaa on\nR1 in a 1k\nC1 a 0 10n\n"
                                        "D1 a 0 DD\nD2 0 a DD\nX1 a o o opamp\nR2 o b 1k\nC2 b 0 10n\nD3 b 0 DD\n"
                                        "D4 0 b DD\n.model DD D(IS=2.52n N=1.752)\n",
                                        "t.cir");
    INFO(two.errors);
    CHECK(two.errors.empty());
    CHECK(two.notes.find("(IIR halfbands), plus 0.25 (ADAA)") != std::string::npos);
    CHECK(two.source.find("phase not linear), plus 0.25 (ADAA)") != std::string::npos);
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
    // blocks=auto: block 2; block 3 fails the 1x check, and its matched design passes it, so it runs at 1x
    CHECK(ts.regionFirst == 1);
    CHECK(ts.regionLast == 1);
    CHECK(ts.blocks[1].rateFactor == 4);
    CHECK(ts.blocks[1].designFactor == 2.0 / 3.0);
    CHECK(ts.blocks[2].rateFactor == 1);
    CHECK(ts.blocks[2].design == fc::Block::Design::Matched);
    CHECK(ts.blocks[2].matchedError <= 0.5);
    CHECK(ts.blocks[2].oneX > 13);
    CHECK(ts.blocks[0].rateFactor == 1);
    CHECK(ts.blocks[3].rateFactor == 1);
    CHECK(ts.blocks[0].design == fc::Block::Design::Tr);
    CHECK(ts.blocks[3].design == fc::Block::Design::Tr);

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

TEST_CASE("the OTA and law inputs parse, and Result lists the inputs", "[circuit]")
{
    const Parsed p = parsed(fc::test::otaStage());
    INFO(p.errors);
    REQUIRE(p.errors.empty());
    const fc::Element& g = p.netlist.elements[4];
    CHECK(g.kind == fc::Element::Kind::Ota);
    CHECK(g.nodes == std::vector<std::string>{"0", "y", "a1", "a2"});
    REQUIRE(g.expr);
    CHECK(fc::potsOf(*g.expr) == std::vector<std::string>{"iabc"});
    REQUIRE(p.netlist.input);
    CHECK(p.netlist.input->name == "u");
    REQUIRE(p.netlist.laws.size() == 1);
    CHECK(p.netlist.laws[0].name == "iabc");
    CHECK(p.netlist.laws[0].kind == fc::Input::Kind::Current);
    CHECK_THAT(p.netlist.laws[0].low, WithinRel(1e-5, 1e-15));
    CHECK_THAT(p.netlist.laws[0].high, WithinRel(2e-3, 1e-15));
    // a cv input's range is 0:1 unless it says; a constant gain is a number
    const Parsed cv = parsed(".input u in\n.input c - kind=cv scale=2m\n.output y yb\nR1 in a1 100k\nR2 a1 0 220\n"
                             "G1 0 y OTA a1 yb {c}\nC1 y 0 1n\nR5 y 0 10meg\nX1 y yb yb opamp\n");
    REQUIRE(cv.netlist.laws.size() == 1);
    CHECK(cv.netlist.laws[0].low == 0);
    CHECK(cv.netlist.laws[0].high == 1);
    CHECK(cv.netlist.laws[0].scale == 2e-3);
    CHECK(parsed(fc::test::otaStage("", "1m")).netlist.elements[4].value == 1e-3);

    // tick's and step's arguments: the signal, then the law inputs, in .input order
    const fc::Result r = fc::generate(fc::test::kOtaLp, "otalp.cir");
    INFO(r.errors);
    REQUIRE(r.inputs.size() == 2);
    CHECK(r.inputs[0].name == "u");
    CHECK(r.inputs[0].kind == fc::Input::Kind::Voltage);
    CHECK(r.inputs[1].name == "iabc");
    CHECK(r.inputs[1].kind == fc::Input::Kind::Current);
    CHECK_THAT(r.inputs[1].low, WithinRel(1e-5, 1e-15));
    CHECK_THAT(r.inputs[1].high, WithinRel(2e-3, 1e-15));
    CHECK(r.source.find("float tick(float u, float iabc)") != std::string::npos);
    CHECK(r.source.find("float step(float u, float iabc)") != std::string::npos);
}

TEST_CASE("the OTA's and the law inputs' errors, as clang words them", "[circuit]")
{
    struct Case
    {
        std::string netlist;
        std::string errors;
    };
    const std::string stage = fc::test::otaStage();
    const std::string law = ".input iabc - kind=current range=10u:2m\n";
    // two OTA stages into one capacitor: one block, two ports
    const std::string twoOtas = ".input u in\n" + law +
                                ".output y yb\nR1 in a1 100k\nR2 a1 0 220\nR3 yb a2 100k\nR4 a2 0 220\n"
                                "G1 0 y OTA a1 a2 {iabc}\nG2 0 y OTA a1 a2 {iabc}\nC1 y 0 1n\nR5 y 0 10meg\n"
                                "X1 y yb yb opamp\n";
    // a VCA: nothing the OTA drives reaches its inputs
    const std::string openLoop = ".input u in\n" + law +
                                 ".output y y\nR1 in a1 100k\nR2 a1 0 220\nG1 0 y OTA a1 0 {iabc}\nR5 y 0 10k\n";
    // a diode clipper, then an OTA stage
    const std::string mixed = ".input u in\n" + law +
                              ".output y yb\nR0 in c 2.2k\nC0 c 0 10n\nD1 c 0 DM\nD2 0 c DM\nX0 c in2 in2 opamp\n"
                              "R1 in2 a1 100k\nR2 a1 0 220\nR3 yb a2 100k\nR4 a2 0 220\nG1 0 y OTA a1 a2 {iabc}\n"
                              "C1 y 0 1n\nR5 y 0 10meg\nX1 y yb yb opamp\n.model DM D(IS=2.52n N=1.752)\n";
    // the OTA's output into its + input's side: positive feedback
    const std::string positive = ".input u in\n" + law +
                                 ".output y yb\nR1 in a1 100k\nR2 a1 0 220\nR3 yb a2 100k\nR4 a2 0 220\n"
                                 "G1 0 y OTA a2 a1 {iabc}\nC1 y 0 1n\nR5 y 0 10meg\nX1 y yb yb opamp\n";
    const Case cases[] = {
        {twoOtas, "t.cir:4:1: error: block 1 has 2 nonlinear ports (G1, G2): multi-port Newton is Phase 2\n"},
        {openLoop, "t.cir:6:1: error: OTA G1 has no feedback around it (an open-loop OTA, a VCA): Phase 2\n"},
        {mixed, "t.cir:13:1: error: the oversampled region mixes diode ports (D1|D2) and OTA ports (G1): one solver "
                "serves a region (per-family solvers are Phase 2)\n"},
        {positive, "t.cir:8:1: error: OTA G1 sees positive feedback (kd = -24.86 ohm at 22.05 kHz): its output drives "
                   "its + input; not supported yet\n"},
        {fc::test::otaStage(".solver omega(1)\n"),
         "t.cir:5:1: error: '.solver omega(1)': an OTA's port is solved from the previous step's v: .solver "
         "newton(K)\n"},
        {fc::test::otaStage(".solver omega\n"),
         "t.cir:5:1: error: '.solver omega': an OTA's port is solved from the previous step's v: .solver newton(K)\n"},
        {".input u in\n.input iabc - kind=current\n.output y o\nR1 in o 1k\nR2 o 0 1k\n",
         "t.cir:2:15: error: law input 'iabc' is a current: it needs its range in amps, range=lo:hi (as "
         "range=10u:2m)\n"},
        {".input u in\n" + law + ".output y o\nR1 in o {1k + 1meg * iabc}\nR2 o 0 1k\n",
         "t.cir:4:22: error: law input 'iabc' in R1's value: a law input enters an OTA's gain only (pot-as-port is "
         "Phase 2)\n"},
        {".input u in\n.input c in kind=cv\n.output y o\nR1 in o 1k\nR2 o 0 1k\n",
         "t.cir:2:10: error: 'in': a law input (kind=cv) has no node; write '-' (it enters an OTA's gain, not the "
         "netlist)\n"},
        {".input u -\n.output y o\nR1 in o 1k\nR2 o 0 1k\n",
         "t.cir:1:10: error: '-' is a law input's node: a law input says kind=cv or kind=current\n"},
        {".input u in\n.input c - kind=cv range=1:0\n.output y o\nR1 in o 1k\nR2 o 0 1k\n",
         "t.cir:2:20: error: 'range=1:0': range=lo:hi, two numbers with lo below hi\n"},
        {".input u in\n.input u - kind=cv\n.output y o\nR1 in o 1k\nR2 o 0 1k\n",
         "t.cir:2:8: error: input 'u' is defined twice (first at line 1)\n"},
        {".input u in\n.input v in\n.output y o\nR1 in o 1k\nR2 o 0 1k\n",
         "t.cir:2:1: error: a second voltage .input (the first at line 1): a netlist has one signal (law inputs are "
         "kind=cv|current)\n"},
        {".input u in\n.pot iabc\n" + law + ".output y o\nR1 in o {1k + iabc}\nR2 o 0 1k\n",
         "t.cir:3:8: error: law input 'iabc' has pot 'iabc''s name (line 2)\n"},
        {fc::test::otaStage("", "{iabcc}"),
         "t.cir:9:19: error: unknown pot or law input 'iabcc' (there are no pots; the law inputs: iabc)\n"},
        {fc::test::otaStage("", "{iabc - 1m}"),
         "t.cir:9:1: error: OTA G1's gain is -0.00099 A at iabc 1e-05: an OTA's gain (its bias current) is not "
         "negative\n"},
        {fc::test::otaStage("", "0"),
         "t.cir:9:18: error: G1's gain is 0: an OTA's gain (its bias current) is positive\n"},
        // a linear G's gain is no OTA's: a law input may not enter it
        {".input u in\n" + law + ".output y o\nR1 in o 1k\nG1 o 0 in 0 {iabc}\n",
         "t.cir:5:14: error: law input 'iabc' in G1's value: a law input enters an OTA's gain only (pot-as-port is "
         "Phase 2)\n"},
        {".input u in\n.input step - kind=cv\n.output y o\nR1 in o 1k\nR2 o 0 1k\n",
         "t.cir:2:8: error: law input 'step' is an FMinus keyword or builtin, or a name the module uses: rename it\n"},
    };
    for (const Case& c : cases)
    {
        INFO(c.netlist);
        CHECK(errorsOf(c.netlist) == c.errors);
    }
    // an unused law input warns, as an unused pot does
    const fc::Result r =
        fc::generate(".input u in\n.input cv - kind=cv\n.output y o\nR1 in o 1k\nR2 o 0 1k\n", "t.cir");
    CHECK(r.errors.empty());
    CHECK(r.warnings == "t.cir:2:8: warning: law input 'cv' is never used\n");
}

TEST_CASE("the OTA port is §7.2's", "[circuit]")
{
    // one buffered stage: v = att (u - y), att = 220 / 100220; C1's KCL G y = x + i, G = 2 C fs + 1 / 10 MEG; so p =
    // att u - att x / G and kd = att / G (the hand derivation of report §7.2, with the buffer's y)
    const fc::Plan p = planned(fc::test::otaStage());
    REQUIRE(p.blocks.size() == 1);
    REQUIRE(p.blocks[0].port);
    CHECK(p.blocks[0].port->law == fc::PortDef::Law::Tanh);
    CHECK(p.blocks[0].port->form == fc::PortDef::Form::I);
    CHECK(p.smooth);
    const fc::System s = fc::assemble(p, 0);
    CHECK(s.iForm);
    fc::Messages messages;
    const std::optional<fc::Pivots> pivots = fc::choosePivots(s, p, 0, messages);
    REQUIRE(pivots);
    CHECK(fc::checkPort(s, *pivots, p, 0, messages));
    const double att = 220.0 / 100220.0;
    for (double fs : {44100.0, 96000.0, 384000.0})
    {
        const fc::Linear<double> l = fc::solve(s, *pivots, fc::symbolValues(s, p, 0, fs, {}));
        std::map<std::string, double> c;
        for (const auto& [name, value] : fc::derive(s, l, p.blocks[0], false))
            c[name] = value;
        const double g = 2 * 1e-9 * fs + 1e-7;
        INFO(fs);
        CHECK(near(c["kd_G1"], att / g, 0));
        CHECK(c["kd_G1"] > 0);
        CHECK(near(c["p_G1_by_u"], att, 0));
        CHECK(near(c["p_G1_by_x_C1"], -att / g, 0));
        // the output and C1's voltage are (x + i) / G: no term in u, and so no difference of u and v
        CHECK(!c.count("n_yb_by_u"));
        CHECK(near(c["n_yb_by_x_C1"], 1 / g, 0));
        CHECK(near(c["n_yb_by_i_G1"], 1 / g, 0));
    }
    // the prewarp's fit: the pole a + b g, a = -1 / (10 MEG 1 nF), b = -att / (2 Vt 1 nF)
    const fc::Plan warped = planned(fc::test::otaStage(".prewarp on\n"));
    const fc::PortDef& port = *warped.blocks[0].port;
    REQUIRE(port.prewarp);
    CHECK_THAT(port.poleA, WithinRel(-1 / (10e6 * 1e-9), 1e-9));
    CHECK_THAT(port.poleB, WithinRel(-att / (2 * fc::thermalVoltage(1) * 1e-9), 1e-9));
}

TEST_CASE("F is the antiderivative of each law's solution", "[circuit]")
{
    // F(p) = p v - H(v), H = v^2 / 2 + k Phi(v), v solving v + k phi(v) = p: dF/dp = v (report §5.1), for the pair
    // (k = 2 Is K, phi = sinh(v / N Vt)) and tanh (k = kd g, phi = tanh(v / 2 Vt)), at 200 points over +-20 V
    struct Law
    {
        fc::PortDef::Law law;
        double k;
        double s;
    };
    for (const Law& l : {Law{fc::PortDef::Law::Pair, 2 * 2.52e-9 * 2200, fc::thermalVoltage(1.752)},
                         Law{fc::PortDef::Law::Tanh, 0.0686, 2 * fc::thermalVoltage(1)}})
    {
        auto v = [&](double p)
        {
            return fc::lawConverged(l.law, p, l.k, l.s, 0, nullptr, nullptr);
        };
        auto f = [&](double p)
        {
            const double x = v(p);
            return p * x - (0.5 * x * x + l.k * fc::lawIntegral(l.law, x, l.s));
        };
        double worst = 0;
        for (int i = 0; i < 200; ++i)
        {
            const double p = -20 + 40 * (i + 0.5) / 200;
            const double h = 1e-5;
            worst = std::max(worst, std::fabs((f(p + h) - f(p - h)) / (2 * h) - v(p)));
        }
        INFO(static_cast<int>(l.law));
        CHECK(worst <= 1e-7);
    }
}

TEST_CASE("the bracket keeps every law's iterates finite, in double", "[circuit]")
{
    // v stays in [min(0, p), max(0, p)] for p from 1e-6 to 1e6 V, either sign, 0 to 3 Newton steps, from any start
    for (fc::PortDef::Law law : {fc::PortDef::Law::Pair, fc::PortDef::Law::Tanh})
    {
        const double s = law == fc::PortDef::Law::Pair ? fc::thermalVoltage(1.752) : 2 * fc::thermalVoltage(1);
        for (double k : {1e-9, 1e-3, 1.0, 1e3})
        {
            for (int steps = 0; steps <= 3; ++steps)
            {
                for (double mag = 1e-6; mag <= 1e6; mag *= 10)
                {
                    for (double p : {mag, -mag})
                    {
                        for (double previous : {0.0, p, -p, 1e30})
                        {
                            const fc::Solver::Guess guess = law == fc::PortDef::Law::Pair ? fc::Solver::Guess::Omega
                                                                                          : fc::Solver::Guess::Previous;
                            const double v = fc::lawSteps(law, p, k, s, guess, previous, steps);
                            INFO(static_cast<int>(law) << " k " << k << " steps " << steps << " p " << p);
                            CHECK(std::isfinite(v));
                            CHECK(v >= std::fmin(0.0, p));
                            CHECK(v <= std::fmax(0.0, p));
                        }
                    }
                }
            }
        }
    }
}

TEST_CASE("the carry keeps the stored point's Newton residual, in double", "[circuit]")
{
    // a step of the law input moves the curve, p = v + kg tanh(v / 2 Vt); the carry p_P += (kg - kg_P) t_P (t_P =
    // tanh(v_P / 2 Vt)) moves the stored point onto the new one, its residual p_P - v_P - kg t_P unchanged: the
    // carry's algebra, in double (example-circuits --check, m, measures the emitted carry: a module without it fails)
    const double s = 2 * fc::thermalVoltage(1);
    for (double p : {1e-4, 3e-3, 0.02, -0.05})
    {
        for (int steps : {1, 2})
        {
            const double kg0 = 0.0686 * 0.3;
            const double v = fc::lawSteps(fc::PortDef::Law::Tanh, p, kg0, s, fc::Solver::Guess::Previous, 0, steps);
            const double t = std::tanh(v / s);
            const double before = p - v - kg0 * t;
            for (double kg1 : {kg0 * 4, kg0 / 4, 0.0})
            {
                const double carried = p + (kg1 - kg0) * t;
                const double after = carried - v - kg1 * t;
                CHECK(std::fabs(after - before) <= 1e-12 * std::fabs(p));
            }
        }
    }
}

TEST_CASE("prewarp applies to a one-state OTA block, and warns elsewhere", "[circuit]")
{
    // off by default (the user's ruling): the module's cutoff law is the circuit's
    const fc::Result off = fc::generate(fc::test::otaStage(), "t.cir");
    CHECK(off.source.find("pw1_") == std::string::npos);
    CHECK(off.configuration.find(".prewarp off (default)") != std::string::npos);
    const fc::Result on = fc::generate(fc::test::otaStage(".prewarp on\n"), "t.cir");
    INFO(on.errors);
    CHECK(on.warnings.empty());
    CHECK(on.source.find("g_G1 = pw1_(l_iabc);") != std::string::npos);
    CHECK(on.notes.find("t.cir: note: OTA G1: prewarp; its small-signal pole is a + b g, a = -100 rad/s, b = "
                        "-4.243528e+07 rad/s per A (g 1e-05 to 0.002 A)") != std::string::npos);
    // two states in the block: ignored, with a warning at the OTA
    const std::string twoStates = withLineAdded(fc::test::otaStage(".prewarp on\n"), "C2 y z 1n\nR6 z 0 1k\n");
    const fc::Result two = fc::generate(twoStates, "t.cir");
    INFO(two.errors);
    CHECK(two.warnings ==
          "t.cir:10:1: warning: prewarp is ignored for OTA G1: its block has 2 states (it prewarps a one-pole's "
          "cutoff)\n");
    // an inductor is a state as a capacitor is: a block with C1 and an inductor has 2 states
    const fc::Result withL =
        fc::generate(withLineAdded(fc::test::otaStage(".prewarp on\n"), "L1 y z 10\nR6 z 0 10meg\n"), "t.cir");
    INFO(withL.errors);
    CHECK(withL.warnings ==
          "t.cir:10:1: warning: prewarp is ignored for OTA G1: its block has 2 states (it prewarps a one-pole's "
          "cutoff)\n");
    // a G whose gain pots move (a leak the pot sets) moves the pole the fit is made for: ignored
    const fc::Result potLeak =
        fc::generate(withLineAdded(fc::test::otaStage(".prewarp on\n.pot x\n"), "G2 y 0 y 0 {1u + 100u * x}\n"),
                     "t.cir");
    INFO(potLeak.errors);
    CHECK(potLeak.warnings == "t.cir:11:1: warning: prewarp is ignored for OTA G1: pots move its block's pole, and the "
                              "fit is made at generation\n");
    // and a netlist without an OTA
    CHECK(fc::generate(".input u in\n.output y o\n.prewarp on\nR1 in o 1k\nR2 o 0 1k\n", "t.cir").warnings ==
          "t.cir:3:1: warning: '.prewarp on' is ignored: it prewarps an OTA's cutoff, and the netlist has none\n");
}

TEST_CASE("an OTA module's step names are refused as names", "[circuit]")
{
    // every name the OTA's step, its law-input lines and its ADAA tail use, as a pot, an input and the module
    for (const char* name : {"kg", "kgs", "kgv", "tn", "th", "ib", "lc", "hh", "h2", "s2", "z", "zu", "dl", "wm"})
    {
        INFO(name);
        CHECK(fc::isReservedName(name));
        CHECK(fc::isModuleWord(name));
        const std::string head = ".input u in\n.output y o\n";
        CHECK(errorsOf(head + ".pot " + name + "\nR1 in o {1k + " + name + "}\nR2 o 0 1k\n") ==
              "t.cir:3:6: error: pot '" + std::string(name) +
                  "' is an FMinus keyword or builtin, or a name the module uses: rename it\n");
        CHECK(errorsOf(".input " + std::string(name) + " in\n.output y o\nR1 in o 1k\nR2 o 0 1k\n") ==
              "t.cir:1:8: error: .input '" + std::string(name) +
                  "' is an FMinus keyword or builtin, or a name the module uses: rename it\n");
        CHECK(errorsOf(".title " + std::string(name) + "\n" + head + "R1 in o 1k\nR2 o 0 1k\n") ==
              "t.cir:1:8: error: the module's name '" + std::string(name) +
                  "', from .title, is a name the module uses: rename it\n");
    }
}

// ---- Phase 1's elements, the DC operating point and the diode forms ------------------------------------------------

namespace
{
    // The DC point at the plan's default pots (it must have one).
    fc::DcPoint dcOf(const fc::Plan& plan)
    {
        std::vector<double> pots;
        for (const fc::PotInfo& p : plan.pots)
            pots.push_back(p.defaultValue);
        fc::Messages messages;
        const std::optional<fc::DcPoint> point = fc::solveDc(plan, pots, messages);
        INFO(messages.format("t.cir", fc::Severity::Error));
        REQUIRE(point);
        return *point;
    }

    double nodeDc(const fc::Plan& plan, const std::string& name)
    {
        for (std::size_t n = 0; n < plan.nodes.size(); ++n)
        {
            if (plan.nodes[n].name == name)
                return plan.dc[n];
        }
        FAIL("no node " << name);
        return 0;
    }

    std::string partsOf(const fc::Plan& plan, const fc::Block& b)
    {
        std::string out;
        for (int e : b.elements)
            out += (out.empty() ? "" : " ") + plan.netlist.elements[static_cast<std::size_t>(e)].name;
        return out;
    }
} // namespace

TEST_CASE("an inductor, an E and a G stamp as their equations say", "[circuit]")
{
    // L1 in -> o, R1 o -> 0: the unknowns V(o) and i(L1); KCL o: V(o) / 1k - i(L1) = 0; L1's row in the impedance
    // form: V(in) - V(o) - rl i(L1) = -x_L1, rl = 2 L fs' a symbol
    const fc::Plan rl = planned(fc::test::kRlLowpass);
    REQUIRE(rl.blocks.size() == 1);
    const fc::System s = fc::assemble(rl, 0);
    CHECK(s.unknownNames == (std::vector<std::string>{"V(o)", "i(L1)"}));
    CHECK(s.rowNames == (std::vector<std::string>{"KCL o", "L1"}));
    REQUIRE(s.symbols.size() == 1);
    CHECK(s.symbols[0].name == "rl_L1");
    CHECK(s.symbols[0].kind == fc::System::Symbol::Kind::Rl);
    CHECK(s.a[0][0].constant == 1 / 1000.0);
    CHECK(s.a[0][1].constant == -1);
    CHECK(s.a[1][0].constant == -1);
    CHECK(s.a[1][1].constant == 0);
    CHECK(s.a[1][1].terms == (std::vector<std::pair<int, double>>{{0, -1.0}}));
    REQUIRE(s.sources.size() == 2);
    CHECK(s.sources[1].name == "x_L1");
    CHECK(s.rhs[0][1].constant == -1);   // V(in), on the right
    CHECK(s.rhs[1][1].constant == -1);   // -x_L1
    CHECK(s.rhs[1][0].isZero());
    REQUIRE(s.capacitor.size() == 1);   // the state's update reads the inductor's current
    CHECK(s.readouts[static_cast<std::size_t>(s.capacitor[0].first)].name == "il_L1");
    // at DC (rate 0, rl = 0) an exact short: V(o) = V(in)
    CHECK(fc::symbolValues(s, rl, 0, 0.0, {}) == std::vector<double>{0.0});
    CHECK(fc::symbolValues(s, rl, 0, 48000.0, {}) == std::vector<double>{2 * 0.01 * 48000});

    // E1: V(out) = 11 V(in), a buffer block of its own (both controls the input and ground): its row
    const fc::Plan e = planned(".input u in\n.output y out\nE1 out 0 in 0 11\nR1 out 0 1k\n");
    REQUIRE(e.blocks.size() == 1);
    const fc::System se = fc::assemble(e, 0);
    CHECK(se.unknownNames == std::vector<std::string>{"V(out)"});
    CHECK(se.rowNames == std::vector<std::string>{"E1"});
    CHECK(se.a[0][0].constant == 1);
    CHECK(se.rhs[0][0].constant == 11);
    for (double f : {20.0, 1000.0, 20000.0})
        CHECK(std::abs(fc::analog(e, 0, Complex(0, 2 * std::numbers::pi * f), {}) - Complex(11)) <= 1e-12 * 11);

    // G1 into R1: 2 mA/V of V(in) leaving o, so V(o) = -g R V(in) = -2 V(in); its control the block's input, a
    // right-hand side
    const fc::Plan g = planned(".input u in\n.output y o\nG1 o 0 in 0 2m\nR1 o 0 1k\n");
    const fc::System sg = fc::assemble(g, 0);
    CHECK(sg.rowNames == std::vector<std::string>{"KCL o"});
    CHECK(sg.a[0][0].constant == 1 / 1000.0);
    CHECK(sg.rhs[0][0].constant == -0.002);
    CHECK(std::abs(fc::analog(g, 0, Complex(0, 1000), {}) - Complex(-2)) <= 1e-12 * 2);
    // a pot moves its gain: k_G1, a symbol
    const fc::Plan gp = planned(".input u in\n.output y o\n.pot x\nG1 o 0 in 0 {1m + 2m * x}\nR1 o 0 1k\n");
    const fc::System sgp = fc::assemble(gp, 0);
    REQUIRE(sgp.symbols.size() == 1);
    CHECK(sgp.symbols[0].name == "k_G1");
    CHECK(sgp.symbols[0].kind == fc::System::Symbol::Kind::Gain);
    CHECK(fc::symbolValues(sgp, gp, 0, 48000, {0.25}) == std::vector<double>{0.001 + 0.002 * 0.25});
    CHECK(std::abs(fc::analog(gp, 0, Complex(0, 1000), {1.0}) - Complex(-3)) <= 1e-12 * 3);

    // a floating E (neither output terminal ground) is a row in its block, with its current an unknown: V(a) - V(b) =
    // 2 V(c), R3 and R4 to ground, so V(b) = -V(c) = -V(in) / 2
    const fc::Plan fe =
        planned(".input u in\n.output y b\nR1 in c 1k\nR2 c 0 1k\nE1 a b c 0 2\nR3 a 0 1k\nR4 b 0 1k\n");
    REQUIRE(fe.blocks.size() == 1);
    CHECK(partsOf(fe, fe.blocks[0]) == "R1 R2 E1 R3 R4");
    const fc::System sfe = fc::assemble(fe, 0);
    CHECK(sfe.unknownNames.back() == "i(E1)");
    CHECK(sfe.rowNames.back() == "E1");
    CHECK(std::abs(fc::analog(fe, 0, Complex(0, 1000), {}) - Complex(-0.5)) <= 1e-12);

    // a grounded E cuts like an op amp's output: its controls' block drives b, the next block reads it
    const fc::Plan cut = planned(".input u in\n.output y c\nR1 in a 1k\nR2 a 0 1k\nE1 b 0 a 0 2\nR3 b c 1k\n"
                                 "C1 c 0 1n\n");
    REQUIRE(cut.blocks.size() == 2);
    CHECK(partsOf(cut, cut.blocks[0]) == "R1 R2 E1");
    CHECK(partsOf(cut, cut.blocks[1]) == "R3 C1");
    CHECK(cut.nodes[static_cast<std::size_t>(cut.blocks[1].input)].name == "b");
    CHECK(std::abs(fc::analog(cut, 0, Complex(0, 1000), {}) - Complex(1)) <= 1e-12);   // 2 x the divider's 1/2
    CHECK(fc::generate(".input u in\n.output y c\nR1 in a 1k\nR2 a 0 1k\nE1 b 0 a 0 2\nR3 b c 1k\nC1 c 0 1n\n",
                       "t.cir")
              .source.find("cut at E1's output (exact)") != std::string::npos);
    // its n+ grounded: V(n-) = -gain V(control)
    const fc::Plan flipped = planned(".input u in\n.output y b\nE1 0 b in 0 3\nR1 b 0 1k\n");
    CHECK(std::abs(fc::analog(flipped, 0, Complex(0, 1000), {}) - Complex(-3)) <= 1e-12 * 3);
}

TEST_CASE("with inductors, the trapezoidal blocks are the bilinear map of their analog response", "[circuit]")
{
    // the emitted derivation's response (its Holters form, in double) is H(j 2 fs tan(pi f / fs)) within 1e-9
    // relative from 20 Hz to 20 kHz: an RL low-pass and the wah's RLC band-pass, at each point of its pot grid
    for (const std::string& text : {fc::test::kRlLowpass, fc::test::kWah})
    {
        const fc::Plan plan = planned(text);
        REQUIRE(plan.blocks.size() == 1);
        for (double fs : {44100.0, 48000.0, 96000.0})
        {
            for (const std::vector<double>& pots : fc::potGrid(plan, plan.blocks[0]))
            {
                const fc::Holters h = fc::holters(plan, 0, fs, pots);
                double worst = 0;
                for (double f = 20; f <= 20000; f *= 1.05)
                {
                    const Complex digital = stateSpace(h, std::polar(1.0, 2 * std::numbers::pi * f / fs));
                    const Complex analog =
                        fc::analog(plan, 0, Complex(0, 2 * fs * std::tan(std::numbers::pi * f / fs)), pots);
                    worst = std::fmax(worst, std::abs(digital - analog) / std::abs(analog));
                }
                INFO(plan.module << " at " << fs);
                CHECK(worst < 1e-9);
            }
        }
    }
    // and the analog responses are the textbook's: R / (R + s L); the tank's, a band-pass peaking at 1 / (2 pi
    // sqrt(L C)) with V(a) / V(in) = Rd / (R1 + Rd) there (Rd = R2 + Pq: the reactances cancel)
    const fc::Plan rl = planned(fc::test::kRlLowpass);
    for (double f : {20.0, 1000.0, 15915.5, 20000.0})
    {
        const Complex s(0, 2 * std::numbers::pi * f);
        CHECK(std::abs(fc::analog(rl, 0, s, {}) - 1000.0 / (1000.0 + s * 0.01)) <= 1e-12);
    }
    const fc::Plan wah = planned(fc::test::kWah);
    const double f0 = 1 / (2 * std::numbers::pi * std::sqrt(0.5 * 47e-9));
    for (double res : {0.0, 0.5, 1.0})
    {
        const double rd = 1000 + 22000 * fc::taperValue(fc::Taper::Audio, res);
        const Complex h = fc::analog(wah, 0, Complex(0, 2 * std::numbers::pi * f0), {res});
        CHECK(std::abs(h - rd / (10000 + rd)) <= 1e-9);
    }
}

TEST_CASE("a switch parses and plans as a ramped variable branch", "[circuit]")
{
    const std::string text = ".title Sw\n.input u in\n.output y a\n.switch mode 3 1\nR1 in a 10k\nC1 a 0 10n\n"
                             "S1 a b mode 1,2 ron=10 roff=1meg\nC2 b 0 47n\n";
    const Parsed p = parsed(text);
    INFO(p.errors);
    REQUIRE(p.errors.empty());
    REQUIRE(p.netlist.switches.size() == 1);
    CHECK(p.netlist.switches[0].name == "mode");
    CHECK(p.netlist.switches[0].positions == 3);
    CHECK(p.netlist.switches[0].defaultPosition == 1);
    const fc::Element& s1 = p.netlist.elements[2];
    CHECK(s1.kind == fc::Element::Kind::Switch);
    CHECK(s1.closed == std::vector<int>{1, 2});
    CHECK(s1.ron == 10);
    CHECK(s1.roff == 1e6);
    // its positions are a space apart too ("1, 2"), and ron and roff default to 1 and 10meg
    const Parsed spaced = parsed(".input u in\n.output y a\n.switch mode 3\nR1 in a 10k\nS1 a 0 mode 1, 2\n");
    REQUIRE(spaced.errors.empty());
    CHECK(spaced.netlist.elements[1].closed == std::vector<int>{1, 2});
    CHECK(spaced.netlist.elements[1].ron == 1);
    CHECK(spaced.netlist.elements[1].roff == 10e6);

    const fc::Plan plan = planned(text);
    REQUIRE(plan.switches.size() == 1);
    REQUIRE(plan.blocks.size() == 1);
    const fc::Block& block = plan.blocks[0];
    CHECK(block.switches == std::vector<std::string>{"mode"});
    CHECK(block.hasControls());
    const auto sw = std::find_if(block.branches.begin(), block.branches.end(),
                                 [](const fc::Branch& b) { return b.isSwitch(); });
    REQUIRE(sw != block.branches.end());
    // its grid is its positions; its resistance is ron where it is closed, roff where open, settled
    const std::vector<std::vector<double>> grid = fc::potGrid(plan, block);
    CHECK(grid == std::vector<std::vector<double>>{{0}, {1}, {2}});
    const fc::System system = fc::assemble(plan, 0);
    const auto r = std::find_if(system.symbols.begin(), system.symbols.end(),
                                [](const fc::System::Symbol& y) { return y.name == "r_S1"; });
    REQUIRE(r != system.symbols.end());
    const auto at = static_cast<std::size_t>(r - system.symbols.begin());
    CHECK_THAT(fc::symbolValues(system, plan, 0, 48000, {0})[at], WithinRel(1e6, 1e-12));
    CHECK_THAT(fc::symbolValues(system, plan, 0, 48000, {1})[at], WithinRel(10.0, 1e-12));
    CHECK_THAT(fc::symbolValues(system, plan, 0, 48000, {})[at], WithinRel(10.0, 1e-12));   // the default, 1
    const fc::Result result = fc::generate(text, "t.cir");
    CHECK(result.switches == std::vector<std::string>{"mode"});
    CHECK(result.source.find("void set(int mode)") != std::string::npos);
}

TEST_CASE("a switch's errors, as clang words them", "[circuit]")
{
    const std::string head = ".input u in\n.output y a\nR1 in a 10k\nC1 a 0 10n\n";
    struct Case
    {
        std::string netlist;
        std::string errors;
    };
    const Case cases[] = {
        {head + ".switch mode 3\nS1 a 0 sel 1\n", "t.cir:6:8: error: unknown switch 'sel' (the switches: mode)\n"},
        {head + "S1 a 0 sel 1\n", "t.cir:5:8: error: unknown switch 'sel' (there are no .switch lines)\n"},
        {head + ".switch mode 3\nS1 a 0 mode 1,3\n",
         "t.cir:6:13: error: S1 is closed at position 3 of switch 'mode', whose positions are 0 to 2\n"},
        {head + ".switch mode 2\nS1 a 0 mode 1 ron=0\n",
         "t.cir:6:15: error: 'ron=0': S1's ron is out of range for float coefficients (1e-30 to 1e30 ohms)\n"},
        {head + ".switch mode 2\nS1 a 0 mode 1 roff=2e30\n",
         "t.cir:6:15: error: 'roff=2e30': S1's roff is out of range for float coefficients (1e-30 to 1e30 "
         "ohms)\n"},
        {head + ".switch mode 2\nS1 a 0 mode 1 on=3\n",
         "t.cir:6:15: error: 'on=3': a switch takes ron= and roff= after its positions\n"},
        {head + ".switch mode 2\nS1 a 0 mode one\n",
         "t.cir:6:13: error: 'one': S1's positions are the switch's positions it is closed at, as 0,2\n"},
        {head + ".switch mode 1\nS1 a 0 mode 0\n", "t.cir:5:14: error: '1': a switch has 2 to 8 positions\n"},
        {head + ".switch mode 3 5\nS1 a 0 mode 0\n",
         "t.cir:5:16: error: switch 'mode' defaults to 5; its positions are 0 to 2\n"},
        {head + ".switch mode 2\n.switch MODE 3\nS1 a 0 mode 0\n",
         "t.cir:6:9: error: switch 'MODE' is defined twice (first at line 5)\n"},
        {head + ".pot mode\n.switch Mode 2\nS1 a 0 mode 0\nR2 a 0 {10k * mode}\n",
         "t.cir:6:9: error: switch 'Mode' has pot 'mode''s name (line 5)\n"},
        {head + ".switch u 2\nS1 a 0 u 0\n", "t.cir:5:9: error: switch 'u' has input 'u''s name (line 1)\n"},
        {head + ".switch set 2\nS1 a 0 set 0\n",
         "t.cir:5:9: error: switch 'set' is an FMinus keyword or builtin, or a name the module uses: rename it\n"},
        {head + "S1 a\n", "t.cir:5:1: error: S1: expected 'S<name> a b SWITCH pos[,pos...] [ron=1] [roff=10meg]'\n"},
        // a switch that moves the DC point: the module runs in offsets from one
        {".input u in\n.output y a\n.switch mode 2\nV1 vb 0 4.5\nR1 vb a 10k\nR2 in a 10k\nS1 a 0 mode 1\n"
         "C1 a 0 10n\n",
         "t.cir:3:9: error: switch 'mode' at position 1 moves the DC operating point: the module runs in offsets from "
         "one DC point (one a switch moves is Phase 2's)\n"},
    };
    for (const Case& c : cases)
    {
        INFO(c.netlist);
        CHECK(errorsOf(c.netlist) == c.errors);
    }
    // an unused switch warns, as an unused pot does; '10M' is 10 milliohm
    const fc::Result unused = fc::generate(head + ".switch mode 2\n", "t.cir");
    CHECK(unused.warnings.find("t.cir:5:9: warning: switch 'mode' is never used\n") != std::string::npos);
    const fc::Result milli = fc::generate(head + ".switch mode 2\nS1 a 0 mode 1 roff=10M\n", "t.cir");
    CHECK(milli.warnings.find("t.cir:6:15: warning: '10M' is 10 milliohm in SPICE; write 10meg for a megohm\n") !=
          std::string::npos);
}

TEST_CASE("BDF2's blocks are the BDF2 map of their analog response", "[circuit]")
{
    // BDF2's companions (gc = 3 C fs' / 2, rl = 3 L fs' / 2, and each state's second history) make a linear block's
    // discrete response H(fs (3 - 4 z^-1 + z^-2) / 2), within 1e-9 relative from 20 Hz to 20 kHz: the TS chain's
    // linear blocks, an RL low-pass and the wah's tank, at each point of their pot grids
    struct Case
    {
        std::string text;
        std::vector<int> blocks;
    };
    const Case cases[] = {{fc::test::kScreamer, {0, 2, 3}}, {fc::test::kRlLowpass, {0}}, {fc::test::kWah, {0}}};
    for (const Case& c : cases)
    {
        const fc::Plan plan = planned(c.text);
        for (int b : c.blocks)
        {
            const fc::Block& block = plan.blocks[static_cast<std::size_t>(b)];
            for (double fs : {44100.0, 48000.0, 96000.0})
            {
                for (const std::vector<double>& pots : fc::potGrid(plan, block))
                {
                    const fc::Holters tr = fc::holters(plan, b, fs, pots);
                    const fc::Holters h = fc::holters(plan, b, fs, pots, {}, fc::Method::Bdf2);
                    CHECK(h.states == 2 * tr.states);   // [x; xo]
                    double worst = 0;
                    for (double f = 20; f <= 20000; f *= 1.05)
                    {
                        const Complex digital = stateSpace(h, std::polar(1.0, 2 * std::numbers::pi * f / fs));
                        const Complex analog =
                            fc::analog(plan, b, fc::discreteS(fc::Method::Bdf2, f, fs), pots);
                        if (std::abs(analog) < 1e-9)
                            continue;   // the level pot at 0: silence
                        worst = std::fmax(worst, std::abs(digital - analog) / std::abs(analog));
                    }
                    INFO(plan.module << " block " << b + 1 << " at " << fs);
                    CHECK(worst < 1e-9);
                }
            }
        }
    }
    // the companions' values, and BDF2's map at a low frequency: s within 1e-6 of j 2 pi f (second order)
    const fc::Plan rl = planned(fc::test::kRlLowpass);
    const fc::System s = fc::assemble(rl, 0);
    CHECK(fc::symbolValues(s, rl, 0, 48000.0, {}, fc::Method::Bdf2) == std::vector<double>{1.5 * 0.01 * 48000});
    const Complex low = fc::discreteS(fc::Method::Bdf2, 100, 48000);
    CHECK(std::abs(low - Complex(0, 2 * std::numbers::pi * 100)) <= 1e-4 * 2 * std::numbers::pi * 100);
}

TEST_CASE("the matched design: impulse-invariant poles, the analog magnitude at its three points", "[circuit]")
{
    // An RC low-pass at 8 kHz (one state), a Sallen-Key low-pass at 2.1 kHz (two), the TS tone block and the wah's
    // tank (two, pots moving them), in double: the poles are e^(p T) of the analog poles (the inverse bilinear
    // transform of the trapezoidal form's), and |H| at DC, at 0.83 of Nyquist and at the poles' frequency (or a
    // quarter of the rate above it) is the analog response's
    struct Case
    {
        std::string text;
        int block;
    };
    const Case cases[] = {
        {fc::test::kRc8k, 0}, {fc::test::kSallenKey2k, 0}, {fc::test::kScreamer, 2}, {fc::test::kWah, 0}};
    for (const Case& c : cases)
    {
        const fc::Plan plan = planned(c.text);
        const fc::Block& block = plan.blocks[static_cast<std::size_t>(c.block)];
        CHECK(block.design == fc::Block::Design::Matched);
        CHECK(block.matchedError <= 0.5);
        for (double fs : {44100.0, 48000.0, 96000.0})
        {
            for (const std::vector<double>& pots : fc::potGrid(plan, block))
            {
                const std::optional<fc::TrForm<double>> tr = fc::trForm(plan, c.block, fs, pots);
                REQUIRE(tr);
                const fc::Matched<double> m = fc::matchedDesign(*tr, block.matchedPolarity);
                INFO(plan.module << " at " << fs);
                // the analog poles: the eigenvalues of Ac = 2 fs (A - I)(A + I)^-1
                std::vector<Complex> poles;
                if (tr->order == 1)
                    poles.push_back(2 * fs * (tr->a11 - 1) / (tr->a11 + 1));
                else
                {
                    const double p11 = tr->a11 + 1, p12 = tr->a12, p21 = tr->a21, p22 = tr->a22 + 1;
                    const double det = p11 * p22 - p12 * p21;
                    // (A - I) adj(A + I) / det
                    const double m11 = ((tr->a11 - 1) * p22 - tr->a12 * p21) / det;
                    const double m12 = (-(tr->a11 - 1) * p12 + tr->a12 * p11) / det;
                    const double m21 = (tr->a21 * p22 - (tr->a22 - 1) * p21) / det;
                    const double m22 = (-tr->a21 * p12 + (tr->a22 - 1) * p11) / det;
                    const double trace = 2 * fs * (m11 + m22);
                    const double d = 4 * fs * fs * (m11 * m22 - m12 * m21);
                    const Complex root = std::sqrt(Complex(trace * trace / 4 - d));
                    poles.push_back(trace / 2 + root);
                    poles.push_back(trace / 2 - root);
                }
                if (tr->order == 1)
                    CHECK(std::fabs(m.a1 + std::exp(poles[0].real() / fs)) <= 1e-12);
                else
                {
                    const Complex z1 = std::exp(poles[0] / fs);
                    const Complex z2 = std::exp(poles[1] / fs);
                    CHECK(std::abs(m.a1 + (z1 + z2)) <= 1e-12);
                    CHECK(std::abs(m.a2 - z1 * z2) <= 1e-12);
                }
                // the three points
                auto analogAt = [&](double f)
                {
                    return std::abs(fc::analog(plan, c.block, Complex(0, 2 * std::numbers::pi * f), pots));
                };
                std::vector<double> points{0.0, fc::kMatchTop * fs / 2};
                if (tr->order == 2)
                {
                    const double w0 = std::sqrt(std::abs(poles[0] * poles[1]));   // rad/s
                    points.push_back(std::fmin(w0 / (2 * std::numbers::pi), fs / 4));
                }
                for (double f : points)
                {
                    const double a = analogAt(f);
                    const double h = std::abs(fc::matchedResponse(m, f, fs));
                    INFO("at " << f << " Hz: " << h << " against " << a);
                    CHECK(std::fabs(h - a) <= 1e-9 * a + 1e-13);   // a band-pass's DC: 0 to rounding
                }
            }
        }
    }
    // a matched block's polarity is the analog one's: an inverting RC stage, -10 at DC, its pole at 8 kHz
    const fc::Plan inverting =
        planned(".input u in\n.output y o\nR1 in n 1k\nX1 0 n o opamp\nR2 o n 10k\nC1 o n 1.989n\n");
    REQUIRE(inverting.blocks[0].design == fc::Block::Design::Matched);
    CHECK(inverting.blocks[0].matchedPolarity == -1);
    const fc::Matched<double> m =
        fc::matchedDesign(*fc::trForm(inverting, 0, 48000, {}), inverting.blocks[0].matchedPolarity);
    CHECK_THAT(fc::matchedResponse(m, 1e-6, 48000).real(), WithinRel(-10.0, 1e-6));
}

TEST_CASE("a linear block a matched design cannot serve joins the region, with a note saying why", "[circuit]")
{
    // the tone stack: three states; a linear-only netlist's region is its failing blocks, at the smallest of 2, 4 and
    // 8 at which the trapezoidal rule passes the 1x check (nothing left open: not provisional)
    const fc::Result r = fc::generate(fc::test::kToneStack, "t.cir");
    INFO(r.errors << r.warnings);
    REQUIRE(r.errors.empty());
    CHECK(r.warnings.empty());
    CHECK(!r.provisional);
    CHECK(r.selfTestKey.empty());
    CHECK(r.notes.find("t.cir: note: block 1 (C1, R1, C2, C3, Ptreb, Pbass, Pmid, RL): in the oversampled region: its "
                       "trapezoidal response errs by 13.37 dB at 44.1 kHz, and a matched design cannot serve (3 "
                       "states: a matched design is of order 1 or 2)\n") != std::string::npos);
    const fc::Plan plan = planned(fc::test::kToneStack);
    CHECK(plan.regionFirst == 0);
    CHECK(plan.regionLast == 0);
    CHECK(plan.blocks[0].oneXRegion <= 0.5);
    CHECK(plan.config.oversample == 4);
    CHECK(plan.config.oversampleFrom == "the 1x check");
    // an all-pass, a low-pass less a high-pass (1 - s R C) / (1 + s R C), nearly (R3 couples them): its zero is not
    // minimum-phase, so no magnitude match serves (its magnitude passes the 1x check anyway; the check is asked)
    const std::string allpass = ".input u in\n.output y o\nR1 in x 1k\nC1 x 0 10n\nC2 in z 10n\nR2 z 0 1k\n"
                                "R3 x z 1meg\nE1 o 0 x z 1\nRL o 0 10k\n";
    const fc::MatchedCheck ap = fc::matchedCheck(planned(allpass), 0);
    CHECK(!ap.usable);
    CHECK(ap.why == "its zeros are not minimum-phase (a magnitude match would change its phase)");
    // a linear netlist whose blocks pass at 1x runs there: no region, and .oversample's factor is unused (a note)
    const fc::Result low = fc::generate(fc::test::kRcLowpass + ".oversample 4\n", "t.cir");
    CHECK(low.warnings.empty());
    CHECK(low.notes.find("t.cir: note: the netlist is linear and its blocks run at 1x: .oversample 4 is unused\n") !=
          std::string::npos);
    CHECK(low.source.find("float step(") == std::string::npos);

    // a linear netlist ignores .method bdf2 (a warning), and the configuration line says where its tr came from
    const auto lines = std::count(fc::test::kToneStack.begin(), fc::test::kToneStack.end(), '\n');
    const fc::Result bdf2 = fc::generate(fc::test::kToneStack + ".method bdf2\n", "t.cir");
    CHECK(bdf2.warnings == "t.cir:" + std::to_string(lines + 1) +
                               ":1: warning: '.method bdf2' is ignored: the netlist is linear (its blocks run the "
                               "trapezoidal rule, or matched designs)\n");
    CHECK(bdf2.configuration.find(".method tr (the netlist's bdf2 ignored: linear)") != std::string::npos);
    fc::Options bdf2Options;
    bdf2Options.method = fc::Method::Bdf2;
    CHECK(fc::generate(fc::test::kToneStack, "t.cir", bdf2Options)
              .configuration.find(".method tr (Options' bdf2 ignored: linear)") != std::string::npos);

    // blocks=nonlinear leaves a failing linear block at 1x on the trapezoidal rule; its warning says what blocks=auto
    // would do instead: a matched design at 1x where one passes, the oversampled region where none can
    fc::Options nonlinear;
    nonlinear.region = fc::Region::Nonlinear;
    CHECK(fc::generate(fc::test::kRc8k, "t.cir", nonlinear).warnings ==
          "t.cir:4:1: warning: block 1 (R1, C1) errs by 12.79 dB at 1x (44.1 kHz), more than 0.5 dB: blocks=auto would "
          "run a matched design at 1x (0.32 dB)\n");
    CHECK(fc::generate(fc::test::kToneStack, "t.cir", nonlinear).warnings ==
          "t.cir:7:1: warning: block 1 (C1, R1, C2, C3, Ptreb, Pbass, Pmid, RL) errs by 13.37 dB at 1x (44.1 kHz), more "
          "than 0.5 dB: blocks=auto would oversample it\n");
}

TEST_CASE("the DC operating point, by hand", "[circuit]")
{
    // a 9 V divider into a follower: every node to 1e-12 V; V1 supplies 450 uA (SPICE's i(V1), into n+: -450 uA)
    const fc::Plan divider = planned(fc::test::kDivider);
    CHECK(std::fabs(nodeDc(divider, "vcc") - 9) <= 1e-12);
    CHECK(std::fabs(nodeDc(divider, "b") - 4.5) <= 1e-12);
    CHECK(std::fabs(nodeDc(divider, "out") - 4.5) <= 1e-12);
    CHECK(nodeDc(divider, "in") == 0);
    CHECK(std::fabs(divider.outputDc - 4.5) <= 1e-12);
    const fc::DcPoint dp = dcOf(divider);
    REQUIRE(dp.sources.size() == 1);
    CHECK(dp.sources[0].first == "V1");
    CHECK(std::fabs(dp.sources[0].second + 450e-6) <= 1e-12 * 450e-6);
    // tick adds the output's DC, and says so
    const fc::Result r = fc::generate(fc::test::kDivider, "t.cir");
    REQUIRE(r.errors.empty());
    CHECK(r.source.find("return (gain_ < 1 ? z0 * gain_ : z0) + 4.5f;") != std::string::npos);
    CHECK(r.notes.find("t.cir: note: the output's DC operating point, V(out) = 4.5 V, is tick's constant") !=
          std::string::npos);
    // an inductor's DC current is V / R, to 1e-12: at DC it shorts
    const fc::Plan l = planned(".input u in\n.output y b\nC0 in b 1u\nV1 a 0 9\nL1 a b 1m\nR1 b 0 1k\n");
    const fc::DcPoint lp = dcOf(l);
    const fc::System ls = fc::assemble(l, 0, true);
    const auto at = static_cast<std::size_t>(
        std::find(ls.unknownNames.begin(), ls.unknownNames.end(), "i(L1)") - ls.unknownNames.begin());
    REQUIRE(at < ls.unknownNames.size());
    CHECK(std::fabs(lp.unknowns[0][at] - 9e-3) <= 1e-12 * 9e-3);
    CHECK(std::fabs(nodeDc(l, "b") - 9) <= 1e-12);
    // an I source, and a floating V source: 1 mA into b (R1, 1k), and c 2 V above b, R2 (1 MEG) drawing V(c) / 1 MEG
    // through V1 from b: V(b) / 1k + (V(b) + 2) / 1 MEG = 1 mA
    const fc::Plan iv = planned(".input u in\n.output y c\nC0 in b 1u\nI1 0 b 1m\nR1 b 0 1k\nV1 c b 2\nR2 c 0 1meg\n");
    CHECK(std::fabs(nodeDc(iv, "b") - (1e-3 - 2e-6) / 1.001e-3) <= 1e-12);
    CHECK(std::fabs(nodeDc(iv, "c") - nodeDc(iv, "b") - 2) <= 1e-12);
    // branches in parallel at DC share a current the equations leave free, and the point is still defined: an
    // inductor across a diode pair (the port, solved open, sees 0 V; L1 carries R2's 900 uA), and a rheostat's 0-ohm
    // end across an inductor (the pot's section carries none, as at every other position)
    const std::string tank = ".input u in\n.output y a\nR1 in a 1k\nV1 vb 0 9\nR2 vb a 10k\nL1 a 0 100m\n";
    const std::string pinned = "\n.oversample 4\n.adaa off\n.solver omega(1)\n";
    const fc::Plan shunted = planned(tank + "C1 a 0 10n\nD1 a 0 1N914\nD2 0 a 1N914" + pinned);
    const fc::DcPoint sp = dcOf(shunted);
    CHECK(nodeDc(shunted, "a") == 0);
    REQUIRE(sp.ports.size() == 1);
    CHECK(sp.ports[0].second == 0);
    const fc::System ss = fc::assemble(shunted, 0, true);
    auto unknown = [](const fc::System& s, const std::string& name)
    {
        const auto at = static_cast<std::size_t>(std::find(s.unknownNames.begin(), s.unknownNames.end(), name) -
                                                 s.unknownNames.begin());
        REQUIRE(at < s.unknownNames.size());
        return at;
    };
    CHECK(std::fabs(sp.unknowns[0][unknown(ss, "i(L1)")] - 9e-4) <= 1e-12 * 9e-4);
    CHECK(sp.unknowns[0][unknown(ss, "j(D1|D2)")] == 0);
    const fc::Result shuntedModule = fc::generate(tank + "C1 a 0 10n\nD1 a 0 1N914\nD2 0 a 1N914" + pinned, "t.cir");
    CHECK(shuntedModule.errors.empty());
    CHECK(!shuntedModule.source.empty());
    for (const std::string& rheostat :
         {std::string(".pot x\n") + tank + "P1 a 0 10k {x}\n",
          std::string(".pot x 0\n.input u in\n.output y a\nR1 in a 1k\nP1 a 0 10k {x}\nV1 vb 0 9\nR2 vb a 10k\n"
                      "L1 a 0 100m\n")})
    {
        INFO(rheostat);
        const fc::Result module = fc::generate(rheostat, "t.cir");
        CHECK(module.errors.empty());
        const fc::Plan rp = planned(rheostat);
        const fc::DcPoint pp = dcOf(rp);
        const fc::System ps = fc::assemble(rp, 0, true);
        CHECK(std::fabs(pp.unknowns[0][unknown(ps, "i(L1)")] - 9e-4) <= 1e-12 * 9e-4);
        CHECK(std::fabs(pp.unknowns[0][unknown(ps, "i(P1)")]) <= 1e-18);
    }
    // no DC source: every value 0, the module as Phase 0's
    const fc::Plan none = planned(fc::test::kDiodeTone);
    CHECK(std::all_of(none.dc.begin(), none.dc.end(), [](double v) { return v == 0; }));
    CHECK(none.outputDc == 0);
}

TEST_CASE("the offset form: a biased netlist's module is the unbiased one's", "[circuit]")
{
    // screamer.cir with its 4.5 V reference drawn generates screamer.cir's module but for the comments
    const fc::Result plain = fc::generate(fc::test::kScreamer, "screamer.cir");
    const fc::Result biased = fc::generate(fc::test::screamerBiased(), "screamer.cir");
    REQUIRE(biased.errors.empty());
    CHECK(biased.warnings.empty());
    CHECK(biased.source == plain.source);
    // its DC point: 4.5 V at the op amps' inputs and outputs, 0 V across the port and at the output (after C7)
    const fc::Plan p = planned(fc::test::screamerBiased());
    for (const char* node : {"p1", "n1", "v2", "t1", "tn", "v3"})
    {
        INFO(node);
        CHECK(std::fabs(nodeDc(p, node) - 4.5) <= 1e-9);
    }
    CHECK(std::fabs(nodeDc(p, "out")) <= 1e-9);
    const fc::DcPoint point = dcOf(p);
    REQUIRE(point.ports.size() == 1);
    CHECK(point.ports[0].first == "D1|D2");
    CHECK(point.ports[0].second == 0);
    // .op: the point as notes
    const fc::Result op = fc::generate(".op\n" + fc::test::screamerBiased(), "t.cir");
    CHECK(op.notes.find("t.cir: note: .op: V(n1) = 4.5 V\n") != std::string::npos);
    CHECK(op.notes.find("t.cir: note: .op: i(V1) = 0 A\n") != std::string::npos);
    CHECK(op.notes.find("t.cir: note: .op: port D1|D2: v = 0 V\n") != std::string::npos);
}

TEST_CASE("the DC operating point's errors", "[circuit]")
{
    const std::string head = ".input u in\n.output y a\n";
    struct Case
    {
        std::string netlist;
        std::string errors;
    };
    const Case cases[] = {
        // a diode biased through a resistor from a supply
        {head + "R1 in a 10k\nV1 vb 0 9\nR2 vb a 10k\nD1 a 0 1N914\n",
         "t.cir:6:1: error: port D1 is biased at DC (0.573 V): biased ports need the offset formulation (Phase 2)\n"},
        // a DC current that only the diode can take: biased, by that current
        {head + "C1 in a 1u\nI1 0 a 1m\nD1 a 0 1N914\n",
         "t.cir:5:1: error: port D1 is biased at DC (0.001 A through it): biased ports need the offset formulation "
         "(Phase 2)\n"},
        // an integrator with a DC source at its input: its output has no DC point
        {head + "R1 in a 10k\nC1 a o 1n\nX1 0 a o opamp\nV1 b 0 1\nR3 b a 1k\n",
         "t.cir:3:1: error: block 1 (R1, C1, X1, R3): its DC operating point is undefined: its equations are singular "
         "at DC (an op amp with only capacitors around it, or a node only capacitors reach)\n"},
        // a pot that moves the DC point: the offsets are from one
        {head + ".pot x\nC1 in a 1u\nV1 vb 0 9\nP1 vb a 0 10k {x}\n",
         "t.cir:3:6: error: pot 'x' moves the DC operating point (V(a) is 4.5 at x 0.5, 0 at 0): the module runs in "
         "offsets from one DC point (one a pot moves is Phase 2's)\n"},
        // the topology's: loops of voltage sources and inductors, cut-sets of capacitors and current sources, a
        // source across the signal's node, a node driven twice
        {head + "R1 in a 10k\nR2 a 0 1k\nV1 b 0 1\nL1 b 0 1m\nR3 b a 1k\n",
         "t.cir:6:1: error: a loop of voltage sources, E outputs and inductors (V1, L1): its DC current is "
         "undefined\n"},
        {head + "R1 in a 10k\nR2 a 0 1k\nI1 0 x 1m\nC1 x 0 1n\n",
         "t.cir:5:1: error: node 'x' reaches ground only through capacitors and current sources (I1, C1): its DC "
         "voltage is undefined (add 10meg to ground)\n"},
        {head + "R1 in a 10k\nR2 a 0 1k\nV1 in 0 1\n",
         "t.cir:5:1: error: the V source V1 is across .input's node 'in': the signal is .input's\n"},
        // .input's node and an op amp's output are sources to ground: an inductor from one to another source
        {head + "R1 in a 10k\nR2 a 0 1k\nV1 vb 0 4.5\nL1 in vb 1m\n",
         "t.cir:6:1: error: a loop of voltage sources, E outputs and inductors (.input, V1, L1): its DC current is "
         "undefined\n"},
        {head + "R1 in b 1k\nR2 b 0 1k\nX1 b a a opamp\nL1 a 0 1m\n",
         "t.cir:6:1: error: a loop of voltage sources, E outputs and inductors (X1, L1): its DC current is "
         "undefined\n"},
        // without .input, only that
        {".output y a\nR1 b a 1k\nR4 b 0 1k\nV1 vb 0 4.5\nR2 vb a 1k\nR3 a 0 1k\n",
         "t.cir: error: the netlist has no .input\n"},
        {head + "R1 in a 10k\nX1 0 a b opamp\nR2 b a 1k\nV1 b 0 1\n",
         "t.cir:6:1: error: node 'b' is driven by op amp X1 and by V1\n"},
        {head + "R1 in a 10k\nR2 a 0 1k\nV1 b 0 DC 0 AC 1\nR3 b a 1k\n",
         "t.cir:5:13: error: 'AC 1': a V source is DC: the signal is .input's\n"},
        {head + "R1 in a 10k\nR2 a 0 1k\nI1 b 0 SIN(0 1m 1k)\nR3 b a 1k\n",
         "t.cir:5:8: error: 'SIN(0 1m 1k)': an I source is DC: the signal is .input's\n"},
        // E's controls in two blocks
        {head + "R1 in a 10k\nR2 a 0 1k\nX1 a n o1 opamp\nR3 o1 n 1k\nR4 n 0 1k\nR5 o1 b 1k\nR6 b 0 1k\n"
                "E1 c 0 a b 2\nR7 c 0 1k\n",
         "t.cir:10:1: error: E1's controls, 'a' and 'b', are in two blocks: an E reads one block's nodes (or its "
         "input)\n"},
        // values float coefficients cannot hold
        {head + "R1 in a 1k\nL1 a 0 2e30\n",
         "t.cir:4:8: error: L1's value 2e30 is out of range for float coefficients (1e-30 to 1e30)\n"},
        {head + "R1 in a 1k\nR2 a 0 1k\nV1 b 0 1e31\nR3 b a 1k\n",
         "t.cir:5:8: error: V1's value 1e31 is out of range for float coefficients (at most 1e30 in size)\n"},
        {head + "R1 in a 1k\nR2 a 0 1k\nE1 b 0 a 0 0\nR3 b 0 1k\n",
         "t.cir:5:12: error: E1's gain 0 is out of range for float coefficients (1e-30 to 1e30 in size; not 0)\n"},
    };
    for (const Case& c : cases)
    {
        INFO(c.netlist);
        CHECK(errorsOf(c.netlist) == c.errors);
    }
}

TEST_CASE("a series string is one diode of n N, and an asymmetric pair is two laws", "[circuit]")
{
    // D1 + D2 through m (which nothing else touches) against one diode of N = 2 x 1.752: the same block, to 1e-12
    const fc::Plan string = planned(".input u in\n.output y a\nR1 in a 2.2k\nC1 a 0 10n\nD1 a m DM\nD2 m 0 DM\n"
                                    ".model DM D(IS=2.52n N=1.752)\n");
    const fc::Plan one = planned(".input u in\n.output y a\nR1 in a 2.2k\nC1 a 0 10n\nD1 a 0 DN\n"
                                 ".model DN D(IS=2.52n N=3.504)\n");
    REQUIRE(string.blocks.size() == 1);
    REQUIRE(string.blocks[0].port);
    CHECK(string.blocks[0].port->parts == "D1+D2");
    CHECK(string.blocks[0].port->law == fc::PortDef::Law::Single);
    CHECK(string.blocks[0].port->n == 2 * 1.752);
    for (std::size_t n = 0; n < string.nodes.size(); ++n)
        CHECK(string.nodes[n].absorbed == (string.nodes[n].name == "m"));
    for (double fs : {48000.0, 192000.0})
    {
        const fc::Holters a = fc::holters(string, 0, fs, {});
        const fc::Holters b = fc::holters(one, 0, fs, {});
        CHECK(a.law == b.law);
        CHECK_THAT(a.k, WithinRel(b.k, 1e-12));
        CHECK_THAT(a.nvt, WithinRel(b.nvt, 1e-12));
        CHECK_THAT(a.cp[0], WithinRel(b.cp[0], 1e-12));
        CHECK_THAT(a.dp, WithinRel(b.dp, 1e-12));
        CHECK_THAT(a.a[0][0], WithinRel(b.a[0][0], 1e-12));
        for (double p : {-3.0, -0.1, 0.05, 1.0, 4.5})
            CHECK_THAT(fc::lawConvergedOf(fc::lawShape(a, 1), p, 0, nullptr, nullptr),
                       WithinRel(fc::lawConvergedOf(fc::lawShape(b, 1), p, 0, nullptr, nullptr), 1e-12));
    }
    // two strings each way of one law are a pair; of two, an asymmetric pair
    const fc::Plan pair = planned(".input u in\n.output y a\nR1 in a 2.2k\nC1 a 0 10n\nD1 a m 1N914\nD2 m 0 1N914\n"
                                  "D3 0 k 1N914\nD4 k a 1N914\n");
    CHECK(pair.blocks[0].port->parts == "D1+D2|D3+D4");
    CHECK(pair.blocks[0].port->law == fc::PortDef::Law::Pair);
    CHECK(pair.blocks[0].port->n == 2 * 1.752);
    // asym.cir: one diode against two in series. K = 1 / (1 / R1 + gc) at fs' (R1 || C1's companion); its law is
    // Is K (e^(v / N Vt) - 1) - Is K (e^(-v / 2 N Vt) - 1)
    const fc::Plan asym = planned(fc::test::kAsym);
    REQUIRE(asym.blocks[0].port);
    const fc::PortDef& port = *asym.blocks[0].port;
    CHECK(port.parts == "D1|D2+D3");
    CHECK(port.law == fc::PortDef::Law::Asym);
    CHECK(port.is == 2.52e-9);
    CHECK(port.is2 == 2.52e-9);
    CHECK(port.nvt == fc::thermalVoltage(1.752));
    CHECK(port.nvt2 == fc::thermalVoltage(2 * 1.752));
    const double fs = 4 * 48000 * 2.0 / 3.0;
    const fc::Holters h = fc::holters(asym, 0, fs, {});
    const double k = 1 / (1 / 2200.0 + 2 * 10e-9 * fs);
    CHECK_THAT(h.k, WithinRel(2.52e-9 * k, 1e-12));
    CHECK_THAT(h.k2, WithinRel(2.52e-9 * k, 1e-12));
    CHECK_THAT(h.dp, WithinRel(k / 2200.0, 1e-12));
    CHECK_THAT(h.cp[0], WithinRel(k, 1e-12));
    const fc::LawShape shape = fc::lawShape(h, 1);
    for (double v : {-0.9, -0.2, 0.0, 0.3, 0.6})
    {
        const double law = 2.52e-9 * k * (std::exp(v / port.nvt) - 1) - 2.52e-9 * k * (std::exp(-v / port.nvt2) - 1);
        CHECK(std::fabs(fc::lawValue(shape, v) - law) <= 1e-15 + 1e-12 * std::fabs(law));
    }
}

TEST_CASE("RS is a resistor to an internal node, and the built-in models need no card", "[circuit]")
{
    // a diode with RS gives the module the explicit resistor it stands for gives: RS_D1 from its anode to rs_D1
    const std::string head = ".title Rs\n.input u in\n.output y a\n.oversample 4\n.adaa off\n.solver omega(2)\n"
                             "R1 in a 2.2k\nC1 a 0 10n\n";
    const fc::Result rs = fc::generate(head + "D1 a 0 DM\n.model DM D(IS=2.52n N=1.752 RS=10)\n", "t.cir");
    const fc::Result explicitR =
        fc::generate(head + "D1 rs_D1 0 DM\n.model DM D(IS=2.52n N=1.752)\nRS_D1 a rs_D1 10\n", "t.cir");
    REQUIRE(rs.errors.empty());
    CHECK(rs.warnings.empty());
    CHECK(!rs.source.empty());
    CHECK(rs.source == explicitR.source);
    // a pair's equal RS: one resistor, and a note of the approximation
    const fc::Result pair =
        fc::generate(head + "D1 a 0 DM\nD2 0 a DM\n.model DM D(IS=2.52n N=1.752 RS=10)\n", "t.cir");
    REQUIRE(pair.errors.empty());
    CHECK(pair.notes.find("t.cir: note: D1|D2: RS 10 ohm is one series resistor, RS_D1, for the pair (each diode's RS "
                          "where it conducts: the usual approximation)") != std::string::npos);
    // a string's RS adds: 2 x 5 ohm
    const fc::Plan string = planned(head + "D1 a m DM\nD2 m 0 DM\n.model DM D(IS=2.52n N=1.752 RS=5)\n");
    CHECK(string.netlist.elements.back().name == "RS_D1");
    CHECK(string.netlist.elements.back().value == 10);
    // D1 a 0 1N4148 with no .model card generates (and 1N914), and a card of a built-in's name replaces it
    const fc::Result builtin = fc::generate(fc::test::kHalfWave, "halfwave.cir");
    INFO(builtin.errors);
    CHECK(builtin.errors.empty());
    CHECK(builtin.source.find("D1: Is (e^(v / N Vt) - 1), Is 2.52 nA, N Vt 45.315 mV at 27 C") != std::string::npos);
    const fc::Plan replaced = planned(".input u in\n.output y a\nR1 in a 2.2k\nD1 a 0 1N4148\n"
                                      ".model 1N4148 D(IS=4.352n N=1.906)\n");
    CHECK_THAT(replaced.blocks[0].port->is, WithinRel(4.352e-9, 1e-15));
}

TEST_CASE("each diode law's F is its antiderivative, and its bracket holds, in double", "[circuit]")
{
    // F(p) = p v - H(v), H = v^2 / 2 + G(v), v solving v + g(v) = p: dF/dp = v (report §5.1), at 200 points over
    // +-20 V, for one diode and an asymmetric pair (as for the pair and tanh above)
    const double s = fc::thermalVoltage(1.752);
    const double k = 2.52e-9 * 2200;
    for (const fc::LawShape& law : {fc::LawShape{fc::PortDef::Law::Single, k, s, 0, 1},
                                    fc::LawShape{fc::PortDef::Law::Asym, k, s, 0.5 * k, 2 * s},
                                    fc::LawShape{fc::PortDef::Law::Pair, 2 * k, s, 0, 1}})
    {
        auto v = [&](double p)
        {
            return fc::lawConvergedOf(law, p, 0, nullptr, nullptr);
        };
        auto f = [&](double p)
        {
            const double x = v(p);
            return p * x - (0.5 * x * x + fc::lawAntiderivative(law, x));
        };
        double worst = 0;
        for (int i = 0; i < 200; ++i)
        {
            const double p = -20 + 40 * (i + 0.5) / 200;
            const double h = 1e-5;
            worst = std::max(worst, std::fabs((f(p + h) - f(p - h)) / (2 * h) - v(p)));
        }
        INFO(static_cast<int>(law.law));
        CHECK(worst <= 1e-7);
        // converged, and the derivative is the law's
        for (double p : {-4.5, -0.3, 1e-4, 0.7, 6.0})
        {
            const double x = v(p);
            CHECK(std::fabs(x + fc::lawValue(law, x) - p) <= 1e-12 * std::fmax(1, std::fabs(p)));
            const double d = (fc::lawValue(law, x + 1e-7) - fc::lawValue(law, x - 1e-7)) / 2e-7;
            CHECK(std::fabs(d - fc::lawDerivative(law, x)) <= 1e-4 * std::fmax(1, fc::lawDerivative(law, x)));
        }
        // the bracket: every iterate in [min(0, p), max(0, p)] and finite, from any start, 0 to 3 Newton steps
        for (int steps = 0; steps <= 3; ++steps)
        {
            for (double mag = 1e-6; mag <= 1e6; mag *= 10)
            {
                for (double p : {mag, -mag})
                {
                    for (fc::Solver::Guess guess : {fc::Solver::Guess::Omega, fc::Solver::Guess::Previous})
                    {
                        for (double previous : {0.0, p, -p, 1e30})
                        {
                            const double x = fc::lawStepsOf(law, p, guess, previous, steps);
                            CHECK(std::isfinite(x));
                            CHECK(x >= std::fmin(0.0, p));
                            CHECK(x <= std::fmax(0.0, p));
                        }
                    }
                }
            }
        }
    }
}

TEST_CASE("the diode forms' and the new elements' emitted names are refused as names", "[circuit]")
{
    // an asymmetric pair's step locals, and its ADAA's second exponential
    for (const char* name : {"sa", "si", "sl", "sn", "ef", "er", "cr"})
    {
        INFO(name);
        CHECK(fc::isReservedName(name));
        CHECK(fc::isModuleWord(name));
        const std::string head = ".input u in\n.output y o\n";
        CHECK(errorsOf(head + ".pot " + name + "\nR1 in o {1k + " + name + "}\nR2 o 0 1k\n") ==
              "t.cir:3:6: error: pot '" + std::string(name) +
                  "' is an FMinus keyword or builtin, or a name the module uses: rename it\n");
        CHECK(errorsOf(".title " + std::string(name) + "\n" + head + "R1 in o 1k\nR2 o 0 1k\n") ==
              "t.cir:1:8: error: the module's name '" + std::string(name) +
                  "', from .title, is a name the module uses: rename it\n");
    }
    // every name the new elements' fields and locals use has a '_', which no pot or module name has
    for (const std::string& text : {fc::test::kWah, fc::test::kAsym, fc::test::kDivider})
    {
        const fc::Result r = fc::generate(text, "t.cir");
        for (const char* field : {"x_L1", "rl_L1", "r2_L1", "il_L1", "kr_D1", "ks_D1", "lnr_D1", "kcr_D1", "cr_D1"})
            CHECK(!fc::isPlainName(field));
        CHECK(r.errors.empty());
    }
}
