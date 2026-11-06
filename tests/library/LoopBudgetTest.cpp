// tests/library/LoopBudgetTest.cpp: Program::Options::loopBudget, runaway-loop protection, as a plugin host uses it
// (C++17, FMinus::FMinus alone): the host sets the budget before each call (setLoopBudget), and a process() that would
// never end returns early, with loopBudgetExceeded() set, on the calling thread, in a parallel region's threads, and from
// an entry point that flushes subnormals, which hands the host's floating-point mode back as it was. Within its budget a
// call computes what it would without it, bit for bit; a program compiled without it reads and writes a budget nothing
// counts down.
#include "fminus/FMinus.h"

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <memory>
#include <string>
#include <vector>

#if defined(_WIN32)
#include <malloc.h>   // _aligned_malloc, _aligned_free
#endif
#if defined(__x86_64__)
#include <immintrin.h>
#endif

namespace
{
    // An instance of a module, as a host makes one: aligned, zeroed, the module's size (FMinus.h, Module).
    struct Instance
    {
        explicit Instance(const fminus::Program::Module& module)
        {
            const std::size_t alignment = module.alignment < alignof(std::max_align_t) ? alignof(std::max_align_t)
                                                                                       : module.alignment;
            size = (module.size + alignment - 1) / alignment * alignment;
#if defined(_WIN32)
            memory = _aligned_malloc(size, alignment);
#else
            memory = std::aligned_alloc(alignment, size);
#endif
            std::memset(memory, 0, size);
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

        std::size_t size = 0;
        void* memory = nullptr;
    };

    // An oscillator whose phase wraps in a while loop: an infinite frequency makes the phase infinite, and infinity
    // less one is infinity, so the loop never ends. A plugin's bug of the kind the budget is for.
    const char* const kOsc = "module Osc\n"
                             "{\n"
                             "    param float frequency [[fminus::default(440)]];\n"
                             "    float phase;\n"
                             "\n"
                             "    void process(float* restrict out, int n) [[clang::nonblocking]]\n"
                             "    {\n"
                             "        const float step = frequency[0] / 48000.0f;\n"
                             "        for (int i = 0; i < n; ++i)\n"
                             "        {\n"
                             "            phase = phase + step;\n"
                             "            while (phase >= 1.0f)\n"
                             "                phase = phase - 1.0f;\n"
                             "            out[i] = phase;\n"
                             "        }\n"
                             "    }\n"
                             "};\n";

