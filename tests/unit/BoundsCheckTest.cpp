// tests/unit/BoundsCheckTest.cpp: bounds checks (ir::Options::boundsCheck, ir/Bounds.cpp, opt/BoundsCheck.h), in the
// JIT at each level: an access outside its array stops the call, which records the check and the index, and every
// caller returns early too, writing nothing after the call; a pointer formed may point one past the end; a module's
// field, an element of an array of modules, a poly_load's lanes (a masked one's, only those it takes) and a parallel
// region's body are checked; the first failure is kept; what each check's number means; what the optimiser proves is
// no check; a call within its arrays computes what it would without the checks, bit for bit; and without the option the
// module has nothing of them.
#include "Compile.h"
#include "opt/BoundsCheck.h"
#include "opt/LoopBudget.h"
#include "runtime/Runtime.h"
#include "support/HostTarget.h"

#include <llvm/Support/raw_ostream.h>
#include <llvm/Target/TargetMachine.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace
{
    using fminus::test::lookup;

    constexpr std::array<llvm::OptimizationLevel, 4> kLevels{llvm::OptimizationLevel::O0, llvm::OptimizationLevel::O1,
                                                             llvm::OptimizationLevel::O2, llvm::OptimizationLevel::O3};

    // A program JIT-compiled at -O`level`, with its checks' descriptions.
    struct Checked
    {
        std::unique_ptr<fminus::jit::Jit> jit;
        std::vector<fminus::opt::BoundsSite> sites;

        fminus_rt_bounds_state& record() const
        {
            return *lookup<fminus_rt_bounds_state>(*jit, fminus::opt::kBoundsSymbol);
        }

        // The failed check's message, or empty.
        std::string failed() const
        {
            const fminus_rt_bounds_state& state = record();
            if (state.site <= 0 || static_cast<std::size_t>(state.site) > sites.size())
                return {};
            return fminus::opt::boundsMessage(sites[static_cast<std::size_t>(state.site) - 1], state.index);
        }

        void clear() const
        {
            record() = {0, 0, 0};
        }
    };

    // The program lowered with or without the checks (and the loop budget), or nothing with its errors in `errors`.
    std::unique_ptr<llvm::Module> lowered(std::string_view source, llvm::LLVMContext& context, bool checks = true,
                                          std::string* errors = nullptr, bool budget = false)
    {
        fminus::Diagnostics diagnostics("test.fm");
        const auto program = fminus::frontend::parse(source, diagnostics);
        fminus::ir::Options options;
        options.boundsCheck = checks;
        options.loopBudget = budget;
        std::unique_ptr<llvm::Module> module =
            program ? fminus::ir::lower(*program, context, diagnostics, options) : nullptr;
        if (errors != nullptr)
            *errors = diagnostics.format();
        return module;
    }

    Checked compileAt(std::string_view source, int level, bool checks = true, bool budget = false)
    {
        auto context = std::make_unique<llvm::LLVMContext>();
        std::string errors;
        std::unique_ptr<llvm::Module> module = lowered(source, *context, checks, &errors, budget);
        INFO(errors);
        REQUIRE(module);
        Checked checked;
        checked.sites = fminus::opt::boundsSites(*module);
        checked.jit = llvm::cantFail(fminus::jit::Jit::create(kLevels[static_cast<std::size_t>(level)]));
        llvm::cantFail(checked.jit->add(std::move(module), std::move(context)));
        return checked;
    }

    // The program's IR optimised at -O`level` for this machine, as fminus --emit ll prints it.
    std::string optimisedAt(std::string_view source, int level, bool checks = true)
    {
        llvm::LLVMContext context;
        std::unique_ptr<llvm::Module> module = lowered(source, context, checks);
        REQUIRE(module);
        auto machine = llvm::cantFail(fminus::hostTargetMachine(llvm::CodeGenOptLevel::Default));
        fminus::prepareModule(*module, *machine);
        fminus::optimiseModule(*module, *machine, kLevels[static_cast<std::size_t>(level)]);
        std::string text;
        llvm::raw_string_ostream stream(text);
        module->print(stream, nullptr);
        return text;
    }

    const char* const kPick = "float pick(float i)\n"
                              "{\n"
                              "    float a[8] = {1, 2, 3, 4, 5, 6, 7, 8};\n"
                              "    return a[(int)i];\n"
                              "}\n";
} // namespace

