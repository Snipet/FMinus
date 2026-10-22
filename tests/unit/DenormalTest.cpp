// tests/unit/DenormalTest.cpp: subnormal floats, preserved (IEEE's, C's, the default) or flushed to zero
// (ir::Denormals::Flush, src/ir/Denormals.cpp). Under Flush every definition is marked denormal_fpenv(preservesign),
// the code generator folds constants as flushing hardware computes them, and each entry point, a nonblocking function
// nothing in the program calls, sets the floating-point mode and restores the caller's. Run through the JIT, against
// C++ computing in the same mode, and checked in the IR, x86-64's included.
#include "Compile.h"

#include "ir/Denormals.h"
#include "support/HostTarget.h"

#include <catch2/catch_test_macros.hpp>

#include <llvm/ADT/FloatingPointMode.h>
#include <llvm/IR/Constants.h>
#include <llvm/IR/Function.h>
#include <llvm/IR/InlineAsm.h>
#include <llvm/IR/Instructions.h>
#include <llvm/IR/IntrinsicInst.h>
#include <llvm/IR/IntrinsicsAArch64.h>
#include <llvm/IR/IntrinsicsX86.h>
#include <llvm/IR/Verifier.h>

#include <llvm/Support/raw_ostream.h>
#include <llvm/Target/TargetMachine.h>

#include <array>
#include <bit>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#if defined(__x86_64__)
#include <immintrin.h>
#endif

using namespace fminus::test;
using fminus::ir::Denormals;

namespace
{
    // The floating-point control register, as a host reads and writes it, and its flush-to-zero bits: AArch64's
    // FPCR.FZ, x86's MXCSR.FTZ and MXCSR.DAZ.
#if defined(__aarch64__)
    constexpr std::uint64_t kFlushBits = std::uint64_t{1} << 24;

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
#elif defined(__x86_64__)
    constexpr std::uint64_t kFlushBits = 0x8040;

    std::uint64_t controlRegister()
    {
        return _mm_getcsr();
    }

    void setControlRegister(std::uint64_t value)
    {
        _mm_setcsr(static_cast<unsigned>(value));
    }
#else
#error "the denormal tests read the floating-point control register of arm64 or x86-64"
#endif

    // The host's mode around a scope, as a plugin host's ScopedFtz sets it: flushing, or IEEE's.
    class ScopedMode
    {
    public:
        explicit ScopedMode(bool flush) : saved_(controlRegister())
        {
            setControlRegister(flush ? saved_ | kFlushBits : saved_ & ~kFlushBits);
        }
        ~ScopedMode()
        {
            setControlRegister(saved_);
        }
        ScopedMode(const ScopedMode&) = delete;
        ScopedMode& operator=(const ScopedMode&) = delete;

    private:
        std::uint64_t saved_;
    };

    fminus::ir::Options with(Denormals denormals)
    {
        fminus::ir::Options options;
        options.denormals = denormals;
        return options;
    }

    // The program JIT-compiled at `level`, as Compile.h's compile() does at -O2.
    std::unique_ptr<fminus::jit::Jit> compileAt(std::string_view source, llvm::OptimizationLevel level,
                                                const fminus::ir::Options& options, std::string& errors)
    {
        fminus::Diagnostics diagnostics("test.fm");
        const auto program = fminus::frontend::parse(source, diagnostics);
        auto context = std::make_unique<llvm::LLVMContext>();
        std::unique_ptr<llvm::Module> module =
            program ? fminus::ir::lower(*program, *context, diagnostics, options) : nullptr;
        errors = diagnostics.format();
        if (!module)
            return nullptr;
        auto jit = llvm::cantFail(fminus::jit::Jit::create(level));
        llvm::cantFail(jit->add(std::move(module), std::move(context)));
        return jit;
    }

    // The program lowered, unoptimised, or nothing with its errors in `errors`.
    std::unique_ptr<llvm::Module> lowered(std::string_view source, const fminus::ir::Options& options,
                                          llvm::LLVMContext& context, std::string& errors)
    {
        fminus::Diagnostics diagnostics("test.fm");
        const auto program = fminus::frontend::parse(source, diagnostics);
        std::unique_ptr<llvm::Module> module =
            program ? fminus::ir::lower(*program, context, diagnostics, options) : nullptr;
        errors = diagnostics.format();
        return module;
    }

