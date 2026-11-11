// src/web/SocketsWindows.cpp: the server's sockets on Windows (Sockets.h), on Winsock 2, which curl_global_init has
// started (WSAStartup) for as long as a context lives. A listening socket takes its port with SO_EXCLUSIVEADDRUSE,
// never SO_REUSEADDR (which on Windows lets another process bind the same address and port over it), and is made with
// WSA_FLAG_NO_HANDLE_INHERIT; an accepted one inherits the listener's properties, and its handle is made not inheritable
// again explicitly. There is no SIGPIPE. Never built or run: written from Microsoft's documentation of each call.
#include "web/Sockets.h"

#if FMINUS_HAS_WEB_SERVER && defined(_WIN32)

#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>

#include <cstring>

namespace fminus::web::sockets
{
    namespace
    {
        SOCKET native(Socket socket)
        {
            return static_cast<SOCKET>(socket);
        }

        bool nonBlocking(SOCKET s)
        {
            u_long one = 1;
            return ::ioctlsocket(s, FIONBIO, &one) == 0;
        }
    } // namespace

    Socket listenOn(const Address& address, int port, Bound& outcome) noexcept
    {
        const int family = address.family == 6 ? AF_INET6 : AF_INET;
        const SOCKET s = ::WSASocketW(family, SOCK_STREAM, IPPROTO_TCP, nullptr, 0,
                                      WSA_FLAG_OVERLAPPED | WSA_FLAG_NO_HANDLE_INHERIT);
        if (s == INVALID_SOCKET)
        {
            const int error = ::WSAGetLastError();
            outcome = error == WSAEAFNOSUPPORT || error == WSAEPROTONOSUPPORT ? Bound::Unavailable : Bound::Failed;
            return kNone;
        }
        const auto fail = [&](Bound why)
        {
            ::closesocket(s);
            outcome = why;
            return kNone;
        };
        BOOL one = TRUE;
        if (::setsockopt(s, SOL_SOCKET, SO_EXCLUSIVEADDRUSE, reinterpret_cast<const char*>(&one), sizeof one) != 0)
            return fail(Bound::Failed);
        if (!nonBlocking(s))
            return fail(Bound::Failed);
        int bound = 0;
        if (family == AF_INET6)
        {
            DWORD v6only = 1;
            if (::setsockopt(s, IPPROTO_IPV6, IPV6_V6ONLY, reinterpret_cast<const char*>(&v6only), sizeof v6only) != 0)
                return fail(Bound::Failed);
            sockaddr_in6 in6{};
            in6.sin6_family = AF_INET6;
            in6.sin6_port = ::htons(static_cast<u_short>(port));
            std::memcpy(&in6.sin6_addr, address.bytes.data(), 16);
            bound = ::bind(s, reinterpret_cast<const sockaddr*>(&in6), sizeof in6);
        }
        else
        {
            sockaddr_in in{};
            in.sin_family = AF_INET;
            in.sin_port = ::htons(static_cast<u_short>(port));
            std::memcpy(&in.sin_addr, address.bytes.data(), 4);
            bound = ::bind(s, reinterpret_cast<const sockaddr*>(&in), sizeof in);
        }
        if (bound != 0)
        {
            // With SO_EXCLUSIVEADDRUSE, a port another socket holds is WSAEACCES as often as WSAEADDRINUSE.
            const int error = ::WSAGetLastError();
            return fail(error == WSAEADDRINUSE || error == WSAEACCES ? Bound::InUse
                        : error == WSAEADDRNOTAVAIL || error == WSAEAFNOSUPPORT ? Bound::Unavailable
                                                                               : Bound::Failed);
        }
        if (::listen(s, 64) != 0)
            return fail(Bound::Failed);
        outcome = Bound::Listening;
        return static_cast<Socket>(s);
    }

    int localPort(Socket socket) noexcept
    {
        sockaddr_storage storage{};
        int length = sizeof storage;
        if (::getsockname(native(socket), reinterpret_cast<sockaddr*>(&storage), &length) != 0)
            return 0;
        if (storage.ss_family == AF_INET6)
            return ::ntohs(reinterpret_cast<const sockaddr_in6*>(&storage)->sin6_port);
        return ::ntohs(reinterpret_cast<const sockaddr_in*>(&storage)->sin_port);
    }

    bool localAddress(Socket socket, Address& out) noexcept
    {
        sockaddr_storage storage{};
        int length = sizeof storage;
        if (::getsockname(native(socket), reinterpret_cast<sockaddr*>(&storage), &length) != 0)
            return false;
        out = Address();
        if (storage.ss_family == AF_INET6)
        {
            out.family = 6;
            std::memcpy(out.bytes.data(), &reinterpret_cast<const sockaddr_in6*>(&storage)->sin6_addr, 16);
            return true;
        }
        if (storage.ss_family == AF_INET)
        {
            out.family = 4;
            std::memcpy(out.bytes.data(), &reinterpret_cast<const sockaddr_in*>(&storage)->sin_addr, 4);
            return true;
        }
        return false;
    }

    Socket acceptOn(Socket listener, bool& again) noexcept
    {
        for (;;)
        {
            const SOCKET s = ::accept(native(listener), nullptr, nullptr);
            if (s != INVALID_SOCKET)
            {
                if (!::SetHandleInformation(reinterpret_cast<HANDLE>(s), HANDLE_FLAG_INHERIT, 0) || !nonBlocking(s))
                {
                    ::closesocket(s);
                    continue;
                }
                return static_cast<Socket>(s);
            }
            const int error = ::WSAGetLastError();
            if (error == WSAEINTR || error == WSAECONNRESET)
                continue;   // that connection is gone; the next may wait
            again = error == WSAEWOULDBLOCK;
            return kNone;
        }
    }

    long receive(Socket socket, char* data, std::size_t size) noexcept
    {
        const int want = size > 0x10000000 ? 0x10000000 : static_cast<int>(size);
        const int got = ::recv(native(socket), data, want, 0);
        if (got >= 0)
            return got;
        return ::WSAGetLastError() == WSAEWOULDBLOCK ? -1 : -2;
    }

    long transmit(Socket socket, const char* data, std::size_t size) noexcept
    {
        const int want = size > 0x10000000 ? 0x10000000 : static_cast<int>(size);
        const int sent = ::send(native(socket), data, want, 0);
        if (sent > 0)
            return sent;
        if (sent == 0)
            return -1;
        return ::WSAGetLastError() == WSAEWOULDBLOCK ? -1 : -2;
    }

    void closeSocket(Socket socket) noexcept
    {
        if (socket != kNone)
            ::closesocket(native(socket));
    }
} // namespace fminus::web::sockets

#endif
