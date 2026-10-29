// src/circuit/Mna.h: each block's equations, and one derivation over them for three number types. assemble() stamps a
// block's modified nodal analysis: node voltages, the currents of its variable resistors (the impedance form, so a
// pot's 0-ohm end is exact) and its port's current are the unknowns; the port itself is a voltage source v (Holters'
// v-form). choosePivots() fixes an elimination order at generation, exact +-1 entries first, checked at a grid of
// rates and pots. solve<T>() eliminates in that order and gives every readout (the port's current, the output, each
// capacitor's voltage) as a linear form in the block's sources (its input, the capacitors' states, v); derive<T>()
// turns those into the module's coefficients. T is double (the reference's matrices, holters()), std::complex
// (a linear block's analog response, analog()) or Val (Emit.h: the FMinus text that computes them in set or init),
// so the emitted float arithmetic is the reference's, operation for operation.
#pragma once

#include "circuit/Plan.h"

#include <complex>
#include <cstddef>
#include <optional>
#include <span>
#include <string>
#include <utility>
#include <vector>

namespace fminus::circuit
{
    // constant + sum of coef * symbol: an entry of a block's matrix or right-hand side.
    struct Affine
    {
        double constant = 0;
        std::vector<std::pair<int, double>> terms;   // (symbol, coef), in the order stamped

        bool isZero() const
        {
            return constant == 0 && terms.empty();
        }

        void add(double value)
        {
            constant += value;
        }

        void add(int symbol, double coef)
        {
            for (auto& [s, c] : terms)
            {
                if (s == symbol)
                {
                    c += coef;
                    return;
                }
            }
            terms.emplace_back(symbol, coef);
        }
    };

    struct System
    {
        // A value set or init computes: a capacitor's 2 C fs' (gc_C1), or a variable resistance (r_Ptone).
        struct Symbol
        {
            enum class Kind
            {
                Gc,
                R,
            };

            std::string name;
            Kind kind = Kind::Gc;
            int branch = -1;   // the block's branch it belongs to
        };

        // What drives the block: its input (a voltage), a capacitor's state (a current), the port's voltage v.
        struct Source
        {
            enum class Kind
            {
                Input,
                State,
                Port,
            };

            std::string name;   // "u" (the .input's name) or "n_p1"; "x_C1"; "v_D1"
            Kind kind = Kind::Input;
            int branch = -1;   // a state's capacitor
        };

        // A linear form in the unknowns and the sources: what the block reads out.
        struct Readout
        {
            enum class Kind
            {
                PortCurrent,   // j_D1
                Node,          // n_b: the block's output
                Capacitor,     // vc_C3
            };

            std::string name;
            Kind kind = Kind::Node;
            std::string label;                              // "V(b)", "V(n1) - V(v2)"
            std::vector<std::pair<int, double>> unknowns;   // (unknown, coef)
            std::vector<std::pair<int, double>> sources;    // (source, coef): a terminal on a source
        };

        int n = 0;                               // unknowns, and rows
        std::vector<std::string> unknownNames;   // "V(a)", "i(Ptone)", "j(D1|D2)"
        std::vector<std::string> rowNames;       // "KCL a", "XU1", "Ptone", "D1|D2"
        std::vector<std::vector<Affine>> a;      // [row][unknown]
        std::vector<Symbol> symbols;
        std::vector<Source> sources;            // the input, the states in netlist order, then v
        std::vector<std::vector<Affine>> rhs;   // [source][row]
        std::vector<Readout> readouts;
        int input = -1;                                  // the input's source index
        int port = -1;                                   // v's source index
        int portCurrent = -1;                            // j's readout
        int output = -1;                                 // the output's readout
        std::vector<std::pair<int, double>> capacitor;   // per state source: its voltage's readout, and a sign
    };

    // The block's equations (Plan.h's Block, by chain index).
    System assemble(const Plan& plan, int block);

    // The elimination order: (row, unknown) for each step.
    struct Pivots
    {
        std::vector<std::pair<int, int>> order;
    };

