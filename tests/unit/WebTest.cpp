// tests/unit/WebTest.cpp: the web library (src/web/Web.h), at its own level: the C functions FMinus's http_ builtins
// call and the host's Context and Policy, without the compiler. The policy's pattern language and its classic
// confusions (userinfo, an allowed host as the prefix of a longer one, letter case, trailing dots, ports, IP addresses
// in several spellings, percent-encoding, backslashes); the address check; every status a request can end with,
// against a loopback HTTP server the test itself starts on an ephemeral port (fixed responses, a slow one, redirects
// to an allowed and to a forbidden target, a body over the limit, a dropped connection, a response that never ends);
// destroying a context with requests in flight and with a thread waiting; many requests at once; close and cancel
// racing completion; and, where the library's TLS is Mbed TLS, HTTPS against the same server with a certificate
// authority the test makes, a wrong host name, and an untrusted authority, and a program's https request through the
// JIT with the host's roots. Then the language's side, against the same
// server: a program that fetches and parses a JSON body through the JIT (fminus::Program, Options::web) and as an
// executable (FMINUS_WEB_ALLOW), the policy refusing through http_status, a Program destroyed with requests in flight
// and stopped with a thread in http_wait. No request, and no DNS lookup, ever leaves this machine's loopback interface
// (a name is a resolve entry's, or under .localhost, which libcurl answers itself). In a build without
// FMINUS_WEB there is nothing to test, and nothing here is compiled (tests/unit/HttpTest.cpp tests the builtins there).
#include "fminus/Config.h"

#if FMINUS_HAS_WEB

#include "fminus/FMinus.h"
#include "frontend/Frontend.h"
#include "ir/CodeGen.h"
#include "support/Aot.h"
#include "support/HostTarget.h"
#include "web/Policy.h"
#include "web/Web.h"

#include <llvm/IR/LLVMContext.h>
#include <llvm/IR/Module.h>
#include <llvm/Support/Error.h>
#include <llvm/Target/TargetMachine.h>

#include <catch2/catch_test_macros.hpp>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <map>
#include <memory>
#include <mutex>
#include <random>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

#if !defined(_WIN32)
#include <arpa/inet.h>
#include <mbedtls/pk.h>
#include <mbedtls/ssl.h>
#include <mbedtls/x509_crt.h>
#include <netinet/in.h>
#include <poll.h>
#include <psa/crypto.h>
#include <sys/socket.h>
#include <sys/wait.h>
#include <unistd.h>
#endif

using fminus::web::Context;
using fminus::web::Policy;
using namespace std::chrono_literals;

namespace
{
    std::unique_ptr<Context> makeContext(const Policy& policy)
    {
        std::string error;
        std::unique_ptr<Context> context = Context::create(policy, error);
        INFO(error);
        REQUIRE(context);
        return context;
    }

    Policy allowing(std::vector<std::string> patterns)
    {
        Policy policy;
        policy.allow = std::move(patterns);
        return policy;
    }

    // What the policy says of `url`.
    std::int32_t check(const Policy& policy, std::string_view url)
    {
        return makeContext(policy)->check(url);
    }

    void* get(Context& context, const std::string& url)
    {
        return fminus_web_http_get(context.handle(), url.data(), static_cast<std::int32_t>(url.size()), nullptr, 0);
    }

    void* post(Context& context, const std::string& url, std::string_view body, std::string_view type)
    {
        return fminus_web_http_post(context.handle(), url.data(), static_cast<std::int32_t>(url.size()), nullptr, 0,
                                    body.data(), static_cast<std::int32_t>(body.size()), type.data(),
                                    static_cast<std::int32_t>(type.size()));
    }

    std::string body(const void* request)
    {
        const void* data = fminus_web_http_body(request);
        return data == nullptr ? std::string()
                               : std::string(static_cast<const char*>(data),
                                             static_cast<std::size_t>(fminus_web_http_size(request)));
    }

    fminus::web::Address address(std::string_view text)
    {
        fminus::web::Address out;
        if (!fminus::web::parseAddress(text, 4, out))
            REQUIRE(fminus::web::parseAddress(text, 6, out));
        return out;
    }
} // namespace

// ---- the policy -----------------------------------------------------------------------------------------------------

TEST_CASE("a policy's patterns are checked when its context is made, and a bad one is named", "[web][policy]")
{
    CHECK(fminus::web::checkPolicy(Policy()).empty());
    for (const char* bad : {"example.com",
                            "ftp://example.com",
                            "https://",
                            "https://ex*ample.com",
                            "https://*",
                            "https://*.",
                            "https://a.*.example.com",
                            "https://example.com/a*b",
                            "https://example.com/**",
                            "https://example.com:0",
                            "https://example.com:65536",
                            "https://example.com:",
                            "https://example.com:x",
                            "https://u@example.com",
                            "https://u:p@example.com/",
                            "https://example.com/%2e",
                            "https://example.com/?q",
                            "https://example.com/#f",
                            "https://example.com\\@evil.com",
                            "https://[::1",
                            "https://[::1]x",
                            "https://[nope]",
                            "https://[fe80::1%25eth0]",
                            "https://0x7f.0.0.1",
                            "https://010.0.0.1",
                            "https://127.1",
                            "https://1.2.3",
                            "https://example.123",
                            "https://exa mple.com",
                            "https://ex_ample.com",
                            "https://-.com.",
                            "https://example..com",
                            "https://\xc3\xa9xample.com"})
    {
        INFO(bad);
        Policy policy = allowing({bad});
        const std::string error = fminus::web::checkPolicy(policy);
        CHECK(!error.empty());
        std::string created;
        CHECK(Context::create(policy, created) == nullptr);
        CHECK(created == error);
    }
    for (const char* good : {"https://example.com", "HTTPS://Example.COM/", "http://example.com:8080/a/*",
                             "https://*.example.com", "https://127.0.0.1:*", "http://[::1]:8080", "http://localhost:*",
                             "https://example.com./x", "https://xn--e1afmkfd.example/"})
    {
        INFO(good);
        CHECK(fminus::web::checkPolicy(allowing({good})).empty());
    }
    CHECK(fminus::web::checkPolicy(allowing({"ftp://example.com"})).find("'ftp://example.com'") != std::string::npos);
    Policy limits;
    limits.maxBodyBytes = 0;
    CHECK(!fminus::web::checkPolicy(limits).empty());
    limits = Policy();
    limits.timeoutMs = 0;
    CHECK(!fminus::web::checkPolicy(limits).empty());
    limits = Policy();
    limits.maxRequests = 0;
    CHECK(!fminus::web::checkPolicy(limits).empty());
    limits = Policy();
    limits.maxRedirects = -1;
    CHECK(!fminus::web::checkPolicy(limits).empty());
    limits = Policy();
    limits.caFile = std::string("a\nb");
    CHECK(!fminus::web::checkPolicy(limits).empty());
    for (const char* bad : {"example.com:80", "example.com:x:127.0.0.1", "+example.com:80:127.0.0.1",
                            "example.com:80:nope", "*:80:127.0.0.1"})
    {
        INFO(bad);
        limits = Policy();
        limits.resolve = {bad};
        CHECK(!fminus::web::checkPolicy(limits).empty());
    }
    limits = Policy();
    limits.resolve = {"a.example.com:443:127.0.0.1", "b.example.com:80:[::1]"};
    CHECK(fminus::web::checkPolicy(limits).empty());
}

TEST_CASE("the default policy allows nothing", "[web][policy]")
{
    const Policy none;
    for (const char* url : {"https://example.com/", "http://127.0.0.1/", "http://localhost/", "https://[::1]/"})
        CHECK(check(none, url) == FMINUS_WEB_REFUSED);
}

