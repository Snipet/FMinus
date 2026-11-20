// src/runtime/Print.cpp: printf and eprintf (Runtime.h), C's printf to stdout and to stderr, the text made by
// Printf.h's formatPrintf from a format the code generator checked (ir/Print.cpp) and the values it passes, every
// number a double and, for the _s functions, every %s's string in two arrays. A call's text goes to the C library's
// stream under the stream's lock, so the text of two calls from two threads (a parallel for's iterations) never
// interleaves, as POSIX promises of C's printf. The stream is C's: buffered as C buffers it (a line at a time to a
// terminal, in blocks to a pipe or a file; stderr not at all), and flushed when the process exits, so an executable's
// text and main's printed result come out in the order the program made them.
//
// The C library's stream may allocate its buffer on first use, and writing blocks while a pipe is full: the effects
// count printf and eprintf as allocating (ir/Calls.cpp), refused in a nonallocating or nonblocking function.
#include "runtime/Printf.h"
#include "runtime/Runtime.h"

#include <cstdio>
#include <cstring>
#include <limits>

namespace
{
    // Writes into the stream through a buffer of its own, counting the bytes, with the stream's lock held for the whole
    // call (C's lock of a stream is recursive, so fwrite takes it again inside).
    class StreamWriter
    {
      public:
        explicit StreamWriter(std::FILE* stream) noexcept
            : stream_(stream)
        {
#if defined(_WIN32)
            _lock_file(stream_);
#else
            flockfile(stream_);
#endif
        }

        ~StreamWriter()
        {
#if defined(_WIN32)
            _unlock_file(stream_);
#else
            funlockfile(stream_);
#endif
        }

        StreamWriter(const StreamWriter&) = delete;
        StreamWriter& operator=(const StreamWriter&) = delete;

        void put(char c) noexcept
        {
            if (used_ == sizeof buffer_)
                flush();
            buffer_[used_++] = c;
        }

        void put(const char* first, const char* last) noexcept
        {
            while (first != last)
            {
                if (used_ == sizeof buffer_)
                    flush();
                std::size_t n = static_cast<std::size_t>(last - first);
                if (n > sizeof buffer_ - used_)
                    n = sizeof buffer_ - used_;
                std::memcpy(buffer_ + used_, first, n);
                used_ += n;
                first += n;
            }
        }

        // The bytes written, or -1 when the stream refused some (C's printf's result).
        std::int32_t finish() noexcept
        {
            flush();
            if (failed_)
                return -1;
            return written_ > static_cast<std::int64_t>(std::numeric_limits<std::int32_t>::max())
                       ? std::numeric_limits<std::int32_t>::max()
                       : static_cast<std::int32_t>(written_);
        }

      private:
        void flush() noexcept
        {
            if (used_ == 0)
                return;
            const std::size_t done = std::fwrite(buffer_, 1, used_, stream_);   // the lock is recursive
            written_ += static_cast<std::int64_t>(done);
            failed_ = failed_ || done != used_;
            used_ = 0;
        }

        std::FILE* stream_;
        char buffer_[512];
        std::size_t used_ = 0;
        std::int64_t written_ = 0;
        bool failed_ = false;
    };

    std::int32_t print(std::FILE* stream, const char* format, std::int32_t length, const double* values,
                       std::int32_t count, const fminus::runtime::PrintTexts* texts) noexcept
    {
        StreamWriter out(stream);
        fminus::runtime::formatPrintf(out, format, length, values, count, texts);
        return out.finish();
    }
} // namespace

extern "C" std::int32_t fminus_rt_printf(const char* format, std::int32_t length, const double* values,
                                         std::int32_t count) noexcept
{
    return print(stdout, format, length, values, count, nullptr);
}

extern "C" std::int32_t fminus_rt_printf_s(const char* format, std::int32_t length, const double* values,
                                           std::int32_t count, const char* const* data, const std::int32_t* lengths,
                                           std::int32_t texts) noexcept
{
    const fminus::runtime::PrintTexts strings{data, lengths, texts};
    return print(stdout, format, length, values, count, &strings);
}

extern "C" std::int32_t fminus_rt_eprintf(const char* format, std::int32_t length, const double* values,
                                          std::int32_t count) noexcept
{
    return print(stderr, format, length, values, count, nullptr);
}

extern "C" std::int32_t fminus_rt_eprintf_s(const char* format, std::int32_t length, const double* values,
                                            std::int32_t count, const char* const* data, const std::int32_t* lengths,
                                            std::int32_t texts) noexcept
{
    const fminus::runtime::PrintTexts strings{data, lengths, texts};
    return print(stderr, format, length, values, count, &strings);
}
