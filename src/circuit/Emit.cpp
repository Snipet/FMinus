// src/circuit/Emit.cpp: Val's arithmetic, the Code builder, and the module's text (Emit.h). The emitted shapes are
// mkbench.py's (experiments/), whose costs the report measured: the omega3 guess, Newton steps in a bracket, the ADAA
// tail, halfband methods on prefixed fields, and tick's region samples in field arrays.
#include "circuit/Emit.h"

#include "circuit/Halfbands.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <format>
#include <map>
#include <stdexcept>

namespace fminus::circuit
{
    // ---- Val --------------------------------------------------------------------------------------------------------

    Val::Val(double value)
    {
        if (value != 0)
        {
            kind = Kind::Const;
            c = value;
        }
    }

    Val Val::var(std::string name, Code* code)
    {
        Val v;
        v.kind = Kind::Var;
        v.name = std::move(name);
        v.code = code;
        return v;
    }

    bool isZero(const Val& v)
    {
        return v.kind == Val::Kind::Zero;
    }

    bool isUnit(const Val& v)
    {
        return v.kind == Val::Kind::Const && (v.c == 1 || v.c == -1);
    }

    namespace
    {
        Code* codeOf(const Val& a, const Val& b)
        {
            Code* code = a.code != nullptr ? a.code : b.code;
            if (code == nullptr)
                throw std::logic_error("Val: an operation on a Var with no Code");
            return code;
        }

        // An operand as a sign and an unsigned text.
        struct Signed
        {
            bool negative = false;
            std::string text;
            std::vector<std::string> uses;
        };

        Signed signedOf(const Val& v)
        {
            if (v.kind == Val::Kind::Const)
                return {v.c < 0, literal(std::fabs(v.c), true), {}};
            return {v.negated, v.name, {v.name}};
        }

        Val temp(Code* code, const std::string& text, std::vector<std::string> uses, bool negative)
        {
            Val t = code->temp(text, uses);
            t.negated = negative;
            return t;
        }

        std::vector<std::string> joined(std::vector<std::string> a, const std::vector<std::string>& b)
        {
            a.insert(a.end(), b.begin(), b.end());
            return a;
        }
    } // namespace

    Val operator-(const Val& a)
    {
        Val r = a;
        if (r.kind == Val::Kind::Const)
            r.c = -r.c;
        else if (r.kind == Val::Kind::Var)
            r.negated = !r.negated;
        return r;
    }

    Val operator+(const Val& a, const Val& b)
    {
        if (isZero(a))
            return b;
        if (isZero(b))
            return a;
        if (a.kind == Val::Kind::Const && b.kind == Val::Kind::Const)
            return Val(a.c + b.c);
        if (a.kind == Val::Kind::Var && b.kind == Val::Kind::Var && a.name == b.name && a.negated != b.negated)
            return Val();
        Code* code = codeOf(a, b);
        const Signed x = signedOf(a);
        const Signed y = signedOf(b);
        const std::vector<std::string> uses = joined(x.uses, y.uses);
        if (!x.negative && !y.negative)
            return temp(code, x.text + " + " + y.text, uses, false);
        if (!x.negative && y.negative)
            return temp(code, x.text + " - " + y.text, uses, false);
        if (x.negative && !y.negative)
            return temp(code, y.text + " - " + x.text, uses, false);
        return temp(code, x.text + " + " + y.text, uses, true);
    }

    Val operator-(const Val& a, const Val& b)
    {
        return a + (-b);
    }

    Val operator*(const Val& a, const Val& b)
    {
        if (isZero(a) || isZero(b))
            return Val();
        if (a.kind == Val::Kind::Const && b.kind == Val::Kind::Const)
            return Val(a.c * b.c);
        if (isUnit(a))
            return a.c > 0 ? b : -b;
        if (isUnit(b))
            return b.c > 0 ? a : -a;
        Code* code = codeOf(a, b);
        const Signed x = signedOf(a);
        const Signed y = signedOf(b);
        const bool negative = x.negative != y.negative;
        // a constant first: 0.5f * x
        if (b.kind == Val::Kind::Const)
            return temp(code, y.text + " * " + x.text, joined(x.uses, y.uses), negative);
        return temp(code, x.text + " * " + y.text, joined(x.uses, y.uses), negative);
    }

    Val operator/(const Val& a, const Val& b)
    {
        if (isZero(b))
            throw std::logic_error("Val: a division by a structural zero");
        if (isZero(a))
            return Val();
        if (a.kind == Val::Kind::Const && b.kind == Val::Kind::Const)
            return Val(a.c / b.c);
        if (isUnit(b))
            return b.c > 0 ? a : -a;
        Code* code = codeOf(a, b);
        const Signed y = signedOf(b);
        if (a.kind == Val::Kind::Const)
        {
            // a constant's sign stays on it: -1 / t10 is K, positive
            const double numerator = y.negative ? -a.c : a.c;
            return temp(code, literal(numerator, true) + " / " + y.text, y.uses, false);
        }
        const Signed x = signedOf(a);
        return temp(code, x.text + " / " + y.text, joined(x.uses, y.uses), x.negative != y.negative);
    }

    Val logOf(const Val& v)
    {
        if (v.kind == Val::Kind::Const && v.c > 0)
            return Val(std::log(v.c));
        if (v.kind != Val::Kind::Var || v.negated)
            throw std::logic_error("Val: the log of a value that is not positive");
        return temp(v.code, "logf(" + v.name + ")", {v.name}, false);
    }

    void annotate(Val& v, const std::string& comment)
    {
        if (v.kind == Val::Kind::Var && v.code != nullptr)
            v.code->comment(v, (v.negated ? "minus " : "") + comment);
    }

