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
//
// The server (a build with FMINUS_WEB_SERVER too: fminus/Config.h's FMINUS_HAS_WEB_SERVER; Server.h has its design).
// A program listens where its host's policy says (ServerPolicy, below: nowhere by default), with the table of the route
// literals it was compiled with ("PUT /osc/%d/gain"), and takes the requests that reach it, oldest first, on any thread,
// the audio thread among them: taking one, reading it and answering it are wait-free. The library answers everything
// else itself, on the I/O thread, before a request reaches the program, in this order (each a status, and the
// connection closed after it unless it says otherwise):
//
//   1. The connection: beyond maxConnections, it is closed at once, unanswered.
//   2. The head: at most 16384 bytes and 64 header fields (431), within requestTimeoutMs of its first byte, or of the
//      connection's, for its first request (408). HTTP/1.1 or 1.0 (505; 400 for a version llhttp does not know at
//      all), a method of GET, HEAD, POST, PUT, PATCH,
//      DELETE and OPTIONS (501), a target in origin form whose path is "/" or segments of letters, digits, "-", ".",
//      "_" and "~" (no percent-encoding, and no empty, "." or ".." segment) and whose query, if any, is printable ASCII
//      without "#" (400). A Transfer-Encoding is 501, an Upgrade or an Expect 400 and 417, and a head llhttp does not
//      read as HTTP (every lenient flag of its off) 400.
//   3. Host: exactly one, the connection's local address and port (the port left out only when it is 80; an IPv6
//      address in brackets), "localhost" and the port on a loopback connection, or one of the policy's hosts and the
//      port; else 403.
//   4. Static content: a GET or HEAD of a file's path (ServerPolicy::files) is answered with it here, 200 (and the
//      connection kept alive, unless the request has a body), whatever comes below.
//   5. Where it comes from: Sec-Fetch-Site, when present, same-origin or none, and Origin, when present, "http://" and
//      the Host just checked (letter case aside; "null" never is), else 403; a Content-Type of a form
//      (application/x-www-form-urlencoded, multipart/form-data, text/plain) 415.
//   6. The route: a path no route has, 404; a method none has for the path, 405 with Allow (GET brings HEAD; a file's
//      path allows GET and HEAD). OPTIONS is a method like the others: a preflight gets no permission.
//   7. The token, when the policy has one: "Authorization: Bearer <token>", compared in constant time, else 401 with
//      WWW-Authenticate: Bearer.
//   8. The body: a Content-Length over maxRequestBytes is 413, before a byte of it is read; the whole request within
//      requestTimeoutMs (408).
//   9. A slot: none free is 503 with Retry-After (the connection kept alive). Else the request is published to the
//      program, which has replyTimeoutMs to answer it, or the library answers 503 with Retry-After for it.
//
// Every response says HTTP/1.1 and carries X-Content-Type-Options: nosniff and Cross-Origin-Resource-Policy:
// same-origin; a program's and the library's own Cache-Control: no-store; a static file X-Frame-Options: DENY,
// Content-Security-Policy: frame-ancestors 'none' and Referrer-Policy: same-origin. Never a Server header, nor any
// Access-Control-* one. A connection carries one request at a time (bytes after it wait until it is answered), is kept
// alive after a reply over HTTP/1.1 unless the request said Connection: close, is closed after one over HTTP/1.0, and is
// closed after idleTimeoutMs with nothing on it, or when a reply cannot be sent within requestTimeoutMs.
//
// The listen forms (ServerPolicy::listen), each with a port of 0 to 65535 (0: the system picks one, the same on both
// addresses where it binds two):
//
//   "127.0.0.1:8080"     that address (any of 127.0.0.0/8): loopback
//   "[::1]:8080"         the IPv6 loopback address
//   "localhost:8080"     both loopback addresses, 127.0.0.1 and ::1, one port; ::1 left out on a machine without IPv6
//   "192.168.1.20:8080"  one address of the machine, IPv4 in dotted decimal or IPv6 in brackets: not loopback
//   "*:8080"             every interface, 0.0.0.0 and :: (each IPv6 socket IPV6_V6ONLY): not loopback
//
// A listen that is not loopback needs a token or open. A port browsers refuse (the Fetch standard's bad ports) is
// refused; so are the unspecified addresses themselves (write "*"), multicast and broadcast ones, an IPv4 address in
// an IPv6 one, and IPv6 link-local ones (which need a zone).
#pragma once

