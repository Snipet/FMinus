// tests/unit/StrictMode.cpp: a body run in a child under seccomp's strict mode, and the parent that waits for its
// result without ever waiting for the child to end (StrictMode.h). The child makes its system calls raw (syscall(2)),
// so that no sanitizer's interceptor runs one of its own around them; the parent alone builds the words of an outcome.
#include "StrictMode.h"

#include <catch2/catch_test_macros.hpp>

#if defined(__linux__)
#include <algorithm>
#include <atomic>
#include <cerrno>
#include <cstdint>
#include <cstring>
#include <fcntl.h>
#include <linux/seccomp.h>
#include <new>
#include <poll.h>
#include <signal.h>
#include <sys/mman.h>
#include <sys/prctl.h>
#include <sys/syscall.h>
#include <sys/wait.h>
#include <unistd.h>
#endif

namespace fminus::test::detail
{
#if defined(__linux__)
    namespace
    {
        // The child's exit status when the kernel refuses strict mode (before it, so the whole process may leave).
        constexpr int kNoStrictMode = 2;

        std::string failure(const char* what)
        {
            return std::string("the child could not be started: ") + what + ": " + std::strerror(errno);
        }

        // What a child that ended (every thread of it) before its result arrived came to, from its status.
        std::string ended(int status)
        {
            if (WIFSIGNALED(status) && WTERMSIG(status) == SIGKILL)
                return "the child was killed before its result arrived (SIGKILL): strict mode kills the thread that "
                       "makes a system call it does not allow, and its body made one";
            if (WIFSIGNALED(status))
                return "the child died of signal " + std::to_string(WTERMSIG(status)) + " (" +
                       ::strsignal(WTERMSIG(status)) + ") before its result arrived";
            if (WIFEXITED(status))
                return "the child exited with status " + std::to_string(WEXITSTATUS(status)) +
                       " before its result arrived";
            return "the child ended before its result arrived (wait status " + std::to_string(status) + ")";
        }
    }   // namespace

