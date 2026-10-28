// src/runtime/Parallel.cpp: the pool behind FMinus's parallel for and parallel { } (Runtime.h; ir/Parallel.cpp lowers
// both to fminus_rt_parallel_for). One pool for the process, shared by every program in it: a thread for each core but
// one, made by the first fminus_rt_parallel_start, off the audio thread, and joined by the last fminus_rt_parallel_stop.
//
// A region is a task (an outlined body), its context and a range of iterations, begin to end - 1. Its caller publishes
// it, takes chunks of it itself, and returns once every chunk has run. The pool runs one region at a time: a caller
// that finds it held (by a host's other audio thread, or by the region it is itself running a chunk of) runs its own
// region alone, on its own thread. So nothing ever waits for the pool, and a region inside a region never deadlocks.
//
// The protocol, on a handful of atomics and no lock:
//
//   publish   the caller holding the pool (`busy`) writes the region's fields, then the cursor, generation << 32 | 0,
//             the next iteration to take: a store that releases the fields to whoever reads the cursor. Then it bumps
//             `signal`, which idle threads watch, and wakes as many of those asleep as the region has seats left for.
//   take      a thread takes the chunk [next, next + grain) with a compare-and-swap of the cursor that also checks the
//             generation, and runs it; then the next, until none is left. Chunks are taken as threads come free, so a
//             slow core (an efficiency core, a thread the system stopped) holds up only the chunk it has.
//   join      a pool thread counts itself `inside`, then reads the cursor: a closed one (kClosed) has nothing for it,
//             and it leaves without reading anything else. An open one's fields are safe to read: the region's caller
//             writes the next region's only once no thread is inside.
//   close     when the caller finds no chunk left, it closes the cursor, so no thread joins any more, and waits, spinning,
//             for the threads still inside: those running the last chunks. Then it lets go of the pool.
//
// The counter and the cursor are sequentially consistent, which is what makes join and close safe: a thread counted
// inside before the caller looks at the count is waited for, and one counted after it finds the cursor closed.
//
// Pool threads run in the caller's floating-point mode: each reads the caller's control register from the region and
// sets its own to it while it runs chunks (FPCR on arm64: flush-to-zero, the rounding mode; MXCSR on x86-64, DAZ too),
// so an iteration computes on any thread what it would on the caller's. That is what Denormals::Flush needs, whose code
// assumes the mode its entry point set.
//
// Waiting. A pool thread that has taken a seat in a region, or that a caller woke for one, spins on `signal` for
// kSpinBeforeSleeping, so regions that follow one another closely (two in one block) start at once. Then it sleeps, on
// a word of its own, its bed (std::atomic::wait: a futex, __ulock on macOS), so that a caller can wake one thread and
// no other. A region seats `seats` pool threads: the caller counts the threads still awake, which will look at the
// region themselves, and wakes only as many sleepers as the seats left, the first beds first, a system call each that
// does not block. It does not wait for them to wake, and starts on its chunks at once, so a region that is over before
// they are up costs it only those calls. A thread that finds no seat in a region (more were awake than it seats) goes
// on spinning only for what was left of its time, and sleeps: a region of two iterations keeps one pool thread busy,
// not every one. On macOS the threads run at the user-interactive quality of service, the highest a thread can ask
// for; they are not real-time threads, and an audio workgroup they do not join may run them late, while the caller
// takes their chunks.
//
// Sleeping, and waking, on a bed, whose state is kAwake, kAsleep or kWoken (sequentially consistent, as `signal`):
//
//   sleep     the thread sets its bed kAsleep, then looks at `signal` again: a region published since it last looked
//             it joins without sleeping; otherwise it waits while the bed is kAsleep.
//   wake      the caller bumps `signal`, then looks at the beds: one kAsleep it sets kWoken with a compare-and-swap,
//             and notifies. Either the caller sees the bed kAsleep and wakes the thread, or the thread, which set it
//             before looking, sees the new `signal`: no thread sleeps through a region it was counted for.
//   stop      as wake, for every bed, after `stopping`.
#include "runtime/Runtime.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <new>
#include <thread>
#include <vector>

