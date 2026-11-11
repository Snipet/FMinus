// src/circuit/Laws.cpp: each law's text and its double twin (Laws.h). The pair's text is Phase 0's (mkbench.py's
// shapes, whose costs the report measured: the omega3 guess, Newton steps in a bracket, the ADAA tail); the OTA's is
// the same frame with tanh's law: the previous step's v, Newton steps whose derivative is at least 1 (so they never
// divide by a small number), and ADAA with log cosh in the form that does not overflow.
#include "circuit/Laws.h"

#include "circuit/Reference.h"

#include <algorithm>
#include <cmath>
#include <format>
#include <numbers>

namespace fminus::circuit
{
    namespace
    {
        // omega3's breakpoints and cubic (D'Angelo, Gabrielli, Turchet, DAFx-19), as adaa.cpp has them; ADAA's |dp|
        // below which the mean is the midpoint's (volts).
        constexpr double kOmegaLow = -3.341459552768620;
        constexpr double kOmegaA = -1.314293149877800e-3;
        constexpr double kOmegaB = 4.775931364975583e-2;
        constexpr double kOmegaC = 3.631952663804445e-1;
        constexpr double kOmegaD = 6.313183464296682e-1;
        constexpr double kAdaaEpsilon = 1e-5;
        // ADAA's series for the exponentials at the solved v, from the last Newton step's at its point v0: used while
        // |dl| = |v - v0| / N Vt is below this, the exact expf beyond it.
        constexpr double kSeriesBound = 0.25;

        // Whether ADAA's tail takes its exponentials from the last Newton (or Halley) step's (an exp-affine law with a
        // step).
        bool seriesTail(const PortDef& port, const LawOperands& o)
        {
            return o.adaa && !port.smooth() && o.solver.steps >= 1;
        }

        // Newton (or Halley) step k of the solver's: its lines in braces, or for the last step of a series tail without
        // them, after v0 = v, so that ADAA's tail reads its point and its exponentials.
        void newtonStep(std::vector<LawLine>& out, const PortDef& port, const LawOperands& o, int k,
                        const std::vector<LawLine>& body, const char* kind = "Newton")
        {
            const std::string what = std::format("{} step {} of {}, kept in the bracket", kind, k, o.solver.steps);
            if (k == o.solver.steps && seriesTail(port, o))
            {
                out.push_back({"const " + o.type + " v0 = v;", what + "; ADAA's series reads its v0 and exponentials"});
                for (const LawLine& l : body)
                    out.push_back(l);
                return;
            }
            out.push_back({"{", what});
            for (const LawLine& l : body)
                out.push_back({"    " + l.text, l.note});
            out.push_back({"}", ""});
        }

        // e^dl to dl^4, as cosh(dl) + sinh(dl): its even part, its odd part
        std::string seriesEven(const std::string& dl2)
        {
            return "1 + " + dl2 + " * (0.5f + " + literal(1.0 / 24) + " * " + dl2 + ")";
        }

        std::string seriesOdd(const std::string& dl, const std::string& dl2)
        {
            return dl + " * (1 + " + literal(1.0 / 6) + " * " + dl2 + ")";
        }

        std::vector<LawLine> pairSolve(const PortDef& port, const LawOperands& o)
        {
            const std::string& T = o.type;
            const std::string ivt = literal(1 / port.nvt);
            const std::string minusIvt = literal(-1 / port.nvt);
            const std::string nvt = literal(port.nvt);
            std::vector<LawLine> out;
            if (o.adaa)
                out = adaaHeadText(port, o);
            if (o.solver.guess == Solver::Guess::Omega)
            {
                out.push_back(
                    {"const " + T + " q = fabsf(p);", "omega3 (D'Angelo et al. 2019): the conducting diode alone"});
                out.push_back({"const " + T + " arg = (q + " + o.ka + ") * " + ivt + o.plusLna + ";", ""});
                out.push_back({"const " + T + " w = arg <= " + literal(kOmegaLow) + " ? 0", ""});
                out.push_back({"              : arg < 8 ? ((" + literal(kOmegaA) + " * arg + " + literal(kOmegaB) +
                                   ") * arg + " + literal(kOmegaC) + ") * arg + " + literal(kOmegaD),
                               ""});
                out.push_back({"                        : arg - logf(fmaxf(arg, 8));", ""});
                out.push_back({T + " v = q + " + o.ka + " - " + nvt + " * w;", ""});
                out.push_back({"v = p < 0 ? -v : v;", ""});
                out.push_back({"const " + T + " lo = fminf(p, 0);", ""});
                out.push_back({"const " + T + " hi = fmaxf(p, 0);", ""});
                if (o.solver.steps == 0)
                    out.push_back({"v = fminf(fmaxf(v, lo), hi);", "no Newton step: the guess, in the bracket"});
            }
            else
            {
                out.push_back({"const " + T + " lo = fminf(p, 0);", ""});
                out.push_back({"const " + T + " hi = fmaxf(p, 0);", ""});
                out.push_back({T + " v = fminf(fmaxf(v_" + o.port + ", lo), hi);",
                               "the previous step's solution, in the bracket"});
            }
            if (o.solver.shape == Solver::Shape::Halley)
            {
                // Halley's steps (takesHalley's comment below), from the guess in the bracket, where their denominator
                // is bounded: the same exponentials as a Newton step's, and f'' = kb (e - ei) / N Vt from them
                if (o.solver.guess == Solver::Guess::Omega)
                    out.push_back({"v = fminf(fmaxf(v, lo), hi);", "the guess, in the bracket: Halley's start"});
                for (int k = 1; k <= o.solver.steps; ++k)
                    newtonStep(out, port, o, k,
                               {{"const " + T + " e = expf(v * " + ivt + ");", ""},
                                {"const " + T + " ei = expf(v * " + minusIvt + ");",
                                 k == 1 ? "its own kernel beside e's (1 / e would wait on e)" : ""},
                                {"const " + T + " sh = e - ei;", ""},
                                {"const " + T + " f = v - p + " + o.ka + " * sh;", ""},
                                {"const " + T + " d = 1 + " + o.kb + " * (e + ei);", "f'"},
                                {"const " + T + " f2 = (" + o.kb + " * " + ivt + ") * sh;", "f''"},
                                {"v = fminf(fmaxf(v - (2 * f * d) / (2 * d * d - f * f2), lo), hi);",
                                 k == 1 ? "one divide, its denominator above d * d" : ""}},
                               "Halley");
                return out;
            }
            for (int k = 1; k <= o.solver.steps; ++k)
                newtonStep(out, port, o, k,
                           {{"const " + T + " e = expf(v * " + ivt + ");", ""},
                            {"const " + T + " ei = expf(v * " + minusIvt + ");",
                             k == 1 ? "its own kernel beside e's (1 / e would wait on e)" : ""},
                            {"const " + T + " f = v - p + " + o.ka + " * (e - ei);", ""},
                            {"const " + T + " d = 1 + " + o.kb + " * (e + ei);", ""},
                            {"v = fminf(fmaxf(v - f / d, lo), hi);", ""}});
            return out;
        }

