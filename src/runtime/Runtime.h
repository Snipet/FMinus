// src/runtime/Runtime.h: the FMinus runtime, the C functions generated code calls. It builds as its own static
// library, libfminus_runtime.a (FMinus::Runtime), with no LLVM in it. A process that JITs links it and hands each
// function to LLJIT by address (support/JitRuntime.h); an executable from fminus::aot links the library itself, so
// JIT'd and ahead-of-time code call the same functions. Every function here is listed in RuntimeSymbols.def.
//
// An integration piece that came with the project skeleton: the ABI version below is a placeholder for the real
// runtime. Keep at least one source in runtime/ (the library cannot be empty).
#pragma once

#include <cstdint>

// Four floats in one vector register, as FMinus's float4 is (LLVM's <4 x float>): GCC's and clang's vector of 16 bytes,
// which the C ABI passes and returns in one register on arm64 and x86-64, as it does clang's ext_vector_type(4).
typedef float fminus_rt_float4 __attribute__((vector_size(16)));

// A parallel region's body (ir/Parallel.cpp): runs its iterations, or its statements, begin to end - 1, over what its
// context holds.
typedef void (*fminus_rt_task)(void* context, std::int32_t begin, std::int32_t end);

extern "C"
{
    // The runtime's ABI version, for generated code to check against the one it was compiled for.
    std::int32_t fminus_rt_abi_version() noexcept;

    // An executable's start (fminus::aot::addEntryPoint), before it calls the program's main. Reads main's `count`
    // arguments from the command line into `args`, each as C's strtof reads it, and returns 0; or tells stderr what is
    // wrong (the wrong number of arguments, or one that is not a number) and returns 2, the executable's exit status.
    std::int32_t fminus_rt_main_args(std::int32_t argc, char** argv, std::int32_t count, float* args) noexcept;

    // An executable's end: prints main's result and a newline on stdout as `fminus --run` prints a result, the
    // shortest decimal that reads back as the same float.
    void fminus_rt_main_result(float result) noexcept;

    // The same for a main that returns bool: prints true or false.
    void fminus_rt_main_bool_result(bool result) noexcept;

    // The heap, for FMinus's malloc, calloc and free: count * size bytes (malloc: 1 * size), aligned to 64 bytes (a
    // cache line, and the alignment of the widest vector, AVX-512's) and zeroed, as every variable FMinus declares
    // starts at 0, and touched, so no page faults in later. Null when the count or the size is not positive, the
    // product is more than 16 GiB (a mistake: zeroing it would take the machine's memory), or the memory is not there.
    // The memory comes from C's aligned_alloc, so C's free releases it too, and fminus_rt_free releases C's malloc's.
    // On Windows, whose C runtime has no aligned_alloc, it comes from _aligned_malloc: only _aligned_free (which
    // fminus_rt_free calls there) releases it, and fminus_rt_free cannot release malloc's.
    // Not for the audio thread: allocating takes a lock in the system's allocator, and can take a page fault.
    void* fminus_rt_malloc(std::int64_t size) noexcept;
    void* fminus_rt_calloc(std::int64_t count, std::int64_t size) noexcept;
    void fminus_rt_free(void* memory) noexcept;

    // The FFT (Fft.cpp), for FMinus's fft_create, fft_create_complex, fft_forward, fft_inverse and fft_destroy. A plan
    // is for transforms of n points: real (n floats into n/2 + 1 complex bins, each a (re, im) pair of floats, and
    // back) or complex (n complex values into n, and back). Neither direction scales, so inverse(forward(x)) is n times
    // x. Making a plan allocates (its tables, and buffers for the transforms) and takes time: not for the audio thread.
    // It is null when n is not a size the library takes. A transform allocates nothing, takes no lock, reads `in` (n
    // floats, n/2 + 1 or n complex values, as the plan and the direction say) and writes `out`, which must not overlap
    // it; a null plan, or a plan of the other kind, does nothing.
    void* fminus_rt_fft_create(std::int32_t n) noexcept;
    void* fminus_rt_fft_create_complex(std::int32_t n) noexcept;
    void fminus_rt_fft_destroy(void* plan) noexcept;
    void fminus_rt_fft_forward_real(void* plan, const float* in, float* out) noexcept;
    void fminus_rt_fft_inverse_real(void* plan, const float* in, float* out) noexcept;
    void fminus_rt_fft_forward_complex(void* plan, const float* in, float* out) noexcept;
    void fminus_rt_fft_inverse_complex(void* plan, const float* in, float* out) noexcept;

    // The math builtins sinf, cosf, expf, tanhf and logf (Math.cpp), as the code generator's math modes call them
    // (ir::Options::math, ir/Math.cpp). None allocates, locks or makes a system call; each is a function of its
    // argument alone, which the code generator tells LLVM (it reads no memory and is defined for every argument).
    //
    // The vector mode's, FMinus's default: FMinus's own kernels (MathKernels.h), the same IEEE operations on every
    // target. A lane of a four-lane function is the scalar function of that lane, bit for bit, whatever the other lanes
    // hold: so a float gets one result in a scalar loop and in its vectorised body, in a float4, and in a poly float of
    // any width, on arm64 and on x86-64.
    float fminus_rt_sinf(float x) noexcept;
    float fminus_rt_cosf(float x) noexcept;
    float fminus_rt_expf(float x) noexcept;
    float fminus_rt_tanhf(float x) noexcept;
    float fminus_rt_logf(float x) noexcept;
    fminus_rt_float4 fminus_rt_sin_f4(fminus_rt_float4 x) noexcept;
    fminus_rt_float4 fminus_rt_cos_f4(fminus_rt_float4 x) noexcept;
    fminus_rt_float4 fminus_rt_exp_f4(fminus_rt_float4 x) noexcept;
    fminus_rt_float4 fminus_rt_tanh_f4(fminus_rt_float4 x) noexcept;
    fminus_rt_float4 fminus_rt_log_f4(fminus_rt_float4 x) noexcept;

    // FMinus's parallel for and parallel { } (Parallel.cpp): runs task(context, ...) over the iterations begin to
    // end - 1, spread over the pool's threads and the calling thread, in chunks each thread takes as it finishes its
    // last, and returns when every one has run. `threads` is the most threads that take part, the caller's among them;
    // 0 is all there are. Every chunk runs in the caller's floating-point mode (FPCR on arm64, MXCSR on x86-64). It runs
    // the whole range on the calling thread, in order, when the pool is not started, when `threads` is 1, for one
    // iteration, inside a chunk of another region the pool runs, and when another region holds the pool (a host's
    // other audio thread's). It allocates nothing and takes no lock, so the audio thread may call it: at the end it
    // spins for the chunks other threads are still running, and pool threads asleep (no region for 50 us) it wakes
    // with a system call that does not block.
    void fminus_rt_parallel_for(fminus_rt_task task, void* context, std::int32_t begin, std::int32_t end,
                                std::int32_t threads) noexcept;

    // The pool, started, or one more user of it counted: a thread for every core but one, asleep until a region
    // needs it. Each start needs its stop. Not for the audio thread: it makes threads. Returns how many threads a
    // region can run on, the caller's among them (1 on a machine of one core).
    std::int32_t fminus_rt_parallel_start() noexcept;

    // A start's end. The last stops the pool's threads and waits for them: no region may be running then.
    void fminus_rt_parallel_stop() noexcept;

    // The libm mode's: libm's sinf, cosf, ..., which C++'s std::sin of a float calls, under names LLVM does not know,
    // so that it neither folds nor vectorises them.
    float fminus_rt_libm_sinf(float x) noexcept;
    float fminus_rt_libm_cosf(float x) noexcept;
    float fminus_rt_libm_expf(float x) noexcept;
    float fminus_rt_libm_tanhf(float x) noexcept;
    float fminus_rt_libm_logf(float x) noexcept;
}
