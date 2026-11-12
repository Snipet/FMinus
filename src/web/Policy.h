// src/web/Policy.h: the policy's patterns, compiled; the check of a URL against them, with libcurl's URL parser; and
// the check of the address a connection opens (Web.h has the language and the rules this implements).
#pragma once

#include "web/Web.h"

#include <array>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

typedef struct Curl_URL CURLU;

namespace fminus::web
{
    // An IPv4 or IPv6 address, as bytes in network order.
    struct Address
    {
        int family = 0;                          // 4 or 6; 0: none
        std::array<unsigned char, 16> bytes{};   // the first 4 for IPv4
    };

    // Which addresses a connection under a pattern may reach.
    enum class Reach
    {
        Public,     // a name: public addresses (or, with privateAddresses, all but unspecified and multicast)
        Private,    // a name, under a policy with privateAddresses
        Loopback,   // localhost
        Exactly,    // an IP address: that one
    };

    struct Pattern
    {
        enum class Host
        {
            Name,
            Suffix,   // "*." name: `host` holds "." name
            Address,
            Localhost,
        };
        bool https = true;
        Host kind = Host::Name;
        std::string host;   // lower case, no trailing dot
        Address address;    // Host::Address
        int port = 0;       // 0: any
        std::string path;   // "" with prefix: any path
        bool prefix = true;
    };

    // The server's part of a policy, compiled (ServerPolicy::listen read into addresses, a port and whether they are
    // loopback; the hosts in lower case; the files by path). Only a build with FMINUS_WEB_SERVER fills it in.
    struct ServerSetup
    {
        bool listens = false;             // the policy has a listen
        std::vector<Address> addresses;   // what to bind, in order: one, or two (localhost, *)
        bool localhost = false;           // "localhost": ::1 may be left out on a machine without IPv6
        bool any = false;                 // "*": :: may be left out likewise
        bool loopback = false;            // every address is loopback
        int port = 0;                     // 0: the system picks
        std::vector<std::string> hosts;   // lower case
    };

    struct CompiledPolicy
    {
        Policy policy;
        std::vector<Pattern> patterns;
        ServerSetup server;
    };

    // Compiles `policy` into `out`; an empty string, or what is wrong.
    std::string compilePolicy(const Policy& policy, CompiledPolicy& out);

    // What a checked URL is: its handle (which the request is made from), what its host is, and which addresses its
    // connections may reach.
    struct Target
    {
        CURLU* url = nullptr;   // owned by the caller once check() returns 0 (curl_url_cleanup)
        bool https = true;
        Reach reach = Reach::Public;
        Address address;   // Reach::Exactly
    };

    // Checks `text` against the policy: 0, with `target` filled in (the caller then owns target.url), or
    // FMINUS_WEB_REFUSED, FMINUS_WEB_LIMIT, FMINUS_WEB_INVALID or FMINUS_WEB_FAILED, with nothing to free.
    std::int32_t checkUrl(const CompiledPolicy& policy, std::string_view text, Target& target);

    // Whether a connection to `address` may be opened under `reach` (and `exactly`, for Reach::Exactly).
    bool reachable(Reach reach, const Address& exactly, const Address& address);

    // Whether `type` may be a request's content type.
    bool validContentType(std::string_view type);

    // Reads an IPv4 address in dotted decimal (four numbers of 0 to 255, no leading zeros) or an IPv6 address, into
    // `out`; false when `text` is neither.
    bool parseAddress(std::string_view text, int family, Address& out);

    // Whether `name` (lower case, without a trailing dot) is a name a pattern may hold (Web.h): letters, digits and "-"
    // in labels of 1 to 63 bytes, at most 253 bytes, its last label not all digits nor starting with 0x.
    bool validName(std::string_view name);

#if FMINUS_HAS_WEB_SERVER
    // The server's part of the policy (ServerPolicy.cpp): compiled into `out`, or what is wrong with it.
    std::string compileServerPolicy(const ServerPolicy& policy, ServerSetup& out);

    // Whether `address` is a loopback one: 127.0.0.0/8 or ::1.
    bool loopbackAddress(const Address& address);

    // Whether browsers refuse to reach `port` (the Fetch standard's bad ports; 0 among them).
    bool badPort(int port);

    // Whether `path` is one rule 2 allows (Web.h): "/", or "/" and segments of letters, digits, "-", ".", "_" and "~",
    // none of them empty, "." or "..".
    bool servablePath(std::string_view path);

    // Rule 3: whether a request's Host, `host`, names the server, on a connection that arrived at `local`, port
    // `port`: the address itself (an IPv6 one in brackets), "localhost" when `local` is loopback, or one of the
    // setup's hosts; each with ":" and the port, which may be left out only when it is 80. Letter case is ignored.
    bool hostAllowed(const ServerSetup& setup, std::string_view host, const Address& local, int port);

    // Rule 5: whether a request's Origin, `origin`, is the server's own: "http://" and the Host already allowed,
    // letter case aside. "null" never is.
    bool originAllowed(std::string_view origin, std::string_view host);
#endif
} // namespace fminus::web
