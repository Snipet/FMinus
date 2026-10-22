// tests/unit/VersionTest.cpp: knob versioning (src/opt/KnobVersioning.h) and flat streams. A method whose loop
// branches on an int or bool knob, or on whether a stream is flat, gets a copy of its body for each value; each must
// compute what the method computes without the copies, bit for bit, for every value in the knob's domain and for an int
// outside it. The budget caps the copies; a knob that steers no loop, a float, a wide int and a frozen parameter are
// left alone; a versioned method stays nonblocking; the IR stays valid at every level. A stream whose pointer is null
// reads the slot's value, at -O0 too, and a stream with samples reads them as before, bit for bit. (Bit for bit with
// math calls too, since the tests compile in the default, vector math mode: a math call is the runtime's function,
// which gives a float the same result in a copy's vectorised loop as in the method's scalar one, and is never folded.
// Only the fast mode could move a math call's last bits; KnobVersioning.h.)
#include "Compile.h"

#include "fminus/FMinus.h"
#include "opt/KnobVersioning.h"
#include "support/HostTarget.h"

#include <catch2/catch_test_macros.hpp>

#include <llvm/Analysis/ValueTracking.h>
#include <llvm/AsmParser/Parser.h>
#include <llvm/IR/Function.h>
#include <llvm/IR/InstIterator.h>
#include <llvm/IR/Instructions.h>
#include <llvm/IR/LLVMContext.h>
#include <llvm/IR/Module.h>
#include <llvm/IR/IntrinsicInst.h>
#include <llvm/IR/Verifier.h>
#include <llvm/Passes/PassBuilder.h>
#include <llvm/Support/SourceMgr.h>
#include <llvm/Support/raw_ostream.h>
#include <llvm/Target/TargetMachine.h>

#include <array>
#include <bit>
#include <climits>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <iterator>
#include <map>
#include <memory>
#include <set>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

using namespace fminus::test;
using Kind = fminus::ir::ParamBinding::Kind;

namespace
{
    // A parameter's slot, as a C++ host declares it: the stream's pointer, then the value.
    template <typename T>
    struct Slot
    {
        const float* stream = nullptr;
        T value{};
    };

    // A shaper whose mode picks the work a sample: a gain, a soft clip or a hard clip; bypassed, the input. Its peak is
    // a field, kept through every copy.
    constexpr const char* kShape = "module Shape\n"
                                   "{\n"
                                   "    param int mode [[fminus::range(0, 2)]];\n"
                                   "    param bool bypass;\n"
                                   "    param float drive;\n"
                                   "    float peak;\n"
                                   "\n"
                                   "    void process(const float* restrict in, float* restrict out, int n)\n"
                                   "        [[clang::nonblocking]]\n"
                                   "    {\n"
                                   "        for (int i = 0; i < n; ++i)\n"
                                   "        {\n"
                                   "            const float x = drive[i] * in[i];\n"
                                   "            float y = in[i];\n"
                                   "            if (!bypass[i])\n"
                                   "                y = mode[i] == 0 ? x : mode[i] == 1 ? x / (1 + fabsf(x))\n"
                                   "                                                    : fminf(fmaxf(x, -1), 1);\n"
                                   "            out[i] = y;\n"
                                   "            peak = fmaxf(peak, fabsf(y));\n"
                                   "        }\n"
                                   "    }\n"
                                   "};\n";

    struct Shape
    {
        Slot<int> mode;
        Slot<bool> bypass;
        Slot<float> drive;
        float peak = 0.0f;
    };

    constexpr float kDrive = 1.5f;

    // Shape::process's output for one sample, in C++, one rounding at a time (clang would fuse a multiply and an add
    // written as one expression; FMinus never does).
    float shaped(int mode, bool bypass, float in)
    {
        if (bypass)
            return in;
        const float x = kDrive * in;
        if (mode == 0)
            return x;
        if (mode == 1)
        {
            const float denominator = 1.0f + std::fabs(x);
            return x / denominator;
        }
        return std::fmin(std::fmax(x, -1.0f), 1.0f);
    }

    fminus::ir::Options versions(unsigned knobVersions, std::map<std::string, fminus::ir::ParamBinding> parameters = {})
    {
        fminus::ir::Options options;
        options.knobVersions = knobVersions;
        options.parameters = std::move(parameters);
        return options;
    }

    // The program JIT-compiled at `level` (Compile.h's compile() is -O2's).
    std::unique_ptr<fminus::jit::Jit> compileAt(std::string_view source, const fminus::ir::Options& options,
                                                llvm::OptimizationLevel level)
    {
        fminus::Diagnostics diagnostics("test.fm");
        const auto program = fminus::frontend::parse(source, diagnostics);
        auto context = std::make_unique<llvm::LLVMContext>();
        std::unique_ptr<llvm::Module> module =
            program ? fminus::ir::lower(*program, *context, diagnostics, options) : nullptr;
        INFO(diagnostics.format());
        REQUIRE(module);
        auto jit = llvm::cantFail(fminus::jit::Jit::create(level));
        llvm::cantFail(jit->add(std::move(module), std::move(context)));
        return jit;
    }

    // The program lowered and optimised for this machine, as the JIT does it: to read what the pass made.
    std::unique_ptr<llvm::Module> optimised(std::string_view source, const fminus::ir::Options& options,
                                            llvm::LLVMContext& context,
                                            llvm::OptimizationLevel level = llvm::OptimizationLevel::O2)
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

    // How many copies of its body the pass gave `name`: its "fminus-knob-versions", or 1.
    int copiesOf(const llvm::Module& module, const char* name)
    {
        const llvm::Function* function = module.getFunction(name);
        REQUIRE(function != nullptr);
        if (!function->hasFnAttribute(fminus::opt::kVersionsAttribute))
            return 1;
        return std::stoi(function->getFnAttribute(fminus::opt::kVersionsAttribute).getValueAsString().str());
    }

    // The functions `name` calls, by name (intrinsics included).
    std::set<std::string> calledBy(const llvm::Module& module, const char* name)
    {
        std::set<std::string> callees;
        for (const llvm::Instruction& instruction : llvm::instructions(*module.getFunction(name)))
        {
            // An annotation is not a call: it compiles to nothing. (The loop unroller adds an llvm.assume where it
            // unrolls at run time, as it does on x86-64.)
            if (const auto* intrinsic = llvm::dyn_cast<llvm::IntrinsicInst>(&instruction);
                intrinsic != nullptr && intrinsic->isAssumeLikeIntrinsic())
                continue;
            if (const auto* call = llvm::dyn_cast<llvm::CallBase>(&instruction))
                callees.insert(call->getCalledOperand()->getName().str());
        }
        return callees;
    }

    // Runs the pass alone on `function`, and says whether the function is still valid IR.
    bool versionAlone(llvm::Function& function)
    {
        {
            // Declared in this order so they are destroyed in the reverse one (HostTarget.cpp's optimiseModule).
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
            pipeline.addPass(fminus::opt::KnobVersioningPass());
            pipeline.run(function, functions);
        }
        std::string problems;
        llvm::raw_string_ostream os(problems);
        const bool broken = llvm::verifyFunction(function, &os);
        INFO(problems);
        CHECK_FALSE(broken);
        return !broken;
    }

