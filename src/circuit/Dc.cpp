// src/circuit/Dc.cpp: the DC operating point (Dc.h).
#include "circuit/Dc.h"

#include "circuit/Laws.h"
#include "circuit/Mna.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <format>
#include <map>

namespace fminus::circuit
{
    namespace
    {
        // A x = b for each right-hand side in `bs` (columns), by Gaussian elimination with partial pivoting. False when
        // a pivot is below 1e-13 of the matrix's largest entry (singular).
        bool luSolve(std::vector<std::vector<double>> a, std::vector<std::vector<double>>& bs)
        {
            const std::size_t n = a.size();
            double top = 0;
            for (const std::vector<double>& row : a)
            {
                for (double x : row)
                    top = std::max(top, std::fabs(x));
            }
            for (std::size_t k = 0; k < n; ++k)
            {
                std::size_t best = k;
                for (std::size_t r = k + 1; r < n; ++r)
                {
                    if (std::fabs(a[r][k]) > std::fabs(a[best][k]))
                        best = r;
                }
                if (!(std::fabs(a[best][k]) > 1e-13 * top))
                    return false;
                std::swap(a[k], a[best]);
                for (std::vector<double>& b : bs)
                    std::swap(b[k], b[best]);
                for (std::size_t r = k + 1; r < n; ++r)
                {
                    const double m = a[r][k] / a[k][k];
                    if (m == 0)
                        continue;
                    for (std::size_t c = k; c < n; ++c)
                        a[r][c] -= m * a[k][c];
                    for (std::vector<double>& b : bs)
                        b[r] -= m * b[k];
                }
            }
            for (std::vector<double>& b : bs)
            {
                for (std::size_t k = n; k-- > 0;)
                {
                    double acc = b[k];
                    for (std::size_t c = k + 1; c < n; ++c)
                        acc -= a[k][c] * b[c];
                    b[k] = acc / a[k][k];
                }
            }
            return true;
        }

        std::string partsText(const Plan& plan, const Block& block)
        {
            std::string out;
            for (int e : block.elements)
                out += (out.empty() ? "" : ", ") + plan.netlist.elements[static_cast<std::size_t>(e)].name;
            return out;
        }

        // A value for the notes: 9 significant digits, and 0 for what is double's rounding of 0.
        std::string figure(double value, double tiny, const char* unit)
        {
            if (std::fabs(value) < tiny)
                value = 0;
            struct Prefix
            {
                double scale;
                const char* name;
            };
            static constexpr std::array<Prefix, 7> kPrefixes{
                {{1e-15, "f"}, {1e-12, "p"}, {1e-9, "n"}, {1e-6, "u"}, {1e-3, "m"}, {1, ""}, {1e3, "k"}}};
            Prefix chosen{1, ""};
            if (value != 0)
            {
                chosen = kPrefixes[0];
                for (const Prefix& p : kPrefixes)
                {
                    if (std::fabs(value) >= p.scale * 0.9999999)
                        chosen = p;
                }
            }
            return std::format("{:.9g} {}{}", value / chosen.scale, chosen.name, unit);
        }

        // Whether a DC source drives the netlist (without one, every DC value is 0).
        bool hasSources(const Plan& plan)
        {
            if (!plan.currents.empty())
                return true;
            for (const Node& n : plan.nodes)
            {
                if (n.source == Node::Source::Dc)
                    return true;
            }
            for (const Block& b : plan.blocks)
            {
                for (const Branch& br : b.branches)
                {
                    if (br.kind == Branch::Kind::Voltage)
                        return true;
                }
            }
            return false;
        }
    } // namespace

