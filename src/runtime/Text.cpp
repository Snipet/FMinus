// src/runtime/Text.cpp: the type string's functions (Runtime.h), for == and a switch on a string, and the string_*
// builtins the code generator does not make inline (ir/Text.cpp). A string is a pointer and a length over bytes
// something else owns; each of these reads them, and string_copy writes into the program's char array, and nothing
// else: no allocation, no lock, no wait, a loop bounded by the lengths. A number is read as the web server reads a
// query's (web/Server.cpp): the whole text, by fast_float's general format (Numbers.h) or as a decimal int.
#include "runtime/Numbers.h"
#include "runtime/Runtime.h"

#include <algorithm>
#include <charconv>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <string_view>
#include <system_error>

namespace
{
    // The `n` bytes at `s`, or none for a null pointer or a length that is not positive.
    std::string_view view(const char* s, std::int32_t n)
    {
        return s == nullptr || n <= 0 ? std::string_view() : std::string_view(s, static_cast<std::size_t>(n));
    }
} // namespace

extern "C" std::int32_t fminus_rt_string_equal(const char* a, std::int32_t an, const char* b, std::int32_t bn) noexcept
{
    return view(a, an) == view(b, bn) ? 1 : 0;   // the lengths first: no byte is read when they differ
}

extern "C" std::int32_t fminus_rt_string_find(const char* s, std::int32_t n, const char* w, std::int32_t wn) noexcept
{
    const std::size_t at = view(s, n).find(view(w, wn));
    return at == std::string_view::npos ? -1 : static_cast<std::int32_t>(at);
}

extern "C" float fminus_rt_string_float(const char* s, std::int32_t n, float fallback) noexcept
{
    const std::string_view text = view(s, n);
    float value = 0.0f;
    if (text.empty()
        || fminus::runtime::readFloat(text.data(), static_cast<std::int32_t>(text.size()),
                                      fminus::runtime::NumberSyntax::General, value)
               != static_cast<std::int32_t>(text.size())
        || !std::isfinite(value))
        return fallback;
    return value;
}

extern "C" std::int32_t fminus_rt_string_int(const char* s, std::int32_t n, std::int32_t fallback) noexcept
{
    const std::string_view text = view(s, n);
    std::int32_t value = 0;
    const std::from_chars_result read = std::from_chars(text.data(), text.data() + text.size(), value, 10);
    if (text.empty() || read.ec != std::errc() || read.ptr != text.data() + text.size())
        return fallback;
    return value;
}

extern "C" std::int32_t fminus_rt_string_copy(char* out, std::int32_t cap, const char* s, std::int32_t n) noexcept
{
    const std::string_view text = view(s, n);
    if (out != nullptr && cap > 0 && !text.empty())
        std::memmove(out, text.data(), std::min<std::size_t>(text.size(), static_cast<std::size_t>(cap)));
    return static_cast<std::int32_t>(text.size());
}
