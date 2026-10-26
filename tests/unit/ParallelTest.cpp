// tests/unit/ParallelTest.cpp: parallel for and parallel { } (extending.md, recipe 22; src/ir/Parallel.cpp) and the
// runtime's pool that runs them (src/runtime/Parallel.cpp). Through the JIT: each iteration and each statement runs
// once, whatever the range, nested, in modules' methods, with what they capture by value and by reference, and gives
// the same results on all the threads as on one; flushing subnormals on every thread under Denormals::Flush; the races
// refused, and break, continue and return. From C++, the pool itself: every chunk taken once, by several threads, at
// most `threads` of them, in the caller's floating-point mode, a region in a region on its own thread, two callers at
// once, and many small regions in a row (run them under tsan too).
#include "Compile.h"
#include "fminus/FMinus.h"

#include "runtime/Runtime.h"

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <memory>
#include <mutex>
#include <set>
#include <string>
#include <thread>
#include <vector>

#if defined(__x86_64__)
#include <immintrin.h>
#endif

using namespace fminus::test;

namespace
{
    // The runtime's pool, for a test: without it every region runs alone, on the calling thread.
    struct Pool
    {
        Pool() : threads(fminus_rt_parallel_start()) {}
        ~Pool()
        {
            fminus_rt_parallel_stop();
        }
        Pool(const Pool&) = delete;
        Pool& operator=(const Pool&) = delete;

        std::int32_t threads;   // a region's, the caller's among them
    };

    fminus::ir::Options onThreads(unsigned threads)
    {
        fminus::ir::Options options;
        options.threads = threads;
        return options;
    }

    // The floating-point control register and its flush-to-zero bits: FPCR.FZ on arm64, MXCSR's FTZ and DAZ on x86-64.
#if defined(__aarch64__)
    constexpr std::uint64_t kFlushBits = std::uint64_t{1} << 24;

    std::uint64_t floatMode()
    {
        std::uint64_t value = 0;
        asm volatile("mrs %0, fpcr" : "=r"(value) : : "memory");
        return value;
    }

    void setFloatMode(std::uint64_t value)
    {
        asm volatile("msr fpcr, %0" : : "r"(value) : "memory");
    }
#elif defined(__x86_64__)
    constexpr std::uint64_t kFlushBits = (1u << 15) | (1u << 6);

    std::uint64_t floatMode()
    {
        return _mm_getcsr();
    }

    void setFloatMode(std::uint64_t value)
    {
        _mm_setcsr(static_cast<unsigned>(value));
    }
#endif

    // Memory a module lives in, zeroed and aligned as any module's needs.
    struct Instance
    {
        explicit Instance(std::size_t bytes) : memory(new (std::align_val_t{64}) std::uint8_t[bytes]())
        {
        }
        ~Instance()
        {
            ::operator delete[](memory, std::align_val_t{64});
        }
        Instance(const Instance&) = delete;
        Instance& operator=(const Instance&) = delete;

        std::uint8_t* memory;
    };

    // A region in a region: each outer chunk starts one of 100 iterations, which must all run on its own thread.
    struct Nested
    {
        std::atomic<int> iterations{0};
        std::atomic<int> elsewhere{0};   // inner chunks run on another thread than the chunk that started them
    };

    struct Inner
    {
        Nested* nested;
        std::thread::id thread;
    };

    void innerChunk(void* context, std::int32_t begin, std::int32_t end)
    {
        const auto& inner = *static_cast<Inner*>(context);
        if (std::this_thread::get_id() != inner.thread)
            inner.nested->elsewhere.fetch_add(1);
        inner.nested->iterations.fetch_add(end - begin);
    }

    void outerChunk(void* context, std::int32_t begin, std::int32_t end)
    {
        for (std::int32_t i = begin; i < end; ++i)
        {
            Inner inner{static_cast<Nested*>(context), std::this_thread::get_id()};
            fminus_rt_parallel_for(innerChunk, &inner, 0, 100, 0);
        }
    }
} // namespace

