// tests/unit/AttributeTest.cpp: what FMinus tells LLVM that LLVM could not find out. The runtime's functions are
// declared with what they do (ir/Calls.cpp, describeRuntime), and loops whose condition is not a constant expression
// must make progress (ir/Statements.cpp, C11 6.8.5p6). The IR is checked in tests/lit/fminus/runtime-attributes.fm and
// progress.fm; these run the programs in the JIT, at -O2, and check that what must not change has not: an FFT still
// writes its output and reads its input, a plan's null still says its size is refused, the heap's blocks still read
// 0, and loops still run as written. A prototype of libm's function says, on Darwin, that it writes no memory
// (ir/Functions.cpp, describeLibm; the IR in libm-prototypes.fm): these check its declarations for each target, and
// that the calls LLVM then moves compute what libm does.
#include "Compile.h"

#include <catch2/catch_test_macros.hpp>

#include <llvm/IR/Function.h>
#include <llvm/Support/ModRef.h>

#include <array>
#include <cmath>
#include <string>

using namespace fminus::test;

namespace
{
    // A module that reads its fields across transforms: `gain`, which no transform can change, and `spectrum`, which
    // one writes. It lives on FMinus's heap, so the host needs no copy of its layout.
    constexpr const char* kAnalyser = "module Analyser\n"
                                      "{\n"
                                      "    void* plan;\n"
                                      "    float gain;\n"
                                      "    float2 spectrum[17];\n"
                                      "\n"
                                      "    float level(const float* in)\n"
                                      "    {\n"
                                      "        float2 bins[17];\n"
                                      "        const float before = gain;\n"
                                      "        fft_forward(plan, in, bins);\n"
                                      "        return before + gain + bins[1].x;\n"
                                      "    }\n"
                                      "\n"
                                      "    float rewrite(const float* in)\n"
                                      "    {\n"
                                      "        const float before = spectrum[1].x;\n"
                                      "        fft_forward(plan, in, spectrum);\n"
                                      "        return before + spectrum[1].x;\n"
                                      "    }\n"
                                      "};\n"
                                      "\n"
                                      "Analyser* analyser_create(float gain)\n"
                                      "{\n"
                                      "    Analyser* a = calloc(1, sizeof(Analyser));\n"
                                      "    a->plan = fft_create(32);\n"
                                      "    a->gain = gain;\n"
                                      "    a->spectrum[1] = (float2)(100, 0);\n"
                                      "    return a;\n"
                                      "}\n"
                                      "\n"
                                      "float level(Analyser* a, const float* in) { return a->level(in); }\n"
                                      "float rewrite(Analyser* a, const float* in) { return a->rewrite(in); }\n"
                                      "\n"
                                      "void analyser_destroy(Analyser* a)\n"
                                      "{\n"
                                      "    fft_destroy(a->plan);\n"
                                      "    free(a);\n"
                                      "}\n";

    // A loop that calls libm's asinf and powf of its invariants.
    constexpr const char* kTone =
        "float asinf(float x);\n"
        "float powf(float x, float y);\n"
        "\n"
        "void tone(float* restrict out, const float* restrict in, int n, float cutoff, float level)\n"
        "{\n"
        "    for (int i = 0; i < n; ++i)\n"
        "        out[i] = in[i] * asinf(3.14159265f * cutoff / 48000) * powf(10, level / 20);\n"
        "}\n";

    // And another of libm's, one whose type is not libm's, one libm does not have, and a definition of a name of
    // libm's (lowered, not run: nothing defines mystery).
    const std::string kPrototypes = std::string(kTone)
                                    + "float remquof(float x, float y, int* quotient);\n"
                                      "float atanf(int x);\n"
                                      "float mystery(float x);\n"
                                      "float coshf(float x) { return x; }\n"
                                      "float others(float x, int* q) { return remquof(x, 2, q) + atanf(1) + mystery(x); }\n";

