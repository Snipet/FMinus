// tests/unit/HttpTest.cpp: the web library's builtins in the compiler (src/ir/Calls.cpp, src/ir/Web.cpp; docs/web.md),
// none of which makes a request: the effects of each (a request starts, is cancelled and is closed off the audio
// thread, http_wait blocks, the reads are wait-free); the status codes' names, their values the library's; a URL's
// errors; Program::webRequests() for several call sites, patterns and files; Options::web checked when the program
// compiles; a request refused by the default policy, and a null handle, through the JIT; no function of a program can
// take a name of the library's, and the JIT has none of an executable's. tests/unit/WebTest.cpp
// fetches from a loopback server through the JIT and an executable. In a build without FMINUS_WEB, every http_
// builtin and every status code's name is the error that names the option.
#include "Compile.h"
#include "fminus/Config.h"
#include "fminus/FMinus.h"
#include "ir/Lowering.h"
#if FMINUS_HAS_WEB
#include "support/JitRuntime.h"
#include "support/LLVMInit.h"
#include "web/Web.h"

#include <llvm/ExecutionEngine/Orc/LLJIT.h>
#endif

#include <catch2/catch_test_macros.hpp>

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

using namespace fminus::test;

namespace
{
    // Every http_ builtin, and a call of each a program could make.
    constexpr std::string_view kCalls[][2] = {
        {"http_get", "void* a(int k) { return http_get(\"https://example.com/%d.json\", k); }\n"},
        {"http_post", "void* b(const void* d) { return http_post(\"https://example.com/\", d, 4, \"text/plain\"); }\n"},
        {"http_wait", "int c(void* r) { return http_wait(r, 100); }\n"},
        {"http_status", "int e(void* r) { return http_status(r); }\n"},
        {"http_size", "int f(void* r) { return http_size(r); }\n"},
        {"http_body", "const void* g(void* r) { return http_body(r); }\n"},
        {"http_progress", "int h(void* r) { return http_progress(r); }\n"},
        {"http_cancel", "void i(void* r) { http_cancel(r); }\n"},
        {"http_close", "void j(void* r) { http_close(r); }\n"},
        {"HTTP_REFUSED", "int k() { return HTTP_REFUSED; }\n"},
        {"HTTP_TIMEOUT", "int l(int s) { switch (s) { case HTTP_TIMEOUT: return 1; default: return 0; } }\n"},
    };
} // namespace

TEST_CASE("the effects: a request starts, stops and is freed off the audio thread; http_wait blocks; reads are "
          "wait-free",
          "[codegen][web][effects]")
{
    using namespace fminus::ir::detail;
    for (std::string_view name : {"http_get", "http_post", "http_cancel", "http_close"})
    {
        INFO(name);
        CHECK(allocates(name));
        CHECK(!blocks(name));
    }
    CHECK(blocks("http_wait"));
    CHECK(!allocates("http_wait"));
    for (std::string_view name : {"http_status", "http_size", "http_body", "http_progress"})
    {
        INFO(name);
        CHECK(!allocates(name));
        CHECK(!blocks(name));
    }
    if (FMINUS_HAS_WEB == 0)
        return;   // a call of one is an error of its own (below)
    std::string errors;
    auto jit = compile("module Loader\n"
                       "{\n"
                       "    void* request;\n"
                       "    float gain;\n"
                       "    void load() { request = http_get(\"https://example.com/gain.json\"); }\n"
                       "    int settle() [[clang::nonallocating]] { return http_wait(request, 10); }\n"
                       "    void process(float* out, int n) [[clang::nonblocking]]\n"
                       "    {\n"
                       "        if (http_status(request) == 200 && http_size(request) == 4)\n"
                       "            gain = (float)http_progress(request) + (http_body(request) ? 1.0f : 0.0f);\n"
                       "        for (int i = 0; i < n; ++i)\n"
                       "            out[i] = out[i] * gain;\n"
                       "    }\n"
                       "    void end() { http_cancel(request); http_close(request); }\n"
                       "};\n",
                       &errors);
    INFO(errors);
    CHECK(jit);
    CHECK(errorsOf("void* open() { return http_get(\"https://example.com/\"); }\n"
                   "int process() [[clang::nonblocking]]\n"
                   "{\n"
                   "    void* r = open();\n"
                   "    const int s = http_wait(r, 0);\n"
                   "    http_close(r);\n"
                   "    return s + http_status(r);\n"
                   "}\n"
                   "void stop(void* r) [[clang::nonallocating]] { http_cancel(r); }\n")
          == "test.fm:4:15: error: function with 'nonblocking' attribute must not call non-'nonblocking' function "
             "'open'\n"
             "test.fm:1:23: note: function cannot be inferred 'nonblocking' because it calls non-'nonblocking' "
             "function 'http_get'\n"
             "test.fm:5:19: error: function with 'nonblocking' attribute must not call non-'nonblocking' function "
             "'http_wait'\n"
             "test.fm:6:5: error: function with 'nonblocking' attribute must not call non-'nonblocking' function "
             "'http_close'\n"
             "test.fm:9:47: error: function with 'nonallocating' attribute must not call non-'nonallocating' function "
             "'http_cancel'\n");
}

