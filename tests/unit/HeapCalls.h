// tests/unit/HeapCalls.h: what proves that a runtime function allocates nothing, where it can be proved. HeapCalls
// counts the calls of the C heap's functions (malloc, calloc, realloc, free, aligned_alloc, posix_memalign, memalign,
// and C++'s new and delete, which call them) that this thread makes while it is alive: HeapCalls.cpp defines them in
// fminus-tests, each counting and then calling glibc's own, which the runtime's code, linked into the same executable,
// calls too. Only on Linux with glibc, and not under a sanitizer, which has its own heap: elsewhere counted() is false,
// and a test that needs the count skips.
#pragma once

namespace fminus::test
{
    class HeapCalls
    {
    public:
        HeapCalls();
        ~HeapCalls();
        HeapCalls(const HeapCalls&) = delete;
        HeapCalls& operator=(const HeapCalls&) = delete;

        long count() const;   // the calls this thread made since this began counting

        static bool counted();   // whether this build counts them at all
    };
} // namespace fminus::test
