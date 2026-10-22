// tests/unit/ZeroInitTest.cpp: zero-init elision (src/opt/ZeroInitElision.h). A local array's memset goes when the
// program overwrites every element it reads before reading it, and stays otherwise; either way a program reads what C
// with FMinus's zeroing says it reads. Each call runs on a dirty stack (dirtyStack), so a memset removed when it should
// not have been shows as garbage, not as a lucky 0. The IR checks are in tests/lit/fminus/zeroinit.fm.
#include "Compile.h"
#include "opt/ZeroInitElision.h"
#include "support/HostTarget.h"

#include <catch2/catch_test_macros.hpp>

#include <llvm/Analysis/LoopAnalysisManager.h>
#include <llvm/AsmParser/Parser.h>
#include <llvm/IR/IntrinsicInst.h>
#include <llvm/IR/PassManager.h>
#include <llvm/Passes/PassBuilder.h>
#include <llvm/Support/SourceMgr.h>
#include <llvm/Target/TargetMachine.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <string>
#include <type_traits>
#include <vector>

using namespace fminus::test;

namespace
{
    constexpr std::array kLevels{llvm::OptimizationLevel::O1, llvm::OptimizationLevel::O2,
                                 llvm::OptimizationLevel::O3};

    const char* levelName(llvm::OptimizationLevel level)
    {
        return level == llvm::OptimizationLevel::O1 ? "-O1" : level == llvm::OptimizationLevel::O2 ? "-O2" : "-O3";
    }

    // Fills 64 KB of the stack below the caller's frame with 1e6, where the frames of the next calls the caller makes
    // will be: a local array that is not zeroed reads that, not 0. Volatile, so the stores stay. The top few dozen
    // bytes keep what dirtyStack saved there (registers, a canary), which padded() steps over.
    [[gnu::noinline]] void dirtyStack()
    {
        volatile float junk[16384];
        for (int i = 0; i < 16384; ++i)
            junk[i] = 1.0e6f;
    }

    // Calls `function` from below a kilobyte of padding, so its whole frame lies where dirtyStack wrote. The padding's
    // address escapes into an empty asm statement, so all of it stays in this frame, and does again after the call, so
    // the call is not a tail call (which would drop this frame first).
    template <typename Result, typename... Args>
    [[gnu::noinline]] Result padded(Result (*function)(Args...), std::type_identity_t<Args>... args)
    {
        char padding[1024];
        asm volatile("" : : "r"(padding) : "memory");
        if constexpr (std::is_void_v<Result>)
        {
            function(args...);
            asm volatile("" : : "r"(padding) : "memory");
        }
        else
        {
            const Result result = function(args...);
            asm volatile("" : : "r"(padding) : "memory");
            return result;
        }
    }

    // function(args...) on a dirty stack.
    template <typename Result, typename... Args>
    [[gnu::noinline]] Result dirty(Result (*function)(Args...), std::type_identity_t<Args>... args)
    {
        dirtyStack();
        return padded(function, args...);
    }

    // As Compile.h's compile(), at any level.
    std::unique_ptr<fminus::jit::Jit> compileAt(std::string_view source, llvm::OptimizationLevel level)
    {
        fminus::Diagnostics diagnostics("test.fm");
        const auto program = fminus::frontend::parse(source, diagnostics);
        auto context = std::make_unique<llvm::LLVMContext>();
        std::unique_ptr<llvm::Module> module = program ? fminus::ir::lower(*program, *context, diagnostics) : nullptr;
        INFO(diagnostics.format());
        REQUIRE(module);
        auto jit = llvm::cantFail(fminus::jit::Jit::create(level));
        llvm::cantFail(jit->add(std::move(module), std::move(context)));
        return jit;
    }

    // The program's IR after optimiseModule at `level`, as the JIT compiles it, and the memsets each function kept.
    struct Optimised
    {
        std::unique_ptr<llvm::LLVMContext> context;
        std::unique_ptr<llvm::Module> module;

        int memsets(std::string_view name) const
        {
            const llvm::Function* function = module->getFunction(name);
            REQUIRE(function != nullptr);
            int count = 0;
            for (const llvm::BasicBlock& block : *function)
            {
                for (const llvm::Instruction& instruction : block)
                    count += llvm::isa<llvm::MemSetInst>(instruction) ? 1 : 0;
            }
            return count;
        }
    };

