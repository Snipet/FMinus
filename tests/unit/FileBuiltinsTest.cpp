// tests/unit/FileBuiltinsTest.cpp: the file interface's builtins in the compiler and the host (src/ir/Calls.cpp,
// src/ir/Files.cpp, Program::Options::files; docs/research/files/README.md): the effects of each (opening, reading,
// listing, writing and closing off the audio thread, a handle's reads wait-free); the status codes' names, their
// values the runtime's; a name's errors, each at its byte, and the build's ceilings (a type outside FMINUS_FILES_TYPES,
// an absolute name without FMINUS_FILES_ANYWHERE) through scanFileName, which takes them as arguments; Program::files()
// for several call sites, operations and files; Options::files checked when the program compiles; no function or
// constant of a program can take a name of the file interface's, and the JIT has none of an executable's. Then programs
// that list a folder of raw float tables, load them and play one through the JIT, and write one back in a build with
// writing; the same as an executable with its environment; the reads a [[clang::nonblocking]] function makes, proved to
// allocate nothing and make no system call through the JIT's code; and a Program that frees what its program holds.
// tests/unit/FilesTest.cpp tests the runtime itself, its traps built for real. In a build without FMINUS_FILES, every
// file_ builtin and every status code's name is the error that names the option.
//
// Everything a test reads or writes is in a directory it makes under the build tree (beside bin/) and removes.
#include "Compile.h"
#include "fminus/Config.h"
#include "fminus/FMinus.h"
#include "ir/Lowering.h"

#include <catch2/catch_test_macros.hpp>

#include <cstdint>
#include <initializer_list>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#if FMINUS_HAS_FILES
#include "HeapCalls.h"
#include "frontend/Frontend.h"
#include "runtime/Files.h"
#include "support/Aot.h"
#include "support/HostTarget.h"
#include "support/JitRuntime.h"
#include "support/LLVMInit.h"

#include <llvm/ExecutionEngine/Orc/LLJIT.h>
#include <llvm/IR/LLVMContext.h>
#include <llvm/Target/TargetMachine.h>

#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <memory>
#include <utility>
#endif

#if FMINUS_HAS_FILES && !defined(_WIN32)
#include <sys/wait.h>
#include <unistd.h>
#elif FMINUS_HAS_FILES
#include <process.h>
#endif

#if FMINUS_HAS_FILES && defined(__GLIBC__)
#include <malloc.h>
#endif

#if FMINUS_HAS_FILES && defined(__linux__)
#include <linux/seccomp.h>
#include <sys/prctl.h>
#include <sys/syscall.h>
#endif

using namespace fminus::test;

namespace
{
    // Every file_ builtin, a status code's name, and a call of each a program could make, with the option it needs.
    struct Call
    {
        std::string_view name;
        std::string_view source;
        bool writes;
    };
    constexpr Call kCalls[] = {
        {"file_read", "void* a(int k) { return file_read(\"presets/%d.json\", k); }\n", false},
        {"file_status", "int b(void* f) { return file_status(f); }\n", false},
        {"file_size", "int c(void* f) { return file_size(f); }\n", false},
        {"file_data", "const void* d(void* f) { return file_data(f); }\n", false},
        {"file_list", "void* e() { return file_list(\"kit/*.f32\"); }\n", false},
        {"file_count", "int g(void* l) { return file_count(l); }\n", false},
        {"file_read_nth", "void* h(void* l) { return file_read_nth(l, 2); }\n", false},
        {"file_close", "void i(void* f) { file_close(f); }\n", false},
        {"file_write", "int j(const void* d, int k) { return file_write(\"user/take%d.f32\", d, 4, k); }\n", true},
        {"file_append", "int k(const void* d) { return file_append(\"user/log.txt\", d, 4); }\n", true},
        {"FILE_REFUSED", "int l() { return FILE_REFUSED; }\n", false},
        {"FILE_LIMIT", "int m(int s) { switch (s) { case FILE_LIMIT: return 1; default: return 0; } }\n", false},
    };

    std::string notBuiltError(std::string_view name, bool writes)
    {
        if (FMINUS_HAS_FILES == 0 || !writes)
            return "'" + std::string(name) + "' needs the file interface, which this build of FMinus leaves out "
                   "(FMINUS_FILES=OFF)";
        return "'" + std::string(name) + "' needs file writing, which this build of FMinus leaves out "
               "(FMINUS_FILES_WRITE=OFF)";
    }

    // Skips the case in a build whose FMINUS_FILES_TYPES leaves out one of the types its programs name (lower case):
    // the ceiling's own errors are scanFileName's case below, FilesTest.cpp's policy case and lit's files-types.fm.
    void fixtures(std::initializer_list<std::string_view> types)
    {
        const std::string_view all = ";" FMINUS_FILES_TYPES ";";
        if (FMINUS_HAS_FILES == 0 || all == ";;")
            return;
        for (const std::string_view type : types)
            if (all.find(";" + std::string(type) + ";") == std::string_view::npos)
                SKIP("this build's FMINUS_FILES_TYPES (" FMINUS_FILES_TYPES ") leaves out a type the case names");
    }

    // scanFileName's first error, as "offset: message", or "" for a good name.
    std::string scanned(std::string_view text, const fminus::ir::detail::FileNameRules& rules)
    {
        const std::vector<fminus::ir::detail::FormatScan::Error> errors = fminus::ir::detail::scanFileName(text, rules);
        return errors.empty() ? std::string() : std::to_string(errors.front().offset) + ": " + errors.front().message;
    }
} // namespace