TEST_CASE("a parallel for runs each iteration once, whatever its range", "[parallel]")
{
    const Pool pool;
    std::string errors;
    auto jit = compile("void fill(float* out, int first, int end, float k)\n"
                       "{\n"
                       "    parallel for (int i = first; i < end; ++i)\n"
                       "        out[i - first] = i * k;\n"
                       "}\n"
                       "void count(int* hits, int n)\n"
                       "{\n"
                       "    parallel for (int i = 0; i < n; i++)\n"
                       "        hits[i] += 1;\n"
                       "}\n"
                       "void through(float* out, int n)\n"   // <=: n + 1 iterations
                       "{\n"
                       "    parallel for (int i = 0; i <= n; i += 1)\n"
                       "        out[i] = 1;\n"
                       "}\n"
                       "void odd(float* out, int n)\n"   // continue ends the iteration
                       "{\n"
                       "    parallel for (int i = 0; i < n; ++i)\n"
                       "    {\n"
                       "        if (i % 2 == 0)\n"
                       "            continue;\n"
                       "        out[i] = i;\n"
                       "    }\n"
                       "}\n",
                       &errors);
    INFO(errors);
    REQUIRE(jit);
    auto fill = lookup<void(float*, int, int, float)>(*jit, "fill");
    for (const auto& [first, end] : std::vector<std::pair<int, int>>{
             {0, 0}, {5, 5}, {7, 3}, {0, 1}, {0, 2}, {-50, 50}, {3, 1003}, {0, 100000}})
    {
        const auto size = static_cast<std::size_t>(std::max(end - first, 0));
        std::vector<float> out(size + 1, -1.0f);   // one past the range: untouched
        fill(out.data(), first, end, 0.5f);
        INFO(first << " to " << end);
        for (std::size_t i = 0; i < size; ++i)
            REQUIRE(out[i] == static_cast<float>(first + static_cast<int>(i)) * 0.5f);
        CHECK(out[size] == -1.0f);
    }

    std::vector<int> hits(200000, 0);
    lookup<void(int*, int)>(*jit, "count")(hits.data(), static_cast<int>(hits.size()));
    CHECK(std::ranges::all_of(hits, [](int hit) { return hit == 1; }));

    std::vector<float> through(12, 0.0f);
    lookup<void(float*, int)>(*jit, "through")(through.data(), 10);
    CHECK(std::ranges::all_of(through.begin(), through.begin() + 11, [](float x) { return x == 1.0f; }));
    CHECK(through[11] == 0.0f);

    std::vector<float> odd(1000, -1.0f);
    lookup<void(float*, int)>(*jit, "odd")(odd.data(), 1000);
    for (std::size_t i = 0; i < odd.size(); ++i)
        REQUIRE(odd[i] == (i % 2 == 0 ? -1.0f : static_cast<float>(i)));
}

