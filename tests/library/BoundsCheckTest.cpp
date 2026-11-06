// tests/library/BoundsCheckTest.cpp: Program::Options::boundsCheck, bounds checks for development builds, as a plugin
// host uses them (C++17, FMinus::FMinus alone): a process() whose index leaves its array returns early, and the host
// reads where (boundsViolation: the file, an #include's too, the line and column, the array, its type and extent, the
// index) and clears it, before it re-initialises the instance (an init run while the record holds a check stops
// partway); the first failure is kept; from an entry point that flushes subnormals the host's floating-point mode comes
// back as it was. Within its arrays a call computes what it would without the checks, bit for bit, and a program
// compiled without them has none, in its IR too.
#include "fminus/FMinus.h"

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
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

    // An echo whose delay, in samples, is a parameter: its read position wraps once, so a delay longer than the line
    // reads before it. A plugin's bug of the kind the checks are for.
    const char* const kEcho = "module Echo\n"
                              "{\n"
                              "    param int delay [[fminus::range(1, 512)]] [[fminus::default(100)]];\n"
                              "    float line[512];\n"
                              "    int at;\n"
                              "\n"
                              "    void process(const float* restrict in, float* restrict out, int n)"
                              " [[clang::nonblocking]]\n"
                              "    {\n"
                              "        for (int i = 0; i < n; ++i)\n"
                              "        {\n"
                              "            int back = at - delay[i];\n"
                              "            if (back < 0)\n"
                              "                back += 512;\n"
                              "            const float y = line[back];\n"
                              "            line[at] = in[i] + 0.5f * y;\n"
                              "            out[i] = in[i] + y;\n"
                              "            at = at == 511 ? 0 : at + 1;\n"
                              "        }\n"
                              "    }\n"
                              "};\n";

    using Process = void(void*, const float*, float*, int);
} // namespace

TEST_CASE("Options::boundsCheck: a process() that reads before its array returns early, and the host is told where",
          "[library][bounds]")
{
    std::string errors;
    fminus::Program::Options options;
    CHECK_FALSE(options.boundsCheck);   // off by default
    options.boundsCheck = true;
    options.fileName = "echo.fm";
    auto program = fminus::Program::compile(kEcho, errors, options);
    INFO(errors);
    REQUIRE(program);
    auto* process = program->function<Process>("Echo::process", errors);
    REQUIRE(process != nullptr);
    const fminus::Program::Module* echo = program->module("Echo");
    const fminus::Program::Parameter* delay = program->parameter("Echo::delay");
    REQUIRE(echo != nullptr);
    REQUIRE(delay != nullptr);
    Instance instance(*echo);
    fminus::Program::setDefaults(instance.memory, *echo);
    std::vector<float> in(256, 1.0f);
    std::vector<float> out(256, 0.0f);

    CHECK_FALSE(program->boundsViolation());
    process(instance.memory, in.data(), out.data(), 256);
    CHECK_FALSE(program->boundsViolation());
    CHECK(out[99] == 1.0f);    // the line was silent
    CHECK(out[100] == 2.0f);   // the first sample's echo
    CHECK(out[255] == 2.5f);   // and the echo of its echo

    // The bug: a delay longer than the line. The first sample reads line[0 + 256 - 700 + 512], line[-188].
    std::memset(instance.memory, 0, instance.size);
    fminus::Program::setValue(instance.memory, *delay, 700.0);
    std::fill(out.begin(), out.end(), -1.0f);
    process(instance.memory, in.data(), out.data(), 256);
    const std::optional<fminus::Program::BoundsViolation> failed = program->boundsViolation();
    REQUIRE(failed);
    CHECK(std::string(failed->file) == "echo.fm");   // string_views, the Program's
    CHECK(failed->line == 14);
    CHECK(failed->column == 33);
    CHECK(std::string(failed->array) == "line");
    CHECK(std::string(failed->type) == "float[512]");
    CHECK(failed->extent == 512);
    CHECK(failed->index == -188);
    CHECK(failed->width == 1);
    CHECK(failed->message() == "echo.fm:14:33: index -188 out of bounds for 'line' of type 'float[512]'");
    CHECK(out[0] == -1.0f);   // the first sample's read failed: nothing after it in the call ran

    // Kept until the host clears it, which a later failure does not change.
    fminus::Program::setValue(instance.memory, *delay, 600.0);
    process(instance.memory, in.data(), out.data(), 256);
    REQUIRE(program->boundsViolation());
    CHECK(program->boundsViolation()->index == -188);

    // The host silences it, fixes it, and runs it again on an instance it made again, the record cleared.
    program->clearBoundsViolation();
    CHECK_FALSE(program->boundsViolation());
    std::memset(instance.memory, 0, instance.size);
    fminus::Program::setDefaults(instance.memory, *echo);
    process(instance.memory, in.data(), out.data(), 256);
    CHECK_FALSE(program->boundsViolation());
    CHECK(out[255] == 2.5f);
}

