// src/circuit/Dc.cpp: the DC operating point (Dc.h).
#include "circuit/Dc.h"

#include "circuit/Laws.h"
#include "circuit/Mna.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <format>
#include <limits>
#include <map>
#include <numeric>

namespace fminus::circuit
{
    namespace
    {
        using Matrix = std::vector<std::vector<double>>;

        // What luSolve found: whether each right-hand side is consistent, and the null vectors of the free columns.
        struct Solved
        {
            std::vector<bool> ok;
            Matrix nulls;
        };

        // A x = b for each right-hand side in `bs` (columns, solved in place), by Gaussian elimination with partial
        // pivoting into row-echelon form, the columns taken in `order`. A column without a pivot above 1e-13 of the
        // matrix's largest entry leaves its unknown free: 0 in the solutions (the current two branches in parallel at
        // DC share, an output nothing at DC fixes), and its null vector (that unknown 1, the others what the equations
        // then make them) in `nulls`. A right-hand side is consistent when no column is free, or when every row's
        // residual is within 1e-9 of the size of its terms. Without a free column this is the elimination of an
        // invertible matrix, operation for operation.
        Solved luSolve(Matrix a, Matrix& bs, const std::vector<std::size_t>& order)
        {
            const Matrix a0 = a;
            const Matrix b0 = bs;
            const std::size_t n = a.size();
            double top = 0;
            for (const std::vector<double>& row : a)
            {
                for (double x : row)
                    top = std::max(top, std::fabs(x));
            }
            std::vector<std::size_t> pivotOf;   // each pivot row's column
            std::vector<std::size_t> free;
            std::vector<std::size_t> place(n, 0);   // each column's place in `order`
            for (std::size_t i = 0; i < order.size(); ++i)
                place[order[i]] = i;
            std::size_t row = 0;
            for (std::size_t k : order)
            {
                if (row == n)
                {
                    free.push_back(k);
                    continue;
                }
                std::size_t best = row;
                for (std::size_t r = row + 1; r < n; ++r)
                {
                    if (std::fabs(a[r][k]) > std::fabs(a[best][k]))
                        best = r;
                }
                if (!(std::fabs(a[best][k]) > 1e-13 * top))
                {
                    free.push_back(k);
                    continue;
                }
                std::swap(a[row], a[best]);
                for (std::vector<double>& b : bs)
                    std::swap(b[row], b[best]);
                for (std::size_t r = row + 1; r < n; ++r)
                {
                    const double m = a[r][k] / a[row][k];
                    if (m == 0)
                        continue;
                    for (std::size_t c = 0; c < n; ++c)
                        a[r][c] -= m * a[row][c];
                    for (std::vector<double>& b : bs)
                        b[r] -= m * b[row];
                }
                pivotOf.push_back(k);
                ++row;
            }
            // back substitution in the pivot rows, the free unknowns as `x` holds them (a pivot row reads the columns
            // after its own in `order`)
            auto back = [&](const std::vector<double>& b, std::vector<double>& x)
            {
                for (std::size_t i = pivotOf.size(); i-- > 0;)
                {
                    const std::size_t k = pivotOf[i];
                    double acc = b[i];
                    for (std::size_t j = place[k] + 1; j < order.size(); ++j)
                        acc -= a[i][order[j]] * x[order[j]];
                    x[k] = acc / a[i][k];
                }
            };
            Solved out;
            for (std::size_t c = 0; c < bs.size(); ++c)
            {
                std::vector<double> x(n, 0.0);
                back(bs[c], x);
                bool ok = true;
                for (std::size_t r = 0; r < n && !free.empty(); ++r)
                {
                    double residual = -b0[c][r];
                    double size = std::fabs(b0[c][r]);
                    for (std::size_t k = 0; k < n; ++k)
                    {
                        residual += a0[r][k] * x[k];
                        size += std::fabs(a0[r][k] * x[k]);
                    }
                    ok = ok && std::fabs(residual) <= 1e-9 * size;
                }
                out.ok.push_back(ok);
                bs[c] = x;
            }
            for (std::size_t f : free)
            {
                std::vector<double> x(n, 0.0);
                x[f] = 1;
                back(std::vector<double>(n, 0.0), x);
                out.nulls.push_back(x);
            }
            return out;
        }

