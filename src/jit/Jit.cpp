// src/jit/Jit.cpp
#include "jit/Jit.h"

#include "support/HostTarget.h"
#include "support/JitRuntime.h"

#include <llvm/ExecutionEngine/Orc/JITTargetMachineBuilder.h>
#include <llvm/ExecutionEngine/Orc/LLJIT.h>
#include <llvm/ExecutionEngine/Orc/ThreadSafeModule.h>
#include <llvm/IR/LLVMContext.h>
#include <llvm/IR/Module.h>
#include <llvm/Target/TargetMachine.h>

namespace fminus::jit
{
    llvm::Expected<std::unique_ptr<Jit>> Jit::create(llvm::OptimizationLevel level, Modules modules)
    {
        const llvm::CodeGenOptLevel codegen = codeGenLevel(level);
        auto machine = hostTargetMachine(codegen);
        if (!machine)
            return machine.takeError();
        auto builder = llvm::orc::JITTargetMachineBuilder::detectHost();
        if (!builder)
            return builder.takeError();
        builder->setCodeGenOptLevel(codegen);
        auto lljit = llvm::orc::LLJITBuilder().setJITTargetMachineBuilder(std::move(*builder)).create();
        if (!lljit)
            return lljit.takeError();

        std::unique_ptr<Jit> jit(new Jit());
        jit->level_ = level;
        jit->machine_ = std::move(*machine);
        jit->jit_ = std::move(*lljit);
        if (llvm::Error error = defineRuntimeSymbols(*jit->jit_))
            return error;
        if (modules == Modules::Optimised)
            return jit;   // LLJIT's transform layer, with no transform set, hands each module on as it is

        Jit* self = jit.get();
        jit->jit_->getIRTransformLayer().setTransform(
            [self](llvm::orc::ThreadSafeModule tsm,
                   llvm::orc::MaterializationResponsibility&) -> llvm::Expected<llvm::orc::ThreadSafeModule> {
                tsm.withModuleDo(
                    [self](llvm::Module& module) { optimiseModule(module, *self->machine_, self->level_); });
                return std::move(tsm);
            });
        return jit;
    }

    Jit::~Jit() = default;

    llvm::Error Jit::add(std::unique_ptr<llvm::Module> module, std::unique_ptr<llvm::LLVMContext> context)
    {
        module->setDataLayout(jit_->getDataLayout());
        module->setTargetTriple(jit_->getTargetTriple());
        return jit_->addIRModule(llvm::orc::ThreadSafeModule(std::move(module), std::move(context)));
    }

    llvm::Expected<llvm::orc::ExecutorAddr> Jit::lookup(llvm::StringRef name)
    {
        return jit_->lookup(name);
    }
} // namespace fminus::jit
