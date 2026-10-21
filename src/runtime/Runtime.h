// src/runtime/Runtime.h: the FMinus runtime, the C functions generated code calls. It builds as its own static
// library, libfminus_runtime.a (FMinus::Runtime), with no LLVM in it. A process that JITs links it and hands each
// function to LLJIT by address (support/JitRuntime.h); an executable from fminus::aot links the library itself, so
// JIT'd and ahead-of-time code call the same functions. Every function here is listed in RuntimeSymbols.def.
//
// An integration piece that came with the project skeleton: the ABI version below is a placeholder for the real
// runtime. Keep at least one source in runtime/ (the library cannot be empty).
#pragma once

#include <cstdint>

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
}
