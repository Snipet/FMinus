// src/support/JitRuntime.cpp
#include "support/JitRuntime.h"

#include "runtime/Runtime.h"
#if FMINUS_HAS_WEB
#include "web/Web.h"   // the web library's functions, web/WebSymbols.def
#endif

#include <llvm/ExecutionEngine/Orc/AbsoluteSymbols.h>
#include <llvm/ExecutionEngine/Orc/LLJIT.h>

namespace fminus
{
    llvm::Error defineRuntimeSymbols(llvm::orc::LLJIT& jit)
    {
        const llvm::JITSymbolFlags flags = llvm::JITSymbolFlags::Exported | llvm::JITSymbolFlags::Callable;
        llvm::orc::SymbolMap symbols;
#define FMINUS_RUNTIME_SYMBOL(name) \
    symbols[jit.mangleAndIntern(#name)] = {llvm::orc::ExecutorAddr::fromPtr(&name), flags};
#include "runtime/RuntimeSymbols.def"
#if FMINUS_HAS_WEB
#include "web/WebSymbols.def"
#endif
#undef FMINUS_RUNTIME_SYMBOL
        return jit.getMainJITDylib().define(llvm::orc::absoluteSymbols(std::move(symbols)));
    }
} // namespace fminus