    // Hand-written IR, parsed.
    std::unique_ptr<llvm::Module> parsed(const char* ir, llvm::LLVMContext& context)
    {
        llvm::SMDiagnostic diagnostic;
        std::unique_ptr<llvm::Module> module = llvm::parseAssemblyString(ir, diagnostic, context);
        INFO(diagnostic.getMessage().str());
        REQUIRE(module);
        return module;
    }

    // The streams whose samples `name` loads four at a time, in their alias scope, by parameter: each such load's
    // address, less its offsets, is a stream's pointer (%a.stream) or the base its reads select (%d.base, %d.base.i
    // once inlined), named after the parameter.
    std::set<std::string> vectorStreams(const llvm::Module& module, const char* name)
    {
        std::set<std::string> streams;
        for (const llvm::Instruction& instruction : llvm::instructions(*module.getFunction(name)))
        {
            const auto* load = llvm::dyn_cast<llvm::LoadInst>(&instruction);
            if (load == nullptr || !load->getType()->isVectorTy()
                || !load->hasMetadata(llvm::LLVMContext::MD_alias_scope))
                continue;
            const llvm::Value* base = load->getPointerOperand();
            while (const auto* address = llvm::dyn_cast<llvm::GetElementPtrInst>(base))
                base = address->getPointerOperand();
            const std::string named = base->getName().str();
            streams.insert(named.substr(0, named.find('.')));
        }
        return streams;
    }

    // The bits of each float, so that a comparison is exact (-0 is not 0).
    std::vector<std::uint32_t> bits(const std::vector<float>& values)
    {
        std::vector<std::uint32_t> out;
        for (const float value : values)
            out.push_back(std::bit_cast<std::uint32_t>(value));
        return out;
    }

    // An input that reaches every branch of the shaper: signed zeros, tiny and huge values, both signs.
    std::vector<float> testSignal()
    {
        std::vector<float> in{0.0f, -0.0f, 1e-30f, -1e-30f, 1e30f, -1e30f, 0.5f, -0.5f, 1.0f, -1.0f};
        for (int i = 0; i < 70; ++i)
            in.push_back(static_cast<float>(i % 23) * 0.37f - 4.0f);
        return in;
    }

    // Shape::process over `in` with its knobs at these values: the output, then the peak the field kept.
    std::vector<float> runShape(fminus::jit::Jit& jit, int mode, bool bypass, const std::vector<float>& in)
    {
        Shape shape;
        shape.mode.value = mode;
        shape.bypass.value = bypass;
        shape.drive.value = kDrive;
        std::vector<float> out(in.size());
        lookup<void(Shape*, const float*, float*, int)>(jit, "Shape::process")(&shape, in.data(), out.data(),
                                                                               static_cast<int>(in.size()));
        out.push_back(shape.peak);
        return out;
    }

    // The same, in C++.
    std::vector<float> shapedCpp(int mode, bool bypass, const std::vector<float>& in)
    {
        std::vector<float> out;
        float peak = 0.0f;
        for (const float x : in)
        {
            out.push_back(shaped(mode, bypass, x));
            peak = std::fmax(peak, std::fabs(out.back()));
        }
        out.push_back(peak);
        return out;
    }
} // namespace

TEST_CASE("a method versioned on its knobs computes what it computes unversioned, bit for bit", "[codegen][versions]")
{
    llvm::LLVMContext context;
    CHECK(copiesOf(*optimised(kShape, versions(8), context), "Shape::process") == 6);   // mode's 3 times bypass's 2

    std::string errors;
    auto versioned = compile(kShape, &errors, versions(8));
    auto plain = compile(kShape, &errors, versions(0));
    INFO(errors);
    REQUIRE(versioned);
    REQUIRE(plain);
    const std::vector<float> in = testSignal();
    for (const int mode : {0, 1, 2})
    {
        for (const bool bypass : {false, true})
        {
            INFO("mode " << mode << ", bypass " << bypass);
            const std::vector<float> expected = runShape(*plain, mode, bypass, in);
            CHECK(bits(runShape(*versioned, mode, bypass, in)) == bits(expected));
            CHECK(bits(expected) == bits(shapedCpp(mode, bypass, in)));
        }
    }
}

TEST_CASE("an int knob outside its fminus::range runs the method's original body", "[codegen][versions]")
{
    std::string errors;
    auto versioned = compile(kShape, &errors, versions(8));
    auto plain = compile(kShape, &errors, versions(0));
    INFO(errors);
    REQUIRE(versioned);
    REQUIRE(plain);
    const std::vector<float> in = testSignal();
    for (const int mode : {-1, 3, 7, INT_MIN, INT_MAX})
    {
        for (const bool bypass : {false, true})
        {
            INFO("mode " << mode << ", bypass " << bypass);
            CHECK(bits(runShape(*versioned, mode, bypass, in)) == bits(runShape(*plain, mode, bypass, in)));
            CHECK(bits(runShape(*versioned, mode, bypass, in)) == bits(shapedCpp(mode, bypass, in)));
        }
    }
}

TEST_CASE("the budget caps a method's copies: its knobs in declaration order, while their product fits",
          "[codegen][versions]")
{
    // Three ints over 0..3 are 64 combinations; under a budget of 8, only the first is versioned on, 4 copies. A bool
    // declared after them still fits beside it (4 times 2).
    const std::string loop = "    void process(const float* restrict in, float* restrict out, int n)\n"
                             "    {\n"
                             "        for (int i = 0; i < n; ++i)\n"
                             "        {\n"
                             "            float x = in[i];\n"
                             "            if (a[i] == 1)\n"
                             "                x = x * 2;\n"
                             "            if (b[i] == 2)\n"
                             "                x = x + 1;\n"
                             "            if (c[i] == 3)\n"
                             "                x = -x;\n"
                             "            out[i] = LAST;\n"
                             "        }\n"
                             "    }\n";
    const std::string knobs = "    param int a [[fminus::range(0, 3)]];\n"
                              "    param int b [[fminus::range(0, 3)]];\n"
                              "    param int c [[fminus::range(0, 3)]];\n";
    std::string three = loop;
    three.replace(three.find("LAST"), 4, "x");
    std::string four = loop;
    four.replace(four.find("LAST"), 4, "d[i] ? x * 0.5f : x");
    const std::string source = "module Three\n{\n" + knobs + three + "};\n" + "module Four\n{\n" + knobs
                             + "    param bool d;\n" + four + "};\n";

    const std::map<unsigned, std::pair<int, int>> expected{
        {0, {1, 1}},   {1, {1, 1}},   {3, {1, 2}},    {4, {4, 4}},
        {8, {4, 8}},   {16, {16, 16}}, {64, {64, 64}}, {200, {64, 128}},
    };
    for (const auto& [budget, copies] : expected)
    {
        INFO("budget " << budget);
        llvm::LLVMContext context;
        const auto module = optimised(source, versions(budget), context);
        CHECK(copiesOf(*module, "Three::process") == copies.first);
        CHECK(copiesOf(*module, "Four::process") == copies.second);
    }

    // And the copies compute what the method does, a's value outside its range too.
    struct Four
    {
        Slot<int> a, b, c;
        Slot<bool> d;
    };
    std::string errors;
    auto versioned = compile(source, &errors, versions(8));
    auto plain = compile(source, &errors, versions(0));
    INFO(errors);
    REQUIRE(versioned);
    REQUIRE(plain);
    const std::vector<float> in = testSignal();
    const auto run = [&](fminus::jit::Jit& jit, const Four& knobsAt) {
        Four instance = knobsAt;
        std::vector<float> out(in.size());
        lookup<void(Four*, const float*, float*, int)>(jit, "Four::process")(&instance, in.data(), out.data(),
                                                                             static_cast<int>(in.size()));
        return out;
    };
    for (const int a : {0, 1, 3, 5})
    {
        for (const int c : {0, 3})
        {
            for (const bool d : {false, true})
            {
                Four at;
                at.a.value = a;
                at.b.value = 2;
                at.c.value = c;
                at.d.value = d;
                INFO("a " << a << ", c " << c << ", d " << d);
                CHECK(bits(run(*versioned, at)) == bits(run(*plain, at)));
            }
        }
    }
}

