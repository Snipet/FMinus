// src/circuit/Netlist.h: the circuit compiler's first stage. A .cir netlist (SPICE's lines: elements, .model cards,
// comments, continuations and units, plus the directives docs/circuits.md lists) becomes a Netlist, its parts and
// directives as written, each with its line and column. Also here: Messages, which every stage reports into (errors,
// warnings and notes, formatted as clang's), the {expressions} over pots, and how a number is printed as an FMinus
// float literal.
#pragma once

#include "fminus/Circuit.h"

#include <functional>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace fminus::circuit
{
    // A position in the netlist: line and column from 1 (a column counts bytes). Line 0 is the netlist as a whole.
    struct Loc
    {
        int line = 0;
        int column = 0;
    };

    enum class Severity
    {
        Error,
        Warning,
        Note,
    };

    // A message, and the notes that explain it (printed after it, as clang's notes are).
    struct Message
    {
        Severity severity = Severity::Error;
        Loc loc;
        std::string text;
        std::vector<Message> notes;
    };

    // What every stage reports into. format() gives one severity's messages as "file:line:col: error: text" lines in
    // source order (a message without a line, "file: note: text", after the others), each followed by its notes.
    class Messages
    {
    public:
        void error(Loc loc, std::string text, std::vector<Message> notes = {});
        void warning(Loc loc, std::string text);
        void note(Loc loc, std::string text);
        bool hasErrors() const;
        std::string format(std::string_view fileName, Severity severity) const;

    private:
        std::vector<Message> messages_;
    };

    // What literal() throws for a value beyond float's range (the netlist's values, folded, can give one); generate()
    // reports it as an error of the netlist.
    struct NotAFloat : std::runtime_error
    {
        using std::runtime_error::runtime_error;
    };

    // A float literal in FMinus text: std::to_chars's shortest round trip of the float `value` rounds to (locale-free),
    // with ".0" when it has no point or exponent, and "f". With `smallInt`, an integral value of at most 16 in size is
    // an int literal instead ("2 * gc_C1"): only where the other operand is a float, which converts it exactly.
    std::string literal(double value, bool smallInt = false);

    // Lowercase, for the names SPICE compares without case.
    std::string folded(std::string_view text);

    // Whether `name` matches [A-Za-z][A-Za-z0-9]*: a pot's, an input's, an output's or a module's name.
    bool isPlainName(std::string_view name);

    // A pot's taper: t = taper(x) for its position x in 0..1.
    //   Lin    x
    //   Audio  (81^x - 1) / 80
    //   Rlog   1 - audio(1 - x)
    enum class Taper
    {
        Lin,
        Audio,
        Rlog,
    };

    const char* taperName(Taper taper);
    double taperValue(Taper taper, double x);

    // A {expression} over pots: numbers, pot names, + - * /, unary minus, ( ) and ^ with an integer exponent 0 to 8.
    struct Expr
    {
        enum class Op
        {
            Number,
            Pot,
            Neg,
            Add,
            Sub,
            Mul,
            Div,
            Pow,
        };

        Op op = Op::Number;
        double value = 0;       // a Number's
        std::string pot;        // a Pot's name, folded
        std::string spelling;   // a Pot's name, as written
        int exponent = 0;       // a Pow's
        Loc loc;
        std::vector<Expr> args;   // the operands: one for Neg and Pow, two for the others
    };

    // Its value, given each pot's (by folded name).
    double evaluate(const Expr& expr, const std::function<double(const std::string&)>& pot);

    // The folded names of the pots it reads, each once, in the order it first reads them.
    std::vector<std::string> potsOf(const Expr& expr);

    // As FMinus text: `pot` spells each pot (by folded name); subexpressions without a pot are folded in double.
    std::string printExpr(const Expr& expr, const std::function<std::string(const std::string&)>& pot);

    // An element of the netlist.
    struct Element
    {
        enum class Kind
        {
            Resistor,    // R<name> n+ n- value|{expr}
            Capacitor,   // C<name> n+ n- value
            Diode,       // D<name> anode cathode model
            Pot,         // P<name> e1 w e2 value {expr} [taper=...], or P<name> a b value {expr} [taper=...]
            OpAmp,       // X<name> inp inn out opamp
        };

        Kind kind = Kind::Resistor;
        std::string name;   // as written: "R1"
        Loc loc;
        std::vector<std::string> nodes;   // as written
        std::vector<Loc> nodeLocs;
        double value = 0;   // ohms or farads (a pot's whole resistance)
        Loc valueLoc;
        std::optional<Expr> expr;     // a variable resistor's resistance, or a pot's position
        std::optional<Taper> taper;   // a pot's taper=
        Loc taperLoc;
        std::string model;   // a diode's, as written
        Loc modelLoc;
        std::string text;   // the line, its tokens joined by spaces (for the generated comments)
    };

    // A .model card: a diode's.
    struct ModelCard
    {
        std::string name;
        Loc loc;
        double is = 1e-14;   // IS, amps
        double n = 1;        // N
    };

    // A .pot line.
    struct PotLine
    {
        std::string name;
        Loc loc;
        double defaultValue = 0.5;
        std::optional<Taper> taper;
    };

    // A .input or .output line.
    struct IoLine
    {
        std::string name;
        std::string node;
        Loc loc;
        Loc nodeLoc;
        double scale = 1;
    };

    // .target: what examples/circuits checks (the generator only echoes it).
    struct TargetLine
    {
        std::optional<double> solver;   // dB
        std::optional<double> alias;    // dB
        std::optional<double> level;    // volts
        std::optional<double> freq;     // Hz
        Loc loc;
    };

    // A setting a directive makes, with where it was made; `isAuto` when it said auto.
    template <typename T>
    struct Setting
    {
        std::optional<T> value;
        bool isAuto = false;
        Loc loc;
    };

    struct Netlist
    {
        std::string title;
        Loc titleLoc;
        std::vector<Element> elements;
        std::vector<ModelCard> models;
        std::vector<PotLine> pots;
        std::optional<IoLine> input;
        std::optional<IoLine> output;
        Setting<int> oversample;
        Setting<Halfband> halfband;
        Setting<Region> region;
        Setting<bool> adaa;
        Setting<Solver> solver;   // bare `omega`: Guess::Omega with steps 0 and solverBare
        bool solverBare = false;
        Setting<int> channels;
        TargetLine target;
    };

    // The netlist in `text`, as far as it reads; its errors and warnings go to `messages`.
    Netlist parse(std::string_view text, Messages& messages);
} // namespace fminus::circuit
