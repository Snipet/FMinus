// tests/unit/LoopBudgetTest.cpp: the loop budget (ir::Options::loopBudget, opt/LoopBudget.h), in the JIT at each level:
// a call that runs out returns early with the flag set, and so does every caller; what a loop without a count, a loop
// with one and a recursion take of it; what a stopped call leaves in memory (its loop's stores to the output, and a
// module's field the optimiser kept in a register, not stored at all); -1 left after a stop, so that no count wraps
// however many calls run out; and a call within its budget computing what it would without the budget, bit for bit.
#include "Compile.h"
#include "opt/LoopBudget.h"
#include "runtime/Runtime.h"

#include <llvm/AsmParser/Parser.h>
#include <llvm/Support/SourceMgr.h>

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstring>
#include <limits>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace
{
    using fminus::test::lookup;

    // The program JIT-compiled at -O`level` (2 by default), with or without the budget; nothing, with its errors in
    // `errors`, when it does not compile.
    std::unique_ptr<fminus::jit::Jit> compileAt(std::string_view source, int level, bool budget = true,
                                                std::string* errors = nullptr)
    {
        constexpr std::array<llvm::OptimizationLevel, 4> levels{
            llvm::OptimizationLevel::O0, llvm::OptimizationLevel::O1, llvm::OptimizationLevel::O2,
            llvm::OptimizationLevel::O3};
        fminus::Diagnostics diagnostics("test.fm");
        const auto program = fminus::frontend::parse(source, diagnostics);
        auto context = std::make_unique<llvm::LLVMContext>();
        fminus::ir::Options options;
        options.loopBudget = budget;
        std::unique_ptr<llvm::Module> module =
            program ? fminus::ir::lower(*program, *context, diagnostics, options) : nullptr;
        if (errors != nullptr)
            *errors = diagnostics.format();
        if (!module)
            return nullptr;
        auto jit = llvm::cantFail(fminus::jit::Jit::create(levels[static_cast<std::size_t>(level)]));
        llvm::cantFail(jit->add(std::move(module), std::move(context)));
        return jit;
    }

    // The program's budget, as a host reads and writes it.
    fminus_rt_loop_budget_state& budgetOf(fminus::jit::Jit& jit)
    {
        return *lookup<fminus_rt_loop_budget_state>(jit, fminus::opt::kLoopBudgetSymbol);
    }

    void arm(fminus_rt_loop_budget_state& budget, std::int64_t left)
    {
        budget.left = left;
        budget.stopped = 0;
    }

    // Never ends for x > 0: a float's condition has no count.
    const char* const kSpin = "float spin(float x)\n"
                              "{\n"
                              "    while (x > 0)\n"
                              "        x = x * 1.0f;\n"
                              "    return x;\n"
                              "}\n";
} // namespace

TEST_CASE("a loop that never ends returns early with the flag set, at every level", "[budget]")
{
    for (int level = 0; level <= 3; ++level)
    {
        INFO("-O" << level);
        std::string errors;
        auto jit = compileAt(kSpin, level, true, &errors);
        INFO(errors);
        REQUIRE(jit);
        auto* spin = lookup<float(float)>(*jit, "spin");
        fminus_rt_loop_budget_state& budget = budgetOf(*jit);

        arm(budget, 1000);
        CHECK(spin(1.0f) == 0.0f);
        CHECK(budget.stopped == 1);
        CHECK(budget.left == -1);   // the branch back that went past it

        arm(budget, 1000);
        CHECK(spin(-1.0f) == -1.0f);   // no branch back
        CHECK(budget.stopped == 0);
        CHECK(budget.left == 1000);
    }
}

