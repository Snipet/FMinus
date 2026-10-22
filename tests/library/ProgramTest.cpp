// tests/library/ProgramTest.cpp: the public API, include/fminus/FMinus.h, as a host uses it. Built in C++17 against
// FMinus::FMinus alone (tests/CMakeLists.txt), without LLVM's headers or FMinus's internals, so it also checks that the
// header asks for nothing more.
#include "fminus/FMinus.h"

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <string>
#include <thread>
#include <vector>

#if defined(_WIN32)
#include <malloc.h>   // _aligned_malloc, _aligned_free
#endif

namespace
{
    // C's aligned_alloc, and the free that goes with it. The Windows C runtime has no aligned_alloc: its aligned memory
    // comes from _aligned_malloc and goes back to _aligned_free, not free (as the FMinus runtime's does).
    void* alignedAlloc(std::size_t alignment, std::size_t size)
    {
#if defined(_WIN32)
        return _aligned_malloc(size, alignment);
#else
        return std::aligned_alloc(alignment, size);
#endif
    }

    void alignedFree(void* memory)
    {
#if defined(_WIN32)
        _aligned_free(memory);
#else
        std::free(memory);
#endif
    }

    // FMinus's float4 at the C boundary: clang's ext_vector_type(4).
    using float4 = float __attribute__((ext_vector_type(4)));

    // A block kernel, as a plugin calls one: the loop over the samples is FMinus's.
    const char* const kGain = "void gain(const float* restrict in, float* restrict out, int n, float g)\n"
                              "    [[clang::nonblocking]]\n"
                              "{\n"
                              "    for (int i = 0; i < n; ++i)\n"
                              "        out[i] = g * in[i];\n"
                              "}\n";

    using Gain = void(const float*, float*, int, float);
} // namespace

TEST_CASE("a Program compiles FMinus source and gives its functions as C function pointers", "[library]")
{
    std::string errors = "left over";
    auto program = fminus::Program::compile(kGain, errors);
    INFO(errors);
    REQUIRE(program);
    CHECK(errors.empty());
    Gain* gain = program->function<Gain>("gain", errors);
    REQUIRE(gain != nullptr);
    const std::array<float, 5> in{1.0f, -2.0f, 3.0f, -4.0f, 5.0f};
    std::array<float, 5> out{};
    gain(in.data(), out.data(), 5, 0.5f);
    CHECK(out == std::array<float, 5>{0.5f, -1.0f, 1.5f, -2.0f, 2.5f});
    CHECK(program->address("gain", errors) == reinterpret_cast<std::uintptr_t>(gain));
    CHECK(std::string(FMINUS_VERSION_STRING).size() >= 5);   // Config.h comes with the header
}

TEST_CASE("a Program's functions take and return C's types", "[library]")
{
    std::string errors;
    auto program = fminus::Program::compile("float twice(float x) { return 2.0f * x; }\n"
                                            "int rest(int a, int b) { return a % b; }\n"
                                            "bool positive(float x) { return x > 0.0f; }\n"
                                            "float4 sum(float4 a, float4 b) { return a + b; }\n"
                                            "float* ramp(int n)\n"
                                            "{\n"
                                            "    float* p = calloc(n, sizeof(float));\n"
                                            "    for (int i = 0; i < n; ++i)\n"
                                            "        p[i] = i;\n"
                                            "    return p;\n"
                                            "}\n"
                                            "void release(void* p) { free(p); }\n",
                                            errors);
    INFO(errors);
    REQUIRE(program);
    CHECK(program->function<float(float)>("twice", errors)(1.5f) == 3.0f);
    CHECK(program->function<int(int, int)>("rest", errors)(-7, 3) == -1);   // C's: the sign of the dividend
    auto* positive = program->function<bool(float)>("positive", errors);
    CHECK(positive(0.25f));
    CHECK(!positive(-0.25f));
    auto* sum = program->function<float4(float4, float4)>("sum", errors);
    const float4 s = sum(float4{1, 2, 3, 4}, float4{10, 20, 30, 40});
    CHECK((s.x == 11.0f && s.y == 22.0f && s.z == 33.0f && s.w == 44.0f));
    float* p = program->function<float*(int)>("ramp", errors)(64);   // FMinus's heap: the runtime is linked
    REQUIRE(p != nullptr);
    CHECK(reinterpret_cast<std::uintptr_t>(p) % 64 == 0);
    CHECK((p[0] == 0.0f && p[63] == 63.0f));
    program->function<void(void*)>("release", errors)(p);
}

