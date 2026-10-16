// src/support/LLVMInit.h: registers the host target with LLVM, once per process. An integration helper that came with
// the project skeleton: keep it, change it or replace it.
#pragma once

namespace fminus
{
    // Registers the host's code generator, assembly printer, assembly parser and disassembler (LLVM's
    // InitializeNativeTarget* functions). Call it before creating a JIT or a TargetMachine. It is safe to call any
    // number of times from any thread; only the first call does the work.
    void initializeLLVM();
} // namespace fminus
