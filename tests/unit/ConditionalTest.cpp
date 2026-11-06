// tests/unit/ConditionalTest.cpp: C's conditionals and macros (src/frontend/Lexer.l, Preprocessor.cpp): #if, #ifdef,
// #ifndef, #elif, #else and #endif choose the groups the scanner reads, on macros that #define, #undef and the host's
// defines make; a skipped group is never scanned; an #if's expression is C's; a macro's name in code is an error; and
// every directive's error is clang's, at its place.
#include "frontend/Frontend.h"

#include <catch2/catch_test_macros.hpp>

#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace
{
    using Files = std::map<std::string, std::string, std::less<>>;

    // The program's dump, or its errors, with `defines` the host's, and `files` what an #include finds (each look-up
    // into `asked`, by name).
    std::string parsed(std::string_view source, const fminus::frontend::Defines& defines = {}, const Files& files = {},
                       std::vector<std::string>* asked = nullptr)
    {
        fminus::Diagnostics diagnostics("test.fm");
        const fminus::frontend::IncludeResolver include =
            [&](std::string_view name, std::string_view) -> std::optional<fminus::frontend::IncludedFile> {
            if (asked != nullptr)
                asked->emplace_back(name);
            const auto found = files.find(name);
            if (found == files.end())
                return std::nullopt;
            return fminus::frontend::IncludedFile{found->first, found->second};
        };
        const auto program = fminus::frontend::parse(source, diagnostics, include, defines);
        return program ? fminus::ast::dump(*program) : diagnostics.format();
    }

    // Which group of `#if <condition>` the scanner reads: "yes", "no", or the errors.
    std::string condition(std::string_view expression, const fminus::frontend::Defines& defines = {})
    {
        const std::string dump = parsed("#if " + std::string(expression) + "\n"
                                        "float yes() { return 1; }\n"
                                        "#else\n"
                                        "float no() { return 0; }\n"
                                        "#endif\n",
                                        defines);
        if (dump.starts_with("(function float yes"))
            return "yes";
        if (dump.starts_with("(function float no"))
            return "no";
        return dump;
    }

    const std::string kOne = "(function float f ()\n  (return 1))\n";
    const std::string kTwo = "(function float f ()\n  (return 2))\n";
} // namespace

