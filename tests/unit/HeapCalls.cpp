// tests/unit/HeapCalls.cpp: the C heap's functions, counted (HeapCalls.h). An executable's own malloc is the one every
// object in the process calls, as LD_PRELOAD's would be; each of these counts the call when this thread is counting
// and then calls glibc's own (__libc_malloc and the rest, which glibc exports for exactly this). The counts are the
// thread's, so another thread's allocations (Catch2's, LLVM's) never reach a test's. (memalign is glibc's, declared in
// <malloc.h>, which nothing here includes.)
#include "HeapCalls.h"

#include <cstddef>

#if defined(__has_feature)
#if __has_feature(address_sanitizer) || __has_feature(thread_sanitizer) || __has_feature(memory_sanitizer)
#define FMINUS_TEST_SANITIZED 1
#endif
#endif
#if defined(__SANITIZE_ADDRESS__) || defined(__SANITIZE_THREAD__)
#define FMINUS_TEST_SANITIZED 1
#endif

#if defined(__linux__) && defined(__GLIBC__) && !defined(FMINUS_TEST_SANITIZED)
#define FMINUS_TEST_HEAP_CALLS 1
#include <cerrno>
#include <cstdlib>   // C's declarations of these, which the definitions below match (noexcept)

extern "C"
{
    void* __libc_malloc(std::size_t size);
    void* __libc_calloc(std::size_t count, std::size_t size);
    void* __libc_realloc(void* memory, std::size_t size);
    void __libc_free(void* memory);
    void* __libc_memalign(std::size_t alignment, std::size_t size);
}

namespace
{
    // Plain, initial-exec thread-locals of the executable: set up before any code of its runs, and read without a call.
    thread_local int tCounting = 0;
    thread_local long tCalls = 0;

    void counted()
    {
        if (tCounting > 0)
            ++tCalls;
    }
} // namespace

extern "C"
{
    void* malloc(std::size_t size) noexcept
    {
        counted();
        return __libc_malloc(size);
    }

    void* calloc(std::size_t count, std::size_t size) noexcept
    {
        counted();
        return __libc_calloc(count, size);
    }

    void* realloc(void* memory, std::size_t size) noexcept
    {
        counted();
        return __libc_realloc(memory, size);
    }

    void free(void* memory) noexcept
    {
        counted();
        __libc_free(memory);
    }

    void* aligned_alloc(std::size_t alignment, std::size_t size) noexcept
    {
        counted();
        return __libc_memalign(alignment, size);
    }

    void* memalign(std::size_t alignment, std::size_t size) noexcept
    {
        counted();
        return __libc_memalign(alignment, size);
    }

    int posix_memalign(void** memory, std::size_t alignment, std::size_t size) noexcept
    {
        counted();
        if (alignment < sizeof(void*) || (alignment & (alignment - 1)) != 0)
            return EINVAL;
        void* block = __libc_memalign(alignment, size);
        if (block == nullptr)
            return ENOMEM;
        *memory = block;
        return 0;
    }
}
#endif

namespace fminus::test
{
#if defined(FMINUS_TEST_HEAP_CALLS)
    HeapCalls::HeapCalls()
    {
        if (tCounting++ == 0)
            tCalls = 0;
    }

    HeapCalls::~HeapCalls()
    {
        --tCounting;
    }

    long HeapCalls::count() const
    {
        return tCalls;
    }

    bool HeapCalls::counted()
    {
        return true;
    }
#else
    HeapCalls::HeapCalls() = default;
    HeapCalls::~HeapCalls() = default;

    long HeapCalls::count() const
    {
        return 0;
    }

    bool HeapCalls::counted()
    {
        return false;
    }
#endif
} // namespace fminus::test
