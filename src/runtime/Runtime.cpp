// src/runtime/Runtime.cpp
#include "runtime/Runtime.h"

#include <charconv>
#include <cstdio>
#include <cstdlib>

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
