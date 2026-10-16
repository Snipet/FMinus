// tests/smoke/calc/Calc.y: the toolchain smoke test's grammar: integer arithmetic, one result per ';'. It shows the
// Bison 3.8 setup a real FMinus grammar can copy:
//   - the C++ skeleton with variant semantic values, token constructors (make_NUMBER, ...) and locations;
//   - a reentrant Flex scanner (Calc.l) passed in as yyscan_t, the two joined by CalcScanner.h;
//   - detailed error messages with LAC, and recovery at ';'.
// It came with the project skeleton; delete it once the real grammar has its own tests.
%require "3.8"
%language "c++"
%header
%locations

%define api.namespace {fminus::smoke}
%define api.parser.class {CalcParser}
%define api.value.type variant
%define api.token.constructor
%define api.token.prefix {TOK_}
%define parse.assert
%define parse.error detailed
%define parse.lac full

%lex-param {yyscan_t scanner}
%parse-param {yyscan_t scanner} {CalcResult& result}

%code requires {
    #include <cstdint>
    #include <string>
    #include <vector>

    // Flex's handle of a reentrant scanner (its generated header declares the same type the same way).
    #ifndef YY_TYPEDEF_YY_SCANNER_T
    #define YY_TYPEDEF_YY_SCANNER_T
    typedef void* yyscan_t;
    #endif

    namespace fminus::smoke
    {
        struct CalcResult
        {
            std::vector<std::int64_t> values;   // one per statement that parsed
            std::vector<std::string> errors;    // "line.column: message"
        };
    } // namespace fminus::smoke
}

%code {
    #include "smoke/calc/CalcScanner.h"   // the scanner's entry point, yylex
}

%token PLUS "+" MINUS "-" STAR "*" SLASH "/" LPAREN "(" RPAREN ")" SEMI ";"
%token <std::int64_t> NUMBER "number"
%nterm <std::int64_t> expr

%left "+" "-"
%left "*" "/"
%precedence NEG

%%

input
    : %empty
    | input statement
    ;

statement
    : expr ";"      { result.values.push_back($1); }
    | error ";"     { yyerrok; }
    ;

expr
    : NUMBER                    { $$ = $1; }
    | expr "+" expr             { $$ = $1 + $3; }
    | expr "-" expr             { $$ = $1 - $3; }
    | expr "*" expr             { $$ = $1 * $3; }
    | expr "/" expr             {
                                    if ($3 == 0)
                                    {
                                        error(@3, "division by zero");
                                        $$ = 0;
                                    }
                                    else
                                        $$ = $1 / $3;
                                }
    | "-" expr %prec NEG        { $$ = -$2; }
    | "(" expr ")"              { $$ = $2; }
    ;

%%

void fminus::smoke::CalcParser::error(const location_type& where, const std::string& message)
{
    result.errors.push_back(std::to_string(where.begin.line) + "." + std::to_string(where.begin.column) + ": "
                            + message);
}
