// src/circuit/Analysis.h: the measurements of the report's targets (§2), as adaa.cpp made them: a radix-2 FFT, a long
// windowed-sinc decimator (so the measuring filter never limits the measurement), the in-band error of one signal
// against another, the inharmonic energy of a sine's response, its first and third harmonics, and the stress signals:
// sines on odd FFT bins (report §9.7), a band-limited saw on one, a log sweep and band-limited noise. A host rate of 48
// kHz throughout.
#pragma once

#include <complex>
#include <cstddef>
#include <span>
#include <vector>

namespace fminus::circuit
{
    inline constexpr double kHostRate = 48000;

    // In place; a.size() is a power of two.
    void fft(std::vector<std::complex<double>>& a);

    // x, at M times the host rate, decimated to it: outputs [first, first + count) of the Blackman-Harris windowed sinc
    // (half-length 48 M taps, cutoff 0.46 / M of the high rate), y[o] = sum h[i] x[o M - i], zero outside x.
    std::vector<double> decimateWindow(std::span<const double> x, int m, std::size_t first, std::size_t count);

    // The error of `a` against `ref` below 20 kHz, relative to ref's energy there, in dB: N-sample windows (N a power
    // of two) of host-rate signals, ref delayed by `delay` host samples (a phase shift: exact for periodic signals).
    // Rectangular for periodic signals; `hann` for a sweep or noise.
    double bandErrorDb(std::span<const double> a, std::span<const double> ref, double delay = 0, bool hann = false);

    // The energy below 20 kHz that is not on a harmonic of bin `bin`, relative to all the energy below 20 kHz, in dB.
    double inharmonicDb(std::span<const double> y, int bin);

    // The energy of y's bins below 20 kHz (|Y(b)|^2 for b = 0 to 20 kHz's bin), a rectangular window.
    std::vector<double> bandPower(std::span<const double> y);

    // inharmonicDb's energy in excess of a reference's, bin by bin: the sum over the bins below 20 kHz that are not on
    // a harmonic of `bin` of max(0, |Y(b)|^2 - ref[b]) (ref: bandPower of a reference of the same signal, which holds
    // what the circuit's linear transients put there), relative to all of y's energy below 20 kHz, in dB.
    double inharmonicExcessDb(std::span<const double> y, std::span<const double> ref, int bin);

    // y's first and third harmonics of bin `bin` against ref's, in dB.
    void harmonicDiffDb(std::span<const double> y, std::span<const double> ref, int bin, double* h1, double* h3);

    // The amplitude of the sinusoid of `cycles` periods a window in y: one DFT bin, summed directly (for windows that
    // are not a power of two long).
    double binMagnitude(std::span<const double> y, double cycles);

    // A sine of amplitude amp on bin `bin` of an N-point window at 48 kHz, sampled at M times that, n samples.
    std::vector<double> sine(double amp, int bin, std::size_t window, int m, std::size_t n);

    // A log sweep from 20 Hz to 20 kHz over n samples at M x 48 kHz, of amplitude amp.
    std::vector<double> sweep(double amp, int m, std::size_t n);

    // A band-limited saw on bin `bin` of an N-point window at 48 kHz, sampled at M times that, n samples: the series
    // (2 amp / pi) sum (-1)^(k+1) sin(k w t) / k over the harmonics below 20 kHz (an ideal saw of peak amp, band-limited;
    // its peak is about 1.09 amp), periodic in the window (N M a power of two).
    std::vector<double> saw(double amp, int bin, std::size_t window, int m, std::size_t n);

    // Noise of peak amp, band-limited to 20 kHz, n samples at M x 48 kHz (n a power of two; periodic in n).
    std::vector<double> noise(double amp, int m, std::size_t n, unsigned seed = 7);
} // namespace fminus::circuit
