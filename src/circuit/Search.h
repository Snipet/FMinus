// src/circuit/Search.h: the self-test's arithmetic (Circuit.h's selfTest; report §2, §3.2, §6): the stress matrix (its
// points and signals, and the units, one of each), the double references a unit is measured against, a unit's gates
// (T1-T4) from the emitted module's output, their reduction into a Verdict, the candidates of the open dimensions in
// the report's cost-model order (the exp-affine row and the smooth-monotone one), the ordered search, the
// no-configuration choice, and the step budget. No LLVM: what
// compiles and runs a candidate is src/api/CircuitSelfTest.cpp, which hands this file the samples it ran.
#pragma once

#include "circuit/Plan.h"
#include "fminus/Circuit.h"

#include <cstddef>
#include <cstdint>
#include <functional>
#include <limits>
#include <optional>
#include <span>
#include <string>
#include <vector>

namespace fminus::circuit
{
    // The gates (report §2): T1 and T2 at most -60 dB, T3 within 0.5 dB (the user's ruling: H1 and H3).
    inline constexpr double kGateDb = -60;
    inline constexpr double kResponseDb = 0.5;
    // T3's H3 counts within 100 dB of H1 (Phase 0's rule) and at least 1e-6 of the input sine's amplitude (-120 dB):
    // below that, float's own rounding (about -140 dB of the circuit's largest signals, filtered by it) is a
    // measurable part of it, and a filter's H3 there (an OTA low-pass's at 3 kHz with its cutoff at 83 Hz, -160 dB) is
    // not a response anyone hears.
    inline constexpr double kH3FloorDb = -120;
    // The no-configuration cap (report §6.5): 4 % of a core a channel at 48 kHz, in model cycles a host sample.
    inline constexpr double kCostCap = 3500;
    // The budget when SelfTestOptions::budget is 0, in steps.
    inline constexpr std::int64_t kDefaultBudget = 4'000'000'000;

    // The analysis window: 4096 host samples (odd bins 9, 85 and 257 are 105, 996 and 3012 Hz) after 1024 of warm-up,
    // and 64 after it for the decimator's taps.
    struct StressShape
    {
        std::size_t window = 4096;
        std::size_t warm = 1024;
        std::size_t tail = 64;
        std::vector<int> bins{9, 85, 257};   // the sines' odd bins; T3 counts all but the first
        int referenceFactor = 64;            // T3's reference rate, times the host's

        std::size_t host() const
        {
            return warm + window + tail;
        }
    };

    // A point of the matrix: every pot's value (plan.pots' order) then every switch's position (plan.switches'), as
    // potGrid's points are; every law input's (plan.laws' order; each at its low end, middle or high end, lawLevels'
    // 0, 1 or 2), and how the notes name it ("drive 1", "mode 2", "iabc 0.000141").
    struct StressPoint
    {
        std::vector<double> pots;
        std::vector<double> laws;
        std::vector<int> lawLevels;
        std::string label;
    };

    // A signal of the matrix, its amplitude in the input's samples (volts / .input's scale).
    struct StressSignal
    {
        enum class Kind
        {
            Sine,
            Sweep,
            Noise,
            Saw,         // band-limited, on an odd bin (law inputs only: a synth's stress)
            Modulated,   // a sine at L with law input `law` moving +-2 octaves at 37 Hz around its middle
        };

        std::string name;   // "2.2 V, 3012 Hz", "sweep at 4.5 V", "996 Hz at 5 V, iabc +-2 octaves at 37 Hz"
        Kind kind = Kind::Sine;
        double amp = 0;
        int bin = 0;
        int law = -1;            // Modulated: which law input moves (plan.laws' index)
        bool overload = false;   // 2.2 L: T4 only
        bool t1 = true;          // measured on T1
        bool t2 = false;         // on T2 (sines up to L, and the saw), against the 64x reference
        bool t3 = false;         // on T3 (sines up to L, on bins 85 and 257)
    };

    // The matrix: points x signals, a unit each, unit u = point u / signals.size(), signal u % signals.size().
    struct StressMatrix
    {
        StressShape shape;
        double level = 1;          // L, volts
        std::string levelFrom;     // ".target level" or ".input's scale"
        double inputScale = 1;     // volts a sample, in and out (T3's H3 floor compares them in volts)
        double outputScale = 1;
        int upstream = 0;          // the blocks before the region, which T1's input runs through (regionInput)
        std::vector<StressPoint> points;
        std::vector<StressSignal> signals;

        int units() const
        {
            return static_cast<int>(points.size() * signals.size());
        }

        const StressPoint& point(int unit) const
        {
            return points[static_cast<std::size_t>(unit) / signals.size()];
        }

        const StressSignal& signal(int unit) const
        {
            return signals[static_cast<std::size_t>(unit) % signals.size()];
        }

        // "2.2 V, 3012 Hz, drive 1"
        std::string where(int unit) const;

