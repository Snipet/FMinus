// tests/unit/HttpServerTest.cpp: the web server's builtins in the compiler (src/ir/Calls.cpp, src/ir/WebServer.cpp;
// docs/research/web/server.md, §4), none of which listens: the effects of each (http_listen and http_close allocate,
// http_accept blocks, everything on a request is wait-free); a route literal's errors and a reply's format's, each at
// its byte; a query's name; Program::webRoutes() for several call sites, patterns and files; the route table every
// http_listen hands the library, each literal once; the declarations' attributes, which must stay true of the
// library's code; Options::web.server checked when the program compiles. tests/unit/WebServeTest.cpp serves through
// the JIT and an executable, over loopback. In a build without FMINUS_WEB_SERVER, every server builtin is the error
// that names the option (http_size, http_body and http_close stay the web library's).
#include "Compile.h"
#include "fminus/Config.h"
#include "fminus/FMinus.h"
#include "ir/Lowering.h"
#if FMINUS_HAS_WEB_SERVER
#include "support/JitRuntime.h"
#include "support/LLVMInit.h"
#include "web/Web.h"

#include <llvm/ExecutionEngine/Orc/LLJIT.h>
#include <llvm/IR/Constants.h>
#include <llvm/IR/GlobalVariable.h>
#include <llvm/IR/Instructions.h>
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
    // Every builtin of the server's own, and a call of each a program could make.
    constexpr std::string_view kCalls[][2] = {
        {"http_listen", "void* a() { return http_listen(); }\n"},
        {"http_port", "int b(void* s) { return http_port(s); }\n"},
        {"http_next", "void* c(void* s) { return http_next(s); }\n"},
        {"http_accept", "void* d(void* s) { return http_accept(s, 10); }\n"},
        {"http_is", "bool e(void* r) { return http_is(r, \"GET /a/%d\"); }\n"},
        {"http_arg", "int f(void* r) { return http_arg(r, 0); }\n"},
        {"http_query_float", "float g(void* r) { return http_query_float(r, \"gain\", 0.5f); }\n"},
        {"http_query_int", "int h(void* r) { return http_query_int(r, \"n\", 1); }\n"},
        {"http_reply", "int i(void* r) { return http_reply(r, 204); }\n"},
        {"http_reply_bytes", "int j(void* r, const float* t) { return http_reply_bytes(r, 200, t, 8, \"\"); }\n"},
        {"http_reply_text", "int k(void* r, float g) { return http_reply_text(r, 200, \"\", \"%g\", g); }\n"},
    };

#if FMINUS_HAS_WEB_SERVER
    // The program lowered (not optimised), or null with its errors in `errors`.
    std::unique_ptr<llvm::Module> lowered(std::string_view source, llvm::LLVMContext& context, std::string& errors)
    {
        fminus::Diagnostics diagnostics("test.fm");
        const auto program = fminus::frontend::parse(source, diagnostics);
        std::unique_ptr<llvm::Module> module =
            program ? fminus::ir::lower(*program, context, diagnostics, fminus::ir::Options{}) : nullptr;
        errors = diagnostics.format();
        return module;
    }
#endif
} // namespace

