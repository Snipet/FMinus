// src/runtime/Npy.cpp: NumPy's .npy files read (Runtime.h; docs/tables.md), in every build: what numpy.save writes and
// numpy.load reads, the way data comes back from Python. Its format, version 1.0 to 3.0: the magic "\x93NUMPY", the
// major and minor version bytes, the header's length (two bytes, little-endian, in 1.0; four in 2.0 and 3.0), the
// header, a Python dict literal ({'descr': '<f4', 'fortran_order': False, 'shape': (3, 4), }) padded with spaces and a
// newline, then the values, `shape`'s product of them, in C order. Read here: little-endian float32 ('<f4'), float64
// ('<f8') and int32 ('<i4') values ('|' for a byte order that does not matter is not one of them), C order only. The
// header is read as text and trusted for nothing: every value and every byte is checked against the bytes given. The
// values are copied as the machine holds them, little-endian, as every target of FMinus's is. It allocates nothing and
// waits for nothing: the audio thread may read one.
#include "runtime/Runtime.h"

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <limits>
#include <string_view>

namespace
{
    struct Header
    {
        char kind = 0;                 // 'f' or 'i'
        int size = 0;                  // 4 or 8 bytes a value
        std::int32_t dimensions = 0;
        std::int64_t shape[8] = {};
        std::int64_t count = 1;        // the values
        const unsigned char* values = nullptr;
    };

    // The text after `key` and its colon in the dict, without spaces: a view to its end, or empty.
    std::string_view valueOf(std::string_view header, std::string_view key)
    {
        const std::size_t at = header.find(key);
        if (at == std::string_view::npos)
            return {};
        std::string_view rest = header.substr(at + key.size());
        const std::size_t colon = rest.find_first_not_of(" ");
        if (colon == std::string_view::npos || rest[colon] != ':')
            return {};
        rest.remove_prefix(colon + 1);
        const std::size_t start = rest.find_first_not_of(" ");
        return start == std::string_view::npos ? std::string_view{} : rest.substr(start);
    }

    bool read(const void* data, std::int32_t bytes, Header& h)
    {
        if (data == nullptr || bytes < 10)
            return false;
        const auto* b = static_cast<const unsigned char*>(data);
        if (std::memcmp(b, "\x93NUMPY", 6) != 0)
            return false;
        const unsigned major = b[6];
        std::int64_t length = 0;
        std::int64_t start = 0;
        if (major == 1)
        {
            length = b[8] | (b[9] << 8);
            start = 10;
        }
        else if (major == 2 || major == 3)
        {
            if (bytes < 12)
                return false;
            length = static_cast<std::int64_t>(b[8]) | (static_cast<std::int64_t>(b[9]) << 8)
                     | (static_cast<std::int64_t>(b[10]) << 16) | (static_cast<std::int64_t>(b[11]) << 24);
            start = 12;
        }
        else
            return false;
        if (start + length > bytes)
            return false;
        const std::string_view header(reinterpret_cast<const char*>(b + start), static_cast<std::size_t>(length));
        // descr: '<f4', '<f8' or '<i4' (a quote of either kind around it)
        const std::string_view descr = valueOf(header, "'descr'");
        if (descr.size() < 5 || (descr[0] != '\'' && descr[0] != '"') || descr[4] != descr[0] || descr[1] != '<')
            return false;
        h.kind = descr[2];
        h.size = descr[3] - '0';
        if (!((h.kind == 'f' && (h.size == 4 || h.size == 8)) || (h.kind == 'i' && h.size == 4)))
            return false;
        if (!valueOf(header, "'fortran_order'").starts_with("False"))
            return false;
        // shape: a tuple of integers, (), (n,) or (a, b, ...)
        std::string_view shape = valueOf(header, "'shape'");
        if (shape.empty() || shape[0] != '(')
            return false;
        shape.remove_prefix(1);
        h.dimensions = 0;
        h.count = 1;
        for (;;)
        {
            const std::size_t at = shape.find_first_not_of(" ");
            if (at == std::string_view::npos)
                return false;
            shape.remove_prefix(at);
            if (shape[0] == ')')
                break;
            std::int64_t n = 0;
            std::size_t digits = 0;
            while (digits < shape.size() && shape[digits] >= '0' && shape[digits] <= '9')
            {
                n = n * 10 + (shape[digits] - '0');
                if (n > std::numeric_limits<std::int32_t>::max() || ++digits > 10)
                    return false;
            }
            if (digits == 0 || h.dimensions == 8)
                return false;
            h.shape[h.dimensions++] = n;
            h.count *= n;
            if (h.count > std::numeric_limits<std::int32_t>::max())
                return false;
            shape.remove_prefix(digits);
            const std::size_t next = shape.find_first_not_of(" ");
            if (next == std::string_view::npos)
                return false;
            shape.remove_prefix(next);
            if (shape[0] == ',')
                shape.remove_prefix(1);
            else if (shape[0] != ')')
                return false;
        }
        if (h.count * h.size > bytes - (start + length))
            return false;
        h.values = b + start + length;
        return true;
    }
} // namespace

extern "C" std::int32_t fminus_rt_npy_floats(const void* data, std::int32_t bytes, float* out,
                                             std::int32_t cap) noexcept
{
    Header h;
    if (!read(data, bytes, h))
        return -1;
    const std::int64_t written = out == nullptr || cap < 0 ? 0 : std::min<std::int64_t>(cap, h.count);
    for (std::int64_t k = 0; k < written; ++k)
    {
        const unsigned char* at = h.values + k * h.size;
        if (h.kind == 'f' && h.size == 4)
            std::memcpy(&out[k], at, 4);
        else if (h.kind == 'f')
        {
            double value = 0.0;
            std::memcpy(&value, at, 8);
            out[k] = static_cast<float>(value);
        }
        else
        {
            std::int32_t value = 0;
            std::memcpy(&value, at, 4);
            out[k] = static_cast<float>(value);
        }
    }
    return static_cast<std::int32_t>(h.count);
}

extern "C" std::int32_t fminus_rt_npy_shape(const void* data, std::int32_t bytes, std::int32_t axis) noexcept
{
    Header h;
    if (!read(data, bytes, h))
        return -1;
    if (axis == -1)
        return h.dimensions;
    if (axis < 0 || axis >= h.dimensions)
        return -1;
    return static_cast<std::int32_t>(h.shape[axis]);
}
