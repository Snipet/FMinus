// tests/unit/LoopBudgetTest.cpp: the loop budget (ir::Options::loopBudget, opt/LoopBudget.h), in the JIT at each level:
// a call that runs out returns early with the flag set, and so does every caller; what a loop without a count, a loop
// with one and a recursion take of it; what the program wrote, kept as it was where it stopped; and a call within its
// budget computing what it would without the budget, bit for bit.
#include "Compile.h"
#include "opt/LoopBudget.h"
#include "runtime/Runtime.h"

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
        CHECK(std::all_of(out.begin(), out.end(), [](float x) { return x == 0.0f; }));
    }
}

TEST_CASE("a call that stops leaves what it wrote as it was at that branch back", "[budget]")
{
    // A loop with no count (a float's condition), so it stops at a branch back: as many iterations run as the budget
    // allows branches back, and one more.
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