TEST_CASE("the server's effects: http_listen and http_close allocate, http_accept blocks, a request's are wait-free",
          "[codegen][web][server][effects]")
{
    using namespace fminus::ir::detail;
    for (std::string_view name : {"http_listen", "http_close"})
    {
        INFO(name);
        CHECK(allocates(name));
        CHECK(!blocks(name));
    }
    CHECK(blocks("http_accept"));
    CHECK(!allocates("http_accept"));
    for (std::string_view name : {"http_port", "http_next", "http_is", "http_arg", "http_query_float", "http_query_int",
                                  "http_size", "http_body", "http_reply", "http_reply_bytes", "http_reply_text"})
    {
        INFO(name);
        CHECK(!allocates(name));
        CHECK(!blocks(name));
    }
    if (FMINUS_HAS_WEB_SERVER == 0)
        return;   // a call of one is an error of its own (below)
    std::string errors;
    auto jit = compile("module Synth\n"
                       "{\n"
                       "    void* server;\n"
                       "    float gain;\n"
                       "    float table[16];\n"
                       "    void init() { server = http_listen(); }\n"
                       "    void* load(int ms) [[clang::nonallocating]] { return http_accept(server, ms); }\n"
                       "    void process(float* out, int n) [[clang::nonblocking]]\n"
                       "    {\n"
                       "        if (http_port(server) <= 0)\n"
                       "            return;\n"
                       "        for (void* r = http_next(server); r; r = http_next(server))\n"
                       "        {\n"
                       "            if (http_is(r, \"PUT /gain\"))\n"
                       "                http_reply(r, http_query_int(r, \"db\", 0) == 0 ? 204 : 400);\n"
                       "            else if (http_is(r, \"PUT /table/%d\"))\n"
                       "            {\n"
                       "                gain = http_query_float(r, \"value\", gain);\n"
                       "                table[http_arg(r, 0) & 15] = (float)http_size(r) + (http_body(r) ? 1.0f : 0.0f);\n"
                       "                http_reply_bytes(r, 200, table, 64, \"application/octet-stream\");\n"
                       "            }\n"
                       "            else\n"
                       "                http_reply_text(r, 200, \"application/json\", \"{\\\"gain\\\":%g}\", gain);\n"
                       "        }\n"
                       "        for (int i = 0; i < n; ++i)\n"
                       "            out[i] = out[i] * gain;\n"
                       "    }\n"
                       "    void end() { http_close(server); }\n"
                       "};\n",
                       &errors);
    INFO(errors);
    CHECK(jit);
    CHECK(errorsOf("void* start() [[clang::nonallocating]] { return http_listen(); }\n"
                   "void* take(void* s) [[clang::nonblocking]] { return http_accept(s, 0); }\n"
                   "int ready(void* s) [[clang::nonblocking]] { return http_port(s); }\n"
                   "void stop(void* s) [[clang::nonblocking]] { http_close(s); }\n")
          == "test.fm:1:49: error: function with 'nonallocating' attribute must not call non-'nonallocating' function "
             "'http_listen'\n"
             "test.fm:2:53: error: function with 'nonblocking' attribute must not call non-'nonblocking' function "
             "'http_accept'\n"
             "test.fm:4:45: error: function with 'nonblocking' attribute must not call non-'nonblocking' function "
             "'http_close'\n");
}

TEST_CASE("a server builtin is known in every build, and an error that names the option in one without the server",
          "[codegen][web][server]")
{
    for (const auto& [name, call] : kCalls)
    {
        INFO(name);
        std::string errors;
        const auto jit = compile(call, &errors);
        if (FMINUS_HAS_WEB_SERVER != 0)
        {
            CHECK(jit);
            CHECK(errors.empty());
        }
        else
        {
            CHECK(!jit);
            CHECK(errors.find("error: '" + std::string(name) + "' needs the web server, which this build of FMinus "
                              "leaves out (FMINUS_WEB_SERVER=OFF)\n")
                  != std::string::npos);
        }
    }
    // A prototype with the builtin's own types declares nothing new; a definition is refused, in either build.
    CHECK(errorsOf("void* http_listen() { return http_listen(); }\n")
          == "test.fm:1:7: error: 'http_listen' is a builtin and cannot be redefined\n");
    CHECK(errorsOf("int http_port(int s);\n")
          == "test.fm:1:5: error: conflicting types for 'http_port'\n");
    std::string errors;
    const auto declared = compile("void* http_next(void* s);\nint http_arg(void* r, int i);\n", &errors);
    CHECK(declared);
    CHECK(errors.empty());
}