    Val bindField(const std::string& field, Val value)
    {
        if (value.kind != Val::Kind::Var)
            return value;
        Code* code = value.code;
        code->assign(field, value);
        code->fields_.push_back(field);
        return Val::var(field, code);
    }

    std::string operandText(const Val& v)
    {
        if (v.kind == Val::Kind::Zero)
            return "0";
        if (v.kind == Val::Kind::Const)
            return literal(v.c);
        return (v.negated ? "-" : "") + v.name;
    }

    // ---- Code -------------------------------------------------------------------------------------------------------

    Code::Code(std::set<std::string> reserved)
        : reserved_(std::move(reserved))
    {
    }

    Val Code::temp(const std::string& text, const std::vector<std::string>& uses)
    {
        // the same expression again (the elimination meets a product twice) is the same temporary
        if (const auto it = seen_.find(text); it != seen_.end())
            return Val::var(it->second, this);
        std::string name;
        do
            name = "t" + std::to_string(next_++);
        while (reserved_.count(name));
        seen_.emplace(text, name);
        stmts_.push_back({name, true, text, {}, uses});
        return Val::var(name, this);
    }

    void Code::assign(const std::string& field, const Val& v)
    {
        std::vector<std::string> uses;
        if (v.kind == Val::Kind::Var)
            uses.push_back(v.name);
        stmts_.push_back({field, false, operandText(v), {}, uses});
    }

    void Code::raw(const std::string& text, const std::vector<std::string>& uses, const std::string& comment)
    {
        stmts_.push_back({{}, false, text, comment, uses});
    }

    void Code::comment(const Val& v, const std::string& text)
    {
        if (!stmts_.empty() && stmts_.back().declares && stmts_.back().lhs == v.name && stmts_.back().note.empty())
            stmts_.back().note = text;
    }

    namespace
    {
        std::string withComment(std::string line, const std::string& note, std::size_t column = 56)
        {
            if (note.empty())
                return line;
            if (line.size() + 2 < column)
                line.append(column - line.size(), ' ');
            else
                line += "  ";
            return line + "// " + note;
        }
    } // namespace

    std::vector<std::string> Code::lines() const
    {
        std::map<std::string, int> uses;
        for (const Stmt& s : stmts_)
        {
            for (const std::string& u : s.uses)
                ++uses[u];
        }
        std::vector<std::string> out;
        for (std::size_t i = 0; i < stmts_.size(); ++i)
        {
            const Stmt& s = stmts_[i];
            if (s.lhs.empty())
            {
                out.push_back(withComment(s.rhs, s.note));
                continue;
            }
            if (s.declares && i + 1 < stmts_.size())
            {
                const Stmt& next = stmts_[i + 1];
                if (!next.declares && !next.lhs.empty() && next.rhs == s.lhs && uses[s.lhs] == 1)
                {
                    out.push_back(withComment(next.lhs + " = " + s.rhs + ";", s.note));
                    ++i;
                    continue;
                }
            }
            out.push_back(withComment((s.declares ? "const float " : "") + s.lhs + " = " + s.rhs + ";", s.note));
        }
        return out;
    }

    // ---- keywords ---------------------------------------------------------------------------------------------------

    bool isFMinusKeyword(std::string_view name)
    {
        static constexpr auto kWords =
            std::to_array<std::string_view>({// keywords (src/frontend/Lexer.l)
                                             "float", "const", "return", "bool", "int", "void", "restrict", "sizeof",
                                             "float2", "float4", "float8", "poly", "true", "false", "if", "else",
                                             "while", "for", "break", "continue", "parallel", "module", "this", "param",
                                             // builtins (src/ir/Calls.cpp), and main, a program's entry
                                             "fabsf", "sqrtf", "sinf", "cosf", "expf", "logf", "floorf", "fminf",
                                             "fmaxf", "tanhf", "tanf", "malloc", "calloc", "free", "main"});
        return std::find(kWords.begin(), kWords.end(), name) != kWords.end();
    }

    namespace
    {
        // omega3's breakpoints and cubic (D'Angelo, Gabrielli, Turchet, DAFx-19), as adaa.cpp has them; ln 81, the
        // audio taper's (81^x - 1) / 80 as exp; ADAA's |dp| below which the mean is the midpoint's.
        constexpr double kOmegaLow = -3.341459552768620;
        constexpr double kOmegaA = -1.314293149877800e-3;
        constexpr double kOmegaB = 4.775931364975583e-2;
        constexpr double kOmegaC = 3.631952663804445e-1;
        constexpr double kOmegaD = 6.313183464296682e-1;
        constexpr double kLn81 = 4.394449154672439;
        constexpr double kAdaaEpsilon = 1e-5;
    } // namespace

    // ---- a block's coefficients -------------------------------------------------------------------------------------