TEST_CASE("an http_ builtin and a status code's name are known in every build, and an error that names the option "
          "in one without the web library",
          "[codegen][web]")
{
    for (const auto& [name, call] : kCalls)
    {
        INFO(name);
        std::string errors;
        const auto jit = compile(call, &errors);
        if (FMINUS_HAS_WEB != 0)
        {
            CHECK(jit);
            CHECK(errors.empty());
        }
        else
        {
            CHECK(!jit);
            CHECK(errors.find("error: '" + std::string(name) + "' needs the web library, which this build of FMinus "
                              "leaves out (FMINUS_WEB=OFF)\n")
                  != std::string::npos);
        }
    }
    // A program's own declaration of such a name is its own, in either build.
    std::string errors;
    auto jit = compile("enum { HTTP_LIMIT = 40 };\n"
                       "int own() { int HTTP_REFUSED = 7; return HTTP_REFUSED + HTTP_LIMIT; }\n",
                       &errors);
    INFO(errors);
    REQUIRE(jit);
    CHECK(lookup<int()>(*jit, "own")() == 47);
}

TEST_CASE("a program cannot declare a function of the web library's, in either build", "[codegen][web]")
{
    // A prototype of one would call the library past the URL literal and webRequests(): fminus_web_main_start, an
    // executable's, would even make a context from the process's environment (FMINUS_WEB_ALLOW) that its host never
    // gave. Every fminus_web_ name is refused, a prototype's or a definition's, whether the program makes requests or
    // not.
    CHECK(errorsOf("int fminus_web_main_start(void* slot, void* name);\n")
          == "test.fm:1:5: error: 'fminus_web_main_start' is a name of the web library, which the code FMinus "
             "generates calls: a program cannot declare it\n");
    CHECK(errorsOf("void* fminus_web_http_get(void* context, void* url, int length, void* ints, int count);\n"
                   "int f(void* u) { return fminus_web_http_get(u, u, 4, u, 0) != 0; }\n")
          == "test.fm:1:7: error: 'fminus_web_http_get' is a name of the web library, which the code FMinus "
             "generates calls: a program cannot declare it\n"
             "test.fm:2:25: error: call to undeclared function 'fminus_web_http_get'\n");
    CHECK(errorsOf("int fminus_web_http_status(void* r) { return 200; }\n")
          == "test.fm:1:5: error: 'fminus_web_http_status' is a name of the web library, which the code FMinus "
             "generates calls: a program cannot declare it\n");
    CHECK(errorsOf("float fminus_web_context(float x) { return x; }\n"
                   "void* j() { return http_get(\"https://e.com/\"); }\n")
                  .starts_with("test.fm:1:7: error: 'fminus_web_context' is a name of the web library, which the "
                               "code FMinus generates calls: a program cannot declare it\n"));
    // A module's method of such a name is the module's own.
    std::string errors;
    CHECK(compile("module M { int fminus_web_main_start(int a) { return a; } };\n", &errors));
    CHECK(errors.empty());
}

