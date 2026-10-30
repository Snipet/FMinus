// src/circuit/Reference.cpp: the double reference (Reference.h), a port of adaa.cpp's run<double>.
#include "circuit/Reference.h"

#include "circuit/Laws.h"

#include <algorithm>
#include <cmath>
#include <cstddef>

namespace fminus::circuit
{
    namespace
    {
        double omega3(double x)
        {
            const double x1 = -3.341459552768620;
            const double x2 = 8.0;
            const double a = -1.314293149877800e-3;
            const double b = 4.775931364975583e-2;
            const double c = 3.631952663804445e-1;
            const double d = 6.313183464296682e-1;
            if (x <= x1)
                return 0;
            if (x < x2)
                return ((a * x + b) * x + c) * x + d;
            return x - std::log(x);
        }

        double newtonSteps(double p, double k, double nvt, double guess, int steps)
        {
            const double lo = std::fmin(0.0, p);
            const double hi = std::fmax(0.0, p);
            double v = std::fmin(std::fmax(guess, lo), hi);
            for (int i = 0; i < steps; ++i)
            {
                const double e = std::exp(v / nvt);
                const double ei = 1 / e;
                const double f = v - p + k * 0.5 * (e - ei);
                const double d = 1 + (k / nvt) * 0.5 * (e + ei);
                v = std::fmin(std::fmax(v - f / d, lo), hi);
            }
            return v;
        }

        // One block's state through a run.
        struct Runner
        {
            const Holters& h;
            RefOptions o;
            RefStats* stats;
            std::vector<double> x1;   // x(n-1)
            std::vector<double> x2;   // x(n-2)
            std::vector<double> next;
            double u1 = 0;
            double p1 = 0;
            double v1 = 0;
            double c1 = 1;    // cosh(v(n-1) / nvt)
            double t1 = 0;    // an OTA's tanh(v(n-1) / 2 Vt)
            double lc1 = 0;   // and log cosh
            double kg1 = 0;   // its k g at n-1
            double g1 = 0;    // one diode's or an asymmetric pair's G(v(n-1)), the law's antiderivative

            Runner(const Holters& block, const RefOptions& options, RefStats* s)
                : h(block)
                , o(options)
                , stats(s)
                , x1(static_cast<std::size_t>(block.states), 0.0)
                , x2(static_cast<std::size_t>(block.states), 0.0)
                , next(static_cast<std::size_t>(block.states), 0.0)
            {
                if (!h.port)
                    o.adaa = false;
                if (h.port && (h.law == PortDef::Law::Single || h.law == PortDef::Law::Asym))
                    g1 = lawAntiderivative(lawShape(h, 1), 0);
            }

            double solve(double p, double kg)
            {
                if (h.law == PortDef::Law::Single || h.law == PortDef::Law::Asym)
                {
                    const LawShape shape = lawShape(h, 1);
                    if (o.steps >= 0)
                        return lawStepsOf(shape, p, o.guess, v1, o.steps);
                    long unconverged = 0;
                    int iterations = 0;
                    const double v = lawConvergedOf(shape, p, v1, &unconverged, &iterations);
                    if (stats != nullptr)
                    {
                        stats->unconverged += unconverged;
                        stats->maxIterations = std::max(stats->maxIterations, iterations);
                    }
                    return v;
                }
                if (h.law == PortDef::Law::Tanh)
                {
                    if (o.steps >= 0)
                        return lawSteps(h.law, p, kg, h.nvt, Solver::Guess::Previous, v1, o.steps);
                    long unconverged = 0;
                    int iterations = 0;
                    const double v = lawConverged(h.law, p, kg, h.nvt, v1, &unconverged, &iterations);
                    if (stats != nullptr)
                    {
                        stats->unconverged += unconverged;
                        stats->maxIterations = std::max(stats->maxIterations, iterations);
                    }
                    return v;
                }
                if (o.steps < 0)
                    return converged(p, h.k, h.nvt, stats);
                const double g = o.guess == Solver::Guess::Omega ? omegaGuess(p, h.k, h.nvt) : v1;
                return newtonSteps(p, h.k, h.nvt, g, o.steps);
            }

