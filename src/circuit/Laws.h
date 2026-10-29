// src/circuit/Laws.h: a nonlinear port's law, twice. A port's equation, once its block is reduced, is
// v + k phi(v) = p (report §6.1), k > 0: a diode pair's k = 2 Is K, phi = sinh(v / N Vt) (exp-affine, EA); an OTA's
// k = kd g, phi = tanh(v / 2 Vt) (smooth monotone, SM), g its gain, which a law input may move every sample. Here is
// each law's FMinus text, which a block's step runs (the guess, the Newton steps kept in the bracket [min(0, p),
// max(0, p)], ADAA's closed-form mean and what the step stores for it), and its double twin, which the reference runs
// (phi, phi', the antiderivative, the guess, the fixed steps and the converged solve). The text and the twin are the
// same arithmetic, operation for operation, as Phase 0's were for the pair.
#pragma once

#include "circuit/Mna.h"

#include <string>
#include <vector>

namespace fminus::circuit
{
    // A line of a method's body (without its indent), and its comment.
    struct LawLine
    {
        std::string text;
        std::string note;
    };

    // What a law's text reads: its block's constants as operands (a field, or a literal when the block's derivation
    // folded it), and the configuration.
    struct LawOperands
    {
        std::string type;   // float or float2: v, p and what depends on them
        std::string port;   // the port's ident: "D1", "G1"
        // a pair's: ka = Is K, kb = ka / N Vt, kc = ka N Vt (ADAA), and " + lna" (lna = log kb) with its sign
        std::string ka;
        std::string kb;
        std::string kc;
        std::string plusLna;
        // an OTA's: kd = K, and its gain g (a field when it moves, or a literal)
        std::string kd;
        std::string g;
        Solver solver;
        bool adaa = false;
    };

    // From p to v: the guess and the Newton steps, each in the bracket. An OTA's starts with kg = kd g and kgs = kg / 2
    // Vt (with ADAA, kgv = kg 2 Vt and the carry: the stored point onto this step's curve, p_P += (kg - kgo_P) t_P,
    // which keeps its Newton residual; report §5.1's mean pairs that point with this step's on one curve).
    std::vector<LawLine> solveText(const PortDef& port, const LawOperands& o);

    // ADAA's mean over [p_P, p] (the closed form F(p) = p v - H(v), H = v^2 / 2 + k Phi(v)): a pair's of v, as vb;
    // an OTA's of its current (the i-form's source), as ib.
    std::vector<LawLine> adaaText(const PortDef& port, const LawOperands& o);

    // What the step stores after its states: with ADAA, the point (p_P, and a pair's c_P = 2 cosh(v / N Vt); an OTA's
    // t_P = tanh(v / 2 Vt), lc_P = log cosh(v / 2 Vt) and kgo_P = kd g). v_P is the caller's.
    std::vector<LawLine> storeText(const PortDef& port, const LawOperands& o);

    // The fields ADAA adds for the law, after v_P and p_P: (name, its type, its comment, its value at clear).
    struct LawField
    {
        std::string name;
        bool vector = true;   // float2 with .channels 2 (else float)
        std::string note;
        std::string clear;
    };

    std::vector<LawField> adaaFields(const PortDef& port);

    // ---- the double twin ----------------------------------------------------------------------------------------

    // k of v + k phi(v) = p: a pair's h.k, an OTA's h.k g.
    double lawScale(const Holters& h, double g);

    // phi(v), phi'(v), and the antiderivative of phi, for the law's voltage scale s (a pair's N Vt, an OTA's 2 Vt):
    // sinh(v / s), cosh(v / s) / s, s cosh(v / s); tanh(v / s), (1 - tanh^2) / s, s lc(v / s), lc(x) = log cosh x as
    // the emitted code computes it, |x| + log(1 + e^-2|x|) - ln 2.
    double lawPhi(PortDef::Law law, double v, double s);
    double lawSlope(PortDef::Law law, double v, double s);
    double lawIntegral(PortDef::Law law, double v, double s);

    // log cosh x, |x| + log(1 + e^-2|x|) - ln 2: exact at large |x|, where cosh overflows.
    double logCosh(double x);

    // The guess and `steps` Newton steps in the bracket, as the emitted text: a pair's omega3 (or the previous v), an
    // OTA's previous v.
    double lawSteps(PortDef::Law law, double p, double k, double s, Solver::Guess guess, double previous, int steps);

    // The solution to convergence: a pair's from omega3, at most 300 Newton steps in the bracket (Reference.h's
    // converged); an OTA's from the previous v by Newton kept in the bracket, which shrinks to where the root is and is
    // bisected where a step would leave it, until |dv| < 1e-16 + 1e-15 |v|. `unconverged` counts a solve that runs
    // out of its 300 steps; `iterations` gets the most it took.
    double lawConverged(PortDef::Law law, double p, double k, double s, double previous, long* unconverged,
                        int* iterations);
} // namespace fminus::circuit
