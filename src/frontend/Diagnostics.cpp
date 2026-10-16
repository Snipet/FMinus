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

    void Diagnostics::error(SourceLocation location, std::string message)
    {
        errors_.push_back({location, std::move(message)});
    }

    std::string Diagnostics::format() const
    {
        std::vector<Diagnostic> sorted = errors_;
        std::ranges::stable_sort(sorted, [](const Diagnostic& a, const Diagnostic& b) {
            return a.location.line != b.location.line ? a.location.line < b.location.line
                                                      : a.location.column < b.location.column;
        });
        std::string text;
        for (const Diagnostic& diagnostic : sorted)
        {
            text += fileName_ + ":" + std::to_string(diagnostic.location.line) + ":"
                    + std::to_string(diagnostic.location.column) + ": error: " + diagnostic.message + "\n";
        }
        return text;
    }
} // namespace fminus