TEST_CASE("a stream's test for null is chosen before the knobs, so its loop vectorises as it did",
          "[codegen][versions][param]")
{
    // The mode, declared first, would take the whole budget (8 values), and leave each copy reading the gain under a
    // branch, which keeps the loop from vectorising. The flat test goes first: 2 copies, and the mode only once 16 fit.
    const char* source = "module Level\n"
                         "{\n"
                         "    param int mode [[fminus::range(0, 7)]];\n"
                         "    param float gain;\n"
                         "    void process(const float* restrict in, float* restrict out, int n)\n"
                         "    {\n"
                         "        for (int i = 0; i < n; ++i)\n"
                         "            out[i] = (mode[i] == 3 ? -in[i] : in[i]) * gain[i];\n"
                         "    }\n"
                         "};\n";
    const auto stream = std::map<std::string, fminus::ir::ParamBinding>{{"Level::gain", {Kind::Stream, 0.0}}};
    for (const auto& [budget, copies] : std::map<unsigned, int>{{8, 2}, {16, 16}})
    {
        INFO("budget " << budget);
        llvm::LLVMContext context;
        const auto module = optimised(source, versions(budget, stream), context);
        CHECK(copiesOf(*module, "Level::process") == copies);
        bool vectorLoad = false;   // a vector of the stream's samples, in their alias scope
        for (const llvm::Instruction& instruction : llvm::instructions(*module->getFunction("Level::process")))
        {
            vectorLoad = vectorLoad
                      || (llvm::isa<llvm::LoadInst>(instruction) && instruction.getType()->isVectorTy()
                          && instruction.hasMetadata(llvm::LLVMContext::MD_alias_scope));
        }
        CHECK(vectorLoad);
    }
}

TEST_CASE("a knob that steers no loop is not versioned, nor a float, a wide int or one without a range",
          "[codegen][versions]")
{
    const char* source = "module Plain\n"
                         "{\n"
                         "    param int mode [[fminus::range(0, 2)]];\n"
                         "    param int wide [[fminus::range(0, 15)]];\n"
                         "    param int odd [[fminus::range(0, 2.5)]];\n"
                         "    param int open;\n"
                         "    param float level;\n"
                         "\n"
                         "    void outside(const float* restrict in, float* restrict out, int n)\n"
                         "    {\n"
                         "        const float g = mode[0] == 1 ? 2 : 1;   // decided once a call, before the loop\n"
                         "        for (int i = 0; i < n; ++i)\n"
                         "            out[i] = g * in[i];\n"
                         "    }\n"
                         "\n"
                         "    void data(const float* restrict in, float* restrict out, int n)\n"
                         "    {\n"
                         "        for (int i = 0; i < n; ++i)\n"
                         "            out[i] = in[i] * mode[i];   // a value in the loop, not a decision\n"
                         "    }\n"
                         "\n"
                         "    void untagged(const float* restrict in, float* restrict out, int n)\n"
                         "    {\n"
                         "        for (int i = 0; i < n; ++i)\n"
                         "            out[i] = wide[i] == 3 || odd[i] == 1 || open[i] == 2 || level[i] > 1 ? in[i]\n"
                         "                                                                                : -in[i];\n"
                         "    }\n"
                         "\n"
                         "    float tick(float x)   // no loop\n"
                         "    {\n"
                         "        return mode[0] == 2 ? x : -x;\n"
                         "    }\n"
                         "};\n";
    llvm::LLVMContext context;
    const auto module = optimised(source, versions(8), context);
    for (const char* method : {"Plain::outside", "Plain::data", "Plain::untagged", "Plain::tick"})
    {
        INFO(method);
        CHECK(copiesOf(*module, method) == 1);
    }

    // They still compute what they say.
    struct Plain
    {
        Slot<int> mode, wide, odd, open;
        Slot<float> level;
    } plain;
    plain.mode.value = 1;
    plain.wide.value = 3;
    std::string errors;
    auto jit = compile(source, &errors, versions(8));
    INFO(errors);
    REQUIRE(jit);
    const std::array<float, 4> in{1.0f, -2.0f, 3.0f, -4.0f};
    std::array<float, 4> out{};
    lookup<void(Plain*, const float*, float*, int)>(*jit, "Plain::outside")(&plain, in.data(), out.data(), 4);
    CHECK(out == std::array<float, 4>{2.0f, -4.0f, 6.0f, -8.0f});
    lookup<void(Plain*, const float*, float*, int)>(*jit, "Plain::untagged")(&plain, in.data(), out.data(), 4);
    CHECK(out == in);
    CHECK(lookup<float(Plain*, float)>(*jit, "Plain::tick")(&plain, 5.0f) == -5.0f);
}

TEST_CASE("a stream read only before the loop is not versioned on, and leaves the budget to the knob that steers it",
          "[codegen][versions][param]")
{
    // The streams' reads select on their tests where the method starts, outside the loop, which uses what they read:
    // a value, not where the loop's loads are. So only the mode steers the loop: 3 copies. Counted as steering, the
    // streams, chosen first, would take 4 of the 8 and leave no room for the mode's 3.
    const char* source = "module Before\n"
                         "{\n"
                         "    param float a;\n"
                         "    param float b;\n"
                         "    param int mode [[fminus::range(0, 2)]];\n"
                         "    float y;\n"
                         "\n"
                         "    void process(const float* restrict in, float* restrict out, int n)\n"
                         "    {\n"
                         "        const float ga = a[0];\n"
                         "        const float gb = b[0];\n"
                         "        for (int i = 0; i < n; ++i)\n"
                         "        {\n"
                         "            const float x = in[i] * ga + gb;\n"
                         "            if (mode[i] == 0)\n"
                         "                y = y * 0.5f + x;\n"
                         "            else if (mode[i] == 1)\n"
                         "                y = x - y;\n"
                         "            else\n"
                         "                y = y + x * x;\n"
                         "            out[i] = y;\n"
                         "        }\n"
                         "    }\n"
                         "};\n";
    const auto streams = std::map<std::string, fminus::ir::ParamBinding>{{"Before::a", {Kind::Stream, 0.0}},
                                                                         {"Before::b", {Kind::Stream, 0.0}}};
    llvm::LLVMContext context;
    CHECK(copiesOf(*optimised(source, versions(8, streams), context), "Before::process") == 3);

    // Flat or with samples, each copy reads what the slot says, as the unversioned method does.
    struct Before
    {
        Slot<float> a, b;
        Slot<int> mode;
        float y = 0.0f;
    };
    std::string errors;
    auto versioned = compile(source, &errors, versions(8, streams));
    auto plain = compile(source, &errors, versions(0, streams));
    INFO(errors);
    REQUIRE(versioned);
    REQUIRE(plain);
    const std::array<float, 4> in{1.0f, -2.0f, 3.0f, -4.0f};
    const std::array<float, 1> samples{-1.5f};
    for (const bool flat : {true, false})
    {
        for (const int mode : {0, 1, 2})
        {
            INFO((flat ? "flat, mode " : "samples, mode ") << mode);
            std::array<std::array<float, 4>, 2> out{};
            for (std::size_t which = 0; which < 2; ++which)
            {
                Before before;
                before.a.value = 0.5f;
                before.b.value = 0.25f;
                before.a.stream = flat ? nullptr : samples.data();
                before.mode.value = mode;
                auto& jit = which == 0 ? *versioned : *plain;
                const auto process = lookup<void(Before*, const float*, float*, int)>(jit, "Before::process");
                process(&before, in.data(), out[which].data(), 4);
            }
            CHECK(out[0] == out[1]);
        }
    }
}