        // omega3's cubic and its tails, on `arg`, as w (the pair's lines)
        void omegaLines(std::vector<LawLine>& out, const std::string& T)
        {
            out.push_back({"const " + T + " w = arg <= " + literal(kOmegaLow) + " ? 0", ""});
            out.push_back({"              : arg < 8 ? ((" + literal(kOmegaA) + " * arg + " + literal(kOmegaB) +
                               ") * arg + " + literal(kOmegaC) + ") * arg + " + literal(kOmegaD),
                           ""});
            out.push_back({"                        : arg - logf(fmaxf(arg, 8));", ""});
        }

        // One diode, v + ka (e^(v / N Vt) - 1) = p: omega3 for p > 0 (exact there: v + ka e^(v / N Vt) = p + ka),
        // p + ka for p < 0 (the diode off); Newton's derivative 1 + kb e is at least 1.
        std::vector<LawLine> singleSolve(const PortDef& port, const LawOperands& o)
        {
            const std::string& T = o.type;
            const std::string ivt = literal(1 / port.nvt);
            std::vector<LawLine> out;
            if (o.adaa)
                out = adaaHeadText(port, o);
            if (o.solver.guess == Solver::Guess::Omega)
            {
                out.push_back({"const " + T + " q = fmaxf(p, 0);",
                               "omega3 (D'Angelo et al. 2019) for p > 0: v + ka e^(v / N Vt) = p + ka"});
                out.push_back({"const " + T + " arg = (q + " + o.ka + ") * " + ivt + o.plusLna + ";", ""});
                omegaLines(out, T);
                out.push_back({T + " v = p < 0 ? p + " + o.ka + " : q + " + o.ka + " - " + literal(port.nvt) + " * w;",
                               "p < 0: the diode off, v near p + ka"});
                out.push_back({"const " + T + " lo = fminf(p, 0);", ""});
                out.push_back({"const " + T + " hi = fmaxf(p, 0);", ""});
                if (o.solver.steps == 0)
                    out.push_back({"v = fminf(fmaxf(v, lo), hi);", "no Newton step: the guess, in the bracket"});
            }
            else
            {
                out.push_back({"const " + T + " lo = fminf(p, 0);", ""});
                out.push_back({"const " + T + " hi = fmaxf(p, 0);", ""});
                out.push_back({T + " v = fminf(fmaxf(v_" + o.port + ", lo), hi);",
                               "the previous step's solution, in the bracket"});
            }
            for (int k = 1; k <= o.solver.steps; ++k)
                newtonStep(out, port, o, k,
                           {{"const " + T + " e = expf(v * " + ivt + ");", ""},
                            {"const " + T + " f = v - p + " + o.ka + " * (e - 1);", ""},
                            {"const " + T + " d = 1 + " + o.kb + " * e;", ""},
                            {"v = fminf(fmaxf(v - f / d, lo), hi);", ""}});
            return out;
        }

