// tests/unit/NpyTest.cpp: NumPy's .npy read (runtime/Npy.cpp), files made here byte by byte as numpy.save writes
// them: version 1.0 and 2.0 headers, float32, float64 and int32 values, 0 to 3 dimensions, a cap; and what is refused
// (another type, big-endian, Fortran order, a header or values cut short, a shape past what is there). Then the
// builtins through the JIT.
#include "Compile.h"
#include "runtime/Runtime.h"

#include <catch2/catch_test_macros.hpp>

#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

namespace
{
    // An .npy file of `header`'s dict and `values`' bytes: version 1.0 (a 2-byte length) or 2.0 (4 bytes), the
    // header padded with spaces and a newline to a multiple of 64 bytes, as numpy pads it.
    std::string npy(const std::string& dict, const void* values, std::size_t size, int major = 1)
    {
        std::string file = "\x93NUMPY";
        file += static_cast<char>(major);
        file += '\0';
        const std::size_t prefix = file.size() + (major == 1 ? 2 : 4);
        std::string header = dict;
        while ((prefix + header.size() + 1) % 64 != 0)
            header += ' ';
        header += '\n';
        const auto length = static_cast<std::uint32_t>(header.size());
        for (int b = 0; b < (major == 1 ? 2 : 4); ++b)
            file += static_cast<char>((length >> (8 * b)) & 0xff);
        file += header;
        file.append(static_cast<const char*>(values), size);
        return file;
    }

    std::int32_t floats(const std::string& file, std::vector<float>& out)
    {
        return fminus_rt_npy_floats(file.data(), static_cast<std::int32_t>(file.size()), out.data(),
                                    static_cast<std::int32_t>(out.size()));
    }

    std::int32_t shape(const std::string& file, std::int32_t axis)
    {
        return fminus_rt_npy_shape(file.data(), static_cast<std::int32_t>(file.size()), axis);
    }
} // namespace

TEST_CASE("npy: float32, float64 and int32 values, and their shape", "[npy]")
{
    const float m[6] = {0.0f, 0.5f, 1.0f, 1.5f, 2.0f, 2.5f};
    const std::string m32 = npy("{'descr': '<f4', 'fortran_order': False, 'shape': (2, 3), }", m, sizeof m);
    std::vector<float> out(8, -1.0f);
    CHECK(floats(m32, out) == 6);
    CHECK(std::vector<float>(out.begin(), out.begin() + 7) == std::vector<float>{0, 0.5f, 1, 1.5f, 2, 2.5f, -1});
    CHECK(shape(m32, -1) == 2);
    CHECK(shape(m32, 0) == 2);
    CHECK(shape(m32, 1) == 3);
    CHECK(shape(m32, 2) == -1);

    const double v[3] = {0.1, -2.5, 1e30};
    const std::string v64 = npy("{'descr': '<f8', 'fortran_order': False, 'shape': (3,), }", v, sizeof v, 2);
    CHECK(floats(v64, out) == 3);
    CHECK((out[0] == 0.1f && out[1] == -2.5f && out[2] == 1e30f));
    CHECK(shape(v64, -1) == 1);

    const std::int32_t k[2] = {7, -8};
    const std::string i32 = npy("{'descr': '<i4', 'fortran_order': False, 'shape': (1, 1, 2), }", k, sizeof k);
    CHECK(floats(i32, out) == 2);
    CHECK((out[0] == 7.0f && out[1] == -8.0f));
    CHECK(shape(i32, -1) == 3);
    CHECK(shape(i32, 2) == 2);

    const float s = 3.25f;
    const std::string scalar = npy("{'descr': '<f4', 'fortran_order': False, 'shape': (), }", &s, sizeof s);
    CHECK(floats(scalar, out) == 1);   // no dimension: one value
    CHECK(out[0] == 3.25f);
    CHECK(shape(scalar, -1) == 0);

    std::vector<float> two(2, -1.0f);
    CHECK(floats(m32, two) == 6);      // how many it holds; the first 2 written
    CHECK((two[0] == 0.0f && two[1] == 0.5f));
    CHECK(fminus_rt_npy_floats(m32.data(), static_cast<std::int32_t>(m32.size()), nullptr, 4) == 6);
}

TEST_CASE("npy: what is no file it reads", "[npy]")
{
    const float m[2] = {1.0f, 2.0f};
    std::vector<float> out(4);
    const auto refused = [&](const std::string& file) {
        INFO(file);
        CHECK(floats(file, out) == -1);
        CHECK(shape(file, -1) == -1);
    };
    refused(npy("{'descr': '|u1', 'fortran_order': False, 'shape': (2,), }", m, 2));
    refused(npy("{'descr': '>f4', 'fortran_order': False, 'shape': (2,), }", m, sizeof m));
    refused(npy("{'descr': '<f2', 'fortran_order': False, 'shape': (2,), }", m, sizeof m));
    refused(npy("{'descr': '<f4', 'fortran_order': True, 'shape': (2,), }", m, sizeof m));
    refused(npy("{'descr': '<f4', 'fortran_order': False, 'shape': (3,), }", m, sizeof m));   // values cut short
    refused(npy("{'descr': '<f4', 'fortran_order': False, 'shape': (2, x), }", m, sizeof m));
    refused(npy("{'descr': '<f4', 'fortran_order': False, }", m, sizeof m));                  // no shape
    refused(npy("{'descr': '<f4', 'fortran_order': False, 'shape': (99999999999,), }", m, sizeof m));
    refused(npy("{'descr': '<f4', 'fortran_order': False, 'shape': (2,), }", m, sizeof m, 4)); // no such version
    const std::string good = npy("{'descr': '<f4', 'fortran_order': False, 'shape': (2,), }", m, sizeof m);
    refused(good.substr(0, 40));   // the header cut short
    refused("NUMPY");
    CHECK(fminus_rt_npy_floats(nullptr, 100, out.data(), 4) == -1);
}

TEST_CASE("npy: the builtins, from a program", "[npy]")
{
    auto jit = fminus::test::compile(R"(
float sum(const void* data, int bytes)
{
    float v[8];
    const int n = npy_floats(data, bytes, v, 8);
    float total = 0;
    for (int i = 0; i < n && i < 8; ++i)
        total += v[i];
    return total + npy_shape(data, bytes, 0) * 100 + npy_shape(data, bytes, -1) * 1000;
}
)");
    REQUIRE(jit != nullptr);
    const float m[6] = {1, 2, 3, 4, 5, 6};
    const std::string file = npy("{'descr': '<f4', 'fortran_order': False, 'shape': (2, 3), }", m, sizeof m);
    const auto sum = fminus::test::lookup<float(const void*, int)>(*jit, "sum");
    CHECK(sum(file.data(), static_cast<int>(file.size())) == 21.0f + 200.0f + 2000.0f);
}