    Optimised optimise(std::string_view source, llvm::OptimizationLevel level)
    {
        fminus::Diagnostics diagnostics("test.fm");
        const auto program = fminus::frontend::parse(source, diagnostics);
        auto machine = llvm::cantFail(fminus::hostTargetMachine(fminus::codeGenLevel(level)));
        fminus::ir::Options options;
        options.polyLanes = fminus::polyLanes(*machine);
        options.dataLayout = machine->createDataLayout().getStringRepresentation();
        Optimised optimised{std::make_unique<llvm::LLVMContext>(), nullptr};
        optimised.module = program ? fminus::ir::lower(*program, *optimised.context, diagnostics, options) : nullptr;
        INFO(diagnostics.format());
        REQUIRE(optimised.module);
        fminus::prepareModule(*optimised.module, *machine);
        fminus::optimiseModule(*optimised.module, *machine, level);
        return optimised;
    }

    // 64 small whole numbers, so every sum below is exact in float; and the same with two of them negative.
    std::array<float, 64> ramp()
    {
        std::array<float, 64> values{};
        for (std::size_t i = 0; i < values.size(); ++i)
            values[i] = static_cast<float>(i % 7 + 1);
        return values;
    }

    std::array<float, 64> rampWithNegatives()
    {
        std::array<float, 64> values = ramp();
        values[5] = -3.0f;
        values[9] = -2.0f;
        return values;
    }

    // 2 * values[i] summed over [from, to).
    float doubled(const std::array<float, 64>& values, int from, int to)
    {
        float sum = 0.0f;
        for (int i = from; i < to; ++i)
            sum += 2.0f * values[static_cast<std::size_t>(i)];
        return sum;
    }

    // The programs whose memsets go: a loop or a fill writes, from element 0, as far as the reads go.
    constexpr const char* kCovered = R"(
module Smooth
{
    float y;
    void process(const float* restrict in, float* restrict out, int n) [[clang::nonblocking]]
    {
        float tmp[256];
        for (int i = 0; i < n; ++i)
            tmp[i] = in[i] * 0.5f;
        for (int i = 0; i < n; ++i)
        {
            y += 0.01f * (tmp[i] - y);
            out[i] = y;
        }
    }
};

module SmoothField
{
    float y;
    float tmp[256];
    void process(const float* restrict in, float* restrict out, int n) [[clang::nonblocking]]
    {
        for (int i = 0; i < n; ++i)
            tmp[i] = in[i] * 0.5f;
        for (int i = 0; i < n; ++i)
        {
            y += 0.01f * (tmp[i] - y);
            out[i] = y;
        }
    }
};

module Voices
{
    Smooth voices[4];
    void process(const float* restrict in, float* restrict out, int n)
    {
        for (int v = 0; v < 4; ++v)
            voices[v].process(in, out, n);
    }
};

module Counted
{
    int count;
    float run(const float* restrict in)
    {
        float t[64];
        for (int i = 0; i < count; ++i)
            t[i] = in[i] * 2.0f;
        float s = 0;
        for (int i = 0; i < count; ++i)
            s += t[i];
        return s;
    }
};

float sum64(const float* restrict in)
{
    float t[64];
    for (int i = 0; i < 64; ++i)
        t[i] = in[i] * 2.0f;
    float s = 0;
    for (int i = 0; i < 64; ++i)
        s += t[i];
    return s;
}

float copied(const float* restrict in, int n)
{
    float t[64];
    for (int i = 0; i < n; ++i)
        t[i] = in[i];
    float s = 0;
    for (int i = 0; i < n; ++i)
        s += t[i] * 2.0f;
    return s;
}

float filled(int n)
{
    float t[64];
    for (int i = 0; i < n; ++i)
        t[i] = 1.5f;
    float s = 0;
    for (int i = 0; i < n; ++i)
        s += t[i];
    return s;
}

float triangle(const float* restrict in)
{
    float a[16];
    float s = 0;
    for (int j = 0; j < 16; ++j)
    {
        for (int i = 0; i < j; ++i)
            a[i] = in[i] * 2.0f;
        for (int i = 0; i < j; ++i)
            s += a[i];
    }
    return s;
}

void blocks(const float* restrict in, float* restrict out, int n, int count)
{
    for (int b = 0; b < count; ++b)
    {
        float tmp[64];
        for (int i = 0; i < n; ++i)
            tmp[i] = in[b * n + i] * 2.0f;
        for (int i = 0; i < n; ++i)
            out[b * n + i] = tmp[i] + 1.0f;
    }
}
)";

    // The programs whose memsets stay: each reads an element nothing wrote since the declaration, or may.
    constexpr const char* kUncovered = R"(
