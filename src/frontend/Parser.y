// src/frontend/Parser.y: the grammar of FMinus v0, a subset of C (Bison 3.8: C++, variant semantic values moved
// automatically, token constructors, locations, detailed errors with LAC). It builds the syntax tree (Ast.h) into the
// ParseContext.
//
//     program      : function*
//     function     : type IDENTIFIER "(" (param ("," param)*)? ")" (block | ";")     a definition, or a prototype
//     param        : "const"? type IDENTIFIER?                                       (a prototype may omit names)
//     type         : "float" | "bool"
//     block        : "{" (declaration | statement)* "}"
//     declaration  : "const"? type declarator ("," declarator)* ";"
//     declarator   : IDENTIFIER ("=" expr)?
//     statement    : block | expr ";" | "return" expr ";" | ";"
//                  | "if" "(" expr ")" statement ("else" statement)? | "while" "(" expr ")" statement
//                  | "for" "(" (declaration | expr? ";") expr? ";" expr? ")" statement | "break" ";" | "continue" ";"
//     expr         : NUMBER | "true" | "false" | IDENTIFIER | IDENTIFIER "(" (expr ("," expr)*)? ")" | "(" expr ")"
//                  | ("-" | "+" | "!") expr | expr ("*" | "/" | "%") expr | expr ("+" | "-") expr
//                  | expr ("<" | "<=" | ">" | ">=") expr | expr ("==" | "!=") expr
//                  | expr "&&" expr | expr "||" expr | expr "?" expr ":" expr
//                  | expr ("=" | "+=" | "-=" | "*=" | "/=" | "%=") expr
//
// Precedence and associativity are C's, from the loosest: assignment (right to left), ?: (right to left), ||, &&,
// equality, relational, additive, multiplicative, unary.
// The grammar accepts any expression on the left of an assignment; the code generator rejects what is not a variable
// ("expression is not assignable"), so buffer elements and fields can join it later.
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

    namespace fminus::frontend
    {
        // One name of a declaration (`a = 1` in `float a = 1, b;`), before the declaration knows its type.
        struct Declarator
        {
            SourceLocation location;
            std::string name;
            ast::ExprPtr init;   // null without an initialiser
        };
    } // namespace fminus::frontend
}

%code {
    #include "frontend/Scanner.h"   // yylex

    namespace
    {
        fminus::SourceLocation at(const fminus::frontend::location& where)
        {
            return {where.begin.line, where.begin.column};
        }

        fminus::ast::ExprPtr binary(const fminus::frontend::location& where, fminus::ast::BinaryOp op,
                                    fminus::ast::ExprPtr lhs, fminus::ast::ExprPtr rhs)
        {
            return fminus::ast::makeExpr(at(where), fminus::ast::BinaryExpr{op, std::move(lhs), std::move(rhs)});
        }

        fminus::ast::ExprPtr logical(const fminus::frontend::location& where, fminus::ast::LogicalOp op,
                                     fminus::ast::ExprPtr lhs, fminus::ast::ExprPtr rhs)
        {
            return fminus::ast::makeExpr(at(where), fminus::ast::LogicalExpr{op, std::move(lhs), std::move(rhs)});
        }

        fminus::ast::ExprPtr assign(const fminus::frontend::location& where, fminus::ast::AssignOp op,
                                    fminus::ast::ExprPtr target, fminus::ast::ExprPtr value)
        {
            return fminus::ast::makeExpr(at(where), fminus::ast::AssignExpr{op, std::move(target), std::move(value)});
        }
    } // namespace
}

%token FLOAT "float" CONST "const" RETURN "return" BOOL "bool" TRUE "true" FALSE "false"
%token LT "<" LE "<=" GT ">" GE ">=" EQ "==" NE "!=" NOT "!" AND "&&" OR "||" QUESTION "?" COLON ":"
%token IF "if" ELSE "else" WHILE "while" FOR "for" BREAK "break" CONTINUE "continue"
%token PLUS "+" MINUS "-" STAR "*" SLASH "/" PERCENT "%"
%token ASSIGN "=" PLUS_ASSIGN "+=" MINUS_ASSIGN "-=" STAR_ASSIGN "*=" SLASH_ASSIGN "/=" PERCENT_ASSIGN "%="
%token LPAREN "(" RPAREN ")" LBRACE "{" RBRACE "}" COMMA "," SEMI ";"
%token <std::string> IDENTIFIER "identifier"
%token <std::string> NUMBER "number"