    std::optional<BlockCode> deriveBlock(const Plan& plan, int blockIndex, Messages& messages)
    {
        BlockCode bc;
        bc.system = assemble(plan, blockIndex);
        std::optional<Pivots> pivots = choosePivots(bc.system, plan, blockIndex, messages);
        if (!pivots)
            return std::nullopt;
        bc.pivots = *pivots;
        const Block& block = plan.blocks[static_cast<std::size_t>(blockIndex)];
        auto potName = [&](const std::string& key)
        {
            for (const PotInfo& p : plan.pots)
            {
                if (p.key == key)
                    return p.name;
            }
            return key;
        };
        std::set<std::string> reserved;
        for (const std::string& key : block.pots)
            reserved.insert(potName(key));
        Code code(reserved);
        // The variable resistances, from the (clamped) pots.
        std::set<int> taperDone;
        for (const Branch& br : block.branches)
        {
            if (br.kind != Branch::Kind::Variable)
                continue;
            const Element& el = plan.netlist.elements[static_cast<std::size_t>(br.element)];
            const std::string note =
                el.text + (el.kind == Element::Kind::Pot ? std::string(", ") + taperName(br.taper) : "");
            const std::string expr = printExpr(br.expr, potName);
            if (br.section == 0)
            {
                bc.rLines.push_back(withComment("const float r_" + br.ident + " = " + expr + ";", note));
                continue;
            }
            const std::string clamped = "fminf(fmaxf(" + expr + ", 0), 1)";
            const std::string tapered =
                br.taper == Taper::Lin     ? clamped
                : br.taper == Taper::Audio ? "(expf(" + literal(kLn81) + " * " + clamped + ") - 1) * 0.0125f"
                                           : "1 - (expf(" + literal(kLn81) + " * (1 - " + clamped + ")) - 1) * 0.0125f";
            const bool rheostat = el.nodes.size() == 2;
            if (rheostat && br.taper == Taper::Lin)
            {
                bc.rLines.push_back(
                    withComment("const float r_" + br.ident + " = " + literal(br.value) + " * " + clamped + ";", note));
                continue;
            }
            const std::string t = "t_" + (rheostat ? br.ident : br.ident.substr(0, br.ident.size() - 2));
            if (taperDone.insert(br.element).second)
                bc.rLines.push_back(withComment("const float " + t + " = " + tapered + ";", note));
            if (br.section == 1)
                bc.rLines.push_back("const float r_" + br.ident + " = " + literal(br.value) + " * (1 - " + t + ");");
            else
                bc.rLines.push_back("const float r_" + br.ident + " = " + literal(br.value) + " * " + t + ";");
        }
        std::vector<Val> symbols;
        for (const System::Symbol& s : bc.system.symbols)
            symbols.push_back(Val::var(s.name, &code));
        const Linear<Val> linear = solve(bc.system, bc.pivots, symbols);
        bc.coefficients = derive(bc.system, linear, block, plan.config.adaa && block.port.has_value());
        for (auto& [name, v] : bc.coefficients)
            v.code = nullptr;
        bc.lines = code.lines();
        return bc;
    }

    // ---- the module -------------------------------------------------------------------------------------------------

    namespace
    {
        std::string engineering(double value, const char* unit)
        {
            struct Prefix
            {
                double scale;
                const char* name;
            };
            static constexpr std::array<Prefix, 7> kPrefixes{
                {{1e-15, "f"}, {1e-12, "p"}, {1e-9, "n"}, {1e-6, "u"}, {1e-3, "m"}, {1, ""}, {1e3, "k"}}};
            Prefix chosen = kPrefixes[0];
            for (const Prefix& p : kPrefixes)
            {
                if (std::fabs(value) >= p.scale * 0.9999999)
                    chosen = p;
            }
            return std::format("{:.5g} {}{}", value / chosen.scale, chosen.name, unit);
        }

        std::string partsText(const Plan& plan, const Block& block)
        {
            std::string out;
            for (int e : block.elements)
                out += (out.empty() ? "" : ", ") + plan.netlist.elements[static_cast<std::size_t>(e)].name;
            return out;
        }

        std::string rateText(const Block& b)
        {
            return b.rateFactor == 1 ? std::string("the host rate") : std::to_string(b.rateFactor) + " rate";
        }

        int stagesOf(int oversample)
        {
            return oversample == 8 ? 3 : oversample == 4 ? 2 : oversample == 2 ? 1 : 0;
        }

        std::string configText(const Config& c)
        {
            const std::string solver = std::string(c.solver.guess == Solver::Guess::Omega ? "omega(" : "newton(") +
                                       std::to_string(c.solver.steps) + ")";
            const std::string region = c.region == Region::Auto        ? "auto"
                                       : c.region == Region::Nonlinear ? "nonlinear"
                                                                       : "all";
            std::vector<std::pair<std::string, std::string>> items{
                {".oversample " + std::to_string(c.oversample), c.oversampleFrom},
                {c.halfband == Halfband::Iir ? "iir" : "fir", c.halfbandFrom},
                {"blocks=" + region, c.regionFrom},
                {std::string(".adaa ") + (c.adaa ? "on" : "off"), c.adaaFrom},
                {".solver " + solver, c.solverFrom},
            };
            if (c.channels != 1 || c.channelsFrom != "default")
                items.emplace_back(".channels " + std::to_string(c.channels), c.channelsFrom);
            const bool same = std::all_of(items.begin(), items.end(),
                                          [&](const auto& i) { return i.second == items.front().second; });
            auto fromName = [](const std::string& from)
            {
                return from == "default" ? std::string("the defaults")
                       : from == "auto"  ? std::string("auto: Phase 0's default")
                                         : from;
            };
            std::string text = same ? "configuration from " + fromName(items.front().second) + ": " : "configuration: ";
            for (std::size_t i = 0; i < items.size(); ++i)
            {
                text += (i == 0 ? "" : i <= 2 ? " " : ", ") + items[i].first;
                if (!same)
                    text += " (" + fromName(items[i].second) + ")";
            }
            return text + "; no self-test (Phase 1)";
        }

        struct Writer
        {
            const Plan& plan;
            const std::vector<BlockCode>& blocks;
            std::string type;   // float or float2: the signals and states
            std::string out;

            void line(const std::string& text, const std::string& note = {}, std::size_t column = 60)
            {
                out += withComment(text, note, column) + "\n";
            }

            bool adaaBlock(const Block& b) const
            {
                return b.port.has_value() && plan.config.adaa;
            }

            Val coefficient(std::size_t b, const std::string& name) const
            {
                for (const auto& [n, v] : blocks[b].coefficients)
                {
                    if (n == name)
                        return v;
                }
                return Val();
            }