TEST_CASE("a pattern's host, scheme, port and path, against the classic confusions", "[web][policy]")
{
    const Policy policy = allowing({"https://example.com/presets/*"});
    struct Case
    {
        const char* url;
        std::int32_t status;
    };
    const Case cases[] = {
        {"https://example.com/presets/a.json", 0},
        {"https://example.com/presets/", 0},
        {"https://example.com/presets/a/b.json?x=1&y=2", 0},   // any query
        {"https://example.com/presets/a.json#frag", 0},
        {"HTTPS://EXAMPLE.COM/presets/a.json", 0},    // the scheme's and the host's case
        {"https://example.com./presets/a.json", 0},   // one trailing dot
        {"https://example.com:443/presets/a.json", 0},
        {"https://example.com/presets/a%20b.json", 0},
        {"https://example.com/Presets/a.json", FMINUS_WEB_REFUSED},   // a path's case matters
        {"https://example.com/presets", FMINUS_WEB_REFUSED},
        {"https://example.com/presetsX/a.json", FMINUS_WEB_REFUSED},
        {"https://example.com/other/a.json", FMINUS_WEB_REFUSED},
        {"http://example.com/presets/a.json", FMINUS_WEB_REFUSED},   // https only
        {"https://example.com:8443/presets/a.json", FMINUS_WEB_REFUSED},
        {"https://example.com../presets/a.json", FMINUS_WEB_INVALID},          // libcurl's parser refuses it
        {"https://example.com.evil.com/presets/a.json", FMINUS_WEB_REFUSED},   // the allowed host as a prefix
        {"https://evilexample.com/presets/a.json", FMINUS_WEB_REFUSED},
        {"https://sub.example.com/presets/a.json", FMINUS_WEB_REFUSED},
        {"https://example.com@evil.com/presets/a.json", FMINUS_WEB_REFUSED},   // userinfo
        {"https://user:password@example.com/presets/a.json", FMINUS_WEB_REFUSED},
        {"https://user@example.com/presets/a.json", FMINUS_WEB_REFUSED},
        {"https://:@example.com/presets/a.json", FMINUS_WEB_REFUSED},
        {"https://evil.com#@example.com/presets/a.json", FMINUS_WEB_REFUSED},
        {"https://evil.com?@example.com/presets/a.json", FMINUS_WEB_REFUSED},
        {"https://example.com\\@evil.com/presets/a.json", FMINUS_WEB_REFUSED},   // backslashes
        {"https://example.com\\presets\\a.json", FMINUS_WEB_REFUSED},
        {"https://example.com/presets\\..\\admin", FMINUS_WEB_REFUSED},
        {"https://example.com/presets/../admin", FMINUS_WEB_REFUSED},   // dot segments, removed by libcurl
        {"https://example.com/presets/./../admin", FMINUS_WEB_REFUSED},
        {"https://example.com/presets/%2e%2e/admin", FMINUS_WEB_REFUSED},   // percent-encoding
        {"https://example.com/presets/%2E%2E/admin", FMINUS_WEB_REFUSED},
        {"https://example.com/presets/..%2fadmin", FMINUS_WEB_REFUSED},
        {"https://example.com/presets/..%5cadmin", FMINUS_WEB_REFUSED},
        // A dot segment with a parameter, which libcurl keeps and a servlet container removes, ";" and all.
        {"https://example.com/presets/..;/admin", FMINUS_WEB_REFUSED},
        {"https://example.com/presets/..;x=1/admin", FMINUS_WEB_REFUSED},
        {"https://example.com/presets/.;/a.json", FMINUS_WEB_REFUSED},
        {"https://example.com/presets/..;", FMINUS_WEB_REFUSED},
        {"https://example.com/presets/a.json;v=2", 0},   // a parameter of a segment that is not a dot
        {"https://example.com/presets/...;/a.json", 0},
        {"https://example.com/presets/a..;/b.json", 0},
        {"https://ex%61mple.com/presets/a.json", 0},   // libcurl decodes it: the request goes to example.com
        {"https://example.com/presets/a.json\r\nHost: evil.com", FMINUS_WEB_REFUSED},   // CR, LF
        {"https://example.com/presets/a\n.json", FMINUS_WEB_REFUSED},
        {"https://example.com/presets/a\t.json", FMINUS_WEB_REFUSED},
        {"https://example.com/presets/a .json", FMINUS_WEB_REFUSED},
        {"https://example.com/presets/\xc3\xa9.json", FMINUS_WEB_REFUSED},
        {"ftp://example.com/presets/a.json", FMINUS_WEB_REFUSED},
        {"file:///etc/passwd", FMINUS_WEB_REFUSED},
        {"example.com/presets/a.json", FMINUS_WEB_INVALID},
        {"https://", FMINUS_WEB_INVALID},
        {"", FMINUS_WEB_INVALID},
    };
    for (const Case& c : cases)
    {
        INFO(c.url);
        CHECK(check(policy, c.url) == c.status);
    }
    // NUL in the middle of a URL.
    const std::string withNul("https://example.com/presets/a\0.json", 35);
    CHECK(check(policy, withNul) == FMINUS_WEB_REFUSED);
    // Longer than 8192 bytes.
    CHECK(check(policy, "https://example.com/presets/" + std::string(9000, 'a')) == FMINUS_WEB_LIMIT);

    // An exact path, and a host's own port.
    // "/" is a path like any other: exactly "/".
    const Policy root = allowing({"https://example.com/"});
    CHECK(check(root, "https://example.com/") == 0);
    CHECK(check(root, "https://example.com") == 0);
    CHECK(check(root, "https://example.com/x") == FMINUS_WEB_REFUSED);

    const Policy exact = allowing({"http://example.com:8080/exact"});
    CHECK(check(exact, "http://example.com:8080/exact") == 0);
    CHECK(check(exact, "http://example.com:8080/exact?a=1") == 0);
    CHECK(check(exact, "http://example.com:8080/exact/") == FMINUS_WEB_REFUSED);
    CHECK(check(exact, "http://example.com:8080/exactly") == FMINUS_WEB_REFUSED);
    CHECK(check(exact, "http://example.com/exact") == FMINUS_WEB_REFUSED);
    CHECK(check(exact, "http://example.com:80/exact") == FMINUS_WEB_REFUSED);

    // Any port; and a host without a path is any path.
    const Policy anyPort = allowing({"https://example.com:*"});
    CHECK(check(anyPort, "https://example.com:1/") == 0);
    CHECK(check(anyPort, "https://example.com/any/thing") == 0);
    CHECK(check(anyPort, "http://example.com/") == FMINUS_WEB_REFUSED);

    // A name with a numeric last label is no name, in a URL either.
    CHECK(check(allowing({"https://*.example.com"}), "https://a.example.123/") == FMINUS_WEB_REFUSED);
}

TEST_CASE("a '*.' pattern is its subdomains, and not the name itself", "[web][policy]")
{
    const Policy policy = allowing({"https://*.example.com"});
    CHECK(check(policy, "https://a.example.com/") == 0);
    CHECK(check(policy, "https://a.b.example.com/x") == 0);
    CHECK(check(policy, "https://A.Example.Com./x") == 0);
    CHECK(check(policy, "https://example.com/") == FMINUS_WEB_REFUSED);
    CHECK(check(policy, "https://aexample.com/") == FMINUS_WEB_REFUSED);
    CHECK(check(policy, "https://a.example.com.evil.com/") == FMINUS_WEB_REFUSED);
    CHECK(check(policy, "https://a.example.co/") == FMINUS_WEB_REFUSED);
}

TEST_CASE("an IP address pattern, against the URL's spellings of addresses", "[web][policy]")
{
    const Policy v4 = allowing({"http://127.0.0.1:*"});
    // libcurl reads these as 127.0.0.1, so the policy does too.
    for (const char* url :
         {"http://127.0.0.1:8080/x", "http://127.1:8080/x", "http://2130706433:8080/x", "http://0x7f.0.0.1:8080/x",
          "http://0x7f000001:8080/x", "http://0177.0.0.1:8080/x", "http://127.0.1:8080/x"})
    {
        INFO(url);
        CHECK(check(v4, url) == 0);
    }
    for (const char* url : {"https://127.0.0.1/", "http://127.0.0.2/", "http://[::ffff:127.0.0.1]/", "http://[::1]/",
                            "http://localhost/", "http://127.0.0.1.nip.io/", "http://0x7f.0.0.1.example/"})
    {
        INFO(url);
        CHECK(check(v4, url) == FMINUS_WEB_REFUSED);
    }
    const Policy v6 = allowing({"http://[::1]:*"});
    CHECK(check(v6, "http://[::1]:80/") == 0);
    CHECK(check(v6, "http://[0:0:0:0:0:0:0:1]:80/") == 0);
    CHECK(check(v6, "http://[0000::0001]/") == 0);
    CHECK(check(v6, "http://[::2]/") == FMINUS_WEB_REFUSED);
    CHECK(check(v6, "http://[::1%25lo]/") == FMINUS_WEB_REFUSED);   // a zone
    CHECK(check(v6, "http://127.0.0.1/") == FMINUS_WEB_REFUSED);
    const Policy localhost = allowing({"http://localhost:*"});
    CHECK(check(localhost, "http://localhost:1/") == 0);
    CHECK(check(localhost, "http://LOCALHOST./") == 0);
    CHECK(check(localhost, "http://localhost.example.com/") == FMINUS_WEB_REFUSED);
    CHECK(check(localhost, "http://127.0.0.1/") == FMINUS_WEB_REFUSED);
}

TEST_CASE("the address check: a name reaches only public addresses, localhost only loopback", "[web][policy]")
{
    using fminus::web::Reach;
    using fminus::web::reachable;
    const fminus::web::Address none;
    for (const char* blocked : {"127.0.0.1",
                                "127.255.0.9",
                                "10.1.2.3",
                                "172.16.0.1",
                                "172.31.255.255",
                                "192.168.1.1",
                                "169.254.169.254",
                                "100.64.0.1",
                                "0.0.0.0",
                                "0.1.2.3",
                                "224.0.0.1",
                                "239.1.1.1",
                                "240.0.0.1",
                                "255.255.255.255",
                                "192.0.2.1",
                                "198.51.100.7",
                                "203.0.113.9",
                                "198.18.0.1",
                                "192.0.0.8",
                                "::",
                                "::1",
                                "::ffff:127.0.0.1",
                                "::ffff:10.0.0.1",
                                "::ffff:169.254.169.254",
                                "::127.0.0.1",
                                "64:ff9b::7f00:1",
                                "64:ff9b::a9fe:a9fe",
                                "2002:7f00:1::",
                                "2002:a00:1::",
                                "fe80::1",
                                "fec0::1",
                                "fc00::1",
                                "fd12:3456::1",
                                "ff02::1",
                                "2001:db8::1",
                                "2001::1",
                                "100::1"})
    {
        INFO(blocked);
        CHECK_FALSE(reachable(Reach::Public, none, address(blocked)));
    }
    for (const char* open : {"8.8.8.8", "1.1.1.1", "93.184.216.34", "172.32.0.1", "100.128.0.1", "::ffff:8.8.8.8",
                             "2606:4700:4700::1111", "2a00:1450::1", "64:ff9b::808:808", "2002:808:808::"})
    {
        INFO(open);
        CHECK(reachable(Reach::Public, none, address(open)));
    }
    CHECK(reachable(Reach::Loopback, none, address("127.0.0.1")));
    CHECK(reachable(Reach::Loopback, none, address("127.0.0.53")));
    CHECK(reachable(Reach::Loopback, none, address("::1")));
    CHECK(reachable(Reach::Loopback, none, address("::ffff:127.0.0.1")));
    CHECK_FALSE(reachable(Reach::Loopback, none, address("10.0.0.1")));
    CHECK_FALSE(reachable(Reach::Loopback, none, address("8.8.8.8")));
    CHECK(reachable(Reach::Private, none, address("10.0.0.1")));
    CHECK(reachable(Reach::Private, none, address("127.0.0.1")));
    CHECK(reachable(Reach::Private, none, address("8.8.8.8")));
    CHECK_FALSE(reachable(Reach::Private, none, address("0.0.0.0")));
    CHECK_FALSE(reachable(Reach::Private, none, address("::")));
    CHECK_FALSE(reachable(Reach::Private, none, address("224.0.0.1")));
    CHECK(reachable(Reach::Exactly, address("127.0.0.1"), address("127.0.0.1")));
    CHECK_FALSE(reachable(Reach::Exactly, address("127.0.0.1"), address("127.0.0.2")));
    CHECK_FALSE(reachable(Reach::Exactly, address("127.0.0.1"), address("::ffff:127.0.0.1")));
}

