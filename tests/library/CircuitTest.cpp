// tests/library/CircuitTest.cpp: fminus::circuit (include/fminus/Circuit.h) as a host uses it: a netlist generated,
// compiled and run; a broken one's errors; #include of a .cir through the host's include resolver, the playground's
// path; and the self-test as a host runs it: the provisional module first, the test on the host's threads, its answer
// stored as text, read back and generated with. C++17, against FMinus::FMinus alone.
#include "fminus/Circuit.h"

#include "fminus/FMinus.h"

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstddef>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

#if defined(_WIN32)
#include <malloc.h>
#endif

namespace
{
    // A diode clipper: R1 into C1, an antiparallel pair across it (example 1 without its tone stage).
    const char* const kClipper = "* clipper.cir\n"
                                 ".title Clipper\n"
                                 ".input u in\n"
                                 ".output y a\n"
                                 "R1 in a 2.2k\n"
                                 "C1 a 0 10n\n"
                                 "D1 a 0 DM\n"
                                 "D2 0 a DM\n"
                                 ".model DM D(IS=2.52n N=1.752)\n";

    struct Free
    {
        void operator()(void* p) const
        {
#if defined(_WIN32)
            _aligned_free(p);
#else
            std::free(p);
#endif
        }
    };

    std::unique_ptr<void, Free> instance(const fminus::Program::Module& module)
    {
        const std::size_t alignment = std::max(module.alignment, alignof(std::max_align_t));
        const std::size_t size = (module.size + alignment - 1) / alignment * alignment;
#if defined(_WIN32)
        void* memory = _aligned_malloc(size, alignment);
#else
        void* memory = std::aligned_alloc(alignment, size);
#endif
        std::memset(memory, 0, module.size);
        return std::unique_ptr<void, Free>(memory);
    }

    // The clipper's DC point: v + R1 2 Is sinh(v / N Vt) = u, solved in double (by bisection).
    double exactRoot(double u)
    {
        const double nvt = 1.752 * 1.380649e-23 * 300.15 / 1.602176634e-19;
        double lo = 0;
        double hi = u;
        for (int i = 0; i < 200; ++i)
        {
            const double m = 0.5 * (lo + hi);
            if (m + 2200 * 2 * 2.52e-9 * std::sinh(m / nvt) > u)
                hi = m;
            else
                lo = m;
        }
        return lo;
    }

    // The output after `n` host samples of `level` volts, through the module's tick.
    float settle(fminus::Program& program, const std::string& module, float level, int n)
    {
        std::string errors;
        const fminus::Program::Module* m = program.module(module);
        REQUIRE(m != nullptr);
        auto self = instance(*m);
        auto* init = program.function<void(void*, float)>(module + "::init", errors);
        auto* tick = program.function<float(void*, float)>(module + "::tick", errors);
        REQUIRE(init != nullptr);
        REQUIRE(tick != nullptr);
        init(self.get(), 48000);
        float y = 0;
        for (int i = 0; i < n; ++i)
            y = tick(self.get(), level);
        return y;
    }
} // namespace

TEST_CASE("a netlist becomes a module a host runs", "[circuit]")
{
    const fminus::circuit::Result result = fminus::circuit::generate(kClipper, "clipper.cir");
    INFO(result.errors);
    REQUIRE(result.errors.empty());
    CHECK(result.warnings.empty());
    CHECK(result.module == "Clipper");
    CHECK(result.pots.empty());
    CHECK(result.notes.find("clipper.cir: note: block 1 (R1, C1, D1, D2)") != std::string::npos);
    std::string errors;
    auto program = fminus::Program::compile(result.source, errors);
    INFO(errors);
    REQUIRE(program);
    CHECK(program->nonBlocking("Clipper::tick"));
    // at 1 V DC, the DC point (omega3 + 1 Newton step lands 2.5e-5 V above it there)
    const float y = settle(*program, "Clipper", 1.0f, 12000);
    CHECK(std::fabs(static_cast<double>(y) - exactRoot(1.0)) < 1e-4);
}

TEST_CASE("a broken netlist gives no source and clang-style errors", "[circuit]")
{
    const fminus::circuit::Result result =
        fminus::circuit::generate(".input u in\n.output y a\nR1 in a 2.2k\nD1 a 0 DX\n.model DM D\n", "pedal.cir");
    CHECK(result.source.empty());
    CHECK(result.module.empty());
    CHECK(result.errors ==
          "pedal.cir:4:8: error: unknown model 'DX' (the models: DM; built in: 1N914, 1N4148)\n");
    fminus::circuit::Options options;
    options.oversample = 3;
    CHECK(fminus::circuit::generate(kClipper, "clipper.cir", options).errors ==
          "clipper.cir:1:1: error: Options::oversample is 3; it is 1, 2, 4 or 8\n");
}

