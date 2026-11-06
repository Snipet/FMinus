// src/frontend/Preprocessor.h: what FMinus has of C's preprocessor besides #include: macros, and the expressions of
// #if and #elif. The scanner (Lexer.l's directive()) reads the lines; this is the text of them it hands over.
//
// A macro is C's object-like one, a name and a value (`#define LEVEL 2`, `#define DEBUG`, or the host's,
// Program::Options::defines), but FMinus reads it only where C's preprocessor reads it as a condition: in #if and
// #elif, where it expands to its value, and in #ifdef, #ifndef and defined(), where it is defined or not. A macro's
// name in code is an error, never a substitution, so a program FMinus compiles means the same in C. An #if's
// expression is C's integer constant expression of the preprocessor (C17 6.10.1), in 64-bit integers: numbers,
// macros, defined, true and false (C23's 1 and 0), any other name 0, the unary and binary operators and ?:.
// Nothing here knows Flex or Bison: an error is a PreprocessorError, at a place in the directive's line, which the
// scanner turns into a syntax error there.
#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>
#include <map>
#include <string>
#include <string_view>

namespace fminus::frontend
{
    // The macros defined so far: each name's value, its text as written after the name, without its comments and
    // with each run of spaces one space, as C compares two definitions of a macro (empty for `#define DEBUG`).
    using Macros = std::map<std::string, std::string, std::less<>>;

    // An error in a directive's line: the bytes [offset, offset + length) of the line, and clang's words.
    struct PreprocessorError
    {
        std::size_t offset = 0;
        std::size_t length = 1;
        std::string message;
    };

    // The directive's line from `from` on, its comments made spaces, as C's translation reads a line before the
    // directive does (the bytes keep their places). A /* comment must end on the line: a directive ends at its line's
    // end. Throws PreprocessorError.
    std::string withoutComments(std::string_view line, std::size_t from);

    // The value of the expression from `from` to the end of `line` (an #if's or an #elif's, after withoutComments):
    // its macros expanded, each to its value, C's way (a macro is not expanded inside its own value). The operands of
    // && and || and the branch of ?: that are not computed raise no error, as in C. Throws PreprocessorError: a
    // token in a macro's value is at fault at the macro's name.
    std::int64_t evaluate(std::string_view line, std::size_t from, const Macros& macros);

    // A value as Macros keeps it: trimmed, each run of spaces one space.
    std::string normalisedValue(std::string_view value);
} // namespace fminus::frontend
