// tests/library/ProfileTest.cpp: where a Program's machine code is, Program::codeRanges and codeRangeAt, as a host's
// sampling profiler uses them (C++17, FMinus::FMinus alone): each range's function, module and kind, at the address
// address() gives; the list whole after compile(); a thunk's range once callable() has made it; a program whose code
// does not link; and a reference sampler, which interrupts the thread that runs a program, maps each program counter to
// its range, and finds the program's time where it spends it: in each function's own code at -O0, and from -O1 in the
// caller a function was inlined into, whose range lists it.
#include "fminus/FMinus.h"

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <map>
#include <memory>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

#if defined(_WIN32)
#include <malloc.h>   // _aligned_malloc, _aligned_free
#else
#include <pthread.h>
#include <signal.h>
#if defined(__APPLE__)
#include <sys/ucontext.h>   // <ucontext.h>'s routines are deprecated there; its type is all a handler needs
#else
#include <ucontext.h>
#endif
#endif

// ThreadSanitizer holds a signal back until the thread next runs instrumented code: never the JIT's.
#if defined(__SANITIZE_THREAD__)
#define FMINUS_TEST_TSAN 1
#elif defined(__has_feature)
#if __has_feature(thread_sanitizer)
#define FMINUS_TEST_TSAN 1
#endif
#endif

namespace
{
    using fminus::Program;
    using Kind = Program::CodeRange::Kind;
    using Names = std::vector<std::string>;

    // An instance of a module, as a host makes one: aligned, zeroed, the module's size (FMinus.h, Module).
    struct Instance
    {
        explicit Instance(const Program::Module& module)
        {
            const std::size_t alignment = module.alignment < alignof(std::max_align_t) ? alignof(std::max_align_t)
                                                                                       : module.alignment;
            const std::size_t size = (module.size + alignment - 1) / alignment * alignment;
#if defined(_WIN32)
            memory = _aligned_malloc(size, alignment);
#else
            memory = std::aligned_alloc(alignment, size);
#endif
            std::memset(memory, 0, size);
            Program::setDefaults(memory, module);
        }
        ~Instance()
        {
#if defined(_WIN32)
            _aligned_free(memory);
#else
            std::free(memory);
#endif
        }
        Instance(const Instance&) = delete;
        Instance& operator=(const Instance&) = delete;

        void* memory = nullptr;
    };

    // An oscillator whose process inlines its tick, a function a parallel region calls, an entry point (which under
    // Denormals::Flush is a few instructions around its body), and a sinf whose argument may be large (FMinus's own
    // path for it, fminus.large.sin).
    const char* const kSource = "module Osc\n"
                                "{\n"
                                "    float phase;\n"
                                "    param float freq [[fminus::default(0.01)]];\n"
                                "\n"
                                "    float tick()\n"
                                "    {\n"
                                "        phase = phase + freq[0];\n"
                                "        if (phase >= 1.0f)\n"
                                "            phase = phase - 1.0f;\n"
                                "        return sinf(phase * 6.2831853f + freq[0] * 1000.0f);\n"
                                "    }\n"
                                "\n"
                                "    void process(float* out, int n) [[clang::nonblocking]]\n"
                                "    {\n"
                                "        for (int i = 0; i < n; i++)\n"
                                "            out[i] = tick();\n"
                                "    }\n"
                                "};\n"
                                "\n"
                                "float shape(float x)\n"
                                "{\n"
                                "    return x / (1.0f + x * x);\n"
                                "}\n"
                                "\n"
                                "void run(Osc* o, float* out, int n) [[clang::nonblocking]]\n"
                                "{\n"
                                "    o->process(out, n);\n"
                                "    parallel for (int i = 0; i < n; i++)\n"
                                "        out[i] = shape(out[i]);\n"
                                "}\n";

    std::unique_ptr<Program> compiled(const char* source, const Program::Options& options = {})
    {
        std::string errors;
        auto program = Program::compile(source, errors, options);
        INFO(errors);
        REQUIRE(program != nullptr);
        return program;
    }

    std::vector<Program::CodeRange> rangesOf(Program& program)
    {
        std::string errors;
        std::vector<Program::CodeRange> ranges = program.codeRanges(errors);
        INFO(errors);
        REQUIRE(errors.empty());
        return ranges;
    }

    const Program::CodeRange* named(const std::vector<Program::CodeRange>& ranges, const std::string& symbol)
    {
        const auto found = std::find_if(ranges.begin(), ranges.end(),
                                        [&](const Program::CodeRange& range) { return range.symbol == symbol; });
        return found != ranges.end() ? &*found : nullptr;
    }