TEST_CASE("a Program that does not compile gives FMinus's messages, as clang words them", "[library]")
{
    std::string errors;
    fminus::Program::Options options;
    options.fileName = "host.fm";
    CHECK(fminus::Program::compile("float f(float x) { return y; }\n", errors, options) == nullptr);
    CHECK(errors == "host.fm:1:27: error: use of undeclared identifier 'y'\n");

    CHECK(fminus::Program::compile("float f(float x) { return x }\n", errors) == nullptr);   // a syntax error
    CHECK(errors.rfind("program.fm:1:", 0) == 0);

    // A function that could reach the heap, marked nonblocking: refused, with clang's notes down to the call.
    CHECK(fminus::Program::compile("float* make(int n) { return calloc(n, sizeof(float)); }\n"
                                   "void run(float* p, int n) [[clang::nonblocking]] { p = make(n); }\n",
                                   errors)
          == nullptr);
    CHECK(errors.find("error:") != std::string::npos);
    CHECK(errors.find("nonblocking") != std::string::npos);
    CHECK(errors.find("note:") != std::string::npos);
}

TEST_CASE("a Program's function that does not exist is a null pointer and an error", "[library]")
{
    std::string errors;
    auto program = fminus::Program::compile(kGain, errors);
    REQUIRE(program);
    CHECK(program->function<Gain>("missing", errors) == nullptr);
    CHECK(errors.find("missing") != std::string::npos);
    CHECK(program->address("gain", errors) != 0);   // the program goes on working
    CHECK(errors.empty());
}

TEST_CASE("a Program says which functions are nonblocking and nonallocating", "[library]")
{
    std::string errors;
    auto program = fminus::Program::compile("float* make(int n) { return calloc(n, sizeof(float)); }\n"
                                            "void scale(float* p, int n, float g) [[clang::nonallocating]]\n"
                                            "{\n"
                                            "    for (int i = 0; i < n; ++i)\n"
                                            "        p[i] = g * p[i];\n"
                                            "}\n"
                                            "void run(float* p, int n) [[clang::nonblocking]];\n"   // a prototype
                                            "void run(float* p, int n) { scale(p, n, 0.5f); }\n",
                                            errors);
    INFO(errors);
    REQUIRE(program);
    CHECK(program->nonBlocking("run"));
    CHECK(program->nonAllocating("run"));   // nonblocking includes it
    CHECK(!program->nonBlocking("scale"));
    CHECK(program->nonAllocating("scale"));
    CHECK(!program->nonBlocking("make"));
    CHECK(!program->nonAllocating("make"));
    CHECK(!program->nonBlocking("missing"));
}

TEST_CASE("a Program's options: the optimisation level, a poly float's lanes, and out-of-range values refused",
          "[library]")
{
    std::string errors;
    const std::array<float, 3> in{1.0f, 2.0f, 3.0f};
    for (int level = 0; level <= 3; ++level)
    {
        fminus::Program::Options options;
        options.optimisation = level;
        auto program = fminus::Program::compile(kGain, errors, options);
        INFO("-O" << level << ": " << errors);
        REQUIRE(program);
        std::array<float, 3> out{};
        program->function<Gain>("gain", errors)(in.data(), out.data(), 3, 2.0f);
        CHECK(out == std::array<float, 3>{2.0f, 4.0f, 6.0f});
    }

    auto native = fminus::Program::compile(kGain, errors);
    REQUIRE(native);
    const unsigned lanes = native->polyLanes();   // this machine's: 4 with NEON or SSE
    CHECK((lanes >= 1 && lanes <= 64 && (lanes & (lanes - 1)) == 0));
    fminus::Program::Options eight;
    eight.polyLanes = 8;
    auto wide = fminus::Program::compile("float lanes() { return poly_lanes(); }\n", errors, eight);
    INFO(errors);
    REQUIRE(wide);
    CHECK(wide->polyLanes() == 8);
    CHECK(wide->function<float()>("lanes", errors)() == 8.0f);   // the program's poly floats have 8 lanes

    fminus::Program::Options bad;
    bad.optimisation = 4;
    CHECK(fminus::Program::compile(kGain, errors, bad) == nullptr);
    CHECK(errors == "fminus::Program: optimisation 4 is not a level from 0 to 3\n");
    bad.optimisation = 2;
    bad.polyLanes = 3;
    CHECK(fminus::Program::compile(kGain, errors, bad) == nullptr);
    CHECK(errors == "fminus::Program: polyLanes 3 is not 0 (this machine's) or a power of two from 1 to 64\n");
}