TEST_CASE("a frozen parameter is not versioned: it is a constant already", "[codegen][versions]")
{
    const auto frozen = versions(8, {{"Shape::mode", {Kind::Frozen, 1.0}}, {"Shape::bypass", {Kind::Frozen, 0.0}}});
    llvm::LLVMContext context;
    CHECK(copiesOf(*optimised(kShape, frozen, context), "Shape::process") == 1);
    std::string errors;
    auto jit = compile(kShape, &errors, frozen);
    auto knobs = compile(kShape, &errors, versions(0));
    INFO(errors);
    REQUIRE(jit);
    REQUIRE(knobs);
    const std::vector<float> in = testSignal();
    CHECK(bits(runShape(*jit, 2, true, in)) == bits(runShape(*knobs, 1, false, in)));   // the slots are not read
}

TEST_CASE("a versioned method stays nonblocking, and calls nothing the unversioned one does not",
          "[codegen][versions]")
{
    // A biquad whose coefficients a mode picks, with sinf and cosf a sample: what LLVM's own unswitching (-O3 only,
    // to a cost of 50) leaves in the loop.
    const char* source = "module Filter\n"
                         "{\n"
                         "    param float cutoff;\n"
                         "    param bool highpass;\n"
                         "    float s1;\n"
                         "    float s2;\n"
                         "\n"
                         "    void process(const float* restrict in, float* restrict out, int n)\n"
                         "        [[clang::nonblocking]]\n"
                         "    {\n"
                         "        for (int i = 0; i < n; ++i)\n"
                         "        {\n"
                         "            const float w = 6.2831853f * cutoff[i] / 48000;\n"
                         "            const float c = cosf(w);\n"
                         "            const float alpha = sinf(w) / 1.4142f;\n"
                         "            const float a0 = 1 + alpha;\n"
                         "            const float b0 = highpass[i] ? (1 + c) / 2 / a0 : (1 - c) / 2 / a0;\n"
                         "            const float b1 = highpass[i] ? -(1 + c) / a0 : (1 - c) / a0;\n"
                         "            const float y = b0 * in[i] + s1;\n"
                         "            s1 = b1 * in[i] + 2 * c / a0 * y + s2;\n"
                         "            s2 = b0 * in[i] - (1 - alpha) / a0 * y;\n"
                         "            out[i] = y;\n"
                         "        }\n"
                         "    }\n"
                         "};\n";
    for (const unsigned budget : {8u, 0u})
    {
        fminus::Program::Options options;
        options.knobVersions = budget;
        options.parameters["Filter::cutoff"] = fminus::Program::Binding::stream();
        std::string errors;
        const auto program = fminus::Program::compile(source, errors, options);
        INFO(errors);
        REQUIRE(program);
        CHECK(program->nonBlocking("Filter::process"));
    }
    const auto stream = versions(8, {{"Filter::cutoff", {Kind::Stream, 0.0}}});
    llvm::LLVMContext context;
    const auto versioned = optimised(source, stream, context);
    const auto plain = optimised(source, versions(0, {{"Filter::cutoff", {Kind::Stream, 0.0}}}), context);
    CHECK(copiesOf(*versioned, "Filter::process") == 4);   // the cutoff flat or not, times highpass
    const std::set<std::string> callees = calledBy(*versioned, "Filter::process");
    const std::set<std::string> before = calledBy(*plain, "Filter::process");
    for (const std::string& callee : callees)
    {
        INFO(callee);
        CHECK(before.contains(callee));
    }
}

TEST_CASE("versioned IR is valid at every level, with a local array and a knob that bounds a loop",
          "[codegen][versions]")
{
    // An FIR whose length is a knob: each copy's inner loop has a constant trip count. The weights are a local
    // array, whose stack slot must stay in the entry block.
    const char* source = "module Fir\n"
                         "{\n"
                         "    param int taps [[fminus::range(1, 4)]];\n"
                         "    float history[4];\n"
                         "\n"
                         "    void process(const float* restrict in, float* restrict out, int n)\n"
                         "    {\n"
                         "        float weights[4] = {0.4f, 0.3f, 0.2f, 0.1f};\n"
                         "        for (int i = 0; i < n; ++i)\n"
                         "        {\n"
                         "            for (int k = 3; k > 0; --k)\n"
                         "                history[k] = history[k - 1];\n"
                         "            history[0] = in[i];\n"
                         "            float acc = 0;\n"
                         "            for (int k = 0; k < taps[i]; ++k)\n"
                         "                acc += weights[k] * history[k];\n"
                         "            out[i] = acc;\n"
                         "        }\n"
                         "    }\n"
                         "};\n";
    for (const llvm::OptimizationLevel level :
         {llvm::OptimizationLevel::O1, llvm::OptimizationLevel::O2, llvm::OptimizationLevel::O3})
    {
        llvm::LLVMContext context;
        const auto module = optimised(source, versions(8), context, level);
        std::string problems;
        llvm::raw_string_ostream os(problems);
        const bool broken = llvm::verifyModule(*module, &os);
        INFO(problems);
        CHECK_FALSE(broken);
        CHECK(copiesOf(*module, "Fir::process") == 4);
        for (const llvm::Instruction& instruction : llvm::instructions(*module->getFunction("Fir::process")))
        {
            if (llvm::isa<llvm::AllocaInst>(instruction))
                CHECK(instruction.getParent()->isEntryBlock());
        }
    }
    {
        llvm::LLVMContext context;
        CHECK(copiesOf(*optimised(source, versions(8), context, llvm::OptimizationLevel::O0), "Fir::process") == 1);
    }

    struct Fir
    {
        Slot<int> taps;
        std::array<float, 4> history{};
    };
    std::string errors;
    auto versioned = compile(source, &errors, versions(8));
    auto plain = compile(source, &errors, versions(0));
    INFO(errors);
    REQUIRE(versioned);
    REQUIRE(plain);
    const std::vector<float> in = testSignal();
    const auto run = [&](fminus::jit::Jit& jit, int taps) {
        Fir fir;
        fir.taps.value = taps;
        std::vector<float> out(in.size());
        lookup<void(Fir*, const float*, float*, int)>(jit, "Fir::process")(&fir, in.data(), out.data(),
                                                                           static_cast<int>(in.size()));
        return out;
    };
    for (const int taps : {-1, 0, 1, 2, 3, 4})
    {
        INFO("taps " << taps);
        CHECK(bits(run(*versioned, taps)) == bits(run(*plain, taps)));
    }
}

