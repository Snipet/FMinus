// src/circuit/Emit.h: FMinus text. Val is the number type that prints: Mna.h's derivation, run on Vals, writes the
// float code that computes a block's coefficients (in its b<N>_set, or init), folding as it goes. Code is a method
// body being written. emit() writes the whole module from a plan and each block's derivation.
#pragma once

#include "circuit/Mna.h"

#include <map>
#include <optional>
#include <set>
#include <string>
#include <vector>

namespace fminus::circuit
{
    class Code;

    // Zero, a constant (folded in double), or a named float (a field, a parameter or a temporary), maybe negated.
    // Arithmetic on Vals folds 0 x = 0, x +- 0 = x, +-1 x = +-x, constants with constants, and x - x = 0; every other
    // operation prints `const float t<N> = a op b;` into the Code of its operands and gives t<N>. A negation is carried
    // in the Val (and absorbed by the operation that uses it), not printed. A Var's deps say what it depends on: the
    // rate (a capacitor's gc, an inductor's rl), the pots (a variable resistance, a gain of pots, a switch's), both or
    // neither; an operation's are its operands', so a block's derivation splits into what init computes once and what
    // b<N>_set computes when a pot moves (the Woodbury form, Code::split).
    struct Val
    {
        enum class Kind
        {
            Zero,
            Const,
            Var,
        };

        static constexpr int kRate = 1;
        static constexpr int kPots = 2;

        Kind kind = Kind::Zero;
        double c = 0;           // a Const's value
        std::string name;       // a Var's
        bool negated = false;   // a Var's sign
        Code* code = nullptr;   // where a Var's operations print
        int deps = 0;           // a Var's: kRate, kPots, both or neither

        Val() = default;
        explicit Val(double value);
        static Val var(std::string name, Code* code, int deps = 0);
    };

    Val operator+(const Val& a, const Val& b);
    Val operator-(const Val& a, const Val& b);
    Val operator*(const Val& a, const Val& b);
    Val operator/(const Val& a, const Val& b);
    Val operator-(const Val& a);
    bool isZero(const Val& v);
    Val logOf(const Val& v);
    // expf, sqrtf and cosf of a Val (folded in double for a constant); `whereNegative(c, a, b)` is `c < 0 ? a : b`
    // (chosen at generation for a constant c). The matched designs' arithmetic (Linear.h).
    Val expOf(const Val& v);
    Val sqrtOf(const Val& v);
    Val cosOf(const Val& v);
    Val whereNegative(const Val& c, const Val& a, const Val& b);
    void annotate(Val& v, const std::string& comment);
    Val bindField(const std::string& field, Val value);

    // Whether v is exactly +1 or -1 (a Const).
    bool isUnit(const Val& v);

    // A method body being written: statements, each `const float t<N> = ...;`, `field = ...;` or text. lines() gives
    // them with each temporary used once, by the field assignment that follows it, merged into that assignment.
    class Code
    {
    public:
        // `reserved`: names its temporaries skip (the method's parameters: pots may be named t0).
        explicit Code(std::set<std::string> reserved = {});

        // Prints `const float t<N> = text;` and gives t<N>; `uses` are the names the text reads, `deps` what it
        // depends on (Val's).
        Val temp(const std::string& text, const std::vector<std::string>& uses, int deps = 0);

        // Prints `field = <v>;` (a negated Var as -t).
        void assign(const std::string& field, const Val& v);

        // Whether bindField assigns fields (true), or returns its value as it is (a matched block's trapezoidal
        // coefficients are its design's inputs, not fields).
        void bindFields(bool on)
        {
            fieldsOn_ = on;
        }

        // A line of its own (a local's declaration, a call), reading `uses`.
        void raw(const std::string& text, const std::vector<std::string>& uses = {}, const std::string& comment = {});

        // A comment on the statement that defined `v`, if it is the last one written.
        void comment(const Val& v, const std::string& text);

        std::vector<std::string> lines() const;