TEST_CASE("an index outside its array stops the call, which records the check and the index, at every level",
          "[bounds]")
{
    for (int level = 0; level <= 3; ++level)
    {
        INFO("-O" << level);
        const Checked checked = compileAt(kPick, level);
        auto* pick = lookup<float(float)>(*checked.jit, "pick");
        REQUIRE(checked.sites.size() == 1);
        CHECK(checked.record().site == 0);

        CHECK(pick(0.0f) == 1.0f);
        CHECK(pick(7.0f) == 8.0f);
        CHECK(checked.record().site == 0);

        CHECK(pick(8.0f) == 0.0f);   // returned early
        CHECK(checked.failed() == "test.fm:4:13: index 8 out of bounds for 'a' of type 'float[8]'");
        checked.clear();
        CHECK(pick(-1.0f) == 0.0f);
        CHECK(checked.failed() == "test.fm:4:13: index -1 out of bounds for 'a' of type 'float[8]'");
        CHECK(checked.record().index == -1);
        checked.clear();
        CHECK(pick(1e9f) == 0.0f);
        CHECK(checked.record().index == 1'000'000'000);
    }
}

TEST_CASE("each check is described where it is: its file, line and column, the array as written, its type and extent",
          "[bounds]")
{
    llvm::LLVMContext context;
    const auto fixed = lowered("module Voice\n"
                               "{\n"
                               "    float taps[4];\n"
                               "    float tap(int k) { return taps[k]; }\n"
                               "};\n"
                               "module Synth\n"
                               "{\n"
                               "    Voice voices[3];\n"
                               "    float play(int v, int k) { return voices[v].tap(k) + voices[v].taps[k]; }\n"
                               "};\n"
                               "float through(Voice* p, int k) { float4 w[2]; return p->taps[k] + w[k].x; }\n"
                               "float formed(int k) { float a[5]; float* e = a + k; return *(a + k) + e[0]; }\n",
                               context);
    REQUIRE(fixed);
    const std::vector<fminus::opt::BoundsSite> sites = fminus::opt::boundsSites(*fixed);
    std::vector<std::string> described;
    for (const fminus::opt::BoundsSite& site : sites)
        described.push_back(fminus::opt::boundsMessage(site, 9));
    // Each at its operator: a subscript's [, a pointer's + and a dereference's *. e[0] is unchecked: e is a variable.
    // *(a + k) checks the pointer formed, then its element.
    CHECK(described == std::vector<std::string>{
                           "test.fm:4:35: index 9 out of bounds for 'taps' of type 'float[4]'",
                           "test.fm:9:45: index 9 out of bounds for 'voices' of type 'Voice[3]'",
                           "test.fm:9:64: index 9 out of bounds for 'voices' of type 'Voice[3]'",
                           "test.fm:9:72: index 9 out of bounds for 'voices[v].taps' of type 'float[4]'",
                           "test.fm:11:61: index 9 out of bounds for 'p->taps' of type 'float[4]'",
                           "test.fm:11:68: index 9 out of bounds for 'w' of type 'float4[2]'",
                           "test.fm:12:48: pointer offset 9 out of bounds for 'a' of type 'float[5]'",
                           "test.fm:12:64: pointer offset 9 out of bounds for 'a' of type 'float[5]'",
                           "test.fm:12:60: index 9 out of bounds for 'a' of type 'float[5]'",
                       });
}

TEST_CASE("a pointer formed may point to any element or one past the last, and no further", "[bounds]")
{
    const char* const formed = "float formed(float i)\n"
                               "{\n"
                               "    float a[4] = {1, 2, 3, 4};\n"
                               "    const float* end = &a[4];\n"
                               "    const float* p = a + (int)i;\n"
                               "    return (float)(end - p);\n"
                               "}\n"
                               "float last(float i)\n"
                               "{\n"
                               "    float a[4] = {1, 2, 3, 4};\n"
                               "    return *(a + 4 - (int)i);\n"
                               "}\n";
    for (int level = 0; level <= 3; ++level)
    {
        INFO("-O" << level);
        const Checked checked = compileAt(formed, level);
        auto* f = lookup<float(float)>(*checked.jit, "formed");
        auto* last = lookup<float(float)>(*checked.jit, "last");
        CHECK(f(0.0f) == 4.0f);
        CHECK(f(4.0f) == 0.0f);
        CHECK(checked.record().site == 0);
        CHECK(f(5.0f) == 0.0f);
        CHECK(checked.failed() == "test.fm:5:24: pointer offset 5 out of bounds for 'a' of type 'float[4]'");
        checked.clear();
        CHECK(f(-1.0f) == 0.0f);
        CHECK(checked.record().index == -1);
        checked.clear();

        CHECK(last(1.0f) == 4.0f);
        CHECK(last(4.0f) == 1.0f);
        CHECK(checked.record().site == 0);
        CHECK(last(0.0f) == 0.0f);   // one past the end, formed, but not an element
        CHECK(checked.failed() == "test.fm:11:12: index 4 out of bounds for 'a' of type 'float[4]'");
        checked.clear();
    }
}

TEST_CASE("a stop returns through every caller, none of which goes on after the call", "[bounds]")
{
    // At -O0 every call stays a call: inner records the check and returns 0, middle sees the record after its call
    // and returns 0, and outer does too, so none of the writes after the calls happens. From -O1 the calls are inlined
    // and the check is a trap to the optimiser, which moves no store past it.
    const char* const nested = "float inner(int i)\n"
                               "{\n"
                               "    float a[4] = {1, 2, 3, 4};\n"
                               "    return a[i];\n"
                               "}\n"
                               "float middle(float* out, int i)\n"
                               "{\n"
                               "    const float x = inner(i);\n"
                               "    out[1] = x;\n"
                               "    return x * 2.0f;\n"
                               "}\n"
                               "void outer(float* out, float i)\n"
                               "{\n"
                               "    out[0] = 5.0f;\n"
                               "    out[2] = middle(out, (int)i);\n"
                               "    out[3] = 7.0f;\n"
                               "}\n";
    for (int level = 0; level <= 3; ++level)
    {
        INFO("-O" << level);
        const Checked checked = compileAt(nested, level);
        auto* outer = lookup<void(float*, float)>(*checked.jit, "outer");
        std::array<float, 4> out{};
        outer(out.data(), 2.0f);
        CHECK(out == std::array<float, 4>{5.0f, 3.0f, 6.0f, 7.0f});
        CHECK(checked.record().site == 0);

        out = {-1.0f, -1.0f, -1.0f, -1.0f};
        outer(out.data(), 4.0f);
        CHECK(checked.failed() == "test.fm:4:13: index 4 out of bounds for 'a' of type 'float[4]'");
        CHECK(out[1] == -1.0f);
        CHECK(out[2] == -1.0f);
        CHECK(out[3] == -1.0f);
        checked.clear();
    }
}

TEST_CASE("the first failed check is kept until the host clears it", "[bounds]")
{
    const char* const two = "float a4(float i) { float a[4]; return a[(int)i]; }\n"
                            "float b2(float i) { float b[2]; return b[(int)i]; }\n";
    for (int level = 0; level <= 3; ++level)
    {
        INFO("-O" << level);
        const Checked checked = compileAt(two, level);
        auto* a4 = lookup<float(float)>(*checked.jit, "a4");
        auto* b2 = lookup<float(float)>(*checked.jit, "b2");
        CHECK(a4(6.0f) == 0.0f);
        CHECK(b2(3.0f) == 0.0f);   // stops too, and records nothing
        CHECK(checked.failed() == "test.fm:1:41: index 6 out of bounds for 'a' of type 'float[4]'");
        checked.clear();
        CHECK(b2(3.0f) == 0.0f);
        CHECK(checked.failed() == "test.fm:2:41: index 3 out of bounds for 'b' of type 'float[2]'");
        checked.clear();
    }

    // While the record holds a check, a call returns at its first call of a function that has checks: at -O0, where
    // the call stays one, middle returns 0 although its own index is fine.
    const char* const calls = "float inner(int i) { float a[4] = {1, 2, 3, 4}; return a[i]; }\n"
                              "float middle(float i) { return inner((int)i) + 1.0f; }\n";
    const Checked checked = compileAt(calls, 0);
    auto* middle = lookup<float(float)>(*checked.jit, "middle");
    CHECK(middle(9.0f) == 0.0f);
    CHECK(middle(1.0f) == 0.0f);
    checked.clear();
    CHECK(middle(1.0f) == 3.0f);
}

TEST_CASE("a module's fields, an array of modules and a ring buffer's index are checked", "[bounds]")
{
    const char* const ring = "module Line\n"
                             "{\n"
                             "    float buf[8];\n"
                             "    int at;\n"
                             "    void write(float x) { buf[at] = x; at = at + 1; }\n"
                             "    float back(int k) { return buf[at - k]; }\n"
                             "};\n"
                             "module Bank\n"
                             "{\n"
                             "    Line lines[2];\n"
                             "    void write(int line, float x) { lines[line].write(x); }\n"
                             "};\n";
    for (int level = 0; level <= 3; ++level)
    {
        INFO("-O" << level);
        const Checked checked = compileAt(ring, level);
        auto* write = lookup<void(void*, int, float)>(*checked.jit, "Bank::write");
        auto* back = lookup<float(void*, int)>(*checked.jit, "Line::back");
        // Bank is two Lines of { float buf[8]; int at; }, 36 bytes each.
        alignas(16) std::array<unsigned char, 72> bank{};
        for (int i = 0; i < 8; ++i)
            write(bank.data(), 1, static_cast<float>(i + 1));
        CHECK(checked.record().site == 0);
        CHECK(back(bank.data() + 36, 1) == 8.0f);
        CHECK(back(bank.data() + 36, 0) == 0.0f);   // buf[8]: past the end
        CHECK(checked.failed() == "test.fm:6:35: index 8 out of bounds for 'buf' of type 'float[8]'");
        checked.clear();

        write(bank.data(), 1, 9.0f);   // the ninth write: buf[8]
        CHECK(checked.failed() == "test.fm:5:30: index 8 out of bounds for 'buf' of type 'float[8]'");
        checked.clear();
        float last = 0.0f;
        std::memcpy(&last, bank.data() + 36 + 32, sizeof last);   // what lies past buf: at, an int, untouched
        CHECK(last != 9.0f);
        write(bank.data(), 2, 1.0f);
        CHECK(checked.failed() == "test.fm:11:42: index 2 out of bounds for 'lines' of type 'Line[2]'");
        checked.clear();
    }
}

TEST_CASE("a 2-D array's every dimension is checked, each against its own extent, named in the message", "[bounds]")
{
    const char* const grid = "module Bank\n"
                             "{\n"
                             "    float gain[4][16];\n"
                             "    float at(int v, int i) { return gain[v][i]; }\n"
                             "    void set(int v, int i, float x) { gain[v][i] = x; }\n"
                             "    float sum(int v) { float s = 0; for (int i = 0; i < 16; ++i) s += gain[v][i];"
                             " return s; }\n"
                             "};\n"
                             "float row(float v, float i)\n"
                             "{\n"
                             "    const float t[2][3] = {{1, 2, 3}, {4, 5, 6}};\n"
                             "    const float* r = t[(int)v];\n"   // a row, decayed: checked as an element of t
                             "    return *(t[1] + (int)i);\n"      // a pointer formed from a row: its own extent
                             "}\n";
    for (int level = 0; level <= 3; ++level)
    {
        INFO("-O" << level);
        const Checked checked = compileAt(grid, level);
        auto* at = lookup<float(void*, int, int)>(*checked.jit, "Bank::at");
        auto* set = lookup<void(void*, int, int, float)>(*checked.jit, "Bank::set");
        auto* sum = lookup<float(void*, int)>(*checked.jit, "Bank::sum");
        auto* row = lookup<float(float, float)>(*checked.jit, "row");
        alignas(16) std::array<float, 64 + 4> bank{};   // the bank, and what lies past it
        set(bank.data(), 3, 15, 2.0f);
        set(bank.data(), 1, 0, 3.0f);
        CHECK(checked.record().site == 0);
        CHECK(bank[3 * 16 + 15] == 2.0f);
        CHECK(at(bank.data(), 1, 0) == 3.0f);
        CHECK(sum(bank.data(), 3) == 2.0f);

        CHECK(at(bank.data(), 4, 0) == 0.0f);   // the rows: 4 of them
        CHECK(checked.failed() == "test.fm:4:41: index 4 out of bounds for 'gain' of type 'float[4][16]'");
        checked.clear();
        CHECK(at(bank.data(), 0, 16) == 0.0f);   // a row's elements: 16, though row 1 follows in memory
        CHECK(checked.failed() == "test.fm:4:44: index 16 out of bounds for 'gain[v]' of type 'float[16]'");
        checked.clear();
        set(bank.data(), 2, -1, 9.0f);
        CHECK(checked.failed() == "test.fm:5:46: index -1 out of bounds for 'gain[v]' of type 'float[16]'");
        CHECK(checked.record().index == -1);
        checked.clear();
        CHECK(bank[2 * 16 - 1] == 0.0f);   // the store never happened
        set(bank.data(), 4, 0, 9.0f);
        CHECK(checked.failed() == "test.fm:5:43: index 4 out of bounds for 'gain' of type 'float[4][16]'");
        checked.clear();
        CHECK(bank[64] == 0.0f);
        CHECK(sum(bank.data(), -1) == 0.0f);
        CHECK(checked.failed() == "test.fm:6:75: index -1 out of bounds for 'gain' of type 'float[4][16]'");
        checked.clear();

        CHECK(row(1.0f, 2.0f) == 6.0f);
        CHECK(checked.record().site == 0);
        CHECK(row(2.0f, 0.0f) == 0.0f);   // t[2], a row past the last, even to make a pointer of it
        CHECK(checked.failed() == "test.fm:11:23: index 2 out of bounds for 't' of type 'float[2][3]'");
        checked.clear();
        CHECK(row(0.0f, 4.0f) == 0.0f);   // t[1] + 4: past the row's end, one past is 3
        CHECK(checked.failed() == "test.fm:12:19: pointer offset 4 out of bounds for 't[1]' of type 'float[3]'");
        checked.clear();
    }
}

TEST_CASE("a method called through a pointer into an array of modules is called on one of its modules", "[bounds]")
{
    // (voices + n)->bump() and (&voices[n])->bump() may form voices + 2, one past the end, but not call bump on it,
    // which would write guard: the call is checked as an element, as (voices + n)->level is. Each check of a call is
    // at the method's name, where the call's errors are.
    const char* const bank = "module Voice\n"
                             "{\n"
                             "    float level;\n"
                             "    void bump() { level = level + 1.0f; }\n"
                             "};\n"
                             "module Bank\n"
                             "{\n"
                             "    Voice voices[2];\n"
                             "    float guard;\n"
                             "    void viaArrow(int n) { (voices + n)->bump(); }\n"
                             "    void viaAddress(int n) { (&voices[n])->bump(); }\n"
                             "    void viaField(int n) { (voices + n)->level = 1.0f; }\n"
                             "    void viaElement(int n) { voices[n].bump(); }\n"
                             "};\n";
    for (int level = 0; level <= 3; ++level)
    {
        INFO("-O" << level);
        const Checked checked = compileAt(bank, level);
        // Bank is { Voice voices[2]; float guard; }, three floats.
        const char* const names[] = {"Bank::viaArrow", "Bank::viaAddress", "Bank::viaField", "Bank::viaElement"};
        const char* const expected[] = {
            "test.fm:10:42: index 2 out of bounds for 'voices' of type 'Voice[2]'",
            "test.fm:11:44: index 2 out of bounds for 'voices' of type 'Voice[2]'",
            "test.fm:12:40: index 2 out of bounds for 'voices' of type 'Voice[2]'",
            "test.fm:13:36: index 2 out of bounds for 'voices' of type 'Voice[2]'",
        };
        for (std::size_t way = 0; way < std::size(names); ++way)
        {
            INFO(names[way]);
            auto* call = lookup<void(float*, int)>(*checked.jit, names[way]);
            std::array<float, 3> instance{};
            call(instance.data(), 1);
            CHECK(instance == std::array<float, 3>{0.0f, 1.0f, 0.0f});
            CHECK(checked.record().site == 0);
            call(instance.data(), 2);   // voices[2] would be guard
            CHECK(instance == std::array<float, 3>{0.0f, 1.0f, 0.0f});
            CHECK(checked.failed() == expected[way]);
            CHECK(checked.record().index == 2);
            checked.clear();
        }
        // Further than one past the end, the pointer formed fails first.
        auto* viaArrow = lookup<void(float*, int)>(*checked.jit, "Bank::viaArrow");
        std::array<float, 3> instance{};
        viaArrow(instance.data(), 3);
        CHECK(checked.failed() == "test.fm:10:36: pointer offset 3 out of bounds for 'voices' of type 'Voice[2]'");
        checked.clear();
    }
}

TEST_CASE("a poly_load or poly_store checks every lane it reaches, or with a mask each lane the mask takes",
          "[bounds]")
{
    const char* const lanes = "float load(float i)\n"
                              "{\n"
                              "    float a[8] = {1, 2, 3, 4, 5, 6, 7, 8};\n"
                              "    return reduce_add(poly_load(a + (int)i));\n"
                              "}\n"
                              "float masked(float i, float n)\n"
                              "{\n"
                              "    float a[8] = {1, 2, 3, 4, 5, 6, 7, 8};\n"
                              "    return reduce_add(poly_load(a + (int)i, poly_index() < (int)n));\n"
                              "}\n"
                              "float store(float i)\n"
                              "{\n"
                              "    float a[8];\n"
                              "    poly_store(a + (int)i, (poly float)(1.0f));\n"
                              "    return a[0];\n"
                              "}\n";
    for (int level = 0; level <= 3; ++level)
    {
        INFO("-O" << level);
        const Checked checked = compileAt(lanes, level);   // this machine's lanes: 4 (NEON, SSE) or more
        auto* load = lookup<float(float)>(*checked.jit, "load");
        auto* masked = lookup<float(float, float)>(*checked.jit, "masked");
        auto* store = lookup<float(float)>(*checked.jit, "store");
        int width = 0;   // the poly_load's
        for (const fminus::opt::BoundsSite& site : checked.sites)
            width = std::max(width, site.width);
        REQUIRE(width >= 4);
        if (width > 8)
            continue;   // every unmasked poly_load of 8 floats fails
        CHECK(load(static_cast<float>(8 - width)) > 0.0f);
        CHECK(checked.record().site == 0);
        CHECK(load(static_cast<float>(9 - width)) == 0.0f);
        CHECK(checked.failed() == "test.fm:4:23: " + std::to_string(width) + " elements from index "
                                      + std::to_string(9 - width) + " out of bounds for 'a' of type 'float[8]'");
        checked.clear();

        CHECK(masked(6.0f, 2.0f) == 15.0f);   // lanes 6 and 7
        CHECK(masked(7.0f, 0.0f) == 0.0f);    // no lane taken
        CHECK(checked.record().site == 0);
        CHECK(masked(6.0f, 3.0f) == 0.0f);   // lane 2 is a[8]
        CHECK(checked.failed() == "test.fm:9:23: index 8 out of bounds for 'a' of type 'float[8]'");
        checked.clear();
        CHECK(masked(-2.0f, 4.0f) == 0.0f);   // the first lane outside is a[-2]
        CHECK(checked.record().index == -2);
        checked.clear();

        CHECK(store(0.0f) == 1.0f);
        CHECK(store(static_cast<float>(9 - width)) == 0.0f);
        CHECK(checked.record().index == 9 - width);
        checked.clear();
    }
}

TEST_CASE("a parallel region's failed check stops its part and the call", "[bounds]")
{
    const char* const region = "void spread(float* restrict out, float shift)\n"
                               "{\n"
                               "    float a[16];\n"
                               "    parallel for (int k = 0; k < 16; ++k)\n"
                               "        out[k] = a[k + (int)shift] + 1.0f;\n"
                               "    out[16] = 2.0f;\n"
                               "}\n";
    struct Pool   // the runtime's, for the region's parts
    {
        Pool()
        {
            fminus_rt_parallel_start();
        }
        ~Pool()
        {
            fminus_rt_parallel_stop();
        }
        Pool(const Pool&) = delete;
        Pool& operator=(const Pool&) = delete;
    };
    const Pool pool;
    for (int level = 0; level <= 3; ++level)
    {
        INFO("-O" << level);
        const Checked checked = compileAt(region, level);
        auto* spread = lookup<void(float*, float)>(*checked.jit, "spread");
        std::vector<float> out(17, 0.0f);
        spread(out.data(), 0.0f);
        CHECK(checked.record().site == 0);
        CHECK(out[15] == 1.0f);
        CHECK(out[16] == 2.0f);

        std::fill(out.begin(), out.end(), -1.0f);
        spread(out.data(), 4.0f);
        CHECK(checked.record().site != 0);
        CHECK(checked.record().index >= 16);   // whichever part failed first: k + 4 for k from 12
        CHECK(checked.record().index < 20);
        CHECK(out[16] == -1.0f);   // the code after the region never ran
        checked.clear();
    }
}

TEST_CASE("an index the optimiser proves within its array is no check", "[bounds]")
{
    // A for over the whole array, its condition the extent: from -O1 the check goes, and with it the record's use.
    const char* const whole = "float total(void)\n"
                              "{\n"
                              "    float a[16];\n"
                              "    for (int i = 0; i < 16; ++i)\n"
                              "        a[i] = (float)i;\n"
                              "    float s = 0;\n"
                              "    for (int i = 0; i < 16; ++i)\n"
                              "        s += a[i];\n"
                              "    return s;\n"
                              "}\n"
                              "float third(void) { float a[4] = {1, 2, 3, 4}; return a[2]; }\n";
    CHECK(optimisedAt(whole, 0).find("fminus.bounds.stop") == std::string::npos);   // the pass made each a return
    CHECK(optimisedAt(whole, 0).find("cmpxchg") != std::string::npos);
    for (int level = 1; level <= 3; ++level)
    {
        INFO("-O" << level);
        const std::string ir = optimisedAt(whole, level);
        CHECK(ir.find("cmpxchg") == std::string::npos);
        CHECK(ir.find("bounds.unwind") == std::string::npos);
    }
}

TEST_CASE("within its arrays a call computes what it would without the checks, bit for bit", "[bounds]")
{
    // A comb filter on a ring buffer of a module, its index wrapped by a compare, which the optimiser cannot prove
    // within the buffer: the check stays in the loop.
    const char* const comb = "module Comb\n"
                             "{\n"
                             "    float line[37];\n"
                             "    int at;\n"
                             "    void process(const float* restrict in, float* restrict out, int n)\n"
                             "    {\n"
                             "        for (int i = 0; i < n; ++i)\n"
                             "        {\n"
                             "            const float y = line[at];\n"
                             "            line[at] = in[i] + 0.7f * tanhf(y);\n"
                             "            out[i] = 0.5f * (in[i] + y);\n"
                             "            at = at + 1 == 37 ? 0 : at + 1;\n"
                             "        }\n"
                             "    }\n"
                             "};\n";
    std::vector<float> in(4096);
    for (std::size_t i = 0; i < in.size(); ++i)
        in[i] = std::sin(0.01f * static_cast<float>(i)) + (i % 97 == 0 ? 1.0f : 0.0f);
    for (int level = 0; level <= 3; ++level)
    {
        INFO("-O" << level);
        std::array<std::vector<float>, 2> outs;
        for (const bool checks : {false, true})
        {
            const Checked checked = compileAt(comb, level, checks);
            auto* process = lookup<void(void*, const float*, float*, int)>(*checked.jit, "Comb::process");
            alignas(16) std::array<unsigned char, 37 * 4 + 4> state{};
            std::vector<float>& out = outs[checks ? 1 : 0];
            out.assign(in.size(), 0.0f);
            for (std::size_t at = 0; at < in.size(); at += 256)
                process(state.data(), in.data() + at, out.data() + at, 256);
            if (checks)
                CHECK(checked.record().site == 0);
        }
        CHECK(std::memcmp(outs[0].data(), outs[1].data(), outs[0].size() * sizeof(float)) == 0);
    }
}

TEST_CASE("the loop budget and the bounds checks stop a call each in its own way, together", "[bounds]")
{
    // walk's loop has no count (a float's condition): it counts a branch back at a time, and its fifth iteration
    // reads a[4]. spin never ends for x > 0.
    const char* const both = "float walk(float n)\n"
                             "{\n"
                             "    float a[4];\n"
                             "    float s = 0;\n"
                             "    while (s < n)\n"
                             "        s += 1.0f + a[(int)s];\n"
                             "    return s;\n"
                             "}\n"
                             "float spin(float x)\n"
                             "{\n"
                             "    float a[2];\n"
                             "    while (x > 0)\n"
                             "        x = x * 1.0f + a[(int)x & 1];\n"
                             "    return x;\n"
                             "}\n";
    for (int level = 0; level <= 3; ++level)
    {
        INFO("-O" << level);
        const Checked checked = compileAt(both, level, true, true);
        auto* walk = lookup<float(float)>(*checked.jit, "walk");
        auto* spin = lookup<float(float)>(*checked.jit, "spin");
        auto& budget = *lookup<fminus_rt_loop_budget_state>(*checked.jit, fminus::opt::kLoopBudgetSymbol);
        budget = {1000, 0};
        CHECK(walk(3.0f) == 3.0f);
        CHECK(checked.record().site == 0);
        CHECK(budget.stopped == 0);

        budget = {1000, 0};
        CHECK(walk(6.0f) == 0.0f);   // a[4]
        CHECK(checked.failed() == "test.fm:6:22: index 4 out of bounds for 'a' of type 'float[4]'");
        CHECK(budget.stopped == 0);
        // What the loop took before the stop is written back: its four branches back at -O0, and from -O1 what is
        // left of them once the optimiser has rotated the loop or unrolled it.
        CHECK(budget.left <= 1000);
        CHECK(budget.left >= 996);
        if (level == 0)
            CHECK(budget.left == 996);
        checked.clear();

        budget = {100, 0};
        CHECK(spin(1.0f) == 0.0f);   // out of budget
        CHECK(budget.stopped == 1);
        CHECK(checked.record().site == 0);
    }
}

TEST_CASE("without the option, a module has nothing of the checks", "[bounds]")
{
    llvm::LLVMContext context;
    const auto module = lowered(kPick, context, false);
    REQUIRE(module);
    CHECK(module->getNamedGlobal(fminus::opt::kBoundsSymbol) == nullptr);
    CHECK(module->getFunction(fminus::opt::kBoundsStopFunction) == nullptr);
    CHECK(module->getNamedMetadata(fminus::opt::kBoundsSitesMetadata) == nullptr);
    CHECK(module->getModuleFlag(fminus::opt::kBoundsCheckFlag) == nullptr);
    for (int level = 0; level <= 3; ++level)
    {
        const std::string ir = optimisedAt(kPick, level, false);
        CHECK(ir.find("fminus.bounds") == std::string::npos);
        CHECK(ir.find("fminus_rt_bounds") == std::string::npos);
        CHECK(ir.find("fminus-bounds") == std::string::npos);
    }
}

TEST_CASE("a constant index outside its array fails when it runs, and changes no error", "[bounds]")
{
    const char* const fixed = "float five(float x)\n"
                              "{\n"
                              "    float a[4];\n"
                              "    if (x > 0)\n"
                              "        return a[5];\n"
                              "    return x;\n"
                              "}\n"
                              "const float table[4] = {1, 2, 3, 4};\n"
                              "const float* const past = &table[6];\n";
    llvm::LLVMContext context;
    std::string errors;
    // A constant of the top level is no code that runs: its pointer is not checked (nor is it in C, where it is
    // undefined), and the program compiles as it does without the checks.
    CHECK(lowered(fixed, context, true, &errors) != nullptr);
    CHECK(errors.empty());
    std::string unchecked;

    const char* const runs = "float five(float x)\n"
                             "{\n"
                             "    float a[4];\n"
                             "    if (x > 0)\n"
                             "        return a[5];\n"
                             "    return x;\n"
                             "}\n";
    for (int level = 0; level <= 3; ++level)
    {
        INFO("-O" << level);
        const Checked checked = compileAt(runs, level);
        auto* five = lookup<float(float)>(*checked.jit, "five");
        CHECK(five(-1.0f) == -1.0f);
        CHECK(checked.record().site == 0);
        CHECK(five(1.0f) == 0.0f);
        CHECK(checked.failed() == "test.fm:5:17: index 5 out of bounds for 'a' of type 'float[4]'");
    }

    // A non-void function that can reach its end is an error with the checks too, a constant's failing check
    // before it notwithstanding.
    const char* const end = "float missing(float x)\n"
                            "{\n"
                            "    float a[4];\n"
                            "    a[5] = x;\n"
                            "}\n";
    CHECK(lowered(end, context, true, &errors) == nullptr);
    CHECK(lowered(end, context, false, &unchecked) == nullptr);
    CHECK(errors == unchecked);
    CHECK(errors == "test.fm:1:7: error: non-void function 'missing' does not return a value\n");
}

TEST_CASE("the checks reserve their record's name", "[bounds]")
{
    llvm::LLVMContext context;
    std::string errors;
    const char* const named = "float fminus_rt_bounds(float x) { return x; }\n";
    CHECK(lowered(named, context, true, &errors) == nullptr);
    CHECK(errors == "test.fm:1:7: error: the bounds checks reserve 'fminus_rt_bounds' for their record, and this "
                    "program declares a function of that name: rename it\n");
    CHECK(lowered(named, context, false, &errors) != nullptr);
}
