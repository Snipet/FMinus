// src/runtime/Math.cpp: the math builtins' functions (Runtime.h), for the code generator's vector and libm math modes
// (ir/Math.cpp).
//
// The vector mode's promise is that a float gets one result wherever it is computed: alone, in any lane of a vector,
// beside any other lanes. On Apple systems the functions are libsystem_m's vector functions, _simd_sin_f4 and its
// kin (the ones behind <simd/math.h>), which are two to four times faster than libm's for four floats. Their exp, tanh
// and log keep the promise themselves: lane k of _simd_exp_f4(v) is lane 0 of _simd_exp_f4 of four copies of v[k], for
// every float in every lane beside any others (checked exhaustively on macOS 27, an M5; MathTest.cpp checks it again
// on every run). Their sin and cos don't: a vector takes a fast path when every lane is finite and at most 1e6 in
// magnitude (exactly 1,000,000: the next float takes the other), and a slow path for all four lanes otherwise, and
// the two paths give about 37% of arguments results up to 3 ulp apart. Each path alone is lane-independent. So
// fminus_rt_sin_f4 takes the fast path only when every lane is within kSinCosLimit, and otherwise computes each lane
// on its own, as the scalar function does; a lane's result then depends on that lane alone.
//
// Elsewhere there is no such library in the system, and the functions are libm's, a lane at a time: the vector mode
// is then the libm mode's results, and a vectorised loop of them is a loop of libm calls.
#include "runtime/Runtime.h"

#include <cmath>
#if defined(__APPLE__) && defined(__aarch64__)
#include <arm_neon.h>
#endif

#if defined(__APPLE__)
// libsystem_m's. The SDK declares them on clang's ext_vector_type(4) float, which the C ABI passes as it passes a
// vector_size(16) one, in one register. They are declared here, rather than through <simd/math.h>, which declares
// _simd_log_f4 only for a deployment target whose SIMD_LIBRARY_VERSION is 3 or later, though every macOS FMinus runs
// on exports it; and noexcept, as C functions are, so that a call of one from a noexcept function is a tail call
// rather than a call inside std::terminate's landing pad.
extern "C" fminus_rt_float4 _simd_sin_f4(fminus_rt_float4 x) noexcept;
extern "C" fminus_rt_float4 _simd_cos_f4(fminus_rt_float4 x) noexcept;
extern "C" fminus_rt_float4 _simd_exp_f4(fminus_rt_float4 x) noexcept;
extern "C" fminus_rt_float4 _simd_tanh_f4(fminus_rt_float4 x) noexcept;
extern "C" fminus_rt_float4 _simd_log_f4(fminus_rt_float4 x) noexcept;
#endif

namespace
{
#if defined(__APPLE__)
    fminus_rt_float4 splat(float x)
    {
        return fminus_rt_float4{x, x, x, x};
    }

    // The largest magnitude for which fminus_rt_sin_f4 and fminus_rt_cos_f4 hand a whole vector to libsystem_m: below
    // its fast path's edge, 1e6, so that every lane of such a vector is on that path, as it is in the vector of four
    // copies of itself that the scalar function computes. 2^16 leaves the edge room to come down by a factor of 15 in
    // a later macOS before the two would disagree, and costs audio nothing: its arguments are phases and angular
    // frequencies of a few radians, and a sine at 65536 radians has already lost its phase to float's spacing there
    // (2^-7 rad). A larger lane, an infinity or a NaN sends the vector lane by lane, four calls instead of one.
    constexpr float kSinCosLimit = 65536.0f;

    // Whether every lane is within kSinCosLimit, false for a NaN or an infinity. On arm64, the greatest magnitude, by
    // NEON's fmaxv, which (unlike fmaxnmv) gives NaN when a lane is NaN, and a NaN is not within anything: fabs, fmaxv,
    // fcmp and a branch, half of what the lane-by-lane comparison below compiles to. Elsewhere, lane by lane.
    bool withinLimit(fminus_rt_float4 x)
    {
#if defined(__aarch64__)
        return vmaxvq_f32(vabsq_f32(x)) <= kSinCosLimit;
#else
        const auto inside = (x <= kSinCosLimit) & (x >= -kSinCosLimit);   // all ones in a lane within it
        return (inside[0] & inside[1] & inside[2] & inside[3]) != 0;
#endif
    }
#endif