%nterm <std::vector<ast::Function>> functions
%nterm <ast::Function> function
%nterm <std::vector<ast::Param>> params param_list
%nterm <ast::Param> param
%nterm <bool> qualifier
%nterm <ast::Type> type
%nterm <std::vector<ast::Stmt>> block block_items block_item declaration
%nterm <ast::Stmt> statement
%nterm <std::vector<ast::Stmt>> for_init
%nterm <ast::ExprPtr> optional_expr
%nterm <std::vector<Declarator>> declarators
%nterm <Declarator> declarator
%nterm <ast::ExprPtr> expr
%nterm <std::vector<ast::ExprPtr>> args arg_list

// The dangling else: in `if (a) if (b) s; else t;` the else could close either if. Giving it more precedence than the
// shorter rule (THEN) makes Bison shift it, so it closes the nearest if, as in C.
%precedence THEN
%precedence "else"

%right "=" "+=" "-=" "*=" "/=" "%="
%right "?" ":"
%left "||"
%left "&&"
%left "==" "!="
%left "<" "<=" ">" ">="
%left "+" "-"
%left "*" "/" "%"
%precedence UNARY

%%

program
    : functions                             { parseContext.program.functions = $1; }
    ;

functions
    : %empty                                {}
    | functions function                    { $$ = $1; $$.push_back($2); }
    ;

function
    : type IDENTIFIER "(" params ")" block  { $$ = ast::Function{at(@2), $2, $4, $1, $6, true}; }
    | type IDENTIFIER "(" params ")" ";"    { $$ = ast::Function{at(@2), $2, $4, $1, {}, false}; }
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
    : qualifier type IDENTIFIER             { $$ = ast::Param{at(@3), $3, $2, $1}; }
    | qualifier type                        { $$ = ast::Param{at(@2), "", $2, $1}; }
    ;

qualifier
    : %empty                                { $$ = false; }
    | "const"                               { $$ = true; }
    ;

type
    : "float"                               { $$ = ast::Type::Float; }
    | "bool"                                { $$ = ast::Type::Bool; }
    ;

block
    : "{" block_items "}"                   { $$ = $2; }
    ;

block_items
    : %empty                                {}
    | block_items block_item                { $$ = $1; for (ast::Stmt& item : $2) $$.push_back(std::move(item)); }
    ;

// A block item is a list: a declaration gives one DeclStmt per declarator.
block_item
    : declaration                           { $$ = $1; }
    | statement                             { $$.push_back($1); }
    ;

declaration
    : qualifier type declarators ";"        {
                                                const bool isConst = $1;
                                                const ast::Type type = $2;
                                                for (Declarator& d : $3)
                                                    $$.push_back(ast::Stmt{d.location, ast::DeclStmt{isConst, type,
                                                                           std::move(d.name), std::move(d.init)}});
                                            }
    ;

declarators
    : declarator                            { $$.push_back($1); }
    | declarators "," declarator            { $$ = $1; $$.push_back($3); }
    ;

declarator
    : IDENTIFIER                            { $$ = Declarator{at(@1), $1, nullptr}; }
    | IDENTIFIER "=" expr                   { $$ = Declarator{at(@1), $1, $3}; }
    ;

statement
    : block                                 { $$ = ast::Stmt{at(@1), ast::BlockStmt{$1}}; }
    | expr ";"                              { $$ = ast::Stmt{at(@1), ast::ExprStmt{$1}}; }
    | "return" expr ";"                     { $$ = ast::Stmt{at(@1), ast::ReturnStmt{$2}}; }
    | ";"                                   { $$ = ast::Stmt{at(@1), ast::EmptyStmt{}}; }
    | "if" "(" expr ")" statement %prec THEN
                                            { $$ = ast::Stmt{at(@1), ast::IfStmt{$3, ast::own($5), nullptr}}; }
    | "if" "(" expr ")" statement "else" statement
                                            { $$ = ast::Stmt{at(@1), ast::IfStmt{$3, ast::own($5), ast::own($7)}}; }
    | "while" "(" expr ")" statement        { $$ = ast::Stmt{at(@1), ast::WhileStmt{$3, ast::own($5)}}; }
    | "for" "(" for_init optional_expr ";" optional_expr ")" statement
                                            { $$ = ast::Stmt{at(@1), ast::ForStmt{$3, $4, $6, ast::own($8)}}; }
    | "break" ";"                           { $$ = ast::Stmt{at(@1), ast::BreakStmt{}}; }
    | "continue" ";"                        { $$ = ast::Stmt{at(@1), ast::ContinueStmt{}}; }
    ;