TEST_CASE("a request with no context, or a null request, needs nothing and reaches nothing", "[web]")
{
    const std::string url = "https://example.com/";
    void* request = fminus_web_http_get(nullptr, url.data(), static_cast<std::int32_t>(url.size()), nullptr, 0);
    REQUIRE(request != nullptr);
    CHECK(fminus_web_http_status(request) == FMINUS_WEB_REFUSED);
    CHECK(fminus_web_http_wait(request, -1) == FMINUS_WEB_REFUSED);
    CHECK(fminus_web_http_size(request) == 0);
    CHECK(fminus_web_http_body(request) == nullptr);
    fminus_web_http_cancel(request);
    fminus_web_http_close(request);
    fminus_web_http_close(request);   // the shared refused request: closing it is harmless, twice too
    CHECK(fminus_web_http_status(request) == FMINUS_WEB_REFUSED);
    void* posted = fminus_web_http_post(nullptr, url.data(), static_cast<std::int32_t>(url.size()), nullptr, 0, "x", 1,
                                        "text/plain", 10);
    CHECK(fminus_web_http_status(posted) == FMINUS_WEB_REFUSED);

    CHECK(fminus_web_http_status(nullptr) == FMINUS_WEB_INVALID);
    CHECK(fminus_web_http_wait(nullptr, 10) == FMINUS_WEB_INVALID);
    CHECK(fminus_web_http_size(nullptr) == 0);
    CHECK(fminus_web_http_body(nullptr) == nullptr);
    CHECK(fminus_web_http_progress(nullptr) == 0);
    fminus_web_http_cancel(nullptr);
    fminus_web_http_close(nullptr);
}

TEST_CASE("a refused request is refused before anything is sent, and starts no I/O thread", "[web]")
{
    REQUIRE_FALSE(fminus::web::ioThreadRunning());
    auto context = makeContext(allowing({"https://example.com/allowed/*"}));
    void* request = get(*context, "https://example.com/not-allowed");
    REQUIRE(request != nullptr);
    CHECK(fminus_web_http_status(request) == FMINUS_WEB_REFUSED);
    void* invalid = get(*context, "not a url");
    CHECK(fminus_web_http_status(invalid) == FMINUS_WEB_REFUSED);   // a space
    void* unparsed = get(*context, "https:/");
    CHECK(fminus_web_http_status(unparsed) == FMINUS_WEB_INVALID);
    CHECK_FALSE(fminus::web::ioThreadRunning());
    fminus_web_http_close(request);
    // `invalid` and `unparsed` are freed with the context.
}

#if !defined(_WIN32)

// ---- a loopback server ----------------------------------------------------------------------------------------------

namespace
{
    // A certificate authority and a server's certificate, made by the test with Mbed TLS (its PSA keys: ECDSA P-256).
    struct Pki
    {
        std::string caPem;
        std::string certPem;
        std::string keyPem;
    };

    struct Key
    {
        mbedtls_pk_context pk;
        Key()
        {
            mbedtls_pk_init(&pk);
            psa_key_attributes_t attributes = PSA_KEY_ATTRIBUTES_INIT;
            psa_set_key_type(&attributes, PSA_KEY_TYPE_ECC_KEY_PAIR(PSA_ECC_FAMILY_SECP_R1));
            psa_set_key_bits(&attributes, 256);
            psa_set_key_usage_flags(&attributes,
                                    PSA_KEY_USAGE_SIGN_HASH | PSA_KEY_USAGE_SIGN_MESSAGE | PSA_KEY_USAGE_EXPORT);
            psa_set_key_algorithm(&attributes, PSA_ALG_ECDSA(PSA_ALG_SHA_256));
            mbedtls_svc_key_id_t id = MBEDTLS_SVC_KEY_ID_INIT;
            REQUIRE(psa_generate_key(&attributes, &id) == PSA_SUCCESS);
            REQUIRE(mbedtls_pk_copy_from_psa(id, &pk) == 0);
            psa_destroy_key(id);
        }
        ~Key()
        {
            mbedtls_pk_free(&pk);
        }
    };

    std::string certificate(Key& subject, Key& issuer, const char* subjectName, const char* issuerName, bool ca,
                            const std::vector<std::string>& dnsNames, bool loopbackIp, unsigned char serial)
    {
        mbedtls_x509write_cert crt;
        mbedtls_x509write_crt_init(&crt);
        mbedtls_x509write_crt_set_version(&crt, MBEDTLS_X509_CRT_VERSION_3);
        const unsigned char serialBytes[] = {0x01, serial};
        REQUIRE(mbedtls_x509write_crt_set_serial_raw(&crt, const_cast<unsigned char*>(serialBytes), 2) == 0);
        REQUIRE(mbedtls_x509write_crt_set_validity(&crt, "20250101000000", "20450101000000") == 0);
        REQUIRE(mbedtls_x509write_crt_set_subject_name(&crt, subjectName) == 0);
        REQUIRE(mbedtls_x509write_crt_set_issuer_name(&crt, issuerName) == 0);
        mbedtls_x509write_crt_set_subject_key(&crt, &subject.pk);
        mbedtls_x509write_crt_set_issuer_key(&crt, &issuer.pk);
        mbedtls_x509write_crt_set_md_alg(&crt, MBEDTLS_MD_SHA256);
        REQUIRE(mbedtls_x509write_crt_set_basic_constraints(&crt, ca ? 1 : 0, -1) == 0);
        REQUIRE(mbedtls_x509write_crt_set_subject_key_identifier(&crt) == 0);
        REQUIRE(mbedtls_x509write_crt_set_authority_key_identifier(&crt) == 0);
        REQUIRE(mbedtls_x509write_crt_set_key_usage(&crt, ca ? MBEDTLS_X509_KU_KEY_CERT_SIGN | MBEDTLS_X509_KU_CRL_SIGN
                                                             : MBEDTLS_X509_KU_DIGITAL_SIGNATURE) == 0);
        std::vector<mbedtls_x509_san_list> sans(dnsNames.size() + (loopbackIp ? 1 : 0));
        static const unsigned char kLoopback[4] = {127, 0, 0, 1};
        for (std::size_t i = 0; i < sans.size(); ++i)
        {
            std::memset(&sans[i], 0, sizeof sans[i]);
            if (i < dnsNames.size())
            {
                sans[i].node.type = MBEDTLS_X509_SAN_DNS_NAME;
                sans[i].node.san.unstructured_name.p =
                    reinterpret_cast<unsigned char*>(const_cast<char*>(dnsNames[i].data()));
                sans[i].node.san.unstructured_name.len = dnsNames[i].size();
            }
            else
            {
                sans[i].node.type = MBEDTLS_X509_SAN_IP_ADDRESS;
                sans[i].node.san.unstructured_name.p = const_cast<unsigned char*>(kLoopback);
                sans[i].node.san.unstructured_name.len = 4;
            }
            sans[i].next = i + 1 < sans.size() ? &sans[i + 1] : nullptr;
        }
        if (!sans.empty())
            REQUIRE(mbedtls_x509write_crt_set_subject_alternative_name(&crt, sans.data()) == 0);
        std::vector<unsigned char> pem(8192);
        REQUIRE(mbedtls_x509write_crt_pem(&crt, pem.data(), pem.size()) == 0);
        mbedtls_x509write_crt_free(&crt);
        return std::string(reinterpret_cast<const char*>(pem.data()));
    }

    std::string keyPem(Key& key)
    {
        std::vector<unsigned char> pem(4096);
        REQUIRE(mbedtls_pk_write_key_pem(&key.pk, pem.data(), pem.size()) == 0);
        return std::string(reinterpret_cast<const char*>(pem.data()));
    }

    // A CA, and a server certificate it signs for `names` (and 127.0.0.1).
    Pki makePki(const std::vector<std::string>& names, const char* caName = "CN=FMinus Test CA,O=FMinus")
    {
        REQUIRE(psa_crypto_init() == PSA_SUCCESS);
        Key ca;
        Key server;
        Pki pki;
        pki.caPem = certificate(ca, ca, caName, caName, true, {}, false, 1);
        pki.certPem = certificate(server, ca, "CN=FMinus Test Server", caName, false, names, true, 2);
        pki.keyPem = keyPem(server);
        return pki;
    }

    // A file the test writes, removed when it goes.
    struct TempFile
    {
        std::filesystem::path path;
        explicit TempFile(const std::string& contents)
        {
            static std::atomic<int> counter{0};
            path = std::filesystem::temp_directory_path() /
                   ("fminus-web-" + std::to_string(::getpid()) + "-" + std::to_string(counter++) + ".pem");
            std::FILE* file = std::fopen(path.c_str(), "wb");
            REQUIRE(file != nullptr);
            std::fwrite(contents.data(), 1, contents.size(), file);
            std::fclose(file);
        }
        ~TempFile()
        {
            std::error_code ignored;
            std::filesystem::remove(path, ignored);
        }
    };

    // An HTTP/1.1 server on 127.0.0.1 (or another loopback address), an ephemeral port, a thread a connection; each
    // response closes its connection. With a Pki, it serves TLS.
    //
    //   /ok                 200 "hello"
    //   /status/<n>         <n> "status <n>"
    //   /slow               nothing until the client goes or the server stops, then 200
    //   /redirect/<n>?<url> <n> with Location: <url> (relative or absolute, as written)
    //   /loop               302 with Location: /loop
    //   /echo               200 "<method> <content-type> <body>"
    //   /big?<n>            200, Content-Length <n>, <n> bytes ('a'...)
    //   /chunked?<n>        200, chunked, <n> bytes
    //   /drop               the connection closed, no response
    //   /partial            Content-Length 100, 10 bytes, closed
    //   /endless            200, chunked, a chunk every 5 ms, for ever
    //   /pack/<n>.json      200, {"gain": 0.5, "index": <n>, "table": [<n>, <n> + 1, 0.25]}
    class Server
    {
    public:
        explicit Server(const Pki* pki = nullptr, const char* host = "127.0.0.1")
            : host_(host)
        {
            listener_ = ::socket(AF_INET, SOCK_STREAM, 0);
            REQUIRE(listener_ >= 0);
            const int one = 1;
            ::setsockopt(listener_, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one);
            sockaddr_in where{};
            where.sin_family = AF_INET;
            where.sin_port = 0;
            REQUIRE(::inet_pton(AF_INET, host, &where.sin_addr) == 1);
            REQUIRE(::bind(listener_, reinterpret_cast<sockaddr*>(&where), sizeof where) == 0);
            REQUIRE(::listen(listener_, 128) == 0);
            socklen_t length = sizeof where;
            REQUIRE(::getsockname(listener_, reinterpret_cast<sockaddr*>(&where), &length) == 0);
            port_ = ntohs(where.sin_port);
            if (pki != nullptr)
            {
                tls_ = true;
                mbedtls_x509_crt_init(&chain_);
                mbedtls_pk_init(&key_);
                mbedtls_ssl_config_init(&config_);
                REQUIRE(mbedtls_x509_crt_parse(&chain_, reinterpret_cast<const unsigned char*>(pki->certPem.c_str()),
                                               pki->certPem.size() + 1) == 0);
                REQUIRE(mbedtls_pk_parse_key(&key_, reinterpret_cast<const unsigned char*>(pki->keyPem.c_str()),
                                             pki->keyPem.size() + 1, nullptr, 0) == 0);
                REQUIRE(mbedtls_ssl_config_defaults(&config_, MBEDTLS_SSL_IS_SERVER, MBEDTLS_SSL_TRANSPORT_STREAM,
                                                    MBEDTLS_SSL_PRESET_DEFAULT) == 0);
                REQUIRE(mbedtls_ssl_conf_own_cert(&config_, &chain_, &key_) == 0);
            }
            acceptor_ = std::thread([this] { acceptLoop(); });
        }