TEST_CASE("a parallel block runs each of its statements once, and a region in a region runs", "[parallel]")
{
    const Pool pool;
    std::string errors;
    auto jit = compile("float halves(float x, float* out)\n"
                       "{\n"
                       "    float a = 0;\n"
                       "    float b = 0;\n"
                       "    parallel\n"
                       "    {\n"
                       "        a = x * 2;\n"
                       "        b = x + 1;\n"
                       "        {\n"   // a block is one statement: its own run in order
                       "            out[0] = x;\n"
                       "            out[1] = out[0] * 3;\n"
                       "        }\n"
                       "        if (x > 0) out[2] = 1; else out[2] = -1;\n"
                       "        ;\n"
                       "    }\n"
                       "    return a + b;\n"
                       "}\n"
                       "void sides(float* left, float* right, int n)\n"
                       "{\n"
                       "    parallel\n"
                       "    {\n"
                       "        parallel for (int i = 0; i < n; ++i) left[i] = i;\n"
                       "        parallel for (int i = 0; i < n; ++i) right[i] = -i;\n"
                       "    }\n"
                       "}\n"
                       "void grid(float* cells, int rows, int columns)\n"
                       "{\n"
                       "    parallel for (int r = 0; r < rows; ++r)\n"
                       "        parallel for (int c = 0; c < columns; ++c)\n"
                       "            cells[r * columns + c] = r * 1000 + c;\n"
                       "}\n"
                       "void empty() { parallel { } }\n",
                       &errors);
    INFO(errors);
    REQUIRE(jit);
    std::vector<float> out(3, 0.0f);
    CHECK(lookup<float(float, float*)>(*jit, "halves")(3.0f, out.data()) == 10.0f);
    CHECK(out == std::vector<float>{3.0f, 9.0f, 1.0f});

    std::vector<float> left(20000), right(20000);
    lookup<void(float*, float*, int)>(*jit, "sides")(left.data(), right.data(), 20000);
    for (std::size_t i = 0; i < left.size(); ++i)
    {
        REQUIRE(left[i] == static_cast<float>(i));
        REQUIRE(right[i] == -static_cast<float>(i));
    }

    std::vector<float> cells(64 * 300, -1.0f);
    lookup<void(float*, int, int)>(*jit, "grid")(cells.data(), 64, 300);
    for (int r = 0; r < 64; ++r)
    {
        for (int c = 0; c < 300; ++c)
            REQUIRE(cells[static_cast<std::size_t>(r * 300 + c)] == static_cast<float>(r * 1000 + c));
    }
    lookup<void()>(*jit, "empty")();
}

TEST_CASE("a module's parallel for runs its voices at once, and gives what one thread gives", "[parallel][module]")
{
    // Each voice renders its own buffer and moves its own phase (an element of the synth's array, each iteration its
    // own); scaled() reads a field and changes nothing, so every iteration may call it.
    constexpr const char* kSynth = "module Voice\n"
                                   "{\n"
                                   "    float phase;\n"
                                   "    float level;\n"
                                   "    float out[256];\n"
                                   "    void render(int n, float step)\n"
                                   "    {\n"
                                   "        parallel for (int i = 0; i < n; ++i)\n"   // in the synth's: alone
                                   "            out[i] = level * sinf(phase + i * step);\n"
                                   "        phase += n * step;\n"
                                   "    }\n"
                                   "};\n"
                                   "module Synth\n"
                                   "{\n"
                                   "    Voice voices[8];\n"
                                   "    float gain;\n"
                                   "    void start()\n"
                                   "    {\n"
                                   "        for (int v = 0; v < 8; ++v)\n"
                                   "            voices[v].level = v + 1;\n"
                                   "        gain = 0.25f;\n"
                                   "    }\n"
                                   "    float scaled(float x) { return x * gain; }\n"
                                   "    void process(float* mix, int n) [[clang::nonblocking]]\n"
                                   "    {\n"
                                   "        parallel for (int v = 0; v < 8; ++v)\n"
                                   "            voices[v].render(n, 0.01f * (v + 1));\n"
                                   "        for (int i = 0; i < n; ++i)\n"
                                   "        {\n"
                                   "            float sum = 0;\n"
                                   "            for (int v = 0; v < 8; ++v)\n"
                                   "                sum += voices[v].out[i];\n"
                                   "            mix[i] = sum;\n"
                                   "        }\n"
                                   "        parallel for (int i = 0; i < n; ++i)\n"
                                   "            mix[i] = scaled(mix[i]);\n"
                                   "    }\n"
                                   "};\n";
    const Pool pool;
    std::string errors;
    auto together = compile(kSynth, &errors);
    INFO(errors);
    REQUIRE(together);
    auto alone = compile(kSynth, &errors, onThreads(1));
    REQUIRE(alone);

    const Instance a(16384);
    const Instance b(16384);
    lookup<void(void*)>(*together, "Synth::start")(a.memory);
    lookup<void(void*)>(*alone, "Synth::start")(b.memory);
    auto processTogether = lookup<void(void*, float*, int)>(*together, "Synth::process");
    auto processAlone = lookup<void(void*, float*, int)>(*alone, "Synth::process");
    std::vector<float> mixA(256), mixB(256);
    for (int block = 0; block < 20; ++block)
    {
        processTogether(a.memory, mixA.data(), 256);
        processAlone(b.memory, mixB.data(), 256);
        REQUIRE(std::memcmp(mixA.data(), mixB.data(), mixA.size() * sizeof(float)) == 0);   // bit for bit
    }
    CHECK(std::memcmp(a.memory, b.memory, 16384) == 0);   // every voice's state too
    // And what it computes: the last block's first sample, each voice at the phase its 19 blocks before took it to, in
    // C++ one rounding at a time (clang would fuse a multiply and an add written in one expression; FMinus never does).
    float sum = 0.0f;
    for (int v = 0; v < 8; ++v)
    {
        const float step = 0.01f * static_cast<float>(v + 1);
        float phase = 0.0f;
        for (int block = 0; block < 19; ++block)
        {
            const float advance = 256.0f * step;
            phase = phase + advance;
        }
        const float sample = static_cast<float>(v + 1) * fminus::math::sin(phase);
        sum = sum + sample;
    }
    CHECK(mixA[0] == sum * 0.25f);
}

