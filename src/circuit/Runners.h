// src/circuit/Runners.h: FMinus functions that drive a generated module over buffers, a call a run (testing.md's rule),
// appended to its source by the self-test (src/api/CircuitSelfTest.cpp) and by examples/circuits: fm_set (set() from
// buffers of pots and switch positions), fm_steps (the region's step over a buffer) and fm_ticks (tick over a buffer,
// with set() on the absolute 16-sample grid from a pot buffer). Their parameters end in '_', which no module, pot or
// input name does, so they shadow nothing.
#pragma once

#include <string>

namespace fminus::circuit
{
    // The module the runners drive: its name and the shape of its methods.
    struct RunnerShape
    {
        std::string module;   // Result::module
        int pots = 0;         // set()'s floats (Result::pots)
        int switches = 0;     // and its ints, after them (Result::switches)
        int laws = 0;         // tick's and step's floats after the signal (Result::inputs, but the first)
        int channels = 1;     // 2: tick and step on float2s
        bool step = true;     // the module has a step() (an oversampled region)
    };

    // FMinus text, every function [[clang::nonblocking]]:
    //   void fm_set(M* c_, const float* pots_, const int* switches_)        set(pots_[0..], switches_[0..])
    //   void fm_steps(M* c_, const float* u_, const float* law_, float* y_, int n_)
    //        y_[i] = step(u_[i], law_[i L + 0], ...)                        (with a region and one channel)
    //   void fm_ticks(M* c_, const float* u_, const float* law_, const float* pots_, const int* switches_, float* y_,
    //                 int n_, int start_)
    //        set(pots_[i P ..], switches_[i S ..]) where (start_ + i) % 16 == 0, then y_[i] = tick(u_[i], law_[i L ..])
    //   void fm_ticks2(M* c_, const float* a_, const float* b_, const float* law_, const float* pots_,
    //                  const int* switches_, float* ya_, float* yb_, int n_, int start_)
    //        the same on (a_[i], b_[i]) as a float2 (.channels 2, instead of fm_ticks; the law inputs broadcast)
    // law_ and switches_ are read only when the module has law inputs or switches (null otherwise).
    std::string runnerSource(const RunnerShape& shape);
} // namespace fminus::circuit