TEST_CASE("a route literal is a method, one space and a path the server serves, each mistake at its byte",
          "[codegen][web][server]")
{
    using fminus::ir::detail::scanRoute;
    // The good ones, in every build (the checker is the compiler's own).
    for (std::string_view route : {"GET /", "PUT /gain", "POST /osc/%d/detune", "PATCH /a-b_c.d~e/%d.json",
                                   "DELETE /%d/%d/%d/%d/%d/%d/%d/%d", "GET /v1.2/x%dy", "GET /...", "GET /.a/a."})
    {
        INFO(route);
        CHECK(scanRoute(route).empty());
    }
    const auto first = [](std::string_view route) {
        const std::vector<fminus::ir::detail::FormatScan::Error> errors = scanRoute(route);
        REQUIRE(errors.size() == 1);
        return std::pair{errors[0].offset, errors[0].message};
    };
    CHECK(first("GET") == std::pair{std::size_t{0}, std::string("a route is a method, a space and a path, as \"PUT "
                                                                "/gain\"")});
    CHECK(first("get /a").first == 0);
    CHECK(first("HEAD /a").second.ends_with("(a GET route answers HEAD too)"));
    CHECK(first("CONNECT /a").second == "'CONNECT' is not a method a route can name: GET, POST, PUT, PATCH or DELETE");
    CHECK(first("GET  /a") == std::pair{std::size_t{4}, std::string("a route has one space between its method and "
                                                                    "its path")});
    CHECK(first("GET a").first == 4);
    CHECK(first("GET ").first == 3);
    CHECK(first("GET /a//b").first == 6);
    CHECK(first("GET /a/") == std::pair{std::size_t{6}, std::string("a route's path has no empty segment")});
    CHECK(first("GET /a/../b") == std::pair{std::size_t{7}, std::string("a route's path has no '.' or '..' segment")});
    CHECK(first("GET /.").first == 5);
    CHECK(first("GET /%d%d").first == 7);
    CHECK(first("GET /%d/%d/%d/%d/%d/%d/%d/%d/%d") == std::pair{std::size_t{29}, std::string("a route takes at most 8 "
                                                                                            "%d")});
    CHECK(first("GET /%d0") == std::pair{std::size_t{7}, std::string("a digit right after %d can never match: %d "
                                                                     "takes all the digits there are")});
    CHECK(first("GET /50%%") == std::pair{std::size_t{7}, std::string("a route's path cannot hold '%': %d, a number, "
                                                                     "is its only conversion")});
    CHECK(first("GET /%s") == std::pair{std::size_t{6}, std::string("invalid conversion specifier 's'")});
    CHECK(first("GET /%5d") == std::pair{std::size_t{6}, std::string("invalid conversion specifier '5d'")});
    CHECK(first("GET /%") == std::pair{std::size_t{5}, std::string("incomplete format specifier")});
    CHECK(first("GET /a?b=1").first == 6);
    CHECK(first("GET /a#b").first == 6);
    CHECK(first("GET /a b") == std::pair{std::size_t{6}, std::string("character ' ' is not allowed in a route's path: "
                                                                    "letters, digits, '/', '-', '.', '_', '~' and "
                                                                    "%d")});
    CHECK(first("GET /\xc3\xa9").second.starts_with("character '\\xc3'"));
    if (FMINUS_HAS_WEB_SERVER == 0)
        return;
    // Through the language: the error at its byte, the literal's own column.
    CHECK(errorsOf("bool a(void* r) { return http_is(r, \"HEAD /x\"); }\n")
          == "test.fm:1:38: error: 'HEAD' is not a method a route can name: GET, POST, PUT, PATCH or DELETE (a GET "
             "route answers HEAD too)\n");
    CHECK(errorsOf("bool b(void* r) { return http_is(r, \"GET /osc/%d%d\"); }\n")
          == "test.fm:1:49: error: a %d right after another can never match: each takes all the digits there are\n");
    CHECK(errorsOf("bool c(void* r) { return http_is(r, \"GET /a/\\\\b\"); }\n")
          == "test.fm:1:45: error: character '\\' is not allowed in a route's path: letters, digits, '/', '-', '.', "
             "'_', '~' and %d\n");
    CHECK(errorsOf("bool d(void* r) { return http_is(r, \"PUT /%f\"); }\n")
          == "test.fm:1:44: error: invalid conversion specifier 'f'\n"
             "test.fm:1:44: note: a route takes %d, a decimal number of the path\n");
    CHECK(errorsOf("bool e(void* r, int k) { return http_is(r, k); }\n")
          == "test.fm:1:44: error: argument to 'http_is' must be a string literal\n");
    CHECK(errorsOf("bool f(void* r) { return http_is(r, \"GET /a/\\x01\"); }\n")
          == "test.fm:1:45: error: character '\\x01' is not allowed in a route's path: letters, digits, '/', '-', "
             "'.', '_', '~' and %d\n");
}