TEST_CASE("the budget is unlimited until the host sets it, and what is left carries over", "[budget]")
{
    auto jit = compileAt("int halvings(float x)\n"
                         "{\n"
                         "    int n = 0;\n"
                         "    while (x > 1)\n"
                         "    {\n"
                         "        x = x * 0.5f;\n"
                         "        n = n + 1;\n"
                         "    }\n"
                         "    return n;\n"
                         "}\n",
                         0);
    REQUIRE(jit);
    auto* halvings = lookup<int(float)>(*jit, "halvings");
    fminus_rt_loop_budget_state& budget = budgetOf(*jit);
    CHECK(budget.left == std::numeric_limits<std::int64_t>::max());
    CHECK(budget.stopped == 0);
    CHECK(halvings(1024.0f) == 10);
    CHECK(budget.left == std::numeric_limits<std::int64_t>::max() - 10);

    // At -O0 a while loop branches back once an iteration: exactly as many as it halves.
    for (const int k : {1, 5, 20})
    {
        INFO("2^" << k);
        const float x = static_cast<float>(1 << k);
        arm(budget, k);
        CHECK(halvings(x) == k);
        CHECK(budget.left == 0);
        CHECK(budget.stopped == 0);
        arm(budget, k - 1);
        CHECK(halvings(x) == 0);
        CHECK(budget.stopped == 1);
    }

    // Not set again, the next call has what the last left: none, so it stops at its first branch back.
    arm(budget, 3);
    CHECK(halvings(16.0f) == 0);
    CHECK(halvings(4.0f) == 0);
    CHECK(budget.stopped == 1);
    arm(budget, 3);
    CHECK(halvings(4.0f) == 2);
}

TEST_CASE("a loop with a count is charged it before it starts: it runs whole or not at all", "[budget]")
{
    // A loop over a block, which the optimiser counts (and vectorises). Charged in its preheader, it either runs all of
    // its iterations or stops before the first, writing nothing. 1024 samples are whole vectors at any width: only the
    // vectorised loop runs, and none of its remainders, each charged where it starts.
    const char* const fill = "void fill(float* restrict out, int n)\n"
                             "{\n"
                             "    for (int i = 0; i < n; ++i)\n"
                             "        out[i] = (float)i + 1.0f;\n"
                             "}\n";
    for (int level = 1; level <= 3; ++level)
    {
        INFO("-O" << level);
        auto jit = compileAt(fill, level);
        REQUIRE(jit);
        auto* f = lookup<void(float*, int)>(*jit, "fill");
        fminus_rt_loop_budget_state& budget = budgetOf(*jit);
        std::vector<float> out(1024, 0.0f);

        arm(budget, 1'000'000);
        f(out.data(), 1024);
        CHECK(budget.stopped == 0);
        CHECK(out[1023] == 1024.0f);
        const std::int64_t taken = 1'000'000 - budget.left;
        CHECK(taken > 0);
        CHECK(taken < 1024);   // a vectorised loop branches back once a vector of samples

        // Exactly enough runs it all again; one less runs none of it.
        std::fill(out.begin(), out.end(), 0.0f);
        arm(budget, taken);
        f(out.data(), 1024);
        CHECK(budget.stopped == 0);
        CHECK(budget.left == 0);
        CHECK(out[1023] == 1024.0f);
        std::fill(out.begin(), out.end(), 0.0f);
        arm(budget, taken - 1);
        f(out.data(), 1024);
        CHECK(budget.stopped == 1);
        CHECK(budget.left == -1);
        CHECK(std::all_of(out.begin(), out.end(), [](float x) { return x == 0.0f; }));
    }
}

TEST_CASE("a stop leaves -1, so that no charge wraps however many calls run out", "[budget]")
{
    // A pointer loop over an empty range written one short, e below a: it never ends, and its count, (e - a - 4) / 4
    // unsigned, is 2^62 - 2, charged in its preheader when optimised. Were a stop to leave its own count, `left` would
    // be near -2^62 after one call, near INT64_MIN after two, and the third's charge would wrap it positive and let the
    // loop write through memory. Each call stops before the loop, writing nothing, and leaves -1.
    const char* const ramp = "void ramp(float* a, float* e)\n"
                             "{\n"
                             "    float x = 1.0f;\n"
                             "    for (float* p = a; p != e; ++p)\n"
                             "    {\n"
                             "        *p = x;\n"
                             "        x = x + 1.0f;\n"
                             "    }\n"
                             "}\n";
    for (int level = 1; level <= 3; ++level)
    {
        INFO("-O" << level);
        auto jit = compileAt(ramp, level);
        REQUIRE(jit);
        auto* f = lookup<void(float*, float*)>(*jit, "ramp");
        fminus_rt_loop_budget_state& budget = budgetOf(*jit);
        std::vector<float> memory(64, 0.0f);
        float* const a = memory.data() + 32;
        arm(budget, 1000);
        f(a, a - 1);
        CHECK(budget.stopped == 1);
        REQUIRE(budget.left == -1);   // charged, not run: no more calls if it is not
        for (int call = 0; call < 4; ++call)
        {
            INFO("call " << call);
            f(a, a - 1);
            CHECK(budget.stopped == 1);
            CHECK(budget.left == -1);
        }
        CHECK(std::all_of(memory.begin(), memory.end(), [](float x) { return x == 0.0f; }));
    }
}

