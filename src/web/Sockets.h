// src/web/Sockets.h: the server's sockets, the only part of it that differs between systems: SocketsPosix.cpp (Linux,
// macOS and the other POSIX systems) and SocketsWindows.cpp (Winsock). Every socket made here is non-blocking, is not
// inherited by a child process, and never raises SIGPIPE:
//
//   |                       | Linux                         | macOS                       | Windows                    |
//   | not inherited         | SOCK_CLOEXEC, accept4         | FD_CLOEXEC after the call   | WSA_FLAG_NO_HANDLE_INHERIT,|
//   |                       |                               |                             | and cleared after accept   |
//   | no SIGPIPE            | MSG_NOSIGNAL on each send     | SO_NOSIGPIPE on the socket, | (there is no signal)       |
//   |                       | (and on any POSIX system that | and MSG_NOSIGNAL where the  |                            |
//   |                       | has it; neither: #error)      | SDK has it                  |                            |
//   | taking the port       | SO_REUSEADDR                  | SO_REUSEADDR                | SO_EXCLUSIVEADDRUSE, never |
//   |                       |                               |                             | SO_REUSEADDR               |
//   | an IPv6 listener      | IPV6_V6ONLY, always           | the same                    | the same                   |
//
// SO_REUSEADDR lets a restarted program take its port without waiting out TIME_WAIT, and on Linux and macOS does not
// let a second listener bind the same address; on Windows it would, so there the port is taken exclusively. curl, which
// waits on these sockets among its own (Web.cpp's I/O thread), makes each one non-blocking on Windows itself
// (WSAEventSelect); they are made so here too. Linux is the only system this was built and run on.
#pragma once

#include "fminus/Config.h"

#if FMINUS_HAS_WEB_SERVER

#include "web/Policy.h"

#include <cstddef>
#include <cstdint>

namespace fminus::web::sockets
{
#if defined(_WIN32)
    using Socket = std::uintptr_t;          // SOCKET
    inline constexpr Socket kNone = ~Socket(0);   // INVALID_SOCKET
#else
    using Socket = int;
    inline constexpr Socket kNone = -1;
#endif

    // How a listen went.
    enum class Bound
    {
        Listening,
        InUse,         // the address and port are taken
        Unavailable,   // the family is not there (no IPv6), or the address is none of this machine's
        Failed,        // anything else: no permission, no descriptors, a sandbox
    };

    // A listening socket on `address` (family 4 or 6; an address of all zeros is every interface of that family) and
    // `port` (0: the system picks one), with a backlog of 64; kNone, with why in `outcome`, when it could not be made.
    Socket listenOn(const Address& address, int port, Bound& outcome) noexcept;

    // The socket's local port, or 0; and its local address (false when it has none to give).
    int localPort(Socket socket) noexcept;
    bool localAddress(Socket socket, Address& out) noexcept;

    // The next connection waiting on `listener`, made non-blocking and not inherited, with no SIGPIPE; or kNone, with
    // `again` true when none is waiting, false when accepting failed (out of descriptors, for one: the caller stops
    // accepting for a while rather than spin).
    Socket acceptOn(Socket listener, bool& again) noexcept;

    // recv: the bytes read (> 0), 0 when the peer closed its side, -1 when there is nothing now, -2 when the
    // connection failed (reset).
    long receive(Socket socket, char* data, std::size_t size) noexcept;

    // send: the bytes sent (> 0), -1 when nothing can be sent now, -2 when the connection failed (the peer is gone).
    long transmit(Socket socket, const char* data, std::size_t size) noexcept;

    void closeSocket(Socket socket) noexcept;
} // namespace fminus::web::sockets

#endif