        ~Server()
        {
            stop_.store(true);
            acceptor_.join();
            ::close(listener_);
            std::vector<std::thread> connections;
            {
                std::lock_guard lock(mutex_);
                connections.swap(connections_);
            }
            for (std::thread& connection : connections)
                connection.join();
            if (tls_)
            {
                mbedtls_ssl_config_free(&config_);
                mbedtls_pk_free(&key_);
                mbedtls_x509_crt_free(&chain_);
            }
        }

        int port() const
        {
            return port_;
        }

        std::string url(std::string_view path) const
        {
            return std::string(tls_ ? "https://" : "http://") + host_ + ":" + std::to_string(port_) + std::string(path);
        }

        // How many requests for `path` (without its query) arrived.
        int hits(const std::string& path) const
        {
            std::lock_guard lock(mutex_);
            const auto found = hits_.find(path);
            return found == hits_.end() ? 0 : found->second;
        }

        int connections() const
        {
            return accepted_.load();
        }

        // The bytes of bodies sent, over every connection.
        std::int64_t sent() const
        {
            return sent_.load();
        }

        // Connections still being served.
        int open() const
        {
            return open_.load();
        }

    private:
        struct Connection
        {
            Server* server;
            int fd;
            bool tls = false;
            mbedtls_ssl_context ssl;

            // Bytes in, or 0 when the client is gone, the server stops, or the read times out at `deadline`.
            int read(char* data, int size)
            {
                for (;;)
                {
                    if (server->stop_.load())
                        return 0;
                    if (tls)
                    {
                        const int got = mbedtls_ssl_read(&ssl, reinterpret_cast<unsigned char*>(data),
                                                         static_cast<std::size_t>(size));
                        if (got == MBEDTLS_ERR_SSL_WANT_READ || got == MBEDTLS_ERR_SSL_WANT_WRITE ||
                            got == MBEDTLS_ERR_SSL_RECEIVED_NEW_SESSION_TICKET)
                            continue;
                        return got > 0 ? got : 0;
                    }
                    pollfd one{fd, POLLIN, 0};
                    if (::poll(&one, 1, 20) <= 0)
                        continue;
                    const ssize_t got = ::recv(fd, data, static_cast<std::size_t>(size), 0);
                    return got > 0 ? static_cast<int>(got) : 0;
                }
            }

            bool write(std::string_view data)
            {
                while (!data.empty())
                {
                    if (server->stop_.load())
                        return false;
                    if (tls)
                    {
                        const int put =
                            mbedtls_ssl_write(&ssl, reinterpret_cast<const unsigned char*>(data.data()), data.size());
                        if (put == MBEDTLS_ERR_SSL_WANT_READ || put == MBEDTLS_ERR_SSL_WANT_WRITE)
                            continue;
                        if (put <= 0)
                            return false;
                        data.remove_prefix(static_cast<std::size_t>(put));
                        continue;
                    }
                    pollfd one{fd, POLLOUT, 0};
                    if (::poll(&one, 1, 20) <= 0)
                        continue;
                    const ssize_t put = ::send(fd, data.data(), data.size(), MSG_NOSIGNAL);
                    if (put <= 0)
                        return false;
                    data.remove_prefix(static_cast<std::size_t>(put));
                }
                return true;
            }

            // Whether the client has closed its end (or the server stops).
            bool gone()
            {
                if (server->stop_.load())
                    return true;
                pollfd one{fd, POLLIN, 0};
                if (::poll(&one, 1, 10) <= 0)
                    return false;
                char byte;
                return ::recv(fd, &byte, 1, MSG_PEEK | MSG_DONTWAIT) <= 0;
            }
        };

        static int sendRaw(void* context, const unsigned char* data, std::size_t size)
        {
            auto* connection = static_cast<Connection*>(context);
            pollfd one{connection->fd, POLLOUT, 0};
            if (::poll(&one, 1, 20) <= 0)
                return MBEDTLS_ERR_SSL_WANT_WRITE;
            const ssize_t put = ::send(connection->fd, data, size, MSG_NOSIGNAL);
            return put < 0 ? MBEDTLS_ERR_SSL_CONN_EOF : static_cast<int>(put);
        }

        static int receiveRaw(void* context, unsigned char* data, std::size_t size)
        {
            auto* connection = static_cast<Connection*>(context);
            pollfd one{connection->fd, POLLIN, 0};
            if (::poll(&one, 1, 20) <= 0)
                return MBEDTLS_ERR_SSL_WANT_READ;
            const ssize_t got = ::recv(connection->fd, data, size, 0);
            return got <= 0 ? MBEDTLS_ERR_SSL_CONN_EOF : static_cast<int>(got);
        }

        void acceptLoop()
        {
            while (!stop_.load())
            {
                pollfd one{listener_, POLLIN, 0};
                if (::poll(&one, 1, 20) <= 0)
                    continue;
                const int fd = ::accept(listener_, nullptr, nullptr);
                if (fd < 0)
                    continue;
                ++accepted_;
                ++open_;
                std::lock_guard lock(mutex_);
                connections_.emplace_back(
                    [this, fd]
                    {
                        serve(fd);
                        ::close(fd);
                        --open_;
                    });
            }
        }

        void serve(int fd)
        {
            Connection connection{this, fd, false, {}};
            if (tls_)
            {
                connection.tls = true;
                mbedtls_ssl_init(&connection.ssl);
                if (mbedtls_ssl_setup(&connection.ssl, &config_) != 0)
                {
                    mbedtls_ssl_free(&connection.ssl);
                    return;
                }
                mbedtls_ssl_set_bio(&connection.ssl, &connection, &sendRaw, &receiveRaw, nullptr);
                int shaken = 0;
                while ((shaken = mbedtls_ssl_handshake(&connection.ssl)) != 0)
                {
                    if ((shaken != MBEDTLS_ERR_SSL_WANT_READ && shaken != MBEDTLS_ERR_SSL_WANT_WRITE) || stop_.load())
                    {
                        mbedtls_ssl_free(&connection.ssl);
                        return;
                    }
                }
            }
            respond(connection);
            if (tls_)
            {
                mbedtls_ssl_close_notify(&connection.ssl);
                mbedtls_ssl_free(&connection.ssl);
            }
        }

        void respond(Connection& connection)
        {
            // The request: its head, and a body of Content-Length bytes.
            std::string request;
            char buffer[4096];
            std::size_t headEnd = std::string::npos;
            while ((headEnd = request.find("\r\n\r\n")) == std::string::npos)
            {
                const int got = connection.read(buffer, sizeof buffer);
                if (got == 0)
                    return;
                request.append(buffer, static_cast<std::size_t>(got));
            }
            const std::string head = request.substr(0, headEnd);
            std::string content = request.substr(headEnd + 4);
            const std::string method = head.substr(0, head.find(' '));
            const std::size_t targetStart = head.find(' ') + 1;
            const std::string target = head.substr(targetStart, head.find(' ', targetStart) - targetStart);
            const std::string path = target.substr(0, target.find('?'));
            const std::string query = target.find('?') == std::string::npos ? "" : target.substr(target.find('?') + 1);
            auto header = [&](const std::string& name)
            {
                std::string lower = head;
                for (char& c : lower)
                    c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
                const std::size_t at = lower.find("\r\n" + name + ":");
                if (at == std::string::npos)
                    return std::string();
                std::size_t start = at + 2 + name.size() + 1;
                while (start < head.size() && head[start] == ' ')
                    ++start;
                return head.substr(start, head.find("\r\n", start) - start);
            };
            const std::string length = header("content-length");
            const std::size_t contentLength = length.empty() ? 0 : std::stoul(length);
            while (content.size() < contentLength)
            {
                const int got = connection.read(buffer, sizeof buffer);
                if (got == 0)
                    return;
                content.append(buffer, static_cast<std::size_t>(got));
            }
            {
                std::lock_guard lock(mutex_);
                ++hits_[path];
            }

            auto reply = [&](int status, const std::string& extra, const std::string& bodyText)
            {
                connection.write("HTTP/1.1 " + std::to_string(status) +
                                 " X\r\nContent-Length: " + std::to_string(bodyText.size()) +
                                 "\r\nConnection: close\r\n" + extra + "\r\n" + bodyText);
                sent_ += static_cast<std::int64_t>(bodyText.size());
            };
            static const std::string kChunk(65536, 'a');
            if (path == "/ok")
                reply(200, "", "hello");
            else if (path.starts_with("/status/"))
                reply(std::stoi(path.substr(8)), "", "status " + path.substr(8));
            else if (path == "/slow")
            {
                while (!connection.gone())
                    std::this_thread::sleep_for(5ms);
            }
            else if (path == "/loop")
                reply(302, "Location: /loop\r\n", "");
            else if (path.starts_with("/redirect/"))
                reply(std::stoi(path.substr(10)), "Location: " + query + "\r\n", "");
            else if (path == "/echo")
                reply(200, "", method + " " + header("content-type") + " " + content);
            else if (path == "/big")
            {
                std::int64_t left = std::stoll(query);
                if (!connection.write("HTTP/1.1 200 OK\r\nContent-Length: " + std::to_string(left) +
                                      "\r\nConnection: close\r\n\r\n"))
                    return;
                while (left > 0)
                {
                    const auto part = static_cast<std::size_t>(std::min<std::int64_t>(left, 65536));
                    if (!connection.write(std::string_view(kChunk).substr(0, part)))
                        return;
                    sent_ += static_cast<std::int64_t>(part);
                    left -= static_cast<std::int64_t>(part);
                }
            }
            else if (path == "/chunked" || path == "/endless")
            {
                const bool endless = path == "/endless";
                std::int64_t left = endless ? 0 : std::stoll(query);
                if (!connection.write("HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked\r\nConnection: close\r\n\r\n"))
                    return;
                while (endless || left > 0)
                {
                    const auto part =
                        endless ? std::size_t(16) : static_cast<std::size_t>(std::min<std::int64_t>(left, 16384));
                    char size[32];
                    std::snprintf(size, sizeof size, "%zx\r\n", part);
                    if (!connection.write(size) || !connection.write(std::string_view(kChunk).substr(0, part)) ||
                        !connection.write("\r\n"))
                        return;
                    sent_ += static_cast<std::int64_t>(part);
                    left -= static_cast<std::int64_t>(part);
                    if (endless)
                        std::this_thread::sleep_for(5ms);
                }
                connection.write("0\r\n\r\n");
            }
            else if (path.starts_with("/pack/") && path.ends_with(".json"))
            {
                const int n = std::stoi(path.substr(6));
                reply(200, "Content-Type: application/json\r\n",
                      "{\"gain\": 0.5, \"index\": " + std::to_string(n) + ", \"table\": [" + std::to_string(n) + ", " +
                          std::to_string(n + 1) + ", 0.25]}");
            }
            else if (path == "/drop")
                return;
            else if (path == "/partial")
                connection.write("HTTP/1.1 200 OK\r\nContent-Length: 100\r\nConnection: close\r\n\r\n0123456789");
            else
                reply(404, "", "no such path");
        }

