// src/web/ServerPolicy.cpp: the server's part of a policy (Web.h's ServerPolicy), checked and compiled; and the checks
// of a request's Host and Origin against it (rules 3 and 5), which the server makes on every request. A build without
// FMINUS_WEB_SERVER compiles nothing here.
#include "web/Policy.h"

#if FMINUS_HAS_WEB_SERVER

#include <algorithm>
#include <cstdint>
#include <set>
#include <string>
#include <string_view>

namespace fminus::web
{
    namespace
    {
        // The Fetch standard's bad ports (fetch.spec.whatwg.org, "bad port", as of 2026-11): a browser refuses a URL
        // with one, whatever listens there.
        constexpr int kBadPorts[] = {0,    1,    7,    9,    11,   13,   15,   17,   19,   20,   21,   22,   23,
                                     25,   37,   42,   43,   53,   69,   77,   79,   87,   95,   101,  102,  103,
                                     104,  109,  110,  111,  113,  115,  117,  119,  123,  135,  137,  139,  143,
                                     161,  179,  389,  427,  465,  512,  513,  514,  515,  526,  530,  531,  532,
                                     540,  548,  554,  556,  563,  587,  601,  636,  989,  990,  993,  995,  1719,
                                     1720, 1723, 2049, 3659, 4045, 4190, 5060, 5061, 6000, 6566, 6665, 6666, 6667,
                                     6668, 6669, 6679, 6697, 10080};

        constexpr std::int32_t kMaxToken = 256;
        constexpr std::int64_t kMaxReplyMemory = std::int64_t(1) << 28;   // every slot's buffer together

        bool isDigit(char c)
        {
            return c >= '0' && c <= '9';
        }

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

        bool equalsIgnoringCase(std::string_view a, std::string_view b)
        {
            if (a.size() != b.size())
                return false;
            for (std::size_t i = 0; i < a.size(); ++i)
            {
                if (lower(a[i]) != lower(b[i]))
                    return false;
            }
            return true;
        }

        // A port of 1 to 5 digits, without a leading zero (but "0" itself), of at most 65535.
        bool readPort(std::string_view text, int& port)
        {
            if (text.empty() || text.size() > 5 || !std::all_of(text.begin(), text.end(), isDigit) ||
                (text.size() > 1 && text[0] == '0'))
                return false;
            int value = 0;
            for (char c : text)
                value = value * 10 + (c - '0');
            if (value > 65535)
                return false;
            port = value;
            return true;
        }

        std::string quoted(std::string_view text)
        {
            return "'" + std::string(text) + "'";
        }

        std::string range(std::string_view field, std::int64_t value, std::int64_t low, std::int64_t high)
        {
            return "server." + std::string(field) + " is " + std::to_string(value) + " (" + std::to_string(low) +
                   " to " + std::to_string(high) + ")";
        }

        // What an address a listen names may not be, or empty.
        std::string unlistenable(const Address& address)
        {
            const auto& b = address.bytes;
            if (address.family == 4)
            {
                if (b[0] == 0 && b[1] == 0 && b[2] == 0 && b[3] == 0)
                    return "is the unspecified address: write '*' for every interface";
                if (b[0] == 0)
                    return "is in 0.0.0.0/8, which is no interface's";
                if (b[0] >= 224)
                    return "is a multicast, broadcast or reserved address";
                return {};
            }
            if (std::all_of(b.begin(), b.end(), [](unsigned char x) { return x == 0; }))
                return "is the unspecified address: write '*' for every interface";
            if (b[0] == 0xff)
                return "is a multicast address";
            if (b[0] == 0xfe && (b[1] & 0xc0) == 0x80)
                return "is a link-local address, which needs a zone (a listen takes none)";
            if (std::all_of(b.begin(), b.begin() + 10, [](unsigned char x) { return x == 0; }) &&
                ((b[10] == 0xff && b[11] == 0xff) || (b[10] == 0 && b[11] == 0)) && !loopbackAddress(address))
                return "is an IPv4 address inside an IPv6 one: write the IPv4 address";
            return {};
        }

