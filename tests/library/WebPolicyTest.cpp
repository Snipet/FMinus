// tests/library/WebPolicyTest.cpp: the web library's host API as a host of FMinus::FMinus uses it, in C++17
// (docs/web.md): Options::web reaches nothing by default, webRequests() lists a program's URLs before anything runs,
// and stopWeb() is harmless with nothing to stop. In a build without the web library (fminus/Config.h's
// FMINUS_HAS_WEB 0) the same host code compiles, the policy is not read, and a program that makes no request lists
// none. tests/unit/WebTest.cpp fetches from a loopback server; nothing here makes a request.
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