TEST_CASE("a loop whose count may not fit an int64_t counts a branch back at a time", "[budget]")
{
    // IR written by hand (FMinus's pointers are to 4 bytes or more): a loop over bytes, `p != e` with e one below p,
    // whose count is 2^64 - 2, kept scalar. Charged it, the subtraction would add 2 to the budget and the loop would
    // write through memory; it counts its branches back instead, and stops after the budget's, at each level.
    constexpr std::array<llvm::OptimizationLevel, 3> levels{llvm::OptimizationLevel::O1, llvm::OptimizationLevel::O2,
                                                            llvm::OptimizationLevel::O3};
    for (std::size_t level = 0; level < levels.size(); ++level)
    {
        INFO("-O" << level + 1);
        auto context = std::make_unique<llvm::LLVMContext>();
        llvm::SMDiagnostic diagnostic;
        std::unique_ptr<llvm::Module> module = llvm::parseAssemblyString(
            "@fminus_rt_loop_budget = global { i64, i32 } { i64 9223372036854775807, i32 0 }, align 8\n"
            "define void @bytes(ptr %a, ptr %e) {\n"
            "entry:\n"
            "  %empty = icmp eq ptr %a, %e\n"
            "  br i1 %empty, label %done, label %loop\n"
            "loop:\n"
            "  %p = phi ptr [ %a, %entry ], [ %next, %loop ]\n"
            "  %v = phi i8 [ 1, %entry ], [ %v.next, %loop ]\n"
            "  store i8 %v, ptr %p, align 1\n"
            "  %v.next = add i8 %v, 1\n"
            "  %next = getelementptr inbounds i8, ptr %p, i64 1\n"
            "  %more = icmp ne ptr %next, %e\n"
            "  br i1 %more, label %loop, label %done, !llvm.loop !1\n"
            "done:\n"
            "  ret void\n"
            "}\n"
            "!llvm.module.flags = !{!0}\n"
            "!0 = !{i32 7, !\"fminus.loop-budget\", i32 1}\n"
            "!1 = distinct !{!1, !2, !3}\n"
            "!2 = !{!\"llvm.loop.vectorize.enable\", i1 false}\n"
            "!3 = !{!\"llvm.loop.unroll.disable\"}\n",
            diagnostic, *context);
        REQUIRE(module);
        auto jit = llvm::cantFail(fminus::jit::Jit::create(levels[level]));
        llvm::cantFail(jit->add(std::move(module), std::move(context)));
        auto* bytes = lookup<void(std::uint8_t*, std::uint8_t*)>(*jit, "bytes");
        fminus_rt_loop_budget_state& budget = budgetOf(*jit);
        std::vector<std::uint8_t> memory(256, 0);
        std::uint8_t* const a = memory.data() + 16;
        arm(budget, 10);
        bytes(a, a - 1);
        CHECK(budget.stopped == 1);
        CHECK(budget.left == -1);
        for (std::size_t i = 0; i < memory.size(); ++i)
        {
            INFO("byte " << i);
            const std::size_t at = i - 16;   // eleven bytes from a: ten branches back, and the one that ran out
            CHECK(memory[i] == (i >= 16 && at <= 10 ? static_cast<std::uint8_t>(at + 1) : 0));
        }
    }
}

TEST_CASE("a loop's stores to its output are there up to the branch back where it stopped", "[budget]")
{
    // A loop with no count (a float's condition), so it stops at a branch back: as many iterations run as the budget
    // allows branches back, and one more. Its stores go through its argument, in the loop, so each that ran is there.
    const char* const ramp = "void ramp(float* restrict out, float x)\n"
                             "{\n"
                             "    int i = 0;\n"
                             "    while (x > 0)\n"
                             "    {\n"
                             "        out[i] = x;\n"
                             "        x = x - 1.0f;\n"
                             "        i = i + 1;\n"
                             "    }\n"
                             "}\n";
    for (int level = 0; level <= 3; ++level)
    {
        INFO("-O" << level);
        auto jit = compileAt(ramp, level);
        REQUIRE(jit);
        auto* f = lookup<void(float*, float)>(*jit, "ramp");
        fminus_rt_loop_budget_state& budget = budgetOf(*jit);
        std::vector<float> out(100, 0.0f);
        arm(budget, 10);
        f(out.data(), 100.0f);
        CHECK(budget.stopped == 1);
        for (std::size_t i = 0; i < out.size(); ++i)
        {
            INFO("out[" << i << "]");
            CHECK(out[i] == (i <= 10 ? 100.0f - static_cast<float>(i) : 0.0f));
        }
    }
}

