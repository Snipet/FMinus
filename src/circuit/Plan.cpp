// src/circuit/Plan.cpp: elaboration, the netlist's checks, the partition into blocks at ideal op amps, their chain,
// the configuration and the oversampled region (Plan.h).
#include "circuit/Plan.h"

#include "circuit/Emit.h"
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
        // the methods, the step's locals (an exp-affine port's, then an OTA's: kd g, its slope and its 2 Vt multiple,
        // Newton's tanh, the tanh at v and the current (or ADAA's mean of it), ADAA's half step in x, its square, e^h,
        // sinh(h), z, 1 + z, the log cosh difference and the midpoint's p - v), and os1, the one field without a '_'
        // (tick's region sample)
        static constexpr std::array<std::string_view, 37> kModule{
            "init", "clear", "set", "tick", "step", "p",  "q",  "arg", "w",   "v",  "lo", "hi", "e",  "ei", "f",  "d",
            "ec",   "c",     "dp",  "vm",   "vb",   "ua", "kg", "kgs", "kgv", "tn", "th", "ib", "hh", "h2", "eh", "s2",
            "z",    "zu",    "dl",  "wm",   "os1"};
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
        // and the guard's fields and locals (ck_, fd_, fdstep_, sum_, gain_)
        static constexpr std::array<std::string_view, 17> kOther{
            "rate", "clang", "nonblocking", "x",   "o",   "s0",     "s1",   "t",    "y",
            "v0",   "v1",    "i",           "ck_", "fd_", "fdstep_", "sum_", "gain_"};
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
            bool port = false;    // a port's: a diode pair's (anode, cathode), or an OTA's (n+, n-)
            bool ota = false;     // an OTA's: no DC path through it; its four nodes join one block
            std::vector<int> nodes;   // an OTA's n+, n-, nc+, nc-
            int pair = -1;        // which port
        };
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

    std::vector<std::vector<double>> potGrid(const Plan& plan, const Block& block)
    {
        std::vector<double> defaults;
        for (const PotInfo& p : plan.pots)
            defaults.push_back(p.defaultValue);
        std::vector<std::size_t> mine;
        for (const std::string& key : block.pots)
        {
            for (std::size_t i = 0; i < plan.pots.size(); ++i)
            {
                if (plan.pots[i].key == key)
                    mine.push_back(i);
            }
        }
        std::vector<std::vector<double>> grid;
        const double values[] = {0.0, 0.5, 1.0};
        if (mine.size() <= 3)
        {
            std::size_t count = 1;
            for (std::size_t i = 0; i < mine.size(); ++i)
                count *= 3;
            for (std::size_t k = 0; k < count; ++k)
            {
                std::vector<double> point = defaults;
                std::size_t rest = k;
                for (std::size_t i : mine)
                {
                    point[i] = values[rest % 3];
                    rest /= 3;
                }
                grid.push_back(point);
            }
            return grid;
        }
        for (std::size_t i : mine)
        {
            for (double v : values)
            {
                std::vector<double> point = defaults;
                point[i] = v;
                grid.push_back(point);
            }
        }
        for (double v : {0.0, 1.0})
        {
            std::vector<double> point = defaults;
            for (std::size_t i : mine)
                point[i] = v;
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

        // .prewarp on: each OTA in a one-state block without pots in its network gets its pole's affine fit, a + b g
        // from the ends of its gain's range, checked at the middle (a residual of 1e-6 at most, else a warning that
        // prewarp is ignored for it); any other OTA, or a netlist without one, warns that prewarp is ignored.
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
                int states = 0;
                bool variable = false;
                for (const Branch& br : b.branches)
                {
                    states += br.kind == Branch::Kind::Capacitor ? 1 : 0;
                    variable = variable || br.kind == Branch::Kind::Variable;
                }
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
                messages.note({}, std::format("OTA {}: prewarp; its small-signal pole is a + b g, a = {:.7g} rad/s, b = "
                                              "{:.7g} rad/s per A (g {:g} to {:g} A)",
                                              port.parts, aCoef, bCoef, lo, hi));
            }
            if (!any)
                messages.warning(directive, "'.prewarp on' is ignored: it prewarps an OTA's cutoff, and the netlist has "
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
        std::vector<PortDef> ports;   // the diode pairs' and the OTAs'
        std::vector<OpAmp> opamps;
        std::vector<std::size_t> diodes;
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
            case Element::Kind::Diode:
            {
                bool found = false;
                for (const ModelCard& m : netlist.models)
                    found = found || folded(m.name) == folded(el.model);
                if (!found)
                {
                    std::string list;
                    for (const ModelCard& m : netlist.models)
                        list += (list.empty() ? "" : ", ") + m.name;
                    messages.error(
                        el.modelLoc,
                        "unknown model '" + el.model + "' (" +
                            (list.empty() ? std::string("there are no .model cards") : "the models: " + list) + ")");
                }
                diodes.push_back(e);
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

        // ---- diode pairs ----
        std::vector<int> pairOf(elements.size(), -1);
        for (std::size_t i = 0; i < diodes.size(); ++i)
        {
            const std::size_t di = diodes[i];
            if (pairOf[di] >= 0)
                continue;
            const std::vector<int>& ni = elementNodes[di];
            std::size_t partner = elements.size();
            for (std::size_t j = i + 1; j < diodes.size(); ++j)
            {
                const std::size_t dj = diodes[j];
                if (pairOf[dj] >= 0)
                    continue;
                const std::vector<int>& nj = elementNodes[dj];
                if (nj[0] == ni[1] && nj[1] == ni[0])
                {
                    partner = dj;
                    break;
                }
            }
            if (partner == elements.size())
            {
                messages.error(elements[di].loc, elements[di].name +
                                                     " has no antiparallel partner: Phase 0 solves antiparallel diode "
                                                     "pairs only");
                continue;
            }
            if (folded(elements[di].model) != folded(elements[partner].model))
            {
                pairOf[di] = pairOf[partner] = static_cast<int>(elements.size());   // reported: no second error
                messages.error(elements[partner].loc, "antiparallel diodes " + elements[di].name + ", " +
                                                          elements[partner].name +
                                                          " have different models: not supported yet");
                continue;
            }
            PortDef port;
            port.name = elementIdent[di];
            port.parts = elements[di].name + "|" + elements[partner].name;
            port.anode = ni[0];
            port.cathode = ni[1];
            port.from = ni[0];   // a two-terminal port: its current flows anode to cathode, through it
            port.to = ni[1];
            for (const ModelCard& m : netlist.models)
            {
                if (folded(m.name) == folded(elements[di].model))
                {
                    port.is = m.is;
                    port.n = m.n;
                }
            }
            port.nvt = thermalVoltage(port.n);
            port.elements = {static_cast<int>(di), static_cast<int>(partner)};
            pairOf[di] = static_cast<int>(ports.size());
            pairOf[partner] = static_cast<int>(ports.size());
            Piece piece;
            piece.branch.a = port.anode;
            piece.branch.b = port.cathode;
            piece.branch.part = port.parts;
            piece.branch.element = static_cast<int>(di);
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

        // ---- op amps: their outputs are sources ----
        std::map<int, std::size_t> driverOf;
        for (std::size_t k = 0; k < opamps.size(); ++k)
        {
            const OpAmp& op = opamps[k];
            const Loc loc = elements[static_cast<std::size_t>(op.element)].loc;
            if (op.out == 0)
                messages.error(loc, "op amp " + op.name + "'s output is ground");
            else if (op.out == p.inputNode)
                messages.error(loc, "op amp " + op.name + " drives .input's node '" +
                                        p.nodes[static_cast<std::size_t>(op.out)].name + "'");
            else if (const auto it = driverOf.find(op.out); it != driverOf.end())
                messages.error(loc, "node '" + p.nodes[static_cast<std::size_t>(op.out)].name +
                                        "' is driven by two op amps (" + opamps[it->second].name + ", " + op.name +
                                        ")");
            else
            {
                driverOf.emplace(op.out, k);
                p.nodes[static_cast<std::size_t>(op.out)].source = Node::Source::OpAmp;
            }
        }
        auto isSource = [&](int n)
        {
            return n == p.inputNode || driverOf.count(n) > 0;
        };

        // ---- DC paths: every node reaches ground, .input or an op amp's output through R, P or a diode pair ----
        {
            UnionFind dc(p.nodes.size());
            for (const Piece& piece : pieces)
            {
                // a resistor's, a pot's or a diode pair's; not a capacitor's, nor an OTA's (a current source)
                if ((piece.branch.kind != Branch::Kind::Capacitor || piece.port) && !piece.ota)
                    dc.join(static_cast<std::size_t>(piece.branch.a), static_cast<std::size_t>(piece.branch.b));
            }
            std::set<std::size_t> anchored{dc.find(0)};
            if (p.inputNode > 0)
                anchored.insert(dc.find(static_cast<std::size_t>(p.inputNode)));
            for (const auto& [out, k] : driverOf)
                anchored.insert(dc.find(static_cast<std::size_t>(out)));
            std::set<std::size_t> reported;
            for (std::size_t n = 1; n < p.nodes.size(); ++n)
            {
                const std::size_t root = dc.find(n);
                if (anchored.count(root) || reported.count(root))
                    continue;
                reported.insert(root);
                std::string caps;
                Loc loc;
                for (const Piece& piece : pieces)
                {
                    const Branch& b = piece.branch;
                    if (b.kind != Branch::Kind::Capacitor || piece.port || piece.ota)
                        continue;
                    if (dc.find(static_cast<std::size_t>(b.a)) == root ||
                        dc.find(static_cast<std::size_t>(b.b)) == root)
                    {
                        caps += (caps.empty() ? "" : ", ") + b.part;
                        if (loc.line == 0)
                            loc = elements[static_cast<std::size_t>(b.element)].loc;
                    }
                }
                if (caps.empty())
                    messages.error(loc,
                                   "node '" + p.nodes[n].name +
                                       "' has no path to ground: its DC voltage is undefined (add 10meg to ground)");
                else
                    messages.error(loc, "node '" + p.nodes[n].name + "' reaches ground only through capacitors (" +
                                            caps + "): its DC voltage is undefined (add 10meg to ground)");
            }
        }
        if (messages.hasErrors())
            return std::nullopt;

        // ---- the partition: components of the nodes no source drives ----
        UnionFind uf(p.nodes.size());
        auto regular = [&](int n)
        {
            return n != 0 && !isSource(n);
        };
        for (const Piece& piece : pieces)
        {
            const Branch& b = piece.branch;
            if (piece.ota)
            {
                // an OTA joins its four nodes into one block (those no source drives)
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
        const std::size_t followerBase = p.nodes.size();
        auto component = [&](std::size_t key)
        {
            const auto it = componentOf.find(key);
            if (it != componentOf.end())
                return it->second;
            const int c = static_cast<int>(componentPieces.size());
            componentOf.emplace(key, c);
            componentPieces.emplace_back();
            componentOpAmps.emplace_back();
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
            if (pieces[i].ota)
            {
                for (int n : pieces[i].nodes)
                {
                    if (c < 0 && regular(n))
                        c = component(uf.find(static_cast<std::size_t>(n)));
                }
                if (c < 0)
                {
                    messages.error(elements[static_cast<std::size_t>(element)].loc,
                                   "OTA " + b.part + "'s nodes are all ground or driven: nothing solves for it");
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
                // its feedback: some branch of inn's component reaches out
                bool reaches = false;
                for (std::size_t i : componentPieces[static_cast<std::size_t>(c)])
                    reaches = reaches || pieces[i].branch.a == op.out || pieces[i].branch.b == op.out;
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
                else
                {
                    block.branches.push_back(piece.branch);
                    parts.insert(piece.branch.element);
                }
                for (int n : piece.ota ? piece.nodes : std::vector<int>{piece.branch.a, piece.branch.b})
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
            block.elements.assign(parts.begin(), parts.end());
            block.nodes.assign(nodeSet.begin(), nodeSet.end());   // node indices are in order of first appearance
            std::stable_sort(block.branches.begin(), block.branches.end(),
                             [](const Branch& a, const Branch& b) { return a.element < b.element; });
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
                if (n == 0 || holder[static_cast<std::size_t>(n)] == static_cast<int>(c))
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
                                        "): not supported yet (Phase 1)");
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
                                                    " blocks read .input: Phase 0's blocks form one chain (Phase 1)");
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
                            "): Phase 0's blocks form one chain (Phase 1)");
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
                                            ") is off the chain from .input to .output: not supported yet (Phase 1)");
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
            for (const Branch& b : block.branches)
            {
                if (b.kind == Branch::Kind::Variable)
                {
                    for (const std::string& k : potsOf(b.expr))
                        keys.insert(k);
                }
            }
            if (block.port)
                keys.insert(block.port->gainPots.begin(), block.port->gainPots.end());
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
                messages.error(mixedAt, "the oversampled region mixes diode ports (" + diodeList + ") and OTA ports (" + otas +
                                       "): one solver serves a region (per-family solvers are Phase 2)");
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
        // The integrator: only TR yet, so an open .method is TR, and not the self-test's to choose (a BDF2 twin is
        // a candidate only for a block the ringing check flags: Phase 1's last slice).
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
        if (portBlocks.empty())
        {
            std::string worst;
            for (const Block& b : p.blocks)
            {
                if (b.oneX > kTolerance)
                    worst += std::format("; {} errs by {:.1f} dB at 1x (44.1 kHz)", blockLabel(b), b.oneX);
            }
            if (cfg.oversampleFrom != "default" || !worst.empty())
                messages.warning({}, "the netlist is linear: it runs at 1x (oversampling a linear netlist, and matched "
                                     "designs, are Phase 1)" +
                                         worst);
        }
        else
        {
            int first = portBlocks.front();
            int last = portBlocks.back();
            if (cfg.region == Region::All)
            {
                first = 0;
                last = static_cast<int>(p.blocks.size()) - 1;
            }
            else if (cfg.region == Region::Auto)
            {
                for (std::size_t i = 0; i < p.blocks.size(); ++i)
                {
                    if (!p.blocks[i].port && p.blocks[i].oneX > kTolerance)
                    {
                        first = std::min(first, static_cast<int>(i));
                        last = std::max(last, static_cast<int>(i));
                    }
                }
            }
            p.regionFirst = first;
            p.regionLast = last;
            for (int i = first; i <= last; ++i)
            {
                Block& b = p.blocks[static_cast<std::size_t>(i)];
                b.inRegion = true;
                b.rateFactor = cfg.oversample;
                if (b.port && cfg.adaa)
                    b.designFactor = 2.0 / 3.0;
            }
            for (std::size_t i = 0; i < p.blocks.size(); ++i)
            {
                Block& b = p.blocks[i];
                if (b.port || !pivotsOf[i])
                    continue;
                if (b.inRegion)
                {
                    b.oneXRegion =
                        oneXError(systems[i], *pivotsOf[i], p, static_cast<int>(i), kCheckRate * b.rateFactor);
                    if (b.oneXRegion > kTolerance)
                        messages.warning(blockLoc(b),
                                         std::format("{} errs by {:.2f} dB at {}x 44.1 kHz, more than {} dB even "
                                                     "oversampled (matched designs are Phase 1)",
                                                     blockLabel(b), b.oneXRegion, b.rateFactor, kTolerance));
                }
                else if (b.oneX > kTolerance)
                    messages.warning(blockLoc(b), std::format("{} errs by {:.2f} dB at 1x (44.1 kHz), more than {} dB: "
                                                              "blocks=auto would oversample it",
                                                              blockLabel(b), b.oneX, kTolerance));
            }
        }

        // ---- prewarp: each OTA's small-signal pole, fitted as a + b g ----
        if (cfg.prewarp)
            fitPrewarp(p, systems, messages);
        return p;
    }
} // namespace fminus::circuit
