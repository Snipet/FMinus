// tests/unit/ParamTest.cpp: a module's parameters (extending.md, recipe 14), the values a host sets: a knob, read once
// when a method starts; a stream, read a sample at a time from a buffer; a frozen parameter, compiled in. Their slots
// in a C++ host's memory, a module's parameters inside another module, one instance under programs that bind it
// differently, and the errors, run through the JIT.
#include "Compile.h"

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <climits>
#include <cstddef>
#include <map>
#include <string>
#include <string_view>
#include <utility>

using namespace fminus::test;
using Kind = fminus::ir::ParamBinding::Kind;

namespace
{
    // A parameter's slot, as a C++ host declares it: the stream's pointer, then the value. The same for every binding.
    template <typename T>
    struct Slot
    {
        const float* stream = nullptr;
        T value{};
    };

    // A gain with a switch, the module most tests share.
    constexpr const char* kAmp = "module Amp\n"
                                 "{\n"
                                 "    param float gain;\n"
                                 "    param bool on;\n"
                                 "    param int mode;\n"
                                 "\n"
                                 "    void process(const float* restrict in, float* restrict out, int n)\n"
                                 "        [[clang::nonblocking]]\n"
                                 "    {\n"
                                 "        for (int i = 0; i < n; ++i)\n"
                                 "            out[i] = on[i] ? in[i] * gain[i] : in[i];\n"
                                 "    }\n"
                                 "\n"
                                 "    float overwrite(float* at)\n"
                                 "    {\n"
                                 "        *at = 7;\n"
                                 "        return this->gain[0];\n"
                                 "    }\n"
                                 "\n"
                                 "    int modeOf() { return mode[0]; }\n"
                                 "    float shadowed() { float gain = 3; return gain; }\n"
                                 "};\n";

    struct Amp
    {
        Slot<float> gain;
        Slot<bool> on;
        Slot<int> mode;
    };

    using Process = void(Amp*, const float*, float*, int);

    fminus::ir::Options bound(std::map<std::string, fminus::ir::ParamBinding> parameters)
    {
        fminus::ir::Options options;
        options.parameters = std::move(parameters);
        return options;
    }

    std::array<float, 4> processed(fminus::jit::Jit& jit, Amp& amp, const std::array<float, 4>& in)
    {
        std::array<float, 4> out{};
        lookup<Process>(jit, "Amp::process")(&amp, in.data(), out.data(), 4);
        return out;
    }

    // One step of a one-pole in C++, one rounding at a time (ModuleTest.cpp's next()).
    float next(float& y, float k, float x)
    {
        const float step = k * (x - y);
        y = y + step;
        return y;
    }

    std::string errorsWith(std::string_view source, const fminus::ir::Options& options)
    {
        std::string errors;
        CHECK(compile(source, &errors, options) == nullptr);
        return errors;
    }
} // namespace

TEST_CASE("a knob is the host's value, read once when the method starts", "[codegen][param]")
{
    std::string errors;
    auto jit = compile(kAmp, &errors);
    INFO(errors);
    REQUIRE(jit);
    const std::array<float, 4> in{1.0f, 2.0f, 3.0f, 4.0f};
    Amp amp;
    amp.gain.value = 0.5f;
    amp.on.value = true;
    CHECK(processed(*jit, amp, in) == std::array<float, 4>{0.5f, 1.0f, 1.5f, 2.0f});
    amp.gain.value = 2.0f;   // between calls: the next call reads it
    CHECK(processed(*jit, amp, in) == std::array<float, 4>{2.0f, 4.0f, 6.0f, 8.0f});
    amp.on.value = false;
    CHECK(processed(*jit, amp, in) == in);

    // Read when the method starts: a write to the value during the call is not what the call reads.
    CHECK(lookup<float(Amp*, float*)>(*jit, "Amp::overwrite")(&amp, &amp.gain.value) == 2.0f);
    CHECK(amp.gain.value == 7.0f);

    amp.mode.value = -3;
    CHECK(lookup<int(Amp*)>(*jit, "Amp::modeOf")(&amp) == -3);
    CHECK(lookup<float(Amp*)>(*jit, "Amp::shadowed")(&amp) == 3.0f);   // a local named gain is the local
}

