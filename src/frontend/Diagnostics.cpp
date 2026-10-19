// src/frontend/Diagnostics.cpp
#include "frontend/Diagnostics.h"

#include <algorithm>
#include <utility>

namespace fminus
{
    Diagnostics::Diagnostics(std::string fileName)
        : fileName_(std::move(fileName))
    {
    }

    void Diagnostics::error(SourceLocation location, std::string message, std::vector<Diagnostic> notes)
    {
        errors_.push_back({location, std::move(message), std::move(notes)});
    }

    std::string Diagnostics::format() const
    {
        std::vector<Diagnostic> sorted = errors_;
        std::ranges::stable_sort(sorted, [](const Diagnostic& a, const Diagnostic& b) {
            return a.location.line != b.location.line ? a.location.line < b.location.line
                                                      : a.location.column < b.location.column;
        });
        const auto line = [this](const Diagnostic& diagnostic, const char* severity) {
            return fileName_ + ":" + std::to_string(diagnostic.location.line) + ":"
                   + std::to_string(diagnostic.location.column) + ": " + severity + ": " + diagnostic.message + "\n";
        };
        std::string text;
        for (const Diagnostic& diagnostic : sorted)
        {
            text += line(diagnostic, "error");
            for (const Diagnostic& note : diagnostic.notes)
                text += line(note, "note");
        }
        return text;
    }
} // namespace fminus
