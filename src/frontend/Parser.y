// src/frontend/Parser.y: the grammar of FMinus v0, a subset of C (Bison 3.8: C++, variant semantic values moved
// automatically, token constructors, locations, detailed errors with LAC). It builds the syntax tree (Ast.h) into the
// ParseContext.
//
//     program      : (function | module)*
//     function     : "const"? type pointer IDENTIFIER "(" params ")" attributes (block | ";")    or a prototype
//     module       : "module" IDENTIFIER "{" (declaration | method)* "}" ";"      the declarations are its fields
//     method       : "const"? type pointer IDENTIFIER "(" params ")" attributes block
//     attributes   : ("[" "[" attribute ("," attribute)* "]" "]")*      attribute: IDENTIFIER ("::" IDENTIFIER)?
//     params       : (param ("," param)*)? | "void"                                  (void) is none, as in C
//     param        : "const"? type pointer IDENTIFIER? | "const"? type IDENTIFIER "[" NUMBER? "]"   a pointer
//     type         : "float" | "bool" | "int" | "void" | VECTOR_TYPE | "poly" ("float" | "bool") | MODULE_NAME
//     pointer      : ("*" ("const" | "restrict")*)?        with a *, the first const is the pointee's (const float*),
//                                                          and one after it the pointer's own (float* const)
//     block        : "{" (declaration | statement)* "}"
//     declaration  : "const"? type declarator ("," declarator)* ";"
//     declarator   : pointer IDENTIFIER ("=" expr)?                                  as in C, `float *p, x;`
//                  | IDENTIFIER "[" NUMBER "]" ("=" "{" (expr ("," expr)* ","?)? "}")?        an array
//                  | pointer IDENTIFIER "=" "{" (expr ("," expr)*)? "}"     parsed, for its error: only arrays
//     statement    : block | expr ";" | "return" expr? ";" | ";"
//                  | "if" "(" expr ")" statement ("else" statement)? | "while" "(" expr ")" statement
//                  | "for" "(" (declaration | expr? ";") expr? ";" expr? ")" statement | "break" ";" | "continue" ";"
//     expr         : NUMBER | "true" | "false" | IDENTIFIER | IDENTIFIER "(" (expr ("," expr)*)? ")" | "(" expr ")"
//                  | ("-" | "+" | "!" | "*" | "&") expr | expr "[" expr "]"         *p, &x, p[i]
//                  | "sizeof" "(" "const"? type pointer ")"                          a type's size, an int
//                  | expr ("*" | "/" | "%") expr | expr ("+" | "-") expr
//                  | expr ("<" | "<=" | ">" | ">=") expr | expr ("==" | "!=") expr
//                  | expr "&&" expr | expr "||" expr | expr "?" expr ":" expr
//                  | "(" VECTOR_TYPE ")" "(" (expr ("," expr)*)? ")"                 OpenCL C's vector literal
//                  | "(" VECTOR_TYPE ")" "{" (expr ("," expr)* ","?)? "}"            C's: lanes left out are 0
//                  | "(" "poly" "float" ")" "(" expr ")" | expr "." IDENTIFIER       a broadcast; a swizzle or field
//                  | expr "->" IDENTIFIER | expr ("." | "->") IDENTIFIER "(" (expr ("," expr)*)? ")" | "this"
//                  | ("++" | "--") expr | expr ("++" | "--") | "(" ("float" | "int" | "bool") ")" expr    a cast
//                  | expr ("=" | "+=" | "-=" | "*=" | "/=" | "%=") expr
//
// Precedence and associativity are C's, from the loosest: assignment (right to left), ?: (right to left), ||, &&,
// equality, relational, additive, multiplicative, unary (and casts), and the postfix operators: ".", "->", "[" and
// "++" and "--" after their operand.
// A module's name is a type from its declaration on: the scanner returns it as MODULE_NAME (ParseContext.h), as a C
// compiler returns a typedef's name, so `OnePole* p;` is a declaration and `a * b;` a product.
// The grammar accepts any expression on the left of an assignment; the code generator rejects what is not an lvalue
// (a variable, an element p[i] or *p, components of either: "expression is not assignable"), so fields can join it.
//
// To add a construct: its tokens in Lexer.l and here, a rule here, a node in Ast.h, its dump in Ast.cpp and its
// lowering in src/ir/ (Lowering.h says which file). Bison's report (build tree: src/generated/frontend/Parser.output)
// and -Wcounterexamples explain any conflict.
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
        // A declarator's `*` and the qualifiers after it: `* const restrict`. Without the *, nothing.
        struct Pointer
        {
            bool isPointer = false;
            bool isConst = false;      // the pointer's own: `float* const p`
            bool isRestrict = false;
        };

        // One name of a declaration (`*p = 0` in `float *p = 0, x;`), before the declaration knows its type.
        struct Declarator
        {
            SourceLocation location;
            Pointer pointer;
            std::string name;
            ast::ExprPtr init;      // null without an initialiser
            int count = 0;          // an array's elements; 0 for a name that is not an array
            bool hasList = false;   // `= {...}`
            std::vector<ast::ExprPtr> list = {};
        };

        // A module's body as it is parsed: its fields, and its methods (Functions named OnePole::tick).
        struct ModuleBody
        {
            std::vector<ast::Field> fields;
            std::vector<ast::Function> methods;
        };

        // A whole module declaration: the module, which the program already holds, and its methods, which go into
        // the program's functions where the module is.
        struct ModuleDeclaration
        {
            ast::Module* module = nullptr;
            std::vector<ast::Function> methods;
        };

        // What a name is declared as, from the declaration's `const` and type and the name's pointer: `const float x`
        // is a const float; `const float* p` is a pointer to const floats, and not itself const; `float* const p` is
        // a const pointer.
        struct Declared
        {
            ast::Type type;
            bool isConst;
            bool isRestrict;
        };

        inline Declared declared(bool isConst, ast::Type type, const Pointer& pointer)
        {
            if (!pointer.isPointer)
                return {type, isConst, false};
            return {ast::Type::pointerTo(type, isConst), pointer.isConst, pointer.isRestrict};
        }
    } // namespace fminus::frontend
}

