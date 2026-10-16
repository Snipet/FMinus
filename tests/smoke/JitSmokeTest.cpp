// tests/smoke/JitSmokeTest.cpp: the LLVM integration, end to end. It shows four things:
//   - ORC's LLJIT targets the host CPU and its features;
//   - the new pass manager's O3 pipeline runs through LLJIT's IR transform layer (LLJIT does no IR optimisation of its
//     own), with the same TargetMachine and pipeline AOT compilation uses (support/HostTarget.h);
//   - JIT'd code calls the FMinus runtime, defined in the JIT by address (support/JitRuntime.h; no dlsym into the
//     host);
//   - the loop vectoriser uses the host's vector unit.
// It came with the project skeleton. Keep it as a toolchain check, or delete it once the real compiler covers this.
#include "runtime/Runtime.h"
#include "support/HostTarget.h"
#include "support/JitRuntime.h"

#include <catch2/catch_test_macros.hpp>

#include <llvm/AsmParser/Parser.h>
#include <llvm/ExecutionEngine/Orc/JITTargetMachineBuilder.h>
#include <llvm/ExecutionEngine/Orc/LLJIT.h>
#include <llvm/ExecutionEngine/Orc/ThreadSafeModule.h>
#include <llvm/IR/LLVMContext.h>
#include <llvm/IR/Module.h>
#include <llvm/IR/Verifier.h>
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
    constexpr const char* kSmokeIR = R"IR(
declare i32 @fminus_rt_abi_version()

define i32 @call_runtime() {
  %v = call i32 @fminus_rt_abi_version()
  ret i32 %v
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
} // namespace

TEST_CASE("LLJIT compiles optimised IR for the host CPU and runs it", "[smoke][jit]")
{
    std::unique_ptr<llvm::TargetMachine> machine = llvm::cantFail(fminus::hostTargetMachine());
    std::string optimisedIR;   // what the transform layer below hands to code generation

    auto builder = llvm::cantFail(llvm::orc::JITTargetMachineBuilder::detectHost());
    builder.setCodeGenOptLevel(llvm::CodeGenOptLevel::Aggressive);
    std::unique_ptr<llvm::orc::LLJIT> jit =
        llvm::cantFail(llvm::orc::LLJITBuilder().setJITTargetMachineBuilder(builder).create());
    CHECK(jit->getTargetTriple().getArch() == llvm::Triple(llvm::sys::getProcessTriple()).getArch());
    llvm::cantFail(fminus::defineRuntimeSymbols(*jit));

    // Every module added from here on goes through O3 before code generation.
    jit->getIRTransformLayer().setTransform(
        [&](llvm::orc::ThreadSafeModule tsm,
            llvm::orc::MaterializationResponsibility&) -> llvm::Expected<llvm::orc::ThreadSafeModule> {
            tsm.withModuleDo([&](llvm::Module& module) {
                fminus::optimiseModule(module, *machine, llvm::OptimizationLevel::O3);
                llvm::raw_string_ostream os(optimisedIR);
                module.print(os, nullptr);
            });
            return std::move(tsm);
        });

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

    SECTION("JIT'd code calls the FMinus runtime")
    {
        auto callRuntime = llvm::cantFail(jit->lookup("call_runtime")).toPtr<std::int32_t (*)()>();
        CHECK(callRuntime() == fminus_rt_abi_version());
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