TEST_CASE("a stopped call may leave its instance at no point of the source", "[budget]")
{
    // The oscillator's phase wrap of extending.md, recipe 29. Optimised, `phase` lives in a register through the block's
    // loop and is stored after it (LICM), so a call that stops in the wrap of its fifth sample has written four samples
    // and left `phase` as it was before the call: a state the source never passes through, which is why a host
    // re-initialises an instance after a stop. (Should the optimiser stop keeping the field in a register, this test
    // needs another example; the docs' warning stands.)
    const char* const wrap = "module Wrap\n"
                             "{\n"
                             "    float phase;\n"
                             "\n"
                             "    void process(const float* restrict in, float* restrict out, int n)\n"
                             "    {\n"
                             "        for (int i = 0; i < n; ++i)\n"
                             "        {\n"
                             "            phase = phase + in[i];\n"
                             "            while (phase >= 1.0f)\n"
                             "                phase = phase - 1.0f;\n"
                             "            out[i] = phase;\n"
                             "        }\n"
                             "    }\n"
                             "};\n";
    struct Wrap
    {
        float phase = 0.0f;
    };
    std::vector<float> in(8, 0.125f);
    in[4] = std::numeric_limits<float>::infinity();   // infinity less one is infinity: the wrap never ends
    for (int level = 1; level <= 3; ++level)
    {
        INFO("-O" << level);
        auto jit = compileAt(wrap, level);
        REQUIRE(jit);
        auto* process = lookup<void(Wrap*, const float*, float*, int)>(*jit, "Wrap::process");
        fminus_rt_loop_budget_state& budget = budgetOf(*jit);
        Wrap instance;
        std::vector<float> out(8, -1.0f);
        arm(budget, 1000);
        process(&instance, in.data(), out.data(), 8);
        CHECK(budget.stopped == 1);
        CHECK(out[3] == 0.5f);    // four samples written
        CHECK(out[4] == -1.0f);   // and no more
        CHECK(instance.phase == 0.0f);   // but the phase they advanced, never
    }
}

TEST_CASE("a recursion counts each call, and every caller returns at once when one runs out", "[budget]")
{
    // For x > 0 the deepest calls spin. A caller that went on after a call ran out would add its 1, and the sum would
    // be 2^n - 1; each returns 0 at once instead.
    const std::string source = std::string(kSpin)
                               + "int calls(int n, float x)\n"
                                 "{\n"
                                 "    if (n == 0)\n"
                                 "        return (int)spin(x);\n"
                                 "    return calls(n - 1, x) + calls(n - 1, x) + 1;\n"
                                 "}\n"
                                 "int fib(int n)\n"
                                 "{\n"
                                 "    return n < 2 ? n : fib(n - 1) + fib(n - 2);\n"
                                 "}\n";
    for (int level = 0; level <= 3; ++level)
    {
        INFO("-O" << level);
        auto jit = compileAt(source, level);
        REQUIRE(jit);
        auto* calls = lookup<int(int, float)>(*jit, "calls");
        auto* fib = lookup<int(int)>(*jit, "fib");
        fminus_rt_loop_budget_state& budget = budgetOf(*jit);

        arm(budget, 1'000'000);
        CHECK(calls(4, 0.0f) == 15);
        CHECK(budget.stopped == 0);
        arm(budget, 1'000'000);
        CHECK(calls(4, 1.0f) == 0);
        CHECK(budget.stopped == 1);

        arm(budget, 1'000'000);
        CHECK(fib(20) == 6765);
        CHECK(budget.stopped == 0);
        arm(budget, 100);
        CHECK(fib(20) == 0);
        CHECK(budget.stopped == 1);
    }

    // At -O0 each call counts one: fib(10) makes 177.
    auto jit = compileAt(source, 0);
    REQUIRE(jit);
    auto* fib = lookup<int(int)>(*jit, "fib");
    fminus_rt_loop_budget_state& budget = budgetOf(*jit);
    arm(budget, 177);
    CHECK(fib(10) == 55);
    CHECK(budget.left == 0);
    arm(budget, 176);
    CHECK(fib(10) == 0);
    CHECK(budget.stopped == 1);
}