namespace
{
    // A gain whose gain may be a stream, and a read of it outside a loop.
    constexpr const char* kGain = "module Gain\n"
                                  "{\n"
                                  "    param float gain;\n"
                                  "    param bool on;\n"
                                  "\n"
                                  "    void process(const float* restrict in, float* restrict out, int n)\n"
                                  "        [[clang::nonblocking]]\n"
                                  "    {\n"
                                  "        for (int i = 0; i < n; ++i)\n"
                                  "            out[i] = on[i] ? in[i] * gain[i] : in[i];\n"
                                  "    }\n"
                                  "\n"
                                  "    float at(int i) { return gain[i]; }\n"
                                  "};\n";

    struct Gain
    {
        Slot<float> gain;
        Slot<bool> on;
    };

    // A biquad low-pass whose coefficients are computed a sample from its cutoff (a stream): a sine, a cosine and
    // six divides, which a flat cutoff takes out of the loop.
    constexpr const char* kBiquad = "module Biquad\n"
                                    "{\n"
                                    "    param float cutoff;\n"
                                    "    float s1;\n"
                                    "    float s2;\n"
                                    "\n"
                                    "    void process(const float* restrict in, float* restrict out, int n)\n"
                                    "        [[clang::nonblocking]]\n"
                                    "    {\n"
                                    "        for (int i = 0; i < n; ++i)\n"
                                    "        {\n"
                                    "            const float w = 6.2831853f * cutoff[i] / 48000;\n"
                                    "            const float c = cosf(w);\n"
                                    "            const float alpha = sinf(w) / 1.4142f;\n"
                                    "            const float a0 = 1 + alpha;\n"
                                    "            const float b0 = (1 - c) / 2 / a0;\n"
                                    "            const float b1 = (1 - c) / a0;\n"
                                    "            const float a1 = -2 * c / a0;\n"
                                    "            const float a2 = (1 - alpha) / a0;\n"
                                    "            const float x = in[i];\n"
                                    "            const float y = b0 * x + s1;\n"
                                    "            s1 = b1 * x - a1 * y + s2;\n"
                                    "            s2 = b0 * x - a2 * y;\n"
                                    "            out[i] = y;\n"
                                    "        }\n"
                                    "    }\n"
                                    "};\n";

    struct Biquad
    {
        Slot<float> cutoff;
        float s1 = 0.0f;
        float s2 = 0.0f;
    };

    // A one-pole whose coefficient is a stream, its state a field (ParamTest.cpp's): no math function in it, so the C++
    // below is an oracle in any build. (A biquad's sinf and cosf would be one only through fminus::math: C++'s std::sin
    // and std::cos are libm's, which the vector mode's are not, and from -O1 clang makes the two one call to
    // __sincosf_stret, whose last bits differ from libm's sinf and cosf.)
    constexpr const char* kSmooth = "module Smooth\n"
                                    "{\n"
                                    "    param float k;\n"
                                    "    float y;\n"
                                    "    void process(const float* restrict in, float* restrict out, int n)\n"
                                    "        [[clang::nonblocking]]\n"
                                    "    {\n"
                                    "        for (int i = 0; i < n; ++i)\n"
                                    "            out[i] = y += k[i] * (in[i] - y);\n"
                                    "    }\n"
                                    "};\n";

    struct Smooth
    {
        Slot<float> k;
        float y = 0.0f;
    };

    // Smooth::process in C++, one rounding at a time, its coefficient a sample each; the state last.
    std::vector<float> smoothCpp(const std::vector<float>& in, const std::vector<float>& k)
    {
        float y = 0.0f;
        std::vector<float> out;
        for (std::size_t i = 0; i < in.size(); ++i)
        {
            const float step = k[i] * (in[i] - y);
            y = y + step;
            out.push_back(y);
        }
        out.push_back(y);
        return out;
    }

    std::vector<float> runSmooth(fminus::jit::Jit& jit, const std::vector<float>& in, const float* stream, float value)
    {
        Smooth smooth;
        smooth.k.stream = stream;
        smooth.k.value = value;
        std::vector<float> out(in.size());
        lookup<void(Smooth*, const float*, float*, int)>(jit, "Smooth::process")(&smooth, in.data(), out.data(),
                                                                                 static_cast<int>(in.size()));
        out.push_back(smooth.y);
        return out;
    }

    std::vector<float> runBiquad(fminus::jit::Jit& jit, const std::vector<float>& in, const float* stream, float value)
    {
        Biquad biquad;
        biquad.cutoff.stream = stream;
        biquad.cutoff.value = value;
        std::vector<float> out(in.size());
        lookup<void(Biquad*, const float*, float*, int)>(jit, "Biquad::process")(&biquad, in.data(), out.data(),
                                                                                 static_cast<int>(in.size()));
        out.push_back(biquad.s1);
        out.push_back(biquad.s2);
        return out;
    }
} // namespace

TEST_CASE("a stream whose pointer is null reads the slot's value, at every level", "[codegen][versions][param]")
{
    const auto stream = std::map<std::string, fminus::ir::ParamBinding>{{"Gain::gain", {Kind::Stream, 0.0}}};
    const std::array<float, 4> in{1.0f, 2.0f, 3.0f, 4.0f};
    const std::array<float, 4> ramp{0.0f, 0.25f, 0.5f, 0.75f};
    for (const llvm::OptimizationLevel level : {llvm::OptimizationLevel::O0, llvm::OptimizationLevel::O2})
    {
        for (const unsigned budget : {8u, 0u})
        {
            INFO("budget " << budget << (level == llvm::OptimizationLevel::O0 ? ", -O0" : ", -O2"));
            auto jit = compileAt(kGain, versions(budget, stream), level);
            Gain gain;
            gain.gain.value = 0.5f;
            gain.on.value = true;
            std::array<float, 4> out{};
            const auto process = lookup<void(Gain*, const float*, float*, int)>(*jit, "Gain::process");
            process(&gain, in.data(), out.data(), 4);
            CHECK(out == std::array<float, 4>{0.5f, 1.0f, 1.5f, 2.0f});
            CHECK(lookup<float(Gain*, int)>(*jit, "Gain::at")(&gain, 3) == 0.5f);

            // The same instance with samples, then flat again: a call reads what its pointer says.
            gain.gain.stream = ramp.data();
            process(&gain, in.data(), out.data(), 4);
            CHECK(out == std::array<float, 4>{0.0f, 0.5f, 1.5f, 3.0f});
            CHECK(lookup<float(Gain*, int)>(*jit, "Gain::at")(&gain, 3) == 0.75f);
            gain.gain.stream = nullptr;
            gain.gain.value = 2.0f;
            process(&gain, in.data(), out.data(), 4);
            CHECK(out == std::array<float, 4>{2.0f, 4.0f, 6.0f, 8.0f});
            gain.on.value = false;
            process(&gain, in.data(), out.data(), 4);
            CHECK(out == in);
        }
    }
}

