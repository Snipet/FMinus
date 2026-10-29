// src/circuit/Mna.cpp: a block's stamps (assemble), its elimination order (choosePivots), and the derivation run on
// numbers: the Holters form the reference simulates, and a linear block's analog response with its 1x check.
#include "circuit/Mna.h"

#include "circuit/Emit.h"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <format>
#include <limits>
#include <map>
#include <numbers>
#include <stdexcept>

namespace fminus::circuit
{
    namespace
    {
        // Where a node stands in a block's equations.
        struct Term
        {
            enum class Kind
            {
                Ground,
                Unknown,
                Source,
            };

            Kind kind = Kind::Ground;
            int index = -1;
            double scale = 1;   // a source's: .input's scale
        };
    } // namespace

    System assemble(const Plan& plan, int blockIndex)
    {
        const Block& block = plan.blocks.at(static_cast<std::size_t>(blockIndex));
        System s;
        std::map<int, int> unknownOf;   // node -> unknown
        std::map<int, int> kclRow;      // node -> its KCL row
        auto nodeName = [&](int node)
        {
            return plan.nodes[static_cast<std::size_t>(node)].name;
        };
        for (int node : block.nodes)
        {
            unknownOf[node] = s.n++;
            s.unknownNames.push_back("V(" + nodeName(node) + ")");
        }
        for (int node : block.driven)
        {
            unknownOf[node] = s.n++;
            s.unknownNames.push_back("V(" + nodeName(node) + ")");
        }
        std::vector<int> branchCurrent(block.branches.size(), -1);
        for (std::size_t b = 0; b < block.branches.size(); ++b)
        {
            if (block.branches[b].kind == Branch::Kind::Variable)
            {
                branchCurrent[b] = s.n++;
                s.unknownNames.push_back("i(" + block.branches[b].ident + ")");
            }
        }
        int portCurrent = -1;
        if (block.port)
        {
            portCurrent = s.n++;
            s.unknownNames.push_back("j(" + block.port->parts + ")");
        }
        const auto un = static_cast<std::size_t>(s.n);
        s.a.assign(un, std::vector<Affine>(un));

        // Rows: KCL of the solved nodes, the nullors, the variable branches, the port.
        int row = 0;
        for (int node : block.nodes)
        {
            kclRow[node] = row++;
            s.rowNames.push_back("KCL " + nodeName(node));
        }
        std::vector<int> nullorRow;
        for (const OpAmp& op : block.opamps)
        {
            nullorRow.push_back(row++);
            s.rowNames.push_back(op.name);
        }
        std::vector<int> variableRow(block.branches.size(), -1);
        for (std::size_t b = 0; b < block.branches.size(); ++b)
        {
            if (block.branches[b].kind == Branch::Kind::Variable)
            {
                variableRow[b] = row++;
                s.rowNames.push_back(block.branches[b].ident);
            }
        }
        int portRow = -1;
        if (block.port)
        {
            portRow = row++;
            s.rowNames.push_back(block.port->parts);
        }
        if (row != s.n)
            throw std::logic_error("assemble: rows and unknowns differ");

        // Sources: the input, each capacitor's state, the port's v.
        const bool fromInput = block.input == plan.inputNode;
        s.sources.push_back(
            {fromInput ? plan.inputName : "n_" + plan.nodes[static_cast<std::size_t>(block.input)].ident,
             System::Source::Kind::Input, -1});
        s.input = 0;
        std::vector<int> stateOf(block.branches.size(), -1);
        for (std::size_t b = 0; b < block.branches.size(); ++b)
        {
            if (block.branches[b].kind == Branch::Kind::Capacitor)
            {
                stateOf[b] = static_cast<int>(s.sources.size());
                s.sources.push_back({"x_" + block.branches[b].ident, System::Source::Kind::State, static_cast<int>(b)});
            }
        }
        if (block.port)
        {
            s.port = static_cast<int>(s.sources.size());
            s.sources.push_back({"v_" + block.port->name, System::Source::Kind::Port, -1});
        }
        s.rhs.assign(s.sources.size(), std::vector<Affine>(un));

        // Symbols: each capacitor's gc, each variable branch's r.
        std::vector<int> symbolOf(block.branches.size(), -1);
        for (std::size_t b = 0; b < block.branches.size(); ++b)
        {
            const Branch& br = block.branches[b];
            if (br.kind == Branch::Kind::Capacitor)
            {
                symbolOf[b] = static_cast<int>(s.symbols.size());
                s.symbols.push_back({"gc_" + br.ident, System::Symbol::Kind::Gc, static_cast<int>(b)});
            }
            else if (br.kind == Branch::Kind::Variable)
            {
                symbolOf[b] = static_cast<int>(s.symbols.size());
                s.symbols.push_back({"r_" + br.ident, System::Symbol::Kind::R, static_cast<int>(b)});
            }
        }

        auto term = [&](int node)
        {
            Term t;
            if (node == 0)
                return t;
            if (const auto it = unknownOf.find(node); it != unknownOf.end())
            {
                t.kind = Term::Kind::Unknown;
                t.index = it->second;
                return t;
            }
            if (node == block.input)
            {
                t.kind = Term::Kind::Source;
                t.index = s.input;
                t.scale = fromInput ? plan.inputScale : 1.0;
                return t;
            }
            throw std::logic_error("assemble: block " + std::to_string(block.index) + " reads node '" + nodeName(node) +
                                   "', which is not its input");
        };
        auto addTo = [](Affine& target, const Affine& y, double factor)
        {
            target.add(y.constant * factor);
            for (const auto& [sym, coef] : y.terms)
                target.add(sym, coef * factor);
        };
        // An admittance y between p and q: y (Vp - Vq) leaves p, and the reverse leaves q.
        auto admittance = [&](int p, int q, const Affine& y)
        {
            for (int side = 0; side < 2; ++side)
            {
                const int here = side == 0 ? p : q;
                const int there = side == 0 ? q : p;
                const auto it = kclRow.find(here);
                if (it == kclRow.end())
                    continue;
                const auto r = static_cast<std::size_t>(it->second);
                addTo(s.a[r][static_cast<std::size_t>(unknownOf.at(here))], y, 1);
                const Term t = term(there);
                if (t.kind == Term::Kind::Unknown)
                    addTo(s.a[r][static_cast<std::size_t>(t.index)], y, -1);
                else if (t.kind == Term::Kind::Source)
                    addTo(s.rhs[static_cast<std::size_t>(t.index)][r], y, t.scale);
            }
        };
        // +-1 times a node's voltage in row r's left-hand side (moved to the right-hand side for a source).
        auto voltage = [&](int r, int node, double sign)
        {
            const Term t = term(node);
            if (t.kind == Term::Kind::Unknown)
                s.a[static_cast<std::size_t>(r)][static_cast<std::size_t>(t.index)].add(sign);
            else if (t.kind == Term::Kind::Source)
                s.rhs[static_cast<std::size_t>(t.index)][static_cast<std::size_t>(r)].add(-sign * t.scale);
        };
        // A current unknown leaving `from` and entering `to`.
        auto current = [&](int from, int to, int unknown)
        {
            if (const auto it = kclRow.find(from); it != kclRow.end())
                s.a[static_cast<std::size_t>(it->second)][static_cast<std::size_t>(unknown)].add(1);
            if (const auto it = kclRow.find(to); it != kclRow.end())
                s.a[static_cast<std::size_t>(it->second)][static_cast<std::size_t>(unknown)].add(-1);
        };

        for (std::size_t b = 0; b < block.branches.size(); ++b)
        {
            const Branch& br = block.branches[b];
            switch (br.kind)
            {
            case Branch::Kind::Conductance:
            {
                Affine y;
                y.add(br.g);
                admittance(br.a, br.b, y);
                break;
            }
            case Branch::Kind::Capacitor:
            {
                Affine y;
                y.add(symbolOf[b], 1);
                admittance(br.a, br.b, y);
                // the history: x injected into a, drawn from b
                const auto x = static_cast<std::size_t>(stateOf[b]);
                if (const auto it = kclRow.find(br.a); it != kclRow.end())
                    s.rhs[x][static_cast<std::size_t>(it->second)].add(1);
                if (const auto it = kclRow.find(br.b); it != kclRow.end())
                    s.rhs[x][static_cast<std::size_t>(it->second)].add(-1);
                break;
            }
            case Branch::Kind::Variable:
            {
                current(br.a, br.b, branchCurrent[b]);
                const int r = variableRow[b];
                voltage(r, br.a, 1);
                voltage(r, br.b, -1);
                s.a[static_cast<std::size_t>(r)][static_cast<std::size_t>(branchCurrent[b])].add(symbolOf[b], -1);
                break;
            }
            }
        }
        for (std::size_t k = 0; k < block.opamps.size(); ++k)
        {
            voltage(nullorRow[k], block.opamps[k].inp, 1);
            voltage(nullorRow[k], block.opamps[k].inn, -1);
        }
        if (block.port)
        {
            current(block.port->anode, block.port->cathode, portCurrent);
            voltage(portRow, block.port->anode, 1);
            voltage(portRow, block.port->cathode, -1);
            s.rhs[static_cast<std::size_t>(s.port)][static_cast<std::size_t>(portRow)].add(1);
        }

        // Readouts: the port's current, the output, each capacitor's voltage (one readout a node pair).
        auto formOf = [&](int p, int q, double scale)
        {
            System::Readout r;
            for (int side = 0; side < 2; ++side)
            {
                const int node = side == 0 ? p : q;
                const double sign = (side == 0 ? 1.0 : -1.0) * scale;
                const Term t = term(node);
                if (t.kind == Term::Kind::Unknown)
                    r.unknowns.emplace_back(t.index, sign);
                else if (t.kind == Term::Kind::Source)
                    r.sources.emplace_back(t.index, sign * t.scale);
            }
            return r;
        };
        auto label = [&](int p, int q)
        {
            if (q == 0)
                return "V(" + nodeName(p) + ")";
            if (p == 0)
                return "-V(" + nodeName(q) + ")";
            return "V(" + nodeName(p) + ") - V(" + nodeName(q) + ")";
        };
        if (block.port)
        {
            System::Readout r;
            r.name = "j_" + block.port->name;
            r.kind = System::Readout::Kind::PortCurrent;
            r.label = "j(" + block.port->parts + ")";
            r.unknowns.emplace_back(portCurrent, 1);
            s.portCurrent = static_cast<int>(s.readouts.size());
            s.readouts.push_back(r);
        }
        const bool lastBlock = blockIndex + 1 == static_cast<int>(plan.blocks.size());
        const double outScale = lastBlock ? 1 / plan.outputScale : 1;
        {
            System::Readout r = formOf(block.output, 0, outScale);
            r.name = "n_" + plan.nodes[static_cast<std::size_t>(block.output)].ident;
            r.kind = System::Readout::Kind::Node;
            r.label = label(block.output, 0);
            s.output = static_cast<int>(s.readouts.size());
            s.readouts.push_back(r);
        }
        struct Pair
        {
            int p;
            int q;
            int readout;
        };
        std::vector<Pair> pairs;
        if (outScale == 1)
            pairs.push_back({block.output, 0, s.output});
        for (std::size_t b = 0; b < block.branches.size(); ++b)
        {
            const Branch& br = block.branches[b];
            if (br.kind != Branch::Kind::Capacitor)
                continue;
            int found = -1;
            double sign = 1;
            for (const Pair& pr : pairs)
            {
                if (pr.p == br.a && pr.q == br.b)
                    found = pr.readout;
                else if (pr.p == br.b && pr.q == br.a)
                {
                    found = pr.readout;
                    sign = -1;
                }
                if (found >= 0)
                    break;
            }
            if (found < 0)
            {
                System::Readout r = formOf(br.a, br.b, 1);
                r.name = "vc_" + br.ident;
                r.kind = System::Readout::Kind::Capacitor;
                r.label = label(br.a, br.b);
                found = static_cast<int>(s.readouts.size());
                s.readouts.push_back(r);
                pairs.push_back({br.a, br.b, found});
            }
            s.capacitor.emplace_back(found, sign);
        }
        return s;
    }