    // The program lowered and optimised at `level` for this machine, as the JIT does it: to read what the pipeline
    // made of it.
    std::unique_ptr<llvm::Module> optimised(std::string_view source, const fminus::ir::Options& options,
                                            llvm::LLVMContext& context, llvm::OptimizationLevel level)
    {
        fminus::Diagnostics diagnostics("test.fm");
        const auto program = fminus::frontend::parse(source, diagnostics);
        auto machine = llvm::cantFail(fminus::hostTargetMachine(fminus::codeGenLevel(level)));
        fminus::ir::Options lowering = options;
        lowering.dataLayout = machine->createDataLayout().getStringRepresentation();
        std::unique_ptr<llvm::Module> module =
            program ? fminus::ir::lower(*program, context, diagnostics, lowering) : nullptr;
        INFO(diagnostics.format());
        REQUIRE(module);
        fminus::prepareModule(*module, *machine);
        fminus::optimiseModule(*module, *machine, level);
        return module;
    }

    // The values a test passes through memory the compiler cannot see into, so C++ computes them at run time, in the
    // mode the test set, as the JIT'd code does.
    float opaque(float value)
    {
        volatile float slot = value;
        return slot;
    }

    bool subnormal(float value)
    {
        return std::fpclassify(value) == FP_SUBNORMAL;
    }

    // The research's one-pole gone silent: y += k * (0 - y), its state decaying into the subnormals. In C++, a
    // statement a rounding, as FMinus computes it.
    [[gnu::noinline]] float onePole(float y, float k, int n)
    {
        for (int i = 0; i < n; ++i)
        {
            const float step = k * (0.0f - y);
            y = y + step;
        }
        return y;
    }

    constexpr const char* kProduct = "float product(float a, float b) [[clang::nonblocking]] { return a * b; }\n"
                                     "\n"
                                     "float onepole(float y, float k, int n) [[clang::nonblocking]]\n"
                                     "{\n"
                                     "    for (int i = 0; i < n; ++i)\n"
                                     "        y += k * (0 - y);\n"
                                     "    return y;\n"
                                     "}\n";

    // A parameter's slot, as a C++ host declares it (ParamTest.cpp).
    struct Slot
    {
        const float* stream = nullptr;
        float value = 0.0f;
    };

    struct Tiny
    {
        Slot k;
    };

    // Expressions on a parameter that produce or read a subnormal: frozen, they fold; as a knob, they run.
    constexpr const char* kTiny = "module Tiny\n"
                                  "{\n"
                                  "    param float k;\n"
                                  "\n"
                                  "    float built() [[clang::nonblocking]] { return k[0] * 1e-10f; }\n"
                                  "    float later() [[clang::nonblocking]]\n"
                                  "    {\n"
                                  "        const float a = k[0];\n"
                                  "        return a * 1e-10f;\n"
                                  "    }\n"
                                  "    bool above() [[clang::nonblocking]] { return k[0] > 0; }\n"
                                  "    float4 lanes() [[clang::nonblocking]]\n"
                                  "    {\n"
                                  "        return (float4)(k[0], 1, -k[0], 2) * 1e-10f;\n"
                                  "    }\n"
                                  "};\n";

    struct TinyResults
    {
        float built;
        float later;
        bool above;
        std::array<float, 4> lanes;
    };

    TinyResults run(fminus::jit::Jit& jit, float k)
    {
        Tiny tiny;
        tiny.k.value = k;
        return {lookup<float(Tiny*)>(jit, "Tiny::built")(&tiny), lookup<float(Tiny*)>(jit, "Tiny::later")(&tiny),
                lookup<bool(Tiny*)>(jit, "Tiny::above")(&tiny),
                lanesOf(lookup<float4(Tiny*)>(jit, "Tiny::lanes")(&tiny))};
    }

    bool sameBits(float a, float b)
    {
        return std::bit_cast<std::uint32_t>(a) == std::bit_cast<std::uint32_t>(b);
    }

    bool sameBits(const TinyResults& a, const TinyResults& b)
    {
        bool same = sameBits(a.built, b.built) && sameBits(a.later, b.later) && a.above == b.above;
        for (std::size_t i = 0; i < 4; ++i)
            same = same && sameBits(a.lanes[i], b.lanes[i]);
        return same;
    }

    llvm::DenormalFPEnv modeOf(const llvm::Function& function)
    {
        return function.getDenormalFPEnv();
    }

    constexpr llvm::DenormalFPEnv kPreserveSign{llvm::DenormalMode::getPreserveSign()};
    constexpr llvm::DenormalFPEnv kDynamic{llvm::DenormalMode::getDynamic()};

    // How many writes of the control register a wrapper makes before it has looked at the caller's mode.
    int writesInEntryBlock(const llvm::Function& function)
    {
        int writes = 0;
        for (const llvm::Instruction& instruction : function.getEntryBlock())
        {
            if (const auto* call = llvm::dyn_cast<llvm::IntrinsicInst>(&instruction))
            {
                const llvm::Intrinsic::ID id = call->getIntrinsicID();
                writes += id == llvm::Intrinsic::aarch64_set_fpcr || id == llvm::Intrinsic::x86_sse_ldmxcsr ? 1 : 0;
            }
        }
        return writes;
    }