TEST_CASE("a reply's format is printf's %d, %g, %.Ng, %s and %%, checked as clang checks printf's",
          "[codegen][web][server]")
{
    using fminus::ir::detail::scanReplyFormat;
    const auto places = [](std::string_view format) {
        const auto scan = scanReplyFormat(format);
        CHECK(scan.errors.empty());
        std::string kinds;
        for (const auto& place : scan.places)
            kinds += place.text ? 's' : place.real ? 'g' : 'd';
        return kinds;
    };
    CHECK(places("{\"gain\":%g,\"n\":%d,\"x\":%.9g,\"y\":%.17g,\"z\":%.1g} 100%%") == "gdggg");
    CHECK(places("%s=%d %s%%") == "sds");
    CHECK(places("%.05g") == "g");
    CHECK(places("no places") == "");
    const auto error = [](std::string_view format) {
        const auto scan = scanReplyFormat(format);
        REQUIRE(scan.errors.size() == 1);
        return std::pair{scan.errors[0].offset, scan.errors[0].message};
    };
    CHECK(error("%.18g") == std::pair{std::size_t{1}, std::string("a reply's %.Ng takes a precision N of 1 to 17")});
    CHECK(error("%.0g").first == 1);
    CHECK(error("%.g").first == 1);
    CHECK(error("%.017g").first == 1);   // the library reads one or two digits
    CHECK(error("%.*g").first == 1);
    CHECK(error("a %f") == std::pair{std::size_t{3}, std::string("invalid conversion specifier 'f'")});
    CHECK(error("%5g") == std::pair{std::size_t{1}, std::string("invalid conversion specifier '5g'")});
    CHECK(error("%-d") == std::pair{std::size_t{1}, std::string("invalid conversion specifier '-d'")});
    CHECK(error("%lg") == std::pair{std::size_t{1}, std::string("invalid conversion specifier 'lg'")});
    CHECK(error("%.3f") == std::pair{std::size_t{1}, std::string("invalid conversion specifier '.3f'")});
    CHECK(error("%5s") == std::pair{std::size_t{1}, std::string("invalid conversion specifier '5s'")});
    CHECK(error("%ls") == std::pair{std::size_t{1}, std::string("invalid conversion specifier 'ls'")});
    CHECK(error("%y") == std::pair{std::size_t{1}, std::string("invalid conversion specifier 'y'")});
    CHECK(error("50%") == std::pair{std::size_t{2}, std::string("incomplete format specifier")});
    if (FMINUS_HAS_WEB_SERVER == 0)
        return;
    const std::string note = "note: a reply's format takes %d, an int; %g and %.Ng (N from 1 to 17), a float or a "
                             "double; %s, a string; and %%, a percent sign\n";
    CHECK(errorsOf("int a(void* r, float x) { return http_reply_text(r, 200, \"\", \"%d\", x); }\n")
          == "test.fm:1:68: error: format specifies type 'int' but the argument has type 'float'\n");
    CHECK(errorsOf("int b(void* r, int x) { return http_reply_text(r, 200, \"\", \"%g\", x); }\n")
          == "test.fm:1:66: error: format specifies type 'double' but the argument has type 'int'\n");
    CHECK(errorsOf("int c(void* r, float4 v) { return http_reply_text(r, 200, \"\", \"%g\", v); }\n")
          == "test.fm:1:69: error: format specifies type 'double' but the argument has type 'float4'\n");
    CHECK(errorsOf("int d(void* r) { return http_reply_text(r, 200, \"\", \"%g\", \"x\"); }\n")
          == "test.fm:1:59: error: format specifies type 'double' but the argument has type 'char *'\n");
    CHECK(errorsOf("int e(void* r, int x) { return http_reply_text(r, 200, \"\", \"%d %g\", x); }\n")
          == "test.fm:1:65: error: more '%' conversions than data arguments\n");
    CHECK(errorsOf("int f(void* r, int x) { return http_reply_text(r, 200, \"\", \"%d\", x, x); }\n")
          == "test.fm:1:69: error: data argument not used by format string\n");
    // One left over is "not used" whatever it is, once; one with an error of its own has that error alone.
    CHECK(errorsOf("int f2(void* r, int x) { return http_reply_text(r, 200, \"\", \"%d\", x, \"x\"); }\n")
          == "test.fm:1:70: error: data argument not used by format string\n");
    CHECK(errorsOf("int f3(void* r, int x) { return http_reply_text(r, 200, \"\", \"%d\", x, nope); }\n")
          == "test.fm:1:70: error: use of undeclared identifier 'nope'\n");
    CHECK(errorsOf("int g(void* r, double x) { return http_reply_text(r, 200, \"\", \"%.18g\", x); }\n")
          == "test.fm:1:65: error: a reply's %.Ng takes a precision N of 1 to 17\ntest.fm:1:65: " + note);
    CHECK(errorsOf("int h(void* r, double x) { return http_reply_text(r, 200, \"\", \"x=%f\", x); }\n")
          == "test.fm:1:67: error: invalid conversion specifier 'f'\ntest.fm:1:67: " + note);
    CHECK(errorsOf("int i(void* r) { return http_reply_text(r, 200, \"text/plain\"); }\n")
          == "test.fm:1:25: error: too few arguments to function call, expected at least 4, have 3\n");
    CHECK(errorsOf("int j(void* r, int k) { return http_reply_text(r, 200, \"\", k); }\n")
          == "test.fm:1:60: error: argument to 'http_reply_text' must be a string literal\n");
    // %s takes a string, a literal among them; and a string is no %d's.
    CHECK(errorsOf("int s(void* r, int x) { return http_reply_text(r, 200, \"\", \"%s\", x); }\n")
          == "test.fm:1:66: error: format specifies type 'string' but the argument has type 'int'\n");
    CHECK(errorsOf("int t(void* r, string x) { return http_reply_text(r, 200, \"\", \"%d\", x); }\n")
          == "test.fm:1:69: error: format specifies type 'int' but the argument has type 'string'\n");
    CHECK(errorsOf("int u(void* r, string x) { return http_reply_text(r, 200, \"\", \"%s %s\", x); }\n")
          == "test.fm:1:68: error: more '%' conversions than data arguments\n");
    // A bool for %d, as C promotes it; a float or a double, and C's double constants, for %g; a string or a literal for
    // %s.
    std::string errors;
    CHECK(compile("int k(void* r, bool b, float f, double d, string s)\n"
                  "{ return http_reply_text(r, 200, \"\", \"%d %g %.9g %.17g %g %s %s %%\", b, f, d, 0.1, f * 0.5, s, "
                  "\"x\"); }\n",
                  &errors));
    CHECK(errors.empty());
}

