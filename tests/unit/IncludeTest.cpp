// tests/unit/IncludeTest.cpp: #include (src/frontend/Lexer.l): its file read where the directive is, each file once,
// the messages naming the file with clang's include stack, and the directives FMinus does not have.
#include "frontend/Frontend.h"

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>

#include <functional>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace
{
    using Files = std::map<std::string, std::string, std::less<>>;

    // The program's dump, or its errors, with `files` what an #include finds, by name from any file. Each look-up goes
    // into `asked`, as "name from file".
    std::string parsed(std::string_view source, const Files& files, std::vector<std::string>* asked = nullptr)
    {
        fminus::Diagnostics diagnostics("test.fm");
        const fminus::frontend::IncludeResolver include =
            [&](std::string_view name, std::string_view from) -> std::optional<fminus::frontend::IncludedFile> {
            if (asked != nullptr)
                asked->push_back(std::string(name) + " from " + std::string(from));
            const auto found = files.find(name);
            if (found == files.end())
                return std::nullopt;
            return fminus::frontend::IncludedFile{found->first, found->second};
        };
        const auto program = fminus::frontend::parse(source, diagnostics, include);
        return program ? fminus::ast::dump(*program) : diagnostics.format();
    }

    std::string parsed(std::string_view source)
    {
        return parsed(source, {});
    }
} // namespace

TEST_CASE("#include reads its file where the directive is", "[frontend][include]")
{
    const Files files{{"twice.fm", "float twice(float x)\n{\n    return x * 2;\n}\n"},
                      {"pole.fm", "module Pole { float y; };\n"},
                      {"body.fm", "return 1;\n"}};
    CHECK(parsed("float one() { return 1; }\n#include \"twice.fm\"\nfloat f(float x) { return twice(x); }\n", files)
          == "(function float one ()\n"
             "  (return 1))\n"
             "(function float twice ((float x))\n"
             "  (return (* x 2)))\n"
             "(function float f ((float x))\n"
             "  (return (call twice x)))\n");
    // A module's name is a type after the #include that declares it, and a directive may be any line, a body's too.
    CHECK(parsed("#include \"pole.fm\"\nfloat f() { Pole p; return p.y; }\n", files)
          == "(module Pole\n"
             "  (field float y))\n"
             "(function float f ()\n"
             "  (decl Pole p)\n"
             "  (return (. p y)))\n");
    CHECK(parsed("float f()\n{\n#include \"body.fm\"\n}\n", files) == "(function float f ()\n  (return 1))\n");
    // Spaces around the #, and comments after the file's name, as C allows.
    CHECK(parsed("  #  include \"body.fm\" /* one */ // two\n", files) == parsed("#include \"body.fm\"\n", files));
    // Lines in the program's own file go on being counted from the #include.
    CHECK(parsed("#include \"twice.fm\"\n\nfloat f(float x) { return x + ; }\n", files)
          == "test.fm:3:31: error: syntax error, unexpected ;\n");
}

TEST_CASE("each file is read once, as if it began with #pragma once", "[frontend][include]")
{
    const Files files{{"test.fm", "float f() { return 1; }\n"},
                      {"common.fm", "module Common { float y; };\n"},
                      {"a.fm", "#include \"common.fm\"\nfloat a(Common* c) { return c->y; }\n"},
                      {"b.fm", "#pragma once\n"
                               "#include \"common.fm\"\n"
                               "#include \"test.fm\"\n"
                               "float b(Common* c) { return 2; }\n"}};
    std::vector<std::string> asked;
    const std::string program = parsed("#include \"a.fm\"\n#include \"b.fm\"\n#include \"a.fm\"\n#include \"test.fm\"\n"
                                       "float f() { return 1; }\n",
                                       files, &asked);
    CHECK(program
          == "(module Common\n"
             "  (field float y))\n"
             "(function float a ((Common * c))\n"
             "  (return (-> c y)))\n"
             "(function float b ((Common * c))\n"
             "  (return 2))\n"
             "(function float f ()\n"
             "  (return 1))\n");
    // The resolver is asked for every #include, the program's own file's too, with the name of the file it is in.
    CHECK(asked
          == std::vector<std::string>{"a.fm from test.fm", "common.fm from a.fm", "b.fm from test.fm",
                                      "common.fm from b.fm", "test.fm from b.fm", "a.fm from test.fm",
                                      "test.fm from test.fm"});
    // #pragma once, and any other pragma, are C's: ignored where FMinus does not know them.
    CHECK(parsed("#pragma once\n#pragma clang fp contract(off)\nfloat f() { return 1; }\n")
          == "(function float f ()\n  (return 1))\n");
}

TEST_CASE("an #include that finds no file is an error at its name", "[frontend][include]")
{
    CHECK(parsed("float f() { return 1; }\n#include \"nope.fm\"\n")
          == "test.fm:2:10: error: 'nope.fm' file not found\n");
    // Without a resolver, none is found.
    fminus::Diagnostics diagnostics("test.fm");
    CHECK_FALSE(fminus::frontend::parse("#include \"twice.fm\"\n", diagnostics));
    CHECK(diagnostics.format() == "test.fm:1:10: error: 'twice.fm' file not found\n");
}