    // ---- the elimination order ----------------------------------------------------------------------------------

    namespace
    {
        const double kHostRates[] = {22050, 44100, 48000, 96000, 192000, 384000};
        constexpr double kThreshold = 0.1;

        std::string partsOf(const Plan& plan, const Block& block)
        {
            std::string out;
            std::size_t count = 0;
            for (int e : block.elements)
            {
                if (count == 4)
                {
                    out += ", ...";
                    break;
                }
                out += (out.empty() ? "" : ", ") + plan.netlist.elements[static_cast<std::size_t>(e)].name;
                ++count;
            }
            return out;
        }

        std::string formatRate(double rate)
        {
            std::string s = std::format("{:.6g}", rate / 1000);
            return s + " kHz";
        }

        // One step of the elimination, as solve() does it, on a dense matrix of T.
        template <typename T>
        void eliminate(std::vector<std::vector<T>>& a, std::size_t pr, std::size_t pc, const std::vector<bool>& rowDone,
                       const std::vector<bool>& colDone)
        {
            const std::size_t n = a.size();
            bool haveReciprocal = false;
            T reciprocal = T(0.0);
            for (std::size_t r = 0; r < n; ++r)
            {
                if (rowDone[r] || r == pr || isZero(a[r][pc]))
                    continue;
                if (!haveReciprocal)
                {
                    reciprocal = T(1.0) / a[pr][pc];
                    haveReciprocal = true;
                }
                const T m = a[r][pc] * reciprocal;
                for (std::size_t c = 0; c < n; ++c)
                {
                    if (colDone[c] || c == pc || isZero(a[pr][c]))
                        continue;
                    a[r][c] = a[r][c] - m * a[pr][c];
                }
                a[r][pc] = T(0.0);
            }
        }