        // The operations it prints: one a temporary.
        int operations() const;

        // The statements split by what they depend on (the Woodbury form): those of the rate alone (or of nothing) for
        // init, those of the pots for b<N>_set. A rate-only temporary that a pot's statement reads becomes a field
        // of init's, `prefix` and a number (w2_0, w2_1, ...), which `fields` lists. Each list is merged as lines()
        // merges, and its operations counted.
        struct Split
        {
            std::vector<std::string> init;
            std::vector<std::string> set;
            std::vector<std::string> fields;
            int initOperations = 0;
            int setOperations = 0;
        };

        Split split(const std::string& prefix) const;

        // The fields assigned (bind's), in order.
        const std::vector<std::string>& fields() const
        {
            return fields_;
        }

    private:
        struct Stmt
        {
            std::string lhs;         // a temporary's or a field's name; empty for raw text
            bool declares = false;   // const float lhs = ...
            std::string rhs;         // or the raw text
            std::string note;
            std::vector<std::string> uses;
            int deps = 0;            // what its value depends on (Val's kRate, kPots)
        };

        static std::vector<std::string> merged(const std::vector<Stmt>& stmts);

        std::set<std::string> reserved_;
        std::map<std::string, std::string> seen_;   // an expression's text, and its temporary
        std::map<std::string, int> depsOf_;         // a temporary's deps
        bool fieldsOn_ = true;
        int next_ = 0;
        std::vector<Stmt> stmts_;
        std::vector<std::string> fields_;
        friend Val bindField(const std::string& field, Val value);
    };

    // A Val as an operand: a literal, or a name with its sign (-t3).
    std::string operandText(const Val& v);

    // A block's equations, its pivots, and its coefficients as FMinus: the statements of its b<N>_set (or, for a
    // block without pots, of init's part), after the lines that compute its variable resistances from the pots. A block
    // with pots or switches is derived twice: in Phase 0's pivot order, every statement in b<N>_set, and with its
    // variable branches' currents pivoted last (the Woodbury form: Holters and Zölzer's (R + Q)^-1 is the k x k
    // elimination those leave), the rate-only statements in init and only what the pots move in b<N>_set; the form
    // whose b<N>_set does fewer operations is kept (`woodbury`), and `form` says which, with both counts.
    struct BlockCode
    {
        System system;
        Pivots pivots;
        Coefficients<Val> coefficients;   // each a field (Var), a literal (Const), or left out (Zero)
        std::vector<std::string> lines;
        std::vector<std::string> rLines;
        std::vector<std::string> initLines;   // the Woodbury form's rate-only part, which init computes
        std::vector<std::string> wFields;     // and the fields b<N>_set reads it from: w<N>_<k>
        bool woodbury = false;
        std::string form;   // "b3_set: the Woodbury form, 24 operations (init 70); all in set, 92"
    };

    // Block `block`'s BlockCode, or nothing (an error in `messages`) when its equations are singular. Without
    // `woodbury`, all in b<N>_set whatever it costs (the bench's comparison).
    std::optional<BlockCode> deriveBlock(const Plan& plan, int block, Messages& messages, bool woodbury = true);

    // The module's text; `lines` gets, for each of its lines, the netlist's line it comes from (Result::lines).
    std::string emit(const Plan& plan, const std::vector<BlockCode>& blocks, std::vector<int>* lines = nullptr);

    // The configuration as one line (Result::configuration, and the header's): each choice, grouped by where it came
    // from ("configuration: .oversample 4 iir blocks=auto, .adaa off, .solver omega(2) (the netlist); .method tr
    // (default)").
    std::string configText(const Config& config);

    // The delay ADAA adds, in host samples: half a step at the region's rate for each port block it runs in.
    double adaaLatency(const Plan& plan);

    // The FMinus keywords and builtins (a pot, an input or the module may not be named one).
    bool isFMinusKeyword(std::string_view name);
} // namespace fminus::circuit