TEST_CASE("a stream is read a sample at a time, from the buffer the host points it at", "[codegen][param]")
{
    std::string errors;
    auto jit = compile(kAmp, &errors, bound({{"Amp::gain", {Kind::Stream, 0.0}}}));
    INFO(errors);
    REQUIRE(jit);
    const std::array<float, 4> ramp{0.0f, 0.25f, 0.5f, 0.75f};
    Amp amp;
    amp.gain.stream = ramp.data();
    amp.gain.value = 100.0f;   // a stream's value is not read
    amp.on.value = true;
    CHECK(processed(*jit, amp, {4.0f, 4.0f, 4.0f, 4.0f}) == std::array<float, 4>{0.0f, 1.0f, 2.0f, 3.0f});

    // A one-pole whose coefficient is a stream, its state a field: the state stays in a register through the loop
    // (the stream cannot overlap it, codegen's alias scope), and the output is C++'s, sample for sample.
    auto smooth = compile("module Smooth\n"
                          "{\n"
                          "    param float k;\n"
                          "    float y;\n"
                          "    void process(const float* restrict in, float* restrict out, int n)\n"
                          "        [[clang::nonblocking]]\n"
                          "    {\n"
                          "        for (int i = 0; i < n; ++i)\n"
                          "            out[i] = y += k[i] * (in[i] - y);\n"
                          "    }\n"
                          "};\n",
                          &errors, bound({{"Smooth::k", {Kind::Stream, 0.0}}}));
    INFO(errors);
    REQUIRE(smooth);
    struct Smooth
    {
        Slot<float> k;
        float y = 0.0f;
    } filter;
    std::array<float, 64> x{};
    std::array<float, 64> k{};
    for (std::size_t i = 0; i < x.size(); ++i)
    {
        x[i] = i % 8 < 4 ? 1.0f : -1.0f;
        k[i] = 0.01f + 0.015f * static_cast<float>(i);
    }
    filter.k.stream = k.data();
    std::array<float, 64> out{};
    lookup<void(Smooth*, const float*, float*, int)>(*smooth, "Smooth::process")(&filter, x.data(), out.data(), 64);
    float y = 0.0f;
    for (std::size_t i = 0; i < x.size(); ++i)
        CHECK(out[i] == next(y, k[i], x[i]));
    CHECK(filter.y == y);
}

TEST_CASE("a frozen parameter is a constant, compiled in: its slot is not read", "[codegen][param]")
{
    std::string errors;
    auto jit = compile(kAmp, &errors,
                       bound({{"Amp::gain", {Kind::Frozen, 0.1}},
                              {"Amp::on", {Kind::Frozen, 0.5}},
                              {"Amp::mode", {Kind::Frozen, -2.7}}}));
    INFO(errors);
    REQUIRE(jit);
    Amp amp;   // gain 0, off: what the slots hold, which the program does not read
    const std::array<float, 4> in{1.0f, 2.0f, 3.0f, 4.0f};
    CHECK(processed(*jit, amp, in) == std::array<float, 4>{0.1f * 1.0f, 0.1f * 2.0f, 0.1f * 3.0f, 0.1f * 4.0f});
    CHECK(lookup<int(Amp*)>(*jit, "Amp::modeOf")(&amp) == -2);   // C's conversion: toward 0

    // An int's value past an int's range is its end; a bool frozen at 0 is false.
    auto ends = compile(kAmp, &errors, bound({{"Amp::mode", {Kind::Frozen, 1e12}}, {"Amp::on", {Kind::Frozen, 0}}}));
    REQUIRE(ends);
    amp.gain.value = 2.0f;
    amp.on.value = true;
    CHECK(lookup<int(Amp*)>(*ends, "Amp::modeOf")(&amp) == INT_MAX);
    CHECK(processed(*ends, amp, in) == in);
    auto low = compile(kAmp, &errors, bound({{"Amp::mode", {Kind::Frozen, -1e12}}}));
    REQUIRE(low);
    CHECK(lookup<int(Amp*)>(*low, "Amp::modeOf")(&amp) == INT_MIN);
}

