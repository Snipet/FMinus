// src/frontend/Frontend.h: FMinus v0 source to its syntax tree.
#pragma once

#include "frontend/Ast.h"
#include "frontend/Diagnostics.h"

#include <functional>
#include <map>
#include <optional>
#include <string>
#include <string_view>

namespace fminus::frontend
{
    // A file an #include reads: its name, as the messages give it, and its text. The name is also how the scanner knows
    // a file it has read: each is read once, as if it began with #pragma once, so a second #include of a name reads
    // nothing (the program's own file's name, Diagnostics::fileName, is read from the start).
    struct IncludedFile
    {
        std::string name;
        std::string text;
    };

    // The file of `#include "name"` in the file named `from` (as the messages give it): the file, or nothing when there
    // is none ("'name' file not found").
    using IncludeResolver = std::function<std::optional<IncludedFile>(std::string_view name, std::string_view from)>;

    // Macros defined before the program's first line, C's -D: each name's value ("" for none). Each name passes
    // macroNameError.
    using Defines = std::map<std::string, std::string>;

    // Lexes and parses `source` (the grammar is Parser.y), reading its #includes through `include` (without one, no
    // #include finds its file), with `defines` defined, and the macros FMinus predefines (predefinedMacros). Returns
    // the program, or nothing when it reported errors into `diagnostics` (it stops at the first syntax error, and at an
    // #include that finds no file).
    std::optional<ast::Program> parse(std::string_view source, Diagnostics& diagnostics,
                                      const IncludeResolver& include = {}, const Defines& defines = {});

    // Why `name` cannot name a macro, in clang's words, or nothing: it must be an identifier, not `defined` (C's rule),
    // not a macro FMinus predefines (__FMINUS_JSON__, __FMINUS_XML__, __FMINUS_WEB__, __FMINUS_WEB_SERVER__,
    // __FMINUS_FILES__, __FMINUS_FILES_WRITE__: Preprocessor.h), and not one of
    // FMinus's keywords, which a macro would hide in C (FMinus substitutes none in code). In Lexer.l, beside the
    // keywords' rules.
    std::optional<std::string> macroNameError(std::string_view name);
} // namespace fminus::frontend