TEST_CASE("#if, #ifdef, #ifndef, #elif and #else choose the group the scanner reads", "[frontend][conditional]")
{
    const std::string program = "#ifdef DEBUG\n"
                                "float f() { return 1; }\n"
                                "#else\n"
                                "float f() { return 2; }\n"
                                "#endif\n";
    CHECK(parsed(program) == kTwo);
    CHECK(parsed("#define DEBUG\n" + program) == kOne);
    CHECK(parsed("#define DEBUG\n#undef DEBUG\n" + program) == kTwo);
    CHECK(parsed("#ifndef DEBUG\nfloat f() { return 1; }\n#endif\n") == kOne);
    CHECK(parsed("#define DEBUG 0\n#ifndef DEBUG\nfloat f() { return 1; }\n#endif\n") == "");   // defined, though 0

    // #elif: the first group whose condition holds, and none after it, whatever theirs.
    const std::string chain = "#if LEVEL == 1\n"
                              "float f() { return 1; }\n"
                              "#elif LEVEL >= 2\n"
                              "float f() { return 2; }\n"
                              "#elif LEVEL >= 1\n"
                              "float f() { return 3; }\n"
                              "#else\n"
                              "float f() { return 4; }\n"
                              "#endif\n";
    CHECK(parsed("#define LEVEL 1\n" + chain) == kOne);
    CHECK(parsed("#define LEVEL 3\n" + chain) == kTwo);
    CHECK(parsed(chain) == "(function float f ()\n  (return 4))\n");
    // C23's #elifdef and #elifndef: #elif defined(NAME), and #elif !defined(NAME).
    const std::string named = "#ifdef A\n"
                              "float f() { return 1; }\n"
                              "#elifdef B\n"
                              "float f() { return 2; }\n"
                              "#elifndef C\n"
                              "float f() { return 3; }\n"
                              "#endif\n";
    CHECK(parsed(named, {{"A", ""}, {"B", ""}}) == kOne);
    CHECK(parsed(named, {{"B", ""}}) == kTwo);
    CHECK(parsed(named) == "(function float f ()\n  (return 3))\n");
    CHECK(parsed(named, {{"C", ""}}) == "");

    // Nested: a conditional in a skipped group is skipped whole, its own #else too.
    CHECK(parsed("#if 0\n"
                 "#if 1\n"
                 "float f() { return 3; }\n"
                 "#else\n"
                 "float f() { return 4; }\n"
                 "#endif\n"
                 "#else\n"
                 "#ifdef NOPE\n"
                 "float f() { return 5; }\n"
                 "#elif 1\n"
                 "float f() { return 1; }\n"
                 "#endif\n"
                 "#endif\n")
          == kOne);

    // Any line, a module's fields and a body's statements too: instrumentation compiled in or out.
    const std::string meter = "module Meter\n"
                              "{\n"
                              "    float y;\n"
                              "#ifdef DEBUG\n"
                              "    float peak;\n"
                              "#endif\n"
                              "    float tick(float x)\n"
                              "    {\n"
                              "        y = x;\n"
                              "#ifdef DEBUG\n"
                              "        peak = fmaxf(peak, fabsf(x));\n"
                              "#endif\n"
                              "        return y;\n"
                              "    }\n"
                              "};\n";
    CHECK(parsed(meter) == "(module Meter\n"
                           "  (field float y))\n"
                           "(method float Meter::tick ((float x))\n"
                           "  (= y x)\n"
                           "  (return y))\n");
    CHECK(parsed(meter, {{"DEBUG", "1"}}) == "(module Meter\n"
                                              "  (field float y)\n"
                                              "  (field float peak))\n"
                                              "(method float Meter::tick ((float x))\n"
                                              "  (= y x)\n"
                                              "  (= peak (call fmaxf peak (call fabsf x)))\n"
                                              "  (return y))\n");
}

TEST_CASE("a skipped group is never scanned: only its comments and its conditionals' lines are read",
          "[frontend][conditional]")
{
    // Not FMinus, an unknown directive, an #include and a #define: none of them is read.
    std::vector<std::string> asked;
    CHECK(parsed("#if 0\n"
                 "this is not FMinus: 'quotes, $, @ and \"strings\n"
                 "#warning what C has\n"
                 "#include \"missing.fm\"\n"
                 "#define DEBUG\n"
                 "#endif\n"
                 "#ifndef DEBUG\n"
                 "float f() { return 1; }\n"
                 "#endif\n",
                 {}, {}, &asked)
          == kOne);
    CHECK(asked.empty());

    // A block comment hides the #endif inside it; a line comment ends with its line.
    CHECK(parsed("#if 0\n"
                 "/*\n"
                 "#endif\n"
                 "*/ float g() { return 2; }\n"
                 "// #else\n"
                 "#else\n"
                 "float f() { return 1; }\n"
                 "#endif /* the end */ // of it\n")
          == kOne);

    // A # inside a line starts no directive, in a skipped group as in code.
    CHECK(parsed("#if 0\nfloat f() { return 2; } #endif\n#endif\nfloat f() { return 1; }\n") == kOne);

    // Lines are counted through a skipped group and the directives.
    CHECK(parsed("#if 0\n\n/* two\nlines */\n#endif\n#define X\nfloat f() { return 1 + ; }\n")
          == "test.fm:7:24: error: syntax error, unexpected ;\n");
}

