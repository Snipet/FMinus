// tests/unit/CodeRangesTest.cpp: where the JIT's machine code is (Jit::code, jit/Jit.h), for a host's sampling
// profiler: each function's code at the address its look-up gives, sized, sorted and apart, through JITLink and through
// RuntimeDyld (Windows' linker) alike, and sized alike where the two link the same code; the parallel regions, an entry
// point and its body, FMinus's own code and a thunk among them; and which calls the optimiser inlined, read from the
// calls, counted, before and after it (Jit::inlinedCalls).
#include "Compile.h"

#include "jit/Call.h"
#include "support/HostTarget.h"

#include <catch2/catch_test_macros.hpp>

#include <llvm/Target/TargetMachine.h>

#include <algorithm>
#include <cstdint>
#include <string>
#include <vector>

namespace
{
    using fminus::jit::Jit;

    // An oscillator whose process inlines its tick (from -O1), a function a parallel region calls, an entry point, and
    // a sinf whose argument may be large (FMinus's own path for it, fminus.large.sin).
    constexpr const char* kSource = R"(
module Osc
{
    float phase;
    param float freq;

    float tick()
    {
        phase = phase + freq[0];
        if (phase >= 1.0f)
            phase = phase - 1.0f;
        return sinf(phase * 6.2831853f + freq[0] * 1000.0f);
    }

    void process(float* out, int n) [[clang::nonblocking]]
    {
        for (int i = 0; i < n; i++)
            out[i] = tick();
    }
};

float shape(float x)
{
    return x / (1.0f + x * x);
}

void run(Osc* o, float* out, int n) [[clang::nonblocking]]
{
    o->process(out, n);
    parallel for (int i = 0; i < n; i++)
        out[i] = shape(out[i]);
}
)";

    std::unique_ptr<Jit> jitOf(const char* source, llvm::OptimizationLevel level, Jit::Linker linker,
                               fminus::ir::Denormals denormals = fminus::ir::Denormals::Preserve)
    {
        fminus::Diagnostics diagnostics("test.fm");
        const auto program = fminus::frontend::parse(source, diagnostics);
        auto context = std::make_unique<llvm::LLVMContext>();
        fminus::ir::Options options;
        options.denormals = denormals;
        std::unique_ptr<llvm::Module> module =
            program ? fminus::ir::lower(*program, *context, diagnostics, options) : nullptr;
        INFO(diagnostics.format());
        REQUIRE(module);
        auto jit = llvm::cantFail(Jit::create(level, Jit::Modules::Unoptimised, linker));
        llvm::cantFail(jit->add(std::move(module), std::move(context)));
        return jit;
    }

    const Jit::Code* named(const std::vector<Jit::Code>& code, const std::string& name)
    {
        const auto found =
            std::find_if(code.begin(), code.end(), [&](const Jit::Code& function) { return function.name == name; });
        return found != code.end() ? &*found : nullptr;
    }

    // The function `name`, which must be there.
    const Jit::Code& required(const std::vector<Jit::Code>& code, const std::string& name)
    {
        const Jit::Code* found = named(code, name);
        INFO(name);
        REQUIRE(found != nullptr);
        return *found;
    }

    std::vector<std::string> names(const std::vector<Jit::Code>& code)
    {
        std::vector<std::string> all;
        for (const Jit::Code& function : code)
            all.push_back(function.name);
        return all;
    }
} // namespace