float shorter(const float* restrict in, int n)
{
    float t[64];
    for (int i = 0; i < n - 1; ++i) t[i] = in[i] * 2.0f;
    float s = 0;
    for (int i = 0; i < n; ++i) s += t[i];
    return s;
}
float accumulated(const float* restrict in, int n)
{
    float t[64];
    for (int i = 0; i < n; ++i) t[i] = t[i] + in[i];
    float s = 0;
    for (int i = 0; i < n; ++i) s += t[i];
    return s;
}
float positive(const float* restrict in, int n)
{
    float t[64];
    for (int i = 0; i < n; ++i)
        if (in[i] > 0) t[i] = in[i];
    float s = 0;
    for (int i = 0; i < n; ++i) s += t[i];
    return s;
}
float untilNegative(const float* restrict in, int n)
{
    float t[64];
    for (int i = 0; i < n; ++i)
    {
        if (in[i] < 0) break;
        t[i] = in[i] * 2.0f;
    }
    float s = 0;
    for (int i = 0; i < n; ++i) s += t[i];
    return s;
}
float evens(const float* restrict in, int n)
{
    float t[64];
    for (int i = 0; i < n / 2; ++i) t[2 * i] = in[i] * 2.0f;
    float s = 0;
    for (int i = 0; i < n; ++i) s += t[i];
    return s;
}
float fromOne(const float* restrict in, int n)
{
    float t[64];
    for (int i = 1; i < n; ++i) t[i] = in[i] * 2.0f;
    float s = 0;
    for (int i = 0; i < n; ++i) s += t[i];
    return s;
}
float pastEnd(const float* restrict in, int n)
{
    float t[64];
    for (int i = 0; i < n; ++i) t[i] = in[i] * 2.0f;
    return t[n];
}
float onePast(const float* restrict in, int n)
{
    float t[64];
    for (int i = 0; i < n; ++i) t[i] = in[i] * 2.0f;
    float s = 0;
    for (int i = 0; i <= n; ++i) s += t[i];
    return s;
}
float grown(const float* restrict in, int n)
{
    float t[64];
    for (int i = 0; i < n; ++i) t[i] = in[i] * 2.0f;
    n = n + 1;
    float s = 0;
    for (int i = 0; i < n; ++i) s += t[i];
    return s;
}
float twoBounds(const float* restrict in, int n, int m)
{
    float t[64];
    for (int i = 0; i < n; ++i) t[i] = in[i] * 2.0f;
    float s = 0;
    for (int i = 0; i < m; ++i) s += t[i];
    return s;
}
float shifted(const float* restrict in, int n)
{
    float t[64];
    float* p = t + 2;
    for (int i = 0; i < n; ++i) p[i] = in[i] * 2.0f;
    float s = 0;
    for (int i = 0; i < n; ++i) s += t[i];
    return s;
}
float first(const float* restrict in, int n)
{
    float t[64];
    for (int i = 0; i < n; ++i) t[i] = in[i] * 3.0f;
    return t[0];
}
float firstIf(const float* restrict in, int n, int m)
{
    float t[64];
    for (int i = 0; i < n; ++i) t[i] = in[i] * 2.0f;
    float s = 0;
    if (m > 0) s = t[0];
    return s;
}
float growing(const float* restrict in, int n)
{
    float s = 0;
    for (int r = 0; r < 4; ++r)
    {
        float a[64];
        for (int i = 0; i < r; ++i) a[i] = in[i] * 2.0f;
        for (int i = 0; i < n; ++i) s += a[i];
    }
    return s;
}
float outside(const float* restrict in)
{
    float a[8];
    float s = 0;
    for (int r = 0; r < 4; ++r)
    {
        for (int i = 0; i < 2; ++i) a[i] = in[i] * 2.0f;
        s += a[r];
    }
    return s;
}
float alternate(const float* restrict in, int n)
{
    float t[64];
    float s = 0;
    for (int r = 0; r < 4; ++r)
    {
        int m = 0;
        if (r % 2 == 1) m = n;
        for (int i = 0; i < m; ++i) t[i] = in[i] * 2.0f;
        for (int i = 0; i < n; ++i) s += t[i];
    }
    return s;
}
float padded(const float* restrict in, int n)
{
    float frame[64];
    for (int i = 0; i < n; ++i) frame[i] = in[i] * 2.0f;
    void* plan = fft_create(64);
    float2 bins[33];
    fft_forward(plan, frame, bins);
    fft_destroy(plan);
    return bins[0].x;
}
)";
} // namespace

