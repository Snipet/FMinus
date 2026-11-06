/* src/frontend/Lexer.l: the scanner of FMinus v0, C's tokens (Flex 2.6; reentrant, prefixed "fminus", read from
 * memory). It returns Bison's symbols (Parser.y) with their locations, which it keeps in the scanner's state
 * (Scanner.h).
 *
 * Comments are C's: // to the end of the line, and block comments, which do not nest. Constants are C's: floating
 * ones (1.5, 1., .5, 2e3, 1.5e-3f, hex floats such as 0x1.8p1 and 0x1p-3f) and integer ones (2, 010 in octal, 0x1F in
 * hex), one token whose spelling tells the code generator which. The f or F suffix is allowed only where C allows it
 * (not on 2; in 0x1F the F is a digit), and is kept in the spelling. A floating constant without one is a float or a
 * double by where it is used, which the code generator decides (ir/Expressions.cpp, lowerAs).
 * To add a token: a rule here, its %token in Parser.y. Keywords come before {IDENTIFIER}: with equal lengths, Flex
 * picks the earlier rule. A keyword's rule is all macroNameError needs to refuse it as a macro's name: it reads the name
 * with this scanner. An identifier that names a module declared so far is a type, MODULE_NAME (ParseContext.h).
 *
 * A line that starts with # (after spaces) is a directive, C's preprocessor's. `#include "file"` reads the file there,
 * as C does (through the IncludeResolver the front end was given), and `#pragma ...` is ignored, as C ignores the
 * pragmas it does not know. Each file is read once: an #include of a file read already (the program's own too) reads
 * nothing, as if every file began with #pragma once. The scanner reads an included file from a buffer of its own and
 * goes back to the one it interrupted at the file's end; its locations point at the file (SourceFile), whose index in
 * the Diagnostics names it in the messages.
 * The conditionals are C's: #if, #ifdef, #ifndef, #elif, #else and #endif, and C23's #elifdef and #elifndef, on
 * macros that #define and #undef define and the host gives (Frontend.h's Defines). A group that is skipped is the
 * start condition SKIPPED, in which the scanner reads only comments, line ends and the conditionals' lines: its text
 * is never scanned, so it need not be FMinus. A macro is read only in conditions (Preprocessor.h): its name in code is
 * an error. A directive is one line, which its # begins (after spaces): a # after a comment that begins its line is
 * an error, where C reads a directive (ScannerState::lineStart), and a block comment in a directive's line, in any
 * group, must end on it.
 */
%option reentrant noyywrap nounput noinput nounistd never-interactive batch
%option prefix="fminus"
%option extra-type="fminus::frontend::ScannerState*"

%x BLOCK_COMMENT SKIPPED

%top{
// Before anything Flex generates: the parser's types, the scanner's state type and YY_DECL.
#include "frontend/Scanner.h"
}

%{
#include <algorithm>
#include <climits>
#include <memory>
#include <new>
#include <optional>
#include <string>
#include <string_view>
#include <utility>

using Parser = fminus::frontend::Parser;

namespace
{
    // The directive on one line, `text` (its first # after spaces), at the scanner's location: the file an #include
    // reads, registered with the Diagnostics and the scanner's files, or nothing (a #pragma, an #include of a file
    // read already, C's empty directive #, a macro's definition, a conditional, any line of a skipped group). Throws
    // the directive's error.
    std::optional<fminus::frontend::IncludedFile> directive(fminus::frontend::ScannerState& state,
                                                            std::string_view text);

    // The error of a macro's name in code.
    std::string macroInCode(const std::string& name);

    // The error of a # after a comment that begins its line, where C reads a directive.
    constexpr const char* kCommentBeforeDirective = "a comment before a directive's '#' is not supported: a directive "
                                                    "begins its line, after spaces only";
} // namespace

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
    // A call after the first follows a token the parser took: its line has text before the scanner.
    if (yyextra->started)
        yyextra->lineStart = false;
    yyextra->started = true;
    yyextra->where.step();
%}

[ \t\r]+                    { yyextra->where.step(); }
\n+                         {
                                yyextra->where.lines(static_cast<int>(yyleng));
                                yyextra->where.step();
                                yyextra->lineStart = true;
                            }
"//".*                      { yyextra->where.step(); }

