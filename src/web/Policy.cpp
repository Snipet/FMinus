// src/web/Policy.cpp
#include "web/Policy.h"

#include <algorithm>
#include <cstring>
#include <curl/curl.h>
#include <limits>
#include <memory>

#if defined(_WIN32)
#include <winsock2.h>
#include <ws2tcpip.h>
#else
#include <arpa/inet.h>
#include <sys/socket.h>
#endif

namespace fminus::web
{
    namespace
    {
        constexpr std::size_t kMaxUrlBytes = 8192;

        char lower(char c)
        {
            return c >= 'A' && c <= 'Z' ? static_cast<char>(c - 'A' + 'a') : c;
        }

        std::string lowered(std::string_view text)
        {
            std::string out(text);
            for (char& c : out)
                c = lower(c);
            return out;
        }

        bool isDigit(char c)
        {
            return c >= '0' && c <= '9';
        }

        // A name of letters, digits and '-' (not first or last) in labels of 1 to 63 bytes separated by '.', at most
        // 253 bytes, already lower case and without its trailing dot; one whose last label is all digits, or starts
        // with 0x, reads as a number to some resolvers, and is not one.
        bool validName(std::string_view name)
        {
            if (name.empty() || name.size() > 253)
                return false;
            std::size_t start = 0;
            for (;;)
            {
                const std::size_t dot = name.find('.', start);
                const std::string_view label =
                    name.substr(start, dot == std::string_view::npos ? std::string_view::npos : dot - start);
                if (label.empty() || label.size() > 63 || label.front() == '-' || label.back() == '-')
                    return false;
                for (char c : label)
                {
                    if (!(isDigit(c) || (c >= 'a' && c <= 'z') || c == '-'))
                        return false;
                }
                if (dot == std::string_view::npos)
                {
                    if (std::all_of(label.begin(), label.end(), isDigit) || label.starts_with("0x"))
                        return false;
                    return true;
                }
                start = dot + 1;
            }
        }

        // Strips one trailing dot: "example.com." is "example.com".
        std::string_view withoutTrailingDot(std::string_view host)
        {
            if (!host.empty() && host.back() == '.')
                host.remove_suffix(1);
            return host;
        }

        bool parsePort(std::string_view text, int& port)
        {
            if (text.empty() || text.size() > 5 || !std::all_of(text.begin(), text.end(), isDigit))
                return false;
            int value = 0;
            for (char c : text)
                value = value * 10 + (c - '0');
            if (value < 1 || value > 65535)
                return false;
            port = value;
            return true;
        }

        std::string quoted(std::string_view pattern)
        {
            return "'" + std::string(pattern) + "'";
        }

