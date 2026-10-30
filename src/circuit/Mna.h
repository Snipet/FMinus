// src/circuit/Mna.h: each block's equations, and one derivation over them for three number types. assemble() stamps a
// block's modified nodal analysis: node voltages, the currents of its variable resistors and inductors (the impedance
// form, so a pot's 0-ohm end is exact and an inductor is a short at DC), of its V sources and floating E sources, and
// its port's current are the unknowns; the port itself is a voltage source v (Holters' v-form), or an OTA's current i. choosePivots() fixes an elimination order at generation, exact +-1 entries first, checked at a grid of
// rates and pots. solve<T>() eliminates in that order and gives every readout (the port's current, the output, each
// capacitor's voltage, each inductor's current) as a linear form in the block's sources (its input, the states, v);
// derive<T>()
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
        // A value set or init computes: a capacitor's 2 C fs' (gc_C1), a variable resistance (r_Ptone), an
        // inductor's 2 L fs' (rl_L1), or an E's or a G's gain that pots move (k_E1).
        struct Symbol
        {
            enum class Kind
            {
                Gc,
                R,
                Rl,
                Gain,
            };

            std::string name;
            Kind kind = Kind::Gc;
            int branch = -1;       // the block's branch it belongs to (Gc, R, Rl)
            int controlled = -1;   // Gain: the block's controlled source
        };

        // What drives the block: its input (a voltage), a capacitor's or an inductor's state, the port's voltage v
        // (or an OTA's current i), and at the DC operating point only (assemble's `dc`), the DC sources' values.
        struct Source
        {
            enum class Kind
            {
                Input,
                State,
                Port,
                Constant,
            };

            std::string name;   // "u" (the .input's name) or "n_p1"; "x_C1", "x_L1"; "v_D1"; "V1", "I1"
            Kind kind = Kind::Input;
            int branch = -1;    // a state's capacitor or inductor
            double value = 0;   // a Constant's volts or amps
        };

        // A linear form in the unknowns and the sources: what the block reads out.
        struct Readout
        {
            enum class Kind
            {
                PortCurrent,   // j_D1
                Node,          // n_b: the block's output
                Capacitor,     // vc_C3
                Current,       // il_L1: an inductor's current
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
        int port = -1;                                   // the port's source: v (the v-form), or i (the i-form)
        int portCurrent = -1;                            // its readout: j (the v-form), or v (the i-form)
        bool iForm = false;                              // the port is a controlled source (an OTA): its current
                                                         // the source i, its control voltage the readout v
        int output = -1;                                 // the output's readout
        // per state source: the readout its update reads (a capacitor's voltage, an inductor's current), and a sign
        std::vector<std::pair<int, double>> capacitor;
        // the unknowns and rows of the variable branches (pots' sections, {expr} resistors, switches): their currents
        // and their rows V(a) - V(b) - r i = 0, the only place an r appears (the Woodbury form pivots them last)
        std::vector<bool> variableUnknown;
        std::vector<bool> variableRow;
    };

    // The block's equations (Plan.h's Block, by chain index), in the offsets from the DC operating point the module
    // runs in: a DC source is no source (a V source a short, a grounded one's node ground, an I source open). With
    // `dc`, the DC sources too, as Constant sources after the others (their values in Source::value), for the DC
    // operating point (Dc.h).
    System assemble(const Plan& plan, int block, bool dc = false);

    // The elimination order: (row, unknown) for each step.
    struct Pivots
    {
        std::vector<std::pair<int, int>> order;
    };

    // The order, chosen and checked over the block's grid of rates and pots; nothing (with an error) when the
    // equations are singular somewhere on it. With `variableLast`, an order that pivots the variable branches'
    // currents last, on their own rows (the Woodbury form: until then nothing a pot moves enters the elimination),
    // under the same threshold and residual rules; nothing (and no error) when there is none.
    std::optional<Pivots> choosePivots(const System& system, const Plan& plan, int block, Messages& messages,
                                       bool variableLast = false);

    // Whether a port block's self-impedance K is positive (and finite) at every point of that grid, as the solver's
    // bracket [min(0, p), max(0, p)] and log(Is K / N Vt) need; otherwise an error (positive feedback makes K
    // negative). A block without a port passes.
    bool checkPort(const System& system, const Pivots& pivots, const Plan& plan, int block, Messages& messages);

    // The smallest |pivot| / max |its column| of the elimination in `pivots`, over the block's grid of rates and pots.
    double pivotMargin(const System& system, const Pivots& pivots, const Plan& plan, int block);

    // The values of the system's symbols at a design rate (fs', the rate the block's capacitors see) and pots (a point
    // as potGrid's: the pots, then the switches' positions; a switch left out is at its default): a capacitor's gc =
    // 2 C fs' and an inductor's rl = 2 L fs' (the trapezoidal rule), 3 C fs' / 2 and 3 L fs' / 2 (BDF2), with the
    // block's integrator, or `method` when given.
    std::vector<double> symbolValues(const System& system, const Plan& plan, int block, double designRate,
                                     const std::vector<double>& pots, std::optional<Method> method = std::nullopt);

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

    // The module's coefficients for one block. With a diode port P (the v-form): from j = (p - v) / K, K = -1 / (dj/dv)
    // (the port's self-impedance, kd in v + kd f(v) = p) and p_P_by_s = (dj/ds) K for each source s but v; ka_P = Is
    // K, kb_P = ka / (N Vt), lna_P = log(kb), and kc_P = ka N Vt with ADAA (an asymmetric pair's other diode too:
    // kr_P = Is2 K, ks_P, lnr_P and kcr_P). With an OTA P (the i-form): from its
    // control voltage v = p - kd i, p_P_by_s = dv/ds for each source s but i, and kd_P = -dv/di (its gain g multiplies
    // it in the step, where a law input may move it every sample). Then <readout>_by_<source> for the output and each
    // capacitor's voltage, on every source (an OTA's i too). A zero coefficient is left out.
    template <typename T>
    Coefficients<T> derive(const System& system, const Linear<T>& linear, const Block& block, bool adaa)
    {
        Coefficients<T> out;
        if (block.port && system.iForm)
        {
            // the i-form: v = p - kd i, read off the control voltage's readout
            const auto r = static_cast<std::size_t>(system.portCurrent);
            const auto i = static_cast<std::size_t>(system.port);
            T kd = -linear.coef[r][i];
            annotate(kd, "kd, the OTA's loop: its control voltage per unit of its current, negated");
            for (std::size_t s = 0; s < system.sources.size(); ++s)
            {
                if (s == i || isZero(linear.coef[r][s]))
                    continue;
                const std::string name = "p_" + block.port->name + "_by_" + system.sources[s].name;
                out.emplace_back(name, bindField(name, linear.coef[r][s]));
            }
            out.emplace_back("kd_" + block.port->name, bindField("kd_" + block.port->name, kd));
        }
        else if (block.port)
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
            if (block.port->law == PortDef::Law::Asym)
            {
                // the cathode-to-anode diode's: kr = Is2 K, ks = kr / (N2 Vt), lnr = log(ks), kcr = kr N2 Vt
                const T kr = bindField("kr_" + p, T(block.port->is2) * k);
                out.emplace_back("kr_" + p, kr);
                const T ks = bindField("ks_" + p, kr * T(1 / block.port->nvt2));
                out.emplace_back("ks_" + p, ks);
                out.emplace_back("lnr_" + p, bindField("lnr_" + p, logOf(ks)));
                if (adaa)
                    out.emplace_back("kcr_" + p, bindField("kcr_" + p, kr * T(block.port->nvt2)));
            }
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
    // With BDF2 its state is [x; xo], 2N: each capacitor's and inductor's history and its second part (the emitted
    // step's xo_<C>), x <- h - xo and xo <- h / 4 with h = 4/3 g v (a capacitor's; an inductor's 4/3 r i).
    //   p = Cp x(n-1) + dp u(n);  v solves v + k sinh(v / nvt) = p, k = 2 Is K (a diode pair),
    //                             v + k (e^(v / nvt) - 1) = p, k = Is K (one diode),
    //                             v + k (e^(v / nvt) - 1) - k2 (e^(-v / nvt2) - 1) = p (an asymmetric pair), or
    //                             v + k g tanh(v / nvt) = p, k = K and nvt = 2 Vt (an OTA, g its gain)
    //   x(n) = A x(n-1) + B u(n) + W v;  y(n) = Cy x(n-1) + Dy u(n) + wy v
    struct Holters
    {
        PortDef::Law law = PortDef::Law::Pair;
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
        double k = 0;   // 2 Is K (a pair), Is K (one diode, an asymmetric pair's forward one), K (an OTA: kd)
        double k2 = 0;    // an asymmetric pair's other diode: Is2 K
        double nvt2 = 0;  // and its N2 Vt
        double g = 1;   // an OTA's gain, amps, when no stream of gains is given (Reference.h)
        // An OTA's block in its own i-form as well (the matrices above are its v-form, v for i substituted): x(n) = Ai
        // x(n-1) + Bi u(n) + Wi i, y(n) = Cyi x(n-1) + Dyi u(n) + wyi i, i its current, g tanh(v / 2 Vt). The
        // reference runs this one, as the emitted code does (with ADAA, the mean of i, not of v).
        bool iForm = false;
        std::vector<std::vector<double>> ai;
        std::vector<double> bi;
        std::vector<double> wi;
        std::vector<double> cyi;
        double dyi = 0;
        double wyi = 0;
    };

    // Block `block`'s Holters form at the design rate fs' (the rate its capacitors see), pots (a point as potGrid's)
    // and law inputs (an OTA's gain's, in plan.laws' order; empty: each at its low end), with the block's integrator,
    // or `method` when given.
    Holters holters(const Plan& plan, int block, double designRate, const std::vector<double>& pots,
                    const std::vector<double>& laws = {}, std::optional<Method> method = std::nullopt);



    // A linear block's analog response at s: its output per volt of its input, with each capacitor an admittance s C.
    std::complex<double> analog(const Plan& plan, int block, std::complex<double> s, const std::vector<double>& pots);

    // The same with the system and pivots at hand.
    std::complex<double> analog(const System& system, const Pivots& pivots, const Plan& plan, int block,
                                std::complex<double> s, const std::vector<double>& pots);

    // A linear block's 1x check at a host rate: the worst |dB H_d(f) - dB H(j 2 pi f)| over 20 Hz to 20 kHz in steps
    // of 1.01x, at each point of its pot grid, counting only where dB H is within 40 dB of its maximum, and skipping a
    // pot point where |H| is below -120 dB everywhere. H_d(f) is the integrator's map of the analog response: the
    // trapezoidal rule's H(j 2 fs tan(pi f / fs)), BDF2's H(fs (3 - 4 z^-1 + z^-2) / 2) at z = e^(j 2 pi f / fs).
    double oneXError(const System& system, const Pivots& pivots, const Plan& plan, int block, double rate,
                     Method method = Method::Tr);

    // The discrete s an integrator puts in place of the analog one at f Hz and `rate`: the trapezoidal rule's j 2 fs
    // tan(pi f / fs), BDF2's fs (3 - 4 z^-1 + z^-2) / 2 at z = e^(j 2 pi f / fs).
    std::complex<double> discreteS(Method method, double f, double rate);
} // namespace fminus::circuit