    std::string listed(const std::vector<Program::CodeRange>& ranges)
    {
        std::ostringstream text;
        for (const Program::CodeRange& range : ranges)
            text << range.symbol << " (" << range.size << " bytes) ";
        return text.str();
    }
} // namespace

TEST_CASE("codeRanges: each range's function, module and kind, where address() finds it, whole after compile()",
          "[library][profile]")
{
    Program::Options options;
    options.denormals = Program::Denormals::Flush;   // run, an entry point, is then its body and a few instructions
    auto program = compiled(kSource, options);

    // No function has been looked up: codeRanges links the program's code, all of it.
    const std::vector<Program::CodeRange> ranges = rangesOf(*program);
    INFO(listed(ranges));
    REQUIRE(ranges.size() >= 7);
    for (std::size_t i = 1; i < ranges.size(); ++i)
        CHECK(ranges[i - 1].address + ranges[i - 1].size <= ranges[i].address);   // sorted, and apart

    std::string errors;
    const Program::CodeRange* tick = named(ranges, "Osc::tick");
    REQUIRE(tick != nullptr);
    CHECK(tick->kind == Kind::Function);
    CHECK(tick->function == "Osc::tick");
    CHECK(tick->module == "Osc");
    CHECK(tick->address == program->address("Osc::tick", errors));
    CHECK(tick->size > 0);
    CHECK(tick->inlined.empty());

    const Program::CodeRange* process = named(ranges, "Osc::process");   // no entry point: run calls it
    REQUIRE(process != nullptr);
    CHECK(process->kind == Kind::Function);
    CHECK(process->function == "Osc::process");
    CHECK(process->module == "Osc");
    CHECK(process->inlined == Names{"Osc::tick"});

    const Program::CodeRange* entry = named(ranges, "run");
    REQUIRE(entry != nullptr);
    CHECK(entry->kind == Kind::EntryPoint);
    CHECK(entry->function == "run");
    CHECK(entry->module.empty());
    CHECK(entry->address == program->address("run", errors));

    const Program::CodeRange* body = named(ranges, "run.body");
    REQUIRE(body != nullptr);
    CHECK(body->kind == Kind::Function);
    CHECK(body->function == "run");
    CHECK(body->inlined == Names{"Osc::process", "Osc::tick"});   // process's body, which holds tick, inlined

    const Program::CodeRange* region = named(ranges, "run.parallel.1");
    REQUIRE(region != nullptr);
    CHECK(region->kind == Kind::Parallel);
    CHECK(region->function == "run");
    CHECK(region->inlined == Names{"shape"});

    const Program::CodeRange* own = named(ranges, "fminus.large.sin");
    REQUIRE(own != nullptr);
    CHECK(own->kind == Kind::Support);
    CHECK(own->function.empty());
    CHECK(own->module.empty());

    SECTION("codeRangeAt finds the range that holds an address, and none outside them")
    {
        CHECK(Program::codeRangeAt(ranges, tick->address) == tick);
        CHECK(Program::codeRangeAt(ranges, tick->address + tick->size - 1) == tick);
        const Program::CodeRange* next = Program::codeRangeAt(ranges, tick->address + tick->size);
        CHECK(next != tick);
        CHECK(Program::codeRangeAt(ranges, ranges.front().address - 1) == nullptr);
        CHECK(Program::codeRangeAt(ranges, ranges.back().address + ranges.back().size) == nullptr);
        CHECK(Program::codeRangeAt(ranges, 0) == nullptr);
        CHECK(Program::codeRangeAt({}, tick->address) == nullptr);
        for (const Program::CodeRange& range : ranges)
            CHECK(Program::codeRangeAt(ranges, range.address + range.size / 2) == &range);
    }

    SECTION("the code stays where it is, and a thunk callable() makes is listed from then on")
    {
        REQUIRE(named(ranges, "fminus.call.shape") == nullptr);
        REQUIRE(program->callable("shape", errors) != nullptr);
        const std::vector<Program::CodeRange> again = rangesOf(*program);
        INFO(listed(again));
        CHECK(again.size() == ranges.size() + 1);
        for (const Program::CodeRange& range : ranges)
        {
            const Program::CodeRange* same = named(again, range.symbol);
            REQUIRE(same != nullptr);
            CHECK(same->address == range.address);
            CHECK(same->size == range.size);
        }
        const Program::CodeRange* thunk = named(again, "fminus.call.shape");
        REQUIRE(thunk != nullptr);
        CHECK(thunk->kind == Kind::Thunk);
        CHECK(thunk->function == "shape");
    }
}