TEST_CASE("the effects: opening, reading, listing, writing and closing allocate; a handle's reads are wait-free",
          "[codegen][files][effects]")
{
    fixtures({"f32"});
    using namespace fminus::ir::detail;
    for (std::string_view name : {"file_read", "file_list", "file_read_nth", "file_close", "file_write", "file_append"})
    {
        INFO(name);
        CHECK(allocates(name));
        CHECK(!blocks(name));
    }
    for (std::string_view name : {"file_status", "file_size", "file_data", "file_count"})
    {
        INFO(name);
        CHECK(!allocates(name));
        CHECK(!blocks(name));
    }
    if (FMINUS_HAS_FILES == 0)
        return;   // a call of one is an error of its own (below)
    // The reads, in a nonblocking function and a nonallocating one, and in what they call.
    std::string errors;
    auto jit = compile("int reads(void* f, void* l) [[clang::nonblocking]]\n"
                       "{\n"
                       "    const void* d = file_data(f);\n"
                       "    return file_status(f) + file_size(f) + file_count(l) + (d ? 1 : 0);\n"
                       "}\n"
                       "int quiet(void* f, void* l) [[clang::nonallocating]] { return reads(f, l) + file_size(l); }\n",
                       &errors);
    INFO(errors);
    CHECK(jit);
    // Every one that touches the disk, refused in both, and through a function that calls one.
    CHECK(errorsOf("void* open() { return file_read(\"kit/a.f32\"); }\n"
                   "int audio(void* f, void* l) [[clang::nonblocking]]\n"
                   "{\n"
                   "    void* a = open();\n"
                   "    void* b = file_list(\"kit/*.f32\");\n"
                   "    void* c = file_read_nth(l, 0);\n"
                   "    file_close(f);\n"
                   "    return file_status(a) + file_count(b) + file_size(c);\n"
                   "}\n"
                   "int quiet(void* f, void* l) [[clang::nonallocating]]\n"
                   "{\n"
                   "    void* a = file_read(\"kit/a.f32\");\n"
                   "    void* b = file_list(\"kit/*.f32\");\n"
                   "    void* c = file_read_nth(l, 0);\n"
                   "    file_close(f);\n"
                   "    return file_status(a) + file_count(b) + file_size(c);\n"
                   "}\n")
          == "test.fm:4:15: error: function with 'nonblocking' attribute must not call non-'nonblocking' function "
             "'open'\n"
             "test.fm:1:23: note: function cannot be inferred 'nonblocking' because it calls non-'nonblocking' "
             "function 'file_read'\n"
             "test.fm:5:15: error: function with 'nonblocking' attribute must not call non-'nonblocking' function "
             "'file_list'\n"
             "test.fm:6:15: error: function with 'nonblocking' attribute must not call non-'nonblocking' function "
             "'file_read_nth'\n"
             "test.fm:7:5: error: function with 'nonblocking' attribute must not call non-'nonblocking' function "
             "'file_close'\n"
             "test.fm:12:15: error: function with 'nonallocating' attribute must not call non-'nonallocating' "
             "function 'file_read'\n"
             "test.fm:13:15: error: function with 'nonallocating' attribute must not call non-'nonallocating' "
             "function 'file_list'\n"
             "test.fm:14:15: error: function with 'nonallocating' attribute must not call non-'nonallocating' "
             "function 'file_read_nth'\n"
             "test.fm:15:5: error: function with 'nonallocating' attribute must not call non-'nonallocating' "
             "function 'file_close'\n");
    if (FMINUS_HAS_FILES_WRITE == 0)
        return;
    CHECK(errorsOf("int audio(const void* d) [[clang::nonblocking]] { return file_write(\"user/a.f32\", d, 4); }\n"
                   "int quiet(const void* d) [[clang::nonallocating]] { return file_append(\"user/a.f32\", d, 4); }\n")
          == "test.fm:1:58: error: function with 'nonblocking' attribute must not call non-'nonblocking' function "
             "'file_write'\n"
             "test.fm:2:60: error: function with 'nonallocating' attribute must not call non-'nonallocating' "
             "function 'file_append'\n");
}

TEST_CASE("a file_ builtin and a status code's name are known in every build, and an error that names the option in "
          "one without it",
          "[codegen][files]")
{
    fixtures({"json", "f32", "txt"});
    for (const Call& call : kCalls)
    {
        INFO(call.name);
        std::string errors;
        const auto jit = compile(call.source, &errors);
        if (FMINUS_HAS_FILES != 0 && (!call.writes || FMINUS_HAS_FILES_WRITE != 0))
        {
            CHECK(jit);
            CHECK(errors.empty());
        }
        else
        {
            CHECK(!jit);
            CHECK(errors.find("error: " + notBuiltError(call.name, call.writes) + "\n") != std::string::npos);
        }
    }
    using namespace fminus::ir::detail;
    CHECK(isBuilt(Feature::Files) == (FMINUS_HAS_FILES != 0));
    CHECK(isBuilt(Feature::FilesWrite) == (FMINUS_HAS_FILES_WRITE != 0));
    CHECK(notBuilt("file_read", Feature::Files) == notBuiltError("file_read", false));
    CHECK(notBuilt("file_write", Feature::FilesWrite) == notBuiltError("file_write", true));
    // A program's own declaration of such a name is its own, in every build.
    std::string errors;
    auto jit = compile("enum { FILE_LIMIT = 40 };\n"
                       "int own() { int FILE_REFUSED = 7; return FILE_REFUSED + FILE_LIMIT; }\n",
                       &errors);
    INFO(errors);
    REQUIRE(jit);
    CHECK(lookup<int()>(*jit, "own")() == 47);
}

TEST_CASE("a program cannot declare a function or a constant of the file interface's, in any build", "[codegen][files]")
{
    // A prototype of one would reach the runtime past the name's literal, its checks and files():
    // fminus_rt_files_main_start, an executable's, would even make a context from the process's environment
    // (FMINUS_FILES_READ) that its host never gave. Every fminus_rt_file_ and fminus_rt_files_ name is refused, a
    // prototype's or a definition's, and a constant's, whether the program names a file or not.
    CHECK(errorsOf("int fminus_rt_files_main_start(void* slot, void* name);\n")
          == "test.fm:1:5: error: 'fminus_rt_files_main_start' is a name of the file interface, which the code FMinus "
             "generates calls: a program cannot declare it\n");
    CHECK(errorsOf("void* fminus_rt_file_read(void* context, void* name, int length, void* ints, int count);\n"
                   "int f(void* u) { return fminus_rt_file_read(u, u, 4, u, 0) != 0; }\n")
          == "test.fm:1:7: error: 'fminus_rt_file_read' is a name of the file interface, which the code FMinus "
             "generates calls: a program cannot declare it\n"
             "test.fm:2:25: error: call to undeclared function 'fminus_rt_file_read'\n");
    CHECK(errorsOf("int fminus_rt_file_status(void* f) { return 0; }\n")
          == "test.fm:1:5: error: 'fminus_rt_file_status' is a name of the file interface, which the code FMinus "
             "generates calls: a program cannot declare it\n");
    CHECK(errorsOf("float fminus_rt_files_context(float x) { return x; }\n")
          == "test.fm:1:7: error: 'fminus_rt_files_context' is a name of the file interface, which the code FMinus "
             "generates calls: a program cannot declare it\n");
    CHECK(errorsOf("const int fminus_rt_files_context = 3;\n")
          == "test.fm:1:11: error: 'fminus_rt_files_context' is a name of the file interface, which the code FMinus "
             "generates calls: a program cannot declare it\n");
    CHECK(errorsOf("const float fminus_rt_files_main_stop = 1.0f;\n")
          == "test.fm:1:13: error: 'fminus_rt_files_main_stop' is a name of the file interface, which the code "
             "FMinus generates calls: a program cannot declare it\n");
    // A module's method of such a name is the module's own, and so is a name that only begins alike.
    std::string errors;
    CHECK(compile("module M { int fminus_rt_files_main_start(int a) { return a; } };\n"
                  "int fminus_rt_filed(int a) { return a; }\n",
                  &errors));
    CHECK(errors.empty());
}

