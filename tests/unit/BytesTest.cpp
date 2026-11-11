// tests/unit/BytesTest.cpp: bytes_floats and bytes_pcm16 (extending.md, recipe 38): raw little-endian float32 and
// 16-bit PCM, decoded into floats from bytes of any alignment, as many as the cap takes, the count of all of them
// returned; null data and negative sizes empty; wait-free, so a nonblocking function calls them; and their errors. Run
// through the JIT, and the runtime's functions called from C++ as a host would.
#include "Compile.h"
#include "runtime/Runtime.h"

#include <catch2/catch_test_macros.hpp>

#include <bit>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <limits>
#include <vector>

using namespace fminus::test;

namespace
{
    // The program's kernels, on the audio thread.
    constexpr const char* kProgram =
        "int floats(const void* data, int bytes, float* out, int cap) [[clang::nonblocking]]\n"
        "{\n"
        "    return bytes_floats(data, bytes, out, cap);\n"
        "}\n"
        "int pcm16(const void* data, int bytes, float* out, int cap) [[clang::nonblocking]]\n"
        "{\n"
        "    return bytes_pcm16(data, bytes, out, cap);\n"
        "}\n";

    using Decode = int(const void*, int, float*, int);

    // `values`' bits as a server writes them: little-endian, whatever this machine's order.
    std::vector<unsigned char> littleEndian(const std::vector<std::uint32_t>& values, std::size_t offset = 0)
    {
        std::vector<unsigned char> bytes(offset);
        for (const std::uint32_t bits : values)
        {
            for (int i = 0; i < 4; ++i)
                bytes.push_back(static_cast<unsigned char>(bits >> (8 * i)));
        }
        return bytes;
    }
} // namespace

TEST_CASE("bytes_floats reads little-endian float32s, every bit of each, at any alignment", "[codegen][bytes]")
{
    std::string errors;
    auto jit = compile(kProgram, &errors);
    INFO(errors);
    REQUIRE(jit);
    const auto floats = lookup<Decode>(*jit, "floats");

    const std::vector<std::uint32_t> bits{
        std::bit_cast<std::uint32_t>(1.0f), std::bit_cast<std::uint32_t>(-0.0f),
        std::bit_cast<std::uint32_t>(std::numeric_limits<float>::infinity()),
        std::bit_cast<std::uint32_t>(std::numeric_limits<float>::denorm_min()),
        0x7FC01234u,   // a quiet NaN with a payload
        std::bit_cast<std::uint32_t>(-1234.5f)};
    for (const std::size_t offset : {0, 1, 3})   // the data a byte or three off a float's alignment
    {
        INFO(offset);
        const std::vector<unsigned char> data = littleEndian(bits, offset);
        std::vector<float> out(8, 7.0f);
        CHECK(floats(data.data() + offset, static_cast<int>(bits.size() * 4), out.data(), 8) == 6);
        for (std::size_t i = 0; i < bits.size(); ++i)
            CHECK(std::bit_cast<std::uint32_t>(out[i]) == bits[i]);
        CHECK(out[6] == 7.0f);   // nothing past the values
    }
}

TEST_CASE("bytes_floats and bytes_pcm16 count every value, write at most cap, and take no data as empty",
          "[codegen][bytes]")
{
    std::string errors;
    auto jit = compile(kProgram, &errors);
    INFO(errors);
    REQUIRE(jit);
    const auto floats = lookup<Decode>(*jit, "floats");
    const auto pcm16 = lookup<Decode>(*jit, "pcm16");
    const std::vector<unsigned char> data = littleEndian({0x3F800000u, 0x40000000u, 0x40400000u});   // 1, 2, 3

    std::vector<float> out(4, 9.0f);
    CHECK(floats(data.data(), 12, out.data(), 2) == 3);   // three there, two taken
    CHECK(out == std::vector<float>{1.0f, 2.0f, 9.0f, 9.0f});
    CHECK(floats(data.data(), 12, out.data(), 0) == 3);   // how many, and nothing written
    CHECK(floats(data.data(), 12, out.data(), -5) == 3);
    CHECK(floats(data.data(), 12, nullptr, 4) == 3);
    CHECK(floats(data.data(), 11, out.data(), 4) == 2);   // a partial value at the end is none
    CHECK(floats(data.data(), 3, out.data(), 4) == 0);
    CHECK(floats(nullptr, 12, out.data(), 4) == 0);
    CHECK(floats(data.data(), -4, out.data(), 4) == 0);
    CHECK(pcm16(data.data(), 7, out.data(), 0) == 3);
    CHECK(pcm16(nullptr, 7, out.data(), 4) == 0);
    CHECK(pcm16(data.data(), -2, out.data(), 4) == 0);
    CHECK(out == std::vector<float>{1.0f, 2.0f, 9.0f, 9.0f});
}