    // The program lowered for `triple`, unoptimised.
    std::unique_ptr<llvm::Module> lowered(std::string_view source, const std::string& triple,
                                          llvm::LLVMContext& context)
    {
        fminus::Diagnostics diagnostics("test.fm");
        const auto program = fminus::frontend::parse(source, diagnostics);
        fminus::ir::Options options;
        options.targetTriple = triple;
        std::unique_ptr<llvm::Module> module =
            program ? fminus::ir::lower(*program, context, diagnostics, options) : nullptr;
        INFO(diagnostics.format());
        REQUIRE(module);
        return module;
    }

    // 32 samples of an impulse: every bin of its spectrum is 1.
    std::array<float, 32> impulse()
    {
        std::array<float, 32> x{};
        x[0] = 1.0f;
        return x;
    }
} // namespace

TEST_CASE("an FFT changes nothing of a module's but its output, and still writes that", "[codegen][attributes]")
{
    std::string errors;
    auto jit = compile(kAnalyser, &errors);
    INFO(errors);
    REQUIRE(jit);
    void* analyser = lookup<void*(float)>(*jit, "analyser_create")(0.5f);
    REQUIRE(analyser != nullptr);
    const std::array<float, 32> x = impulse();
    // gain, read once across the transform (OPT in runtime-attributes.fm), is still the gain: 0.5 + 0.5 + bin 1.
    CHECK(lookup<float(void*, const float*)>(*jit, "level")(analyser, x.data()) == 2.0f);
    // The module's own spectrum is the transform's output: read again after it, it is the new bin, 1, not 100.
    CHECK(lookup<float(void*, const float*)>(*jit, "rewrite")(analyser, x.data()) == 101.0f);
    lookup<void(void*)>(*jit, "analyser_destroy")(analyser);
}

TEST_CASE("an FFT reads the input stored just before it", "[codegen][attributes]")
{
    std::string errors;
    auto jit = compile("float impulse_bin(void* plan, int k)\n"
                       "{\n"
                       "    float x[32];\n"
                       "    float2 bins[17];\n"
                       "    x[0] = 1;\n"   // a store the transform must see
                       "    fft_forward(plan, x, bins);\n"
                       "    return bins[k].x;\n"
                       "}\n"
                       "float round_trip(void* plan)\n"
                       "{\n"
                       "    float x[32];\n"
                       "    float2 bins[17];\n"
                       "    float y[32];\n"
                       "    x[3] = 2;\n"
                       "    fft_forward(plan, x, bins);\n"
                       "    fft_inverse(plan, bins, y);\n"   // the bins the forward transform wrote
                       "    return y[3];\n"
                       "}\n"
                       "void* plan(int n) { return fft_create(n); }\n"
                       "void release(void* p) { fft_destroy(p); }\n",
                       &errors);
    INFO(errors);
    REQUIRE(jit);
    void* p = lookup<void*(int)>(*jit, "plan")(32);
    REQUIRE(p != nullptr);
    auto bin = lookup<float(void*, int)>(*jit, "impulse_bin");
    CHECK(bin(p, 0) == 1.0f);
    CHECK(bin(p, 5) == 1.0f);
    CHECK(lookup<float(void*)>(*jit, "round_trip")(p) == 64.0f);   // n times 2: neither direction scales
    lookup<void(void*)>(*jit, "release")(p);
}