#if defined(__APPLE__)
#include <pthread.h>
#include <pthread/qos.h>
#elif defined(__linux__)
#include <pthread.h>
#endif
#if defined(__x86_64__) || defined(_M_X64)
#include <immintrin.h>
#endif

namespace
{
    // A cursor's iteration once its region is closed: no thread takes a chunk of it, or joins it, any more.
    constexpr std::uint32_t kClosed = 0xFFFFFFFFu;

    // The most iterations the cursor counts in one region; a longer range runs as two regions, one after the other.
    constexpr std::int64_t kMostIterations = std::int64_t{1} << 31;

    // A chunk is about a quarter of a thread's share: small enough that threads finishing early take more, large
    // enough that the compare-and-swap that takes one stays rare.
    constexpr std::int64_t kChunksPerThread = 4;

    // How long a pool thread spins for the next region before it sleeps: from when it last took a seat in one, or was
    // woken for one.
    constexpr std::chrono::microseconds kSpinBeforeSleeping{50};

    // A bed's states (the top of this file).
    constexpr std::uint32_t kAwake = 0;    // running, or spinning on `signal`
    constexpr std::uint32_t kAsleep = 1;   // asleep on the bed, or about to be: a caller may wake it
    constexpr std::uint32_t kWoken = 2;    // a caller (or the stop) has woken it

    // How long the caller spins (pauses) for the last chunks before it yields its core between looks: a thread
    // running one may be waiting for the core.
    constexpr unsigned kPausesBeforeYielding = 1u << 14;

    // One step of a spin: tells the core the loop is waiting (arm64's YIELD, x86's PAUSE).
    inline void pause() noexcept
    {
#if defined(__aarch64__)
        asm volatile("yield" ::: "memory");
#elif defined(_M_ARM64)
        __yield();
#elif defined(__x86_64__) || defined(_M_X64)
        _mm_pause();
#endif
    }

    // The floating-point control register: FPCR on arm64, MXCSR on x86-64. Elsewhere 0, and each thread keeps its own.
    std::uint64_t floatMode() noexcept
    {
#if defined(__aarch64__)
        std::uint64_t mode = 0;
        asm volatile("mrs %0, fpcr" : "=r"(mode) : : "memory");
        return mode;
#elif defined(__x86_64__) || defined(_M_X64)
        return _mm_getcsr();
#else
        return 0;
#endif
    }

    void setFloatMode(std::uint64_t mode) noexcept
    {
#if defined(__aarch64__)
        asm volatile("msr fpcr, %0" : : "r"(mode) : "memory");
#elif defined(__x86_64__) || defined(_M_X64)
        _mm_setcsr(static_cast<unsigned>(mode));
#else
        (void)mode;
#endif
    }

    // A pool thread's own word to sleep on, on a cache line of its own, so that a caller wakes the threads it wants
    // and no others.
    struct alignas(64) Bed
    {
        std::atomic<std::uint32_t> state{kAwake};
    };

    struct Pool
    {
        // The region, as its caller wrote it: read by a pool thread only while it is inside (the top of this file).
        std::atomic<fminus_rt_task> task{nullptr};
        std::atomic<void*> context{nullptr};
        std::atomic<std::int32_t> begin{0};
        std::atomic<std::uint32_t> count{0};   // iterations
        std::atomic<std::uint32_t> grain{1};   // a chunk's iterations
        std::atomic<std::int32_t> seats{0};    // the pool threads that may join: the region's `threads`, less one
        std::atomic<std::uint64_t> mode{0};    // the caller's floating-point mode

        std::atomic<std::uint64_t> cursor{kClosed};   // generation << 32 | the next iteration, from begin
        std::atomic<std::int32_t> inside{0};          // pool threads in the region
        std::atomic<std::uint32_t> signal{0};         // bumped for each region (and to stop): what idle threads watch
        std::atomic<bool> busy{false};                // a caller holds the pool
        std::atomic<bool> stopping{false};
        std::atomic<std::int32_t> workers{0};         // pool threads running, for a caller's look
        std::uint32_t generation = 0;                 // the last region's: the caller holding the pool writes it