        // An asymmetric pair, v + ka (e^(v / N Vt) - 1) - kr (e^(-v / N2 Vt) - 1) = p: omega3 for the diode that
        // conducts (p's sign's), then Newton with both exponentials.
        std::vector<LawLine> asymSolve(const PortDef& port, const LawOperands& o)
        {
            const std::string& T = o.type;
            const std::string ivt = literal(1 / port.nvt);
            const std::string ivr = literal(1 / port.nvt2);
            const std::string minusIvr = literal(-1 / port.nvt2);
            std::vector<LawLine> out;
            if (o.adaa)
                out = adaaHeadText(port, o);
            if (o.solver.guess == Solver::Guess::Omega)
            {
                out.push_back({"const " + T + " q = fabsf(p);",
                               "omega3 (D'Angelo et al. 2019) for the diode that conducts, p's sign's"});
                out.push_back({"const " + T + " sa = p < 0 ? " + o.kr + " : " + o.ka + ";", "its Is K"});
                out.push_back({"const " + T + " si = p < 0 ? " + ivr + " : " + ivt + ";", "1 / its N Vt"});
                out.push_back({"const " + T + " sl = p < 0 ? " + o.lnr + " : " + o.lna + ";", "log(its Is K / N Vt)"});
                out.push_back({"const " + T + " sn = p < 0 ? " + literal(port.nvt2) + " : " + literal(port.nvt) + ";",
                               "its N Vt"});
                out.push_back({"const " + T + " arg = (q + sa) * si + sl;", ""});
                omegaLines(out, T);
                out.push_back({T + " v = q + sa - sn * w;", ""});
                out.push_back({"v = p < 0 ? -v : v;", ""});
                out.push_back({"const " + T + " lo = fminf(p, 0);", ""});
                out.push_back({"const " + T + " hi = fmaxf(p, 0);", ""});
                if (o.solver.steps == 0)
                    out.push_back({"v = fminf(fmaxf(v, lo), hi);", "no Newton step: the guess, in the bracket"});
            }
            else
            {
                out.push_back({"const " + T + " lo = fminf(p, 0);", ""});
                out.push_back({"const " + T + " hi = fmaxf(p, 0);", ""});
                out.push_back({T + " v = fminf(fmaxf(v_" + o.port + ", lo), hi);",
                               "the previous step's solution, in the bracket"});
            }
            for (int k = 1; k <= o.solver.steps; ++k)
                newtonStep(out, port, o, k,
                           {{"const " + T + " ef = expf(v * " + ivt + ");", ""},
                            {"const " + T + " er = expf(v * " + minusIvr + ");", ""},
                            {"const " + T + " f = v - p + " + o.ka + " * (ef - 1) - " + o.kr + " * (er - 1);", ""},
                            {"const " + T + " d = 1 + " + o.kb + " * ef + " + o.ks + " * er;", ""},
                            {"v = fminf(fmaxf(v - f / d, lo), hi);", ""}});
            return out;
        }

        std::vector<LawLine> tanhSolve(const PortDef& port, const LawOperands& o)
        {
            const std::string& T = o.type;
            const std::string ivt = literal(1 / port.nvt);
            std::vector<LawLine> out;
            out.push_back({"const float kg = " + o.kd + " * " + o.g + ";",
                           "kd g: this step's law, v + kg tanh(v / 2 Vt) = p"});
            out.push_back({"const float kgs = kg * " + ivt + ";", "its slope at v = 0, kg / 2 Vt"});
            if (o.adaa)
            {
                out.push_back({"const float kgv = kg * " + literal(port.nvt) + ";", "ADAA: kg 2 Vt"});
                for (LawLine& l : adaaHeadText(port, o))
                    out.push_back(std::move(l));
            }
            out.push_back({"const " + T + " lo = fminf(p, 0);", ""});
            out.push_back({"const " + T + " hi = fmaxf(p, 0);", ""});
            // The first Newton step starts from one of the two points whose tanh the step already has: the previous
            // step's v, whose tanh that step stored (t_P), where it is nearer p than 0 is, else 0 (tanh 0 = 0). So
            // newton(K) evaluates K tanhf, not K + 1, and the start is not clamped into the bracket, which would
            // need tanhf at the bracket's end. The rule takes the previous v wherever it is within |p| of p (the
            // usual case: the previous v is about p), and 0 where p has crossed 0 or fallen below half the previous
            // v: starting there from the previous v, on tanh's flat part far from the root,
            // Newton overshoots (an OTA driven past its linear range, v_P = -0.89 V, p = +0.097 V, kg = 0.078: 1.4e-3
            // V of residual after newton(3), the output 3e-4 V off the converged reference, against 4e-6 V from 0;
            // examples/circuits --check's gain step, 1 uA to 200 uA under newton(2): 2.2e-5 of the signal against
            // 9.6e-6). Only the step's result is clamped to the bracket, and that keeps v finite for any input: a
            // NaN or infinite v_P fails the test and starts from 0, with tanh 0, so the start is finite; |tn| <= 1
            // and kgs >= 0 make d = 1 + kgs (1 - tn^2) at least 1, so f / d is no larger than f; and a NaN from
            // anywhere else (t_P, p) reaches fmaxf, which turns it into the bracket's end. Without a Newton step the
            // guess is the clamped previous v.
            const std::string nearer = "fabsf(v_" + o.port + " - p) < fabsf(p)";
            if (o.solver.steps == 0)
                out.push_back({T + " v = fminf(fmaxf(v_" + o.port + ", lo), hi);",
                               "the previous step's solution, in the bracket"});
            else
                out.push_back({T + " v = " + nearer + " ? v_" + o.port + " : 0;",
                               "the previous step's solution where it is nearer p than 0 is, else 0"});
            for (int k = 1; k <= o.solver.steps; ++k)
            {
                out.push_back({"{", std::format("Newton step {} of {}, kept in the bracket", k, o.solver.steps)});
                if (k == 1)
                    out.push_back({"    const " + T + " tn = " + nearer + " ? t_" + o.port + " : 0;",
                                   "tanh(v / 2 Vt): the one stored, or tanh 0"});
                else
                    out.push_back({"    const " + T + " tn = tanhf(v * " + ivt + ");", ""});
                out.push_back({"    const " + T + " f = v - p + kg * tn;", ""});
                out.push_back({"    const " + T + " d = 1 + kgs * (1 - tn * tn);", "at least 1"});
                out.push_back({"    v = fminf(fmaxf(v - f / d, lo), hi);", ""});
                out.push_back({"}", ""});
            }
            if (!o.adaa)
            {
                // the current, by the law at v (not (p - v) / kd, a small difference of p and v)
                out.push_back({"const " + T + " th = tanhf(v * " + ivt + ");", ""});
                out.push_back({"const " + T + " ib = " + o.g + " * th;", "the OTA's current, g tanh(v / 2 Vt)"});
            }
            return out;
        }
    } // namespace