    StrictEnd runStrict(void (*run)(const void* body, void* out) noexcept, const void* body, void* out,
                        std::size_t size, std::chrono::milliseconds limit, void (*first)())
    {
        int fds[2];
        if (::pipe2(fds, O_CLOEXEC) != 0)   // close-on-exec: no program another thread starts holds its end open
            return {Strict::failed, failure("pipe2")};
        // The word the kernel clears when the child's thread ends while another thread of the child's lives on
        // (set_tid_address(2)): on a page shared with the child, which the parent reads.
        void* page = ::mmap(nullptr, sizeof(std::atomic<std::int32_t>), PROT_READ | PROT_WRITE,
                            MAP_SHARED | MAP_ANONYMOUS, -1, 0);
        if (page == MAP_FAILED)
        {
            StrictEnd end{Strict::failed, failure("mmap")};
            ::close(fds[0]);
            ::close(fds[1]);
            return end;
        }
        auto* alive = new (page) std::atomic<std::int32_t>(1);
        const pid_t child = ::fork();
        if (child < 0)
        {
            StrictEnd end{Strict::failed, failure("fork")};
            ::close(fds[0]);
            ::close(fds[1]);
            ::munmap(page, sizeof *alive);
            return end;
        }
        if (child == 0)
        {
            if (first != nullptr)
                first();
            ::syscall(SYS_set_tid_address, alive);
            if (::syscall(SYS_prctl, PR_SET_SECCOMP, SECCOMP_MODE_STRICT, 0, 0, 0) != 0)
                ::syscall(SYS_exit_group, kNoStrictMode);
            run(body, out);
            const char* bytes = static_cast<const char*>(out);
            for (std::size_t sent = 0; sent < size;)
            {
                const long n = ::syscall(SYS_write, fds[1], bytes + sent, size - sent);
                if (n <= 0)
                    break;
                sent += static_cast<std::size_t>(n);
            }
            for (;;)
                ::syscall(SYS_exit, 0);   // this thread only: whatever else the child holds, the parent kills
        }

        ::close(fds[1]);
        const auto deadline = std::chrono::steady_clock::now() + limit;
        char* into = static_cast<char*>(out);
        std::size_t got = 0;
        enum
        {
            waiting,
            arrived,
            closed,
            threadEnded,
            timedOut,
            broken
        } state = waiting;
        int readError = 0;
        while (state == waiting)
        {
            // The word first: once it reads 0 the thread has ended, and whatever it wrote is in the pipe.
            const bool gone = alive->load(std::memory_order_acquire) == 0;
            const auto left = std::chrono::ceil<std::chrono::milliseconds>(deadline - std::chrono::steady_clock::now());
            const int wait =
                gone ? 0 : static_cast<int>(std::max<std::int64_t>(0, std::min<std::int64_t>(5, left.count())));
            pollfd ready{fds[0], POLLIN, 0};
            const int polled = ::poll(&ready, 1, wait);
            if (polled < 0 && errno != EINTR)
            {
                readError = errno;
                state = broken;
            }
            else if (polled > 0)
            {
                const ssize_t n = ::read(fds[0], into + got, size - got);
                if (n > 0)
                    got += static_cast<std::size_t>(n);
                else if (n == 0)
                    state = closed;   // every end of the pipe's gone: the whole child has ended
                else if (errno != EINTR)
                {
                    readError = errno;
                    state = broken;
                }
                if (got == size)
                    state = arrived;
            }
            else if (polled == 0 && gone)
                state = threadEnded;
            else if (polled == 0 && std::chrono::steady_clock::now() >= deadline)
                state = timedOut;
        }
        ::close(fds[0]);
        int status = 0;
        if (state != closed)
            ::kill(child, SIGKILL);   // the child has given all it will: whatever of it is left, ended
        while (::waitpid(child, &status, 0) < 0 && errno == EINTR)
        {
        }
        ::munmap(page, sizeof *alive);

        switch (state)
        {
        case arrived:
            return {Strict::returned, {}};
        case closed:
            if (got == 0 && WIFEXITED(status) && WEXITSTATUS(status) == kNoStrictMode)
                return {Strict::unavailable, "seccomp's strict mode is not available here: the system calls are not "
                                             "checked"};
            return {Strict::killed,
                    ended(status) + " (" + std::to_string(got) + " of its " + std::to_string(size) + " bytes)"};
        case threadEnded:
            return {Strict::killed,
                    "strict mode killed the child's thread before its result arrived: its body made a system call "
                    "strict mode does not allow (another thread of the child's, as a sanitizer's, kept its process "
                    "alive, and the parent ended it)"};
        case timedOut:
            return {Strict::late, "no result within " + std::to_string(limit.count()) +
                                      " ms, the child's thread still running: its body hung (a loop that does not "
                                      "end, or a read that waits)"};
        case broken:
            return {Strict::failed, std::string("the child's result could not be read: ") + std::strerror(readError)};
        case waiting:
            break;
        }
        return {Strict::failed, "the child's end is unknown"};
    }
#else
    StrictEnd runStrict(void (*)(const void*, void*) noexcept, const void*, void*, std::size_t,
                        std::chrono::milliseconds, void (*)())
    {
        return {Strict::unavailable, "seccomp is Linux's: the system calls are not checked here"};
    }
#endif

    void reportStrict(Strict outcome, const std::string& why, const std::source_location& where)
    {
        if (outcome == Strict::unavailable)
            WARN(why);
        else if (outcome != Strict::returned)
        {
            std::string line = where.file_name();
            const std::size_t slash = line.find_last_of("/\\");
            if (slash != std::string::npos)
                line.erase(0, slash + 1);
            FAIL_CHECK("in strict mode (" << line << ":" << where.line() << "): " << why);
        }
    }
}   // namespace fminus::test::detail