TEST_CASE("scanFileName: the rules of a name, and the build's ceilings it is given", "[codegen][files]")
{
    // Every build compiles the check; a build's own ceilings are FMINUS_FILES_TYPES and FMINUS_FILES_ANYWHERE, which
    // these rules stand in for, so that one build tests them all.
    using fminus::ir::detail::FileNameRules;
    FileNameRules read;
    CHECK(scanned("presets/7.json", read).empty());
    CHECK(scanned("bank%d/pad%d.f32", read).empty());
    CHECK(scanned("kit/sub/deeper/a.b.wav", read).empty());
    CHECK(scanned("kit/Privé é.wav", read).empty());
    CHECK(scanned("kit/100%%.wav", read).empty());
    CHECK(scanned("kit_2-b/x.WAV", read).empty());
    CHECK(scanned("kit/COM%d0.wav", read).empty());   // COM10 and up are no device
    CHECK(scanned("kit/a.abcdefghijklmnop", read).empty());   // 16 letters

    // Types: the build's, any letter case, the type error naming them; only the last extension counts.
    FileNameRules typed;
    typed.types = "wav;f32";
    CHECK(scanned("kit/a.WAV", typed).empty());
    CHECK(scanned("kit/a.exe.f32", typed).empty());
    CHECK(scanned("kit/a.wav.exe", typed) == "10: 'exe' is not a type this build of FMinus can touch "
                                             "(FMINUS_FILES_TYPES: wav;f32)");
    CHECK(scanned("presets/%d.json", typed) == "11: 'json' is not a type this build of FMinus can touch "
                                               "(FMINUS_FILES_TYPES: wav;f32)");
    typed.pattern = true;
    CHECK(scanned("kit/*.F32", typed).empty());
    CHECK(scanned("kit/*.aiff", typed) == "6: 'aiff' is not a type this build of FMinus can touch "
                                          "(FMINUS_FILES_TYPES: wav;f32)");

    // Absolute names: an error in a build without them; in one with them, this target's form and only for reading.
    const std::string leftOut = "0: an absolute name needs names outside folders, which this build of FMinus leaves "
                                "out (FMINUS_FILES_ANYWHERE=OFF): a name begins with a folder its host maps, as "
                                "\"presets/7.json\"";
    CHECK(scanned("/home/me/kick.wav", read) == leftOut);
    CHECK(scanned("C:/Samples/kick.wav", read) == leftOut);
    FileNameRules anywhere;
    anywhere.anywhere = true;
    CHECK(scanned("/home/me/kick.wav", anywhere).empty());
    CHECK(scanned("/kick.wav", anywhere).empty());
    CHECK(scanned("/home/%d/kick.wav", anywhere).empty());
    CHECK(scanned("/home/me/.ssh/id.pub", anywhere)
          == "9: a segment of a file's name does not begin with '.': '.' and '..' and hidden files are out of reach");
    CHECK(scanned("/home/../etc/a.txt", anywhere)
          == "6: a segment of a file's name does not begin with '.': '.' and '..' and hidden files are out of reach");
    CHECK(scanned("//server/share/a.wav", anywhere) == "1: a file's name has no empty segment: no '//', and no '/' at "
                                                       "its end");
    CHECK(scanned("/", anywhere) == "0: a file's name has no empty segment: no '//', and no '/' at its end");
    CHECK(scanned("C:/Samples/kick.wav", anywhere) == "0: an absolute name begins with '/', as \"/home/me/kick.wav\"");
    anywhere.pattern = true;
    CHECK(scanned("/home/me/kit/*.wav", anywhere).empty());
    anywhere.pattern = false;
    anywhere.writes = true;
    CHECK(scanned("/home/me/kick.wav", anywhere) == "0: a file is written only in a folder its host grants for "
                                                    "writing, never by an absolute name");
    CHECK(scanned("user/kick.wav", anywhere).empty());
    // On Windows: a drive letter, ':' and '/'; a '/' alone is not one there.
    FileNameRules windows;
    windows.anywhere = true;
    windows.windows = true;
    CHECK(scanned("C:/Samples/kick.wav", windows).empty());
    CHECK(scanned("d:/kick.wav", windows).empty());
    CHECK(scanned("/home/me/kick.wav", windows) == "0: an absolute name on Windows begins with a drive letter, ':' and "
                                                   "'/', as \"C:/Samples/kick.wav\"");
    CHECK(scanned("C:\\Samples\\kick.wav", windows) == "0: a file's name is a folder its host maps, '/' and a path "
                                                       "inside it, as \"presets/7.json\"");
    CHECK(scanned("C:/Samples/a:b.wav", windows) == "12: character ':' is not allowed in a file's name: no control "
                                                    "character, and none of \\ : * ? \" < > |");
    CHECK(scanned("C:/AUX/kick.wav", windows) == "3: 'AUX' is a device name of Windows: no segment of a file's name "
                                                 "may be one, in any letter case, with or without an extension");

    // Lengths: a folder's 64 bytes, a segment's 255, 16 segments, 1024 bytes at the least a %d writes (one digit).
    CHECK(scanned(std::string(64, 'f') + "/a.wav", read).empty());
    CHECK(scanned(std::string(65, 'f') + "/a.wav", read) == "0: a folder's name is at most 64 bytes");
    CHECK(scanned("kit/" + std::string(251, 'a') + ".wav", read).empty());
    CHECK(scanned("kit/" + std::string(252, 'a') + ".wav", read) == "4: a segment of a file's name is at most 255 "
                                                                    "bytes");
    std::string sixteen = "kit";
    for (int i = 0; i < 14; ++i)
        sixteen += "/d";
    CHECK(scanned(sixteen + "/a.wav", read).empty());
    CHECK(scanned(sixteen + "/d/a.wav", read) == "33: a file's name has at most 16 segments, the folder's among them");
    std::string long1024 = "kit";
    while (long1024.size() + 1 + 250 < 1024)
        long1024 += "/" + std::string(250, 'd');
    long1024 += "/" + std::string(1024 - long1024.size() - 5, 'f') + ".wav";
    REQUIRE(long1024.size() == 1024);
    CHECK(scanned(long1024, read).empty());
    CHECK(scanned(long1024.substr(0, 1019) + "%d" + long1024.substr(1020), read).empty());   // its %d one digit
    CHECK(scanned(long1024.substr(0, 1019) + "ff" + long1024.substr(1020), read)
          == "0: a file's name is at most 1024 bytes once its %d are filled in");
}

#if FMINUS_HAS_FILES
TEST_CASE("the JIT defines the file interface's functions, never an executable's start and stop", "[codegen][files]")
{
    fminus::initializeLLVM();
    auto jit = llvm::cantFail(llvm::orc::LLJITBuilder().create());
    llvm::cantFail(fminus::defineRuntimeSymbols(*jit));
    std::vector<const char*> defined{"fminus_rt_file_read",  "fminus_rt_file_status", "fminus_rt_file_size",
                                     "fminus_rt_file_data",  "fminus_rt_file_list",   "fminus_rt_file_count",
                                     "fminus_rt_file_read_nth", "fminus_rt_file_close"};
    std::vector<const char*> absent{"fminus_rt_files_main_start", "fminus_rt_files_main_stop"};
    for (const char* name : {"fminus_rt_file_write", "fminus_rt_file_append"})
        (FMINUS_HAS_FILES_WRITE != 0 ? defined : absent).push_back(name);
    for (const char* name : defined)
    {
        INFO(name);
        auto found = jit->lookup(name);
        CHECK(static_cast<bool>(found));
        if (!found)
            llvm::consumeError(found.takeError());
    }
    for (const char* name : absent)
    {
        INFO(name);
        auto found = jit->lookup(name);
        CHECK(!found);
        if (!found)
            llvm::consumeError(found.takeError());
    }
}

TEST_CASE("the status codes' names are the runtime's values", "[codegen][files]")
{
    std::string errors;
    auto jit = compile("int code(int k)\n"
                       "{\n"
                       "    const int codes[7] = {FILE_OK, FILE_MISSING, FILE_REFUSED, FILE_DENIED, FILE_LIMIT,\n"
                       "                          FILE_FAILED, FILE_INVALID};\n"
                       "    return codes[k];\n"
                       "}\n",
                       &errors);
    INFO(errors);
    REQUIRE(jit);
    auto* code = lookup<int(int)>(*jit, "code");
    const std::int32_t expected[] = {FMINUS_FILE_OK,     FMINUS_FILE_MISSING, FMINUS_FILE_REFUSED, FMINUS_FILE_DENIED,
                                     FMINUS_FILE_LIMIT,  FMINUS_FILE_FAILED,  FMINUS_FILE_INVALID};
    for (int k = 0; k < 7; ++k)
        CHECK(code(k) == expected[k]);
}