    // A Halley step, v - 2 f f' / (2 f'^2 - f f''), f = v - p + g(v), takes the exponentials (or the tanh) a Newton
    // step evaluates and one divide. It is a step only where its denominator cannot vanish, which each law's f'' = g''
    // decides, from a point v in the bracket [min(0, p), max(0, p)] (v and p of one sign, |v| <= |p|):
    // - a diode pair, g = ka (e - ei), e = e^(v / N Vt), ei = 1 / e: f' = d = 1 + kb (e + ei), f'' = h = kb (e - ei) /
    //   N Vt, of v's sign (kb = ka / N Vt). f h > 0 only where f has v's sign too, and there v - p, of the other sign
    //   or 0, leaves |f| <= ka |e - ei|: f h <= ka kb (e - ei)^2 / N Vt = kb^2 (e - ei)^2 < kb^2 (e + ei)^2 < d^2. So
    //   2 d^2 - f h > d^2 >= 1 (the step is at most 2 |f| / d) from any point of the bracket: a pair takes it;
    // - one diode, g = ka (e - 1): f' = 1 + kb e, f'' = kb e / N Vt > 0 everywhere. For p < 0 the bracket is [p, 0],
    //   and at v = 0, f = -p and d = 1 + kb: 2 d^2 - f h = 2 (1 + kb)^2 + p kb / N Vt, 0 at p = -2 (1 + kb)^2 N Vt / kb
    //   (some hundreds of volts at a 1N914's Is and 2.2k, fewer as Is K grows): it keeps Newton's;
    // - an asymmetric pair, g = ka (ef - 1) - kr (er - 1), er = e^(-v / N2 Vt): f' = 1 + kb ef + ks er, f'' = kb ef /
    //   N Vt - ks er / N2 Vt, whose sign is not v's. At v = 0, f = -p and f'' = kb / N Vt - ks / N2 Vt: where p's sign
    //   is the other one (p < 0 when the forward diode's kb / N Vt is the larger), 2 d^2 - f h = 2 (1 + kb + ks)^2 -
    //   |p f''| vanishes at a large enough |p|: it keeps Newton's;
    // - an OTA, g = kg t, t = tanh(v / 2 Vt): f' = 1 + kgs (1 - t^2), f'' = -2 kgs t (1 - t^2) / 2 Vt, of -v's sign.
    //   f h > 0 where 0 < v < the root (p > 0; and mirrored), and there |f| reaches p: at p = 1 V, kgs = 1, v = 34 mV
    //   (t = 0.58), f = -0.94, f'' = -14.9 and 2 d^2 - f h = 5.6 - 13.9 < 0. The start reaches it (the previous v of a
    //   quiet signal, which a step to 1 V leaves within |p| of p): it keeps Newton's.
    bool takesHalley(PortDef::Law law)
    {
        return law == PortDef::Law::Pair;
    }

    std::string stepsText(const Solver& solver, PortDef::Law law)
    {
        const bool halley = solver.shape == Solver::Shape::Halley && takesHalley(law);
        std::string text =
            std::to_string(solver.steps) + (halley ? " Halley" : " Newton") + (solver.steps == 1 ? " step" : " steps");
        if (solver.shape == Solver::Shape::Halley && !halley)
            text += std::format(" (halley({}): {}'s law keeps Newton steps)", solver.steps,
                                law == PortDef::Law::Single ? "one diode"
                                : law == PortDef::Law::Asym ? "an asymmetric pair"
                                                            : "an OTA");
        return text;
    }

    std::vector<LawLine> solveText(const PortDef& port, const LawOperands& o)
    {
        switch (port.law)
        {
        case PortDef::Law::Tanh:
            return tanhSolve(port, o);
        case PortDef::Law::Single:
            return singleSolve(port, o);
        case PortDef::Law::Asym:
            return asymSolve(port, o);
        case PortDef::Law::Pair:
            break;
        }
        return pairSolve(port, o);
    }

    std::vector<LawLine> adaaHeadText(const PortDef& port, const LawOperands& o)
    {
        const std::string& T = o.type;
        const std::string& P = o.port;
        std::vector<LawLine> out;
        if (port.smooth())
            out.push_back({"p_" + P + " = p_" + P + " + (kg - kgo_" + P + ") * t_" + P + ";",
                           "ADAA: the last point onto this step's curve (kd g moved), its residual kept"});
        out.push_back(
            {"const " + T + " dp = p - p_" + P + ";", "ADAA: the step in p, and its reciprocal, which the mean"});
        out.push_back({"const " + T + " rdp = 1 / dp;", "multiplies by: a divide beside the solve, not after it"});
        return out;
    }

