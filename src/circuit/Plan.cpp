// src/circuit/Plan.cpp: elaboration, the netlist's checks, the partition into blocks at ideal op amps, their chain,
// the configuration and the oversampled region (Plan.h).
#include "circuit/Plan.h"

#include "circuit/Dc.h"
#include "circuit/Emit.h"
#include "circuit/Linear.h"
#include "circuit/Mna.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <format>
#include <limits>
#include <map>
#include <numeric>
#include <set>
#include <string_view>
#include <utility>

namespace fminus::circuit
{
    double thermalVoltage(double n)
    {
        constexpr double kBoltzmann = 1.380649e-23;   // J/K (CODATA 2018, exact)
        constexpr double kCharge = 1.602176634e-19;   // C (exact)
        constexpr double kTnom = 300.15;              // SPICE's TNOM, 27 C
        return n * kBoltzmann * kTnom / kCharge;
    }

    namespace
    {
        // Whether `name` is `prefix` followed by one digit or more.
        bool numbered(std::string_view name, char prefix)
        {
            return name.size() >= 2 && name[0] == prefix &&
                   std::all_of(name.begin() + 1, name.end(), [](char c) { return c >= '0' && c <= '9'; });
        }
    } // namespace

    namespace
    {
        // FNV-1a 64 over `text`.
        std::uint64_t fnv1a(std::string_view text)
        {
            std::uint64_t h = 0xcbf29ce484222325ull;
            for (const char c : text)
            {
                h ^= static_cast<unsigned char>(c);
                h *= 0x100000001b3ull;
            }
            return h;
        }

        std::string solverText(const Solver& s)
        {
            return std::string(s.guess == Solver::Guess::Omega ? "omega(" : "newton(") + std::to_string(s.steps) + ")";
        }
    } // namespace

    std::string keyText(const Netlist& netlist, const Options& options)
    {
        std::string text = "fminus-circuit-selftest " + std::to_string(kSelfTestVersion) + "\n";
        for (const std::string& line : netlist.canonical)
            text += line + "\n";
        // the Options that change the answer (moduleName, channels and chosen do not)
        text += "options";
        if (options.oversample)
            text += " oversample=" + std::to_string(*options.oversample);
        if (options.halfband)
            text += std::string(" halfband=") + (*options.halfband == Halfband::Iir ? "iir" : "fir");
        if (options.region)
            text += std::string(" region=") + (*options.region == Region::Auto        ? "auto"
                                               : *options.region == Region::Nonlinear ? "nonlinear"
                                                                                      : "all");
        if (options.solver)
            text += " solver=" + solverText(*options.solver);
        if (options.adaa)
            text += std::string(" adaa=") + (*options.adaa ? "on" : "off");
        if (options.method)
            text += std::string(" method=") + (*options.method == Method::Tr ? "tr" : "bdf2");
        if (options.prewarp)
            text += std::string(" prewarp=") + (*options.prewarp ? "on" : "off");
        return text + "\n";
    }

    std::string keyOf(const Netlist& netlist, const Options& options)
    {
        return std::format("{:016x}", fnv1a(keyText(netlist, options)));
    }

    std::string configurationText(int oversample, Halfband halfband, bool adaa, const Solver& solver,
                                  bool withHalfband)
    {
        std::string text = std::to_string(oversample) + "x";
        if (withHalfband)
            text += halfband == Halfband::Iir ? " IIR" : " FIR";
        if (adaa)
            text += ", ADAA";
        return text + ", " + solverText(solver);
    }

    bool isReservedName(std::string_view name)
    {
        if (isFMinusKeyword(name))
            return true;
        // the methods, the step's locals (an exp-affine port's; an asymmetric pair's conducting side's Is K, 1 / N Vt,
        // log and N Vt, its two exponentials and ADAA's second; then an OTA's: kd g, its slope and its 2 Vt multiple,
        // Newton's tanh, the tanh at v and the current (or ADAA's mean of it), ADAA's log cosh at v, its half step in
        // x, its square, sinh(h), z, 1 + z, the log cosh difference and the midpoint's p - v), and os1, the one field
        // without a '_' (tick's region sample)
        static constexpr std::array<std::string_view, 44> kModule{
            "init", "clear", "set", "tick", "step", "p",  "q",  "arg", "w",   "v",  "lo", "hi", "e",  "ei", "f",
            "d",    "ec",    "c",   "dp",   "vm",   "vb", "sa", "si",  "sl",  "sn", "ef", "er", "cr", "ua", "kg",
            "kgs",  "kgv",   "tn",  "th",   "ib",   "lc", "hh", "h2",  "s2",  "z",  "zu", "dl", "wm", "os1"};
        if (std::find(kModule.begin(), kModule.end(), name) != kModule.end())
            return true;
        // b<N> (a block's step), y<k> and z<k> (tick's locals)
        return numbered(name, 'b') || numbered(name, 'y') || numbered(name, 'z');
    }

    bool isModuleWord(std::string_view name)
    {
        if (isReservedName(name))
            return true;
        // init's parameter, the attribute's words, the halfband methods' parameters and locals, clear's loop
        // variable, and set's temporaries t<N>: FMinus reads a module's name as a type everywhere in the program
        // and the guard's fields and locals (ck_, fd_, fdstep_, sum_, gain_), a switch's ramp step (swstep_; its
        // other names, sw_<S>, to_<S> and BDF2's h_<C>, carry the part's name, as the coefficients do) and a matched
        // block's output (ym_)
        static constexpr std::array<std::string_view, 19> kOther{
            "rate", "clang", "nonblocking", "x",   "o",   "s0",      "s1",   "t",     "y",       "v0",
            "v1",   "i",     "ck_",         "fd_", "ym_", "fdstep_", "sum_", "gain_", "swstep_"};
        return std::find(kOther.begin(), kOther.end(), name) != kOther.end() || numbered(name, 't');
    }

    namespace
    {
        class UnionFind
        {
        public:
            explicit UnionFind(std::size_t n)
                : parent_(n)
            {
                std::iota(parent_.begin(), parent_.end(), 0);
            }

            std::size_t find(std::size_t x)
            {
                while (parent_[x] != x)
                {
                    parent_[x] = parent_[parent_[x]];
                    x = parent_[x];
                }
                return x;
            }

            void join(std::size_t a, std::size_t b)
            {
                a = find(a);
                b = find(b);
                if (a != b)
                    parent_[std::max(a, b)] = std::min(a, b);
            }

        private:
            std::vector<std::size_t> parent_;
        };

        std::string identOf(std::string_view name)
        {
            std::string out(name);
            for (char& c : out)
            {
                const bool ok = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_';
                if (!ok)
                    c = '_';
            }
            return out;
        }

        std::string unique(std::string ident, std::set<std::string>& taken)
        {
            if (taken.insert(ident).second)
                return ident;
            for (int k = 2;; ++k)
            {
                std::string candidate = ident + "_" + std::to_string(k);
                if (taken.insert(candidate).second)
                    return candidate;
            }
        }

        std::string baseNameOf(std::string_view path)
        {
            const std::size_t slash = path.find_last_of("/\\");
            return std::string(slash == std::string_view::npos ? path : path.substr(slash + 1));
        }

        std::string partsText(const Plan& plan, const Block& block)
        {
            std::string out;
            for (int e : block.elements)
                out += (out.empty() ? "" : ", ") + plan.netlist.elements[static_cast<std::size_t>(e)].name;
            return out;
        }

        // One element's branch (or the pieces of a pot), before the partition.
        struct Piece
        {
            Piece() = default;

            Piece(Branch b)   // NOLINT: a branch alone is a piece
                : branch(std::move(b))
            {
            }

            Branch branch;
            bool port = false;    // a port's: a diode's or a pair's (anode, cathode), or an OTA's (n+, n-)
            bool ota = false;     // an OTA's: no DC path through it; its four nodes join one block
            int controlled = -1;  // a G's or an E's that is not driven (into the plan's list): its four nodes join
            std::vector<int> nodes;   // an OTA's, a G's or an E's n+, n-, nc+, nc-
            int pair = -1;        // which port

            // Whether its nodes join one block (an OTA's, a G's, a floating E's), rather than a branch's two.
            bool joinsAll() const
            {
                return ota || controlled >= 0;
            }
        };

        // A node's name as SPICE compares it: folded, and gnd is 0.
        std::string nodeKey(std::string_view name)
        {
            std::string key = folded(name);
            return key == "gnd" ? "0" : key;
        }

        // The model a diode names: a .model card's, else a built-in's (1N914, 1N4148).
        std::optional<ModelCard> modelNamed(const Netlist& netlist, const std::string& name)
        {
            for (const ModelCard& m : netlist.models)
            {
                if (folded(m.name) == folded(name))
                    return m;
            }
            for (const ModelCard& m : builtinModels())
            {
                if (folded(m.name) == folded(name))
                    return m;
            }
            return std::nullopt;
        }

        // A diode port, before its nodes are numbered: its strings each way (element indices, anode to cathode),
        // and their models (one diode's each).
        struct DiodeGroup
        {
            std::vector<int> forward;   // the port's direction: its anode is the first's anode
            std::vector<int> reverse;   // the antiparallel string, or none
            ModelCard fwd;
            ModelCard rev;
        };

        // A list of names: "D1+D2".
        std::string joinedNames(const std::vector<Element>& elements, const std::vector<int>& list, const char* by)
        {
            std::string out;
            for (int e : list)
                out += (out.empty() ? "" : by) + elements[static_cast<std::size_t>(e)].name;
            return out;
        }

