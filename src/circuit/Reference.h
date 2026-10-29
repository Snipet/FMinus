// src/circuit/Reference.h: a block in Holters' form (Mna.h) simulated in double, the reference the checks compare the
// emitted float code against. It is adaa.cpp's run<double> (docs/research/analog-circuits/experiments/), for N states
// and at most one port of either law (Laws.h): solved to convergence (a diode pair's from the closed-form guess, report
// §9.7's fix; an OTA's by bracketed Newton from the previous step's v), or with the emitted algorithm's fixed steps,
// plain or with ADAA (an OTA's with the emitted carry when its gain moves). An OTA's gain is the block's Holters::g,
// or a stream of one a sample. Its matrices are holters(): the derivation the emitted code computes in float, run in
// double.
#pragma once

#include "circuit/Mna.h"

#include <functional>
#include <span>
#include <vector>

namespace fminus::circuit
{
    struct RefOptions
    {
        bool adaa = false;   // Holters' first-order ADAA, with the closed-form F
        int steps = -1;      // -1: solved to convergence; otherwise the emitted Newton steps
        Solver::Guess guess = Solver::Guess::Omega;
        double eps = 1e-5;   // ADAA's |dp| below which the mean is the midpoint (volts)
        // ADAA, an OTA's: the stored point carried onto each step's curve when kd g moves, as the emitted step does.
        // false leaves it on the last step's curve: a check's control, to measure what the carry does.
        bool carry = true;
        // Polled every 4096 steps: true stops the run where it is (the rest of the output is 0, and stats->stopped
        // is set). Empty: never.
        std::function<bool()> stop;
    };

    struct RefStats
    {
        long unconverged = 0;   // samples whose converged solve ran out of steps (every check wants 0)
        long samples = 0;       // the steps run (each block's, in a chain)
        int maxIterations = 0;
        bool stopped = false;   // RefOptions::stop stopped a run
    };

    // omega3's guess for v + k sinh(v / nvt) = p (adaa.cpp's omegaGuess).
    double omegaGuess(double p, double k, double nvt);

    // The port's solution, to convergence: from omega3, at most 300 Newton steps in the bracket, until
    // |dv| < 1e-16 + 1e-15 |v|.
    double converged(double p, double k, double nvt, RefStats* stats = nullptr);

    // The block's output for the input u, every sample at the rate it was designed for (fs', or 3/2 fs' with ADAA);
    // an OTA's gain is gains[n] at sample n when `gains` is not empty (as long as u), else block.g.
    std::vector<double> simulate(const Holters& block, std::span<const double> u, const RefOptions& options,
                                 RefStats* stats = nullptr, std::span<const double> gains = {});

    // Blocks in a chain, each one's output the next one's input, all at one rate; block b's OTA gain is gains[b][n]
    // where gains[b] is not empty.
    std::vector<double> simulateChain(std::span<const Holters> blocks, std::span<const double> u,
                                      const RefOptions& options, RefStats* stats = nullptr,
                                      std::span<const std::vector<double>> gains = {});
} // namespace fminus::circuit
