// src/circuit/Plan.h: the circuit compiler's second stage. A Netlist is elaborated (diode pairs merged into ports,
// pots into variable resistors, models and tapers resolved), checked (connections, DC paths, op amps' feedback), cut
// into blocks at ideal op amps, and ordered as a chain from .input to .output. The plan also holds the
// configuration (Options over directives over defaults, with where each choice came from), which blocks run in the
// oversampled region, and each linear block's 1x check.
#pragma once

#include "circuit/Netlist.h"

#include <optional>
#include <string>
#include <vector>

namespace fminus::circuit
{
    // A node of the netlist. Node 0 is ground.
    struct Node
    {
        enum class Source
        {
            None,    // a node a block solves for
            Input,   // .input's: a voltage source to ground
            OpAmp,   // an op amp's output: driven, whatever its load
        };

        std::string name;    // as first written
        std::string ident;   // for FMinus names: [A-Za-z0-9_], unique
        Source source = Source::None;
        int owner = -1;   // the block (chain index) that solves for or drives it; -1 for ground and .input
        int connections = 0;
    };

    // A two-terminal branch of a block's network.
    struct Branch
    {
        enum class Kind
        {
            Conductance,   // a constant resistor: g = 1 / R
            Capacitor,     // a trapezoidal companion: gc = 2 C fs', and its state
            Variable,      // a resistance that moves with pots: its current is an unknown (the impedance form)
        };

        Kind kind = Kind::Conductance;
        int a = 0;   // nodes; current flows a -> b
        int b = 0;
        double g = 0;        // Conductance
        double c = 0;        // Capacitor, farads
        std::string part;    // the part's name, as written
        std::string ident;       // its FMinus form, unique among the parts': "C1", "Ptone_a"
        std::string partIdent;   // its element's FMinus form: "Ptone" for both of a pot's sections
        int element = -1;        // in the netlist
        // Variable: r = value * (1 - t) (section 1, e1-w), value * t (section 2, w-e2, or a rheostat), or the
        // resistor's {expr} (section 0); t = taper(clamp01(expr)).
        int section = 0;
        double value = 0;
        Expr expr;
        Taper taper = Taper::Lin;
    };

    // A nonlinear port: its voltage v = V(anode) - V(cathode) and its current j = f(v), which flows through it from
    // `from` to `to`. The port's equation, once its block is reduced, is v + kd f(v) = p (report §6.1), kd > 0.
    //   Pair  an antiparallel diode pair, f = 2 Is sinh(v / N Vt) (the exp-affine family, EA)
    //   Tanh  an OTA (G<name> n+ n- OTA nc+ nc- gain), f = g tanh(v / 2 Vt) with g its gain, which may move every
    //         sample with a law input (the smooth-monotone family, SM)
    // (Single and Asym, a single diode and an asymmetric pair, are the next slice's.) Form V is a two-terminal port,
    // whose current flows between the terminals its voltage is across (a diode: from = anode, to = cathode), stamped
    // in Holters' v-form (Mna.h: v a source, j an unknown); Form I is a controlled source, its voltage across its
    // control nodes and its current between two others (an OTA: anode, cathode = nc+, nc-; from, to = n+, n-, SPICE's
    // G direction), stamped in the i-form (its current i a source, its control voltage a readout).
    struct PortDef
    {
        enum class Law
        {
            Pair,
            Single,
            Asym,
            Tanh,
        };

        enum class Form
        {
            V,
            I,
        };