    // How a wrapper calls its body: through the body's address passed through an empty inline asm (the first count),
    // and directly (the second), which a call that LLVM keeps between the writes of the register must not be.
    std::pair<int, int> callsOfBody(const llvm::Function& entry, const llvm::Function& body)
    {
        int laundered = 0;
        int direct = 0;
        for (const llvm::BasicBlock& block : entry)
        {
            for (const llvm::Instruction& instruction : block)
            {
                const auto* call = llvm::dyn_cast<llvm::CallInst>(&instruction);
                if (call == nullptr)
                    continue;
                if (call->getCalledOperand() == &body)
                    ++direct;
                const auto* address = llvm::dyn_cast<llvm::CallInst>(call->getCalledOperand());
                if (address != nullptr && address->isInlineAsm() && address->getArgOperand(0) == &body
                    && llvm::cast<llvm::InlineAsm>(address->getCalledOperand())->getAsmString().empty())
                    ++laundered;
            }
        }
        return {laundered, direct};
    }

    // The intrinsics a function calls, by ID, in order.
    std::vector<llvm::Intrinsic::ID> intrinsicsOf(const llvm::Function& function)
    {
        std::vector<llvm::Intrinsic::ID> ids;
        for (const llvm::BasicBlock& block : function)
        {
            for (const llvm::Instruction& instruction : block)
            {
                if (const auto* call = llvm::dyn_cast<llvm::IntrinsicInst>(&instruction))
                    ids.push_back(call->getIntrinsicID());
            }
        }
        return ids;
    }
} // namespace

TEST_CASE("under Flush an entry point runs flushing: a subnormal result is 0, with its sign, and so is an operand",
          "[codegen][denormals]")
{
    std::string errors;
    auto jit = compile(kProduct, &errors, with(Denormals::Flush));
    INFO(errors);
    REQUIRE(jit);
    auto* product = lookup<float(float, float)>(*jit, "product");
    const ScopedMode host(false);   // the host computes as IEEE says
    REQUIRE(subnormal(opaque(1e-30f) * opaque(1e-10f)));

    const float positive = product(1e-30f, 1e-10f);
    CHECK(positive == 0.0f);
    CHECK_FALSE(std::signbit(positive));
    const float negative = product(-1e-30f, 1e-10f);
    CHECK(negative == 0.0f);
    CHECK(std::signbit(negative));
    CHECK(product(1e-40f, 1e30f) == 0.0f);   // a subnormal operand reads as 0: IEEE gives 1e-10
    CHECK(product(1.5f, 4.0f) == 6.0f);      // normal numbers are as they were
    CHECK(product(1e-20f, 1e-17f) == opaque(1e-20f) * opaque(1e-17f));   // 1e-37, above the least normal
}

TEST_CASE("an entry point restores the caller's mode on return, from every return, and flushing stays flushing",
          "[codegen][denormals]")
{
    std::string errors;
    auto jit = compile("float pick(float a, float b, int n) [[clang::nonblocking]]\n"
                       "{\n"
                       "    if (n == 0)\n"
                       "        return a * b;\n"
                       "    for (int i = 0; i < n; ++i)\n"
                       "    {\n"
                       "        if (i == 2)\n"
                       "            return a * b * 2;\n"
                       "        a *= 0.5f;\n"
                       "    }\n"
                       "    return a * b;\n"
                       "}\n"
                       "\n"
                       "void scale(const float* restrict in, float* restrict out, int n, float g)\n"
                       "    [[clang::nonblocking]]\n"
                       "{\n"
                       "    for (int i = 0; i < n; ++i)\n"
                       "        out[i] = in[i] * g;\n"
                       "}\n",
                       &errors, with(Denormals::Flush));
    INFO(errors);
    REQUIRE(jit);
    auto* pick = lookup<float(float, float, int)>(*jit, "pick");
    auto* scale = lookup<void(const float*, float*, int, float)>(*jit, "scale");

    for (const bool flushing : {false, true})
    {
        INFO("the host " << (flushing ? "flushing" : "in IEEE mode"));
        const ScopedMode host(flushing);
        const std::uint64_t before = controlRegister();
        for (const int n : {0, 1, 5})   // each return: the first, after the loop, inside it
        {
            CHECK(pick(1e-30f, 1e-10f, n) == 0.0f);
            CHECK(controlRegister() == before);
        }
        CHECK(pick(3.0f, 2.0f, 0) == 6.0f);
        CHECK(controlRegister() == before);

        const std::array<float, 4> in{1e-30f, 1.0f, -1e-30f, 1e-20f};
        std::array<float, 4> out{};
        scale(in.data(), out.data(), 4, 1e-10f);
        CHECK(out[0] == 0.0f);
        CHECK(out[1] == 1e-10f);
        CHECK(out[2] == 0.0f);
        CHECK(std::signbit(out[2]));
        CHECK(out[3] == opaque(1e-20f) * opaque(1e-10f));
        CHECK(controlRegister() == before);
    }
}

