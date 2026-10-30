// src/circuit/Analysis.cpp: the measurements (Analysis.h), as adaa.cpp computes them.
#include "circuit/Analysis.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <numbers>
#include <stdexcept>

namespace fminus::circuit
{
    using Complex = std::complex<double>;

    void fft(std::vector<Complex>& a)
    {
        const std::size_t n = a.size();
        if (n == 0 || (n & (n - 1)) != 0)
            throw std::invalid_argument("fft: the size is not a power of two");
        for (std::size_t i = 1, j = 0; i < n; ++i)
        {
            std::size_t bit = n >> 1;
            for (; (j & bit) != 0; bit >>= 1)
                j ^= bit;
            j ^= bit;
            if (i < j)
                std::swap(a[i], a[j]);
        }
        for (std::size_t len = 2; len <= n; len <<= 1)
        {
            const double angle = -2 * std::numbers::pi / static_cast<double>(len);
            for (std::size_t i = 0; i < n; i += len)
            {
                for (std::size_t j = 0; j < len / 2; ++j)
                {
                    // the twiddle directly (no running product), so a long transform keeps its accuracy
                    const Complex w = std::polar(1.0, angle * static_cast<double>(j));
                    const Complex u = a[i + j];
                    const Complex v = a[i + j + len / 2] * w;
                    a[i + j] = u + v;
                    a[i + j + len / 2] = u - v;
                }
            }
        }
    }

    std::vector<double> decimateWindow(std::span<const double> x, int m, std::size_t first, std::size_t count)
    {
        std::vector<double> y(count);
        if (m == 1)
        {
            for (std::size_t o = 0; o < count; ++o)
                y[o] = first + o < x.size() ? x[first + o] : 0;
            return y;
        }
        const int half = 48 * m;
        std::vector<double> h(static_cast<std::size_t>(2 * half + 1));
        const double fc = 0.5 / m * 0.92;
        double sum = 0;
        for (int i = -half; i <= half; ++i)
        {
            const double t = i;
            const double sinc = i == 0 ? 2 * fc : std::sin(2 * std::numbers::pi * fc * t) / (std::numbers::pi * t);
            const double u = static_cast<double>(i + half) / (2 * half);
            const double w = 0.35875 - 0.48829 * std::cos(2 * std::numbers::pi * u) +
                             0.14128 * std::cos(4 * std::numbers::pi * u) -
                             0.01168 * std::cos(6 * std::numbers::pi * u);
            h[static_cast<std::size_t>(i + half)] = sinc * w;
            sum += sinc * w;
        }
        for (double& v : h)
            v /= sum;
        for (std::size_t o = 0; o < count; ++o)
        {
            const long c = static_cast<long>(first + o) * m;
            double acc = 0;
            for (int i = -half; i <= half; ++i)
            {
                const long j = c - i;
                if (j >= 0 && j < static_cast<long>(x.size()))
                    acc += h[static_cast<std::size_t>(i + half)] * x[static_cast<std::size_t>(j)];
            }
            y[o] = acc;
        }
        return y;
    }

    namespace
    {
        std::vector<Complex> spectrum(std::span<const double> x, bool hann)
        {
            std::vector<Complex> a(x.size());
            const double n = static_cast<double>(x.size());
            for (std::size_t i = 0; i < x.size(); ++i)
            {
                const double w = hann ? 0.5 - 0.5 * std::cos(2 * std::numbers::pi * static_cast<double>(i) / n) : 1.0;
                a[i] = x[i] * w;
            }
            fft(a);
            return a;
        }

        std::size_t topBin(std::size_t n)
        {
            return static_cast<std::size_t>(20000.0 / kHostRate * static_cast<double>(n));
        }
    } // namespace

    double bandErrorDb(std::span<const double> a, std::span<const double> ref, double delay, bool hann)
    {
        if (a.size() != ref.size())
            throw std::invalid_argument("bandErrorDb: the windows differ in length");
        const std::vector<Complex> A = spectrum(a, hann);
        const std::vector<Complex> R = spectrum(ref, hann);
        const std::size_t n = a.size();
        double ee = 0;
        double rr = 0;
        for (std::size_t b = 1; b <= topBin(n); ++b)
        {
            const Complex shift =
                std::polar(1.0, -2 * std::numbers::pi * static_cast<double>(b) * delay / static_cast<double>(n));
            ee += std::norm(A[b] - R[b] * shift);
            rr += std::norm(R[b]);
        }
        return 10 * std::log10(ee / rr + 1e-40);
    }

    double inharmonicDb(std::span<const double> y, int bin)
    {
        const std::vector<Complex> a = spectrum(y, false);
        double harmonic = 0;
        double other = 0;
        for (std::size_t b = 1; b <= topBin(y.size()); ++b)
        {
            const double e = std::norm(a[b]);
            if (b % static_cast<std::size_t>(bin) == 0)
                harmonic += e;
            else
                other += e;
        }
        return 10 * std::log10(other / (harmonic + other) + 1e-40);
    }

    std::vector<double> bandPower(std::span<const double> y)
    {
        const std::vector<Complex> a = spectrum(y, false);
        std::vector<double> out(topBin(y.size()) + 1);
        for (std::size_t b = 0; b < out.size(); ++b)
            out[b] = std::norm(a[b]);
        return out;
    }

