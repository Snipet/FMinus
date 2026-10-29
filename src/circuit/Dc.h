// src/circuit/Dc.h: the circuit compiler's fourth stage, the DC operating point. Each block in chain order, in double,
// at rate 0 (a capacitor's gc = 0 opens it, an inductor's rl = 0 shorts it), its DC sources' values its right-hand
// side and its input the block before's output (.input's is 0 V), by a dense LU with partial pivoting. The module
// runs in offsets from this point: its states, ports and signals are differences from it, so its init and clear set
// 0, its V sources are 0 V (shorts), its I sources open, and tick adds the output's DC back. That is exact for a linear
// network and a port at 0 V at the DC point (f(0) = 0); a port with a DC voltage across it (biased) needs the offset
// formulation of Phase 2, as does a DC point that a pot moves.
#pragma once

#include "circuit/Plan.h"

#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace fminus::circuit
{
    // The DC operating point at one point of the pots.
    struct DcPoint
    {
        std::vector<double> nodes;                              // each node's volts, by index (a string's inner: 0)
        std::vector<std::pair<std::string, double>> sources;    // each V source's current, SPICE's i(V): from n+
                                                                // through the source to n-, in netlist order
        std::vector<std::pair<std::string, double>> ports;      // each port's voltage (its parts, volts), chain order
        std::vector<std::vector<double>> unknowns;              // each block's solution, what a pot must not move
        std::string biased;   // when a port has a DC voltage across it: "port D1 is biased at DC (0.61 V)"; else empty
        int biasedElement = -1;   // and that port's first element
    };

    // The point at `pots` (plan.pots' order), or nothing with an error in `messages`: a block whose equations are
    // singular at DC while DC sources drive it (an op amp with only capacitors around it, say).
    std::optional<DcPoint> solveDc(const Plan& plan, const std::vector<double>& pots, Messages& messages);

    // Stage 4: the point at the default pots into plan.dc and plan.outputDc, checked: no port biased ("port D1 is
    // biased at DC (0.61 V): biased ports need the offset formulation (Phase 2)", at its element), and no pot moving it
    // (each at 0, 0.5 and 1, the others at their defaults); with .op, the point as notes (each node's voltage, each V
    // source's current, each port's voltage); a nonzero output DC, a note. False, with errors, when the module could
    // not run in offsets from it.
    bool dcPoint(Plan& plan, Messages& messages);
} // namespace fminus::circuit