    // The order, chosen and checked over the block's grid of rates and pots; nothing (with an error) when the
    // equations are singular somewhere on it.
    std::optional<Pivots> choosePivots(const System& system, const Plan& plan, int block, Messages& messages);

    // Whether a port block's self-impedance K is positive (and finite) at every point of that grid, as the solver's
    // bracket [min(0, p), max(0, p)] and log(Is K / N Vt) need; otherwise an error (positive feedback makes K
    // negative). A block without a port passes.
    bool checkPort(const System& system, const Pivots& pivots, const Plan& plan, int block, Messages& messages);

    // The smallest |pivot| / max |its column| of the elimination in `pivots`, over the block's grid of rates and pots.
    double pivotMargin(const System& system, const Pivots& pivots, const Plan& plan, int block);

    // The values of the system's symbols at a design rate (fs', the rate the block's capacitors see) and pots.
    std::vector<double> symbolValues(const System& system, const Plan& plan, int block, double designRate,
                                     const std::vector<double>& pots);

    // ---- the derivation, for any T --------------------------------------------------------------------------------

    inline bool isZero(double x)
    {
        return x == 0;
    }

    inline bool isZero(const std::complex<double>& x)
    {
        return x == std::complex<double>(0);
    }

    // Where T is Val, these name a result in the text (a comment) and store it into a field; for numbers, nothing.
    inline void annotate(double&, const std::string&) {}

    inline void annotate(std::complex<double>&, const std::string&) {}

    inline double bindField(const std::string&, double value)
    {
        return value;
    }

    inline std::complex<double> bindField(const std::string&, std::complex<double> value)
    {
        return value;
    }

    inline double logOf(double x)
    {
        return std::log(x);
    }

    template <typename T>
    T fromAffine(const Affine& a, const std::vector<T>& symbols)
    {
        T acc = T(0.0);
        for (const auto& [s, coef] : a.terms)
        {
            const T term = coef == 1    ? symbols[static_cast<std::size_t>(s)]
                           : coef == -1 ? -symbols[static_cast<std::size_t>(s)]
                                        : T(coef) * symbols[static_cast<std::size_t>(s)];
            acc = isZero(acc) ? term : acc + term;
        }
        if (a.constant != 0)
            acc = isZero(acc) ? T(a.constant) : acc + T(a.constant);
        return acc;
    }

    // [readout][source]
    template <typename T>
    struct Linear
    {
        std::vector<std::vector<T>> coef;
    };

