// tests/smoke/calc/CalcDriver.cpp: runs the scanner over an in-memory buffer and the parser over the scanner. This is
// the shape a JIT front end takes: its source comes from a string, never from a file or stdin.
#include "smoke/calc/CalcScanner.h"
#include "smoke/calc/Calc.yy.hpp"   // the reentrant scanner's API: fmcalclex_init_extra, fmcalc_scan_bytes, ...

#include <climits>
#include <stdexcept>

namespace fminus::smoke
{
    CalcResult calc(std::string_view text)
    {
        if (text.size() > static_cast<std::size_t>(INT_MAX))
            throw std::length_error("calc: input too long");

        CalcScannerState state;
        yyscan_t scanner = nullptr;
        if (fmcalclex_init_extra(&state, &scanner) != 0)
            throw std::runtime_error("calc: cannot create the scanner");
        YY_BUFFER_STATE buffer = fmcalc_scan_bytes(text.data(), static_cast<int>(text.size()), scanner);

        CalcResult result;
        CalcParser parser(scanner, result);
        parser.parse();

        fmcalc_delete_buffer(buffer, scanner);
        fmcalclex_destroy(scanner);
        return result;
    }
} // namespace fminus::smoke