TEST_CASE("a query's name and a reply's content type are literals, checked where they are written",
          "[codegen][web][server]")
{
    if (FMINUS_HAS_WEB_SERVER == 0)
        return;
    CHECK(errorsOf("float a(void* r) { return http_query_float(r, \"a b\", 0.0f); }\n")
          == "test.fm:1:49: error: character ' ' is not allowed in a query's name: letters, digits, '_', '-' and "
             "'.'\n");
    CHECK(errorsOf("int b(void* r) { return http_query_int(r, \"a=1\", 0); }\n")
          == "test.fm:1:45: error: character '=' is not allowed in a query's name: letters, digits, '_', '-' and "
             "'.'\n");
    CHECK(errorsOf("int c(void* r) { return http_query_int(r, \"\", 0); }\n")
          == "test.fm:1:43: error: a query's name is 1 to 64 bytes, not 0\n");
    CHECK(errorsOf("int d(void* r) { return http_query_int(r, \"" + std::string(65, 'n') + "\", 0); }\n")
          == "test.fm:1:43: error: a query's name is 1 to 64 bytes, not 65\n");
    CHECK(errorsOf("int e(void* r, int k) { return http_query_int(r, k, 0); }\n")
          == "test.fm:1:50: error: argument to 'http_query_int' must be a string literal\n");
    CHECK(errorsOf("int f(void* r, const void* d) { return http_reply_bytes(r, 200, d, 4, \"a\\r\\nb\"); }\n")
          == "test.fm:1:73: error: character '\\x0d' is not allowed in a content type: it is printable ASCII\n");
    CHECK(errorsOf("int g(void* r) { return http_reply_text(r, 200, \"" + std::string(256, 't') + "\", \"\"); }\n")
          == "test.fm:1:49: error: a content type is at most 255 bytes, not 256\n");
    std::string errors;
    CHECK(compile("float h(void* r) { return http_query_float(r, \"" + std::string(64, 'n') + "\", 0.0f)\n"
                  "                         + (float)http_query_int(r, \"a.b-c_D9\", 0); }\n",
                  &errors));
    CHECK(errors.empty());
}

