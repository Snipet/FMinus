// src/frontend/Diagnostics.h: source locations, and the errors the front end and the code generator report.
#pragma once

#include <algorithm>
#include <cstddef>
#include <string>
#include <vector>

namespace fminus
{
    // A position in a source file: line and column, both from 1 (the column counts bytes), and the file, an index into
    // the compilation's files (Diagnostics::file): 0 is the program's own, and each file an #include reads has the
    // next.
    struct SourceLocation
    {
        int line = 1;
        int column = 1;
        int file = 0;
    };

    // An error or a warning, and the notes that explain it (each a location and a message of its own, as clang's notes
    // are).
    struct Diagnostic
    {
        SourceLocation location;
        std::string message;
        std::vector<Diagnostic> notes;
    };

    // The errors and warnings of one compilation: of the program's own file, and of the files its #includes read. An
    // error stops the program from compiling; a warning, which names its flag as clang's do ("... [-Wswitch]"), does
    // not. The two are kept apart, so that errors().size() counts errors only, which the code generator's rule for an
    // operand's own error reads (Lowering::Value::failed).
    class Diagnostics
    {
    public:
        // A file of the compilation: its name, as the messages give it, and the #include that read it (in the file
        // includedFrom.file; the program's own file has none, and includedFrom is then unused).
        struct File
        {
            std::string name;
            SourceLocation includedFrom;
        };

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
        // A warning: its message ends with its flag, as clang prints it ("... not handled in switch [-Wswitch]").
        void warning(SourceLocation location, std::string message, std::vector<Diagnostic> notes = {});
        const std::vector<Diagnostic>& warnings() const
        {
            return warnings_;
        }
        // The program's own file's name.
        const std::string& fileName() const
        {
            return files_.front().name;
        }

        // How many errors and warnings there are so far: where rewind() goes back to.
        struct Mark
        {
            std::size_t errors = 0;
            std::size_t warnings = 0;
        };
        Mark mark() const
        {
            return {errors_.size(), warnings_.size()};
        }
        // Drops every error and warning reported since `mark`. For the code generator, which lowers a deferred
        // statement again at each of its block's exits (ir/Statements.cpp): its errors were reported the first time,
        // and each is reported once.
        void rewind(Mark mark)
        {
            errors_.erase(errors_.begin() + static_cast<std::ptrdiff_t>(std::min(errors_.size(), mark.errors)),
                          errors_.end());
            warnings_.erase(warnings_.begin() + static_cast<std::ptrdiff_t>(std::min(warnings_.size(), mark.warnings)),
                            warnings_.end());
        }

        // A file an #include at `includedFrom` read, named `name`: its index, for its locations' `file`.
        int addFile(std::string name, SourceLocation includedFrom);
        const File& file(int index) const
        {
            return files_.at(static_cast<std::size_t>(index));
        }
        int fileCount() const
        {
            return static_cast<int>(files_.size());
        }

        // Every error as "<file>:<line>:<column>: error: <message>\n" and every warning as "...: warning: <message>\n",
        // together in source order, as clang prints them (errors() and warnings() keep the order reported: the code
        // generator's declaration pass reports before it lowers the bodies), each followed by its notes,
        // "<file>:<line>:<column>: note: <message>\n", in their own order wherever they point. Source order is
        // the order the compiler reads the text in: an included file's lines come where its #include is. A line in an
        // included file follows clang's "In file included from <file>:<line>:\n" lines, from the program's own file
        // in, when the line before it was in another file.
        std::string format() const;

    private:
        std::vector<File> files_;
        std::vector<Diagnostic> errors_;
        std::vector<Diagnostic> warnings_;
    };
} // namespace fminus