TEST_CASE("under Preserve subnormals are numbers, the mode is the host's, and the code is as it was",
          "[codegen][denormals]")
{
    std::string errors;
    auto jit = compile(kProduct, &errors, with(Denormals::Preserve));
    INFO(errors);
    REQUIRE(jit);
    auto* product = lookup<float(float, float)>(*jit, "product");
    {
        const ScopedMode host(false);
        const std::uint64_t before = controlRegister();
        const float tiny = product(1e-30f, 1e-10f);
        CHECK(subnormal(tiny));
        CHECK(sameBits(tiny, opaque(1e-30f) * opaque(1e-10f)));
        CHECK(sameBits(product(1e-40f, 1e30f), opaque(1e-40f) * opaque(1e30f)));
        CHECK(controlRegister() == before);
    }
    {
        // Preserve sets nothing: in a host that flushes, the code flushes.
        const ScopedMode host(true);
        const std::uint64_t before = controlRegister();
        CHECK(product(1e-30f, 1e-10f) == 0.0f);
        CHECK(controlRegister() == before);
    }
    // Options' default is Preserve.
    CHECK(fminus::ir::Options{}.denormals == Denormals::Preserve);
}

TEST_CASE("a one-pole gone silent sticks at a subnormal under Preserve, and above them under Flush, as C++ does",
          "[codegen][denormals]")
{
    std::string errors;
    auto preserved = compile(kProduct, &errors, with(Denormals::Preserve));
    auto flushed = compile(kProduct, &errors, with(Denormals::Flush));
    INFO(errors);
    REQUIRE(preserved);
    REQUIRE(flushed);
    using OnePole = float(float, float, int);
    constexpr float k = 0.01f;
    constexpr int n = 20000;   // 0.99^20000 is 1e-87: far below the least subnormal, were it exact

    const ScopedMode host(false);
    const float ieee = lookup<OnePole>(*preserved, "onepole")(1.0f, k, n);
    CHECK(subnormal(ieee));   // where k * y rounds to nothing: stuck, for ever
    CHECK(sameBits(ieee, onePole(1.0f, k, n)));

    const float zeroed = lookup<OnePole>(*flushed, "onepole")(1.0f, k, n);
    CHECK_FALSE(subnormal(zeroed));   // where k * y is a subnormal, flushed to 0: stuck at a normal number
    volatile float reference = 0.0f;   // written before the mode is restored
    {
        const ScopedMode flushing(true);
        reference = onePole(opaque(1.0f), opaque(k), n);
    }
    CHECK(sameBits(zeroed, reference));
}

TEST_CASE("a nonblocking function the program calls is no entry point: it runs in its caller's mode",
          "[codegen][denormals]")
{
    std::string errors;
    auto jit = compile("float mul(float a, float b) [[clang::nonblocking]] { return a * b; }\n"
                       "float twice(float a, float b) [[clang::nonblocking]] { return mul(a, b) * 2; }\n"
                       "float power(float x, int n) [[clang::nonblocking]] { return n ? x * power(x, n - 1) : 1; }\n"
                       "module Gain\n"
                       "{\n"
                       "    float g;\n"
                       "    float tick(float x) [[clang::nonblocking]] { return g * x; }\n"
                       "    float process(float x) [[clang::nonblocking]] { return tick(x); }\n"
                       "};\n",
                       &errors, with(Denormals::Flush));
    INFO(errors);
    REQUIRE(jit);
    struct Gain
    {
        float g = 1e-10f;
    } gain;
    const ScopedMode host(false);
    CHECK(lookup<float(float, float)>(*jit, "twice")(1e-30f, 1e-10f) == 0.0f);   // an entry point
    CHECK(lookup<float(Gain*, float)>(*jit, "Gain::process")(&gain, 1e-30f) == 0.0f);

    // Called by twice, by itself, by process: in the host's mode, which here is IEEE's.
    CHECK(subnormal(lookup<float(float, float)>(*jit, "mul")(1e-30f, 1e-10f)));
    CHECK(subnormal(lookup<float(float, int)>(*jit, "power")(1e-20f, 2)));
    CHECK(subnormal(lookup<float(Gain*, float)>(*jit, "Gain::tick")(&gain, 1e-30f)));
    {
        const ScopedMode flushing(true);   // a host that calls them sets the mode itself
        CHECK(lookup<float(float, float)>(*jit, "mul")(1e-30f, 1e-10f) == 0.0f);
    }
}

