// src/circuit/Search.cpp: the self-test's arithmetic (Search.h), and Choice's text (Circuit.h). The stress matrix and
// the gates are examples/circuits/check.cpp's (Phase 0's section c), generalised from one block to a plan's chain.
#include "circuit/Search.h"

#include "circuit/Analysis.h"
#include "circuit/Mna.h"
#include "circuit/Reference.h"

#include <algorithm>
#include <array>
#include <charconv>
#include <cmath>
#include <format>
#include <locale>
#include <sstream>
#include <stdexcept>
#include <system_error>

namespace fminus::circuit
{
    // ---- Choice -----------------------------------------------------------------------------------------------------

    namespace
    {
        // A double as the shortest text that reads back as it (std::to_chars: locale-free).
        std::string shortest(double value)
        {
            if (std::isnan(value))
                return "nan";
            if (std::isinf(value))
                return value < 0 ? "-inf" : "inf";
            std::array<char, 64> buffer{};
            const std::to_chars_result r = std::to_chars(buffer.data(), buffer.data() + buffer.size(), value);
            if (r.ec != std::errc())
                throw std::runtime_error("std::to_chars failed");
            return std::string(buffer.data(), r.ptr);
        }

        std::optional<double> readDouble(std::string_view text)
        {
            if (text == "nan")
                return std::numeric_limits<double>::quiet_NaN();
            if (text == "inf")
                return std::numeric_limits<double>::infinity();
            if (text == "-inf")
                return -std::numeric_limits<double>::infinity();
            if (text.empty() || text.find_first_not_of("0123456789+-.eE") != std::string_view::npos)
                return std::nullopt;
            std::istringstream stream{std::string(text)};
            stream.imbue(std::locale::classic());
            double v = 0;
            stream >> v;
            if (stream.fail() || !stream.eof())
                return std::nullopt;
            return v;
        }

        std::optional<int> readInt(std::string_view text)
        {
            int v = 0;
            const std::from_chars_result r = std::from_chars(text.data(), text.data() + text.size(), v);
            if (r.ec != std::errc() || r.ptr != text.data() + text.size())
                return std::nullopt;
            return v;
        }

        std::string solverText(const Solver& s)
        {
            return std::string(s.guess == Solver::Guess::Omega ? "omega(" : "newton(") + std::to_string(s.steps) + ")";
        }

        std::optional<Solver> readSolver(std::string_view text)
        {
            Solver s;
            std::string_view rest;
            if (text.starts_with("omega("))
            {
                s.guess = Solver::Guess::Omega;
                rest = text.substr(6);
            }
            else if (text.starts_with("newton("))
            {
                s.guess = Solver::Guess::Previous;
                rest = text.substr(7);
            }
            else
                return std::nullopt;
            if (!rest.ends_with(")"))
                return std::nullopt;
            const std::optional<int> k = readInt(rest.substr(0, rest.size() - 1));
            const int lo = s.guess == Solver::Guess::Omega ? 0 : 1;
            if (!k || *k < lo || *k > 8)
                return std::nullopt;
            s.steps = *k;
            return s;
        }