        // Every unknown, by the elimination in `pivots`, for a right-hand side b.
        std::vector<double> solveAll(std::vector<std::vector<double>> a, std::vector<double> b, const Pivots& pivots)
        {
            const std::size_t n = a.size();
            std::vector<bool> rowDone(n, false);
            std::vector<bool> colDone(n, false);
            for (const auto& [pr0, pc0] : pivots.order)
            {
                const auto pr = static_cast<std::size_t>(pr0);
                const auto pc = static_cast<std::size_t>(pc0);
                for (std::size_t r = 0; r < n; ++r)
                {
                    if (rowDone[r] || r == pr || a[r][pc] == 0)
                        continue;
                    const double m = a[r][pc] * (1.0 / a[pr][pc]);
                    for (std::size_t c = 0; c < n; ++c)
                    {
                        if (colDone[c] || c == pc || a[pr][c] == 0)
                            continue;
                        a[r][c] -= m * a[pr][c];
                    }
                    b[r] -= m * b[pr];
                    a[r][pc] = 0;
                }
                rowDone[pr] = true;
                colDone[pc] = true;
            }
            std::vector<double> z(n, 0.0);
            for (std::size_t k = n; k-- > 0;)
            {
                const auto pr = static_cast<std::size_t>(pivots.order[k].first);
                const auto pc = static_cast<std::size_t>(pivots.order[k].second);
                double acc = b[pr];
                for (std::size_t j = k + 1; j < n; ++j)
                {
                    const auto c = static_cast<std::size_t>(pivots.order[j].second);
                    acc -= a[pr][c] * z[c];
                }
                z[pc] = acc / a[pr][pc];
            }
            return z;
        }