TEST_CASE("a region reads what its function read where it starts: knobs, streams, and its variables", "[parallel][module]")
{
    // A knob's value and a stream's samples, read inside a region as the method reads them; a variable assigned before
    // the region, read in it (by value); one the region's block assigns (by reference).
    std::string errors;
    const Pool pool;
    auto jit = compile("module Gain\n"
                       "{\n"
                       "    param float level;\n"
                       "    param float shape;\n"
                       "    float last;\n"
                       "    void process(float* out, int n) [[clang::nonblocking]]\n"
                       "    {\n"
                       "        float offset = 1;\n"
                       "        offset = offset * 2;\n"
                       "        parallel for (int i = 0; i < n; ++i)\n"
                       "            out[i] = level[i] * shape[i] + offset;\n"
                       "        float first = 0;\n"
                       "        parallel\n"
                       "        {\n"
                       "            first = out[0];\n"
                       "            last = out[n - 1];\n"
                       "        }\n"
                       "        out[0] = first;\n"
                       "    }\n"
                       "};\n",
                       &errors, [] {
                           fminus::ir::Options options;
                           options.parameters["Gain::shape"] = {fminus::ir::ParamBinding::Kind::Stream, 0.0};
                           return options;
                       }());
    INFO(errors);
    REQUIRE(jit);
    // { const float* stream; float value; } for each parameter, then last.
    struct Slot
    {
        const float* stream;
        float value;
    };
    struct Gain
    {
        Slot level;
        Slot shape;
        float last;
    };
    Gain gain{{nullptr, 0.5f}, {nullptr, 3.0f}, 0.0f};
    std::vector<float> shape(5000);
    for (std::size_t i = 0; i < shape.size(); ++i)
        shape[i] = static_cast<float>(i);
    gain.shape.stream = shape.data();
    std::vector<float> out(5000);
    auto process = lookup<void(Gain*, float*, int)>(*jit, "Gain::process");
    process(&gain, out.data(), 5000);
    for (std::size_t i = 0; i < out.size(); ++i)
        REQUIRE(out[i] == 0.5f * static_cast<float>(i) + 2.0f);
    CHECK(gain.last == 0.5f * 4999.0f + 2.0f);

    gain.shape.stream = nullptr;   // a flat stream: its value, every sample
    process(&gain, out.data(), 5000);
    CHECK(std::ranges::all_of(out, [](float x) { return x == 0.5f * 3.0f + 2.0f; }));
}

