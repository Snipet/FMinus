// src/frontend/Frontend.h: FMinus v0 source to its syntax tree.
#pragma once

#include "frontend/Ast.h"
#include "frontend/Diagnostics.h"

#include <optional>
#include <string_view>

namespace fminus::frontend
{
    // Lexes and parses `source` (the grammar is Parser.y). Returns the program, or nothing when it reported errors into
    // `diagnostics` (it stops at the first syntax error).
    std::optional<ast::Program> parse(std::string_view source, Diagnostics& diagnostics);
} // namespace fminus::frontend