        struct GridPoint
        {
            double hostRate = 0;
            double designRate = 0;
            std::vector<double> pots;
        };

        std::vector<GridPoint> pivotGrid(const Plan& plan, const Block& block)
        {
            std::vector<GridPoint> grid;
            const std::vector<std::vector<double>> pots = potGrid(plan, block);
            for (double rate : kHostRates)
            {
                for (const std::vector<double>& p : pots)
                    grid.push_back({rate, rate * block.rateFactor * block.designFactor, p});
            }
            return grid;
        }

        std::string pointText(const Plan& plan, const Block& block, const GridPoint& g)
        {
            std::string out;
            for (const std::string& key : block.pots)
            {
                for (std::size_t i = 0; i < plan.pots.size(); ++i)
                {
                    if (plan.pots[i].key == key)
                        out += std::format("{}={:g}, ", plan.pots[i].name, g.pots[i]);
                }
            }
            return out + formatRate(g.hostRate);
        }

        std::vector<std::vector<double>> numeric(const System& system, const std::vector<double>& symbols)
        {
            const auto n = static_cast<std::size_t>(system.n);
            std::vector<std::vector<double>> a(n, std::vector<double>(n, 0.0));
            for (std::size_t r = 0; r < n; ++r)
            {
                for (std::size_t c = 0; c < n; ++c)
                {
                    if (!system.a[r][c].isZero())
                        a[r][c] = fromAffine(system.a[r][c], symbols);
                }
            }
            return a;
        }

