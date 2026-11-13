// src/frontend/Parser.y: the grammar of FMinus v0, a subset of C (Bison 3.8: C++, variant semantic values moved
// automatically, token constructors, locations, detailed errors with LAC). It builds the syntax tree (Ast.h) into the
// ParseContext.
//
//     program      : (function | module | enumeration | declaration)*     a declaration there is of constants
//     function     : "const"? type pointer IDENTIFIER "(" params ")" attributes (block | ";")    or a prototype
//     module       : "module" IDENTIFIER "{" (declaration | parameter | method | enumeration)* "}" ";"   fields
//     parameter    : "param" type IDENTIFIER attributes ";"                     a field the host sets
//     method       : "const"? type pointer IDENTIFIER "(" params ")" attributes block
//     attributes   : ("[" "[" attribute ("," attribute)* "]" "]")*
//     attribute    : attribute_word ("::" attribute_word)? ("(" attribute_arg ("," attribute_arg)* ")")?
//     attribute_word: IDENTIFIER | MODULE_NAME | a keyword or a type's word  C23 reads a keyword there as a name
//     attribute_arg: "-"? NUMBER | STRING+                 a number, or a string (C's adjacent literals are one)
//     params       : (param ("," param)*)? | "void"                                  (void) is none, as in C
//     param        : "const"? type pointer IDENTIFIER? | "const"? type IDENTIFIER array_sizes       a pointer
//     type         : "float" | "double" | "bool" | "int" | "void" | VECTOR_TYPE | "poly" ("float" | "bool")
//                  | MODULE_NAME | HANDLE_TYPE              http_response, json_doc, ...: the runtime's handles
//                  | "char" | "string"                  a byte, an element or a pointee only; a view of bytes
//                  | "enum" IDENTIFIER                  int (or its fixed type), the name a tag declared before
//     pointer      : ("*" ("const" | "restrict")*)?        with a *, the first const is the pointee's (const float*),
//                                                          and one after it the pointer's own (float* const)
//     block        : "{" (declaration | enumeration | unlabeled | label)* "}"     C23's: a label is an item
//     enumeration  : "const"? "enum" IDENTIFIER? (":" type)? "{" enumerator ("," enumerator)* ","? "}"
//                    (declarator ("," declarator)*)? ";"        C's, names of its type after it; C23's fixed type
//                  | "const"? "enum" IDENTIFIER ":" type ";"         C23's: a tag with a fixed type, ahead of its list
//     enumerator   : IDENTIFIER ("=" expr)?                                  the expr an integer constant expression
//     declaration  : "const"? type declarator ("," declarator)* ";"
//     declarator   : pointer IDENTIFIER attributes ("=" expr)?            as in C, `float *p, x;`; C23's attributes
//                  | IDENTIFIER attributes array_sizes ("=" initializer_list)?        an array
//                  | pointer IDENTIFIER attributes "=" initializer_list        parsed, for its error: only arrays
//     initializer_list : "{" (initializer ("," initializer)* ","?)? "}"              C's, C23's {} too
//     initializer  : expr | initializer_list                         a braced list, a 2-D array's row
//     array_sizes  : array_size array_size?                         FMinus's arrays are one or two dimensions deep
//     array_size   : "[" expr? "]"              an integer constant expression, or none: the list's length
//     statement    : label statement | unlabeled
//     unlabeled    : block | expr ";" | "return" expr? ";" | ";"
//                  | "if" "(" expr ")" statement ("else" statement)? | "while" "(" expr ")" statement
//                  | "for" "(" (declaration | expr? ";") expr? ";" expr? ")" statement | "break" ";" | "continue" ";"
//                  | "switch" "(" expr ")" statement
//                  | ("if" | "switch") "(" (declaration expr | "const"? type declarator) ")" ...   C2y's: names
//                                                        in scope to the statement's end, the else's too
//                  | "parallel" "for" "(" (declaration | expr? ";") expr? ";" expr? ")" statement
//                  | "parallel" block                    FMinus's: the iterations, or the statements, at once
//                  | ("defer" | "_Defer") statement      TS 25755's: run when the block it is in is left
//                  | "serve" "(" expr ("," expr)? ")" "{" route* "}"    FMinus's: the requests waiting for its routes
//                  | "reply" expr ("," expr)* ";"        answers the route's request, and leaves the route
//                  | "when" "(" expr ")" statement       FMinus's: run each time the condition becomes true
//                  | "within" "(" expr ")" block         FMinus's: every request started in it ended at its end
//     route        : "route" STRING+ ("(" (route_param ("," route_param)*)? ")")? attributes block
//     route_param  : "const"? type pointer IDENTIFIER attributes        an int a {hole} or the query fills, ...
//     label        : "case" expr ":" | "default" ":"                         the expr an integer constant expression
//     expr         : NUMBER | "true" | "false" | IDENTIFIER | IDENTIFIER "(" (expr ("," expr)*)? ")" | "(" expr ")"
//                  | STRING+          a string literal: a builtin's argument, or a value where a string is wanted
//                  | ("-" | "+" | "!" | "~" | "*" | "&") expr | expr "[" expr "]"   *p, &x, p[i]
//                  | "sizeof" "(" "const"? type pointer array_sizes? ")"             a type's size, an int
//                  | expr ("*" | "/" | "%") expr | expr ("+" | "-") expr | expr ("<<" | ">>") expr
//                  | expr ("<" | "<=" | ">" | ">=") expr | expr ("==" | "!=") expr
//                  | expr "&" expr | expr "^" expr | expr "|" expr                   an int's bits
//                  | expr "&&" expr | expr "||" expr | expr "?" expr ":" expr
//                  | "(" VECTOR_TYPE ")" "(" (expr ("," expr)*)? ")"                 OpenCL C's vector literal
//                  | "(" VECTOR_TYPE ")" "{" (expr ("," expr)* ","?)? "}"            C's: lanes left out are 0
//                  | "(" "poly" "float" ")" "(" expr ")" | expr "." IDENTIFIER       a broadcast; a swizzle or field
//                  | expr "->" IDENTIFIER | expr ("." | "->") IDENTIFIER "(" (expr ("," expr)*)? ")" | "this"
//                  | ("++" | "--") expr | expr ("++" | "--")
//                  | "(" ("float" | "double" | "int" | "bool") ")" expr                       a cast
//                  | "(" "enum" IDENTIFIER ")" expr                                  a cast to int, as C's to an enum
//                  | expr ("=" | "+=" | "-=" | "*=" | "/=" | "%=" | "&=" | "|=" | "^=" | "<<=" | ">>=") expr
//                  | "json" json_object | "json" json_array     a JSON literal: a sink's argument, never a value
//                  | "json" "(" expr ")"                         one value written at the top: json (preset)
//     json_object  : "{" (json_member ("," json_member)* ","?)? "}"
//     json_member  : STRING+ ":" json_value | IDENTIFIER           a name alone is its own key and its value
//     json_array   : "[" (json_value ("," json_value)* ","?)? "]"
//     json_value   : expr | json_object | json_array
//
// Precedence and associativity are C's, from the loosest: assignment (right to left), ?: (right to left), ||, &&, |,
// ^, &, equality, relational, shifts, additive, multiplicative, unary (and casts), and the postfix operators: ".",
// "->", "[" and "++" and "--" after their operand. So `a & b == c` is a & (b == c), as in C, and `a << 1 + 2` is
// a << 3. The unary "&" and "-" take UNARY's precedence (%prec), their binary ones their own.
// A string literal is an expression to the grammar, as in C, so a call's arguments may be one. The code generator
// takes it as the argument of a builtin's string parameter (ir/Calls.cpp), or as a value of FMinus's type string where
// one is wanted (an initialiser, an assignment, an argument, an operand of == or !=, a case), and refuses it anywhere
// else, each error at the literal. So the grammar stays C's.
// A module's name is a type from its declaration on: the scanner returns it as MODULE_NAME (ParseContext.h), as a C
// compiler returns a typedef's name, so `OnePole* p;` is a declaration and `a * b;` a product.
// The grammar accepts any expression on the left of an assignment; the code generator rejects what is not an lvalue
// (a variable, an element p[i] or *p, components of either: "expression is not assignable"), so fields can join it.
// A case or default label is C23's: a block item of its own, so a declaration may follow one and one may end a block;
// where a statement is expected (an if's branch, a switch's body), `case 1: s` is the label and its statement, a
// CaseStmt with a body. The code generator checks that a label is in a switch, and that a case's value is an integer
// constant expression.
// An array's size is an expression, `float buf[ROWS * COLS]`, which the code generator works out: an integer constant
// expression, whose enumeration constants only it knows the values of. A declaration at the top level declares
// constants, `const float table[] = {...};`: Program::constants, where they are among the functions, modules and
// enumerations, for the code generator to give each function and method those declared before it.
// An enumeration is declared at the top level, in a block or in a module, and may declare names of its type after its
// list, as C's declares variables: `enum Mode { A, B } mode;` is the enumeration, then the declaration
// `enum Mode mode;` (constants at the top level, fields in a module). Its tag goes into the parse context's scopes, a
// block's from its `{` to its `}` (a module's into the file's, as a C struct's), from its closing brace on, where
// `enum Mode` names its enumerated type: int, with which enumeration it is (ast::Type::enumerated). C23's fixed
// underlying type, `enum Mode : int { ... }` or `: bool`, is the type instead, and declares the tag ahead of its list
// too, `enum Mode : int;` (ParseContext's Tag). Its constants stay identifiers, which the code generator finds in its
// scopes, as it finds variables: in C they share one name space, and a local may hide one.
//
// To add a construct: its tokens in Lexer.l and here, a rule here, a node in Ast.h, its dump in Ast.cpp and its
// lowering in src/ir/ (Lowering.h says which file). Bison's report (build tree: src/generated/frontend/Parser.output)
// and -Wcounterexamples explain any conflict.
%require "3.8"
%language "c++"
%header
%locations