        std::string host_;
        int listener_ = -1;
        int port_ = 0;
        bool tls_ = false;
        mbedtls_x509_crt chain_;
        mbedtls_pk_context key_;
        mbedtls_ssl_config config_;
        std::atomic<bool> stop_{false};
        std::thread acceptor_;
        mutable std::mutex mutex_;
        std::vector<std::thread> connections_;
        std::map<std::string, int> hits_;
        std::atomic<int> accepted_{0};
        std::atomic<int> open_{0};
        std::atomic<std::int64_t> sent_{0};
    };

    // A policy for the loopback server's address.
    Policy loopback(std::string scheme = "http")
    {
        Policy policy = allowing({scheme + "://127.0.0.1:*"});
        policy.timeoutMs = 10000;
        return policy;
    }

    // A port nothing listens on: one bound, then closed.
    int closedPort()
    {
        const int fd = ::socket(AF_INET, SOCK_STREAM, 0);
        sockaddr_in where{};
        where.sin_family = AF_INET;
        ::inet_pton(AF_INET, "127.0.0.1", &where.sin_addr);
        ::bind(fd, reinterpret_cast<sockaddr*>(&where), sizeof where);
        socklen_t length = sizeof where;
        ::getsockname(fd, reinterpret_cast<sockaddr*>(&where), &length);
        ::close(fd);
        return ntohs(where.sin_port);
    }

    template <typename Predicate>
    bool eventually(Predicate predicate, std::chrono::milliseconds limit = 5000ms)
    {
        const auto end = std::chrono::steady_clock::now() + limit;
        while (std::chrono::steady_clock::now() < end)
        {
            if (predicate())
                return true;
            std::this_thread::sleep_for(2ms);
        }
        return predicate();
    }
} // namespace

// ---- requests, against the loopback server --------------------------------------------------------------------------

TEST_CASE("a GET: its status, size, body and progress once done", "[web][http]")
{
    Server server;
    auto context = makeContext(loopback());
    void* request = get(*context, server.url("/ok"));
    REQUIRE(request != nullptr);
    CHECK(fminus_web_http_wait(request, -1) == 200);
    CHECK(fminus_web_http_status(request) == 200);
    CHECK(fminus_web_http_size(request) == 5);
    CHECK(fminus_web_http_progress(request) == 5);
    CHECK(body(request) == "hello");
    CHECK(static_cast<const char*>(fminus_web_http_body(request))[5] == '\0');
    CHECK(fminus::web::ioThreadRunning());
    // A done request: cancel leaves it as it is, and the body has not moved.
    const void* before = fminus_web_http_body(request);
    fminus_web_http_cancel(request);
    CHECK(fminus_web_http_status(request) == 200);
    CHECK(fminus_web_http_body(request) == before);
    fminus_web_http_close(request);

    void* missing = get(*context, server.url("/status/404"));
    CHECK(fminus_web_http_wait(missing, -1) == 404);
    CHECK(body(missing) == "status 404");
    void* empty = get(*context, server.url("/status/204"));
    CHECK(fminus_web_http_wait(empty, -1) == 204);
    CHECK(fminus_web_http_size(empty) == 0);
    REQUIRE(fminus_web_http_body(empty) != nullptr);   // an empty body is an empty string, not null
    CHECK(static_cast<const char*>(fminus_web_http_body(empty))[0] == '\0');
    // %d in the URL, as a program's literal has them.
    const std::string format = "http://127.0.0.1:%d/status/%d";
    const std::int32_t ints[] = {server.port(), 418};
    void* formatted =
        fminus_web_http_get(context->handle(), format.data(), static_cast<std::int32_t>(format.size()), ints, 2);
    CHECK(fminus_web_http_wait(formatted, -1) == 418);
    CHECK(server.hits("/status/418") == 1);
}

TEST_CASE("a wait times out with the request still pending; the policy's timeout ends it", "[web][http]")
{
    Server server;
    Policy policy = loopback();
    policy.timeoutMs = 300;
    auto context = makeContext(policy);
    void* request = get(*context, server.url("/slow"));
    const auto start = std::chrono::steady_clock::now();
    CHECK(fminus_web_http_wait(request, 20) == FMINUS_WEB_PENDING);
    CHECK(fminus_web_http_wait(request, 0) == FMINUS_WEB_PENDING);
    CHECK(fminus_web_http_status(request) == FMINUS_WEB_PENDING);
    CHECK(fminus_web_http_body(request) == nullptr);
    CHECK(fminus_web_http_size(request) == 0);
    CHECK(fminus_web_http_wait(request, -1) == FMINUS_WEB_TIMEOUT);
    const auto took = std::chrono::steady_clock::now() - start;
    CHECK(took >= 250ms);
    CHECK(took < 3s);
    CHECK(fminus_web_http_body(request) == nullptr);

    // A response that never ends is ended by the timeout too, with a body limit far above what arrives.
    void* endless = get(*context, server.url("/endless"));
    CHECK(fminus_web_http_wait(endless, -1) == FMINUS_WEB_TIMEOUT);
    CHECK(fminus_web_http_size(endless) == 0);
    CHECK(fminus_web_http_progress(endless) == 0);
}

TEST_CASE("cancel ends a pending request at once, and close frees one still pending", "[web][http]")
{
    Server server;
    auto context = makeContext(loopback());
    void* request = get(*context, server.url("/slow"));
    REQUIRE(eventually([&] { return server.hits("/slow") == 1; }));
    fminus_web_http_cancel(request);
    CHECK(fminus_web_http_status(request) == FMINUS_WEB_CANCELLED);
    CHECK(fminus_web_http_wait(request, -1) == FMINUS_WEB_CANCELLED);
    fminus_web_http_cancel(request);
    CHECK(fminus_web_http_status(request) == FMINUS_WEB_CANCELLED);
    fminus_web_http_close(request);
    CHECK(eventually([&] { return server.open() == 0; }));   // the connection is gone

    void* closed = get(*context, server.url("/slow"));
    REQUIRE(eventually([&] { return server.hits("/slow") == 2; }));
    fminus_web_http_close(closed);
    CHECK(eventually([&] { return server.open() == 0; }));
}

TEST_CASE("redirects are followed one hop at a time, each checked by the policy", "[web][http]")
{
    Server server;
    Server forbidden(nullptr, "127.0.0.2");
    Policy policy = loopback();
    auto context = makeContext(policy);

    SECTION("to an allowed target, relative and absolute")
    {
        void* relative = get(*context, server.url("/redirect/302?/ok"));
        CHECK(fminus_web_http_wait(relative, -1) == 200);
        CHECK(body(relative) == "hello");
        void* absolute = get(*context, server.url("/redirect/301?" + server.url("/status/201")));
        CHECK(fminus_web_http_wait(absolute, -1) == 201);
        void* twice = get(*context, server.url("/redirect/307?/redirect/308?/ok"));
        CHECK(fminus_web_http_wait(twice, -1) == 200);
        CHECK(server.hits("/ok") == 2);
    }
    SECTION("to a forbidden target: refused, and the target never reached")
    {
        for (const std::string& target : {forbidden.url("/ok"), std::string("http://169.254.169.254/latest/meta-data/"),
                                          std::string("http://localhost:") + std::to_string(server.port()) + "/ok",
                                          std::string("https://127.0.0.1:") + std::to_string(server.port()) + "/ok",
                                          std::string("ftp://127.0.0.1/ok"), std::string("file:///etc/passwd")})
        {
            INFO(target);
            void* request = get(*context, server.url("/redirect/302?" + target));
            CHECK(fminus_web_http_wait(request, -1) == FMINUS_WEB_REFUSED);
            CHECK(fminus_web_http_body(request) == nullptr);
        }
        CHECK(forbidden.connections() == 0);
        CHECK(server.hits("/ok") == 0);
        // Credentials in a redirect's Location: libcurl's redirect URL leaves them out, so none are sent.
        void* withCredentials = get(
            *context, server.url("/redirect/302?http://user:pw@127.0.0.1:" + std::to_string(server.port()) + "/echo"));
        CHECK(fminus_web_http_wait(withCredentials, -1) == 200);
    }
    SECTION("a loop ends at the policy's limit; with no redirects allowed, a redirect is over it")
    {
        void* loop = get(*context, server.url("/loop"));
        CHECK(fminus_web_http_wait(loop, -1) == FMINUS_WEB_LIMIT);
        CHECK(server.hits("/loop") == policy.maxRedirects + 1);
        Policy none = loopback();
        none.maxRedirects = 0;
        auto strict = makeContext(none);
        void* request = get(*strict, server.url("/redirect/302?/ok"));
        CHECK(fminus_web_http_wait(request, -1) == FMINUS_WEB_LIMIT);
        CHECK(server.hits("/ok") == 0);
        void* noLocation = get(*context, server.url("/redirect/303"));
        CHECK(fminus_web_http_wait(noLocation, -1) == 303);   // an empty Location: no redirect to follow
    }
}

