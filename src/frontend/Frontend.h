// src/frontend/Frontend.h: FMinus v0 source to its syntax tree.
#pragma once

#include "frontend/Ast.h"
#include "frontend/Diagnostics.h"

#include <functional>
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

    // Lexes and parses `source` (the grammar is Parser.y), reading its #includes through `include` (without one, no
    // #include finds its file). Returns the program, or nothing when it reported errors into `diagnostics` (it stops at
    // the first syntax error, and at an #include that finds no file).
    std::optional<ast::Program> parse(std::string_view source, Diagnostics& diagnostics,
                                      const IncludeResolver& include = {});
} // namespace fminus::frontend