            // Whether a readout or a state of block i reads its input (ADAA then keeps it a step).
            bool usesInput(std::size_t i) const
            {
                const System& s = blocks[i].system;
                const std::string suffix = "_by_" + s.sources[static_cast<std::size_t>(s.input)].name;
                for (const auto& [name, v] : blocks[i].coefficients)
                {
                    if (isZero(v) || name.rfind("p_", 0) == 0)
                        continue;
                    if (name.size() > suffix.size() &&
                        name.compare(name.size() - suffix.size(), suffix.size(), suffix) == 0)
                        return true;
                }
                return false;
            }

            // A readout's terms over the step's sources.
            std::vector<std::pair<Val, std::string>> terms(std::size_t i, const std::string& readout,
                                                           const std::vector<std::string>& sourceText) const
            {
                const System& s = blocks[i].system;
                std::vector<std::pair<Val, std::string>> t;
                for (std::size_t k = 0; k < s.sources.size(); ++k)
                    t.emplace_back(coefficient(i, readout + "_by_" + s.sources[k].name), sourceText[k]);
                return t;
            }

            // sum of coefficient * source, as FMinus text ("0" when every term is zero).
            static std::string linear(const std::vector<std::pair<Val, std::string>>& terms)
            {
                std::string text;
                for (const auto& [coef, source] : terms)
                {
                    if (isZero(coef))
                        continue;
                    bool negative = false;
                    std::string term;
                    if (coef.kind == Val::Kind::Const)
                    {
                        negative = coef.c < 0;
                        term = std::fabs(coef.c) == 1 ? source : literal(std::fabs(coef.c)) + " * " + source;
                    }
                    else
                    {
                        negative = coef.negated;
                        term = coef.name + " * " + source;
                    }
                    if (text.empty())
                        text = negative ? "-" + term : term;
                    else
                        text += (negative ? " - " : " + ") + term;
                }
                return text.empty() ? "0" : text;
            }

            // A readout that is +-1 times one source is that source, signed.
            static std::optional<std::string> inlined(const std::vector<std::pair<Val, std::string>>& terms)
            {
                const std::pair<Val, std::string>* only = nullptr;
                for (const auto& t : terms)
                {
                    if (isZero(t.first))
                        continue;
                    if (only != nullptr)
                        return std::nullopt;
                    only = &t;
                }
                if (only == nullptr || !isUnit(only->first))
                    return std::nullopt;
                return (only->first.c < 0 ? "-" : "") + only->second;
            }

            void header();
            void fields();
            void init();
            void clear();
            void set();
            void blockSet(std::size_t i);
            void blockStep(std::size_t i);
            void step();
            void tick();
        };

        void Writer::header()
        {
            const Config& c = plan.config;
            line("// " + plan.module + ": generated from " + plan.baseName +
                 " by fminus::circuit (Phase 0). Do not edit: edit " + plan.baseName + ".");
            std::string states;
            int stateCount = 0;
            int portCount = 0;
            std::string ports;
            for (const Block& b : plan.blocks)
            {
                for (const Branch& br : b.branches)
                {
                    if (br.kind == Branch::Kind::Capacitor)
                    {
                        states += (states.empty() ? "" : ", ") + br.part;
                        ++stateCount;
                    }
                }
                if (b.port)
                {
                    ++portCount;
                    ports += (ports.empty() ? "" : "; ") + b.port->parts + ": 2 Is sinh(v / N Vt), Is " +
                             engineering(b.port->is, "A") + ", N Vt " + engineering(b.port->nvt, "V") + " at 27 C";
                }
            }
            std::string pots;
            for (const PotInfo& p : plan.pots)
                pots += (pots.empty() ? "" : ", ") + p.name;
            line("//   " + std::to_string(plan.blocks.size()) + (plan.blocks.size() == 1 ? " block, " : " blocks, ") +
                 std::to_string(stateCount) + (stateCount == 1 ? " state" : " states") +
                 (states.empty() ? "" : " (" + states + ")") + ", " + std::to_string(portCount) +
                 (portCount == 1 ? " nonlinear port" : " nonlinear ports") + (ports.empty() ? "" : " (" + ports + ")") +
                 ", " + std::to_string(plan.pots.size()) + (plan.pots.size() == 1 ? " pot" : " pots") +
                 (pots.empty() ? "" : " (" + pots + ")"));
            for (std::size_t i = 0; i < plan.blocks.size(); ++i)
            {
                const Block& b = plan.blocks[i];
                std::string text = "//   block " + std::to_string(b.index) + " (" + partsText(plan, b) +
                                   "): " + plan.nodes[static_cast<std::size_t>(b.input)].name + " -> " +
                                   plan.nodes[static_cast<std::size_t>(b.output)].name + "; ";
                if (i > 0)
                {
                    const Block& before = plan.blocks[i - 1];
                    const bool opampOut =
                        std::find(before.driven.begin(), before.driven.end(), b.input) != before.driven.end();
                    text += opampOut ? "cut at an op amp's output (exact); " : "cut at an op amp's + input (exact); ";
                }
                if (b.port)
                {
                    const std::string an = plan.nodes[static_cast<std::size_t>(b.port->anode)].name;
                    const std::string ca = plan.nodes[static_cast<std::size_t>(b.port->cathode)].name;
                    const std::string at = b.port->cathode == 0 ? "V(" + an + ")"
                                           : b.port->anode == 0 ? "-V(" + ca + ")"
                                                                : "V(" + an + ") - V(" + ca + ")";
                    text += "port " + b.port->parts + " at " + at + "; at " + rateText(b);
                    if (adaaBlock(b))
                        text += ", designed at " + rateText(b) + " 2/3 (ADAA)";
                }
                else
                {
                    text += "linear; at " + rateText(b);
                    if (b.inRegion)
                        text += std::format(" (1x check: {:.2f} dB off at 44.1 kHz, {:.2f} dB at {}x)", b.oneX,
                                            b.oneXRegion, b.rateFactor);
                    else
                        text += std::format(" (1x check: {:.2f} dB off at 44.1 kHz{})", b.oneX,
                                            b.oneX > 0.5 ? ", more than 0.5" : "");
                }
                line(text);
            }
            if (plan.regionFirst >= 0)
            {
                std::string text = "//   solver: ";
                text += c.solver.guess == Solver::Guess::Omega ? "omega3 + " : "the previous step's v + ";
                text += std::to_string(c.solver.steps) + (c.solver.steps == 1 ? " Newton step" : " Newton steps");
                text += " in [min(0, p), max(0, p)]";
                if (c.adaa)
                    text += "; ADAA (Holters 2020, closed-form F)";
                line(text);
            }
            line("//   " + configText(c));
            const TargetLine& t = plan.netlist.target;
            if (t.loc.line > 0)
            {
                std::string text;
                if (t.solver)
                    text += std::format(" solver {:g} dB", *t.solver);
                if (t.alias)
                    text += std::string(text.empty() ? "" : ",") + std::format(" alias {:g} dB", *t.alias);
                if (t.level)
                    text += std::format(" at {:g} V", *t.level);
                if (t.freq)
                    text += std::string(t.level ? "," : " at") + std::format(" {:g} kHz", *t.freq / 1000);
                line("//   .target:" + text + " (checked by examples/circuits --check, not here)");
            }
            if (plan.regionFirst >= 0)
            {
                std::string text = c.oversample == 1
                                       ? std::string("//   latency: none from oversampling (1x)")
                                       : std::format("//   latency: about {:.1f} host samples at 1 kHz ({})",
                                                     roundTripDelay(c.oversample, c.halfband, 1000),
                                                     c.halfband == Halfband::Iir ? "IIR halfbands, phase not linear"
                                                                                 : "FIR halfbands, linear phase");
                bool adaa = false;
                for (int i = plan.regionFirst; i <= plan.regionLast; ++i)
                    adaa = adaa || adaaBlock(plan.blocks[static_cast<std::size_t>(i)]);
                if (adaa)
                    text += std::format(", plus {:g} (ADAA)", 0.5 / c.oversample);
                line(text);
            }
            else
                line("//   latency: none (1x)");
        }

