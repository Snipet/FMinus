/* tests/smoke/calc/Calc.l: the toolchain smoke test's scanner (Flex 2.6). It shows the setup a real FMinus scanner
 * can copy:
 *   - reentrant, with no yywrap, no input()/yyunput, no unistd.h and no interactive mode;
 *   - a prefix ("fmcalc"), so its symbols cannot clash with another scanner's in the same binary;
 *   - it returns Bison's symbols, and keeps its location in the scanner's extra data (CalcScanner.h).
 * It came with the project skeleton; delete it once the real scanner has its own tests. */
%option reentrant noyywrap nounput noinput nounistd never-interactive batch
%option prefix="fmcalc"
%option extra-type="fminus::smoke::CalcScannerState*"

%top{
// Before anything Flex generates: the parser's types, the scanner's state type and YY_DECL.
#include "smoke/calc/CalcScanner.h"
}

%{
#include <charconv>
#include <string>

using Parser = fminus::smoke::CalcParser;

// Every match advances the location by its length; rules call step() to start the next token there.
#define YY_USER_ACTION yyextra->where.columns(static_cast<int>(yyleng));
%}

DIGIT   [0-9]

%%

%{
    yyextra->where.step();
%}

[ \t\r]+        { yyextra->where.step(); }
\n+             { yyextra->where.lines(static_cast<int>(yyleng)); yyextra->where.step(); }
{DIGIT}+        {
                    std::int64_t value = 0;
                    const auto [end, error] = std::from_chars(yytext, yytext + yyleng, value);
                    if (error != std::errc{} || end != yytext + yyleng)
                        throw Parser::syntax_error(yyextra->where, "number out of range: " + std::string(yytext));
                    return Parser::make_NUMBER(value, yyextra->where);
                }
"+"             { return Parser::make_PLUS(yyextra->where); }
"-"             { return Parser::make_MINUS(yyextra->where); }
"*"             { return Parser::make_STAR(yyextra->where); }
"/"             { return Parser::make_SLASH(yyextra->where); }
"("             { return Parser::make_LPAREN(yyextra->where); }
")"             { return Parser::make_RPAREN(yyextra->where); }
";"             { return Parser::make_SEMI(yyextra->where); }
.               { throw Parser::syntax_error(yyextra->where, "unexpected character '" + std::string(yytext) + "'"); }
<<EOF>>         { return Parser::make_YYEOF(yyextra->where); }

%%