TEST_CASE("codeRanges: the optimiser's inlining reads the same when compile() optimises (keepIr), and is none at -O0",
          "[library][profile][ir]")
{
    auto plain = compiled(kSource);
    Program::Options kept;
    kept.keepIr = Program::KeepIr::Optimised;
    auto optimised = compiled(kSource, kept);
    const std::vector<Program::CodeRange> a = rangesOf(*plain);
    const std::vector<Program::CodeRange> b = rangesOf(*optimised);
    REQUIRE(a.size() == b.size());
    for (std::size_t i = 0; i < a.size(); ++i)
    {
        INFO(a[i].symbol);
        CHECK(a[i].symbol == b[i].symbol);
        CHECK(a[i].inlined == b[i].inlined);
        CHECK(a[i].size == b[i].size);   // the same machine code
    }
    const Program::CodeRange* run = named(a, "run");
    REQUIRE(run != nullptr);
    CHECK(run->kind == Kind::Function);   // no entry point's wrapper without Denormals::Flush
    CHECK(run->inlined == Names{"Osc::process", "Osc::tick"});

    Program::Options none;
    none.optimisation = 0;
    auto unoptimised = compiled(kSource, none);
    for (const Program::CodeRange& range : rangesOf(*unoptimised))
    {
        INFO(range.symbol);
        CHECK(range.inlined.empty());
    }
}

TEST_CASE("codeRanges: a program whose code does not link has none, and says why; one with no functions has none",
          "[library][profile]")
{
    auto unlinked = compiled("float nowhere(float x);\n"
                             "float g(float x) { return nowhere(x); }\n");
    std::string errors;
    CHECK(unlinked->codeRanges(errors).empty());
    CHECK(!errors.empty());   // LLVM's words: "Failed to materialize symbols: ..."

    auto empty = compiled("module Gain\n"
                          "{\n"
                          "    float level;\n"
                          "};\n");
    CHECK(empty->codeRanges(errors).empty());
    CHECK(errors.empty());
}

#if !defined(_WIN32) && !defined(FMINUS_TEST_TSAN)
namespace
{
    // The reference sampler: another thread interrupts the thread that runs the program with a signal every 200 µs,
    // and the handler stores the program counter the signal interrupted, read from the context the kernel saved. It
    // only stores, into memory set aside before: no allocation and no lock in a signal handler. (On Windows a sampler
    // suspends the thread instead, SuspendThread, and reads GetThreadContext's Rip; macOS's thread_get_state does the
    // same for a thread of the process.)
    constexpr std::size_t kSamples = 400;
    std::atomic<std::uintptr_t> sampled[kSamples];
    std::atomic<std::size_t> taken{0};

    std::uintptr_t programCounter(const ucontext_t& context)
    {
#if defined(__APPLE__) && defined(__aarch64__)
        return static_cast<std::uintptr_t>(__darwin_arm_thread_state64_get_pc(context.uc_mcontext->__ss));
#elif defined(__APPLE__) && defined(__x86_64__)
        return static_cast<std::uintptr_t>(context.uc_mcontext->__ss.__rip);
#elif defined(__linux__) && defined(__x86_64__)
        return static_cast<std::uintptr_t>(context.uc_mcontext.gregs[REG_RIP]);
#elif defined(__linux__) && defined(__aarch64__)
        return static_cast<std::uintptr_t>(context.uc_mcontext.pc);
#else
        (void)context;
        return 0;
#endif
    }

    void onSample(int, siginfo_t*, void* context)
    {
        const std::size_t at = taken.load(std::memory_order_relaxed);
        if (at >= kSamples)
            return;
        sampled[at].store(programCounter(*static_cast<const ucontext_t*>(context)), std::memory_order_relaxed);
        taken.store(at + 1, std::memory_order_release);
    }

    // Runs `work` on this thread, again and again, while another thread samples it, until kSamples samples (or 30 s):
    // each one's program counter.
    std::vector<std::uintptr_t> sample(const std::function<void()>& work)
    {
        taken.store(0);
        struct sigaction action = {};
        action.sa_sigaction = onSample;
        action.sa_flags = SA_SIGINFO | SA_RESTART;
        sigemptyset(&action.sa_mask);
        struct sigaction previous = {};
        REQUIRE(sigaction(SIGPROF, &action, &previous) == 0);
        const pthread_t target = pthread_self();
        std::atomic<bool> done{false};
        std::thread sampler([&] {
            const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(30);
            while (taken.load(std::memory_order_acquire) < kSamples && std::chrono::steady_clock::now() < deadline)
            {
                pthread_kill(target, SIGPROF);
                std::this_thread::sleep_for(std::chrono::microseconds(200));
            }
            done.store(true);
        });
        while (!done.load())
            work();
        sampler.join();
        // Ignoring the signal discards one still pending (POSIX), so the handler that was there before never meets it.
        struct sigaction ignore = {};
        ignore.sa_handler = SIG_IGN;
        sigemptyset(&ignore.sa_mask);
        sigaction(SIGPROF, &ignore, nullptr);
        sigaction(SIGPROF, &previous, nullptr);
        std::vector<std::uintptr_t> counters;
        for (std::size_t i = 0; i < taken.load(); ++i)
            counters.push_back(sampled[i].load(std::memory_order_relaxed));
        return counters;
    }

