// src/frontend/ParseContext.h: what the parser builds and reports into (Parser.y's parse-param).
#pragma once

#include "frontend/Ast.h"
#include "frontend/Diagnostics.h"

namespace fminus::frontend
{
    struct ParseContext
    {
        ast::Program program;
        Diagnostics& diagnostics;
    };
} // namespace fminus::frontend
