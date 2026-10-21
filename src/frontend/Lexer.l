/* src/frontend/Lexer.l: the scanner of FMinus v0, C's tokens (Flex 2.6; reentrant, prefixed "fminus", read from
 * memory). It returns Bison's symbols (Parser.y) with their locations, which it keeps in the scanner's state
 * (Scanner.h).
 *
 * Comments are C's: // to the end of the line, and block comments, which do not nest. Constants are C's: floating
 * ones (1.5, 1., .5, 2e3, 1.5e-3f, hex floats such as 0x1.8p1 and 0x1p-3f) and integer ones (2, 010 in octal, 0x1F in
 * hex), one token whose spelling tells the code generator which. The f or F suffix is allowed only where C allows it
 * (not on 2; in 0x1F the F is a digit), and is kept in the spelling.
 * To add a token: a rule here, its %token in Parser.y. Keywords come before {IDENTIFIER}: with equal lengths, Flex
 * picks the earlier rule. An identifier that names a module declared so far is a type, MODULE_NAME (ParseContext.h).
 */
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
#include <utility>

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
"bool"                      { return Parser::make_BOOL(yyextra->where); }
"int"                       { return Parser::make_INT(yyextra->where); }
"void"                      { return Parser::make_VOID(yyextra->where); }
"restrict"                  { return Parser::make_RESTRICT(yyextra->where); }
"sizeof"                    { return Parser::make_SIZEOF(yyextra->where); }
"float2"                    { return Parser::make_VECTOR_TYPE(fminus::ast::Type::Float2, yyextra->where); }
"float4"                    { return Parser::make_VECTOR_TYPE(fminus::ast::Type::Float4, yyextra->where); }
"float8"                    { return Parser::make_VECTOR_TYPE(fminus::ast::Type::Float8, yyextra->where); }
"poly"                      { return Parser::make_POLY(yyextra->where); }
"true"                      { return Parser::make_TRUE(yyextra->where); }
"false"                     { return Parser::make_FALSE(yyextra->where); }
"if"                        { return Parser::make_IF(yyextra->where); }
"else"                      { return Parser::make_ELSE(yyextra->where); }
"while"                     { return Parser::make_WHILE(yyextra->where); }
"for"                       { return Parser::make_FOR(yyextra->where); }
"break"                     { return Parser::make_BREAK(yyextra->where); }
"continue"                  { return Parser::make_CONTINUE(yyextra->where); }
"module"                    { return Parser::make_MODULE(yyextra->where); }
"this"                      { return Parser::make_THIS(yyextra->where); }
"param"                     { return Parser::make_PARAM(yyextra->where); }

{DIGITS}"."[0-9]*{EXPONENT}?[fF]?   |
"."{DIGITS}{EXPONENT}?[fF]?         |
{DIGITS}{EXPONENT}[fF]?             |
{DIGITS}                            |
0[xX]{HEXDIGITS}                    |
0[xX]({HEXDIGITS}("."[0-9a-fA-F]*)?|"."{HEXDIGITS}){BINEXPONENT}[fF]? {
                                return Parser::make_NUMBER(std::string(yytext, yyleng), yyextra->where);
                            }
{DIGITS}[fF]                {
                                const std::string suffix(1, yytext[yyleng - 1]);
                                throw Parser::syntax_error(yyextra->where,
                                                           "invalid suffix '" + suffix + "' on integer constant");
                            }
{IDENTIFIER}                {
                                std::string name(yytext, yyleng);
                                if (const fminus::ast::Module* module = yyextra->context->moduleNamed(name))
                                    return Parser::make_MODULE_NAME(fminus::ast::Type::moduleType(module),
                                                                    yyextra->where);
                                return Parser::make_IDENTIFIER(std::move(name), yyextra->where);
                            }

"++"                        { return Parser::make_INCREMENT(yyextra->where); }
"--"                        { return Parser::make_DECREMENT(yyextra->where); }
"+="                        { return Parser::make_PLUS_ASSIGN(yyextra->where); }
"-="                        { return Parser::make_MINUS_ASSIGN(yyextra->where); }
"*="                        { return Parser::make_STAR_ASSIGN(yyextra->where); }
"/="                        { return Parser::make_SLASH_ASSIGN(yyextra->where); }
"%="                        { return Parser::make_PERCENT_ASSIGN(yyextra->where); }
"->"                        { return Parser::make_ARROW(yyextra->where); }
"+"                         { return Parser::make_PLUS(yyextra->where); }
"<="                        { return Parser::make_LE(yyextra->where); }
">="                        { return Parser::make_GE(yyextra->where); }
"=="                        { return Parser::make_EQ(yyextra->where); }
"!="                        { return Parser::make_NE(yyextra->where); }
"&&"                        { return Parser::make_AND(yyextra->where); }
"||"                        { return Parser::make_OR(yyextra->where); }
"<"                         { return Parser::make_LT(yyextra->where); }
">"                         { return Parser::make_GT(yyextra->where); }
"!"                         { return Parser::make_NOT(yyextra->where); }
"&"                         { return Parser::make_AMPERSAND(yyextra->where); }
"?"                         { return Parser::make_QUESTION(yyextra->where); }
"."                         { return Parser::make_DOT(yyextra->where); }
"::"                        { return Parser::make_COLONCOLON(yyextra->where); }
":"                         { return Parser::make_COLON(yyextra->where); }
"-"                         { return Parser::make_MINUS(yyextra->where); }
"*"                         { return Parser::make_STAR(yyextra->where); }
"/"                         { return Parser::make_SLASH(yyextra->where); }
"%"                        { return Parser::make_PERCENT(yyextra->where); }
"="                         { return Parser::make_ASSIGN(yyextra->where); }
"("                         { return Parser::make_LPAREN(yyextra->where); }
")"                         { return Parser::make_RPAREN(yyextra->where); }
"["                         { return Parser::make_LBRACKET(yyextra->where); }
"]"                         { return Parser::make_RBRACKET(yyextra->where); }
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
