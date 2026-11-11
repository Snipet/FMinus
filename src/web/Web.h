// src/web/Web.h: the web library, fminus_web (a build with FMINUS_WEB only; cmake/FMinusWeb.cmake). An HTTP(S) client
// for FMinus programs, on libcurl, under a policy the host sets: what a program may reach, how much it may download,
// for how long. It has no LLVM in it, as the runtime has none.
//
// Two halves:
//
// - The host's, in C++ (namespace fminus::web): a Policy, and a Context that holds a policy and the requests made under
//   it. The default is no network: a context whose policy allows nothing reaches nothing, and a program given no
//   context (null) reaches nothing either. Destroying a context cancels what it has in flight and frees every request
//   made under it, closed or not, so a program that never closes what it opens leaks nothing past its context.
//
// - Generated code's, in C (fminus_web_http_*): the functions FMinus's http_ builtins call, listed in WebSymbols.def.
//   A request is started under a context and comes back at once; it can be waited on, polled, cancelled and closed.
//
// One I/O thread for the process drives every request (libcurl's multi interface). It starts when a context first
// starts a request, and is joined when the last context that used it is destroyed; it is never one of the parallel
// regions' threads. A request's result is published by the I/O thread with one atomic store, of its status: its
// status, size, body and progress are then wait-free reads, which the audio thread may make. A completed body never
// moves until the request is closed.
//
// The policy is checked when a request starts, against its URL once complete (its %d filled in), and again at every
// redirect, which the library follows itself, one hop at a time (libcurl never follows one), so a redirect can never
// reach what the policy does not allow. The URL is read with libcurl's own parser (curl_url), and the request is made
// from the very handle that was checked, so the check and the request cannot disagree about what a URL means. And the
// address each connection reaches is checked as it is opened (below), so a name the policy allows cannot be made to
// point at the machine itself or its network. Certificates are always verified: nothing a program writes can turn that
// off, nor widen the policy.
//
// The pattern language, Policy::allow (each pattern a string; a URL is allowed when one pattern matches it):
//
//   pattern = scheme "://" host [ ":" port ] [ path ]
//   scheme  = "https" | "http"           a URL's scheme must be the pattern's: http is reached only by a pattern
//                                        that says http (letter case is ignored here and in the host)
//   host    = name                       that name exactly: example.com (one trailing dot is ignored, here and in
//                                        the URL: example.com. is example.com)
//           | "*." name                  any name that ends in "." name, with at least one label before it:
//                                        *.example.com matches a.example.com and a.b.example.com, not example.com
//           | IPv4                       an address in dotted decimal, four numbers of 0 to 255 without leading zeros:
//                                        127.0.0.1. A URL may spell it as libcurl reads it (127.1, 0x7f.0.0.1,
//                                        2130706433): what is compared is the address libcurl makes of it
//           | "[" IPv6 "]"               an IPv6 address, any spelling inet_pton takes, no zone (%...): [::1]
//           | "localhost"                the name localhost, reached only on a loopback address
//   port    = 1 to 65535 | "*"           that port, or any. Without one, the scheme's own port only (443, 80)
//   path    = "/" chars [ "*" ]          the URL's path (its query is not matched) equals chars exactly, or, with
//                                        a final "*", starts with chars: /presets/* matches /presets/a.json and
//                                        /presets/a/b, not /presets or /presetsX. Without a path, any path; "/"
//                                        is the path "/" alone ("/*" is any). A "*" anywhere else, and "%", "?",
//                                        "#", "\" anywhere in a pattern, are errors
//
//   A name is letters, digits, "-" (not first or last in a label) and ".", in labels of 1 to 63 bytes; one whose last
//   label is all digits (or starts with 0x) and that is not an IPv4 address in dotted decimal is an error, as is
//   userinfo ("@") anywhere. A URL's host is read as libcurl reads it: its case and one trailing dot aside, a
//   percent-encoded host is decoded first (ex%61mple.com is example.com, where the request then goes).
//
// What a URL must be besides, or it is refused (FMINUS_WEB_REFUSED): printable ASCII with no space and no backslash
// (so no CR, LF or NUL, nor any other control byte), at most 8192 bytes; no userinfo (user, password or login
// options, even empty) and no IPv6 zone; a host that is not a number in some other spelling (a last label all digits
// or 0x...) unless libcurl reads it as an IPv4 address, and nothing but letters, digits, "-" and "."; a scheme other
// than http and https, whether libcurl knows it or not; a path with no percent-encoded ".", "/" or "\" (%2E, %2F,
// %5C, in either case: a server may decode them into a path the pattern did not mean). libcurl has already removed
// "." and ".." segments from the path when it is matched. A URL libcurl cannot parse at all is FMINUS_WEB_INVALID.
//
// The address each connection opens, as it opens it (after the name's resolution, so a name cannot be rebound to
// another address between the check and the connection):
//   - for a pattern by name or "*." name: a public address only, unless the policy sets privateAddresses: never a
//     loopback, private (RFC 1918, fc00::/7), link-local, carrier-grade NAT, unspecified, multicast, documentation or
//     reserved one (an IPv4 address inside an IPv6 one, mapped ::ffff:a.b.c.d, compatible ::a.b.c.d, NAT64's
//     64:ff9b::/96 or 6to4's 2002::/16, is judged as that IPv4 address). With privateAddresses, anything but an
//     unspecified or multicast address;
//   - for "localhost": a loopback address only;
//   - for an IP address: that address only.
// A connection refused here ends the request as FMINUS_WEB_REFUSED.
//
// A content type (http_post's) is printable ASCII, at most 255 bytes: anything else, CR and LF among it, is refused.
#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