        // One pattern (Web.h's language) into `out`; an empty string, or why it is not one.
        std::string compilePattern(std::string_view text, Pattern& out)
        {
            for (char c : text)
            {
                const auto byte = static_cast<unsigned char>(c);
                if (byte <= 0x20 || byte >= 0x7f)
                    return "pattern " + quoted(text) + " holds a space, a control byte or a byte that is not ASCII";
                if (c == '%' || c == '?' || c == '#' || c == '\\' || c == '@')
                    return "pattern " + quoted(text) + " holds '" + std::string(1, c) + "', which no pattern may";
            }
            const std::size_t schemeEnd = text.find("://");
            if (schemeEnd == std::string_view::npos)
                return "pattern " + quoted(text) + " has no scheme (https:// or http://)";
            const std::string scheme = lowered(text.substr(0, schemeEnd));
            if (scheme != "https" && scheme != "http")
                return "pattern " + quoted(text) + " has the scheme '" + scheme + "': only https and http are allowed";
            out.https = scheme == "https";
            std::string_view rest = text.substr(schemeEnd + 3);
            const std::size_t slash = rest.find('/');
            std::string_view authority = rest.substr(0, slash);
            std::string_view path = slash == std::string_view::npos ? std::string_view() : rest.substr(slash);

            // The host, and the port after it.
            std::string_view host;
            std::string_view port;
            if (authority.starts_with('['))
            {
                const std::size_t close = authority.find(']');
                if (close == std::string_view::npos)
                    return "pattern " + quoted(text) + " opens an IPv6 address with '[' and never closes it";
                host = authority.substr(1, close - 1);
                std::string_view after = authority.substr(close + 1);
                if (!after.empty())
                {
                    if (after.front() != ':')
                        return "pattern " + quoted(text) + " has something after its IPv6 address that is not a port";
                    port = after.substr(1);
                    if (port.empty())
                        return "pattern " + quoted(text) + " has a ':' and no port";
                }
                if (!parseAddress(host, 6, out.address))
                    return "pattern " + quoted(text) + " has '[" + std::string(host) + "]', which is no IPv6 address";
                out.kind = Pattern::Host::Address;
            }
            else
            {
                const std::size_t colon = authority.find(':');
                host = authority.substr(0, colon);
                if (colon != std::string_view::npos)
                {
                    port = authority.substr(colon + 1);
                    if (port.empty())
                        return "pattern " + quoted(text) + " has a ':' and no port";
                }
                const std::string name = lowered(withoutTrailingDot(host));
                if (host.empty() || name.empty())
                    return "pattern " + quoted(text) + " has no host";
                if (name == "localhost")
                {
                    out.kind = Pattern::Host::Localhost;
                    out.host = name;
                }
                else if (parseAddress(name, 4, out.address))
                    out.kind = Pattern::Host::Address;
                else if (name.starts_with("*."))
                {
                    if (!validName(name.substr(2)))
                        return "pattern " + quoted(text) + " has '" + std::string(host) +
                               "', which is no '*.' and a name";
                    out.kind = Pattern::Host::Suffix;
                    out.host = name.substr(1);
                }
                else if (!validName(name))
                {
                    if (name.find('*') != std::string::npos)
                        return "pattern " + quoted(text) + " has a '*' in its host other than a leading '*.'";
                    return "pattern " + quoted(text) + " has '" + std::string(host) +
                           "', which is no name (letters, digits, '-' and '.') and no IPv4 address in dotted decimal";
                }
                else
                {
                    out.kind = Pattern::Host::Name;
                    out.host = name;
                }
            }
            if (port.empty())
                out.port = out.https ? 443 : 80;
            else if (port == "*")
                out.port = 0;
            else if (!parsePort(port, out.port))
                return "pattern " + quoted(text) + " has the port '" + std::string(port) + "' (1 to 65535, or *)";

            // The path: exact, or a prefix with a final '*'.
            if (path.empty())
            {
                out.path.clear();
                out.prefix = true;
                return {};
            }
            const std::size_t star = path.find('*');
            if (star != std::string_view::npos && star != path.size() - 1)
                return "pattern " + quoted(text) + " has a '*' in its path other than at its end";
            out.prefix = star != std::string_view::npos;
            out.path = std::string(out.prefix ? path.substr(0, star) : path);
            return {};
        }

        bool hostMatches(const Pattern& pattern, bool isAddress, const std::string& name, const Address& address)
        {
            switch (pattern.kind)
            {
            case Pattern::Host::Name:
                return !isAddress && name == pattern.host;
            case Pattern::Host::Suffix:
                return !isAddress && name.size() > pattern.host.size() && name.ends_with(pattern.host);
            case Pattern::Host::Localhost:
                return !isAddress && name == "localhost";
            case Pattern::Host::Address:
                return isAddress && address.family == pattern.address.family && address.bytes == pattern.address.bytes;
            }
            return false;
        }

        // A part of a URL handle as a string ("" with `present` false when it has none).
        bool urlPart(CURLU* url, CURLUPart part, unsigned flags, std::string& out)
        {
            char* text = nullptr;
            if (curl_url_get(url, part, &text, flags) != CURLUE_OK || text == nullptr)
                return false;
            out = text;
            curl_free(text);
            return true;
        }

        // The IPv4 address an IPv4-mapped (::ffff:0:0/96), IPv4-compatible (::/96), NAT64 (64:ff9b::/96) or 6to4
        // (2002::/16) IPv6 address carries, or the address itself.
        Address embedded(const Address& address)
        {
            if (address.family != 6)
                return address;
            const auto& b = address.bytes;
            auto v4 = [&](int at)
            {
                Address out;
                out.family = 4;
                std::copy_n(b.begin() + at, 4, out.bytes.begin());
                return out;
            };
            const bool zero10 = std::all_of(b.begin(), b.begin() + 10, [](unsigned char x) { return x == 0; });
            if (zero10 && b[10] == 0xff && b[11] == 0xff)
                return v4(12);
            if (zero10 && b[10] == 0 && b[11] == 0 && !(b[12] == 0 && b[13] == 0 && b[14] == 0 && b[15] <= 1))
                return v4(12);
            if (b[0] == 0x00 && b[1] == 0x64 && b[2] == 0xff && b[3] == 0x9b &&
                std::all_of(b.begin() + 4, b.begin() + 12, [](unsigned char x) { return x == 0; }))
                return v4(12);
            if (b[0] == 0x20 && b[1] == 0x02)
                return v4(2);
            return address;
        }