%define api.namespace {fminus::frontend}
%define api.filename.type {const fminus::frontend::SourceFile}   // which file a location is in (ParseContext.h)
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

    #include <optional>
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
            ast::ExprPtr init;             // null without an initialiser
            bool isArray = false;          // `name[size]` or `name[]`
            ast::ExprPtr size = nullptr;   // an array's, as written; null for `[]`
            bool hasList = false;          // `= {...}`
            std::vector<ast::ExprPtr> list = {};
            bool rows = false;                // `name[size][rowSize]`: a 2-D array
            ast::ExprPtr rowSize = nullptr;   // its rows' size, as written; null for `[]`, which C refuses
            std::vector<ast::Brace> braces = {};
            std::vector<ast::Attribute> attributes = {};   // C23's, after the name: `level [[ui::unit("dB")]]`
            SourceLocation comma = {};   // the comma before it, in a declaration's list after the first
        };

        // A route's parameter list as written: none at all, or a list, `()` too.
        struct RouteParams
        {
            bool written = false;
            std::vector<ast::RouteParam> params = {};
        };

        // An if's or a switch's head (C2y's, N3356): the declarations before its condition, and the condition.
        struct SelectionHead
        {
            std::vector<ast::Stmt> init;
            ast::ExprPtr condition;
        };

        // An array's sizes as written: `[size]`, or a 2-D array's `[size][rowSize]` (`rows`). Null for `[]`.
        struct ArraySizes
        {
            ast::ExprPtr size = nullptr;
            bool rows = false;
            ast::ExprPtr rowSize = nullptr;
            std::vector<ast::Attribute> attributes = {};   // a declarator's after its sizes: its type's, an error
        };

        // An initialiser list as written, `{1, {2, 3}}`: its values in order, and the braces among them (ast::Brace).
        struct InitList
        {
            std::vector<ast::ExprPtr> items;
            std::vector<ast::Brace> braces;
        };

        // A string literal's token (Lexer.l's stringValue): its characters, C's escapes resolved, and the offset in the
        // token, from its opening quote, at which each one is spelt.
        struct StringToken
        {
            std::string value;
            std::vector<int> offsets;
        };

        // C23's fixed underlying type of an enumeration as written, `: int`: the type and where; or none.
        struct EnumBase
        {
            bool fixed = false;
            ast::Type type = ast::Type::Int;
            SourceLocation location = {};
        };

        // An enumeration up to its closing brace, and its declaration's `const`, for the names declared after it.
        struct EnumHead
        {
            ast::Enum enumeration;
            bool isConst = false;
        };

        // An enumeration's declaration: the enumeration (none for C23's tag declared ahead of its list), and the
        // names declared with its type after its list, `enum Mode { A, B } mode;`, as a declaration's DeclStmts.
        struct EnumDeclaration
        {
            std::optional<ast::Enum> enumeration;
            std::vector<ast::Stmt> declarations;
        };

        // A module's body as it is parsed: its fields, its methods (Functions named OnePole::tick) and its
        // enumerations.
        struct ModuleBody
        {
            std::vector<ast::Field> fields;
            std::vector<ast::Function> methods;
            std::vector<ast::Enum> enums;
        };

        // A whole module declaration: the module, which the program already holds, and its methods, which go into
        // the program's functions where the module is, and its enumerations, which go into the program's.
        struct ModuleDeclaration
        {
            ast::Module* module = nullptr;
            std::vector<ast::Function> methods;
            std::vector<ast::Enum> enums;
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

    #include <memory>
    #include <utility>

    namespace
    {
        fminus::SourceLocation at(const fminus::frontend::location& where)
        {
            return fminus::frontend::sourceLocation(where);
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

        // C23's fixed underlying type, checked: an integer type that is no enumerated type, which in FMinus is int or
        // bool. Another is clang's error, and the enumeration's type is then int.
        fminus::ast::Type underlyingType(const fminus::frontend::EnumBase& base,
                                         fminus::frontend::ParseContext& context)
        {
            const fminus::ast::Type type = base.type;
            if (type.enumeration != 0)
                context.diagnostics.error(base.location, "an enumerated type is no underlying type: C23 takes an "
                                                         "integer type that is not an enumeration");
            else if (type == fminus::ast::Type::Int || type == fminus::ast::Type::Bool)
                return type;
            else
                context.diagnostics.error(base.location, "non-integral type '" + fminus::ast::typeName(type)
                                                             + "' is an invalid underlying type");
            return fminus::ast::Type::Int;
        }

        // A tag declared again in its scope, `fixed` its fixed underlying type or none: C23's rule that the two agree,
        // in clang's words, with a note at the declaration before. True when they do.
        bool sameTag(const fminus::frontend::Tag& previous, const std::optional<fminus::ast::Type>& fixed,
                     fminus::SourceLocation where, fminus::frontend::ParseContext& context)
        {
            const std::vector<fminus::Diagnostic> before{
                fminus::Diagnostic{previous.location, "previous declaration is here", {}}};
            if (previous.fixed.has_value() != fixed.has_value())
            {
                context.diagnostics.error(where,
                                          std::string("enumeration previously declared with ")
                                              + (previous.fixed ? "fixed" : "nonfixed") + " underlying type",
                                          before);
                return false;
            }
            if (fixed && *fixed != *previous.fixed)
            {
                context.diagnostics.error(where,
                                          "enumeration redeclared with different underlying type '"
                                              + fminus::ast::typeName(*fixed) + "' (was '"
                                              + fminus::ast::typeName(*previous.fixed) + "')",
                                          before);
                return false;
            }
            return true;
        }

        // An enumeration as parsed, at its closing brace: its id, its fixed underlying type if it has one, and its
        // tag, if it has one, declared in the innermost scope, where a second definition of the name is clang's
        // error. A tag C23 declared there ahead of its list (forwardEnum) gets its list here, and keeps its id.
        fminus::ast::Enum declaredEnum(const fminus::frontend::location& keyword,
                                       const fminus::frontend::location& name, std::string tag,
                                       const fminus::frontend::EnumBase& base,
                                       std::vector<fminus::ast::Enumerator> enumerators,
                                       fminus::frontend::ParseContext& context)
        {
            fminus::ast::Enum enumeration;
            enumeration.location = at(tag.empty() ? keyword : name);
            enumeration.fixed = base.fixed;
            if (base.fixed)
                enumeration.underlying = underlyingType(base, context);
            const std::optional<fminus::ast::Type> fixed =
                base.fixed ? std::optional(enumeration.underlying) : std::nullopt;
            enumeration.id = ++context.enumerations;
            if (!tag.empty())
            {
                auto& scope = context.tags.back();
                const auto found = scope.find(tag);
                if (found == scope.end())
                    scope.emplace(tag, fminus::frontend::Tag{enumeration.location, enumeration.id, fixed, true});
                else if (sameTag(found->second, fixed, enumeration.location, context))
                {
                    if (found->second.defined)
                        context.diagnostics.error(
                            enumeration.location, "redefinition of '" + tag + "'",
                            {fminus::Diagnostic{found->second.location, "previous definition is here", {}}});
                    else
                    {
                        found->second.location = enumeration.location;
                        found->second.defined = true;
                        enumeration.id = found->second.id;
                    }
                }
            }
            enumeration.tag = std::move(tag);
            enumeration.enumerators = std::move(enumerators);
            return enumeration;
        }

        // C23's `enum Mode : int;`: the tag declared ahead of its list, in the innermost scope, a type from here on.
        // Without a fixed type, C has no such declaration but of a tag already defined (C11 6.7.2.3p3), which declares
        // nothing; a tag must name it.
        void forwardEnum(const fminus::frontend::location& end, const fminus::frontend::location& name,
                         const std::string& tag, const fminus::frontend::EnumBase& base,
                         fminus::frontend::ParseContext& context)
        {
            if (tag.empty())
            {
                context.diagnostics.error(at(end), "unnamed enumeration must be a definition");
                return;
            }
            auto& scope = context.tags.back();
            const auto found = scope.find(tag);
            if (!base.fixed)
            {
                if (context.tagNamed(tag) == nullptr)
                    context.diagnostics.error(at(name), "ISO C forbids forward references to 'enum' types");
                else if (found != scope.end())
                    sameTag(found->second, std::nullopt, at(name), context);
                return;
            }
            const fminus::ast::Type underlying = underlyingType(base, context);
            if (found == scope.end())
                scope.emplace(tag, fminus::frontend::Tag{at(name), ++context.enumerations, underlying, false});
            else
                sameTag(found->second, underlying, at(name), context);
        }

        // `enum Mode`, a type: its enumerated type (int, or its fixed type, with its enumeration's id), when a tag of
        // the name is visible. C has no enumeration declared ahead of its constants but C23's with a fixed underlying
        // type, and neither has FMinus.
        fminus::ast::Type enumType(const fminus::frontend::location& name, const std::string& tag,
                                   fminus::frontend::ParseContext& context)
        {
            const fminus::frontend::Tag* found = context.tagNamed(tag);
            if (found == nullptr)
            {
                context.diagnostics.error(at(name), "ISO C forbids forward references to 'enum' types");
                return fminus::ast::Type::Int;
            }
            return fminus::ast::Type::enumerated(found->fixed.value_or(fminus::ast::Type::Int), found->id);
        }

        // The name declared with its pointer, and, for an array, the array of what the rest declares: `float x[4]`
        // is a float[4], `float g[4][16]` a float[4][16] (an array of rows), and `const float c[2]` a const float[2],
        // whose elements are read-only. The array's counts are the code generator's to work out, from the
        // declaration's sizes (DeclStmt::size and rowSize).
        fminus::frontend::Declared declaredName(bool isConst, fminus::ast::Type type,
                                                fminus::frontend::Declarator& d,
                                                fminus::frontend::ParseContext& context)
        {
            fminus::frontend::Declared name = fminus::frontend::declared(isConst, type, d.pointer);
            if (!d.isArray)
                return name;
            if (d.pointer.isPointer)
                context.diagnostics.error(d.location, "FMinus has no arrays of pointers");
            else
                name.type = fminus::ast::Type::arrayOf(d.rows ? fminus::ast::Type::arrayOf(type, 0) : type, 0);
            return name;
        }

        // A string literal's token, `token` at `where`, appended to `literal`: its bytes, and where each is spelt (the
        // token's column, and the byte's offset in it: a token is one line).
        void appendString(fminus::ast::StringExpr& literal, const fminus::frontend::location& where,
                          fminus::frontend::StringToken token)
        {
            const fminus::SourceLocation start = at(where);
            literal.value += token.value;
            for (const int offset : token.offsets)
                literal.at.push_back(fminus::SourceLocation{start.line, start.column + offset, start.file});
        }

        // A declarator's name, its pointer and the attributes after the name.
        fminus::frontend::Declarator named(const fminus::frontend::location& where, fminus::frontend::Pointer pointer,
                                           std::string name, std::vector<fminus::ast::Attribute> attributes)
        {
            fminus::frontend::Declarator d{at(where), pointer, std::move(name), nullptr};
            d.attributes = std::move(attributes);
            return d;
        }

        // A declarator's array sizes, `[4][16]`. Attributes after them, `taps[16] [[...]]`, would be C23's for the
        // array's type, which takes none in FMinus: the first is the error, in clang's words, and they then go with
        // the name's, as if written there.
        void setSizes(fminus::frontend::Declarator& d, fminus::frontend::ArraySizes sizes,
                      fminus::frontend::ParseContext& context)
        {
            d.isArray = true;
            d.size = std::move(sizes.size);
            d.rows = sizes.rows;
            d.rowSize = std::move(sizes.rowSize);
            if (sizes.attributes.empty())
                return;
            context.diagnostics.error(sizes.attributes.front().location,
                                      "'" + sizes.attributes.front().name + "' attribute cannot be applied to types: "
                                          "the attributes of '" + d.name + "' go after its name, before its size");
            for (fminus::ast::Attribute& attribute : sizes.attributes)
                d.attributes.push_back(std::move(attribute));
        }

        // A declarator's initialiser list, `= {...}`.
        void setList(fminus::frontend::Declarator& d, fminus::frontend::InitList list)
        {
            d.hasList = true;
            d.list = std::move(list.items);
            d.braces = std::move(list.braces);
        }

        // `items` appended to `list`: their values after its, and their braces after its, moved along.
        void append(fminus::frontend::InitList& list, fminus::frontend::InitList items)
        {
            for (fminus::ast::Brace brace : items.braces)
            {
                brace.first += list.items.size();
                list.braces.push_back(brace);
            }
            for (fminus::ast::ExprPtr& item : items.items)
                list.items.push_back(std::move(item));
        }

        // A declaration's statements, one DeclStmt for each declarator: `const float a = 1, *p;`.
        std::vector<fminus::ast::Stmt> declarationsOf(bool isConst, fminus::ast::Type type,
                                                      std::vector<fminus::frontend::Declarator> declarators,
                                                      fminus::frontend::ParseContext& context)
        {
            std::vector<fminus::ast::Stmt> statements;
            for (fminus::frontend::Declarator& d : declarators)
            {
                const fminus::frontend::Declared name = declaredName(isConst, type, d, context);
                statements.push_back(fminus::ast::Stmt{
                    d.location, fminus::ast::DeclStmt{name.isConst, name.type, std::move(d.name), std::move(d.init),
                                                      name.isRestrict, d.hasList, std::move(d.list),
                                                      std::move(d.size), std::move(d.rowSize),
                                                      std::move(d.braces), std::move(d.attributes)}});
            }
            return statements;
        }

        // A module's fields, from a declaration's statements, with their attributes.
        void addFields(std::vector<fminus::ast::Stmt> declarations, std::vector<fminus::ast::Field>& fields)
        {
            for (fminus::ast::Stmt& stmt : declarations)
            {
                auto& decl = std::get<fminus::ast::DeclStmt>(stmt.node);
                fields.push_back(fminus::ast::Field{stmt.location, std::move(decl.name), decl.type, decl.isConst,
                                                   decl.init != nullptr || decl.hasList, false,
                                                   std::move(decl.attributes), std::move(decl.size), 0,
                                                   std::move(decl.rowSize)});
            }
        }

        // The program's constants, from a top-level declaration's statements, where they are: after `functions` of
        // the program's functions, and its modules and enumerations so far.
        void addConstants(std::vector<fminus::ast::Stmt> declarations, std::size_t functions,
                          fminus::ast::Program& program)
        {
            for (fminus::ast::Stmt& stmt : declarations)
                program.constants.push_back(fminus::ast::Constant{stmt.location,
                                                                  std::move(std::get<fminus::ast::DeclStmt>(stmt.node)),
                                                                  functions, program.modules.size(),
                                                                  program.enums.size()});
        }
    } // namespace
}

%token FLOAT "float" CONST "const" RETURN "return" BOOL "bool" INT "int" VOID "void" TRUE "true" FALSE "false"
%token DOUBLE "double"
%token RESTRICT "restrict" AMPERSAND "&" LBRACKET "[" RBRACKET "]" SIZEOF "sizeof" COLONCOLON "::"
%token LT "<" LE "<=" GT ">" GE ">=" EQ "==" NE "!=" NOT "!" AND "&&" OR "||" QUESTION "?" COLON ":"
%token IF "if" ELSE "else" WHILE "while" FOR "for" BREAK "break" CONTINUE "continue" PARALLEL "parallel"
%token SWITCH "switch" CASE "case" DEFAULT "default" ENUM "enum"
%token MODULE "module" THIS "this" ARROW "->" PARAM "param"
%token DEFER "defer" DEFER_TS "_Defer"
%token SERVE "serve" ROUTE "route" REPLY "reply" JSON "json"
%token CHAR "char" STRING_TYPE "string"
%token WHEN "when"
%token WITHIN "within"
%token <ast::Type> MODULE_NAME "module name"
%token <ast::Type> VECTOR_TYPE "vector type"
%token <ast::Type> HANDLE_TYPE "handle type"
%token DOT "." POLY "poly"
%token PLUS "+" MINUS "-" STAR "*" SLASH "/" PERCENT "%" INCREMENT "++" DECREMENT "--"
%token PIPE "|" CARET "^" TILDE "~" SHL "<<" SHR ">>"
%token ASSIGN "=" PLUS_ASSIGN "+=" MINUS_ASSIGN "-=" STAR_ASSIGN "*=" SLASH_ASSIGN "/=" PERCENT_ASSIGN "%="
%token AND_ASSIGN "&=" OR_ASSIGN "|=" XOR_ASSIGN "^=" SHL_ASSIGN "<<=" SHR_ASSIGN ">>="
%token LPAREN "(" RPAREN ")" LBRACE "{" RBRACE "}" COMMA "," SEMI ";"
%token <std::string> IDENTIFIER "identifier"
%token <std::string> NUMBER "number"
%token <StringToken> STRING "string literal"

%nterm <std::vector<ast::Function>> items
%nterm <ast::Function> function
%nterm <ModuleDeclaration> module
%nterm <std::string> module_name enum_tag
%nterm <EnumDeclaration> enumeration
%nterm <EnumHead> enum_head
%nterm <EnumBase> enum_base
%nterm <std::vector<Declarator>> enum_declarators
%nterm <std::vector<ast::Enumerator>> enumerators
%nterm <ast::Enumerator> enumerator
%nterm <ModuleBody> module_body module_member
%nterm <ast::ExprPtr> array_size
%nterm <ArraySizes> array_sizes declarator_sizes
%nterm <InitList> initializer_list initializers initializer
%nterm <std::vector<ast::Attribute>> attributes attribute_specifiers attribute_specifier attribute_list
%nterm <ast::Attribute> attribute
%nterm <std::vector<ast::AttributeArg>> attribute_args
%nterm <ast::AttributeArg> attribute_arg
%nterm <std::string> attribute_word
%nterm <ast::StringExpr> string_literal
%nterm <std::vector<ast::Param>> params param_list
%nterm <ast::Param> param
%nterm <bool> qualifier
%nterm <Pointer> pointer pointer_qualifiers
%nterm <ast::Type> type cast_type
%nterm <std::vector<ast::Stmt>> block block_items block_item declaration
%nterm <ast::Stmt> statement unlabeled label
%nterm <std::vector<ast::Stmt>> for_init
%nterm <ast::ExprPtr> optional_expr
%nterm <std::vector<Declarator>> declarators
%nterm <Declarator> declarator
%nterm <SelectionHead> selection_head
%nterm <std::vector<ast::Route>> serve_items
%nterm <ast::Route> route
%nterm <RouteParams> route_params
%nterm <std::vector<ast::RouteParam>> route_param_list
%nterm <ast::RouteParam> route_param
%nterm <ast::JsonValue> json_value json_object json_members json_member json_array json_values
%nterm <ast::ExprPtr> expr
%nterm <std::vector<ast::ExprPtr>> args arg_list

// The dangling else: in `if (a) if (b) s; else t;` the else could close either if. Giving it more precedence than the
// shorter rule (THEN) makes Bison shift it, so it closes the nearest if, as in C.
%precedence THEN
%precedence "else"

%right "=" "+=" "-=" "*=" "/=" "%=" "&=" "|=" "^=" "<<=" ">>="
%right "?" ":"
%left "||"
%left "&&"
%left "|"
%left "^"
%left "&"
%left "==" "!="
%left "<" "<=" ">" ">="
%left "<<" ">>"
%left "+" "-"
%left "*" "/" "%"
%precedence UNARY
%precedence "." "->" "++" "--" "["

%%

program
    : items                                 { parseContext.program.functions = $1; }
    ;

// The program's functions, a module's methods among them where the module is. A declaration's names are constants.
items
    : %empty                                {}
    | items function                        { $$ = $1; $$.push_back($2); }
    | items module                          {
                                                $$ = $1;
                                                ModuleDeclaration declaration = $2;
                                                declaration.module->position = $$.size();
                                                for (ast::Enum& enumeration : declaration.enums)
                                                {
                                                    enumeration.position = $$.size();
                                                    parseContext.program.enums.push_back(std::move(enumeration));
                                                }
                                                for (ast::Function& method : declaration.methods)
                                                    $$.push_back(std::move(method));
                                            }
    | items enumeration                     {
                                                $$ = $1;
                                                EnumDeclaration declared = $2;
                                                if (declared.enumeration)
                                                {
                                                    ast::Enum& enumeration = *declared.enumeration;
                                                    enumeration.position = $$.size();
                                                    enumeration.modules = parseContext.program.modules.size();
                                                    parseContext.program.enums.push_back(std::move(enumeration));
                                                }
                                                addConstants(std::move(declared.declarations), $$.size(),
                                                             parseContext.program);
                                            }
    | items declaration                     { $$ = $1; addConstants($2, $$.size(), parseContext.program); }
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
                                                $$.enums = std::move(body.enums);
                                                for (ast::Enum& enumeration : $$.enums)
                                                {
                                                    enumeration.modules = parseContext.program.modules.size() - 1;
                                                    enumeration.module = $$.module;
                                                }
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
                                                // A member's enumeration comes before the fields it declares.
                                                for (ast::Enum& enumeration : member.enums)
                                                    $$.enums.push_back(std::move(enumeration));
                                                for (ast::Field& field : member.fields)
                                                {
                                                    field.enums = $$.enums.size();   // the module's before it
                                                    $$.fields.push_back(std::move(field));
                                                }
                                                for (ast::Function& method : member.methods)
                                                    $$.methods.push_back(std::move(method));
                                            }
    ;