TEST_CASE("within its budget a call computes what it would without the budget, bit for bit", "[budget]")
{
    // Kernels of each kind: a vectorised loop of a math builtin, a recurrence with a float4's coefficients, nested
    // loops (the inner one over taps), and a while loop in a for, at each level.
    const char* const kernels = "void shape(const float* restrict in, float* restrict out, int n)\n"
                                "{\n"
                                "    for (int i = 0; i < n; ++i)\n"
                                "        out[i] = tanhf(3.0f * in[i]) + sinf(in[i]);\n"
                                "}\n"
                                "void filter(const float* restrict in, float* restrict out, int n)\n"
                                "{\n"
                                "    float s1 = 0;\n"
                                "    float s2 = 0;\n"
                                "    for (int i = 0; i < n; ++i)\n"
                                "    {\n"
                                "        const float4 k = (float4){0.2f, 0.4f, -0.3f, 0.1f} * (1.0f + 0.1f * in[i]);\n"
                                "        const float y = k.x * in[i] + s1;\n"
                                "        s1 = k.y * in[i] - k.z * y + s2;\n"
                                "        s2 = k.x * in[i] - k.w * y;\n"
                                "        out[i] = y;\n"
                                "    }\n"
                                "}\n"
                                "void fir(const float* restrict in, float* restrict out, int n)\n"
                                "{\n"
                                "    for (int i = 15; i < n; ++i)\n"
                                "    {\n"
                                "        float s = 0;\n"
                                "        for (int t = 0; t < 16; ++t)\n"
                                "            s = s + in[i - t] * (1.0f / (float)(t + 1));\n"
                                "        out[i] = s;\n"
                                "    }\n"
                                "}\n"
                                "void wrap(const float* restrict in, float* restrict out, int n)\n"
                                "{\n"
                                "    for (int i = 0; i < n; ++i)\n"
                                "    {\n"
                                "        float p = in[i] * 50.0f;\n"
                                "        while (p >= 1.0f)\n"
                                "            p = p - 1.0f;\n"
                                "        while (p < 0.0f)\n"
                                "            p = p + 1.0f;\n"
                                "        out[i] = p;\n"
                                "    }\n"
                                "}\n";
    constexpr int n = 1027;   // no multiple of a vector: every remainder loop runs
    std::vector<float> in(n);
    std::uint32_t seed = 1;
    for (float& x : in)
    {
        seed = seed * 1664525u + 1013904223u;
        x = static_cast<float>(seed >> 8) / 16777216.0f * 2.0f - 1.0f;
    }
    for (int level = 0; level <= 3; ++level)
    {
        auto plain = compileAt(kernels, level, false);
        auto budgeted = compileAt(kernels, level, true);
        REQUIRE(plain);
        REQUIRE(budgeted);
        fminus_rt_loop_budget_state& budget = budgetOf(*budgeted);
        for (const char* name : {"shape", "filter", "fir", "wrap"})
        {
            INFO("-O" << level << " " << name);
            std::vector<float> expected(n, 0.0f);
            std::vector<float> got(n, 0.0f);
            lookup<void(const float*, float*, int)>(*plain, name)(in.data(), expected.data(), n);
            arm(budget, 1'000'000);
            lookup<void(const float*, float*, int)>(*budgeted, name)(in.data(), got.data(), n);
            CHECK(budget.stopped == 0);
            CHECK(std::memcmp(expected.data(), got.data(), sizeof(float) * n) == 0);
        }
    }
}

TEST_CASE("the budget's name is its own while a program has the budget", "[budget]")
{
    const char* const source = "float fminus_rt_loop_budget(float x) { return x; }\n";
    std::string errors;
    CHECK(compileAt(source, 2, true, &errors) == nullptr);
    CHECK(errors == "test.fm:1:7: error: the loop budget reserves 'fminus_rt_loop_budget' for its counter, and this "
                    "program declares a function of that name: rename it\n");
    auto jit = compileAt(source, 2, false, &errors);
    INFO(errors);
    REQUIRE(jit);
    CHECK(lookup<float(float)>(*jit, "fminus_rt_loop_budget")(2.0f) == 2.0f);
}