%code {
    #include "frontend/Scanner.h"   // yylex

    #include <charconv>
    #include <memory>
    #include <system_error>
    #include <utility>

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

        fminus::ast::ExprPtr increment(const fminus::frontend::location& where, fminus::ast::IncrementOp op,
                                       fminus::ast::ExprPtr target)
        {
            return fminus::ast::makeExpr(at(where), fminus::ast::IncrementExpr{op, std::move(target)});
        }

        // An array's size, a positive integer constant (C's, in decimal, octal or hex). Reports the error, and gives
        // 1, for anything else.
        int arraySize(const fminus::frontend::location& where, const std::string& spelling,
                      fminus::frontend::ParseContext& context)
        {
            const bool hex = spelling.size() > 1 && spelling[0] == '0' && (spelling[1] == 'x' || spelling[1] == 'X');
            if (spelling.find_first_of(hex ? ".pP" : ".eEfF") != std::string::npos)
            {
                context.diagnostics.error(at(where), "size of array has non-integer type 'float'");
                return 1;
            }
            const int base = hex ? 16 : spelling.size() > 1 && spelling[0] == '0' ? 8 : 10;
            long long value = 0;
            const char* first = spelling.data() + (hex ? 2 : 0);
            const char* last = spelling.data() + spelling.size();
            const auto [end, failure] = std::from_chars(first, last, value, base);
            if (end != last || failure != std::errc{} || value > (1 << 30))
            {
                context.diagnostics.error(at(where), "array is too large: FMinus's arrays have at most 2^30 elements");
                return 1;
            }
            if (value == 0)
            {
                context.diagnostics.error(at(where), "zero size arrays are not allowed");
                return 1;
            }
            return static_cast<int>(value);
        }

        // The name declared with its pointer, and, for an array, the array of what the rest declares: `float x[4]`
        // is a float[4], and `const float c[2]` a const float[2], whose elements are read-only.
        fminus::frontend::Declared declaredName(bool isConst, fminus::ast::Type type,
                                                fminus::frontend::Declarator& d,
                                                fminus::frontend::ParseContext& context)
        {
            fminus::frontend::Declared name = fminus::frontend::declared(isConst, type, d.pointer);
            if (d.count == 0)
                return name;
            if (d.pointer.isPointer)
                context.diagnostics.error(d.location, "FMinus has no arrays of pointers");
            else
                name.type = fminus::ast::Type::arrayOf(type, d.count);
            return name;
        }
    } // namespace
}

