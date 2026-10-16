// src/frontend/Parser.y: the grammar of FMinus v0 (Bison 3.8: C++, variant semantic values moved automatically,
// token constructors, locations, detailed errors with LAC). It builds the syntax tree (Ast.h) into the ParseContext.
//
//     program    : function*
//     function   : "fn" IDENTIFIER "(" (param ("," param)*)? ")" "->" type "{" statement* "}"
//     param      : IDENTIFIER ":" type
//     type       : "float"
//     statement  : "let" IDENTIFIER "=" expr ";"  |  "return" expr ";"
//     expr       : NUMBER | IDENTIFIER | IDENTIFIER "(" (expr ("," expr)*)? ")"
//                | expr ("+" | "-" | "*" | "/") expr | "-" expr | "(" expr ")"
//
// To add a construct: its tokens in Lexer.l and here, a rule here, a node in Ast.h, its dump in Ast.cpp and its
// lowering in ir/CodeGen.cpp. Bison's report (build tree: src/generated/frontend/Parser.output) and -Wcounterexamples
// explain any conflict.
%require "3.8"
%language "c++"
%header
%locations

%define api.namespace {fminus::frontend}
%define api.parser.class {Parser}
%define api.value.type variant
%define api.value.automove
%define api.token.constructor
%define api.token.prefix {TOK_}
%define parse.assert
%define parse.error detailed
%define parse.lac full

%lex-param {yyscan_t scanner}
%parse-param {yyscan_t scanner} {ParseContext& parseContext}

%code requires {
    #include "frontend/ParseContext.h"

    #include <string>
    #include <vector>

    // Flex's handle of a reentrant scanner (its generated header declares the same type the same way).
    #ifndef YY_TYPEDEF_YY_SCANNER_T
    #define YY_TYPEDEF_YY_SCANNER_T
    typedef void* yyscan_t;
    #endif
}

%code {
    #include "frontend/Scanner.h"   // yylex

    namespace
    {
        fminus::SourceLocation at(const fminus::frontend::location& where)
        {
            return {where.begin.line, where.begin.column};
        }
    } // namespace
}

%token FN "fn" LET "let" RETURN "return" FLOAT "float"
%token ARROW "->" PLUS "+" MINUS "-" STAR "*" SLASH "/" ASSIGN "="
%token LPAREN "(" RPAREN ")" LBRACE "{" RBRACE "}" COMMA "," COLON ":" SEMI ";"
%token <std::string> IDENTIFIER "identifier"
%token <std::string> NUMBER "number"

%nterm <std::vector<ast::Function>> functions
%nterm <ast::Function> function
%nterm <std::vector<ast::Param>> params param_list
%nterm <ast::Param> param
%nterm <ast::Type> type
%nterm <std::vector<ast::Stmt>> statements
%nterm <ast::Stmt> statement
%nterm <ast::ExprPtr> expr
%nterm <std::vector<ast::ExprPtr>> args arg_list

%left "+" "-"
%left "*" "/"
%precedence NEG

%%

program
    : functions                             { parseContext.program.functions = $1; }
    ;

functions
    : %empty                                {}
    | functions function                    { $$ = $1; $$.push_back($2); }
    ;

function
    : "fn" IDENTIFIER "(" params ")" "->" type "{" statements "}"
                                            { $$ = ast::Function{at(@1), $2, $4, $7, $9}; }
    ;

params
    : %empty                                {}
    | param_list                            { $$ = $1; }
    ;

param_list
    : param                                 { $$.push_back($1); }
    | param_list "," param                  { $$ = $1; $$.push_back($3); }
    ;

param
    : IDENTIFIER ":" type                   { $$ = ast::Param{at(@1), $1, $3}; }
    ;

type
    : "float"                               { $$ = ast::Type::Float; }
    ;

statements
    : %empty                                {}
    | statements statement                  { $$ = $1; $$.push_back($2); }
    ;

statement
    : "let" IDENTIFIER "=" expr ";"         { $$ = ast::Stmt{at(@1), ast::LetStmt{$2, $4}}; }
    | "return" expr ";"                     { $$ = ast::Stmt{at(@1), ast::ReturnStmt{$2}}; }
    ;

expr
    : NUMBER                                { $$ = ast::makeExpr(at(@1), ast::NumberExpr{$1}); }
    | IDENTIFIER                            { $$ = ast::makeExpr(at(@1), ast::VariableExpr{$1}); }
    | IDENTIFIER "(" args ")"               { $$ = ast::makeExpr(at(@1), ast::CallExpr{$1, $3}); }
    | expr "+" expr                         { $$ = ast::makeExpr(at(@2), ast::BinaryExpr{ast::BinaryOp::Add, $1, $3}); }
    | expr "-" expr                         { $$ = ast::makeExpr(at(@2), ast::BinaryExpr{ast::BinaryOp::Subtract, $1, $3}); }
    | expr "*" expr                         { $$ = ast::makeExpr(at(@2), ast::BinaryExpr{ast::BinaryOp::Multiply, $1, $3}); }
    | expr "/" expr                         { $$ = ast::makeExpr(at(@2), ast::BinaryExpr{ast::BinaryOp::Divide, $1, $3}); }
    | "-" expr %prec NEG                    { $$ = ast::makeExpr(at(@1), ast::UnaryExpr{ast::UnaryOp::Negate, $2}); }
    | "(" expr ")"                          { $$ = $2; }
    ;

args
    : %empty                                {}
    | arg_list                              { $$ = $1; }
    ;

arg_list
    : expr                                  { $$.push_back($1); }
    | arg_list "," expr                     { $$ = $1; $$.push_back($3); }
    ;

%%

void fminus::frontend::Parser::error(const location_type& where, const std::string& message)
{
    parseContext.diagnostics.error(at(where), message);
}