        enum class Kind
        {
            Unspecified,
            Loopback,
            Multicast,
            Private,   // private, link-local, carrier-grade NAT, documentation, reserved: not the internet's
            Public,
        };

        Kind classify(const Address& raw)
        {
            const Address address = embedded(raw);
            const auto& b = address.bytes;
            if (address.family == 4)
            {
                const unsigned a0 = b[0];
                const unsigned a1 = b[1];
                const unsigned a2 = b[2];
                if (a0 == 0)
                    return Kind::Unspecified;   // 0.0.0.0/8: "this network"
                if (a0 == 127)
                    return Kind::Loopback;
                if (a0 >= 224 && a0 <= 239)
                    return Kind::Multicast;
                if (a0 >= 240 || a0 == 10 || (a0 == 172 && (a1 & 0xf0) == 16) || (a0 == 192 && a1 == 168) ||
                    (a0 == 169 && a1 == 254) || (a0 == 100 && (a1 & 0xc0) == 64) ||
                    (a0 == 192 && a1 == 0 && (a2 == 0 || a2 == 2)) || (a0 == 198 && (a1 & 0xfe) == 18) ||
                    (a0 == 198 && a1 == 51 && a2 == 100) || (a0 == 203 && a1 == 0 && a2 == 113) ||
                    (a0 == 192 && a1 == 88 && a2 == 99))
                    return Kind::Private;
                return Kind::Public;
            }
            if (address.family != 6)
                return Kind::Unspecified;
            const bool zero15 = std::all_of(b.begin(), b.begin() + 15, [](unsigned char x) { return x == 0; });
            if (zero15 && b[15] == 0)
                return Kind::Unspecified;
            if (zero15 && b[15] == 1)
                return Kind::Loopback;
            if (b[0] == 0xff)
                return Kind::Multicast;
            if (b[0] == 0x00 ||                                    // ::/8, what embedded() did not take
                (b[0] & 0xfe) == 0xfc ||                           // fc00::/7, unique local
                (b[0] == 0xfe && (b[1] & 0x80) == 0x80) ||         // fe80::/10 link-local, fec0::/10 site-local
                (b[0] == 0x01 && b[1] == 0x00) ||                  // 100::/64 and the rest of 100::/16, discard
                (b[0] == 0x20 && b[1] == 0x01 && b[2] < 0x02) ||   // 2001::/23, Teredo and the IETF's
                (b[0] == 0x20 && b[1] == 0x01 && b[2] == 0x0d && b[3] == 0xb8))   // 2001:db8::/32, documentation
                return Kind::Private;
            return Kind::Public;
        }
    } // namespace

    bool parseAddress(std::string_view text, int family, Address& out)
    {
        if (family == 4)
        {
            Address address;
            address.family = 4;
            std::size_t start = 0;
            for (int part = 0; part < 4; ++part)
            {
                const std::size_t dot = text.find('.', start);
                if ((part < 3) == (dot == std::string_view::npos))
                    return false;
                const std::string_view number =
                    text.substr(start, dot == std::string_view::npos ? std::string_view::npos : dot - start);
                if (number.empty() || number.size() > 3 || (number.size() > 1 && number[0] == '0') ||
                    !std::all_of(number.begin(), number.end(), isDigit))
                    return false;
                int value = 0;
                for (char c : number)
                    value = value * 10 + (c - '0');
                if (value > 255)
                    return false;
                address.bytes[static_cast<std::size_t>(part)] = static_cast<unsigned char>(value);
                start = dot + 1;
            }
            out = address;
            return true;
        }
        if (family != 6 || text.empty() || text.size() > 45 || text.find('%') != std::string_view::npos)
            return false;
        const std::string copy(text);
        Address address;
        address.family = 6;
        if (inet_pton(AF_INET6, copy.c_str(), address.bytes.data()) != 1)
            return false;
        out = address;
        return true;
    }