TEST_CASE("a local array that the program overwrites before reading it loses its memset", "[codegen][zeroinit]")
{
    for (const llvm::OptimizationLevel level : kLevels)
    {
        INFO(levelName(level));
        const Optimised optimised = optimise(kCovered, level);
        CHECK(optimised.memsets("Smooth::process") == 0);    // the bound an argument
        CHECK(optimised.memsets("Voices::process") == 0);    // the same, inlined four times
        CHECK(optimised.memsets("Counted::run") == 0);       // the bound a field, read after the declaration
        CHECK(optimised.memsets("sum64") == 0);              // a constant bound
        CHECK(optimised.memsets("copied") == 0);             // a copy, which LLVM turns into a memcpy
        CHECK(optimised.memsets("filled") == 0);             // a fill, which LLVM turns into a memset.pattern
        CHECK(optimised.memsets("triangle") == 0);           // declared before a loop whose iterations write and read
        CHECK(optimised.memsets("blocks") == 0);             // declared in a loop, zeroed afresh each iteration
        CHECK(optimised.memsets("SmoothField::process") == 0);   // a field has no memset to begin with
    }
}

TEST_CASE("without the memset, a program reads what it wrote, for every block size", "[codegen][zeroinit]")
{
    const std::array<float, 64> in = ramp();
    std::array<float, 256> input{};
    for (std::size_t i = 0; i < input.size(); ++i)
        input[i] = static_cast<float>(i % 13) - 6.0f;
    for (const llvm::OptimizationLevel level : kLevels)
    {
        INFO(levelName(level));
        auto jit = compileAt(kCovered, level);
        using Process = void(void*, const float*, float*, int);
        auto* local = lookup<Process>(*jit, "Smooth::process");
        auto* field = lookup<Process>(*jit, "SmoothField::process");
        auto* voices = lookup<Process>(*jit, "Voices::process");

        // The local's filter and the field's agree bit for bit, n from 0 (nothing is read) to the array's size.
        std::vector<float> localState(1, 0.0f);
        std::vector<float> fieldState(257, 0.0f);
        std::vector<float> voicesState(4, 0.0f);
        for (const int n : {0, 1, 3, 32, 64, 255, 256, 0, 17})
        {
            INFO("n = " << n);
            std::array<float, 256> fromLocal{};
            std::array<float, 256> fromField{};
            std::array<float, 256> fromVoices{};
            dirty(local, localState.data(), input.data(), fromLocal.data(), n);
            dirty(field, fieldState.data(), input.data(), fromField.data(), n);
            dirty(voices, voicesState.data(), input.data(), fromVoices.data(), n);
            CHECK(fromLocal == fromField);
            CHECK(localState[0] == fieldState[0]);
            CHECK(fromVoices == fromLocal);   // each voice filters as one Smooth does, the last one's output kept
            CHECK(voicesState == std::vector<float>(4, localState[0]));
        }

        std::vector<int> counted{0};
        auto* run = lookup<float(void*, const float*)>(*jit, "Counted::run");
        for (const int count : {0, 1, 5, 64})
        {
            counted[0] = count;
            CHECK(dirty(run, counted.data(), in.data()) == doubled(in, 0, count));
        }
        CHECK(dirty(lookup<float(const float*)>(*jit, "sum64"), in.data()) == doubled(in, 0, 64));
        for (const int n : {0, 1, 7, 64})
        {
            INFO("n = " << n);
            CHECK(dirty(lookup<float(const float*, int)>(*jit, "copied"), in.data(), n) == doubled(in, 0, n));
            CHECK(dirty(lookup<float(int)>(*jit, "filled"), n) == 1.5f * static_cast<float>(n));
        }
        float triangle = 0.0f;
        for (int j = 0; j < 16; ++j)
            triangle += doubled(in, 0, j);
        CHECK(dirty(lookup<float(const float*)>(*jit, "triangle"), in.data()) == triangle);

        std::array<float, 192> blocked{};
        dirty(lookup<void(const float*, float*, int, int)>(*jit, "blocks"), input.data(), blocked.data(), 48, 4);
        for (std::size_t i = 0; i < blocked.size(); ++i)
            CHECK(blocked[i] == input[i] * 2.0f + 1.0f);
    }
}

