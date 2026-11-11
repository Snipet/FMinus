// tests/library/WebPolicyTest.cpp: the web library's host API as a host of FMinus::FMinus uses it, in C++17
// (docs/web.md): Options::web reaches nothing by default, webRequests() lists a program's URLs before anything runs,
// and stopWeb() is harmless with nothing to stop. In a build without the web library (fminus/Config.h's
// FMINUS_HAS_WEB 0) the same host code compiles, the policy is not read, and a program that makes no request lists
// none. The server's part too: Options::web.server listens nowhere by default, webRoutes() lists a program's routes,
// and webPort() is 0 until a program listens, in every build. tests/unit/WebTest.cpp fetches from a loopback server,
// tests/unit/WebServeTest.cpp serves on one; nothing here makes a request or listens.
#include "fminus/FMinus.h"

#include <catch2/catch_test_macros.hpp>

#include <memory>
#include <string>

TEST_CASE("Options::web reaches nothing by default, and webRequests lists every URL", "[library][web]")
{
    const fminus::Program::Options defaults;
    CHECK(defaults.web.allow.empty());
    CHECK(defaults.web.caFile.empty());
    CHECK_FALSE(defaults.web.privateAddresses);

    std::string errors;
    auto quiet = fminus::Program::compile("float f(float x) { return x; }\n", errors);
    REQUIRE(quiet);
    CHECK(quiet->webRequests().empty());
    quiet->stopWeb();

#if FMINUS_HAS_WEB
    auto program = fminus::Program::compile("int fetch(int k)\n"
                                            "{\n"
                                            "    void* r = http_get(\"https://example.com/presets/%d.json\", k);\n"
                                            "    const int status = http_wait(r, -1);\n"
                                            "    http_close(r);\n"
                                            "    return status;\n"
                                            "}\n",
                                            errors);
    INFO(errors);
    REQUIRE(program);
    REQUIRE(program->webRequests().size() == 1);
    const fminus::Program::WebRequest& request = program->webRequests()[0];
    CHECK(request.method == "GET");
    CHECK(request.pattern == "https://example.com/presets/*.json");
    CHECK(request.file == "program.fm");
    CHECK(request.line == 3);
    auto* fetch = program->function<int(int)>("fetch", errors);
    REQUIRE(fetch != nullptr);
    CHECK(fetch(7) == -4);   // HTTP_REFUSED: no network by default, and nothing sent
    program->stopWeb();
    CHECK(fetch(7) == -4);   // still refused: no context to stop, and none to reach
#endif
}

TEST_CASE("Options::web.server listens nowhere by default, webRoutes lists every route, webPort is 0 until a program "
          "listens",
          "[library][web][server]")
{
    const fminus::Program::Options defaults;
    CHECK(defaults.web.server.listen.empty());
    CHECK(defaults.web.server.token.empty());
    CHECK(defaults.web.server.hosts.empty());
    CHECK(defaults.web.server.files.empty());
    CHECK_FALSE(defaults.web.server.open);
    CHECK(defaults.web.server.maxConnections == 16);
    CHECK(defaults.web.server.maxPending == 16);
    CHECK(defaults.web.server.maxRequestBytes == (1 << 20));
    CHECK(defaults.web.server.maxReplyBytes == (64 << 10));
    CHECK(defaults.web.server.requestTimeoutMs == 10000);
    CHECK(defaults.web.server.replyTimeoutMs == 2000);
    CHECK(defaults.web.server.idleTimeoutMs == 15000);

    std::string errors;
    auto quiet = fminus::Program::compile("float f(float x) { return x; }\n", errors);
    REQUIRE(quiet);
    CHECK(quiet->webRoutes().empty());
    CHECK(quiet->webPort() == 0);

#if FMINUS_HAS_WEB_SERVER
    auto program = fminus::Program::compile("int serve()\n"
                                            "{\n"
                                            "    void* server = http_listen();\n"
                                            "    void* r = http_next(server);\n"
                                            "    if (http_is(r, \"PUT /osc/%d/gain\"))\n"
                                            "        return http_reply(r, 204);\n"
                                            "    return http_port(server);\n"
                                            "}\n",
                                            errors);
    INFO(errors);
    REQUIRE(program);
    REQUIRE(program->webRoutes().size() == 1);
    const fminus::Program::WebRoute& route = program->webRoutes()[0];
    CHECK(route.method == "PUT");
    CHECK(route.pattern == "/osc/*/gain");
    CHECK(route.file == "program.fm");
    CHECK(route.line == 5);
    CHECK(route.column == 9);
    auto* serve = program->function<int()>("serve", errors);
    REQUIRE(serve != nullptr);
    CHECK(serve() == -4);   // HTTP_REFUSED: no listen by default, so no server
    CHECK(program->webPort() == 0);
    program->stopWeb();
#endif
}
