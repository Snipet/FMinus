// src/circuit/Mna.cpp: a block's stamps (assemble), its elimination order (choosePivots), and the derivation run on
// numbers: the Holters form the reference simulates, and a linear block's analog response with its 1x check.
#include "circuit/Mna.h"

#include "circuit/Emit.h"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <format>
#include <functional>
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

    System assemble(const Plan& plan, int blockIndex, bool dc)
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
        // The impedance form's currents: a variable resistor's, an inductor's, a V source's (one between two nodes
        // neither of which is ground), in branch order; then a floating E's.
        auto hasCurrent = [](const Branch& b)
        {
            return b.kind == Branch::Kind::Variable || b.kind == Branch::Kind::Inductor ||
                   b.kind == Branch::Kind::Voltage;
        };
        std::vector<int> branchCurrent(block.branches.size(), -1);
        for (std::size_t b = 0; b < block.branches.size(); ++b)
        {
            if (hasCurrent(block.branches[b]))
            {
                branchCurrent[b] = s.n++;
                s.unknownNames.push_back("i(" + block.branches[b].ident + ")");
            }
        }
        std::vector<int> controlledCurrent(block.controlled.size(), -1);
        for (std::size_t k = 0; k < block.controlled.size(); ++k)
        {
            const Controlled& c = block.controlled[k];
            if (c.kind == Controlled::Kind::Vcvs && !c.driven)
            {
                controlledCurrent[k] = s.n++;
                s.unknownNames.push_back("i(" + c.ident + ")");
            }
        }
        // A two-terminal port (a diode, a pair) is Holters' v-form: its voltage the source v, its current an unknown
        // j. A controlled source (an OTA) is the i-form: its current the source i, its control voltage a readout, so
        // the block's readouts are sums of its states and i, never small differences of the input and v.
        const bool iForm = block.port && block.port->form == PortDef::Form::I;
        s.iForm = iForm;
        int portCurrent = -1;
        if (block.port && !iForm)
        {
            portCurrent = s.n++;
            s.unknownNames.push_back("j(" + block.port->parts + ")");
        }
        const auto un = static_cast<std::size_t>(s.n);
        s.a.assign(un, std::vector<Affine>(un));

        // Rows: KCL of the solved nodes, the nullors, the driven E sources, the branches with a current, the
        // floating E sources, the port.
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
        std::vector<int> controlledRow(block.controlled.size(), -1);
        for (std::size_t k = 0; k < block.controlled.size(); ++k)
        {
            if (block.controlled[k].driven)
            {
                controlledRow[k] = row++;
                s.rowNames.push_back(block.controlled[k].name);
            }
        }
        std::vector<int> branchRow(block.branches.size(), -1);
        for (std::size_t b = 0; b < block.branches.size(); ++b)
        {
            if (hasCurrent(block.branches[b]))
            {
                branchRow[b] = row++;
                s.rowNames.push_back(block.branches[b].ident);
            }
        }
        for (std::size_t k = 0; k < block.controlled.size(); ++k)
        {
            if (controlledCurrent[k] >= 0)
            {
                controlledRow[k] = row++;
                s.rowNames.push_back(block.controlled[k].name);
            }
        }
        int portRow = -1;
        if (block.port && !iForm)
        {
            portRow = row++;
            s.rowNames.push_back(block.port->parts);
        }
        if (row != s.n)
            throw std::logic_error("assemble: rows and unknowns differ");
        s.variableUnknown.assign(un, false);
        s.variableRow.assign(un, false);
        for (std::size_t b = 0; b < block.branches.size(); ++b)
        {
            if (block.branches[b].kind != Branch::Kind::Variable)
                continue;
            s.variableUnknown[static_cast<std::size_t>(branchCurrent[b])] = true;
            s.variableRow[static_cast<std::size_t>(branchRow[b])] = true;
        }

        // Sources: the input, each capacitor's and inductor's state, the port's v; with `dc`, the DC sources.
        const bool fromInput = block.input == plan.inputNode;
        s.sources.push_back(
            {fromInput ? plan.inputName : "n_" + plan.nodes[static_cast<std::size_t>(block.input)].ident,
             System::Source::Kind::Input, -1, 0});
        s.input = 0;
        std::vector<int> stateOf(block.branches.size(), -1);
        for (std::size_t b = 0; b < block.branches.size(); ++b)
        {
            if (block.branches[b].state())
            {
                stateOf[b] = static_cast<int>(s.sources.size());
                s.sources.push_back(
                    {"x_" + block.branches[b].ident, System::Source::Kind::State, static_cast<int>(b), 0});
            }
        }
        if (block.port)
        {
            s.port = static_cast<int>(s.sources.size());
            s.sources.push_back({(iForm ? "i_" : "v_") + block.port->name, System::Source::Kind::Port, -1, 0});
        }
        // the DC sources a block sees: its V branches, the grounded V sources' nodes it touches, the I sources at its
        // nodes (each once)
        std::vector<int> constantOf(block.branches.size(), -1);   // a V branch's
        std::map<int, int> constantAt;                            // a DC node's
        std::vector<std::pair<std::size_t, int>> currents;        // (plan.currents index, its source)
        if (dc)
        {
            for (std::size_t b = 0; b < block.branches.size(); ++b)
            {
                const Branch& br = block.branches[b];
                if (br.kind != Branch::Kind::Voltage)
                    continue;
                constantOf[b] = static_cast<int>(s.sources.size());
                s.sources.push_back({br.part, System::Source::Kind::Constant, static_cast<int>(b), br.dc});
            }
            auto touch = [&](int node)
            {
                const NodeInfo& n = plan.nodes[static_cast<std::size_t>(node)];
                if (n.source != NodeInfo::Source::Dc || constantAt.count(node))
                    return;
                constantAt[node] = static_cast<int>(s.sources.size());
                s.sources.push_back({plan.netlist.elements[static_cast<std::size_t>(n.element)].name,
                                     System::Source::Kind::Constant, -1, n.value});
            };
            for (const Branch& br : block.branches)
            {
                touch(br.a);
                touch(br.b);
            }
            if (block.port)
            {
                for (int node : {block.port->anode, block.port->cathode, block.port->from, block.port->to})
                    touch(node);
            }
            for (const OpAmp& op : block.opamps)
            {
                touch(op.inp);
                touch(op.inn);
            }
            for (const Controlled& c : block.controlled)
            {
                for (int node : {c.p, c.n, c.cp, c.cn})
                    touch(node);
            }
            for (std::size_t k = 0; k < plan.currents.size(); ++k)
            {
                const CurrentSource& c = plan.currents[k];
                if (!kclRow.count(c.a) && !kclRow.count(c.b))
                    continue;
                currents.emplace_back(k, static_cast<int>(s.sources.size()));
                s.sources.push_back({c.name, System::Source::Kind::Constant, -1, c.value});
            }
        }
        s.rhs.assign(s.sources.size(), std::vector<Affine>(un));

        // Symbols: each capacitor's gc, each variable branch's r, each inductor's rl; each gain pots move.
        std::vector<int> symbolOf(block.branches.size(), -1);
        for (std::size_t b = 0; b < block.branches.size(); ++b)
        {
            const Branch& br = block.branches[b];
            if (br.kind == Branch::Kind::Capacitor)
            {
                symbolOf[b] = static_cast<int>(s.symbols.size());
                s.symbols.push_back({"gc_" + br.ident, System::Symbol::Kind::Gc, static_cast<int>(b), -1});
            }
            else if (br.kind == Branch::Kind::Variable)
            {
                symbolOf[b] = static_cast<int>(s.symbols.size());
                s.symbols.push_back({"r_" + br.ident, System::Symbol::Kind::R, static_cast<int>(b), -1});
            }
            else if (br.kind == Branch::Kind::Inductor)
            {
                symbolOf[b] = static_cast<int>(s.symbols.size());
                s.symbols.push_back({"rl_" + br.ident, System::Symbol::Kind::Rl, static_cast<int>(b), -1});
            }
        }
        std::vector<Affine> gainOf(block.controlled.size());
        for (std::size_t k = 0; k < block.controlled.size(); ++k)
        {
            const Controlled& c = block.controlled[k];
            if (c.expr)
            {
                gainOf[k].add(static_cast<int>(s.symbols.size()), 1);
                s.symbols.push_back({"k_" + c.ident, System::Symbol::Kind::Gain, -1, static_cast<int>(k)});
            }
            else
                gainOf[k].add(c.gain);
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
            if (plan.nodes[static_cast<std::size_t>(node)].source == NodeInfo::Source::Dc)
            {
                // a constant: ground in the offsets
                if (dc)
                {
                    t.kind = Term::Kind::Source;
                    t.index = constantAt.at(node);
                }
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
        // y times a node's voltage in row r's left-hand side (moved to the right-hand side for a source).
        auto voltageTimes = [&](int r, int node, const Affine& y, double factor)
        {
            const Term t = term(node);
            if (t.kind == Term::Kind::Unknown)
                addTo(s.a[static_cast<std::size_t>(r)][static_cast<std::size_t>(t.index)], y, factor);
            else if (t.kind == Term::Kind::Source)
                addTo(s.rhs[static_cast<std::size_t>(t.index)][static_cast<std::size_t>(r)], y, -factor * t.scale);
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
                const int r = branchRow[b];
                voltage(r, br.a, 1);
                voltage(r, br.b, -1);
                s.a[static_cast<std::size_t>(r)][static_cast<std::size_t>(branchCurrent[b])].add(symbolOf[b], -1);
                break;
            }
            case Branch::Kind::Inductor:
            {
                // V(a) - V(b) - rl i = -x: the trapezoidal rule's v = rl i - x (x = rl i + v a step earlier)
                current(br.a, br.b, branchCurrent[b]);
                const int r = branchRow[b];
                voltage(r, br.a, 1);
                voltage(r, br.b, -1);
                s.a[static_cast<std::size_t>(r)][static_cast<std::size_t>(branchCurrent[b])].add(symbolOf[b], -1);
                s.rhs[static_cast<std::size_t>(stateOf[b])][static_cast<std::size_t>(r)].add(-1);
                break;
            }
            case Branch::Kind::Voltage:
            {
                // V(a) - V(b) = dc: 0 in the offsets, a short with its current an unknown
                current(br.a, br.b, branchCurrent[b]);
                const int r = branchRow[b];
                voltage(r, br.a, 1);
                voltage(r, br.b, -1);
                if (dc)
                    s.rhs[static_cast<std::size_t>(constantOf[b])][static_cast<std::size_t>(r)].add(1);
                break;
            }
            }
        }
        for (std::size_t k = 0; k < block.opamps.size(); ++k)
        {
            voltage(nullorRow[k], block.opamps[k].inp, 1);
            voltage(nullorRow[k], block.opamps[k].inn, -1);
        }
        for (std::size_t k = 0; k < block.controlled.size(); ++k)
        {
            const Controlled& c = block.controlled[k];
            const Affine& g = gainOf[k];
            if (c.kind == Controlled::Kind::Vccs)
            {
                // g (V(cp) - V(cn)) leaves p and enters n
                for (const auto& [node, sign] : {std::pair{c.p, 1.0}, std::pair{c.n, -1.0}})
                {
                    const auto it = kclRow.find(node);
                    if (it == kclRow.end())
                        continue;
                    voltageTimes(it->second, c.cp, g, sign);
                    voltageTimes(it->second, c.cn, g, -sign);
                }
                continue;
            }
            const int r = controlledRow[k];
            if (c.driven)
            {
                // V(out) - sign g (V(cp) - V(cn)) = 0
                voltage(r, c.out, 1);
                voltageTimes(r, c.cp, g, -c.sign);
                voltageTimes(r, c.cn, g, c.sign);
                continue;
            }
            // V(p) - V(n) - g (V(cp) - V(cn)) = 0, its current from p through it to n
            current(c.p, c.n, controlledCurrent[k]);
            voltage(r, c.p, 1);
            voltage(r, c.n, -1);
            voltageTimes(r, c.cp, g, -1);
            voltageTimes(r, c.cn, g, 1);
        }
        for (const auto& [k, source] : currents)
        {
            // value amps from a through the source to b: drawn from a, injected into b
            const CurrentSource& c = plan.currents[k];
            if (const auto it = kclRow.find(c.a); it != kclRow.end())
                s.rhs[static_cast<std::size_t>(source)][static_cast<std::size_t>(it->second)].add(-1);
            if (const auto it = kclRow.find(c.b); it != kclRow.end())
                s.rhs[static_cast<std::size_t>(source)][static_cast<std::size_t>(it->second)].add(1);
        }
        if (block.port && !iForm)
        {
            // the v-form: j, the port's current (an unknown), flows from anode to cathode through it, and its row
            // V(anode) - V(cathode) = v makes its voltage the source v
            current(block.port->from, block.port->to, portCurrent);
            voltage(portRow, block.port->anode, 1);
            voltage(portRow, block.port->cathode, -1);
            s.rhs[static_cast<std::size_t>(s.port)][static_cast<std::size_t>(portRow)].add(1);
        }
        else if (block.port)
        {
            // the i-form: the source i leaves `from` (n+) and enters `to` (n-), SPICE's G direction
            const auto i = static_cast<std::size_t>(s.port);
            if (const auto it = kclRow.find(block.port->from); it != kclRow.end())
                s.rhs[i][static_cast<std::size_t>(it->second)].add(-1);
            if (const auto it = kclRow.find(block.port->to); it != kclRow.end())
                s.rhs[i][static_cast<std::size_t>(it->second)].add(1);
        }

        // Readouts: the port's current, the output, each capacitor's voltage (one readout a node pair), each
        // inductor's current.
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
        if (block.port && !iForm)
        {
            System::Readout r;
            r.name = "j_" + block.port->name;
            r.kind = System::Readout::Kind::PortCurrent;
            r.label = "j(" + block.port->parts + ")";
            r.unknowns.emplace_back(portCurrent, 1);
            s.portCurrent = static_cast<int>(s.readouts.size());
            s.readouts.push_back(r);
        }
        else if (block.port)
        {
            // the OTA's control voltage, V(nc+) - V(nc-): v = p - kd i
            System::Readout r = formOf(block.port->anode, block.port->cathode, 1);
            r.name = "vp_" + block.port->name;
            r.kind = System::Readout::Kind::PortCurrent;
            r.label = label(block.port->anode, block.port->cathode);
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
            if (br.kind == Branch::Kind::Inductor)
            {
                System::Readout r;
                r.name = "il_" + br.ident;
                r.kind = System::Readout::Kind::Current;
                r.label = "i(" + br.part + ")";
                r.unknowns.emplace_back(branchCurrent[b], 1);
                s.capacitor.emplace_back(static_cast<int>(s.readouts.size()), 1);
                s.readouts.push_back(r);
                continue;
            }
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
                                       const std::vector<GridPoint>& grid, bool preferOnes, bool variableLast,
                                       std::string& failure)
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
            std::size_t fixedLeft = 0;   // the unknowns that are not variable branches' currents
            for (std::size_t c = 0; c < n; ++c)
                fixedLeft += system.variableUnknown[c] ? 0 : 1;
            for (std::size_t step = 0; step < n; ++step)
            {
                // with variableLast, until the other unknowns are eliminated, neither a variable row nor column
                auto allowed = [&](std::size_t r, std::size_t c)
                {
                    if (!variableLast || fixedLeft == 0)
                        return true;
                    return !system.variableRow[r] && !system.variableUnknown[c];
                };
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
                            if (colDone[c] || !allowed(r, c) || !isUnit(structure[r][c]) ||
                                relative(r, c) < kThreshold)
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
                            if (colDone[c] || !allowed(r, c) || isZero(structure[r][c]))
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
                fixedLeft -= system.variableUnknown[bestC] ? 0 : 1;
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

    std::optional<Pivots> choosePivots(const System& system, const Plan& plan, int blockIndex, Messages& messages,
                                       bool variableLast)
    {
        const Block& block = plan.blocks.at(static_cast<std::size_t>(blockIndex));
        const std::vector<GridPoint> grid = pivotGrid(plan, block);
        std::string failure;
        if (std::optional<Pivots> p = tryOrder(system, plan, block, grid, true, variableLast, failure))
            return p;
        std::string second;
        if (std::optional<Pivots> p = tryOrder(system, plan, block, grid, false, variableLast, second))
            return p;
        if (variableLast)
            return std::nullopt;
        Loc loc;
        if (!block.elements.empty())
            loc = plan.netlist.elements[static_cast<std::size_t>(block.elements.front())].loc;
        messages.error(loc, "block " + std::to_string(block.index) + " (" + partsOf(plan, block) +
                                "): its equations are singular " + second +
                                ": an op amp without negative feedback, or a node cut off from the rest");
        return std::nullopt;
    }

    bool checkPort(const System& system, const Pivots& pivots, const Plan& plan, int blockIndex, Messages& messages)
    {
        const Block& block = plan.blocks.at(static_cast<std::size_t>(blockIndex));
        if (!block.port)
            return true;
        const auto j = static_cast<std::size_t>(system.portCurrent);
        const auto v = static_cast<std::size_t>(system.port);
        for (const GridPoint& g : pivotGrid(plan, block))
        {
            const Linear<double> l =
                solve(system, pivots, symbolValues(system, plan, blockIndex, g.designRate, g.pots));
            // kd: the v-form's 1 / -(dj/dv); the i-form's -(dv/di)
            const double k = system.iForm ? -l.coef[j][v] : 1 / -l.coef[j][v];
            if (k > 0 && std::isfinite(k))
                continue;
            Loc loc;
            if (!block.elements.empty())
                loc = plan.netlist.elements[static_cast<std::size_t>(block.elements.front())].loc;
            const std::string around = block.opamps.empty() ? std::string() : " around " + block.opamps.front().name;
            if (k == 0 && system.iForm)
                messages.error(plan.netlist.elements[static_cast<std::size_t>(block.port->elements.front())].loc,
                               "OTA " + block.port->parts +
                                   " has no feedback around it (an open-loop OTA, a VCA): Phase 2");
            else if (k < 0 && block.port->form == PortDef::Form::I)
                messages.error(plan.netlist.elements[static_cast<std::size_t>(block.port->elements.front())].loc,
                               std::format("OTA {} sees positive feedback (kd = {:.4g} ohm at {}): its output drives "
                                           "its + input; not supported yet",
                                           block.port->parts, k, pointText(plan, block, g)));
            else if (k < 0)
                messages.error(loc, std::format("port {} sees a negative resistance (K = {:.4g} ohm at {}): positive "
                                                "feedback{} is not supported yet",
                                                block.port->parts, k, pointText(plan, block, g), around));
            else
                messages.error(loc, std::format("port {}'s self-impedance is not a positive number (K = {:.4g} ohm at "
                                                "{})",
                                                block.port->parts, k, pointText(plan, block, g)));
            return false;
        }
        return true;
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
                                     const std::vector<double>& pots, std::optional<Method> method)
    {
        const Block& block = plan.blocks.at(static_cast<std::size_t>(blockIndex));
        // a companion's factor of C fs' (or L fs'): 2 for the trapezoidal rule, 3 / 2 for BDF2
        const double companion = method.value_or(block.method) == Method::Bdf2 ? 1.5 : 2.0;
        std::vector<double> out;
        auto pot = [&](const std::string& key)
        {
            return potValue(plan, pots, key);
        };
        for (const System::Symbol& s : system.symbols)
        {
            if (s.kind == System::Symbol::Kind::Gain)
            {
                out.push_back(evaluate(*block.controlled[static_cast<std::size_t>(s.controlled)].expr, pot));
                continue;
            }
            const Branch& br = block.branches[static_cast<std::size_t>(s.branch)];
            if (s.kind == System::Symbol::Kind::Gc)
            {
                out.push_back(companion * br.c * designRate);
                continue;
            }
            if (s.kind == System::Symbol::Kind::Rl)
            {
                out.push_back(companion * br.l * designRate);
                continue;
            }
            if (br.isSwitch())
            {
                // settled at the point's position: the emitted ramp's end
                out.push_back(switchResistance(br, switchRamp(br, switchPosition(plan, pots, br.switchKey))));
                continue;
            }
            const double x = evaluate(br.expr, pot);
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

    Holters holters(const Plan& plan, int blockIndex, double designRate, const std::vector<double>& pots,
                    const std::vector<double>& laws, std::optional<Method> method)
    {
        const Block& block = plan.blocks.at(static_cast<std::size_t>(blockIndex));
        const bool bdf2 = method.value_or(block.method) == Method::Bdf2;
        const System system = assemble(plan, blockIndex);
        const Pivots pivots = pivotsOrThrow(system, plan, blockIndex);
        const Linear<double> l =
            solve(system, pivots, symbolValues(system, plan, blockIndex, designRate, pots, method));
        Holters h;
        std::vector<std::size_t> states;
        for (std::size_t s = 0; s < system.sources.size(); ++s)
        {
            if (system.sources[s].kind == System::Source::Kind::State)
                states.push_back(s);
        }
        // BDF2's state is [x; xo]: each companion's history, then its second part
        const std::size_t count = states.size();
        const std::size_t size = bdf2 ? 2 * count : count;
        h.states = static_cast<int>(size);
        h.port = block.port.has_value();
        const auto in = static_cast<std::size_t>(system.input);
        const auto out = static_cast<std::size_t>(system.output);
        double kSelf = 0;
        // each readout's coefficient on a source as the v-form has it: the i-form's i = (v - p) / Ki substituted (in
        // double, where its differences cost nothing), so the reference runs both forms as one
        const auto port = static_cast<std::size_t>(system.port);
        const auto pr = static_cast<std::size_t>(system.portCurrent);
        auto onV = [&](std::size_t ro)
        {
            return system.iForm ? l.coef[ro][port] / l.coef[pr][port] : l.coef[ro][port];
        };
        auto on = [&](std::size_t ro, std::size_t src)
        {
            if (!h.port || !system.iForm)
                return l.coef[ro][src];
            return l.coef[ro][src] - l.coef[ro][port] * l.coef[pr][src] / l.coef[pr][port];
        };
        if (h.port)
        {
            const auto j = pr;
            const auto v = port;
            kSelf = system.iForm ? -l.coef[j][v] : 1 / -l.coef[j][v];
            h.kd = kSelf;
            for (std::size_t s : states)
                h.cp.push_back(system.iForm ? l.coef[j][s] : l.coef[j][s] * kSelf);
            h.cp.resize(size, 0.0);
            h.dp = system.iForm ? l.coef[j][in] : l.coef[j][in] * kSelf;
            h.law = block.port->law;
            h.is = block.port->is;
            h.nvt = block.port->nvt;
            // the pair's v + 2 Is K sinh(v / N Vt); an OTA's v + K g tanh(v / 2 Vt), g its gain (at the point's pots
            // and the law inputs `laws`)
            h.k = block.port->smooth()                       ? kSelf
                  : block.port->law == PortDef::Law::Pair ? 2 * block.port->is * kSelf
                                                          : block.port->is * kSelf;
            h.k2 = block.port->is2 * kSelf;
            h.nvt2 = block.port->nvt2;
            if (block.port->smooth())
                h.g = gainAt(plan, *block.port, pots, laws);
            h.wy = onV(out);
        }
        for (std::size_t s : states)
            h.cy.push_back(on(out, s));
        h.cy.resize(size, 0.0);
        h.dy = on(out, in);
        // a state's update, x <- 2 gc v - x (a capacitor) or 2 rl i - x (an inductor) with the trapezoidal rule: its
        // factor, 2 gc or 2 rl, 4 C fs' or 4 L fs'; BDF2's x <- h - xo, xo <- h / 4, h = 4/3 g v: its factor 4/3 g,
        // 2 C fs' or 2 L fs' (g = 3 C fs' / 2)
        auto twice = [&](std::size_t i)
        {
            const Branch& br = block.branches[static_cast<std::size_t>(system.sources[states[i]].branch)];
            return (bdf2 ? 2 : 4) * (br.kind == Branch::Kind::Inductor ? br.l : br.c) * designRate;
        };
        // the update's rows over [x; xo], from each state's readout's coefficients
        auto rows = [&](std::vector<std::vector<double>>& a, std::vector<double>& b, std::vector<double>& w,
                        const std::function<double(std::size_t, std::size_t)>& coef,
                        const std::function<double(std::size_t)>& onPort)
        {
            a.assign(size, std::vector<double>(size, 0.0));
            b.assign(size, 0.0);
            w.assign(size, 0.0);
            for (std::size_t i = 0; i < count; ++i)
            {
                const double g2 = twice(i);
                const auto [readout, sign] = system.capacitor[i];
                const auto ro = static_cast<std::size_t>(readout);
                for (std::size_t j = 0; j < count; ++j)
                    a[i][j] = g2 * sign * coef(ro, states[j]) - (!bdf2 && i == j ? 1 : 0);
                b[i] = g2 * sign * coef(ro, in);
                if (h.port)
                    w[i] = g2 * sign * onPort(ro);
                if (!bdf2)
                    continue;
                a[i][count + i] = -1;
                for (std::size_t j = 0; j < count; ++j)
                    a[count + i][j] = 0.25 * a[i][j];
                b[count + i] = 0.25 * b[i];
                w[count + i] = 0.25 * w[i];
            }
        };
        rows(h.a, h.b, h.w, on, onV);
        if (h.port && system.iForm)
        {
            h.iForm = true;
            rows(
                h.ai, h.bi, h.wi, [&](std::size_t ro, std::size_t src) { return l.coef[ro][src]; },
                [&](std::size_t ro) { return l.coef[ro][port]; });
            for (std::size_t s : states)
                h.cyi.push_back(l.coef[out][s]);
            h.cyi.resize(size, 0.0);
            h.dyi = l.coef[out][in];
            h.wyi = l.coef[out][port];
        }
        return h;
    }

    std::complex<double> analog(const System& system, const Pivots& pivots, const Plan& plan, int blockIndex,
                                std::complex<double> s, const std::vector<double>& pots)
    {
        const Block& block = plan.blocks.at(static_cast<std::size_t>(blockIndex));
        // r and the gains from the pots (as numbers), gc := s C, rl := s L
        const std::vector<double> numbers = symbolValues(system, plan, blockIndex, 1.0, pots);
        std::vector<std::complex<double>> symbols;
        for (std::size_t k = 0; k < system.symbols.size(); ++k)
        {
            const System::Symbol& sym = system.symbols[k];
            if (sym.kind == System::Symbol::Kind::Gc)
                symbols.push_back(s * block.branches[static_cast<std::size_t>(sym.branch)].c);
            else if (sym.kind == System::Symbol::Kind::Rl)
                symbols.push_back(s * block.branches[static_cast<std::size_t>(sym.branch)].l);
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

    std::complex<double> analogPortCurrent(const Plan& plan, int blockIndex, std::complex<double> s,
                                           const std::vector<double>& pots)
    {
        const System system = assemble(plan, blockIndex);
        return analogPortCurrent(system, pivotsOrThrow(system, plan, blockIndex), plan, blockIndex, s, pots);
    }

    std::complex<double> analogPortCurrent(const System& system, const Pivots& pivots, const Plan& plan,
                                           int blockIndex, std::complex<double> s, const std::vector<double>& pots)
    {
        const Block& block = plan.blocks.at(static_cast<std::size_t>(blockIndex));
        const std::vector<double> numbers = symbolValues(system, plan, blockIndex, 1.0, pots);
        std::vector<std::complex<double>> symbols;
        for (std::size_t k = 0; k < system.symbols.size(); ++k)
        {
            const System::Symbol& sym = system.symbols[k];
            if (sym.kind == System::Symbol::Kind::Gc)
                symbols.push_back(s * block.branches[static_cast<std::size_t>(sym.branch)].c);
            else if (sym.kind == System::Symbol::Kind::Rl)
                symbols.push_back(s * block.branches[static_cast<std::size_t>(sym.branch)].l);
            else
                symbols.emplace_back(numbers[k], 0.0);
        }
        const Linear<std::complex<double>> l = solve(system, pivots, symbols);
        return l.coef[static_cast<std::size_t>(system.portCurrent)][static_cast<std::size_t>(system.input)];
    }

    std::complex<double> discreteS(Method method, double f, double rate)
    {
        if (method == Method::Tr)
            return {0, 2 * rate * std::tan(std::numbers::pi * f / rate)};
        const std::complex<double> zi = std::polar(1.0, -2 * std::numbers::pi * f / rate);   // z^-1
        return rate * (3.0 - 4.0 * zi + zi * zi) / 2.0;
    }

    double oneXError(const System& system, const Pivots& pivots, const Plan& plan, int blockIndex, double rate,
                     Method method)
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
                const double h =
                    std::abs(analog(system, pivots, plan, blockIndex, discreteS(method, freqs[i], rate), pots));
                worst = std::max(worst, std::fabs(20 * std::log10(h + 1e-300) - analogDb[i]));
            }
        }
        return worst;
    }
} // namespace fminus::circuit