// A declaration's names are fields; a parameter is a field the host sets; a function is a method, with `this` before
// its parameters; an enumeration's constants are the file's, from here on.
module_member
    : "param" type IDENTIFIER attributes ";"
                                            {
                                                $$.fields.push_back(ast::Field{at(@3), $3, $2, false, false, true,
                                                                               $4, nullptr, 0});
                                            }
    | declaration                           { addFields($1, $$.fields); }
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
                                                                     ast::Param{at(@4), "this", self, true, false,
                                                                                nullptr});
                                                $$.methods.push_back(std::move(method));
                                            }
    | enumeration                           {
                                                EnumDeclaration declared = $1;
                                                if (declared.enumeration)
                                                    $$.enums.push_back(std::move(*declared.enumeration));
                                                addFields(std::move(declared.declarations), $$.fields);
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
// `void process(...) [[clang::nonblocking]]`; after a parameter's name or a declarator's, where they appertain to
// what it declares: `float level [[ui::unit("dB")]];`. None is an empty list, which the parser takes before it reads
// a `[` (a declarator's size, or an attribute: the next token says which).
attributes
    : %empty                                {}
    | attributes attribute_specifier        { $$ = $1; for (ast::Attribute& a : $2) $$.push_back(std::move(a)); }
    ;

// One or more, where a `[` that is no attribute's could come instead (after an array's size).
attribute_specifiers
    : attribute_specifier                   { $$ = $1; }
    | attribute_specifiers attribute_specifier
                                            { $$ = $1; for (ast::Attribute& a : $2) $$.push_back(std::move(a)); }
    ;

attribute_specifier
    : "[" "[" attribute_list "]" "]"        { $$ = $3; }
    ;

attribute_list
    : attribute                             { $$.push_back($1); }
    | attribute_list "," attribute          { $$ = $1; $$.push_back($3); }
    ;

attribute
    : attribute_word "::" attribute_word    { $$ = ast::Attribute{at(@1), $1 + "::" + $3, {}}; }
    | attribute_word                        { $$ = ast::Attribute{at(@1), $1, {}}; }
    | attribute_word "::" attribute_word "(" attribute_args ")"
                                            { $$ = ast::Attribute{at(@1), $1 + "::" + $3, $5}; }
    | attribute_word "(" attribute_args ")" { $$ = ast::Attribute{at(@1), $1, $3}; }
    ;

// An attribute's namespace and its name: an identifier, which may be a module's name, as `[[meter::label("Peak")]]`'s
// is in a program with a module meter, or a keyword, which C23 reads there as an identifier: `fminus::default`,
// `[[ui::enum("a", "b")]]`, `[[gnu::const]]`. Every token the scanner makes of an identifier's spelling is here
// (FrontendTest checks each keyword of the parser's tokens): the type words, the statements' and FMinus's own.
attribute_word
    : IDENTIFIER                            { $$ = $1; }
    | MODULE_NAME                           { const ast::Type type = $1; $$ = type.module->name; }
    | VECTOR_TYPE                           { $$ = ast::typeName($1); }
    | HANDLE_TYPE                           { $$ = ast::typeName($1); }
    | "float"                               { $$ = "float"; }
    | "double"                              { $$ = "double"; }
    | "bool"                                { $$ = "bool"; }
    | "int"                                 { $$ = "int"; }
    | "void"                                { $$ = "void"; }
    | "const"                               { $$ = "const"; }
    | "restrict"                            { $$ = "restrict"; }
    | "sizeof"                              { $$ = "sizeof"; }
    | "true"                                { $$ = "true"; }
    | "false"                               { $$ = "false"; }
    | "return"                              { $$ = "return"; }
    | "if"                                  { $$ = "if"; }
    | "else"                                { $$ = "else"; }
    | "while"                               { $$ = "while"; }
    | "for"                                 { $$ = "for"; }
    | "break"                               { $$ = "break"; }
    | "continue"                            { $$ = "continue"; }
    | "switch"                              { $$ = "switch"; }
    | "case"                                { $$ = "case"; }
    | "default"                             { $$ = "default"; }
    | "enum"                                { $$ = "enum"; }
    | "parallel"                            { $$ = "parallel"; }
    | "poly"                                { $$ = "poly"; }
    | "module"                              { $$ = "module"; }
    | "this"                                { $$ = "this"; }
    | "param"                               { $$ = "param"; }
    | "defer"                               { $$ = "defer"; }
    | "_Defer"                              { $$ = "_Defer"; }
    | "serve"                               { $$ = "serve"; }
    | "route"                               { $$ = "route"; }
    | "reply"                               { $$ = "reply"; }
    | "json"                                { $$ = "json"; }
    | "char"                                { $$ = "char"; }
    | "string"                              { $$ = "string"; }
    | "when"                                { $$ = "when"; }
    | "within"                              { $$ = "within"; }
    ;

// An attribute's arguments: numbers, a minus before any of them, and strings (C23's balanced tokens, of which FMinus
// takes these).
attribute_args
    : attribute_arg                         { $$.push_back($1); }
    | attribute_args "," attribute_arg      { $$ = $1; $$.push_back($3); }
    ;

attribute_arg
    : NUMBER                                { $$ = ast::AttributeArg{$1, false}; }
    | "-" NUMBER                            { $$ = ast::AttributeArg{"-" + $2, false}; }
    | string_literal                        { $$ = ast::AttributeArg{$1.value, true}; }
    ;

// C's adjacent string literals are one: "Cut" "off" is "Cutoff".
string_literal
    : STRING                                { appendString($$, @1, $1); }
    | string_literal STRING                 { $$ = $1; appendString($$, @2, $2); }
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
                                                $$ = ast::Param{at(@4), $4, d.type, d.isConst, d.isRestrict, nullptr};
                                            }
    | qualifier type pointer                {
                                                const Declared d = declared($1, $2, $3);
                                                $$ = ast::Param{at(@2), "", d.type, d.isConst, d.isRestrict, nullptr};
                                            }
    | qualifier type pointer IDENTIFIER array_sizes
                                            {
                                                // C's: a parameter declared as an array is a pointer to its first
                                                // element, and the size, checked, says nothing. A 2-D one's would be
                                                // a pointer to its first row, which FMinus has no parameter of.
                                                const Pointer pointer = $3;
                                                ArraySizes sizes = $5;
                                                if (pointer.isPointer)
                                                    parseContext.diagnostics.error(at(@4),
                                                                                   "FMinus has no arrays of pointers");
                                                else if (sizes.rows)
                                                    parseContext.diagnostics.error(
                                                        at(@4), "FMinus has no parameters of 2-D arrays, which would "
                                                                "be pointers to rows: pass a row (a float*), or the "
                                                                "module that holds the array");
                                                $$ = ast::Param{at(@4), $4, ast::Type::pointerTo($2, $1), false, false,
                                                                std::move(sizes.size)};
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
    | "double"                              { $$ = ast::Type::Double; }
    | "bool"                                { $$ = ast::Type::Bool; }
    | "int"                                 { $$ = ast::Type::Int; }
    | "void"                                { $$ = ast::Type::Void; }
    | VECTOR_TYPE                           { $$ = $1; }
    | HANDLE_TYPE                           { $$ = $1; }
    | "char"                                { $$ = ast::Type::Char; }
    | "string"                              { $$ = ast::Type::String; }
    | "poly" "float"                        { $$ = ast::Type::PolyFloat; }
    | "poly" "bool"                         { $$ = ast::Type::PolyBool; }
    | "poly" "double"                       {
                                                // A poly type's lanes are floats (or bools): a double is no lane.
                                                parseContext.diagnostics.error(at(@2), "FMinus has poly float and poly "
                                                                               "bool, not 'poly double': a double is "
                                                                               "no vector lane");
                                                $$ = ast::Type::PolyFloat;
                                            }
    | MODULE_NAME                           { $$ = $1; }
    | "enum" IDENTIFIER                     { $$ = enumType(@2, $2, parseContext); }
    ;

// The types a cast names: C casts between scalars (a vector's literal is OpenCL C's (float4)(...)).
cast_type
    : "float"                               { $$ = ast::Type::Float; }
    | "double"                              { $$ = ast::Type::Double; }
    | "int"                                 { $$ = ast::Type::Int; }
    | "bool"                                { $$ = ast::Type::Bool; }
    | "enum" IDENTIFIER                     { $$ = enumType(@2, $2, parseContext); }
    ;

// A block is a scope of the enumerations' tags too.
block
    : "{"                                   { parseContext.tags.emplace_back(); }
      block_items "}"                       { parseContext.tags.pop_back(); $$ = $3; }
    ;

block_items
    : %empty                                {}
    | block_items block_item                { $$ = $1; for (ast::Stmt& item : $2) $$.push_back(std::move(item)); }
    ;

// A block item is a list: a declaration gives one DeclStmt per declarator. A label is one of its own (C23's), a
// CaseStmt or DefaultStmt without a body: the items after it follow it.
block_item
    : declaration                           { $$ = $1; }
    | enumeration                           {
                                                EnumDeclaration declared = $1;
                                                if (declared.enumeration)
                                                {
                                                    const SourceLocation where = declared.enumeration->location;
                                                    $$.push_back(ast::Stmt{
                                                        where, ast::EnumStmt{std::move(*declared.enumeration)}});
                                                }
                                                for (ast::Stmt& stmt : declared.declarations)
                                                    $$.push_back(std::move(stmt));
                                            }
    | unlabeled                             { $$.push_back($1); }
    | label                                 { $$.push_back($1); }
    ;

// An enumeration, and the names it declares of its type, as C's (`enum Mode { A, B } mode;`; `const` is theirs); or
// C23's tag declared ahead of its list, with its fixed underlying type.
enumeration
    : enum_head enum_declarators ";"        {
                                                EnumHead head = $1;
                                                const ast::Type type = ast::Type::enumerated(
                                                    head.enumeration.underlying, head.enumeration.id);
                                                $$.declarations = declarationsOf(head.isConst, type, $2, parseContext);
                                                $$.enumeration = std::move(head.enumeration);
                                            }
    | qualifier "enum" enum_tag enum_base ";"
                                            { forwardEnum(@5, @3, $3, $4, parseContext); }
    ;

// An enumeration up to its closing brace, from which its tag, if any, is a type (the names after it may use it). C's
// `enum { }` is an error.
enum_head
    : qualifier "enum" enum_tag enum_base "{" enumerators "}"
                                            { $$ = EnumHead{declaredEnum(@2, @3, $3, $4, $6, parseContext), $1}; }
    | qualifier "enum" enum_tag enum_base "{" enumerators "," "}"
                                            { $$ = EnumHead{declaredEnum(@2, @3, $3, $4, $6, parseContext), $1}; }
    | qualifier "enum" enum_tag enum_base "{" "}"
                                            {
                                                parseContext.diagnostics.error(at(@6), "use of empty enum");
                                                $$ = EnumHead{declaredEnum(@2, @3, $3, $4, {}, parseContext), $1};
                                            }
    ;

// C23's fixed underlying type, `: int`.
enum_base
    : %empty                                { $$ = EnumBase{}; }
    | ":" type                              { $$ = EnumBase{true, $2, at(@2)}; }
    ;

enum_declarators
    : %empty                                {}
    | declarators                           { $$ = $1; }
    ;

enum_tag
    : %empty                                { $$ = ""; }
    | IDENTIFIER                            { $$ = $1; }
    ;

enumerators
    : enumerator                            { $$.push_back($1); }
    | enumerators "," enumerator            { $$ = $1; $$.push_back($3); }
    ;

enumerator
    : IDENTIFIER                            { $$ = ast::Enumerator{at(@1), $1, nullptr}; }
    | IDENTIFIER "=" expr                   { $$ = ast::Enumerator{at(@1), $1, $3}; }
    ;

declaration
    : qualifier type declarators ";"        { $$ = declarationsOf($1, $2, $3, parseContext); }
    ;

declarators
    : declarator                            { $$.push_back($1); }
    | declarators "," declarator            { $$ = $1; $$.push_back($3); $$.back().comma = at(@2); }
    ;

// C23's attributes after a declarator's name appertain to what it declares: a field's are the host's
// (Field::attributes), and a local's or a constant's are the code generator's error. They and the sizes after them
// are read without a rule ending between them, since a `[` after the attributes may begin either.
declarator
    : pointer IDENTIFIER attributes         { $$ = named(@2, $1, $2, $3); }
    | pointer IDENTIFIER attributes "=" expr
                                            { $$ = named(@2, $1, $2, $3); $$.init = $5; }
    | pointer IDENTIFIER attributes "=" initializer_list
                                            { $$ = named(@2, $1, $2, $3); setList($$, $5); }
    | pointer IDENTIFIER attributes declarator_sizes
                                            { $$ = named(@2, $1, $2, $3); setSizes($$, $4, parseContext); }
    | pointer IDENTIFIER attributes declarator_sizes "=" expr
                                            {
                                                $$ = named(@2, $1, $2, $3);
                                                setSizes($$, $4, parseContext);
                                                $$.init = $6;
                                            }
    | pointer IDENTIFIER attributes declarator_sizes "=" initializer_list
                                            {
                                                $$ = named(@2, $1, $2, $3);
                                                setSizes($$, $4, parseContext);
                                                setList($$, $6);
                                            }
    ;

// A declarator's sizes, and any attributes after them (setSizes' error).
declarator_sizes
    : array_sizes                           { $$ = $1; }
    | array_sizes attribute_specifiers      { $$ = $1; $$.attributes = $2; }
    ;

// An initialiser list: its values, and the braces of the lists inside it, a 2-D array's rows (ast::Brace). C23's
// `{}` is an empty one, and a comma may end one.
initializer_list
    : "{" "}"                               {}
    | "{" initializers "}"                  { $$ = $2; }
    | "{" initializers "," "}"              { $$ = $2; }
    ;

initializers
    : initializer                           { $$ = $1; }
    | initializers "," initializer          { $$ = $1; append($$, $3); }
    ;

// A value, or a braced list: its values, inside a brace that holds them all, one level deeper than its own.
initializer
    : expr                                  { $$.items.push_back($1); }
    | initializer_list                      {
                                                InitList list = $1;
                                                $$.braces.push_back(ast::Brace{at(@1), 0, list.items.size(), 1});
                                                for (ast::Brace& brace : list.braces)
                                                    ++brace.depth;
                                                append($$, std::move(list));
                                            }
    ;

// An array's sizes as written: one, or a 2-D array's two, the rows' last. A third is FMinus's error (C would have an
// array of arrays of arrays), which keeps the first two.
array_sizes
    : array_size                            { $$.size = $1; }
    | array_sizes array_size                {
                                                $$ = $1;
                                                if ($$.rows)
                                                    parseContext.diagnostics.error(at(@2), "FMinus's arrays have at "
                                                                                           "most two dimensions");
                                                else
                                                {
                                                    $$.rows = true;
                                                    $$.rowSize = $2;
                                                }
                                            }
    ;

// An array's size as written: an expression (the code generator checks that it is an integer constant expression,
// and its value), or none, `[]`, for an array whose list gives it.
array_size
    : "[" expr "]"                          { $$ = $2; }
    | "[" "]"                               { $$ = nullptr; }
    ;

// A statement where one is expected: an unlabeled one, or a label and the statement it labels.
statement
    : unlabeled                             { $$ = $1; }
    | label statement                       {
                                                $$ = $1;
                                                ast::StmtPtr body = ast::own($2);
                                                if (auto* label = std::get_if<ast::CaseStmt>(&$$.node))
                                                    label->body = std::move(body);
                                                else
                                                    std::get<ast::DefaultStmt>($$.node).body = std::move(body);
                                            }
    ;

label
    : "case" expr ":"                       { $$ = ast::Stmt{at(@1), ast::CaseStmt{$2, nullptr}}; }
    | "default" ":"                         { $$ = ast::Stmt{at(@1), ast::DefaultStmt{nullptr}}; }
    ;

unlabeled
    : block                                 { $$ = ast::Stmt{at(@1), ast::BlockStmt{$1}}; }
    | expr ";"                              { $$ = ast::Stmt{at(@1), ast::ExprStmt{$1}}; }
    | "return" expr ";"                     { $$ = ast::Stmt{at(@1), ast::ReturnStmt{$2}}; }
    | "return" ";"                          { $$ = ast::Stmt{at(@1), ast::ReturnStmt{nullptr}}; }
    | ";"                                   { $$ = ast::Stmt{at(@1), ast::EmptyStmt{}}; }
    | "if" "(" expr ")" statement %prec THEN
                                            { $$ = ast::Stmt{at(@1), ast::IfStmt{$3, ast::own($5), nullptr, {}}}; }
    | "if" "(" expr ")" statement "else" statement
                                            { $$ = ast::Stmt{at(@1), ast::IfStmt{$3, ast::own($5), ast::own($7), {}}}; }
    | "if" "(" selection_head ")" statement %prec THEN
                                            {
                                                SelectionHead head = $3;
                                                $$ = ast::Stmt{at(@1), ast::IfStmt{std::move(head.condition),
                                                                                   ast::own($5), nullptr,
                                                                                   std::move(head.init)}};
                                            }
    | "if" "(" selection_head ")" statement "else" statement
                                            {
                                                SelectionHead head = $3;
                                                $$ = ast::Stmt{at(@1), ast::IfStmt{std::move(head.condition),
                                                                                   ast::own($5), ast::own($7),
                                                                                   std::move(head.init)}};
                                            }
    | "while" "(" expr ")" statement        { $$ = ast::Stmt{at(@1), ast::WhileStmt{$3, ast::own($5)}}; }
    | "for" "(" for_init optional_expr ";" optional_expr ")" statement
                                            { $$ = ast::Stmt{at(@1), ast::ForStmt{$3, $4, $6, ast::own($8)}}; }
    | "break" ";"                           { $$ = ast::Stmt{at(@1), ast::BreakStmt{}}; }
    | "continue" ";"                        { $$ = ast::Stmt{at(@1), ast::ContinueStmt{}}; }
    | "switch" "(" expr ")" statement       { $$ = ast::Stmt{at(@1), ast::SwitchStmt{$3, ast::own($5), {}}}; }
    | "switch" "(" selection_head ")" statement
                                            {
                                                SelectionHead head = $3;
                                                $$ = ast::Stmt{at(@1), ast::SwitchStmt{std::move(head.condition),
                                                                                       ast::own($5),
                                                                                       std::move(head.init)}};
                                            }
    | "parallel" "for" "(" for_init optional_expr ";" optional_expr ")" statement
                                            {
                                                $$ = ast::Stmt{at(@1), ast::ParallelForStmt{$4, $5, $7, ast::own($9)}};
                                            }
    | "parallel" block                      { $$ = ast::Stmt{at(@1), ast::ParallelBlockStmt{$2}}; }
    | defer statement                       { $$ = ast::Stmt{at(@1), ast::DeferStmt{ast::own($2)}}; }
    | "serve" "(" arg_list ")" "{" serve_items "}"
                                            { $$ = ast::Stmt{at(@1), ast::ServeStmt{$3, $6}}; }
    | "reply" arg_list ";"                  { $$ = ast::Stmt{at(@1), ast::ReplyStmt{$2}}; }
    | "reply" ";"                           {
                                                parseContext.diagnostics.error(at(@2), "expected expression");
                                                $$ = ast::Stmt{at(@1), ast::ReplyStmt{}};
                                            }
    | "when" "(" expr ")" statement         { $$ = ast::Stmt{at(@1), ast::WhenStmt{$3, ast::own($5)}}; }
    | "within" "(" expr ")" block           {
                                                $$ = ast::Stmt{at(@1), ast::WithinStmt{
                                                                           $3, ast::own(ast::Stmt{
                                                                                   at(@5), ast::BlockStmt{$5}})}};
                                            }
    ;

// A serve's routes. Anything else in its braces parses, and is an error: only a route may stand there.
serve_items
    : %empty                                {}
    | serve_items route                     { $$ = $1; $$.push_back($2); }
    | serve_items block_item                {
                                                $$ = $1;
                                                parseContext.diagnostics.error(at(@2), "only a route may stand in a "
                                                                               "serve's braces: route \"METHOD /path\" "
                                                                               "(parameters) { ... }");
                                            }
    ;

// A route: its literal, its parameters (none, or a list), attributes after them (the code generator's error) and
// its block, whose closing brace the missing reply's error is at.
route
    : "route" string_literal route_params attributes block
                                            {
                                                RouteParams params = $3;
                                                $$.location = at(@1);
                                                $$.literal = $2;
                                                $$.literalLocation = at(@2);
                                                $$.hasParams = params.written;
                                                $$.params = std::move(params.params);
                                                $$.attributes = $4;
                                                $$.body = $5;
                                                const fminus::frontend::location& end = @5;
                                                $$.closing = at(end);
                                                $$.closing.line = end.end.line;
                                                $$.closing.column = end.end.column - 1;
                                            }
    ;

route_params
    : %empty                                {}
    | "(" ")"                               { $$.written = true; }
    | "(" route_param_list ")"              { $$.written = true; $$.params = $2; }
    ;

route_param_list
    : route_param                           { $$.push_back($1); }
    | route_param_list "," route_param      { $$ = $1; $$.push_back($3); }
    ;

// As a function's parameter, with C23's attributes after its name, as a field's: a range, a default.
route_param
    : qualifier type pointer IDENTIFIER attributes
                                            {
                                                const Declared d = declared($1, $2, $3);
                                                $$ = ast::RouteParam{at(@4), $4, d.type, d.isConst, $5};
                                            }
    ;

// TS 25755's keyword, and the spelling it encourages, which FMinus takes as a keyword too. Its statement is an
// unlabeled one in the TS; a case or default label there parses, as clang parses it, and is the code generator's error:
// a switch would jump into the deferred statement (ir/Statements.cpp).
defer
    : "defer"
    | "_Defer"
    ;

// C2y's head of an if or a switch with a declaration (N3356, N3580): a declaration, its ; included, and the condition;
// or one declarator with an initialiser, whose name is then the condition, as C2y rewrites `if (T x = e)` to
// `if (T x = e; x)`. clang's errors for what C2y refuses: more than one declarator in that form, at the comma; none
// with no initialiser, at the name; and a first clause that is no declaration, at its start.
selection_head
    : declaration expr                      { $$.init = $1; $$.condition = $2; }
    | qualifier type declarators            {
                                                std::vector<Declarator> declarators = $3;
                                                if (declarators.size() > 1)
                                                    parseContext.diagnostics.error(declarators[1].comma, "multiple "
                                                                                   "declarations are not allowed");
                                                declarators.resize(1);
                                                Declarator& d = declarators.front();
                                                if (!d.init && !d.hasList)
                                                    parseContext.diagnostics.error(d.location, "variable declaration "
                                                                                   "in condition must have an "
                                                                                   "initializer");
                                                const SourceLocation where = d.location;
                                                std::string name = d.name;
                                                $$.init = declarationsOf($1, $2, std::move(declarators), parseContext);
                                                $$.condition = ast::makeExpr(where, ast::VariableExpr{std::move(name)});
                                            }
    | declaration qualifier type declarator {
                                                // A second declaration: C2y's condition is an expression.
                                                const bool isConst = $2;
                                                parseContext.diagnostics.error(at(isConst ? @2 : @3),
                                                                               "expected expression");
                                                $$.init = $1;
                                                $$.condition = ast::makeExpr(at(@4), ast::BoolExpr{false});
                                            }
    | expr ";" expr                         {
                                                parseContext.diagnostics.error(at(@1), "first clause in condition "
                                                                                       "must be a declaration");
                                                $$.init.push_back(ast::Stmt{at(@1), ast::ExprStmt{$1}});
                                                $$.condition = $3;
                                            }
    | ";" expr                              {
                                                parseContext.diagnostics.error(at(@1), "first clause in condition "
                                                                                       "must be a declaration");
                                                $$.condition = $2;
                                            }
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
    | "(" expr ")"                          { $$ = $2; $$->opening = at(@1); }   // the outermost's, set last
    | "-" expr %prec UNARY                  { $$ = ast::makeExpr(at(@1), ast::UnaryExpr{ast::UnaryOp::Negate, $2}); }
    | "+" expr %prec UNARY                  { $$ = ast::makeExpr(at(@1), ast::UnaryExpr{ast::UnaryOp::Plus, $2}); }
    | "!" expr %prec UNARY                  { $$ = ast::makeExpr(at(@1), ast::UnaryExpr{ast::UnaryOp::Not, $2}); }
    | "~" expr %prec UNARY                  {
                                                $$ = ast::makeExpr(at(@1),
                                                                   ast::UnaryExpr{ast::UnaryOp::Complement, $2});
                                            }
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
                                                $$ = ast::makeExpr(at(@1), ast::SizeofExpr{declared($3, $4, $5).type,
                                                                                           nullptr, nullptr});
                                            }
    | "sizeof" "(" qualifier type pointer array_sizes ")"
                                            {
                                                // An array type's name, C's abstract declarator: `float[4][16]`.
                                                const Pointer pointer = $5;
                                                ArraySizes sizes = $6;
                                                ast::Type type = declared($3, $4, pointer).type;
                                                if (pointer.isPointer)
                                                    parseContext.diagnostics.error(at(@5), "FMinus has no arrays of "
                                                                                           "pointers");
                                                else
                                                    type = ast::Type::arrayOf(
                                                        sizes.rows ? ast::Type::arrayOf(type, 0) : type, 0);
                                                $$ = ast::makeExpr(at(@1), ast::SizeofExpr{type, std::move(sizes.size),
                                                                                           std::move(sizes.rowSize)});
                                            }
    | "++" expr %prec UNARY                 { $$ = increment(@1, ast::IncrementOp::PreIncrement, $2); }
    | "--" expr %prec UNARY                 { $$ = increment(@1, ast::IncrementOp::PreDecrement, $2); }
    | expr "++"                             { $$ = increment(@2, ast::IncrementOp::PostIncrement, $1); }
    | expr "--"                             { $$ = increment(@2, ast::IncrementOp::PostDecrement, $1); }
    | "(" cast_type ")" expr %prec UNARY    { $$ = ast::makeExpr(at(@1), ast::CastExpr{$2, $4}); }
    | "true"                                { $$ = ast::makeExpr(at(@1), ast::BoolExpr{true}); }
    | "false"                               { $$ = ast::makeExpr(at(@1), ast::BoolExpr{false}); }
    | string_literal                        { $$ = ast::makeExpr(at(@1), $1); }
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
    | expr "<<" expr                        { $$ = binary(@2, ast::BinaryOp::ShiftLeft, $1, $3); }
    | expr ">>" expr                        { $$ = binary(@2, ast::BinaryOp::ShiftRight, $1, $3); }
    | expr "&" expr                         { $$ = binary(@2, ast::BinaryOp::BitAnd, $1, $3); }
    | expr "^" expr                         { $$ = binary(@2, ast::BinaryOp::BitXor, $1, $3); }
    | expr "|" expr                         { $$ = binary(@2, ast::BinaryOp::BitOr, $1, $3); }
    | expr "=" expr                         { $$ = assign(@2, ast::AssignOp::Assign, $1, $3); }
    | expr "+=" expr                        { $$ = assign(@2, ast::AssignOp::Add, $1, $3); }
    | expr "-=" expr                        { $$ = assign(@2, ast::AssignOp::Subtract, $1, $3); }
    | expr "*=" expr                        { $$ = assign(@2, ast::AssignOp::Multiply, $1, $3); }
    | expr "/=" expr                        { $$ = assign(@2, ast::AssignOp::Divide, $1, $3); }
    | expr "%=" expr                        { $$ = assign(@2, ast::AssignOp::Remainder, $1, $3); }
    | expr "&=" expr                        { $$ = assign(@2, ast::AssignOp::BitAnd, $1, $3); }
    | expr "|=" expr                        { $$ = assign(@2, ast::AssignOp::BitOr, $1, $3); }
    | expr "^=" expr                        { $$ = assign(@2, ast::AssignOp::BitXor, $1, $3); }
    | expr "<<=" expr                       { $$ = assign(@2, ast::AssignOp::ShiftLeft, $1, $3); }
    | expr ">>=" expr                       { $$ = assign(@2, ast::AssignOp::ShiftRight, $1, $3); }
    | expr "&&" expr                        { $$ = logical(@2, ast::LogicalOp::And, $1, $3); }
    | expr "||" expr                        { $$ = logical(@2, ast::LogicalOp::Or, $1, $3); }
    | expr "?" expr ":" expr                { $$ = ast::makeExpr(at(@2), ast::ConditionalExpr{$1, $3, $5}); }
    | "json" json_object                    { $$ = ast::makeExpr(at(@1), ast::JsonExpr{$2}); }
    | "json" json_array                     { $$ = ast::makeExpr(at(@1), ast::JsonExpr{$2}); }
    | "json" "(" expr ")"                   {
                                                ast::JsonValue value;
                                                value.location = at(@3);
                                                value.value = $3;
                                                $$ = ast::makeExpr(at(@1), ast::JsonExpr{std::move(value)});
                                            }
    ;