TEST_CASE("fminus::math is what a Program's sinf, cosf, expf, tanhf and logf compute; Options::math picks another",
          "[library][math]")
{
    // A block of each, the loop FMinus's (and vectorised, at -O2), against C++ a sample at a time.
    const char* const kMath = "void math(const float* restrict in, float* restrict out, int n)\n"
                              "{\n"
                              "    for (int i = 0; i < n; ++i)\n"
                              "        out[i] = sinf(in[i]) + cosf(in[i]) + expf(in[i]) + tanhf(in[i]) + logf(in[i]);\n"
                              "}\n";
    using Math = void(const float*, float*, int);
    std::vector<float> in(1001);
    for (std::size_t i = 0; i < in.size(); ++i)
        in[i] = 0.01f + 0.037f * static_cast<float>(i);
    // The C++ a host would write, one rounding a statement as FMinus's IR has it, calling its functions through
    // pointers the compiler cannot see through. With std::'s seen through, clang at -O2 makes the sine and the cosine
    // of one argument one call of Apple's __sincosf_stret, whose sine is an ulp from sinf's for some arguments (one of
    // these).
    using Function = float (*)(float);
    const auto expected = [&](Function sin, Function cos, Function exp, Function tanh, Function log) {
        volatile Function opaque[5] = {sin, cos, exp, tanh, log};
        std::vector<float> out(in.size());
        for (std::size_t i = 0; i < in.size(); ++i)
        {
            float sum = opaque[0](in[i]);
            sum = sum + opaque[1](in[i]);
            sum = sum + opaque[2](in[i]);
            sum = sum + opaque[3](in[i]);
            out[i] = sum + opaque[4](in[i]);
        }
        return out;
    };
    const auto computed = [&](fminus::Program::Math math, int level) {
        fminus::Program::Options options;
        options.math = math;
        options.optimisation = level;
        std::string errors;
        auto program = fminus::Program::compile(kMath, errors, options);
        std::vector<float> out(in.size());
        if (program)
            program->function<Math>("math", errors)(in.data(), out.data(), static_cast<int>(in.size()));
        return out;
    };
    const std::vector<float> vector =
        expected(fminus::math::sin, fminus::math::cos, fminus::math::exp, fminus::math::tanh, fminus::math::log);
    const std::vector<float> libm = expected([](float x) { return std::sin(x); }, [](float x) { return std::cos(x); },
                                             [](float x) { return std::exp(x); }, [](float x) { return std::tanh(x); },
                                             [](float x) { return std::log(x); });
    for (const int level : {0, 2})
    {
        INFO("-O" << level);
        CHECK(computed(fminus::Program::Math::Vector, level) == vector);   // the default's: fminus::math
        CHECK(computed(fminus::Program::Math::Libm, level) == libm);       // the C library's
    }
    CHECK(fminus::Program::Options{}.math == fminus::Program::Math::Vector);
}