    std::vector<LawLine> adaaText(const PortDef& port, const LawOperands& o)
    {
        const std::string& T = o.type;
        const std::string& P = o.port;
        const std::string ivt = literal(1 / port.nvt);
        std::vector<LawLine> out;
        if (port.smooth())
        {
            // the mean of the current i = (p - G(p)) / kd over [p(n-1), p(n)]: (p - G)'s mean is the midpoint's, wm =
            // (w + w_P) / 2, plus (kd (Phi(v) - Phi(v_P)) - wm (v - v_P)) / dp (report §5.1's closed form,
            // rearranged), with w = p - v = kg tanh(v / 2 Vt) from the law, not from p and v (their difference is
            // small). Phi's difference, kg 2 Vt (log cosh x - log cosh x_P), takes one of two forms by the step
            // |x - x_P|:
            // - below 1, log1p(z) with z = cosh x / cosh x_P - 1 = 2 sinh(h) (t_P cosh(h) + sinh(h)), h = (x - x_P) /
            //   2: from the stored v's own difference, so it agrees with v - v_P (two rounded log cosh values do not,
            //   where x - x_P is small), sinh(h) a polynomial (|h| < 0.5), cosh(h) = sqrt(1 + sinh^2), and log1p as
            //   Kahan's log(1 + z) z / ((1 + z) - 1). There z is at least e^-1 - 1 (t_P = -1, h = 0.5), so 1 + z
            //   loses nothing;
            // - from 1 on, the stored log cosh values' difference, lc - lc_P, lc = |x| - log(1 + |tanh x|) (log cosh
            //   in the form that cannot overflow): a step that large has no cancellation to fear, where z cancels to
            //   1 + z <= 0 once t_P has rounded to -+1 and the step runs back toward 0 (|x_P| > 9, |h| > 4).
            out.push_back({"const " + T + " th = tanhf(v * " + ivt + ");",
                           "ADAA: the mean of the current over [p(n-1), p(n)], closed form"});
            out.push_back({"const " + T + " lc = fabsf(v * " + ivt + ") - logf(1 + fabsf(th));",
                           "log cosh(v / 2 Vt), which cannot overflow"});
            out.push_back({"const " + T + " hh = fminf(fmaxf((v - v_" + P + ") * " + literal(0.5 / port.nvt) +
                               ", -0.5f), 0.5f);",
                           "(x - x_P) / 2, x = v / 2 Vt, held to the polynomial's range"});
            out.push_back({"const " + T + " h2 = hh * hh;", ""});
            out.push_back({"const " + T + " s2 = hh * (1 + h2 * (" + literal(1.0 / 6) + " + h2 * (" +
                               literal(1.0 / 120) + " + h2 * " + literal(1.0 / 5040) + ")));",
                           "sinh(h)"});
            out.push_back({"const " + T + " z = 2 * s2 * (t_" + P + " * sqrtf(1 + s2 * s2) + s2);",
                           "cosh x / cosh x_P - 1"});
            out.push_back({"const " + T + " zu = 1 + z;", ""});
            out.push_back({"const " + T + " dl = fabsf(hh) < 0.5f ? (zu == 1 ? z : logf(zu) * z / (zu - 1)) : lc - " +
                               "lc_" + P + ";",
                           "log cosh x - log cosh x_P: log1p(z), or from |x - x_P| = 1 on the difference"});
            out.push_back({"const " + T + " wm = 0.5f * kg * (th + t_" + P + ");",
                           "p - v at the midpoint, from the law: kd times its current"});
            out.push_back({"const " + T + " ib = (fabsf(dp) > " + literal(kAdaaEpsilon) +
                               " ? wm + (kgv * dl - wm * (v - v_" + P + ")) * rdp : wm) * " + o.rkd + ";",
                           "the mean current, (p - G(p)) / kd over [p_P, p]: the midpoint's and a correction"});
            return out;
        }
        if (port.law == PortDef::Law::Single || port.law == PortDef::Law::Asym)
        {
            // vb = vm + ((v - v_P) (pm - vm) - K (Phi(v) - Phi(v_P))) / dp, with K Phi = kc e - ka v (one diode), and
            // the other diode's kcr er + kr v (an asymmetric pair): the ka v and kr v terms join (v - v_P)'s factor
            const bool asym = port.law == PortDef::Law::Asym;
            const std::string minusIvr = asym ? literal(-1 / port.nvt2) : std::string();
            if (seriesTail(port, o))
            {
                // e^(v / N Vt) = e^(v0 / N Vt) e^dl, dl = (v - v0) / N Vt, from the last Newton step's exponential at
                // its point v0 (an asymmetric pair's other diode's likewise, its dl in -1 / N2 Vt)
                const std::string e0 = asym ? "ef" : "e";
                out.push_back({"const " + T + " dl = (v - v0) * " + ivt + ";",
                               "ADAA: the mean of G over [p(n-1), p(n)], closed form"});
                out.push_back({"const " + T + " dl2 = dl * dl;", "e^(v / N Vt) = e^(v0 / N Vt) e^dl, a series in dl"});
                out.push_back({"const " + T + " c = fabsf(dl) < " + literal(kSeriesBound), ""});
                out.push_back(
                    {"          ? " + e0 + " * (" + seriesEven("dl2") + " + " + seriesOdd("dl", "dl2") + ")", ""});
                out.push_back({"          : expf(v * " + ivt + ");", "rarely: the exact form where dl is not small"});
                if (asym)
                {
                    out.push_back({"const " + T + " dr = (v - v0) * " + minusIvr + ";", "and e^(-v / N2 Vt), from er"});
                    out.push_back({"const " + T + " dr2 = dr * dr;", ""});
                    out.push_back({"const " + T + " cr = fabsf(dr) < " + literal(kSeriesBound), ""});
                    out.push_back(
                        {"           ? er * (" + seriesEven("dr2") + " + " + seriesOdd("dr", "dr2") + ")", ""});
                    out.push_back({"           : expf(v * " + minusIvr + ");", ""});
                }
            }
            else
            {
                out.push_back({"const " + T + " c = expf(v * " + ivt + ");",
                               "ADAA: the mean of G over [p(n-1), p(n)], closed form"});
                if (asym)
                    out.push_back({"const " + T + " cr = expf(v * " + minusIvr + ");", ""});
            }
            out.push_back({"const " + T + " vm = 0.5f * (v + v_" + P + ");", ""});
            const std::string factor = "0.5f * (p + p_" + P + ") - vm + " + o.ka + (asym ? " - " + o.kr : "");
            const std::string phi =
                o.kc + " * (c - c_" + P + ")" + (asym ? " - " + o.kcr + " * (cr - cr_" + P + ")" : "");
            out.push_back({"const " + T + " vb = fabsf(dp) > " + literal(kAdaaEpsilon) + " ? vm + ((v - v_" + P +
                               ") * (" + factor + ") - " + phi + ") * rdp : vm;",
                           ""});
            return out;
        }
        if (seriesTail(port, o))
        {
            // 2 cosh(x0 + dl) = (e + ei) cosh(dl) + (e - ei) sinh(dl), x0 = v0 / N Vt and dl = (v - v0) / N Vt, from
            // the last Newton step's exponentials at its point v0 (the addition theorem; no kernel on the chain)
            out.push_back({"const " + T + " dl = (v - v0) * " + ivt + ";",
                           "ADAA: the mean of G over [p(n-1), p(n)], closed form"});
            out.push_back({"const " + T + " dl2 = dl * dl;", "2 cosh(v / N Vt) from e and ei, a series in dl"});
            out.push_back({"const " + T + " c = fabsf(dl) < " + literal(kSeriesBound), ""});
            out.push_back(
                {"          ? (e + ei) * (" + seriesEven("dl2") + ") + (e - ei) * (" + seriesOdd("dl", "dl2") + ")",
                 ""});
            out.push_back({"          : expf(v * " + ivt + ") + expf(v * " + literal(-1 / port.nvt) + ");",
                           "rarely: the exact form where dl is not small"});
        }
        else
        {
            out.push_back({"const " + T + " ec = expf(v * " + ivt + ");",
                           "ADAA: the mean of G over [p(n-1), p(n)], closed form"});
            out.push_back({"const " + T + " c = ec + expf(v * " + literal(-1 / port.nvt) + ");", ""});
        }
        const std::string phi = o.kc + " * (c - c_" + P + ")";
        out.push_back({"const " + T + " vm = 0.5f * (v + v_" + P + ");", ""});
        out.push_back({"const " + T + " vb = fabsf(dp) > " + literal(kAdaaEpsilon) + " ? vm + ((v - v_" + P +
                           ") * (0.5f * (p + p_" + P + ") - vm) - " + phi + ") * rdp : vm;",
                       ""});
        return out;
    }