    // Right-looking LU in the pivot order, without permutations: one reciprocal per pivot for the multipliers,
    // updates over the pivot row's nonzeros; then, per source, its right-hand side, forward and back substitution
    // (a divide by each pivot), and the readouts as sums of the unknowns they read.
    template <typename T>
    Linear<T> solve(const System& system, const Pivots& pivots, const std::vector<T>& symbols)
    {
        const int n = system.n;
        const auto un = static_cast<std::size_t>(n);
        std::vector<std::vector<T>> a(un, std::vector<T>(un, T(0.0)));
        for (std::size_t r = 0; r < un; ++r)
        {
            for (std::size_t c = 0; c < un; ++c)
            {
                if (!system.a[r][c].isZero())
                    a[r][c] = fromAffine(system.a[r][c], symbols);
            }
        }
        std::vector<int> rowStep(un, -1);
        std::vector<int> colStep(un, -1);
        for (std::size_t k = 0; k < pivots.order.size(); ++k)
        {
            rowStep[static_cast<std::size_t>(pivots.order[k].first)] = static_cast<int>(k);
            colStep[static_cast<std::size_t>(pivots.order[k].second)] = static_cast<int>(k);
        }
        std::vector<std::vector<std::pair<std::size_t, T>>> multipliers(un);
        for (std::size_t k = 0; k < un; ++k)
        {
            const auto pr = static_cast<std::size_t>(pivots.order[k].first);
            const auto pc = static_cast<std::size_t>(pivots.order[k].second);
            bool haveReciprocal = false;
            T reciprocal = T(0.0);
            for (std::size_t r = 0; r < un; ++r)
            {
                if (rowStep[r] <= static_cast<int>(k) || isZero(a[r][pc]))
                    continue;
                if (!haveReciprocal)
                {
                    reciprocal = T(1.0) / a[pr][pc];
                    haveReciprocal = true;
                }
                const T m = a[r][pc] * reciprocal;
                multipliers[k].emplace_back(r, m);
                for (std::size_t c = 0; c < un; ++c)
                {
                    if (colStep[c] <= static_cast<int>(k) || isZero(a[pr][c]))
                        continue;
                    a[r][c] = a[r][c] - m * a[pr][c];
                }
                a[r][pc] = T(0.0);
            }
        }

        Linear<T> out;
        out.coef.assign(system.readouts.size(), std::vector<T>(system.sources.size(), T(0.0)));
        // The unknowns some readout reads, and those back substitution needs for them.
        std::vector<bool> needed(un, false);
        for (const System::Readout& r : system.readouts)
        {
            for (const auto& [u, coef] : r.unknowns)
                needed[static_cast<std::size_t>(u)] = true;
        }
        // (step k's unknown reads those pivoted after it, so the walk goes forward)
        for (std::size_t k = 0; k < un; ++k)
        {
            const auto pr = static_cast<std::size_t>(pivots.order[k].first);
            const auto pc = static_cast<std::size_t>(pivots.order[k].second);
            if (!needed[pc])
                continue;
            for (std::size_t c = 0; c < un; ++c)
            {
                if (colStep[c] > static_cast<int>(k) && !isZero(a[pr][c]))
                    needed[c] = true;
            }
        }
        for (std::size_t s = 0; s < system.sources.size(); ++s)
        {
            std::vector<T> b(un, T(0.0));
            for (std::size_t r = 0; r < un; ++r)
            {
                if (!system.rhs[s][r].isZero())
                    b[r] = fromAffine(system.rhs[s][r], symbols);
            }
            for (std::size_t k = 0; k < un; ++k)
            {
                const auto pr = static_cast<std::size_t>(pivots.order[k].first);
                if (isZero(b[pr]))
                    continue;
                for (const auto& [r, m] : multipliers[k])
                    b[r] = isZero(b[r]) ? -(m * b[pr]) : b[r] - m * b[pr];
            }
            std::vector<T> z(un, T(0.0));
            for (std::size_t k = un; k-- > 0;)
            {
                const auto pr = static_cast<std::size_t>(pivots.order[k].first);
                const auto pc = static_cast<std::size_t>(pivots.order[k].second);
                if (!needed[pc])
                    continue;
                T acc = b[pr];
                for (std::size_t c = 0; c < un; ++c)
                {
                    if (colStep[c] > static_cast<int>(k) && !isZero(a[pr][c]) && !isZero(z[c]))
                        acc = isZero(acc) ? -(a[pr][c] * z[c]) : acc - a[pr][c] * z[c];
                }
                if (!isZero(acc))
                {
                    z[pc] = acc / a[pr][pc];
                    annotate(z[pc], system.unknownNames[pc] + " per unit of " + system.sources[s].name);
                }
            }
            for (std::size_t o = 0; o < system.readouts.size(); ++o)
            {
                T acc = T(0.0);
                for (const auto& [u, coef] : system.readouts[o].unknowns)
                {
                    const T& zu = z[static_cast<std::size_t>(u)];
                    if (isZero(zu))
                        continue;
                    const T term = coef == 1 ? zu : coef == -1 ? -zu : T(coef) * zu;
                    acc = isZero(acc) ? term : acc + term;
                }
                for (const auto& [src, coef] : system.readouts[o].sources)
                {
                    if (static_cast<std::size_t>(src) == s)
                        acc = isZero(acc) ? T(coef) : acc + T(coef);
                }
                out.coef[o][s] = acc;
            }
        }
        return out;
    }

    // A coefficient of the module: its field's name, and its value.
    template <typename T>
    using Coefficients = std::vector<std::pair<std::string, T>>;

