// src/support/JitRuntime.h: hands the FMinus runtime (runtime/Runtime.h) to an LLJIT. An integration piece that came
// with the project skeleton.
#pragma once

#include <llvm/Support/Error.h>

namespace llvm::orc
{
    class LLJIT;
} // namespace llvm::orc

namespace fminus
{
    // Defines every function of runtime/RuntimeSymbols.def in the JIT's main JITDylib as an absolute symbol (its
    // address in this process), so JIT'd code calls the runtime an executable would link, with no dlsym into the host.
    llvm::Error defineRuntimeSymbols(llvm::orc::LLJIT& jit);
} // namespace fminus