TEST_CASE("an include resolver generates .cir files", "[circuit]")
{
    // The host's files, in memory; a name ending in .cir is a netlist, whose text is the module generate() writes,
    // named <file>.fm so that a second #include of it reads nothing.
    const std::map<std::string, std::string, std::less<>> files{{"clipper.cir", kClipper}};
    std::string circuitErrors;
    fminus::Program::Options options;
    options.fileName = "play.fm";
    options.include = [&](std::string_view name, std::string_view) -> std::optional<fminus::Program::IncludedFile>
    {
        const auto found = files.find(name);
        if (found == files.end())
            return std::nullopt;
        if (name.size() > 4 && name.substr(name.size() - 4) == ".cir")
        {
            const fminus::circuit::Result r = fminus::circuit::generate(found->second, name);
            circuitErrors += r.errors;
            return fminus::Program::IncludedFile{std::string(name) + ".fm", r.source};
        }
        return fminus::Program::IncludedFile{found->first, found->second};
    };
    const std::string program = "#include \"clipper.cir\"\n"
                                "#include \"clipper.cir\"\n"
                                "float dc(float level)\n"
                                "{\n"
                                "    Clipper c;\n"
                                "    c.init(48000);\n"
                                "    float y = 0;\n"
                                "    for (int i = 0; i < 12000; ++i)\n"
                                "        y = c.tick(level);\n"
                                "    return y;\n"
                                "}\n";
    std::string errors;
    auto compiled = fminus::Program::compile(program, errors, options);
    INFO(errors);
    REQUIRE(compiled);
    CHECK(circuitErrors.empty());
    auto* dc = compiled->function<float(float)>("dc", errors);
    REQUIRE(dc != nullptr);
    CHECK(std::fabs(static_cast<double>(dc(1.0f)) - exactRoot(1.0)) < 1e-4);
}

TEST_CASE("a host runs the self-test and generates with its answer", "[circuit]")
{
    // 1. generate: no configuration directive, so a provisional module, and the key its answer will go by
    const fminus::circuit::Result first = fminus::circuit::generate(kClipper, "clipper.cir");
    INFO(first.errors);
    REQUIRE(first.errors.empty());
    CHECK(first.provisional);
    CHECK(first.selfTestKey.size() == 16);
    CHECK(first.selfTestKey == fminus::circuit::selfTestKey(kClipper, "clipper.cir", {}));
    CHECK(first.notes.find("clipper.cir: note: the self-test chooses this circuit's configuration; provisional: 8x "
                           "IIR, omega(2)") != std::string::npos);
    // 2. the provisional module plays meanwhile
    std::string errors;
    auto provisional = fminus::Program::compile(first.source, errors);
    INFO(errors);
    REQUIRE(provisional);
    CHECK(std::isfinite(settle(*provisional, "Clipper", 1.0f, 4800)));
    // 3. the self-test on the host's threads: two here
    fminus::circuit::SelfTestOptions test;
    test.parallel = [](int count, const std::function<void(int)>& job)
    {
        std::atomic<int> next{0};
        auto work = [&]
        {
            for (int i = next++; i < count; i = next++)
                job(i);
        };
        std::thread other(work);
        work();
        other.join();
    };
    const fminus::circuit::SelfTestResult tested = fminus::circuit::selfTest(kClipper, "clipper.cir", {}, test);
    INFO(tested.errors);
    INFO(tested.notes);
    REQUIRE(tested.errors.empty());
    REQUIRE(tested.choice);
    CHECK(!tested.cancelled);
    CHECK(tested.key == first.selfTestKey);
    CHECK(tested.candidates > 0);
    // 4. the host stores the answer as text, under the key
    const std::string stored = tested.choice->text();
    CHECK(stored.find("fminus-circuit-choice 1 key=" + first.selfTestKey + " ") == 0);
    // 5. reads it back, and generates with it: the self-test's configuration, no longer provisional
    std::string why;
    fminus::circuit::Options options;
    options.chosen = fminus::circuit::Choice::parse(stored, &why);
    INFO(why);
    REQUIRE(options.chosen);
    const fminus::circuit::Result second = fminus::circuit::generate(kClipper, "clipper.cir", options);
    INFO(second.errors);
    REQUIRE(second.errors.empty());
    CHECK(!second.provisional);
    CHECK(second.configuration.find("(the self-test " + first.selfTestKey + ")") != std::string::npos);
    auto program = fminus::Program::compile(second.source, errors);
    INFO(errors);
    REQUIRE(program);
    CHECK(program->nonBlocking("Clipper::tick"));
    const float y = settle(*program, "Clipper", 1.0f, 12000);
    CHECK(std::isfinite(y));
    CHECK(std::fabs(static_cast<double>(y) - exactRoot(1.0)) < 1e-3);
}
