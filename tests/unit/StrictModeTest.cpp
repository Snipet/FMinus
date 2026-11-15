// tests/unit/StrictModeTest.cpp: the helpers behind every wait-free proof, proved able to fail (a proof that cannot
// fail proves nothing). StrictMode.h: a body that makes only the system calls strict mode allows returns its result;
// one that makes another (getpid, an allocation the heap must ask the kernel for) fails, in words that say strict mode
// killed it; one that never returns fails at the limit, in words that say it hung; and a proof written with
// inStrictMode fails its test case on each. Each in three children: one alone, one that another thread keeps alive
// after its own ends (the helper's test starts it there, as ThreadSanitizer's runtime does in a child forked from a
// process with one thread), and one forked beside another thread of the parent's. HeapCalls.h: it counts what a body
// allocates, where it counts (glibc, no sanitizer; elsewhere it warns, as the proofs do). Linux only, but the warning.
#include "StrictMode.h"

#include "HeapCalls.h"

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <atomic>
#include <chrono>
#include <cstdlib>
#include <string>
#include <thread>
#include <vector>

#if defined(__linux__)
#include <sys/syscall.h>
#include <unistd.h>
#endif

#if defined(__SANITIZE_THREAD__)
#define FMINUS_TEST_TSAN 1
#elif defined(__has_feature)
#if __has_feature(thread_sanitizer)
#define FMINUS_TEST_TSAN 1
#endif
#endif

using namespace fminus::test;
using namespace std::chrono_literals;

#if defined(__linux__)
namespace
{
#if !defined(FMINUS_TEST_TSAN)
    // A thread that waits for ever, started in the child before strict mode: the child lives on after its own thread
    // ends, as a sanitizer's thread keeps it.
    void keepAlive()
    {
        std::thread(
            []
            {
                for (;;)
                    ::pause();
            })
            .detach();
    }
#endif

    // The children each case runs its bodies in, each by its name.
    struct Child
    {
        const char* name;
        void (*first)();
        bool besideAThread;   // forked while another thread of the parent's runs
    };

    std::vector<Child> children()
    {
        std::vector<Child> all{{"a child alone", nullptr, false}, {"a child forked beside a thread", nullptr, true}};
#if !defined(FMINUS_TEST_TSAN)
        // Under ThreadSanitizer the child alone is this one (its runtime starts a thread there when the parent has
        // one), and its runtime refuses a thread started in a child of a process with several (die_after_fork).
        all.push_back({"a child another thread keeps alive", keepAlive, false});
#endif
        return all;
    }

    // body() run in strict mode in `child`, beside a thread of this process's if it asks for one.
    template <typename Body>
    auto runIn(const Child& child, Body&& body, std::chrono::milliseconds limit = kStrictLimit)
    {
        std::atomic<bool> done{false};
        std::thread beside;
        if (child.besideAThread)
            beside = std::thread(
                [&done]
                {
                    while (!done.load())
                        std::this_thread::sleep_for(1ms);
                });
        auto sealed = runInStrictMode(std::forward<Body>(body), limit, child.first);
        done = true;
        if (beside.joinable())
            beside.join();
        return sealed;
    }

    bool available()
    {
        return runInStrictMode([] { return 0; }).available();
    }
}   // namespace

TEST_CASE("strict mode: a body that makes only the system calls it allows returns its result", "[strict]")
{
    if (!available())
        SKIP("seccomp's strict mode is not available here");
    for (const Child& child : children())
    {
        INFO(child.name);
        // write and read are allowed, even where they fail (no such descriptor).
        const auto sealed = runIn(
            child, []
            { return std::array<long, 3>{42, ::syscall(SYS_write, -1, "x", 1), ::syscall(SYS_read, -1, nullptr, 0)}; });
        INFO(sealed.why());
        REQUIRE(sealed.outcome() == Strict::returned);
        CHECK(sealed.why().empty());
        CHECK((*sealed)[0] == 42);
        CHECK((*sealed)[1] == -1);
        CHECK((*sealed)[2] == -1);
    }
}

