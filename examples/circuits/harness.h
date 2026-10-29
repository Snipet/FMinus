// examples/circuits/harness.h: what example-circuits' three modes (--check, --tables, the cost table) share: the
// example's netlists, a generated module compiled through fminus::Program with runners appended (fm_steps, the
// oversampled region's step over a buffer; fm_ticks, tick over a buffer with set() on the absolute 16-sample grid from
// a pot buffer: one call a run, testing.md's rule), and the double reference of a plan's blocks
// (src/circuit/Reference.h).
#pragma once

#include "circuit/Analysis.h"
#include "circuit/Mna.h"
#include "circuit/Plan.h"
#include "circuit/Reference.h"
#include "fminus/Circuit.h"
#include "fminus/FMinus.h"

#include <cstdint>
#include <memory>
#include <span>
#include <string>
#include <vector>

namespace circuits
{
    namespace fc = fminus::circuit;

    // A .cir file of this example's directory (FMINUS_EXAMPLE_DIR).
    std::string netlist(const std::string& name);

    // `text` with its line that starts with `directive` (".adaa") replaced by `line`, or `line` added before .end.
    std::string withDirective(std::string text, const std::string& directive, const std::string& line);

    // `text` with its .model card's N= replaced (1.75140653 gives the experiments' 45.3 mV, 1.73980781 their 45.0 mV).
    std::string withN(std::string text, const std::string& n);

    // An instance of a module, aligned and zeroed, as a host makes one.
    class Instance
    {
    public:
        explicit Instance(const fminus::Program::Module& module);
        ~Instance();
        Instance(const Instance&) = delete;
        Instance& operator=(const Instance&) = delete;

        void* get() const
        {
            return memory_;
        }

    private:
        void* memory_ = nullptr;
    };

    // A generated module, compiled.
    struct Circuit
    {
        std::string fileName;
        fc::Result result;
        fc::Plan plan;
        std::unique_ptr<fminus::Program> program;
        const fminus::Program::Module* module = nullptr;
        void (*init)(void*, float) = nullptr;
        void (*clear)(void*) = nullptr;
        // fm_steps(c, u, y, n): y[i] = step(u[i]) (absent without a region, or with .channels 2)
        void (*steps)(void*, const float*, float*, int) = nullptr;
        // fm_ticks(c, u, pots, y, n, start): set(pots[i P ...]) where (start + i) % 16 == 0, then y[i] = tick(u[i])
        void (*ticks)(void*, const float*, const float*, float*, int, int) = nullptr;
        // fm_ticks2(c, a, b, pots, ya, yb, n, start): the same on (a[i], b[i]) as a float2 (.channels 2)
        void (*ticks2)(void*, const float*, const float*, const float*, float*, float*, int, int) = nullptr;
        std::uintptr_t set = 0;

        int pots() const
        {
            return static_cast<int>(plan.pots.size());
        }

        // set(pots...), with 0 to 3 pots.
        void callSet(void* self, std::span<const double> pots) const;
    };

    // `text` generated with `options` (its errors, warnings and FMinus's in `errors`) and compiled in `math`, with
    // subnormals flushed and one thread (the report's settings); `extra` is FMinus appended to it (a bench's).
    std::unique_ptr<Circuit> build(const std::string& text, const std::string& fileName, const fc::Options& options,
                                   std::string& errors, fminus::Program::Math math = fminus::Program::Math::Vector,
                                   const std::string& extra = {});

    // The region's step over u, at the region's rate, from init(48000) and set(pots).
    std::vector<double> runSteps(const Circuit& c, std::span<const double> u, std::span<const double> pots);

    // tick over u at 48 kHz from init(48000), the pots held (set on the grid), in calls of `call` samples (0: one
    // call).
    std::vector<double> runTicks(const Circuit& c, std::span<const double> u, std::span<const double> pots,
                                 int call = 0);

    // The same with the pots a buffer, pots() values a sample.
    std::vector<double> runTicksWith(const Circuit& c, std::span<const double> u, std::span<const float> potBuffer,
                                     int call = 0);

    // A point of pots (plan.pots' order) with `key` at `value` and the others at their defaults.
    std::vector<double> potsWith(const fc::Plan& plan, const std::string& key, double value);

    // The plan's blocks in Holters' form at `rate` (every block there, as a reference chain does), ADAA's 2/3 for a
    // port block when `adaa`.
    std::vector<fc::Holters> chainAt(const fc::Plan& plan, std::span<const double> pots, double rate, bool adaa);

    // dB of an amplitude ratio.
    double dB(double ratio);

    // Whether every value is finite.
    bool finite(std::span<const double> y);

    // The three modes (check.cpp, tables.cpp, bench.cpp): each returns the process's exit status.
    int check();
    int tables();

    struct BenchOptions
    {
        bool all = false;   // mkbench.py's whole matrix
        std::string only;   // a regex of rows
        fminus::Program::Math math = fminus::Program::Math::Vector;
        int reps = 7;
        int samples = 1000000;
    };

    int bench(const BenchOptions& options);
} // namespace circuits