TEST_CASE("code ranges: each function's code where its look-up says, sized, sorted and apart, through either linker",
          "[jit][profile]")
{
    using Denormals = fminus::ir::Denormals;
    for (const Jit::Linker linker : {Jit::Linker::Default, Jit::Linker::RuntimeDyld})
    {
        for (const Denormals denormals : {Denormals::Preserve, Denormals::Flush})
        {
            DYNAMIC_SECTION((linker == Jit::Linker::Default ? "LLJIT's linker" : "RuntimeDyld")
                            << (denormals == Denormals::Flush ? ", flushing" : ""))
            {
                auto jit = jitOf(kSource, llvm::OptimizationLevel::O2, linker, denormals);
                CHECK(jit->code().empty());   // nothing is linked before a look-up
                const std::uint64_t run = llvm::cantFail(jit->lookup("run")).getValue();
                const std::vector<Jit::Code> code = jit->code();
                INFO(Catch::Detail::stringify(names(code)));
                REQUIRE(!code.empty());

                // Each of the program's functions where its look-up finds it (an entry point's code, under Flush, the
                // few instructions around its body, which is code of its own).
                for (const char* function : {"Osc::tick", "Osc::process", "shape", "run"})
                {
                    INFO(function);
                    const Jit::Code* found = named(code, function);
                    REQUIRE(found != nullptr);
                    CHECK(found->address == llvm::cantFail(jit->lookup(function)).getValue());
                    CHECK(found->size > 0);
                }
                CHECK(required(code, "run").address == run);
                CHECK((named(code, "run.body") != nullptr) == (denormals == Denormals::Flush));
                CHECK(named(code, "run.parallel.1") != nullptr);
                CHECK(named(code, "fminus.large.sin") != nullptr);

                // Sorted by address, apart, and named as the module names them: no global prefix (macOS's '_'), and
                // no assembler's label (ltmp0).
                for (std::size_t i = 0; i < code.size(); ++i)
                {
                    INFO(code[i].name);
                    CHECK(code[i].name.front() != '_');
                    CHECK(code[i].name.find("ltmp") == std::string::npos);
                    if (i > 0)
                        CHECK(code[i - 1].address + code[i - 1].size <= code[i].address);
                }
            }
        }
    }
}

TEST_CASE("code ranges: RuntimeDyld sizes each function as JITLink does, where the two link the same code",
          "[jit][profile]")
{
    // LLJIT gives JITLink the small code model, and leaves RuntimeDyld (Windows') the target's for a JIT: the large
    // one on x86-64, and on AArch64 but Windows'. There code that names an address (a global's, a constant's, a
    // function's) is not the same, and a function's size neither: run, which passes its parallel region's to the
    // pool, is 8 bytes longer on an M5. These functions name none, so the two objects are the same, and so must be
    // each function's place and size, but the last's, which RuntimeDyld runs to its section's end.
    constexpr const char* kIntegers = R"(
int mix(int a, int b)
{
    return a * 3 + b / 7;
}

int pick(int a, int b, int c)
{
    if (a > b)
        return b > c ? b - a : c * a;
    return a - c;
}

int fold(int a)
{
    return (a % 5) * (a / 3) - a;
}
)";
    auto linked = jitOf(kIntegers, llvm::OptimizationLevel::O2, Jit::Linker::Default);
    auto loaded = jitOf(kIntegers, llvm::OptimizationLevel::O2, Jit::Linker::RuntimeDyld);
    REQUIRE(static_cast<bool>(linked->lookup("mix")));
    REQUIRE(static_cast<bool>(loaded->lookup("mix")));
    const std::vector<Jit::Code> byJitLink = linked->code();
    const std::vector<Jit::Code> byRuntimeDyld = loaded->code();
    INFO(Catch::Detail::stringify(names(byJitLink)));
    REQUIRE(names(byRuntimeDyld) == names(byJitLink));
    REQUIRE(byJitLink.size() == 3);
    for (std::size_t i = 0; i < byJitLink.size(); ++i)
    {
        const Jit::Code& jitLink = byJitLink[i];
        const Jit::Code& runtimeDyld = byRuntimeDyld[i];
        INFO(jitLink.name << ": " << jitLink.size << " bytes through JITLink, " << runtimeDyld.size
                          << " through RuntimeDyld");
        CHECK(runtimeDyld.address - byRuntimeDyld.front().address == jitLink.address - byJitLink.front().address);
        if (i + 1 < byJitLink.size())
            CHECK(runtimeDyld.size == jitLink.size);
        else
            CHECK(runtimeDyld.size >= jitLink.size);
    }
}