TEST_CASE("a POST: its body and content type; refused content types; redirects by 303 and 307", "[web][http]")
{
    Server server;
    Policy policy = loopback();
    policy.maxBodyBytes = 1 << 20;
    auto context = makeContext(policy);
    void* request = post(*context, server.url("/echo"), "{\"level\":0.5}", "application/json");
    CHECK(fminus_web_http_wait(request, -1) == 200);
    CHECK(body(request) == "POST application/json {\"level\":0.5}");
    void* untyped = post(*context, server.url("/echo"), "abc", "");
    CHECK(fminus_web_http_wait(untyped, -1) == 200);
    CHECK(body(untyped) == "POST application/octet-stream abc");
    void* empty = post(*context, server.url("/echo"), "", "text/plain");
    CHECK(fminus_web_http_wait(empty, -1) == 200);
    CHECK(body(empty) == "POST text/plain ");

    void* by303 = post(*context, server.url("/redirect/303?/echo"), "abc", "text/plain");
    CHECK(fminus_web_http_wait(by303, -1) == 200);
    CHECK(body(by303) == "GET  ");
    void* by307 = post(*context, server.url("/redirect/307?/echo"), "abc", "text/plain");
    CHECK(fminus_web_http_wait(by307, -1) == 200);
    CHECK(body(by307) == "POST text/plain abc");
    const int echoes = server.hits("/echo");

    for (const std::string_view type : {std::string_view("text/plain\r\nX-Evil: 1"), std::string_view("a\nb"),
                                        std::string_view("a\0b", 3), std::string_view("caf\xc3\xa9")})
    {
        void* refused = post(*context, server.url("/echo"), "abc", type);
        CHECK(fminus_web_http_status(refused) == FMINUS_WEB_REFUSED);
    }
    CHECK(fminus_web_http_status(post(*context, server.url("/echo"), "abc", std::string(256, 'a'))) ==
          FMINUS_WEB_REFUSED);
    CHECK(fminus_web_http_status(post(*context, server.url("/echo"), std::string((1 << 20) + 1, 'x'), "a/b")) ==
          FMINUS_WEB_LIMIT);
    const std::string url = server.url("/echo");
    CHECK(fminus_web_http_status(fminus_web_http_post(context->handle(), url.data(),
                                                      static_cast<std::int32_t>(url.size()), nullptr, 0, "x", -1, "a/b",
                                                      3)) == FMINUS_WEB_INVALID);
    CHECK(fminus_web_http_status(fminus_web_http_post(context->handle(), url.data(),
                                                      static_cast<std::int32_t>(url.size()), nullptr, 0, nullptr, 4,
                                                      "a/b", 3)) == FMINUS_WEB_INVALID);
    CHECK(server.hits("/echo") == echoes);
}

TEST_CASE("a body over the limit ends the request at the limit, not after downloading it", "[web][http]")
{
    Server server;
    Policy policy = loopback();
    policy.maxBodyBytes = 1 << 20;
    auto context = makeContext(policy);
    void* exact = get(*context, server.url("/big?" + std::to_string(1 << 20)));
    CHECK(fminus_web_http_wait(exact, -1) == 200);
    CHECK(fminus_web_http_size(exact) == 1 << 20);
    fminus_web_http_close(exact);
    const std::int64_t before = server.sent();

    // A Content-Length over the limit: refused from the header. Without one (chunked): stopped at the limit. The
    // server has 256 MiB to send each time; what it got out is what the socket buffers took.
    void* declared = get(*context, server.url("/big?" + std::to_string(256 << 20)));
    CHECK(fminus_web_http_wait(declared, -1) == FMINUS_WEB_LIMIT);
    CHECK(fminus_web_http_body(declared) == nullptr);
    void* chunked = get(*context, server.url("/chunked?" + std::to_string(256 << 20)));
    CHECK(fminus_web_http_wait(chunked, -1) == FMINUS_WEB_LIMIT);
    CHECK(fminus_web_http_size(chunked) == 0);
    REQUIRE(eventually([&] { return server.open() == 0; }));
    CHECK(server.sent() - before < 64 << 20);
}

TEST_CASE("a dropped connection, a short body and a closed port are transport failures", "[web][http]")
{
    Server server;
    auto context = makeContext(loopback());
    CHECK(fminus_web_http_wait(get(*context, server.url("/drop")), -1) == FMINUS_WEB_TRANSPORT);
    CHECK(fminus_web_http_wait(get(*context, server.url("/partial")), -1) == FMINUS_WEB_TRANSPORT);
    const std::string closed = "http://127.0.0.1:" + std::to_string(closedPort()) + "/ok";
    CHECK(fminus_web_http_wait(get(*context, closed), -1) == FMINUS_WEB_TRANSPORT);
    // Not HTTP at all: a TLS client against the plain server.
    Policy tls = loopback("https");
    tls.caFile = "/nonexistent/fminus-ca.pem";
    auto secure = makeContext(tls);
    const std::int32_t status = fminus_web_http_wait(get(*secure, server.url("/ok").replace(0, 4, "https")), -1);
    CHECK((status == FMINUS_WEB_TRANSPORT || status == FMINUS_WEB_CERTIFICATE));
}

TEST_CASE("a name the policy allows reaches only the addresses its pattern allows", "[web][http]")
{
    Server server;
    const std::string port = std::to_string(server.port());
    Policy policy = allowing({"http://public.fminus.test:*"});
    policy.resolve = {"public.fminus.test:" + port + ":127.0.0.1"};
    {
        auto context = makeContext(policy);
        void* request = get(*context, "http://public.fminus.test:" + port + "/ok");
        CHECK(fminus_web_http_wait(request, -1) == FMINUS_WEB_REFUSED);   // a public name, on loopback: refused
        CHECK(server.connections() == 0);
    }
    policy.privateAddresses = true;
    {
        auto context = makeContext(policy);
        void* request = get(*context, "http://public.fminus.test:" + port + "/ok");
        CHECK(fminus_web_http_wait(request, -1) == 200);
    }
    // Another context, without the entry, does not see it (each context has its own DNS entries). Without a lookup
    // that could leave the machine: a name under .localhost, which libcurl answers itself (127.0.0.1, where the server
    // is) when its own DNS entries have none, while the first context's entry sends it to 127.0.0.2, where nothing
    // listens.
    {
        Policy first = allowing({"http://split.localhost:*"});
        first.privateAddresses = true;
        first.resolve = {"split.localhost:" + port + ":127.0.0.2"};
        auto pinned = makeContext(first);
        void* there = get(*pinned, "http://split.localhost:" + port + "/ok");
        CHECK(fminus_web_http_wait(there, -1) == FMINUS_WEB_TRANSPORT);
        Policy other = allowing({"http://split.localhost:*"});
        other.privateAddresses = true;
        auto context = makeContext(other);
        void* request = get(*context, "http://split.localhost:" + port + "/ok");
        CHECK(fminus_web_http_wait(request, -1) == 200);
        // And the first context's entry is still its own.
        CHECK(fminus_web_http_wait(get(*pinned, "http://split.localhost:" + port + "/ok"), -1)
              == FMINUS_WEB_TRANSPORT);
    }
    // localhost: loopback only, which is where it is.
    {
        auto context = makeContext(allowing({"http://localhost:*"}));
        void* request = get(*context, "http://localhost:" + port + "/ok");
        CHECK(fminus_web_http_wait(request, -1) == 200);
    }
}

TEST_CASE("a context limits its requests in flight", "[web][http]")
{
    Server server;
    Policy policy = loopback();
    policy.maxRequests = 2;
    auto context = makeContext(policy);
    void* first = get(*context, server.url("/slow"));
    void* second = get(*context, server.url("/slow"));
    void* third = get(*context, server.url("/ok"));
    CHECK(fminus_web_http_status(third) == FMINUS_WEB_LIMIT);
    fminus_web_http_close(first);
    void* fourth = get(*context, server.url("/ok"));
    CHECK(fminus_web_http_wait(fourth, -1) == 200);
    CHECK(fminus_web_http_status(second) == FMINUS_WEB_PENDING);
}

TEST_CASE("destroying a context cancels what is in flight and frees every request", "[web][http][lifetime]")
{
    Server server;
    REQUIRE_FALSE(fminus::web::ioThreadRunning());
    {
        auto context = makeContext(loopback());
        for (int i = 0; i < 8; ++i)
            get(*context, server.url("/slow"));
        get(*context, server.url("/ok"));
        get(*context, server.url("/endless"));
        REQUIRE(eventually([&] { return server.hits("/slow") == 8; }));
        CHECK(fminus::web::ioThreadRunning());
        const auto start = std::chrono::steady_clock::now();
        context.reset();
        CHECK(std::chrono::steady_clock::now() - start < 2s);
    }
    CHECK_FALSE(fminus::web::ioThreadRunning());   // the last context that used it is gone: joined
    CHECK(eventually([&] { return server.open() == 0; }));

    // Two contexts: the thread stays while one of them is alive.
    auto first = makeContext(loopback());
    auto second = makeContext(loopback());
    CHECK(fminus_web_http_wait(get(*first, server.url("/ok")), -1) == 200);
    CHECK(fminus_web_http_wait(get(*second, server.url("/ok")), -1) == 200);
    first.reset();
    CHECK(fminus::web::ioThreadRunning());
    CHECK(fminus_web_http_wait(get(*second, server.url("/ok")), -1) == 200);
    second.reset();
    CHECK_FALSE(fminus::web::ioThreadRunning());
    // And a new context starts it again.
    auto third = makeContext(loopback());
    CHECK(fminus_web_http_wait(get(*third, server.url("/ok")), -1) == 200);
    CHECK(fminus::web::ioThreadRunning());
}

