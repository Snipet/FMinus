// src/support/HostTarget.cpp
#include "support/HostTarget.h"

#include "opt/KnobVersioning.h"
#include "opt/ZeroInitElision.h"
#include "support/LLVMInit.h"

#include <llvm/Analysis/TargetLibraryInfo.h>
#include <llvm/Analysis/TargetTransformInfo.h>
#include <llvm/CodeGen/ReplaceWithVeclib.h>
#include <llvm/ExecutionEngine/Orc/JITTargetMachineBuilder.h>
#include <llvm/IR/LLVMContext.h>
#include <llvm/IR/Metadata.h>
#include <llvm/IR/Module.h>
#include <llvm/IR/SystemLibraries.h>
#include <llvm/Passes/PassBuilder.h>
#include <llvm/Support/CommandLine.h>
#include <llvm/Target/TargetMachine.h>

#include <algorithm>
#include <mutex>

namespace fminus
{
    namespace
    {
        // The SLP vectoriser's cost threshold: how much cheaper, in its cost model's units, a tree of packed
        // operations must be than the scalar code it replaces (LLVM's default is 0). The model counts instructions,
        // not the latency of a chain that carries a value from one sample to the next. At 0 it packs a stereo filter's
        // two channels, independent recurrences, into one vector, with shuffles on the path from each sample to the
        // next: one longer chain where the core ran two side by side. A frozen stereo state-variable filter's loop then
        // takes 31 cycles a sample instead of 20 (llvm-mca, apple-m5). From 10, on every example and the plugins'
        // programs measured, SLP leaves such loops scalar, the code it would give with SLP off, and it still packs what
        // pays: four calls of one math builtin into one four-lane call (up to a threshold of 40), and a float4 built
        // from scalars (up to 15). docs/simd.md, "The auto-vectorisers", has the measurements.
        constexpr const char* kSlpThreshold = "10";

        // LLVM's vectorisers take their thresholds from its command-line options, which are the process's: set
        // slp-threshold once, before the first pipeline runs, unless the host set it on LLVM's command line itself.
        void raiseSlpThreshold()
        {
            static std::once_flag once;
            std::call_once(once, [] {
                auto& options = llvm::cl::getRegisteredOptions();
                const auto found = options.find("slp-threshold");
                if (found != options.end() && found->second->getNumOccurrences() == 0)
                    found->second->addOccurrence(0, "slp-threshold", kSlpThreshold);
            });
        }
    } // namespace

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
        raiseSlpThreshold();
        llvm::PassBuilder passes(&machine, tuning);

        // A vector math library, in the fast math mode only (the module's kMathFlag; HostTarget.h). On Apple systems,
        // libsystem_m's _simd_sin_f4, _simd_exp_f4, ... (in libSystem, which every process and executable links
        // already): the target library analysis tells the loop vectoriser about them, and ReplaceWithVeclib, last,
        // turns the vector intrinsics that float4 builtins lower to (llvm.sin.v4f32) into calls to them, where the
        // backend would call sinf once per lane. They are about three times faster, and up to 3 ulp from the exact
        // result where sinf is within 1. They take four lanes only: a float2 or float8 call is still one sinf per
        // lane. (Accelerate's mapping, the other on Apple systems, also replaces llvm.fabs and llvm.sqrt, which NEON
        // does in one instruction, with calls.) The vector and libm modes' calls are the runtime's functions, which
        // neither the library analysis nor ReplaceWithVeclib knows; the vector mode's name their vector variants
        // themselves (ir/Math.cpp).
        const auto* math = llvm::dyn_cast_or_null<llvm::MDString>(module.getModuleFlag(kMathFlag));
        const bool vectorLibrary = math == nullptr || math->getString() == "fast";
        llvm::TargetLibraryInfoImpl library(machine.getTargetTriple());
        if (vectorLibrary && machine.getTargetTriple().isOSDarwin())
            library.addVectorizableFunctionsFromVecLib(llvm::VectorLibrary::DarwinLibSystemM,
                                                       machine.getTargetTriple());
        functions.registerPass([&] { return llvm::TargetLibraryAnalysis(library); });   // before the default one
        if (vectorLibrary)
            passes.registerOptimizerLastEPCallback(
                [](llvm::ModulePassManager& modulePasses, llvm::OptimizationLevel, llvm::ThinOrFullLTOPhase) {
                    modulePasses.addPass(llvm::createModuleToFunctionPassAdaptor(llvm::ReplaceWithVeclib()));
                });

        // ---- Zero-init elision (src/opt/ZeroInitElision.h) ----------------------------------------------------------
        // FMinus zeroes every local array where it is declared; this drops the memset when the program writes every
        // element it reads first. At the end of each function's simplification, from -O1 (LLVM's -O0 pipeline calls
        // this callback too, and the pass stays out of it: -O0 keeps every memset), after the inliner has brought its
        // callees in: the loop passes have rotated the loops and given them their induction variables, so
        // ScalarEvolution can count their iterations, and at -O2 and -O3 LICM's loop adaptor has just put them back in
        // loop-simplify form and DSE has removed what it can. It runs before the loop vectoriser, which would split a
        // store loop into vector and remainder loops.
        passes.registerScalarOptimizerLateEPCallback(
            [](llvm::FunctionPassManager& functionPasses, llvm::OptimizationLevel level) {
                if (level != llvm::OptimizationLevel::O0)
                    functionPasses.addPass(opt::ZeroInitElisionPass());
            });
        // ---- end of zero-init elision ------------------------------------------------------------------------------
        // ---- Knob versioning (opt/KnobVersioning.h), from -O1 -------------------------------------------------------
        // A method whose loop branches on an int or bool knob, or on whether a stream is flat, gets a copy of its body
        // for each value. Early: after the pipeline's first SimplifyCFG, SROA and EarlyCSE, which turn the code
        // generator's stack slots into the SSA values the pass follows from a knob to its branch, and before IPSCCP and
        // the inliner, so each method is versioned on its own knobs, and the whole pipeline then simplifies each copy.
        passes.registerPipelineEarlySimplificationEPCallback(
            [](llvm::ModulePassManager& modulePasses, llvm::OptimizationLevel level, llvm::ThinOrFullLTOPhase) {
                if (level != llvm::OptimizationLevel::O0)
                    modulePasses.addPass(llvm::createModuleToFunctionPassAdaptor(opt::KnobVersioningPass()));
            });
        // ---- end of knob versioning ---------------------------------------------------------------------------------

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
