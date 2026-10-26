// tests/library/ParallelTest.cpp: a program's parallel for and parallel { } through the public API, as a host runs
// them: a Program with a region starts FMinus's pool, its nonblocking process() runs the region on the pool's threads
// and its own, with the parameters the host set, and gives what one thread gives (Options::threads = 1), bit for bit;
// several Programs share the pool, and destroying them stops it. In C++17, against FMinus::FMinus alone.
#include "fminus/FMinus.h"

#include <catch2/catch_test_macros.hpp>

#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <string>
#include <thread>
#include <vector>

#if defined(_WIN32)
#include <malloc.h>   // _aligned_malloc, _aligned_free
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

        void* memory = nullptr;
        std::size_t size = 0;
    };

    // A synth of eight voices, rendered at once, each into its own buffer, then mixed; its gain a knob, its tone a
    // stream.
    const char* const kSynth = "module Voice\n"
                               "{\n"
                               "    float phase;\n"
                               "    float out[512];\n"
                               "    void render(int n, float step)\n"
                               "    {\n"
                               "        for (int i = 0; i < n; ++i)\n"
                               "        {\n"
                               "            out[i] = sinf(phase) + 0.5f * sinf(2 * phase);\n"
                               "            phase += step;\n"
                               "        }\n"
                               "    }\n"
                               "};\n"
                               "module Synth\n"
                               "{\n"
                               "    param float gain [[fminus::range(0, 1)]] [[fminus::default(0.5)]];\n"
                               "    param float tone;\n"
                               "    Voice voices[8];\n"
                               "    void process(float* restrict left, float* restrict right, int n)"
                               " [[clang::nonblocking]]\n"
                               "    {\n"
                               "        parallel for (int v = 0; v < 8; ++v)\n"
                               "            voices[v].render(n, 0.001f * (v + 1));\n"
                               "        parallel\n"
                               "        {\n"
                               "            for (int i = 0; i < n; ++i)\n"
                               "                left[i] = gain[i] * (voices[0].out[i] + voices[2].out[i] + voices[4].out[i]"
                               " + voices[6].out[i]);\n"
                               "            for (int i = 0; i < n; ++i)\n"
                               "                right[i] = gain[i] * tone[i] * (voices[1].out[i] + voices[3].out[i]"
                               " + voices[5].out[i] + voices[7].out[i]);\n"
                               "        }\n"
                               "    }\n"
                               "};\n";

    using Process = void(void*, float*, float*, int);

    std::unique_ptr<fminus::Program> compileSynth(unsigned threads)
    {
        fminus::Program::Options options;
        options.threads = threads;
        options.parameters["Synth::tone"] = fminus::Program::Binding::stream();
        std::string errors;
        auto program = fminus::Program::compile(kSynth, errors, options);
        INFO(errors);
        REQUIRE(program);
        return program;
    }
} // namespace

TEST_CASE("a Program's parallel regions run on the pool, and give what one thread gives", "[library][parallel]")
{
    auto together = compileSynth(0);
    auto alone = compileSynth(1);
    CHECK(together->nonBlocking("Synth::process"));
    const fminus::Program::Module* module = together->module("Synth");
    REQUIRE(module != nullptr);
    const Instance a(*module);
    const Instance b(*module);
    fminus::Program::setDefaults(a.memory, *module);
    fminus::Program::setDefaults(b.memory, *module);
    std::vector<float> tone(256);
    for (std::size_t i = 0; i < tone.size(); ++i)
        tone[i] = static_cast<float>(i) / 256.0f;
    fminus::Program::setStream(a.memory, *together->parameter("Synth::tone"), tone.data());
    fminus::Program::setStream(b.memory, *alone->parameter("Synth::tone"), tone.data());

    std::string errors;
    auto* processTogether = together->function<Process>("Synth::process", errors);
    auto* processAlone = alone->function<Process>("Synth::process", errors);
    REQUIRE(processTogether != nullptr);
    REQUIRE(processAlone != nullptr);
    std::vector<float> leftA(256), rightA(256), leftB(256), rightB(256);
    for (int block = 0; block < 50; ++block)
    {
        if (block == 25)
        {
            fminus::Program::setValue(a.memory, *together->parameter("Synth::gain"), 0.25);
            fminus::Program::setValue(b.memory, *alone->parameter("Synth::gain"), 0.25);
        }
        processTogether(a.memory, leftA.data(), rightA.data(), 256);
        processAlone(b.memory, leftB.data(), rightB.data(), 256);
        REQUIRE(std::memcmp(leftA.data(), leftB.data(), leftA.size() * sizeof(float)) == 0);
        REQUIRE(std::memcmp(rightA.data(), rightB.data(), rightA.size() * sizeof(float)) == 0);
    }
    CHECK(std::memcmp(a.memory, b.memory, a.size) == 0);
    CHECK(leftA[10] != 0.0f);
    CHECK(rightA[0] == 0.0f);   // tone[0] is 0
}

TEST_CASE("Programs share the pool, on several threads at once, and the last one's end stops it",
          "[library][parallel]")
{
    // Two Programs, each run from a thread of its own (two hosts' audio threads): one holds the pool at a time, and
    // the other's region runs on its own thread meanwhile. Destroyed, they leave no pool thread behind.
    for (int round = 0; round < 3; ++round)
    {
        auto first = compileSynth(0);
        auto second = compileSynth(0);
        const fminus::Program::Module* module = first->module("Synth");
        const Instance a(*module);
        const Instance b(*module);
        fminus::Program::setDefaults(a.memory, *module);
        fminus::Program::setDefaults(b.memory, *module);
        std::string errors;
        auto* runFirst = first->function<Process>("Synth::process", errors);
        auto* runSecond = second->function<Process>("Synth::process", errors);
        REQUIRE(runFirst != nullptr);
        REQUIRE(runSecond != nullptr);
        std::vector<float> leftA(128), rightA(128), leftB(128), rightB(128);
        std::thread other([&] {
            for (int block = 0; block < 100; ++block)
                runSecond(b.memory, leftB.data(), rightB.data(), 128);
        });
        for (int block = 0; block < 100; ++block)
            runFirst(a.memory, leftA.data(), rightA.data(), 128);
        other.join();
        CHECK(std::memcmp(leftA.data(), leftB.data(), leftA.size() * sizeof(float)) == 0);
        CHECK(std::memcmp(a.memory, b.memory, a.size) == 0);
    }
}