        bool isKey(std::string_view key)
        {
            return key.size() == 16 && std::all_of(key.begin(), key.end(),
                                                   [](char c) { return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'); });
        }
    } // namespace

    std::string Choice::text() const
    {
        std::string out = "fminus-circuit-choice " + std::to_string(kSelfTestVersion) + " key=" + key +
                          " oversample=" + std::to_string(oversample) + " adaa=" + (adaa ? "on" : "off") +
                          " solver=" + solverText(solver) + " method=" + (method == Method::Tr ? "tr" : "bdf2") +
                          " met=" + (met ? "yes" : "no") + " t1=" + shortest(t1) + " t2=" + shortest(t2) +
                          " t3=" + shortest(t3);
        if (!worst.empty())
            out += " worst=" + worst;
        return out;
    }

    std::optional<Choice> Choice::parse(std::string_view text, std::string* why)
    {
        auto fail = [&](std::string reason) -> std::optional<Choice>
        {
            if (why != nullptr)
                *why = std::move(reason);
            return std::nullopt;
        };
        while (!text.empty() && (text.back() == '\n' || text.back() == '\r' || text.back() == ' '))
            text.remove_suffix(1);
        // worst= takes the rest of the line
        std::string_view worst;
        bool hasWorst = false;
        if (const std::size_t at = text.find(" worst="); at != std::string_view::npos)
        {
            worst = text.substr(at + 7);
            text = text.substr(0, at);
            hasWorst = true;
        }
        std::vector<std::string_view> words;
        for (std::size_t i = 0; i < text.size();)
        {
            const std::size_t end = std::min(text.find(' ', i), text.size());
            if (end > i)
                words.push_back(text.substr(i, end - i));
            i = end + 1;
        }
        if (words.size() < 2 || words[0] != "fminus-circuit-choice")
            return fail("not a self-test's answer (it begins 'fminus-circuit-choice')");
        const std::optional<int> version = readInt(words[1]);
        if (!version)
            return fail("its version '" + std::string(words[1]) + "' is not a number");
        if (*version != kSelfTestVersion)
            return fail("it is self-test version " + std::to_string(*version) + "'s; this one's is " +
                        std::to_string(kSelfTestVersion));
        Choice c;
        c.worst = std::string(worst);
        if (hasWorst && worst.empty())
            return fail("worst= is empty");
        const std::array<std::string_view, 9> names{"key", "oversample", "adaa", "solver", "method",
                                                    "met", "t1",         "t2",   "t3"};
        std::array<bool, 9> seen{};
        for (std::size_t i = 2; i < words.size(); ++i)
        {
            const std::size_t eq = words[i].find('=');
            const std::string_view name = words[i].substr(0, eq);
            const std::string_view value = eq == std::string_view::npos ? std::string_view() : words[i].substr(eq + 1);
            const auto it = std::find(names.begin(), names.end(), name);
            if (eq == std::string_view::npos || it == names.end())
                return fail("'" + std::string(words[i]) + "' is not one of its fields");
            const auto index = static_cast<std::size_t>(it - names.begin());
            if (seen[index])
                return fail(std::string(name) + "= is given twice");
            seen[index] = true;
            const std::string bad = "'" + std::string(words[i]) + "' is not a valid " + std::string(name);
            switch (index)
            {
            case 0:
                if (!isKey(value))
                    return fail(bad + " (16 hex digits)");
                c.key = std::string(value);
                break;
            case 1:
            {
                const std::optional<int> os = readInt(value);
                if (!os || (*os != 1 && *os != 2 && *os != 4 && *os != 8))
                    return fail(bad + " (1, 2, 4 or 8)");
                c.oversample = *os;
                break;
            }
            case 2:
                if (value != "on" && value != "off")
                    return fail(bad + " (on or off)");
                c.adaa = value == "on";
                break;
            case 3:
            {
                const std::optional<Solver> s = readSolver(value);
                if (!s)
                    return fail(bad + " (omega(K) or newton(K))");
                c.solver = *s;
                break;
            }
            case 4:
                if (value != "tr" && value != "bdf2")
                    return fail(bad + " (tr or bdf2)");
                c.method = value == "tr" ? Method::Tr : Method::Bdf2;
                break;
            case 5:
                if (value != "yes" && value != "no")
                    return fail(bad + " (yes or no)");
                c.met = value == "yes";
                break;
            default:
            {
                const std::optional<double> v = readDouble(value);
                if (!v)
                    return fail(bad + " (a number)");
                (index == 6 ? c.t1 : index == 7 ? c.t2 : c.t3) = *v;
                break;
            }
            }
        }
        for (std::size_t i = 0; i < names.size(); ++i)
        {
            if (!seen[i])
                return fail("it has no " + std::string(names[i]) + "=");
        }
        return c;
    }

    // ---- the stress matrix ------------------------------------------------------------------------------------------

    std::string StressMatrix::where(int unit) const
    {
        const std::string& label = point(unit).label;
        return signal(unit).name + (label.empty() ? std::string() : ", " + label);
    }

    namespace
    {
        std::size_t nextPow2(std::size_t n)
        {
            std::size_t p = 1;
            while (p < n)
                p <<= 1;
            return p;
        }
    } // namespace

    StressMatrix stressMatrix(const Plan& plan, const StressShape& shape)
    {
        StressMatrix m;
        m.shape = shape;
        if (plan.netlist.target.level)
        {
            m.level = *plan.netlist.target.level;
            m.levelFrom = ".target level";
        }
        else
        {
            m.level = std::fabs(plan.inputScale);
            m.levelFrom = ".input's scale (full scale)";
        }
        m.upstream = std::max(plan.regionFirst, 0);
        // the pots the region and the blocks before it read, in .pot order
        std::vector<std::size_t> axes;
        const int last = plan.regionFirst >= 0 ? plan.regionLast : static_cast<int>(plan.blocks.size()) - 1;
        for (std::size_t i = 0; i < plan.pots.size(); ++i)
        {
            bool read = false;
            for (int b = 0; b <= last; ++b)
            {
                const std::vector<std::string>& keys = plan.blocks[static_cast<std::size_t>(b)].pots;
                read = read || std::find(keys.begin(), keys.end(), plan.pots[i].key) != keys.end();
            }
            if (read)
                axes.push_back(i);
        }
        std::vector<double> defaults;
        for (const PotInfo& p : plan.pots)
            defaults.push_back(p.defaultValue);
        auto label = [&](const std::vector<double>& pots)
        {
            std::string text;
            for (std::size_t i : axes)
                text += (text.empty() ? "" : ", ") + plan.pots[i].name + std::format(" {:g}", pots[i]);
            return text;
        };
        const double values[] = {0.0, 0.5, 1.0};
        if (axes.size() <= 3)
        {
            std::size_t count = 1;
            for (std::size_t i = 0; i < axes.size(); ++i)
                count *= 3;
            for (std::size_t k = 0; k < count; ++k)
            {
                std::vector<double> point = defaults;
                std::size_t rest = k;
                for (std::size_t i : axes)
                {
                    point[i] = values[rest % 3];
                    rest /= 3;
                }
                m.points.push_back({point, label(point)});
            }
        }
        else
        {
            for (std::size_t i : axes)
            {
                for (double v : values)
                {
                    std::vector<double> point = defaults;
                    point[i] = v;
                    m.points.push_back({point, label(point)});
                }
            }
            for (double v : {0.0, 1.0})
            {
                std::vector<double> point = defaults;
                for (std::size_t i : axes)
                    point[i] = v;
                m.points.push_back({point, label(point)});
            }
        }
        const double scale = std::fabs(plan.inputScale);
        const double level = m.level;
        for (double volts : {level / 15, level / 4.5, level / 2.25, level, 2.2 * level})
        {
            for (std::size_t b = 0; b < shape.bins.size(); ++b)
            {
                StressSignal s;
                s.kind = StressSignal::Kind::Sine;
                s.amp = volts / scale;
                s.bin = shape.bins[b];
                s.name = std::format("{:.3g} V, {:.0f} Hz", volts,
                                     s.bin * kHostRate / static_cast<double>(shape.window));
                s.overload = volts > level * 1.01;
                s.t1 = !s.overload;
                s.t2 = !s.overload;
                s.t3 = !s.overload && b > 0;
                m.signals.push_back(s);
            }
        }
        for (StressSignal::Kind kind : {StressSignal::Kind::Sweep, StressSignal::Kind::Noise})
        {
            StressSignal s;
            s.kind = kind;
            s.amp = level / scale;
            s.name = std::format("{} at {:.3g} V", kind == StressSignal::Kind::Sweep ? "sweep" : "noise", level);
            m.signals.push_back(s);
        }
        return m;
    }

    std::vector<double> signalSamples(const StressMatrix& matrix, const StressSignal& signal, int m, std::size_t n)
    {
        switch (signal.kind)
        {
        case StressSignal::Kind::Sine:
            return sine(signal.amp, signal.bin, matrix.shape.window, m, n);
        case StressSignal::Kind::Sweep:
            return sweep(signal.amp, m, n);
        case StressSignal::Kind::Noise:
        {
            std::vector<double> v = noise(signal.amp, m, nextPow2(n));
            v.resize(n);
            return v;
        }
        }
        return {};
    }

    // ---- the references ---------------------------------------------------------------------------------------------

    Reference64 reference64(const Plan& plan, const StressMatrix& matrix, int unit, const Stop& stop)
    {
        Reference64 r;
        const StressSignal& signal = matrix.signal(unit);
        const int factor = matrix.shape.referenceFactor;
        std::vector<Holters> chain;
        for (std::size_t b = 0; b < plan.blocks.size(); ++b)
            chain.push_back(holters(plan, static_cast<int>(b), kHostRate * factor, matrix.point(unit).pots));
        const std::size_t n = static_cast<std::size_t>(factor) * matrix.shape.host();
        RefOptions o;
        o.stop = stop;
        RefStats stats;
        const std::vector<double> y = simulateChain(chain, signalSamples(matrix, signal, factor, n), o, &stats);
        if (stats.stopped)
            return r;
        const auto rate = static_cast<std::size_t>(factor);
        const std::span<const double> window =
            std::span<const double>(y).subspan(rate * matrix.shape.warm, rate * matrix.shape.window);
        r.h1 = binMagnitude(window, signal.bin);
        r.h3 = binMagnitude(window, 3 * signal.bin);
        r.power = bandPower(decimateWindow(y, factor, matrix.shape.warm, matrix.shape.window));
        r.unconverged = stats.unconverged;
        r.done = true;
        return r;
    }

    std::vector<double> regionInput(const Plan& plan, const StressMatrix& matrix, int unit, int oversample,
                                    const Stop& stop, bool* stopped)
    {
        const std::size_t n = static_cast<std::size_t>(oversample) * matrix.shape.host();
        std::vector<double> u = signalSamples(matrix, matrix.signal(unit), oversample, n);
        if (plan.regionFirst <= 0)
            return u;
        std::vector<Holters> before;
        for (int b = 0; b < plan.regionFirst; ++b)
            before.push_back(holters(plan, b, kHostRate * oversample, matrix.point(unit).pots));
        RefOptions o;
        o.stop = stop;
        RefStats stats;
        std::vector<double> y = simulateChain(before, u, o, &stats);
        if (stopped != nullptr)
            *stopped = stats.stopped;
        return y;
    }

    ReferenceT1 referenceT1(const Plan& plan, const StressMatrix& matrix, int unit, int oversample, bool adaa,
                            Method method, const Stop& stop)
    {
        if (method != Method::Tr)
            throw std::logic_error("referenceT1: only the trapezoidal rule");
        ReferenceT1 r;
        bool stopped = false;
        const std::vector<double> u = regionInput(plan, matrix, unit, oversample, stop, &stopped);
        if (stopped)
            return r;
        std::vector<Holters> region;
        for (int b = plan.regionFirst; b <= plan.regionLast; ++b)
        {
            const double design = kHostRate * oversample *
                                  (adaa && plan.blocks[static_cast<std::size_t>(b)].port ? 2.0 / 3.0 : 1.0);
            region.push_back(holters(plan, b, design, matrix.point(unit).pots));
        }
        RefOptions o;
        o.adaa = adaa;
        o.stop = stop;
        RefStats stats;
        const std::vector<double> y = simulateChain(region, u, o, &stats);
        if (stats.stopped)
            return r;
        r.window = decimateWindow(y, oversample, matrix.shape.warm, matrix.shape.window);
        r.unconverged = stats.unconverged;
        r.done = true;
        return r;
    }

    // ---- a unit's gates, and the verdict ----------------------------------------------------------------------------

    UnitVerdict measureUnit(const StressMatrix& matrix, int unit, int oversample, std::span<const double> steps,
                            std::span<const double> ticks, const ReferenceT1* t1, const Reference64* t3)
    {
        UnitVerdict v;
        v.ran = true;
        const auto isFinite = [](double x) { return std::isfinite(x); };
        v.finite =
            std::all_of(steps.begin(), steps.end(), isFinite) && std::all_of(ticks.begin(), ticks.end(), isFinite);
        if (!v.finite)
            return v;
        const StressSignal& signal = matrix.signal(unit);
        const StressShape& shape = matrix.shape;
        if (signal.t1 && t1 != nullptr)
        {
            const std::vector<double> yd = decimateWindow(steps, oversample, shape.warm, shape.window);
            v.t1 = bandErrorDb(yd, t1->window, 0, signal.kind != StressSignal::Kind::Sine);
        }
        const std::span<const double> window = ticks.subspan(shape.warm, shape.window);
        if (signal.t2 && t3 != nullptr)
            v.t2 = inharmonicExcessDb(window, t3->power, signal.bin);
        if (signal.t3 && t3 != nullptr)
        {
            const double h1 = std::fabs(20 * std::log10(binMagnitude(window, signal.bin) / t3->h1));
            v.t3 = h1;
            v.t3Which = "H1";
            if (t3->h3 / t3->h1 >= 1e-5)   // H3 counts within 100 dB of H1
            {
                const double h3 = std::fabs(20 * std::log10(binMagnitude(window, 3 * signal.bin) / t3->h3));
                if (h3 > h1)
                {
                    v.t3 = h3;
                    v.t3Which = "H3";
                }
            }
        }
        return v;
    }

    bool Verdict::passes() const
    {
        return finite && unconverged == 0 && t1.value <= kGateDb && t2.value <= kGateDb && t3.value <= kResponseDb;
    }

    double Verdict::excess() const
    {
        if (!finite)
            return std::numeric_limits<double>::infinity();
        return std::max({t1.value - kGateDb, t2.value - kGateDb, t3.value - kResponseDb});
    }

    std::string Verdict::worst() const
    {
        if (!finite)
            return "T4 not finite at " + notFinite;
        if (unconverged > 0)
            return std::format("T4: the references ran out of Newton steps on {} samples", unconverged);
        const double e1 = t1.value - kGateDb;
        const double e2 = t2.value - kGateDb;
        const double e3 = t3.value - kResponseDb;
        if (e3 >= e1 && e3 >= e2)
            return std::format("T3 {:.2f} dB at {}", t3.value, t3.where);
        if (e2 >= e1)
            return std::format("T2 {:.1f} dB at {}", t2.value, t2.where);
        return std::format("T1 {:.1f} dB at {}", t1.value, t1.where);
    }

    std::string Verdict::figures() const
    {
        return std::format("T1 {:.1f} dB, T2 {:.1f} dB, T3 {:.2f} dB", t1.value, t2.value, t3.value);
    }

    Verdict reduce(const StressMatrix& matrix, std::span<const UnitVerdict> units, long unconverged)
    {
        Verdict v;
        v.unconverged = unconverged;
        for (std::size_t u = 0; u < units.size(); ++u)
        {
            const UnitVerdict& r = units[u];
            if (!r.ran)
                continue;
            const std::string where = matrix.where(static_cast<int>(u));
            if (!r.finite)
            {
                if (v.finite)
                    v.notFinite = where;
                v.finite = false;
                continue;
            }
            if (std::isfinite(r.t1))
                v.t1.max(r.t1, where);
            if (std::isfinite(r.t2))
                v.t2.max(r.t2, where);
            if (std::isfinite(r.t3))
                v.t3.max(r.t3, where + ", " + r.t3Which);
        }
        return v;
    }

    // ---- the candidates ---------------------------------------------------------------------------------------------

    std::string Candidate::text() const
    {
        return configurationText(oversample, Halfband::Iir, adaa, solver, false);
    }

    namespace
    {
        // The report's §3.2 model, cycles on the recursion's critical path.
        struct CostTable
        {
            double p = 10;            // p = G x + H u
            double perState = 2;      // and each state in it
            double omega = 65;        // omega3's guess
            double previous = 5;      // the previous step's solution
            double newtonPair = 70;   // a Newton step of a diode pair (2 Is sinh)
            double adaaEa = 57;       // ADAA's tail on an exp-affine law
            double update = 5;        // the state update
            double linear = 5;        // a linear block's step
            double linearPerState = 3;
            double halfband = 12;     // a halfband stage's sample at its high rate, up and down
        };

        constexpr CostTable kCost;

        int statesOf(const Block& b)
        {
            return static_cast<int>(std::count_if(b.branches.begin(), b.branches.end(),
                                                  [](const Branch& br) { return br.kind == Branch::Kind::Capacitor; }));
        }
    } // namespace

    double modelCost(const Plan& plan, int oversample, bool adaa, const Solver& solver)
    {
        double total = 0;
        for (const Block& b : plan.blocks)
        {
            const double rate = b.inRegion ? oversample : 1;
            const int states = statesOf(b);
            double step = 0;
            if (b.port)
                step = kCost.p + kCost.perState * states +
                       (solver.guess == Solver::Guess::Omega ? kCost.omega : kCost.previous) +
                       kCost.newtonPair * solver.steps + (adaa ? kCost.adaaEa : 0) + kCost.update;
            else
                step = kCost.linear + kCost.linearPerState * states;
            total += rate * step;
        }
        if (plan.regionFirst >= 0 && oversample > 1)
            total += kCost.halfband * (2 * oversample - 2);   // stage s runs 2^s samples: 2 + 4 + ... + oversample
        return total;
    }

    std::vector<Candidate> candidates(const Plan& plan)
    {
        const Config& c = plan.config;
        std::vector<Candidate> out;
        if (!c.anyOpen())
            return out;
        const std::vector<int> factors =
            c.oversampleOpen ? std::vector<int>{1, 2, 4, 8} : std::vector<int>{c.oversample};
        const std::vector<bool> adaas = c.adaaOpen ? std::vector<bool>{false, true} : std::vector<bool>{c.adaa};
        const std::vector<Solver> solvers = c.solverOpen
                                                ? std::vector<Solver>{{Solver::Guess::Omega, 1}, {Solver::Guess::Omega, 2}}
                                                : std::vector<Solver>{c.solver};
        for (int os : factors)
        {
            for (bool adaa : adaas)
            {
                for (const Solver& s : solvers)
                    out.push_back({os, adaa, s, c.method, modelCost(plan, os, adaa, s)});
            }
        }
        orderCandidates(out);
        return out;
    }

    void orderCandidates(std::vector<Candidate>& out)
    {
        std::stable_sort(out.begin(), out.end(),
                         [](const Candidate& a, const Candidate& b) { return a.cost < b.cost; });
        // within 5 % of the cheapest of a group: ADAA first, then fewer Newton steps
        for (std::size_t i = 0; i < out.size();)
        {
            std::size_t j = i + 1;
            while (j < out.size() && out[j].cost <= 1.05 * out[i].cost)
                ++j;
            std::stable_sort(out.begin() + static_cast<long>(i), out.begin() + static_cast<long>(j),
                             [](const Candidate& a, const Candidate& b)
                             {
                                 if (a.adaa != b.adaa)
                                     return a.adaa;
                                 return a.solver.steps < b.solver.steps;
                             });
            i = j;
        }
    }

    // ---- the search -------------------------------------------------------------------------------------------------

    SearchOutcome search(const std::vector<Candidate>& candidates,
                         const std::function<std::optional<Verdict>(const Candidate&)>& measure,
                         const std::function<bool()>& cancelled, bool every)
    {
        SearchOutcome out;
        for (const Candidate& c : candidates)
        {
            if (cancelled && cancelled())
            {
                out.stopped = true;
                return out;
            }
            std::optional<Verdict> v = measure(c);
            if (!v)
            {
                out.stopped = true;
                return out;
            }
            out.tried.push_back({c, std::move(*v)});
            if (out.chosen < 0 && out.tried.back().verdict.passes())
            {
                out.chosen = static_cast<int>(out.tried.size()) - 1;
                out.met = true;
                if (!every)
                    return out;
            }
        }
        if (out.chosen < 0)
            out.chosen = noConfig(out.tried);
        return out;
    }

    int noConfig(std::span<const Tried> tried)
    {
        int best = -1;
        for (std::size_t i = 0; i < tried.size(); ++i)
        {
            const Tried& t = tried[i];
            if (t.candidate.cost > kCostCap || !t.verdict.finite || t.verdict.unconverged > 0)
                continue;
            if (best < 0)
            {
                best = static_cast<int>(i);
                continue;
            }
            const Tried& b = tried[static_cast<std::size_t>(best)];
            const double e = t.verdict.excess();
            const double eb = b.verdict.excess();
            if (e < eb || (e == eb && t.candidate.oversample > b.candidate.oversample))
                best = static_cast<int>(i);
        }
        return best;
    }

    Choice choiceOf(const std::string& key, const Candidate& candidate, const Verdict& verdict, bool met)
    {
        Choice c;
        c.key = key;
        c.oversample = candidate.oversample;
        c.adaa = candidate.adaa;
        c.solver = candidate.solver;
        c.method = candidate.method;
        c.met = met;
        c.t1 = verdict.t1.value;
        c.t2 = verdict.t2.value;
        c.t3 = verdict.t3.value;
        if (!met)
            c.worst = verdict.worst();
        return c;
    }

    std::string directivesOf(const Config& open, const Choice& choice)
    {
        std::string text;
        auto add = [&](const std::string& d)
        {
            text += (text.empty() ? "" : ", ") + d;
        };
        if (open.oversampleOpen)
            add(".oversample " + std::to_string(choice.oversample));
        if (open.adaaOpen)
            add(std::string(".adaa ") + (choice.adaa ? "on" : "off"));
        if (open.solverOpen)
            add(".solver " + solverText(choice.solver));
        if (open.methodOpen)
            add(std::string(".method ") + (choice.method == Method::Tr ? "tr" : "bdf2"));
        return text;
    }

    // ---- the budget -------------------------------------------------------------------------------------------------

    std::int64_t steps64(const StressMatrix& matrix)
    {
        std::int64_t n = 0;
        for (int u = 0; u < matrix.units(); ++u)
        {
            if (matrix.signal(u).t2)
                n += static_cast<std::int64_t>(matrix.shape.referenceFactor) *
                     static_cast<std::int64_t>(matrix.shape.host());
        }
        return n;
    }

    std::int64_t stepsT1(const StressMatrix& matrix, int oversample)
    {
        std::int64_t n = 0;
        for (int u = 0; u < matrix.units(); ++u)
        {
            if (matrix.signal(u).t1)
                n += static_cast<std::int64_t>(oversample) * static_cast<std::int64_t>(matrix.shape.host());
        }
        return n;
    }

    std::int64_t stepsCandidate(const StressMatrix& matrix, int oversample)
    {
        return static_cast<std::int64_t>(matrix.units()) * (matrix.upstream > 0 ? 3 : 2) * oversample *
               static_cast<std::int64_t>(matrix.shape.host());
    }
} // namespace fminus::circuit