        std::string name;    // the first part's ident: "D1", "G1"
        std::string parts;   // "D1|D2", "G1"
        Law law = Law::Pair;
        Form form = Form::V;
        int anode = 0;   // v = V(anode) - V(cathode): a diode's terminals, an OTA's control inputs
        int cathode = 0;
        int from = 0;   // j flows from `from` to `to`: a diode's anode and cathode, an OTA's n+ and n-
        int to = 0;
        double is = 0;    // Pair: IS
        double n = 1;     // Pair: N
        double nvt = 0;   // Pair: N k T / q at 27 C; Tanh: 2 Vt (k T / q at 27 C, twice)
        // Tanh: the gain, amps: a constant, or an expression of pots and law inputs
        std::optional<Expr> gain;
        double gainValue = 0;              // the constant
        std::vector<std::string> gainPots;   // the pots it reads, folded, in .pot order
        std::vector<std::string> gainLaws;   // the law inputs it reads, folded, in .input order
        // Tanh, .prewarp on: the small-signal pole, a + b g rad/s (an affine fit at generation), and whether it applies
        bool prewarp = false;
        double poleA = 0;
        double poleB = 0;
        std::vector<int> elements;

        bool smooth() const
        {
            return law == Law::Tanh;
        }
    };

    // An ideal op amp (a nullor): V(inp) = V(inn), any output current, no input current.
    struct OpAmp
    {
        std::string name;
        int inp = 0;
        int inn = 0;
        int out = 0;
        int element = -1;
    };

    // A block: the part of the network an op amp's output (or .input) cuts off, solved as one system.
    struct Block
    {
        int index = 0;                  // 1, 2, ... in chain order
        std::vector<int> nodes;         // the nodes it solves for, in netlist order
        std::vector<int> driven;        // the op amp outputs it drives
        std::vector<int> elements;      // its parts, in netlist order
        std::vector<Branch> branches;   // in netlist order
        std::vector<OpAmp> opamps;
        std::optional<PortDef> port;
        int input = -1;                  // the node it reads (.input's, or the block before's output)
        int output = -1;                 // the node the next block reads, or .output's
        std::vector<std::string> pots;   // the pots it reads (its network's and its OTA's gain), folded, in .pot order
        bool inRegion = false;
        int rateFactor = 1;        // runs at rateFactor x the host's rate
        double designFactor = 1;   // designed at designFactor x that (2/3 with ADAA)
        double oneX = 0;           // a linear block's 1x check: worst dB at 44.1 kHz
        double oneXRegion = 0;     // and at oversample x 44.1 kHz
    };

    struct PotInfo
    {
        std::string name;   // as written
        std::string key;    // folded
        double defaultValue = 0.5;
        std::optional<Taper> taper;
        bool used = false;
    };

    // A law input (.input NAME - kind=cv|current): tick's and step's floats after the signal, sample x scale clamped
    // to [low, high] where it is used.
    struct LawInfo
    {
        std::string name;   // as written: the parameter's name
        std::string key;    // folded
        Input::Kind kind = Input::Kind::Cv;
        double scale = 1;
        double low = 0;
        double high = 1;
        Loc loc;
        bool used = false;

        // The geometric middle of the range, or the arithmetic one when it starts at 0 or below.
        double middle() const;
    };

    // The configuration, and where each choice came from: "the netlist", "Options", "the self-test <key>",
    // "provisional" or "default". The searched dimensions (oversample, adaa, solver, method) are open when neither
    // the Options nor the netlist name them (or the netlist writes auto): Options::chosen fills them when its key is
    // the netlist's, and otherwise the provisional configuration does. The provisional configuration for a 1-port
    // exp-affine block is Phase 0's measured default, the one that meets the report's targets (T1-T4) on both of its
    // examples' full stress sets: 8x, IIR halfbands, plain omega3 + 2 Newton steps (docs/circuits.md); for a smooth-
    // monotone (OTA) port it is 2x, the previous step's v + 2 Newton steps.
    struct Config
    {
        int oversample = 8;
        Halfband halfband = Halfband::Iir;
        Region region = Region::Auto;
        Solver solver{Solver::Guess::Omega, 2};
        bool adaa = false;
        int channels = 1;
        Method method = Method::Tr;
        bool prewarp = false;   // .prewarp: an OTA's cutoff law prewarped
        bool otas = false;      // the ports are OTAs: configText names .prewarp
        std::string oversampleFrom = "default";
        std::string halfbandFrom = "default";
        std::string regionFrom = "default";
        std::string solverFrom = "default";
        std::string adaaFrom = "default";
        std::string channelsFrom = "default";
        std::string methodFrom = "default";
        std::string prewarpFrom = "default";
        // Which searched dimensions are open (the self-test's to choose); none for a netlist without a port.
        bool oversampleOpen = false;
        bool adaaOpen = false;
        bool solverOpen = false;
        bool methodOpen = false;
        bool provisional = false;          // a dimension is open and no matching Options::chosen filled it
        std::optional<Choice> chosen;      // the Options::chosen that filled the open dimensions
        bool anyOpen() const
        {
            return oversampleOpen || adaaOpen || solverOpen || methodOpen;
        }
    };