TEST_CASE("under Flush an entry point whose body touches no memory runs flushing too, at every level",
          "[codegen][denormals]")
{
    // Each body is memory(none) once FunctionAttrs has run: a call LLVM may move, as it may an fmul, unless the
    // wrapper's call is one it must keep between the writes of the register (Denormals.cpp). At -O1 InstCombine sank
    // the call past the write that restores the caller's mode, so the body ran in an IEEE host's mode.
    constexpr const char* source = "float scaled(float x) [[clang::nonblocking]] { return x * 1e-30f * 1e-10f; }\n"
                                   "float soft(float x) [[clang::nonblocking]] { return tanhf(x) * 1e-10f; }\n"
                                   "bool above(float x) [[clang::nonblocking]] { return x * 1e-30f > 0; }\n"
                                   "float4 lanes(float4 v) [[clang::nonblocking]] { return v * 1e-30f * 1e-10f; }\n";
    for (const auto& [level, name] : {std::pair{llvm::OptimizationLevel::O0, "-O0"},
                                      std::pair{llvm::OptimizationLevel::O1, "-O1"},
                                      std::pair{llvm::OptimizationLevel::O2, "-O2"},
                                      std::pair{llvm::OptimizationLevel::O3, "-O3"}})
    {
        INFO(name);
        if (level != llvm::OptimizationLevel::O0)
        {
            llvm::LLVMContext context;
            const auto module = optimised(source, with(Denormals::Flush), context, level);
            for (const char* function : {"scaled", "soft", "above", "lanes"})
            {
                INFO(function);
                const llvm::Function* entry = module->getFunction(function);
                const llvm::Function* body = module->getFunction(std::string(function) + ".body");
                REQUIRE(entry != nullptr);
                REQUIRE(body != nullptr);
                CHECK(body->doesNotAccessMemory());   // the case this test is for
                const auto [laundered, direct] = callsOfBody(*entry, *body);
                CHECK(laundered >= 1);   // LLVM did not see through the asm
                CHECK(direct == 0);
            }
        }
        std::string errors;
        auto jit = compileAt(source, level, with(Denormals::Flush), errors);
        INFO(errors);
        REQUIRE(jit);
        auto* scaled = lookup<float(float)>(*jit, "scaled");
        auto* soft = lookup<float(float)>(*jit, "soft");
        auto* above = lookup<bool(float)>(*jit, "above");
        auto* lanes = lookup<float4(float4)>(*jit, "lanes");
        for (const bool flushing : {false, true})
        {
            INFO("the host " << (flushing ? "flushing" : "in IEEE mode"));
            float tiny = 1.0f;
            float negative = 1.0f;
            float tanh = 1.0f;
            bool positive = true;
            std::array<float, 4> four{};
            std::uint64_t before = 0;
            std::uint64_t after = 0;
            {
                const ScopedMode host(flushing);
                before = controlRegister();
                tiny = scaled(1.0f);
                negative = scaled(-1.0f);
                tanh = soft(1e-30f);
                positive = above(1e-10f);
                four = lanesOf(lanes(float4{1.0f, -1.0f, 1e20f, 0.0f}));
                after = controlRegister();
            }
            // Compared outside the host's scope: there a comparison would read a subnormal as 0 if it flushes.
            CHECK(sameBits(tiny, 0.0f));
            CHECK(sameBits(negative, -0.0f));
            CHECK(sameBits(tanh, 0.0f));
            CHECK_FALSE(positive);
            CHECK(sameBits(four[0], 0.0f));
            CHECK(sameBits(four[1], -0.0f));
            CHECK(four[2] == opaque(1e20f) * opaque(1e-30f) * opaque(1e-10f));   // 1e-20: normal, as it was
            CHECK(after == before);
        }
    }
}

