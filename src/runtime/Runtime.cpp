// src/runtime/Runtime.cpp
#include "runtime/Runtime.h"

#include <charconv>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <limits>

extern "C" std::int32_t fminus_rt_abi_version() noexcept
{
    return 1;
}

extern "C" std::int32_t fminus_rt_main_args(std::int32_t argc, char** argv, std::int32_t count, float* args) noexcept
{
    const char* program = argc > 0 ? argv[0] : "fminus";
    if (argc - 1 != count)
    {
        std::fprintf(stderr, "%s: 'main' takes %d argument%s, not %d\n", program, count, count == 1 ? "" : "s",
                     argc - 1);
        return 2;
    }
    for (std::int32_t i = 0; i < count; ++i)
    {
        const char* text = argv[i + 1];
        char* end = nullptr;
        args[i] = std::strtof(text, &end);
        if (end == text || *end != '\0')
        {
            std::fprintf(stderr, "%s: '%s' is not a number\n", program, text);
            return 2;
        }
    }
    return 0;
}

extern "C" void fminus_rt_main_result(float result) noexcept
{
    char text[32];   // the longest float, -1.17549435e-38, is 15 characters
    const std::to_chars_result written = std::to_chars(text, text + sizeof text - 1, result);
    *written.ptr = '\n';
    std::fwrite(text, 1, static_cast<std::size_t>(written.ptr + 1 - text), stdout);
}

extern "C" void fminus_rt_main_bool_result(bool result) noexcept
{
    std::fputs(result ? "true\n" : "false\n", stdout);
}

namespace
{
    constexpr std::int64_t kAlignment = 64;
    constexpr std::int64_t kLargest = std::int64_t{1} << 34;   // 16 GiB: any more is a mistake, and zeroing it would
                                                               // take the machine's memory
}

extern "C" void* fminus_rt_malloc(std::int64_t size) noexcept
{
    return fminus_rt_calloc(1, size);
}

extern "C" void* fminus_rt_calloc(std::int64_t count, std::int64_t size) noexcept
{
    if (count <= 0 || size <= 0 || count > kLargest / size)
        return nullptr;
    // aligned_alloc wants a multiple of the alignment. Zeroing touches every page, so the audio thread will not fault
    // on one later.
    const std::int64_t bytes = (count * size + kAlignment - 1) / kAlignment * kAlignment;
    void* memory = std::aligned_alloc(static_cast<std::size_t>(kAlignment), static_cast<std::size_t>(bytes));
    if (memory != nullptr)
        std::memset(memory, 0, static_cast<std::size_t>(bytes));
    return memory;
}

extern "C" void fminus_rt_free(void* memory) noexcept
{
    std::free(memory);
}