    std::vector<LawLine> storeText(const PortDef& port, const LawOperands& o)
    {
        const std::string& P = o.port;
        std::vector<LawLine> out;
        if (port.smooth())
            out.push_back({"t_" + P + " = th;", ""});
        if (!o.adaa)
            return out;
        out.push_back({"p_" + P + " = p;", ""});
        if (port.smooth())
        {
            out.push_back({"lc_" + P + " = lc;", ""});
            out.push_back({"kgo_" + P + " = kg;", ""});
        }
        else
            out.push_back({"c_" + P + " = c;", ""});
        if (port.law == PortDef::Law::Asym)
            out.push_back({"cr_" + P + " = cr;", ""});
        return out;
    }

    std::vector<LawLine> carryText(const PortDef& port, const LawOperands& o)
    {
        const std::string& P = o.port;
        std::vector<LawLine> out;
        switch (port.law)
        {
        case PortDef::Law::Pair:
            out.push_back({"{", "ADAA: the last point onto the new curve"});
            out.push_back({"    const " + o.type + " e = expf(v_" + P + " * " + literal(1 / port.nvt) + ");", ""});
            out.push_back({"    p_" + P + " = p_" + P + " + (ka_" + P + " - kprev_" + P + ") * (e - expf(v_" + P +
                               " * " + literal(-1 / port.nvt) + "));",
                           ""});
            out.push_back({"}", ""});
            break;
        case PortDef::Law::Single:
            out.push_back({"p_" + P + " = p_" + P + " + (ka_" + P + " - kprev_" + P + ") * (c_" + P + " - 1);",
                           "ADAA: the last point onto the new curve (c_" + P + " is e^(v_" + P + " / N Vt))"});
            break;
        case PortDef::Law::Asym:
            out.push_back({"p_" + P + " = p_" + P + " + (ka_" + P + " - kprev_" + P + ") * (c_" + P + " - 1) - (kr_" +
                               P + " - krprev_" + P + ") * (cr_" + P + " - 1);",
                           "ADAA: the last point onto the new curve"});
            break;
        case PortDef::Law::Tanh:
            break;
        }
        return out;
    }

    std::vector<LawField> adaaFields(const PortDef& port)
    {
        const std::string& P = port.name;
        if (port.law == PortDef::Law::Pair)
            return {{"c_" + P, true, "ADAA: 2 cosh(v_" + P + " / N Vt)", "2"}};
        if (port.law == PortDef::Law::Single)
            return {{"c_" + P, true, "ADAA: e^(v_" + P + " / N Vt)", "1"}};
        if (port.law == PortDef::Law::Asym)
            return {{"c_" + P, true, "ADAA: e^(v_" + P + " / N Vt)", "1"},
                    {"cr_" + P, true, "ADAA: e^(-v_" + P + " / N2 Vt), the other diode's", "1"}};
        return {{"lc_" + P, true, "ADAA: log cosh(v_" + P + " / 2 Vt), for the mean of a long step", "0"},
                {"kgo_" + P, false, "ADAA: kd g at the last step", "0"}};
    }

    std::vector<LawField> portFields(const PortDef& port)
    {
        if (port.law == PortDef::Law::Tanh)
            return {
                {"t_" + port.name, true,
                 "tanh(v_" + port.name + " / 2 Vt): the next step's first Newton step's (and ADAA's carry and mean)",
                 "0"}};
        return {};
    }

    // ---- the double twin ------------------------------------------------------------------------------------------