            // An OTA's step: its law's scale this step, kd g (g the stream's, or the block's).
            double step(double u, double g)
            {
                if (h.law == PortDef::Law::Tanh)
                    return stepTanh(u, g);
                return step(u);
            }

            // An OTA's step, as the emitted code: v + kg tanh(v / 2 Vt) = p, kg = kd g, in the block's i-form (its
            // current i = g tanh(v / 2 Vt) the source); with ADAA, the carry of the last point onto this step's curve
            // (p1 += (kg - kg1) t1), then the mean of i over [p1, p], (pm - G) / kd with G the mean of v (report
            // §5.1's closed form, with log cosh).
            double stepTanh(double u, double g)
            {
                const double kg = h.k * g;
                const std::size_t n = x1.size();
                if (stats != nullptr)
                    ++stats->samples;
                double p = h.dp * u;
                for (std::size_t i = 0; i < n; ++i)
                    p += h.cp[i] * x1[i];
                const double v = solve(p, kg);
                if (!o.adaa)
                {
                    const double i0 = g * std::tanh(v / h.nvt);
                    double y = h.dyi * u + h.wyi * i0;
                    for (std::size_t i = 0; i < n; ++i)
                        y += h.cyi[i] * x1[i];
                    for (std::size_t i = 0; i < n; ++i)
                    {
                        double s = h.bi[i] * u + h.wi[i] * i0;
                        for (std::size_t j = 0; j < n; ++j)
                            s += h.ai[i][j] * x1[j];
                        next[i] = s;
                    }
                    x1.swap(next);
                    v1 = v;
                    return y;
                }
                if (o.carry)
                    p1 += (kg - kg1) * t1;
                const double lc = logCosh(v / h.nvt);
                const double dpv = p - p1;
                const double vm = 0.5 * (v + v1);
                const double pm = 0.5 * (p + p1);
                double vbar = vm;
                if (std::fabs(dpv) > o.eps)
                    vbar = vm + ((v - v1) * (pm - vm) - kg * h.nvt * (lc - lc1)) / dpv;
                const double ibar = (pm - vbar) / h.k;
                const double ua = 0.5 * (u + u1);
                double y = h.dyi * ua + h.wyi * ibar;
                for (std::size_t i = 0; i < n; ++i)
                    y += h.cyi[i] * 0.5 * (x1[i] + x2[i]);
                for (std::size_t i = 0; i < n; ++i)
                {
                    double s = h.bi[i] * ua + h.wi[i] * ibar;
                    for (std::size_t j = 0; j < n; ++j)
                        s += h.ai[i][j] * 0.5 * (x1[j] + x2[j]);
                    next[i] = s;
                }
                x2 = x1;
                x1.swap(next);
                u1 = u;
                p1 = p;
                v1 = v;
                t1 = std::tanh(v / h.nvt);
                lc1 = lc;
                kg1 = kg;
                return y;
            }

