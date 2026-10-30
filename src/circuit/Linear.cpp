// src/circuit/Linear.cpp: the matched design's check at generation, its response, and its emitted coefficients
// (Linear.h).
#include "circuit/Linear.h"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <format>
#include <limits>
#include <numbers>

namespace fminus::circuit
{
    std::complex<double> matchedResponse(const Matched<double>& m, double f, double rate)
    {
        const std::complex<double> w = std::polar(1.0, -2 * std::numbers::pi * f / rate);   // z^-1
        return (m.b0 + m.b1 * w + m.b2 * w * w) / (1.0 + m.a1 * w + m.a2 * w * w);
    }

    std::optional<TrForm<double>> trForm(const Plan& plan, int block, double rate, const std::vector<double>& pots)
    {
        const Holters h = holters(plan, block, rate, pots, {}, Method::Tr);
        if (h.port || h.states < 1 || h.states > 2)
            return std::nullopt;
        TrForm<double> tr;
        tr.order = h.states;
        tr.a11 = h.a[0][0];
        tr.b1 = h.b[0];
        tr.c1 = h.cy[0];
        tr.d = h.dy;
        if (h.states == 2)
        {
            tr.a12 = h.a[0][1];
            tr.a21 = h.a[1][0];
            tr.a22 = h.a[1][1];
            tr.b2 = h.b[1];
            tr.c2 = h.cy[1];
        }
        return tr;
    }

    namespace
    {
        // The analog numerator in s / 2 fs, N0 + N1 s^ + N2 s^2, from the trapezoidal form (matchedDesign's).
        std::vector<double> numerator(const TrForm<double>& tr)
        {
            if (tr.order == 1)
            {
                const double beta1 = tr.c1 * tr.b1 - tr.a11 * tr.d;
                return {tr.d + beta1, tr.d - beta1};
            }
            const double trace = tr.a11 + tr.a22;
            const double det = tr.a11 * tr.a22 - tr.a12 * tr.a21;
            const double cb = tr.c1 * tr.b1 + tr.c2 * tr.b2;
            const double cab = tr.c1 * (tr.a11 * tr.b1 + tr.a12 * tr.b2) + tr.c2 * (tr.a21 * tr.b1 + tr.a22 * tr.b2);
            const double beta1 = cb - trace * tr.d;
            const double beta2 = det * tr.d - trace * cb + cab;
            return {tr.d + beta1 + beta2, 2 * (tr.d - beta2), tr.d - beta1 + beta2};
        }

        // The match frequency's analog s^ = j x: x, from w0 (or pi / 4 above it).
        double matchX(const TrForm<double>& tr)
        {
            if (tr.order == 1)
                return 0;
            const double trace = tr.a11 + tr.a22;
            const double det = tr.a11 * tr.a22 - tr.a12 * tr.a21;
            const double w2 = (1 - trace + det) / (1 + trace + det);
            const double quarter = std::numbers::pi * std::numbers::pi / 16;
            return std::sqrt(w2 - quarter < 0 ? w2 : quarter);
        }
    } // namespace

