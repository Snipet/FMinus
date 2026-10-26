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
 *
 * A line that starts with # (after spaces) is a directive, C's preprocessor's, of which FMinus has two:
 * `#include "file"` reads the file there, as C does (through the IncludeResolver the front end was given), and
 * `#pragma ...` is ignored, as C ignores the pragmas it does not know. Each file is read once: an #include of a file
 * read already (the program's own too) reads nothing, as if every file began with #pragma once. The scanner reads an
 * included file from a buffer of its own and goes back to the one it interrupted at the file's end; its locations
 * point at the file (SourceFile), whose index in the Diagnostics names it in the messages.
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
#include <algorithm>
#include <climits>
#include <optional>
#include <string>
#include <string_view>
#include <utility>

using Parser = fminus::frontend::Parser;

namespace
{
    // The directive on one line, `text` (its first # after spaces), at the scanner's location: the file an #include
    // reads, registered with the Diagnostics and the scanner's files, or nothing (a #pragma, an #include of a file
    // read already, C's empty directive #). Throws the directive's error.
    std::optional<fminus::frontend::IncludedFile> directive(fminus::frontend::ScannerState& state,
                                                            std::string_view text);
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
    yyextra->where.step();
%}

[ \t\r]+                    { yyextra->where.step(); }
\n+                         { yyextra->where.lines(static_cast<int>(yyleng)); yyextra->where.step(); }
"//".*                      { yyextra->where.step(); }

^[ \t]*"#"[^\n]*            {
                                std::optional<fminus::frontend::IncludedFile> file
                                    = directive(*yyextra, std::string_view(yytext, static_cast<std::size_t>(yyleng)));
                                if (!file)
                                    yyextra->where.step();
                                else
                                {
                                    // Read the file from here (Flex copies its text), then come back after the
                                    // #include (<<EOF>>).
                                    yyextra->open.push_back({nullptr, YY_CURRENT_BUFFER, yyextra->where});
                                    yyextra->open.back().buffer = yy_scan_bytes(
                                        file->text.data(), static_cast<int>(file->text.size()), yyscanner);
                                    yyextra->where.initialize(&yyextra->files.back());
                                }
                            }

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
<<EOF>>                     {
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

    constexpr std::size_t kMaxIncludeDepth = 200;   // clang's

    // C's directives FMinus does not have.
    constexpr std::string_view kDirectivesOfC[] = {"define", "undef", "if",   "ifdef", "ifndef", "elif",    "elifdef",
                                                   "elifndef", "else", "endif", "line", "error", "warning", "embed"};

    bool isSpace(char c)
    {
        return c == ' ' || c == '\t' || c == '\r' || c == '\f' || c == '\v';
    }

    bool isIdentifierChar(char c)
    {
        return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_';
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

    // Whether the rest of the line from `i` is spaces and comments: // to its end, and /* */ closed on it.
    bool onlyComments(std::string_view text, std::size_t i)
    {
        while (true)
        {
            while (i < text.size() && isSpace(text[i]))
                ++i;
            if (i == text.size() || text.substr(i, 2) == "//")
                return true;
            if (text.substr(i, 2) != "/*")
                return false;
            const std::size_t end = text.find("*/", i + 2);
            if (end == std::string_view::npos)
                return false;
            i = end + 2;
        }
    }

    std::optional<fminus::frontend::IncludedFile> directive(fminus::frontend::ScannerState& state,
                                                            std::string_view text)
    {
        const location& where = state.where;
        std::size_t i = 0;
        const auto skipSpaces = [&] {
            while (i < text.size() && isSpace(text[i]))
                ++i;
        };
        skipSpaces();
        const std::size_t hash = i++;
        skipSpaces();
        const std::size_t nameStart = i;
        while (i < text.size() && isIdentifierChar(text[i]))
            ++i;
        const std::string name(text.substr(nameStart, i - nameStart));
        if (name.empty() && onlyComments(text, i))
            return std::nullopt;   // C's null directive
        if (name == "pragma")
            return std::nullopt;   // C ignores the pragmas it does not know; once is what FMinus does for every file
        if (name != "include")
        {
            for (const std::string_view other : kDirectivesOfC)
                if (name == other)
                    throw Parser::syntax_error(within(where, nameStart, name.size()),
                                               "#" + name + " is not supported: FMinus's directives are #include and "
                                                            "#pragma");
            throw Parser::syntax_error(within(where, nameStart, std::max<std::size_t>(name.size(), 1)),
                                       "invalid preprocessing directive");
        }

        skipSpaces();
        if (i < text.size() && text[i] == '<')
            throw Parser::syntax_error(within(where, i, 1), "#include <...> is not supported: FMinus has no system "
                                                            "headers (write #include \"file\")");
        const std::size_t close = i < text.size() && text[i] == '"' ? text.find('"', i + 1) : std::string_view::npos;
        if (close == std::string_view::npos)
            throw Parser::syntax_error(within(where, i, 1), "expected \"FILENAME\" or <FILENAME>");
        const location quoted = within(where, i, close - i + 1);
        const std::string file(text.substr(i + 1, close - i - 1));
        if (file.empty())
            throw Parser::syntax_error(quoted, "empty filename");
        if (!onlyComments(text, close + 1))
        {
            std::size_t extra = close + 1;
            while (extra < text.size() && isSpace(text[extra]))
                ++extra;
            throw Parser::syntax_error(within(where, extra, text.size() - extra),
                                       "extra tokens at end of #include directive");
        }

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
} // namespace