#if defined(__aarch64__) || defined(__x86_64__)
TEST_CASE("under Denormals::Flush a region flushes subnormals on every thread", "[parallel][denormals]")
{
    // A nonblocking entry point sets flush-to-zero for its body; the pool's threads run its region in that mode too.
    // 1e-30 * 1e-10 is 1e-40, a subnormal: 0 when flushed. A host in IEEE's mode calls it.
    const Pool pool;
    constexpr const char* kScale = "void scale(const float* restrict in, float* restrict out, int n, float k)"
                                   " [[clang::nonblocking]]\n"
                                   "{\n"
                                   "    parallel for (int i = 0; i < n; ++i)\n"
                                   "        out[i] = in[i] * k;\n"
                                   "}\n";
    fminus::ir::Options flushing;
    flushing.denormals = fminus::ir::Denormals::Flush;
    std::string errors;
    auto flushed = compile(kScale, &errors, flushing);
    INFO(errors);
    REQUIRE(flushed);
    auto preserved = compile(kScale, &errors);
    REQUIRE(preserved);
    REQUIRE((floatMode() & kFlushBits) == 0);
    std::vector<float> in(400000, 1e-30f), out(400000, 1.0f);
    lookup<void(const float*, float*, int, float)>(*flushed, "scale")(in.data(), out.data(), 400000, 1e-10f);
    CHECK(std::ranges::all_of(out, [](float x) { return x == 0.0f; }));
    CHECK((floatMode() & kFlushBits) == 0);   // the caller's mode back
    const volatile float tiny = 1e-30f;
    const float subnormal = tiny * 1e-10f;
    REQUIRE(subnormal != 0.0f);
    lookup<void(const float*, float*, int, float)>(*preserved, "scale")(in.data(), out.data(), 400000, 1e-10f);
    CHECK(std::ranges::all_of(out, [&](float x) { return x == subnormal; }));
}
#endif

TEST_CASE("a parallel for refuses what every iteration would race on", "[parallel][errors]")
{
    const std::string synth = "module OnePole\n"
                              "{\n"
                              "    float y;\n"
                              "    float k;\n"
                              "    float tick(float x) { y += k * (x - y); return y; }\n"
                              "    float gain() { return k; }\n"
                              "};\n"
                              "module Synth\n"
                              "{\n"
                              "    OnePole voices[4];\n"
                              "    OnePole filter;\n"
                              "    float level;\n"
                              "    void bump() { level += 1; }\n"
                              "    void process(float* out, int n)\n"
                              "    {\n"
                              "        float total = 0;\n"
                              "        parallel for (int i = 0; i < n; ++i)\n"
                              "        {\n";
    const auto errors = [&](const std::string& body) { return errorsOf(synth + body + "        }\n    }\n};\n"); };
    CHECK(errors("            total += out[i];\n")
          == "test.fm:19:13: error: every iteration of the parallel for assigns 'total', and the iterations run at "
             "once: give each an element of its own (an array indexed by the loop variable), or combine them after "
             "the loop\n");
    CHECK(errors("            level = out[i];\n")
          == "test.fm:19:13: error: every iteration of the parallel for assigns 'level', and the iterations run at "
             "once: give each an element of its own (an array indexed by the loop variable), or combine them after "
             "the loop\n");
    CHECK(errors("            filter.tick(out[i]);\n")
          == "test.fm:19:20: error: every iteration of the parallel for calls 'OnePole::tick' on 'filter', which "
             "changes it, and the iterations run at once: give each iteration a module of its own (an array of them, "
             "indexed by the loop variable)\n");
    CHECK(errors("            bump();\n")
          == "test.fm:19:13: error: every iteration of the parallel for calls 'Synth::bump', which changes the module "
             "it is called on ('this'), and the iterations run at once\n");
    CHECK(errors("            i = 3;\n")
          == "test.fm:19:13: error: cannot assign 'i', the loop variable of a parallel for: each iteration has its "
             "own\n");
    CHECK(errors("            if (out[i] > 1) break;\n")
          == "test.fm:19:29: error: 'break' cannot leave a parallel for: its iterations run at once, and none can end "
             "the others\n");
    CHECK(errors("            return;\n")
          == "test.fm:19:13: error: cannot return from a parallel for: its iterations run at once, and none can end "
             "the function\n");
    // What each iteration reaches through an index, or a pointer, and what changes nothing, it may: no error.
    std::string none;
    CHECK(compile(synth
                      + "            out[i] = filter.gain() * level;\n"
                        "            voices[i % 4].tick(out[i]);\n"
                        "            float* p = out + i;\n"
                        "            *p += 1;\n"
                        "            float mine = total;\n"
                        "            mine += 1;\n"
                        "            if (mine > 2) continue;\n"
                        "        }\n    }\n};\n",
                  &none)
          != nullptr);
    CHECK(none.empty());
}

