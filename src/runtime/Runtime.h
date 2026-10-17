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
}