TEST_CASE("code ranges: what the optimiser inlined into each function, and at -O0 nothing", "[jit][profile]")
{
    SECTION("-O2: process holds tick, run holds both, and the parallel region holds shape")
    {
        auto jit = jitOf(kSource, llvm::OptimizationLevel::O2, Jit::Linker::Default);
        REQUIRE(static_cast<bool>(jit->lookup("run")));
        const std::vector<Jit::Code> code = jit->code();
        using Names = std::vector<std::string>;
        CHECK(required(code, "Osc::process").inlined == Names{"Osc::tick"});
        CHECK(required(code, "run").inlined == Names{"Osc::process", "Osc::tick"});
        CHECK(required(code, "run.parallel.1").inlined == Names{"shape"});
        CHECK(required(code, "Osc::tick").inlined.empty());   // its own code, which the program keeps for its callers
        CHECK(required(code, "shape").inlined.empty());
    }
    SECTION("-O0: every function's code is its own")
    {
        auto jit = jitOf(kSource, llvm::OptimizationLevel::O0, Jit::Linker::Default);
        REQUIRE(static_cast<bool>(jit->lookup("run")));
        for (const Jit::Code& function : jit->code())
        {
            INFO(function.name);
            CHECK(function.inlined.empty());
        }
    }
}

TEST_CASE("code ranges: inlinedCalls follows the calls the optimiser removed, and stops at those it kept",
          "[jit][profile]")
{
    // f called g and k once each; g called h, and h called g back (recursion). After: f calls k only, or h too.
    const Jit::Calls before{{"f", {{"g", 1}, {"k", 1}}}, {"g", {{"h", 1}}}, {"h", {{"g", 1}}}, {"k", {}},
                            {"gone", {{"k", 1}}}};
    using Names = std::vector<std::string>;

    const Jit::Inlined inlined =
        Jit::inlinedCalls(before, {{"f", {{"k", 1}}}, {"g", {{"h", 1}}}, {"h", {{"g", 1}}}, {"k", {}}});
    CHECK(inlined.at("f") == Names{"g", "h"});   // g, and h through it; k is still called, so its code is its own
    CHECK(inlined.size() == 1);                  // g and h call each other still, k calls nothing, gone is gone

    const Jit::Inlined kept = Jit::inlinedCalls(before, {{"f", {{"h", 1}, {"k", 1}}}});
    CHECK(kept.at("f") == Names{"g"});   // h is called still, by g's code in f: h's code, and g's inside it, are h's

    CHECK(Jit::inlinedCalls(before, before).empty());
    CHECK(Jit::inlinedCalls({{"f", {{"f", 1}}}}, {{"f", {}}}).empty());   // a function in itself is no inlining
}

TEST_CASE("code ranges: inlinedCalls counts the calls, so a callee inlined at one call and called at another is listed",
          "[jit][profile]")
{
    using Names = std::vector<std::string>;

    // f called g twice, and g called h. One of the calls is inlined, the other kept: g's code is in f, and so is h's
    // unless f calls h after (the call g's code in f brought).
    const Jit::Calls twice{{"f", {{"g", 2}}}, {"g", {{"h", 1}}}, {"h", {}}};
    CHECK(Jit::inlinedCalls(twice, {{"f", {{"g", 1}, {"h", 1}}}}).at("f") == Names{"g"});
    CHECK(Jit::inlinedCalls(twice, {{"f", {{"g", 1}}}}).at("f") == Names{"g", "h"});
    CHECK(Jit::inlinedCalls(twice, twice).empty());
    CHECK(Jit::inlinedCalls(twice, {{"f", {{"g", 3}}}}).empty());   // a call copied (a loop unrolled) is no inlining

    // A diamond: f called g and k, which each called h. Both inlined, they bring f two calls of h: h is inlined when
    // f calls it fewer times than that, whichever of g and k the walk meets first.
    const Jit::Calls diamond{{"f", {{"g", 1}, {"k", 1}}}, {"g", {{"h", 1}}}, {"k", {{"h", 1}}}, {"h", {}}};
    CHECK(Jit::inlinedCalls(diamond, {{"f", {{"h", 2}}}}).at("f") == Names{"g", "k"});
    CHECK(Jit::inlinedCalls(diamond, {{"f", {{"h", 1}}}}).at("f") == Names{"g", "h", "k"});
    CHECK(Jit::inlinedCalls(diamond, {{"f", {}}}).at("f") == Names{"g", "h", "k"});

    // A chain of calls, each function calling the next 1,000 times, counts past what a size_t holds: the count stays
    // at its largest (wrapped, it would be 0 from c22 on, 1000^22 being a multiple of 2^64).
    Jit::Calls chain;
    for (int i = 0; i < 40; ++i)
        chain["c" + std::to_string(i)] = {{"c" + std::to_string(i + 1), 1000}};
    const Jit::Inlined all = Jit::inlinedCalls(chain, {{"c0", {{"c40", 1}}}});
    CHECK(all.at("c0").size() == 40);   // c1 to c40, of whose calls one is left
}

