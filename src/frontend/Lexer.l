/* src/frontend/Lexer.l: the scanner of FMinus v0 (Flex 2.6; reentrant, prefixed "fminus", read from memory). It
 * returns Bison's symbols (Parser.y) with their locations, which it keeps in the scanner's state (Scanner.h).
 * To add a token: a rule here, its %token in Parser.y. Keywords come before {IDENTIFIER}: with equal lengths, Flex
 * picks the earlier rule. */
%option reentrant noyywrap nounput noinput nounistd never-interactive batch
%option prefix="fminus"
%option extra-type="fminus::frontend::ScannerState*"

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
EXPONENT    [eE][+-]?{DIGITS}
IDENTIFIER  [A-Za-z_][A-Za-z0-9_]*

%%

%{
    yyextra->where.step();
%}

[ \t\r]+        { yyextra->where.step(); }
\n+             { yyextra->where.lines(static_cast<int>(yyleng)); yyextra->where.step(); }
"//".*          { yyextra->where.step(); }

"fn"            { return Parser::make_FN(yyextra->where); }
"let"           { return Parser::make_LET(yyextra->where); }
"return"        { return Parser::make_RETURN(yyextra->where); }
"float"         { return Parser::make_FLOAT(yyextra->where); }

{DIGITS}("."[0-9]*)?{EXPONENT}?|"."{DIGITS}{EXPONENT}? {
                    return Parser::make_NUMBER(std::string(yytext, yyleng), yyextra->where);
                }
{IDENTIFIER}    { return Parser::make_IDENTIFIER(std::string(yytext, yyleng), yyextra->where); }

"->"            { return Parser::make_ARROW(yyextra->where); }
"+"             { return Parser::make_PLUS(yyextra->where); }
"-"             { return Parser::make_MINUS(yyextra->where); }
"*"             { return Parser::make_STAR(yyextra->where); }
"/"             { return Parser::make_SLASH(yyextra->where); }
"="             { return Parser::make_ASSIGN(yyextra->where); }
"("             { return Parser::make_LPAREN(yyextra->where); }
")"             { return Parser::make_RPAREN(yyextra->where); }
"{"             { return Parser::make_LBRACE(yyextra->where); }
"}"             { return Parser::make_RBRACE(yyextra->where); }
","             { return Parser::make_COMMA(yyextra->where); }
":"             { return Parser::make_COLON(yyextra->where); }
";"             { return Parser::make_SEMI(yyextra->where); }

.               {
                    throw Parser::syntax_error(yyextra->where,
                                               "unexpected character '" + std::string(yytext, yyleng) + "'");
                }
<<EOF>>         { return Parser::make_YYEOF(yyextra->where); }

%%