TEST_CASE("a file's name is a literal checked where it is written, each mistake at its byte", "[codegen][files]")
{
    fixtures({"json", "wav", "txt", "f32"});
    const auto error = [](std::string_view call) {
        return errorsOf("void* f(int k) { return " + std::string(call) + "; }\n");
    };
    // The literal's first byte is column 36 in `void* f(int k) { return file_read("`, the literal's own 35.
    CHECK(error("file_read(k)") == "test.fm:1:35: error: argument to 'file_read' must be a string literal\n");
    CHECK(error("file_read(\"kit\")") == "test.fm:1:36: error: a file's name is a folder its host maps, '/' and a path "
                                         "inside it, as \"presets/7.json\"\n");
    CHECK(error("file_read(\"kit//a.wav\")")
          == "test.fm:1:40: error: a file's name has no empty segment: no '//', and no '/' at its end\n");
    CHECK(error("file_read(\"k.t/a.wav\")") == "test.fm:1:37: error: character '.' is not allowed in a folder's name: "
                                               "ASCII letters, digits, '_' and '-'\n");
    CHECK(error("file_read(\"kit/../a.wav\")") == "test.fm:1:40: error: a segment of a file's name does not begin with "
                                                  "'.': '.' and '..' and hidden files are out of reach\n");
    CHECK(error("file_read(\"kit/.env.wav\")") == "test.fm:1:40: error: a segment of a file's name does not begin with "
                                                  "'.': '.' and '..' and hidden files are out of reach\n");
    CHECK(error("file_read(\"kit/ a.wav\")")
          == "test.fm:1:40: error: a segment of a file's name does not begin with a space\n");
    CHECK(error("file_read(\"kit/a.wav.\")") == "test.fm:1:45: error: a segment of a file's name does not end with '.' "
                                                "or a space, which Windows would take away\n");
    CHECK(error("file_read(\"kit/a \\x2f b.wav\")") == "test.fm:1:41: error: a segment of a file's name does not end "
                                                       "with '.' or a space, which Windows would take away\n");
    for (const char* forbidden : {"\\\\", ":", "*", "?", "\\\"", "<", ">", "|", "\\t", "\\x7f", "\\xc2\\x85"})
    {
        INFO(forbidden);
        const std::string errors = error("file_read(\"kit/a" + std::string(forbidden) + ".wav\")");
        CHECK(errors.starts_with("test.fm:1:41: error: character '"));
        CHECK(errors.ends_with("' is not allowed in a file's name: no control character, and none of \\ : * ? \" < > "
                               "|\n"));
    }
    CHECK(error("file_read(\"kit/a\\xff.wav\")")
          == "test.fm:1:41: error: illegal character encoding in a file's name, byte '\\xff': a name is UTF-8\n");
    for (const char* device : {"NUL.wav", "con.txt", "Com1.f32", "lpt\\xc2\\xb9.wav", "conin$.wav", "CONOUT$.json",
                               "prn", "aux .wav"})
    {
        INFO(device);
        const std::string errors = error("file_read(\"kit/sub/" + std::string(device) + "\")");
        CHECK(errors.starts_with("test.fm:1:44: error: '"));
        CHECK(errors.find("' is a device name of Windows: no segment of a file's name may be one, in any letter "
                          "case, with or without an extension\n")
              != std::string::npos);
    }
    CHECK(error("file_read(\"kit/COM%d.wav\", k)")
          == "test.fm:1:40: error: 'COM%d' is a device name of Windows when its %d is a digit: no segment of a file's "
             "name may be one, in any letter case, with or without an extension\n");
    CHECK(error("file_read(\"kit/readme\")") == "test.fm:1:40: error: a file's name ends with its type, an extension: "
                                               "'.' and 1 to 16 ASCII letters and digits, as \"7.json\"\n");
    CHECK(error("file_read(\"kit/a.%d\", k)")
          == "test.fm:1:42: error: a file's extension is its type, which the literal writes: no %d in it\n");
    CHECK(error("file_read(\"kit/a.w%dv\", k)")
          == "test.fm:1:43: error: a file's extension is its type, which the literal writes: no %d in it\n");
    CHECK(error("file_read(\"kit/%s.wav\", k)") == "test.fm:1:41: error: invalid conversion specifier 's'\n"
                                                   "test.fm:1:41: note: a format here takes %d, an int, and %%, a "
                                                   "percent sign\n");
    CHECK(error("file_read(\"kit/%d.wav\")") == "test.fm:1:41: error: more '%' conversions than data arguments\n");
    CHECK(error("file_read(\"kit/a.wav\", k)") == "test.fm:1:48: error: data argument not used by format string\n");
    CHECK(error("file_read(\"kit/%d.wav\", 1.5f)")
          == "test.fm:1:49: error: format specifies type 'int' but the argument has type 'float'\n");
    // A listing's pattern: one '*', in its file part, before the extension.
    CHECK(error("file_list(\"kit/a.wav\")") == "test.fm:1:40: error: a listing's pattern has one '*' in its file part, "
                                               "as \"kit/*.f32\"\n");
    CHECK(error("file_list(\"kit/*a*.wav\")") == "test.fm:1:42: error: a listing's pattern has one '*'\n");
    CHECK(error("file_list(\"kit/a.*\")") == "test.fm:1:42: error: a listing's '*' comes before its extension, which "
                                             "the pattern writes, as \"kit/*.f32\"\n");
    CHECK(error("file_list(\"kit/*/a.wav\")")
          == "test.fm:1:40: error: a listing's '*' is in its file part, the name's last segment\n");
    CHECK(error("file_list(\"k*t/a*.wav\")") == "test.fm:1:37: error: character '*' is not allowed in a folder's "
                                                "name: ASCII letters, digits, '_' and '-'\n");
    // Absolute names: what the build allows (FMINUS_FILES_ANYWHERE).
    std::string errors;
    if (FMINUS_HAS_FILES_ANYWHERE == 0)
        CHECK(error("file_read(\"/home/me/a.wav\")")
              == "test.fm:1:36: error: an absolute name needs names outside folders, which this build of FMinus leaves "
                 "out (FMINUS_FILES_ANYWHERE=OFF): a name begins with a folder its host maps, as \"presets/7.json\"\n");
    else
        CHECK(compile("void* f() { return file_read(\"/home/me/a.wav\"); }\n", &errors));
    CHECK(compile("void* fine(int k, int j) { return file_read(\"bank%d/pad%d/v%d.f32\", k, j, k + j); }\n"
                  "void* list(int k) { return file_list(\"bank%d/pad*.f32\", k); }\n",
                  &errors));
    CHECK(errors.empty());
}

