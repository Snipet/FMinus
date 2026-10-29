// src/circuit/Reference.h: a block in Holters' form (Mna.h) simulated in double, the reference the checks compare the
// emitted float code against. It is adaa.cpp's run<double> (docs/research/analog-circuits/experiments/), for N states
// and at most one port: solved to convergence from the closed-form guess (report §9.7's fix), or with the emitted
// algorithm's fixed steps, plain or with ADAA. Its matrices are holters(): the derivation the emitted code computes
// in float, run in double.
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

    // The block's output for the input u, every sample at the rate it was designed for (fs', or 3/2 fs' with ADAA).
    std::vector<double> simulate(const Holters& block, std::span<const double> u, const RefOptions& options,
                                 RefStats* stats = nullptr);

    // Blocks in a chain, each one's output the next one's input, all at one rate.
    std::vector<double> simulateChain(std::span<const Holters> blocks, std::span<const double> u,
                                      const RefOptions& options, RefStats* stats = nullptr);
} // namespace fminus::circuit