%token FLOAT "float" CONST "const" RETURN "return" BOOL "bool" INT "int" VOID "void" TRUE "true" FALSE "false"
%token RESTRICT "restrict" AMPERSAND "&" LBRACKET "[" RBRACKET "]" SIZEOF "sizeof" COLONCOLON "::"
%token LT "<" LE "<=" GT ">" GE ">=" EQ "==" NE "!=" NOT "!" AND "&&" OR "||" QUESTION "?" COLON ":"
%token IF "if" ELSE "else" WHILE "while" FOR "for" BREAK "break" CONTINUE "continue"
%token MODULE "module" THIS "this" ARROW "->"
%token <ast::Type> MODULE_NAME "module name"
%token <ast::Type> VECTOR_TYPE "vector type"
%token DOT "." POLY "poly"
%token PLUS "+" MINUS "-" STAR "*" SLASH "/" PERCENT "%" INCREMENT "++" DECREMENT "--"
%token ASSIGN "=" PLUS_ASSIGN "+=" MINUS_ASSIGN "-=" STAR_ASSIGN "*=" SLASH_ASSIGN "/=" PERCENT_ASSIGN "%="
%token LPAREN "(" RPAREN ")" LBRACE "{" RBRACE "}" COMMA "," SEMI ";"
%token <std::string> IDENTIFIER "identifier"
%token <std::string> NUMBER "number"

%nterm <std::vector<ast::Function>> items
%nterm <ast::Function> function
%nterm <ModuleDeclaration> module
%nterm <std::string> module_name
%nterm <ModuleBody> module_body module_member
%nterm <int> array_size
%nterm <std::vector<ast::Attribute>> attributes attribute_list
%nterm <ast::Attribute> attribute
%nterm <std::vector<ast::Param>> params param_list
%nterm <ast::Param> param
%nterm <bool> qualifier
%nterm <Pointer> pointer pointer_qualifiers
%nterm <ast::Type> type cast_type
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
%precedence "." "->" "++" "--" "["

%%

program
    : items                                 { parseContext.program.functions = $1; }
    ;

// The program's functions, a module's methods among them where the module is.
items
    : %empty                                {}
    | items function                        { $$ = $1; $$.push_back($2); }
    | items module                          {
                                                $$ = $1;
                                                ModuleDeclaration declaration = $2;
                                                declaration.module->position = $$.size();
                                                for (ast::Function& method : declaration.methods)
                                                    $$.push_back(std::move(method));
                                            }
    ;

// The module joins the program, and its name the types, as soon as the name is read: its fields may point to it.
module
    : "module" module_name                  <ast::Module*>{
                                                auto module = std::make_unique<ast::Module>();
                                                module->location = at(@2);
                                                module->name = $2;
                                                $$ = module.get();
                                                parseContext.moduleNames.emplace($$->name, $$);   // the first stays
                                                parseContext.program.modules.push_back(std::move(module));
                                                parseContext.currentModule = $$;
                                            }
      "{" module_body "}" ";"               {
                                                ModuleBody body = $5;
                                                $$.module = $3;
                                                $$.module->fields = std::move(body.fields);
                                                $$.methods = std::move(body.methods);
                                                parseContext.currentModule = nullptr;
                                            }
    ;

// A second module of a name is the code generator's error (its first declaration keeps the name).
module_name
    : IDENTIFIER                            { $$ = $1; }
    | MODULE_NAME                           { const ast::Type type = $1; $$ = type.module->name; }
    ;

