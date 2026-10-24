// src/frontend/ParseContext.h: what the parser builds and reports into (Parser.y's parse-param), and what the scanner
// reads from it: the modules declared so far, whose names it returns as types (C's way with typedef names), so that
// `OnePole lp;` and `OnePole* p` parse as declarations.
#pragma once

#include "frontend/Ast.h"
#include "frontend/Diagnostics.h"

#include <string>
#include <unordered_map>

namespace fminus::frontend
{
    // A file the scanner reads, as Bison's locations point at it (Parser.y's api.filename.type): its index among the
    // Diagnostics' files. A location that points at none is in the program's own file.
    struct SourceFile
    {
        int index = 0;

        friend bool operator==(const SourceFile&, const SourceFile&) = default;   // Bison compares positions so
    };

    struct ParseContext
    {
        ast::Program program;
        Diagnostics& diagnostics;
        std::unordered_map<std::string, const ast::Module*> moduleNames = {};   // from each module's name on
        ast::Module* currentModule = nullptr;                                     // the one being parsed

        const ast::Module* moduleNamed(const std::string& name) const
        {
            const auto found = moduleNames.find(name);
            return found == moduleNames.end() ? nullptr : found->second;
        }
    };
} // namespace fminus::frontend
