// tests/library/IncludeTest.cpp: Program::Options::include, a host's files for #include: read where the directive is,
// each once, and named in the errors. C++17, as a host builds.
#include "fminus/FMinus.h"

#include <catch2/catch_test_macros.hpp>

#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace
{
    // A host's files, in memory, by name; each look-up goes into `asked`, as "name from file".
    struct Files
    {
        std::map<std::string, std::string, std::less<>> text;
        std::vector<std::string> asked;

        fminus::Program::IncludeResolver resolver()
        {
            return [this](std::string_view name,
                          std::string_view from) -> std::optional<fminus::Program::IncludedFile> {
                asked.push_back(std::string(name) + " from " + std::string(from));
                const auto found = text.find(name);
                if (found == text.end())
                    return std::nullopt;
                return fminus::Program::IncludedFile{found->first, found->second};
            };
        }
    };
} // namespace

TEST_CASE("Options::include reads a host's files for #include, each once", "[library][include]")
{
    Files files;
    files.text["pole.fm"] = "module Pole\n"
                            "{\n"
                            "    float y;\n"
                            "    float tick(float x, float k) { return y += k * (x - y); }\n"
                            "};\n";
    files.text["gain.fm"] = "#include \"pole.fm\"\n"
                            "float gain(float x) { return 2.0f * x; }\n";
    fminus::Program::Options options;
    options.fileName = "main.fm";
    options.include = files.resolver();
    std::string errors;
    auto program = fminus::Program::compile("#include \"pole.fm\"\n"
                                            "#include \"gain.fm\"\n"
                                            "float smooth(float x)\n"
                                            "{\n"
                                            "    Pole p;\n"
                                            "    p.tick(x, 0.5f);\n"
                                            "    return gain(p.tick(x, 0.5f));\n"
                                            "}\n",
                                            errors, options);
    INFO(errors);
    REQUIRE(program);
    auto* smooth = program->function<float(float)>("smooth", errors);
    REQUIRE(smooth != nullptr);
    CHECK(smooth(4.0f) == 6.0f);   // 2, then 3, doubled
    CHECK(program->module("Pole") != nullptr);
    CHECK(files.asked
          == std::vector<std::string>{"pole.fm from main.fm", "gain.fm from main.fm", "pole.fm from gain.fm"});
}

TEST_CASE("Options::include: an error in an included file names it, and a file not found is an error",
          "[library][include]")
{
    Files files;
    files.text["lib.fm"] = "float twice(float x)\n{\n    return x * z;\n}\n";
    fminus::Program::Options options;
    options.fileName = "main.fm";
    options.include = files.resolver();
    std::string errors;
    CHECK(fminus::Program::compile("float one() { return 1.0f; }\n#include \"lib.fm\"\n", errors, options) == nullptr);
    CHECK(errors == "In file included from main.fm:2:\nlib.fm:3:16: error: use of undeclared identifier 'z'\n");
    CHECK(fminus::Program::compile("#include \"nope.fm\"\n", errors, options) == nullptr);
    CHECK(errors == "main.fm:1:10: error: 'nope.fm' file not found\n");
    // Without a resolver, the default, no #include finds its file.
    CHECK(fminus::Program::compile("#include \"lib.fm\"\n", errors) == nullptr);
    CHECK(errors == "program.fm:1:10: error: 'lib.fm' file not found\n");
}