#if FMINUS_HAS_WEB
TEST_CASE("the JIT defines the library's request functions, never an executable's start and stop", "[codegen][web]")
{
    fminus::initializeLLVM();
    auto jit = llvm::cantFail(llvm::orc::LLJITBuilder().create());
    llvm::cantFail(fminus::defineRuntimeSymbols(*jit));
    for (const char* name : {"fminus_web_http_get", "fminus_web_http_post", "fminus_web_http_wait",
                             "fminus_web_http_status", "fminus_web_http_size", "fminus_web_http_body",
                             "fminus_web_http_progress", "fminus_web_http_cancel", "fminus_web_http_close"})
    {
        INFO(name);
        auto found = jit->lookup(name);
        CHECK(static_cast<bool>(found));
        if (!found)
            llvm::consumeError(found.takeError());
    }
    for (const char* name : {"fminus_web_main_start", "fminus_web_main_stop"})
    {
        INFO(name);
        auto found = jit->lookup(name);
        CHECK(!found);
        if (!found)
            llvm::consumeError(found.takeError());
    }
}

TEST_CASE("the status codes' names are the library's values", "[codegen][web]")
{
    std::string errors;
    auto jit = compile("int code(int k)\n"
                       "{\n"
                       "    const int codes[9] = {HTTP_PENDING, HTTP_TRANSPORT, HTTP_TIMEOUT, HTTP_CANCELLED, "
                       "HTTP_REFUSED,\n"
                       "                          HTTP_LIMIT, HTTP_CERTIFICATE, HTTP_INVALID, HTTP_FAILED};\n"
                       "    return codes[k];\n"
                       "}\n",
                       &errors);
    INFO(errors);
    REQUIRE(jit);
    auto* code = lookup<int(int)>(*jit, "code");
    const std::int32_t expected[] = {FMINUS_WEB_PENDING, FMINUS_WEB_TRANSPORT, FMINUS_WEB_TIMEOUT,
                                     FMINUS_WEB_CANCELLED, FMINUS_WEB_REFUSED, FMINUS_WEB_LIMIT,
                                     FMINUS_WEB_CERTIFICATE, FMINUS_WEB_INVALID, FMINUS_WEB_FAILED};
    for (int k = 0; k < 9; ++k)
        CHECK(code(k) == expected[k]);
}

TEST_CASE("a URL is a literal: its format, its bytes, its scheme, no credentials", "[codegen][web]")
{
    CHECK(errorsOf("void* a(int k) { return http_get(k); }\n")
          == "test.fm:1:34: error: argument to 'http_get' must be a string literal\n");
    CHECK(errorsOf("void* b(int k) { return http_get(\"https://e.com/%d/%d\", k); }\n")
          == "test.fm:1:53: error: more '%' conversions than data arguments\n");
    CHECK(errorsOf("void* c(int k) { return http_get(\"https://e.com/%s\", k); }\n")
          == "test.fm:1:50: error: invalid conversion specifier 's'\n"
             "test.fm:1:50: note: a format here takes %d, an int, and %%, a percent sign\n");
    CHECK(errorsOf("void* d() { return http_get(\"https://e.com/a\\nHost: x\"); }\n")
          == "test.fm:1:45: error: character '\\x0a' is not allowed in a URL: it is printable ASCII, without spaces or "
             "backslashes\n");
    CHECK(errorsOf("void* e() { return http_get(\"https://e.com\\\\a\"); }\n")
          == "test.fm:1:43: error: character '\\' is not allowed in a URL: it is printable ASCII, without spaces or "
             "backslashes\n");
    CHECK(errorsOf("void* f() { return http_get(\"file:///etc/passwd\"); }\n")
          == "test.fm:1:30: error: a URL's scheme is http or https, as 'https://example.com/a.json'\n");
    CHECK(errorsOf("void* g() { return http_get(\"https://:@e.com/\"); }\n")
          == "test.fm:1:39: error: a URL cannot carry credentials ('@' before its host): the web library refuses "
             "them\n");
    // The context's global is the library's name: a constant of the name, as a function of it (below).
    CHECK(errorsOf("const int fminus_web_context = 3;\n"
                   "void* j() { return http_get(\"https://e.com/\"); }\n")
          == "test.fm:2:20: error: the web library reserves 'fminus_web_context' for the program's context, and this "
             "program declares something of that name: rename it\n");
    // An '@' after the host is the path's, not credentials.
    std::string errors;
    CHECK(compile("void* h() { return http_get(\"HTTPS://e.com/a@b?c=@\"); }\n", &errors));
    CHECK(errors.empty());
    CHECK(errorsOf("void* i(const void* d) { return http_post(\"https://e.com/\", d, 1, \"a\\tb\"); }\n")
          == "test.fm:1:69: error: character '\\x09' is not allowed in a content type: it is printable ASCII\n");
}