        std::vector<std::size_t> naturalOrder(std::size_t n)
        {
            std::vector<std::size_t> order(n);
            std::iota(order.begin(), order.end(), std::size_t{0});
            return order;
        }

        // A block's DC equations at some pots: its matrix at rate 0 (gc = 0, rl = 0), its right-hand side with its
        // constants at their values and its input at the volts given, and its port's column (per unit of its v or i).
        struct Equations
        {
            System s;
            Matrix a;
            std::vector<double> rhs;
            std::vector<double> port;
            std::vector<double> values;   // each source's value (the port's 0)
            // the order of elimination when the matrix is singular: the pots' sections' currents last, so a 0-ohm
            // section in parallel with an inductor (or a V source) at DC is the one left carrying nothing, as it does
            // at every other position of its pot
            std::vector<std::size_t> order;
        };

        Equations equationsOf(const Plan& plan, int b, const std::vector<double>& pots, double input)
        {
            const Block& block = plan.blocks[static_cast<std::size_t>(b)];
            Equations eq;
            eq.s = assemble(plan, b, true);
            const System& s = eq.s;
            const auto n = static_cast<std::size_t>(s.n);
            const std::vector<double> symbols = symbolValues(s, plan, b, 0.0, pots);
            eq.a.assign(n, std::vector<double>(n, 0.0));
            for (std::size_t r = 0; r < n; ++r)
            {
                for (std::size_t c = 0; c < n; ++c)
                {
                    if (!s.a[r][c].isZero())
                        eq.a[r][c] = fromAffine(s.a[r][c], symbols);
                }
            }
            // each source's value: the constants', the input's DC (the block before's output; .input's is 0 V), the
            // states' 0 (at DC a capacitor's current and an inductor's voltage are 0), the port's 0
            eq.values.assign(s.sources.size(), 0.0);
            for (std::size_t k = 0; k < s.sources.size(); ++k)
            {
                if (s.sources[k].kind == System::Source::Kind::Constant)
                    eq.values[k] = s.sources[k].value;
            }
            eq.values[static_cast<std::size_t>(s.input)] = input;
            eq.rhs.assign(n, 0.0);
            eq.port.assign(n, 0.0);
            for (std::size_t k = 0; k < s.sources.size(); ++k)
            {
                for (std::size_t r = 0; r < n; ++r)
                {
                    if (s.rhs[k][r].isZero())
                        continue;
                    const double x = fromAffine(s.rhs[k][r], symbols);
                    eq.rhs[r] += x * eq.values[k];
                    if (block.port && static_cast<int>(k) == s.port)
                        eq.port[r] += x;
                }
            }
            // the unknowns: the nodes, the driven nodes, the branches' currents in branch order, the floating E
            // sources' and the port's
            std::size_t at = block.nodes.size() + block.driven.size();
            std::vector<std::size_t> last;
            for (const Branch& br : block.branches)
            {
                if (br.kind == Branch::Kind::Variable)
                    last.push_back(at++);
                else if (br.kind == Branch::Kind::Inductor || br.kind == Branch::Kind::Voltage)
                    ++at;
            }
            for (std::size_t k = 0; k < n; ++k)
            {
                if (std::find(last.begin(), last.end(), k) == last.end())
                    eq.order.push_back(k);
            }
            eq.order.insert(eq.order.end(), last.begin(), last.end());
            return eq;
        }

        // luSolve in the natural order, and when that leaves a column free, again in the equations' order.
        Solved solveIn(const Matrix& a, Matrix& bs, const std::vector<std::size_t>& order)
        {
            Matrix tried = bs;
            Solved solved = luSolve(a, tried, naturalOrder(a.size()));
            if (!solved.nulls.empty() && order != naturalOrder(a.size()))
            {
                tried = bs;
                solved = luSolve(a, tried, order);
            }
            bs = tried;
            return solved;
        }