TEST_CASE("a stream with samples reads them as before, bit for bit, and a flat one reads its value as samples",
          "[codegen][versions][param]")
{
    std::vector<float> in;
    std::vector<float> sweep;
    std::vector<float> k;
    for (int i = 0; i < 256; ++i)
    {
        in.push_back(i % 16 < 8 ? 0.8f : -0.8f);
        sweep.push_back(200.0f + 30.0f * static_cast<float>(i));
        k.push_back(0.01f + 0.003f * static_cast<float>(i));
    }
    for (const unsigned budget : {8u, 0u})
    {
        INFO("budget " << budget);
        std::string errors;
        auto smooth = compile(kSmooth, &errors, versions(budget, {{"Smooth::k", {Kind::Stream, 0.0}}}));
        INFO(errors);
        REQUIRE(smooth);
        CHECK(bits(runSmooth(*smooth, in, k.data(), 5.0f)) == bits(smoothCpp(in, k)));   // the value is not read
        const std::vector<float> still(in.size(), 0.125f);
        CHECK(bits(runSmooth(*smooth, in, nullptr, 0.125f)) == bits(smoothCpp(in, still)));
    }

    // The biquad: its flat copy computes its coefficients once, before the loop, and gives what the method gives a
    // stream of the value; its copy for samples is the method as it was.
    const std::vector<float> flat(in.size(), 1000.0f);
    const auto stream = std::map<std::string, fminus::ir::ParamBinding>{{"Biquad::cutoff", {Kind::Stream, 0.0}}};
    std::string errors;
    auto versioned = compile(kBiquad, &errors, versions(8, stream));
    auto plain = compile(kBiquad, &errors, versions(0, stream));
    INFO(errors);
    REQUIRE(versioned);
    REQUIRE(plain);
    llvm::LLVMContext context;
    CHECK(copiesOf(*optimised(kBiquad, versions(8, stream), context), "Biquad::process") == 2);
    CHECK(bits(runBiquad(*versioned, in, sweep.data(), 5.0f)) == bits(runBiquad(*plain, in, sweep.data(), 5.0f)));
    const std::vector<float> still = runBiquad(*plain, in, flat.data(), 0.0f);
    CHECK(bits(runBiquad(*versioned, in, flat.data(), 0.0f)) == bits(still));
    CHECK(bits(runBiquad(*versioned, in, nullptr, 1000.0f)) == bits(still));
    CHECK(bits(runBiquad(*plain, in, nullptr, 1000.0f)) == bits(still));
}

namespace
{
    // IR written by hand, as FMinus's code generator never writes it: the scope declared inside the loop, as after
    // inlining, and a loop ID. An int over {0, 1}: two copies, and the original body for any other value. No module
    // flag: the default budget.
    constexpr const char* kScopedIr = "define void @f(ptr %this, ptr noalias %out, i32 %n) {\n"
                                      "entry:\n"
                                      "  %mode = load atomic i32, ptr %this unordered, align 4, !fminus.domain !0\n"
                                      "  br label %loop\n"
                                      "loop:\n"
                                      "  %i = phi i32 [ 0, %entry ], [ %next, %latch ]\n"
                                      "  call void @llvm.experimental.noalias.scope.decl(metadata !1)\n"
                                      "  %one = icmp eq i32 %mode, 1\n"
                                      "  br i1 %one, label %then, label %else\n"
                                      "then:\n"
                                      "  store float 1.0, ptr %out, align 4, !alias.scope !1\n"
                                      "  br label %latch\n"
                                      "else:\n"
                                      "  store float 2.0, ptr %out, align 4, !alias.scope !1\n"
                                      "  br label %latch\n"
                                      "latch:\n"
                                      "  %next = add i32 %i, 1\n"
                                      "  %done = icmp eq i32 %next, %n\n"
                                      "  br i1 %done, label %exit, label %loop, !llvm.loop !4\n"
                                      "exit:\n"
                                      "  ret void\n"
                                      "}\n"
                                      "declare void @llvm.experimental.noalias.scope.decl(metadata)\n"
                                      "!0 = !{i32 0, i32 1}\n"
                                      "!1 = !{!2}\n"
                                      "!2 = distinct !{!2, !3, !\"scope\"}\n"
                                      "!3 = distinct !{!3, !\"domain\"}\n"
                                      "!4 = distinct !{!4, !5}\n"
                                      "!5 = !{!\"llvm.loop.mustprogress\"}\n";

    // How many instructions `function` has.
    std::size_t sizeOf(const llvm::Function& function)
    {
        return static_cast<std::size_t>(std::distance(llvm::inst_begin(function), llvm::inst_end(function)));
    }
} // namespace

TEST_CASE("each copy of a body gets its own alias scope where the body declares one, and its own loop ID",
          "[codegen][versions]")
{
    llvm::LLVMContext context;
    std::unique_ptr<llvm::Module> module = parsed(kScopedIr, context);
    llvm::Function& function = *module->getFunction("f");
    REQUIRE(versionAlone(function));
    CHECK(copiesOf(*module, "f") == 2);

    // Three bodies (the two copies and the original), each with its own scope and loop ID; each store in the scope
    // its own body declares.
    std::set<const llvm::MDNode*> declared;
    std::set<const llvm::MDNode*> stored;
    std::set<const llvm::MDNode*> loopIDs;
    for (const llvm::Instruction& instruction : llvm::instructions(function))
    {
        if (const auto* decl = llvm::dyn_cast<llvm::NoAliasScopeDeclInst>(&instruction))
            declared.insert(decl->getScopeList());
        if (llvm::isa<llvm::StoreInst>(instruction))
            stored.insert(instruction.getMetadata(llvm::LLVMContext::MD_alias_scope));
        if (const llvm::MDNode* loop = instruction.getMetadata(llvm::LLVMContext::MD_loop))
            loopIDs.insert(loop);
    }
    CHECK(declared.size() == 3);
    CHECK(stored == declared);
    CHECK(loopIDs.size() == 3);
}

TEST_CASE("the pass leaves a function it has versioned alone when it runs again", "[codegen][versions]")
{
    // FMinus's optimised IR optimised again (fminus x.fm | fminus-build -O3): the knob is still tagged, and the
    // original body still branches on it, but the function has its copies already.
    llvm::LLVMContext context;
    std::unique_ptr<llvm::Module> module = parsed(kScopedIr, context);
    llvm::Function& function = *module->getFunction("f");
    REQUIRE(versionAlone(function));
    const std::size_t once = sizeOf(function);
    const std::size_t blocks = function.size();
    REQUIRE(versionAlone(function));
    CHECK(sizeOf(function) == once);
    CHECK(function.size() == blocks);
    CHECK(copiesOf(*module, "f") == 2);
}