        // Whether the unit is measured: a modulated signal only at the points where its law input is at its middle
        // (around which it moves); every other unit.
        bool runs(int unit) const;
    };

    // The plan's matrix (report §2): the axes are the pots the region and the blocks before it read, at 0, 0.5 and 1,
    // the switches they read, at each position, and the law inputs, at their low end, middle (geometric; arithmetic
    // when the range starts at 0) and high end (with three axes or fewer the full grid; otherwise each alone, the
    // others at their defaults (a law input at its middle), then all low and all high); sines at L/15, L/4.5, L/2.25, L and 2.2 L on the shape's bins, a 20 Hz-20
    // kHz log sweep and band-limited noise at L; with law inputs, a synth's too: a band-limited saw at L on the first
    // bin (105 Hz), and for each law input a 996 Hz sine at L with that input moving +-2 octaves at 37 Hz around its
    // middle (linearly over its range when it starts at 0). L is .target level, else .input's scale (full scale).
    StressMatrix stressMatrix(const Plan& plan, const StressShape& shape = {});

    // A unit's signal at m times the host rate, n samples.
    std::vector<double> signalSamples(const StressMatrix& matrix, const StressSignal& signal, int m, std::size_t n);

    // A unit's law inputs at m times the host rate, n samples, each host sample's value held over its m: law L of
    // sample i at [i * laws + L] (the runners' law_ buffer). The point's values, or the modulated input's motion.
    std::vector<double> lawSamples(const Plan& plan, const StressMatrix& matrix, int unit, int m, std::size_t n);

    // The same as tick's and step's arguments: each law input's samples over its .input scale, which the module
    // multiplies back before its clamp. lawSamples' values are in the range's units (amps for kind=current), which
    // gains and the references read; a module fed them raw would compute scale times them.
    std::vector<double> lawArguments(const Plan& plan, const StressMatrix& matrix, int unit, int m, std::size_t n);

    // What stops a reference: polled every 4096 steps.
    using Stop = std::function<bool()>;

    // What the region sees of a unit's signal, at `oversample` times the host's rate (oversample x the shape's host
    // samples): the signal through the blocks before the region (linear: trapezoidal, in double, designed at that
    // rate), so at the level and with the pots the region meets; the signal itself when the region is the chain's
    // first block. T1's reference and the candidate's step both run on it. *stopped is set when `stop` stopped it.
    std::vector<double> regionInput(const Plan& plan, const StressMatrix& matrix, int unit, int oversample,
                                    const Stop& stop, bool* stopped = nullptr);

    // A unit's T2 and T3 reference: the converged 64x chain (every block, trapezoidal, no ADAA): its fundamental's and
    // third harmonic's amplitudes over the window (T3), and the energy of its bins below 20 kHz, decimated to the
    // host's rate (T2 counts the tick's inharmonic energy in excess of it: what the linear blocks' transients put
    // there, such as a 1.6 Hz output coupling's still settling in the window, is not an alias).
    struct Reference64
    {
        bool done = false;
        double h1 = 0;
        double h3 = 0;
        std::vector<double> power;
        long unconverged = 0;
    };

    Reference64 reference64(const Plan& plan, const StressMatrix& matrix, int unit, const Stop& stop);

    // A unit's T1 reference: the region's blocks in double at the candidate's rate, ADAA and integrator, solved to
    // convergence, on the unit's regionInput, decimated to the host's window.
    struct ReferenceT1
    {
        bool done = false;
        std::vector<double> window;
        long unconverged = 0;
    };

    ReferenceT1 referenceT1(const Plan& plan, const StressMatrix& matrix, int unit, int oversample, bool adaa,
                            Method method, const Stop& stop);

    // A unit's gates for one candidate, from what its module computed: `steps`, the region's step over the signal at
    // the region's rate; `ticks`, tick over it at the host's.
    struct UnitVerdict
    {
        bool ran = false;
        double t1 = -std::numeric_limits<double>::infinity();
        double t2 = -std::numeric_limits<double>::infinity();
        double t3 = -std::numeric_limits<double>::infinity();
        std::string t3Which;   // "H1" or "H3"
        bool finite = true;
    };

    UnitVerdict measureUnit(const StressMatrix& matrix, int unit, int oversample, std::span<const double> steps,
                            std::span<const double> ticks, const ReferenceT1* t1, const Reference64* t3);

    // A gate's worst value and where it fell, over the units that measured it.
    struct GateWorst
    {
        double value = -std::numeric_limits<double>::infinity();
        std::string where;
        int measured = 0;   // the units that gave it a value

        void max(double v, const std::string& at)
        {
            ++measured;
            if (v > value)
            {
                value = v;
                where = at;
            }
        }
    };