#include "fminus/Config.h"

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
    // What a host allows a program's server (above). The default allows none: an empty listen. In a build without
    // FMINUS_WEB_SERVER it is not read.
    struct ServerPolicy
    {
        std::string listen;                // where the program may listen (the forms above); empty: nowhere
        std::vector<std::string> hosts;    // names a request's Host may say besides the address ("studio.local"):
                                           // letters, digits, "-" and ".", as Policy::allow's names; no port
        std::string token;                 // a secret every request to a route must carry: 16 to 256 characters of
                                           // A-Z, a-z, 0-9, "-" and "_", from the system's random source
        bool open = false;                 // a listen that is not loopback, with no token: the host means it
        struct File
        {
            std::string path;    // "/" or "/app.js": a path rule 2 allows, without a query, each path once
            std::string type;    // its Content-Type: printable ASCII with a "/", at most 255 bytes
            std::string bytes;   // its body
        };
        std::vector<File> files;               // static content the library serves itself (rule 4)
        std::int32_t maxConnections = 16;      // open at once (1 to 1024)
        std::int32_t maxPending = 16;          // requests the program has not answered yet: the slots (1 to 1024)
        std::int32_t maxRequestBytes = 1 << 20;   // a request's body (0 to 2^30)
        std::int32_t maxReplyBytes = 64 << 10;    // a reply's body (0 to 2^24); each slot holds a buffer of this
                                                  // size from the start, and all of them at most 2^28 bytes
        std::int32_t requestTimeoutMs = 10000;    // to send a whole request, head and body; and to take a reply
        std::int32_t replyTimeoutMs = 2000;       // for the program to answer one, before the library answers 503
        std::int32_t idleTimeoutMs = 15000;       // a connection kept alive with nothing on it
                                                  // (each time: 1 to 86400000)
    };

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
        ServerPolicy server;                    // where the program's server may listen, and how (above)
    };

    // Whether `policy` is one a context can take: an empty string, or what is wrong with it (the first bad pattern,
    // quoted, and why; a limit out of range; in a build with the server, the first mistake in its server part).
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
        // and, if it was the last context to have used the I/O thread, joins that thread. Its server is closed first
        // (stop()), every thread in fminus_web_http_accept woken and waited for, and then freed with every request
        // it received. No thread may start a request under it, or use one of its requests or its server, once this
        // has begun, except to wait on one already waited on, or in fminus_web_http_accept.
        ~Context();

        Context(const Context&) = delete;
        Context& operator=(const Context&) = delete;

        // What fminus_web_http_get and fminus_web_http_post take: the context, opaque.
        void* handle() noexcept;

        // Stops the context's network for good: every request in flight is cancelled (FMINUS_WEB_CANCELLED, which
        // wakes the threads waiting on it) before this returns, and every request started from then on is
        // cancelled at once, a request one starts while this runs too. Its server, if it listens, is closed as
        // fminus_web_http_close closes it (below), and one listened for from then on never listens (its port is
        // FMINUS_WEB_CANCELLED). What has completed stays as it is, its body
        // readable until it is closed or the context destroyed. Any thread may call it, any number of times; it waits
        // for the I/O thread (briefly), so not the audio thread.
        void stop() noexcept;

        // What the policy says of `url` as it stands, without making a request: 0 when a pattern allows it, else
        // FMINUS_WEB_REFUSED, FMINUS_WEB_LIMIT (too long) or FMINUS_WEB_INVALID. The address check is not part of it
        // (that needs a connection).
        std::int32_t check(std::string_view url) const;

        // The port its server listens on (1 to 65535), once the program has listened (fminus_web_http_listen); 0
        // before, when its policy has no listen or the address could not be bound, once the server is closed, and in a
        // build without FMINUS_WEB_SERVER. Takes the context's mutex: not for the audio thread.
        std::int32_t port() const noexcept;

        struct State;

    private:
        explicit Context(std::unique_ptr<State> state);
        std::unique_ptr<State> state_;
    };

    // Whether the I/O thread is running (for tests: it is joined when the last context that used it goes).
    bool ioThreadRunning();