        // A bed for each thread the pool may make, made with the pool and never freed: a caller that read `workers`
        // before a stop looks at beds still there.
        std::unique_ptr<Bed[]> beds;
        std::size_t bedCount = 0;

        // Under `made` (below): starting and stopping.
        std::vector<std::thread> threads;
        int users = 0;
    };

    // The pool, made by the first start and never destroyed, so that a thread still running at exit finds it whole.
    std::atomic<Pool*> thePool{nullptr};
    std::mutex made;   // start and stop

    // Whether this thread is running a chunk of a region the pool runs: a region it starts then runs here, alone.
    thread_local bool inRegion = false;

    // Takes chunks of region `generation` and runs them, until none is left (or the region is another's).
    void take(Pool& pool, std::uint32_t generation, fminus_rt_task task, void* context, std::int32_t begin,
              std::uint32_t count, std::uint32_t grain) noexcept
    {
        std::uint64_t cursor = pool.cursor.load(std::memory_order_relaxed);
        for (;;)
        {
            const auto next = static_cast<std::uint32_t>(cursor);
            if (static_cast<std::uint32_t>(cursor >> 32) != generation || next >= count)   // kClosed is past any count
                return;
            const std::uint32_t upto = next + std::min(grain, count - next);
            const std::uint64_t taken = (cursor & ~std::uint64_t{0xFFFFFFFFu}) | upto;
            if (pool.cursor.compare_exchange_weak(cursor, taken, std::memory_order_relaxed, std::memory_order_relaxed))
            {
                task(context, static_cast<std::int32_t>(std::int64_t{begin} + next),
                     static_cast<std::int32_t>(std::int64_t{begin} + upto));
                cursor = pool.cursor.load(std::memory_order_relaxed);
            }
        }
    }

    // A pool thread's part in the region its caller published, if there is still room in it, and chunks; whether it
    // took a seat. A thread that comes when the region is closed already leaves without counting itself, which its
    // caller would wait for.
    bool join(Pool& pool) noexcept
    {
        if (static_cast<std::uint32_t>(pool.cursor.load(std::memory_order_acquire)) == kClosed)
            return false;
        const std::int32_t before = pool.inside.fetch_add(1, std::memory_order_seq_cst);
        const std::uint64_t cursor = pool.cursor.load(std::memory_order_seq_cst);
        if (static_cast<std::uint32_t>(cursor) == kClosed || before >= pool.seats.load(std::memory_order_relaxed))
        {
            pool.inside.fetch_sub(1, std::memory_order_release);
            return false;
        }
        const fminus_rt_task task = pool.task.load(std::memory_order_relaxed);
        void* context = pool.context.load(std::memory_order_relaxed);
        const std::int32_t begin = pool.begin.load(std::memory_order_relaxed);
        const std::uint32_t count = pool.count.load(std::memory_order_relaxed);
        const std::uint32_t grain = pool.grain.load(std::memory_order_relaxed);
        const std::uint64_t mode = pool.mode.load(std::memory_order_relaxed);
        const std::uint64_t own = floatMode();
        if (own != mode)
            setFloatMode(mode);
        inRegion = true;
        take(pool, static_cast<std::uint32_t>(cursor >> 32), task, context, begin, count, grain);
        inRegion = false;
        if (own != mode)
            setFloatMode(own);
        pool.inside.fetch_sub(1, std::memory_order_release);   // what the caller's acquire waits for
        return true;
    }