        // The listen forms (Web.h) into `out`, or what is wrong with `listen`.
        std::string compileListen(const ServerPolicy& policy, ServerSetup& out)
        {
            const std::string& listen = policy.listen;
            const std::string name = "server.listen " + quoted(listen);
            for (char c : listen)
            {
                const auto byte = static_cast<unsigned char>(c);
                if (byte <= 0x20 || byte >= 0x7f)
                    return name + " holds a space, a control byte or a byte that is not ASCII";
            }
            const std::size_t colon = listen.rfind(':');
            if (colon == std::string::npos)
                return name + " has no port (\"127.0.0.1:8080\", \"localhost:0\", ...)";
            const std::string_view host = std::string_view(listen).substr(0, colon);
            const std::string_view portText = std::string_view(listen).substr(colon + 1);
            int port = 0;
            if (!readPort(portText, port))
                return name + " has the port " + quoted(portText) + " (0 to 65535, without leading zeros)";
            if (port != 0 && badPort(port))
                return name + " has the port " + std::to_string(port) +
                       ", which browsers refuse to reach (the Fetch standard's bad ports)";
            out.port = port;
            out.addresses.clear();
            Address address;
            if (host == "*")
            {
                address.family = 4;
                out.addresses.push_back(address);
                address.family = 6;
                out.addresses.push_back(address);
                out.any = true;
                out.loopback = false;
            }
            else if (equalsIgnoringCase(host, "localhost"))
            {
                parseAddress("127.0.0.1", 4, address);
                out.addresses.push_back(address);
                parseAddress("::1", 6, address);
                out.addresses.push_back(address);
                out.localhost = true;
                out.loopback = true;
            }
            else if (host.size() > 2 && host.front() == '[' && host.back() == ']')
            {
                if (!parseAddress(host.substr(1, host.size() - 2), 6, address))
                    return name + " has " + quoted(host) + ", which is no IPv6 address (and a listen takes no zone)";
                if (std::string why = unlistenable(address); !why.empty())
                    return name + ": " + std::string(host) + " " + why;
                out.addresses.push_back(address);
                out.loopback = loopbackAddress(address);
            }
            else if (parseAddress(host, 4, address))
            {
                if (std::string why = unlistenable(address); !why.empty())
                    return name + ": " + std::string(host) + " " + why;
                out.addresses.push_back(address);
                out.loopback = loopbackAddress(address);
            }
            else
                return name + " has " + quoted(host) +
                       ": a listen is an IPv4 address in dotted decimal, an IPv6 address in brackets, localhost or *";
            if (!out.loopback && policy.token.empty() && !policy.open)
                return name + " is not a loopback address: the policy needs a token (server.token), or open "
                              "(server.open) to say that the port is meant to be open to anyone who can reach it";
            out.listens = true;
            return {};
        }
    } // namespace

    bool loopbackAddress(const Address& address)
    {
        if (address.family == 4)
            return address.bytes[0] == 127;
        if (address.family != 6)
            return false;
        return std::all_of(address.bytes.begin(), address.bytes.begin() + 15, [](unsigned char x) { return x == 0; }) &&
               address.bytes[15] == 1;
    }

    bool badPort(int port)
    {
        return std::find(std::begin(kBadPorts), std::end(kBadPorts), port) != std::end(kBadPorts);
    }

    bool servablePath(std::string_view path)
    {
        if (path.empty() || path.front() != '/')
            return false;
        if (path.size() == 1)
            return true;
        std::size_t start = 1;
        for (;;)
        {
            const std::size_t slash = path.find('/', start);
            const std::string_view segment =
                path.substr(start, slash == std::string_view::npos ? std::string_view::npos : slash - start);
            if (segment.empty() || segment == "." || segment == "..")
                return false;
            for (char c : segment)
            {
                const bool letter = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z');
                if (!(letter || isDigit(c) || c == '-' || c == '.' || c == '_' || c == '~'))
                    return false;
            }
            if (slash == std::string_view::npos)
                return true;
            start = slash + 1;
        }
    }