TEST_CASE("Options::boundsCheck: the host clears the record, then re-initialises: an init run before stops partway",
          "[library][bounds]")
{
    // At -O0 init's calls of clear stay calls, each of a function with a check, after which init reads the record.
    const char* const voice = "module Voice\n"
                              "{\n"
                              "    float taps[4];\n"
                              "    float gain;\n"
                              "    int ready;\n"
                              "    void clear(int k) { taps[k] = 0.0f; }\n"
                              "    void init()\n"
                              "    {\n"
                              "        for (int k = 0; k < 4; ++k)\n"
                              "            clear(k);\n"
                              "        gain = 0.5f;\n"
                              "        ready = 1;\n"
                              "    }\n"
                              "    float tap(int k) { return taps[k] * gain; }\n"
                              "    int isReady() { return ready; }\n"
                              "};\n";
    std::string errors;
    fminus::Program::Options options;
    options.boundsCheck = true;
    options.optimisation = 0;
    options.fileName = "voice.fm";
    auto program = fminus::Program::compile(voice, errors, options);
    INFO(errors);
    REQUIRE(program);
    auto* init = program->function<void(void*)>("Voice::init", errors);
    auto* tap = program->function<float(void*, int)>("Voice::tap", errors);
    auto* isReady = program->function<int(void*)>("Voice::isReady", errors);
    REQUIRE(init != nullptr);
    REQUIRE(tap != nullptr);
    REQUIRE(isReady != nullptr);
    Instance instance(*program->module("Voice"));

    CHECK(tap(instance.memory, 4) == 0.0f);   // the bug: taps[4]
    REQUIRE(program->boundsViolation());
    CHECK(program->boundsViolation()->message() == "voice.fm:14:35: index 4 out of bounds for 'taps' of type "
                                                   "'float[4]'");

    // The wrong order: init first. Its first call of clear returns, the record still holds the check, and init returns
    // after it, the instance half set up; and the first failure is kept, so nothing says so.
    init(instance.memory);
    CHECK(isReady(instance.memory) == 0);
    REQUIRE(program->boundsViolation());
    CHECK(program->boundsViolation()->index == 4);

    // The right one: clear the record, then re-initialise.
    program->clearBoundsViolation();
    init(instance.memory);
    CHECK_FALSE(program->boundsViolation());
    CHECK(isReady(instance.memory) == 1);
    CHECK(tap(instance.memory, 3) == 0.0f);
    CHECK_FALSE(program->boundsViolation());
}

TEST_CASE("Options::boundsCheck: a check in an #include'd file names that file", "[library][bounds]")
{
    std::string errors;
    fminus::Program::Options options;
    options.boundsCheck = true;
    options.fileName = "synth.fm";
    options.include = [](std::string_view name, std::string_view) -> std::optional<fminus::Program::IncludedFile> {
        if (name != "taps.fm")
            return std::nullopt;
        return fminus::Program::IncludedFile{"lib/taps.fm", "module Taps\n"
                                                            "{\n"
                                                            "    float taps[4];\n"
                                                            "    float tap(int k) { return taps[k]; }\n"
                                                            "};\n"};
    };
    auto program = fminus::Program::compile("#include \"taps.fm\"\n"
                                            "float voice(Taps* t, float k) { return t->tap((int)k); }\n",
                                            errors, options);
    INFO(errors);
    REQUIRE(program);
    auto* voice = program->function<float(void*, float)>("voice", errors);
    REQUIRE(voice != nullptr);
    Instance taps(*program->module("Taps"));
    CHECK(voice(taps.memory, 3.0f) == 0.0f);
    CHECK_FALSE(program->boundsViolation());
    CHECK(voice(taps.memory, 4.0f) == 0.0f);
    REQUIRE(program->boundsViolation());
    CHECK(program->boundsViolation()->message() == "lib/taps.fm:4:35: index 4 out of bounds for 'taps' of type "
                                                   "'float[4]'");
}