        void Writer::fields()
        {
            bool first = true;
            for (const Block& b : plan.blocks)
            {
                for (const Branch& br : b.branches)
                {
                    if (br.kind != Branch::Kind::Capacitor)
                        continue;
                    line("    " + type + " x_" + br.ident + ";",
                         first ? br.part + "'s companion state (trapezoidal): x <- 2 gc v - x" : "", 31);
                    first = false;
                }
            }
            for (std::size_t i = 0; i < plan.blocks.size(); ++i)
            {
                const Block& b = plan.blocks[i];
                if (!adaaBlock(b))
                    continue;
                bool firstX = true;
                for (const Branch& br : b.branches)
                {
                    if (br.kind != Branch::Kind::Capacitor)
                        continue;
                    line("    " + type + " xo_" + br.ident + ";",
                         firstX ? "ADAA: x_" + br.ident + " a step earlier" : "", 31);
                    firstX = false;
                }
            }
            for (std::size_t i = 0; i < plan.blocks.size(); ++i)
            {
                const Block& b = plan.blocks[i];
                if (!b.port)
                    continue;
                const std::string& p = b.port->name;
                line("    " + type + " v_" + p + ";", b.port->parts + ": the port's voltage at the last step", 31);
                if (!adaaBlock(b))
                    continue;
                line("    " + type + " p_" + p + ";", "ADAA: the port's open-circuit voltage at the last step", 31);
                line("    " + type + " c_" + p + ";", "ADAA: 2 cosh(v_" + p + " / N Vt)", 31);
                if (usesInput(i))
                    line("    " + type + " uo_" + blocks[i].system.sources[0].name + ";",
                         "ADAA: block " + std::to_string(b.index) + "'s input a step earlier", 31);
            }
            first = true;
            for (const Block& b : plan.blocks)
            {
                for (const Branch& br : b.branches)
                {
                    if (br.kind != Branch::Kind::Capacitor)
                        continue;
                    line("    float gc_" + br.ident + ";", first ? "2 C fs', the companion's conductance" : "", 31);
                    first = false;
                }
            }
            first = true;
            for (const Block& b : plan.blocks)
            {
                for (const Branch& br : b.branches)
                {
                    if (br.kind != Branch::Kind::Capacitor)
                        continue;
                    line("    float g2_" + br.ident + ";", first ? "2 gc" : "", 31);
                    first = false;
                }
            }
            for (std::size_t i = 0; i < plan.blocks.size(); ++i)
            {
                bool firstField = true;
                for (const auto& [name, v] : blocks[i].coefficients)
                {
                    if (v.kind != Val::Kind::Var)
                        continue;
                    line("    float " + name + ";",
                         firstField ? "block " + std::to_string(i + 1) + "'s coefficients" : "", 31);
                    firstField = false;
                }
            }
            if (plan.regionFirst < 0)
                return;
            const int stages = stagesOf(plan.config.oversample);
            for (int s = 1; s <= stages; ++s)
                emitHalfbandFields(out, s, plan.config.halfband, type);
            for (int s = 1; s <= stages; ++s)
            {
                const int count = 1 << (s - 1);
                for (int j = 0; j < count; ++j)
                {
                    const std::string name = s == 1 ? "os1" : "os" + std::to_string(s) + "_" + std::to_string(j);
                    std::string note;
                    if (j == 0)
                        note = s == 1 ? "a host sample, as two at 2 rate"
                                      : std::format("a sample at {} rate, as two at {} rate", count, 2 * count);
                    line("    " + type + " " + name + "[2];", note, 31);
                }
            }
        }

