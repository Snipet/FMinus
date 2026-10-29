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
    // in the Val (and absorbed by the operation that uses it), not printed.
    struct Val
    {
        enum class Kind
        {
            Zero,
            Const,
            Var,
        };

        Kind kind = Kind::Zero;
        double c = 0;           // a Const's value
        std::string name;       // a Var's
        bool negated = false;   // a Var's sign
        Code* code = nullptr;   // where a Var's operations print

        Val() = default;
        explicit Val(double value);
        static Val var(std::string name, Code* code);
    };

    Val operator+(const Val& a, const Val& b);
    Val operator-(const Val& a, const Val& b);
    Val operator*(const Val& a, const Val& b);
    Val operator/(const Val& a, const Val& b);
    Val operator-(const Val& a);
    bool isZero(const Val& v);
    Val logOf(const Val& v);
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

        // Prints `const float t<N> = text;` and gives t<N>; `uses` are the names the text reads.
        Val temp(const std::string& text, const std::vector<std::string>& uses);

        // Prints `field = <v>;` (a negated Var as -t).
        void assign(const std::string& field, const Val& v);

        // A line of its own (a local's declaration, a call), reading `uses`.
        void raw(const std::string& text, const std::vector<std::string>& uses = {}, const std::string& comment = {});

        // A comment on the statement that defined `v`, if it is the last one written.
        void comment(const Val& v, const std::string& text);

        std::vector<std::string> lines() const;

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
        };

        std::set<std::string> reserved_;
        std::map<std::string, std::string> seen_;   // an expression's text, and its temporary
        int next_ = 0;
        std::vector<Stmt> stmts_;
        std::vector<std::string> fields_;
        friend Val bindField(const std::string& field, Val value);
    };

    // A Val as an operand: a literal, or a name with its sign (-t3).
    std::string operandText(const Val& v);

    // A block's equations, its pivots, and its coefficients as FMinus: the statements of its b<N>_set (or, for a
    // block without pots, of init's part), after the lines that compute its variable resistances from the pots.
    struct BlockCode
    {
        System system;
        Pivots pivots;
        Coefficients<Val> coefficients;   // each a field (Var), a literal (Const), or left out (Zero)
        std::vector<std::string> lines;
        std::vector<std::string> rLines;
    };

    // Block `block`'s BlockCode, or nothing (an error in `messages`) when its equations are singular.
    std::optional<BlockCode> deriveBlock(const Plan& plan, int block, Messages& messages);

    // The module's text.
    std::string emit(const Plan& plan, const std::vector<BlockCode>& blocks);

    // The FMinus keywords and builtins (a pot, an input or the module may not be named one).
    bool isFMinusKeyword(std::string_view name);
} // namespace fminus::circuit