TEST_CASE("an #if's expression is C's integer constant expression, in 64-bit integers", "[frontend][conditional]")
{
    // Numbers, C's precedence and operators.
    for (const char* expression :
         {"1", "-1", "0x10 == 16", "010 == 8", "0 == 0", "1 + 2 * 3 == 7", "(1 + 2) * 3 == 9", "7 / 2 == 3",
          "-7 / 2 == -3", "-7 % 2 == -1", "~0 == -1", "!0", "-(-3) == +3", "1 << 62 > 0", "(1 << 63) < 0",
          "-8 >> 1 == -4", "1 << 64 == 1", "(3 & 5) == 1", "(3 | 5) == 7", "(3 ^ 5) == 6", "3 & 5 == 1 || 1",
          "1 < 2 && 2 <= 2 && 3 > 2 && 3 >= 3 && 1 != 2", "1 ? 2 : 0", "0 ? 0 : 3", "1 ? 0 ? 0 : 4 : 0",
          "9223372036854775807 + 1 < 0", "(-9223372036854775807 - 1) / -1 < 0", "(-9223372036854775807 - 1) % -1 == 0",
          "true", "!false", "true + true == 2"})
    {
        INFO(expression);
        CHECK(condition(expression) == "yes");
    }
    for (const char* expression : {"0", "1 - 1", "!1", "NOPE", "float", "false", "defined NOPE", "defined(NOPE)"})
    {
        INFO(expression);
        CHECK(condition(expression) == "no");
    }

    // && and || short-circuit, and ?: computes one branch: a division by zero in the others is no error.
    CHECK(condition("0 && 1 / 0") == "no");
    CHECK(condition("1 || 1 % 0") == "yes");
    CHECK(condition("1 ? 1 : 1 / 0") == "yes");
    CHECK(condition("0 ? 1 / 0 : 1") == "yes");

    // Macros expand to their values, in turn, C's way: as text, and never inside their own values.
    const fminus::frontend::Defines defines{{"DEBUG", ""}, {"LEVEL", "2"}, {"TWO", "1 + 1"},  {"A", "B"},
                                            {"B", "LEVEL"}, {"SELF", "SELF"}, {"NAME", "DEBUG"}};
    for (const char* expression : {"LEVEL >= 2", "TWO * 2 == 3", "A == 2", "!SELF", "defined DEBUG", "defined(LEVEL)",
                                   "defined NAME && !defined(NOPE)", "defined(TWO) + defined LEVEL == 2"})
    {
        INFO(expression);
        CHECK(condition(expression, defines) == "yes");
    }
    // defined's operand is the name itself, never expanded: NAME is defined, though its value names no macro.
    CHECK(condition("defined(NAME)", {{"NAME", "NOPE"}}) == "yes");
    CHECK(condition("NAME", {{"NAME", "NOPE"}}) == "no");
}

TEST_CASE("the host's defines are macros from before the first line", "[frontend][conditional]")
{
    const std::string program = "#if defined(DEBUG) && LEVEL > 1\n"
                                "float f() { return 1; }\n"
                                "#else\n"
                                "float f() { return 2; }\n"
                                "#endif\n";
    CHECK(parsed(program) == kTwo);
    CHECK(parsed(program, {{"DEBUG", "1"}, {"LEVEL", "2"}}) == kOne);
    CHECK(parsed(program, {{"DEBUG", "1"}, {"LEVEL", " 1 "}}) == kTwo);

    // The file's definition of one: the same value (spaces aside) is no redefinition, as in C; the idiom of a default.
    CHECK(parsed("#define LEVEL 1  +  1\n#if LEVEL == 2\nfloat f() { return 1; }\n#endif\n", {{"LEVEL", "1 + 1"}})
          == kOne);
    CHECK(parsed("#ifndef LEVEL\n#define LEVEL 2\n#endif\n#if LEVEL == 3\nfloat f() { return 1; }\n#endif\n",
                 {{"LEVEL", "3"}})
          == kOne);
    CHECK(parsed("#define LEVEL 2\n", {{"LEVEL", "3"}}) == "test.fm:1:9: error: 'LEVEL' macro redefined\n");
    CHECK(parsed("float f() { return 1; }\n", {{"1X", ""}})
          == "test.fm:1:1: error: invalid macro definition '1X': macro name must be an identifier\n");

    // Their names, as the front end checks one for the host.
    CHECK(fminus::frontend::macroNameError("DEBUG") == std::nullopt);
    CHECK(fminus::frontend::macroNameError("_x1") == std::nullopt);
    CHECK(fminus::frontend::macroNameError("") == "macro name missing");
    CHECK(fminus::frontend::macroNameError("1X") == "macro name must be an identifier");
    CHECK(fminus::frontend::macroNameError("A-B") == "macro name must be an identifier");
    CHECK(fminus::frontend::macroNameError("defined") == "'defined' cannot be used as a macro name");
    for (const char* keyword : {"float", "const", "return", "bool", "int", "void", "restrict", "sizeof", "float2",
                                "float4", "float8", "poly", "true", "false", "if", "else", "while", "for", "break",
                                "continue", "switch", "case", "default", "enum", "parallel", "module", "this", "param"})
    {
        INFO(keyword);
        CHECK(fminus::frontend::macroNameError(keyword)
              == "'" + std::string(keyword) + "' cannot be used as a macro name: it is one of FMinus's keywords");
    }
}