TEST_CASE("a parallel for counts up by one, from an int it declares", "[parallel][errors]")
{
    const auto errors = [](const std::string& head) {
        return errorsOf("void f(float* out, int n)\n{\n    " + head + " { }\n}\n");
    };
    CHECK(errors("parallel for (float x = 0; x < 3; x += 1)")
          == "test.fm:3:25: error: the loop variable of a parallel for is an int, not 'float': parallel for (int i = "
             "first; i < end; ++i)\n");
    CHECK(errors("parallel for (int i = 0; i < n; i += 2)")
          == "test.fm:3:39: error: a parallel for counts up by one: its step is '++i', 'i++' or 'i += 1'\n");
    CHECK(errors("parallel for (int i = 0; n > i; ++i)")
          == "test.fm:3:32: error: the condition of a parallel for is where it ends, 'i < end' or 'i <= end': "
             "parallel for (int i = first; i < end; ++i)\n");
    CHECK(errors("parallel for (int i = 0; i < i + n; ++i)")
          == "test.fm:3:36: error: the end of a parallel for is computed once, before the loop: it cannot use 'i'\n");
    CHECK(errors("parallel for (int i = 0; i < 2.5f; ++i)")
          == "test.fm:3:34: error: the end of a parallel for is an int, not 'float'\n");
    CHECK(errors("int i = 0; parallel for (; i < n; ++i)")
          == "test.fm:3:16: error: a parallel for declares the int it counts with, and where it starts: parallel for "
             "(int i = first; i < end; ++i)\n");
    // The runtime's function is the runtime's: a program's of its name would take the call.
    CHECK(errorsOf("void fminus_rt_parallel_for(int x) { }\nvoid f(float* out) { parallel for (int i = 0; i < 4; ++i) "
                   "out[i] = 0; }\n")
          == "test.fm:2:22: error: 'parallel' reserves 'fminus_rt_parallel_for' for the runtime's function, and this "
             "program declares a function of that name: rename it\n"
             "test.fm:1:6: note: 'fminus_rt_parallel_for' declared here\n");
}

TEST_CASE("a parallel block refuses statements that race, and declarations", "[parallel][errors]")
{
    const std::string head = "module OnePole\n"
                             "{\n"
                             "    float y;\n"
                             "    float k;\n"
                             "    float tick(float x) { y += k * (x - y); return y; }\n"
                             "    float gain() { return k; }\n"
                             "};\n"
                             "float f(float x)\n"
                             "{\n"
                             "    OnePole left;\n"
                             "    OnePole right;\n"
                             "    float a = 0;\n"
                             "    float b = 0;\n"
                             "    parallel\n"
                             "    {\n";
    const auto errors = [&](const std::string& body) { return errorsOf(head + body + "    }\n    return a + b;\n}\n"); };
    CHECK(errors("        a = x;\n"
                 "        b = a + 1;\n")
          == "test.fm:17:13: error: the statements of a parallel block run at once: this one uses 'a', which another "
             "changes\n"
             "test.fm:16:9: note: 'a' is changed here\n");
    CHECK(errors("        a = x;\n"
                 "        a = 2;\n")
          == "test.fm:17:9: error: the statements of a parallel block run at once: this one changes 'a', which "
             "another changes too\n"
             "test.fm:16:9: note: 'a' is changed here\n");
    CHECK(errors("        a = left.gain();\n"
                 "        b = left.tick(x);\n")
          == "test.fm:17:18: error: the statements of a parallel block run at once: this one changes 'left', which "
             "another uses\n"
             "test.fm:16:18: note: 'left' is used here\n");
    CHECK(errors("        float c = 2;\n")
          == "test.fm:16:15: error: a statement of a parallel block cannot declare a variable ('c' would end with "
             "it): declare it before the block, and assign it in one of the block's statements\n");
    CHECK(errors("        if (x > 0) break;\n")
          == "test.fm:16:20: error: 'break' cannot leave a statement of a parallel block\n");
    CHECK(errors("        for (int i = 0; i < 2; ++i) continue;\n"
                 "        if (x > 0) continue;\n")
          == "test.fm:17:20: error: 'continue' cannot leave a statement of a parallel block\n");
    CHECK(errors("        return 1;\n") == "test.fm:16:9: error: cannot return from a statement of a parallel block\n");
    // Two filters, each its own statement's, and a value both read: no error.
    std::string none;
    CHECK(compile(head
                      + "        a = left.tick(x);\n"
                        "        b = right.tick(x);\n"
                        "    }\n    return a + b;\n}\n",
                  &none)
          != nullptr);
    CHECK(none.empty());
}

