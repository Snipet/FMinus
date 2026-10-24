// src/frontend/Scanner.h: the glue between the scanner (Lexer.l) and the parser (Parser.y), the pattern of
// tests/smoke/calc/:
//   - the parser's header first, for its symbol and location types;
//   - the scanner's state (Flex's yyextra), here the location (and where an open block comment began), the parse
//     context, whose modules' names the scanner returns as types, and the files #include reads;
//   - the scanner's function under its own name (Flex defines it through YY_DECL), and the yylex the parser calls
//     forwarding to it: the prefixed scanner #defines yylex to fminuslex in its own files.
#pragma once

#include "frontend/Frontend.h"
#include "frontend/Parser.tab.hpp"

#include <deque>
#include <set>
#include <string>
#include <vector>

namespace fminus::frontend
{
    // An #include's file while the scanner reads it: its buffer, the buffer it interrupted, and where the scanner was
    // in that one (at the #include). The buffers are Flex's YY_BUFFER_STATEs, which only Lexer.l and Frontend.cpp see.
    struct OpenInclude
    {
        void* buffer = nullptr;
        void* outer = nullptr;
        location where;
    };

    struct ScannerState
    {
        location where;
        location commentStart;   // of the block comment being skipped, for "unterminated /* comment"
        const ParseContext* context = nullptr;
        Diagnostics* diagnostics = nullptr;   // the files #include reads are its files

        // #include: how a name finds its file (null: none is found); the files read so far, which Bison's locations
        // point at (a deque, so that they stay where they are); the names of the files read, the program's own
        // first, as each is read once; and the files being read, the innermost last.
        const IncludeResolver* include = nullptr;
        std::deque<SourceFile> files;
        std::set<std::string, std::less<>> read;
        std::vector<OpenInclude> open;
    };

    // The source location of a Bison location's start.
    inline SourceLocation sourceLocation(const location& where)
    {
        const int file = where.begin.filename != nullptr ? where.begin.filename->index : 0;
        return {where.begin.line, where.begin.column, file};
    }

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