        void Writer::init()
        {
            line("    void init(float rate) [[clang::nonblocking]]", "the host's rate");
            line("    {");
            for (const Block& b : plan.blocks)
            {
                for (const Branch& br : b.branches)
                {
                    if (br.kind != Branch::Kind::Capacitor)
                        continue;
                    const double factor = 2 * br.c * b.rateFactor * b.designFactor;
                    const std::string fs =
                        (b.rateFactor == 1 ? std::string("rate") : std::to_string(b.rateFactor) + " rate") +
                        (b.designFactor != 1 ? " 2/3" : "");
                    line("        gc_" + br.ident + " = " + literal(factor) + " * rate;",
                         "2 " + br.part + " fs', fs' = " + fs);
                }
            }
            for (const Block& b : plan.blocks)
            {
                for (const Branch& br : b.branches)
                {
                    if (br.kind == Branch::Kind::Capacitor)
                        line("        g2_" + br.ident + " = 2 * gc_" + br.ident + ";");
                }
            }
            for (std::size_t i = 0; i < plan.blocks.size(); ++i)
            {
                if (!plan.blocks[i].pots.empty() || blocks[i].lines.empty())
                    continue;
                line("        {", "block " + std::to_string(i + 1) + "'s coefficients (it has no pots)");
                for (const std::string& l : blocks[i].lines)
                    line("            " + l);
                line("        }");
            }
            line("        clear();");
            if (!plan.pots.empty())
            {
                std::string args;
                std::string note;
                for (const PotInfo& p : plan.pots)
                {
                    args += (args.empty() ? "" : ", ") + literal(p.defaultValue);
                    note += (note.empty() ? ".pot " : ", ") + p.name + std::format(" {:g}", p.defaultValue);
                }
                line("        set(" + args + ");", note);
            }
            line("    }");
        }

        void Writer::clear()
        {
            line("    void clear() [[clang::nonblocking]]", "the DC operating point: no bias, 0 V everywhere");
            line("    {");
            for (const Block& b : plan.blocks)
            {
                for (const Branch& br : b.branches)
                {
                    if (br.kind == Branch::Kind::Capacitor)
                        line("        x_" + br.ident + " = 0;");
                }
            }
            for (std::size_t i = 0; i < plan.blocks.size(); ++i)
            {
                const Block& b = plan.blocks[i];
                if (adaaBlock(b))
                {
                    for (const Branch& br : b.branches)
                    {
                        if (br.kind == Branch::Kind::Capacitor)
                            line("        xo_" + br.ident + " = 0;");
                    }
                }
                if (!b.port)
                    continue;
                const std::string& p = b.port->name;
                line("        v_" + p + " = 0;");
                if (!adaaBlock(b))
                    continue;
                line("        p_" + p + " = 0;");
                line("        c_" + p + " = 2;");
                if (usesInput(i))
                    line("        uo_" + blocks[i].system.sources[0].name + " = 0;");
            }
            if (plan.regionFirst >= 0)
            {
                for (int s = 1; s <= stagesOf(plan.config.oversample); ++s)
                    emitHalfbandClear(out, s, plan.config.halfband);
            }
            line("    }");
        }

        void Writer::set()
        {
            if (plan.pots.empty())
                return;
            std::string params;
            std::string names;
            for (const PotInfo& p : plan.pots)
            {
                params += (params.empty() ? "" : ", ") + std::string("float ") + p.name;
                names += (names.empty() ? "" : ", ") + p.name;
            }
            line("    void set(" + params + ") [[clang::nonblocking]]",
                 ".pot " + names + ", 0 to 1: call it on the absolute 16-sample grid");
            line("    {");
            for (const Block& b : plan.blocks)
            {
                if (b.pots.empty())
                    continue;
                std::string args;
                for (const std::string& key : b.pots)
                {
                    for (const PotInfo& p : plan.pots)
                    {
                        if (p.key == key)
                            args += (args.empty() ? "" : ", ") + std::string("fminf(fmaxf(") + p.name + ", 0), 1)";
                    }
                }
                line("        b" + std::to_string(b.index) + "_set(" + args + ");");
            }
            line("    }");
        }

        void Writer::blockSet(std::size_t i)
        {
            const Block& b = plan.blocks[i];
            if (b.pots.empty())
                return;
            std::string params;
            for (const std::string& key : b.pots)
            {
                for (const PotInfo& p : plan.pots)
                {
                    if (p.key == key)
                        params += (params.empty() ? "" : ", ") + std::string("float ") + p.name;
                }
            }
            line("    void b" + std::to_string(b.index) + "_set(" + params + ") [[clang::nonblocking]]",
                 "block " + std::to_string(b.index) + "'s coefficients");
            line("    {");
            for (const std::string& l : blocks[i].rLines)
                line("        " + l);
            for (const std::string& l : blocks[i].lines)
                line("        " + l);
            line("    }");
        }