TEST_CASE("files() lists every name, its operation and where it is, before anything runs", "[library][files]")
{
    fixtures({"f32", "json", "txt"});
    fminus::Program::Options options;
    options.fileName = "loader.fm";
    options.include = [](std::string_view name, std::string_view) -> std::optional<fminus::Program::IncludedFile> {
        if (name != "kit.fm")
            return std::nullopt;
        return fminus::Program::IncludedFile{"kit.fm", "void* kit(int bank)\n"
                                                       "{\n"
                                                       "    return file_list(\"kits/bank%d/*.f32\", bank);\n"
                                                       "}\n"};
    };
    std::string source = "#include \"kit.fm\"\n"
                         "void* preset(int k)\n"
                         "{\n"
                         "    if (k < 0)\n"
                         "        return file_read(\"presets/default.json\");\n"
                         "    return file_read(\"presets/%d-%d.json\", k, k + 1);\n"
                         "}\n"
                         "void* table(void* l, int i) { return file_read_nth(l, i); }\n";
    if (FMINUS_HAS_FILES_WRITE != 0)
        source += "int save(const void* d, int n, int k)\n"
                  "{\n"
                  "    return file_write(\"user/take%d.f32\", d, n, k) + file_append(\"user/log 100%%.txt\", d, n);\n"
                  "}\n";
    std::string errors;
    auto program = fminus::Program::compile(source, errors, options);
    INFO(errors);
    REQUIRE(program);
    const std::vector<fminus::Program::FileUse>& uses = program->files();
    REQUIRE(uses.size() == (FMINUS_HAS_FILES_WRITE != 0 ? 5u : 3u));
    CHECK(uses[0].operation == "list");
    CHECK(uses[0].pattern == "kits/bank*/*.f32");
    CHECK(uses[0].file == "kit.fm");
    CHECK(uses[0].line == 3);
    CHECK(uses[0].column == 12);
    CHECK(uses[1].operation == "read");
    CHECK(uses[1].pattern == "presets/default.json");
    CHECK(uses[1].file == "loader.fm");
    CHECK(uses[1].line == 5);
    CHECK(uses[1].column == 16);
    CHECK(uses[2].pattern == "presets/*-*.json");
    CHECK(uses[2].line == 6);
    CHECK(uses[2].column == 12);
    if (FMINUS_HAS_FILES_WRITE != 0)
    {
        CHECK(uses[3].operation == "write");
        CHECK(uses[3].pattern == "user/take*.f32");
        CHECK(uses[3].line == 11);
        CHECK(uses[4].operation == "append");
        CHECK(uses[4].pattern == "user/log 100%.txt");
        CHECK(uses[4].column == 53);
    }
    // A program that names none lists none, and has no file context.
    auto quiet = fminus::Program::compile("void* nth(void* l) { return file_read_nth(l, 0); }\n", errors);
    REQUIRE(quiet);
    CHECK(quiet->files().empty());
}

TEST_CASE("Options::files is checked when the program compiles; the default reaches no file", "[library][files]")
{
    fixtures({"json"});
    const char* source = "int load(int k)\n"
                         "{\n"
                         "    void* f = file_read(\"presets/%d.json\", k);\n"
                         "    const int status = file_status(f);\n"
                         "    file_close(f);\n"
                         "    return status;\n"
                         "}\n"
                         "int none(void* h) { return file_status(h) + 10 * file_size(h) + 100 * file_count(h)\n"
                         "                          + (file_data(h) ? 1000 : 0); }\n"
                         "int nth(void* l, int i) { void* f = file_read_nth(l, i); const int s = file_status(f);\n"
                         "                          file_close(f); file_close(l); return s; }\n";
    std::string errors;
    fminus::Program::Options options;
    options.files.folders = {{"pre sets", "/nowhere", false, {}}};
    CHECK(fminus::Program::compile(source, errors, options) == nullptr);
    CHECK(errors == "fminus::Program: files: folders: 'pre sets' is not a folder's name: 1 to 64 ASCII letters, "
                    "digits, '_' and '-', not beginning with '-'\n");
    options.files.folders.clear();
    options.files.maxFileBytes = FMINUS_FILES_MAX_BYTES + (FMINUS_FILES_MAX_BYTES < 2147483646 ? 1 : 0);
    if (FMINUS_FILES_MAX_BYTES < 2147483646)
    {
        CHECK(fminus::Program::compile(source, errors, options) == nullptr);
        CHECK(errors.starts_with("fminus::Program: files: maxFileBytes is "));
    }
    options.files.maxFileBytes = fminus::Program::FilePolicy{}.maxFileBytes;
    options.files.anywhere = true;
    if (FMINUS_HAS_FILES_ANYWHERE == 0)
    {
        CHECK(fminus::Program::compile(source, errors, options) == nullptr);
        CHECK(errors == "fminus::Program: files: anywhere is set, and this build of FMinus leaves out absolute names "
                        "(FMINUS_FILES_ANYWHERE=OFF)\n");
    }
    options.files.anywhere = false;
    options.files.folders = {{"presets", "/nowhere/at/all", true, {}}};
    CHECK(fminus::Program::compile(source, errors, options) == nullptr);
    if (FMINUS_HAS_FILES_WRITE == 0)
        CHECK(errors == "fminus::Program: files: folders: 'presets': write is set, and this build of FMinus leaves out "
                        "writing files (FMINUS_FILES_WRITE=OFF)\n");
    else   // a folder whose directory cannot be opened, once the program names a file
        CHECK(errors.starts_with("fminus::Program: files: folders: 'presets': '/nowhere/at/all' cannot be opened: "));
    // The same policy, for a program that names no file: checked, and no folder opened.
    options.files.folders[0].write = false;
    CHECK(fminus::Program::compile("float f(float x) { return x; }\n", errors, options) != nullptr);

    // The default: no files, every name refused before anything is opened.
    auto program = fminus::Program::compile(source, errors);
    INFO(errors);
    REQUIRE(program);
    auto* load = program->function<int(int)>("load", errors);
    REQUIRE(load);
    CHECK(load(1) == FMINUS_FILE_REFUSED);
    // A null handle reads as no file, and closes as nothing.
    auto* none = program->function<int(void*)>("none", errors);
    REQUIRE(none);
    CHECK(none(nullptr) == FMINUS_FILE_INVALID);
    auto* nth = program->function<int(void*, int)>("nth", errors);
    REQUIRE(nth);
    CHECK(nth(nullptr, 0) == FMINUS_FILE_INVALID);
}

namespace
{
    namespace fs = std::filesystem;

    // A directory of the test's own beside bin/ (the build tree), removed when it goes.
    class Scratch
    {
    public:
        Scratch()
        {
            static std::atomic<int> made{0};
            fs::path tree;
#if defined(__linux__)
            std::error_code error;
            const fs::path self = fs::read_symlink("/proc/self/exe", error);
            if (!error)
                tree = self.parent_path().parent_path();
#endif
            if (tree.empty())
                tree = fs::current_path();
#if defined(_WIN32)
            const std::string process = std::to_string(::_getpid());   // <process.h>
#else
            const std::string process = std::to_string(::getpid());
#endif
            path_ = tree / "file-builtins-test" / ("t" + process + "-" + std::to_string(made++));
            fs::remove_all(path_);
            // The parent is shared by the tests ctest runs at once, each a process, and removed by whichever finds it
            // empty: one may find it gone between making it and making its own directory, and makes it again.
            for (int attempt = 0;; ++attempt)
            {
                std::error_code failed;
                fs::create_directories(path_, failed);
                if (!failed)
                    break;
                if (failed != std::errc::no_such_file_or_directory || attempt == 100)
                    fs::create_directories(path_);   // throws, saying why
            }
            path_ = fs::canonical(path_);
        }
        ~Scratch()
        {
            std::error_code ignored;
            fs::remove_all(path_, ignored);
            fs::remove(path_.parent_path(), ignored);   // file-builtins-test/, when this was the last
        }
        Scratch(const Scratch&) = delete;
        Scratch& operator=(const Scratch&) = delete;

