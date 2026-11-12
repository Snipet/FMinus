// src/web/SocketsPosix.cpp: the server's sockets on Linux, macOS and the other POSIX systems (Sockets.h). Linux takes
// its flags in the calls themselves (SOCK_CLOEXEC, SOCK_NONBLOCK, accept4, MSG_NOSIGNAL); elsewhere they are set right
// after (FD_CLOEXEC, O_NONBLOCK, SO_NOSIGPIPE, and MSG_NOSIGNAL on each send where the system has it), which leaves a
// moment in which a fork in another thread could inherit the descriptor: the price of a system without the flags. Only
// the Linux half has been built and run.
#include "web/Sockets.h"

#if FMINUS_HAS_WEB_SERVER && !defined(_WIN32)

#include <arpa/inet.h>
#include <cerrno>
#include <cstring>
#include <fcntl.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

namespace fminus::web::sockets
{
    namespace
    {
        // MSG_NOSIGNAL wherever the system has it (Linux, the BSDs, macOS's recent SDKs), so that a system without
        // SO_NOSIGPIPE still sends without SIGPIPE; a system with neither cannot keep Sockets.h's promise.
#if defined(MSG_NOSIGNAL)
        constexpr int kSendFlags = MSG_NOSIGNAL;
#elif defined(SO_NOSIGPIPE)
        constexpr int kSendFlags = 0;
#else
#error "FMinus: the web server needs MSG_NOSIGNAL or SO_NOSIGPIPE, or a send to a closed client raises SIGPIPE"
#endif

        // Not inherited and non-blocking, where the call could not say so itself; and no SIGPIPE where the socket
        // carries that (macOS).
        bool prepare(int fd)
        {
#if !defined(__linux__)
            const int fdFlags = ::fcntl(fd, F_GETFD);
            if (fdFlags < 0 || ::fcntl(fd, F_SETFD, fdFlags | FD_CLOEXEC) != 0)
                return false;
            const int flags = ::fcntl(fd, F_GETFL);
            if (flags < 0 || ::fcntl(fd, F_SETFL, flags | O_NONBLOCK) != 0)
                return false;
#endif
#if defined(SO_NOSIGPIPE)
            int one = 1;
            if (::setsockopt(fd, SOL_SOCKET, SO_NOSIGPIPE, &one, sizeof one) != 0)
                return false;
#endif
            (void)fd;
            return true;
        }
    } // namespace

    Socket listenOn(const Address& address, int port, Bound& outcome) noexcept
    {
        const int family = address.family == 6 ? AF_INET6 : AF_INET;
#if defined(__linux__)
        const int fd = ::socket(family, SOCK_STREAM | SOCK_CLOEXEC | SOCK_NONBLOCK, 0);
#else
        const int fd = ::socket(family, SOCK_STREAM, 0);
#endif
        if (fd < 0)
        {
            outcome = errno == EAFNOSUPPORT || errno == EPROTONOSUPPORT ? Bound::Unavailable : Bound::Failed;
            return kNone;
        }
        const auto fail = [&](Bound why)
        {
            ::close(fd);
            outcome = why;
            return kNone;
        };
        if (!prepare(fd))
            return fail(Bound::Failed);
        int one = 1;
        if (::setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one) != 0)
            return fail(Bound::Failed);
        int bound = 0;
        if (family == AF_INET6)
        {
            if (::setsockopt(fd, IPPROTO_IPV6, IPV6_V6ONLY, &one, sizeof one) != 0)
                return fail(Bound::Failed);
            sockaddr_in6 in6{};
            in6.sin6_family = AF_INET6;
            in6.sin6_port = htons(static_cast<std::uint16_t>(port));
            std::memcpy(&in6.sin6_addr, address.bytes.data(), 16);
            bound = ::bind(fd, reinterpret_cast<const sockaddr*>(&in6), sizeof in6);
        }
        else
        {
            sockaddr_in in{};
            in.sin_family = AF_INET;
            in.sin_port = htons(static_cast<std::uint16_t>(port));
            std::memcpy(&in.sin_addr, address.bytes.data(), 4);
            bound = ::bind(fd, reinterpret_cast<const sockaddr*>(&in), sizeof in);
        }
        if (bound != 0)
            return fail(errno == EADDRINUSE ? Bound::InUse
                                            : errno == EADDRNOTAVAIL || errno == EAFNOSUPPORT ? Bound::Unavailable
                                                                                              : Bound::Failed);
        if (::listen(fd, 64) != 0)
            return fail(errno == EADDRINUSE ? Bound::InUse : Bound::Failed);
        outcome = Bound::Listening;
        return fd;
    }

    int localPort(Socket socket) noexcept
    {
        sockaddr_storage storage{};
        socklen_t length = sizeof storage;
        if (::getsockname(socket, reinterpret_cast<sockaddr*>(&storage), &length) != 0)
            return 0;
        if (storage.ss_family == AF_INET6)
        {
            sockaddr_in6 in6{};
            std::memcpy(&in6, &storage, sizeof in6);
            return ntohs(in6.sin6_port);
        }
        sockaddr_in in{};
        std::memcpy(&in, &storage, sizeof in);
        return ntohs(in.sin_port);
    }

    bool localAddress(Socket socket, Address& out) noexcept
    {
        sockaddr_storage storage{};
        socklen_t length = sizeof storage;
        if (::getsockname(socket, reinterpret_cast<sockaddr*>(&storage), &length) != 0)
            return false;
        out = Address();
        if (storage.ss_family == AF_INET6)
        {
            sockaddr_in6 in6{};
            std::memcpy(&in6, &storage, sizeof in6);
            out.family = 6;
            std::memcpy(out.bytes.data(), &in6.sin6_addr, 16);
            return true;
        }
        if (storage.ss_family == AF_INET)
        {
            sockaddr_in in{};
            std::memcpy(&in, &storage, sizeof in);
            out.family = 4;
            std::memcpy(out.bytes.data(), &in.sin_addr, 4);
            return true;
        }
        return false;
    }

    Socket acceptOn(Socket listener, bool& again) noexcept
    {
        for (;;)
        {
#if defined(__linux__)
            const int fd = ::accept4(listener, nullptr, nullptr, SOCK_CLOEXEC | SOCK_NONBLOCK);
#else
            const int fd = ::accept(listener, nullptr, nullptr);
#endif
            if (fd >= 0)
            {
                if (!prepare(fd))
                {
                    ::close(fd);
                    continue;
                }
                return fd;
            }
            if (errno == EINTR || errno == ECONNABORTED || errno == EPROTO)
                continue;   // that connection is gone; the next may wait
            again = errno == EAGAIN || errno == EWOULDBLOCK;
            return kNone;
        }
    }

    long receive(Socket socket, char* data, std::size_t size) noexcept
    {
        for (;;)
        {
            const ssize_t got = ::recv(socket, data, size, 0);
            if (got >= 0)
                return static_cast<long>(got);
            if (errno == EINTR)
                continue;
            return errno == EAGAIN || errno == EWOULDBLOCK ? -1 : -2;
        }
    }

    long transmit(Socket socket, const char* data, std::size_t size) noexcept
    {
        for (;;)
        {
            const ssize_t sent = ::send(socket, data, size, kSendFlags);
            if (sent > 0)
                return static_cast<long>(sent);
            if (sent == 0)
                return -1;
            if (errno == EINTR)
                continue;
            return errno == EAGAIN || errno == EWOULDBLOCK ? -1 : -2;
        }
    }

    void closeSocket(Socket socket) noexcept
    {
        if (socket != kNone)
            ::close(socket);
    }
} // namespace fminus::web::sockets

#endif