TEST_CASE("destroying a context while threads wait on its requests wakes them, then frees", "[web][http][lifetime]")
{
    Server server;
    for (int round = 0; round < 5; ++round)
    {
        auto context = makeContext(loopback());
        std::vector<void*> requests;
        for (int i = 0; i < 4; ++i)
            requests.push_back(get(*context, server.url("/slow")));
        std::atomic<int> cancelled{0};
        std::atomic<int> waiting{0};
        std::vector<std::thread> waiters;
        for (void* request : requests)
        {
            for (int k = 0; k < 2; ++k)
            {
                waiters.emplace_back(
                    [&, request]
                    {
                        ++waiting;
                        if (fminus_web_http_wait(request, -1) == FMINUS_WEB_CANCELLED)
                            ++cancelled;
                    });
            }
        }
        REQUIRE(eventually([&] { return waiting.load() == 8; }));
        std::this_thread::sleep_for(10ms);   // into the wait
        context.reset();
        for (std::thread& waiter : waiters)
            waiter.join();
        CHECK(cancelled.load() == 8);
    }
    CHECK_FALSE(fminus::web::ioThreadRunning());
}

TEST_CASE("many requests at once, from several threads and contexts", "[web][http]")
{
    Server server;
    Policy policy = loopback();
    policy.maxRequests = 1000;
    std::atomic<int> ok{0};
    std::vector<std::thread> threads;
    auto shared = makeContext(policy);
    for (int t = 0; t < 4; ++t)
    {
        threads.emplace_back(
            [&, t]
            {
                std::string error;
                auto own = Context::create(policy, error);
                Context& context = t % 2 == 0 ? *shared : *own;
                std::vector<void*> requests;
                for (int i = 0; i < 50; ++i)
                    requests.push_back(get(context, server.url("/status/" + std::to_string(250 + i))));   // no 204
                for (int i = 0; i < 50; ++i)
                {
                    if (fminus_web_http_wait(requests[static_cast<std::size_t>(i)], -1) == 250 + i &&
                        body(requests[static_cast<std::size_t>(i)]) == "status " + std::to_string(250 + i))
                        ++ok;
                    fminus_web_http_close(requests[static_cast<std::size_t>(i)]);
                }
            });
    }
    for (std::thread& thread : threads)
        thread.join();
    CHECK(ok.load() == 200);
}

TEST_CASE("close and cancel racing completion", "[web][http][lifetime]")
{
    Server server;
    Policy policy = loopback();
    policy.maxRequests = 1000;
    auto context = makeContext(policy);
    std::mt19937 random(12345);
    int done = 0;
    int cancelled = 0;
    for (int i = 0; i < 300; ++i)
    {
        void* request = get(*context, server.url("/ok"));
        if (random() % 3 != 0)
            std::this_thread::sleep_for(std::chrono::microseconds(random() % 2000));
        switch (random() % 3)
        {
        case 0:
            fminus_web_http_close(request);
            continue;
        case 1:
        {
            fminus_web_http_cancel(request);
            const std::int32_t status = fminus_web_http_status(request);
            CHECK((status == 200 || status == FMINUS_WEB_CANCELLED));
            if (status == 200)
            {
                ++done;
                CHECK(body(request) == "hello");
            }
            else
                ++cancelled;
            fminus_web_http_close(request);
            continue;
        }
        default:
        {
            // Another thread closes it while this one polls it to completion, which it may not see.
            std::thread closer(
                [request]
                {
                    fminus_web_http_wait(request, -1);
                    fminus_web_http_close(request);
                });
            closer.join();
            continue;
        }
        }
    }
    INFO(done << " done, " << cancelled << " cancelled");
    SUCCEED();
}

TEST_CASE("the audio thread's reads are wait-free: a poll loop sees the status arrive", "[web][http]")
{
    Server server;
    auto context = makeContext(loopback());
    void* request = get(*context, server.url("/big?100000"));
    // As a nonblocking process() would: poll, read progress, never wait.
    std::int32_t progress = 0;
    while (fminus_web_http_status(request) == FMINUS_WEB_PENDING)
    {
        const std::int32_t now = fminus_web_http_progress(request);
        CHECK(now >= progress);
        progress = now;
        // The body appears with the status, never before it (the status is read after the body here: it may have
        // arrived in between).
        if (fminus_web_http_body(request) != nullptr)
            CHECK(fminus_web_http_status(request) == 200);
        std::this_thread::yield();
    }
    CHECK(fminus_web_http_status(request) == 200);
    CHECK(fminus_web_http_size(request) == 100000);
    CHECK(fminus_web_http_progress(request) == 100000);
}

// ---- HTTPS, with a certificate authority the test makes -------------------------------------------------------------

TEST_CASE("HTTPS against the loopback server, with the test's own certificate authority", "[web][https]")
{
    // This test uses the library's Mbed TLS itself (its keys, its TLS server), whose global state libcurl frees when
    // the last context goes (curl_global_cleanup): a context held for the whole test keeps it.
    auto keeper = makeContext(Policy());
    const Pki pki = makePki({"tls.fminus.test", "localhost"});
    const Pki other = makePki({"tls.fminus.test"}, "CN=Another CA");
    TempFile ca(pki.caPem);
    TempFile otherCa(other.caPem);
    Server server(&pki);
    const std::string port = std::to_string(server.port());

    Policy policy = loopback("https");
    policy.caFile = ca.path.string();
    policy.privateAddresses = true;
    policy.allow.push_back("https://tls.fminus.test:*");
    policy.allow.push_back("https://wrong.fminus.test:*");
    policy.allow.push_back("http://127.0.0.1:*");
    policy.resolve = {"tls.fminus.test:" + port + ":127.0.0.1", "wrong.fminus.test:" + port + ":127.0.0.1"};
    auto context = makeContext(policy);

    SECTION("by address, and by name")
    {
        void* byAddress = get(*context, server.url("/ok"));
        CHECK(fminus_web_http_wait(byAddress, -1) == 200);
        CHECK(body(byAddress) == "hello");
        void* byName = get(*context, "https://tls.fminus.test:" + port + "/echo");
        CHECK(fminus_web_http_wait(byName, -1) == 200);
        void* posted = post(*context, "https://tls.fminus.test:" + port + "/echo", "secret", "text/plain");
        CHECK(fminus_web_http_wait(posted, -1) == 200);
        CHECK(body(posted) == "POST text/plain secret");
    }
    SECTION("a certificate for another name")
    {
        void* request = get(*context, "https://wrong.fminus.test:" + port + "/ok");
        CHECK(fminus_web_http_wait(request, -1) == FMINUS_WEB_CERTIFICATE);
        CHECK(server.hits("/ok") == 0);
    }
    SECTION("an authority the policy does not trust, or the system's roots")
    {
        Policy untrusted = policy;
        untrusted.caFile = otherCa.path.string();
        auto strict = makeContext(untrusted);
        CHECK(fminus_web_http_wait(get(*strict, server.url("/ok")), -1) == FMINUS_WEB_CERTIFICATE);
        Policy system = policy;
        system.caFile.clear();
        auto systemContext = makeContext(system);
        CHECK(fminus_web_http_wait(get(*systemContext, server.url("/ok")), -1) == FMINUS_WEB_CERTIFICATE);
        Policy missing = policy;
        missing.caFile = "/nonexistent/fminus-ca.pem";
        auto missingContext = makeContext(missing);
        CHECK(fminus_web_http_wait(get(*missingContext, server.url("/ok")), -1) == FMINUS_WEB_CERTIFICATE);
        CHECK(server.hits("/ok") == 0);
    }
    SECTION("a redirect from https to http needs a pattern for http")
    {
        Server plain;
        void* allowed = get(*context, server.url("/redirect/302?" + plain.url("/ok")));
        CHECK(fminus_web_http_wait(allowed, -1) == 200);
        Policy httpsOnly = policy;
        httpsOnly.allow = {"https://127.0.0.1:*"};
        auto strict = makeContext(httpsOnly);
        void* refused = get(*strict, server.url("/redirect/302?" + plain.url("/ok")));
        CHECK(fminus_web_http_wait(refused, -1) == FMINUS_WEB_REFUSED);
        CHECK(plain.hits("/ok") == 1);
    }
    SECTION("a body over the limit, and a timeout, over TLS")
    {
        Policy small = policy;
        small.maxBodyBytes = 1000;
        small.timeoutMs = 300;
        auto limited = makeContext(small);
        CHECK(fminus_web_http_wait(get(*limited, server.url("/chunked?100000")), -1) == FMINUS_WEB_LIMIT);
        CHECK(fminus_web_http_wait(get(*limited, server.url("/slow")), -1) == FMINUS_WEB_TIMEOUT);
    }
    SECTION("through the language: a program's https request, its roots the host's")
    {
        const char* source = "int fetch(int port)\n"
                             "{\n"
                             "    void* r = http_get(\"https://tls.fminus.test:%d/ok\", port);\n"
                             "    const int status = http_wait(r, -1);\n"
                             "    const int size = status == 200 ? http_size(r) : 0;\n"
                             "    http_close(r);\n"
                             "    return status * 100 + size;\n"
                             "}\n";
        fminus::Program::Options options;
        options.web.allow = {"https://tls.fminus.test:*/ok"};
        options.web.caFile = ca.path.string();
        options.web.privateAddresses = true;
        options.web.resolve = {"tls.fminus.test:" + port + ":127.0.0.1"};
        std::string errors;
        auto program = fminus::Program::compile(source, errors, options);
        INFO(errors);
        REQUIRE(program);
        auto* fetch = program->function<int(int)>("fetch", errors);
        REQUIRE(fetch);
        CHECK(fetch(server.port()) == 200 * 100 + 5);   // "hello"
        options.web.caFile = otherCa.path.string();
        auto untrusting = fminus::Program::compile(source, errors, options);
        REQUIRE(untrusting);
        auto* refused = untrusting->function<int(int)>("fetch", errors);
        REQUIRE(refused);
        CHECK(refused(server.port()) == FMINUS_WEB_CERTIFICATE * 100);
        CHECK(server.hits("/ok") == 1);
    }
}

