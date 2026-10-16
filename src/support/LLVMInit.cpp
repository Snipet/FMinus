// src/support/LLVMInit.cpp
#include "support/LLVMInit.h"

#include <llvm/Support/TargetSelect.h>

#include <mutex>

namespace fminus
{
    void initializeLLVM()
    {
        static std::once_flag once;
        std::call_once(once, [] {
            llvm::InitializeNativeTarget();
            llvm::InitializeNativeTargetAsmPrinter();
            llvm::InitializeNativeTargetAsmParser();
            llvm::InitializeNativeTargetDisassembler();
        });
    }
} // namespace fminus
