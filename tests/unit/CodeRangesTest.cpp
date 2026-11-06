// tests/unit/CodeRangesTest.cpp: where the JIT's machine code is (Jit::code, jit/Jit.h), for a host's sampling
// profiler: each function's code at the address its look-up gives, sized, sorted and apart, through JITLink and through
// RuntimeDyld (Windows' linker) alike; the parallel regions, an entry point and its body, FMinus's own code and a thunk
// among them; and which calls the optimiser inlined, read from the calls before and after it (Jit::inlinedCalls).
#include "Compile.h"

#include "jit/Call.h"

#include <catch2/catch_test_macros.hpp>

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
                CHECK(named(code, "run")->address == run);
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

TEST_CASE("code ranges: what the optimiser inlined into each function, and at -O0 nothing", "[jit][profile]")
{
    SECTION("-O2: process holds tick, run holds both, and the parallel region holds shape")
    {
        auto jit = jitOf(kSource, llvm::OptimizationLevel::O2, Jit::Linker::Default);
        REQUIRE(static_cast<bool>(jit->lookup("run")));
        const std::vector<Jit::Code> code = jit->code();
        using Names = std::vector<std::string>;
        CHECK(named(code, "Osc::process")->inlined == Names{"Osc::tick"});
        CHECK(named(code, "run")->inlined == Names{"Osc::process", "Osc::tick"});
        CHECK(named(code, "run.parallel.1")->inlined == Names{"shape"});
        CHECK(named(code, "Osc::tick")->inlined.empty());   // its own code, which the program keeps for its callers
        CHECK(named(code, "shape")->inlined.empty());
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
    // f called g and k; g called h, and h called g back (recursion). After: f calls k only, or h too.
    const Jit::Calls before{{"f", {"g", "k"}}, {"g", {"h"}}, {"h", {"g"}}, {"k", {}}, {"gone", {"k"}}};
    using Names = std::vector<std::string>;

    const Jit::Calls inlined = Jit::inlinedCalls(before, {{"f", {"k"}}, {"g", {"h"}}, {"h", {"g"}}, {"k", {}}});
    CHECK(inlined.at("f") == Names{"g", "h"});   // g, and h through it; k is still called, so its code is its own
    CHECK(inlined.size() == 1);                  // g and h call each other still, k calls nothing, gone is gone

    const Jit::Calls kept = Jit::inlinedCalls(before, {{"f", {"h", "k"}}});
    CHECK(kept.at("f") == Names{"g"});   // h is called still: its code, and g's inside it, are h's

    CHECK(Jit::inlinedCalls(before, before).empty());
    CHECK(Jit::inlinedCalls({{"f", {"f"}}}, {{"f", {}}}).empty());   // a function in itself is no inlining
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