// A JSON literal's values: an expression, an object or an array, nested as JSON nests them.
json_value
    : expr                                  {
                                                $$.location = at(@1);
                                                $$.value = $1;
                                            }
    | json_object                           { $$ = $1; }
    | json_array                            { $$ = $1; }
    ;

json_object
    : "{" "}"                               { $$.kind = ast::JsonValue::Kind::Object; $$.location = at(@1); }
    | "{" json_members "}"                  { $$ = $2; $$.location = at(@1); }
    | "{" json_members "," "}"              { $$ = $2; $$.location = at(@1); }
    ;

json_members
    : json_member                           { $$ = $1; }
    | json_members "," json_member          {
                                                $$ = $1;
                                                ast::JsonValue member = $3;
                                                $$.keys.push_back(std::move(member.keys.front()));
                                                $$.keyLocations.push_back(member.keyLocations.front());
                                                $$.elements.push_back(std::move(member.elements.front()));
                                            }
    ;

// One member, as an object of one: `"key": value`, or a name alone, `gain`, which is `"gain": gain`.
json_member
    : string_literal ":" json_value         {
                                                $$.kind = ast::JsonValue::Kind::Object;
                                                $$.keys.push_back($1);
                                                $$.keyLocations.push_back(at(@1));
                                                $$.elements.push_back($3);
                                            }
    | IDENTIFIER                            {
                                                const std::string name = $1;
                                                const SourceLocation where = at(@1);
                                                ast::StringExpr key{name, {}};
                                                for (std::size_t i = 0; i < name.size(); ++i)
                                                    key.at.push_back(SourceLocation{where.line,
                                                                                    where.column
                                                                                        + static_cast<int>(i),
                                                                                    where.file});
                                                $$.kind = ast::JsonValue::Kind::Object;
                                                $$.keys.push_back(std::move(key));
                                                $$.keyLocations.push_back(where);
                                                ast::JsonValue value;
                                                value.location = where;
                                                value.value = ast::makeExpr(where, ast::VariableExpr{name});
                                                $$.elements.push_back(std::move(value));
                                            }
    ;

json_array
    : "[" "]"                               { $$.kind = ast::JsonValue::Kind::Array; $$.location = at(@1); }
    | "[" json_values "]"                   { $$ = $2; $$.location = at(@1); }
    | "[" json_values "," "]"               { $$ = $2; $$.location = at(@1); }
    ;

json_values
    : json_value                            { $$.kind = ast::JsonValue::Kind::Array; $$.elements.push_back($1); }
    | json_values "," json_value            { $$ = $1; $$.elements.push_back($3); }
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