TEST_CASE("under Flush a frozen parameter's folding agrees with a knob's run time, at -O0 to -O3",
          "[codegen][denormals]")
{
    for (const auto& [level, name] : {std::pair{llvm::OptimizationLevel::O0, "-O0"},
                                      std::pair{llvm::OptimizationLevel::O1, "-O1"},
                                      std::pair{llvm::OptimizationLevel::O2, "-O2"},
                                      std::pair{llvm::OptimizationLevel::O3, "-O3"}})
    {
        INFO(name);
        std::string errors;
        fminus::ir::Options knob = with(Denormals::Flush);
        fminus::ir::Options frozen = with(Denormals::Flush);
        frozen.parameters["Tiny::k"] = {fminus::ir::ParamBinding::Kind::Frozen, 1e-30};
        auto knobs = compileAt(kTiny, level, knob, errors);
        INFO(errors);
        REQUIRE(knobs);
        auto constants = compileAt(kTiny, level, frozen, errors);
        REQUIRE(constants);
        const ScopedMode host(false);

        // 1e-30 * 1e-10 is 1e-40, a subnormal: flushed to 0 by the hardware, and by the folding of the code
        // generator (built) and of LLVM (later, once mem2reg has made `a` the constant).
        const TinyResults ran = run(*knobs, 1e-30f);
        CHECK(ran.built == 0.0f);
        CHECK(ran.later == 0.0f);
        CHECK(ran.above);
        CHECK(ran.lanes == std::array<float, 4>{0.0f, 1e-10f, -0.0f, 2e-10f});
        CHECK(std::signbit(ran.lanes[2]));
        CHECK(sameBits(run(*constants, 0.0f), ran));

        // A subnormal operand reads as 0: 1e-40 > 0 is false, run or folded.
        frozen.parameters["Tiny::k"] = {fminus::ir::ParamBinding::Kind::Frozen, 1e-40};
        auto tiny = compileAt(kTiny, level, frozen, errors);
        REQUIRE(tiny);
        const TinyResults read = run(*knobs, 1e-40f);
        CHECK_FALSE(read.above);
        CHECK(read.built == 0.0f);
        CHECK(sameBits(run(*tiny, 0.0f), read));
    }
}

TEST_CASE("under Preserve a frozen parameter folds as IEEE computes, as a knob runs in an IEEE host",
          "[codegen][denormals]")
{
    std::string errors;
    fminus::ir::Options frozen;
    frozen.parameters["Tiny::k"] = {fminus::ir::ParamBinding::Kind::Frozen, 1e-30};
    auto knobs = compile(kTiny, &errors);
    auto constants = compile(kTiny, &errors, frozen);
    INFO(errors);
    REQUIRE(knobs);
    REQUIRE(constants);
    const TinyResults folded = run(*constants, 0.0f);
    {
        const ScopedMode host(false);
        const TinyResults ran = run(*knobs, 1e-30f);
        CHECK(subnormal(ran.built));
        CHECK(sameBits(folded, ran));
    }
    // What Flush is for: a host that flushes runs the knob's code flushing, while the frozen program's constant was
    // folded as IEEE says, so the two disagree. (Checked outside the flushing scope: a comparison there would read a
    // subnormal as 0, fpclassify's too.)
    TinyResults flushing{};
    {
        const ScopedMode host(true);
        flushing = run(*knobs, 1e-30f);
    }
    CHECK(flushing.built == 0.0f);
    CHECK(subnormal(folded.built));
}