TEST_CASE("Options::boundsCheck: within its arrays a call computes what it would without them, bit for bit",
          "[library][bounds]")
{
    std::string errors;
    fminus::Program::Options options;
    auto plain = fminus::Program::compile(kEcho, errors, options);
    options.boundsCheck = true;
    auto checked = fminus::Program::compile(kEcho, errors, options);
    INFO(errors);
    REQUIRE(plain);
    REQUIRE(checked);
    auto* processPlain = plain->function<Process>("Echo::process", errors);
    auto* processChecked = checked->function<Process>("Echo::process", errors);
    REQUIRE(processPlain != nullptr);
    REQUIRE(processChecked != nullptr);
    Instance a(*plain->module("Echo"));
    Instance b(*checked->module("Echo"));
    fminus::Program::setDefaults(a.memory, *plain->module("Echo"));
    fminus::Program::setDefaults(b.memory, *checked->module("Echo"));
    std::vector<float> in(2048);
    for (std::size_t i = 0; i < in.size(); ++i)
        in[i] = static_cast<float>((i * 7919) % 1000) / 500.0f - 1.0f;
    std::vector<float> expected(in.size());
    std::vector<float> got(in.size());
    for (std::size_t at = 0; at < in.size(); at += 128)
    {
        processPlain(a.memory, in.data() + at, expected.data() + at, 128);
        processChecked(b.memory, in.data() + at, got.data() + at, 128);
    }
    CHECK_FALSE(checked->boundsViolation());
    CHECK(std::memcmp(expected.data(), got.data(), expected.size() * sizeof(float)) == 0);
    CHECK(std::memcmp(a.memory, b.memory, a.size) == 0);
}

TEST_CASE("Options::boundsCheck: a program compiled without them has none", "[library][bounds][ir]")
{
    std::string errors;
    fminus::Program::Options options;
    options.keepIr = fminus::Program::KeepIr::Optimised;
    auto plain = fminus::Program::compile(kEcho, errors, options);
    options.boundsCheck = true;
    auto checked = fminus::Program::compile(kEcho, errors, options);
    INFO(errors);
    REQUIRE(plain);
    REQUIRE(checked);
    CHECK(plain->optimisedIr().find("fminus_rt_bounds") == std::string::npos);
    CHECK_FALSE(plain->boundsViolation());
    plain->clearBoundsViolation();   // harmless
    CHECK_FALSE(plain->boundsViolation());
    CHECK(checked->optimisedIr().find("@fminus_rt_bounds = ") != std::string::npos);
    CHECK(checked->optimisedIr().find("bounds.stop:") != std::string::npos);
    CHECK(checked->optimisedIr().find("cmpxchg ptr @fminus_rt_bounds") != std::string::npos);
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

TEST_CASE("Options::boundsCheck: an entry point that flushes hands the host's mode back when it stops",
          "[library][bounds][denormals]")
{
    const std::uint64_t host = controlRegister() & ~kFlushBits;   // an IEEE host
    setControlRegister(host);
    std::string errors;
    fminus::Program::Options options;
    options.boundsCheck = true;
    options.denormals = fminus::Program::Denormals::Flush;
    auto program = fminus::Program::compile(kEcho, errors, options);
    INFO(errors);
    REQUIRE(program);
    auto* process = program->function<Process>("Echo::process", errors);
    REQUIRE(process != nullptr);
    Instance instance(*program->module("Echo"));
    fminus::Program::setValue(instance.memory, *program->parameter("Echo::delay"), 1000.0);
    std::vector<float> in(16, 1.0f);
    std::vector<float> out(16);
    process(instance.memory, in.data(), out.data(), 16);
    const std::uint64_t after = controlRegister();
    setControlRegister(host);
    CHECK(program->boundsViolation());
    CHECK(after == host);
}
#endif