        const fs::path& path() const
        {
            return path_;
        }
        fs::path operator/(const std::string& name) const
        {
            return path_ / name;
        }

    private:
        fs::path path_;
    };

    // A raw float table: the floats' bytes, little-endian, as bytes_floats reads them.
    void putFloats(const fs::path& file, const std::vector<float>& values)
    {
        fs::create_directories(file.parent_path());
        std::string bytes;
        for (float value : values)
        {
            std::uint32_t bits = 0;
            std::memcpy(&bits, &value, 4);
            for (int b = 0; b < 4; ++b)
                bytes += static_cast<char>((bits >> (8 * b)) & 0xff);
        }
        std::ofstream out(file, std::ios::binary | std::ios::trunc);
        out.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
    }

    // An instance of a module, zeroed, as its layout says.
    struct Instance
    {
        explicit Instance(const fminus::Program::Module& module)
            : alignment(module.alignment), memory(::operator new(module.size, std::align_val_t(module.alignment)))
        {
            std::memset(memory, 0, module.size);
        }
        ~Instance()
        {
            ::operator delete(memory, std::align_val_t(alignment));
        }
        Instance(const Instance&) = delete;
        Instance& operator=(const Instance&) = delete;

        std::size_t alignment;
        void* memory;
    };

    std::string contents(const fs::path& file)
    {
        std::ifstream in(file, std::ios::binary);
        return std::string(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
    }

    // A kit: a folder of raw float tables, listed, each loaded into the module, one played on the audio thread
    // (between blocks: a nonblocking method), one written back in a build with writing.
    std::string kitSource()
    {
        std::string source = "module Kit\n"
                             "{\n"
                             "    float table[4][8];\n"
                             "    int length[4];\n"
                             "    int count;\n"
                             "    int phase;\n"
                             "    // Every table of the kit, in its names' order: how many, or why none.\n"
                             "    int load()\n"
                             "    {\n"
                             "        void* kit = file_list(\"kit/*.f32\");\n"
                             "        if (file_status(kit) != FILE_OK)\n"
                             "            return file_status(kit);\n"
                             "        count = file_count(kit);\n"
                             "        for (int i = 0; i < count && i < 4; ++i)\n"
                             "        {\n"
                             "            void* f = file_read_nth(kit, i);\n"
                             "            length[i] = file_status(f) == FILE_OK\n"
                             "                            "
                             "? bytes_floats(file_data(f), file_size(f), table[i], 8) : 0;\n"
                             "            file_close(f);\n"
                             "        }\n"
                             "        file_close(kit);\n"
                             "        return count;\n"
                             "    }\n"
                             "    void play(int slot, float* out, int n) [[clang::nonblocking]]\n"
                             "    {\n"
                             "        for (int i = 0; i < n; ++i)\n"
                             "        {\n"
                             "            out[i] = length[slot] > 0 ? table[slot][phase % length[slot]] : 0.0f;\n"
                             "            phase = phase + 1;\n"
                             "        }\n"
                             "    }\n";
        if (FMINUS_HAS_FILES_WRITE != 0)
            source += "    int save(int slot) { return file_write(\"user/table%d.f32\", table[slot], "
                      "4 * length[slot], slot); }\n";
        return source + "};\n";
    }
} // namespace

TEST_CASE("a program lists a folder of raw float tables through the JIT, loads them and plays one", "[library][files]")
{
    fixtures({"f32", "wav"});
    const Scratch scratch;
    putFloats(scratch / "samples/a.f32", {1.0f, 2.0f, 3.0f});
    putFloats(scratch / "samples/B.F32", {-1.0f, 0.5f});
    putFloats(scratch / "samples/c.f32", {4.0f, 5.0f, 6.0f, 7.0f, 8.0f, 9.0f, 10.0f});
    putFloats(scratch / "samples/d.wav", {9.0f});            // another type: not listed
    putFloats(scratch / "samples/.e.f32", {9.0f});           // hidden: not listed
    putFloats(scratch / "samples/private/f.f32", {9.0f});    // a directory: not listed
    fminus::Program::Options options;
    options.files.folders = {{"kit", (scratch / "samples").string(), false, {"f32"}}};
    if (FMINUS_HAS_FILES_WRITE != 0)
    {
        fs::create_directories(scratch / "takes");
        options.files.folders.push_back({"user", (scratch / "takes").string(), true, {}});
    }
    std::string errors;
    auto program = fminus::Program::compile(kitSource(), errors, options);
    INFO(errors);
    REQUIRE(program);
    REQUIRE(program->files().size() == (FMINUS_HAS_FILES_WRITE != 0 ? 2u : 1u));
    CHECK(program->files()[0].pattern == "kit/*.f32");
    auto* load = program->function<int(void*)>("Kit::load", errors);
    auto* play = program->function<void(void*, int, float*, int)>("Kit::play", errors);
    REQUIRE(load);
    REQUIRE(play);
    const fminus::Program::Module* module = program->module("Kit");
    REQUIRE(module != nullptr);
    const Instance instance(*module);
    void* kit = instance.memory;
    CHECK(load(kit) == 3);   // B.F32, a.f32, c.f32: by their names' bytes
    float out[7] = {};
    play(kit, 1, out, 7);    // a.f32, round and round
    CHECK(std::vector<float>(out, out + 7) == std::vector<float>{1.0f, 2.0f, 3.0f, 1.0f, 2.0f, 3.0f, 1.0f});
    play(kit, 0, out, 3);    // B.F32, from where the phase is
    CHECK(std::vector<float>(out, out + 3) == std::vector<float>{0.5f, -1.0f, 0.5f});
    play(kit, 2, out, 2);    // c.f32, from phase 10: its 4th and 5th of 7
    CHECK(out[0] == 7.0f);
    CHECK(out[1] == 8.0f);

    // The same program under the default policy: no folder, nothing listed.
    auto refused = fminus::Program::compile(kitSource(), errors);
    REQUIRE(refused);
    auto* refusedLoad = refused->function<int(void*)>("Kit::load", errors);
    REQUIRE(refusedLoad);
    const Instance other(*refused->module("Kit"));
    CHECK(refusedLoad(other.memory) == FMINUS_FILE_REFUSED);

#if FMINUS_HAS_FILES_WRITE
    auto* save = program->function<int(void*, int)>("Kit::save", errors);
    REQUIRE(save);
    CHECK(save(kit, 1) == FMINUS_FILE_OK);
    std::string expected;
    for (float value : {1.0f, 2.0f, 3.0f})
    {
        std::uint32_t bits = 0;
        std::memcpy(&bits, &value, 4);
        for (int b = 0; b < 4; ++b)
            expected += static_cast<char>((bits >> (8 * b)) & 0xff);
    }
    CHECK(contents(scratch / "takes/table1.f32") == expected);
    // kit is read-only: the same name there is refused, and nothing is written.
    auto* refusedSave = refused->function<int(void*, int)>("Kit::save", errors);
    REQUIRE(refusedSave);
    CHECK(refusedSave(kit, 1) == FMINUS_FILE_REFUSED);
#endif
}

#if !defined(_WIN32)
TEST_CASE("an executable lists and loads its tables with FMINUS_FILES_READ, and nothing without it", "[files][aot]")
{
    fixtures({"f32"});
    const Scratch scratch;
    putFloats(scratch / "samples/a.f32", {1.0f, 2.0f, 3.0f});
    putFloats(scratch / "samples/b.f32", {10.0f, 20.0f});
    fs::create_directories(scratch / "takes");
    fs::create_directories(scratch / "tmp");
    // main(k): the k-th table's sum, plus 100 times how many there are; written back too in a build with writing.
    std::string source = "float main(float k)\n"
                         "{\n"
                         "    void* kit = file_list(\"kit/*.f32\");\n"
                         "    if (file_status(kit) != FILE_OK)\n"
                         "        return (float)file_status(kit);\n"
                         "    void* f = file_read_nth(kit, (int)k);\n"
                         "    float table[8];\n"
                         "    const int n = bytes_floats(file_data(f), file_size(f), table, 8);\n"
                         "    float sum = 100.0f * (float)file_count(kit);\n"
                         "    for (int i = 0; i < n; ++i)\n"
                         "        sum += table[i];\n";
    if (FMINUS_HAS_FILES_WRITE != 0)
        source += "    const int written = file_write(\"user/copy%d.f32\", file_data(f), file_size(f), (int)k);\n"
                  "    if (written != FILE_OK)\n"
                  "        sum = 1000.0f * (float)written;\n";
    source += "    file_close(f);\n"
              "    return sum;\n"
              "}\n";
    fminus::Diagnostics diagnostics("kit.fm");
    const auto parsed = fminus::frontend::parse(source, diagnostics);
    REQUIRE(parsed);
    llvm::LLVMContext llvmContext;
    auto machine = llvm::cantFail(fminus::hostTargetMachine());
    fminus::ir::Options lowering;
    lowering.polyLanes = fminus::polyLanes(*machine);
    lowering.dataLayout = machine->createDataLayout().getStringRepresentation();
    std::unique_ptr<llvm::Module> module = fminus::ir::lower(*parsed, llvmContext, diagnostics, lowering);
    INFO(diagnostics.format());
    REQUIRE(module);
    REQUIRE_FALSE(llvm::errorToBool(fminus::aot::addEntryPoint(*module)));
    fminus::prepareModule(*module, *machine);
    fminus::optimiseModule(*module, *machine, llvm::OptimizationLevel::O2);
    const fs::path exe = scratch / "kit";
    const fs::path out = scratch / "kit.out";
    {
        // The linker's temporary object goes in the test's own directory too.
        const char* before = std::getenv("TMPDIR");
        const std::string saved = before != nullptr ? before : "";
        ::setenv("TMPDIR", (scratch / "tmp").c_str(), 1);
        llvm::Error built = fminus::aot::emitExecutable(*module, *machine, {.output = exe.string()});
        if (before != nullptr)
            ::setenv("TMPDIR", saved.c_str(), 1);
        else
            ::unsetenv("TMPDIR");
        REQUIRE_FALSE(llvm::errorToBool(std::move(built)));
    }
    const auto run = [&](const std::string& environment, const std::string& k) {
        const std::string command = "env -u FMINUS_FILES_READ -u FMINUS_FILES_WRITE -u FMINUS_FILES_DENY "
                                    "-u FMINUS_FILES_TYPES -u FMINUS_FILES_ANYWHERE " + environment + " '"
                                    + exe.string() + "' " + k + " > '" + out.string() + "' 2>&1";
        const int status = std::system(command.c_str());
        return std::pair{WIFEXITED(status) ? WEXITSTATUS(status) : -1, contents(out)};
    };
    const std::string read = "FMINUS_FILES_READ='kit=" + (scratch / "samples").string() + "'";
    const std::string write = FMINUS_HAS_FILES_WRITE != 0
                                  ? " FMINUS_FILES_WRITE='user=" + (scratch / "takes").string() + "'"
                                  : std::string();
    CHECK(run(read + write, "0") == std::pair{0, std::string("206\n")});
    CHECK(run(read + write, "1") == std::pair{0, std::string("230\n")});
    CHECK(run("", "0") == std::pair{0, std::string("-2\n")});   // FILE_REFUSED: no files by default
    // A type the folder does not allow: the listing refused.
    CHECK(run(read + write + " FMINUS_FILES_TYPES=wav", "0") == std::pair{0, std::string("-2\n")});
    CHECK(run(read + write + " FMINUS_FILES_DENY='" + (scratch / "samples").string() + "'", "0")
          == std::pair{0, std::string("-2\n")});
    const auto [status, text] = run("FMINUS_FILES_READ='kit=" + (scratch / "missing").string() + "'", "0");
    CHECK(status == 2);
    CHECK(text.find("FMINUS_FILES_READ: folders: 'kit': '") != std::string::npos);
    if (FMINUS_HAS_FILES_WRITE != 0)
    {
        CHECK(contents(scratch / "takes/copy1.f32").size() == 8);
        CHECK(run(read, "1") == std::pair{0, std::string("-2000\n")});   // user/ not granted: FILE_REFUSED
    }
}
#endif

#if FMINUS_HAS_FILES_ANYWHERE && !defined(_WIN32)
TEST_CASE("an absolute name is read where the policy says anywhere, and refused elsewhere", "[library][files]")
{
    fixtures({"f32"});
    const Scratch scratch;
    putFloats(scratch / "abs/a.f32", {2.5f});
    putFloats(scratch / "abs/b.f32", {0.5f});
    const std::string directory = (scratch / "abs").string();
    std::size_t segments = 0;
    for (char c : directory)
        segments += c == '/' ? 1 : 0;
    if (segments + 1 > 16 || directory.size() + 6 > 1024)
    {
        WARN("the build tree is too deep for an absolute name of 16 segments: " << directory);
        return;
    }
    const std::string source = "float one() { void* f = file_read(\"" + directory + "/a.f32\");\n"
                               "              float v[1]; "
                               "const int n = bytes_floats(file_data(f), file_size(f), v, 1);\n"
                               "              const float s = n == 1 ? v[0] : (float)file_status(f);\n"
                               "              file_close(f); return s; }\n"
                               "int count() { void* l = file_list(\"" + directory + "/*.F32\"); const int n =\n"
                               "              file_status(l) == FILE_OK ? file_count(l) : file_status(l);\n"
                               "              file_close(l); return n; }\n";
    fminus::Program::Options options;
    options.files.anywhere = true;
    std::string errors;
    auto program = fminus::Program::compile(source, errors, options);
    INFO(errors);
    REQUIRE(program);
    REQUIRE(program->files().size() == 2);
    CHECK(program->files()[0].pattern == directory + "/a.f32");
    auto* one = program->function<float()>("one", errors);
    auto* count = program->function<int()>("count", errors);
    REQUIRE(one);
    REQUIRE(count);
    CHECK(one() == 2.5f);
    CHECK(count() == 2);
    // The same names under a deny-list of the directory, and with no anywhere: refused.
    options.files.deny = {directory};
    auto denied = fminus::Program::compile(source, errors, options);
    REQUIRE(denied);
    CHECK(denied->function<float()>("one", errors)() == static_cast<float>(FMINUS_FILE_REFUSED));
    CHECK(denied->function<int()>("count", errors)() == FMINUS_FILE_REFUSED);
    auto folders = fminus::Program::compile(source, errors);
    REQUIRE(folders);
    CHECK(folders->function<float()>("one", errors)() == static_cast<float>(FMINUS_FILE_REFUSED));
}
#endif

#if defined(__linux__)
TEST_CASE("a [[clang::nonblocking]] function, compiled by the JIT, reads a file's and a listing's handles without "
          "allocating or making a system call",
          "[library][files]")
{
    fixtures({"f32"});
    // FilesTest.cpp proves it of the runtime's functions called from C++; this is the same proof of the code a
    // program's builtins become: the JIT'd reads, through the definitions the JIT gives them.
    const Scratch scratch;
    putFloats(scratch / "samples/a.f32", {1.5f, -2.0f, 0.25f});
    fminus::Program::Options options;
    options.files.folders = {{"kit", (scratch / "samples").string(), false, {}}};
    std::string errors;
    auto program = fminus::Program::compile("void* load() { return file_read(\"kit/a.f32\"); }\n"
                                            "void* list() { return file_list(\"kit/*.f32\"); }\n"
                                            "void* missing() { return file_read(\"kit/none.f32\"); }\n"
                                            "float sum(void* f, void* l) [[clang::nonblocking]]\n"
                                            "{\n"
                                            "    float t[4];\n"
                                            "    const int n = bytes_floats(file_data(f), file_size(f), t, 4);\n"
                                            "    float s = (float)(file_status(f) + 10 * file_count(l)\n"
                                            "                      + 100 * file_status(l) + 1000 * n);\n"
                                            "    for (int i = 0; i < n; ++i)\n"
                                            "        s += t[i];\n"
                                            "    return s;\n"
                                            "}\n",
                                            errors, options);
    INFO(errors);
    REQUIRE(program);
    auto* load = program->function<void*()>("load", errors);
    auto* list = program->function<void*()>("list", errors);
    auto* missing = program->function<void*()>("missing", errors);
    auto* sum = program->function<float(void*, void*)>("sum", errors);
    REQUIRE(load);
    REQUIRE(list);
    REQUIRE(missing);
    REQUIRE(sum);
    void* f = load();
    void* l = list();
    void* gone = missing();
    const float expected = 0.0f + 10.0f + 0.0f + 3000.0f - 0.25f;
    REQUIRE(sum(f, l) == expected);
    const float refused = sum(gone, nullptr);   // FILE_MISSING, nothing read; null reads as FILE_INVALID
    CHECK(refused == static_cast<float>(FMINUS_FILE_MISSING + 100 * FMINUS_FILE_INVALID));
    if (HeapCalls::counted())
    {
        long calls = 0;
        bool same = true;
        {
            const HeapCalls counting;
            for (int i = 0; i < 1000; ++i)
                same = same && sum(f, l) == expected && sum(gone, nullptr) == refused;
            calls = counting.count();
        }
        CHECK(same);
        CHECK(calls == 0);
    }
    // In a child under seccomp's strict mode, where any system call but read, write and exit kills it.
    int fds[2];
    REQUIRE(::pipe(fds) == 0);
    const pid_t child = ::fork();
    REQUIRE(child >= 0);
    if (child == 0)
    {
        ::close(fds[0]);
        if (::prctl(PR_SET_SECCOMP, SECCOMP_MODE_STRICT) != 0)
            ::syscall(SYS_exit, 2);
        const float result[2] = {sum(f, l), sum(gone, nullptr)};
        const ssize_t written = ::write(fds[1], result, sizeof result);
        ::syscall(SYS_exit, written == sizeof result ? 0 : 1);
    }
    ::close(fds[1]);
    int result = 0;
    ::waitpid(child, &result, 0);
    float got[2] = {};
    const ssize_t read = ::read(fds[0], got, sizeof got);
    ::close(fds[0]);
    INFO("status " << result);
    if (WIFEXITED(result) && WEXITSTATUS(result) == 2)
        WARN("seccomp's strict mode is not available here: the system calls are not checked");
    else
    {
        CHECK(WIFEXITED(result));
        CHECK(WEXITSTATUS(result) == 0);
        CHECK(read == static_cast<ssize_t>(sizeof got));
        CHECK(got[0] == expected);
        CHECK(got[1] == refused);
    }
}

TEST_CASE("destroying a Program frees what its program holds and closes its folders", "[library][files]")
{
    fixtures({"f32"});
    const Scratch scratch;
    putFloats(scratch / "samples/a.f32", std::vector<float>(1024, 0.5f));
    const auto openDescriptors = [] {
        long n = 0;
        for ([[maybe_unused]] const fs::directory_entry& entry : fs::directory_iterator("/proc/self/fd"))
            ++n;
        return n;
    };
    fminus::Program::Options options;
    options.files.folders = {{"kit", (scratch / "samples").string(), false, {}},
                             {"more", (scratch / "samples").string(), false, {}}};
    options.files.maxOpen = 3;
    const char* source = "int hold() { void* f = file_read(\"kit/a.f32\"); return file_status(f); }\n"
                         "int list() { void* l = file_list(\"more/*.f32\"); return file_status(l); }\n";
    std::string warm;
    CHECK(fminus::Program::compile("float f(float x) { return x; }\n", warm) != nullptr);   // LLVM's own, once
    const long before = openDescriptors();
    {
        std::string errors;
        auto program = fminus::Program::compile(source, errors, options);
        INFO(errors);
        REQUIRE(program);
        CHECK(openDescriptors() >= before + 2);   // each folder's directory, held open
        auto* hold = program->function<int()>("hold", errors);
        auto* list = program->function<int()>("list", errors);
        REQUIRE(hold);
        REQUIRE(list);
        // Never closed: three places of maxOpen's three taken, the fourth refused.
        CHECK(hold() == FMINUS_FILE_OK);
        CHECK(list() == FMINUS_FILE_OK);
        CHECK(hold() == FMINUS_FILE_OK);
        CHECK(hold() == FMINUS_FILE_LIMIT);
        CHECK(openDescriptors() >= before + 2);   // nothing a read opens stays open
    }
    CHECK(openDescriptors() == before);
#if defined(__GLIBC__)
    // The bytes of a file the program never closed are freed with it: 40 MiB, which glibc's malloc maps by itself
    // (past its largest threshold for the heap, 32 MiB), and unmaps when freed.
    if (HeapCalls::counted())   // glibc's own heap, not a sanitizer's
    {
        constexpr std::size_t kBig = std::size_t{40} << 20;
        putFloats(scratch / "big/a.f32", std::vector<float>(kBig / 4, 0.25f));
        options.files.folders = {{"kit", (scratch / "big").string(), false, {}}};
        const std::size_t mapped = mallinfo2().hblkhd;
        {
            std::string errors;
            auto program = fminus::Program::compile(source, errors, options);
            INFO(errors);
            REQUIRE(program);
            auto* hold = program->function<int()>("hold", errors);
            REQUIRE(hold);
            CHECK(hold() == FMINUS_FILE_OK);
            CHECK(mallinfo2().hblkhd >= mapped + kBig);
        }
        CHECK(mallinfo2().hblkhd < mapped + kBig);
    }
#endif
}
#endif
#endif