TEST_CASE("webRequests lists every URL literal, its method and where it is, before anything runs", "[library][web]")
{
    fminus::Program::Options options;
    options.fileName = "loader.fm";
    options.include = [](std::string_view name, std::string_view) -> std::optional<fminus::Program::IncludedFile> {
        if (name != "report.fm")
            return std::nullopt;
        return fminus::Program::IncludedFile{
            "report.fm", "void* report(const void* d, int n, int k)\n"
                         "{\n"
                         "    return http_post(\"https://api.example.com/levels/%d?unit=100%%\", d, n, "
                         "\"application/json\", k);\n"
                         "}\n"};
    };
    std::string errors;
    auto program = fminus::Program::compile("#include \"report.fm\"\n"
                                            "void* preset(int bank, int k)\n"
                                            "{\n"
                                            "    if (bank == 0)\n"
                                            "        return http_get(\"https://example.com/presets/%d.json\", k);\n"
                                            "    return http_get(\"https://cdn.example.com/banks/%d/presets/%d.json\", "
                                            "bank, k);\n"
                                            "}\n"
                                            "void* ir() { return http_get(\"https://example.com/rooms/hall.f32\"); }\n",
                                            errors, options);
    INFO(errors);
    REQUIRE(program);
    const std::vector<fminus::Program::WebRequest>& requests = program->webRequests();
    REQUIRE(requests.size() == 4);
    CHECK(requests[0].method == "POST");
    CHECK(requests[0].pattern == "https://api.example.com/levels/*?unit=100%");
    CHECK(requests[0].file == "report.fm");
    CHECK(requests[0].line == 3);
    CHECK(requests[0].column == 12);
    CHECK(requests[1].method == "GET");
    CHECK(requests[1].pattern == "https://example.com/presets/*.json");
    CHECK(requests[1].file == "loader.fm");
    CHECK(requests[1].line == 5);
    CHECK(requests[1].column == 16);
    CHECK(requests[2].pattern == "https://cdn.example.com/banks/*/presets/*.json");
    CHECK(requests[2].line == 6);
    CHECK(requests[3].pattern == "https://example.com/rooms/hall.f32");
    CHECK(requests[3].line == 8);
    // A program that makes none lists none.
    auto quiet = fminus::Program::compile("float f(float x) { return x; }\n", errors);
    REQUIRE(quiet);
    CHECK(quiet->webRequests().empty());
}

TEST_CASE("Options::web is checked when the program compiles; the default reaches nothing", "[library][web]")
{
    const char* source = "int fetch(int k)\n"
                         "{\n"
                         "    void* r = http_get(\"https://example.com/presets/%d.json\", k);\n"
                         "    const int status = http_wait(r, -1);\n"
                         "    http_close(r);\n"
                         "    return status;\n"
                         "}\n"
                         "int none(void* r) { return http_status(r) + 10 * http_size(r) + 100 * http_progress(r)\n"
                         "                        + (http_body(r) ? 1000 : 0) + http_wait(r, 5); }\n"
                         "void quiet(void* r) { http_cancel(r); http_close(r); }\n";
    std::string errors;
    fminus::Program::Options options;
    options.web.allow = {"https://example.com/presets/*", "gopher://example.com/"};
    CHECK(fminus::Program::compile(source, errors, options) == nullptr);
    CHECK(errors.starts_with("fminus::Program: web: pattern 'gopher://example.com/'"));
    options.web.allow.clear();
    options.web.maxBodyBytes = 0;
    CHECK(fminus::Program::compile(source, errors, options) == nullptr);
    CHECK(errors.starts_with("fminus::Program: web: "));

    // The default: no network, every request refused before anything is sent.
    auto program = fminus::Program::compile(source, errors);
    INFO(errors);
    REQUIRE(program);
    auto* fetch = program->function<int(int)>("fetch", errors);
    REQUIRE(fetch);
    CHECK(fetch(1) == FMINUS_WEB_REFUSED);
    // A null handle does nothing, and reads as a failed request.
    auto* none = program->function<int(void*)>("none", errors);
    REQUIRE(none);
    CHECK(none(nullptr) == 2 * FMINUS_WEB_INVALID);
    auto* quiet = program->function<void(void*)>("quiet", errors);
    REQUIRE(quiet);
    quiet(nullptr);
    program->stopWeb();   // nothing to stop: harmless
}
#endif