        // Whether a linear form (coef[k] x[k]) is fixed: the same along every null vector, within 1e-9 of its terms.
        bool fixedAlong(const std::vector<std::pair<int, double>>& form, const Matrix& nulls)
        {
            for (const std::vector<double>& null : nulls)
            {
                double acc = 0;
                double size = 0;
                for (const auto& [k, coef] : form)
                {
                    acc += coef * null[static_cast<std::size_t>(k)];
                    size += std::fabs(coef * null[static_cast<std::size_t>(k)]);
                }
                if (!(std::fabs(acc) <= 1e-9 * size))
                    return false;
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
            for (const NodeInfo& n : plan.nodes)
            {
                if (n.source == NodeInfo::Source::Dc)
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

        // Whether `point` (solveDc's at the default pots) is a DC point at `pots` too: block by block, at the point's
        // unknowns, its input's volts and its port's source, every row of the equations at `pots` holds within 1e-9
        // of the size of its terms (and 1e-12 of the block's largest row's). So the module, which runs in offsets
        // from that point, is exact there, whichever point the equations there would pick when they leave one free.
        bool holds(const Plan& plan, const DcPoint& point, const std::vector<double>& pots)
        {
            for (std::size_t b = 0; b < plan.blocks.size(); ++b)
            {
                const Block& block = plan.blocks[b];
                const Equations eq = equationsOf(plan, static_cast<int>(b), pots,
                                                 point.nodes[static_cast<std::size_t>(block.input)]);
                const std::vector<double>& x = point.unknowns[b];
                const std::size_t n = x.size();
                std::vector<double> residual(n, 0.0);
                std::vector<double> size(n, 0.0);
                double top = 0;
                for (std::size_t r = 0; r < n; ++r)
                {
                    const double rhs = eq.rhs[r] + point.held[b] * eq.port[r];
                    residual[r] = -rhs;
                    size[r] = std::fabs(rhs);
                    for (std::size_t c = 0; c < n; ++c)
                    {
                        residual[r] += eq.a[r][c] * x[c];
                        size[r] += std::fabs(eq.a[r][c] * x[c]);
                    }
                    top = std::max(top, size[r]);
                }
                for (std::size_t r = 0; r < n; ++r)
                {
                    if (!(std::fabs(residual[r]) <= 1e-9 * size[r] + 1e-12 * top))
                        return false;
                }
            }
            return true;
        }
    } // namespace

    std::optional<DcPoint> solveDc(const Plan& plan, const std::vector<double>& pots, Messages& messages)
    {
        DcPoint point;
        point.nodes.assign(plan.nodes.size(), 0.0);
        for (std::size_t n = 0; n < plan.nodes.size(); ++n)
        {
            if (plan.nodes[n].source == NodeInfo::Source::Dc)
                point.nodes[n] = plan.nodes[n].value;
        }
        // each element's current from its first node to its second, where a block's unknowns hold it (a variable
        // resistor's or a pot section's, an inductor's, a V source's, a floating E's): (element, section) -> amps
        std::map<std::pair<int, int>, double> currents;
        for (std::size_t b = 0; b < plan.blocks.size(); ++b)
        {
            const Block& block = plan.blocks[b];
            const Equations eq =
                equationsOf(plan, static_cast<int>(b), pots, point.nodes[static_cast<std::size_t>(block.input)]);
            const System& s = eq.s;
            const auto n = static_cast<std::size_t>(s.n);
            auto undefined = [&]
            {
                Loc loc;
                if (!block.elements.empty())
                    loc = plan.netlist.elements[static_cast<std::size_t>(block.elements.front())].loc;
                messages.error(loc, "block " + std::to_string(block.index) + " (" + partsText(plan, block) +
                                        "): its DC operating point is undefined: its equations are singular at DC "
                                        "(an op amp with only capacitors around it, or a node only capacitors reach)");
            };
            // no DC source reaches it: all 0
            const bool driven = std::any_of(eq.rhs.begin(), eq.rhs.end(), [](double x) { return x != 0; });
            std::vector<double> z(n, 0.0);
            double held = 0;      // the port's source at the point: the v-form's v
            double p0 = 0;        // the port's p at DC, v + kd f(v) = p0
            double k = 0;         // and its kd there, when it has one
            double through = 0;   // or, when nothing at DC moves its current, that current
            bool found = !driven;
            if (driven && block.port && !s.iForm)
            {
                // a diode's port open (j = 0: its row and j's column dropped): p0 is the voltage across it, V(anode) -
                // V(cathode), and kd the resistance the network shows it, so an inductor or a 0-ohm end across it at
                // DC leaves nothing free; when the open network leaves that voltage free (the port a node's only
                // DC path), the v-form below
                const System::Readout& ro = s.readouts[static_cast<std::size_t>(s.portCurrent)];
                const auto j = static_cast<std::size_t>(ro.unknowns.front().first);
                std::size_t row = n;
                for (std::size_t r = 0; r < n; ++r)
                {
                    if (!s.rhs[static_cast<std::size_t>(s.port)][r].isZero())
                        row = r;
                }
                std::vector<std::size_t> keep;   // the open network's unknowns, and its rows
                for (std::size_t c = 0; c < n; ++c)
                {
                    if (c != j)
                        keep.push_back(c);
                }
                std::vector<std::size_t> rows;
                for (std::size_t r = 0; r < n; ++r)
                {
                    if (r != row)
                        rows.push_back(r);
                }
                const std::size_t m = n - 1;
                Matrix open(m, std::vector<double>(m, 0.0));
                Matrix cols(2, std::vector<double>(m, 0.0));
                for (std::size_t r = 0; r < m; ++r)
                {
                    for (std::size_t c = 0; c < m; ++c)
                        open[r][c] = eq.a[rows[r]][keep[c]];
                    cols[0][r] = eq.rhs[rows[r]];
                    cols[1][r] = -eq.a[rows[r]][j];   // per amp of j, moved to the right
                }
                std::vector<std::size_t> order;
                for (std::size_t c : eq.order)
                {
                    if (c != j)
                        order.push_back(c > j ? c - 1 : c);
                }
                const Solved solved = solveIn(open, cols, order);
                std::vector<std::pair<int, double>> across;   // the port's row on the open network's unknowns
                for (std::size_t c = 0; c < m; ++c)
                {
                    if (row < n && eq.a[row][keep[c]] != 0)
                        across.emplace_back(static_cast<int>(c), eq.a[row][keep[c]]);
                }
                auto voltage = [&](const std::vector<double>& x)
                {
                    double acc = 0;
                    for (const auto& [c, coef] : across)
                        acc += coef * x[static_cast<std::size_t>(c)];
                    return acc;
                };
                if (row < n && solved.ok[0] && fixedAlong(across, solved.nulls))
                {
                    p0 = voltage(cols[0]) - eq.rhs[row];
                    k = solved.ok[1] ? -voltage(cols[1]) : std::numeric_limits<double>::infinity();
                    for (std::size_t c = 0; c < m; ++c)
                        z[keep[c]] = cols[0][c];
                    held = p0;
                    found = true;
                }
            }
            if (!found)
            {
                // the whole system: a diode's port shorted (its v = 0), an OTA's open (its i = 0)
                Matrix cols{eq.rhs};
                if (block.port)
                    cols.push_back(eq.port);
                const Solved solved = solveIn(eq.a, cols, eq.order);
                if (!solved.ok[0])
                {
                    undefined();
                    return std::nullopt;
                }
                z = cols[0];
                if (block.port)
                {
                    // the port's readout at its source's 0 and per unit of it: v-form j = r0 + r1 v, K = -1 / r1, p0 =
                    // K r0; i-form v = r0 + r1 i, p0 = r0, kd = -r1. It must be fixed, and its source free to move.
                    const System::Readout& ro = s.readouts[static_cast<std::size_t>(s.portCurrent)];
                    if (!solved.ok[1] || !fixedAlong(ro.unknowns, solved.nulls))
                    {
                        undefined();
                        return std::nullopt;
                    }
                    auto read = [&](const std::vector<double>& x, bool unit)
                    {
                        double acc = 0;
                        for (const auto& [u, coef] : ro.unknowns)
                            acc += coef * x[static_cast<std::size_t>(u)];
                        for (const auto& [src, coef] : ro.sources)
                        {
                            const auto at = static_cast<std::size_t>(src);
                            acc += coef * (unit ? (src == s.port ? 1.0 : 0.0) : eq.values[at]);
                        }
                        return acc;
                    };
                    const double r0 = read(cols[0], false);
                    const double r1 = read(cols[1], true);
                    if (s.iForm)
                    {
                        p0 = r0;
                        k = -r1;
                    }
                    else if (r1 == 0)
                        through = r0;   // nothing at DC moves the port's current: the network sets it
                    else
                    {
                        p0 = r0 == 0 ? 0.0 : -r0 / r1;
                        k = -1 / r1;
                    }
                }
            }
            if (block.port)
            {
                // a port with a DC voltage across it, or a DC current through it, is biased: its voltage, the law
                // solved at the default pots and law inputs' low ends, for the message
                double v = 0;
                if (std::fabs(p0) > 1e-9 || through != 0)
                {
                    std::string figure;
                    if (through != 0)
                        figure = std::format("{:.3g} A through it", through);
                    else
                    {
                        Holters h;
                        h.law = block.port->law;
                        h.k = block.port->smooth()                       ? k
                              : block.port->law == PortDef::Law::Pair ? 2 * block.port->is * k
                                                                      : block.port->is * k;
                        h.nvt = block.port->nvt;
                        h.k2 = block.port->is2 * k;
                        h.nvt2 = block.port->nvt2;
                        const double g = block.port->smooth() ? gainAt(plan, *block.port, pots, {}) : 1;
                        v = k > 0 && std::isfinite(k) ? lawConvergedOf(lawShape(h, g), p0, 0, nullptr, nullptr) : p0;
                        figure = std::format("{:.3g} V", v);
                    }
                    if (point.biased.empty())
                    {
                        point.biased = std::format("port {} is biased at DC ({})", block.port->parts, figure);
                        point.biasedElement = block.port->elements.front();
                    }
                }
                point.ports.emplace_back(block.port->parts, v);
            }
            // the nodes, then the driven nodes, then the currents in branch order and the floating E sources'
            std::size_t at = 0;
            for (int node : block.nodes)
                point.nodes[static_cast<std::size_t>(node)] = z[at++];
            for (int node : block.driven)
                point.nodes[static_cast<std::size_t>(node)] = z[at++];
            for (const Branch& br : block.branches)
            {
                if (br.kind == Branch::Kind::Variable || br.kind == Branch::Kind::Inductor ||
                    br.kind == Branch::Kind::Voltage)
                    currents[{br.element, br.section}] = z[at++];
            }
            for (const Controlled& c : block.controlled)
            {
                if (c.kind == Controlled::Kind::Vcvs && !c.driven)
                    currents[{c.element, 0}] = z[at++];
            }
            point.unknowns.push_back(z);
            point.held.push_back(held);
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
            // no pot may move it: the offsets are from one DC point, which must stay a DC point at every position
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
                    if (holds(plan, *point, at))
                        continue;
                    // what the point there is, for the message
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
                    std::string moved;
                    for (std::size_t b = 0; b < other->unknowns.size() && moved.empty(); ++b)
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
                            moved = std::format(" ({} is {:.6g} at {} {:g}, {:.6g} at {:g})", s.unknownNames[k],
                                                u0[k] + 0.0, pot.name, pot.defaultValue, u1[k] + 0.0, x);
                            break;
                        }
                    }
                    messages.error(loc, std::format("pot '{}' moves the DC operating point{}: the module runs in "
                                                    "offsets from one DC point (one a pot moves is Phase 2's)",
                                                    pot.name, moved));
                    return false;
                }
            }
            // nor a switch: each of its positions, the rest at their defaults
            for (std::size_t i = 0; i < plan.switches.size(); ++i)
            {
                const SwitchInfo& w = plan.switches[i];
                if (!w.used)
                    continue;
                for (int position = 0; position < w.positions; ++position)
                {
                    if (position == w.defaultPosition)
                        continue;
                    std::vector<double> at = defaultPoint(plan);
                    at[plan.pots.size() + i] = position;
                    if (holds(plan, *point, at))
                        continue;
                    messages.error(w.loc, std::format("switch '{}' at position {} moves the DC operating point: the "
                                                      "module runs in offsets from one DC point (one a switch moves "
                                                      "is Phase 2's)",
                                                      w.name, position));
                    return false;
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