TEST_CASE("elements nothing wrote since their declaration still read 0", "[codegen][zeroinit]")
{
    const std::array<float, 64> in = ramp();
    const std::array<float, 64> negatives = rampWithNegatives();
    for (const llvm::OptimizationLevel level : kLevels)
    {
        INFO(levelName(level));
        auto jit = compileAt(kUncovered, level);
        using Sum = float(const float*, int);
        for (const int n : {0, 1, 2, 6, 12, 40})
        {
            INFO("n = " << n);
            CHECK(dirty(lookup<Sum>(*jit, "shorter"), in.data(), n) == doubled(in, 0, n - 1));
            CHECK(dirty(lookup<Sum>(*jit, "accumulated"), in.data(), n) == doubled(in, 0, n) / 2.0f);
            float positive = 0.0f;
            for (int i = 0; i < n; ++i)
                positive += negatives[static_cast<std::size_t>(i)] > 0 ? negatives[static_cast<std::size_t>(i)] : 0;
            CHECK(dirty(lookup<Sum>(*jit, "positive"), negatives.data(), n) == positive);
            CHECK(dirty(lookup<Sum>(*jit, "untilNegative"), negatives.data(), n)
                  == doubled(negatives, 0, std::min(n, 5)));
            CHECK(dirty(lookup<Sum>(*jit, "evens"), in.data(), n) == doubled(in, 0, n / 2));
            CHECK(dirty(lookup<Sum>(*jit, "fromOne"), in.data(), n) == doubled(in, 1, n));
            CHECK(dirty(lookup<Sum>(*jit, "pastEnd"), in.data(), n) == 0.0f);
            CHECK(dirty(lookup<Sum>(*jit, "onePast"), in.data(), n) == doubled(in, 0, n));
            CHECK(dirty(lookup<Sum>(*jit, "grown"), in.data(), n) == doubled(in, 0, n));
            using Bounded = float(const float*, int, int);
            CHECK(dirty(lookup<Bounded>(*jit, "twoBounds"), in.data(), n, n + 3) == doubled(in, 0, n));
            CHECK(dirty(lookup<Sum>(*jit, "shifted"), in.data(), n) == doubled(in, 0, n - 2));
            CHECK(dirty(lookup<Sum>(*jit, "first"), in.data(), n) == (n > 0 ? 3.0f * in[0] : 0.0f));
            CHECK(dirty(lookup<Bounded>(*jit, "firstIf"), in.data(), n, 1) == doubled(in, 0, n > 0 ? 1 : 0));
            float growing = 0.0f;
            for (int r = 0; r < 4; ++r)
                growing += doubled(in, 0, std::min(r, n));
            CHECK(dirty(lookup<Sum>(*jit, "growing"), in.data(), n) == growing);
            CHECK(dirty(lookup<Sum>(*jit, "alternate"), in.data(), n) == 3.0f * doubled(in, 0, n));   // round 0 reads 0
            const float dc = dirty(lookup<Sum>(*jit, "padded"), in.data(), n);   // the FFT reads all 64 elements
            CHECK(std::abs(dc - doubled(in, 0, n)) < 1.0e-3f);
        }
        // a[2] and a[3] read 0.
        CHECK(dirty(lookup<float(const float*)>(*jit, "outside"), in.data()) == doubled(in, 0, 2));
    }
}