TEST_CASE("strict mode: a body that makes a system call it does not allow fails, and says strict mode killed it",
          "[strict]")
{
    if (!available())
        SKIP("seccomp's strict mode is not available here");
    for (const Child& child : children())
    {
        INFO(child.name);
        const auto start = std::chrono::steady_clock::now();
        const auto pid = runIn(child, [] { return ::syscall(SYS_getpid); });
        const auto took = std::chrono::steady_clock::now() - start;
        INFO(pid.why());
        CHECK(pid.outcome() == Strict::killed);
        CHECK(!pid);
        CHECK(pid.why().find("strict mode kill") != std::string::npos);
        CHECK(took < kStrictLimit / 2);   // seen when it happened, not at the limit
        if (child.first != nullptr)
            CHECK(pid.why().find("kept its process alive") != std::string::npos);
        // An allocation beyond what the heap holds (64 MiB, past glibc's largest threshold for mmap) asks the
        // kernel; a small one may well not, which is why a proof counts the heap's calls as well.
        const auto allocated = runIn(child,
                                     []
                                     {
                                         void* volatile block = std::malloc(std::size_t{64} << 20);
                                         const bool got = block != nullptr;
                                         std::free(block);
                                         return got;
                                     });
        INFO(allocated.why());
        CHECK(allocated.outcome() == Strict::killed);
        CHECK(allocated.why().find("strict mode kill") != std::string::npos);
    }
}

TEST_CASE("strict mode: a body that never returns fails at the limit, and says it hung", "[strict]")
{
    if (!available())
        SKIP("seccomp's strict mode is not available here");
    for (const Child& child : children())
    {
        INFO(child.name);
        std::atomic<bool> spinning{true};
        const auto start = std::chrono::steady_clock::now();
        const auto hung = runIn(
            child,
            [&spinning]
            {
                while (spinning.load(std::memory_order_relaxed))
                {
                }
                return 1;
            },
            300ms);
        const auto took = std::chrono::steady_clock::now() - start;
        INFO(hung.why());
        CHECK(hung.outcome() == Strict::late);
        CHECK(hung.why().find("hung") != std::string::npos);
        CHECK(took >= 300ms);
        CHECK(took < 20s);
    }
}

// A proof written with inStrictMode, which must fail its test case: [!shouldfail] passes when it does.
TEST_CASE("strict mode: a proof whose body makes a system call it does not allow fails", "[strict][!shouldfail]")
{
    if (!available())
        SKIP("seccomp's strict mode is not available here");
    if (const auto pid = inStrictMode([] { return ::syscall(SYS_getpid); }))
        CHECK(*pid > 0);
}

TEST_CASE("strict mode: a proof whose body hangs fails", "[strict][!shouldfail]")
{
    if (!available())
        SKIP("seccomp's strict mode is not available here");
    std::atomic<bool> spinning{true};
    const auto spin = [&spinning]
    {
        while (spinning.load(std::memory_order_relaxed))
        {
        }
        return 1;
    };
    if (const auto sealed = inStrictMode(spin, 300ms))
        CHECK(*sealed == 1);
}
#else
TEST_CASE("strict mode: seccomp is Linux's, and a proof here warns that it checks nothing", "[strict]")
{
    const auto sealed = inStrictMode([] { return 1; });
    CHECK(sealed.outcome() == Strict::unavailable);
    CHECK(!sealed);
}
#endif

TEST_CASE("HeapCalls counts what a body allocates, where it counts", "[strict]")
{
    // The other half of a wait-free proof: a hundred rounds that allocate, counted as the proofs count theirs.
    const auto allocate = []
    {
        void* volatile block = std::malloc(16);
        std::free(block);
        return block != nullptr;
    };
    if (HeapCalls::counted())
    {
        long calls = 0;
        bool same = true;
        {
            const HeapCalls counting;
            for (int i = 0; i < 100; ++i)
                same = same && allocate();
            calls = counting.count();
        }
        CHECK(same);
        CHECK(calls >= 200);   // a malloc and a free a round: a proof's CHECK(calls == 0) fails
        long none = -1;
        {
            const HeapCalls counting;
            none = counting.count();
        }
        CHECK(none == 0);
    }
    else
        WARN("this build does not count the heap's calls (a sanitizer): allocation is not checked");
}