TEST_CASE("bytes_pcm16 scales little-endian 16-bit samples to [-1, 1), exactly", "[codegen][bytes]")
{
    std::string errors;
    auto jit = compile(kProgram, &errors);
    INFO(errors);
    REQUIRE(jit);
    const auto pcm16 = lookup<Decode>(*jit, "pcm16");
    const std::vector<std::int16_t> samples{0, 1, -1, 16384, -32768, 32767};
    std::vector<unsigned char> data{0};   // a byte off a sample's alignment
    for (const std::int16_t sample : samples)
    {
        data.push_back(static_cast<unsigned char>(static_cast<std::uint16_t>(sample) & 0xFF));
        data.push_back(static_cast<unsigned char>(static_cast<std::uint16_t>(sample) >> 8));
    }
    std::vector<float> out(samples.size());
    CHECK(pcm16(data.data() + 1, static_cast<int>(2 * samples.size()), out.data(), static_cast<int>(out.size()))
          == 6);
    CHECK(out == std::vector<float>{0.0f, 1.0f / 32768, -1.0f / 32768, 0.5f, -1.0f, 32767.0f / 32768});

    // The runtime's function, as a host calls it: the same.
    std::vector<float> host(samples.size());
    CHECK(fminus_rt_bytes_pcm16(data.data() + 1, 12, host.data(), 6) == 6);
    CHECK(host == out);
}

TEST_CASE("bytes_floats and bytes_pcm16 decode in place, out at the data's own address", "[codegen][bytes]")
{
    std::string errors;
    auto jit = compile(kProgram, &errors);
    INFO(errors);
    REQUIRE(jit);
    const auto floats = lookup<Decode>(*jit, "floats");
    const auto pcm16 = lookup<Decode>(*jit, "pcm16");

    // Eight samples in the first 16 bytes of eight floats: each float takes the place of two samples.
    const std::vector<std::int16_t> samples{16384, -32768, 1, -1, 32767, 0, -16384, 8192};
    std::vector<float> buffer(samples.size(), 9.0f);
    auto* bytes = reinterpret_cast<unsigned char*>(buffer.data());
    for (std::size_t i = 0; i < samples.size(); ++i)
    {
        bytes[2 * i] = static_cast<unsigned char>(static_cast<std::uint16_t>(samples[i]) & 0xFF);
        bytes[2 * i + 1] = static_cast<unsigned char>(static_cast<std::uint16_t>(samples[i]) >> 8);
    }
    CHECK(pcm16(buffer.data(), 16, buffer.data(), 8) == 8);
    for (std::size_t i = 0; i < samples.size(); ++i)
        CHECK(buffer[i] == static_cast<float>(samples[i]) / 32768);

    // A cap short of the count: the first three, from samples still in place.
    for (std::size_t i = 0; i < samples.size(); ++i)
    {
        bytes[2 * i] = static_cast<unsigned char>(static_cast<std::uint16_t>(samples[i]) & 0xFF);
        bytes[2 * i + 1] = static_cast<unsigned char>(static_cast<std::uint16_t>(samples[i]) >> 8);
    }
    CHECK(pcm16(buffer.data(), 16, buffer.data(), 3) == 8);
    CHECK(buffer[0] == 0.5f);
    CHECK(buffer[1] == -1.0f);
    CHECK(buffer[2] == 1.0f / 32768);

    // Float32s in place: each value's bits where they were.
    const std::vector<unsigned char> data = littleEndian({0x3F800000u, 0x7FC01234u, 0xC49A5000u});
    std::vector<float> same(3);
    std::memcpy(same.data(), data.data(), data.size());
    CHECK(floats(same.data(), 12, same.data(), 3) == 3);
    CHECK(std::bit_cast<std::uint32_t>(same[0]) == 0x3F800000u);
    CHECK(std::bit_cast<std::uint32_t>(same[1]) == 0x7FC01234u);
    CHECK(same[2] == -1234.5f);
}

TEST_CASE("bytes_floats and bytes_pcm16 take bytes and floats, as their prototypes say", "[codegen][bytes]")
{
    // Any pointer is bytes, as any converts to C's const void*; the output is floats.
    std::string errors;
    auto jit = compile("int f(const float2* v, float* out) { return bytes_floats(v, 8, out, 2); }\n", &errors);
    INFO(errors);
    REQUIRE(jit);
    const float2 v = {0.25f, -8.0f};
    float out[2] = {};
    CHECK(lookup<int(const float2*, float*)>(*jit, "f")(&v, out) == 2);
    CHECK(out[0] == 0.25f);
    CHECK(out[1] == -8.0f);

    CHECK(errorsOf("int a(const void* d, int* o) { return bytes_floats(d, 4, o, 1); }\n"
                   "int b(const void* d, const float* o) { return bytes_pcm16(d, 4, o, 1); }\n"
                   "int c(const void* d, float* o) { return bytes_floats(d, 4, o); }\n"
                   "int e(float x, float* o) { return bytes_floats(x, 4, o, 1); }\n")
          == "test.fm:1:39: error: incompatible pointer types passing 'int *' to parameter of type 'float *'\n"
             "test.fm:2:47: error: passing 'const float *' to parameter of type 'float *' discards qualifiers\n"
             "test.fm:3:41: error: too few arguments to function call, expected 4, have 3\n"
             "test.fm:4:35: error: passing 'float' to parameter of incompatible type 'const void *'\n");
}
