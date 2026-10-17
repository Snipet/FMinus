/* src/frontend/Lexer.l: the scanner of FMinus v0, C's tokens (Flex 2.6; reentrant, prefixed "fminus", read from
 * memory). It returns Bison's symbols (Parser.y) with their locations, which it keeps in the scanner's state
 * (Scanner.h).
 *
 * Comments are C's: // to the end of the line, and block comments, which do not nest. Floating constants are C's:
 * 1.5, 1., .5, 2e3, 1.5e-3f, hex floats such as 0x1.8p1 and 0x1p-3f, and integer constants such as 2, which name the
 * same float. The f or F suffix is allowed only where C allows it (not on 2), and is kept in the spelling.
 * To add a token: a rule here, its %token in Parser.y. Keywords come before {IDENTIFIER}: with equal lengths, Flex
 * picks the earlier rule. */
%option reentrant noyywrap nounput noinput nounistd never-interactive batch
%option prefix="fminus"
%option extra-type="fminus::frontend::ScannerState*"

%x BLOCK_COMMENT

%top{
// Before anything Flex generates: the parser's types, the scanner's state type and YY_DECL.
#include "frontend/Scanner.h"
}

%{
#include <string>

using Parser = fminus::frontend::Parser;

// Every match advances the location by its length; rules call step() to start the next token there.
#define YY_USER_ACTION yyextra->where.columns(static_cast<int>(yyleng));
%}

DIGITS      [0-9]+
HEXDIGITS   [0-9a-fA-F]+
EXPONENT    [eE][+-]?{DIGITS}
BINEXPONENT [pP][+-]?{DIGITS}
IDENTIFIER  [A-Za-z_][A-Za-z0-9_]*

%%

%{
    yyextra->where.step();
%}

[ \t\r]+                    { yyextra->where.step(); }
\n+                         { yyextra->where.lines(static_cast<int>(yyleng)); yyextra->where.step(); }
"//".*                      { yyextra->where.step(); }

"/*"                        { yyextra->commentStart = yyextra->where; BEGIN(BLOCK_COMMENT); }
<BLOCK_COMMENT>"*/"         { yyextra->where.step(); BEGIN(INITIAL); }
<BLOCK_COMMENT>[^*\n]+      { }
<BLOCK_COMMENT>"*"          { }
<BLOCK_COMMENT>\n           { yyextra->where.lines(1); }
<BLOCK_COMMENT><<EOF>>      { throw Parser::syntax_error(yyextra->commentStart, "unterminated /* comment"); }

"float"                     { return Parser::make_FLOAT(yyextra->where); }
"const"                     { return Parser::make_CONST(yyextra->where); }
"return"                    { return Parser::make_RETURN(yyextra->where); }

{DIGITS}"."[0-9]*{EXPONENT}?[fF]?   |
"."{DIGITS}{EXPONENT}?[fF]?         |
{DIGITS}{EXPONENT}[fF]?             |
{DIGITS}                            |
0[xX]({HEXDIGITS}("."[0-9a-fA-F]*)?|"."{HEXDIGITS}){BINEXPONENT}[fF]? {
                                return Parser::make_NUMBER(std::string(yytext, yyleng), yyextra->where);
                            }
{DIGITS}[fF]                {
                                const std::string suffix(1, yytext[yyleng - 1]);
                                throw Parser::syntax_error(yyextra->where,
                                                           "invalid suffix '" + suffix + "' on integer constant");
                            }
{IDENTIFIER}                { return Parser::make_IDENTIFIER(std::string(yytext, yyleng), yyextra->where); }

"+="                        { return Parser::make_PLUS_ASSIGN(yyextra->where); }
"-="                        { return Parser::make_MINUS_ASSIGN(yyextra->where); }
"*="                        { return Parser::make_STAR_ASSIGN(yyextra->where); }
"/="                        { return Parser::make_SLASH_ASSIGN(yyextra->where); }
"+"                         { return Parser::make_PLUS(yyextra->where); }
"-"                         { return Parser::make_MINUS(yyextra->where); }
"*"                         { return Parser::make_STAR(yyextra->where); }
"/"                         { return Parser::make_SLASH(yyextra->where); }
"="                         { return Parser::make_ASSIGN(yyextra->where); }
"("                         { return Parser::make_LPAREN(yyextra->where); }
")"                         { return Parser::make_RPAREN(yyextra->where); }
"{"                         { return Parser::make_LBRACE(yyextra->where); }
"}"                         { return Parser::make_RBRACE(yyextra->where); }
","                         { return Parser::make_COMMA(yyextra->where); }
";"                         { return Parser::make_SEMI(yyextra->where); }

.                           {
                                const std::string character(yytext, yyleng);
                                throw Parser::syntax_error(yyextra->where, "unexpected character '" + character + "'");
                            }
<<EOF>>                     { return Parser::make_YYEOF(yyextra->where); }

%%