    using Process = void(void*, float*, int);
} // namespace

TEST_CASE("Options::loopBudget: a process() that never ends returns early, and the host is told",
          "[library][budget]")
{
    std::string errors;
    fminus::Program::Options options;
    CHECK_FALSE(options.loopBudget);   // off by default
    options.loopBudget = true;
    auto program = fminus::Program::compile(kOsc, errors, options);
    INFO(errors);
    REQUIRE(program);
    auto* process = program->function<Process>("Osc::process", errors);
    REQUIRE(process != nullptr);
    const fminus::Program::Module* osc = program->module("Osc");
    const fminus::Program::Parameter* frequency = program->parameter("Osc::frequency");
    REQUIRE(osc != nullptr);
    REQUIRE(frequency != nullptr);
    Instance instance(*osc);
    fminus::Program::setDefaults(instance.memory, *osc);
    std::vector<float> out(256, -1.0f);

    // Unlimited until the host sets it.
    CHECK(program->loopBudgetLeft() == std::numeric_limits<std::int64_t>::max());
    CHECK_FALSE(program->loopBudgetExceeded());

    program->setLoopBudget(1'000'000);
    process(instance.memory, out.data(), 256);
    CHECK_FALSE(program->loopBudgetExceeded());
    CHECK(program->loopBudgetLeft() > 0);
    CHECK(program->loopBudgetLeft() < 1'000'000);   // the block's loop, and the wraps
    CHECK(out[255] > 0.0f);
    CHECK(out[255] < 1.0f);

    // The bug: an infinite frequency. The call returns, early, with the flag set; what it wrote before it stopped stays.
    fminus::Program::setValue(instance.memory, *frequency, std::numeric_limits<double>::infinity());
    std::fill(out.begin(), out.end(), -1.0f);
    program->setLoopBudget(1'000'000);
    process(instance.memory, out.data(), 256);
    CHECK(program->loopBudgetExceeded());
    CHECK(program->loopBudgetLeft() < 0);
    CHECK(out[255] == -1.0f);

    // The host silences it, fixes it (or keeps the program it had), and runs it again.
    fminus::Program::setValue(instance.memory, *frequency, 440.0);
    std::memset(instance.memory, 0, instance.size);
    fminus::Program::setDefaults(instance.memory, *osc);
    program->setLoopBudget(1'000'000);
    CHECK_FALSE(program->loopBudgetExceeded());
    process(instance.memory, out.data(), 256);
    CHECK_FALSE(program->loopBudgetExceeded());
    CHECK(out[255] > 0.0f);
}

TEST_CASE("Options::loopBudget: within its budget a call computes what it would without it, bit for bit",
          "[library][budget]")
{
    std::string errors;
    fminus::Program::Options options;
    auto plain = fminus::Program::compile(kOsc, errors, options);
    options.loopBudget = true;
    auto budgeted = fminus::Program::compile(kOsc, errors, options);
    INFO(errors);
    REQUIRE(plain);
    REQUIRE(budgeted);
    auto* processPlain = plain->function<Process>("Osc::process", errors);
    auto* processBudgeted = budgeted->function<Process>("Osc::process", errors);
    REQUIRE(processPlain != nullptr);
    REQUIRE(processBudgeted != nullptr);
    Instance a(*plain->module("Osc"));
    Instance b(*budgeted->module("Osc"));
    for (const double hz : {440.0, 12345.6, 96000.0 * 3.5})
    {
        fminus::Program::setValue(a.memory, *plain->parameter("Osc::frequency"), hz);
        fminus::Program::setValue(b.memory, *budgeted->parameter("Osc::frequency"), hz);
        std::vector<float> expected(1000);
        std::vector<float> got(1000);
        for (int block = 0; block < 4; ++block)
        {
            processPlain(a.memory, expected.data(), 1000);
            budgeted->setLoopBudget(1'000'000);
            processBudgeted(b.memory, got.data(), 1000);
            CHECK_FALSE(budgeted->loopBudgetExceeded());
            CHECK(std::memcmp(expected.data(), got.data(), sizeof(float) * expected.size()) == 0);
        }
    }
}

TEST_CASE("Options::loopBudget: a program compiled without it counts nothing", "[library][budget]")
{
    std::string errors;
    auto program = fminus::Program::compile(kOsc, errors);
    REQUIRE(program);
    auto* process = program->function<Process>("Osc::process", errors);
    REQUIRE(process != nullptr);
    Instance instance(*program->module("Osc"));
    fminus::Program::setDefaults(instance.memory, *program->module("Osc"));
    std::vector<float> out(256);
    program->setLoopBudget(0);
    process(instance.memory, out.data(), 256);
    CHECK_FALSE(program->loopBudgetExceeded());
    CHECK(program->loopBudgetLeft() == 0);
    CHECK(out[255] > 0.0f);
}

TEST_CASE("Options::loopBudget: a parallel region's threads stop too", "[library][budget]")
{
    // Each iteration spins for x > 0. The region's threads each count from where their part starts, run out and
    // return; the call returns after the region, at once.
    const char* const source = "void spin_all(float* restrict out, int n, float x) [[clang::nonblocking]]\n"
                               "{\n"
                               "    parallel for (int i = 0; i < n; ++i)\n"
                               "    {\n"
                               "        float y = x + (float)i;\n"
                               "        while (y > 0)\n"
                               "            y = y * 1.0f;\n"
                               "        out[i] = y;\n"
                               "    }\n"
                               "}\n";
    for (const unsigned threads : {0u, 1u})
    {
        INFO("threads " << threads);
        std::string errors;
        fminus::Program::Options options;
        options.loopBudget = true;
        options.threads = threads;
        auto program = fminus::Program::compile(source, errors, options);
        INFO(errors);
        REQUIRE(program);
        auto* spinAll = program->function<void(float*, int, float)>("spin_all", errors);
        REQUIRE(spinAll != nullptr);
        std::vector<float> out(64, 1.0f);

        program->setLoopBudget(100'000);
        spinAll(out.data(), 64, -10.0f);   // from i = 11, y > 0: these spin
        CHECK(program->loopBudgetExceeded());

        program->setLoopBudget(100'000);
        spinAll(out.data(), 64, -1000.0f);   // none spins
        CHECK_FALSE(program->loopBudgetExceeded());
        CHECK(out[63] == -937.0f);
    }
}

#if defined(__aarch64__) || defined(__x86_64__)
namespace
{
#if defined(__aarch64__)
    constexpr std::uint64_t kFlushBits = std::uint64_t{1} << 24;   // FPCR.FZ

    std::uint64_t controlRegister()
    {
        std::uint64_t value = 0;
        asm volatile("mrs %0, fpcr" : "=r"(value) : : "memory");
        return value;
    }

    void setControlRegister(std::uint64_t value)
    {
        asm volatile("msr fpcr, %0" : : "r"(value) : "memory");
    }
#else
    constexpr std::uint64_t kFlushBits = 0x8040;   // MXCSR's FTZ and DAZ
    constexpr std::uint64_t kExceptionFlags = 0x3F;

    std::uint64_t controlRegister()
    {
        return _mm_getcsr() & ~kExceptionFlags;
    }

    void setControlRegister(std::uint64_t value)
    {
        _mm_setcsr(static_cast<unsigned>(value));
    }
#endif
} // namespace

TEST_CASE("Options::loopBudget: an entry point that flushes hands the host's mode back when it stops",
          "[library][budget][denormals]")
{
    const std::uint64_t host = controlRegister() & ~kFlushBits;   // an IEEE host
    setControlRegister(host);
    std::string errors;
    fminus::Program::Options options;
    options.loopBudget = true;
    options.denormals = fminus::Program::Denormals::Flush;
    auto program = fminus::Program::compile(kOsc, errors, options);
    INFO(errors);
    REQUIRE(program);
    auto* process = program->function<Process>("Osc::process", errors);
    REQUIRE(process != nullptr);
    Instance instance(*program->module("Osc"));
    fminus::Program::setValue(instance.memory, *program->parameter("Osc::frequency"),
                              std::numeric_limits<double>::infinity());
    std::vector<float> out(16);
    program->setLoopBudget(10'000);
    process(instance.memory, out.data(), 16);
    const std::uint64_t after = controlRegister();
    setControlRegister(host);
    CHECK(program->loopBudgetExceeded());
    CHECK(after == host);
}
#endif

TEST_CASE("Options::loopBudget: the optimised IR shows the checks", "[library][budget][ir]")
{
    std::string errors;
    fminus::Program::Options options;
    options.loopBudget = true;
    options.keepIr = fminus::Program::KeepIr::Optimised;
    auto program = fminus::Program::compile(kOsc, errors, options);
    INFO(errors);
    REQUIRE(program);
    const std::string& ir = program->optimisedIr();
    CHECK(ir.find("@fminus_rt_loop_budget = ") != std::string::npos);
    CHECK(ir.find("budget.stop:") != std::string::npos);
    CHECK(ir.find("\"fminus-loop-budget\"=\"counts\"") != std::string::npos);
}