#if FMINUS_HAS_WEB_SERVER
    // A directory's files as a server's static content (ServerPolicy::files), for a host that has no files of its own
    // to give: the driver's --web-root, an executable's FMINUS_WEB_ROOT (ServerRoot.cpp). Read once, now:
    //   - regular files beneath it only: no symbolic link is followed (a link, to a file or a directory, is skipped),
    //     nor is anything that is not a regular file or a directory read. On POSIX systems each directory is opened
    //     beneath the one that listed it and each file beneath its directory (openat, O_NOFOLLOW), so a directory
    //     swapped for a link while the root is read never leads out of it; on Windows each file is read by its path,
    //     and the root must be writable by no one else while it is read;
    //   - each at the path its place beneath the directory makes ("/app/main.js"), which must be one the server
    //     serves (Web.h, rule 2: each segment letters, digits, '-', '.', '_' and '~', and not "." or ".."); a name that
    //     begins with '.' is hidden, and skipped with what is beneath it, as is a file or a directory whose path the
    //     server cannot serve;
    //   - an index.html also at its directory's path ("/", "/app");
    //   - its type from its extension, in any letter case: html, js, mjs, css, json, svg, png, jpg, gif, ico, wasm,
    //     txt and wav; anything else application/octet-stream;
    //   - at most 256 files and 16 MiB in all (an index.html's second path counts neither), and 4096 entries looked at.
    // Fills `files` (in the order of their paths) and returns an empty string, with what it skipped and why in
    // `skipped` ("'.git': hidden"); or returns what is wrong (not a directory, too many files or bytes, a file that
    // cannot be read), and `files` is then as it was.
    std::string readServerRoot(const std::string& directory, std::vector<ServerPolicy::File>& files,
                               std::vector<std::string>& skipped);