    // Each lane as the scalar function computes it. Out of line, so that fminus_rt_sin_f4's fast path, a tail call,
    // needs no stack frame.
    [[gnu::noinline]] fminus_rt_float4 eachLane(float (*function)(float) noexcept, fminus_rt_float4 x)
    {
        return fminus_rt_float4{function(x[0]), function(x[1]), function(x[2]), function(x[3])};
    }
} // namespace

#if defined(__APPLE__)
// A scalar call is lane 0 of the vector function of four copies of its argument. For sin and cos that is what
// fminus_rt_sin_f4 gives a lane on either of its paths: the copies are within kSinCosLimit together or not at all.
extern "C" float fminus_rt_sinf(float x) noexcept
{
    return _simd_sin_f4(splat(x))[0];
}

extern "C" float fminus_rt_cosf(float x) noexcept
{
    return _simd_cos_f4(splat(x))[0];
}

extern "C" float fminus_rt_expf(float x) noexcept
{
    return _simd_exp_f4(splat(x))[0];
}

extern "C" float fminus_rt_tanhf(float x) noexcept
{
    return _simd_tanh_f4(splat(x))[0];
}

extern "C" float fminus_rt_logf(float x) noexcept
{
    return _simd_log_f4(splat(x))[0];
}

extern "C" fminus_rt_float4 fminus_rt_sin_f4(fminus_rt_float4 x) noexcept
{
    return withinLimit(x) ? _simd_sin_f4(x) : eachLane(fminus_rt_sinf, x);
}

extern "C" fminus_rt_float4 fminus_rt_cos_f4(fminus_rt_float4 x) noexcept
{
    return withinLimit(x) ? _simd_cos_f4(x) : eachLane(fminus_rt_cosf, x);
}

// exp, tanh and log are lane-independent as they are. They go through the runtime all the same, one branch more than
// a call from the generated code: so the code generator's IR is the same on every target, the functions MathTest.cpp
// checks are the ones programs call, and a macOS on which one of them stopped being lane-independent would need a
// change here alone.
extern "C" fminus_rt_float4 fminus_rt_exp_f4(fminus_rt_float4 x) noexcept
{
    return _simd_exp_f4(x);
}

extern "C" fminus_rt_float4 fminus_rt_tanh_f4(fminus_rt_float4 x) noexcept
{
    return _simd_tanh_f4(x);
}

extern "C" fminus_rt_float4 fminus_rt_log_f4(fminus_rt_float4 x) noexcept
{
    return _simd_log_f4(x);
}
#else
extern "C" float fminus_rt_sinf(float x) noexcept
{
    return std::sin(x);
}

extern "C" float fminus_rt_cosf(float x) noexcept
{
    return std::cos(x);
}

extern "C" float fminus_rt_expf(float x) noexcept
{
    return std::exp(x);
}

extern "C" float fminus_rt_tanhf(float x) noexcept
{
    return std::tanh(x);
}

extern "C" float fminus_rt_logf(float x) noexcept
{
    return std::log(x);
}

extern "C" fminus_rt_float4 fminus_rt_sin_f4(fminus_rt_float4 x) noexcept
{
    return eachLane(fminus_rt_sinf, x);
}

extern "C" fminus_rt_float4 fminus_rt_cos_f4(fminus_rt_float4 x) noexcept
{
    return eachLane(fminus_rt_cosf, x);
}

extern "C" fminus_rt_float4 fminus_rt_exp_f4(fminus_rt_float4 x) noexcept
{
    return eachLane(fminus_rt_expf, x);
}

extern "C" fminus_rt_float4 fminus_rt_tanh_f4(fminus_rt_float4 x) noexcept
{
    return eachLane(fminus_rt_tanhf, x);
}

extern "C" fminus_rt_float4 fminus_rt_log_f4(fminus_rt_float4 x) noexcept
{
    return eachLane(fminus_rt_logf, x);
}
#endif

// libm's, every target. std::sin of a float is sinf.
extern "C" float fminus_rt_libm_sinf(float x) noexcept
{
    return std::sin(x);
}

extern "C" float fminus_rt_libm_cosf(float x) noexcept
{
    return std::cos(x);
}

extern "C" float fminus_rt_libm_expf(float x) noexcept
{
    return std::exp(x);
}

extern "C" float fminus_rt_libm_tanhf(float x) noexcept
{
    return std::tanh(x);
}

extern "C" float fminus_rt_libm_logf(float x) noexcept
{
    return std::log(x);
}