    bool reachable(Reach reach, const Address& exactly, const Address& address)
    {
        switch (reach)
        {
        case Reach::Exactly:
            return address.family == exactly.family && address.bytes == exactly.bytes;
        case Reach::Loopback:
            return classify(address) == Kind::Loopback;
        case Reach::Public:
            return classify(address) == Kind::Public;
        case Reach::Private:
        {
            const Kind kind = classify(address);
            return kind != Kind::Unspecified && kind != Kind::Multicast;
        }
        }
        return false;
    }

    bool validContentType(std::string_view type)
    {
        if (type.size() > 255)
            return false;
        return std::all_of(type.begin(), type.end(),
                           [](char c)
                           {
                               const auto byte = static_cast<unsigned char>(c);
                               return byte >= 0x20 && byte < 0x7f;
                           });
    }

    std::string compilePolicy(const Policy& policy, CompiledPolicy& out)
    {
        if (policy.maxBodyBytes < 1 || policy.maxBodyBytes > std::numeric_limits<std::int32_t>::max() - 1)
            return "maxBodyBytes is " + std::to_string(policy.maxBodyBytes) + " (1 to 2147483646)";
        if (policy.timeoutMs < 1)
            return "timeoutMs is " + std::to_string(policy.timeoutMs) + " (at least 1)";
        if (policy.maxRedirects < 0 || policy.maxRedirects > 100)
            return "maxRedirects is " + std::to_string(policy.maxRedirects) + " (0 to 100)";
        if (policy.maxRequests < 1)
            return "maxRequests is " + std::to_string(policy.maxRequests) + " (at least 1)";
        for (char c : policy.caFile)
        {
            if (c == '\0' || c == '\r' || c == '\n')
                return "caFile holds a NUL, CR or LF";
        }
        for (const std::string& entry : policy.resolve)
        {
            // name:port:address, as CURLOPT_RESOLVE takes it, without its +, - and * forms.
            const std::size_t first = entry.find(':');
            const std::size_t second = first == std::string::npos ? first : entry.find(':', first + 1);
            int port = 0;
            Address address;
            std::string_view addressText =
                second == std::string::npos ? "" : std::string_view(entry).substr(second + 1);
            if (addressText.size() > 2 && addressText.front() == '[' && addressText.back() == ']')
                addressText = addressText.substr(1, addressText.size() - 2);
            if (second == std::string::npos || !validName(lowered(withoutTrailingDot(entry.substr(0, first)))) ||
                !parsePort(std::string_view(entry).substr(first + 1, second - first - 1), port) ||
                !(parseAddress(addressText, 4, address) || parseAddress(addressText, 6, address)))
                return "resolve entry '" + entry + "' is not name:port:address";
        }
        CompiledPolicy compiled;
        compiled.policy = policy;
        for (const std::string& text : policy.allow)
        {
            Pattern pattern;
            std::string error = compilePattern(text, pattern);
            if (!error.empty())
                return error;
            compiled.patterns.push_back(std::move(pattern));
        }
        out = std::move(compiled);
        return {};
    }