TEST_CASE("under Flush every definition is marked preservesign, and an entry point is a dynamic wrapper of its body",
          "[codegen][denormals]")
{
    constexpr const char* source = "float ext(float x) [[clang::nonblocking]];\n"
                                   "float mul(float a, float b) [[clang::nonblocking]] { return a * b; }\n"
                                   "float init(float a) { return a; }\n"
                                   "bool gate(bool on, float x) [[clang::nonblocking]] { return on && mul(x, x) > 0; }"
                                   "\n"
                                   "module OnePole\n"
                                   "{\n"
                                   "    float y;\n"
                                   "    void process(float* restrict out, int n) [[clang::nonblocking]]\n"
                                   "    {\n"
                                   "        for (int i = 0; i < n; ++i)\n"
                                   "            out[i] = y *= 0.5f;\n"
                                   "    }\n"
                                   "};\n";
    llvm::LLVMContext context;
    std::string errors;
    auto module = lowered(source, with(Denormals::Flush), context, errors);
    INFO(errors);
    REQUIRE(module);
    CHECK_FALSE(llvm::verifyModule(*module, &llvm::errs()));

    // Every definition, wrapped or not, assumes the flushing mode; a wrapper the caller's.
    for (const char* name : {"mul", "init", "gate.body", "OnePole::process.body"})
    {
        INFO(name);
        const llvm::Function* function = module->getFunction(name);
        REQUIRE(function != nullptr);
        CHECK(modeOf(*function) == kPreserveSign);
    }
    for (const char* name : {"gate", "OnePole::process"})
    {
        INFO(name);
        const llvm::Function* entry = module->getFunction(name);
        REQUIRE(entry != nullptr);
        CHECK(entry->hasExternalLinkage());
        CHECK(modeOf(*entry) == kDynamic);
        CHECK_FALSE(entry->hasFnAttribute(llvm::Attribute::NoInline));
        const llvm::Function* body = module->getFunction(std::string(name) + ".body");
        REQUIRE(body != nullptr);
        CHECK(body->hasInternalLinkage());
        CHECK(body->hasFnAttribute(llvm::Attribute::NoInline));
        CHECK(intrinsicsOf(*entry) == std::vector<llvm::Intrinsic::ID>{llvm::Intrinsic::aarch64_get_fpcr,
                                                                      llvm::Intrinsic::aarch64_set_fpcr,
                                                                      llvm::Intrinsic::aarch64_set_fpcr});
        CHECK(writesInEntryBlock(*entry) == 0);   // each write behind the test of the caller's mode
        // Through its address passed through an empty asm, on each of the two paths, and never directly.
        CHECK(callsOfBody(*entry, *body) == std::pair{2, 0});
    }
    // The C ABI's attributes go with the name: gate's bools are zeroext, process's `this` nonnull and its out noalias.
    const llvm::Function* gate = module->getFunction("gate");
    CHECK(gate->hasRetAttribute(llvm::Attribute::ZExt));
    CHECK(gate->hasParamAttribute(0, llvm::Attribute::ZExt));
    const llvm::Function* process = module->getFunction("OnePole::process");
    CHECK(process->hasParamAttribute(0, llvm::Attribute::NonNull));
    CHECK(process->hasParamAttribute(1, llvm::Attribute::NoAlias));

    // mul is called by gate; init is not nonblocking; ext has no definition, so nothing to mark.
    CHECK(module->getFunction("mul.body") == nullptr);
    CHECK(module->getFunction("init.body") == nullptr);
    CHECK_FALSE(module->getFunction("ext")->hasFnAttribute(llvm::Attribute::DenormalFPEnv));

    // Under Preserve: no attribute and no wrapper anywhere.
    llvm::LLVMContext other;
    auto preserved = lowered(source, with(Denormals::Preserve), other, errors);
    REQUIRE(preserved);
    for (const llvm::Function& function : *preserved)
    {
        INFO(function.getName().str());
        CHECK_FALSE(function.hasFnAttribute(llvm::Attribute::DenormalFPEnv));
        CHECK_FALSE(function.getName().ends_with(".body"));
        CHECK_FALSE(function.getName().starts_with("llvm.aarch64"));
    }
}

TEST_CASE("on x86-64 an entry point sets MXCSR's FTZ and DAZ, through stack slots", "[codegen][denormals]")
{
    llvm::LLVMContext context;
    std::string errors;
    fminus::ir::Options options = with(Denormals::Flush);
    options.targetTriple = "x86_64-apple-macosx14.0.0";
    auto module = lowered("void process(float* restrict out, int n) [[clang::nonblocking]]\n"
                          "{\n"
                          "    for (int i = 0; i < n; ++i)\n"
                          "        out[i] *= 1e-10f;\n"
                          "}\n",
                          options, context, errors);
    INFO(errors);
    REQUIRE(module);
    CHECK_FALSE(llvm::verifyModule(*module, &llvm::errs()));
    const llvm::Function* entry = module->getFunction("process");
    REQUIRE(entry != nullptr);
    CHECK(modeOf(*entry) == kDynamic);
    CHECK(intrinsicsOf(*entry) == std::vector<llvm::Intrinsic::ID>{llvm::Intrinsic::x86_sse_stmxcsr,
                                                                  llvm::Intrinsic::x86_sse_ldmxcsr,
                                                                  llvm::Intrinsic::x86_sse_ldmxcsr});
    CHECK(writesInEntryBlock(*entry) == 0);
    bool setsBoth = false;   // or i32 %mode, 0x8040: FTZ (bit 15) and DAZ (bit 6)
    for (const llvm::Instruction& instruction : entry->getEntryBlock())
    {
        if (const auto* bits = llvm::dyn_cast<llvm::BinaryOperator>(&instruction);
            bits != nullptr && bits->getOpcode() == llvm::Instruction::Or)
        {
            const auto* mask = llvm::dyn_cast<llvm::ConstantInt>(bits->getOperand(1));
            setsBoth = mask != nullptr && mask->getZExtValue() == 0x8040;
        }
    }
    CHECK(setsBoth);
    const llvm::Function* body = module->getFunction("process.body");
    CHECK(modeOf(*body) == kPreserveSign);
    CHECK(callsOfBody(*entry, *body) == std::pair{2, 0});
}

