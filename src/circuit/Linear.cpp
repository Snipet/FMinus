// src/circuit/Linear.cpp: the matched design's check at generation, its response, and its emitted coefficients
// (Linear.h).
#include "circuit/Linear.h"

#include "circuit/Laws.h"

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

        // The analog denominator in s / 2 fs, A0 + A1 s^ + A2 s^2 (matchedDesign's).
        std::vector<double> denominator(const TrForm<double>& tr)
        {
            if (tr.order == 1)
                return {1 - tr.a11, 1 + tr.a11};
            const double trace = tr.a11 + tr.a22;
            const double det = tr.a11 * tr.a22 - tr.a12 * tr.a21;
            return {1 - trace + det, 2 * (1 - det), 1 + trace + det};
        }

        // The block's analog response at f Hz from its trapezoidal form at `rate` (its inverse bilinear transform: the
        // circuit's own response, exactly).
        std::complex<double> trAnalog(const TrForm<double>& tr, double f, double rate)
        {
            const std::complex<double> s(0, std::numbers::pi * f / rate);   // s / 2 fs
            const std::vector<double> n = numerator(tr);
            const std::vector<double> d = denominator(tr);
            std::complex<double> num = 0;
            std::complex<double> den = 0;
            for (std::size_t k = n.size(); k-- > 0;)
                num = num * s + n[k];
            for (std::size_t k = d.size(); k-- > 0;)
                den = den * s + d[k];
            return num / den;
        }

        // The match frequency's analog s^ = j x: x, from w0, clamped to [kMatchFloor, pi / 4].
        double matchX(const TrForm<double>& tr)
        {
            if (tr.order == 1)
                return 0;
            const std::vector<double> d = denominator(tr);
            const double w2 = d[0] / d[2];
            const double quarter = std::numbers::pi * std::numbers::pi / 16;
            const double upTo = w2 - quarter < 0 ? w2 : quarter;
            return std::sqrt(upTo < kMatchFloor * kMatchFloor ? kMatchFloor * kMatchFloor : upTo);
        }

        TrForm<float> rounded(const TrForm<double>& tr)
        {
            TrForm<float> f;
            f.order = tr.order;
            f.a11 = static_cast<float>(tr.a11);
            f.a12 = static_cast<float>(tr.a12);
            f.a21 = static_cast<float>(tr.a21);
            f.a22 = static_cast<float>(tr.a22);
            f.b1 = static_cast<float>(tr.b1);
            f.b2 = static_cast<float>(tr.b2);
            f.c1 = static_cast<float>(tr.c1);
            f.c2 = static_cast<float>(tr.c2);
            f.d = static_cast<float>(tr.d);
            return f;
        }

        // A point of the control grid as the notes give it: "cut 0.0625, mode 1" (the block's pots and switches).
        std::string pointText(const Plan& plan, const Block& block, const std::vector<double>& point)
        {
            std::string text;
            for (const std::string& key : block.pots)
            {
                for (const PotInfo& pot : plan.pots)
                {
                    if (pot.key == key)
                        text += std::format("{}{} {:g}", text.empty() ? "" : ", ", pot.name,
                                            potValue(plan, point, key));
                }
            }
            for (const std::string& key : block.switches)
            {
                for (const SwitchInfo& sw : plan.switches)
                {
                    if (sw.key == key)
                        text += std::format("{}{} {}", text.empty() ? "" : ", ", sw.name,
                                            switchPosition(plan, point, key));
                }
            }
            return text;
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
                        out.why = std::format("its coefficients are not finite at {:g} kHz", rate / 1000);
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
        // The float run: the module computes the design in float, and near z = 1 (a low pole, a high rate) float
        // keeps fewer of its digits than double; so the design is run as the module runs it, on the trapezoidal form
        // rounded to float, at every host rate up to 192 kHz over a finer grid (17 positions a pot, every switch
        // position), and its float coefficients must be finite, its poles inside the unit circle, and its response
        // within the gate of the analog one by the same rule. (The analog response is the trapezoidal form's inverse
        // bilinear transform, exact.)
        std::vector<double> coarse;
        for (double f = 20; f <= 20000; f *= 1.02)
            coarse.push_back(f);
        double floatWorst = 0;
        std::string floatWhere;
        for (double rate : kMatchedRates)
        {
            for (const std::vector<double>& pots : potGrid(plan, block, 17))
            {
                const std::optional<TrForm<double>> tr = trForm(plan, blockIndex, rate, pots);
                if (!tr)
                {
                    out.why = "its form is not of order 1 or 2";
                    return out;
                }
                const Matched<float> f = matchedDesign(rounded(*tr), out.polarity);
                Matched<double> m;
                m.b0 = static_cast<double>(f.b0);
                m.b1 = static_cast<double>(f.b1);
                m.b2 = static_cast<double>(f.b2);
                m.a1 = static_cast<double>(f.a1);
                m.a2 = static_cast<double>(f.a2);
                const std::string where = pointText(plan, block, pots);
                const std::string at =
                    std::format("at {:g} kHz{}", rate / 1000, where.empty() ? std::string() : ", " + where);
                for (double c : {m.b0, m.b1, m.b2, m.a1, m.a2})
                {
                    if (!std::isfinite(c))
                    {
                        out.why = "its float coefficients are not finite " + at;
                        return out;
                    }
                }
                if (!(m.a2 < 1 && std::fabs(m.a1) < 1 + m.a2))
                {
                    out.why = "its float poles are not inside the unit circle " + at;
                    return out;
                }
                std::vector<double> analogDb;
                double peak = -1e300;
                for (double fr : coarse)
                {
                    analogDb.push_back(20 * std::log10(std::abs(trAnalog(*tr, fr, rate)) + 1e-300));
                    peak = std::max(peak, analogDb.back());
                }
                if (peak < -120)
                    continue;
                for (std::size_t i = 0; i < coarse.size(); ++i)
                {
                    if (analogDb[i] < peak - 40)
                        continue;
                    const double h = std::abs(matchedResponse(m, coarse[i], rate));
                    const double off = std::fabs(20 * std::log10(h + 1e-300) - analogDb[i]);
                    if (off > floatWorst)
                    {
                        floatWorst = off;
                        floatWhere = at;
                    }
                }
            }
        }
        out.floatWorst = floatWorst;
        if (floatWorst > kOneXTolerance)
        {
            out.why = std::format("in float, as the module computes it, its response errs by {:.2f} dB {}",
                                  floatWorst, floatWhere);
            return out;
        }
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

    // ---- the method check -------------------------------------------------------------------------------------------

    std::optional<std::vector<std::complex<double>>> eigenvalues(std::vector<std::vector<double>> a)
    {
        using C = std::complex<double>;
        const std::size_t n = a.size();
        std::vector<C> out;
        if (n == 0)
            return out;
        // Hessenberg: Householder reflections H = I - 2 v v^T on rows and columns k + 1 .. n - 1
        for (std::size_t k = 0; k + 2 < n; ++k)
        {
            double norm = 0;
            for (std::size_t i = k + 1; i < n; ++i)
                norm += a[i][k] * a[i][k];
            norm = std::sqrt(norm);
            if (norm == 0)
                continue;
            const double alpha = a[k + 1][k] > 0 ? -norm : norm;
            std::vector<double> v(n, 0.0);
            for (std::size_t i = k + 1; i < n; ++i)
                v[i] = a[i][k];
            v[k + 1] -= alpha;
            double vn = 0;
            for (std::size_t i = k + 1; i < n; ++i)
                vn += v[i] * v[i];
            vn = std::sqrt(vn);
            if (vn == 0)
                continue;
            for (std::size_t i = k + 1; i < n; ++i)
                v[i] /= vn;
            for (std::size_t j = 0; j < n; ++j)
            {
                double dot = 0;
                for (std::size_t i = k + 1; i < n; ++i)
                    dot += v[i] * a[i][j];
                for (std::size_t i = k + 1; i < n; ++i)
                    a[i][j] -= 2 * v[i] * dot;
            }
            for (std::size_t i = 0; i < n; ++i)
            {
                double dot = 0;
                for (std::size_t j = k + 1; j < n; ++j)
                    dot += a[i][j] * v[j];
                for (std::size_t j = k + 1; j < n; ++j)
                    a[i][j] -= 2 * dot * v[j];
            }
        }
        std::vector<std::vector<C>> h(n, std::vector<C>(n));
        for (std::size_t i = 0; i < n; ++i)
        {
            for (std::size_t j = 0; j < n; ++j)
                h[i][j] = a[i][j];
        }
        // the shifted QR algorithm on the active window [lo, hi], by Givens rotations
        std::size_t hi = n - 1;
        int iterations = 0;
        int total = 0;
        while (true)
        {
            if (hi == 0)
            {
                out.push_back(h[0][0]);
                break;
            }
            std::size_t lo = hi;
            while (lo > 0 && std::abs(h[lo][lo - 1]) > 1e-15 * (std::abs(h[lo][lo]) + std::abs(h[lo - 1][lo - 1])))
                --lo;
            if (lo == hi)
            {
                out.push_back(h[hi][hi]);
                --hi;
                iterations = 0;
                continue;
            }
            if (++total > 100 * static_cast<int>(n))
                return std::nullopt;
            // Wilkinson's shift: the trailing 2 x 2's eigenvalue nearer its corner (an exceptional one now and then)
            const C p = h[hi - 1][hi - 1];
            const C q = h[hi - 1][hi];
            const C r = h[hi][hi - 1];
            const C d = h[hi][hi];
            const C mean = 0.5 * (p + d);
            const C root = std::sqrt(mean * mean - (p * d - q * r));
            C mu = std::abs(mean + root - d) < std::abs(mean - root - d) ? mean + root : mean - root;
            if (++iterations % 11 == 0)
                mu = d + std::abs(r);
            for (std::size_t k = lo; k <= hi; ++k)
                h[k][k] -= mu;
            std::vector<std::pair<double, C>> rotations;
            for (std::size_t k = lo; k < hi; ++k)
            {
                const C x = h[k][k];
                const C y = h[k + 1][k];
                const double norm = std::sqrt(std::norm(x) + std::norm(y));
                double c = 1;
                C sn = 0;
                if (norm > 0)
                {
                    if (std::abs(x) == 0)
                    {
                        c = 0;
                        sn = std::conj(y) / std::abs(y);
                    }
                    else
                    {
                        c = std::abs(x) / norm;
                        sn = (x / std::abs(x)) * std::conj(y) / norm;
                    }
                }
                rotations.emplace_back(c, sn);
                for (std::size_t j = k; j <= hi; ++j)
                {
                    const C top = h[k][j];
                    const C bottom = h[k + 1][j];
                    h[k][j] = c * top + sn * bottom;
                    h[k + 1][j] = -std::conj(sn) * top + c * bottom;
                }
            }
            for (std::size_t k = lo; k < hi; ++k)
            {
                const auto [c, sn] = rotations[k - lo];
                for (std::size_t i = lo; i <= std::min(k + 2, hi); ++i)
                {
                    const C left = h[i][k];
                    const C right = h[i][k + 1];
                    h[i][k] = c * left + std::conj(sn) * right;
                    h[i][k + 1] = -sn * left + c * right;
                }
            }
            for (std::size_t k = lo; k <= hi; ++k)
                h[k][k] += mu;
        }
        return out;
    }

    RingingCheck ringingCheck(const Plan& plan, int blockIndex, double designRate)
    {
        const Block& block = plan.blocks.at(static_cast<std::size_t>(blockIndex));
        RingingCheck out;
        if (!block.port)
            return out;
        const PortDef& port = *block.port;
        const double level =
            plan.netlist.target.level ? *plan.netlist.target.level : std::fabs(plan.inputScale);
        // the blocks' systems and pivots, through this one, solved once
        std::vector<System> systems;
        std::vector<Pivots> pivots;
        for (int k = 0; k <= blockIndex; ++k)
        {
            systems.push_back(assemble(plan, k));
            Messages scratch;
            const std::optional<Pivots> order = choosePivots(systems.back(), plan, k, scratch);
            if (!order)
                return out;
            pivots.push_back(*order);
        }
        // the conductances: (G, pots)
        std::vector<std::pair<double, std::vector<double>>> points;
        for (const std::vector<double>& pots : potGrid(plan, block))
        {
            if (port.smooth())
            {
                // tanh's slope is largest at v = 0: g / 2 Vt, at the gains the law inputs' ends give
                std::vector<double> lows;
                std::vector<double> highs;
                for (const LawInfo& l : plan.laws)
                {
                    lows.push_back(l.low);
                    highs.push_back(l.high);
                }
                for (const std::vector<double>& laws : {lows, highs})
                    points.emplace_back(gainAt(plan, port, pots, laws) / port.nvt, pots);
                continue;
            }
            // a diode's: at v = 0, and at the largest short-circuit current at L on 20 Hz to 20 kHz
            const double at0 = port.law == PortDef::Law::Pair   ? 2 * port.is / port.nvt
                               : port.law == PortDef::Law::Asym ? port.is / port.nvt + port.is2 / port.nvt2
                                                                : port.is / port.nvt;
            points.emplace_back(at0, pots);
            double current = 0;
            for (double f = 20; f <= 20000; f *= 1.05)
            {
                const std::complex<double> s(0, 2 * std::numbers::pi * f);
                double gain = level / std::fabs(plan.inputScale);   // samples, through the blocks before
                for (int b = 0; b < blockIndex; ++b)
                {
                    const auto k = static_cast<std::size_t>(b);
                    gain *= std::abs(analog(systems[k], pivots[k], plan, b, s, pots));
                }
                const auto self = static_cast<std::size_t>(blockIndex);
                current = std::max(current, gain * std::abs(analogPortCurrent(systems[self], pivots[self], plan,
                                                                               blockIndex, s, pots)));
            }
            const double high = port.law == PortDef::Law::Pair
                                    ? std::sqrt(current * current + 4 * port.is * port.is) / port.nvt
                                : port.law == PortDef::Law::Asym
                                    ? std::max((current + port.is) / port.nvt, (current + port.is2) / port.nvt2)
                                    : (current + port.is) / port.nvt;
            points.emplace_back(high, pots);
        }
        double nearest = std::numeric_limits<double>::infinity();
        for (const auto& [g, pots] : points)
        {
            const Holters h = holters(plan, blockIndex, designRate, pots, {}, Method::Tr);
            const double s = 1 / (1 + h.kd * g);
            std::vector<std::vector<double>> a = h.a;
            for (std::size_t i = 0; i < a.size(); ++i)
            {
                for (std::size_t j = 0; j < a.size(); ++j)
                    a[i][j] += s * h.w[i] * h.cp[j];
            }
            const std::optional<std::vector<std::complex<double>>> eig = eigenvalues(a);
            if (!eig)
                continue;
            for (const std::complex<double>& z : *eig)
            {
                const double distance = std::abs(z + 1.0);
                if (distance < nearest)
                {
                    nearest = distance;
                    out.eigenvalue = z;
                    out.conductance = g;
                }
            }
        }
        out.flagged = nearest < 0.1;
        return out;
    }
} // namespace fminus::circuit
