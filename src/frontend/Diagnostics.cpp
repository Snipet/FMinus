// src/frontend/Diagnostics.cpp
#include "frontend/Diagnostics.h"

#include <algorithm>
#include <utility>

namespace fminus
{
    Diagnostics::Diagnostics(std::string fileName)
    {
        files_.push_back({std::move(fileName), {}});
    }

    void Diagnostics::error(SourceLocation location, std::string message, std::vector<Diagnostic> notes)
    {
        errors_.push_back({location, std::move(message), std::move(notes)});
    }

    int Diagnostics::addFile(std::string name, SourceLocation includedFrom)
    {
        files_.push_back({std::move(name), includedFrom});
        return static_cast<int>(files_.size()) - 1;
    }

    std::string Diagnostics::format() const
    {
        // Where a location is in the order the text is read: the line and column of each #include on the way to it,
        // from the program's own file in, then its own. Compared as a sequence, an included file's lines sort where
        // its #include is.
        const auto readOrder = [this](SourceLocation location) {
            std::vector<std::pair<int, int>> order{{location.line, location.column}};
            for (int file = location.file; file > 0; file = files_[static_cast<std::size_t>(file)].includedFrom.file)
            {
                const SourceLocation& from = files_[static_cast<std::size_t>(file)].includedFrom;
                order.emplace_back(from.line, from.column);
            }
            std::ranges::reverse(order);
            return order;
        };
        std::vector<Diagnostic> sorted = errors_;
        std::ranges::stable_sort(sorted, [&](const Diagnostic& a, const Diagnostic& b) {
            return readOrder(a.location) < readOrder(b.location);
        });

        // clang's include stack: the #includes that read `file`, the program's own file's first.
        const auto includeStack = [this](int file) {
            std::vector<const SourceLocation*> chain;
            for (; file > 0; file = files_[static_cast<std::size_t>(file)].includedFrom.file)
                chain.push_back(&files_[static_cast<std::size_t>(file)].includedFrom);
            std::string text;
            for (auto from = chain.rbegin(); from != chain.rend(); ++from)
                text += "In file included from " + files_[static_cast<std::size_t>((*from)->file)].name + ":"
                        + std::to_string((*from)->line) + ":\n";
            return text;
        };
        std::string text;
        int previousFile = 0;
        const auto line = [&](const Diagnostic& diagnostic, const char* severity) {
            const int file = diagnostic.location.file;
            if (file != previousFile && file != 0)
                text += includeStack(file);
            previousFile = file;
            text += files_[static_cast<std::size_t>(file)].name + ":" + std::to_string(diagnostic.location.line) + ":"
                    + std::to_string(diagnostic.location.column) + ": " + severity + ": " + diagnostic.message + "\n";
        };
        for (const Diagnostic& diagnostic : sorted)
        {
            line(diagnostic, "error");
            for (const Diagnostic& note : diagnostic.notes)
                line(note, "note");
        }
        return text;
    }
} // namespace fminus
