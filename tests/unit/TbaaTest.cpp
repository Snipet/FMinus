// tests/unit/TbaaTest.cpp: C's strict-aliasing rule, told to LLVM as TBAA (src/ir/Tbaa.cpp). Programs that keep the
// rule compute what they computed without the tags: loops over a module's buffers that also write its int and pointer
// fields, float vectors and poly floats over float memory, the FFT's float2 bins in calloc'd memory, copies and
// addresses. Each runs in the JIT at -O2, with the tags and without them (Options::strictAliasing), against C++. And
// every load and store the code generator writes of memory carries its type's tag.
#include "Compile.h"

#include <catch2/catch_test_macros.hpp>

#include <llvm/IR/InstIterator.h>
#include <llvm/IR/Instructions.h>
#include <llvm/IR/IntrinsicInst.h>
#include <llvm/IR/Metadata.h>
#include <llvm/Support/raw_ostream.h>

#include <array>
#include <cmath>
#include <cstddef>
#include <map>
#include <memory>
#include <string>
#include <vector>

using namespace fminus::test;

namespace
{
    // The two ways to compile a program: with the tags (the default), and without them, as clang's
    // -fno-strict-aliasing. A program that keeps the rule computes the same under both.
    std::array<fminus::ir::Options, 2> bothRules()
    {
        fminus::ir::Options strict;
        fminus::ir::Options loose;
        loose.strictAliasing = false;
        return {strict, loose};
    }

    // The research's loop over two buffers that are fields, with a gain, and an int field the loop counts in.
    constexpr const char* kAccumulator = "module Acc\n"
                                         "{\n"
                                         "    float* tail;\n"
                                         "    float* result;\n"
                                         "    int size;\n"
                                         "    float gain;\n"
                                         "    int count;\n"
                                         "\n"
                                         "    void add() [[clang::nonblocking]]\n"
                                         "    {\n"
                                         "        for (int i = 0; i < size; ++i)\n"
                                         "        {\n"
                                         "            tail[i] += gain * result[i];\n"
                                         "            ++count;\n"
                                         "        }\n"
                                         "    }\n"
                                         "};\n";

    // What a C++ host declares to hold one, and add() in C++, one rounding a statement (FMinus never fuses).
    struct Acc
    {
        float* tail = nullptr;
        float* result = nullptr;
        int size = 0;
        float gain = 0.0f;
        int count = 0;
    };

    void add(Acc& acc)
    {
        for (int i = 0; i < acc.size; ++i)
        {
            const float step = acc.gain * acc.result[i];
            acc.tail[i] = acc.tail[i] + step;
            ++acc.count;
        }
    }

    std::vector<float> ramp(std::size_t n, float scale)
    {
        std::vector<float> values(n);
        for (std::size_t i = 0; i < n; ++i)
            values[i] = scale * static_cast<float>(i % 7) - 1.5f;
        return values;
    }
} // namespace

TEST_CASE("a method's loop over its fields' buffers computes what it did, its int field too", "[codegen][tbaa]")
{
    for (const fminus::ir::Options& options : bothRules())
    {
        INFO("strict aliasing: " << options.strictAliasing);
        std::string errors;
        auto jit = compile(kAccumulator, &errors, options);
        INFO(errors);
        REQUIRE(jit);
        auto run = lookup<void(Acc*)>(*jit, "Acc::add");
        for (const int n : {0, 1, 3, 4, 37, 64, 513})
        {
            INFO("n = " << n);
            const auto size = static_cast<std::size_t>(n);

            // Two buffers apart, as the loop is written for.
            std::vector<float> tail = ramp(size, 0.25f);
            std::vector<float> result = ramp(size, 0.5f);
            std::vector<float> expectedTail = tail;
            Acc acc{tail.data(), result.data(), n, 0.75f, 5};
            Acc expected{expectedTail.data(), result.data(), n, 0.75f, 5};
            run(&acc);
            run(&acc);
            add(expected);
            add(expected);
            CHECK(tail == expectedTail);
            CHECK(acc.count == expected.count);
            CHECK(acc.tail == tail.data());   // the pointer fields are left as they were
            CHECK(acc.result == result.data());

            // The same buffer, and one a float behind the other: floats may alias floats, so each iteration reads
            // what the one before it wrote, as C's loop does.
            std::vector<float> one = ramp(size + 1, 0.125f);
            std::vector<float> expectedOne = one;
            Acc same{one.data(), one.data(), n, 0.5f, 0};
            Acc expectedSame{expectedOne.data(), expectedOne.data(), n, 0.5f, 0};
            run(&same);
            add(expectedSame);
            CHECK(one == expectedOne);
            Acc behind{one.data() + 1, one.data(), n, 1.0f, 0};
            Acc expectedBehind{expectedOne.data() + 1, expectedOne.data(), n, 1.0f, 0};
            run(&behind);
            add(expectedBehind);
            CHECK(one == expectedOne);
            CHECK(behind.count == n);
        }
    }
}