TEST_CASE("an error in an included file names it, after clang's include stack", "[frontend][include]")
{
    const Files files{{"outer.fm", "float outer() { return 1; }\n#include \"inner.fm\"\n"},
                      {"inner.fm", "float inner(float x)\n{\n    return x + ;\n}\n"},
                      {"open.fm", "float g() { return 1; }\n/* never closed\n"},
                      {"missing.fm", "#include \"nope.fm\"\n"}};
    CHECK(parsed("float f() { return 1; }\n#include \"outer.fm\"\n", files)
          == "In file included from test.fm:2:\n"
             "In file included from outer.fm:2:\n"
             "inner.fm:3:16: error: syntax error, unexpected ;\n");
    // A comment does not go on past its file's end.
    CHECK(parsed("#include \"open.fm\"\n*/\n", files)
          == "In file included from test.fm:1:\nopen.fm:2:1: error: unterminated /* comment\n");
    CHECK(parsed("#include \"missing.fm\"\n", files)
          == "In file included from test.fm:1:\nmissing.fm:1:10: error: 'nope.fm' file not found\n");
}

TEST_CASE("errors print in the order the files are read, each included file's after its include stack",
          "[frontend][include]")
{
    fminus::Diagnostics diagnostics("x.fm");
    const int a = diagnostics.addFile("a.fm", {2, 1, 0});
    const int b = diagnostics.addFile("b.fm", {4, 1, a});
    CHECK(diagnostics.file(b).name == "b.fm");
    CHECK(diagnostics.fileCount() == 3);
    diagnostics.error({9, 1, 0}, "last");
    diagnostics.error({1, 5, b}, "in b", {{{1, 2, 0}, "a note in x", {}}, {{3, 3, b}, "a note in b", {}}});
    diagnostics.error({1, 1, 0}, "first");
    diagnostics.error({3, 7, a}, "in a");
    // a.fm's lines come where x.fm's line 2 is, and b.fm's where a.fm's line 4 is. The stack prints again when a line
    // comes back into a file from another.
    CHECK(diagnostics.format()
          == "x.fm:1:1: error: first\n"
             "In file included from x.fm:2:\n"
             "a.fm:3:7: error: in a\n"
             "In file included from x.fm:2:\n"
             "In file included from a.fm:4:\n"
             "b.fm:1:5: error: in b\n"
             "x.fm:1:2: note: a note in x\n"
             "In file included from x.fm:2:\n"
             "In file included from a.fm:4:\n"
             "b.fm:3:3: note: a note in b\n"
             "x.fm:9:1: error: last\n");
}

TEST_CASE("the directives FMinus does not have are errors, in clang's words where clang has some",
          "[frontend][include]")
{
    CHECK(parsed("#include twice.fm\n") == "test.fm:1:10: error: expected \"FILENAME\" or <FILENAME>\n");
    CHECK(parsed("#include \"twice.fm\n") == "test.fm:1:10: error: expected \"FILENAME\" or <FILENAME>\n");
    CHECK(parsed("#include\n") == "test.fm:1:9: error: expected \"FILENAME\" or <FILENAME>\n");
    CHECK(parsed("#include \"\"\n") == "test.fm:1:10: error: empty filename\n");
    CHECK(parsed("#include <twice.fm>\n")
          == "test.fm:1:10: error: #include <...> is not supported: FMinus has no system headers (write #include "
             "\"file\")\n");
    CHECK(parsed("#include \"twice.fm\" twice\n")
          == "test.fm:1:21: error: extra tokens at end of #include directive\n");
    CHECK(parsed("#define GAIN 2\n")
          == "test.fm:1:2: error: #define is not supported: FMinus's directives are #include and #pragma\n");
    CHECK(parsed("  # ifdef X\n")
          == "test.fm:1:5: error: #ifdef is not supported: FMinus's directives are #include and #pragma\n");
    CHECK(parsed("#includes \"twice.fm\"\n") == "test.fm:1:2: error: invalid preprocessing directive\n");
    CHECK(parsed("#!\n") == "test.fm:1:2: error: invalid preprocessing directive\n");
    CHECK(parsed("#\n# // C's null directive\nfloat f() { return 1; }\n") == "(function float f ()\n  (return 1))\n");
    // A # that does not start its line is no directive.
    CHECK(parsed("float f() { return 1; } #include \"twice.fm\"\n")
          == "test.fm:1:25: error: unexpected character '#'\n");
}

TEST_CASE("#include nests at most 200 files deep", "[frontend][include]")
{
    // Each file includes the next, for ever.
    fminus::Diagnostics diagnostics("test.fm");
    int read = 0;
    const fminus::frontend::IncludeResolver endless =
        [&read](std::string_view, std::string_view) -> std::optional<fminus::frontend::IncludedFile> {
        ++read;
        return fminus::frontend::IncludedFile{"f" + std::to_string(read) + ".fm", "#include \"next.fm\"\n"};
    };
    CHECK_FALSE(fminus::frontend::parse("#include \"next.fm\"\n", diagnostics, endless));
    CHECK(read == 200);
    CHECK_THAT(diagnostics.format(), Catch::Matchers::EndsWith("In file included from f199.fm:1:\n"
                                                               "f200.fm:1:10: error: #include nested too deeply\n"));
}
