// src/runtime/Math.cpp: the math builtins' functions (Runtime.h), for the code generator's vector and libm math modes
// (ir/Math.cpp).
//
// The vector mode's promise is that a float gets one result wherever it is computed: alone, in any lane of a vector,
// beside any other lanes, on any target. Its functions are FMinus's own kernels (MathKernels.h): one sequence of IEEE
// operations each, which the scalar function runs on a float and the four-lane one on each lane of a vector at once,
// so a lane's result is the scalar function's by construction, whatever the other lanes hold, and the same on every
// target. MathTest.cpp checks it on every run, every lane against the scalar function.
//
// The libm mode's are the C library's, under names LLVM does not know.
#include "runtime/MathKernels.h"
#include "runtime/Runtime.h"

#include <cmath>

namespace
{
    using fminus::kernels::Scalar;
    using Four = fminus::kernels::Vector<4>;
} // namespace

extern "C" float fminus_rt_sinf(float x) noexcept
{
    Scalar o;
    return fminus::kernels::sin(o, x);
}

extern "C" float fminus_rt_cosf(float x) noexcept
{
    Scalar o;
    return fminus::kernels::cos(o, x);
}

extern "C" float fminus_rt_expf(float x) noexcept
{
    Scalar o;
    return fminus::kernels::exp(o, x);
}

extern "C" float fminus_rt_tanhf(float x) noexcept
{
    Scalar o;
    return fminus::kernels::tanh(o, x);
}

extern "C" float fminus_rt_logf(float x) noexcept
{
    Scalar o;
    return fminus::kernels::log(o, x);
}

extern "C" float fminus_rt_tanf(float x) noexcept
{
    Scalar o;
    return fminus::kernels::tan(o, x);
}

extern "C" fminus_rt_float4 fminus_rt_sin_f4(fminus_rt_float4 x) noexcept
{
    Four o;
    return fminus::kernels::sin(o, x);
}

extern "C" fminus_rt_float4 fminus_rt_cos_f4(fminus_rt_float4 x) noexcept
{
    Four o;
    return fminus::kernels::cos(o, x);
}

extern "C" fminus_rt_float4 fminus_rt_exp_f4(fminus_rt_float4 x) noexcept
{
    Four o;
    return fminus::kernels::exp(o, x);
}

extern "C" fminus_rt_float4 fminus_rt_tanh_f4(fminus_rt_float4 x) noexcept
{
    Four o;
    return fminus::kernels::tanh(o, x);
}

extern "C" fminus_rt_float4 fminus_rt_log_f4(fminus_rt_float4 x) noexcept
{
    Four o;
    return fminus::kernels::log(o, x);
}

extern "C" fminus_rt_float4 fminus_rt_tan_f4(fminus_rt_float4 x) noexcept
{
    Four o;
    return fminus::kernels::tan(o, x);
}

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

extern "C" float fminus_rt_libm_tanf(float x) noexcept
{
    return std::tan(x);
}
