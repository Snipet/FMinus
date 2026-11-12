// src/runtime/Bytes.cpp: bytes to numbers, for FMinus's bytes_floats and bytes_pcm16 (Runtime.h): a payload a program's
// own server sends, raw little-endian float32 or 16-bit PCM, needs no parser. Each value is read from its bytes one by
// one, so the machine's byte order and the data's alignment do not matter. And the scaffolding of string literals as
// builtins' arguments, __fminus_string_hash.
#include "runtime/Format.h"
#include "runtime/Runtime.h"

#include <algorithm>
#include <bit>
#include <cstdint>

namespace
{
    // How many of `count` values a call writes: none for a null `out` or a cap that is not positive.
    std::int32_t taken(const float* out, std::int32_t cap, std::int32_t count)
    {
        return out == nullptr ? 0 : std::clamp(cap, 0, count);
    }

    std::uint32_t byteAt(const unsigned char* data, std::int32_t i)
    {
        return data[i];
    }
} // namespace

extern "C" std::int32_t fminus_rt_bytes_floats(const void* data, std::int32_t bytes, float* out,
                                               std::int32_t cap) noexcept
{
    if (data == nullptr || bytes < 0)
        return 0;
    const auto* in = static_cast<const unsigned char*>(data);
    const std::int32_t count = bytes / 4;
    const std::int32_t n = taken(out, cap, count);
    for (std::int32_t i = 0; i < n; ++i)
    {
        const std::uint32_t bits = byteAt(in, 4 * i) | byteAt(in, 4 * i + 1) << 8 | byteAt(in, 4 * i + 2) << 16
                                   | byteAt(in, 4 * i + 3) << 24;
        out[i] = std::bit_cast<float>(bits);
    }
    return count;
}

extern "C" std::int32_t fminus_rt_bytes_pcm16(const void* data, std::int32_t bytes, float* out,
                                              std::int32_t cap) noexcept
{
    if (data == nullptr || bytes < 0)
        return 0;
    const auto* in = static_cast<const unsigned char*>(data);
    const std::int32_t count = bytes / 2;
    // The last value first: a value's float takes the place of two samples, so a program decoding in place (`out` at
    // `data`'s address) finds every sample still there when its turn comes. bytes_floats takes four bytes to four,
    // so it may go first to last.
    for (std::int32_t i = taken(out, cap, count) - 1; i >= 0; --i)
    {
        const auto bits = static_cast<std::uint16_t>(byteAt(in, 2 * i) | byteAt(in, 2 * i + 1) << 8);
        const auto sample = static_cast<std::int16_t>(bits);
        out[i] = static_cast<float>(sample) * (1.0f / 32768.0f);   // exact: a 16-bit integer times a power of two
    }
    return count;
}

extern "C" std::int32_t fminus_rt_string_hash(const char* text, std::int32_t length, const std::int32_t* ints,
                                              std::int32_t count) noexcept
{
    std::uint32_t hash = 2166136261u;   // FNV-1a's offset basis
    fminus::runtime::expandFormat(text, length, ints, count, [&](char c) {
        hash = (hash ^ static_cast<unsigned char>(c)) * 16777619u;   // and its prime
    });
    return static_cast<std::int32_t>(hash);
}
