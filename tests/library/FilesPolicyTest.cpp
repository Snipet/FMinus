// tests/library/FilesPolicyTest.cpp: the file interface's host API as a host of FMinus::FMinus uses it, in C++17
// (docs/research/files/README.md, §6): Options::files reaches no file by default, and files() lists a program's names
// before anything runs. In a build without the file interface (fminus/Config.h's FMINUS_HAS_FILES 0) the same host
// code compiles, the policy is not read, and a program that names no file lists none. tests/unit/FileBuiltinsTest.cpp
// reads and writes files through the JIT and an executable; nothing here opens one.
#include "fminus/FMinus.h"

#include <catch2/catch_test_macros.hpp>

#include <memory>
#include <string>

TEST_CASE("Options::files reaches no file by default, and files lists every name", "[library][files]")
{
    const fminus::Program::Options defaults;
    CHECK(defaults.files.folders.empty());
    CHECK(defaults.files.deny.empty());
    CHECK(defaults.files.types.empty());
    CHECK_FALSE(defaults.files.anywhere);
    CHECK_FALSE(defaults.files.singleLink);
    CHECK(defaults.files.maxFileBytes == ((256 << 20) < FMINUS_FILES_MAX_BYTES ? (256 << 20) : FMINUS_FILES_MAX_BYTES));
    CHECK(defaults.files.maxOpen == 64);
    CHECK(defaults.files.maxHeldBytes == (std::int64_t{1} << 30));
    CHECK(defaults.files.maxWrittenBytes == (std::int64_t{1} << 30));
    CHECK(defaults.files.maxFilesWritten == 1024);
    CHECK(defaults.files.maxListed == 4096);
    const fminus::Program::FilePolicy::Folder folder;
    CHECK_FALSE(folder.write);
    CHECK(folder.types.empty());

    std::string errors;
    auto quiet = fminus::Program::compile("float f(float x) { return x; }\n", errors);
    REQUIRE(quiet);
    CHECK(quiet->files().empty());

#if FMINUS_HAS_FILES
    // A type this build may touch: json, or in a build with FMINUS_FILES_TYPES the first of those.
    std::string type = FMINUS_FILES_TYPES;
    type = type.empty() ? "json" : type.substr(0, type.find(';'));
    auto program = fminus::Program::compile("int load(int k)\n"
                                            "{\n"
                                            "    void* f = file_read(\"presets/%d." + type + "\", k);\n"
                                            "    const int status = file_status(f);\n"
                                            "    file_close(f);\n"
                                            "    return status;\n"
                                            "}\n",
                                            errors);
    INFO(errors);
    REQUIRE(program);
    REQUIRE(program->files().size() == 1);
    const fminus::Program::FileUse& use = program->files()[0];
    CHECK(use.operation == "read");
    CHECK(use.pattern == "presets/*." + type);
    CHECK(use.file == "program.fm");
    CHECK(use.line == 3);
    CHECK(use.column == 15);
    auto* load = program->function<int(int)>("load", errors);
    REQUIRE(load != nullptr);
    CHECK(load(7) == -2);   // FILE_REFUSED: no files by default, and nothing opened
#endif
}