TEST_CASE("the pool takes every chunk once, on several threads, at most `threads` of them", "[parallel][runtime]")
{
    const Pool pool;
    REQUIRE(pool.threads == static_cast<std::int32_t>(std::max(1u, std::thread::hardware_concurrency())));

    // Every iteration once, over many ranges.
    struct Hits
    {
        std::vector<std::atomic<int>> hits;
        std::int32_t first;
    };
    const auto mark = [](void* context, std::int32_t begin, std::int32_t end) {
        auto& hits = *static_cast<Hits*>(context);
        for (std::int32_t i = begin; i < end; ++i)
            hits.hits[static_cast<std::size_t>(i - hits.first)].fetch_add(1, std::memory_order_relaxed);
    };
    for (const auto& [first, end] : std::vector<std::pair<std::int32_t, std::int32_t>>{
             {0, 1}, {0, 2}, {0, 3}, {-7, 9}, {100, 1123}, {0, 65536}, {-1000000, -999000}})
    {
        Hits hits{std::vector<std::atomic<int>>(static_cast<std::size_t>(end - first)), first};
        fminus_rt_parallel_for(mark, &hits, first, end, 0);
        INFO(first << " to " << end);
        CHECK(std::ranges::all_of(hits.hits, [](const std::atomic<int>& hit) { return hit.load() == 1; }));
    }

    // Several threads take part, and no more than `threads`: each chunk sleeps, so that the others come to take one.
    struct Threads
    {
        std::mutex lock;
        std::set<std::thread::id> seen;
    };
    const auto note = [](void* context, std::int32_t, std::int32_t) {
        auto& threads = *static_cast<Threads*>(context);
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
        const std::lock_guard<std::mutex> held(threads.lock);
        threads.seen.insert(std::this_thread::get_id());
    };
    // A busy machine may not run a pool thread before the caller has taken every chunk: a few tries.
    if (pool.threads >= 2)
    {
        std::size_t most = 0;
        for (int attempt = 0; attempt < 20 && most < 2; ++attempt)
        {
            Threads all;
            fminus_rt_parallel_for(note, &all, 0, 4 * pool.threads, 0);
            CHECK(all.seen.size() <= static_cast<std::size_t>(pool.threads));
            most = std::max(most, all.seen.size());
        }
        CHECK(most >= 2);
        for (int attempt = 0; attempt < 5; ++attempt)
        {
            Threads two;
            fminus_rt_parallel_for(note, &two, 0, 64, 2);
            CHECK(two.seen.size() <= 2);
        }
    }
    Threads one;
    fminus_rt_parallel_for(note, &one, 0, 8, 1);
    CHECK(one.seen == std::set<std::thread::id>{std::this_thread::get_id()});
}