        // The order, by exact +-1 entries first (least Markowitz cost, then the lowest row and column), then by the
        // best worst-over-the-grid magnitude in its column; or by that magnitude alone. `failure` says where it fails.
        std::optional<Pivots> tryOrder(const System& system, const Plan& plan, const Block& block,
                                       const std::vector<GridPoint>& grid, bool preferOnes, std::string& failure)
        {
            const auto n = static_cast<std::size_t>(system.n);
            Code scratch;
            std::vector<Val> symbols;
            for (const System::Symbol& s : system.symbols)
                symbols.push_back(Val::var(s.name, &scratch));
            std::vector<std::vector<Val>> structure(n, std::vector<Val>(n));
            for (std::size_t r = 0; r < n; ++r)
            {
                for (std::size_t c = 0; c < n; ++c)
                {
                    if (!system.a[r][c].isZero())
                        structure[r][c] = fromAffine(system.a[r][c], symbols);
                }
            }
            std::vector<std::vector<std::vector<double>>> values;
            std::vector<std::vector<double>> symbolSets;
            for (const GridPoint& g : grid)
            {
                symbolSets.push_back(symbolValues(system, plan, block.index - 1, g.designRate, g.pots));
                values.push_back(numeric(system, symbolSets.back()));
            }
            std::vector<bool> rowDone(n, false);
            std::vector<bool> colDone(n, false);
            Pivots pivots;
            for (std::size_t step = 0; step < n; ++step)
            {
                // Column maxima over the remaining rows, at each grid point.
                auto relative = [&](std::size_t r, std::size_t c)
                {
                    double worst = std::numeric_limits<double>::infinity();
                    for (const auto& a : values)
                    {
                        double top = 0;
                        for (std::size_t rr = 0; rr < n; ++rr)
                        {
                            if (!rowDone[rr])
                                top = std::max(top, std::fabs(a[rr][c]));
                        }
                        const double rel = top > 0 ? std::fabs(a[r][c]) / top : 0;
                        worst = std::min(worst, rel);
                    }
                    return worst;
                };
                std::size_t bestR = n;
                std::size_t bestC = n;
                if (preferOnes)
                {
                    long bestCost = std::numeric_limits<long>::max();
                    for (std::size_t r = 0; r < n; ++r)
                    {
                        if (rowDone[r])
                            continue;
                        for (std::size_t c = 0; c < n; ++c)
                        {
                            // an exact +-1, and (threshold pivoting) at least 0.1 of its column everywhere on the
                            // grid: float runs the elimination, so the growth a smaller pivot allows is lost digits
                            if (colDone[c] || !isUnit(structure[r][c]) || relative(r, c) < kThreshold)
                                continue;
                            long rowCount = 0;
                            long colCount = 0;
                            for (std::size_t k = 0; k < n; ++k)
                            {
                                if (!colDone[k] && !isZero(structure[r][k]))
                                    ++rowCount;
                                if (!rowDone[k] && !isZero(structure[k][c]))
                                    ++colCount;
                            }
                            const long cost = (rowCount - 1) * (colCount - 1);
                            if (cost < bestCost)
                            {
                                bestCost = cost;
                                bestR = r;
                                bestC = c;
                            }
                        }
                    }
                }
                if (bestR == n)
                {
                    double bestScore = -1;
                    for (std::size_t r = 0; r < n; ++r)
                    {
                        if (rowDone[r])
                            continue;
                        for (std::size_t c = 0; c < n; ++c)
                        {
                            if (colDone[c] || isZero(structure[r][c]))
                                continue;
                            const double score = relative(r, c);
                            if (score > bestScore)
                            {
                                bestScore = score;
                                bestR = r;
                                bestC = c;
                            }
                        }
                    }
                }
                if (bestR == n)
                {
                    std::size_t left = 0;
                    while (left < n && colDone[left])
                        ++left;
                    failure = "(nothing determines " + system.unknownNames[std::min(left, n - 1)] + ")";
                    return std::nullopt;
                }
                // Every pivot at least 1e-9 of its column, everywhere on the grid.
                for (std::size_t g = 0; g < values.size(); ++g)
                {
                    double top = 0;
                    for (std::size_t rr = 0; rr < n; ++rr)
                    {
                        if (!rowDone[rr])
                            top = std::max(top, std::fabs(values[g][rr][bestC]));
                    }
                    if (!(std::fabs(values[g][bestR][bestC]) >= 1e-9 * top) || top == 0)
                    {
                        failure = "at " + pointText(plan, block, grid[g]);
                        return std::nullopt;
                    }
                }
                eliminate(structure, bestR, bestC, rowDone, colDone);
                for (auto& a : values)
                    eliminate(a, bestR, bestC, rowDone, colDone);
                rowDone[bestR] = true;
                colDone[bestC] = true;
                pivots.order.emplace_back(static_cast<int>(bestR), static_cast<int>(bestC));
            }
            // The double solve's relative residual, at every grid point and for every source.
            for (std::size_t g = 0; g < grid.size(); ++g)
            {
                const std::vector<std::vector<double>> a = numeric(system, symbolSets[g]);
                double norm = 0;
                for (const auto& row : a)
                {
                    double sum = 0;
                    for (double v : row)
                        sum += std::fabs(v);
                    norm = std::max(norm, sum);
                }
                for (std::size_t s = 0; s < system.sources.size(); ++s)
                {
                    std::vector<double> b(n, 0.0);
                    for (std::size_t r = 0; r < n; ++r)
                    {
                        if (!system.rhs[s][r].isZero())
                            b[r] = fromAffine(system.rhs[s][r], symbolSets[g]);
                    }
                    const std::vector<double> z = solveAll(a, b, pivots);
                    double residual = 0;
                    double zNorm = 0;
                    double bNorm = 0;
                    for (std::size_t r = 0; r < n; ++r)
                    {
                        double sum = -b[r];
                        for (std::size_t c = 0; c < n; ++c)
                            sum += a[r][c] * z[c];
                        residual = std::max(residual, std::fabs(sum));
                        zNorm = std::max(zNorm, std::fabs(z[r]));
                        bNorm = std::max(bNorm, std::fabs(b[r]));
                    }
                    const double scale = norm * zNorm + bNorm;
                    if (!std::isfinite(residual) || (scale > 0 && residual > 1e-9 * scale))
                    {
                        failure = "at " + pointText(plan, block, grid[g]);
                        return std::nullopt;
                    }
                }
            }
            return pivots;
        }
    } // namespace

