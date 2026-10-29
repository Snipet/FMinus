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
        std::string ident;   // its FMinus form: "C1", "Ptone_a"
        int element = -1;    // in the netlist
        // Variable: r = value * (1 - t) (section 1, e1-w), value * t (section 2, w-e2, or a rheostat), or the
        // resistor's {expr} (section 0); t = taper(clamp01(expr)).
        int section = 0;
        double value = 0;
        Expr expr;
        Taper taper = Taper::Lin;
    };

    // A nonlinear port: an antiparallel diode pair, i = 2 Is sinh(v / N Vt), v = V(anode) - V(cathode).
    struct PortDef
    {
        std::string name;    // the first diode's, "D1" (its ident)
        std::string parts;   // "D1|D2"
        int anode = 0;
        int cathode = 0;
        double is = 0;
        double n = 1;
        double nvt = 0;   // N k T / q at 27 C
        std::vector<int> elements;
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
        std::vector<std::string> pots;   // the pots it reads, folded, in .pot order
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

    // The configuration, and where each choice came from: "the netlist", "Options", "default" or "auto". The defaults
    // are Phase 0's for a 1-port exp-affine block, the configuration that meets the report's targets (T1-T4) on both
    // of its examples' full stress sets: 8x, IIR halfbands, plain omega3 + 2 Newton steps (docs/circuits.md).
    struct Config
    {
        int oversample = 8;
        Halfband halfband = Halfband::Iir;
        Region region = Region::Auto;
        Solver solver{Solver::Guess::Omega, 2};
        bool adaa = false;
        int channels = 1;
        std::string oversampleFrom = "default";
        std::string halfbandFrom = "default";
        std::string regionFrom = "default";
        std::string solverFrom = "default";
        std::string adaaFrom = "default";
        std::string channelsFrom = "default";
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
        std::vector<Block> blocks;   // in chain order
        int regionFirst = -1;        // the oversampled run of blocks, chain indices; -1: none
        int regionLast = -1;
        int inputNode = 0;
        int outputNode = 0;
        std::string inputName;   // .input's: tick's and step's parameter
        double inputScale = 1;
        double outputScale = 1;
    };

    // The physical N Vt of a diode, N k 300.15 / q (CODATA's k and q, SPICE's TNOM of 27 C).
    double thermalVoltage(double n);

    // The pot positions a check runs a block at: its pots at {0, 0.5, 1}, the full grid for 3 pots or fewer, otherwise
    // each pot alone (the others at their defaults) and all at 0 and all at 1. Each point gives every pot's value, by
    // folded name, in plan.pots' order.
    std::vector<std::vector<double>> potGrid(const Plan& plan, const Block& block);

    // A pot's value by folded name, from a point of potGrid (or the defaults).
    double potValue(const Plan& plan, const std::vector<double>& point, const std::string& key);

    // The plan, or nothing when the netlist has errors (in `messages`).
    std::optional<Plan> plan(const Netlist& netlist, const Options& options, std::string_view fileName,
                             Messages& messages);

    // Names a pot or an input may not take: FMinus's keywords and builtins, and the names the module uses.
    bool isReservedName(std::string_view name);
} // namespace fminus::circuit