// The first clause of a for: a declaration (its ; included), an expression and ;, or just ;.
for_init
    : declaration                           { $$ = $1; }
    | expr ";"                              { $$.push_back(ast::Stmt{at(@1), ast::ExprStmt{$1}}); }
    | ";"                                   {}
    ;

optional_expr
    : %empty                                { $$ = nullptr; }
    | expr                                  { $$ = $1; }
    ;

expr
    : NUMBER                                { $$ = ast::makeExpr(at(@1), ast::NumberExpr{$1}); }
    | IDENTIFIER                            { $$ = ast::makeExpr(at(@1), ast::VariableExpr{$1}); }
    | IDENTIFIER "(" args ")"               { $$ = ast::makeExpr(at(@1), ast::CallExpr{$1, $3}); }
    | "(" expr ")"                          { $$ = $2; }
    | "-" expr %prec UNARY                  { $$ = ast::makeExpr(at(@1), ast::UnaryExpr{ast::UnaryOp::Negate, $2}); }
    | "+" expr %prec UNARY                  { $$ = ast::makeExpr(at(@1), ast::UnaryExpr{ast::UnaryOp::Plus, $2}); }
    | "!" expr %prec UNARY                  { $$ = ast::makeExpr(at(@1), ast::UnaryExpr{ast::UnaryOp::Not, $2}); }
    | "true"                                { $$ = ast::makeExpr(at(@1), ast::BoolExpr{true}); }
    | "false"                               { $$ = ast::makeExpr(at(@1), ast::BoolExpr{false}); }
    | expr "<" expr                         { $$ = binary(@2, ast::BinaryOp::Less, $1, $3); }
    | expr ">" expr                         { $$ = binary(@2, ast::BinaryOp::Greater, $1, $3); }
    | expr "<=" expr                        { $$ = binary(@2, ast::BinaryOp::LessEqual, $1, $3); }
    | expr ">=" expr                        { $$ = binary(@2, ast::BinaryOp::GreaterEqual, $1, $3); }
    | expr "==" expr                        { $$ = binary(@2, ast::BinaryOp::Equal, $1, $3); }
    | expr "!=" expr                        { $$ = binary(@2, ast::BinaryOp::NotEqual, $1, $3); }
    | expr "*" expr                         { $$ = binary(@2, ast::BinaryOp::Multiply, $1, $3); }
    | expr "/" expr                         { $$ = binary(@2, ast::BinaryOp::Divide, $1, $3); }
    | expr "%" expr                         { $$ = binary(@2, ast::BinaryOp::Remainder, $1, $3); }
    | expr "+" expr                         { $$ = binary(@2, ast::BinaryOp::Add, $1, $3); }
    | expr "-" expr                         { $$ = binary(@2, ast::BinaryOp::Subtract, $1, $3); }
    | expr "=" expr                         { $$ = assign(@2, ast::AssignOp::Assign, $1, $3); }
    | expr "+=" expr                        { $$ = assign(@2, ast::AssignOp::Add, $1, $3); }
    | expr "-=" expr                        { $$ = assign(@2, ast::AssignOp::Subtract, $1, $3); }
    | expr "*=" expr                        { $$ = assign(@2, ast::AssignOp::Multiply, $1, $3); }
    | expr "/=" expr                        { $$ = assign(@2, ast::AssignOp::Divide, $1, $3); }
    | expr "%=" expr                        { $$ = assign(@2, ast::AssignOp::Remainder, $1, $3); }
    | expr "&&" expr                        { $$ = logical(@2, ast::LogicalOp::And, $1, $3); }
    | expr "||" expr                        { $$ = logical(@2, ast::LogicalOp::Or, $1, $3); }
    | expr "?" expr ":" expr                { $$ = ast::makeExpr(at(@2), ast::ConditionalExpr{$1, $3, $5}); }
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