    std::optional<Pivots> choosePivots(const System& system, const Plan& plan, int blockIndex, Messages& messages)
    {
        const Block& block = plan.blocks.at(static_cast<std::size_t>(blockIndex));
        const std::vector<GridPoint> grid = pivotGrid(plan, block);
        std::string failure;
        if (std::optional<Pivots> p = tryOrder(system, plan, block, grid, true, failure))
            return p;
        std::string second;
        if (std::optional<Pivots> p = tryOrder(system, plan, block, grid, false, second))
            return p;
        Loc loc;
        if (!block.elements.empty())
            loc = plan.netlist.elements[static_cast<std::size_t>(block.elements.front())].loc;
        messages.error(loc, "block " + std::to_string(block.index) + " (" + partsOf(plan, block) +
                                "): its equations are singular " + second +
                                ": an op amp without negative feedback, or a node cut off from the rest");
        return std::nullopt;
    }

    double pivotMargin(const System& system, const Pivots& pivots, const Plan& plan, int blockIndex)
    {
        const Block& block = plan.blocks.at(static_cast<std::size_t>(blockIndex));
        const auto n = static_cast<std::size_t>(system.n);
        double worst = std::numeric_limits<double>::infinity();
        for (const GridPoint& g : pivotGrid(plan, block))
        {
            std::vector<std::vector<double>> a =
                numeric(system, symbolValues(system, plan, blockIndex, g.designRate, g.pots));
            std::vector<bool> rowDone(n, false);
            std::vector<bool> colDone(n, false);
            for (const auto& [r0, c0] : pivots.order)
            {
                const auto r = static_cast<std::size_t>(r0);
                const auto c = static_cast<std::size_t>(c0);
                double top = 0;
                for (std::size_t rr = 0; rr < n; ++rr)
                {
                    if (!rowDone[rr])
                        top = std::max(top, std::fabs(a[rr][c]));
                }
                worst = std::min(worst, top > 0 ? std::fabs(a[r][c]) / top : 0.0);
                eliminate(a, r, c, rowDone, colDone);
                rowDone[r] = true;
                colDone[c] = true;
            }
        }
        return worst;
    }

