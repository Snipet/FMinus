// src/runtime/Fft.cpp: the FFT of Runtime.h, on PFFFT (cmake/FMinusFft.cmake), which is compiled into this library. A
// plan holds PFFFT's setup and three buffers of its own, aligned for PFFFT's SIMD and touched when the plan is made:
// so a transform never allocates, and the caller's buffers need neither that alignment nor PFFFT's formats.
//
// PFFFT's real transform packs its n/2 + 1 bins into n floats: bin 0 and bin n/2, which are real, share the first
// pair, as F(0) + i F(n/2). A plan unpacks them into FMinus's n/2 + 1 (re, im) pairs, n + 2 floats, and packs them back
// for the inverse (whose bin 0 and bin n/2 it takes as real, as FFTW does). A complex transform's format is PFFFT's
// own already: n (re, im) pairs. Neither direction scales, as PFFFT's don't.
#include "runtime/Runtime.h"

#include <pffft/pffft.h>

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <initializer_list>
#include <new>

namespace
{
    enum class Kind
    {
        Real,
        Complex,
    };

    struct Plan
    {
        Kind kind;
        std::size_t n;
        PFFFT_Setup* setup;
        float* work;   // PFFFT's scratch: n floats, 2n for a complex plan
        float* in;     // the input, copied when the caller's is not aligned, or the bins packed for an inverse
        float* out;    // the output, before it is unpacked or copied out
    };

    // pffft_new_setup asserts past 2^26 points; a plan refuses them first.
    constexpr std::int32_t kMaxPoints = 1 << 26;

    // PFFFT's SIMD wants its buffers on 16 bytes (SSE's aligned loads; NEON's are only faster there).
    bool aligned(const void* p)
    {
        return reinterpret_cast<std::uintptr_t>(p) % 16 == 0;
    }

    void release(Plan* plan)
    {
        if (plan->setup != nullptr)
            pffft_destroy_setup(plan->setup);
        pffft_aligned_free(plan->work);
        pffft_aligned_free(plan->in);
        pffft_aligned_free(plan->out);
        delete plan;
    }

    void* create(std::int32_t n, Kind kind)
    {
        if (n <= 0 || n > kMaxPoints)
            return nullptr;
        // Null for a size PFFFT does not take: a multiple of 32 points (16 complex), and of no prime but 2, 3 and 5.
        PFFFT_Setup* setup = pffft_new_setup(n, kind == Kind::Real ? PFFFT_REAL : PFFFT_COMPLEX);
        if (setup == nullptr)
            return nullptr;
        Plan* plan = new (std::nothrow) Plan{kind, static_cast<std::size_t>(n), setup, nullptr, nullptr, nullptr};
        if (plan == nullptr)
        {
            pffft_destroy_setup(setup);
            return nullptr;
        }
        const std::size_t bytes = plan->n * (kind == Kind::Real ? 1 : 2) * sizeof(float);
        for (float** buffer : {&plan->work, &plan->in, &plan->out})
        {
            *buffer = static_cast<float*>(pffft_aligned_malloc(bytes));
            if (*buffer == nullptr)
            {
                release(plan);
                return nullptr;
            }
            std::memset(*buffer, 0, bytes);   // touched now, so the audio thread takes no page fault on it
        }
        return plan;
    }

    // `plan` as a plan of `kind`, or nullptr (a null plan, or one of the other kind: the transform does nothing).
    const Plan* as(void* plan, Kind kind)
    {
        const auto* p = static_cast<const Plan*>(plan);
        return p != nullptr && p->kind == kind ? p : nullptr;
    }

    // A complex transform: PFFFT's ordered format is FMinus's, so the buffers go straight through when aligned.
    void complexTransform(void* plan, const float* in, float* out, pffft_direction_t direction)
    {
        const Plan* p = as(plan, Kind::Complex);
        if (p == nullptr)
            return;
        const std::size_t floats = 2 * p->n;
        const float* input = in;
        if (!aligned(in))
        {
            std::memcpy(p->in, in, floats * sizeof(float));
            input = p->in;
        }
        float* output = aligned(out) ? out : p->out;
        pffft_transform_ordered(p->setup, input, output, p->work, direction);
        if (output != out)
            std::memcpy(out, output, floats * sizeof(float));
    }
} // namespace

extern "C" void* fminus_rt_fft_create(std::int32_t n) noexcept
{
    return create(n, Kind::Real);
}

extern "C" void* fminus_rt_fft_create_complex(std::int32_t n) noexcept
{
    return create(n, Kind::Complex);
}

extern "C" void fminus_rt_fft_destroy(void* plan) noexcept
{
    if (plan != nullptr)
        release(static_cast<Plan*>(plan));
}

// n reals into n/2 + 1 bins: PFFFT's packed bins, unpacked. They are in place but for bin n/2, which moves from the
// first pair to the last, so an aligned `out` takes PFFFT's output directly.
extern "C" void fminus_rt_fft_forward_real(void* plan, const float* in, float* out) noexcept
{
    const Plan* p = as(plan, Kind::Real);
    if (p == nullptr)
        return;
    const std::size_t n = p->n;
    const float* input = in;
    if (!aligned(in))
    {
        std::memcpy(p->in, in, n * sizeof(float));
        input = p->in;
    }
    float* bins = aligned(out) ? out : p->out;
    pffft_transform_ordered(p->setup, input, bins, p->work, PFFFT_FORWARD);
    const float nyquist = bins[1];
    if (bins != out)
        std::memcpy(out, bins, n * sizeof(float));
    out[1] = 0.0f;   // bin 0 is real
    out[n] = nyquist;
    out[n + 1] = 0.0f;
}

// n/2 + 1 bins into n reals: the bins packed as PFFFT has them (bin 0's and bin n/2's imaginary parts dropped).
extern "C" void fminus_rt_fft_inverse_real(void* plan, const float* in, float* out) noexcept
{
    const Plan* p = as(plan, Kind::Real);
    if (p == nullptr)
        return;
    const std::size_t n = p->n;
    p->in[0] = in[0];
    p->in[1] = in[n];
    std::memcpy(p->in + 2, in + 2, (n - 2) * sizeof(float));
    float* output = aligned(out) ? out : p->out;
    pffft_transform_ordered(p->setup, p->in, output, p->work, PFFFT_BACKWARD);
    if (output != out)
        std::memcpy(out, output, n * sizeof(float));
}

extern "C" void fminus_rt_fft_forward_complex(void* plan, const float* in, float* out) noexcept
{
    complexTransform(plan, in, out, PFFFT_FORWARD);
}

extern "C" void fminus_rt_fft_inverse_complex(void* plan, const float* in, float* out) noexcept
{
    complexTransform(plan, in, out, PFFFT_BACKWARD);
}