    // What a profiler reports: the samples each function's code took, the program's code's share of them, and the
    // functions by their share, the most first ("62.5%  Work::slow").
    struct Profile
    {
        std::size_t samples = 0;
        std::size_t inProgram = 0;
        std::map<std::string, std::size_t> byFunction;

        std::string report() const
        {
            std::vector<std::pair<std::size_t, std::string>> top;
            for (const auto& [function, count] : byFunction)
                top.emplace_back(count, function);
            std::sort(top.rbegin(), top.rend());
            std::ostringstream text;
            text << samples << " samples, " << inProgram << " in the program's code\n";
            for (const auto& [count, function] : top)
                text << "  " << 100.0 * static_cast<double>(count) / static_cast<double>(samples) << "%  " << function
                     << "\n";
            return text.str();
        }
    };

    Profile profiled(const std::vector<std::uintptr_t>& counters, const std::vector<Program::CodeRange>& ranges)
    {
        Profile profile;
        profile.samples = counters.size();
        for (const std::uintptr_t counter : counters)
        {
            if (const Program::CodeRange* range = Program::codeRangeAt(ranges, counter))
            {
                ++profile.inProgram;
                ++profile.byFunction[range->function];
            }
        }
        return profile;
    }

    // Two methods of one loop, slow's three times as long as fast's, and a function that calls both. Each call takes
    // longer than the time between two samples, so that the samples spread over each, and do not fall in step with
    // the calls.
    const char* const kWork = "module Work\n"
                              "{\n"
                              "    float state;\n"
                              "\n"
                              "    float slow(int n)\n"
                              "    {\n"
                              "        float x = state;\n"
                              "        for (int i = 0; i < 3 * n; i++)\n"
                              "            x = x * 0.9999f + 0.5f;\n"
                              "        state = x;\n"
                              "        return x;\n"
                              "    }\n"
                              "\n"
                              "    float fast(int n)\n"
                              "    {\n"
                              "        float x = state;\n"
                              "        for (int i = 0; i < n; i++)\n"
                              "            x = x * 0.9999f + 0.5f;\n"
                              "        state = x;\n"
                              "        return x;\n"
                              "    }\n"
                              "};\n"
                              "\n"
                              "float spin(Work* w, int rounds)\n"
                              "{\n"
                              "    float sum = 0.0f;\n"
                              "    for (int r = 0; r < rounds; r++)\n"
                              "        sum = sum + w->slow(20000) + w->fast(20000);\n"
                              "    return sum;\n"
                              "}\n";
} // namespace

TEST_CASE("codeRanges: a reference sampler finds a program's time in the functions that spend it", "[library][profile]")
{
    for (const int level : {0, 2})
    {
        DYNAMIC_SECTION("-O" << level)
        {
            Program::Options options;
            options.optimisation = level;
            auto program = compiled(kWork, options);
            const std::vector<Program::CodeRange> ranges = rangesOf(*program);
            std::string errors;
            auto* spin = program->function<float(void*, int)>("spin", errors);
            REQUIRE(spin != nullptr);
            Instance work(*program->module("Work"));
            volatile float sink = 0.0f;

            const Profile profile = profiled(sample([&] { sink = sink + spin(work.memory, 10); }), ranges);
            INFO(profile.report());
            REQUIRE(profile.samples == kSamples);
            // The thread runs the program's code all but the time it takes to call spin again.
            CHECK(profile.inProgram >= profile.samples * 9 / 10);
            const auto count = [&](const char* function) {
                const auto found = profile.byFunction.find(function);
                return found != profile.byFunction.end() ? found->second : std::size_t{0};
            };
            if (level == 0)
            {
                // Each function's code is its own: slow takes about three times fast's samples, spin few.
                CHECK(2 * count("Work::slow") >= 3 * count("Work::fast"));
                CHECK(count("Work::fast") >= profile.samples / 10);
                CHECK(count("spin") < profile.samples / 10);
            }
            else
            {
                // Both methods are inlined into spin, whose range says so: their samples are spin's, and their own
                // ranges, which nothing calls, take none.
                const Program::CodeRange* spun = named(ranges, "spin");
                REQUIRE(spun != nullptr);
                CHECK(spun->inlined == Names{"Work::fast", "Work::slow"});
                CHECK(count("spin") >= profile.inProgram * 9 / 10);
                CHECK(count("Work::slow") == 0);
                CHECK(count("Work::fast") == 0);
            }
        }
    }
}
#endif