        // The diodes, elaborated on the netlist's node names (so a resistor RS needs can be written into it before
        // the nodes are numbered, as the netlist would write it): each diode's model (a card's, or a built-in's);
        // series strings, n diodes of one model through nodes nothing else touches (not .input's or .output's), each
        // one diode of n N; antiparallel strings paired; and a model's RS as a resistor RS_<D> to a new node rs_<D> at
        // the port's anode (one for a pair, whose two RS must be equal: the usual approximation, said in a note).
        // `absorbed` gets the strings' inner nodes, by key.
        std::vector<DiodeGroup> elaborateDiodes(Netlist& netlist, Messages& messages, std::set<std::string>& absorbed)
        {
            std::vector<Element>& elements = netlist.elements;
            const std::size_t count = elements.size();
            std::vector<std::optional<ModelCard>> model(count);
            std::vector<int> diodes;
            for (std::size_t e = 0; e < count; ++e)
            {
                const Element& el = elements[e];
                if (el.kind != Element::Kind::Diode)
                    continue;
                model[e] = modelNamed(netlist, el.model);
                if (!model[e])
                {
                    std::string list;
                    for (const ModelCard& m : netlist.models)
                        list += (list.empty() ? "" : ", ") + m.name;
                    std::string builtin;
                    for (const ModelCard& m : builtinModels())
                        builtin += (builtin.empty() ? "" : ", ") + m.name;
                    messages.error(el.modelLoc, "unknown model '" + el.model + "' (" +
                                                    (list.empty() ? std::string("no .model cards")
                                                                  : "the models: " + list) +
                                                    "; built in: " + builtin + ")");
                    continue;
                }
                if (nodeKey(el.nodes[0]) == nodeKey(el.nodes[1]))
                {
                    messages.error(el.loc, el.name + "'s anode and cathode are one node, '" + el.nodes[0] + "'");
                    continue;
                }
                diodes.push_back(static_cast<int>(e));
            }
            // each node's terminals: (element, which)
            std::map<std::string, std::vector<std::pair<int, int>>> terminals;
            for (std::size_t e = 0; e < count; ++e)
            {
                const Element& el = elements[e];
                for (std::size_t k = 0; k < el.nodes.size(); ++k)
                    terminals[nodeKey(el.nodes[k])].emplace_back(static_cast<int>(e), static_cast<int>(k));
                if (el.kind == Element::Kind::Pot && el.nodes.size() == 3)
                    terminals[nodeKey(el.nodes[1])].emplace_back(static_cast<int>(e), 1);
            }
            const std::string inputKey = netlist.input ? nodeKey(netlist.input->node) : std::string();
            const std::string outputKey = netlist.output ? nodeKey(netlist.output->node) : std::string();
            auto known = [&](int e)
            {
                return std::find(diodes.begin(), diodes.end(), e) != diodes.end();
            };
            // strings: a node that two diodes alone touch, one's cathode and the other's anode
            std::vector<int> next(count, -1);
            std::vector<int> prev(count, -1);
            for (const auto& [key, list] : terminals)
            {
                if (key == "0" || key == inputKey || key == outputKey || list.size() != 2)
                    continue;
                const auto [e0, k0] = list[0];
                const auto [e1, k1] = list[1];
                if (e0 == e1 || !known(e0) || !known(e1) || k0 == k1)
                    continue;
                const int from = k0 == 1 ? e0 : e1;   // its cathode is the node
                const int to = k0 == 1 ? e1 : e0;     // its anode is
                const Element& a = elements[static_cast<std::size_t>(from)];
                const Element& b = elements[static_cast<std::size_t>(to)];
                if (folded(a.model) != folded(b.model))
                {
                    messages.error(b.loc, "diodes " + a.name + " and " + b.name + " in series through node '" +
                                              a.nodes[1] + "' have different models (" + a.model + ", " + b.model +
                                              "): a series string is of one model");
                    continue;
                }
                next[static_cast<std::size_t>(from)] = to;
                prev[static_cast<std::size_t>(to)] = from;
            }
            struct Chain
            {
                std::vector<int> d;
                std::string anode;   // keys
                std::string cathode;
            };
            std::vector<Chain> chains;
            std::vector<bool> done(count, false);
            for (int start : diodes)
            {
                if (prev[static_cast<std::size_t>(start)] >= 0)
                    continue;
                Chain c;
                for (int at = start; at >= 0 && !done[static_cast<std::size_t>(at)];
                     at = next[static_cast<std::size_t>(at)])
                {
                    done[static_cast<std::size_t>(at)] = true;
                    c.d.push_back(at);
                }
                chains.push_back(c);
            }
            for (int d : diodes)   // a ring of diodes, each node touched by two of them alone: each its own
            {
                if (!done[static_cast<std::size_t>(d)])
                    chains.push_back({{d}, {}, {}});
            }
            std::stable_sort(chains.begin(), chains.end(), [](const Chain& a, const Chain& b) { return a.d < b.d; });
            for (Chain& c : chains)
            {
                c.anode = nodeKey(elements[static_cast<std::size_t>(c.d.front())].nodes[0]);
                c.cathode = nodeKey(elements[static_cast<std::size_t>(c.d.back())].nodes[1]);
                for (std::size_t k = 0; k + 1 < c.d.size(); ++k)
                    absorbed.insert(nodeKey(elements[static_cast<std::size_t>(c.d[k])].nodes[1]));
            }
            // antiparallel strings pair, the first of each in netlist order
            std::vector<DiodeGroup> groups;
            std::vector<bool> paired(chains.size(), false);
            for (std::size_t i = 0; i < chains.size(); ++i)
            {
                if (paired[i])
                    continue;
                DiodeGroup g;
                g.forward = chains[i].d;
                g.fwd = *model[static_cast<std::size_t>(g.forward.front())];
                for (std::size_t j = i + 1; j < chains.size(); ++j)
                {
                    if (!paired[j] && chains[j].anode == chains[i].cathode && chains[j].cathode == chains[i].anode)
                    {
                        paired[j] = true;
                        g.reverse = chains[j].d;
                        g.rev = *model[static_cast<std::size_t>(g.reverse.front())];
                        break;
                    }
                }
                paired[i] = true;
                groups.push_back(std::move(g));
            }
            // RS: a resistor to a new node at the port's anode
            std::set<std::string> nodeKeys{"0"};
            std::set<std::string> elementKeys;
            for (const Element& el : elements)
            {
                elementKeys.insert(folded(el.name));
                for (const std::string& n : el.nodes)
                    nodeKeys.insert(nodeKey(n));
            }
            auto fresh = [](std::set<std::string>& taken, const std::string& name, bool node)
            {
                std::string candidate = name;
                for (int k = 2; taken.count(node ? nodeKey(candidate) : folded(candidate)); ++k)
                    candidate = name + "_" + std::to_string(k);
                taken.insert(node ? nodeKey(candidate) : folded(candidate));
                return candidate;
            };
            for (DiodeGroup& g : groups)
            {
                const double rsF = static_cast<double>(g.forward.size()) * g.fwd.rs;
                const double rsR = g.reverse.empty() ? rsF : static_cast<double>(g.reverse.size()) * g.rev.rs;
                const std::string parts = joinedNames(elements, g.forward, "+") +
                                          (g.reverse.empty() ? "" : "|" + joinedNames(elements, g.reverse, "+"));
                if (rsF != rsR)
                {
                    messages.error(elements[static_cast<std::size_t>(g.reverse.front())].loc,
                                   std::format("antiparallel {} have unequal RS ({:g} and {:g} ohm): a pair shares one "
                                               "series resistor",
                                               parts, rsF, rsR));
                    continue;
                }
                if (rsF == 0)
                    continue;
                Element& first = elements[static_cast<std::size_t>(g.forward.front())];
                const std::string anode = first.nodes[0];
                const std::string node = fresh(nodeKeys, "rs_" + first.name, true);
                Element r;
                r.kind = Element::Kind::Resistor;
                r.name = fresh(elementKeys, "RS_" + first.name, false);
                r.loc = first.loc;
                r.nodes = {anode, node};
                r.nodeLocs = {first.nodeLocs[0], first.nodeLocs[0]};
                r.value = rsF;
                r.valueLoc = first.modelLoc;
                r.text = r.name + " " + anode + " " + node + " " + std::format("{:g}", rsF);
                first.nodes[0] = node;
                if (!g.reverse.empty())
                    elements[static_cast<std::size_t>(g.reverse.back())].nodes[1] = node;
                if (!g.reverse.empty())
                    messages.note({}, std::format("{}: RS {:g} ohm is one series resistor, {}, for the pair (each "
                                                  "diode's RS where it conducts: the usual approximation)",
                                                  parts, rsF, r.name));
                elements.push_back(std::move(r));
            }
            return groups;
        }
    } // namespace

    double LawInfo::middle() const
    {
        return low > 0 ? std::sqrt(low * high) : 0.5 * (low + high);
    }

    double gainAt(const Plan& plan, const PortDef& port, const std::vector<double>& pots,
                  const std::vector<double>& laws)
    {
        if (!port.gain)
            return port.gainValue;
        return evaluate(*port.gain,
                        [&](const std::string& key)
                        {
                            for (std::size_t i = 0; i < plan.laws.size(); ++i)
                            {
                                if (plan.laws[i].key != key)
                                    continue;
                                const LawInfo& l = plan.laws[i];
                                const double x = i < laws.size() ? laws[i] : l.low;
                                // as tick clamps it: fmaxf, then fminf (a NaN is the low end)
                                return std::fmin(std::fmax(x, l.low), l.high);
                            }
                            return std::clamp(potValue(plan, pots, key), 0.0, 1.0);
                        });
    }

    double prewarped(const PortDef& port, double g, double designRate)
    {
        if (!port.prewarp)
            return g;
        // pw<N>_'s operations: pws = 2 fs', pwk = 1 / pws, and a product by 1 / b
        const double pws = 2 * designRate;
        const double pwk = 1 / pws;
        const double pole = port.poleA + port.poleB * g;
        const double w = std::fmin(std::fabs(pole) * pwk, 1.5);
        const double t = pws * std::tan(w);
        return ((pole < 0 ? -t : t) - port.poleA) * (1 / port.poleB);
    }

    std::vector<double> defaultPoint(const Plan& plan)
    {
        std::vector<double> point;
        for (const PotInfo& p : plan.pots)
            point.push_back(p.defaultValue);
        for (const SwitchInfo& w : plan.switches)
            point.push_back(w.defaultPosition);
        return point;
    }

    std::vector<std::vector<double>> potGrid(const Plan& plan, const Block& block)
    {
        const std::vector<double> defaults = defaultPoint(plan);
        // the axes: (index into the point, its values)
        std::vector<std::pair<std::size_t, std::vector<double>>> axes;
        for (const std::string& key : block.pots)
        {
            for (std::size_t i = 0; i < plan.pots.size(); ++i)
            {
                if (plan.pots[i].key == key)
                    axes.push_back({i, {0.0, 0.5, 1.0}});
            }
        }
        for (const std::string& key : block.switches)
        {
            for (std::size_t i = 0; i < plan.switches.size(); ++i)
            {
                if (plan.switches[i].key != key)
                    continue;
                std::vector<double> positions;
                for (int k = 0; k < plan.switches[i].positions; ++k)
                    positions.push_back(k);
                axes.push_back({plan.pots.size() + i, positions});
            }
        }
        std::vector<std::vector<double>> grid;
        if (axes.size() <= 3)
        {
            std::size_t count = 1;
            for (const auto& axis : axes)
                count *= axis.second.size();
            for (std::size_t k = 0; k < count; ++k)
            {
                std::vector<double> point = defaults;
                std::size_t rest = k;
                for (const auto& [i, values] : axes)
                {
                    point[i] = values[rest % values.size()];
                    rest /= values.size();
                }
                grid.push_back(point);
            }
            return grid;
        }
        for (const auto& [i, values] : axes)
        {
            for (double v : values)
            {
                std::vector<double> point = defaults;
                point[i] = v;
                grid.push_back(point);
            }
        }
        for (bool high : {false, true})
        {
            std::vector<double> point = defaults;
            for (const auto& [i, values] : axes)
                point[i] = high ? values.back() : values.front();
            grid.push_back(point);
        }
        return grid;
    }

    double potValue(const Plan& plan, const std::vector<double>& point, const std::string& key)
    {
        for (std::size_t i = 0; i < plan.pots.size(); ++i)
        {
            if (plan.pots[i].key == key)
                return i < point.size() ? point[i] : plan.pots[i].defaultValue;
        }
        return 0;
    }

    int switchPosition(const Plan& plan, const std::vector<double>& point, const std::string& key)
    {
        for (std::size_t i = 0; i < plan.switches.size(); ++i)
        {
            if (plan.switches[i].key != key)
                continue;
            const std::size_t at = plan.pots.size() + i;
            return at < point.size() ? static_cast<int>(point[at]) : plan.switches[i].defaultPosition;
        }
        return 0;
    }

    double switchRamp(const Branch& branch, int position)
    {
        return std::find(branch.closed.begin(), branch.closed.end(), position) != branch.closed.end() ? 1.0 : 0.0;
    }

    double switchResistance(const Branch& branch, double s)
    {
        const double lnOff = std::log(branch.roff);
        return std::exp(lnOff + s * (std::log(branch.ron) - lnOff));
    }

