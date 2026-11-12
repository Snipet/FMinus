// src/circuit/Laws.h: a nonlinear port's law, twice. A port's equation, once its block is reduced, is
// v + k phi(v) = p (report §6.1), k > 0: a diode pair's k = 2 Is K, phi = sinh(v / N Vt); one diode's k = Is K, phi =
// e^(v / N Vt) - 1; an asymmetric pair's Is K (e^(v / N Vt) - 1) - Is2 K (e^(-v / N2 Vt) - 1) (the exp-affine family,
// EA); an OTA's k = kd g, phi = tanh(v / 2 Vt) (smooth monotone, SM), g its gain, which a law input may move every
// sample. Here is
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
        // a diode's: ka = Is K, kb = ka / N Vt, kc = ka N Vt (ADAA), lna = log kb, and " + lna" with its sign
        std::string ka;
        std::string kb;
        std::string kc;
        std::string lna;
        std::string plusLna;
        // an asymmetric pair's other diode: kr = Is2 K, ks = kr / N2 Vt, kcr = kr N2 Vt (ADAA), lnr = log ks
        std::string kr;
        std::string ks;
        std::string kcr;
        std::string lnr;
        // an OTA's: kd = K, rkd = 1 / kd (ADAA), and its gain g (a field when it moves, or a literal)
        std::string kd;
        std::string rkd;
        std::string g;
        Solver solver;
        bool adaa = false;
    };

    // Whether the law's port takes a Halley step under .solver halley(K): a diode pair's, whose Halley denominator
    // cannot vanish from a point in the bracket; one diode's, an asymmetric pair's and an OTA's keep Newton steps, K of
    // them (Laws.cpp gives each law's f'' and why).
    bool takesHalley(PortDef::Law law);

    // A port's steps under `solver`, as the notes and the header say them: "2 Newton steps", "1 Halley step", or for
    // a law that keeps Newton's under halley(K), "1 Newton step (halley(1): one diode's law keeps Newton steps)".
    std::string stepsText(const Solver& solver, PortDef::Law law);

    // From p to v: the guess and the Newton (or Halley) steps, each in the bracket. An OTA's starts with kg = kd g and
    // kgs = kg / 2 Vt (with ADAA, kgv = kg 2 Vt). With ADAA it starts with adaaHeadText's lines (an OTA's after kgv).
    std::vector<LawLine> solveText(const PortDef& port, const LawOperands& o);

    // ADAA's lines that need p alone, at the top of the step: an OTA's carry, the stored point onto this step's curve,
    // p_P += (kg - kgo_P) t_P, which keeps its Newton residual (report §5.1's mean pairs that point with this step's on
    // one curve); then dp = p - p_P and its reciprocal rdp, which the mean multiplies by at the end (a divide there
    // would wait on the solve; here it runs beside it).
    std::vector<LawLine> adaaHeadText(const PortDef& port, const LawOperands& o);

    // ADAA's mean over [p_P, p] (the closed form F(p) = p v - H(v), H = v^2 / 2 + k Phi(v)): a diode's of v, as vb;
    // an OTA's of its current (the i-form's source), as ib. It reads adaaHeadText's dp and rdp.
    std::vector<LawLine> adaaText(const PortDef& port, const LawOperands& o);

    // What the step stores after its states: an OTA's t_P = tanh(v / 2 Vt); with ADAA, the point (p_P, and a pair's
    // c_P = 2 cosh(v / N Vt); one diode's c_P = e^(v / N Vt); an asymmetric pair's that and cr_P = e^(-v / N2 Vt); an
    // OTA's lc_P = log cosh(v / 2 Vt) and kgo_P = kd g). v_P is the caller's.
    std::vector<LawLine> storeText(const PortDef& port, const LawOperands& o);

    // b<N>_set's carry, when a pot moves a diode's K (ADAA): the stored point onto the new curve, v_P held, its Newton
    // residual kept, p_P += (K' - K) f(v_P): a pair's from e^(v_P / N Vt) computed again, one diode's and an
    // asymmetric pair's from the stored values. `before` names the old ka (and kr): kprev_P (krprev_P).
    std::vector<LawLine> carryText(const PortDef& port, const LawOperands& o);

    // The fields ADAA adds for the law, after v_P and p_P: (name, its type, its comment, its value at clear).
    struct LawField
    {
        std::string name;
        bool vector = true;   // float2 with .channels 2 (else float)
        std::string note;
        std::string clear;
    };

    std::vector<LawField> adaaFields(const PortDef& port);

    // The fields every port of the law carries, after v_P, with or without ADAA: an OTA's t_P = tanh(v_P / 2 Vt), which
    // the next step's first Newton step reads (and ADAA's carry and mean); none for a diode's.
    std::vector<LawField> portFields(const PortDef& port);

    // ---- the double twin ----------------------------------------------------------------------------------------

    // A law's shape, v + g(v) = p: g = k sinh(v / s) (Pair, k = 2 Is K), k (e^(v / s) - 1) (Single, k = Is K), that
    // less k2 (e^(-v / s2) - 1) (Asym), k tanh(v / s) (Tanh, k = kd g).
    struct LawShape
    {
        PortDef::Law law = PortDef::Law::Pair;
        double k = 0;
        double s = 1;
        double k2 = 0;
        double s2 = 1;
    };

    // A block's shape (an OTA's at its gain g).
    LawShape lawShape(const Holters& h, double g);

    // g(v), g'(v) and the antiderivative G(v) (pair: k s cosh(v / s); single: k (s e^(v / s) - v); asym: that plus
    // k2 (s2 e^(-v / s2) + v); tanh: k s lc(v / s)).
    double lawValue(const LawShape& law, double v);
    double lawDerivative(const LawShape& law, double v);
    double lawAntiderivative(const LawShape& law, double v);

    // The closed-form guess the emitted text starts from (an exp-affine law's: omega3 for the conducting diode; one
    // diode's p + k for p < 0, the diode off).
    double lawGuess(const LawShape& law, double p);

    // The guess (or the previous v) and `steps` Newton steps in the bracket, as the emitted text; and the solution to
    // convergence (as lawConverged below: a pair's and an OTA's are theirs, one diode's and an asymmetric pair's by
    // bracketed Newton from lawGuess).
    // `start`, when given, gets the last Newton step's starting point (ADAA's series reads it). `shape` Halley: a
    // pair's steps are Halley's (lawSteps), the other laws' Newton's (takesHalley).
    double lawStepsOf(const LawShape& law, double p, Solver::Guess guess, double previous, int steps,
                      double* start = nullptr, Solver::Shape shape = Solver::Shape::Newton);
    double lawConvergedOf(const LawShape& law, double p, double previous, long* unconverged, int* iterations);

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
    // OTA's previous v. `shape` Halley: a pair's steps are Halley's, from the guess in the bracket (an OTA's stay
    // Newton's).
    double lawSteps(PortDef::Law law, double p, double k, double s, Solver::Guess guess, double previous, int steps,
                    double* start = nullptr, Solver::Shape shape = Solver::Shape::Newton);

    // ADAA's tail after Newton steps, as the text: the exponentials at the solved v from the last Newton step's at its
    // point v0, by the addition theorem and e^dl's series to dl^4 (dl = (v - v0) / s; the exact form from |dl| =
    // 0.25 on). cosh(v / s) so (a pair's), and an exp-affine law's antiderivative G(v) with its exponentials so.
    double lawCoshSeries(double v0, double v, double s);
    double lawAntiderivativeSeries(const LawShape& law, double v0, double v);

    // The solution to convergence: a pair's from omega3, at most 300 Newton steps in the bracket (Reference.h's
    // converged); an OTA's from the previous v by Newton kept in the bracket, which shrinks to where the root is and is
    // bisected where a step would leave it, until |dv| < 1e-16 + 1e-15 |v|. `unconverged` counts a solve that runs
    // out of its 300 steps; `iterations` gets the most it took.
    double lawConverged(PortDef::Law law, double p, double k, double s, double previous, long* unconverged,
                        int* iterations);
} // namespace fminus::circuit
