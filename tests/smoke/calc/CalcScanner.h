// tests/smoke/calc/CalcScanner.h: the glue between the smoke test's scanner (Calc.l) and parser (Calc.y), and the
// calculator's entry point. The pattern for a real FMinus front end:
//   - the parser's header first, for its symbol and location types;
//   - the scanner's state (Flex's yyextra): what the rules keep between tokens, here the location;
//   - the scanner's function under a name of its own (Flex defines it through YY_DECL), and the yylex the parser
//     calls forwarding to it. A prefixed Flex scanner #defines yylex to <prefix>lex in its own files, so the name the
//     parser links against must not be yylex itself.
#pragma once

#include "smoke/calc/Calc.tab.hpp"

#include <string_view>

namespace fminus::smoke
{
    struct CalcScannerState
    {
        location where;
    };

    // The scanner's function: the next token, with its location.
    CalcParser::symbol_type nextToken(yyscan_t scanner);

    // What the parser calls (unqualified, so it lives in the parser's namespace).
    inline CalcParser::symbol_type yylex(yyscan_t scanner)
    {
        return nextToken(scanner);
    }

    // Parses and evaluates `text`: every statement that parsed gives a value, every error a message.
    CalcResult calc(std::string_view text);
} // namespace fminus::smoke

// What Flex defines its scanning function as (Calc.l includes this header first, in %top).
#define YY_DECL fminus::smoke::CalcParser::symbol_type fminus::smoke::nextToken(yyscan_t yyscanner)