TEST_CASE("ArrayTest's zeroed arrays read 0 on a dirty stack", "[codegen][zeroinit][array]")
{
    for (const llvm::OptimizationLevel level : kLevels)
    {
        INFO(levelName(level));
        auto jit = compileAt("float ramp(int n)\n"
                             "{\n"
                             "    float a[8];\n"
                             "    for (int i = 0; i < n; ++i)\n"
                             "        a[i] = i;\n"
                             "    float s = 0;\n"
                             "    for (int i = 0; i < 8; ++i)\n"
                             "        s += a[i];\n"
                             "    return s;\n"
                             "}\n"
                             "float listed()\n"
                             "{\n"
                             "    float c[4] = {1, 2};\n"
                             "    return c[0] + 10 * c[1] + 100 * c[2] + 1000 * c[3];\n"
                             "}\n"
                             "float vectors()\n"
                             "{\n"
                             "    float4 v[2];\n"
                             "    v[1].y = 3;\n"
                             "    v[0] = (float4)(1);\n"
                             "    return v[0].x + v[1].y + v[1].x;\n"
                             "}\n"
                             "float fresh()\n"
                             "{\n"
                             "    float s = 0;\n"
                             "    for (int r = 0; r < 3; ++r)\n"
                             "    {\n"
                             "        float a[2];\n"
                             "        s += a[0];\n"
                             "        a[0] = 5;\n"
                             "    }\n"
                             "    return s;\n"
                             "}\n",
                             level);
        for (const int n : {0, 5, 8})
        {
            CHECK(dirty(lookup<float(int)>(*jit, "ramp"), n) == static_cast<float>(n * (n - 1) / 2));
        }
        CHECK(dirty(lookup<float()>(*jit, "listed")) == 21.0f);
        CHECK(dirty(lookup<float()>(*jit, "vectors")) == 4.0f);
        CHECK(dirty(lookup<float()>(*jit, "fresh")) == 0.0f);
    }
}

TEST_CASE("the tests' dirty stack is where a JIT'd function's locals live", "[codegen][zeroinit]")
{
    // A local nothing zeroes, in IR written by hand (FMinus zeroes every one): if it read 0 here, the tests above
    // could not tell a missing memset from a present one. Under ThreadSanitizer (the tsan preset) the frames between
    // dirtyStack's and the JIT'd function's are its runtime's to lay out, and the top of the local is written by them:
    // the harness's premise does not hold there, so this check is skipped (the tests above still pass, less sharply).
#if defined(__has_feature)
#if __has_feature(thread_sanitizer)
    SKIP("ThreadSanitizer's runtime writes the stack between the dirtying and the JIT'd function's frame");
#endif
#endif
    auto context = std::make_unique<llvm::LLVMContext>();
    llvm::SMDiagnostic diagnostic;
    std::unique_ptr<llvm::Module> module =
        llvm::parseAssemblyString("define float @peek(i64 %i) {\n"
                                  "  %a = alloca [256 x float], align 4\n"
                                  "  %p = getelementptr inbounds [256 x float], ptr %a, i64 0, i64 %i\n"
                                  "  %v = load volatile float, ptr %p, align 4\n"
                                  "  ret float %v\n"
                                  "}\n",
                                  diagnostic, *context);
    REQUIRE(module);
    auto jit = llvm::cantFail(fminus::jit::Jit::create(llvm::OptimizationLevel::O2));
    llvm::cantFail(jit->add(std::move(module), std::move(context)));
    auto* peek = lookup<float(std::int64_t)>(*jit, "peek");
    for (std::int64_t i = 0; i < 256; ++i)
    {
        INFO("element " << i);
        CHECK(dirty(peek, i) == 1.0e6f);
    }
}