TEST_CASE("a Program describes its modules, and a host runs their methods on memory it allocates", "[library]")
{
    std::string errors;
    auto program = fminus::Program::compile("module Osc\n"
                                            "{\n"
                                            "    float phase;\n"
                                            "    float step;\n"
                                            "    float4 lanes;\n"   // aligned to 16 bytes
                                            "    Osc* next;\n"
                                            "    float history[3];\n"
                                            "    void init(float s) { step = s; }\n"
                                            "    float tick() [[clang::nonblocking]]\n"
                                            "    {\n"
                                            "        phase += step;\n"
                                            "        if (phase >= 1)\n"
                                            "            phase -= 1;\n"
                                            "        return phase;\n"
                                            "    }\n"
                                            "};\n"
                                            "module Voice\n"
                                            "{\n"
                                            "    Osc osc;\n"
                                            "    float gain;\n"
                                            "    float tick() [[clang::nonblocking]] { return gain * osc.tick(); }\n"
                                            "};\n",
                                            errors);
    INFO(errors);
    REQUIRE(program);
    REQUIRE(program->modules().size() == 2);
    CHECK(program->module("Missing") == nullptr);
    const fminus::Program::Module* osc = program->module("Osc");
    REQUIRE(osc != nullptr);
    CHECK(osc->name == "Osc");
    CHECK(osc->size == 64);   // 4, 4, padding to 16, 16, 8, 12, padding to a multiple of 16: a C struct's layout
    CHECK(osc->alignment == 16);
    REQUIRE(osc->fields.size() == 5);
    const auto described = [](const fminus::Program::Module::Field& field) {
        return field.name + " " + field.type + " " + std::to_string(field.offset) + " " + std::to_string(field.size);
    };
    CHECK(described(osc->fields[0]) == "phase float 0 4");
    CHECK(described(osc->fields[2]) == "lanes float4 16 16");
    CHECK(described(osc->fields[3]) == "next Osc * 32 8");
    CHECK(described(osc->fields[4]) == "history float[3] 40 12");
    CHECK(osc->methods == std::vector<std::string>{"Osc::init", "Osc::tick"});
    const fminus::Program::Module* voice = program->module("Voice");
    REQUIRE(voice != nullptr);
    CHECK(voice->size == 80);
    CHECK(described(voice->fields[1]) == "gain float 64 4");
    CHECK(program->nonBlocking("Voice::tick"));
    CHECK(!program->nonBlocking("Osc::init"));

    // A voice in the host's memory: aligned, zeroed (every field starts at 0), then set up through a method and a
    // field the host writes itself.
    void* memory = alignedAlloc(voice->alignment, voice->size);
    REQUIRE(memory != nullptr);
    std::memset(memory, 0, voice->size);
    auto* bytes = static_cast<unsigned char*>(memory);
    program->function<void(void*, float)>("Osc::init", errors)(bytes + voice->fields[0].offset, 0.25f);
    const float gain = 2.0f;
    std::memcpy(bytes + voice->fields[1].offset, &gain, sizeof gain);
    auto* tick = program->function<float(void*)>("Voice::tick", errors);
    REQUIRE(tick != nullptr);
    std::array<float, 5> out{};
    for (float& y : out)
        y = tick(memory);
    CHECK(out == std::array<float, 5>{0.5f, 1.0f, 1.5f, 0.0f, 0.5f});
    alignedFree(memory);
}

TEST_CASE("Programs compile, look up and run on several threads at once", "[library]")
{
    constexpr int kThreads = 4;
    std::array<float, kThreads> results{};
    std::vector<std::thread> threads;
    for (int t = 0; t < kThreads; ++t)
    {
        threads.emplace_back([t, &results] {
            std::string errors;
            auto program = fminus::Program::compile(kGain, errors);
            Gain* gain = program ? program->function<Gain>("gain", errors) : nullptr;
            if (gain == nullptr)
                return;
            const float in = 3.0f;
            float out = 0.0f;
            gain(&in, &out, 1, static_cast<float>(t + 1));
            results[static_cast<std::size_t>(t)] = out;
        });
    }
    for (std::thread& thread : threads)
        thread.join();
    CHECK(results == std::array<float, kThreads>{3.0f, 6.0f, 9.0f, 12.0f});
}

namespace
{
    // A low-pass whose cutoff is a parameter, with a switch and a mode: the parameters' tests share it.
    const char* const kLowpass = "module Lowpass\n"
                                 "{\n"
                                 "    param float cutoff [[fminus::range(0, 1)]] [[fminus::default(0.25)]];\n"
                                 "    param bool on [[fminus::default(1)]];\n"
                                 "    param int mode [[fminus::range(-4, 4)]];\n"
                                 "    float y;\n"
                                 "\n"
                                 "    void process(const float* restrict in, float* restrict out, int n)\n"
                                 "        [[clang::nonblocking]]\n"
                                 "    {\n"
                                 "        for (int i = 0; i < n; ++i)\n"
                                 "            out[i] = on[i] ? (y += cutoff[i] * (in[i] - y)) : in[i];\n"
                                 "    }\n"
                                 "\n"
                                 "    int modeOf() { return mode[0]; }\n"
                                 "};\n";

    using Process = void(void*, const float*, float*, int);

    // An instance of a module, as a host makes one: aligned, zeroed. aligned_alloc takes an alignment of at least
    // a pointer's on macOS, and a size that is a multiple of it.
    struct Instance
    {
        explicit Instance(const fminus::Program::Module& module)
            : alignment(std::max(module.alignment, alignof(std::max_align_t)))
            , memory(alignedAlloc(alignment, (module.size + alignment - 1) / alignment * alignment))
        {
            if (memory != nullptr)
                std::memset(memory, 0, module.size);
        }

        ~Instance()
        {
            alignedFree(memory);
        }

        Instance(const Instance&) = delete;
        Instance& operator=(const Instance&) = delete;