namespace
{
    // A delay whose write position is a pointer field, moved a sample at a time while the loop stores floats through
    // it, and counted in an int field.
    constexpr const char* kDelay = "module Delay\n"
                                   "{\n"
                                   "    float* line;\n"
                                   "    float* cursor;\n"
                                   "    int length;\n"
                                   "    int written;\n"
                                   "\n"
                                   "    void process(const float* restrict in, float* restrict out, int n)"
                                   " [[clang::nonblocking]]\n"
                                   "    {\n"
                                   "        for (int i = 0; i < n; ++i)\n"
                                   "        {\n"
                                   "            out[i] = *cursor;\n"
                                   "            *cursor = in[i];\n"
                                   "            cursor = cursor + 1;\n"
                                   "            if (cursor == line + length)\n"
                                   "                cursor = line;\n"
                                   "            ++written;\n"
                                   "        }\n"
                                   "    }\n"
                                   "};\n";

    struct Delay
    {
        float* line = nullptr;
        float* cursor = nullptr;
        int length = 0;
        int written = 0;
    };

    void process(Delay& delay, const float* in, float* out, int n)
    {
        for (int i = 0; i < n; ++i)
        {
            out[i] = *delay.cursor;
            *delay.cursor = in[i];
            ++delay.cursor;
            if (delay.cursor == delay.line + delay.length)
                delay.cursor = delay.line;
            ++delay.written;
        }
    }
} // namespace

TEST_CASE("a loop that stores floats keeps a module's pointer and int fields in step", "[codegen][tbaa]")
{
    for (const fminus::ir::Options& options : bothRules())
    {
        INFO("strict aliasing: " << options.strictAliasing);
        std::string errors;
        auto jit = compile(kDelay, &errors, options);
        INFO(errors);
        REQUIRE(jit);
        auto run = lookup<void(Delay*, const float*, float*, int)>(*jit, "Delay::process");
        std::vector<float> line(13, 0.0f);
        std::vector<float> expectedLine(13, 0.0f);
        Delay delay{line.data(), line.data(), 13, 0};
        Delay expected{expectedLine.data(), expectedLine.data(), 13, 0};
        const std::vector<float> in = ramp(200, 0.5f);
        std::vector<float> out(in.size(), -1.0f);
        std::vector<float> expectedOut(in.size(), -1.0f);
        std::size_t at = 0;
        for (const int n : {1, 12, 13, 14, 64, 61, 35})   // 200 samples, in calls of every size around the line's
        {
            INFO("a call of " << n);
            run(&delay, in.data() + at, out.data() + at, n);
            process(expected, in.data() + at, expectedOut.data() + at, n);
            at += static_cast<std::size_t>(n);
            CHECK(delay.cursor - delay.line == expected.cursor - expected.line);
            CHECK(delay.written == expected.written);
        }
        CHECK(at == in.size());
        CHECK(out == expectedOut);
        CHECK(line == expectedLine);
    }
}