#if defined(__aarch64__) || defined(__x86_64__)
TEST_CASE("the pool runs every chunk in its caller's floating-point mode", "[parallel][runtime]")
{
    const Pool pool;
    struct Modes
    {
        std::mutex lock;
        std::set<std::uint64_t> seen;
        std::set<std::thread::id> threads;
    };
    const auto note = [](void* context, std::int32_t, std::int32_t) {
        auto& modes = *static_cast<Modes*>(context);
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
        const std::lock_guard<std::mutex> held(modes.lock);
        modes.seen.insert(floatMode());
        modes.threads.insert(std::this_thread::get_id());
    };
    const std::uint64_t ieee = floatMode();
    for (const std::uint64_t mode : {ieee | kFlushBits, ieee & ~kFlushBits})
    {
        std::size_t most = 0;   // threads that took part (a busy machine may run none of the pool's: a few tries)
        for (int attempt = 0; attempt < 20 && most < 2; ++attempt)
        {
            setFloatMode(mode);
            Modes modes;
            fminus_rt_parallel_for(note, &modes, 0, 4 * pool.threads, 0);
            setFloatMode(ieee);
            CHECK(modes.seen == std::set<std::uint64_t>{mode});
            most = std::max(most, modes.threads.size());
        }
        if (pool.threads >= 2)
            CHECK(most >= 2);
    }
}
#endif

TEST_CASE("a region in a region runs on its thread, and two callers share the pool", "[parallel][runtime]")
{
    const Pool pool;

    // A region a chunk starts runs on that chunk's thread, whole.
    Nested nested;
    fminus_rt_parallel_for(outerChunk, &nested, 0, 64, 0);
    CHECK(nested.iterations.load() == 64 * 100);
    CHECK(nested.elsewhere.load() == 0);

    // Two callers at once, each its own region: one holds the pool, the other runs alone, and both finish.
    const auto add = [](void* context, std::int32_t begin, std::int32_t end) {
        auto& total = *static_cast<std::atomic<std::int64_t>*>(context);
        std::int64_t sum = 0;
        for (std::int32_t i = begin; i < end; ++i)
            sum += i;
        total.fetch_add(sum);
    };
    std::atomic<std::int64_t> first{0}, second{0};
    std::thread other([&] {
        for (int round = 0; round < 200; ++round)
            fminus_rt_parallel_for(add, &first, 0, 10000, 0);
    });
    for (int round = 0; round < 200; ++round)
        fminus_rt_parallel_for(add, &second, 0, 10000, 0);
    other.join();
    const std::int64_t each = std::int64_t{10000} * 9999 / 2;
    CHECK(first.load() == 200 * each);
    CHECK(second.load() == 200 * each);
}

TEST_CASE("the pool runs many small regions in a row, and stops when its last user does", "[parallel][runtime]")
{
    const auto add = [](void* context, std::int32_t begin, std::int32_t end) {
        auto& total = *static_cast<std::atomic<std::int64_t>*>(context);
        total.fetch_add(end - begin, std::memory_order_relaxed);
    };
    {
        const Pool pool;
        const Pool again;   // a second user: one pool
        CHECK(again.threads == pool.threads);
        std::atomic<std::int64_t> total{0};
        for (int round = 0; round < 20000; ++round)
            fminus_rt_parallel_for(add, &total, 0, 64, 0);
        CHECK(total.load() == std::int64_t{20000} * 64);
    }
    // Stopped: a region runs on the caller alone.
    struct Threads
    {
        std::mutex lock;
        std::set<std::thread::id> seen;
    } threads;
    fminus_rt_parallel_for(
        [](void* context, std::int32_t, std::int32_t) {
            auto& t = *static_cast<Threads*>(context);
            const std::lock_guard<std::mutex> held(t.lock);
            t.seen.insert(std::this_thread::get_id());
        },
        &threads, 0, 1000, 0);
    CHECK(threads.seen == std::set<std::thread::id>{std::this_thread::get_id()});
}
