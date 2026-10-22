// tests/library/StrictAliasingTest.cpp: Program::Options::strictAliasing, as a host sets it. A program that keeps C's
// strict-aliasing rule computes the same with it on (the default) and off (clang's -fno-strict-aliasing): a method's
// loop over buffers its module points to, in memory the host owns.
#include "fminus/FMinus.h"

#include <catch2/catch_test_macros.hpp>

#include <cstddef>
#include <string>
#include <vector>

namespace
{
    const char* const kAccumulator = "module Acc\n"
                                     "{\n"
                                     "    float* tail;\n"
                                     "    float* result;\n"
                                     "    int size;\n"
                                     "\n"
                                     "    void add() [[clang::nonblocking]]\n"
                                     "    {\n"
                                     "        for (int i = 0; i < size; ++i)\n"
                                     "            tail[i] += result[i];\n"
                                     "    }\n"
                                     "};\n";

    // The module as the host declares it: the same fields in the same order.
    struct Acc
    {
        float* tail;
        float* result;
        int size;
    };
} // namespace

TEST_CASE("strict aliasing is on by default, and a host may turn it off", "[library]")
{
    CHECK(fminus::Program::Options{}.strictAliasing);
    std::vector<std::vector<float>> tails;
    for (const bool strict : {true, false})
    {
        INFO("strict aliasing: " << strict);
        fminus::Program::Options options;
        options.strictAliasing = strict;
        std::string errors;
        auto program = fminus::Program::compile(kAccumulator, errors, options);
        INFO(errors);
        REQUIRE(program);
        auto* add = program->function<void(void*)>("Acc::add", errors);
        REQUIRE(add != nullptr);
        std::vector<float> tail(67);
        std::vector<float> result(67);
        for (std::size_t i = 0; i < tail.size(); ++i)
        {
            tail[i] = 0.5f * static_cast<float>(i);
            result[i] = 0.25f * static_cast<float>(i % 5);
        }
        Acc acc{tail.data(), result.data(), 67};
        add(&acc);
        add(&acc);
        CHECK(acc.tail == tail.data());
        CHECK(tail[66] == 33.0f + 2.0f * 0.25f);   // 66 % 5 is 1
        tails.push_back(tail);
    }
    CHECK(tails[0] == tails[1]);
}