#endif
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

    // fminus_web_http_get and fminus_web_http_post with strings in the URL: its format has %s places as well as %d,
    // each %s the next of the `textCount` texts, the i-th the `lengths[i]` bytes at `texts[i]` (a %s past the last:
    // empty), each percent-encoded as it is filled in: every byte but the unreserved ones (A-Z, a-z, 0-9, "-", ".", "_",
    // "~") written %XX, in upper case. The request is FMINUS_WEB_INVALID, before anything is looked up or sent, when a
    // text is not one (a negative length, or a null address and bytes), when a %s is in the scheme or the authority
    // (the literal's text up to the "/", "?" or "#" after its "://", the host and the port among it; everything, when
    // the literal has no "://" before the %s), or when a path segment a %s is written in is, filled in, empty, "." or
    // "..". The URL filled in then meets every check a URL meets (above): a text whose "/" or "\" becomes %2F or %5C
    // in the path is FMINUS_WEB_REFUSED, as a literal's would be, and one past 8192 bytes, encoded, FMINUS_WEB_LIMIT.
    // A %s in the query or the fragment is encoded and nothing more. A format with no %s is exactly the entry point
    // without strings. A null context: refused, as theirs.
    void* fminus_web_http_get_s(void* context, const char* url, std::int32_t length, const std::int32_t* ints,
                                std::int32_t count, const char* const* texts, const std::int32_t* lengths,
                                std::int32_t textCount) noexcept;
    void* fminus_web_http_post_s(void* context, const char* url, std::int32_t length, const std::int32_t* ints,
                                 std::int32_t count, const char* const* texts, const std::int32_t* lengths,
                                 std::int32_t textCount, const void* data, std::int32_t bytes, const char* type,
                                 std::int32_t typeLength) noexcept;

    // Blocks the calling thread until the request is done or `timeoutMs` milliseconds have passed (0: does not wait;
    // negative: until it is done, which the policy's timeout bounds), and returns its status then (0 if it is still
    // pending). While the calling thread has a deadline scope open under the request's context (below), its innermost
    // one's deadline bounds the wait too, as if it were the timeout. Takes a lock and waits on a condition variable:
    // not for the audio thread (it neither allocates nor frees). A null request is FMINUS_WEB_INVALID.
    std::int32_t fminus_web_http_wait(void* request, std::int32_t timeoutMs) noexcept;

    // Wait-free reads, for the audio thread: each reads the request's atomics and nothing else, allocates nothing,
    // takes no lock and makes no system call. A null request is FMINUS_WEB_INVALID, or 0, or null, and so is a handle
    // of another kind (a server, or a request the server received), except that size and body also read a request a
    // server received (below).
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
    // audio thread. Null, or a handle of another kind: nothing.
    void fminus_web_http_cancel(void* request) noexcept;

    // Cancels the request if pending, waits for the I/O thread to handle every command queued so far (a cancel
    // Context::stop queued for it among them), then frees it and its body: the handle, and the body's address, are no
    // longer valid. Null: nothing. A request must be closed once at most (as memory is freed once); one its program never
    // closes is freed with its context. Null, or a request a server received: nothing. A server: closes it (below).
    void fminus_web_http_close(void* request) noexcept;

    // ---- deadline scopes (a `within (ms) { ... }` block) -------------------------------------------------------------
    //
    // A scope is the calling thread's and the context's: another thread, or another context, never sees it. While a
    // thread has one open under a context, every request it starts there (fminus_web_http_get, _post) joins its
    // innermost one, and the innermost one's deadline bounds the thread's waits under that context:
    // fminus_web_http_wait, fminus_web_http_accept and fminus_web_http_accept_in return when it passes, with what they
    // return on their own timeout. The policy's timeoutMs and maxRequests apply to each request as ever.

    // Opens a scope on the calling thread under `context`, with a deadline `timeoutMs` milliseconds from now (negative:
    // none of its own). Its deadline is the earlier of its own and the enclosing scope's (the thread's innermost one
    // under the context, if any). Returns 0; or FMINUS_WEB_FAILED when memory ran out, and no scope is open (the caller
    // then does not end one). A null context (a program given no network): nothing, and 0. Allocates and takes the
    // context's mutex: not for the audio thread.
    std::int32_t fminus_web_http_within_begin(void* context, std::int32_t timeoutMs) noexcept;

    // Ends the calling thread's innermost scope under `context`: waits until every request that joined it and is
    // still pending has ended, or until its deadline; then cancels those left, whose status becomes
    // FMINUS_WEB_TIMEOUT (time ended them, not the program), and waits for the I/O thread to have ended them; then
    // closes the scope. When it returns, none of the scope's requests is pending. Requests of the scope the program
    // closed already are not its concern. Returns 0 when every request had ended by itself; FMINUS_WEB_TIMEOUT when the
    // deadline came first and some were cancelled; FMINUS_WEB_CANCELLED when the context was stopped (Context::stop:
    // its requests ended as FMINUS_WEB_CANCELLED, and the wait ends with them) or is being destroyed (~Context wakes
    // it, its requests cancelled, and the scope goes with the context); FMINUS_WEB_INVALID when the thread has no scope
    // open there (nothing is done). A null context: 0. Blocks, as fminus_web_http_wait does: not for the audio thread.
    // A scope left open is freed with its context.
    std::int32_t fminus_web_http_within_end(void* context) noexcept;

    // An executable's (support/Aot.h: the C main of a program that calls http_get or http_post). It has no host, so the
    // person who runs it says what it may reach, in its environment: FMINUS_WEB_ALLOW, the patterns (above) separated
    // by spaces, commas or newlines (unset or empty: no network, the default), and FMINUS_WEB_CA_FILE, PEM roots to
    // trust instead of the system's; the policy's other limits are its defaults. In a build with the server, also
    // where it may listen, FMINUS_WEB_LISTEN (unset or empty: nowhere, the default), and with a listen only,
    // FMINUS_WEB_TOKEN, FMINUS_WEB_HOSTS (names, separated as the patterns are), FMINUS_WEB_ROOT (a directory whose
    // files it serves, read now: readServerRoot) and FMINUS_WEB_OPEN ("1" or "0"). Makes the context, when either
    // FMINUS_WEB_ALLOW or FMINUS_WEB_LISTEN says something, and stores its handle in `*context` (null for no network
    // and no server) and returns 0; or, for a policy the library refuses, says why on stderr ("<program>:
    // FMINUS_WEB_ALLOW: ...", or the server's variable the mistake is in) and returns 2, the executable's status for
    // wrong arguments.
    std::int32_t fminus_web_main_start(void** context, const char* program) noexcept;

    // Destroys the context fminus_web_main_start made (null: nothing), after the program's main has returned.
    void fminus_web_main_stop(void* context) noexcept;