module_body
    : %empty                                {}
    | module_body module_member             {
                                                $$ = $1;
                                                ModuleBody member = $2;
                                                for (ast::Field& field : member.fields)
                                                    $$.fields.push_back(std::move(field));
                                                for (ast::Function& method : member.methods)
                                                    $$.methods.push_back(std::move(method));
                                            }
    ;

// A declaration's names are fields; a function is a method, with `this` before its parameters.
module_member
    : declaration                           {
                                                for (ast::Stmt& stmt : $1)
                                                {
                                                    auto& decl = std::get<ast::DeclStmt>(stmt.node);
                                                    $$.fields.push_back(ast::Field{stmt.location, std::move(decl.name),
                                                                                   decl.type, decl.isConst,
                                                                                   decl.init != nullptr
                                                                                       || decl.hasList});
                                                }
                                            }
    | qualifier type pointer IDENTIFIER "(" params ")" attributes block
                                            {
                                                ast::Module* module = parseContext.currentModule;
                                                const Declared result = declared($1, $2, $3);
                                                const std::string name = $4;
                                                module->methods.push_back(name);
                                                ast::Function method{at(@4), module->name + "::" + name, $6,
                                                                     result.type, $9, true, $8, module};
                                                const ast::Type self =
                                                    ast::Type::pointerTo(ast::Type::moduleType(module), false);
                                                method.params.insert(method.params.begin(),
                                                                     ast::Param{at(@4), "this", self, true, false});
                                                $$.methods.push_back(std::move(method));
                                            }
    ;

function
    : qualifier type pointer IDENTIFIER "(" params ")" attributes block
                                            {
                                                const Declared result = declared($1, $2, $3);
                                                $$ = ast::Function{at(@4), $4, $6, result.type, $9, true, $8};
                                            }
    | qualifier type pointer IDENTIFIER "(" params ")" attributes ";"
                                            {
                                                const Declared result = declared($1, $2, $3);
                                                $$ = ast::Function{at(@4), $4, $6, result.type, {}, false, $8};
                                            }
    ;

// C23's attribute specifiers, after a function's parameter list, where clang's function attributes go:
// `void process(...) [[clang::nonblocking]]`.
attributes
    : %empty                                {}
    | attributes "[" "[" attribute_list "]" "]"
                                            { $$ = $1; for (ast::Attribute& a : $4) $$.push_back(std::move(a)); }
    ;

attribute_list
    : attribute                             { $$.push_back($1); }
    | attribute_list "," attribute          { $$ = $1; $$.push_back($3); }
    ;

attribute
    : IDENTIFIER "::" IDENTIFIER            { $$ = ast::Attribute{at(@1), $1 + "::" + $3}; }
    | IDENTIFIER                            { $$ = ast::Attribute{at(@1), $1}; }
    ;

// `(void)` is C's empty list: one unnamed parameter of type void, which is no parameter.
params
    : %empty                                {}
    | param_list                            {
                                                $$ = $1;
                                                if ($$.size() == 1 && $$[0].name.empty() && !$$[0].isConst
                                                    && $$[0].type == ast::Type::Void)
                                                    $$.clear();
                                            }
    ;

param_list
    : param                                 { $$.push_back($1); }
    | param_list "," param                  { $$ = $1; $$.push_back($3); }
    ;