TEST_CASE("float vectors and poly floats are floats: they read and write float memory, and floats read them",
          "[codegen][tbaa]")
{
    // Each function writes memory as one type and reads it as another of the same elements; a tag of their own would
    // let LLVM return the value stored before.
    const char* source =
        "float frames(float* p)\n"   // a float4 over a float buffer, through a void*
        "{\n"
        "    void* raw = p;\n"
        "    float4* v = raw;\n"
        "    p[1] = 3;\n"
        "    v[0] = v[0] * 2;\n"
        "    return p[1];\n"   // 6
        "}\n"
        "float samples(float4* frames)\n"   // floats over a float4 buffer
        "{\n"
        "    void* raw = frames;\n"
        "    float* p = raw;\n"
        "    frames[0] = (float4)(1, 2, 3, 4);\n"
        "    p[2] = 9;\n"
        "    return frames[0].z + p[3];\n"   // 9 + 4
        "}\n"
        "float lanes(float* p)\n"
        "{\n"
        "    p[0] = 5;\n"
        "    poly_store(p, poly_load(p) + 1);\n"
        "    return p[0];\n"   // 6
        "}\n"
        "float masked(float* p)\n"
        "{\n"
        "    p[0] = 2;\n"
        "    p[1] = 7;\n"
        "    poly_store(p, poly_load(p, poly_index() < 1) * 10, poly_index() < 1);\n"   // lane 0 only
        "    return p[0] + p[1];\n"   // 20 + 7
        "}\n";
    for (const unsigned polyLanes : {4u, 8u})
    {
        for (fminus::ir::Options options : bothRules())
        {
            options.polyLanes = polyLanes;
            INFO("strict aliasing: " << options.strictAliasing << ", poly lanes: " << polyLanes);
            std::string errors;
            auto jit = compile(source, &errors, options);
            INFO(errors);
            REQUIRE(jit);
            alignas(64) std::array<float, 16> buffer{};
            buffer.fill(1.0f);
            CHECK(lookup<float(float*)>(*jit, "frames")(buffer.data()) == 6.0f);
            CHECK(buffer[0] == 2.0f);
            std::array<float4, 2> frames{};
            CHECK(lookup<float(float4*)>(*jit, "samples")(frames.data()) == 13.0f);
            buffer.fill(1.0f);
            CHECK(lookup<float(float*)>(*jit, "lanes")(buffer.data()) == 6.0f);
            CHECK(buffer[polyLanes - 1] == 2.0f);
            CHECK(buffer[polyLanes] == 1.0f);
            buffer.fill(1.0f);
            CHECK(lookup<float(float*)>(*jit, "masked")(buffer.data()) == 27.0f);
        }
    }
}

TEST_CASE("the FFT's float2 bins in calloc'd memory are floats too", "[codegen][tbaa]")
{
    // One allocation, the bins as float2s and as their floats, (re, im) after each other. An impulse's spectrum is 1
    // in every bin; bin 1 is tripled through the floats and bin 2 doubled through the float2s, and each read sees the
    // other's write. The inverse of that spectrum at 0 is n + 2 * 2 + 2 * 1 (the inverse doesn't scale).
    const char* source = "void spectrum(int n, float* results)\n"
                         "{\n"
                         "    void* plan = fft_create(n);\n"
                         "    float* x = calloc(n, sizeof(float));\n"
                         "    void* memory = calloc(n / 2 + 1, sizeof(float2));\n"
                         "    float2* bins = memory;\n"
                         "    float* parts = memory;\n"
                         "    x[0] = 1;\n"
                         "    fft_forward(plan, x, bins);\n"
                         "    parts[2] = parts[2] * 3;\n"
                         "    bins[2] = bins[2] * 2;\n"
                         "    results[0] = parts[2] + parts[4] + bins[1].x;\n"
                         "    results[1] = parts[3] + bins[2].y;\n"
                         "    fft_inverse(plan, bins, x);\n"
                         "    results[2] = x[0];\n"
                         "    fft_destroy(plan);\n"
                         "    free(x);\n"
                         "    free(memory);\n"
                         "}\n";
    for (const fminus::ir::Options& options : bothRules())
    {
        INFO("strict aliasing: " << options.strictAliasing);
        std::string errors;
        auto jit = compile(source, &errors, options);
        INFO(errors);
        REQUIRE(jit);
        for (const int n : {64, 96})
        {
            INFO("n = " << n);
            std::array<float, 3> results{};
            lookup<void(int, float*)>(*jit, "spectrum")(n, results.data());
            CHECK(std::fabs(results[0] - (3.0f + 2.0f + 3.0f)) < 1e-5f);
            CHECK(std::fabs(results[1]) < 1e-5f);
            CHECK(std::fabs(results[2] - (static_cast<float>(n) + 6.0f)) < 1e-3f);
        }
    }
}