    std::optional<DcPoint> solveDc(const Plan& plan, const std::vector<double>& pots, Messages& messages)
    {
        DcPoint point;
        point.nodes.assign(plan.nodes.size(), 0.0);
        for (std::size_t n = 0; n < plan.nodes.size(); ++n)
        {
            if (plan.nodes[n].source == Node::Source::Dc)
                point.nodes[n] = plan.nodes[n].value;
        }
        // each element's current from its first node to its second, where a block's unknowns hold it (a variable
        // resistor's or a pot section's, an inductor's, a V source's, a floating E's): (element, section) -> amps
        std::map<std::pair<int, int>, double> currents;
        for (std::size_t b = 0; b < plan.blocks.size(); ++b)
        {
            const Block& block = plan.blocks[b];
            const System s = assemble(plan, static_cast<int>(b), true);
            const auto n = static_cast<std::size_t>(s.n);
            const std::vector<double> symbols = symbolValues(s, plan, static_cast<int>(b), 0.0, pots);
            std::vector<std::vector<double>> a(n, std::vector<double>(n, 0.0));
            for (std::size_t r = 0; r < n; ++r)
            {
                for (std::size_t c = 0; c < n; ++c)
                {
                    if (!s.a[r][c].isZero())
                        a[r][c] = fromAffine(s.a[r][c], symbols);
                }
            }
            // each source's value: the constants', the input's DC (the block before's output; .input's is 0 V), the
            // states' 0 (at DC a capacitor's current and an inductor's voltage are 0), the port's 0
            std::vector<double> value(s.sources.size(), 0.0);
            for (std::size_t k = 0; k < s.sources.size(); ++k)
            {
                if (s.sources[k].kind == System::Source::Kind::Constant)
                    value[k] = s.sources[k].value;
            }
            value[static_cast<std::size_t>(s.input)] = point.nodes[static_cast<std::size_t>(block.input)];
            std::vector<std::vector<double>> rhs(block.port ? 2 : 1, std::vector<double>(n, 0.0));
            bool driven = false;
            for (std::size_t k = 0; k < s.sources.size(); ++k)
            {
                for (std::size_t r = 0; r < n; ++r)
                {
                    if (s.rhs[k][r].isZero())
                        continue;
                    const double x = fromAffine(s.rhs[k][r], symbols);
                    rhs[0][r] += x * value[k];
                    if (block.port && static_cast<int>(k) == s.port)
                        rhs[1][r] += x;
                }
            }
            driven = std::any_of(rhs[0].begin(), rhs[0].end(), [](double x) { return x != 0; });
            std::vector<std::vector<double>> z = rhs;
            if (driven && !luSolve(a, z))
            {
                Loc loc;
                if (!block.elements.empty())
                    loc = plan.netlist.elements[static_cast<std::size_t>(block.elements.front())].loc;
                messages.error(loc, "block " + std::to_string(block.index) + " (" + partsText(plan, block) +
                                        "): its DC operating point is undefined: its equations are singular at DC "
                                        "(an op amp with only capacitors around it, or a node only capacitors reach)");
                return std::nullopt;
            }
            if (!driven)
                z.assign(rhs.size(), std::vector<double>(n, 0.0));   // no DC source reaches it: all 0
            // the port's open-circuit value at DC: v + kd f(v) = p0, from its readout at the port's 0 and per unit
            if (block.port)
            {
                const System::Readout& ro = s.readouts[static_cast<std::size_t>(s.portCurrent)];
                auto read = [&](const std::vector<double>& x, bool unit)
                {
                    double acc = 0;
                    for (const auto& [u, coef] : ro.unknowns)
                        acc += coef * x[static_cast<std::size_t>(u)];
                    for (const auto& [src, coef] : ro.sources)
                        acc += coef * (unit ? (src == s.port ? 1.0 : 0.0) : value[static_cast<std::size_t>(src)]);
                    return acc;
                };
                const double r0 = read(z[0], false);
                const double r1 = driven ? read(z[1], true) : 0;
                // v-form: j = r0 + r1 v, K = -1 / r1, p0 = K r0; i-form: v = r0 + r1 i, p0 = r0
                const double p0 = s.iForm ? r0 : (r0 == 0 ? 0.0 : -r0 / r1);
                double v = 0;
                if (std::fabs(p0) > 1e-9)
                {
                    // its voltage at DC, for the message: the law solved, at the default pots and law inputs' low ends
                    Holters h;
                    h.law = block.port->law;
                    const double k = s.iForm ? -r1 : -1 / r1;
                    h.k = block.port->smooth()                       ? k
                          : block.port->law == PortDef::Law::Pair ? 2 * block.port->is * k
                                                                  : block.port->is * k;
                    h.nvt = block.port->nvt;
                    h.k2 = block.port->is2 * k;
                    h.nvt2 = block.port->nvt2;
                    const double g = block.port->smooth() ? gainAt(plan, *block.port, pots, {}) : 1;
                    v = k > 0 && std::isfinite(k) ? lawConvergedOf(lawShape(h, g), p0, 0, nullptr, nullptr) : p0;
                    if (point.biased.empty())
                    {
                        point.biased = std::format("port {} is biased at DC ({:.3g} V)", block.port->parts, v);
                        point.biasedElement = block.port->elements.front();
                    }
                }
                point.ports.emplace_back(block.port->parts, v);
            }
            // the nodes, then the driven nodes, then the currents in branch order and the floating E sources'
            std::size_t at = 0;
            for (int node : block.nodes)
                point.nodes[static_cast<std::size_t>(node)] = z[0][at++];
            for (int node : block.driven)
                point.nodes[static_cast<std::size_t>(node)] = z[0][at++];
            for (const Branch& br : block.branches)
            {
                if (br.kind == Branch::Kind::Variable || br.kind == Branch::Kind::Inductor ||
                    br.kind == Branch::Kind::Voltage)
                    currents[{br.element, br.section}] = z[0][at++];
            }
            for (const Controlled& c : block.controlled)
            {
                if (c.kind == Controlled::Kind::Vcvs && !c.driven)
                    currents[{c.element, 0}] = z[0][at++];
            }
            point.unknowns.push_back(z[0]);
        }
        // the V sources' currents: a floating one's its unknown; a grounded one's what its node's other elements draw
        const std::vector<Element>& elements = plan.netlist.elements;
        auto nodeOf = [&](const std::string& name)
        {
            const std::string key = folded(name) == "gnd" ? "0" : folded(name);
            for (std::size_t n = 0; n < plan.nodes.size(); ++n)
            {
                if (folded(plan.nodes[n].name) == key)
                    return static_cast<int>(n);
            }
            return 0;
        };
        auto volts = [&](const std::string& name)
        {
            return point.nodes[static_cast<std::size_t>(nodeOf(name))];
        };
        // the current an element draws from `node` (leaving it into the element)
        auto drawn = [&](std::size_t e, int node) -> double
        {
            const Element& el = elements[e];
            std::vector<int> nn;
            for (const std::string& name : el.nodes)
                nn.push_back(nodeOf(name));
            const double sign = nn[0] == node ? 1 : nn.size() > 1 && nn[1] == node ? -1 : 0;
            auto stored = [&](int section)
            {
                const auto it = currents.find({static_cast<int>(e), section});
                return it == currents.end() ? 0.0 : it->second;
            };
            switch (el.kind)
            {
            case Element::Kind::Resistor:
                return el.expr ? sign * stored(0) : sign * (volts(el.nodes[0]) - volts(el.nodes[1])) / el.value;
            case Element::Kind::Pot:
                if (nn.size() == 2)
                    return sign * stored(2);
                // e1 -> w (section 1), w -> e2 (section 2)
                return (nn[0] == node ? stored(1) : 0) - (nn[1] == node ? stored(1) : 0) +
                       (nn[1] == node ? stored(2) : 0) - (nn[2] == node ? stored(2) : 0);
            case Element::Kind::Inductor:
            case Element::Kind::Voltage:
                return sign * stored(0);
            case Element::Kind::Current:
                return sign * el.value;
            case Element::Kind::Vccs:
            {
                const double g =
                    el.expr ? evaluate(*el.expr, [&](const std::string& key) { return potValue(plan, pots, key); })
                            : el.value;
                return sign * g * (volts(el.nodes[2]) - volts(el.nodes[3]));
            }
            case Element::Kind::Vcvs:
                return sign * stored(0);
            default:
                return 0;   // a capacitor, an unbiased diode or OTA, an op amp's input
            }
        };
        for (std::size_t e = 0; e < elements.size(); ++e)
        {
            const Element& el = elements[e];
            if (el.kind != Element::Kind::Voltage)
                continue;
            const int a = nodeOf(el.nodes[0]);
            const int b = nodeOf(el.nodes[1]);
            if (a != 0 && b != 0)
            {
                const auto it = currents.find({static_cast<int>(e), 0});
                point.sources.emplace_back(el.name, it == currents.end() ? 0.0 : it->second);
                continue;
            }
            const int node = a == 0 ? b : a;
            double supplied = 0;   // into `node` from the source: what the other elements draw from it
            for (std::size_t k = 0; k < elements.size(); ++k)
            {
                if (k != e)
                    supplied += drawn(k, node);
            }
            // i(V) flows from n+ through the source to n-: out of n- into its node, or out of its node into n+
            point.sources.emplace_back(el.name, a == 0 ? supplied : -supplied);
        }
        return point;
    }