// A request's status (fminus_web_http_status): 0 while it is pending, the HTTP status (100 to 599) once a response has
// arrived, or one of these when it ended without one. Each kind of failure has its own.
enum : std::int32_t
{
    FMINUS_WEB_PENDING = 0,
    FMINUS_WEB_TRANSPORT = -1,     // no response: the name did not resolve, the connection failed or was dropped, the
                                   // response was not HTTP, or the TLS handshake failed for another reason than below
    FMINUS_WEB_TIMEOUT = -2,       // the policy's time ran out (the whole request, its redirects included)
    FMINUS_WEB_CANCELLED = -3,     // http_cancel or http_close, or its context was destroyed
    FMINUS_WEB_REFUSED = -4,       // the policy refused it: its URL or a redirect's, or the address a connection
                                   // reached; or the request had no context (a program given no network)
    FMINUS_WEB_LIMIT = -5,         // over a limit: the body (stopped at the limit, not after), the redirects (a 3xx
                                   // with a Location, one more than the policy follows; without one, it is the
                                   // status), the
                                   // requests in flight, a POST's body, a URL's length
    FMINUS_WEB_CERTIFICATE = -6,   // the server's certificate was not trusted, or not for the host; or no root
                                   // certificates were found to trust any
    FMINUS_WEB_INVALID = -7,       // no request: a null handle, a URL libcurl cannot parse, a negative size
    FMINUS_WEB_FAILED = -8,        // the library itself failed: out of memory, or libcurl refused to start
};

namespace fminus::web
{
    // What a host allows a program. The default allows nothing.
    struct Policy
    {
        std::vector<std::string> allow;         // the patterns (above); none: no network
        std::int32_t maxBodyBytes = 16 << 20;   // a response's body, at most (1 to 2^31 - 2), and a POST's
        std::int32_t timeoutMs = 30000;         // a request, its redirects included (at least 1)
        std::int32_t maxRedirects = 5;          // redirects a request follows (0: none); one more is a limit
        std::int32_t maxRequests = 16;          // requests a context has in flight at once (at least 1)
        std::string caFile;                     // PEM roots to trust instead of the system's; empty: the system's
        bool privateAddresses = false;          // names may reach private addresses (above)
        // Names resolved by the host, not by DNS, for this context's requests alone: libcurl's CURLOPT_RESOLVE
        // entries, "name:port:address" (an IPv6 address in brackets). For split DNS, and for tests. The address
        // check applies to them as to any.
        std::vector<std::string> resolve;
    };

    // Whether `policy` is one a context can take: an empty string, or what is wrong with it (the first bad pattern,
    // quoted, and why; a limit out of range).
    std::string checkPolicy(const Policy& policy);

    // A host's policy and the requests made under it. Contexts are independent: each has its own policy, its own
    // DNS entries and connections (libcurl's share interface), and its own requests; all share the one I/O thread.
    class Context
    {
    public:
        // A context for `policy`, or null with `error` set when checkPolicy finds something wrong.
        static std::unique_ptr<Context> create(const Policy& policy, std::string& error);

        // Cancels every request still in flight, wakes every thread waiting on one (its wait returns
        // FMINUS_WEB_CANCELLED), waits for those threads to leave, frees every request made under it, closed or not,
        // and, if it was the last context to have used the I/O thread, joins that thread. No thread may start a
        // request under it, or use one of its requests, once this has begun, except to wait on one already waited on.
        ~Context();

        Context(const Context&) = delete;
        Context& operator=(const Context&) = delete;

        // What fminus_web_http_get and fminus_web_http_post take: the context, opaque.
        void* handle() noexcept;

        // Stops the context's network for good: every request in flight is cancelled (FMINUS_WEB_CANCELLED, which
        // wakes the threads waiting on it) before this returns, and every request started from then on is
        // cancelled at once, a request one starts while this runs too. What has completed stays as it is, its body
        // readable until it is closed or the context destroyed. Any thread may call it, any number of times; it waits
        // for the I/O thread (briefly), so not the audio thread.
        void stop() noexcept;