    double lawScale(const Holters& h, double g)
    {
        return h.law == PortDef::Law::Tanh ? h.k * g : h.k;
    }

    double logCosh(double x)
    {
        const double a = std::fabs(x);
        return a + std::log(1 + std::exp(-2 * a)) - std::numbers::ln2;
    }

    double lawPhi(PortDef::Law law, double v, double s)
    {
        return law == PortDef::Law::Tanh ? std::tanh(v / s) : std::sinh(v / s);
    }

    double lawSlope(PortDef::Law law, double v, double s)
    {
        if (law == PortDef::Law::Tanh)
        {
            const double t = std::tanh(v / s);
            return (1 - t * t) / s;
        }
        return std::cosh(v / s) / s;
    }

    double lawIntegral(PortDef::Law law, double v, double s)
    {
        return law == PortDef::Law::Tanh ? s * logCosh(v / s) : s * std::cosh(v / s);
    }

    double lawSteps(PortDef::Law law, double p, double k, double s, Solver::Guess guess, double previous, int steps,
                    double* start, Solver::Shape shape)
    {
        const double lo = std::fmin(0.0, p);
        const double hi = std::fmax(0.0, p);
        if (law != PortDef::Law::Tanh)
        {
            const double g = guess == Solver::Guess::Omega ? omegaGuess(p, k, s) : previous;
            double v = std::fmin(std::fmax(g, lo), hi);
            for (int i = 0; i < steps; ++i)
            {
                if (start != nullptr)
                    *start = v;
                const double e = std::exp(v / s);
                const double ei = std::exp(-v / s);
                const double f = v - p + k * 0.5 * (e - ei);
                const double d = 1 + (k / s) * 0.5 * (e + ei);
                if (shape == Solver::Shape::Halley)
                {
                    // as the text: f'' = kb (e - ei) / N Vt, kb = k / 2 N Vt
                    const double h = (k / s) * 0.5 / s * (e - ei);
                    v = std::fmin(std::fmax(v - (2 * f * d) / (2 * d * d - f * h), lo), hi);
                }
                else
                    v = std::fmin(std::fmax(v - f / d, lo), hi);
            }
            return v;
        }
        // as the text: the first step from the previous v where it is nearer p than 0 is (its tanh the one the last
        // step stored), else from 0; the previous v clamped to the bracket only when there is no step
        const double ivt = 1 / s;
        const double ks = k * ivt;
        double v = steps == 0                               ? std::fmin(std::fmax(previous, lo), hi)
                   : std::fabs(previous - p) < std::fabs(p) ? previous
                                                            : 0.0;
        for (int i = 0; i < steps; ++i)
        {
            if (start != nullptr)
                *start = v;
            const double t = std::tanh(v * ivt);
            const double f = v - p + k * t;
            const double d = 1 + ks * (1 - t * t);
            v = std::fmin(std::fmax(v - f / d, lo), hi);
        }
        return v;
    }

    double lawConverged(PortDef::Law law, double p, double k, double s, double previous, long* unconverged,
                        int* iterations)
    {
        if (law != PortDef::Law::Tanh)
        {
            RefStats stats;
            const double v = converged(p, k, s, &stats);
            if (unconverged != nullptr)
                *unconverged += stats.unconverged;
            if (iterations != nullptr)
                *iterations = std::max(*iterations, stats.maxIterations);
            return v;
        }
        // h(v) = v + k tanh(v / s) - p rises (h' >= 1), so the root is in [min(0, p), max(0, p)], and each value of h
        // says which side of v it is on
        double lo = std::fmin(0.0, p);
        double hi = std::fmax(0.0, p);
        double v = std::fmin(std::fmax(previous, lo), hi);
        for (int i = 0; i < 300; ++i)
        {
            const double t = std::tanh(v / s);
            const double h = v - p + k * t;
            if (h == 0)
            {
                if (iterations != nullptr)
                    *iterations = std::max(*iterations, i + 1);
                return v;
            }
            (h > 0 ? hi : lo) = v;
            double nv = v - h / (1 + (k / s) * (1 - t * t));
            if (!(nv >= lo && nv <= hi))
                nv = 0.5 * (lo + hi);
            if (std::fabs(nv - v) < 1e-16 + 1e-15 * std::fabs(nv))
            {
                if (iterations != nullptr)
                    *iterations = std::max(*iterations, i + 1);
                return nv;
            }
            v = nv;
        }
        if (unconverged != nullptr)
            ++*unconverged;
        return v;
    }

    LawShape lawShape(const Holters& h, double g)
    {
        return {h.law, lawScale(h, g), h.nvt, h.k2, h.nvt2};
    }

    double lawValue(const LawShape& l, double v)
    {
        switch (l.law)
        {
        case PortDef::Law::Pair:
            return l.k * std::sinh(v / l.s);
        case PortDef::Law::Single:
            return l.k * (std::exp(v / l.s) - 1);
        case PortDef::Law::Asym:
            return l.k * (std::exp(v / l.s) - 1) - l.k2 * (std::exp(-v / l.s2) - 1);
        case PortDef::Law::Tanh:
            return l.k * std::tanh(v / l.s);
        }
        return 0;
    }

    double lawDerivative(const LawShape& l, double v)
    {
        switch (l.law)
        {
        case PortDef::Law::Pair:
            return l.k / l.s * std::cosh(v / l.s);
        case PortDef::Law::Single:
            return l.k / l.s * std::exp(v / l.s);
        case PortDef::Law::Asym:
            return l.k / l.s * std::exp(v / l.s) + l.k2 / l.s2 * std::exp(-v / l.s2);
        case PortDef::Law::Tanh:
        {
            const double t = std::tanh(v / l.s);
            return l.k / l.s * (1 - t * t);
        }
        }
        return 0;
    }