#if FMINUS_HAS_WEB_SERVER
TEST_CASE("the JIT defines the server's functions", "[codegen][web][server]")
{
    fminus::initializeLLVM();
    auto jit = llvm::cantFail(llvm::orc::LLJITBuilder().create());
    llvm::cantFail(fminus::defineRuntimeSymbols(*jit));
    for (const char* name : {"fminus_web_http_listen", "fminus_web_http_port", "fminus_web_http_next",
                             "fminus_web_http_accept", "fminus_web_http_is", "fminus_web_http_arg",
                             "fminus_web_http_query_float", "fminus_web_http_query_int", "fminus_web_http_reply",
                             "fminus_web_http_reply_bytes", "fminus_web_http_reply_text"})
    {
        INFO(name);
        auto found = jit->lookup(name);
        CHECK(static_cast<bool>(found));
        if (!found)
            llvm::consumeError(found.takeError());
    }
}

TEST_CASE("every http_listen hands the library the program's route table, each literal once", "[codegen][web][server]")
{
    llvm::LLVMContext context;
    std::string errors;
    // A route after the listen that needs it, one twice, one in a function nothing calls.
    auto module = lowered("void* start() { return http_listen(); }\n"
                          "void* again() { return http_listen(); }\n"
                          "int taker(void* r)\n"
                          "{\n"
                          "    if (http_is(r, \"GET /state\"))\n"
                          "        return http_reply(r, 204);\n"
                          "    if (http_is(r, \"PUT /osc/%d/gain\") || http_is(r, \"GET /state\"))\n"
                          "        return http_reply(r, 200);\n"
                          "    return http_reply(r, 404);\n"
                          "}\n"
                          "bool unused(void* r) { return http_is(r, \"DELETE /presets/%d\"); }\n",
                          context, errors);
    INFO(errors);
    REQUIRE(module);
    const llvm::GlobalVariable* routes = module->getNamedGlobal("fminus.web.routes");
    const llvm::GlobalVariable* lengths = module->getNamedGlobal("fminus.web.route.lengths");
    REQUIRE(routes != nullptr);
    REQUIRE(lengths != nullptr);
    CHECK(routes->isConstant());
    CHECK(routes->hasPrivateLinkage());
    const auto* table = llvm::cast<llvm::ConstantArray>(routes->getInitializer());
    const auto* sizes = llvm::cast<llvm::ConstantDataArray>(lengths->getInitializer());
    REQUIRE(table->getNumOperands() == 3);
    const std::vector<std::string> expected{"GET /state", "PUT /osc/%d/gain", "DELETE /presets/%d"};
    for (unsigned i = 0; i < 3; ++i)
    {
        INFO(i);
        const auto* literal = llvm::cast<llvm::GlobalVariable>(table->getOperand(i));
        CHECK(llvm::cast<llvm::ConstantDataArray>(literal->getInitializer())->getAsString() == expected[i]);
        CHECK(sizes->getElementAsInteger(i) == expected[i].size());
    }
    int listens = 0;
    for (const llvm::Function& function : *module)
    {
        for (const llvm::BasicBlock& block : function)
        {
            for (const llvm::Instruction& instruction : block)
            {
                const auto* call = llvm::dyn_cast<llvm::CallInst>(&instruction);
                if (call == nullptr || call->getCalledFunction() == nullptr
                    || call->getCalledFunction()->getName() != "fminus_web_http_listen")
                    continue;
                ++listens;
                CHECK(call->getArgOperand(0) != nullptr);   // the context, loaded
                CHECK(call->getArgOperand(1) == routes);
                CHECK(call->getArgOperand(2) == lengths);
                CHECK(llvm::cast<llvm::ConstantInt>(call->getArgOperand(3))->getZExtValue() == 3);
            }
        }
    }
    CHECK(listens == 2);
    CHECK(module->getNamedGlobal(fminus::ir::kWebContextSymbol) != nullptr);

    // A program with no route hands none.
    auto bare = lowered("void* start() { return http_listen(); }\n", context, errors);
    REQUIRE(bare);
    CHECK(bare->getNamedGlobal("fminus.web.routes") == nullptr);
    CHECK(fminus::ir::webRoutes(*bare).empty());
}