    MatchedCheck matchedCheck(const Plan& plan, int blockIndex)
    {
        const Block& block = plan.blocks.at(static_cast<std::size_t>(blockIndex));
        MatchedCheck out;
        int states = 0;
        for (const Branch& br : block.branches)
            states += br.state() ? 1 : 0;
        if (block.port || states < 1 || states > 2)
        {
            out.why = std::format("{} states: a matched design is of order 1 or 2", states);
            return out;
        }
        const System system = assemble(plan, blockIndex);
        Messages scratch;
        const std::optional<Pivots> pivots = choosePivots(system, plan, blockIndex, scratch);
        if (!pivots)
        {
            out.why = "its equations are singular";
            return out;
        }
        std::vector<double> freqs;
        for (double f = 20; f <= 20000; f *= 1.01)
            freqs.push_back(f);
        bool polaritySet = false;
        double worst = 0;
        for (double rate : {44100.0, 48000.0})
        {
            for (const std::vector<double>& pots : potGrid(plan, block))
            {
                const std::optional<TrForm<double>> tr = trForm(plan, blockIndex, rate, pots);
                if (!tr)
                {
                    out.why = "its form is not of order 1 or 2";
                    return out;
                }
                // its zeros: minimum-phase (the numerator's coefficients of one sign), or the magnitude match, which
                // makes them so, changes its phase
                const std::vector<double> n = numerator(*tr);
                double top = 0;
                for (double x : n)
                    top = std::max(top, std::fabs(x));
                bool positive = false;
                bool negative = false;
                for (double x : n)
                {
                    if (std::fabs(x) <= 1e-12 * top)
                        continue;
                    (x > 0 ? positive : negative) = true;
                }
                if (positive && negative)
                {
                    out.why = "its zeros are not minimum-phase (a magnitude match would change its phase)";
                    return out;
                }
                const Matched<double> m = matchedDesign(*tr, 1.0);
                for (double c : {m.b0, m.b1, m.b2, m.a1, m.a2})
                {
                    if (!std::isfinite(c))
                    {
                        out.why = std::format("its factorisation fails at {:g} kHz", rate / 1000);
                        return out;
                    }
                }
                // the polarity: the design's response against the analog one's at the match frequency (or at
                // Nyquist's quarter, for a one-pole)
                const double x = tr->order == 1 ? std::numbers::pi / 4 : matchX(*tr);
                const double f = rate * x / std::numbers::pi;
                const std::complex<double> ha =
                    analog(system, *pivots, plan, blockIndex, std::complex<double>(0, 2 * std::numbers::pi * f), pots);
                const std::complex<double> hm = matchedResponse(m, f, rate);
                const double sign = (hm * std::conj(ha)).real() < 0 ? -1.0 : 1.0;
                if (std::abs(ha) > 0)
                {
                    if (polaritySet && sign != out.polarity)
                    {
                        out.why = "its polarity changes with its pots";
                        return out;
                    }
                    out.polarity = sign;
                    polaritySet = true;
                }
                // the 1x check's rule on the design's response
                std::vector<double> analogDb;
                double peak = -1e300;
                for (double fr : freqs)
                {
                    const double h = std::abs(analog(system, *pivots, plan, blockIndex,
                                                     std::complex<double>(0, 2 * std::numbers::pi * fr), pots));
                    analogDb.push_back(20 * std::log10(h + 1e-300));
                    peak = std::max(peak, analogDb.back());
                }
                if (peak < -120)
                    continue;
                for (std::size_t i = 0; i < freqs.size(); ++i)
                {
                    if (analogDb[i] < peak - 40)
                        continue;
                    const double h = std::abs(matchedResponse(m, freqs[i], rate));
                    worst = std::max(worst, std::fabs(20 * std::log10(h + 1e-300) - analogDb[i]));
                }
            }
        }
        out.worst = worst;
        out.usable = true;
        return out;
    }

    Coefficients<Val> matchedCoefficients(const Plan& plan, int blockIndex, const System& system,
                                          const Linear<Val>& linear, Code& code)
    {
        const Block& block = plan.blocks.at(static_cast<std::size_t>(blockIndex));
        std::vector<std::size_t> states;
        for (std::size_t s = 0; s < system.sources.size(); ++s)
        {
            if (system.sources[s].kind == System::Source::Kind::State)
                states.push_back(s);
        }
        const auto in = static_cast<std::size_t>(system.input);
        const auto out = static_cast<std::size_t>(system.output);
        // a state's update factor: 2 gc (a capacitor's g2_), 2 rl (an inductor's r2_), init's fields
        auto factor = [&](std::size_t i)
        {
            const Branch& br = block.branches[static_cast<std::size_t>(system.sources[states[i]].branch)];
            return Val::var((br.kind == Branch::Kind::Inductor ? "r2_" : "g2_") + br.ident, &code, Val::kRate);
        };
        auto a = [&](std::size_t i, std::size_t j)
        {
            const auto [readout, sign] = system.capacitor[i];
            const Val term = factor(i) * (Val(sign) * linear.coef[static_cast<std::size_t>(readout)][states[j]]);
            return i == j ? term - Val(1.0) : term;
        };
        auto b = [&](std::size_t i)
        {
            const auto [readout, sign] = system.capacitor[i];
            return factor(i) * (Val(sign) * linear.coef[static_cast<std::size_t>(readout)][in]);
        };
        TrForm<Val> tr;
        tr.order = static_cast<int>(states.size());
        tr.a11 = a(0, 0);
        tr.b1 = b(0);
        tr.c1 = linear.coef[out][states[0]];
        tr.d = linear.coef[out][in];
        if (states.size() == 2)
        {
            tr.a12 = a(0, 1);
            tr.a21 = a(1, 0);
            tr.a22 = a(1, 1);
            tr.b2 = b(1);
            tr.c2 = linear.coef[out][states[1]];
        }
        const Matched<Val> m = matchedDesign(tr, block.matchedPolarity);
        const std::string n = std::to_string(block.index);
        Coefficients<Val> coefficients;
        auto bind = [&](const std::string& name, const Val& v)
        {
            coefficients.emplace_back(name + "_b" + n, bindField(name + "_b" + n, v));
        };
        code.bindFields(true);
        bind("mb0", m.b0);
        bind("mb1", m.b1);
        if (tr.order == 2)
            bind("mb2", m.b2);
        bind("ma1", m.a1);
        if (tr.order == 2)
            bind("ma2", m.a2);
        return coefficients;
    }
} // namespace fminus::circuit