        // What the policy says of `url` as it stands, without making a request: 0 when a pattern allows it, else
        // FMINUS_WEB_REFUSED, FMINUS_WEB_LIMIT (too long) or FMINUS_WEB_INVALID. The address check is not part of it
        // (that needs a connection).
        std::int32_t check(std::string_view url) const;

        struct State;

    private:
        explicit Context(std::unique_ptr<State> state);
        std::unique_ptr<State> state_;
    };

    // Whether the I/O thread is running (for tests: it is joined when the last context that used it goes).
    bool ioThreadRunning();
} // namespace fminus::web

extern "C"
{
    // Starts a GET of the URL that `length` bytes at `url` and `count` ints at `ints` make (runtime/Format.h: each %d
    // the next int in decimal, %% a percent sign), under `context` (Context::handle), and returns its request at once.
    // The policy is checked before anything is sent: a refused request is a request too, whose status is already
    // FMINUS_WEB_REFUSED (or _LIMIT, _INVALID). A null context is a program given no network: its requests are
    // refused (one shared request that needs no closing, though closing it is harmless). Null only when memory ran
    // out. Allocates: not for the audio thread.
    void* fminus_web_http_get(void* context, const char* url, std::int32_t length, const std::int32_t* ints,
                              std::int32_t count) noexcept;

    // The same for a POST of `bytes` bytes at `data` (copied: the program's buffer is free again when this returns),
    // with the content type of `typeLength` bytes at `type` (empty: application/octet-stream). A body over the
    // policy's maxBodyBytes is FMINUS_WEB_LIMIT, a negative size FMINUS_WEB_INVALID. A redirect by 307 or 308 sends
    // the body again; by 301, 302 or 303 the next request is a GET, without it.
    void* fminus_web_http_post(void* context, const char* url, std::int32_t length, const std::int32_t* ints,
                               std::int32_t count, const void* data, std::int32_t bytes, const char* type,
                               std::int32_t typeLength) noexcept;

    // Blocks the calling thread until the request is done or `timeoutMs` milliseconds have passed (0: does not wait;
    // negative: until it is done, which the policy's timeout bounds), and returns its status then (0 if it is still
    // pending). Takes a lock and waits on a condition variable: not for the audio thread (it neither allocates nor
    // frees). A null request is FMINUS_WEB_INVALID.
    std::int32_t fminus_web_http_wait(void* request, std::int32_t timeoutMs) noexcept;

    // Wait-free reads, for the audio thread: each reads the request's atomics and nothing else, allocates nothing,
    // takes no lock and makes no system call. A null request is FMINUS_WEB_INVALID, or 0, or null.
    //   status    0 while pending, the HTTP status, or a failure (FMINUS_WEB_*).
    //   size      the body's bytes once the status is an HTTP status; 0 before, and after a failure.
    //   body      the body's address once the status is an HTTP status (with one 0 byte after its last, not counted
    //             in its size), valid until the request is closed; null before, and after a failure.
    //   progress  the bytes of the body received so far (of the current hop, while redirects are followed): the size,
    //             once done.
    // These change from another thread: a caller must read them again each time it needs them (generated code must
    // not treat two calls as one).
    std::int32_t fminus_web_http_status(const void* request) noexcept;
    std::int32_t fminus_web_http_size(const void* request) noexcept;
    const void* fminus_web_http_body(const void* request) noexcept;
    std::int32_t fminus_web_http_progress(const void* request) noexcept;

    // Stops a pending request: when it returns, the status is FMINUS_WEB_CANCELLED, or what the request ended with if
    // it ended first. A done request is left as it is. Waits for the I/O thread to let go of it (briefly): not for the
    // audio thread. Null: nothing.
    void fminus_web_http_cancel(void* request) noexcept;

    // Cancels the request if pending, waits for the I/O thread to handle every command queued so far (a cancel
    // Context::stop queued for it among them), then frees it and its body: the handle, and the body's address, are no
    // longer valid. Null: nothing. A request must be closed once at most (as memory is freed once); one its program never
    // closes is freed with its context.
    void fminus_web_http_close(void* request) noexcept;

    // An executable's (support/Aot.h: the C main of a program that calls http_get or http_post). It has no host, so the
    // person who runs it says what it may reach, in its environment: FMINUS_WEB_ALLOW, the patterns (above) separated
    // by spaces, commas or newlines (unset or empty: no network, the default), and FMINUS_WEB_CA_FILE, PEM roots to
    // trust instead of the system's; the policy's other limits are its defaults. Makes the context and stores its
    // handle in `*context` (null for no network) and returns 0; or, for a policy the library refuses, says why on
    // stderr ("<program>: FMINUS_WEB_ALLOW: ...") and returns 2, the executable's status for wrong arguments.
    std::int32_t fminus_web_main_start(void** context, const char* program) noexcept;

    // Destroys the context fminus_web_main_start made (null: nothing), after the program's main has returned.
    void fminus_web_main_stop(void* context) noexcept;
}
