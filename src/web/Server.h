// src/web/Server.h: the web library's server, between its two halves: Web.cpp, whose contexts make servers and whose
// I/O thread drives them, and Server.cpp, the server itself (Web.h has what it does, rule by rule, and its C
// functions). A build without FMINUS_WEB_SERVER has none of it.
//
// How a request travels, and who touches what:
//
// - Listening sockets and connections are more descriptors in the I/O thread's curl_multi_poll, beside libcurl's own:
//   no thread of the server's own. Each connection is a non-blocking state machine (Server.cpp's Connection), owned by
//   the I/O thread alone: reading a head (llhttp parses it, every limit counted in the server's own callbacks), its
//   body, waiting on the program, writing a response, idle.
// - The server keeps a fixed table of slots, maxPending of them, each with a buffer of maxReplyBytes for its reply, all
//   made when the server is. A slot's state is one 64-bit atomic word: a sequence number (which request it holds,
//   counted by the I/O thread) above 8 bits of phase. The I/O thread fills a free slot with a whole request (its
//   target and its header fields, in one buffer of the head's 16 KiB; its body; the route of the table it was matched
//   to, and that route's places) and publishes it with one store. A program takes the oldest published one, of any
//   route or of a route in its set (the route is also in an atomic of the slot's, stored before the publish, so a
//   taker reads it before it claims), with one compare-and-swap, reads it with plain reads (nothing changes it while it
//   is claimed), writes its reply into the slot's buffer and publishes that with one compare-and-swap. The I/O thread sends the reply on its next turn, and
//   frees the slot. It is never woken for a reply (that is a system call): while a slot is published or claimed it
//   turns once a millisecond, and otherwise when a descriptor, a command or a deadline wakes it.
// - The I/O thread gives up on a request when its client leaves or its reply times out: a published one goes back to
//   free, a claimed one to abandoned (the 503 sent for it), which the program's reply turns to done (and returns
//   FMINUS_WEB_CANCELLED for), and the I/O thread then frees. So the request a program holds never changes under it.
// - Closing a server (fminus_web_http_close, Context::stop, ~Context) detaches it from the I/O thread, which closes
//   every socket it has; then marks it closed and wakes the threads in fminus_web_http_accept. Its slots stay, as do
//   its handles, until the context is destroyed.
#pragma once

#include "fminus/Config.h"

#if FMINUS_HAS_WEB_SERVER

#include "web/Policy.h"
#include "web/Web.h"

#include <chrono>
#include <cstdint>
#include <string_view>
#include <vector>

struct curl_waitfd;   // <curl/curl.h>'s, which Web.cpp and Server.cpp include: not the tests, which see no libcurl

namespace fminus::web
{
    struct Server;

    // The innermost deadline scope (fminus_web_http_within_begin, Web.cpp) the calling thread has open under
    // `context`: true, with its deadline (the earlier of its own and every enclosing one's) in `deadline`; false when
    // the thread has none open there, or its scopes have no deadline. Takes the context's mutex: for the blocking
    // functions only.
    bool scopeDeadline(Context::State* context, std::chrono::steady_clock::time_point& deadline) noexcept;

    // The server every null context, and every context whose policy has no listen, gets: it never listens (its port is
    // FMINUS_WEB_REFUSED), and needs no closing.
    Server* refusedServer() noexcept;

    // A server for the context `context` (whose compiled policy `policy` outlives it) and the program's routes, its
    // sockets bound and listening on the calling thread, unless `stopped` (the context's network is stopped: its port
    // is then FMINUS_WEB_CANCELLED, and it has no sockets). A failed bind or listen is a server whose port is
    // FMINUS_WEB_TRANSPORT. Throws std::bad_alloc.
    Server* makeServer(Context::State* context, const CompiledPolicy& policy, const char* const* routes,
                       const std::int32_t* lengths, std::int32_t count, bool stopped);

    // Whether the server has sockets for the I/O thread to drive (it listens).
    bool serverListens(const Server& server) noexcept;

    // The context a server was made under (null for the refused server).
    Context::State* serverContext(const Server& server) noexcept;