    // The module's coefficients for one block. With a port P: from j = (p - v) / K, K = -1 / (dj/dv) (the port's
    // self-impedance) and p_P_by_s = (dj/ds) K for each source s but v; ka_P = Is K, kb_P = ka / (N Vt), lna_P =
    // log(kb), and kc_P = ka N Vt with ADAA. Then <readout>_by_<source> for the output and each capacitor's voltage.
    // A zero coefficient is left out.
    template <typename T>
    Coefficients<T> derive(const System& system, const Linear<T>& linear, const Block& block, bool adaa)
    {
        Coefficients<T> out;
        if (block.port)
        {
            const auto j = static_cast<std::size_t>(system.portCurrent);
            const auto v = static_cast<std::size_t>(system.port);
            T k = T(1.0) / (-linear.coef[j][v]);
            annotate(k, "K, the port's self-impedance");
            for (std::size_t s = 0; s < system.sources.size(); ++s)
            {
                if (s == v || isZero(linear.coef[j][s]))
                    continue;
                const std::string name = "p_" + block.port->name + "_by_" + system.sources[s].name;
                out.emplace_back(name, bindField(name, linear.coef[j][s] * k));
            }
            const std::string p = block.port->name;
            const T ka = bindField("ka_" + p, T(block.port->is) * k);
            out.emplace_back("ka_" + p, ka);
            const T kb = bindField("kb_" + p, ka * T(1 / block.port->nvt));
            out.emplace_back("kb_" + p, kb);
            out.emplace_back("lna_" + p, bindField("lna_" + p, logOf(kb)));
            if (adaa)
                out.emplace_back("kc_" + p, bindField("kc_" + p, ka * T(block.port->nvt)));
        }
        for (std::size_t o = 0; o < system.readouts.size(); ++o)
        {
            if (static_cast<int>(o) == system.portCurrent)
                continue;
            for (std::size_t s = 0; s < system.sources.size(); ++s)
            {
                if (isZero(linear.coef[o][s]))
                    continue;
                const std::string name = system.readouts[o].name + "_by_" + system.sources[s].name;
                out.emplace_back(name, bindField(name, linear.coef[o][s]));
            }
        }
        return out;
    }

    // A block in Holters' form (adaa.cpp's Block, generalised): N states, one input, one output, at most one port.
    //   p = Cp x(n-1) + dp u(n);  v solves v + k sinh(v / nvt) = p, k = 2 Is K
    //   x(n) = A x(n-1) + B u(n) + W v;  y(n) = Cy x(n-1) + Dy u(n) + wy v
    struct Holters
    {
        int states = 0;
        bool port = false;
        std::vector<double> cp;
        double dp = 0;
        std::vector<std::vector<double>> a;
        std::vector<double> b;
        std::vector<double> w;
        std::vector<double> cy;
        double dy = 0;
        double wy = 0;
        double is = 0;
        double nvt = 0;
        double k = 0;   // 2 Is K
    };

    // Block `block`'s Holters form at the design rate fs' (the rate its capacitors see) and pots (a point as
    // potGrid's).
    Holters holters(const Plan& plan, int block, double designRate, const std::vector<double>& pots);

    // A linear block's analog response at s: its output per volt of its input, with each capacitor an admittance s C.
    std::complex<double> analog(const Plan& plan, int block, std::complex<double> s, const std::vector<double>& pots);

    // The same with the system and pivots at hand.
    std::complex<double> analog(const System& system, const Pivots& pivots, const Plan& plan, int block,
                                std::complex<double> s, const std::vector<double>& pots);

    // A linear block's 1x check at a host rate: the worst |dB H_TR(f) - dB H(j 2 pi f)| over 20 Hz to 20 kHz in steps
    // of 1.01x, at each point of its pot grid, counting only where dB H is within 40 dB of its maximum, and skipping a
    // pot point where |H| is below -120 dB everywhere. H_TR(f) = H(j 2 fs tan(pi f / fs)), the trapezoidal rule's.
    double oneXError(const System& system, const Pivots& pivots, const Plan& plan, int block, double rate);
} // namespace fminus::circuit
