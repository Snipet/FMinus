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

    struct CompiledPolicy
    {
        Policy policy;
        std::vector<Pattern> patterns;
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
} // namespace fminus::web