param
    : qualifier type pointer IDENTIFIER     {
                                                const Declared d = declared($1, $2, $3);
                                                $$ = ast::Param{at(@4), $4, d.type, d.isConst, d.isRestrict};
                                            }
    | qualifier type pointer                {
                                                const Declared d = declared($1, $2, $3);
                                                $$ = ast::Param{at(@2), "", d.type, d.isConst, d.isRestrict};
                                            }
    | qualifier type pointer IDENTIFIER "[" array_size "]"
                                            {
                                                // C's: a parameter declared as an array is a pointer to its first
                                                // element, and the size says nothing.
                                                const Pointer pointer = $3;
                                                if (pointer.isPointer)
                                                    parseContext.diagnostics.error(at(@4),
                                                                                   "FMinus has no arrays of pointers");
                                                $$ = ast::Param{at(@4), $4, ast::Type::pointerTo($2, $1), false, false};
                                            }
    | qualifier type pointer IDENTIFIER "[" "]"
                                            {
                                                const Pointer pointer = $3;
                                                if (pointer.isPointer)
                                                    parseContext.diagnostics.error(at(@4),
                                                                                   "FMinus has no arrays of pointers");
                                                $$ = ast::Param{at(@4), $4, ast::Type::pointerTo($2, $1), false, false};
                                            }
    ;

// A declarator's pointer: nothing, or a * and the qualifiers of the pointer itself.
pointer
    : %empty                                { $$ = Pointer{}; }
    | "*" pointer_qualifiers                { $$ = $2; $$.isPointer = true; }
    ;

pointer_qualifiers
    : %empty                                { $$ = Pointer{}; }
    | pointer_qualifiers "const"            { $$ = $1; $$.isConst = true; }
    | pointer_qualifiers "restrict"         { $$ = $1; $$.isRestrict = true; }
    ;

qualifier
    : %empty                                { $$ = false; }
    | "const"                               { $$ = true; }
    ;

type
    : "float"                               { $$ = ast::Type::Float; }
    | "bool"                                { $$ = ast::Type::Bool; }
    | "int"                                 { $$ = ast::Type::Int; }
    | "void"                                { $$ = ast::Type::Void; }
    | VECTOR_TYPE                           { $$ = $1; }
    | "poly" "float"                        { $$ = ast::Type::PolyFloat; }
    | "poly" "bool"                         { $$ = ast::Type::PolyBool; }
    | MODULE_NAME                           { $$ = $1; }
    ;

// The types a cast names: C casts between scalars (a vector's literal is OpenCL C's (float4)(...)).
cast_type
    : "float"                               { $$ = ast::Type::Float; }
    | "int"                                 { $$ = ast::Type::Int; }
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
                                                {
                                                    const Declared name = declaredName(isConst, type, d, parseContext);
                                                    $$.push_back(ast::Stmt{d.location,
                                                                           ast::DeclStmt{name.isConst, name.type,
                                                                                         std::move(d.name),
                                                                                         std::move(d.init),
                                                                                         name.isRestrict, d.hasList,
                                                                                         std::move(d.list)}});
                                                }
                                            }
    ;

declarators
    : declarator                            { $$.push_back($1); }
    | declarators "," declarator            { $$ = $1; $$.push_back($3); }
    ;

declarator
    : pointer IDENTIFIER                    { $$ = Declarator{at(@2), $1, $2, nullptr}; }
    | pointer IDENTIFIER "=" expr           { $$ = Declarator{at(@2), $1, $2, $4}; }
    | pointer IDENTIFIER "=" "{" args "}"   { $$ = Declarator{at(@2), $1, $2, nullptr, 0, true, $5}; }
    | pointer IDENTIFIER "[" array_size "]" { $$ = Declarator{at(@2), $1, $2, nullptr, $4}; }
    | pointer IDENTIFIER "[" array_size "]" "=" expr
                                            { $$ = Declarator{at(@2), $1, $2, $7, $4}; }
    | pointer IDENTIFIER "[" array_size "]" "=" "{" args "}"
                                            { $$ = Declarator{at(@2), $1, $2, nullptr, $4, true, $8}; }
    | pointer IDENTIFIER "[" array_size "]" "=" "{" arg_list "," "}"
                                            { $$ = Declarator{at(@2), $1, $2, nullptr, $4, true, $8}; }
    ;

array_size
    : NUMBER                                { $$ = arraySize(@1, $1, parseContext); }
    ;

