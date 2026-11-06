// src/frontend/ParseContext.h: what the parser builds and reports into (Parser.y's parse-param), and what the scanner
// reads from it: the modules declared so far, whose names it returns as types (C's way with typedef names), so that
// `OnePole lp;` and `OnePole* p` parse as declarations. Also the enumerations' tags declared so far, by scope, which
// the parser looks up for `enum Mode`.
#pragma once

#include "frontend/Ast.h"
#include "frontend/Diagnostics.h"

#include <string>
#include <unordered_map>
#include <vector>

#include <optional>

namespace fminus::frontend
{
    // A file the scanner reads, as Bison's locations point at it (Parser.y's api.filename.type): its index among the
    // Diagnostics' files. A location that points at none is in the program's own file.
    struct SourceFile
    {
        int index = 0;

        friend bool operator==(const SourceFile&, const SourceFile&) = default;   // Bison compares positions so
    };

    // An enumeration's tag: where it was declared (its definition, once it has one), its enumeration's id (the
    // enumerated type's, ast::Type::enumeration), its fixed underlying type if it has one (C23's `enum Mode : int`),
    // and whether its constants are defined: C23 declares a tag with a fixed type ahead of them, `enum Mode : int;`.
    struct Tag
    {
        SourceLocation location;
        int id = 0;
        std::optional<ast::Type> fixed = std::nullopt;
        bool defined = false;
    };

    struct ParseContext
    {
        ast::Program program;
        Diagnostics& diagnostics;
        std::unordered_map<std::string, const ast::Module*> moduleNames = {};   // from each module's name on
        ast::Module* currentModule = nullptr;                                     // the one being parsed

        // The enumerations' tags, by scope: the file's first (a module's enumerations are the file's, as a C struct's
        // are), then each block being parsed, which Parser.y opens at its `{` and closes at its `}`. `enum Mode` names
        // a tag visible there.
        std::vector<std::unordered_map<std::string, Tag>> tags = {{}};
        int enumerations = 0;   // the ids given so far, one for each enumeration (and each tag declared ahead)

        const ast::Module* moduleNamed(const std::string& name) const
        {
            const auto found = moduleNames.find(name);
            return found == moduleNames.end() ? nullptr : found->second;
        }

        // The tag `name`'s declaration, the innermost visible, or null.
        const Tag* tagNamed(const std::string& name) const
        {
            for (auto scope = tags.rbegin(); scope != tags.rend(); ++scope)
            {
                if (const auto found = scope->find(name); found != scope->end())
                    return &found->second;
            }
            return nullptr;
        }
    };
} // namespace fminus::frontend