<INITIAL,SKIPPED>^[ \t]*"#"[^\n]*  {
                                std::optional<fminus::frontend::IncludedFile> file
                                    = directive(*yyextra, std::string_view(yytext, static_cast<std::size_t>(yyleng)));
                                if (!file)
                                {
                                    // A conditional begins or ends a skipped group.
                                    yyextra->where.step();
                                    BEGIN(yyextra->skipping() ? SKIPPED : INITIAL);
                                }
                                else
                                {
                                    // Read the file from here (Flex copies its text), then come back after the
                                    // #include (<<EOF>>).
                                    yyextra->open.push_back({nullptr, YY_CURRENT_BUFFER, yyextra->where});
                                    yyextra->open.back().buffer = yy_scan_bytes(
                                        file->text.data(), static_cast<int>(file->text.size()), yyscanner);
                                    yyextra->where.initialize(&yyextra->files.back());
                                    yyextra->lineStart = true;
                                }
                            }

<SKIPPED>[ \t\r\f\v]+        { yyextra->where.step(); }
<SKIPPED>[^\n/# \t\r\f\v][^\n/#]*  { yyextra->lineStart = false; yyextra->where.step(); }
<SKIPPED>"/"                { yyextra->lineStart = false; yyextra->where.step(); }
<SKIPPED>"#"                {
                                // Not at a line's start: text, unless only comments come before it, where C reads
                                // a directive.
                                if (yyextra->lineStart)
                                    throw Parser::syntax_error(yyextra->where, kCommentBeforeDirective);
                                yyextra->where.step();
                            }
<SKIPPED>"//"[^\n]*         { yyextra->where.step(); }
<SKIPPED>\n+                {
                                yyextra->where.lines(static_cast<int>(yyleng));
                                yyextra->where.step();
                                yyextra->lineStart = true;
                            }
<SKIPPED><<EOF>>            { throw Parser::syntax_error(yyextra->conditionals.back().where,
                                                         "unterminated conditional directive"); }

<INITIAL,SKIPPED>"/*"       { yyextra->commentStart = yyextra->where; BEGIN(BLOCK_COMMENT); }
<BLOCK_COMMENT>"*/"         { yyextra->where.step(); BEGIN(yyextra->skipping() ? SKIPPED : INITIAL); }
<BLOCK_COMMENT>[^*\n]+      { }
<BLOCK_COMMENT>"*"          { }
<BLOCK_COMMENT>\n           { yyextra->where.lines(1); }
<BLOCK_COMMENT><<EOF>>      { throw Parser::syntax_error(yyextra->commentStart, "unterminated /* comment"); }

"float"                     { return Parser::make_FLOAT(yyextra->where); }
"double"                    { return Parser::make_DOUBLE(yyextra->where); }
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
"switch"                    { return Parser::make_SWITCH(yyextra->where); }
"case"                      { return Parser::make_CASE(yyextra->where); }
"default"                   { return Parser::make_DEFAULT(yyextra->where); }
"enum"                      { return Parser::make_ENUM(yyextra->where); }
"parallel"                  { return Parser::make_PARALLEL(yyextra->where); }
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
                                if (yyextra->macros.contains(name))
                                    throw Parser::syntax_error(yyextra->where, macroInCode(name));
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
"&="                        { return Parser::make_AND_ASSIGN(yyextra->where); }
"|="                        { return Parser::make_OR_ASSIGN(yyextra->where); }
"^="                        { return Parser::make_XOR_ASSIGN(yyextra->where); }
"<<="                       { return Parser::make_SHL_ASSIGN(yyextra->where); }
">>="                       { return Parser::make_SHR_ASSIGN(yyextra->where); }
"<<"                        { return Parser::make_SHL(yyextra->where); }
">>"                        { return Parser::make_SHR(yyextra->where); }
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
"|"                         { return Parser::make_PIPE(yyextra->where); }
"^"                         { return Parser::make_CARET(yyextra->where); }
"~"                         { return Parser::make_TILDE(yyextra->where); }
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

"#"                         {
                                if (yyextra->lineStart)
                                    throw Parser::syntax_error(yyextra->where, kCommentBeforeDirective);
                                throw Parser::syntax_error(yyextra->where, "unexpected character '#'");
                            }
.                           {
                                const std::string character(yytext, yyleng);
                                throw Parser::syntax_error(yyextra->where, "unexpected character '" + character + "'");
                            }
<<EOF>>                     {
                                // A file's conditionals end in it.
                                const std::vector<fminus::frontend::Conditional>& conditionals
                                    = yyextra->conditionals;
                                if (!conditionals.empty() && conditionals.back().file == yyextra->open.size())
                                    throw Parser::syntax_error(conditionals.back().where,
                                                               "unterminated conditional directive");
                                if (yyextra->open.empty())
                                    return Parser::make_YYEOF(yyextra->where);
                                // The end of an included file: back to the file it interrupted, after its #include.
                                const fminus::frontend::OpenInclude done = yyextra->open.back();
                                yyextra->open.pop_back();
                                yy_delete_buffer(YY_CURRENT_BUFFER, yyscanner);
                                yy_switch_to_buffer(static_cast<YY_BUFFER_STATE>(done.outer), yyscanner);
                                yyextra->where = done.where;
                                yyextra->where.step();
                            }

%%

namespace
{
    using fminus::frontend::location;

    using fminus::frontend::PreprocessorError;

    constexpr std::size_t kMaxIncludeDepth = 200;   // clang's

    // C's directives FMinus does not have.
    constexpr std::string_view kDirectivesOfC[] = {"line", "error", "warning", "embed"};

    // The conditionals' directives, C23's #elifdef and #elifndef among them.
    constexpr std::string_view kConditionals[] = {"if",   "ifdef",   "ifndef",   "elif",
                                                  "elifdef", "elifndef", "else", "endif"};

    bool isSpace(char c)
    {
        return c == ' ' || c == '\t' || c == '\r' || c == '\f' || c == '\v';
    }

    bool isIdentifierStart(char c)
    {
        return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '_';
    }

    bool isIdentifierChar(char c)
    {
        return isIdentifierStart(c) || (c >= '0' && c <= '9');
    }

    bool isConditional(std::string_view name)
    {
        return std::ranges::find(kConditionals, name) != std::end(kConditionals);
    }

    // The `length` bytes at `offset` in the token at `where`, a line's.
    location within(const location& where, std::size_t offset, std::size_t length)
    {
        location at = where;
        at.begin.column += static_cast<int>(offset);
        at.end = at.begin;
        at.end.column += static_cast<int>(length);
        return at;
    }

    // The first byte at or after `i` in `line` that is no space (the line's size if none is).
    std::size_t afterSpaces(std::string_view line, std::size_t i)
    {
        while (i < line.size() && isSpace(line[i]))
            ++i;
        return i;
    }

    // Throws unless `line` holds only spaces from `i` on (comments made spaces): `#endif X`.
    void endOfDirective(std::string_view line, std::size_t i, std::string_view name)
    {
        while (i < line.size() && isSpace(line[i]))
            ++i;
        if (i < line.size())
            throw PreprocessorError{i, line.size() - i, "extra tokens at end of #" + std::string(name) + " directive"};
    }

    // The name of a macro at `i` in `line` (after spaces), with `i` left after it and `start` at it: clang's errors
    // when there is none, and when it is no identifier.
    std::string macroName(std::string_view line, std::size_t& i, std::size_t& start)
    {
        while (i < line.size() && isSpace(line[i]))
            ++i;
        start = i;
        if (i == line.size())
            throw PreprocessorError{i, 1, "macro name missing"};
        if (!isIdentifierStart(line[i]))
            throw PreprocessorError{i, 1, "macro name must be an identifier"};
        while (i < line.size() && isIdentifierChar(line[i]))
            ++i;
        return std::string(line.substr(start, i - start));
    }

    // A conditional's directive, `name` at `nameAt` in `line` (its comments made spaces) and what follows it from
    // `rest`, in a skipped group too: C's rules for which group is read, and its errors.
    void conditional(fminus::frontend::ScannerState& state, const std::string& name, std::string_view line,
                     std::size_t nameAt, std::size_t rest)
    {
        std::vector<fminus::frontend::Conditional>& conditionals = state.conditionals;
        const auto evaluate = [&] {
            return fminus::frontend::evaluate(line, rest, state.macros, state.expanded) != 0;
        };
        // #ifdef NAME and #elifdef NAME: whether NAME is a macro; #ifndef and #elifndef: whether it isn't.
        const auto isDefined = [&](const std::string& directive) {
            std::size_t i = rest;
            std::size_t start = 0;
            const std::string macro = macroName(line, i, start);
            endOfDirective(line, i, directive);
            return state.macros.contains(macro) == !directive.ends_with("ndef");
        };
        if (name == "if" || name == "ifdef" || name == "ifndef")
        {
            fminus::frontend::Conditional opened{within(state.where, nameAt, name.size()), state.open.size()};
            if (state.skipping())
                opened.inert = opened.taken = opened.skipping = true;   // in a skipped group: none of it is read
            else
            {
                opened.taken = name == "if" ? evaluate() : isDefined(name);
                opened.skipping = !opened.taken;
            }
            conditionals.push_back(opened);
            return;
        }

        // #elif, #else and #endif belong to the innermost conditional, which must be the file's own.
        if (conditionals.empty() || conditionals.back().file != state.open.size())
            throw PreprocessorError{nameAt, name.size(), "#" + name + " without #if"};
        fminus::frontend::Conditional& innermost = conditionals.back();
        if (name.starts_with("elif"))
        {
            if (innermost.sawElse)
                throw PreprocessorError{nameAt, name.size(), "#" + name + " after #else"};
            if (innermost.taken)
                innermost.skipping = true;   // a group was read: this one is skipped, its condition never computed
            else
                innermost.skipping = !(innermost.taken = name == "elif" ? evaluate() : isDefined(name));
            return;
        }
        if (name == "else")
        {
            if (innermost.sawElse)
                throw PreprocessorError{nameAt, name.size(), "#else after #else"};
            if (!innermost.inert)
                endOfDirective(line, rest, name);
            innermost.sawElse = true;
            innermost.skipping = innermost.taken;
            innermost.taken = true;
            return;
        }
        if (!innermost.inert)
            endOfDirective(line, rest, name);
        conditionals.pop_back();
    }

    // #define NAME value and #undef NAME, the line `line` (its comments made spaces) from `rest` on.
    void define(fminus::frontend::ScannerState& state, const std::string& name, std::string_view line,
                std::size_t rest)
    {
        std::size_t i = rest;
        std::size_t start = 0;
        const std::string macro = macroName(line, i, start);
        if (name == "undef")
        {
            if (macro == "defined")
                throw PreprocessorError{start, macro.size(), "'defined' cannot be used as a macro name"};
            endOfDirective(line, i, name);
            state.macros.erase(macro);
            return;
        }
        if (const std::optional<std::string> wrong = fminus::frontend::macroNameError(macro))
            throw PreprocessorError{start, macro.size(), *wrong};
        if (i < line.size() && line[i] == '(')
            throw PreprocessorError{i, 1, "function-like macros are not supported: a macro in FMinus is a name and a "
                                          "value, for #if"};
        std::string value = fminus::frontend::normalisedValue(line.substr(i));
        const auto [defined, added] = state.macros.try_emplace(macro, value);
        if (!added && defined->second != value)
            throw PreprocessorError{start, macro.size(), "'" + macro + "' macro redefined"};
    }

    std::string macroInCode(const std::string& name)
    {
        return "macro expansion is not supported: '" + name + "' is a macro, which FMinus reads only in #if, #elif, "
               "#ifdef and #ifndef (for a constant, declare an enum or a const)";
    }

    std::optional<fminus::frontend::IncludedFile> directive(fminus::frontend::ScannerState& state,
                                                            std::string_view text)
    {
        const location& where = state.where;
        const auto syntaxError = [&](const PreprocessorError& error) {
            return Parser::syntax_error(within(where, error.offset, error.length), error.message);
        };
        const std::size_t hash = afterSpaces(text, 0);
        // The line after the #, its comments made spaces, as C reads it before the directive. A directive is one line,
        // so a /* comment in it must end there, in any group and whatever the directive: C would carry the line on to
        // the comment's end, and read the lines between as the comment.
        std::string line;
        try
        {
            line = fminus::frontend::withoutComments(text, hash + 1);
        }
        catch (const PreprocessorError& error)
        {
            throw syntaxError(error);
        }
        const std::size_t nameStart = afterSpaces(line, hash + 1);
        std::size_t i = nameStart;
        while (i < line.size() && isIdentifierChar(line[i]))
            ++i;
        const std::string name = line.substr(nameStart, i - nameStart);
        const bool conditionalName = isConditional(name);
        if (state.skipping() && !conditionalName)
            return std::nullopt;   // a skipped group's other lines are text, whatever they say, as in C
        if (conditionalName || name == "define" || name == "undef")
        {
            try
            {
                if (conditionalName)
                    conditional(state, name, line, nameStart, i);
                else
                    define(state, name, line, i);
            }
            catch (const PreprocessorError& error)
            {
                throw syntaxError(error);
            }
            return std::nullopt;
        }
        if (name.empty() && afterSpaces(line, i) == line.size())
            return std::nullopt;   // C's null directive
        if (name == "pragma")
            return std::nullopt;   // C ignores the pragmas it does not know; once is what FMinus does for every file
        if (name != "include")
        {
            for (const std::string_view other : kDirectivesOfC)
                if (name == other)
                    throw Parser::syntax_error(within(where, nameStart, name.size()),
                                               "#" + name + " is not supported: FMinus's directives are #include, "
                                                            "#pragma, #define, #undef and the conditionals (#if, "
                                                            "#ifdef, #ifndef, #elif, #elifdef, #elifndef, #else, "
                                                            "#endif)");
            throw Parser::syntax_error(within(where, nameStart, std::max<std::size_t>(name.size(), 1)),
                                       "invalid preprocessing directive");
        }

        i = afterSpaces(line, i);
        if (i < line.size() && line[i] == '<')
            throw Parser::syntax_error(within(where, i, 1), "#include <...> is not supported: FMinus has no system "
                                                            "headers (write #include \"file\")");
        const std::size_t close = i < line.size() && line[i] == '"' ? line.find('"', i + 1) : std::string::npos;
        if (close == std::string::npos)
            throw Parser::syntax_error(within(where, i, 1), "expected \"FILENAME\" or <FILENAME>");
        const location quoted = within(where, i, close - i + 1);
        const std::string file = line.substr(i + 1, close - i - 1);
        if (file.empty())
            throw Parser::syntax_error(quoted, "empty filename");
        if (const std::size_t extra = afterSpaces(line, close + 1); extra < line.size())
            throw Parser::syntax_error(within(where, extra, line.size() - extra),
                                       "extra tokens at end of #include directive");

        // The file: found, not read yet, and not too deep.
        const fminus::SourceLocation from = fminus::frontend::sourceLocation(within(where, hash, 1));
        if (state.open.size() >= kMaxIncludeDepth)
            throw Parser::syntax_error(quoted, "#include nested too deeply");
        std::optional<fminus::frontend::IncludedFile> found;
        if (state.include != nullptr)
            found = (*state.include)(file, state.diagnostics->file(from.file).name);
        if (!found)
            throw Parser::syntax_error(quoted, "'" + file + "' file not found");
        if (found->text.size() > static_cast<std::size_t>(INT_MAX))
            throw Parser::syntax_error(quoted, "'" + file + "' is too long");
        if (!state.read.insert(found->name).second)
            return std::nullopt;   // read once
        state.files.push_back({state.diagnostics->addFile(found->name, from)});
        return found;
    }

    // Whether this scanner reads `name`, an identifier's spelling, as one of FMinus's keywords or type words: whether
    // a rule above {IDENTIFIER} matches it whole. A scanner of its own reads it, with no module and no macro, so the
    // keywords are the rules themselves, a new one's too, and no list of them can fall behind.
    bool isKeyword(std::string_view name)
    {
        if (name.size() > static_cast<std::size_t>(INT_MAX))
            return false;   // no keyword is as long, and the scanner reads no more
        fminus::Diagnostics diagnostics("");
        fminus::frontend::ParseContext context{{}, diagnostics};
        fminus::frontend::ScannerState state;
        state.context = &context;
        yyscan_t scanner = nullptr;
        if (yylex_init_extra(&state, &scanner) != 0)
            throw std::bad_alloc();
        const std::unique_ptr<void, decltype(&yylex_destroy)> owner(scanner, &yylex_destroy);   // and its buffer
        yy_scan_bytes(name.data(), static_cast<int>(name.size()), scanner);
        return fminus::frontend::nextToken(scanner).kind() != Parser::symbol_kind::S_IDENTIFIER;
    }
} // namespace

std::optional<std::string> fminus::frontend::macroNameError(std::string_view name)
{
    if (name.empty())
        return "macro name missing";
    if (!isIdentifierStart(name.front()) || !std::ranges::all_of(name, isIdentifierChar))
        return "macro name must be an identifier";
    if (name == "defined")
        return "'defined' cannot be used as a macro name";
    if (isKeyword(name))
        return "'" + std::string(name) + "' cannot be used as a macro name: it is one of FMinus's keywords";
    return std::nullopt;
}
