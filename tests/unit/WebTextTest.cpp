// tests/unit/WebTextTest.cpp: a URL with strings in it (src/web/Web.h, fminus_web_http_get_s and _post_s), from C++,
// before any builtin calls them: where a %s may be and where it is refused, the path segments a text may not make,
// texts that are not ones, the checks every URL meets afterwards (a "/" encoded in the path, the length, the policy),
// and a format with no %s, which is the entry point without strings. Every request here ends before anything is sent:
// the policy allows only a path on loopback no URL here reaches (a request that passes every check is refused by the
// pattern), so nothing connects anywhere. That a text arrives encoded as it should is WebServerTest.cpp's, through a
// loopback server. In a build without FMINUS_WEB nothing here is compiled.
#include "fminus/Config.h"

#if FMINUS_HAS_WEB
#include "web/Web.h"

#include <catch2/catch_test_macros.hpp>

#include <cstdint>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

using fminus::web::Context;
using fminus::web::Policy;

namespace
{
    std::unique_ptr<Context> makeContext()
    {
        Policy policy;
        policy.allow = {"http://127.0.0.1:9/never/*"};
        std::string error;
        std::unique_ptr<Context> context = Context::create(policy, error);
        INFO(error);
        REQUIRE(context);
        return context;
    }

    struct Text
    {
        const char* bytes;
        std::int32_t length;
    };

    Text text(std::string_view t)
    {
        return {t.data(), static_cast<std::int32_t>(t.size())};
    }

    // The status a GET of the format with these texts and ints ends with at once (a request that would be sent is
    // refused by the pattern instead: REFUSED says it met every check of its own).
    std::int32_t statusOf(Context& context, std::string_view format, const std::vector<Text>& texts,
                          const std::vector<std::int32_t>& ints = {}, bool post = false)
    {
        std::vector<const char*> addresses;
        std::vector<std::int32_t> lengths;
        for (const Text& t : texts)
        {
            addresses.push_back(t.bytes);
            lengths.push_back(t.length);
        }
        void* r = post ? fminus_web_http_post_s(context.handle(), format.data(), static_cast<std::int32_t>(format.size()),
                                                ints.data(), static_cast<std::int32_t>(ints.size()), addresses.data(),
                                                lengths.data(), static_cast<std::int32_t>(texts.size()), "x", 1, "a/b", 3)
                       : fminus_web_http_get_s(context.handle(), format.data(), static_cast<std::int32_t>(format.size()),
                                               ints.data(), static_cast<std::int32_t>(ints.size()), addresses.data(),
                                               lengths.data(), static_cast<std::int32_t>(texts.size()));
        REQUIRE(r != nullptr);
        const std::int32_t status = fminus_web_http_status(r);
        fminus_web_http_close(r);
        return status;
    }
} // namespace

TEST_CASE("a %s in a URL's scheme or authority is invalid, whatever it holds", "[web]")
{
    const std::unique_ptr<Context> context = makeContext();
    Context& c = *context;
    CHECK(statusOf(c, "%s://127.0.0.1:9/a", {text("http")}) == FMINUS_WEB_INVALID);
    CHECK(statusOf(c, "http%s://127.0.0.1:9/a", {text("")}) == FMINUS_WEB_INVALID);
    CHECK(statusOf(c, "http://%s/a", {text("127.0.0.1:9")}) == FMINUS_WEB_INVALID);
    CHECK(statusOf(c, "http://127.0.0.1%s/a", {text("")}) == FMINUS_WEB_INVALID);
    CHECK(statusOf(c, "http://127.0.0.1:%s/a", {text("9")}) == FMINUS_WEB_INVALID);
    CHECK(statusOf(c, "http://u%s@127.0.0.1:9/a", {text("x")}) == FMINUS_WEB_INVALID);
    CHECK(statusOf(c, "http://127.0.0.1:9%s", {text("")}) == FMINUS_WEB_INVALID);
    CHECK(statusOf(c, "127.0.0.1:9/%s", {text("a")}) == FMINUS_WEB_INVALID);   // no "://": all of it the scheme
    CHECK(statusOf(c, "http:/127.0.0.1/%s", {text("a")}) == FMINUS_WEB_INVALID);
    CHECK(statusOf(c, "%s://127.0.0.1:9/a", {text("http")}, {}, true) == FMINUS_WEB_INVALID);
    // A %d in the port is a URL's as ever; a %s after the authority's end is not in it.
    CHECK(statusOf(c, "http://127.0.0.1:%d/a/%s", {text("b")}, {9}) == FMINUS_WEB_REFUSED);
    CHECK(statusOf(c, "http://127.0.0.1:9?q=%s", {text("x")}) == FMINUS_WEB_REFUSED);
    CHECK(statusOf(c, "http://127.0.0.1:9#%s", {text("x")}) == FMINUS_WEB_REFUSED);
}