    std::int32_t checkUrl(const CompiledPolicy& policy, std::string_view text, Target& target)
    {
        if (text.size() > kMaxUrlBytes)
            return FMINUS_WEB_LIMIT;
        for (char c : text)
        {
            const auto byte = static_cast<unsigned char>(c);
            if (byte <= 0x20 || byte >= 0x7f || c == '\\')
                return FMINUS_WEB_REFUSED;
        }
        if (text.empty())
            return FMINUS_WEB_INVALID;
        // A scheme other than http and https is refused, whether libcurl knows it or not.
        const std::size_t colon = text.find(':');
        if (colon != std::string_view::npos && colon > 0)
        {
            const std::string_view scheme = text.substr(0, colon);
            const bool token = std::all_of(
                scheme.begin(), scheme.end(), [](char c)
                { return isDigit(c) || (lower(c) >= 'a' && lower(c) <= 'z') || c == '+' || c == '-' || c == '.'; });
            if (token && !isDigit(scheme.front()) && lowered(scheme) != "http" && lowered(scheme) != "https")
                return FMINUS_WEB_REFUSED;
        }

        std::unique_ptr<CURLU, decltype(&curl_url_cleanup)> url(curl_url(), &curl_url_cleanup);
        if (!url)
            return FMINUS_WEB_FAILED;
        const std::string copy(text);
        const CURLUcode parsed = curl_url_set(url.get(), CURLUPART_URL, copy.c_str(), 0);
        if (parsed == CURLUE_UNSUPPORTED_SCHEME)
            return FMINUS_WEB_REFUSED;
        if (parsed == CURLUE_OUT_OF_MEMORY)
            return FMINUS_WEB_FAILED;
        if (parsed != CURLUE_OK)
            return FMINUS_WEB_INVALID;

        std::string scheme, host, port, path, ignored;
        if (!urlPart(url.get(), CURLUPART_SCHEME, 0, scheme) || !urlPart(url.get(), CURLUPART_HOST, 0, host) ||
            !urlPart(url.get(), CURLUPART_PORT, CURLU_DEFAULT_PORT, port) ||
            !urlPart(url.get(), CURLUPART_PATH, 0, path))
            return FMINUS_WEB_INVALID;
        scheme = lowered(scheme);
        if (scheme != "https" && scheme != "http")
            return FMINUS_WEB_REFUSED;
        // Credentials, even empty, and a zone.
        if (urlPart(url.get(), CURLUPART_USER, 0, ignored) || urlPart(url.get(), CURLUPART_PASSWORD, 0, ignored) ||
            urlPart(url.get(), CURLUPART_OPTIONS, 0, ignored) || urlPart(url.get(), CURLUPART_ZONEID, 0, ignored))
            return FMINUS_WEB_REFUSED;

        // The host: an address (IPv6 in brackets, or IPv4 as libcurl wrote it), or a name.
        Address address;
        bool isAddress = false;
        std::string name;
        if (host.size() > 2 && host.front() == '[' && host.back() == ']')
        {
            if (!parseAddress(std::string_view(host).substr(1, host.size() - 2), 6, address))
                return FMINUS_WEB_REFUSED;
            isAddress = true;
        }
        else
        {
            name = lowered(withoutTrailingDot(host));
            if (parseAddress(name, 4, address))
                isAddress = true;
            else if (!validName(name))
                return FMINUS_WEB_REFUSED;
        }
        int portNumber = 0;
        if (!parsePort(port, portNumber))
            return FMINUS_WEB_INVALID;

        // Percent-encoded '.', '/' and '\' in the path.
        const std::string lowerPath = lowered(path);
        if (lowerPath.find("%2e") != std::string::npos || lowerPath.find("%2f") != std::string::npos ||
            lowerPath.find("%5c") != std::string::npos)
            return FMINUS_WEB_REFUSED;
        // A dot segment with a parameter, "..;" or ".;x=1": libcurl keeps it (it is no dot segment to RFC 3986), and a
        // server that drops a segment's parameters before it resolves the path (Tomcat, Jetty) climbs out of the prefix
        // the pattern allowed.
        for (std::size_t start = 0; start < path.size();)
        {
            std::size_t end = path.find('/', start);
            if (end == std::string::npos)
                end = path.size();
            const std::string_view segment = std::string_view(path).substr(start, end - start);
            const std::string_view bare = segment.substr(0, segment.find(';'));
            if (bare.size() != segment.size() && (bare == "." || bare == ".."))
                return FMINUS_WEB_REFUSED;
            start = end + 1;
        }

        const bool https = scheme == "https";
        for (const Pattern& pattern : policy.patterns)
        {
            if (pattern.https != https || (pattern.port != 0 && pattern.port != portNumber) ||
                !hostMatches(pattern, isAddress, name, address))
                continue;
            if (pattern.prefix ? !path.starts_with(pattern.path) : path != pattern.path)
                continue;
            target.https = https;
            target.address = address;
            switch (pattern.kind)
            {
            case Pattern::Host::Address:
                target.reach = Reach::Exactly;
                break;
            case Pattern::Host::Localhost:
                target.reach = Reach::Loopback;
                break;
            default:
                target.reach = policy.policy.privateAddresses ? Reach::Private : Reach::Public;
                break;
            }
            target.url = url.release();
            return 0;
        }
        return FMINUS_WEB_REFUSED;
    }
} // namespace fminus::web