statement
    : block                                 { $$ = ast::Stmt{at(@1), ast::BlockStmt{$1}}; }
    | expr ";"                              { $$ = ast::Stmt{at(@1), ast::ExprStmt{$1}}; }
    | "return" expr ";"                     { $$ = ast::Stmt{at(@1), ast::ReturnStmt{$2}}; }
    | "return" ";"                          { $$ = ast::Stmt{at(@1), ast::ReturnStmt{nullptr}}; }
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
    | "*" expr %prec UNARY                  {
                                                $$ = ast::makeExpr(at(@1),
                                                                   ast::UnaryExpr{ast::UnaryOp::Dereference, $2});
                                            }
    | "&" expr %prec UNARY                  {
                                                $$ = ast::makeExpr(at(@1), ast::UnaryExpr{ast::UnaryOp::AddressOf, $2});
                                            }
    | expr "[" expr "]"                     { $$ = ast::makeExpr(at(@2), ast::IndexExpr{$1, $3}); }
    | "sizeof" "(" qualifier type pointer ")"
                                            {
                                                $$ = ast::makeExpr(at(@1), ast::SizeofExpr{declared($3, $4, $5).type});
                                            }
    | "++" expr %prec UNARY                 { $$ = increment(@1, ast::IncrementOp::PreIncrement, $2); }
    | "--" expr %prec UNARY                 { $$ = increment(@1, ast::IncrementOp::PreDecrement, $2); }
    | expr "++"                             { $$ = increment(@2, ast::IncrementOp::PostIncrement, $1); }
    | expr "--"                             { $$ = increment(@2, ast::IncrementOp::PostDecrement, $1); }
    | "(" cast_type ")" expr %prec UNARY    { $$ = ast::makeExpr(at(@1), ast::CastExpr{$2, $4}); }
    | "true"                                { $$ = ast::makeExpr(at(@1), ast::BoolExpr{true}); }
    | "false"                               { $$ = ast::makeExpr(at(@1), ast::BoolExpr{false}); }
    | expr "<" expr                         { $$ = binary(@2, ast::BinaryOp::Less, $1, $3); }
    | expr ">" expr                         { $$ = binary(@2, ast::BinaryOp::Greater, $1, $3); }
    | expr "<=" expr                        { $$ = binary(@2, ast::BinaryOp::LessEqual, $1, $3); }
    | expr ">=" expr                        { $$ = binary(@2, ast::BinaryOp::GreaterEqual, $1, $3); }
    | expr "==" expr                        { $$ = binary(@2, ast::BinaryOp::Equal, $1, $3); }
    | expr "!=" expr                        { $$ = binary(@2, ast::BinaryOp::NotEqual, $1, $3); }
    | "(" VECTOR_TYPE ")" "(" args ")"      { $$ = ast::makeExpr(at(@1), ast::ConstructExpr{$2, $5, false}); }
    | "(" VECTOR_TYPE ")" "{" args "}"      { $$ = ast::makeExpr(at(@1), ast::ConstructExpr{$2, $5, true}); }
    | "(" VECTOR_TYPE ")" "{" arg_list "," "}"
                                            { $$ = ast::makeExpr(at(@1), ast::ConstructExpr{$2, $5, true}); }
    | "(" "poly" "float" ")" "(" args ")"   {
                                                $$ = ast::makeExpr(at(@1),
                                                                   ast::ConstructExpr{ast::Type::PolyFloat, $6});
                                            }
    | expr "." IDENTIFIER                   { $$ = ast::makeExpr(at(@2), ast::MemberExpr{$1, $3, false}); }
    | expr "->" IDENTIFIER                  { $$ = ast::makeExpr(at(@2), ast::MemberExpr{$1, $3, true}); }
    | expr "." IDENTIFIER "(" args ")"      {
                                                $$ = ast::makeExpr(at(@3), ast::MethodCallExpr{$1, $3, $5, false});
                                            }
    | expr "->" IDENTIFIER "(" args ")"     {
                                                $$ = ast::makeExpr(at(@3), ast::MethodCallExpr{$1, $3, $5, true});
                                            }
    | "this"                                { $$ = ast::makeExpr(at(@1), ast::ThisExpr{}); }
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