        std::size_t alignment;
        void* memory;
    };
} // namespace

TEST_CASE("a Program describes a module's parameters, and a host sets them", "[library][param]")
{
    std::string errors;
    auto program = fminus::Program::compile(kLowpass, errors);
    INFO(errors);
    REQUIRE(program);
    const fminus::Program::Module* lowpass = program->module("Lowpass");
    REQUIRE(lowpass != nullptr);
    CHECK(lowpass->size == 56);   // three slots of 16 bytes, then y
    REQUIRE(lowpass->fields.size() == 4);
    CHECK(lowpass->fields[0].type == "param float");
    CHECK(lowpass->fields[0].size == 16);
    CHECK(lowpass->fields[3].offset == 48);
    REQUIRE(lowpass->parameters.size() == 3);

    const fminus::Program::Parameter* cutoff = program->parameter("Lowpass::cutoff");
    REQUIRE(cutoff == &lowpass->parameters[0]);
    CHECK(cutoff->module == "Lowpass");
    CHECK(cutoff->name == "cutoff");
    CHECK(cutoff->type == "float");
    CHECK(cutoff->binding.kind == fminus::Program::Binding::Kind::Knob);
    CHECK(cutoff->streamOffset == 0);
    CHECK(cutoff->valueOffset == 8);
    CHECK(cutoff->minimum == 0.0);
    CHECK(cutoff->maximum == 1.0);
    CHECK(cutoff->defaultValue == 0.25);
    const fminus::Program::Parameter* on = program->parameter("Lowpass::on");
    const fminus::Program::Parameter* mode = program->parameter("Lowpass::mode");
    REQUIRE(on != nullptr);
    REQUIRE(mode != nullptr);
    CHECK(on->type == "bool");
    CHECK(on->valueOffset == 24);
    CHECK(!on->minimum.has_value());
    CHECK(mode->minimum == -4.0);
    CHECK(!mode->defaultValue.has_value());
    CHECK(program->parameter("Lowpass::y") == nullptr);   // a field, not a parameter
    CHECK(program->parameter("Lowpass::cutof") == nullptr);
    CHECK(program->parameter("cutoff") == nullptr);
    CHECK(program->nonBlocking("Lowpass::process"));

    // Zeroed, then each parameter at its default: cutoff 0.25, on; mode, with no default, 0.
    Instance instance(*lowpass);
    REQUIRE(instance.memory != nullptr);
    fminus::Program::setDefaults(instance.memory, *lowpass);
    CHECK(fminus::Program::value(instance.memory, *cutoff) == 0.25);
    CHECK(fminus::Program::value(instance.memory, *on) == 1.0);
    CHECK(fminus::Program::value(instance.memory, *mode) == 0.0);
    auto* process = program->function<Process>("Lowpass::process", errors);
    REQUIRE(process != nullptr);
    const std::array<float, 4> ones{1.0f, 1.0f, 1.0f, 1.0f};
    std::array<float, 4> out{};
    process(instance.memory, ones.data(), out.data(), 4);
    CHECK(out == std::array<float, 4>{0.25f, 0.4375f, 0.578125f, 0.68359375f});

    // An int's value truncated toward 0, a bool's true unless 0.
    fminus::Program::setValue(instance.memory, *mode, -2.9);
    CHECK(fminus::Program::value(instance.memory, *mode) == -2.0);
    CHECK(program->function<int(void*)>("Lowpass::modeOf", errors)(instance.memory) == -2);
    fminus::Program::setValue(instance.memory, *on, 0.0);
    process(instance.memory, ones.data(), out.data(), 4);
    CHECK(out == ones);
}