#if FMINUS_HAS_WEB_SERVER
    // ---- the server (above) -------------------------------------------------------------------------------------
    //
    // A server is a handle (FMINUS_WEB_INVALID, 0 or null for any other, and for null); so is each request it
    // receives, from fminus_web_http_next or _accept until the program answers it, after which the handle is dead (as
    // a closed request is: a program must not use it again). A request is answered once; until then its method, path,
    // query and body never change, and only the thread that took it should read or answer it (a program that hands it
    // to another thread orders the two itself).

    // The program's server under `context` (Context::handle), with its route table: `count` route literals, the i-th
    // the `lengths[i]` bytes at `routes[i]`, each a method, one space and a path in which "%d" stands for a decimal
    // number ("GET /osc/%d/detune") and "%s", a segment by itself, for one whole segment ("PUT /presets/%s"). At most
    // 8 places (%d and %s together) a route. The compiler checks the literals; the library trusts no byte of them, and
    // a literal that is not a route is left out (it never matches; the others keep their indexes).
    //
    // The route a request is published for: of the table's routes whose path matches the request's and whose method
    // admits it (rule 6), the most specific, as Go's ServeMux has it: compared segment by segment, from the first, the
    // first segment that differs deciding, a literal segment is more specific than one with a %d (one with fewer %d than
    // one with more), and one with a %d than a %s. Two routes that rank the same in every segment (which the compiler
    // refuses as an overlap) go to the first in the table. Binds and listens on the calling thread, then hands
    // the sockets to the I/O thread, and returns at once. A context listens once: a second call returns the same
    // server, its routes unchanged. A null context, or one whose policy has no listen, gives one shared server that
    // never listens (its port FMINUS_WEB_REFUSED) and needs no closing. A failed bind or listen (the address is not
    // this machine's, the port is taken, a sandbox forbids it) gives a server whose port is FMINUS_WEB_TRANSPORT: an
    // ordinary outcome, which the program carries on without. A stopped context's server never listens (its port is
    // FMINUS_WEB_CANCELLED). Null only when memory ran out. Allocates: not for the audio thread.
    void* fminus_web_http_listen(void* context, const char* const* routes, const std::int32_t* lengths,
                                 std::int32_t count) noexcept;

    // The port the server listens on (1 to 65535), or why it does not: FMINUS_WEB_REFUSED (its host allowed none),
    // FMINUS_WEB_TRANSPORT (it could not bind or listen), FMINUS_WEB_CANCELLED (closed, or its context stopped),
    // FMINUS_WEB_FAILED (the library ran out of memory). Wait-free.
    std::int32_t fminus_web_http_port(const void* server) noexcept;

    // The oldest request the server has published and no one has taken, now the caller's to answer, or null (none,
    // or no server). Claimed with one compare-and-swap, so any number of threads may take requests at once and each is
    // taken once. Wait-free: a scan of the slots, tried at most twice their number of times.
    void* fminus_web_http_next(void* server) noexcept;

    // The same, waiting up to `timeoutMs` milliseconds for one (0: does not wait; negative: until one comes or the
    // server is closed). Null at once for a server that does not listen, and when the server is closed or its context
    // stopped or destroyed meanwhile. The calling thread's innermost deadline scope under the server's context (above)
    // bounds the wait as its timeout does. Takes a lock and waits on a condition variable: not for the audio thread.
    void* fminus_web_http_accept(void* server, std::int32_t timeoutMs) noexcept;

    // A route set: `words` 64-bit words at `routes`, route i of the table (fminus_web_http_listen's index) in it when
    // bit i % 64 of word i / 64 is 1. A constant of the program: ceil(count / 64) words, one bit for each route its
    // `serve` names. Null, or 0 or fewer words, is the empty set; an index past the words is not in it.
    //
    // fminus_web_http_next for the requests published for a route in the set alone: the oldest of those, claimed, or
    // null. A request for another route is neither taken nor in the way: it waits for a taker whose set has its route,
    // or for replyTimeoutMs (503), as a request no one takes always has. Wait-free, as fminus_web_http_next.
    void* fminus_web_http_next_in(void* server, const std::uint64_t* routes, std::int32_t words) noexcept;

    // fminus_web_http_accept for the set alone: woken by every request published, it takes one for its set or waits on.
    // Blocks, as fminus_web_http_accept.
    void* fminus_web_http_accept_in(void* server, const std::uint64_t* routes, std::int32_t words,
                                    std::int32_t timeoutMs) noexcept;

    // The table index of the route the request was published for (above), which is in the taker's set when it was
    // taken by one; and that route's places become the request's, as fminus_web_http_is of its literal would make
    // them (for fminus_web_http_arg and fminus_web_http_text). FMINUS_WEB_INVALID for a handle that is not a request the
    // caller holds. Wait-free.
    std::int32_t fminus_web_http_route(void* request) noexcept;

    // 1 when the request's method and path are the route literal's, the `length` bytes at `route` (a method, a space
    // and a path, as fminus_web_http_listen takes them; a GET route matches a HEAD request too), else 0. Each %d of the
    // literal takes a decimal number of the path, 1 to 10 digits whose value fits an int (0 to 2147483647), all the
    // digits there are; each %s, which must be a segment by itself (else the literal never matches), one whole segment
    // as it is. When the request matches, the places it filled are stored for fminus_web_http_arg and
    // fminus_web_http_text, and when it does not, they are as they were. It is the program's own test of a literal: the
    // route the request was published for (fminus_web_http_route) does not change what it says. Wait-free: bounded by
    // the literal's and the path's lengths.
    std::int32_t fminus_web_http_is(void* request, const char* route, std::int32_t length) noexcept;

    // The `index`-th place (from 0, %d and %s counted together, in the literal's order) the last fminus_web_http_is
    // that matched filled, or fminus_web_http_route: a number's value; 0 for a text's, and when there is no such place.
    // Wait-free.
    std::int32_t fminus_web_http_arg(const void* request, std::int32_t index) noexcept;

    // The text of a request. Each returns the address of its bytes and stores their count in `*length` (when
    // `length` is not null): no 0 byte after them, valid until the request is answered. A handle that is not a request
    // the caller holds: null, and 0. Wait-free: each reads the request's slot and nothing else.
    //   text        the `index`-th place's bytes (as fminus_web_http_arg counts them): a %s's segment, or a %d's
    //               digits as the path has them ("0042"); null and 0 when there is no such place.
    //   path        the request's path as sent, without its query ("/osc/3/detune").
    //   header      the value of the request's first header field whose name is the `nameLength` bytes at `name`
    //               (an HTTP token; compared without regard to ASCII case), without the white space around it; empty
    //               (a non-null address, 0) when there is none, or the name is not a token. With a token in the
    //               policy, Authorization is the host's secret and is never given (empty). At most 64 fields.
    //   query_text  the value of the query's first field "name=value" whose name is the `nameLength` bytes at `name`,
    //               compared byte for byte, as it is (nothing decoded), as the numbers' reads find it; null and 0 when
    //               there is no such field, a non-null address when there is ("?v=" and "?v" are present and empty).
    const char* fminus_web_http_text(const void* request, std::int32_t index, std::int32_t* length) noexcept;
    const char* fminus_web_http_path(const void* request, std::int32_t* length) noexcept;
    const char* fminus_web_http_header(const void* request, const char* name, std::int32_t nameLength,
                                       std::int32_t* length) noexcept;
    const char* fminus_web_http_query_text(const void* request, const char* name, std::int32_t nameLength,
                                           std::int32_t* length) noexcept;

    // A number of the request's query: the value of the first field "name=value" whose name is the `length` bytes at
    // `name`, compared byte for byte (nothing is decoded), read whole; or `fallback` when there is no such field, or
    // its value is not all a number. A float is read by fast_float (decimal or exponent form, no "+", no hex), correctly
    // rounded, and must be finite: "nan", "inf" and a number past a float's range give the fallback. An int is an
    // optional "-" and 1 or more decimal digits whose value an int holds. Wait-free.
    float fminus_web_http_query_float(const void* request, const char* name, std::int32_t length,
                                      float fallback) noexcept;
    std::int32_t fminus_web_http_query_int(const void* request, const char* name, std::int32_t length,
                                           std::int32_t fallback) noexcept;

    // The replies. Each answers the request with `status` (200 to 599; 204 and 304 with no body), returns 0, and the
    // handle is dead; or returns, having sent nothing:
    //   FMINUS_WEB_LIMIT      the body is over the policy's maxReplyBytes: the request is still the caller's to answer
    //   FMINUS_WEB_INVALID    not a request the caller holds (null, another kind, already answered), or a status, a
    //                         size or a content type that cannot be (the request is still the caller's)
    //   FMINUS_WEB_CANCELLED  the library gave up on the request meanwhile (its client left, its reply timed out and
    //                         was answered 503, or the server closed): the handle is dead, and its slot free
    // A request taken must be answered: its slot is the taker's until it is, whatever its client was told, so one never
    // answered holds its slot until the server closes, and maxPending of them leave every later request a 503.
    // A content type is the `typeLength` bytes at `type`, printable ASCII, at most 255 bytes; empty: none for a reply
    // with no body, application/octet-stream for bytes, text/plain; charset=utf-8 for text. A HEAD request's reply
    // keeps its Content-Length and drops its body. Wait-free: each writes into the request's own buffer, there since
    // the server started, and publishes the reply with one atomic store, which the I/O thread sees within about a
    // millisecond (it is not woken: waking it is a system call).
    std::int32_t fminus_web_http_reply(void* request, std::int32_t status) noexcept;

    // With the `bytes` bytes at `data` as the body (copied: the program's buffer is free again at once).
    std::int32_t fminus_web_http_reply_bytes(void* request, std::int32_t status, const void* data,
                                             std::int32_t bytes, const char* type, std::int32_t typeLength) noexcept;

    // With text as the body: the format, the `formatLength` bytes at `format`, with `count` values at `values`, as C's
    // printf writes it in the "C" locale: %d the next value as an int, %g as a double with 6 significant digits,
    // %.Ng with N (one or two digits; 0 is 1) significant digits, %% a percent sign; written by std::to_chars, so the
    // same in every locale, with no allocation. Each value is a double: the program's int as a double (exactly), its
    // float widened to one (as printf's float argument is), its double as it is. Three more, which no format a program
    // writes may hold (the compiler refuses them) but those it makes of a JSON literal: %r the shortest digits that
    // read back as the same float (the value narrowed back to the float it was widened from), %R as the same double
    // (std::to_chars with no precision), each a NaN or an infinity as null; and %b true, or false for 0. The compiler
    // checks the format; the library trusts no byte of it: a % that begins none of these is itself, a %d of a value
    // that is no int is C's conversion saturated (NaN is 0), and a place past the last value is nothing.
    std::int32_t fminus_web_http_reply_text(void* request, std::int32_t status, const char* type,
                                            std::int32_t typeLength, const char* format, std::int32_t formatLength,
                                            const double* values, std::int32_t count) noexcept;

    // A reply written in place: the body written straight into the request's own reply buffer (the one the replies
    // above copy into), then published. fminus_web_http_reply_buffer gives that buffer's address, and its capacity,
    // the policy's maxReplyBytes, in `*capacity` (when `capacity` is not null); null and 0 for a handle that is not a
    // request the caller holds. The buffer is the caller's to write until it answers the request, and nothing else
    // reads it before then: getting it answers nothing (a request whose buffer was got and nothing published is still
    // unanswered, and still must be answered, by any reply). fminus_web_http_reply_written answers the request with
    // `status` and the first `bytes` bytes of the buffer as its body, with the content type of `typeLength` bytes at
    // `type` (empty: application/octet-stream), and returns as the replies above: 0; FMINUS_WEB_LIMIT for `bytes` over
    // the capacity (the request still the caller's); FMINUS_WEB_INVALID for a handle not held, a negative `bytes`, a
    // status or a content type that cannot be (204 and 304 with a body among them); FMINUS_WEB_CANCELLED. Both are
    // wait-free; json_write (runtime/Runtime.h) writes a literal into the buffer and gives the bytes to publish.
    char* fminus_web_http_reply_buffer(void* request, std::int32_t* capacity) noexcept;
    std::int32_t fminus_web_http_reply_written(void* request, std::int32_t status, std::int32_t bytes,
                                               const char* type, std::int32_t typeLength) noexcept;

    // fminus_web_http_size and fminus_web_http_body (above) on a request a server received: its body, of
    // Content-Length bytes (with one 0 byte after its last, not counted), until the request is answered.
    //
    // fminus_web_http_close (above) on a server: stops listening, closes every connection (a request waiting is given
    // up: its answer is FMINUS_WEB_CANCELLED), wakes every thread in fminus_web_http_accept (null), and leaves the
    // port FMINUS_WEB_CANCELLED. The handle, and those of the requests it received, stay safe to use until the context
    // is destroyed. Any number of times; the shared refused server: nothing. Waits for the I/O thread (briefly): not
    // for the audio thread.
#endif
}