TEST_CASE("a stream's base and stride, selected before the loop, are versioned on, and are constants in each copy",
          "[codegen][versions][param]")
{
    // Two streams as the code generator reads them: each test selects a base and a stride where the function starts,
    // and the loop uses only those, with no select or branch of its own on a test. The first stream's selects come
    // before the second's test, the last knob: they move into the body, so each copy has its own. A stride's 1 is the
    // opaque unit, which a copy for a stream with samples makes 1 again.
    const char* ir = "define void @g(ptr %a, ptr %b, ptr %cell, ptr noalias %out, i32 %n) {\n"
                     "entry:\n"
                     "  %a.flat = icmp eq ptr %a, null, !fminus.domain !0\n"
                     "  %a.unit = call i64 asm \"\", \"=r,0\"(i64 1) #0\n"
                     "  %a.base = select i1 %a.flat, ptr %cell, ptr %a\n"
                     "  %a.stride = select i1 %a.flat, i64 0, i64 %a.unit\n"
                     "  %b.flat = icmp eq ptr %b, null, !fminus.domain !0\n"
                     "  %b.unit = call i64 asm \"\", \"=r,0\"(i64 1) #0\n"
                     "  %b.base = select i1 %b.flat, ptr %cell, ptr %b\n"
                     "  %b.stride = select i1 %b.flat, i64 0, i64 %b.unit\n"
                     "  %count = zext i32 %n to i64\n"
                     "  br label %loop\n"
                     "loop:\n"
                     "  %i = phi i64 [ 0, %entry ], [ %next, %loop ]\n"
                     "  %a.offset = mul i64 %i, %a.stride\n"
                     "  %a.at = getelementptr float, ptr %a.base, i64 %a.offset\n"
                     "  %x = load float, ptr %a.at, align 4\n"
                     "  %b.offset = mul i64 %i, %b.stride\n"
                     "  %b.at = getelementptr float, ptr %b.base, i64 %b.offset\n"
                     "  %y = load float, ptr %b.at, align 4\n"
                     "  %sum = fadd float %x, %y\n"
                     "  %to = getelementptr float, ptr %out, i64 %i\n"
                     "  store float %sum, ptr %to, align 4\n"
                     "  %next = add i64 %i, 1\n"
                     "  %done = icmp eq i64 %next, %count\n"
                     "  br i1 %done, label %exit, label %loop\n"
                     "exit:\n"
                     "  ret void\n"
                     "}\n"
                     "attributes #0 = { nounwind willreturn nosync memory(none) }\n"
                     "!0 = !{i1 false, i1 true}\n";
    llvm::LLVMContext context;
    std::unique_ptr<llvm::Module> module = parsed(ir, context);
    llvm::Function& function = *module->getFunction("g");
    REQUIRE(versionAlone(function));
    CHECK(copiesOf(*module, "g") == 4);

    // Four loops, each with a constant stride for each stream, 0 or 1: no select on a test is left, nor a use of a
    // unit.
    std::map<const llvm::BasicBlock*, std::pair<std::int64_t, std::int64_t>> loops;   // each loop's strides
    for (const llvm::Instruction& instruction : llvm::instructions(function))
    {
        if (const auto* select = llvm::dyn_cast<llvm::SelectInst>(&instruction))
            CHECK_FALSE(select->getCondition()->getName().ends_with(".flat"));
        if (instruction.getOpcode() != llvm::Instruction::Mul)
            continue;
        const auto* stride = llvm::dyn_cast<llvm::ConstantInt>(instruction.getOperand(1));
        INFO(instruction.getName().str());
        REQUIRE(stride != nullptr);
        auto [at, added] = loops.try_emplace(instruction.getParent(), -1, -1);
        (instruction.getName().starts_with("a.") ? at->second.first : at->second.second) = stride->getSExtValue();
    }
    std::set<std::pair<std::int64_t, std::int64_t>> strides;
    for (const auto& [loop, pair] : loops)
        strides.insert(pair);
    CHECK(strides == std::set<std::pair<std::int64_t, std::int64_t>>{{0, 0}, {0, 1}, {1, 0}, {1, 1}});
    for (const char* unit : {"a.unit", "b.unit"})
    {
        for (const llvm::Instruction& instruction : llvm::instructions(function))
        {
            if (instruction.getName() == unit)
                CHECK(instruction.use_empty());
        }
    }
}

namespace
{
    // Five streams in one loop: under the default budget, three of their tests are versioned on (8 copies), and the
    // other two are read as unversioned code reads them, in every copy.
    constexpr const char* kMix = "module Mix\n"
                                 "{\n"
                                 "    param float a;\n"
                                 "    param float b;\n"
                                 "    param float c;\n"
                                 "    param float d;\n"
                                 "    param float e;\n"
                                 "\n"
                                 "    void process(const float* restrict in, float* restrict out, int n)\n"
                                 "        [[clang::nonblocking]]\n"
                                 "    {\n"
                                 "        for (int i = 0; i < n; ++i)\n"
                                 "            out[i] = in[i] * a[i] + b[i] * c[i] + d[i] * e[i];\n"
                                 "    }\n"
                                 "};\n";

    struct Mix
    {
        std::array<Slot<float>, 5> streams;
    };

    // Mix::process's output for one sample, in C++, one rounding at a time.
    float mixed(float x, const std::array<float, 5>& at)
    {
        const float first = x * at[0];
        const float second = at[1] * at[2];
        const float both = first + second;
        const float third = at[3] * at[4];
        return both + third;
    }

    // A gain stage whose method has no loop, inlined into a loop that calls it a sample at a time.
    constexpr const char* kRack = "module Amp\n"
                                  "{\n"
                                  "    param float gain;\n"
                                  "    float apply(float x, int i) { return x * gain[i]; }\n"
                                  "};\n"
                                  "\n"
                                  "module Rack\n"
                                  "{\n"
                                  "    Amp amp;\n"
                                  "\n"
                                  "    void process(const float* restrict in, float* restrict out, int n)\n"
                                  "        [[clang::nonblocking]]\n"
                                  "    {\n"
                                  "        for (int i = 0; i < n; ++i)\n"
                                  "            out[i] = amp.apply(in[i], i);\n"
                                  "    }\n"
                                  "};\n";

    struct Rack
    {
        Slot<float> gain;
    };

    // A smoother over a scratch array, whose step a mode picks and whose input `invert` flips: six copies and the
    // original body, each zeroing the array where it is declared and writing it before reading it.
    constexpr const char* kScratch = "module Scratch\n"
                                     "{\n"
                                     "    param bool invert;\n"
                                     "    param int mode [[fminus::range(0, 2)]];\n"
                                     "    float y;\n"
                                     "\n"
                                     "    void process(const float* restrict in, float* restrict out, int n)\n"
                                     "        [[clang::nonblocking]]\n"
                                     "    {\n"
                                     "        float tmp[256];\n"
                                     "        for (int i = 0; i < n; ++i)\n"
                                     "            tmp[i] = invert[i] ? -in[i] * 0.5f : in[i] * 0.5f;\n"
                                     "        for (int i = 0; i < n; ++i)\n"
                                     "        {\n"
                                     "            const float k = mode[i] == 0 ? 0.01f : mode[i] == 1 ? 0.1f : 0.5f;\n"
                                     "            y += k * (tmp[i] - y);\n"
                                     "            out[i] = y;\n"
                                     "        }\n"
                                     "    }\n"
                                     "};\n";

    struct Scratch
    {
        Slot<bool> invert;
        Slot<int> mode;
        float y = 0.0f;
    };

    // Scratch::process in C++, one rounding at a time; the state last.
    std::vector<float> scratchCpp(bool invert, int mode, const std::vector<float>& in)
    {
        const float k = mode == 0 ? 0.01f : mode == 1 ? 0.1f : 0.5f;
        float y = 0.0f;
        std::vector<float> out;
        for (const float x : in)
        {
            const float half = invert ? -x * 0.5f : x * 0.5f;
            const float step = k * (half - y);
            y = y + step;
            out.push_back(y);
        }
        out.push_back(y);
        return out;
    }
} // namespace