TEST_CASE("a Program binds parameters as the host asks: streams, frozen values, and one instance under both",
          "[library][param]")
{
    std::string errors;
    fminus::Program::Options streamed;
    streamed.parameters["Lowpass::cutoff"] = fminus::Program::Binding::stream();
    auto stream = fminus::Program::compile(kLowpass, errors, streamed);
    INFO(errors);
    REQUIRE(stream);
    fminus::Program::Options frozen;
    frozen.parameters["Lowpass::cutoff"] = fminus::Program::Binding::frozen(0.5);
    frozen.parameters["Lowpass::mode"] = fminus::Program::Binding::frozen(3);
    auto constant = fminus::Program::compile(kLowpass, errors, frozen);
    REQUIRE(constant);
    const fminus::Program::Parameter* cutoff = stream->parameter("Lowpass::cutoff");
    REQUIRE(cutoff != nullptr);
    CHECK(cutoff->binding.kind == fminus::Program::Binding::Kind::Stream);
    CHECK(constant->parameter("Lowpass::cutoff")->binding.value == 0.5);
    CHECK(stream->nonBlocking("Lowpass::process"));   // a stream allocates nothing either

    // Every binding lays the slots out alike, so one instance runs under both programs.
    const fminus::Program::Module& lowpass = *stream->module("Lowpass");
    CHECK(constant->module("Lowpass")->parameters[0].valueOffset == cutoff->valueOffset);
    Instance instance(lowpass);
    REQUIRE(instance.memory != nullptr);
    fminus::Program::setDefaults(instance.memory, lowpass);   // on; cutoff's value 0.25, which neither reads
    const std::array<float, 4> sweep{0.5f, 0.5f, 1.0f, 0.0f};
    fminus::Program::setStream(instance.memory, *cutoff, sweep.data());
    const std::array<float, 4> ones{1.0f, 1.0f, 1.0f, 1.0f};
    std::array<float, 4> out{};
    stream->function<Process>("Lowpass::process", errors)(instance.memory, ones.data(), out.data(), 4);
    CHECK(out == std::array<float, 4>{0.5f, 0.75f, 1.0f, 1.0f});   // each sample's own cutoff
    std::memset(static_cast<unsigned char*>(instance.memory) + lowpass.fields[3].offset, 0, sizeof(float));   // y = 0
    constant->function<Process>("Lowpass::process", errors)(instance.memory, ones.data(), out.data(), 4);
    CHECK(out == std::array<float, 4>{0.5f, 0.75f, 0.875f, 0.9375f});
    CHECK(constant->function<int(void*)>("Lowpass::modeOf", errors)(instance.memory) == 3);

    // A binding that names no parameter, or a stream that is not a float, does not compile.
    fminus::Program::Options wrong;
    wrong.parameters["Lowpass::q"] = fminus::Program::Binding::knob();
    wrong.parameters["Lowpass::on"] = fminus::Program::Binding::stream();
    CHECK(fminus::Program::compile(kLowpass, errors, wrong) == nullptr);
    CHECK(errors
          == "program.fm:1:1: error: the host binds 'Lowpass::q', but the program has no such parameter\n"
             "program.fm:4:16: error: parameter 'on' is a bool: only a float parameter can be a stream\n");
}

TEST_CASE("a knob set from another thread is read once a call, whole", "[library][param]")
{
    std::string errors;
    auto program = fminus::Program::compile(kLowpass, errors);
    INFO(errors);
    REQUIRE(program);
    const fminus::Program::Module& lowpass = *program->module("Lowpass");
    const fminus::Program::Parameter& cutoff = lowpass.parameters[0];
    auto* process = program->function<Process>("Lowpass::process", errors);
    REQUIRE(process != nullptr);
    Instance instance(lowpass);
    REQUIRE(instance.memory != nullptr);
    fminus::Program::setDefaults(instance.memory, lowpass);
    fminus::Program::setValue(instance.memory, cutoff, 1.0);   // with cutoff 1, y is the input: out[i] = in[i]

    // A control thread moves the knob between two values while the audio thread runs blocks. With in[i] = 1 and
    // y = 0 at each block's start, out[0] is the block's cutoff: every sample of the block shows the one value read.
    std::atomic<bool> done{false};
    std::thread control([&] {
        for (int i = 0; !done.load(std::memory_order_relaxed); ++i)
            fminus::Program::setValue(instance.memory, cutoff, i % 2 == 0 ? 1.0 : 0.5);
    });
    const std::array<float, 64> ones = [] {
        std::array<float, 64> filled{};
        filled.fill(1.0f);
        return filled;
    }();
    std::array<float, 64> out{};
    int torn = 0;
    float* y = reinterpret_cast<float*>(static_cast<unsigned char*>(instance.memory) + lowpass.fields[3].offset);
    for (int block = 0; block < 2000; ++block)
    {
        *y = 0.0f;
        process(instance.memory, ones.data(), out.data(), 64);
        const float k = out[0];
        float expected = 0.0f;
        for (float sample : out)
        {
            const float step = k * (1.0f - expected);   // one rounding at a time, as FMinus: no fused multiply-add
            expected = expected + step;
            torn += sample == expected && (k == 1.0f || k == 0.5f) ? 0 : 1;
        }
    }
    done = true;
    control.join();
    CHECK(torn == 0);
}