        void Writer::blockStep(std::size_t i)
        {
            const Block& b = plan.blocks[i];
            const System& s = blocks[i].system;
            const bool adaa = adaaBlock(b);
            const std::string& in = plan.inputName;
            const std::string T = type;
            line("    " + T + " b" + std::to_string(b.index) + "(" + T + " " + in + ") [[clang::nonblocking]]",
                 "block " + std::to_string(b.index) + ", one step at " + rateText(b));
            line("    {");
            std::vector<std::string> pText;
            std::vector<std::string> rText;
            for (const System::Source& src : s.sources)
            {
                switch (src.kind)
                {
                case System::Source::Kind::Input:
                    pText.push_back(in);
                    rText.push_back(adaa ? "ua" : in);
                    break;
                case System::Source::Kind::State:
                    pText.push_back(src.name);
                    rText.push_back(adaa ? "xa_" + src.name.substr(2) : src.name);
                    break;
                case System::Source::Kind::Port:
                    pText.push_back("v");
                    rText.push_back(adaa ? "vb" : "v");
                    break;
                }
            }
            if (b.port)
            {
                const std::string& P = b.port->name;
                const std::string ivt = literal(1 / b.port->nvt);
                const std::string nvt = literal(b.port->nvt);
                std::vector<std::pair<Val, std::string>> pTerms;
                for (std::size_t k = 0; k < s.sources.size(); ++k)
                {
                    if (static_cast<int>(k) != s.port)
                        pTerms.emplace_back(coefficient(i, "p_" + P + "_by_" + s.sources[k].name), pText[k]);
                }
                line("        const " + T + " p = " + linear(pTerms) + ";");
                const Solver& solver = plan.config.solver;
                if (solver.guess == Solver::Guess::Omega)
                {
                    line("        const " + T + " q = fabsf(p);",
                         "omega3 (D'Angelo et al. 2019): the conducting diode alone");
                    line("        const " + T + " arg = (q + ka_" + P + ") * " + ivt + " + lna_" + P + ";");
                    line("        const " + T + " w = arg <= " + literal(kOmegaLow) + " ? 0");
                    line("                      : arg < 8 ? ((" + literal(kOmegaA) + " * arg + " + literal(kOmegaB) +
                         ") * arg + " + literal(kOmegaC) + ") * arg + " + literal(kOmegaD));
                    line("                                : arg - logf(fmaxf(arg, 8));");
                    line("        " + T + " v = q + ka_" + P + " - " + nvt + " * w;");
                    line("        v = p < 0 ? -v : v;");
                    line("        const " + T + " lo = fminf(p, 0);");
                    line("        const " + T + " hi = fmaxf(p, 0);");
                    if (solver.steps == 0)
                        line("        v = fminf(fmaxf(v, lo), hi);", "no Newton step: the guess, in the bracket");
                }
                else
                {
                    line("        const " + T + " lo = fminf(p, 0);");
                    line("        const " + T + " hi = fmaxf(p, 0);");
                    line("        " + T + " v = fminf(fmaxf(v_" + P + ", lo), hi);",
                         "the previous step's solution, in the bracket");
                }
                for (int k = 1; k <= solver.steps; ++k)
                {
                    line("        {", std::format("Newton step {} of {}, kept in the bracket", k, solver.steps));
                    line("            const " + T + " e = expf(v * " + ivt + ");");
                    line("            const " + T + " ei = 1 / e;");
                    line("            const " + T + " f = v - p + ka_" + P + " * (e - ei);");
                    line("            const " + T + " d = 1 + kb_" + P + " * (e + ei);");
                    line("            v = fminf(fmaxf(v - f / d, lo), hi);");
                    line("        }");
                }
                if (adaa)
                {
                    line("        const " + T + " ec = expf(v * " + ivt + ");",
                         "ADAA: the mean of G over [p(n-1), p(n)], closed form");
                    line("        const " + T + " c = ec + 1 / ec;");
                    line("        const " + T + " dp = p - p_" + P + ";");
                    line("        const " + T + " vm = 0.5f * (v + v_" + P + ");");
                    line("        const " + T + " vb = fabsf(dp) > " + literal(kAdaaEpsilon) + " ? vm + ((v - v_" + P +
                         ") * (0.5f * (p + p_" + P + ") - vm) - kc_" + P + " * (c - c_" + P + ")) / dp : vm;");
                }
            }
            const bool input = adaa && usesInput(i);
            if (adaa)
            {
                for (const System::Source& src : s.sources)
                {
                    if (src.kind == System::Source::Kind::State)
                        line("        const " + T + " xa_" + src.name.substr(2) + " = 0.5f * (" + src.name + " + xo_" +
                             src.name.substr(2) + ");");
                }
                if (input)
                    line("        const " + T + " ua = 0.5f * (" + in + " + uo_" + s.sources[0].name + ");");
            }
            // The readouts: a local each, or the source itself when one is +-1 times it.
            std::vector<std::string> readoutText(s.readouts.size());
            for (std::size_t r = 0; r < s.readouts.size(); ++r)
            {
                if (static_cast<int>(r) == s.portCurrent)
                    continue;
                const std::vector<std::pair<Val, std::string>> t = terms(i, s.readouts[r].name, rText);
                if (const std::optional<std::string> direct = inlined(t))
                {
                    readoutText[r] = *direct;
                    continue;
                }
                readoutText[r] = s.readouts[r].name;
                std::string note = s.readouts[r].label;
                if (static_cast<int>(r) == s.output && i + 1 == plan.blocks.size())
                    note += ": .output " + plan.netlist.output->name;
                line("        const " + T + " " + s.readouts[r].name + " = " + linear(t) + ";", note);
            }
            if (adaa)
            {
                for (const System::Source& src : s.sources)
                {
                    if (src.kind == System::Source::Kind::State)
                        line("        xo_" + src.name.substr(2) + " = " + src.name + ";");
                }
            }
            std::size_t state = 0;
            for (const System::Source& src : s.sources)
            {
                if (src.kind != System::Source::Kind::State)
                    continue;
                const auto [readout, sign] = s.capacitor[state++];
                std::string vc = readoutText[static_cast<std::size_t>(readout)];
                bool negative = sign < 0;
                if (!vc.empty() && vc.front() == '-')
                {
                    negative = !negative;
                    vc = vc.substr(1);
                }
                const std::string ident = src.name.substr(2);
                const Branch& br = b.branches[static_cast<std::size_t>(src.branch)];
                std::string note;
                if (vc == "v" || vc == "vb")
                    note = br.part + " across the port: its voltage is v";
                else
                    note = br.part + "'s voltage is " + s.readouts[static_cast<std::size_t>(readout)].label;
                line("        " + src.name + " = " + (negative ? "-" : "") + "g2_" + ident + " * " + vc + " - " +
                         (adaa ? "xa_" + ident : src.name) + ";",
                     note);
            }
            if (b.port)
            {
                const std::string& P = b.port->name;
                line("        v_" + P + " = v;");
                if (adaa)
                {
                    line("        p_" + P + " = p;");
                    line("        c_" + P + " = c;");
                }
                if (input)
                    line("        uo_" + s.sources[0].name + " = " + in + ";");
            }
            line("        return " + readoutText[static_cast<std::size_t>(s.output)] + ";");
            line("    }");
        }