TEST_CASE("code ranges: a method inlined at one call and called at another is listed in its caller's code",
          "[jit][profile]")
{
    // g's first path is short, its second long: with mode 0, the first call's is all that is left, which the optimiser
    // inlines; the second, whose mode is the caller's, stays a call.
    constexpr const char* kTwoCalls = R"(
module Osc
{
    float g(float x, int mode)
    {
        float y = x;
        if (mode == 0)
        {
            for (int k = 0; k < 64; k++)
                y = y * 0.99f + x * 0.01f;
            return y;
        }
        y = y / (1.0f + y * y) + x / (2.0f + x * y);
        y = y / (2.0f + y * y) + x / (3.0f + x * y);
        y = y / (3.0f + y * y) + x / (4.0f + x * y);
        y = y / (4.0f + y * y) + x / (5.0f + x * y);
        y = y / (5.0f + y * y) + x / (6.0f + x * y);
        y = y / (6.0f + y * y) + x / (7.0f + x * y);
        y = y / (7.0f + y * y) + x / (8.0f + x * y);
        y = y / (8.0f + y * y) + x / (9.0f + x * y);
        y = y / (9.0f + y * y) + x / (10.0f + x * y);
        y = y / (10.0f + y * y) + x / (11.0f + x * y);
        y = y / (11.0f + y * y) + x / (12.0f + x * y);
        y = y / (12.0f + y * y) + x / (13.0f + x * y);
        return y;
    }

    void process(float* out, int n, int mode) [[clang::nonblocking]]
    {
        for (int i = 0; i < n; i++)
            out[i] = g(out[i], 0) + g(out[i], mode);
    }
};
)";
    // What the optimiser makes of it: process called g twice before, and once after.
    {
        fminus::Diagnostics diagnostics("test.fm");
        const auto program = fminus::frontend::parse(kTwoCalls, diagnostics);
        llvm::LLVMContext context;
        std::unique_ptr<llvm::Module> module =
            program ? fminus::ir::lower(*program, context, diagnostics, {}) : nullptr;
        INFO(diagnostics.format());
        REQUIRE(module);
        auto machine = llvm::cantFail(fminus::hostTargetMachine(llvm::CodeGenOptLevel::Default));
        fminus::prepareModule(*module, *machine);
        const Jit::Calls before = Jit::directCalls(*module);
        fminus::optimiseModule(*module, *machine, llvm::OptimizationLevel::O2);
        const Jit::Calls after = Jit::directCalls(*module);
        CHECK(before.at("Osc::process").at("Osc::g") == 2);
        CHECK(after.at("Osc::process").at("Osc::g") == 1);
    }
    auto jit = jitOf(kTwoCalls, llvm::OptimizationLevel::O2, Jit::Linker::Default);
    REQUIRE(static_cast<bool>(jit->lookup("Osc::process")));
    const std::vector<Jit::Code> code = jit->code();
    CHECK(required(code, "Osc::process").inlined == std::vector<std::string>{"Osc::g"});
    CHECK(required(code, "Osc::g").inlined.empty());
}

TEST_CASE("code ranges: a thunk's code is the JIT's too, once it is made", "[jit][profile][call]")
{
    auto jit = jitOf(kSource, llvm::OptimizationLevel::O2, Jit::Linker::Default);
    REQUIRE(static_cast<bool>(jit->lookup("shape")));
    CHECK(named(jit->code(), "fminus.call.shape") == nullptr);
    using fminus::jit::CallType;
    auto thunk = fminus::jit::callThunk(*jit, "shape", CallType::Float, {CallType::Float});
    REQUIRE(static_cast<bool>(thunk));
    const std::vector<Jit::Code> code = jit->code();
    const Jit::Code* found = named(code, "fminus.call.shape");
    REQUIRE(found != nullptr);
    CHECK(found->address == reinterpret_cast<std::uintptr_t>(*thunk));
}
