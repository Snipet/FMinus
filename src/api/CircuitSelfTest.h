// src/api/CircuitSelfTest.h: the self-test (include/fminus/Circuit.h's selfTest) with its stress matrix's shape given,
// for this tree's tests: a shorter window or a slower reference rate measures less, faster, with the same code.
#pragma once

#include "circuit/Search.h"
#include "fminus/Circuit.h"

#include <string_view>

namespace fminus::circuit
{
    // selfTest() with `shape` in place of the report's (StressShape's defaults, which selfTest() uses).
    SelfTestResult selfTestWith(std::string_view netlist, std::string_view fileName, const Options& options,
                                const SelfTestOptions& test, const StressShape& shape);
} // namespace fminus::circuit