TEST_CASE("the declarations say what the library does: every call that takes or answers a request changes what "
          "another reads",
          "[codegen][web][server]")
{
    llvm::LLVMContext context;
    std::string errors;
    auto module = lowered("int all(const float* data)\n"
                          "{\n"
                          "    void* s = http_listen();\n"
                          "    void* r = http_next(s);\n"
                          "    void* a = http_accept(s, 1);\n"
                          "    int n = http_port(s) + http_arg(r, 0) + http_query_int(r, \"n\", 0);\n"
                          "    n = n + (http_is(r, \"GET /a\") ? 1 : 0) + (int)http_query_float(r, \"g\", 0.0f);\n"
                          "    n = n + http_reply(r, 204) + http_reply_bytes(a, 200, data, 4, \"\");\n"
                          "    return n + http_reply_text(a, 200, \"\", \"%d\", n);\n"
                          "}\n",
                          context, errors);
    INFO(errors);
    REQUIRE(module);
    const auto function = [&](const char* name) -> const llvm::Function& {
        const llvm::Function* found = module->getFunction(name);
        REQUIRE(found != nullptr);
        return *found;
    };
    const llvm::MemoryEffects library = llvm::MemoryEffects::inaccessibleOrArgMemOnly();
    // Taking a request writes (a compare-and-swap): two calls are two requests, and a polling loop calls it each time.
    for (const char* name : {"fminus_web_http_next", "fminus_web_http_accept", "fminus_web_http_is"})
    {
        INFO(name);
        CHECK(function(name).getMemoryEffects() == library);
        CHECK(!function(name).onlyReadsMemory());
        CHECK(!function(name).hasNoSync());
    }
    CHECK(!function("fminus_web_http_accept").willReturn());   // http_accept(s, -1) may wait for ever
    CHECK(function("fminus_web_http_next").willReturn());
    // The reads read what the I/O thread writes: not readonly, so no two are one and none leaves a loop.
    for (const char* name : {"fminus_web_http_port", "fminus_web_http_arg", "fminus_web_http_query_int",
                             "fminus_web_http_query_float"})
    {
        INFO(name);
        CHECK(function(name).getMemoryEffects() == library);
        CHECK(!function(name).hasNoSync());
        CHECK(function(name).doesNotFreeMemory());
        CHECK(function(name).willReturn());
    }
    // A reply hands the request back, and its body with it: no load of that body may move after it.
    for (const char* name : {"fminus_web_http_reply", "fminus_web_http_reply_bytes", "fminus_web_http_reply_text"})
    {
        INFO(name);
        CHECK(function(name).getMemoryEffects() == llvm::MemoryEffects::unknown());
        CHECK(function(name).doesNotFreeMemory());
        CHECK(function(name).willReturn());
    }
    CHECK(function("fminus_web_http_reply_bytes").hasParamAttribute(2, llvm::Attribute::ReadOnly));
    CHECK(function("fminus_web_http_reply_text").hasParamAttribute(6, llvm::Attribute::ReadOnly));   // the values
    // listen reads the table and the literals it points to, and allocates.
    const llvm::MemoryEffects listen = function("fminus_web_http_listen").getMemoryEffects();
    CHECK(listen.getModRef(llvm::IRMemLocation::Other) == llvm::ModRefInfo::Ref);
    CHECK(listen.getModRef(llvm::IRMemLocation::ArgMem) == llvm::ModRefInfo::ModRef);
    CHECK(listen.getModRef(llvm::IRMemLocation::ErrnoMem) == llvm::ModRefInfo::ModRef);   // read after a failed bind
    CHECK(!function("fminus_web_http_listen").doesNotFreeMemory());
}

