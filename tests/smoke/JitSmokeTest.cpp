// tests/smoke/JitSmokeTest.cpp: the LLVM integration, end to end. It shows four things:
//   - ORC's LLJIT targets the host CPU and its features;
//   - the new pass manager's O3 pipeline runs through LLJIT's IR transform layer (LLJIT does no IR optimisation of its
//     own);
//   - JIT'd code calls a host function, which is defined as an absolute symbol (no dlsym into the host process);
//   - the loop vectoriser uses the host's vector unit.
// It came with the project skeleton. Keep it as a toolchain check, or delete it once the real compiler covers this.
#include "support/LLVMInit.h"

#include <catch2/catch_test_macros.hpp>

#include <llvm/AsmParser/Parser.h>
#include <llvm/ExecutionEngine/Orc/JITTargetMachineBuilder.h>
#include <llvm/ExecutionEngine/Orc/LLJIT.h>
#include <llvm/ExecutionEngine/Orc/ThreadSafeModule.h>
#include <llvm/IR/LLVMContext.h>
#include <llvm/IR/Module.h>
#include <llvm/IR/Verifier.h>
#include <llvm/Passes/PassBuilder.h>
#include <llvm/Support/Error.h>
#include <llvm/Support/SourceMgr.h>
#include <llvm/Support/raw_ostream.h>
#include <llvm/Target/TargetMachine.h>
#include <llvm/TargetParser/Host.h>

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace
{
    // The host function the JIT'd code calls.
    float hostDouble(float x)
    {
        return 2.0f * x;
    }

    constexpr const char* kSmokeIR = R"IR(
declare float @fminus_smoke_host_double(float)

define float @call_host(float %x) {
  %d = call float @fminus_smoke_host_double(float %x)
  %r = fadd float %d, 1.0
  ret float %r
}

define void @gain(ptr noalias %buf, i64 %n, float %g) {
entry:
  %any = icmp sgt i64 %n, 0
  br i1 %any, label %loop, label %exit

loop:
  %i = phi i64 [ 0, %entry ], [ %next, %loop ]
  %p = getelementptr inbounds float, ptr %buf, i64 %i
  %x = load float, ptr %p, align 4
  %y = fmul float %x, %g
  store float %y, ptr %p, align 4
  %next = add nuw nsw i64 %i, 1
  %done = icmp eq i64 %next, %n
  br i1 %done, label %exit, label %loop

exit:
  ret void
}
)IR";

    // O3 for the target machine: its TargetTransformInfo is what lets the vectorisers see the host's vector unit.
    void optimiseO3(llvm::Module& module, llvm::TargetMachine& machine)
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
        passes.buildPerModuleDefaultPipeline(llvm::OptimizationLevel::O3).run(module, modules);
    }
} // namespace

TEST_CASE("LLJIT compiles optimised IR for the host CPU and runs it", "[smoke][jit]")
{
    fminus::initializeLLVM();

    auto builder = llvm::cantFail(llvm::orc::JITTargetMachineBuilder::detectHost());
    builder.setCodeGenOptLevel(llvm::CodeGenOptLevel::Aggressive);
    std::unique_ptr<llvm::TargetMachine> machine = llvm::cantFail(builder.createTargetMachine());
    std::string optimisedIR;   // what the transform layer below hands to code generation
    std::unique_ptr<llvm::orc::LLJIT> jit =
        llvm::cantFail(llvm::orc::LLJITBuilder().setJITTargetMachineBuilder(builder).create());
    CHECK(jit->getTargetTriple().getArch() == llvm::Triple(llvm::sys::getProcessTriple()).getArch());

    // Every module added from here on goes through O3 before code generation.
    jit->getIRTransformLayer().setTransform(
        [&](llvm::orc::ThreadSafeModule tsm,
            llvm::orc::MaterializationResponsibility&) -> llvm::Expected<llvm::orc::ThreadSafeModule> {
            tsm.withModuleDo([&](llvm::Module& module) {
                optimiseO3(module, *machine);
                llvm::raw_string_ostream os(optimisedIR);
                module.print(os, nullptr);
            });
            return std::move(tsm);
        });

    // The host function, as an absolute symbol in the main JITDylib.
    llvm::orc::SymbolMap hostSymbols;
    hostSymbols[jit->mangleAndIntern("fminus_smoke_host_double")] = {
        llvm::orc::ExecutorAddr::fromPtr(&hostDouble), llvm::JITSymbolFlags::Exported | llvm::JITSymbolFlags::Callable};
    llvm::cantFail(jit->getMainJITDylib().define(llvm::orc::absoluteSymbols(std::move(hostSymbols))));

    auto context = std::make_unique<llvm::LLVMContext>();
    llvm::SMDiagnostic diagnostic;
    std::unique_ptr<llvm::Module> module = llvm::parseAssemblyString(kSmokeIR, diagnostic, *context);
    if (!module)
    {
        std::string message;
        llvm::raw_string_ostream os(message);
        diagnostic.print("smoke", os);
        FAIL(message);
    }
    module->setDataLayout(jit->getDataLayout());
    module->setTargetTriple(jit->getTargetTriple());
    REQUIRE_FALSE(llvm::verifyModule(*module, &llvm::errs()));
    llvm::cantFail(jit->addIRModule(llvm::orc::ThreadSafeModule(std::move(module), std::move(context))));

    SECTION("JIT'd code calls into the host")
    {
        auto callHost = llvm::cantFail(jit->lookup("call_host")).toPtr<float (*)(float)>();
        CHECK(callHost(3.0f) == 7.0f);
    }

    SECTION("the gain loop is vectorised and correct, tail included")
    {
        auto gain = llvm::cantFail(jit->lookup("gain")).toPtr<void (*)(float*, std::int64_t, float)>();
        std::vector<float> buffer(1027, 1.5f);
        gain(buffer.data(), static_cast<std::int64_t>(buffer.size()), 2.0f);
        for (float x : buffer)
            REQUIRE(x == 3.0f);
        INFO(optimisedIR);
        CHECK(optimisedIR.find("vector.body") != std::string::npos);
    }
}