    // A pool thread: waits for a region, joins it, and waits for the next, until the pool stops. `seen` is `signal` as
    // it was when the thread was made: a thread the system runs late, after the pool has stopped, still sees the stop.
    void work(Pool& pool, Bed& bed, std::uint32_t seen) noexcept
    {
#if defined(__APPLE__)
        pthread_setname_np("fminus parallel");
        pthread_set_qos_class_self_np(QOS_CLASS_USER_INTERACTIVE, 0);
#elif defined(__linux__)
        pthread_setname_np(pthread_self(), "fminus parallel");
#endif
        auto until = std::chrono::steady_clock::now() + kSpinBeforeSleeping;
        for (;;)
        {
            std::uint32_t now = pool.signal.load(std::memory_order_acquire);
            for (unsigned spins = 1; now == seen; ++spins)
            {
                pause();
                now = pool.signal.load(std::memory_order_acquire);
                if (now == seen && spins % 64 == 0 && std::chrono::steady_clock::now() >= until)
                    break;
            }
            // Asleep, on the bed (the top of this file): marked before `signal` is looked at again, as a caller bumps
            // `signal` before it looks at the beds.
            bool woken = false;
            while (now == seen && !pool.stopping.load(std::memory_order_acquire))
            {
                bed.state.store(kAsleep, std::memory_order_seq_cst);
                now = pool.signal.load(std::memory_order_seq_cst);
                if (now == seen && !pool.stopping.load(std::memory_order_seq_cst))
                {
                    bed.state.wait(kAsleep, std::memory_order_seq_cst);
                    woken = true;
                }
                bed.state.store(kAwake, std::memory_order_relaxed);
                now = pool.signal.load(std::memory_order_acquire);
            }
            if (pool.stopping.load(std::memory_order_acquire))
                return;
            seen = now;
            // Spinning goes on after a seat, or a wake for one; a thread that found none keeps what time it had left.
            if (join(pool) || woken)
                until = std::chrono::steady_clock::now() + kSpinBeforeSleeping;
        }
    }

    // Wakes the sleepers a region has seats for, after its caller bumped `signal`: `seats` less the threads awake,
    // which look at the region themselves; the first beds first, so the same few threads take the small regions.
    void wake(Pool& pool, std::int32_t workers, std::int32_t seats) noexcept
    {
        const std::size_t beds = std::min(static_cast<std::size_t>(workers), pool.bedCount);
        std::int32_t wanted = seats;
        for (std::size_t i = 0; i < beds && wanted > 0; ++i)
        {
            if (pool.beds[i].state.load(std::memory_order_seq_cst) != kAsleep)
                --wanted;
        }
        for (std::size_t i = 0; i < beds && wanted > 0; ++i)
        {
            std::uint32_t asleep = kAsleep;
            if (pool.beds[i].state.compare_exchange_strong(asleep, kWoken, std::memory_order_seq_cst))
            {
                pool.beds[i].state.notify_one();
                --wanted;
            }
        }
    }
} // namespace

