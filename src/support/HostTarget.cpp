// src/support/HostTarget.cpp
#include "support/HostTarget.h"

#include "support/LLVMInit.h"

#include <llvm/Analysis/TargetLibraryInfo.h>
#include <llvm/Analysis/TargetTransformInfo.h>
#include <llvm/CodeGen/ReplaceWithVeclib.h>
#include <llvm/ExecutionEngine/Orc/JITTargetMachineBuilder.h>
#include <llvm/IR/LLVMContext.h>
#include <llvm/IR/Module.h>
#include <llvm/IR/SystemLibraries.h>
#include <llvm/Passes/PassBuilder.h>
#include <llvm/Target/TargetMachine.h>

#include <algorithm>

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

        // A vector math library. On Apple systems, libsystem_m's _simd_sin_f4, _simd_exp_f4, ... (in libSystem, which
        // every process and executable links already): the target library analysis tells the loop vectoriser about
        // them, and ReplaceWithVeclib, last, turns the vector intrinsics that float4 builtins lower to
        // (llvm.sin.v4f32) into calls to them, where the backend would call sinf once per lane. They are about three
        // times faster, and up to 3 ulp from the exact result where sinf is within 1. They take four lanes only: a
        // float2 or float8 call is still one sinf per lane. (Accelerate's mapping, the other on Apple systems, also
        // replaces llvm.fabs and llvm.sqrt, which NEON does in one instruction, with calls.)
        llvm::TargetLibraryInfoImpl library(machine.getTargetTriple());
        if (machine.getTargetTriple().isOSDarwin())
            library.addVectorizableFunctionsFromVecLib(llvm::VectorLibrary::DarwinLibSystemM,
                                                       machine.getTargetTriple());
        functions.registerPass([&] { return llvm::TargetLibraryAnalysis(library); });   // before the default one
        passes.registerOptimizerLastEPCallback(
            [](llvm::ModulePassManager& modulePasses, llvm::OptimizationLevel, llvm::ThinOrFullLTOPhase) {
                modulePasses.addPass(llvm::createModuleToFunctionPassAdaptor(llvm::ReplaceWithVeclib()));
            });

        passes.registerModuleAnalyses(modules);
        passes.registerCGSCCAnalyses(cgsccs);
        passes.registerFunctionAnalyses(functions);
        passes.registerLoopAnalyses(loops);
        passes.crossRegisterProxies(loops, functions, cgsccs, modules);
        passes.buildPerModuleDefaultPipeline(level).run(module, modules);
    }

    unsigned polyLanes(const llvm::TargetMachine& machine)
    {
        // The cost model asks a function, whose attributes may pick another CPU: one without any has the machine's.
        llvm::LLVMContext context;
        llvm::Module module("fminus.poly-lanes", context);
        llvm::Function* function =
            llvm::Function::Create(llvm::FunctionType::get(llvm::Type::getVoidTy(context), false),
                                   llvm::GlobalValue::ExternalLinkage, "f", module);
        const llvm::TargetTransformInfo info = machine.getTargetTransformInfo(*function);
        const llvm::TypeSize bits = info.getRegisterBitWidth(llvm::TargetTransformInfo::RGK_FixedWidthVector);
        return std::max(1u, static_cast<unsigned>(bits.getFixedValue() / 32));
    }
} // namespace fminus