    namespace
    {
        // The smallest and largest gain of an OTA port over its pots' and law inputs' grid (gainAt's, at each pot's
        // 0, 0.5 and 1 and each law input's low end, middle and high end).
        std::pair<double, double> gainRange(const Plan& p, const Block& b)
        {
            const PortDef& port = *b.port;
            std::vector<std::size_t> axes;
            for (std::size_t i = 0; i < p.laws.size(); ++i)
            {
                if (std::find(port.gainLaws.begin(), port.gainLaws.end(), p.laws[i].key) != port.gainLaws.end())
                    axes.push_back(i);
            }
            Block gainBlock = b;
            gainBlock.pots = port.gainPots;
            std::size_t points = 1;
            for (std::size_t i = 0; i < axes.size(); ++i)
                points *= 3;
            double lo = std::numeric_limits<double>::infinity();
            double hi = -lo;
            for (const std::vector<double>& pots : potGrid(p, gainBlock))
            {
                for (std::size_t k = 0; k < points; ++k)
                {
                    std::vector<double> laws;
                    for (const LawInfo& l : p.laws)
                        laws.push_back(l.low);
                    std::size_t rest = k;
                    for (std::size_t i : axes)
                    {
                        const double values[] = {p.laws[i].low, p.laws[i].middle(), p.laws[i].high};
                        laws[i] = values[rest % 3];
                        rest /= 3;
                    }
                    const double g = gainAt(p, port, pots, laws);
                    lo = std::min(lo, g);
                    hi = std::max(hi, g);
                }
            }
            return {lo, hi};
        }

        // The analog small-signal pole of a one-state OTA block at gain g, rad/s: its trapezoidal form at 48 kHz with
        // the port linearised (v (1 + kd g / 2 Vt) = p), z = A + W cp / (1 + kd g / 2 Vt), mapped back through the
        // bilinear transform, s = 2 fs (z - 1) / (z + 1), which the trapezoidal rule is, exactly, on a linear network.
        double smallSignalPole(const Plan& p, int block, double g)
        {
            constexpr double kRate = 48000;
            std::vector<double> defaults;
            for (const PotInfo& pot : p.pots)
                defaults.push_back(pot.defaultValue);
            const Holters h = holters(p, block, kRate, defaults);
            const double z = h.a[0][0] + h.w[0] * h.cp[0] / (1 + h.k * g / h.nvt);
            return 2 * kRate * (z - 1) / (z + 1);
        }

        // .prewarp on: each OTA in a one-state block (one capacitor or inductor) without pots in its network (no pot,
        // no {expr} resistor, no E or G gain of pots) gets its pole's affine fit, a + b g from the ends of its gain's
        // range, checked at the middle (a residual of 1e-6 at most, else a warning that prewarp is ignored for it);
        // any other OTA, or a netlist without one, warns that prewarp is ignored.
        void fitPrewarp(Plan& p, const std::vector<System>& systems, Messages& messages)
        {
            const std::vector<Element>& elements = p.netlist.elements;
            const Loc directive = p.config.prewarpFrom == "the netlist" ? p.netlist.prewarp.loc : Loc{};
            bool any = false;
            for (std::size_t i = 0; i < p.blocks.size(); ++i)
            {
                Block& b = p.blocks[i];
                if (!b.port || !b.port->smooth())
                    continue;
                any = true;
                PortDef& port = *b.port;
                const Loc loc = elements[static_cast<std::size_t>(port.elements.front())].loc;
                // its states (capacitors and inductors), and whether pots move its network (a pot, a resistor's
                // {expr}, an E's or a G's gain)
                int states = 0;
                bool variable = false;
                for (const Branch& br : b.branches)
                {
                    states += br.state() ? 1 : 0;
                    variable = variable || br.kind == Branch::Kind::Variable;
                }
                for (const Controlled& c : b.controlled)
                    variable = variable || c.expr.has_value();
                if (states != 1)
                {
                    messages.warning(loc, std::format("prewarp is ignored for OTA {}: its block has {} states (it "
                                                      "prewarps a one-pole's cutoff)",
                                                      port.parts, states));
                    continue;
                }
                if (variable)
                {
                    messages.warning(loc, "prewarp is ignored for OTA " + port.parts +
                                              ": pots move its block's pole, and the fit is made at generation");
                    continue;
                }
                if (b.method == Method::Bdf2)
                {
                    messages.warning(loc, "prewarp is ignored for OTA " + port.parts +
                                              ": it places the trapezoidal rule's pole, and its block is BDF2 "
                                              "(.method bdf2)");
                    continue;
                }
                (void)systems;
                auto [lo, hi] = gainRange(p, b);
                if (!(hi > lo))
                {
                    lo = 0.5 * hi;
                    hi = 2 * hi;
                }
                if (!(hi > 0))
                {
                    messages.warning(loc, "prewarp is ignored for OTA " + port.parts + ": its gain is 0");
                    continue;
                }
                double s1 = 0;
                double s2 = 0;
                double s3 = 0;
                try
                {
                    s1 = smallSignalPole(p, static_cast<int>(i), lo);
                    s2 = smallSignalPole(p, static_cast<int>(i), 0.5 * (lo + hi));
                    s3 = smallSignalPole(p, static_cast<int>(i), hi);
                }
                catch (const std::exception&)
                {
                    continue;   // a singular block: deriveBlock reports it
                }
                const double bCoef = (s3 - s1) / (hi - lo);
                const double aCoef = s1 - bCoef * lo;
                const double residual = std::fabs(aCoef + bCoef * 0.5 * (lo + hi) - s2) / std::fabs(s2);
                if (!(residual <= 1e-6) || !(bCoef != 0))
                {
                    messages.warning(loc, std::format("prewarp is ignored for OTA {}: its pole is not affine in its "
                                                      "gain (a residual of {:.2g} at its middle)",
                                                      port.parts, residual));
                    continue;
                }
                port.prewarp = true;
                port.poleA = aCoef;
                port.poleB = bCoef;
                messages.note({},
                              std::format("OTA {}: prewarp; its small-signal pole is a + b g, a = {:.7g} rad/s, b = "
                                          "{:.7g} rad/s per A (g {:g} to {:g} A)",
                                          port.parts, aCoef, bCoef, lo, hi));
            }
            if (!any)
                messages.warning(directive,
                                 "'.prewarp on' is ignored: it prewarps an OTA's cutoff, and the netlist has "
                                 "none");
        }
    } // namespace