TEST_CASE("copies, and variables reached through their addresses, keep their values among tagged accesses",
          "[codegen][tbaa]")
{
    // A module's copy is a memcpy, untagged: it reads and writes every field, whatever its type.
    const char* source = "module M { float x; int n; float* p; };\n"
                         "float copied()\n"
                         "{\n"
                         "    M a;\n"
                         "    a.x = 1;\n"
                         "    a.n = 3;\n"
                         "    M b = a;\n"
                         "    a.x = 2;\n"
                         "    a.n = 5;\n"
                         "    return b.x + 10 * a.x + 100 * b.n;\n"   // 1 + 20 + 300
                         "}\n"
                         "int through(M* from, M* to, float* buffer)\n"
                         "{\n"
                         "    from->p = buffer;\n"
                         "    from->n = 4;\n"
                         "    *to = *from;\n"
                         "    to->p[0] = 8;\n"
                         "    return to->n + (int)buffer[0];\n"   // 4 + 8
                         "}\n"
                         "float addressed()\n"
                         "{\n"
                         "    float x = 1;\n"
                         "    int k = 2;\n"
                         "    float* px = &x;\n"
                         "    int* pk = &k;\n"
                         "    *px = 3;\n"
                         "    *pk = 4;\n"
                         "    return x + k;\n"   // 7
                         "}\n";
    for (const fminus::ir::Options& options : bothRules())
    {
        INFO("strict aliasing: " << options.strictAliasing);
        std::string errors;
        auto jit = compile(source, &errors, options);
        INFO(errors);
        REQUIRE(jit);
        CHECK(lookup<float()>(*jit, "copied")() == 321.0f);
        struct M
        {
            float x;
            int n;
            float* p;
        };
        M from{0.5f, 0, nullptr};
        M to{0.0f, 0, nullptr};
        float buffer = 0.0f;
        CHECK(lookup<int(M*, M*, float*)>(*jit, "through")(&from, &to, &buffer) == 12);
        CHECK((to.x == 0.5f && to.p == &buffer));
        CHECK(lookup<float()>(*jit, "addressed")() == 7.0f);
    }
}

namespace
{
    // The program lowered, not optimised, and the name of the TBAA type of each tagged access (its tag's access type,
    // operand 1), by instruction; untagged accesses are listed apart.
    struct Accesses
    {
        std::map<std::string, int> tagged;   // type name -> how many
        std::vector<const llvm::Instruction*> untagged;
        std::vector<std::string> untaggedText;
    };

    std::string tagType(const llvm::MDNode* tag)
    {
        const auto* type = llvm::cast<llvm::MDNode>(tag->getOperand(1));
        return llvm::cast<llvm::MDString>(type->getOperand(0))->getString().str();
    }

    bool accessesMemory(const llvm::Instruction& instruction)
    {
        if (llvm::isa<llvm::LoadInst>(instruction) || llvm::isa<llvm::StoreInst>(instruction))
            return true;
        const auto* intrinsic = llvm::dyn_cast<llvm::IntrinsicInst>(&instruction);
        return intrinsic != nullptr
               && (intrinsic->getIntrinsicID() == llvm::Intrinsic::masked_load
                   || intrinsic->getIntrinsicID() == llvm::Intrinsic::masked_store);
    }
} // namespace