    std::vector<double> symbolValues(const System& system, const Plan& plan, int blockIndex, double designRate,
                                     const std::vector<double>& pots)
    {
        const Block& block = plan.blocks.at(static_cast<std::size_t>(blockIndex));
        std::vector<double> out;
        for (const System::Symbol& s : system.symbols)
        {
            const Branch& br = block.branches[static_cast<std::size_t>(s.branch)];
            if (s.kind == System::Symbol::Kind::Gc)
            {
                out.push_back(2 * br.c * designRate);
                continue;
            }
            const double x = evaluate(br.expr, [&](const std::string& key) { return potValue(plan, pots, key); });
            if (br.section == 0)
            {
                out.push_back(x);
                continue;
            }
            const double t = taperValue(br.taper, std::clamp(x, 0.0, 1.0));
            out.push_back(br.section == 1 ? br.value * (1 - t) : br.value * t);
        }
        return out;
    }

    namespace
    {
        Pivots pivotsOrThrow(const System& system, const Plan& plan, int block)
        {
            Messages scratch;
            std::optional<Pivots> p = choosePivots(system, plan, block, scratch);
            if (!p)
                throw std::runtime_error(scratch.format("netlist", Severity::Error));
            return *p;
        }
    } // namespace

    Holters holters(const Plan& plan, int blockIndex, double designRate, const std::vector<double>& pots)
    {
        const Block& block = plan.blocks.at(static_cast<std::size_t>(blockIndex));
        const System system = assemble(plan, blockIndex);
        const Pivots pivots = pivotsOrThrow(system, plan, blockIndex);
        const Linear<double> l = solve(system, pivots, symbolValues(system, plan, blockIndex, designRate, pots));
        Holters h;
        std::vector<std::size_t> states;
        for (std::size_t s = 0; s < system.sources.size(); ++s)
        {
            if (system.sources[s].kind == System::Source::Kind::State)
                states.push_back(s);
        }
        h.states = static_cast<int>(states.size());
        h.port = block.port.has_value();
        const auto in = static_cast<std::size_t>(system.input);
        const auto out = static_cast<std::size_t>(system.output);
        double kSelf = 0;
        if (h.port)
        {
            const auto j = static_cast<std::size_t>(system.portCurrent);
            const auto v = static_cast<std::size_t>(system.port);
            kSelf = 1 / -l.coef[j][v];
            for (std::size_t s : states)
                h.cp.push_back(l.coef[j][s] * kSelf);
            h.dp = l.coef[j][in] * kSelf;
            h.is = block.port->is;
            h.nvt = block.port->nvt;
            h.k = 2 * block.port->is * kSelf;
            h.wy = l.coef[out][v];
        }
        for (std::size_t s : states)
            h.cy.push_back(l.coef[out][s]);
        h.dy = l.coef[out][in];
        h.a.assign(states.size(), std::vector<double>(states.size(), 0.0));
        h.b.assign(states.size(), 0.0);
        h.w.assign(states.size(), 0.0);
        for (std::size_t i = 0; i < states.size(); ++i)
        {
            const Branch& br = block.branches[static_cast<std::size_t>(system.sources[states[i]].branch)];
            const double g2 = 4 * br.c * designRate;
            const auto [readout, sign] = system.capacitor[i];
            const auto ro = static_cast<std::size_t>(readout);
            for (std::size_t j = 0; j < states.size(); ++j)
                h.a[i][j] = g2 * sign * l.coef[ro][states[j]] - (i == j ? 1 : 0);
            h.b[i] = g2 * sign * l.coef[ro][in];
            if (h.port)
                h.w[i] = g2 * sign * l.coef[ro][static_cast<std::size_t>(system.port)];
        }
        return h;
    }