TEST_CASE("a macro is read only in conditions: its name in code is an error", "[frontend][conditional]")
{
    const std::string error = "error: macro expansion is not supported: 'TAPS' is a macro, which FMinus reads only in "
                              "#if, #elif, #ifdef and #ifndef (for a constant, declare an enum or a const)\n";
    CHECK(parsed("#define TAPS 16\nfloat f() { float buf[TAPS]; return buf[0]; }\n") == "test.fm:2:23: " + error);
    CHECK(parsed("float f() { return TAPS; }\n", {{"TAPS", "1"}}) == "test.fm:1:20: " + error);
    // Undefined, it is a name again; a skipped group's names are never read.
    CHECK(parsed("#define TAPS\n#undef TAPS\nfloat f(float TAPS) { return 1; }\n")
          == "(function float f ((float TAPS))\n  (return 1))\n");
    CHECK(parsed("#define TAPS 16\n#if 0\nfloat buf[TAPS];\n#endif\nfloat f() { return 1; }\n") == kOne);
}

TEST_CASE("conditionals end in the file they begin in, and macros are the compilation's", "[frontend][conditional]")
{
    const Files files{{"debug.fm", "#define DEBUG\n"},
                      {"open.fm", "#ifdef DEBUG\nfloat g() { return 2; }\n"},
                      {"close.fm", "#endif\n"},
                      {"group.fm", "#if 1\nfloat f() { return 1; }\n#else\n#endif\n"}};
    // A macro an included file defines is defined after its #include.
    CHECK(parsed("#include \"debug.fm\"\n#ifdef DEBUG\nfloat f() { return 1; }\n#endif\n", {}, files) == kOne);
    CHECK(parsed("#include \"group.fm\"\n", {}, files) == kOne);
    // An #include in a skipped group reads nothing; one in a group read reads its file.
    std::vector<std::string> asked;
    CHECK(parsed("#ifdef NOPE\n#include \"debug.fm\"\n#else\n#include \"group.fm\"\n#endif\n", {}, files, &asked)
          == kOne);
    CHECK(asked == std::vector<std::string>{"group.fm"});
    // An #if that its file leaves open, and an #endif for the including file's #if.
    CHECK(parsed("#include \"open.fm\"\n#endif\n", {}, files)
          == "In file included from test.fm:1:\nopen.fm:1:2: error: unterminated conditional directive\n");
    CHECK(parsed("#if 1\n#include \"close.fm\"\n", {}, files)
          == "In file included from test.fm:2:\nclose.fm:1:2: error: #endif without #if\n");
}

