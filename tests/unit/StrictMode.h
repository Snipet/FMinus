// tests/unit/StrictMode.h: what proves that code makes no system call, where it can be proved. inStrictMode(body)
// runs body in a child process under seccomp's strict mode, in which any system call but read, write, exit and
// sigreturn kills the thread that makes it, and gives back what body returned there. The child writes the result to a
// pipe and leaves with the raw exit; the parent waits for it, up to a limit, then kills the child and reaps it, so a
// child that another thread keeps alive never stalls the test: ThreadSanitizer starts a thread of its own in a child
// forked from a process with one thread, strict mode (set with prctl) binds only the thread that set it, and the raw
// exit, like the kill of a thread that broke the mode, ends only that thread. The proof holds when the result arrived
// and is the one expected; a child whose thread ended without it (strict mode killed it: a system call it does not
// allow) or sent nothing within the limit (the body hung) fails it, in words that say which. The kernel clears a word
// the parent shares (set_tid_address) when the child's thread ends while another lives on, so a broken mode is seen at
// once in every build. Linux only: elsewhere, and where the kernel refuses strict mode, nothing is proved, and
// inStrictMode warns, as the proofs did before it.
//
// Strict mode sees system calls, not allocations: one the heap serves from what it has makes none. That is HeapCalls'
// half of a wait-free proof (HeapCalls.h), counted in the parent; this is the other.
#pragma once

#include <chrono>
#include <cstddef>
#include <memory>
#include <source_location>
#include <string>
#include <type_traits>
#include <utility>

namespace fminus::test
{
    // How a body run in strict mode ended.
    enum class Strict
    {
        returned,      // its result arrived whole
        unavailable,   // no strict mode here (another system, or a kernel that refuses it): nothing is proved
        killed,        // the child's thread ended before its result arrived: strict mode killed it, or a signal did
        late,          // no result within the limit, the child's thread still running: the body hung
        failed,        // the child could not be started (a pipe, a page, a fork), or its result could not be read
    };

    // Long enough for any body a proof runs, on a loaded machine and under a sanitizer; a thread strict mode killed is
    // seen at once, so only a body that hangs waits this long.
    inline constexpr std::chrono::milliseconds kStrictLimit{30000};

    // What a body returned in strict mode, or why it did not.
    template <typename T>
    class Sealed
    {
    public:
        Sealed(Strict outcome, std::string why, const T& value)
            : outcome_(outcome)
            , why_(std::move(why))
            , value_(value)
        {
        }

        Strict outcome() const
        {
            return outcome_;
        }
        // What happened, in words: empty when it returned.
        const std::string& why() const
        {
            return why_;
        }
        bool available() const
        {
            return outcome_ != Strict::unavailable;
        }
        bool returned() const
        {
            return outcome_ == Strict::returned;
        }
        explicit operator bool() const
        {
            return returned();
        }
        const T& operator*() const
        {
            return value_;
        }
        const T* operator->() const
        {
            return &value_;
        }

    private:
        Strict outcome_;
        std::string why_;
        T value_;
    };

    namespace detail
    {
        struct StrictEnd
        {
            Strict outcome;
            std::string why;
        };

        // StrictMode.cpp. Forks; the child runs first() if there is one, enters strict mode, runs run(body, out) and
        // writes `size` bytes from `out` to the pipe, then leaves with the raw exit. The parent reads them into `out`.
        StrictEnd runStrict(void (*run)(const void* body, void* out) noexcept, const void* body, void* out,
                            std::size_t size, std::chrono::milliseconds limit, void (*first)());

        // WARN where strict mode is unavailable; a failed check with the reason where the result did not arrive.
        void reportStrict(Strict outcome, const std::string& why, const std::source_location& where);
    }   // namespace detail

    // body() in a child under strict mode: what it returned, or why nothing did. Reports nothing; inStrictMode is the
    // proof. `first` runs in the child before strict mode: the helper's own test starts a thread there, as
    // ThreadSanitizer's runtime does.
    template <typename Body>
    auto runInStrictMode(Body&& body, std::chrono::milliseconds limit = kStrictLimit,
                         void (*first)() = nullptr) -> Sealed<std::remove_cvref_t<std::invoke_result_t<Body&>>>
    {
        using T = std::remove_cvref_t<std::invoke_result_t<Body&>>;
        using Run = std::remove_reference_t<Body>;
        static_assert(std::is_trivially_copyable_v<T>, "a body's result crosses a pipe as its bytes");
        // noexcept: an exception that left the body would end the child here, never unwind into the test runner.
        const auto call = [](const void* run, void* out) noexcept
        {
            *static_cast<T*>(out) = (*const_cast<Run*>(static_cast<const Run*>(run)))();
        };
        T value{};
        detail::StrictEnd end =
            detail::runStrict(call, std::addressof(body), std::addressof(value), sizeof value, limit, first);
        return Sealed<T>(end.outcome, std::move(end.why), value);
    }

    // The proof: runInStrictMode, reported. Where strict mode is not available it warns, and where the result did not
    // arrive it fails a check that says why (the caller's line in its words); what the body returned is the caller's
    // to check. `if (const auto sum = inStrictMode(readAll)) CHECK(*sum == expected);`
    template <typename Body>
    auto inStrictMode(Body&& body, std::chrono::milliseconds limit = kStrictLimit,
                      const std::source_location where = std::source_location::current())
    {
        auto sealed = runInStrictMode(std::forward<Body>(body), limit);
        detail::reportStrict(sealed.outcome(), sealed.why(), where);
        return sealed;
    }
}   // namespace fminus::test
