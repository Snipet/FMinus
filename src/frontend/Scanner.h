// src/frontend/Scanner.h: the glue between the scanner (Lexer.l) and the parser (Parser.y), the pattern of
// tests/smoke/calc/:
//   - the parser's header first, for its symbol and location types;
//   - the scanner's state (Flex's yyextra), here the location (and where an open block comment began), and the
//     parse context, whose modules' names the scanner returns as types;
//   - the scanner's function under its own name (Flex defines it through YY_DECL), and the yylex the parser calls
//     forwarding to it: the prefixed scanner #defines yylex to fminuslex in its own files.
#pragma once

#include "frontend/Parser.tab.hpp"

namespace fminus::frontend
{
    struct ScannerState
    {
        location where;
        location commentStart;   // of the block comment being skipped, for "unterminated /* comment"
        const ParseContext* context = nullptr;
    };

    // The next token, with its location.
    Parser::symbol_type nextToken(yyscan_t scanner);

    // What the parser calls (unqualified, so it lives in the parser's namespace).
    inline Parser::symbol_type yylex(yyscan_t scanner)
    {
        return nextToken(scanner);
    }
} // namespace fminus::frontend

// What Flex defines its scanning function as (Lexer.l includes this header first, in %top).
#define YY_DECL fminus::frontend::Parser::symbol_type fminus::frontend::nextToken(yyscan_t yyscanner)