    // A candidate's verdict over the matrix. A gate passes only when some unit measured it: a matrix that measures
    // nothing (every output silent) passes nothing.
    struct Verdict
    {
        GateWorst t1;
        GateWorst t2;
        GateWorst t3;
        bool finite = true;
        std::string notFinite;   // the first unit that was not
        long unconverged = 0;    // the references' samples that ran out of Newton steps

        bool passes() const;
        // How far it misses: the largest of t1 + 60, t2 + 60 and t3 - 0.5 (negative when it passes those; infinite
        // when a gate was measured nowhere).
        double excess() const;
        // The worst gate and where: "T2 -55.1 dB at 2.2 V, 3012 Hz, drive 1"; "T4 not finite at ...".
        std::string worst() const;
        // "T1 -93.1 dB, T2 -79.0 dB, T3 0.10 dB"
        std::string figures() const;
    };

    // The units' verdicts reduced in index order (so the same for any thread count), with the references' counts. A
    // gate's NaN (nothing to measure: the output and its reference both silent, a pot at a muting end) is no
    // measurement; its +inf (one of them silent, the other not) is the worst.
    Verdict reduce(const StressMatrix& matrix, std::span<const UnitVerdict> units, long unconverged);

    // A candidate configuration of the open dimensions, with its model cost.
    struct Candidate
    {
        int oversample = 8;
        bool adaa = false;
        Solver solver;
        Method method = Method::Tr;
        double cost = 0;   // model cycles a host sample (report §3.2)

        // "4x, ADAA, omega(1)"
        std::string text() const;
    };

    // The report's §3.2 model, in cycles a host sample: each region block times its rate (a port block: p, 10 + 2 a
    // state (a capacitor's or an inductor's); the guess, omega 65 or the previous 5; each Newton step, 70 for a diode
    // pair or one diode, 110 for an asymmetric pair (two exponentials), 60 for an OTA's tanh; ADAA's tail, 57 for an
    // exp-affine law, 100 for tanh's log cosh; the state update, 5. A linear block: 5 + 3 a state; with BDF2, a region
    // block's 2 a state more, its second history), each block outside the region at 1x, and 12 for each sample a
    // halfband stage runs at its high rate, up and down together.
    double modelCost(const Plan& plan, int oversample, bool adaa, const Solver& solver, Method method = Method::Tr);

    // The candidates of the plan's open dimensions (report §6.2: an exp-affine port's 1, 2, 4 and 8x, ADAA off and on,
    // omega3 + 1 or 2 Newton steps; a smooth-monotone port's (an OTA's) 1, 2 and 4x, ADAA off and on, the previous
    // step's v + 1, 2 or 3), the pinned ones at the plan's values, cheapest first by modelCost; candidates within 5 %
    // of the cheapest of their group go ADAA first, then fewer steps.
    std::vector<Candidate> candidates(const Plan& plan);

    // candidates()'s order on any list: by cost, then within 5 % of the cheapest of a group, ADAA first, then fewer
    // Newton steps (stable: an equal pair keeps its order).
    void orderCandidates(std::vector<Candidate>& candidates);

    // The search's record: each candidate measured, in order, and the answer.
    struct Tried
    {
        Candidate candidate;
        Verdict verdict;
    };

    struct SearchOutcome
    {
        std::vector<Tried> tried;
        int chosen = -1;        // into tried: the first that passes, or noConfig's; -1: none
        bool met = false;       // it passes
        bool stopped = false;   // measure or cancelled stopped the search
    };

    // The ordered search: measures candidates in order (nothing when `cancelled` is true before one), returns the
    // first that passes, and measures no more unless `every`. When none passes, noConfig's choice, met false. A
    // `measure` that returns nothing stops the search (cancelled, or over budget).
    SearchOutcome search(const std::vector<Candidate>& candidates,
                         const std::function<std::optional<Verdict>(const Candidate&)>& measure,
                         const std::function<bool()>& cancelled, bool every);

    // When nothing passes (report §6.5): the candidate with the least excess among those under kCostCap that are
    // finite everywhere, ties to the higher oversampling; -1 when there is none.
    int noConfig(std::span<const Tried> tried);

    // The answer as a Choice.
    Choice choiceOf(const std::string& key, const Candidate& candidate, const Verdict& verdict, bool met);

    // The directives that pin a choice's open dimensions: ".oversample 4, .adaa on, .solver omega(1)" (the integrator
    // only when the answer is BDF2 or the netlist wrote .method auto: left out, it is the trapezoidal rule).
    std::string directivesOf(const Config& open, const Choice& choice);

    // The steps a stage takes: the 64x references (a unit T2 reads); one configuration's T1 references; one
    // candidate's runs (its steps and its ticks, each oversample x the host's samples, and with blocks before the
    // region, the steps' regionInput).
    std::int64_t steps64(const StressMatrix& matrix);
    std::int64_t stepsT1(const StressMatrix& matrix, int oversample);
    std::int64_t stepsCandidate(const StressMatrix& matrix, int oversample);
} // namespace fminus::circuit