TEST_CASE("a %s that makes a path segment empty, \".\" or \"..\" is invalid; anywhere else they are text",
          "[web]")
{
    const std::unique_ptr<Context> context = makeContext();
    Context& c = *context;
    for (const std::string_view bad : {"", ".", ".."})
    {
        INFO("'" << bad << "'");
        CHECK(statusOf(c, "http://127.0.0.1:9/a/%s/b", {text(bad)}) == FMINUS_WEB_INVALID);
        CHECK(statusOf(c, "http://127.0.0.1:9/a/%s", {text(bad)}) == FMINUS_WEB_INVALID);
        CHECK(statusOf(c, "http://127.0.0.1:9/%s?x", {text(bad)}) == FMINUS_WEB_INVALID);
        CHECK(statusOf(c, "http://127.0.0.1:9/%s#x", {text(bad)}) == FMINUS_WEB_INVALID);
        CHECK(statusOf(c, "http://127.0.0.1:9/a/%s%s", {text(bad), text("")}) == FMINUS_WEB_INVALID);
        CHECK(statusOf(c, "http://127.0.0.1:9/a/x%s", {text(bad)}) == FMINUS_WEB_REFUSED);
        CHECK(statusOf(c, "http://127.0.0.1:9/a/%sx", {text(bad)}) == FMINUS_WEB_REFUSED);
        CHECK(statusOf(c, "http://127.0.0.1:9/a?q=%s", {text(bad)}) == FMINUS_WEB_REFUSED);
        CHECK(statusOf(c, "http://127.0.0.1:9/a?q=%s", {text(bad)}, {}, true) == FMINUS_WEB_REFUSED);
    }
    CHECK(statusOf(c, "http://127.0.0.1:9/a/%s", {text("...")}) == FMINUS_WEB_REFUSED);
    CHECK(statusOf(c, "http://127.0.0.1:9/a/%s%s", {text("."), text(".")}) == FMINUS_WEB_INVALID);
    CHECK(statusOf(c, "http://127.0.0.1:9/a/%s%s", {text("."), text("x")}) == FMINUS_WEB_REFUSED);
    CHECK(statusOf(c, "http://127.0.0.1:9/a/.%s", {text("")}) == FMINUS_WEB_INVALID);   // the segment is "."
    CHECK(statusOf(c, "http://127.0.0.1:9/a/%d%s", {text("")}, {5}) == FMINUS_WEB_REFUSED);
    // A %s past the last text is empty.
    CHECK(statusOf(c, "http://127.0.0.1:9/a/%s", {}) == FMINUS_WEB_INVALID);
    CHECK(statusOf(c, "http://127.0.0.1:9/a/b%s", {}) == FMINUS_WEB_REFUSED);
}

TEST_CASE("a URL filled in with texts meets every check a URL meets", "[web]")
{
    const std::unique_ptr<Context> context = makeContext();
    Context& c = *context;
    // A "/" or a "\" a text writes in the path is %2F or %5C, which a URL's path may not have.
    CHECK(statusOf(c, "http://127.0.0.1:9/a/%s", {text("x/y")}) == FMINUS_WEB_REFUSED);
    CHECK(statusOf(c, "http://127.0.0.1:9/a/%s", {text("x\\y")}) == FMINUS_WEB_REFUSED);
    // The length: 8192 bytes at most, encoded.
    const std::string spaces(2700, ' ');   // 8100 bytes encoded
    const std::string prefix = "http://127.0.0.1:9/nope/";
    const std::string fits(8192 - prefix.size() - 8100, 'a');
    CHECK(statusOf(c, prefix + fits + "%s", {text(spaces)}) == FMINUS_WEB_REFUSED);   // 8192 bytes: the pattern's
    CHECK(statusOf(c, prefix + fits + "a%s", {text(spaces)}) == FMINUS_WEB_LIMIT);
    const std::string huge(1 << 20, '/');
    CHECK(statusOf(c, "http://127.0.0.1:9/a?q=%s", {text(huge)}) == FMINUS_WEB_LIMIT);
    CHECK(statusOf(c, "http://127.0.0.1:9/a/%s", {text(huge)}) == FMINUS_WEB_LIMIT);
    CHECK(statusOf(c, "http://127.0.0.1:9/a/%s/b", {text(std::string(9000, '.'))}) == FMINUS_WEB_LIMIT);
    // A text that is not one.
    CHECK(statusOf(c, "http://127.0.0.1:9/a/%s", {{"x", -1}}) == FMINUS_WEB_INVALID);
    CHECK(statusOf(c, "http://127.0.0.1:9/a?q=%s", {{nullptr, 3}}) == FMINUS_WEB_INVALID);
    CHECK(statusOf(c, "http://127.0.0.1:9/a?q=%s", {{nullptr, 0}}) == FMINUS_WEB_REFUSED);
    CHECK(statusOf(c, "http://127.0.0.1:9/a/x?q=%s", {{"x", -1}}, {}, true) == FMINUS_WEB_INVALID);
}

TEST_CASE("a URL with no %s is the entry point's without strings; a null context refuses", "[web]")
{
    const std::unique_ptr<Context> context = makeContext();
    Context& c = *context;
    const auto plain = [&](std::string_view url, const std::vector<std::int32_t>& ints = {})
    {
        void* r = fminus_web_http_get(c.handle(), url.data(), static_cast<std::int32_t>(url.size()), ints.data(),
                                      static_cast<std::int32_t>(ints.size()));
        const std::int32_t status = fminus_web_http_status(r);
        fminus_web_http_close(r);
        return status;
    };
    for (const std::string_view url : {"http://127.0.0.1:9/a/%d", "http://127.0.0.1:%d/a", "ftp://127.0.0.1/%d",
                                       "http://127.0.0.1:9/a%2F%d", "http://[::1/%d", "http://127.0.0.1:9/%%s"})
    {
        INFO(url);
        CHECK(statusOf(c, url, {text("unused")}, {9}) == plain(url, {9}));
    }
    CHECK(plain("http://127.0.0.1:9/%s") == FMINUS_WEB_REFUSED);   // as text, a percent sign
    void* r = fminus_web_http_get_s(nullptr, "http://%s", 9, nullptr, 0, nullptr, nullptr, 0);
    CHECK(fminus_web_http_status(r) == FMINUS_WEB_REFUSED);
    fminus_web_http_close(r);
    r = fminus_web_http_post_s(nullptr, "http://%s", 9, nullptr, 0, nullptr, nullptr, 0, nullptr, 0, nullptr, 0);
    CHECK(fminus_web_http_status(r) == FMINUS_WEB_REFUSED);
    fminus_web_http_close(r);
}
#endif
