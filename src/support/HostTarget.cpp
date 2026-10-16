// src/support/HostTarget.cpp
#include "support/HostTarget.h"

#include "support/LLVMInit.h"

#include <llvm/ExecutionEngine/Orc/JITTargetMachineBuilder.h>
#include <llvm/IR/Module.h>
#include <llvm/Passes/PassBuilder.h>
#include <llvm/Target/TargetMachine.h>

namespace fminus
{
    llvm::Expected<std::unique_ptr<llvm::TargetMachine>> hostTargetMachine(llvm::CodeGenOptLevel level)
    {
        initializeLLVM();
        auto builder = llvm::orc::JITTargetMachineBuilder::detectHost();
        if (!builder)
            return builder.takeError();
        builder->setCodeGenOptLevel(level);
        return builder->createTargetMachine();
    }

    llvm::CodeGenOptLevel codeGenLevel(llvm::OptimizationLevel level)
    {
        if (level == llvm::OptimizationLevel::O0)
            return llvm::CodeGenOptLevel::None;
        if (level == llvm::OptimizationLevel::O1)
            return llvm::CodeGenOptLevel::Less;
        if (level == llvm::OptimizationLevel::O2)
            return llvm::CodeGenOptLevel::Default;
        return llvm::CodeGenOptLevel::Aggressive;
    }

    void prepareModule(llvm::Module& module, const llvm::TargetMachine& machine)
    {
        module.setTargetTriple(machine.getTargetTriple());
        module.setDataLayout(machine.createDataLayout());
    }

    void optimiseModule(llvm::Module& module, llvm::TargetMachine& machine, llvm::OptimizationLevel level)
    {
        // Declared in this order so they are destroyed in the reverse one (they refer to each other).
        llvm::LoopAnalysisManager loops;
        llvm::FunctionAnalysisManager functions;
        llvm::CGSCCAnalysisManager cgsccs;
        llvm::ModuleAnalysisManager modules;

        llvm::PipelineTuningOptions tuning;
        tuning.LoopVectorization = true;
        tuning.SLPVectorization = true;
        llvm::PassBuilder passes(&machine, tuning);
        passes.registerModuleAnalyses(modules);
        passes.registerCGSCCAnalyses(cgsccs);
        passes.registerFunctionAnalyses(functions);
        passes.registerLoopAnalyses(loops);
        passes.crossRegisterProxies(loops, functions, cgsccs, modules);
        passes.buildPerModuleDefaultPipeline(level).run(module, modules);
    }
} // namespace fminus