TEST_CASE("fft_create is not an allocation LLVM may remove: a null plan still says the size is refused",
          "[codegen][attributes]")
{
    std::string errors;
    auto jit = compile("bool takes(int n)\n"
                       "{\n"
                       "    void* plan = fft_create(n);\n"
                       "    void* none;\n"
                       "    const bool made = plan != none;\n"
                       "    fft_destroy(plan);\n"
                       "    return made;\n"
                       "}\n"
                       "bool takes_seven() { return takes(7); }\n"   // a constant size: LLVM sees it all
                       "bool takes_64() { return takes(64); }\n"
                       "bool takes_complex(int n)\n"
                       "{\n"
                       "    void* plan = fft_create_complex(n);\n"
                       "    void* none;\n"
                       "    const bool made = plan != none;\n"
                       "    fft_destroy(plan);\n"
                       "    return made;\n"
                       "}\n",
                       &errors);
    INFO(errors);
    REQUIRE(jit);
    CHECK_FALSE(lookup<bool()>(*jit, "takes_seven")());
    CHECK(lookup<bool()>(*jit, "takes_64")());
    auto takes = lookup<bool(int)>(*jit, "takes");
    CHECK_FALSE(takes(100));
    CHECK(takes(96));
    auto takesComplex = lookup<bool(int)>(*jit, "takes_complex");
    CHECK_FALSE(takesComplex(40));
    CHECK(takesComplex(48));
}

TEST_CASE("the heap's blocks read 0 until written, kept or removed", "[codegen][attributes]")
{
    std::string errors;
    auto jit = compile("float fresh(int n)\n"   // LLVM removes the block, and knows what it holds
                       "{\n"
                       "    float* p = malloc(n * sizeof(float));\n"
                       "    p[0] = 2;\n"
                       "    const float sum = p[0] + p[1] + p[n - 1];\n"
                       "    free(p);\n"
                       "    return sum;\n"
                       "}\n"
                       "int* kept(int n)\n"   // the host gets this block: it is made, and zeroed
                       "{\n"
                       "    int* p = calloc(n, sizeof(int));\n"
                       "    p[1] = p[0] + p[2] + 3;\n"
                       "    return p;\n"
                       "}\n"
                       "void release(void* p) { free(p); }\n",
                       &errors);
    INFO(errors);
    REQUIRE(jit);
    CHECK(lookup<float(int)>(*jit, "fresh")(4) == 2.0f);
    int* p = lookup<int*(int)>(*jit, "kept")(4);
    REQUIRE(p != nullptr);
    CHECK((p[0] == 0 && p[1] == 3 && p[2] == 0 && p[3] == 0));
    lookup<void(void*)>(*jit, "release")(p);
}

TEST_CASE("loops run as written, whether they must make progress or may run until a break", "[codegen][progress]")
{
    std::string errors;
    auto jit = compile("int steps(int n)\n"   // must make progress: the condition is not constant
                       "{\n"
                       "    int k = 0;\n"
                       "    while (n != 1)\n"
                       "    {\n"
                       "        ++k;\n"
                       "        if (n % 2 == 0)\n"
                       "        {\n"
                       "            n /= 2;\n"
                       "            continue;\n"
                       "        }\n"
                       "        n = 3 * n + 1;\n"
                       "    }\n"
                       "    return k;\n"
                       "}\n"
                       "int steps_for(int n)\n"
                       "{\n"
                       "    int k = 0;\n"
                       "    for (; n != 1; ++k)\n"
                       "        n = n % 2 == 0 ? n / 2 : 3 * n + 1;\n"
                       "    return k;\n"
                       "}\n"
                       "int until(int n)\n"   // constant conditions: they end at a break or a return
                       "{\n"
                       "    int k = 0;\n"
                       "    while (1)\n"
                       "    {\n"
                       "        if (n == 1)\n"
                       "            break;\n"
                       "        n = n % 2 == 0 ? n / 2 : 3 * n + 1;\n"
                       "        ++k;\n"
                       "    }\n"
                       "    for (;;)\n"
                       "    {\n"
                       "        if (k > 1000)\n"
                       "            return -1;\n"
                       "        return k;\n"
                       "    }\n"
                       "}\n",
                       &errors);
    INFO(errors);
    REQUIRE(jit);
    CHECK(lookup<int(int)>(*jit, "steps")(27) == 111);   // 27 takes 111 steps to reach 1
    CHECK(lookup<int(int)>(*jit, "steps_for")(27) == 111);
    CHECK(lookup<int(int)>(*jit, "until")(27) == 111);
    CHECK(lookup<int(int)>(*jit, "steps")(1) == 0);
}