TEST_CASE("one instance runs under programs that bind its parameters differently", "[codegen][param]")
{
    std::string errors;
    auto knob = compile(kAmp, &errors);
    auto stream = compile(kAmp, &errors, bound({{"Amp::gain", {Kind::Stream, 0.0}}}));
    auto frozen = compile(kAmp, &errors, bound({{"Amp::gain", {Kind::Frozen, 3.0}}}));
    INFO(errors);
    REQUIRE(knob);
    REQUIRE(stream);
    REQUIRE(frozen);
    const std::array<float, 4> ramp{1.0f, 2.0f, 3.0f, 4.0f};
    Amp amp;
    amp.gain.stream = ramp.data();
    amp.gain.value = 0.5f;
    amp.on.value = true;
    const std::array<float, 4> ones{1.0f, 1.0f, 1.0f, 1.0f};
    CHECK(processed(*knob, amp, ones) == std::array<float, 4>{0.5f, 0.5f, 0.5f, 0.5f});
    CHECK(processed(*stream, amp, ones) == ramp);
    CHECK(processed(*frozen, amp, ones) == std::array<float, 4>{3.0f, 3.0f, 3.0f, 3.0f});
}

TEST_CASE("a module's parameters are in it wherever it is: inside another module, in an array", "[codegen][param]")
{
    std::string errors;
    auto jit = compile(std::string(kAmp)
                           + "module Voice\n"
                             "{\n"
                             "    float level;\n"
                             "    Amp amps[2];\n"
                             "    void process(const float* restrict in, float* restrict out, int n)\n"
                             "    {\n"
                             "        amps[1].process(in, out, n);\n"
                             "        for (int i = 0; i < n; ++i)\n"
                             "            out[i] *= level;\n"
                             "    }\n"
                             "};\n",
                       &errors);
    INFO(errors);
    REQUIRE(jit);
    struct Voice
    {
        float level = 0.0f;
        std::array<Amp, 2> amps;
    } voice;
    voice.level = 2.0f;
    voice.amps[1].gain.value = 0.25f;
    voice.amps[1].on.value = true;
    const std::array<float, 4> in{1.0f, 2.0f, 3.0f, 4.0f};
    std::array<float, 4> out{};
    lookup<void(Voice*, const float*, float*, int)>(*jit, "Voice::process")(&voice, in.data(), out.data(), 4);
    CHECK(out == std::array<float, 4>{0.5f, 1.0f, 1.5f, 2.0f});
}

TEST_CASE("parameter errors are reported as clang reports its own", "[codegen][param]")
{
    CHECK(errorsWith("module M\n"
                     "{\n"
                     "    param float4 v;\n"
                     "    param int n [[fminus::range(10, 1)]];\n"
                     "    param float g [[fminus::range(0, 1)]] [[fminus::default(2)]];\n"
                     "    param float h [[fminus::range(0)]] [[fminus::default(1, 2)]] [[fminus::smooth(5)]];\n"
                     "    param bool on;\n"
                     "    param float g;\n"
                     "    float tick(int i)\n"
                     "    {\n"
                     "        g[0] = 1;\n"
                     "        float* p = &h[0];\n"
                     "        float a = h;\n"
                     "        float c = h[1.5f];\n"
                     "        return a + n[i];\n"
                     "    }\n"
                     "};\n"
                     "float outside(M* m) { return m->h[0]; }\n",
                     bound({{"M::n", {Kind::Stream, 0.0}}, {"M::on", {Kind::Stream, 0.0}}, {"M::nope", {}}}))
          == "test.fm:1:1: error: the host binds 'M::nope', but the program has no such parameter\n"
             "test.fm:3:18: error: a parameter is a float, an int or a bool, not 'float4'\n"
             "test.fm:4:15: error: parameter 'n' is an int: only a float parameter can be a stream\n"
             "test.fm:4:19: error: fminus::range's minimum must be below its maximum\n"
             "test.fm:5:17: error: the default of 'g', 2, is outside its fminus::range(0, 1)\n"
             "test.fm:6:21: error: 'fminus::range' takes two numbers, a minimum and a maximum\n"
             "test.fm:6:42: error: 'fminus::default' takes one number\n"
             "test.fm:6:68: error: unknown attribute 'fminus::smooth': a parameter takes fminus::range(min, max) and "
             "fminus::default(value)\n"
             "test.fm:7:16: error: parameter 'on' is a bool: only a float parameter can be a stream\n"
             "test.fm:8:17: error: duplicate member 'g'\n"
             "test.fm:11:10: error: parameter 'g' is the host's: a method reads it, and cannot assign it or take its "
             "address\n"
             "test.fm:12:22: error: parameter 'h' is the host's: a method reads it, and cannot assign it or take its "
             "address\n"
             "test.fm:13:19: error: parameter 'h' is read as h[i], in a method of 'M'\n"
             "test.fm:14:20: error: array subscript is not an integer\n"
             "test.fm:18:31: error: parameter 'h' is read as h[i], in a method of 'M'\n");
}
