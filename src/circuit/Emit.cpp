// src/circuit/Emit.cpp: Val's arithmetic, the Code builder, and the module's text (Emit.h). The emitted shapes are
// mkbench.py's (experiments/), whose costs the report measured: the omega3 guess, Newton steps in a bracket, the ADAA
// tail, halfband methods on prefixed fields, and tick's region samples in field arrays.
#include "circuit/Emit.h"

#include "circuit/Halfbands.h"
#include "circuit/Laws.h"
#include "circuit/Linear.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <format>
#include <functional>
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

    Val Val::var(std::string name, Code* code, int deps)
    {
        Val v;
        v.kind = Kind::Var;
        v.name = std::move(name);
        v.code = code;
        v.deps = deps;
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

        Val temp(Code* code, const std::string& text, std::vector<std::string> uses, bool negative, int deps)
        {
            Val t = code->temp(text, uses, deps);
            t.negated = negative;
            return t;
        }

        int depsOf(const Val& a, const Val& b)
        {
            return a.deps | b.deps;
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
        const int deps = depsOf(a, b);
        if (!x.negative && !y.negative)
            return temp(code, x.text + " + " + y.text, uses, false, deps);
        if (!x.negative && y.negative)
            return temp(code, x.text + " - " + y.text, uses, false, deps);
        if (x.negative && !y.negative)
            return temp(code, y.text + " - " + x.text, uses, false, deps);
        return temp(code, x.text + " + " + y.text, uses, true, deps);
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
            return temp(code, y.text + " * " + x.text, joined(x.uses, y.uses), negative, depsOf(a, b));
        return temp(code, x.text + " * " + y.text, joined(x.uses, y.uses), negative, depsOf(a, b));
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
            return temp(code, literal(numerator, true) + " / " + y.text, y.uses, false, b.deps);
        }
        const Signed x = signedOf(a);
        return temp(code, x.text + " / " + y.text, joined(x.uses, y.uses), x.negative != y.negative, depsOf(a, b));
    }

    Val logOf(const Val& v)
    {
        if (v.kind == Val::Kind::Const && v.c > 0)
            return Val(std::log(v.c));
        if (v.kind != Val::Kind::Var || v.negated)
            throw std::logic_error("Val: the log of a value that is not positive");
        return temp(v.code, "logf(" + v.name + ")", {v.name}, false, v.deps);
    }

    namespace
    {
        // A call of a float builtin on a Val: folded for a constant, else a temporary.
        Val callOf(const char* name, const Val& v, double (*fold)(double))
        {
            if (v.kind != Val::Kind::Var)
                return Val(fold(v.kind == Val::Kind::Zero ? 0.0 : v.c));
            return temp(v.code, std::string(name) + "(" + operandText(v) + ")", {v.name}, false, v.deps);
        }
    } // namespace

    Val expOf(const Val& v)
    {
        return callOf("expf", v, [](double x) { return std::exp(x); });
    }

    Val sqrtOf(const Val& v)
    {
        return callOf("sqrtf", v, [](double x) { return std::sqrt(x); });
    }

    Val cosOf(const Val& v)
    {
        return callOf("cosf", v, [](double x) { return std::cos(x); });
    }

    Val sinOf(const Val& v)
    {
        return callOf("sinf", v, [](double x) { return std::sin(x); });
    }

    Val whereNegative(const Val& c, const Val& a, const Val& b)
    {
        if (c.kind != Val::Kind::Var)
            return (c.kind == Val::Kind::Const && c.c < 0) ? a : b;
        std::vector<std::string> uses{c.name};
        for (const Val* x : {&a, &b})
        {
            if (x->kind == Val::Kind::Var)
                uses.push_back(x->name);
        }
        return temp(c.code, operandText(c) + " < 0 ? " + operandText(a) + " : " + operandText(b), uses, false,
                    c.deps | a.deps | b.deps);
    }

    void annotate(Val& v, const std::string& comment)
    {
        if (v.kind == Val::Kind::Var && v.code != nullptr)
            v.code->comment(v, (v.negated ? "minus " : "") + comment);
    }

    Val bindField(const std::string& field, Val value)
    {
        if (value.kind != Val::Kind::Var || !value.code->fieldsOn_)
            return value;
        Code* code = value.code;
        code->assign(field, value);
        code->fields_.push_back(field);
        return Val::var(field, code, value.deps);
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

    Val Code::temp(const std::string& text, const std::vector<std::string>& uses, int deps)
    {
        // the same expression again (the elimination meets a product twice) is the same temporary
        if (const auto it = seen_.find(text); it != seen_.end())
            return Val::var(it->second, this, depsOf_[it->second]);
        std::string name;
        do
            name = "t" + std::to_string(next_++);
        while (reserved_.count(name));
        seen_.emplace(text, name);
        depsOf_[name] = deps;
        stmts_.push_back({name, true, text, {}, uses, deps});
        return Val::var(name, this, deps);
    }

    void Code::assign(const std::string& field, const Val& v)
    {
        std::vector<std::string> uses;
        if (v.kind == Val::Kind::Var)
            uses.push_back(v.name);
        stmts_.push_back({field, false, operandText(v), {}, uses, v.kind == Val::Kind::Var ? v.deps : 0});
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

    std::vector<std::string> Code::merged(const std::vector<Stmt>& stmts)
    {
        std::map<std::string, int> uses;
        for (const Stmt& s : stmts)
        {
            for (const std::string& u : s.uses)
                ++uses[u];
        }
        std::vector<std::string> out;
        for (std::size_t i = 0; i < stmts.size(); ++i)
        {
            const Stmt& s = stmts[i];
            if (s.lhs.empty())
            {
                out.push_back(withComment(s.rhs, s.note));
                continue;
            }
            if (s.declares && i + 1 < stmts.size())
            {
                const Stmt& next = stmts[i + 1];
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

    std::vector<std::string> Code::lines() const
    {
        return merged(stmts_);
    }

    int Code::operations() const
    {
        return static_cast<int>(std::count_if(stmts_.begin(), stmts_.end(), [](const Stmt& s) { return s.declares; }));
    }

    namespace
    {
        // `text` with each whole name in `renames` replaced.
        std::string renamed(const std::string& text, const std::map<std::string, std::string>& renames)
        {
            std::string out;
            std::size_t i = 0;
            auto word = [](char c)
            {
                return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_';
            };
            while (i < text.size())
            {
                if (!word(text[i]))
                {
                    out += text[i++];
                    continue;
                }
                std::size_t end = i;
                while (end < text.size() && word(text[end]))
                    ++end;
                const std::string name = text.substr(i, end - i);
                const auto it = renames.find(name);
                out += it == renames.end() ? name : it->second;
                i = end;
            }
            return out;
        }
    } // namespace

    Code::Split Code::split(const std::string& prefix) const
    {
        std::vector<Stmt> init;
        std::vector<Stmt> set;
        std::set<std::string> initTemps;
        for (const Stmt& s : stmts_)
        {
            if (s.lhs.empty() || (s.deps & Val::kPots) != 0)
                set.push_back(s);
            else
            {
                init.push_back(s);
                if (s.declares)
                    initTemps.insert(s.lhs);
            }
        }
        // init's temporaries that a pot's statement reads: fields, numbered in init's order
        std::set<std::string> read;
        for (const Stmt& s : set)
        {
            for (const std::string& u : s.uses)
            {
                if (initTemps.count(u))
                    read.insert(u);
            }
        }
        Split out;
        std::map<std::string, std::string> renames;
        for (const Stmt& s : init)
        {
            if (s.declares && read.count(s.lhs))
            {
                renames[s.lhs] = prefix + std::to_string(out.fields.size());
                out.fields.push_back(renames[s.lhs]);
            }
        }
        auto apply = [&](std::vector<Stmt>& list)
        {
            for (Stmt& s : list)
            {
                if (const auto it = renames.find(s.lhs); it != renames.end())
                {
                    s.lhs = it->second;
                    s.declares = false;
                }
                s.rhs = renamed(s.rhs, renames);
                for (std::string& u : s.uses)
                {
                    if (const auto it = renames.find(u); it != renames.end())
                        u = it->second;
                }
            }
        };
        out.initOperations = static_cast<int>(
            std::count_if(init.begin(), init.end(), [](const Stmt& s) { return s.declares; }));
        out.setOperations =
            static_cast<int>(std::count_if(set.begin(), set.end(), [](const Stmt& s) { return s.declares; }));
        apply(init);
        apply(set);
        out.init = merged(init);
        out.set = merged(set);
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
        // ln 81, the audio taper's (81^x - 1) / 80 as exp (the laws' constants are Laws.cpp's).
        constexpr double kLn81 = 4.394449154672439;
    } // namespace

    // ---- a block's coefficients -------------------------------------------------------------------------------------

    std::optional<BlockCode> deriveBlock(const Plan& plan, int blockIndex, Messages& messages, bool woodbury)
    {
        BlockCode bc;
        bc.system = assemble(plan, blockIndex);
        std::optional<Pivots> pivots = choosePivots(bc.system, plan, blockIndex, messages);
        if (!pivots || !checkPort(bc.system, *pivots, plan, blockIndex, messages))
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
            if (br.isSwitch())
            {
                // r = exp(ln roff + s (ln ron - ln roff)): a ramp of sw_ moves r by a constant ratio a step
                const double a = std::log(br.roff);
                const double b = std::log(br.ron) - a;
                const std::string ramp = literal(std::fabs(b)) + " * sw_" + br.ident;
                const std::string arg = a == 0 ? (b < 0 ? "-" : "") + ramp
                                                : literal(a) + (b < 0 ? " - " : " + ") + ramp;
                bc.rLines.push_back(withComment("const float r_" + br.ident + " = expf(" + arg + ");", note));
                continue;
            }
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
            const std::string t = "t_" + br.partIdent;
            if (taperDone.insert(br.element).second)
                bc.rLines.push_back(withComment("const float " + t + " = " + tapered + ";", note));
            if (br.section == 1)
                bc.rLines.push_back("const float r_" + br.ident + " = " + literal(br.value) + " * (1 - " + t + ");");
            else
                bc.rLines.push_back("const float r_" + br.ident + " = " + literal(br.value) + " * " + t + ";");
        }
        // The gains pots move: k_<E>, an expression of the (clamped) pots.
        for (const Controlled& c : block.controlled)
        {
            if (!c.expr)
                continue;
            const Element& el = plan.netlist.elements[static_cast<std::size_t>(c.element)];
            bc.rLines.push_back(withComment("const float k_" + c.ident + " = " + printExpr(*c.expr, potName) + ";",
                                            el.text));
        }
        // The derivation in an order, into a Code: a capacitor's gc and an inductor's rl depend on the rate, a
        // variable resistance and a gain of pots on the pots
        auto derived = [&](const Pivots& order, Code& into)
        {
            std::vector<Val> symbols;
            for (const System::Symbol& s : bc.system.symbols)
            {
                const bool rate = s.kind == System::Symbol::Kind::Gc || s.kind == System::Symbol::Kind::Rl;
                symbols.push_back(Val::var(s.name, &into, rate ? Val::kRate : Val::kPots));
            }
            // a matched block's trapezoidal coefficients are its design's inputs, not fields
            const bool matched = block.design == Block::Design::Matched;
            into.bindFields(!matched);
            const Linear<Val> linear = solve(bc.system, order, symbols);
            Coefficients<Val> out = matched ? matchedCoefficients(plan, blockIndex, bc.system, linear, into)
                                            : derive(bc.system, linear, block,
                                                     plan.config.adaa && block.port.has_value());
            for (auto& [name, v] : out)
                v.code = nullptr;
            return out;
        };
        bc.coefficients = derived(bc.pivots, code);
        bc.lines = code.lines();
        if (!block.hasControls() || !woodbury)
            return bc;
        // The Woodbury form: the variable branches' currents pivoted last, so that everything before them depends on
        // the rate alone (init's) and b<N>_set is left with the k x k (R + Q) elimination and what it reaches; kept
        // when its b<N>_set does fewer operations than the all-in-set form's
        const std::string setName = "b" + std::to_string(block.index) + "_set";
        const int all = code.operations();
        Messages scratch;
        const std::optional<Pivots> last = choosePivots(bc.system, plan, blockIndex, scratch, true);
        if (!last)
        {
            bc.form = std::format("{}: all in set, {} operations (no order pivots its variable branches last)",
                                  setName, all);
            return bc;
        }
        Code wood(reserved);
        Coefficients<Val> coefficients = derived(*last, wood);
        Code::Split split = wood.split("w" + std::to_string(block.index) + "_");
        if (split.setOperations >= all)
        {
            bc.form = std::format("{}: all in set, {} operations (the Woodbury form's would be {}, and {} in init)",
                                  setName, all, split.setOperations, split.initOperations);
            return bc;
        }
        bc.form = std::format("{}: the Woodbury form (its variable branches pivoted last), {} operations, and {} in "
                              "init; all in set, {}",
                              setName, split.setOperations, split.initOperations, all);
        bc.pivots = *last;
        bc.coefficients = std::move(coefficients);
        bc.lines = std::move(split.set);
        bc.initLines = std::move(split.init);
        bc.wFields = std::move(split.fields);
        bc.woodbury = true;
        return bc;
    }

    // ---- the module -------------------------------------------------------------------------------------------------

    double adaaLatency(const Plan& plan)
    {
        if (!plan.config.adaa || plan.regionFirst < 0)
            return 0;
        int ports = 0;
        for (int i = plan.regionFirst; i <= plan.regionLast; ++i)
            ports += plan.blocks[static_cast<std::size_t>(i)].port ? 1 : 0;
        return ports * 0.5 / plan.config.oversample;
    }

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

    } // namespace

    std::string configText(const Config& c)
    {
        const std::string region = c.region == Region::Auto        ? "auto"
                                   : c.region == Region::Nonlinear ? "nonlinear"
                                                                   : "all";
        struct Item
        {
            std::string text;
            std::string from;
            bool joins = false;   // follows .oversample with a space (its halfbands and blocks=)
        };
        std::vector<Item> items{
            {".oversample " + std::to_string(c.oversample), c.oversampleFrom},
            {c.halfband == Halfband::Iir ? "iir" : "fir", c.halfbandFrom, true},
            {"blocks=" + region, c.regionFrom, true},
            {std::string(".adaa ") + (c.adaa ? "on" : "off"), c.adaaFrom},
            {std::string(".solver ") + (c.solver.guess == Solver::Guess::Omega ? "omega(" : "newton(") +
                 std::to_string(c.solver.steps) + ")",
             c.solverFrom},
            {std::string(".method ") + (c.method == Method::Tr ? "tr" : "bdf2"), c.methodFrom},
        };
        if (c.otas || c.prewarpFrom != "default")
            items.push_back({std::string(".prewarp ") + (c.prewarp ? "on" : "off"), c.prewarpFrom});
        if (c.channels != 1 || c.channelsFrom != "default")
            items.push_back({".channels " + std::to_string(c.channels), c.channelsFrom});
        // grouped by where they came from, in the order of precedence
        std::vector<std::string> sources;
        for (const Item& i : items)
        {
            if (std::find(sources.begin(), sources.end(), i.from) == sources.end())
                sources.push_back(i.from);
        }
        auto rank = [](const std::string& from)
        {
            return from == "Options" ? 0 : from == "the netlist" ? 1 : from.rfind("the self-test", 0) == 0 ? 2
                                       : from == "provisional"   ? 3
                                                                 : 4;
        };
        std::stable_sort(sources.begin(), sources.end(),
                         [&](const std::string& a, const std::string& b) { return rank(a) < rank(b); });
        std::string text = "configuration: ";
        for (std::size_t g = 0; g < sources.size(); ++g)
        {
            std::string group;
            bool afterOversample = false;
            for (const Item& i : items)
            {
                if (i.from != sources[g])
                {
                    afterOversample = false;
                    continue;
                }
                group += group.empty() ? "" : (i.joins && afterOversample ? " " : ", ");
                group += i.text;
                afterOversample = i.joins || i.text.rfind(".oversample", 0) == 0;
            }
            text += (g == 0 ? "" : "; ") + group + " (" + sources[g] + ")";
        }
        return text;
    }

    namespace
    {
        struct Writer
        {
            const Plan& plan;
            const std::vector<BlockCode>& blocks;
            std::string type;   // float or float2: the signals and states
            std::string out;
            std::vector<int> lines;   // each line of out: the netlist's line it comes from (Result::lines)
            int at = 0;               // the netlist's line the lines written now come from

            void line(const std::string& text, const std::string& note = {}, std::size_t column = 60)
            {
                out += withComment(text, note, column) + "\n";
                lines.push_back(at);
            }

            // Text of whole lines (the halfbands' emitters write it), each from `at`.
            void raw(const std::string& text)
            {
                out += text;
                lines.insert(lines.end(), static_cast<std::size_t>(std::count(text.begin(), text.end(), '\n')), at);
            }

            // The netlist's lines: .title's (the module's frame), the first .pot's, .input's, an element's.
            int titleLine() const
            {
                return plan.netlist.titleLoc.line;
            }

            int potLine() const
            {
                return !plan.netlist.pots.empty()       ? plan.netlist.pots.front().loc.line
                       : !plan.netlist.switches.empty() ? plan.netlist.switches.front().loc.line
                                                        : titleLine();
            }

            // The S elements, in netlist order: each one's ramp sw_<S>.
            std::vector<const Branch*> switchBranches() const
            {
                std::vector<const Branch*> list;
                for (const Block& b : plan.blocks)
                {
                    for (const Branch& br : b.branches)
                    {
                        if (br.isSwitch())
                            list.push_back(&br);
                    }
                }
                std::stable_sort(list.begin(), list.end(),
                                 [](const Branch* x, const Branch* y) { return x->element < y->element; });
                return list;
            }

            // A switch section's ramp at its .switch's default position: 0 or 1.
            double defaultRamp(const Branch& br) const
            {
                for (const SwitchInfo& w : plan.switches)
                {
                    if (w.key == br.switchKey)
                        return switchRamp(br, w.defaultPosition);
                }
                return 0;
            }

            int inputLine() const
            {
                return plan.netlist.input ? plan.netlist.input->loc.line : titleLine();
            }

            int elementLine(int element) const
            {
                return plan.netlist.elements[static_cast<std::size_t>(element)].loc.line;
            }

            int blockLine(const Block& b) const
            {
                return b.elements.empty() ? titleLine() : elementLine(b.elements.front());
            }

            int portLine(const Block& b) const
            {
                return elementLine(b.port->elements.front());
            }

            // The fields the guard sums: every float that carries from one sample to the next (the parts' states,
            // the ports' last voltages, ADAA's last points, the halfbands' delay lines), each with its length (0: not
            // an array).
            std::vector<std::pair<std::string, int>> carried() const
            {
                std::vector<std::pair<std::string, int>> fields;
                for (const Block& b : plan.blocks)
                {
                    if (matchedBlock(b))
                    {
                        fields.emplace_back("s1_b" + std::to_string(b.index), 0);
                        if (orderOf(b) == 2)
                            fields.emplace_back("s2_b" + std::to_string(b.index), 0);
                        continue;
                    }
                    for (const Branch& br : b.branches)
                    {
                        if (br.state())
                            fields.emplace_back("x_" + br.ident, 0);
                    }
                }
                for (std::size_t i = 0; i < plan.blocks.size(); ++i)
                {
                    const Block& b = plan.blocks[i];
                    if (secondHistory(b))
                    {
                        for (const Branch& br : b.branches)
                        {
                            if (br.state())
                                fields.emplace_back("xo_" + br.ident, 0);
                        }
                    }
                    if (!b.port)
                        continue;
                    fields.emplace_back("v_" + b.port->name, 0);
                    if (!adaaBlock(b))
                        continue;
                    fields.emplace_back("p_" + b.port->name, 0);
                    for (const LawField& f : adaaFields(*b.port))
                        fields.emplace_back(f.name, 0);
                    if (usesInput(i))
                        fields.emplace_back("uo_" + blocks[i].system.sources[0].name, 0);
                }
                if (plan.regionFirst >= 0)
                {
                    for (int s = 1; s <= stagesOf(plan.config.oversample); ++s)
                    {
                        for (const auto& state : halfbandStates(s, plan.config.halfband))
                            fields.push_back(state);
                    }
                }
                return fields;
            }

            bool adaaBlock(const Block& b) const
            {
                return b.port.has_value() && plan.config.adaa;
            }

            // Whether a block keeps xo_<C> for its states: ADAA's last states, or BDF2's second history.
            bool secondHistory(const Block& b) const
            {
                return adaaBlock(b) || b.method == Method::Bdf2;
            }

            // Whether a block runs a matched design (its states s1_b<N> and s2_b<N>, not its parts' x_<C>).
            static bool matchedBlock(const Block& b)
            {
                return b.design == Block::Design::Matched;
            }

            // A matched block's order: 1 or 2.
            static int orderOf(const Block& b)
            {
                return static_cast<int>(
                    std::count_if(b.branches.begin(), b.branches.end(), [](const Branch& br) { return br.state(); }));
            }

            // Whether block i's set() moves its ADAA port's curve: ka (an asymmetric pair's kr too) is a field its
            // b<N>_set computes.
            bool movesPortCurve(std::size_t i) const
            {
                const Block& b = plan.blocks[i];
                if (!adaaBlock(b) || b.port->smooth() || !b.hasControls())
                    return false;
                const Val ka = coefficient(i, "ka_" + b.port->name);
                return ka.kind == Val::Kind::Var && (ka.deps & Val::kPots) != 0;
            }

            // Whether its other diode's kr moves too (an asymmetric pair).
            bool movesReverse(std::size_t i) const
            {
                const Block& b = plan.blocks[i];
                if (!movesPortCurve(i) || b.port->law != PortDef::Law::Asym)
                    return false;
                const Val kr = coefficient(i, "kr_" + b.port->name);
                return kr.kind == Val::Kind::Var && (kr.deps & Val::kPots) != 0;
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

            // A pot's name, by its folded key.
            std::string potName(const std::string& key) const
            {
                for (const PotInfo& p : plan.pots)
                {
                    if (p.key == key)
                        return p.name;
                }
                return key;
            }

            // An OTA's gain as FMinus text: its pots spelled by `pot`, its law inputs as l_<name> (tick's and step's
            // clamped values); prewarped through pw<N>_ when .prewarp applies to it.
            std::string gainText(const Block& b, const std::function<std::string(const std::string&)>& pot) const
            {
                const PortDef& port = *b.port;
                std::string text = port.gain ? printExpr(*port.gain,
                                                         [&](const std::string& key)
                                                         {
                                                             for (const LawInfo& l : plan.laws)
                                                             {
                                                                 if (l.key == key)
                                                                     return "l_" + l.name;
                                                             }
                                                             return pot(key);
                                                         })
                                             : literal(port.gainValue);
                if (port.prewarp)
                    text = "pw" + std::to_string(b.index) + "_(" + text + ")";
                return text;
            }

            // Block i's law's operands: its constants as fields or literals (a pair's when K is a constant), its gain.
            LawOperands operands(std::size_t i) const
            {
                LawOperands o;
                const Block& b = plan.blocks[i];
                o.type = type;
                o.solver = plan.config.solver;
                o.adaa = adaaBlock(b);
                if (!b.port)
                    return o;
                const std::string& P = b.port->name;
                o.port = P;
                o.ka = operandText(coefficient(i, "ka_" + P));
                o.kb = operandText(coefficient(i, "kb_" + P));
                o.kc = operandText(coefficient(i, "kc_" + P));
                const Val lna = coefficient(i, "lna_" + P);
                o.plusLna =
                    lna.kind == Val::Kind::Const && lna.c < 0 ? " - " + literal(-lna.c) : " + " + operandText(lna);
                o.lna = operandText(lna);
                o.kr = operandText(coefficient(i, "kr_" + P));
                o.ks = operandText(coefficient(i, "ks_" + P));
                o.kcr = operandText(coefficient(i, "kcr_" + P));
                o.lnr = operandText(coefficient(i, "lnr_" + P));
                o.kd = operandText(coefficient(i, "kd_" + P));
                o.g = gainField(b) ? "g_" + P : literal(b.port->gainValue);
                return o;
            }

            // Whether an OTA's gain is a field: it moves (with pots, set's; with law inputs, each sample's), or is
            // prewarped (init's, at the rate).
            static bool gainField(const Block& b)
            {
                return b.port && b.port->smooth() && (b.port->gain.has_value() || b.port->prewarp);
            }

            // Whether an OTA's gain reads law inputs (tick and step compute it, every host sample).
            static bool lawGain(const Block& b)
            {
                return b.port && b.port->smooth() && !b.port->gainLaws.empty();
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
            void guard();
            std::string regionCall(const std::string& x) const;
            std::string lawParameters() const;
            void lawLines();
            bool hasLawGains() const;
            void prewarpMethods();
        };

        void Writer::header()
        {
            const Config& c = plan.config;
            at = titleLine();
            line("// " + plan.module + ": generated from " + plan.baseName +
                 " by fminus::circuit. Do not edit: edit " + plan.baseName + ".");
            std::string states;
            int stateCount = 0;
            int portCount = 0;
            std::string ports;
            for (const Block& b : plan.blocks)
            {
                for (const Branch& br : b.branches)
                {
                    if (br.state())
                    {
                        states += (states.empty() ? "" : ", ") + br.part;
                        ++stateCount;
                    }
                }
                if (b.port && b.port->smooth())
                {
                    ++portCount;
                    const std::string gain =
                        b.port->gain ? printExpr(*b.port->gain,
                                                 [&](const std::string& key)
                                                 {
                                                     for (const LawInfo& l : plan.laws)
                                                     {
                                                         if (l.key == key)
                                                             return l.name;
                                                     }
                                                     return potName(key);
                                                 })
                                     : engineering(b.port->gainValue, "A");
                    ports += (ports.empty() ? "" : "; ") + b.port->parts + ": an OTA, g tanh(v / 2 Vt), 2 Vt " +
                             engineering(b.port->nvt, "V") + " at 27 C, g = " + gain;
                }
                else if (b.port && b.port->law == PortDef::Law::Pair)
                {
                    ++portCount;
                    ports += (ports.empty() ? "" : "; ") + b.port->parts + ": 2 Is sinh(v / N Vt), Is " +
                             engineering(b.port->is, "A") + ", N Vt " + engineering(b.port->nvt, "V") + " at 27 C";
                }
                else if (b.port && b.port->law == PortDef::Law::Single)
                {
                    ++portCount;
                    ports += (ports.empty() ? "" : "; ") + b.port->parts + ": Is (e^(v / N Vt) - 1), Is " +
                             engineering(b.port->is, "A") + ", N Vt " + engineering(b.port->nvt, "V") + " at 27 C";
                }
                else if (b.port)
                {
                    ++portCount;
                    ports += (ports.empty() ? "" : "; ") + b.port->parts +
                             ": Is (e^(v / N Vt) - 1) - Is2 (e^(-v / N2 Vt) - 1), Is " + engineering(b.port->is, "A") +
                             ", N Vt " + engineering(b.port->nvt, "V") + ", Is2 " + engineering(b.port->is2, "A") +
                             ", N2 Vt " + engineering(b.port->nvt2, "V") + " at 27 C";
                }
            }
            std::string pots;
            for (const PotInfo& p : plan.pots)
                pots += (pots.empty() ? "" : ", ") + p.name;
            std::string switches;
            for (const SwitchInfo& w : plan.switches)
                switches += (switches.empty() ? "" : ", ") + w.name + std::format(": {} positions", w.positions);
            line("//   " + std::to_string(plan.blocks.size()) + (plan.blocks.size() == 1 ? " block, " : " blocks, ") +
                 std::to_string(stateCount) + (stateCount == 1 ? " state" : " states") +
                 (states.empty() ? "" : " (" + states + ")") + ", " + std::to_string(portCount) +
                 (portCount == 1 ? " nonlinear port" : " nonlinear ports") + (ports.empty() ? "" : " (" + ports + ")") +
                 ", " + std::to_string(plan.pots.size()) + (plan.pots.size() == 1 ? " pot" : " pots") +
                 (pots.empty() ? "" : " (" + pots + ")") +
                 (plan.switches.empty() ? std::string()
                                        : ", " + std::to_string(plan.switches.size()) +
                                              (plan.switches.size() == 1 ? " switch (" : " switches (") + switches +
                                              ")"));
            for (std::size_t i = 0; i < plan.blocks.size(); ++i)
            {
                const Block& b = plan.blocks[i];
                std::string text = "//   block " + std::to_string(b.index) + " (" + partsText(plan, b) +
                                   "): " + plan.nodes[static_cast<std::size_t>(b.input)].name + " -> " +
                                   plan.nodes[static_cast<std::size_t>(b.output)].name + "; ";
                if (i > 0)
                {
                    const Block& before = plan.blocks[i - 1];
                    const bool driven =
                        std::find(before.driven.begin(), before.driven.end(), b.input) != before.driven.end();
                    const Node& in = plan.nodes[static_cast<std::size_t>(b.input)];
                    std::string driver = "an op amp's";
                    if (in.source == Node::Source::Vcvs)
                        driver = plan.netlist.elements[static_cast<std::size_t>(in.element)].name + "'s";
                    text += driven ? "cut at " + driver + " output (exact); " : "cut at an op amp's + input (exact); ";
                }
                if (b.port)
                {
                    const std::string an = plan.nodes[static_cast<std::size_t>(b.port->anode)].name;
                    const std::string ca = plan.nodes[static_cast<std::size_t>(b.port->cathode)].name;
                    const std::string across = b.port->cathode == 0 ? "V(" + an + ")"
                                               : b.port->anode == 0 ? "-V(" + ca + ")"
                                                                    : "V(" + an + ") - V(" + ca + ")";
                    text += "port " + b.port->parts + " at " + across;
                    if (b.port->form == PortDef::Form::I)
                        text += ", its current from " + plan.nodes[static_cast<std::size_t>(b.port->from)].name +
                                " to " + plan.nodes[static_cast<std::size_t>(b.port->to)].name;
                    text += "; at " + rateText(b);
                    if (adaaBlock(b))
                        text += ", designed at " + rateText(b) + " 2/3 (ADAA)";
                    if (b.method == Method::Bdf2)
                        text += ", BDF2";
                }
                else if (matchedBlock(b))
                    text += std::format("linear; matched at 1x ({:.2f} dB off at 44.1 and 48 kHz; the trapezoidal "
                                        "rule's {:.2f} at 44.1)",
                                        b.matchedError, b.oneX);
                else
                {
                    text += "linear; at " + rateText(b);
                    if (b.method == Method::Bdf2)
                        text += ", BDF2";
                    if (b.inRegion)
                        text += std::format(" (1x check: {:.2f} dB off at 44.1 kHz, {:.2f} dB at {}x)", b.oneX,
                                            b.oneXRegion, b.rateFactor);
                    else
                        text += std::format(" (1x check: {:.2f} dB off at 44.1 kHz{})", b.oneX,
                                            b.oneX > 0.5 ? ", more than 0.5" : "");
                }
                line(text);
            }
            const bool regionPorts = plan.regionFirst >= 0 &&
                                     std::any_of(plan.blocks.begin() + plan.regionFirst,
                                                 plan.blocks.begin() + plan.regionLast + 1,
                                                 [](const Block& b) { return b.port.has_value(); });
            if (regionPorts)
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
                if (const double adaa = adaaLatency(plan); adaa > 0)
                    text += std::format(", plus {:g} (ADAA)", adaa);
                line(text);
            }
            else
                line("//   latency: none (1x)");
        }

        void Writer::fields()
        {
            // each note on the first field of its kind and integrator: (inductor, BDF2)
            std::set<std::pair<bool, bool>> noted;
            auto firstOf = [&](bool inductor, const Block& b)
            {
                return noted.insert({inductor, b.method == Method::Bdf2}).second;
            };
            for (const Block& b : plan.blocks)
            {
                if (matchedBlock(b))
                {
                    at = blockLine(b);
                    const std::string n = std::to_string(b.index);
                    line("    " + type + " s1_b" + n + ";",
                         "block " + n + "'s matched design: its transposed direct form II's states", 31);
                    if (orderOf(b) == 2)
                        line("    " + type + " s2_b" + n + ";", "", 31);
                    continue;
                }
                for (const Branch& br : b.branches)
                {
                    if (!br.state())
                        continue;
                    at = elementLine(br.element);
                    const bool inductor = br.kind == Branch::Kind::Inductor;
                    const bool bdf2 = b.method == Method::Bdf2;
                    std::string note;
                    if (firstOf(inductor, b))
                        note = br.part + "'s companion state (" + (bdf2 ? "BDF2" : "trapezoidal") +
                               (bdf2 ? std::string("): x <- h - xo, h = 4/3 ") + (inductor ? "rl i" : "gc v")
                                     : std::string("): x <- 2 ") + (inductor ? "rl i - x" : "gc v - x"));
                    line("    " + type + " x_" + br.ident + ";", note, 31);
                }
            }
            for (std::size_t i = 0; i < plan.blocks.size(); ++i)
            {
                const Block& b = plan.blocks[i];
                if (!secondHistory(b))
                    continue;
                bool firstX = true;
                for (const Branch& br : b.branches)
                {
                    if (!br.state())
                        continue;
                    at = elementLine(br.element);
                    std::string note;
                    if (firstX)
                        note = adaaBlock(b) ? "ADAA: x_" + br.ident + " a step earlier"
                                            : "BDF2: x_" + br.ident + "'s second part, h / 4 a step earlier";
                    line("    " + type + " xo_" + br.ident + ";", note, 31);
                    firstX = false;
                }
            }
            for (std::size_t i = 0; i < plan.blocks.size(); ++i)
            {
                const Block& b = plan.blocks[i];
                if (!b.port)
                    continue;
                const std::string& p = b.port->name;
                at = portLine(b);
                line("    " + type + " v_" + p + ";", b.port->parts + ": the port's voltage at the last step", 31);
                if (!adaaBlock(b))
                    continue;
                line("    " + type + " p_" + p + ";", "ADAA: the port's open-circuit voltage at the last step", 31);
                for (const LawField& f : adaaFields(*b.port))
                    line("    " + (f.vector ? type : std::string("float")) + " " + f.name + ";", f.note, 31);
                if (usesInput(i))
                    line("    " + type + " uo_" + blocks[i].system.sources[0].name + ";",
                         "ADAA: block " + std::to_string(b.index) + "'s input a step earlier", 31);
            }
            std::set<std::string> lawPots;   // pv_<pot>: one field for every OTA whose law-input gain reads the pot
            for (const Block& b : plan.blocks)
            {
                if (!gainField(b))
                    continue;
                const std::string& p = b.port->name;
                at = portLine(b);
                line("    float g_" + p + ";",
                     b.port->parts + "'s gain, amps: " +
                         (lawGain(b) ? std::string("each host sample's") : b.port->gain ? "set's" : "init's") +
                         (b.port->prewarp ? ", prewarped" : ""),
                     31);
                if (b.port->prewarp)
                {
                    line("    float pws_" + p + ";", "prewarp: 2 fs', the rate its block is designed at, twice", 31);
                    line("    float pwk_" + p + ";", "1 / (2 fs')", 31);
                }
                if (lawGain(b))
                {
                    for (const std::string& key : b.port->gainPots)
                    {
                        if (lawPots.insert(key).second)
                            line("    float pv_" + potName(key) + ";", "pot " + potName(key) + ", clamped, for tick",
                                 31);
                    }
                }
            }
            {
                bool firstS = true;
                for (const Branch* br : switchBranches())
                {
                    at = elementLine(br->element);
                    line("    float sw_" + br->ident + ";",
                         firstS ? br->part + "'s ramp: 0 open (roff) to 1 closed (ron), set's, 20 ms end to end" : "",
                         31);
                    firstS = false;
                }
                if (!firstS)
                {
                    at = potLine();
                    line("    float swstep_;", "a ramp's most a set() call: 16 / (0.02 rate)", 31);
                }
            }
            noted.clear();
            for (const Block& b : plan.blocks)
            {
                for (const Branch& br : b.branches)
                {
                    if (br.kind != Branch::Kind::Capacitor)
                        continue;
                    at = elementLine(br.element);
                    line("    float gc_" + br.ident + ";",
                         firstOf(false, b) ? (b.method == Method::Bdf2 ? "3 C fs' / 2 (BDF2)" : "2 C fs'") +
                                                 std::string(", the companion's conductance")
                                           : "",
                         31);
                }
            }
            noted.clear();
            for (const Block& b : plan.blocks)
            {
                for (const Branch& br : b.branches)
                {
                    if (br.kind != Branch::Kind::Capacitor)
                        continue;
                    at = elementLine(br.element);
                    line("    float g2_" + br.ident + ";",
                         firstOf(false, b) ? (b.method == Method::Bdf2 ? "4/3 gc" : "2 gc") : "", 31);
                }
            }
            noted.clear();
            for (const Block& b : plan.blocks)
            {
                for (const Branch& br : b.branches)
                {
                    if (br.kind != Branch::Kind::Inductor)
                        continue;
                    at = elementLine(br.element);
                    const bool note = firstOf(true, b);
                    const bool bdf2 = b.method == Method::Bdf2;
                    line("    float rl_" + br.ident + ";",
                         note ? (bdf2 ? "3 L fs' / 2 (BDF2)" : "2 L fs'") + std::string(", the companion's resistance")
                              : "",
                         31);
                    line("    float r2_" + br.ident + ";", note ? (bdf2 ? "4/3 rl" : "2 rl") : "", 31);
                }
            }
            for (std::size_t i = 0; i < plan.blocks.size(); ++i)
            {
                bool firstField = true;
                at = blockLine(plan.blocks[i]);
                for (const auto& [name, v] : blocks[i].coefficients)
                {
                    if (v.kind != Val::Kind::Var)
                        continue;
                    line("    float " + name + ";",
                         firstField ? "block " + std::to_string(i + 1) + "'s coefficients" : "", 31);
                    firstField = false;
                }
                for (std::size_t k = 0; k < blocks[i].wFields.size(); ++k)
                    line("    float " + blocks[i].wFields[k] + ";",
                         k == 0 ? "block " + std::to_string(i + 1) + "'s rate-only terms (init's; b" +
                                      std::to_string(i + 1) + "_set reads them: Woodbury)"
                                : "",
                         31);
            }
            at = titleLine();
            line("    int ck_;", "the guard: ticks since init or clear, mod 16 (it checks the states at 0)", 31);
            line("    float fd_;", "the guard's fade-in after a reset, 0 to 1 over 20 ms", 31);
            line("    float fdstep_;", "1 / (0.02 rate)", 31);
            if (plan.regionFirst < 0)
                return;
            const int stages = stagesOf(plan.config.oversample);
            for (int s = 1; s <= stages; ++s)
            {
                std::string text;
                emitHalfbandFields(text, s, plan.config.halfband, type);
                raw(text);
            }
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
            at = titleLine();
            line("    void init(float rate) [[clang::nonblocking]]", "the host's rate");
            line("    {");
            line("        fdstep_ = 50 / rate;", "the guard's fade: 1 / (0.02 rate), 20 ms");
            if (!plan.switches.empty())
                line("        swstep_ = 800 / rate;", "a switch's ramp a set() call: 16 / (0.02 rate), 20 ms");
            for (const Block& b : plan.blocks)
            {
                for (const Branch& br : b.branches)
                {
                    if (br.kind != Branch::Kind::Capacitor)
                        continue;
                    const bool bdf2 = b.method == Method::Bdf2;
                    const double factor = (bdf2 ? 1.5 : 2) * br.c * b.rateFactor * b.designFactor;
                    const std::string fs =
                        (b.rateFactor == 1 ? std::string("rate") : std::to_string(b.rateFactor) + " rate") +
                        (b.designFactor != 1 ? " 2/3" : "");
                    line("        gc_" + br.ident + " = " + literal(factor) + " * rate;",
                         (bdf2 ? "3 " + br.part + " fs' / 2 (BDF2)" : "2 " + br.part + " fs'") + ", fs' = " + fs);
                }
            }
            for (const Block& b : plan.blocks)
            {
                for (const Branch& br : b.branches)
                {
                    if (br.kind != Branch::Kind::Capacitor)
                        continue;
                    if (b.method == Method::Bdf2)
                        line("        g2_" + br.ident + " = " + literal(2 * br.c * b.rateFactor * b.designFactor) +
                                 " * rate;",
                             "4/3 gc: BDF2's history, 2 " + br.part + " fs'");
                    else
                        line("        g2_" + br.ident + " = 2 * gc_" + br.ident + ";");
                }
            }
            for (const Block& b : plan.blocks)
            {
                for (const Branch& br : b.branches)
                {
                    if (br.kind != Branch::Kind::Inductor)
                        continue;
                    const bool bdf2 = b.method == Method::Bdf2;
                    const double factor = (bdf2 ? 1.5 : 2) * br.l * b.rateFactor * b.designFactor;
                    const std::string fs =
                        (b.rateFactor == 1 ? std::string("rate") : std::to_string(b.rateFactor) + " rate") +
                        (b.designFactor != 1 ? " 2/3" : "");
                    line("        rl_" + br.ident + " = " + literal(factor) + " * rate;",
                         (bdf2 ? "3 " + br.part + " fs' / 2 (BDF2)" : "2 " + br.part + " fs'") + ", fs' = " + fs);
                    if (bdf2)
                        line("        r2_" + br.ident + " = " + literal(2 * br.l * b.rateFactor * b.designFactor) +
                                 " * rate;",
                             "4/3 rl: BDF2's history, 2 " + br.part + " fs'");
                    else
                        line("        r2_" + br.ident + " = 2 * rl_" + br.ident + ";");
                }
            }
            for (const Block& b : plan.blocks)
            {
                if (!b.port || !b.port->prewarp)
                    continue;
                const std::string& P = b.port->name;
                const std::string fs =
                    (b.rateFactor == 1 ? std::string("rate") : std::to_string(b.rateFactor) + " rate") +
                    (b.designFactor != 1 ? " 2/3" : "");
                line("        pws_" + P + " = " + literal(2 * b.rateFactor * b.designFactor) + " * rate;",
                     "prewarp: 2 fs', fs' = " + fs);
                line("        pwk_" + P + " = 1 / pws_" + P + ";");
                if (!b.port->gain)
                    line("        g_" + P + " = " + gainText(b, [](const std::string& k) { return k; }) + ";",
                         b.port->parts + "'s gain, prewarped at this rate");
            }
            for (std::size_t i = 0; i < plan.blocks.size(); ++i)
            {
                at = blockLine(plan.blocks[i]);
                if (blocks[i].woodbury && !blocks[i].initLines.empty())
                {
                    line("        {", "block " + std::to_string(i + 1) + "'s rate-only part (the Woodbury form): b" +
                                          std::to_string(i + 1) + "_set does the rest");
                    for (const std::string& l : blocks[i].initLines)
                        line("            " + l);
                    line("        }");
                    continue;
                }
                if (plan.blocks[i].hasControls() || blocks[i].lines.empty())
                    continue;
                line("        {", "block " + std::to_string(i + 1) + "'s coefficients (it has no pots)");
                for (const std::string& l : blocks[i].lines)
                    line("            " + l);
                line("        }");
            }
            at = titleLine();
            for (std::size_t i = 0; i < plan.blocks.size(); ++i)
            {
                if (movesPortCurve(i))
                    line("        ka_" + plan.blocks[i].port->name + " = 0;",
                         "a number for b" + std::to_string(plan.blocks[i].index) + "_set's first move (v is 0 then)");
                if (movesReverse(i))
                    line("        kr_" + plan.blocks[i].port->name + " = 0;");
            }
            // the switches' ramps start at their defaults, which set(defaults) below then holds; clear() leaves them
            // where set() put them, as the coefficients are (a reset keeps the host's position, and nothing steps)
            for (const Branch* br : switchBranches())
                line("        sw_" + br->ident + " = " + (defaultRamp(*br) == 1 ? "1" : "0") + ";",
                     br->part + " at its switch's default position");
            line("        clear();");
            if (!plan.pots.empty() || !plan.switches.empty())
            {
                std::string args;
                std::string note;
                for (const PotInfo& p : plan.pots)
                {
                    args += (args.empty() ? "" : ", ") + literal(p.defaultValue);
                    note += (note.empty() ? ".pot " : ", ") + p.name + std::format(" {:g}", p.defaultValue);
                }
                bool firstSwitch = true;
                for (const SwitchInfo& w : plan.switches)
                {
                    args += (args.empty() ? "" : ", ") + std::to_string(w.defaultPosition);
                    note += (note.empty() ? "" : "; ") + std::string(firstSwitch ? ".switch " : ", ") + w.name +
                            std::format(" {}", w.defaultPosition);
                    firstSwitch = false;
                }
                line("        set(" + args + ");", note);
            }
            line("    }");
        }

        void Writer::clear()
        {
            at = titleLine();
            line("    void clear() [[clang::nonblocking]]",
                 "the DC operating point: 0 in the offsets the module runs in");
            line("    {");
            line("        ck_ = 0;");
            line("        fd_ = 1;");
            for (const Block& b : plan.blocks)
            {
                if (matchedBlock(b))
                {
                    line("        s1_b" + std::to_string(b.index) + " = 0;");
                    if (orderOf(b) == 2)
                        line("        s2_b" + std::to_string(b.index) + " = 0;");
                    continue;
                }
                for (const Branch& br : b.branches)
                {
                    if (br.state())
                        line("        x_" + br.ident + " = 0;");
                }
            }
            for (std::size_t i = 0; i < plan.blocks.size(); ++i)
            {
                const Block& b = plan.blocks[i];
                if (secondHistory(b))
                {
                    for (const Branch& br : b.branches)
                    {
                        if (br.state())
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
                for (const LawField& f : adaaFields(*b.port))
                    line("        " + f.name + " = " + f.clear + ";");
                if (usesInput(i))
                    line("        uo_" + blocks[i].system.sources[0].name + " = 0;");
            }
            if (plan.regionFirst >= 0)
            {
                for (int s = 1; s <= stagesOf(plan.config.oversample); ++s)
                {
                    std::string text;
                    emitHalfbandClear(text, s, plan.config.halfband);
                    raw(text);
                }
            }
            line("    }");
        }

        void Writer::set()
        {
            if (plan.pots.empty() && plan.switches.empty())
                return;
            std::string params;
            std::string names;
            for (const PotInfo& p : plan.pots)
            {
                params += (params.empty() ? "" : ", ") + std::string("float ") + p.name;
                names += (names.empty() ? ".pot " : ", ") + p.name;
            }
            if (!names.empty())
                names += ", 0 to 1";
            std::string switchNames;
            for (const SwitchInfo& w : plan.switches)
            {
                params += (params.empty() ? "" : ", ") + std::string("int ") + w.name;
                switchNames += (switchNames.empty() ? ".switch " : ", ") + w.name;
            }
            if (!switchNames.empty())
                names += (names.empty() ? "" : "; ") + switchNames;
            at = potLine();
            line("    void set(" + params + ") [[clang::nonblocking]]",
                 names + ": call it on the absolute 16-sample grid" +
                     (plan.switches.empty() ? "" : ", every grid tick while a switch ramps"));
            line("    {");
            // each switch section's ramp toward its target, swstep_ a call, landing on it exactly
            for (const Branch* br : switchBranches())
            {
                const SwitchInfo* w = nullptr;
                for (const SwitchInfo& x : plan.switches)
                {
                    if (x.key == br->switchKey)
                        w = &x;
                }
                std::string test;
                std::string positions;
                for (int position : br->closed)
                {
                    test += (test.empty() ? "" : " || ") + w->name + " == " + std::to_string(position);
                    positions += (positions.empty() ? "" : ", ") + std::to_string(position);
                }
                const std::string& S = br->ident;
                at = elementLine(br->element);
                line("        const float to_" + S + " = " + test + " ? 1.0f : 0.0f;",
                     br->part + " closed at " + w->name + " " + positions);
                line("        sw_" + S + " = fabsf(to_" + S + " - sw_" + S + ") <= 1.001f * swstep_ ? to_" + S +
                     " : sw_" + S + " + (to_" + S + " > sw_" + S + " ? swstep_ : -swstep_);");
            }
            at = potLine();
            for (const Block& b : plan.blocks)
            {
                if (!b.hasControls())
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
            if (!b.hasControls())
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
            at = blockLine(b);
            line("    void b" + std::to_string(b.index) + "_set(" + params + ") [[clang::nonblocking]]",
                 "block " + std::to_string(b.index) + "'s coefficients" +
                     (blocks[i].woodbury ? std::string(": what its pots move (Woodbury)") : ""));
            line("    {");
            const bool shift = movesPortCurve(i);
            const std::string P = b.port ? b.port->name : std::string();
            if (shift)
                line("        const float kprev_" + P + " = ka_" + P + ";", "ADAA: the port's curve before the move");
            if (movesReverse(i))
                line("        const float krprev_" + P + " = kr_" + P + ";");
            for (const std::string& l : blocks[i].rLines)
                line("        " + l);
            for (const std::string& l : blocks[i].lines)
                line("        " + l);
            if (gainField(b) && b.port->gain && !lawGain(b))
                line("        g_" + P + " = " + gainText(b, [&](const std::string& k) { return potName(k); }) + ";",
                     b.port->parts + "'s gain, amps" + (b.port->prewarp ? ", prewarped" : ""));
            if (lawGain(b))
            {
                for (const std::string& key : b.port->gainPots)
                    line("        pv_" + potName(key) + " = " + potName(key) + ";",
                         "for " + b.port->parts + "'s gain, which tick computes");
            }
            if (shift)
            {
                // ADAA's mean pairs (v_P, p_P) with the next step's (v, p) on one curve, p = v + K f(v): when K moves,
                // p_P moves onto the new curve with v_P held (its Newton residual kept), or the closed form's (F(p) -
                // F(p_P)) / (p - p_P) spikes as |p - p_P| falls toward its threshold. A held pot gives the same K, so
                // this changes nothing.
                LawOperands o = operands(i);
                for (const LawLine& l : carryText(*b.port, o))
                    line("        " + l.text, l.note);
            }
            line("    }");
        }

        void Writer::blockStep(std::size_t i)
        {
            const Block& b = plan.blocks[i];
            const System& s = blocks[i].system;
            const bool adaa = adaaBlock(b);
            const std::string& in = plan.inputName;
            const std::string T = type;
            at = blockLine(b);
            line("    " + T + " b" + std::to_string(b.index) + "(" + T + " " + in + ") [[clang::nonblocking]]",
                 "block " + std::to_string(b.index) + ", one step at " + rateText(b) +
                     (matchedBlock(b) ? std::string(": its matched design") : ""));
            line("    {");
            if (matchedBlock(b))
            {
                // transposed direct form II: y = b0 u + s1, s1 <- b1 u - a1 y + s2, s2 <- b2 u - a2 y; its output is
                // ym_, a name no input can take (an input's name is letters and digits)
                const std::string n = "_b" + std::to_string(b.index);
                const bool two = orderOf(b) == 2;
                line("        const " + T + " ym_ = mb0" + n + " * " + in + " + s1" + n + ";");
                line("        s1" + n + " = mb1" + n + " * " + in + " - ma1" + n + " * ym_" + (two ? " + s2" + n : "") +
                     ";");
                if (two)
                    line("        s2" + n + " = mb2" + n + " * " + in + " - ma2" + n + " * ym_;");
                line("        return ym_;");
                line("    }");
                return;
            }
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
                    // a diode's v (ADAA: its mean, vb); an OTA's current (its mean with ADAA), ib
                    pText.push_back("v");
                    rText.push_back(s.iForm ? "ib" : adaa ? "vb" : "v");
                    break;
                case System::Source::Kind::Constant:
                    // the DC operating point's alone (the offsets have none)
                    pText.push_back("0");
                    rText.push_back("0");
                    break;
                }
            }
            const LawOperands law = operands(i);
            if (b.port)
            {
                const std::string& P = b.port->name;
                std::vector<std::pair<Val, std::string>> pTerms;
                for (std::size_t k = 0; k < s.sources.size(); ++k)
                {
                    if (static_cast<int>(k) != s.port)
                        pTerms.emplace_back(coefficient(i, "p_" + P + "_by_" + s.sources[k].name), pText[k]);
                }
                line("        const " + T + " p = " + linear(pTerms) + ";");
                for (const LawLine& l : solveText(*b.port, law))
                    line("        " + l.text, l.note);
                if (adaa)
                {
                    for (const LawLine& l : adaaText(*b.port, law))
                        line("        " + l.text, l.note);
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
                const bool inductor = br.kind == Branch::Kind::Inductor;
                std::string note;
                if (inductor)
                    note = br.part + "'s current is " + s.readouts[static_cast<std::size_t>(readout)].label;
                else if ((vc == "v" || vc == "vb") && !s.iForm)
                    note = br.part + " across the port: its voltage is v";
                else
                    note = br.part + "'s voltage is " + s.readouts[static_cast<std::size_t>(readout)].label;
                const std::string g2 = (negative ? "-" : "") + std::string(inductor ? "r2_" : "g2_") + ident;
                if (b.method == Method::Bdf2)
                {
                    // BDF2's history in two parts: x = 4/3 g v - g v_prev / 3, xo = g v / 3, so x <- h - xo and xo <- h
                    // / 4, h = 4/3 g v (an inductor's 4/3 r i)
                    line("        const " + T + " h_" + ident + " = " + g2 + " * " + vc + ";", note + " (BDF2)");
                    line("        " + src.name + " = h_" + ident + " - xo_" + ident + ";");
                    line("        xo_" + ident + " = 0.25f * h_" + ident + ";");
                    continue;
                }
                line("        " + src.name + " = " + g2 + " * " + vc + " - " + (adaa ? "xa_" + ident : src.name) +
                         ";",
                     note);
            }
            if (b.port)
            {
                const std::string& P = b.port->name;
                line("        v_" + P + " = v;");
                for (const LawLine& l : storeText(*b.port, law))
                    line("        " + l.text, l.note);
                if (input)
                    line("        uo_" + s.sources[0].name + " = " + in + ";");
            }
            line("        return " + readoutText[static_cast<std::size_t>(s.output)] + ";");
            line("    }");
        }

        // The region's blocks on `x`: b3(b2(x)).
        std::string Writer::regionCall(const std::string& x) const
        {
            std::string call = x;
            for (int i = plan.regionFirst; i <= plan.regionLast; ++i)
                call = "b" + std::to_string(i + 1) + "(" + call + ")";
            return call;
        }

        // ", float iabc": the law inputs, tick's and step's parameters after the signal (floats with .channels 2 too,
        // broadcast where they meet a float2).
        std::string Writer::lawParameters() const
        {
            std::string text;
            for (const LawInfo& l : plan.laws)
                text += ", float " + l.name;
            return text;
        }

        // The law inputs, clamped to their ranges (fmaxf first: a NaN is the low end), and the gains of the OTAs that
        // read them: what tick does once a host sample, and step once a call, before the region's blocks run. The
        // gains are held over a host sample's region steps.
        void Writer::lawLines()
        {
            if (!hasLawGains())
                return;
            const int back = at;
            for (const LawInfo& l : plan.laws)
            {
                if (!l.used)
                    continue;
                at = l.loc.line;   // Result::lines: a law input's line, at its .input
                const std::string x = l.scale == 1 ? l.name : l.name + " * " + literal(l.scale);
                line("        const float l_" + l.name + " = fminf(fmaxf(" + x + ", " + literal(l.low) + "), " +
                         literal(l.high) + ");",
                     ".input " + l.name + ", clamped to " + std::format("{:g}:{:g}", l.low, l.high) +
                         " (a NaN is its low end)");
            }
            for (const Block& b : plan.blocks)
            {
                if (!lawGain(b))
                    continue;
                at = portLine(b);   // and an OTA's gain, at the OTA
                line("        g_" + b.port->name + " = " +
                         gainText(b, [&](const std::string& k) { return "pv_" + potName(k); }) + ";",
                     b.port->parts + "'s gain, amps" + (b.port->prewarp ? ", prewarped" : ""));
            }
            at = back;
        }

        bool Writer::hasLawGains() const
        {
            return std::any_of(plan.blocks.begin(), plan.blocks.end(), [](const Block& b) { return lawGain(b); });
        }

        void Writer::prewarpMethods()
        {
            for (const Block& b : plan.blocks)
            {
                if (!b.port || !b.port->prewarp)
                    continue;
                const PortDef& port = *b.port;
                const std::string& P = port.name;
                const std::string a = literal(port.poleA);
                at = portLine(b);
                line("    float pw" + std::to_string(b.index) + "_(float g_) [[clang::nonblocking]]",
                     port.parts + "'s gain prewarped (.prewarp on): the trapezoidal rule's pole at the analog one's");
                line("    {");
                const std::string plusB = (port.poleB < 0 ? " - " : " + ") + literal(std::fabs(port.poleB));
                const std::string minusA = (port.poleA < 0 ? " + " : " - ") + literal(std::fabs(port.poleA));
                line("        const float pole_ = " + a + plusB + " * g_;",
                     std::format("its small-signal pole, a + b g rad/s (a = {:.7g}, b = {:.7g})", port.poleA,
                                 port.poleB));
                line("        const float w_ = fminf(fabsf(pole_) * pwk_" + P + ", 1.5f);",
                     "|pole| / 2 fs', below 1.5 (tan's pole is at pi / 2)");
                line("        const float t_ = pws_" + P + " * tanf(w_);");
                line("        return ((pole_ < 0 ? -t_ : t_)" + minusA + ") * " + literal(1 / port.poleB) + ";",
                     "g' with a + b g' = sign(pole) 2 fs' tan(w)");
                line("    }");
            }
        }

        void Writer::step()
        {
            if (plan.regionFirst < 0)
                return;
            const std::string& in = plan.inputName;
            const std::string region = plan.regionFirst == plan.regionLast
                                           ? "block " + std::to_string(plan.regionFirst + 1)
                                           : std::format("blocks {} to {}", plan.regionFirst + 1, plan.regionLast + 1);
            const std::string rate = plan.config.oversample == 1 ? std::string("the host rate")
                                                                 : std::to_string(plan.config.oversample) + " rate";
            at = inputLine();
            line("    " + type + " step(" + type + " " + in + lawParameters() + ") [[clang::nonblocking]]",
                 "one step of the oversampled region (" + region + "), at " + rate);
            line("    {");
            lawLines();
            line("        return " + regionCall(in) + ";");
            line("    }");
        }

        void Writer::tick()
        {
            const std::string& in = plan.inputName;
            const std::string T = type;
            const IoLine& input = *plan.netlist.input;
            const IoLine& output = *plan.netlist.output;
            at = inputLine();
            line("    " + T + " tick(" + T + " " + in + lawParameters() + ") [[clang::nonblocking]]",
                 std::format("one host sample: .input {} (1.0 = {:g} V) to .output {}", input.name, input.scale,
                             output.name));
            line("    {");
            guard();
            lawLines();
            // with law inputs, the region's blocks are called here, the gains held over its steps (step takes the law
            // inputs, and would compute them again)
            auto stepOf = [&](const std::string& x)
            {
                return plan.laws.empty() ? "step(" + x + ")" : regionCall(x);
            };
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
                    body.push_back("const " + T + " " + name + " = " + stepOf(current) + ";");
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
                        body.push_back("const " + T + " " + ys.back() + " = " + stepOf(values[k]) + ";");
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
            // the output, faded in after a reset (gain_ is 1 when settled: the output is the circuit's, bit for bit)
            for (const std::string& l : body)
                line("        " + l);
            line("        const float gain_ = fd_;", "the guard's fade");
            line("        fd_ = fminf(fd_ + fdstep_, 1);");
            if (plan.outputDc != 0)
                // the module runs in offsets from the DC operating point: the output's own DC is added back, after
                // the fade (a reset returns the circuit to its DC point, not to 0 V)
                line("        return (gain_ < 1 ? " + current + " * gain_ : " + current + ") + " +
                         literal(plan.outputDc) + ";",
                     std::format("V({})'s DC operating point, {:g} V", plan.nodes[static_cast<std::size_t>(
                                                                         plan.outputNode)].name,
                                 plan.outputDc * plan.outputScale));
            else
                line("        return gain_ < 1 ? " + current + " * gain_ : " + current + ";");
            line("    }");
        }

        void Writer::guard()
        {
            // every 16 ticks, the sum of every carried field: not finite (a NaN or an infinity anywhere in it), and
            // the module goes back to its DC point and fades its output in over 20 ms
            const std::vector<std::pair<std::string, int>> fields = carried();
            line("        if (ck_ == 0)", "the guard, every 16 ticks");
            line("        {");
            // the scalars in one sum, then each length of array in a loop
            std::string sum;
            bool first = true;
            auto flush = [&]
            {
                if (sum.empty())
                    return;
                line(first ? "            " + type + " sum_ = " + sum + ";" : "            sum_ = sum_ + " + sum + ";");
                first = false;
                sum.clear();
            };
            for (const auto& [name, count] : fields)
            {
                if (count != 0)
                    continue;
                sum += (sum.empty() ? "" : " + ") + name;
                if (sum.size() > 90)
                    flush();
            }
            if (first && sum.empty())
                sum = "0";
            flush();
            std::vector<int> lengths;
            for (const auto& [name, count] : fields)
            {
                if (count != 0 && std::find(lengths.begin(), lengths.end(), count) == lengths.end())
                    lengths.push_back(count);
            }
            for (int n : lengths)
            {
                std::string terms;
                for (const auto& [name, count] : fields)
                {
                    if (count == n)
                        terms += " + " + name + "[i]";
                }
                line("            for (int i = 0; i < " + std::to_string(n) + "; ++i)");
                line("                sum_ = sum_" + terms + ";");
            }
            if (type == "float2")
                line("            if (!(sum_.x - sum_.x == 0 && sum_.y - sum_.y == 0))", "either lane resets both");
            else
                line("            if (!(sum_ - sum_ == 0))", "a NaN or an infinity");
            line("            {");
            line("                clear();");
            line("                fd_ = 0;");
            line("            }");
            line("        }");
            line("        ck_ = ck_ == 15 ? 0 : ck_ + 1;");
        }
    } // namespace

    std::string emit(const Plan& plan, const std::vector<BlockCode>& blocks, std::vector<int>* lines)
    {
        Writer w{plan, blocks, plan.config.channels == 2 ? "float2" : "float", {}, {}, 0};
        w.header();
        w.at = w.titleLine();
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
        w.prewarpMethods();
        if (plan.regionFirst >= 0)
        {
            w.at = w.titleLine();
            for (int s = 1; s <= stagesOf(plan.config.oversample); ++s)
            {
                std::string text;
                emitHalfbandMethods(text, s, plan.config.halfband, w.type);
                w.raw(text);
            }
        }
        w.step();
        w.tick();
        w.at = w.titleLine();
        w.line("};");
        if (static_cast<std::size_t>(std::count(w.out.begin(), w.out.end(), '\n')) != w.lines.size())
            throw std::logic_error("emit: the line map has " + std::to_string(w.lines.size()) + " lines");
        if (lines != nullptr)
            *lines = std::move(w.lines);
        return w.out;
    }
} // namespace fminus::circuit