    double lawAntiderivative(const LawShape& l, double v)
    {
        switch (l.law)
        {
        case PortDef::Law::Pair:
            return l.k * l.s * std::cosh(v / l.s);
        case PortDef::Law::Single:
            return l.k * (l.s * std::exp(v / l.s) - v);
        case PortDef::Law::Asym:
            return l.k * (l.s * std::exp(v / l.s) - v) + l.k2 * (l.s2 * std::exp(-v / l.s2) + v);
        case PortDef::Law::Tanh:
            return l.k * l.s * logCosh(v / l.s);
        }
        return 0;
    }

    double lawGuess(const LawShape& l, double p)
    {
        // omegaGuess(p, k, s) is the pair's, a = k / 2 its Is K
        switch (l.law)
        {
        case PortDef::Law::Pair:
            return omegaGuess(p, l.k, l.s);
        case PortDef::Law::Single:
            return p < 0 ? p + l.k : omegaGuess(p, 2 * l.k, l.s);
        case PortDef::Law::Asym:
            return p < 0 ? omegaGuess(p, 2 * l.k2, l.s2) : omegaGuess(p, 2 * l.k, l.s);
        case PortDef::Law::Tanh:
            break;
        }
        return 0;
    }

    double lawStepsOf(const LawShape& l, double p, Solver::Guess guess, double previous, int steps, double* start,
                      Solver::Shape shape)
    {
        if (l.law == PortDef::Law::Pair || l.law == PortDef::Law::Tanh)
            return lawSteps(l.law, p, l.k, l.s, guess, previous, steps, start, shape);
        const double lo = std::fmin(0.0, p);
        const double hi = std::fmax(0.0, p);
        double v = std::fmin(std::fmax(guess == Solver::Guess::Omega ? lawGuess(l, p) : previous, lo), hi);
        for (int i = 0; i < steps; ++i)
        {
            if (start != nullptr)
                *start = v;
            v = std::fmin(std::fmax(v - (v - p + lawValue(l, v)) / (1 + lawDerivative(l, v)), lo), hi);
        }
        return v;
    }

    namespace
    {
        // e^dl's series as the text's: cosh(dl) to dl^4 and sinh(dl) to dl^3
        double seriesEvenOf(double dl)
        {
            const double dl2 = dl * dl;
            return 1 + dl2 * (0.5 + dl2 / 24);
        }

        double seriesOddOf(double dl)
        {
            return dl * (1 + dl * dl / 6);
        }

        // e^(v / s) as ADAA's tail computes it from e^(v0 / s): by the series while |dl| < the bound, else exactly
        double seriesExpOf(double v0, double v, double s)
        {
            const double dl = (v - v0) / s;
            if (std::fabs(dl) < kSeriesBound)
                return std::exp(v0 / s) * (seriesEvenOf(dl) + seriesOddOf(dl));
            return std::exp(v / s);
        }
    } // namespace

    double lawCoshSeries(double v0, double v, double s)
    {
        const double dl = (v - v0) / s;
        if (std::fabs(dl) < kSeriesBound)
        {
            const double e = std::exp(v0 / s);
            const double ei = std::exp(-v0 / s);
            return 0.5 * ((e + ei) * seriesEvenOf(dl) + (e - ei) * seriesOddOf(dl));
        }
        return 0.5 * (std::exp(v / s) + std::exp(-v / s));
    }

    double lawAntiderivativeSeries(const LawShape& l, double v0, double v)
    {
        switch (l.law)
        {
        case PortDef::Law::Single:
            return l.k * (l.s * seriesExpOf(v0, v, l.s) - v);
        case PortDef::Law::Asym:
            return l.k * (l.s * seriesExpOf(v0, v, l.s) - v) + l.k2 * (l.s2 * seriesExpOf(v0, v, -l.s2) + v);
        case PortDef::Law::Pair:
            return l.k * l.s * lawCoshSeries(v0, v, l.s);
        case PortDef::Law::Tanh:
            break;
        }
        return lawAntiderivative(l, v);
    }

    double lawConvergedOf(const LawShape& l, double p, double previous, long* unconverged, int* iterations)
    {
        if (l.law == PortDef::Law::Pair || l.law == PortDef::Law::Tanh)
            return lawConverged(l.law, p, l.k, l.s, previous, unconverged, iterations);
        // h(v) = v + g(v) - p rises (h' >= 1): the root is in [min(0, p), max(0, p)], and each value of h says which
        // side of v it is on; Newton from the closed-form guess, bisected where a step would leave the bracket
        double lo = std::fmin(0.0, p);
        double hi = std::fmax(0.0, p);
        double v = std::fmin(std::fmax(lawGuess(l, p), lo), hi);
        for (int i = 0; i < 300; ++i)
        {
            const double h = v - p + lawValue(l, v);
            if (h == 0)
            {
                if (iterations != nullptr)
                    *iterations = std::max(*iterations, i + 1);
                return v;
            }
            (h > 0 ? hi : lo) = v;
            double nv = v - h / (1 + lawDerivative(l, v));
            if (!(nv >= lo && nv <= hi))
                nv = 0.5 * (lo + hi);
            if (std::fabs(nv - v) < 1e-16 + 1e-15 * std::fabs(nv))
            {
                if (iterations != nullptr)
                    *iterations = std::max(*iterations, i + 1);
                return nv;
            }
            v = nv;
        }
        if (unconverged != nullptr)
            ++*unconverged;
        return v;
    }
} // namespace fminus::circuit