TEST_CASE("every load and store of memory carries its type's tag; a slot's first store may not", "[codegen][tbaa]")
{
    const char* source = "module Voice\n"
                         "{\n"
                         "    param float cutoff;\n"
                         "    param int shape;\n"
                         "    param bool on;\n"
                         "    float y;\n"
                         "    int n;\n"
                         "    bool held;\n"
                         "    float* buffer;\n"
                         "    float4 frame;\n"
                         "    float history[4];\n"
                         "\n"
                         "    float tick(float x)\n"
                         "    {\n"
                         "        history[n % 4] = x;\n"
                         "        frame.yx = (float2)(x, y);\n"
                         "        held = on[0] && shape[0] > 1;\n"
                         "        ++n;\n"
                         "        buffer[0] = frame.x;\n"
                         "        return y += cutoff[0] * (x - y);\n"
                         "    }\n"
                         "};\n"
                         "float run(Voice* v, float* p, int count)\n"
                         "{\n"
                         "    int list[3] = {1, 2, 3};\n"
                         "    Voice copy = *v;\n"
                         "    poly_store(p, poly_load(p, poly_index() < count) * 2);\n"
                         "    poly_store(p, poly_load(p) + 1, poly_index() < count);\n"
                         "    return copy.tick(p[0]) + list[count];\n"
                         "}\n";
    std::map<std::string, fminus::ir::ParamBinding> streams{
        {"Voice::cutoff", {fminus::ir::ParamBinding::Kind::Stream, 0.0}}};
    for (const bool stream : {false, true})
    {
        for (const bool strict : {true, false})
        {
            INFO("strict aliasing: " << strict << ", cutoff a stream: " << stream);
            fminus::Diagnostics diagnostics("test.fm");
            const auto program = fminus::frontend::parse(source, diagnostics);
            REQUIRE(program);
            fminus::ir::Options options;
            options.strictAliasing = strict;
            if (stream)
                options.parameters = streams;
            llvm::LLVMContext context;
            const std::unique_ptr<llvm::Module> module = fminus::ir::lower(*program, context, diagnostics, options);
            INFO(diagnostics.format());
            REQUIRE(module);

            Accesses accesses;
            for (const llvm::Function& function : *module)
            {
                for (const llvm::Instruction& instruction : llvm::instructions(function))
                {
                    if (!accessesMemory(instruction))
                        continue;
                    if (const llvm::MDNode* tag = instruction.getMetadata(llvm::LLVMContext::MD_tbaa))
                    {
                        ++accesses.tagged[tagType(tag)];
                        continue;
                    }
                    std::string text;
                    llvm::raw_string_ostream out(text);
                    instruction.print(out);
                    accesses.untaggedText.push_back(text);
                    // Untagged, with the rule: only the store that starts a parameter's or a declaration's stack
                    // slot, which mem2reg removes.
                    const auto* store = llvm::dyn_cast<llvm::StoreInst>(&instruction);
                    if (strict && (store == nullptr || !llvm::isa<llvm::AllocaInst>(store->getPointerOperand())))
                        accesses.untagged.push_back(&instruction);
                }
            }
            for (const std::string& text : accesses.untaggedText)
                UNSCOPED_INFO("untagged:" << text);
            if (!strict)
            {
                CHECK(accesses.tagged.empty());
                continue;
            }
            CHECK(accesses.untagged.empty());
            CHECK(accesses.tagged.size() == 4);   // float, int, bool, any pointer: nothing else
            CHECK(accesses.tagged["float"] > 0);
            CHECK(accesses.tagged["int"] > 0);
            CHECK(accesses.tagged["bool"] > 0);
            CHECK(accesses.tagged["any pointer"] > 0);
        }
    }
}