TEST_CASE("the pass keeps a memset whose local escapes, or that a load reaches first", "[codegen][zeroinit]")
{
    // The pass alone, on IR written by hand, after the analyses it needs: shapes FMinus's pipeline rarely leaves.
    const auto run = [](const char* ir) {
        llvm::LLVMContext context;
        llvm::SMDiagnostic diagnostic;
        std::unique_ptr<llvm::Module> module = llvm::parseAssemblyString(ir, diagnostic, context);
        REQUIRE(module);
        llvm::LoopAnalysisManager loops;
        llvm::FunctionAnalysisManager functions;
        llvm::CGSCCAnalysisManager cgsccs;
        llvm::ModuleAnalysisManager modules;
        llvm::PassBuilder passes;
        passes.registerModuleAnalyses(modules);
        passes.registerCGSCCAnalyses(cgsccs);
        passes.registerFunctionAnalyses(functions);
        passes.registerLoopAnalyses(loops);
        passes.crossRegisterProxies(loops, functions, cgsccs, modules);
        llvm::FunctionPassManager pipeline;
        pipeline.addPass(fminus::opt::ZeroInitElisionPass());
        int kept = 0;
        for (llvm::Function& function : *module)
        {
            if (function.isDeclaration())
                continue;
            pipeline.run(function, functions);
            for (const llvm::BasicBlock& block : function)
            {
                for (const llvm::Instruction& instruction : block)
                    kept += llvm::isa<llvm::MemSetInst>(instruction) ? 1 : 0;
            }
        }
        return kept;
    };

    const auto with = [](std::string text, std::string_view from, std::string_view to) {
        const std::size_t at = text.find(from);
        REQUIRE(at != std::string::npos);
        return text.replace(at, from.size(), to);
    };

    // A rotated store loop under its guard, whose skipping edge joins the loop's exit (LLVM's shape before jump
    // threading), and a read loop under the same guard: the memset goes.
    constexpr const char* kGuarded = R"(
define float @f(ptr %in, i32 %n) {
entry:
  %t = alloca [64 x float], align 4
  call void @llvm.memset.p0.i64(ptr align 4 %t, i8 0, i64 256, i1 false)
  %enter = icmp sgt i32 %n, 0
  br i1 %enter, label %write.ph, label %written
write.ph:
  %count = zext nneg i32 %n to i64
  br label %write
write:
  %i = phi i64 [ 0, %write.ph ], [ %i.next, %write ]
  %from = getelementptr inbounds float, ptr %in, i64 %i
  %x = load float, ptr %from, align 4
  %to = getelementptr inbounds float, ptr %t, i64 %i
  store float %x, ptr %to, align 4
  %i.next = add nuw nsw i64 %i, 1
  %done = icmp eq i64 %i.next, %count
  br i1 %done, label %write.exit, label %write
write.exit:
  br label %written
written:
  %again = icmp sgt i32 %n, 0
  br i1 %again, label %read.ph, label %end
read.ph:
  %count2 = zext nneg i32 %n to i64
  br label %read
read:
  %j = phi i64 [ 0, %read.ph ], [ %j.next, %read ]
  %s = phi float [ 0.0, %read.ph ], [ %s.next, %read ]
  %at = getelementptr inbounds float, ptr %t, i64 %j
  %y = load float, ptr %at, align 4
  %s.next = fadd float %s, %y
  %j.next = add nuw nsw i64 %j, 1
  %done2 = icmp eq i64 %j.next, %count2
  br i1 %done2, label %end, label %read
end:
  %r = phi float [ 0.0, %written ], [ %s.next, %read ]
  ret float %r
}
declare void @llvm.memset.p0.i64(ptr, i8, i64, i1)
)";
    CHECK(run(kGuarded) == 0);

    // The same, but the read loop runs whatever n is: with n <= 0 it reads what the memset wrote.
    CHECK(run(with(with(kGuarded, "%again = icmp sgt i32 %n, 0", "%again = icmp sgt i32 %n, -5"), "zext nneg i32 %n",
                   "zext i32 %n")
                  .c_str())
          == 1);

    // The local's address goes to a call, which may read every element.
    CHECK(run((with(kGuarded, "  %enter", "  call void @use(ptr %t)\n  %enter") + "declare void @use(ptr)\n").c_str())
          == 1);

    // A load between the memset and the loop reads an element the loop has not written yet.
    CHECK(run(with(kGuarded, "  %enter", "  %peek = load float, ptr %t, align 4\n  %enter").c_str()) == 1);

    // The reads go one element further than the writes: the last reads what the memset wrote.
    CHECK(run(with(kGuarded, "%count2 = zext nneg i32 %n to i64",
                   "%more = add nsw i32 %n, 1\n  %count2 = zext nneg i32 %more to i64")
                  .c_str())
          == 1);
}