TEST_CASE("webRoutes lists every route literal, its method and where it is, before anything runs",
          "[library][web][server]")
{
    fminus::Program::Options options;
    options.fileName = "synth.fm";
    options.include = [](std::string_view name, std::string_view) -> std::optional<fminus::Program::IncludedFile> {
        if (name != "presets.fm")
            return std::nullopt;
        return fminus::Program::IncludedFile{"presets.fm", "bool preset(void* r)\n"
                                                           "{\n"
                                                           "    return http_is(r, \"DELETE /presets/%d\")\n"
                                                           "           || http_is(r, \"PATCH /presets/%d/name\");\n"
                                                           "}\n"};
    };
    std::string errors;
    auto program = fminus::Program::compile("#include \"presets.fm\"\n"
                                            "int taker(void* r)\n"
                                            "{\n"
                                            "    if (http_is(r, \"PUT /osc/%d/gain\") || preset(r))\n"
                                            "        return http_reply(r, 204);\n"
                                            "    if (http_is(r, \"GET /state\"))\n"
                                            "        return http_reply_text(r, 200, \"\", \"%d\", 1);\n"
                                            "    return http_reply(r, http_is(r, \"POST /bank/%d/slot/%d\") ? 201 : "
                                            "404);\n"
                                            "}\n",
                                            errors, options);
    INFO(errors);
    REQUIRE(program);
    const std::vector<fminus::Program::WebRoute>& routes = program->webRoutes();
    REQUIRE(routes.size() == 5);
    CHECK(routes[0].method == "DELETE");
    CHECK(routes[0].pattern == "/presets/*");
    CHECK(routes[0].file == "presets.fm");
    CHECK(routes[0].line == 3);
    CHECK(routes[0].column == 12);
    CHECK(routes[1].method == "PATCH");
    CHECK(routes[1].pattern == "/presets/*/name");
    CHECK(routes[1].line == 4);
    CHECK(routes[2].method == "PUT");
    CHECK(routes[2].pattern == "/osc/*/gain");
    CHECK(routes[2].file == "synth.fm");
    CHECK(routes[2].line == 4);
    CHECK(routes[2].column == 9);
    CHECK(routes[3].method == "GET");
    CHECK(routes[3].pattern == "/state");
    CHECK(routes[3].line == 6);
    CHECK(routes[4].method == "POST");
    CHECK(routes[4].pattern == "/bank/*/slot/*");
    CHECK(routes[4].line == 8);
    CHECK(routes[4].column == 26);
    CHECK(program->webRequests().empty());
    CHECK(program->webPort() == 0);   // it never listened
    // A program that names none lists none.
    auto quiet = fminus::Program::compile("float f(float x) { return x; }\n", errors);
    REQUIRE(quiet);
    CHECK(quiet->webRoutes().empty());
}

TEST_CASE("Options::web.server is checked when the program compiles; the default listens nowhere",
          "[library][web][server]")
{
    const char* source = "int port() { return http_port(http_listen()); }\n"
                         "void* none() { return http_next(http_listen()); }\n";
    std::string errors;
    fminus::Program::Options options;
    options.web.server.listen = "0.0.0.0:8080";
    CHECK(fminus::Program::compile(source, errors, options) == nullptr);
    CHECK(errors == "fminus::Program: web: server.listen '0.0.0.0:8080': 0.0.0.0 is the unspecified address: write "
                    "'*' for every interface\n");
    options.web.server.listen = "*:8080";   // every interface, with neither a token nor open
    CHECK(fminus::Program::compile(source, errors, options) == nullptr);
    CHECK(errors.starts_with("fminus::Program: web: server.listen '*:8080' is not a loopback address"));
    options.web.server.listen = "127.0.0.1:6000";   // a port browsers refuse
    CHECK(fminus::Program::compile(source, errors, options) == nullptr);
    options.web.server.listen.clear();
    options.web.server.token = "short";   // checked with or without a listen
    CHECK(fminus::Program::compile(source, errors, options) == nullptr);
    CHECK(errors == "fminus::Program: web: server.token has 5 characters (16 to 256)\n");

    // The default: http_listen's server never listens, and takes nothing.
    auto program = fminus::Program::compile(source, errors);
    INFO(errors);
    REQUIRE(program);
    auto* port = program->function<int()>("port", errors);
    auto* none = program->function<void*()>("none", errors);
    REQUIRE(port);
    REQUIRE(none);
    CHECK(port() == FMINUS_WEB_REFUSED);
    CHECK(none() == nullptr);
    CHECK(program->webPort() == 0);
    program->stopWeb();   // nothing to stop: harmless
}
#endif
