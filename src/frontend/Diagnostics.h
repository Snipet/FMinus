// src/frontend/Diagnostics.h: source locations, and the errors the front end and the code generator report.
#pragma once

#include <string>
#include <vector>

namespace fminus
{
    // A position in a source file: line and column, both from 1 (the column counts bytes).
    struct SourceLocation
    {
        int line = 1;
        int column = 1;
    };

    // An error, and the notes that explain it (each a location and a message of its own, as clang's notes are).
    struct Diagnostic
    {
        SourceLocation location;
        std::string message;
        std::vector<Diagnostic> notes;
    };

    // The errors of one compilation of one source file. Errors only for now: add a severity when warnings arrive.
    class Diagnostics
    {
    public:
        explicit Diagnostics(std::string fileName);

        void error(SourceLocation location, std::string message, std::vector<Diagnostic> notes = {});
        bool hasErrors() const
        {
            return !errors_.empty();
        }
        const std::vector<Diagnostic>& errors() const
        {
            return errors_;
        }
        const std::string& fileName() const
        {
            return fileName_;
        }

        // Every error as "<file>:<line>:<column>: error: <message>\n", in source order (errors() keeps the order
        // reported: the code generator's declaration pass reports before it lowers the bodies), each followed by its
        // notes, "<file>:<line>:<column>: note: <message>\n", in their own order wherever they point.
        std::string format() const;

    private:
        std::string fileName_;
        std::vector<Diagnostic> errors_;
    };
} // namespace fminus