TEST_CASE("a program's own declaration of a runtime function is left as it declares it", "[codegen][attributes]")
{
    // A prototype with the runtime's name and other types: the builtins' calls still go to it, as before, and it takes
    // none of the runtime's attributes (free's allocptr on a float would not verify).
    std::string errors;
    auto jit = compile("float fminus_rt_free(float x);\n"
                       "bool release(int n)\n"
                       "{\n"
                       "    float* p = calloc(n, sizeof(float));\n"
                       "    float* none;\n"
                       "    const bool made = p != none;\n"
                       "    free(p);\n"
                       "    return made;\n"
                       "}\n",
                       &errors);
    INFO(errors);
    REQUIRE(jit);
    CHECK(lookup<bool(int)>(*jit, "release")(4));
}

TEST_CASE("a prototype of libm's writes no memory where libm sets no errno, and computes what libm does",
          "[codegen][attributes]")
{
    for (const std::string triple : {"arm64-apple-macosx14.0.0", "x86_64-apple-macosx14.0.0"})
    {
        INFO(triple);
        llvm::LLVMContext context;
        const auto module = lowered(kPrototypes, triple, context);
        CHECK(module->getFunction("asinf")->doesNotAccessMemory());
        CHECK(module->getFunction("powf")->doesNotAccessMemory());
        CHECK(module->getFunction("asinf")->doesNotThrow());
        CHECK(module->getFunction("asinf")->willReturn());
        // remquof writes its quotient, and nothing else.
        CHECK(module->getFunction("remquof")->getMemoryEffects()
              == llvm::MemoryEffects::argMemOnly(llvm::ModRefInfo::Mod));
        // Not libm's type, not libm's name, the program's own definition: as the program declared them.
        CHECK(module->getFunction("atanf")->getMemoryEffects() == llvm::MemoryEffects::unknown());
        CHECK(module->getFunction("mystery")->getMemoryEffects() == llvm::MemoryEffects::unknown());
        CHECK(module->getFunction("coshf")->getMemoryEffects() == llvm::MemoryEffects::unknown());
    }
    // glibc and the Windows CRT set errno: the declarations stay bare, and LLVM's own inference says they write it.
    for (const std::string triple : {"aarch64-unknown-linux-gnu", "x86_64-unknown-linux-gnu", "x86_64-pc-windows-msvc"})
    {
        INFO(triple);
        llvm::LLVMContext context;
        const auto module = lowered(kPrototypes, triple, context);
        CHECK(module->getFunction("asinf")->getMemoryEffects() == llvm::MemoryEffects::unknown());
        CHECK(module->getFunction("powf")->getMemoryEffects() == llvm::MemoryEffects::unknown());
        CHECK(module->getFunction("remquof")->getMemoryEffects() == llvm::MemoryEffects::unknown());
    }

    // On this machine, at -O2: hoisted or not, every sample is libm's asinf and powf of the invariants, bit for bit.
    std::string errors;
    auto jit = compile(kTone, &errors);
    INFO(errors);
    REQUIRE(jit);
    std::array<float, 67> in{};
    for (std::size_t i = 0; i < in.size(); ++i)
        in[i] = static_cast<float>(i) * 0.37f - 11.0f;
    std::array<float, 67> out{};
    const float cutoff = 1234.5f, level = -7.25f;
    lookup<void(float*, const float*, int, float, float)>(*jit, "tone")(out.data(), in.data(),
                                                                        static_cast<int>(in.size()), cutoff, level);
    volatile float opaqueCutoff = cutoff, opaqueLevel = level;   // so that libm computes them at run time, as there
    const float g = std::asin(3.14159265f * opaqueCutoff / 48000);
    const float a = std::pow(10.0f, opaqueLevel / 20);
    for (std::size_t i = 0; i < in.size(); ++i)
    {
        INFO(i);
        CHECK(out[i] == in[i] * g * a);
    }
}