    std::string compileServerPolicy(const ServerPolicy& policy, ServerSetup& out)
    {
        ServerSetup setup;
        if (policy.maxConnections < 1 || policy.maxConnections > 1024)
            return range("maxConnections", policy.maxConnections, 1, 1024);
        if (policy.maxPending < 1 || policy.maxPending > 1024)
            return range("maxPending", policy.maxPending, 1, 1024);
        if (policy.maxRequestBytes < 0 || policy.maxRequestBytes > (1 << 30))
            return range("maxRequestBytes", policy.maxRequestBytes, 0, 1 << 30);
        if (policy.maxReplyBytes < 0 || policy.maxReplyBytes > (1 << 24))
            return range("maxReplyBytes", policy.maxReplyBytes, 0, 1 << 24);
        if (std::int64_t(policy.maxPending) * policy.maxReplyBytes > kMaxReplyMemory)
            return "server.maxPending times server.maxReplyBytes is " +
                   std::to_string(std::int64_t(policy.maxPending) * policy.maxReplyBytes) +
                   " bytes, which the slots would hold from the start (at most " + std::to_string(kMaxReplyMemory) + ")";
        const std::pair<std::string_view, std::int32_t> times[] = {{"requestTimeoutMs", policy.requestTimeoutMs},
                                                                  {"replyTimeoutMs", policy.replyTimeoutMs},
                                                                  {"idleTimeoutMs", policy.idleTimeoutMs}};
        for (const auto& [field, value] : times)
        {
            if (value < 1 || value > 86400000)
                return range(field, value, 1, 86400000);
        }
        if (!policy.token.empty())
        {
            if (policy.token.size() < 16 || policy.token.size() > kMaxToken)
                return "server.token has " + std::to_string(policy.token.size()) + " characters (16 to " +
                       std::to_string(kMaxToken) + ")";
            for (char c : policy.token)
            {
                const bool letter = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z');
                if (!(letter || isDigit(c) || c == '-' || c == '_'))
                    return "server.token holds a character other than A-Z, a-z, 0-9, '-' and '_'";
            }
        }
        for (const std::string& host : policy.hosts)
        {
            std::string name = lowered(host);
            if (name == "localhost" || !validName(name))
                return "server.hosts has " + quoted(host) +
                       ", which is no name (letters, digits, '-' and '.', no port; localhost needs no entry)";
            setup.hosts.push_back(std::move(name));
        }
        std::set<std::string_view> paths;
        for (const ServerPolicy::File& file : policy.files)
        {
            if (!servablePath(file.path))
                return "server.files has the path " + quoted(file.path) +
                       ": \"/\", or \"/\" and segments of letters, digits, '-', '.', '_' and '~', none empty, '.' or "
                       "'..', and no query";
            if (!paths.insert(file.path).second)
                return "server.files has the path " + quoted(file.path) + " twice";
            if (file.type.empty() || file.type.size() > 255 || !validContentType(file.type) ||
                file.type.find('/') == std::string::npos)
                return "server.files has the type " + quoted(file.type) + " for " + quoted(file.path) +
                       ": printable ASCII with a '/', at most 255 bytes";
            if (file.bytes.size() > static_cast<std::size_t>(INT32_MAX))
                return "server.files has more than 2147483647 bytes for " + quoted(file.path);
        }
        if (!policy.listen.empty())
        {
            if (std::string error = compileListen(policy, setup); !error.empty())
                return error;
        }
        out = std::move(setup);
        return {};
    }

    bool hostAllowed(const ServerSetup& setup, std::string_view host, const Address& local, int port)
    {
        std::string_view name = host;
        std::string_view portText;
        bool hasPort = false;
        if (host.starts_with('['))
        {
            const std::size_t close = host.find(']');
            if (close == std::string_view::npos)
                return false;
            name = host.substr(0, close + 1);
            const std::string_view rest = host.substr(close + 1);
            if (!rest.empty())
            {
                if (rest.front() != ':')
                    return false;
                portText = rest.substr(1);
                hasPort = true;
            }
        }
        else if (const std::size_t colon = host.find(':'); colon != std::string_view::npos)
        {
            name = host.substr(0, colon);
            portText = host.substr(colon + 1);
            hasPort = true;
        }
        if (hasPort)
        {
            int said = 0;
            if (!readPort(portText, said) || said != port)
                return false;
        }
        else if (port != 80)
            return false;
        if (name.empty())
            return false;
        Address address;
        if (name.front() == '[')
            return parseAddress(name.substr(1, name.size() - 2), 6, address) && address.family == local.family &&
                   address.bytes == local.bytes;
        const std::string lower = lowered(name);
        if (parseAddress(lower, 4, address))
            return address.family == local.family && address.bytes == local.bytes;
        if (lower == "localhost")
            return loopbackAddress(local);
        return std::find(setup.hosts.begin(), setup.hosts.end(), lower) != setup.hosts.end();
    }

    bool originAllowed(std::string_view origin, std::string_view host)
    {
        constexpr std::string_view scheme = "http://";
        return origin.size() == scheme.size() + host.size() && equalsIgnoringCase(origin.substr(0, scheme.size()), scheme) &&
               equalsIgnoringCase(origin.substr(scheme.size()), host);
    }
} // namespace fminus::web

#endif