        void Writer::step()
        {
            if (plan.regionFirst < 0)
                return;
            const std::string& in = plan.inputName;
            std::string call = in;
            for (int i = plan.regionFirst; i <= plan.regionLast; ++i)
                call = "b" + std::to_string(i + 1) + "(" + call + ")";
            const std::string region = plan.regionFirst == plan.regionLast
                                           ? "block " + std::to_string(plan.regionFirst + 1)
                                           : std::format("blocks {} to {}", plan.regionFirst + 1, plan.regionLast + 1);
            const std::string rate = plan.config.oversample == 1 ? std::string("the host rate")
                                                                 : std::to_string(plan.config.oversample) + " rate";
            line("    " + type + " step(" + type + " " + in + ") [[clang::nonblocking]]",
                 "one step of the oversampled region (" + region + "), at " + rate);
            line("    {");
            line("        return " + call + ";");
            line("    }");
        }

        void Writer::tick()
        {
            const std::string& in = plan.inputName;
            const std::string T = type;
            const IoLine& input = *plan.netlist.input;
            const IoLine& output = *plan.netlist.output;
            line("    " + T + " tick(" + T + " " + in + ") [[clang::nonblocking]]",
                 std::format("one host sample: .input {} (1.0 = {:g} V) to .output {}", input.name, input.scale,
                             output.name));
            line("    {");
            std::vector<std::string> body;
            std::string current = in;
            int z = 0;
            auto fresh = [&]
            {
                return "z" + std::to_string(z++);
            };
            const int n = static_cast<int>(plan.blocks.size());
            const int preEnd = plan.regionFirst >= 0 ? plan.regionFirst : n;
            for (int i = 0; i < preEnd; ++i)
            {
                const std::string name = fresh();
                body.push_back("const " + T + " " + name + " = b" + std::to_string(i + 1) + "(" + current + ");");
                current = name;
            }
            if (plan.regionFirst >= 0)
            {
                const int stages = stagesOf(plan.config.oversample);
                if (stages == 0)
                {
                    const std::string name = fresh();
                    body.push_back("const " + T + " " + name + " = step(" + current + ");");
                    current = name;
                }
                else
                {
                    std::vector<std::string> values{current};
                    for (int s = 1; s <= stages; ++s)
                    {
                        std::vector<std::string> next;
                        for (std::size_t j = 0; j < values.size(); ++j)
                        {
                            const std::string array =
                                s == 1 ? "os1" : "os" + std::to_string(s) + "_" + std::to_string(j);
                            body.push_back("hb" + std::to_string(s) + "_up(" + values[j] + ", " + array + ");");
                            next.push_back(array + "[0]");
                            next.push_back(array + "[1]");
                        }
                        values = next;
                    }
                    std::vector<std::string> ys;
                    for (std::size_t k = 0; k < values.size(); ++k)
                    {
                        ys.push_back("y" + std::to_string(k));
                        body.push_back("const " + T + " " + ys.back() + " = step(" + values[k] + ");");
                    }
                    for (int s = stages; s >= 1; --s)
                    {
                        std::vector<std::string> next;
                        for (std::size_t j = 0; j + 1 < ys.size(); j += 2)
                        {
                            const std::string name = fresh();
                            body.push_back("const " + T + " " + name + " = hb" + std::to_string(s) + "_down(" + ys[j] +
                                           ", " + ys[j + 1] + ");");
                            next.push_back(name);
                        }
                        ys = next;
                    }
                    current = ys.front();
                }
                for (int i = plan.regionLast + 1; i < n; ++i)
                {
                    const std::string name = fresh();
                    body.push_back("const " + T + " " + name + " = b" + std::to_string(i + 1) + "(" + current + ");");
                    current = name;
                }
            }
            // the last value straight into return
            const std::string last = "const " + T + " " + current + " = ";
            if (!body.empty() && body.back().rfind(last, 0) == 0)
            {
                std::string expr = body.back().substr(last.size());
                expr.pop_back();
                body.back() = "return " + expr + ";";
            }
            else
                body.push_back("return " + current + ";");
            for (const std::string& l : body)
                line("        " + l);
            line("    }");
        }
    } // namespace

    std::string emit(const Plan& plan, const std::vector<BlockCode>& blocks)
    {
        Writer w{plan, blocks, plan.config.channels == 2 ? "float2" : "float", {}};
        w.header();
        w.line("module " + plan.module);
        w.line("{");
        w.fields();
        w.init();
        w.clear();
        w.set();
        for (std::size_t i = 0; i < plan.blocks.size(); ++i)
            w.blockSet(i);
        for (std::size_t i = 0; i < plan.blocks.size(); ++i)
            w.blockStep(i);
        if (plan.regionFirst >= 0)
        {
            for (int s = 1; s <= stagesOf(plan.config.oversample); ++s)
                emitHalfbandMethods(w.out, s, plan.config.halfband, w.type);
        }
        w.step();
        w.tick();
        w.line("};");
        return w.out;
    }
} // namespace fminus::circuit