    std::optional<Plan> plan(const Netlist& netlist, const Options& options, std::string_view fileName,
                             Messages& messages)
    {
        Plan p;
        p.fileName = std::string(fileName);
        p.baseName = baseNameOf(fileName);
        p.netlist = netlist;
        const std::vector<Element>& elements = p.netlist.elements;

        // ---- the module's name ----
        {
            std::string name = options.moduleName;
            Loc loc{1, 1};
            std::string from = "Options::moduleName";
            if (name.empty() && !netlist.title.empty())
            {
                name = netlist.title;
                loc = netlist.titleLoc;
                from = ".title";
            }
            if (name.empty())
            {
                std::string stem = p.baseName;
                const std::size_t dot = stem.rfind('.');
                if (dot != std::string::npos && dot > 0)
                    stem = stem.substr(0, dot);
                if (!stem.empty() && stem[0] >= 'a' && stem[0] <= 'z')
                    stem[0] = static_cast<char>(stem[0] - 'a' + 'A');
                name = stem;
                loc = {};
                from = "the file's name (add .title)";
            }
            if (!isPlainName(name))
                messages.error(loc, "the module's name '" + name + "', from " + from +
                                        ", is not a letter followed by letters and digits");
            else if (isFMinusKeyword(name))
                messages.error(loc, "the module's name '" + name + "' is an FMinus keyword or builtin");
            else if (isModuleWord(name))
                messages.error(loc, "the module's name '" + name + "', from " + from +
                                        ", is a name the module uses: rename it");
            p.module = name;
        }

        // ---- diodes: models, series strings, pairs, and RS as a resistor written into the netlist ----
        std::set<std::string> absorbedKeys;
        const std::vector<DiodeGroup> diodeGroups = elaborateDiodes(p.netlist, messages, absorbedKeys);

        // ---- nodes ----
        p.nodes.push_back({"0", "0", Node::Source::None, -1, 0});
        std::map<std::string, int> nodeOf;
        std::set<std::string> nodeIdents{"0"};
        auto node = [&](const std::string& name)
        {
            const std::string key = folded(name);
            if (key == "0" || key == "gnd")
                return 0;
            if (const auto it = nodeOf.find(key); it != nodeOf.end())
                return it->second;
            const int index = static_cast<int>(p.nodes.size());
            p.nodes.push_back({name, unique(identOf(name), nodeIdents), Node::Source::None, -1, 0});
            nodeOf.emplace(key, index);
            return index;
        };
        auto existing = [&](const std::string& name)
        {
            const std::string key = folded(name);
            if (key == "0" || key == "gnd")
                return 0;
            const auto it = nodeOf.find(key);
            return it == nodeOf.end() ? -1 : it->second;
        };
        std::vector<std::vector<int>> elementNodes(elements.size());
        for (std::size_t e = 0; e < elements.size(); ++e)
        {
            for (const std::string& n : elements[e].nodes)
                elementNodes[e].push_back(node(n));
        }

        // ---- .input, .output ----
        if (!netlist.input)
            messages.error({}, "the netlist has no .input");
        if (!netlist.output)
            messages.error({}, "the netlist has no .output");
        if (netlist.input)
        {
            p.inputName = netlist.input->name;
            p.inputScale = netlist.input->scale;
            p.inputNode = existing(netlist.input->node);
            if (p.inputNode < 0)
                messages.error(netlist.input->nodeLoc,
                               ".input's node '" + netlist.input->node + "' is not in the netlist");
            else if (p.inputNode == 0)
                messages.error(netlist.input->nodeLoc, ".input's node is ground");
            else
            {
                p.nodes[static_cast<std::size_t>(p.inputNode)].source = Node::Source::Input;
                ++p.nodes[static_cast<std::size_t>(p.inputNode)].connections;
            }
            if (isReservedName(netlist.input->name))
                messages.error(netlist.input->loc, ".input '" + netlist.input->name +
                                                       "' is an FMinus keyword or builtin, or a name the module uses: "
                                                       "rename it");
            else if (netlist.input->name == p.module)   // FMinus's names keep their case
                messages.error(netlist.input->loc, ".input '" + netlist.input->name + "' is the module's name");
        }
        if (netlist.output)
        {
            p.outputScale = netlist.output->scale;
            p.outputNode = existing(netlist.output->node);
            if (p.outputNode < 0)
                messages.error(netlist.output->nodeLoc,
                               ".output's node '" + netlist.output->node + "' is not in the netlist");
            else if (p.outputNode == 0)
                messages.error(netlist.output->nodeLoc, ".output's node is ground");
        }

        // ---- pots ----
        for (const PotLine& line : netlist.pots)
        {
            PotInfo info{line.name, folded(line.name), line.defaultValue, line.taper, false};
            if (isReservedName(line.name))
                messages.error(line.loc, "pot '" + line.name +
                                             "' is an FMinus keyword or builtin, or a name the module uses: rename it");
            else if (netlist.input && line.name == netlist.input->name)   // exact: FMinus's names keep their case
                messages.error(line.loc, "pot '" + line.name + "' has .input's name");
            else if (line.name == p.module)
                messages.error(line.loc, "pot '" + line.name + "' is the module's name");
            p.pots.push_back(info);
        }
        // ---- switches: set()'s ints after the pots ----
        for (const SwitchLine& line : netlist.switches)
        {
            SwitchInfo info{line.name, folded(line.name), line.positions, line.defaultPosition, line.loc, false};
            if (isReservedName(line.name))
                messages.error(line.loc, "switch '" + line.name +
                                             "' is an FMinus keyword or builtin, or a name the module uses: rename it");
            else if (line.name == p.module)
                messages.error(line.loc, "switch '" + line.name + "' is the module's name");
            p.switches.push_back(info);
        }
        // ---- law inputs: tick's and step's floats after the signal ----
        for (const IoLine& line : netlist.laws)
        {
            LawInfo info;
            info.name = line.name;
            info.key = folded(line.name);
            info.kind = line.kind;
            info.scale = line.scale;
            info.low = line.low;
            info.high = line.high;
            info.loc = line.loc;
            if (isReservedName(line.name))
                messages.error(line.loc, "law input '" + line.name +
                                             "' is an FMinus keyword or builtin, or a name the module uses: rename it");
            else if (line.name == p.module)
                messages.error(line.loc, "law input '" + line.name + "' is the module's name");
            p.laws.push_back(info);
        }
        auto lawIndex = [&](const std::string& key)
        {
            for (std::size_t i = 0; i < p.laws.size(); ++i)
            {
                if (p.laws[i].key == key)
                    return static_cast<int>(i);
            }
            return -1;
        };
        auto potIndex = [&](const std::string& key)
        {
            for (std::size_t i = 0; i < p.pots.size(); ++i)
            {
                if (p.pots[i].key == key)
                    return static_cast<int>(i);
            }
            return -1;
        };

        // ---- elements: branches, models, tapers ----
        std::set<std::string> partIdents;
        std::vector<std::string> elementIdent(elements.size());
        for (std::size_t e = 0; e < elements.size(); ++e)
            elementIdent[e] = unique(identOf(elements[e].name), partIdents);
        std::vector<Piece> pieces;
        std::vector<PortDef> ports;   // the diodes' and the OTAs'
        std::vector<OpAmp> opamps;
        std::vector<Controlled> controlled;   // the E and linear G sources
        std::vector<std::size_t> voltages;    // the V sources
        for (std::size_t e = 0; e < elements.size(); ++e)
        {
            const Element& el = elements[e];
            const std::vector<int>& nn = elementNodes[e];
            for (int n : nn)
                ++p.nodes[static_cast<std::size_t>(n)].connections;
            if (el.kind == Element::Kind::Pot && nn.size() == 3)
                ++p.nodes[static_cast<std::size_t>(nn[1])].connections;   // a wiper joins two sections
            if (el.expr)
            {
                for (const std::string& key : potsOf(*el.expr))
                {
                    if (const int i = potIndex(key); i >= 0)
                        p.pots[static_cast<std::size_t>(i)].used = true;
                    if (const int i = lawIndex(key); i >= 0)
                        p.laws[static_cast<std::size_t>(i)].used = true;
                }
            }
            Branch b;
            b.part = el.name;
            b.ident = elementIdent[e];
            b.partIdent = elementIdent[e];
            b.element = static_cast<int>(e);
            switch (el.kind)
            {
            case Element::Kind::Resistor:
                b.a = nn[0];
                b.b = nn[1];
                if (el.expr)
                {
                    b.kind = Branch::Kind::Variable;
                    b.section = 0;
                    b.expr = *el.expr;
                }
                else
                {
                    b.kind = Branch::Kind::Conductance;
                    b.g = 1 / el.value;
                }
                pieces.push_back({b});
                break;
            case Element::Kind::Capacitor:
                b.kind = Branch::Kind::Capacitor;
                b.a = nn[0];
                b.b = nn[1];
                b.c = el.value;
                pieces.push_back({b});
                break;
            case Element::Kind::Pot:
            {
                // The taper: the line's taper=, else the .pot's when {expr} is that pot alone, else lin.
                Taper taper = Taper::Lin;
                std::optional<Taper> potTaper;
                if (el.expr->op == Expr::Op::Name)
                {
                    if (const int i = potIndex(el.expr->pot); i >= 0)
                        potTaper = p.pots[static_cast<std::size_t>(i)].taper;
                }
                if (el.taper)
                {
                    taper = *el.taper;
                    if (potTaper && *potTaper != *el.taper)
                        messages.warning(el.taperLoc, el.name + "'s taper=" + taperName(*el.taper) +
                                                          " differs from pot '" + el.expr->spelling + "''s " +
                                                          taperName(*potTaper) + ": taper=" + taperName(*el.taper) +
                                                          " is used");
                }
                else if (potTaper)
                    taper = *potTaper;
                b.kind = Branch::Kind::Variable;
                b.value = el.value;
                b.expr = *el.expr;
                b.taper = taper;
                if (nn.size() == 3)
                {
                    // R(e1-w) = (1 - t) value, R(w-e2) = t value
                    Branch first = b;
                    first.a = nn[0];
                    first.b = nn[1];
                    first.section = 1;
                    first.ident = unique(b.ident + "_a", partIdents);   // r_Ptone_a: unique, as every part's
                    Branch second = b;
                    second.a = nn[1];
                    second.b = nn[2];
                    second.section = 2;
                    second.ident = unique(b.ident + "_b", partIdents);
                    pieces.push_back({first});
                    pieces.push_back({second});
                }
                else
                {
                    b.a = nn[0];
                    b.b = nn[1];
                    b.section = 2;
                    pieces.push_back({b});
                }
                break;
            }
            case Element::Kind::Switch:
            {
                // a resistance that ramps between roff and ron: a variable branch (its current an unknown)
                b.kind = Branch::Kind::Variable;
                b.a = nn[0];
                b.b = nn[1];
                b.section = 3;
                b.switchKey = folded(el.model);
                b.closed = el.closed;
                b.ron = el.ron;
                b.roff = el.roff;
                for (SwitchInfo& w : p.switches)
                {
                    if (w.key == b.switchKey)
                        w.used = true;
                }
                pieces.push_back({b});
                break;
            }
            case Element::Kind::Diode:
                break;   // the diode groups', below
            case Element::Kind::Inductor:
                b.kind = Branch::Kind::Inductor;
                b.a = nn[0];
                b.b = nn[1];
                b.l = el.value;
                pieces.push_back({b});
                break;
            case Element::Kind::Voltage:
                voltages.push_back(e);   // grounded (a node driven) or floating (a branch), once the drivers are known
                break;
            case Element::Kind::Current:
                p.currents.push_back({el.name, static_cast<int>(e), nn[0], nn[1], el.value});
                break;
            case Element::Kind::Vcvs:
            case Element::Kind::Vccs:
            {
                Controlled c;
                c.kind = el.kind == Element::Kind::Vcvs ? Controlled::Kind::Vcvs : Controlled::Kind::Vccs;
                c.name = el.name;
                c.ident = elementIdent[e];
                c.element = static_cast<int>(e);
                c.p = nn[0];
                c.n = nn[1];
                c.cp = nn[2];
                c.cn = nn[3];
                c.gain = el.value;
                c.expr = el.expr;
                controlled.push_back(c);
                break;
            }
            case Element::Kind::OpAmp:
                opamps.push_back({el.name, nn[0], nn[1], nn[2], static_cast<int>(e)});
                break;
            case Element::Kind::Ota:
            {
                // i = g tanh((V(nc+) - V(nc-)) / 2 Vt), leaving n+ and entering n- (SPICE's G); a port of its own
                PortDef port;
                port.name = elementIdent[e];
                port.parts = el.name;
                port.law = PortDef::Law::Tanh;
                port.form = PortDef::Form::I;
                port.from = nn[0];
                port.to = nn[1];
                port.anode = nn[2];
                port.cathode = nn[3];
                port.nvt = 2 * thermalVoltage(1);
                port.gain = el.expr;
                port.gainValue = el.value;
                if (el.expr)
                {
                    const std::vector<std::string> names = potsOf(*el.expr);
                    for (const PotInfo& pot : p.pots)
                    {
                        if (std::find(names.begin(), names.end(), pot.key) != names.end())
                            port.gainPots.push_back(pot.key);
                    }
                    for (const LawInfo& law : p.laws)
                    {
                        if (std::find(names.begin(), names.end(), law.key) != names.end())
                            port.gainLaws.push_back(law.key);
                    }
                }
                port.elements = {static_cast<int>(e)};
                Piece piece;
                piece.branch.a = port.from;
                piece.branch.b = port.to;
                piece.branch.part = port.parts;
                piece.branch.element = static_cast<int>(e);
                piece.port = true;
                piece.ota = true;
                piece.nodes = nn;
                piece.pair = static_cast<int>(ports.size());
                pieces.push_back(piece);
                ports.push_back(port);
                break;
            }
            }
        }
        for (const LawInfo& law : p.laws)
        {
            if (!law.used)
                messages.warning(law.loc, "law input '" + law.name + "' is never used");
        }
        for (const PotInfo& pot : p.pots)
        {
            if (!pot.used)
            {
                for (const PotLine& line : netlist.pots)
                {
                    if (folded(line.name) == pot.key)
                        messages.warning(line.loc, "pot '" + pot.name + "' is never used");
                }
            }
        }
        for (const SwitchInfo& w : p.switches)
        {
            if (!w.used)
                messages.warning(w.loc, "switch '" + w.name + "' is never used");
        }

        // ---- diode ports: a single diode, a series string, a pair (or an asymmetric one) ----
        for (const std::string& key : absorbedKeys)
        {
            if (const auto it = nodeOf.find(key); it != nodeOf.end())
                p.nodes[static_cast<std::size_t>(it->second)].absorbed = true;
        }
        for (const DiodeGroup& g : diodeGroups)
        {
            PortDef port;
            const auto first = static_cast<std::size_t>(g.forward.front());
            port.name = elementIdent[first];
            port.parts = joinedNames(elements, g.forward, "+") +
                         (g.reverse.empty() ? "" : "|" + joinedNames(elements, g.reverse, "+"));
            port.anode = elementNodes[first][0];
            port.cathode = elementNodes[static_cast<std::size_t>(g.forward.back())][1];
            port.from = port.anode;   // a two-terminal port: its current flows anode to cathode, through it
            port.to = port.cathode;
            port.is = g.fwd.is;
            port.n = static_cast<double>(g.forward.size()) * g.fwd.n;
            port.nvt = thermalVoltage(port.n);
            if (g.reverse.empty())
                port.law = PortDef::Law::Single;
            else
            {
                port.is2 = g.rev.is;
                port.n2 = static_cast<double>(g.reverse.size()) * g.rev.n;
                port.nvt2 = thermalVoltage(port.n2);
                port.law = port.is2 == port.is && port.n2 == port.n ? PortDef::Law::Pair : PortDef::Law::Asym;
            }
            port.elements = g.forward;
            port.elements.insert(port.elements.end(), g.reverse.begin(), g.reverse.end());
            Piece piece;
            piece.branch.a = port.anode;
            piece.branch.b = port.cathode;
            piece.branch.part = port.parts;
            piece.branch.element = static_cast<int>(first);
            piece.port = true;
            piece.pair = static_cast<int>(ports.size());
            pieces.push_back(piece);
            ports.push_back(port);
        }

        // ---- connections ----
        for (std::size_t n = 1; n < p.nodes.size(); ++n)
        {
            if (p.nodes[n].connections >= 2)
                continue;
            for (std::size_t e = 0; e < elements.size(); ++e)
            {
                if (std::find(elementNodes[e].begin(), elementNodes[e].end(), static_cast<int>(n)) !=
                    elementNodes[e].end())
                {
                    messages.error(elements[e].loc, "node '" + p.nodes[n].name + "' has one connection (" +
                                                        elements[e].name + "); a node needs two");
                    break;
                }
            }
        }
        if (std::none_of(elementNodes.begin(), elementNodes.end(),
                         [](const std::vector<int>& nn) { return std::find(nn.begin(), nn.end(), 0) != nn.end(); }) &&
            !elements.empty())
            messages.error({}, "nothing connects to ground (0 or gnd)");

        auto nodeName = [&](int n)
        {
            return p.nodes[static_cast<std::size_t>(n)].name;
        };

        // ---- loops of voltage sources, E outputs and inductors (their DC current is undefined) ----
        {
            // .input's node and the op amps' outputs are sources to ground too: a V source, an E or an inductor
            // between two of them (or one and ground) closes a loop through them
            constexpr std::size_t kInput = std::numeric_limits<std::size_t>::max();   // .input's edge
            UnionFind loops(p.nodes.size());
            std::vector<std::vector<std::pair<int, std::size_t>>> tree(p.nodes.size());   // node -> (node, element)
            auto source = [&](int node, std::size_t edge)
            {
                if (node <= 0 || loops.find(static_cast<std::size_t>(node)) == loops.find(0))
                    return;
                loops.join(static_cast<std::size_t>(node), 0);
                tree[static_cast<std::size_t>(node)].emplace_back(0, edge);
                tree[0].emplace_back(node, edge);
            };
            source(p.inputNode, kInput);
            for (const OpAmp& op : opamps)
                source(op.out, static_cast<std::size_t>(op.element));
            // a V source or an E that the driven nodes' checks below refuse for where it sits on .input's node or an
            // op amp's output is left out, so it is not refused twice: a V source on .input's node, or grounded on an
            // op amp's output, or between that output and another driven node; an E driving either
            std::vector<int> opOuts;
            for (const OpAmp& op : opamps)
                opOuts.push_back(op.out);
            auto isOpOut = [&](int n)
            {
                return n > 0 && std::find(opOuts.begin(), opOuts.end(), n) != opOuts.end();
            };
            std::vector<int> drivenNodes = opOuts;   // and the grounded V sources' and E sources' outputs
            for (std::size_t e = 0; e < elements.size(); ++e)
            {
                const Element::Kind kind = elements[e].kind;
                if ((kind == Element::Kind::Voltage || kind == Element::Kind::Vcvs) &&
                    (elementNodes[e][0] == 0) != (elementNodes[e][1] == 0))
                    drivenNodes.push_back(elementNodes[e][0] == 0 ? elementNodes[e][1] : elementNodes[e][0]);
            }
            auto isDriven = [&](int n)
            {
                return n > 0 && (n == p.inputNode || std::find(drivenNodes.begin(), drivenNodes.end(), n) !=
                                                         drivenNodes.end());
            };
            auto refusedLater = [&](std::size_t e)
            {
                const int a = elementNodes[e][0];
                const int b = elementNodes[e][1];
                if (elements[e].kind == Element::Kind::Voltage)
                {
                    if (p.inputNode > 0 && (a == p.inputNode || b == p.inputNode))
                        return true;
                    if (a == 0 || b == 0)
                        return isOpOut(a == 0 ? b : a);
                    return (isOpOut(a) && isDriven(b)) || (isOpOut(b) && isDriven(a));
                }
                if (elements[e].kind == Element::Kind::Vcvs && (a == 0) != (b == 0))
                {
                    const int out = a == 0 ? b : a;
                    return (p.inputNode > 0 && out == p.inputNode) || isOpOut(out);
                }
                return false;
            };
            for (std::size_t e = 0; e < elements.size(); ++e)
            {
                const Element::Kind kind = elements[e].kind;
                if (kind != Element::Kind::Voltage && kind != Element::Kind::Inductor && kind != Element::Kind::Vcvs)
                    continue;
                if (refusedLater(e))
                    continue;
                const int a = elementNodes[e][0];
                const int b = elementNodes[e][1];
                if (loops.find(static_cast<std::size_t>(a)) != loops.find(static_cast<std::size_t>(b)))
                {
                    loops.join(static_cast<std::size_t>(a), static_cast<std::size_t>(b));
                    tree[static_cast<std::size_t>(a)].emplace_back(b, e);
                    tree[static_cast<std::size_t>(b)].emplace_back(a, e);
                    continue;
                }
                // the loop: the tree's path from a to b, then this element
                std::vector<int> from(p.nodes.size(), -2);
                std::vector<std::size_t> via(p.nodes.size(), 0);
                std::vector<int> queue{a};
                from[static_cast<std::size_t>(a)] = -1;
                for (std::size_t q = 0; q < queue.size(); ++q)
                {
                    for (const auto& [to, element] : tree[static_cast<std::size_t>(queue[q])])
                    {
                        if (from[static_cast<std::size_t>(to)] != -2)
                            continue;
                        from[static_cast<std::size_t>(to)] = queue[q];
                        via[static_cast<std::size_t>(to)] = element;
                        queue.push_back(to);
                    }
                }
                std::vector<std::size_t> loop;
                for (int at = b; at != a && at >= 0 && from[static_cast<std::size_t>(at)] != -2;
                     at = from[static_cast<std::size_t>(at)])
                    loop.push_back(via[static_cast<std::size_t>(at)]);
                std::sort(loop.begin(), loop.end(), [](std::size_t x, std::size_t y)
                          { return x == kInput ? y != kInput : y != kInput && x < y; });
                loop.push_back(e);
                std::string names;
                for (std::size_t k : loop)
                    names += (names.empty() ? "" : ", ") + (k == kInput ? std::string(".input") : elements[k].name);
                messages.error(elements[e].loc, "a loop of voltage sources, E outputs and inductors (" + names +
                                                    "): its DC current is undefined");
            }
        }

        // ---- driven nodes: op amps' outputs, grounded E sources' and V sources' ----
        std::map<int, std::size_t> driverOf;   // op amp outputs
        std::map<int, std::string> drivenBy;   // every driven node, and what drives it
        auto drive = [&](int node, const std::string& what, Loc loc)
        {
            if (node == p.inputNode)
            {
                messages.error(loc, what + " drives .input's node '" + nodeName(node) + "'");
                return false;
            }
            if (const auto it = drivenBy.find(node); it != drivenBy.end())
            {
                messages.error(loc, "node '" + nodeName(node) + "' is driven by " + it->second + " and by " + what);
                return false;
            }
            drivenBy.emplace(node, what);
            return true;
        };
        for (std::size_t k = 0; k < opamps.size(); ++k)
        {
            const OpAmp& op = opamps[k];
            const Loc loc = elements[static_cast<std::size_t>(op.element)].loc;
            if (op.out == 0)
                messages.error(loc, "op amp " + op.name + "'s output is ground");
            else if (op.out == p.inputNode)
                messages.error(loc, "op amp " + op.name + " drives .input's node '" + nodeName(op.out) + "'");
            else if (const auto it = driverOf.find(op.out); it != driverOf.end())
                messages.error(loc, "node '" + nodeName(op.out) + "' is driven by two op amps (" +
                                        opamps[it->second].name + ", " + op.name + ")");
            else
            {
                driverOf.emplace(op.out, k);
                drivenBy.emplace(op.out, "op amp " + op.name);
                p.nodes[static_cast<std::size_t>(op.out)].source = Node::Source::OpAmp;
            }
        }
        for (Controlled& c : controlled)
        {
            if (c.kind != Controlled::Kind::Vcvs || (c.p != 0 && c.n != 0))
                continue;
            const Loc loc = elements[static_cast<std::size_t>(c.element)].loc;
            if (c.p == 0 && c.n == 0)
            {
                messages.error(loc, c.name + "'s output terminals are both ground");
                continue;
            }
            // an output terminal grounded: the other is driven, V(out) = sign gain (V(cp) - V(cn))
            c.out = c.p == 0 ? c.n : c.p;
            c.sign = c.p == 0 ? -1 : 1;
            if (!drive(c.out, c.name, loc))
                continue;
            c.driven = true;
            p.nodes[static_cast<std::size_t>(c.out)].source = Node::Source::Vcvs;
            p.nodes[static_cast<std::size_t>(c.out)].element = c.element;
        }
        std::vector<Branch> voltageBranches;
        for (std::size_t e : voltages)
        {
            const Element& el = elements[e];
            const int a = elementNodes[e][0];
            const int b = elementNodes[e][1];
            if (p.inputNode > 0 && (a == p.inputNode || b == p.inputNode))
            {
                messages.error(el.loc, "the V source " + el.name + " is across .input's node '" +
                                           nodeName(p.inputNode) + "': the signal is .input's");
                continue;
            }
            if (a == 0 || b == 0)
            {
                // a terminal grounded: the other node is a constant (ground in the offsets)
                const int at = a == 0 ? b : a;
                if (at == 0 || !drive(at, el.name, el.loc))
                    continue;
                Node& n = p.nodes[static_cast<std::size_t>(at)];
                n.source = Node::Source::Dc;
                n.value = a == 0 ? -el.value : el.value;
                n.element = static_cast<int>(e);
                continue;
            }
            Branch v;
            v.kind = Branch::Kind::Voltage;
            v.a = a;
            v.b = b;
            v.dc = el.value;
            v.part = el.name;
            v.ident = elementIdent[e];
            v.partIdent = elementIdent[e];
            v.element = static_cast<int>(e);
            voltageBranches.push_back(v);
        }
        auto isSource = [&](int n)
        {
            return n == p.inputNode || drivenBy.count(n) > 0;
        };
        for (const Branch& v : voltageBranches)
        {
            if (isSource(v.a) && isSource(v.b))
                messages.error(elements[static_cast<std::size_t>(v.element)].loc,
                               "the V source " + v.part + " joins '" + nodeName(v.a) + "' and '" + nodeName(v.b) +
                                   "', which are both driven");
            else
                pieces.push_back({v});
        }
        for (std::size_t k = 0; k < controlled.size(); ++k)
        {
            const Controlled& c = controlled[k];
            if (c.driven || (c.kind == Controlled::Kind::Vcvs && (c.p == 0 || c.n == 0)))
                continue;
            if (c.kind == Controlled::Kind::Vcvs && isSource(c.p) && isSource(c.n))
            {
                messages.error(elements[static_cast<std::size_t>(c.element)].loc,
                               c.name + "'s output joins '" + nodeName(c.p) + "' and '" + nodeName(c.n) +
                                   "', which are both driven");
                continue;
            }
            Piece piece;
            piece.branch.a = c.p;
            piece.branch.b = c.n;
            piece.branch.part = c.name;
            piece.branch.element = c.element;
            piece.controlled = static_cast<int>(k);
            piece.nodes = {c.p, c.n, c.cp, c.cn};
            pieces.push_back(piece);
        }

        // ---- DC paths: every node reaches ground, .input or a driven node through R, P, L, a diode or a source ----
        {
            UnionFind dc(p.nodes.size());
            for (std::size_t e = 0; e < elements.size(); ++e)
            {
                // not a capacitor, nor a current source (I, a G, an OTA)
                const std::vector<int>& nn = elementNodes[e];
                switch (elements[e].kind)
                {
                case Element::Kind::Resistor:
                case Element::Kind::Switch:
                case Element::Kind::Diode:
                case Element::Kind::Inductor:
                case Element::Kind::Voltage:
                case Element::Kind::Vcvs:
                    dc.join(static_cast<std::size_t>(nn[0]), static_cast<std::size_t>(nn[1]));
                    break;
                case Element::Kind::Pot:
                    for (std::size_t k = 0; k + 1 < nn.size(); ++k)
                        dc.join(static_cast<std::size_t>(nn[k]), static_cast<std::size_t>(nn[k + 1]));
                    break;
                default:
                    break;
                }
            }
            std::set<std::size_t> anchored{dc.find(0)};
            if (p.inputNode > 0)
                anchored.insert(dc.find(static_cast<std::size_t>(p.inputNode)));
            for (const auto& [driven, what] : drivenBy)
                anchored.insert(dc.find(static_cast<std::size_t>(driven)));
            std::set<std::size_t> reported;
            for (std::size_t n = 1; n < p.nodes.size(); ++n)
            {
                const std::size_t root = dc.find(n);
                if (anchored.count(root) || reported.count(root))
                    continue;
                reported.insert(root);
                std::string caps;
                bool currents = false;
                bool capacitors = false;
                Loc loc;
                for (std::size_t e = 0; e < elements.size(); ++e)
                {
                    const Element::Kind kind = elements[e].kind;
                    const bool current = kind == Element::Kind::Current || kind == Element::Kind::Vccs ||
                                         kind == Element::Kind::Ota;
                    if (kind != Element::Kind::Capacitor && !current)
                        continue;
                    const std::vector<int>& nn = elementNodes[e];
                    if (dc.find(static_cast<std::size_t>(nn[0])) != root &&
                        dc.find(static_cast<std::size_t>(nn[1])) != root)
                        continue;
                    caps += (caps.empty() ? "" : ", ") + elements[e].name;
                    currents = currents || current;
                    capacitors = capacitors || !current;
                    if (loc.line == 0)
                        loc = elements[e].loc;
                }
                const std::string through = capacitors && currents ? "capacitors and current sources"
                                            : currents             ? "current sources"
                                                                   : "capacitors";
                if (caps.empty())
                    messages.error(loc,
                                   "node '" + p.nodes[n].name +
                                       "' has no path to ground: its DC voltage is undefined (add 10meg to ground)");
                else
                    messages.error(loc, "node '" + p.nodes[n].name + "' reaches ground only through " + through + " (" +
                                            caps + "): its DC voltage is undefined (add 10meg to ground)");
            }
        }
        if (messages.hasErrors())
            return std::nullopt;

        // ---- the partition: components of the nodes no source drives ----
        UnionFind uf(p.nodes.size());
        auto regular = [&](int n)
        {
            return n != 0 && !isSource(n) && !p.nodes[static_cast<std::size_t>(n)].absorbed;
        };
        for (const Piece& piece : pieces)
        {
            const Branch& b = piece.branch;
            if (piece.joinsAll())
            {
                // an OTA, a G or a floating E joins its four nodes into one block (those no source drives)
                int first = -1;
                for (int n : piece.nodes)
                {
                    if (!regular(n))
                        continue;
                    if (first < 0)
                        first = n;
                    else
                        uf.join(static_cast<std::size_t>(first), static_cast<std::size_t>(n));
                }
                continue;
            }
            if (regular(b.a) && regular(b.b))
                uf.join(static_cast<std::size_t>(b.a), static_cast<std::size_t>(b.b));
        }
        // Component keys: a regular node's root, or a follower's own key (inn = out).
        std::map<std::size_t, int> componentOf;   // root (or followerKey) -> component
        std::vector<std::vector<std::size_t>> componentPieces;
        std::vector<std::vector<std::size_t>> componentOpAmps;
        std::vector<std::vector<std::size_t>> componentDriven;   // the driven E sources (into `controlled`)
        const std::size_t followerBase = p.nodes.size();
        const std::size_t bufferBase = followerBase + opamps.size();   // a driven E's own block
        auto component = [&](std::size_t key)
        {
            const auto it = componentOf.find(key);
            if (it != componentOf.end())
                return it->second;
            const int c = static_cast<int>(componentPieces.size());
            componentOf.emplace(key, c);
            componentPieces.emplace_back();
            componentOpAmps.emplace_back();
            componentDriven.emplace_back();
            return c;
        };
        // In netlist order: the order of the first element of each component.
        std::vector<std::pair<int, std::size_t>> pieceOrder;   // (element, piece)
        for (std::size_t i = 0; i < pieces.size(); ++i)
            pieceOrder.emplace_back(pieces[i].branch.element, i);
        std::stable_sort(pieceOrder.begin(), pieceOrder.end());
        std::vector<std::pair<int, int>> owners;   // (element, component) of every op amp and piece, for ordering
        for (const auto& [element, i] : pieceOrder)
        {
            const Branch& b = pieces[i].branch;
            int c = -1;
            if (pieces[i].joinsAll())
            {
                for (int n : pieces[i].nodes)
                {
                    if (c < 0 && regular(n))
                        c = component(uf.find(static_cast<std::size_t>(n)));
                }
                if (c < 0 && pieces[i].ota)
                {
                    messages.error(elements[static_cast<std::size_t>(element)].loc,
                                   "OTA " + b.part + "'s nodes are all ground or driven: nothing solves for it");
                    continue;
                }
                if (c < 0)
                {
                    messages.warning(elements[static_cast<std::size_t>(element)].loc,
                                     b.part + "'s nodes are all ground or driven: it changes nothing");
                    continue;
                }
            }
            else if (regular(b.a))
                c = component(uf.find(static_cast<std::size_t>(b.a)));
            else if (regular(b.b))
                c = component(uf.find(static_cast<std::size_t>(b.b)));
            if (c < 0)
            {
                const std::string what = b.b == 0 ? "ground" : "'" + p.nodes[static_cast<std::size_t>(b.b)].name + "'";
                messages.warning(elements[static_cast<std::size_t>(element)].loc,
                                 b.part + " joins '" + p.nodes[static_cast<std::size_t>(b.a)].name + "' and " + what +
                                     ", which are both driven: it changes nothing");
                continue;
            }
            componentPieces[static_cast<std::size_t>(c)].push_back(i);
        }
        for (std::size_t k = 0; k < opamps.size(); ++k)
        {
            const OpAmp& op = opamps[k];
            const Loc loc = elements[static_cast<std::size_t>(op.element)].loc;
            int c = -1;
            if (op.inn == op.out)
                // a follower: in its + input's network (a Sallen-Key's), or a buffer block of its own
                c = regular(op.inp) ? component(uf.find(static_cast<std::size_t>(op.inp)))
                                    : component(followerBase + k);
            else if (regular(op.inn))
            {
                c = component(uf.find(static_cast<std::size_t>(op.inn)));
                // its feedback: some branch (or source) of inn's component reaches out
                bool reaches = false;
                for (std::size_t i : componentPieces[static_cast<std::size_t>(c)])
                {
                    const Piece& piece = pieces[i];
                    reaches = reaches || piece.branch.a == op.out || piece.branch.b == op.out ||
                              (piece.joinsAll() &&
                               std::find(piece.nodes.begin(), piece.nodes.end(), op.out) != piece.nodes.end());
                }
                if (!reaches)
                    c = -1;
            }
            if (c < 0)
            {
                messages.error(loc, "op amp " + op.name + " has no negative feedback: an ideal op amp needs one");
                continue;
            }
            componentOpAmps[static_cast<std::size_t>(c)].push_back(k);
        }
        // a driven E: in its controls' block (the one their nodes are in), or a buffer block of its own when both are
        // driven (or ground)
        for (std::size_t k = 0; k < controlled.size(); ++k)
        {
            const Controlled& e = controlled[k];
            if (!e.driven)
                continue;
            std::set<int> comps;
            for (int n : {e.cp, e.cn})
            {
                if (regular(n))
                    comps.insert(component(uf.find(static_cast<std::size_t>(n))));
            }
            if (comps.size() > 1)
            {
                messages.error(elements[static_cast<std::size_t>(e.element)].loc,
                               e.name + "'s controls, '" + nodeName(e.cp) + "' and '" + nodeName(e.cn) +
                                   "', are in two blocks: an E reads one block's nodes (or its input)");
                continue;
            }
            const int c = comps.empty() ? component(bufferBase + k) : *comps.begin();
            componentDriven[static_cast<std::size_t>(c)].push_back(k);
        }
        if (messages.hasErrors())
            return std::nullopt;

        // Blocks, in the order of their first part. A second port is reported once the chain numbers the blocks.
        std::vector<Block> blocks(componentPieces.size());
        std::vector<std::vector<std::string>> extraPorts(componentPieces.size());
        std::vector<int> firstElement(componentPieces.size(), static_cast<int>(elements.size()));
        for (std::size_t c = 0; c < componentPieces.size(); ++c)
        {
            Block& block = blocks[c];
            std::set<int> parts;
            std::set<int> nodeSet;
            for (std::size_t i : componentPieces[c])
            {
                const Piece& piece = pieces[i];
                if (piece.port)
                {
                    if (block.port)
                    {
                        extraPorts[c].push_back(piece.branch.part);
                        for (int e : ports[static_cast<std::size_t>(piece.pair)].elements)
                            parts.insert(e);
                        continue;
                    }
                    block.port = ports[static_cast<std::size_t>(piece.pair)];
                    for (int e : block.port->elements)
                        parts.insert(e);
                }
                else if (piece.controlled >= 0)
                {
                    block.controlled.push_back(controlled[static_cast<std::size_t>(piece.controlled)]);
                    parts.insert(piece.branch.element);
                }
                else
                {
                    block.branches.push_back(piece.branch);
                    parts.insert(piece.branch.element);
                }
                for (int n : piece.joinsAll() ? piece.nodes : std::vector<int>{piece.branch.a, piece.branch.b})
                {
                    if (regular(n))
                        nodeSet.insert(n);
                }
            }
            for (std::size_t k : componentOpAmps[c])
            {
                block.opamps.push_back(opamps[k]);
                block.driven.push_back(opamps[k].out);
                parts.insert(opamps[k].element);
            }
            for (std::size_t k : componentDriven[c])
            {
                block.controlled.push_back(controlled[k]);
                block.driven.push_back(controlled[k].out);
                parts.insert(controlled[k].element);
            }
            block.elements.assign(parts.begin(), parts.end());
            block.nodes.assign(nodeSet.begin(), nodeSet.end());   // node indices are in order of first appearance
            std::stable_sort(block.branches.begin(), block.branches.end(),
                             [](const Branch& a, const Branch& b) { return a.element < b.element; });
            std::stable_sort(block.controlled.begin(), block.controlled.end(),
                             [](const Controlled& a, const Controlled& b) { return a.element < b.element; });
            if (!block.elements.empty())
                firstElement[c] = block.elements.front();
        }
        if (messages.hasErrors())
            return std::nullopt;

        // What each block reads, and who holds it.
        std::vector<int> holder(p.nodes.size(), -1);
        for (std::size_t c = 0; c < blocks.size(); ++c)
        {
            for (int n : blocks[c].nodes)
                holder[static_cast<std::size_t>(n)] = static_cast<int>(c);
            for (int n : blocks[c].driven)
                holder[static_cast<std::size_t>(n)] = static_cast<int>(c);
        }
        std::vector<int> reads(blocks.size(), -1);
        for (std::size_t c = 0; c < blocks.size(); ++c)
        {
            const Block& block = blocks[c];
            std::vector<int> found;
            auto read = [&](int n)
            {
                // a DC source's node is a constant, not an input
                if (n == 0 || holder[static_cast<std::size_t>(n)] == static_cast<int>(c) ||
                    p.nodes[static_cast<std::size_t>(n)].source == Node::Source::Dc)
                    return;
                if (std::find(found.begin(), found.end(), n) == found.end())
                    found.push_back(n);
            };
            for (const Branch& b : block.branches)
            {
                read(b.a);
                read(b.b);
            }
            if (block.port)
            {
                read(block.port->anode);
                read(block.port->cathode);
                read(block.port->from);
                read(block.port->to);
            }
            for (const OpAmp& op : block.opamps)
                read(op.inp);
            for (const Controlled& k : block.controlled)
            {
                for (int n : k.driven ? std::vector<int>{k.cp, k.cn} : std::vector<int>{k.p, k.n, k.cp, k.cn})
                    read(n);
            }
            Loc loc;
            if (!block.elements.empty())
                loc = elements[static_cast<std::size_t>(block.elements.front())].loc;
            const std::string label = "block (" + partsText(p, block) + ")";
            if (found.empty())
                messages.error(loc, label + " reads no input: it is not connected to .input");
            else if (found.size() > 1)
            {
                std::string names;
                for (int n : found)
                    names += (names.empty() ? "" : ", ") + p.nodes[static_cast<std::size_t>(n)].name;
                messages.error(loc, label + " reads " + std::to_string(found.size()) + " inputs (" + names +
                                        "): not supported yet (Phase 2)");
            }
            else
                reads[c] = found.front();
        }
        if (messages.hasErrors())
            return std::nullopt;

        // The chain, from the block that reads .input.
        std::vector<int> upstream(blocks.size(), -1);
        for (std::size_t c = 0; c < blocks.size(); ++c)
            upstream[c] = reads[c] == p.inputNode ? -1 : holder[static_cast<std::size_t>(reads[c])];
        std::vector<int> chain;
        {
            std::vector<int> firsts;
            for (std::size_t c = 0; c < blocks.size(); ++c)
            {
                if (reads[c] == p.inputNode)
                    firsts.push_back(static_cast<int>(c));
            }
            if (firsts.size() != 1)
            {
                messages.error(firsts.empty() ? Loc{} : netlist.input->loc,
                               firsts.empty() ? "no block reads .input"
                                              : std::to_string(firsts.size()) +
                                                    " blocks read .input: the blocks form one chain (branching ones "
                                                    "are Phase 2)");
                return std::nullopt;
            }
            std::vector<bool> visited(blocks.size(), false);
            int at = firsts.front();
            while (at >= 0)
            {
                visited[static_cast<std::size_t>(at)] = true;
                chain.push_back(at);
                std::vector<int> next;
                for (std::size_t c = 0; c < blocks.size(); ++c)
                {
                    if (upstream[c] == at && !visited[c])
                        next.push_back(static_cast<int>(c));
                }
                if (next.size() > 1)
                {
                    messages.error(
                        elements[static_cast<std::size_t>(blocks[static_cast<std::size_t>(next[1])].elements.front())]
                            .loc,
                        "two blocks read block (" + partsText(p, blocks[static_cast<std::size_t>(at)]) +
                            "): the blocks form one chain (branching ones are Phase 2)");
                    return std::nullopt;
                }
                at = next.empty() ? -1 : next.front();
            }
            for (std::size_t c = 0; c < blocks.size(); ++c)
            {
                if (visited[c])
                    continue;
                // A cycle among the rest, or a block hanging off the chain.
                std::string opName;
                int walk = static_cast<int>(c);
                std::set<int> seen;
                while (walk >= 0 && !visited[static_cast<std::size_t>(walk)] && seen.insert(walk).second)
                    walk = upstream[static_cast<std::size_t>(walk)];
                const bool cycle = walk >= 0 && seen.count(walk);
                for (int b : seen)
                {
                    if (!blocks[static_cast<std::size_t>(b)].opamps.empty() && opName.empty())
                        opName = blocks[static_cast<std::size_t>(b)].opamps.front().name;
                }
                const Loc loc = elements[static_cast<std::size_t>(blocks[c].elements.front())].loc;
                if (cycle)
                    messages.error(loc, "a feedback loop around " +
                                            (opName.empty() ? std::string("an op amp") : opName) +
                                            " crosses blocks: not supported yet (Phase 2)");
                else
                    messages.error(loc, "block (" + partsText(p, blocks[c]) +
                                            ") is off the chain from .input to .output: not supported yet (Phase 2)");
                return std::nullopt;
            }
        }
        for (std::size_t i = 0; i < chain.size(); ++i)
        {
            Block block = blocks[static_cast<std::size_t>(chain[i])];
            block.index = static_cast<int>(i) + 1;
            block.input = reads[static_cast<std::size_t>(chain[i])];
            const std::vector<std::string>& extra = extraPorts[static_cast<std::size_t>(chain[i])];
            if (!extra.empty())
            {
                std::string names = block.port->parts;
                for (const std::string& e : extra)
                    names += ", " + e;
                messages.error(elements[static_cast<std::size_t>(block.elements.front())].loc,
                               "block " + std::to_string(block.index) + " has " + std::to_string(extra.size() + 1) +
                                   " nonlinear ports (" + names + "): multi-port Newton is Phase 2");
            }
            p.blocks.push_back(block);
        }
        for (std::size_t i = 0; i < p.blocks.size(); ++i)
        {
            for (int n : p.blocks[i].nodes)
                p.nodes[static_cast<std::size_t>(n)].owner = static_cast<int>(i);
            for (int n : p.blocks[i].driven)
                p.nodes[static_cast<std::size_t>(n)].owner = static_cast<int>(i);
        }
        for (std::size_t i = 0; i + 1 < p.blocks.size(); ++i)
            p.blocks[i].output = p.blocks[i + 1].input;
        {
            Block& last = p.blocks.back();
            const int owner = p.nodes[static_cast<std::size_t>(p.outputNode)].owner;
            if (p.outputNode == p.inputNode)
                messages.error(netlist.output->nodeLoc, ".output reads .input's node itself");
            else if (owner != static_cast<int>(p.blocks.size()) - 1)
                messages.error(netlist.output->nodeLoc,
                               ".output's node '" + p.nodes[static_cast<std::size_t>(p.outputNode)].name +
                                   "' is not the last block's: Phase 0 reads the output at the chain's end");
            last.output = p.outputNode;
        }
        if (messages.hasErrors())
            return std::nullopt;
        for (Block& block : p.blocks)
        {
            std::set<std::string> keys;
            std::set<std::string> switchKeys;
            for (const Branch& b : block.branches)
            {
                if (b.isSwitch())
                    switchKeys.insert(b.switchKey);
                else if (b.kind == Branch::Kind::Variable)
                {
                    for (const std::string& k : potsOf(b.expr))
                        keys.insert(k);
                }
            }
            for (const SwitchInfo& w : p.switches)
            {
                if (switchKeys.count(w.key))
                    block.switches.push_back(w.key);
            }
            if (block.port)
                keys.insert(block.port->gainPots.begin(), block.port->gainPots.end());
            for (const Controlled& k : block.controlled)
            {
                if (k.expr)
                {
                    for (const std::string& key : potsOf(*k.expr))
                        keys.insert(key);
                }
            }
            for (const PotInfo& pot : p.pots)
            {
                if (keys.count(pot.key))
                    block.pots.push_back(pot.key);
            }
        }

        // ---- the ports' families: one solver serves the region, so its ports are all diodes or all OTAs ----
        {
            std::string diodeList;
            std::string otas;
            Loc mixedAt;
            for (const Block& b : p.blocks)
            {
                if (!b.port)
                    continue;
                std::string& list = b.port->smooth() ? otas : diodeList;
                list += (list.empty() ? "" : ", ") + b.port->parts;
                if (!diodeList.empty() && !otas.empty() && mixedAt.line == 0)
                    mixedAt = elements[static_cast<std::size_t>(b.port->elements.front())].loc;
            }
            if (!diodeList.empty() && !otas.empty())
                messages.error(mixedAt, "the oversampled region mixes diode ports (" + diodeList + ") and OTA ports (" +
                                            otas + "): one solver serves a region (per-family solvers are Phase 2)");
            p.smooth = !otas.empty();
            p.config.otas = p.smooth;
        }
        // An OTA's gain is its bias current: not negative anywhere on the pots' and law inputs' grid.
        for (const Block& b : p.blocks)
        {
            if (!b.port || !b.port->smooth() || !b.port->gain)
                continue;
            const PortDef& port = *b.port;
            std::vector<std::size_t> lawAxes;
            for (std::size_t i = 0; i < p.laws.size(); ++i)
            {
                if (std::find(port.gainLaws.begin(), port.gainLaws.end(), p.laws[i].key) != port.gainLaws.end())
                    lawAxes.push_back(i);
            }
            Block gainBlock = b;
            gainBlock.pots = port.gainPots;
            std::size_t lawPoints = 1;
            for (std::size_t i = 0; i < lawAxes.size(); ++i)
                lawPoints *= 3;
            bool reported = false;
            for (const std::vector<double>& pots : potGrid(p, gainBlock))
            {
                for (std::size_t k = 0; k < lawPoints && !reported; ++k)
                {
                    std::vector<double> laws;
                    for (const LawInfo& l : p.laws)
                        laws.push_back(l.low);
                    std::size_t rest = k;
                    std::string where;
                    for (std::size_t i : lawAxes)
                    {
                        const LawInfo& l = p.laws[i];
                        const double values[] = {l.low, l.middle(), l.high};
                        laws[i] = values[rest % 3];
                        rest /= 3;
                        where += std::format("{}{} {:g}", where.empty() ? "" : ", ", l.name, laws[i]);
                    }
                    for (const std::string& key : port.gainPots)
                    {
                        if (const int i = potIndex(key); i >= 0)
                            where += std::format("{}{} {:g}", where.empty() ? "" : ", ",
                                                 p.pots[static_cast<std::size_t>(i)].name,
                                                 pots[static_cast<std::size_t>(i)]);
                    }
                    const double g = gainAt(p, port, pots, laws);
                    if (!(g >= 0) || !std::isfinite(g))
                    {
                        messages.error(elements[static_cast<std::size_t>(port.elements.front())].loc,
                                       std::format("OTA {}'s gain is {:g} A at {}: an OTA's gain (its bias current) is "
                                                   "not negative",
                                                   port.parts, g, where));
                        reported = true;
                    }
                }
            }
        }
        if (messages.hasErrors())
            return std::nullopt;

        // ---- the DC operating point: the module runs in offsets from it ----
        if (!dcPoint(p, messages))
            return std::nullopt;

        // ---- the configuration: Options, then the netlist, then the self-test's answer, then the provisional ----
        Config& cfg = p.config;
        p.key = keyOf(p.netlist, options);
        const bool hasPort =
            std::any_of(p.blocks.begin(), p.blocks.end(), [](const Block& b) { return b.port.has_value(); });
        // The Options' answer, when it is this netlist's (its key, which the version is part of).
        std::optional<Choice> chosen;
        if (options.chosen && options.chosen->key == p.key)
            chosen = options.chosen;
        else if (options.chosen)
            messages.note({}, "Options::chosen is another netlist's or other Options' (key " + options.chosen->key +
                                  "; this one's is " + p.key + "): it is ignored");
        const std::string fromChoice = "the self-test " + p.key;
        // A searched dimension: Options, then the netlist's directive; open when neither names it (nothing is open
        // in a netlist without a port: there is nothing to choose).
        auto resolve = [&](auto& value, std::string& from, bool& open, const auto& option, const auto& setting,
                           const auto& byChoice, const auto& provisional)
        {
            if (option)
            {
                value = *option;
                from = "Options";
            }
            else if (setting.value)
            {
                value = *setting.value;
                from = "the netlist";
            }
            else if (!hasPort)
                from = "default";
            else
            {
                open = true;
                if (chosen)
                {
                    value = byChoice;
                    from = fromChoice;
                }
                else
                {
                    value = provisional;
                    from = "provisional";
                }
            }
        };
        // The provisional configuration: an exp-affine port's is Phase 0's 8x, omega3 + 2 (+ 1 with ADAA pinned); a
        // smooth-monotone port's (an OTA's) 2x, the previous step's v + 2 Newton steps.
        resolve(cfg.oversample, cfg.oversampleFrom, cfg.oversampleOpen, options.oversample, netlist.oversample,
                chosen ? chosen->oversample : 8, p.smooth ? 2 : 8);
        resolve(cfg.adaa, cfg.adaaFrom, cfg.adaaOpen, options.adaa, netlist.adaa, chosen && chosen->adaa, false);
        resolve(cfg.solver, cfg.solverFrom, cfg.solverOpen, options.solver, netlist.solver,
                chosen ? chosen->solver : Solver{},
                p.smooth ? Solver{Solver::Guess::Previous, 2} : Solver{Solver::Guess::Omega, cfg.adaa ? 1 : 2});
        // omega3 is the diode's closed form: an OTA's port starts from the previous step's v
        if (p.smooth && cfg.solver.guess == Solver::Guess::Omega)
        {
            const Loc loc = cfg.solverFrom == "the netlist" ? netlist.solver.loc : Loc{};
            const std::string what = cfg.solverFrom == "the netlist" ? "'.solver"
                                     : cfg.solverFrom == "Options"   ? "'Options::solver"
                                                                     : "'Options::chosen's solver";
            messages.error(loc, what + " omega(" + std::to_string(cfg.solver.steps) +
                                    ")': an OTA's port is solved from the previous step's v: .solver newton(K)");
        }
        else if (p.smooth && netlist.solverBare && !options.solver)
            messages.error(netlist.solver.loc,
                           "'.solver omega': an OTA's port is solved from the previous step's v: .solver newton(K)");
        // The integrator: Options, then the netlist. It is open (the self-test's: the trapezoidal rule, and a BDF2
        // twin of a candidate whose block rings) when the netlist writes `.method auto`, or leaves it out while
        // another searched dimension is open: a netlist that names the rest of its configuration plays the
        // trapezoidal rule, with nothing left to choose (were a .method left out open, every netlist would be).
        if (options.method)
        {
            cfg.method = *options.method;
            cfg.methodFrom = "Options";
        }
        else if (netlist.method.value)
        {
            cfg.method = *netlist.method.value;
            cfg.methodFrom = "the netlist";
        }
        else if (hasPort && (netlist.method.isAuto || cfg.oversampleOpen || cfg.adaaOpen || cfg.solverOpen))
        {
            cfg.methodOpen = true;
            cfg.methodAuto = netlist.method.isAuto;
            cfg.method = chosen ? chosen->method : Method::Tr;
            cfg.methodFrom = chosen ? fromChoice : "provisional";
        }
        if (cfg.adaa && cfg.method == Method::Bdf2)
        {
            // where each came from, as the error names it
            auto said = [&](const std::string& from, const std::string& directive, const std::string& member)
            {
                return from == "the netlist" ? "'" + directive + "'"
                       : from == "Options"   ? "Options::" + member
                                             : "Options::chosen's " + member;
            };
            const Loc loc = cfg.methodFrom == "the netlist" ? netlist.method.loc
                            : cfg.adaaFrom == "the netlist" ? netlist.adaa.loc
                                                            : Loc{};
            messages.error(loc, said(cfg.methodFrom, ".method bdf2", "method bdf2") + " with " +
                                    said(cfg.adaaFrom, ".adaa on", "adaa on") +
                                    ": ADAA is Holters' trapezoidal form");
        }
        if (options.prewarp)
        {
            cfg.prewarp = *options.prewarp;
            cfg.prewarpFrom = "Options";
        }
        else if (netlist.prewarp.value)
        {
            cfg.prewarp = *netlist.prewarp.value;
            cfg.prewarpFrom = "the netlist";
        }
        if (options.halfband)
        {
            cfg.halfband = *options.halfband;
            cfg.halfbandFrom = "Options";
        }
        else if (netlist.halfband.value)
        {
            cfg.halfband = *netlist.halfband.value;
            cfg.halfbandFrom = "the netlist";
        }
        if (options.region)
        {
            cfg.region = *options.region;
            cfg.regionFrom = "Options";
        }
        else if (netlist.region.value)
        {
            cfg.region = *netlist.region.value;
            cfg.regionFrom = "the netlist";
        }
        if (options.channels)
        {
            cfg.channels = *options.channels;
            cfg.channelsFrom = "Options";
        }
        else if (netlist.channels.value)
        {
            cfg.channels = *netlist.channels.value;
            cfg.channelsFrom = "the netlist";
        }
        if (cfg.anyOpen())
        {
            cfg.chosen = chosen;
            cfg.provisional = !chosen;
            if (cfg.provisional)
                messages.note({}, "the self-test chooses this circuit's configuration; provisional: " +
                                      configurationText(cfg.oversample, cfg.halfband, cfg.adaa, cfg.solver, true));
        }

        // ---- the region, and each linear block's 1x check ----
        std::vector<int> portBlocks;
        for (std::size_t i = 0; i < p.blocks.size(); ++i)
        {
            if (p.blocks[i].port)
                portBlocks.push_back(static_cast<int>(i));
        }
        constexpr double kCheckRate = 44100;
        constexpr double kTolerance = 0.5;
        std::vector<System> systems;
        std::vector<std::optional<Pivots>> pivotsOf;
        for (std::size_t i = 0; i < p.blocks.size(); ++i)
        {
            systems.push_back(assemble(p, static_cast<int>(i)));
            if (p.blocks[i].port)
            {
                pivotsOf.emplace_back();
                continue;
            }
            Messages scratch;   // a singular block is reported once, when the module is generated
            pivotsOf.push_back(choosePivots(systems.back(), p, static_cast<int>(i), scratch));
            if (pivotsOf.back())
                p.blocks[i].oneX = oneXError(systems.back(), *pivotsOf.back(), p, static_cast<int>(i), kCheckRate);
        }
        auto blockLabel = [&](const Block& b)
        {
            return "block " + std::to_string(b.index) + " (" + partsText(p, b) + ")";
        };
        auto blockLoc = [&](const Block& b)
        {
            return elements[static_cast<std::size_t>(b.elements.front())].loc;
        };
        const bool linearOnly = portBlocks.empty();
        const int count = static_cast<int>(p.blocks.size());
        // Under blocks=auto, a linear block whose trapezoidal response fails the 1x check runs a matched design at
        // 1x when that passes it (Linear.h), and joins the oversampled region otherwise
        std::vector<MatchedCheck> matched(p.blocks.size());
        std::vector<bool> joins(p.blocks.size(), false);
        for (std::size_t i = 0; i < p.blocks.size(); ++i)
        {
            const Block& b = p.blocks[i];
            if (b.port || !pivotsOf[i] || b.oneX <= kTolerance || cfg.region != Region::Auto)
                continue;
            matched[i] = matchedCheck(p, static_cast<int>(i));
            joins[i] = !matched[i].usable || matched[i].worst > kTolerance;
        }
        int first = linearOnly ? -1 : portBlocks.front();
        int last = linearOnly ? -1 : portBlocks.back();
        if (cfg.region == Region::All)
        {
            first = 0;
            last = count - 1;
        }
        else if (cfg.region == Region::Auto)
        {
            for (int i = 0; i < count; ++i)
            {
                if (!joins[static_cast<std::size_t>(i)])
                    continue;
                first = first < 0 ? i : std::min(first, i);
                last = std::max(last, i);
            }
        }
        bool regionChecked = false;   // the linear-only region's 1x check, already made at its factor
        if (linearOnly && cfg.method == Method::Bdf2)
            messages.warning(cfg.methodFrom == "the netlist" ? netlist.method.loc : Loc{},
                             "'.method bdf2' is ignored: the netlist is linear (its blocks run the trapezoidal rule, "
                             "or matched designs)");
        if (linearOnly)
        {
            // nothing to choose; the oversampled region, if any, at .oversample's factor, else at the smallest of 2,
            // 4 and 8 at which its blocks' trapezoidal responses pass the 1x check
            cfg.method = Method::Tr;
            if (first < 0)
            {
                if (cfg.oversampleFrom != "default")
                    messages.note({}, std::format("the netlist is linear and its blocks run at 1x: .oversample {} is "
                                                  "unused",
                                                  cfg.oversample));
                else
                    cfg.oversample = 1;
            }
            else if (cfg.oversampleFrom == "default")
            {
                cfg.oversample = 8;
                for (int os : {2, 4, 8})
                {
                    bool passes = true;
                    for (int i = first; i <= last && passes; ++i)
                    {
                        const auto k = static_cast<std::size_t>(i);
                        if (!pivotsOf[k])
                            continue;
                        // kept: the region's check below is this one at the factor chosen
                        p.blocks[k].oneXRegion = oneXError(systems[k], *pivotsOf[k], p, i, kCheckRate * os);
                        passes = p.blocks[k].oneXRegion <= kTolerance;
                    }
                    if (passes)
                    {
                        cfg.oversample = os;
                        regionChecked = true;
                        break;
                    }
                }
                cfg.oversampleFrom = "the 1x check";
            }
        }
        if (first >= 0)
        {
            p.regionFirst = first;
            p.regionLast = last;
            for (int i = first; i <= last; ++i)
            {
                Block& b = p.blocks[static_cast<std::size_t>(i)];
                b.inRegion = true;
                b.rateFactor = cfg.oversample;
                b.method = cfg.method;
                if (b.port && cfg.adaa)
                    b.designFactor = 2.0 / 3.0;
            }
        }
        for (std::size_t i = 0; i < p.blocks.size(); ++i)
        {
            Block& b = p.blocks[i];
            if (b.port || !pivotsOf[i])
                continue;
            if (b.inRegion)
            {
                if (!regionChecked)
                    b.oneXRegion = oneXError(systems[i], *pivotsOf[i], p, static_cast<int>(i),
                                             kCheckRate * b.rateFactor, b.method);
                if (b.oneXRegion > kTolerance)
                    messages.warning(blockLoc(b), std::format("{} errs by {:.2f} dB at {}x 44.1 kHz, more than {} dB "
                                                              "even oversampled",
                                                              blockLabel(b), b.oneXRegion, b.rateFactor, kTolerance));
            }
            else if (b.oneX > kTolerance && matched[i].usable && matched[i].worst <= kTolerance)
            {
                b.design = Block::Design::Matched;
                b.matchedError = matched[i].worst;
                b.matchedPolarity = matched[i].polarity;
            }
            else if (b.oneX > kTolerance)
                messages.warning(blockLoc(b), std::format("{} errs by {:.2f} dB at 1x (44.1 kHz), more than {} dB: "
                                                          "blocks=auto would oversample it",
                                                          blockLabel(b), b.oneX, kTolerance));
            // why a block that fails the 1x check runs as it does
            if (b.oneX <= kTolerance || cfg.region != Region::Auto)
                continue;
            if (b.design == Block::Design::Matched)
                messages.note({}, std::format("{}: a matched design at 1x: its trapezoidal response errs by {:.2f} dB "
                                              "at 44.1 kHz, the matched one's by {:.2f} dB at 44.1 and 48 kHz",
                                              blockLabel(b), b.oneX, b.matchedError));
            else if (b.inRegion && !joins[i])
                messages.note({}, std::format("{}: in the oversampled region, between its other blocks (its "
                                              "trapezoidal response errs by {:.2f} dB at 44.1 kHz)",
                                              blockLabel(b), b.oneX));
            else if (!matched[i].usable)
                messages.note({}, std::format("{}: in the oversampled region: its trapezoidal response errs by {:.2f} "
                                              "dB at 44.1 kHz, and a matched design cannot serve ({})",
                                              blockLabel(b), b.oneX, matched[i].why));
            else
                messages.note({}, std::format("{}: in the oversampled region: its trapezoidal response errs by {:.2f} "
                                              "dB at 44.1 kHz, a matched design's by {:.2f} dB, more than {}",
                                              blockLabel(b), b.oneX, matched[i].worst, kTolerance));
        }

        // ---- prewarp: each OTA's small-signal pole, fitted as a + b g ----
        if (cfg.prewarp)
            fitPrewarp(p, systems, messages);

        // ---- the method check (stage 7): a region port block whose linearised trapezoidal A has an eigenvalue near
        // -1 may ring at Nyquist; the self-test measures whether it does ----
        for (int i = std::max(p.regionFirst, 0); p.regionFirst >= 0 && i <= p.regionLast; ++i)
        {
            const Block& b = p.blocks[static_cast<std::size_t>(i)];
            if (!b.port || b.method != Method::Tr)
                continue;
            const RingingCheck check = ringingCheck(p, i, 48000.0 * b.rateFactor);
            if (check.flagged)
                messages.note({}, std::format("{}: an eigenvalue of its linearised trapezoidal A at {}x, {:.4f}, is "
                                              "within 0.1 of -1 (the port at {:.3g} S): it may ring at Nyquist (the "
                                              "self-test measures it)",
                                              blockLabel(b), b.rateFactor, check.eigenvalue.real(),
                                              check.conductance));
        }
        return p;
    }
} // namespace fminus::circuit
