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

        std::vector<LawLine> pairSolve(const PortDef& port, const LawOperands& o)
        {
            const std::string& T = o.type;
            const std::string ivt = literal(1 / port.nvt);
            const std::string nvt = literal(port.nvt);
            std::vector<LawLine> out;
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
            for (int k = 1; k <= o.solver.steps; ++k)
            {
                out.push_back({"{", std::format("Newton step {} of {}, kept in the bracket", k, o.solver.steps)});
                out.push_back({"    const " + T + " e = expf(v * " + ivt + ");", ""});
                out.push_back({"    const " + T + " ei = 1 / e;", ""});
                out.push_back({"    const " + T + " f = v - p + " + o.ka + " * (e - ei);", ""});
                out.push_back({"    const " + T + " d = 1 + " + o.kb + " * (e + ei);", ""});
                out.push_back({"    v = fminf(fmaxf(v - f / d, lo), hi);", ""});
                out.push_back({"}", ""});
            }
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
                out.push_back({"const float kgv = kg * " + literal(port.nvt) + ";", "ADAA: kg 2 Vt"});
            out.push_back({"const " + T + " lo = fminf(p, 0);", ""});
            out.push_back({"const " + T + " hi = fmaxf(p, 0);", ""});
            out.push_back({T + " v = fminf(fmaxf(v_" + o.port + ", lo), hi);",
                           "the previous step's solution, in the bracket"});
            for (int k = 1; k <= o.solver.steps; ++k)
            {
                out.push_back({"{", std::format("Newton step {} of {}, kept in the bracket", k, o.solver.steps)});
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

    std::vector<LawLine> solveText(const PortDef& port, const LawOperands& o)
    {
        return port.smooth() ? tanhSolve(port, o) : pairSolve(port, o);
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
            out.push_back({"p_" + P + " = p_" + P + " + (kg - kgo_" + P + ") * t_" + P + ";",
                           "ADAA: the last point onto this step's curve (kd g moved), its residual kept"});
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
            out.push_back({"const " + T + " dp = p - p_" + P + ";", ""});
            out.push_back({"const " + T + " wm = 0.5f * kg * (th + t_" + P + ");",
                           "p - v at the midpoint, from the law: kd times its current"});
            out.push_back({"const " + T + " ib = (fabsf(dp) > " + literal(kAdaaEpsilon) +
                               " ? wm + (kgv * dl - wm * (v - v_" + P + ")) / dp : wm) / " + o.kd + ";",
                           "the mean current, (p - G(p)) / kd over [p_P, p]: the midpoint's and a correction"});
            return out;
        }
        out.push_back({"const " + T + " ec = expf(v * " + ivt + ");",
                       "ADAA: the mean of G over [p(n-1), p(n)], closed form"});
        out.push_back({"const " + T + " c = ec + 1 / ec;", ""});
        const std::string phi = o.kc + " * (c - c_" + P + ")";
        out.push_back({"const " + T + " dp = p - p_" + P + ";", ""});
        out.push_back({"const " + T + " vm = 0.5f * (v + v_" + P + ");", ""});
        out.push_back({"const " + T + " vb = fabsf(dp) > " + literal(kAdaaEpsilon) + " ? vm + ((v - v_" + P +
                           ") * (0.5f * (p + p_" + P + ") - vm) - " + phi + ") / dp : vm;",
                       ""});
        return out;
    }

    std::vector<LawLine> storeText(const PortDef& port, const LawOperands& o)
    {
        const std::string& P = o.port;
        std::vector<LawLine> out;
        if (!o.adaa)
            return out;
        out.push_back({"p_" + P + " = p;", ""});
        if (port.smooth())
        {
            out.push_back({"t_" + P + " = th;", ""});
            out.push_back({"lc_" + P + " = lc;", ""});
            out.push_back({"kgo_" + P + " = kg;", ""});
        }
        else
            out.push_back({"c_" + P + " = c;", ""});
        return out;
    }

    std::vector<LawField> adaaFields(const PortDef& port)
    {
        const std::string& P = port.name;
        if (!port.smooth())
            return {{"c_" + P, true, "ADAA: 2 cosh(v_" + P + " / N Vt)", "2"}};
        return {{"t_" + P, true, "ADAA: tanh(v_" + P + " / 2 Vt), for the carry and the mean", "0"},
                {"lc_" + P, true, "ADAA: log cosh(v_" + P + " / 2 Vt), for the mean of a long step", "0"},
                {"kgo_" + P, false, "ADAA: kd g at the last step", "0"}};
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

    double lawSteps(PortDef::Law law, double p, double k, double s, Solver::Guess guess, double previous, int steps)
    {
        const double lo = std::fmin(0.0, p);
        const double hi = std::fmax(0.0, p);
        if (law != PortDef::Law::Tanh)
        {
            const double g = guess == Solver::Guess::Omega ? omegaGuess(p, k, s) : previous;
            double v = std::fmin(std::fmax(g, lo), hi);
            for (int i = 0; i < steps; ++i)
            {
                const double e = std::exp(v / s);
                const double ei = 1 / e;
                const double f = v - p + k * 0.5 * (e - ei);
                const double d = 1 + (k / s) * 0.5 * (e + ei);
                v = std::fmin(std::fmax(v - f / d, lo), hi);
            }
            return v;
        }
        const double ivt = 1 / s;
        const double ks = k * ivt;
        double v = std::fmin(std::fmax(previous, lo), hi);
        for (int i = 0; i < steps; ++i)
        {
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
} // namespace fminus::circuit