// ---- the language: programs that fetch, through the JIT and as executables ----------------------------------------

namespace
{
    // A program that fetches pack <k> from the loopback server at <port>, parses it and reads its numbers: the gain
    // times 1000, plus the table's sum, plus the index times 100; or the status, when it is no 200.
    const char* const kFetch = "float fetch(int port, int k)\n"
                               "{\n"
                               "    void* r = http_get(\"http://127.0.0.1:%d/pack/%d.json\", port, k);\n"
                               "    const int status = http_wait(r, -1);\n"
                               "    if (status != 200)\n"
                               "    {\n"
                               "        http_close(r);\n"
                               "        return (float)status;\n"
                               "    }\n"
                               "    void* doc = json_parse(http_body(r), http_size(r));\n"
                               "    http_close(r);\n"
                               "    float table[4];\n"
                               "    const int n = json_floats(doc, \"/table\", table, 4);\n"
                               "    float sum = 0.0f;\n"
                               "    for (int i = 0; i < n; ++i)\n"
                               "        sum += table[i];\n"
                               "    const float result = 1000.0f * json_float(doc, \"/gain\", 0.0f) + sum\n"
                               "                         + 100.0f * (float)json_int(doc, \"/index\", -1);\n"
                               "    json_free(doc);\n"
                               "    return result;\n"
                               "}\n";

    std::unique_ptr<fminus::Program> compileProgram(const std::string& source, const fminus::Program::Options& options)
    {
        std::string errors;
        auto program = fminus::Program::compile(source, errors, options);
        INFO(errors);
        REQUIRE(program);
        return program;
    }
} // namespace

#if FMINUS_HAS_JSON
TEST_CASE("a program fetches from the loopback server through the JIT and reads the JSON it got", "[web][library]")
{
    Server server;
    fminus::Program::Options options;
    options.web.allow = {"http://127.0.0.1:" + std::to_string(server.port()) + "/pack/*"};
    auto program = compileProgram(kFetch, options);
    REQUIRE(program->webRequests().size() == 1);
    CHECK(program->webRequests()[0].pattern == "http://127.0.0.1:*/pack/*.json");
    std::string errors;
    auto* fetch = program->function<float(int, int)>("fetch", errors);
    REQUIRE(fetch);
    CHECK(fetch(server.port(), 3) == 500.0f + 3.0f + 4.0f + 0.25f + 300.0f);
    CHECK(fetch(server.port(), 7) == 500.0f + 7.0f + 8.0f + 0.25f + 700.0f);
    CHECK(server.hits("/pack/3.json") == 1);
}

TEST_CASE("an executable fetches from the loopback server with FMINUS_WEB_ALLOW, and nothing without it",
          "[web][aot]")
{
    Server server;
    const std::string port = std::to_string(server.port());
    const std::string source = std::string(kFetch) + "float main(float port, float k) { return fetch((int)port, "
                                                     "(int)k); }\n";
    fminus::Diagnostics diagnostics("fetch.fm");
    const auto program = fminus::frontend::parse(source, diagnostics);
    REQUIRE(program);
    llvm::LLVMContext llvmContext;
    auto machine = llvm::cantFail(fminus::hostTargetMachine());
    fminus::ir::Options lowering;
    lowering.polyLanes = fminus::polyLanes(*machine);
    lowering.dataLayout = machine->createDataLayout().getStringRepresentation();
    std::unique_ptr<llvm::Module> module = fminus::ir::lower(*program, llvmContext, diagnostics, lowering);
    INFO(diagnostics.format());
    REQUIRE(module);
    REQUIRE_FALSE(llvm::errorToBool(fminus::aot::addEntryPoint(*module)));
    fminus::prepareModule(*module, *machine);
    fminus::optimiseModule(*module, *machine, llvm::OptimizationLevel::O2);
    const std::filesystem::path exe = std::filesystem::temp_directory_path() /
                                      ("fminus-web-fetch-" + std::to_string(::getpid()));
    const std::filesystem::path out = exe.string() + ".out";
    llvm::Error built = fminus::aot::emitExecutable(*module, *machine, {.output = exe.string()});
    REQUIRE_FALSE(llvm::errorToBool(std::move(built)));
    const auto run = [&](const std::string& environment)
    {
        const std::string command = environment + " '" + exe.string() + "' " + port + " 2 > '" + out.string() + "' 2>&1";
        const int status = std::system(command.c_str());
        std::string text;
        if (std::FILE* file = std::fopen(out.c_str(), "rb"))
        {
            char buffer[512];
            std::size_t got = 0;
            while ((got = std::fread(buffer, 1, sizeof buffer, file)) > 0)
                text.append(buffer, got);
            std::fclose(file);
        }
        return std::pair{status, text};
    };
    CHECK(run("env FMINUS_WEB_ALLOW='http://127.0.0.1:" + port + "/pack/*'") == std::pair{0, std::string("705.25\n")});
    CHECK(run("env -u FMINUS_WEB_ALLOW") == std::pair{0, std::string("-4\n")});   // HTTP_REFUSED
    CHECK(run("env FMINUS_WEB_ALLOW='http://127.0.0.1:" + port + "/other/*'") == std::pair{0, std::string("-4\n")});
    const auto [status, text] = run("env FMINUS_WEB_ALLOW='http://127.0.0.1:" + port + "/pack/%2e'");
    CHECK(WEXITSTATUS(status) == 2);
    CHECK(text.find("FMINUS_WEB_ALLOW: pattern") != std::string::npos);
    CHECK(server.hits("/pack/2.json") == 1);
    std::error_code ignored;
    std::filesystem::remove(exe, ignored);
    std::filesystem::remove(out, ignored);
}
#endif

TEST_CASE("the policy refuses through the language: http_status is HTTP_REFUSED, and nothing is sent", "[web][library]")
{
    Server server;
    const std::string port = std::to_string(server.port());
    fminus::Program::Options options;
    options.web.allow = {"http://127.0.0.1:" + port + "/pack/1.json", "http://127.0.0.1:" + port + "/redirect/*"};
    auto program = compileProgram("int status(int port, int k)\n"
                                  "{\n"
                                  "    void* r = http_get(\"http://127.0.0.1:%d/pack/%d.json\", port, k);\n"
                                  "    http_wait(r, -1);\n"
                                  "    const int s = http_status(r) == HTTP_REFUSED ? -1000 : http_status(r);\n"
                                  "    http_close(r);\n"
                                  "    return s;\n"
                                  "}\n"
                                  "int redirected(int port)\n"
                                  "{\n"
                                  "    void* r = http_get(\"http://127.0.0.1:%d/redirect/302?/ok\", port);\n"
                                  "    const int s = http_wait(r, -1);\n"
                                  "    http_close(r);\n"
                                  "    return s;\n"
                                  "}\n",
                                  options);
    std::string errors;
    auto* status = program->function<int(int, int)>("status", errors);
    auto* redirected = program->function<int(int)>("redirected", errors);
    REQUIRE(status);
    REQUIRE(redirected);
    CHECK(status(server.port(), 1) == 200);
    CHECK(status(server.port(), 2) == -1000);
    CHECK(redirected(server.port()) == FMINUS_WEB_REFUSED);   // allowed, but not where it redirects to
    CHECK(server.hits("/pack/2.json") == 0);
    CHECK(server.hits("/redirect/302") == 1);
    CHECK(server.hits("/ok") == 0);
}

TEST_CASE("a Program destroyed with requests in flight, and stopped with a thread in http_wait", "[web][library]")
{
    Server server;
    fminus::Program::Options options;
    options.web.allow = {"http://127.0.0.1:" + std::to_string(server.port()) + "/*"};
    options.web.timeoutMs = 60000;
    const char* source = "void* start(int port) { return http_get(\"http://127.0.0.1:%d/slow\", port); }\n"
                         "int wait(void* r) { return http_wait(r, -1); }\n"
                         "int body(void* r) { return http_size(r) + (http_body(r) ? 1 : 0); }\n"
                         "void* again(int port) { return http_get(\"http://127.0.0.1:%d/ok\", port); }\n";
    std::string errors;
    for (int round = 0; round < 3; ++round)
    {
        // Requests the program never closes, in flight when it goes: cancelled and freed, at once.
        auto program = compileProgram(source, options);
        auto* start = program->function<void*(int)>("start", errors);
        REQUIRE(start);
        const int before = server.hits("/slow");
        for (int i = 0; i < 4; ++i)
            start(server.port());
        REQUIRE(eventually([&] { return server.hits("/slow") == before + 4; }));
        const auto destroying = std::chrono::steady_clock::now();
        program.reset();
        CHECK(std::chrono::steady_clock::now() - destroying < 2s);

        // A loader thread in http_wait, inside the program's code: stopWeb() wakes it with HTTP_CANCELLED; the host
        // joins it, then destroys the Program. A completed body stays readable meanwhile.
        program = compileProgram(source, options);
        start = program->function<void*(int)>("start", errors);
        auto* wait = program->function<int(void*)>("wait", errors);
        auto* body = program->function<int(void*)>("body", errors);
        auto* again = program->function<void*(int)>("again", errors);
        REQUIRE((start && wait && body && again));
        void* done = again(server.port());
        REQUIRE(wait(done) == 200);
        void* slow = start(server.port());
        std::atomic<int> result{1};
        std::atomic<bool> waiting{false};
        std::thread loader(
            [&]
            {
                waiting = true;
                result = wait(slow);
            });
        REQUIRE(eventually([&] { return waiting.load() && server.hits("/slow") == before + 5; }));
        std::this_thread::sleep_for(10ms);   // into the wait
        program->stopWeb();
        loader.join();
        CHECK(result.load() == FMINUS_WEB_CANCELLED);
        CHECK(body(done) == 5 + 1);                          // "hello", still there
        CHECK(wait(again(server.port())) == FMINUS_WEB_CANCELLED);   // stopped for good
        program.reset();
    }
    CHECK(eventually([&] { return server.open() == 0; }));
    CHECK_FALSE(fminus::web::ioThreadRunning());
}

#endif   // !_WIN32

#endif   // FMINUS_HAS_WEB