    double inharmonicExcessDb(std::span<const double> y, std::span<const double> ref, int bin)
    {
        const std::vector<Complex> a = spectrum(y, false);
        const std::size_t top = topBin(y.size());
        if (ref.size() != top + 1)
            throw std::invalid_argument("inharmonicExcessDb: the reference's bins differ");
        double all = 0;
        double other = 0;
        for (std::size_t b = 1; b <= top; ++b)
        {
            const double e = std::norm(a[b]);
            all += e;
            if (b % static_cast<std::size_t>(bin) != 0)
                other += std::max(0.0, e - ref[b]);
        }
        return 10 * std::log10(other / all + 1e-40);
    }

    void harmonicDiffDb(std::span<const double> y, std::span<const double> ref, int bin, double* h1, double* h3)
    {
        const std::vector<Complex> a = spectrum(y, false);
        const std::vector<Complex> r = spectrum(ref, false);
        const auto k = static_cast<std::size_t>(bin);
        *h1 = 20 * std::log10(std::abs(a[k]) / std::abs(r[k]));
        *h3 = 20 * std::log10(std::abs(a[3 * k]) / std::abs(r[3 * k]) + 1e-30);
    }

    double binMagnitude(std::span<const double> y, double cycles)
    {
        // one DFT bin by direct summation: `cycles` periods over the window
        Complex acc = 0;
        const double n = static_cast<double>(y.size());
        for (std::size_t i = 0; i < y.size(); ++i)
            acc += y[i] * std::polar(1.0, -2 * std::numbers::pi * cycles * static_cast<double>(i) / n);
        return std::abs(acc) * 2 / n;
    }

    std::vector<double> sine(double amp, int bin, std::size_t window, int m, std::size_t n)
    {
        std::vector<double> s(n);
        const double f = static_cast<double>(bin) * kHostRate / static_cast<double>(window);
        for (std::size_t i = 0; i < n; ++i)
            s[i] = amp * std::sin(2 * std::numbers::pi * f * static_cast<double>(i) / (kHostRate * m));
        return s;
    }

    std::vector<double> sweep(double amp, int m, std::size_t n)
    {
        std::vector<double> s(n);
        const double f0 = 20;
        const double f1 = 20000;
        const double t = static_cast<double>(n) / (kHostRate * m);
        const double k = std::log(f1 / f0);
        for (std::size_t i = 0; i < n; ++i)
        {
            const double time = static_cast<double>(i) / (kHostRate * m);
            const double phase = 2 * std::numbers::pi * f0 * t / k * (std::exp(time / t * k) - 1);
            s[i] = amp * std::sin(phase);
        }
        return s;
    }

    std::vector<double> saw(double amp, int bin, std::size_t window, int m, std::size_t n)
    {
        // one period of the window, by an inverse FFT of its harmonics (sin(k w t) is a / 2j on bin k b, its mirror
        // the conjugate), then repeated
        const std::size_t period = window * static_cast<std::size_t>(m);
        std::vector<Complex> bins(period, Complex(0));
        const double f0 = static_cast<double>(bin) * kHostRate / static_cast<double>(window);
        for (int k = 1; k * f0 < 20000 && static_cast<std::size_t>(k * bin) < period / 2; ++k)
        {
            const double a = 2 * amp / std::numbers::pi * (k % 2 == 1 ? 1.0 : -1.0) / k;
            const Complex c(0, -0.5 * a);   // a / 2j
            bins[static_cast<std::size_t>(k * bin)] = c;
            bins[period - static_cast<std::size_t>(k * bin)] = std::conj(c);
        }
        for (Complex& c : bins)
            c = std::conj(c);
        fft(bins);
        std::vector<double> s(n);
        for (std::size_t i = 0; i < n; ++i)
            s[i] = bins[i % period].real();
        return s;
    }

    std::vector<double> noise(double amp, int m, std::size_t n, unsigned seed)
    {
        // random phases, flat amplitude, on every bin below 20 kHz: an inverse FFT, then scaled to the peak
        std::vector<Complex> bins(n, Complex(0));
        std::uint64_t state = 0x9E3779B97F4A7C15ull ^ seed;
        auto next = [&]
        {
            state = state * 6364136223846793005ull + 1442695040888963407ull;
            return static_cast<double>(state >> 11) / 9007199254740992.0;
        };
        const auto top = static_cast<std::size_t>(20000.0 / (kHostRate * m) * static_cast<double>(n));
        for (std::size_t b = 1; b <= top && b < n / 2; ++b)
        {
            const Complex c = std::polar(1.0, 2 * std::numbers::pi * next());
            bins[b] = c;
            bins[n - b] = std::conj(c);
        }
        // the inverse transform as the forward one of the conjugate
        for (Complex& c : bins)
            c = std::conj(c);
        fft(bins);
        std::vector<double> s(n);
        double peak = 0;
        for (std::size_t i = 0; i < n; ++i)
        {
            s[i] = bins[i].real();
            peak = std::fmax(peak, std::fabs(s[i]));
        }
        for (double& v : s)
            v *= amp / peak;
        return s;
    }
} // namespace fminus::circuit