            double step(double u)
            {
                const std::size_t n = x1.size();
                if (stats != nullptr)
                    ++stats->samples;
                double v = 0;
                double p = 0;
                if (h.port)
                {
                    p = h.dp * u;
                    for (std::size_t i = 0; i < n; ++i)
                        p += h.cp[i] * x1[i];
                    v = solve(p, h.k);
                }
                if (!o.adaa)
                {
                    double y = h.dy * u + h.wy * v;
                    for (std::size_t i = 0; i < n; ++i)
                        y += h.cy[i] * x1[i];
                    for (std::size_t i = 0; i < n; ++i)
                    {
                        double s = h.b[i] * u + h.w[i] * v;
                        for (std::size_t j = 0; j < n; ++j)
                            s += h.a[i][j] * x1[j];
                        next[i] = s;
                    }
                    x1.swap(next);
                    v1 = v;
                    return y;
                }
                // first-order ADAA of the solved map, in the cancellation-free form:
                //   vbar = vm + [dv (pm - vm) - (G(v) - G(v1))] / dp, G the law's antiderivative (a pair's k nvt cosh)
                const bool pair = h.law == PortDef::Law::Pair;
                const double e = std::exp(v / h.nvt);
                const double c = 0.5 * (e + 1 / e);
                const double g = pair ? 0 : lawAntiderivative(lawShape(h, 1), v);
                const double dpv = p - p1;
                const double dv = v - v1;
                const double vm = 0.5 * (v + v1);
                const double pm = 0.5 * (p + p1);
                double vbar = vm;
                if (std::fabs(dpv) > o.eps)
                    vbar = vm + (dv * (pm - vm) - (pair ? h.k * h.nvt * (c - c1) : g - g1)) / dpv;
                const double ua = 0.5 * (u + u1);
                double y = h.dy * ua + h.wy * vbar;
                for (std::size_t i = 0; i < n; ++i)
                    y += h.cy[i] * 0.5 * (x1[i] + x2[i]);
                for (std::size_t i = 0; i < n; ++i)
                {
                    double s = h.b[i] * ua + h.w[i] * vbar;
                    for (std::size_t j = 0; j < n; ++j)
                        s += h.a[i][j] * 0.5 * (x1[j] + x2[j]);
                    next[i] = s;
                }
                x2 = x1;
                x1.swap(next);
                u1 = u;
                p1 = p;
                v1 = v;
                c1 = c;
                g1 = g;
                return y;
            }
        };
    } // namespace

    double omegaGuess(double p, double k, double nvt)
    {
        const double a = 0.5 * k;
        const double q = std::fabs(p);
        const double v = q + a - nvt * omega3((q + a) / nvt + std::log(a / nvt));
        return p < 0 ? -v : v;
    }

    double converged(double p, double k, double nvt, RefStats* stats)
    {
        const double lo = std::fmin(0.0, p);
        const double hi = std::fmax(0.0, p);
        double v = std::fmin(std::fmax(omegaGuess(p, k, nvt), lo), hi);
        for (int i = 0; i < 300; ++i)
        {
            const double e = std::exp(v / nvt);
            const double ei = 1 / e;
            const double f = v - p + k * 0.5 * (e - ei);
            const double d = 1 + (k / nvt) * 0.5 * (e + ei);
            const double nv = std::fmin(std::fmax(v - f / d, lo), hi);
            if (std::fabs(nv - v) < 1e-16 + 1e-15 * std::fabs(nv))
            {
                if (stats != nullptr)
                    stats->maxIterations = std::max(stats->maxIterations, i + 1);
                return nv;
            }
            v = nv;
        }
        if (stats != nullptr)
            ++stats->unconverged;
        return v;
    }

    std::vector<double> simulate(const Holters& block, std::span<const double> u, const RefOptions& options,
                                 RefStats* stats, std::span<const double> gains)
    {
        Runner r(block, options, stats);
        std::vector<double> out(u.size());
        for (std::size_t n = 0; n < u.size(); ++n)
        {
            if ((n & 4095) == 0 && options.stop && options.stop())
            {
                if (stats != nullptr)
                    stats->stopped = true;
                break;
            }
            out[n] = r.step(u[n], gains.empty() ? block.g : gains[n]);
        }
        return out;
    }

    std::vector<double> simulateChain(std::span<const Holters> blocks, std::span<const double> u,
                                      const RefOptions& options, RefStats* stats,
                                      std::span<const std::vector<double>> gains)
    {
        std::vector<Runner> runners;
        runners.reserve(blocks.size());
        for (const Holters& h : blocks)
            runners.emplace_back(h, options, stats);
        std::vector<double> out(u.size());
        for (std::size_t n = 0; n < u.size(); ++n)
        {
            if ((n & 4095) == 0 && options.stop && options.stop())
            {
                if (stats != nullptr)
                    stats->stopped = true;
                break;
            }
            double s = u[n];
            for (std::size_t b = 0; b < runners.size(); ++b)
            {
                const bool streamed = b < gains.size() && !gains[b].empty();
                s = runners[b].step(s, streamed ? gains[b][n] : blocks[b].g);
            }
            out[n] = s;
        }
        return out;
    }
} // namespace fminus::circuit