TEST_CASE("Flush on a target whose mode FMinus cannot set is an error, and Preserve is not", "[codegen][denormals]")
{
    llvm::LLVMContext context;
    std::string errors;
    fminus::ir::Options options = with(Denormals::Flush);
    options.targetTriple = "riscv64-unknown-linux-gnu";
    CHECK(lowered(kProduct, options, context, errors) == nullptr);
    CHECK(errors == "test.fm:1:1: error: cannot flush subnormals to zero on riscv64: FMinus sets the floating-point "
                    "mode on arm64 and x86-64\n");
    options.denormals = Denormals::Preserve;
    CHECK(lowered(kProduct, options, context, errors) != nullptr);
    CHECK(errors.empty());
}

TEST_CASE("the code generator's folder flushes under Flush, and is ConstantFolder under Preserve",
          "[codegen][denormals]")
{
    llvm::LLVMContext context;
    llvm::Type* type = llvm::Type::getFloatTy(context);
    const auto number = [&](float value) { return llvm::ConstantFP::get(type, static_cast<double>(value)); };
    const auto value = [](llvm::Value* folded) {
        REQUIRE(folded != nullptr);
        return llvm::cast<llvm::ConstantFP>(folded)->getValueAPF().convertToFloat();
    };
    const fminus::ir::detail::DenormalFolder flush(true);
    const fminus::ir::detail::DenormalFolder preserve(false);

    // A subnormal result, from normal operands.
    CHECK(value(flush.FoldBinOp(llvm::Instruction::FMul, number(1e-30f), number(1e-10f))) == 0.0f);
    CHECK(std::signbit(value(flush.FoldBinOp(llvm::Instruction::FMul, number(-1e-30f), number(1e-10f)))));
    CHECK(subnormal(value(preserve.FoldBinOp(llvm::Instruction::FMul, number(1e-30f), number(1e-10f)))));
    // A subnormal operand: 1e-40 * 1e30 is 0 read flushing, 1e-10 in IEEE's arithmetic.
    CHECK(value(flush.FoldBinOp(llvm::Instruction::FMul, number(1e-40f), number(1e30f))) == 0.0f);
    CHECK(value(preserve.FoldBinOp(llvm::Instruction::FMul, number(1e-40f), number(1e30f))) > 0.0f);
    CHECK(value(flush.FoldBinOp(llvm::Instruction::FAdd, number(1e-40f), number(1.0f))) == 1.0f);
    CHECK(value(flush.FoldBinOp(llvm::Instruction::FSub, number(1.5e-38f), number(1.4e-38f))) == 0.0f);
    CHECK(value(flush.FoldBinOp(llvm::Instruction::FDiv, number(1e-30f), number(1e10f))) == 0.0f);
    CHECK(value(flush.FoldBinOp(llvm::Instruction::FMul, number(2.0f), number(3.0f))) == 6.0f);

    // Comparisons read a subnormal as 0.
    llvm::Value* above = flush.FoldCmp(llvm::CmpInst::FCMP_OGT, number(1e-40f), number(0.0f));
    REQUIRE(above != nullptr);
    CHECK(llvm::cast<llvm::ConstantInt>(above)->isZero());
    CHECK(llvm::cast<llvm::ConstantInt>(preserve.FoldCmp(llvm::CmpInst::FCMP_OGT, number(1e-40f), number(0.0f)))
              ->isOne());

    // Vectors lane by lane.
    llvm::Constant* lanes = llvm::ConstantVector::get({number(1e-30f), number(1.0f), number(-1e-30f), number(1e-40f)});
    llvm::Constant* scale = llvm::ConstantVector::getSplat(llvm::ElementCount::getFixed(4), number(1e-10f));
    auto* product = llvm::cast<llvm::Constant>(flush.FoldBinOp(llvm::Instruction::FMul, lanes, scale));
    CHECK(value(product->getAggregateElement(0u)) == 0.0f);
    CHECK(value(product->getAggregateElement(1u)) == 1e-10f);
    CHECK(std::signbit(value(product->getAggregateElement(2u))));
    CHECK(value(product->getAggregateElement(3u)) == 0.0f);

    // What isn't two constants is not folded; integers and fneg are ConstantFolder's.
    llvm::Argument argument(type);
    CHECK(flush.FoldBinOp(llvm::Instruction::FMul, &argument, number(1e-10f)) == nullptr);
    llvm::Type* integer = llvm::Type::getInt32Ty(context);
    llvm::Value* sum = flush.FoldBinOp(llvm::Instruction::Add, llvm::ConstantInt::get(integer, 2),
                                       llvm::ConstantInt::get(integer, 3));
    CHECK(llvm::cast<llvm::ConstantInt>(sum)->getZExtValue() == 5);
    CHECK(subnormal(value(flush.FoldUnOpFMF(llvm::Instruction::FNeg, number(1e-40f), {}))));
}
