// src/circuit/Halfbands.h: the oversampler's 2x halfband stages, S1 (48 <-> 96 kHz), S2 (96 <-> 192) and S3
// (192 <-> 384), as docs/research/analog-circuits/experiments/gen.cpp designed them (hb-coefs.txt): Kaiser-windowed
// FIRs and polyphase-allpass IIRs (HIIR's design), their responses, and the FMinus that runs them (mkbench.py's
// fir() and iir(), as methods on prefixed fields). The coefficients are embedded; designing them is Phase 1.
#pragma once

#include "fminus/Circuit.h"

#include <complex>
#include <span>
#include <string>

namespace fminus::circuit
{
    // Stage s's (1 to 3) FIR odd taps h[1], h[3], ... as 2h (the interpolator's gain); h[0] is 0.5.
    std::span<const double> firTaps(int stage);

    // Stage s's IIR allpass coefficients: even ones on the first branch, odd on the second.
    std::span<const double> iirCoefficients(int stage);

    // The decimator's response at w, radians a sample at the stage's high rate: 0.5 (A0(z^2) + z^-1 A1(z^2)) for the
    // IIR, 0.5 + sum 2h cos((2j + 1) w) for the FIR (zero phase).
    std::complex<double> iirResponse(int stage, double w);
    double firResponse(int stage, double w);

    // Up then down through stages 1 to log2(oversample) (an IIR's group delay at `freq`, a FIR's exact delay), in
    // samples of a 48 kHz host.
    double roundTripDelay(int oversample, Halfband kind, double freq);

    // FMinus: stage s's fields (hb<s>u_*, hb<s>d_*), the statements of clear() that zero them, and its methods
    // hb<s>_up(x, o) (one sample in, two out) and hb<s>_down(v0, v1) (two in, one out), on `type` (float, float2).
    void emitHalfbandFields(std::string& out, int stage, Halfband kind, const std::string& type);
    void emitHalfbandClear(std::string& out, int stage, Halfband kind);
    void emitHalfbandMethods(std::string& out, int stage, Halfband kind, const std::string& type);
} // namespace fminus::circuit