    // Marks the server closed, after the I/O thread has let go of it (Servers::detach): its port is FMINUS_WEB_CANCELLED
    // from now on, and every thread in fminus_web_http_accept is woken (null). Any number of times.
    void closeServer(Server& server) noexcept;

    // Frees a closed server, once every thread in fminus_web_http_accept has left (it waits for them).
    void freeServer(Server* server) noexcept;

    // A server whose sockets the I/O thread will never drive after all (its start failed): closes them, and leaves its
    // port FMINUS_WEB_FAILED.
    void failServer(Server& server) noexcept;

    // The I/O thread's servers: every server it drives. Only the I/O thread calls these.
    class Servers
    {
    public:
        // Starts driving `server` (its listening sockets), or, out of memory, closes its sockets and leaves its port
        // FMINUS_WEB_FAILED.
        void attach(Server* server) noexcept;

        // Stops driving `server`: closes its listening sockets and every connection, and gives up every request it has
        // published or the program holds. Nothing, for a server it does not drive.
        void detach(Server* server) noexcept;

        // One turn: replies to send and slots to free, connections to accept, bytes to read, parse and write,
        // deadlines passed.
        void turn() noexcept;

        // The descriptors to wait on, after a turn, in `out` (cleared first): every listening socket and every
        // connection that is waiting for bytes or has bytes to send; nothing closed, ever (a socket is closed only in
        // a turn or a detach, and the array is made again after each). And how long the wait may be, at most `cap`
        // milliseconds: 1 while a request is published or claimed, else until the next deadline. A turn tries every
        // socket it has something to do with, whatever the wait reported (curl reports no hang-up or error of an extra
        // descriptor, only its readability and writability), so the wait's revents are not read.
        void descriptors(std::vector<curl_waitfd>& out) noexcept;
        int timeoutMs(int cap) const noexcept;

    private:
        std::vector<Server*> servers_;
    };

    // fminus_web_http_size and fminus_web_http_body on a request a server received (Web.cpp dispatches on the kind):
    // its body while the program holds it, else 0 and null.
    std::int32_t receivedSize(const void* handle) noexcept;
    const void* receivedBody(const void* handle) noexcept;

    // Whether `handle` is a server, and whether it is a request a server received.
    bool isServer(const void* handle) noexcept;
    bool isReceived(const void* handle) noexcept;
    Server* asServer(void* handle) noexcept;
    void* serverHandle(Server* server) noexcept;

    // A place of a route's path that a request filled: a number's (%d: its value, and its digits) or a text's (%s: one
    // whole segment). `offset` and `length` are its bytes in the path.
    struct Place
    {
        std::int32_t value = 0;     // a number's; 0 for a text
        std::uint16_t offset = 0;   // the path is at most a head's 16384 bytes
        std::uint16_t length = 0;
        bool text = false;
    };
    constexpr std::int32_t kMaxPlaces = 8;   // places a route may have, %d and %s together

    // The pieces of the engine the tests drive directly (WebServerTest.cpp): whether `path` is `pattern` (a route's
    // path: each %d a decimal number of 1 to 10 digits that fits an int; each %s, a segment by itself, one whole
    // segment of 1 or more of the bytes rule 2 lets a segment hold, and not "." or ".."; a %s that is part of a segment
    // never matches), with the places it filled, in their order, in `places` (at most kMaxPlaces) and their count in
    // `count`.
    bool matchRoute(std::string_view pattern, std::string_view path, Place* places, std::int32_t& count) noexcept;

    // The same, with each place's number in `args` (0 for a text's).
    bool matchPath(std::string_view pattern, std::string_view path, std::int32_t* args, std::int32_t& count) noexcept;

    // How specific a route's path is, Go's ServeMux's way (Web.h, "the route a request is published for"): negative when
    // `a` is more specific than `b`, positive when less, 0 when neither (their segments rank the same). Compared
    // segment by segment from the first, the first that differs deciding: a literal segment before one with a %d (and
    // one with fewer %d before one with more), a %d's before a %s.
    int compareSpecificity(std::string_view a, std::string_view b) noexcept;
} // namespace fminus::web

#endif