TEST_CASE("a loop over more streams than the budget versions on still vectorises, and reads each as it says",
          "[codegen][versions][param]")
{
    std::map<std::string, fminus::ir::ParamBinding> streams;
    for (const char* name : {"Mix::a", "Mix::b", "Mix::c", "Mix::d", "Mix::e"})
        streams[name] = {Kind::Stream, 0.0};

    // Every stream read four samples at a time, unversioned, on one test, and on three (the two left read at a stride
    // the vectoriser checks is 1).
    const std::set<std::string> all{"a", "b", "c", "d", "e"};
    for (const auto& [budget, copies] : std::map<unsigned, int>{{0, 1}, {2, 2}, {8, 8}})
    {
        INFO("budget " << budget);
        llvm::LLVMContext context;
        const auto module = optimised(kMix, versions(budget, streams), context);
        CHECK(copiesOf(*module, "Mix::process") == copies);
        CHECK(vectorStreams(*module, "Mix::process") == all);
    }

    // Every combination of flat streams and streams with samples, over vector iterations and a remainder: what C++
    // computes, bit for bit.
    constexpr int kSamples = 67;
    std::vector<float> in;
    std::array<std::vector<float>, 5> samples;
    for (int i = 0; i < kSamples; ++i)
    {
        in.push_back(0.25f * static_cast<float>(i % 9) - 1.0f);
        for (std::size_t s = 0; s < samples.size(); ++s)
        {
            const float slope = s % 2 == 0 ? 0.013f : -0.017f;
            samples[s].push_back(0.1f * static_cast<float>(s + 1) + slope * static_cast<float>(i));
        }
    }
    const std::array<float, 5> flat{0.5f, -1.25f, 2.0f, 0.75f, -0.375f};
    for (const unsigned budget : {8u, 0u})
    {
        std::string errors;
        auto jit = compile(kMix, &errors, versions(budget, streams));
        INFO(errors);
        REQUIRE(jit);
        const auto process = lookup<void(Mix*, const float*, float*, int)>(*jit, "Mix::process");
        for (unsigned mask = 0; mask < 32; ++mask)   // bit s set: stream s is flat
        {
            INFO("budget " << budget << ", flat streams " << mask);
            Mix mix;
            for (std::size_t s = 0; s < 5; ++s)
            {
                const bool isFlat = ((mask >> s) & 1) != 0;
                mix.streams[s].stream = isFlat ? nullptr : samples[s].data();
                mix.streams[s].value = isFlat ? flat[s] : 1.0e9f;   // a stream with samples does not read its value
            }
            std::vector<float> out(kSamples);
            process(&mix, in.data(), out.data(), kSamples);
            std::vector<float> expected;
            for (int i = 0; i < kSamples; ++i)
            {
                std::array<float, 5> at{};
                for (std::size_t s = 0; s < 5; ++s)
                    at[s] = ((mask >> s) & 1) != 0 ? flat[s] : samples[s][static_cast<std::size_t>(i)];
                expected.push_back(mixed(in[static_cast<std::size_t>(i)], at));
            }
            CHECK(bits(out) == bits(expected));
        }
    }
}

TEST_CASE("a stream read in a method without a loop, inlined into its caller's loop, vectorises there",
          "[codegen][versions][param]")
{
    const auto stream = std::map<std::string, fminus::ir::ParamBinding>{{"Amp::gain", {Kind::Stream, 0.0}}};
    for (const unsigned budget : {8u, 0u})
    {
        INFO("budget " << budget);
        llvm::LLVMContext context;
        const auto module = optimised(kRack, versions(budget, stream), context);
        CHECK(copiesOf(*module, "Amp::apply") == 1);   // no loop of its own
        CHECK(vectorStreams(*module, "Rack::process") == std::set<std::string>{"gain"});

        std::string errors;
        auto jit = compile(kRack, &errors, versions(budget, stream));
        INFO(errors);
        REQUIRE(jit);
        const auto process = lookup<void(Rack*, const float*, float*, int)>(*jit, "Rack::process");
        std::vector<float> in;
        std::vector<float> ramp;
        for (int i = 0; i < 37; ++i)
        {
            in.push_back(static_cast<float>(i % 5) - 2.0f);
            ramp.push_back(0.05f * static_cast<float>(i));
        }
        std::vector<float> out(in.size());
        Rack rack;
        rack.gain.stream = ramp.data();
        rack.gain.value = 1.0e9f;
        process(&rack, in.data(), out.data(), static_cast<int>(in.size()));
        for (std::size_t i = 0; i < in.size(); ++i)
            CHECK(std::bit_cast<std::uint32_t>(out[i]) == std::bit_cast<std::uint32_t>(in[i] * ramp[i]));
        rack.gain.stream = nullptr;
        rack.gain.value = 0.75f;
        process(&rack, in.data(), out.data(), static_cast<int>(in.size()));
        for (std::size_t i = 0; i < in.size(); ++i)
            CHECK(std::bit_cast<std::uint32_t>(out[i]) == std::bit_cast<std::uint32_t>(in[i] * 0.75f));
    }
}

TEST_CASE("each copy has locals of its own, so zero-init elision removes a local array's memset in every copy",
          "[codegen][versions]")
{
    // One array shared by the copies would be zeroed in each, and read in all: no copy's memset would dominate the
    // reads, and zero-init elision would keep every one, at every level. Each copy's own array has its lifetime
    // marked, so the code generator gives them all one stack slot. At -O3 the original body, the fallback for a
    // mode outside 0..2, keeps its memset, as the method without copies does at -O3.
    for (const llvm::OptimizationLevel level :
         {llvm::OptimizationLevel::O1, llvm::OptimizationLevel::O2, llvm::OptimizationLevel::O3})
    {
        const bool o3 = level == llvm::OptimizationLevel::O3;
        INFO((o3 ? "-O3" : level == llvm::OptimizationLevel::O2 ? "-O2" : "-O1"));
        llvm::LLVMContext context;
        const auto module = optimised(kScratch, versions(8), context, level);
        CHECK(copiesOf(*module, "Scratch::process") == 6);
        int arrays = 0;
        for (const llvm::Instruction& instruction : llvm::instructions(*module->getFunction("Scratch::process")))
        {
            if (const auto* local = llvm::dyn_cast<llvm::AllocaInst>(&instruction))
            {
                ++arrays;
                CHECK(local->getParent()->isEntryBlock());
                CHECK(llvm::any_of(local->users(), [](const llvm::User* user) {
                    const auto* call = llvm::dyn_cast<llvm::IntrinsicInst>(user);
                    return call != nullptr && call->getIntrinsicID() == llvm::Intrinsic::lifetime_start;
                }));
            }
            if (const auto* memset = llvm::dyn_cast<llvm::MemSetInst>(&instruction))
            {
                const llvm::Value* local = llvm::getUnderlyingObject(memset->getDest());
                INFO("a memset of " << local->getName().str());
                CHECK((o3 && local->getName() == "tmp"));   // the fallback's, at -O3 only
            }
        }
        CHECK(arrays == 7);   // the six copies' and the original body's
    }

    std::string errors;
    auto versioned = compile(kScratch, &errors, versions(8));
    auto plain = compile(kScratch, &errors, versions(0));
    INFO(errors);
    REQUIRE(versioned);
    REQUIRE(plain);
    const std::vector<float> in = testSignal();
    const auto run = [&](fminus::jit::Jit& jit, bool invert, int mode) {
        Scratch scratch;
        scratch.invert.value = invert;
        scratch.mode.value = mode;
        std::vector<float> out(in.size());
        lookup<void(Scratch*, const float*, float*, int)>(jit, "Scratch::process")(&scratch, in.data(), out.data(),
                                                                                   static_cast<int>(in.size()));
        out.push_back(scratch.y);
        return out;
    };
    for (const bool invert : {false, true})
    {
        for (const int mode : {0, 1, 2, 7})
        {
            INFO("invert " << invert << ", mode " << mode);
            CHECK(bits(run(*versioned, invert, mode)) == bits(run(*plain, invert, mode)));
            CHECK(bits(run(*versioned, invert, mode)) == bits(scratchCpp(invert, mode, in)));
        }
    }
}