TEST_CASE("the directives' errors are clang's, at their places", "[frontend][conditional]")
{
    // The conditionals' order.
    CHECK(parsed("#if 1\nfloat f() { return 1; }\n") == "test.fm:1:2: error: unterminated conditional directive\n");
    CHECK(parsed("  #  ifdef X\n") == "test.fm:1:6: error: unterminated conditional directive\n");
    CHECK(parsed("#if 0\n#if 1\n") == "test.fm:2:2: error: unterminated conditional directive\n");
    CHECK(parsed("#if 0\n/* open\n#endif\n")
          == "test.fm:2:1: error: unterminated /* comment\n");
    CHECK(parsed("#else\n") == "test.fm:1:2: error: #else without #if\n");
    CHECK(parsed("#endif\n") == "test.fm:1:2: error: #endif without #if\n");
    CHECK(parsed("#elif 1\n") == "test.fm:1:2: error: #elif without #if\n");
    CHECK(parsed("#if 1\n#endif\n#endif\n") == "test.fm:3:2: error: #endif without #if\n");
    CHECK(parsed("#if 1\n#else\n#else\n#endif\n") == "test.fm:3:2: error: #else after #else\n");
    CHECK(parsed("#if 1\n#else\n#elif 1\n#endif\n") == "test.fm:3:2: error: #elif after #else\n");
    CHECK(parsed("#if 0\n#else\n#elifndef X\n#endif\n") == "test.fm:3:2: error: #elifndef after #else\n");
    CHECK(parsed("#elifdef X\n") == "test.fm:1:2: error: #elifdef without #if\n");
    CHECK(parsed("#if 0\n#elifdef X Y\n#endif\n")
          == "test.fm:2:12: error: extra tokens at end of #elifdef directive\n");
    CHECK(parsed("#if 0\n#elifdef\n#endif\n") == "test.fm:2:9: error: macro name missing\n");
    CHECK(parsed("#if 0\n#if 1\n#else\n#else\n#endif\n#endif\n") == "test.fm:4:2: error: #else after #else\n");

    // Extra tokens, which clang warns of; none is read in a skipped group's conditional.
    CHECK(parsed("#ifdef X Y\n#endif\n") == "test.fm:1:10: error: extra tokens at end of #ifdef directive\n");
    CHECK(parsed("#ifndef X Y\n#endif\n") == "test.fm:1:11: error: extra tokens at end of #ifndef directive\n");
    CHECK(parsed("#if 1\n#else X\n#endif\n") == "test.fm:2:7: error: extra tokens at end of #else directive\n");
    CHECK(parsed("#if 0\n#else X\n#endif\n") == "test.fm:2:7: error: extra tokens at end of #else directive\n");
    CHECK(parsed("#if 1\n#endif X\n") == "test.fm:2:8: error: extra tokens at end of #endif directive\n");
    CHECK(parsed("#undef X Y\n") == "test.fm:1:10: error: extra tokens at end of #undef directive\n");
    CHECK(parsed("#if 0\n#if 1\n#else X\n#endif Y\n#endif\nfloat f() { return 1; }\n") == kOne);
    // An #elif after a group read is never computed.
    CHECK(parsed("#if 1\nfloat f() { return 1; }\n#elif 1 / 0 +\n#endif\n") == kOne);
    CHECK(parsed("#if 0\n#elif 1 / 0\n#endif\n")
          == "test.fm:2:9: error: division by zero in preprocessor expression\n");

    // Macro names.
    CHECK(parsed("#define\n") == "test.fm:1:8: error: macro name missing\n");
    CHECK(parsed("#define 1\n") == "test.fm:1:9: error: macro name must be an identifier\n");
    CHECK(parsed("#define defined\n") == "test.fm:1:9: error: 'defined' cannot be used as a macro name\n");
    CHECK(parsed("#undef defined\n") == "test.fm:1:8: error: 'defined' cannot be used as a macro name\n");
    CHECK(parsed("#define float double\n")
          == "test.fm:1:9: error: 'float' cannot be used as a macro name: it is one of FMinus's keywords\n");
    CHECK(parsed("#undef\n") == "test.fm:1:7: error: macro name missing\n");
    CHECK(parsed("#ifdef\n#endif\n") == "test.fm:1:7: error: macro name missing\n");
    CHECK(parsed("#ifdef 1\n#endif\n") == "test.fm:1:8: error: macro name must be an identifier\n");
    CHECK(parsed("#define X 1\n#define X 2\n") == "test.fm:2:9: error: 'X' macro redefined\n");
    CHECK(parsed("#define X 1 /* one */\n#define X  1 // one\n#undef X\n#define X 2\n") == "");
    CHECK(parsed("#define F(x) x\n")
          == "test.fm:1:10: error: function-like macros are not supported: a macro in FMinus is a name and a value, "
             "for #if\n");
    CHECK(parsed("#define X 1 /* one\n */\n")
          == "test.fm:1:13: error: a directive's /* comment must end on its line\n");

    // Expressions.
    CHECK(parsed("#if\n#endif\n") == "test.fm:1:4: error: expected value in expression\n");
    CHECK(condition("1 +") == "test.fm:1:8: error: expected value in expression\n");
    CHECK(condition("()") == "test.fm:1:6: error: expected value in expression\n");
    CHECK(condition("(1") == "test.fm:1:7: error: expected ')' in preprocessor expression\n");
    CHECK(condition("1 2") == "test.fm:1:7: error: token is not a valid binary operator in a preprocessor "
                              "subexpression\n");
    CHECK(condition("(1 2)") == "test.fm:1:8: error: token is not a valid binary operator in a preprocessor "
                                "subexpression\n");
    CHECK(condition("1 = 1") == "test.fm:1:7: error: token is not a valid binary operator in a preprocessor "
                                "subexpression\n");
    CHECK(condition("1)") == "test.fm:1:6: error: expected end of line in preprocessor expression\n");
    CHECK(condition("1, 2") == "test.fm:1:6: error: expected end of line in preprocessor expression\n");
    CHECK(condition("* 1") == "test.fm:1:5: error: invalid token at start of a preprocessor expression\n");
    CHECK(condition("$") == "test.fm:1:5: error: invalid token at start of a preprocessor expression\n");
    CHECK(condition("1 ? 2") == "test.fm:1:10: error: expected ':'\n");
    CHECK(condition("1.5") == "test.fm:1:5: error: floating point literal in preprocessor expression\n");
    CHECK(condition("1e5") == "test.fm:1:5: error: floating point literal in preprocessor expression\n");
    CHECK(condition("0x1p3") == "test.fm:1:5: error: floating point literal in preprocessor expression\n");
    CHECK(condition("1f") == "test.fm:1:5: error: invalid suffix 'f' on integer constant\n");
    CHECK(condition("0x") == "test.fm:1:5: error: invalid suffix 'x' on integer constant\n");
    CHECK(condition("1u") == "test.fm:1:5: error: invalid suffix 'u' on integer constant\n");
    CHECK(condition("08") == "test.fm:1:5: error: invalid digit '8' in octal constant\n");
    CHECK(condition("9223372036854775808")
          == "test.fm:1:5: error: integer literal is too large to be represented in a signed integer type\n");
    CHECK(condition("1 / 0") == "test.fm:1:7: error: division by zero in preprocessor expression\n");
    CHECK(condition("1 % (2 - 2)") == "test.fm:1:7: error: remainder by zero in preprocessor expression\n");
    CHECK(condition("defined") == "test.fm:1:12: error: macro name missing\n");
    CHECK(condition("defined(") == "test.fm:1:13: error: macro name missing\n");
    CHECK(condition("defined(X") == "test.fm:1:14: error: missing ')' after 'defined'\n");
    CHECK(condition("defined 1") == "test.fm:1:13: error: macro name must be an identifier\n");
    CHECK(condition("1 /* open") == "test.fm:1:7: error: a directive's /* comment must end on its line\n");
    // A macro's value is at fault at its name: empty, or not an expression.
    CHECK(condition("X", {{"X", ""}}) == "test.fm:1:6: error: expected value in expression\n");
    CHECK(condition("1 + X", {{"X", "2.5"}})
          == "test.fm:1:9: error: floating point literal in preprocessor expression\n");

    // Limits: nesting and an expansion that doubles.
    CHECK(condition(std::string(257, '(') + "1" + std::string(257, ')'))
          == "test.fm:1:261: error: preprocessor expression nested too deeply: more than 256 levels\n");
    CHECK(condition(std::string(300, '!') + "0") == "test.fm:1:261: error: preprocessor expression nested too deeply: "
                                                    "more than 256 levels\n");
    fminus::frontend::Defines doubling{{"M0", "1"}};
    for (int i = 1; i <= 20; ++i)
        doubling["M" + std::to_string(i)] = "M" + std::to_string(i - 1) + " + M" + std::to_string(i - 1);
    CHECK(condition("M20", doubling) == "test.fm:1:5: error: macro expansion is too long: more than 65536 tokens\n");
    CHECK(condition("M10 == 1024", doubling) == "yes");
}