    struct Plan
    {
        std::string fileName;   // as the messages give it
        std::string baseName;   // its last component, for the generated header
        std::string module;     // the module's name
        Netlist netlist;
        std::vector<Node> nodes;   // [0] is ground
        Config config;
        std::vector<PotInfo> pots;   // in .pot order
        std::vector<LawInfo> laws;   // in .input order
        std::vector<Block> blocks;   // in chain order
        bool smooth = false;         // its ports are OTAs (the SM family); otherwise diodes (EA), or none
        int regionFirst = -1;        // the oversampled run of blocks, chain indices; -1: none
        int regionLast = -1;
        int inputNode = 0;
        int outputNode = 0;
        std::string inputName;   // .input's: tick's and step's parameter
        double inputScale = 1;
        double outputScale = 1;
        std::string key;   // the self-test's key (Circuit.h's selfTestKey): the netlist's and the Options'
    };

    // The physical N Vt of a diode, N k 300.15 / q (CODATA's k and q, SPICE's TNOM of 27 C).
    double thermalVoltage(double n);

    // The pot positions a check runs a block at: its pots at {0, 0.5, 1}, the full grid for 3 pots or fewer, otherwise
    // each pot alone (the others at their defaults) and all at 0 and all at 1. Each point gives every pot's value, by
    // folded name, in plan.pots' order.
    std::vector<std::vector<double>> potGrid(const Plan& plan, const Block& block);

    // A pot's value by folded name, from a point of potGrid (or the defaults).
    double potValue(const Plan& plan, const std::vector<double>& point, const std::string& key);

    // An OTA port's gain, amps, at `pots` (a point as potGrid's) and `laws` (each law input's value, in plan.laws'
    // order, clamped to its range as tick clamps it; empty: each at its low end). A constant gain is its value.
    double gainAt(const Plan& plan, const PortDef& port, const std::vector<double>& pots,
                  const std::vector<double>& laws);

    // A prewarped gain (.prewarp on, docs/circuits.md): g' with a + b g' = sign(a + b g) 2 fs' tan(w), w = |a + b g| /
    // (2 fs') below 1.5, at the design rate fs'; the same arithmetic as the emitted code's, in double.
    double prewarped(const PortDef& port, double g, double designRate);

    // The plan, or nothing when the netlist has errors (in `messages`).
    std::optional<Plan> plan(const Netlist& netlist, const Options& options, std::string_view fileName,
                             Messages& messages);

    // The text a self-test's key hashes: "fminus-circuit-selftest <kSelfTestVersion>", the netlist's canonical lines,
    // and the Options that change the answer, a line each.
    std::string keyText(const Netlist& netlist, const Options& options);

    // The self-test's key of a parsed netlist with `options` (Circuit.h's selfTestKey): FNV-1a 64 of keyText, as 16
    // hex digits.
    std::string keyOf(const Netlist& netlist, const Options& options);

    // A configuration as the notes give it: "8x IIR, ADAA, omega(1)" (`halfband`: with the halfbands' kind).
    std::string configurationText(int oversample, Halfband halfband, bool adaa, const Solver& solver,
                                  bool withHalfband);

    // Names a pot or an input may not take: FMinus's keywords and builtins, and the names the module uses.
    bool isReservedName(std::string_view name);

    // Names the module may not take: those, and every other name its text uses (FMinus reads a module's name as a
    // type wherever it appears).
    bool isModuleWord(std::string_view name);
} // namespace fminus::circuit