extern "C" void fminus_rt_parallel_for(fminus_rt_task task, void* context, std::int32_t begin, std::int32_t end,
                                       std::int32_t threads) noexcept
{
    if (end <= begin)
        return;
    const std::int64_t count = std::int64_t{end} - begin;
    if (count >= kMostIterations)
    {
        const auto middle = static_cast<std::int32_t>(begin + count / 2);
        fminus_rt_parallel_for(task, context, begin, middle, threads);
        fminus_rt_parallel_for(task, context, middle, end, threads);
        return;
    }
    // Alone, on this thread: without a pool (or one with no thread), with one thread allowed, for one iteration, inside
    // a chunk of a region the pool runs, or when another caller holds the pool.
    // No more pool threads than the iterations other than the caller's first.
    Pool* pool = thePool.load(std::memory_order_acquire);
    const std::int32_t workers = pool != nullptr ? pool->workers.load(std::memory_order_acquire) : 0;
    const auto seats = static_cast<std::int32_t>(
        std::min<std::int64_t>(threads <= 0 ? workers : std::min(workers, threads - 1), count - 1));
    bool free = false;
    if (seats <= 0 || count == 1 || inRegion
        || !pool->busy.compare_exchange_strong(free, true, std::memory_order_acquire, std::memory_order_relaxed))
    {
        task(context, begin, end);
        return;
    }

    // Publish.
    const std::uint32_t generation = ++pool->generation;
    const auto iterations = static_cast<std::uint32_t>(count);
    const auto grain =
        static_cast<std::uint32_t>(std::max<std::int64_t>(1, count / (std::int64_t{seats + 1} * kChunksPerThread)));
    pool->task.store(task, std::memory_order_relaxed);
    pool->context.store(context, std::memory_order_relaxed);
    pool->begin.store(begin, std::memory_order_relaxed);
    pool->count.store(iterations, std::memory_order_relaxed);
    pool->grain.store(grain, std::memory_order_relaxed);
    pool->seats.store(seats, std::memory_order_relaxed);
    pool->mode.store(floatMode(), std::memory_order_relaxed);
    pool->cursor.store(std::uint64_t{generation} << 32, std::memory_order_seq_cst);
    pool->signal.fetch_add(1, std::memory_order_seq_cst);
    wake(*pool, workers, seats);

    // Take chunks too, then close and wait for the last ones.
    inRegion = true;
    take(*pool, generation, task, context, begin, iterations, grain);
    inRegion = false;
    pool->cursor.store((std::uint64_t{generation} << 32) | kClosed, std::memory_order_seq_cst);
    for (unsigned looks = 0; pool->inside.load(std::memory_order_acquire) != 0; ++looks)
    {
        if (looks < kPausesBeforeYielding)
            pause();
        else
            std::this_thread::yield();
    }
    pool->busy.store(false, std::memory_order_release);
}

extern "C" std::int32_t fminus_rt_parallel_start() noexcept
{
    const std::lock_guard<std::mutex> lock(made);
    Pool* pool = thePool.load(std::memory_order_relaxed);
    const unsigned cores = std::thread::hardware_concurrency();
    const std::size_t wanted = cores > 1 ? cores - 1 : 0;
    if (pool == nullptr)
    {
        pool = new (std::nothrow) Pool;
        if (pool == nullptr)
            return 1;
        pool->beds.reset(new (std::nothrow) Bed[wanted]);
        pool->bedCount = pool->beds != nullptr ? wanted : 0;
        thePool.store(pool, std::memory_order_release);
    }
    if (pool->users++ == 0)
    {
        try
        {
            while (pool->threads.size() < pool->bedCount)
            {
                Bed& bed = pool->beds[pool->threads.size()];
                bed.state.store(kAwake, std::memory_order_relaxed);
                pool->threads.emplace_back(work, std::ref(*pool), std::ref(bed),
                                           pool->signal.load(std::memory_order_acquire));
            }
        }
        catch (...)
        {
            // Fewer threads than cores (the system would make no more): regions run on those there are.
        }
        pool->workers.store(static_cast<std::int32_t>(pool->threads.size()), std::memory_order_release);
    }
    return pool->workers.load(std::memory_order_relaxed) + 1;
}

extern "C" void fminus_rt_parallel_stop() noexcept
{
    const std::lock_guard<std::mutex> lock(made);
    Pool* pool = thePool.load(std::memory_order_relaxed);
    if (pool == nullptr || pool->users == 0 || --pool->users > 0)
        return;
    // A caller from now on runs alone; one running a region finishes it before the pool is taken from it.
    pool->workers.store(0, std::memory_order_release);
    for (bool free = false;
         !pool->busy.compare_exchange_weak(free, true, std::memory_order_acquire, std::memory_order_relaxed);
         free = false)
        std::this_thread::yield();
    pool->stopping.store(true, std::memory_order_release);
    pool->signal.fetch_add(1, std::memory_order_seq_cst);
    for (std::size_t i = 0; i < pool->threads.size(); ++i)
    {
        Bed& bed = pool->beds[i];
        if (bed.state.exchange(kWoken, std::memory_order_seq_cst) == kAsleep)
            bed.state.notify_one();
    }
    for (std::thread& thread : pool->threads)
        thread.join();
    pool->threads.clear();
    pool->stopping.store(false, std::memory_order_relaxed);
    pool->busy.store(false, std::memory_order_release);
}
