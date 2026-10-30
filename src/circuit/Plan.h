// src/circuit/Plan.h: the circuit compiler's second stage. A Netlist is elaborated (diodes into ports: one, a series
// string, a pair, an asymmetric pair, RS written in as a resistor; pots into variable resistors; V sources into driven
// nodes or branches; E, G and I sources; models and tapers resolved), checked (connections, DC paths, loops of
// sources and inductors, op amps' feedback), cut into blocks at ideal op amps and grounded E sources, and ordered as a
// chain from .input to .output. The plan also holds the DC operating point (Dc.h), the configuration (Options over
// directives over defaults, with where each choice came from), which blocks run in the oversampled region, and each
// linear block's 1x check.
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
            Vcvs,    // an E's output, the E's other output terminal grounded: driven, as an op amp's
            Dc,      // a DC voltage source's (V<name> with a terminal grounded): a constant, ground in the offsets
        };

        std::string name;    // as first written
        std::string ident;   // for FMinus names: [A-Za-z0-9_], unique
        Source source = Source::None;
        int owner = -1;   // the block (chain index) that solves for or drives it; -1 for ground and .input
        int connections = 0;
        double value = 0;      // Dc: its volts
        int element = -1;      // Vcvs, Dc: the element that drives it
        bool absorbed = false;   // inside a series string of diodes (D1+D2): the string's law covers it
    };

    // A two-terminal branch of a block's network.
    struct Branch
    {
        enum class Kind
        {
            Conductance,   // a constant resistor: g = 1 / R
            Capacitor,     // a trapezoidal companion: gc = 2 C fs', and its state
            Variable,      // a resistance that moves with pots: its current is an unknown (the impedance form)
            Inductor,      // the impedance form: its current an unknown, V(a) - V(b) - rl i = -x, rl = 2 L fs' (TR),
                           // and its state, x <- 2 rl i - x; at DC (rl = 0) an exact short
            Voltage,       // a DC voltage source neither terminal of which is ground: its current an unknown, V(a) -
                           // V(b) = dc (0 in the offsets: a short)
        };

        Kind kind = Kind::Conductance;
        int a = 0;   // nodes; current flows a -> b
        int b = 0;
        double g = 0;        // Conductance
        double c = 0;        // Capacitor, farads
        double l = 0;        // Inductor, henries
        double dc = 0;       // Voltage: volts
        std::string part;    // the part's name, as written
        std::string ident;       // its FMinus form, unique among the parts': "C1", "Ptone_a"
        std::string partIdent;   // its element's FMinus form: "Ptone" for both of a pot's sections
        int element = -1;        // in the netlist
        // Variable: r = value * (1 - t) (section 1, e1-w), value * t (section 2, w-e2, or a rheostat), or the
        // resistor's {expr} (section 0); t = taper(clamp01(expr)). Section 3 is a switch's (an S element): r =
        // exp(ln roff + s (ln ron - ln roff)), s its ramp from 0 (open) to 1 (closed at `closed`'s positions).
        int section = 0;
        double value = 0;
        Expr expr;
        Taper taper = Taper::Lin;
        std::string switchKey;     // a switch's .switch, folded
        std::vector<int> closed;   // the positions it is closed at
        double ron = 1;
        double roff = 10e6;

        // Whether it is a switch's resistance.
        bool isSwitch() const
        {
            return kind == Kind::Variable && section == 3;
        }

        // Whether it carries a state from step to step: a capacitor or an inductor.
        bool state() const
        {
            return kind == Kind::Capacitor || kind == Kind::Inductor;
        }
    };

    // A controlled source: an E (a VCVS, V(p) - V(n) = gain (V(cp) - V(cn))) or a linear G (a VCCS, gain (V(cp) -
    // V(cn)) amps from p through it to n, SPICE's direction), its gain a constant or an expression of pots.
    //   driven  an E with an output terminal grounded: its other terminal, `out`, is driven (a cut, as an op amp's
    //           output), the E in the block of its controls (or a buffer block of its own when both are driven)
    //   else    an E's row in its block (its current an unknown), or a G's stamp
    struct Controlled
    {
        enum class Kind
        {
            Vcvs,
            Vccs,
        };

        Kind kind = Kind::Vcvs;
        std::string name;    // as written: "E1"
        std::string ident;   // its FMinus form: k_<ident> is its gain's local where pots move it
        int element = -1;
        int p = 0;   // its output terminals
        int n = 0;
        int cp = 0;   // its control nodes
        int cn = 0;
        double gain = 0;            // a constant gain
        std::optional<Expr> expr;   // or one of pots
        bool driven = false;
        int out = 0;          // driven: the node it drives
        double sign = 1;      // driven: V(out) = sign gain (V(cp) - V(cn)) (-1 when p is the grounded one)
    };

    // A DC current source (I<name> n+ n- value): value amps from a through it to b. It is open in the offsets the
    // module runs in, and only the DC operating point (Dc.h) sees it.
    struct CurrentSource
    {
        std::string name;
        int element = -1;
        int a = 0;
        int b = 0;
        double value = 0;
    };

    // A nonlinear port: its voltage v = V(anode) - V(cathode) and its current j = f(v), which flows through it from
    // `from` to `to`. The port's equation, once its block is reduced, is v + kd f(v) = p (report §6.1), kd > 0.
    //   Pair    an antiparallel diode pair, f = 2 Is sinh(v / N Vt) (the exp-affine family, EA)
    //   Single  one diode, f = Is (e^(v / N Vt) - 1) (EA)
    //   Asym    an antiparallel pair of two laws, f = Is (e^(v / N Vt) - 1) - Is2 (e^(-v / N2 Vt) - 1) (EA)
    //   Tanh    an OTA (G<name> n+ n- OTA nc+ nc- gain), f = g tanh(v / 2 Vt) with g its gain, which may move every
    //           sample with a law input (the smooth-monotone family, SM)
    // A series string of one model (D1+D2: n diodes through nodes nothing else touches) is one diode of n N; a pair of
    // strings of one law each way is a Pair, of two laws an Asym. Form V is a two-terminal port,
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
        double is = 0;    // Pair, Single, Asym: IS (Asym: the anode-to-cathode diode's)
        double n = 1;     // and N (a string's: n N)
        double nvt = 0;   // and N k T / q at 27 C; Tanh: 2 Vt (k T / q at 27 C, twice)
        double is2 = 0;   // Asym: the cathode-to-anode diode's IS
        double n2 = 1;    // its N
        double nvt2 = 0;  // its N k T / q
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
        std::vector<int> driven;        // the op amp outputs it drives, then its driven E outputs
        std::vector<int> elements;      // its parts, in netlist order
        std::vector<Branch> branches;   // in netlist order
        std::vector<OpAmp> opamps;
        std::vector<Controlled> controlled;   // its E and G sources, in netlist order
        std::optional<PortDef> port;
        int input = -1;                  // the node it reads (.input's, or the block before's output)
        int output = -1;                 // the node the next block reads, or .output's
        std::vector<std::string> pots;   // the pots it reads (its network's and its OTA's gain), folded, in .pot order
        std::vector<std::string> switches;   // the switches its S elements read, folded, in .switch order
        bool inRegion = false;
        Method method = Method::Tr;   // its integrator: the configuration's in the region, the trapezoidal rule outside
        int rateFactor = 1;        // runs at rateFactor x the host's rate
        double designFactor = 1;   // designed at designFactor x that (2/3 with ADAA)
        double oneX = 0;           // a linear block's 1x check: worst dB at 44.1 kHz
        double oneXRegion = 0;     // and at oversample x 44.1 kHz
        // How a linear block outside the region runs: the trapezoidal rule, or a matched design at 1x (Linear.h),
        // when the trapezoidal rule's response fails the 1x check and the matched one's passes it
        enum class Design
        {
            Tr,
            Matched,
        };
        Design design = Design::Tr;
        double matchedError = 0;      // the matched design's 1x check: worst dB at 44.1 and 48 kHz
        double matchedPolarity = 1;   // the sign its numerator takes (an inverting block's -1)

        // Whether set() moves it: it reads pots or switches (its coefficients are b<N>_set's, else init's).
        bool hasControls() const
        {
            return !pots.empty() || !switches.empty();
        }
    };

    struct PotInfo
    {
        std::string name;   // as written
        std::string key;    // folded
        double defaultValue = 0.5;
        std::optional<Taper> taper;
        bool used = false;
    };

    // A switch (.switch NAME positions [default]): set()'s int argument after the pots, a position from 0 to
    // positions - 1. Each S element that names it ramps its resistance toward ron or roff over 20 ms of set() calls.
    struct SwitchInfo
    {
        std::string name;   // as written
        std::string key;    // folded
        int positions = 2;
        int defaultPosition = 0;
        Loc loc;
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
        bool methodAuto = false;           // the netlist wrote .method auto (the self-test's paste line then names it)
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
        std::vector<SwitchInfo> switches;   // in .switch order
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
        std::vector<CurrentSource> currents;   // the DC current sources, in netlist order
        // The DC operating point (Dc.h) at the default pots: each node's volts. The module runs in offsets from it:
        // its states, ports and signals are differences from these, so init and clear set 0.
        std::vector<double> dc;
        double outputDc = 0;   // V(.output's node) / its scale at the DC point: tick adds it (0: nothing)
    };

    // The physical N Vt of a diode, N k 300.15 / q (CODATA's k and q, SPICE's TNOM of 27 C).
    double thermalVoltage(double n);

    // The control positions a check runs a block at: its pots at {0, 0.5, 1} (at `resolution` positions evenly spaced
    // from 0 to 1) and its switches at each position, the full grid for 3 axes or fewer, otherwise each axis alone (the
    // others at their defaults) and all at their low ends (0, position 0) and all at their high ends (1, the last
    // position). Each point gives every pot's value in plan.pots' order, then every switch's position in
    // plan.switches' order (a point without them: the defaults).
    std::vector<std::vector<double>> potGrid(const Plan& plan, const Block& block, int resolution = 3);

    // A pot's value by folded name, from a point of potGrid (or the defaults).
    double potValue(const Plan& plan, const std::vector<double>& point, const std::string& key);

    // A switch's position by folded name, from a point of potGrid (its default when the point has none).
    int switchPosition(const Plan& plan, const std::vector<double>& point, const std::string& key);

    // The defaults as a point: each pot's default, then each switch's.
    std::vector<double> defaultPoint(const Plan& plan);

    // A switch section's ramp at a position: 1 where it is closed, 0 where open.
    double switchRamp(const Branch& branch, int position);

    // A switch section's resistance at its ramp s: exp(ln roff + s (ln ron - ln roff)), the emitted arithmetic.
    double switchResistance(const Branch& branch, double s);

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