    bool dcPoint(Plan& plan, Messages& messages)
    {
        std::vector<double> defaults;
        for (const PotInfo& p : plan.pots)
            defaults.push_back(p.defaultValue);
        const bool sources = hasSources(plan);
        std::optional<DcPoint> point;
        if (sources)
        {
            point = solveDc(plan, defaults, messages);
            if (!point)
                return false;
            if (!point->biased.empty())
            {
                messages.error(plan.netlist.elements[static_cast<std::size_t>(point->biasedElement)].loc,
                               point->biased + ": biased ports need the offset formulation (Phase 2)");
                return false;
            }
            // no pot may move it: the offsets are from one DC point
            for (std::size_t i = 0; i < plan.pots.size(); ++i)
            {
                const PotInfo& pot = plan.pots[i];
                if (!pot.used)
                    continue;
                Loc loc;
                for (const PotLine& line : plan.netlist.pots)
                {
                    if (folded(line.name) == pot.key)
                        loc = line.loc;
                }
                for (double x : {0.0, 0.5, 1.0})
                {
                    if (x == pot.defaultValue)
                        continue;
                    std::vector<double> at = defaults;
                    at[i] = x;
                    Messages scratch;
                    const std::optional<DcPoint> other = solveDc(plan, at, scratch);
                    if (!other)
                    {
                        messages.error(loc, std::format("pot '{}' at {:g} leaves the DC operating point undefined (a "
                                                        "block singular at DC)",
                                                        pot.name, x));
                        return false;
                    }
                    if (!other->biased.empty())
                    {
                        messages.error(loc, std::format("pot '{}' at {:g} biases a port: {}: biased ports need the "
                                                        "offset formulation (Phase 2)",
                                                        pot.name, x, other->biased));
                        return false;
                    }
                    // the first value that moves (a node's voltage, or a current)
                    for (std::size_t b = 0; b < other->unknowns.size(); ++b)
                    {
                        const std::vector<double>& u0 = point->unknowns[b];
                        const std::vector<double>& u1 = other->unknowns[b];
                        double top = 0;
                        for (std::size_t k = 0; k < u0.size(); ++k)
                            top = std::max({top, std::fabs(u0[k]), std::fabs(u1[k])});
                        for (std::size_t k = 0; k < u0.size(); ++k)
                        {
                            if (std::fabs(u0[k] - u1[k]) <= 1e-9 * std::max(std::fabs(u0[k]), std::fabs(u1[k])) +
                                                                 1e-12 * top)
                                continue;
                            const System s = assemble(plan, static_cast<int>(b), true);
                            messages.error(loc, std::format("pot '{}' moves the DC operating point ({} is {:.6g} at "
                                                            "{} {:g}, {:.6g} at {:g}): the module runs in offsets from "
                                                            "one DC point (one a pot moves is Phase 2's)",
                                                            pot.name, s.unknownNames[k], u0[k] + 0.0, pot.name,
                                                            pot.defaultValue, u1[k] + 0.0, x));
                            return false;
                        }
                    }
                }
            }
            plan.dc = point->nodes;
        }
        else
            plan.dc.assign(plan.nodes.size(), 0.0);
        plan.outputDc = plan.dc[static_cast<std::size_t>(plan.outputNode)] / plan.outputScale;
        double top = 0;
        for (double v : plan.dc)
            top = std::max(top, std::fabs(v));
        const double tiny = 1e-12 * std::max(top, 1.0);
        if (std::fabs(plan.outputDc * plan.outputScale) < tiny)
            plan.outputDc = 0;
        if (plan.outputDc != 0)
            messages.note({}, "the output's DC operating point, V(" +
                                  plan.nodes[static_cast<std::size_t>(plan.outputNode)].name + ") = " +
                                  figure(plan.outputDc * plan.outputScale, tiny, "V") +
                                  ", is tick's constant: the module runs in offsets from the DC point");
        if (plan.netlist.op.line > 0)
        {
            // .op: each node's voltage, each V source's current, each port's voltage
            for (std::size_t n = 1; n < plan.nodes.size(); ++n)
            {
                if (plan.nodes[n].absorbed)
                    continue;
                messages.note({}, ".op: V(" + plan.nodes[n].name + ") = " + figure(plan.dc[n], tiny, "V"));
            }
            if (point)
            {
                double topI = 0;
                for (const auto& [name, i] : point->sources)
                    topI = std::max(topI, std::fabs(i));
                for (const auto& [name, i] : point->sources)
                    messages.note({}, ".op: i(" + name + ") = " + figure(i, 1e-12 * std::max(topI, 1e-3), "A"));
                for (const auto& [parts, v] : point->ports)
                    messages.note({}, ".op: port " + parts + ": v = " + figure(v, tiny, "V"));
            }
            else
            {
                for (const Element& el : plan.netlist.elements)
                {
                    if (el.kind == Element::Kind::Voltage)
                        messages.note({}, ".op: i(" + el.name + ") = 0 A");
                }
                for (const Block& b : plan.blocks)
                {
                    if (b.port)
                        messages.note({}, ".op: port " + b.port->parts + ": v = 0 V");
                }
            }
        }
        return true;
    }
} // namespace fminus::circuit