    std::complex<double> analog(const System& system, const Pivots& pivots, const Plan& plan, int blockIndex,
                                std::complex<double> s, const std::vector<double>& pots)
    {
        const Block& block = plan.blocks.at(static_cast<std::size_t>(blockIndex));
        // r from the pots (as numbers), gc := s C
        const std::vector<double> numbers = symbolValues(system, plan, blockIndex, 1.0, pots);
        std::vector<std::complex<double>> symbols;
        for (std::size_t k = 0; k < system.symbols.size(); ++k)
        {
            const System::Symbol& sym = system.symbols[k];
            if (sym.kind == System::Symbol::Kind::Gc)
                symbols.push_back(s * block.branches[static_cast<std::size_t>(sym.branch)].c);
            else
                symbols.emplace_back(numbers[k], 0.0);
        }
        const Linear<std::complex<double>> l = solve(system, pivots, symbols);
        return l.coef[static_cast<std::size_t>(system.output)][static_cast<std::size_t>(system.input)];
    }

    std::complex<double> analog(const Plan& plan, int block, std::complex<double> s, const std::vector<double>& pots)
    {
        const System system = assemble(plan, block);
        return analog(system, pivotsOrThrow(system, plan, block), plan, block, s, pots);
    }

    double oneXError(const System& system, const Pivots& pivots, const Plan& plan, int blockIndex, double rate)
    {
        const Block& block = plan.blocks.at(static_cast<std::size_t>(blockIndex));
        double worst = 0;
        std::vector<double> freqs;
        for (double f = 20; f <= 20000; f *= 1.01)
            freqs.push_back(f);
        for (const std::vector<double>& pots : potGrid(plan, block))
        {
            std::vector<double> analogDb;
            double top = -1e300;
            for (double f : freqs)
            {
                const double h =
                    std::abs(analog(system, pivots, plan, blockIndex, {0, 2 * std::numbers::pi * f}, pots));
                analogDb.push_back(20 * std::log10(h + 1e-300));
                top = std::max(top, analogDb.back());
            }
            if (top < -120)
                continue;
            for (std::size_t i = 0; i < freqs.size(); ++i)
            {
                if (analogDb[i] < top - 40)
                    continue;
                const double warped = 2 * rate * std::tan(std::numbers::pi * freqs[i] / rate);
                const double h = std::abs(analog(system, pivots, plan, blockIndex, {0, warped}, pots));
                worst = std::max(worst, std::fabs(20 * std::log10(h + 1e-300) - analogDb[i]));
            }
        }
        return worst;
    }
} // namespace fminus::circuit
