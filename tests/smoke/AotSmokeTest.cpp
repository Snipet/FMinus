// tests/smoke/AotSmokeTest.cpp: ahead-of-time compilation end to end (support/Aot.h). A module with a main is
// optimised for the host, written as an object, linked with the FMinus runtime by the compiler driver, and run as its
// own process; it calls the runtime and a vectorised gain loop. It came with the project skeleton.
#include "support/Aot.h"
#include "support/HostTarget.h"

#include <catch2/catch_test_macros.hpp>

#include <llvm/ADT/SmallString.h>
#include <llvm/AsmParser/Parser.h>
#include <llvm/IR/LLVMContext.h>
#include <llvm/IR/Module.h>
#include <llvm/Support/Error.h>
#include <llvm/Support/FileSystem.h>
#include <llvm/Support/Path.h>
#include <llvm/Support/Program.h>
#include <llvm/Support/SourceMgr.h>
#include <llvm/Support/raw_ostream.h>
#include <llvm/Target/TargetMachine.h>

#include <memory>
#include <string>

namespace
{
    // main exits 42 when the runtime answers and the gain loop doubled every sample, 1 otherwise.
    constexpr const char* kProgramIR = R"IR(
declare i32 @fminus_rt_abi_version()

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

define i32 @main() {
entry:
  %buf = alloca [1027 x float], align 16
  br label %fill
fill:
  %i = phi i64 [ 0, %entry ], [ %inext, %fill ]
  %p = getelementptr inbounds [1027 x float], ptr %buf, i64 0, i64 %i
  store float 1.5, ptr %p, align 4
  %inext = add nuw nsw i64 %i, 1
  %filled = icmp eq i64 %inext, 1027
  br i1 %filled, label %run, label %fill
run:
  call void @gain(ptr %buf, i64 1027, float 2.0)
  br label %check
check:
  %j = phi i64 [ 0, %run ], [ %jnext, %next ]
  %q = getelementptr inbounds [1027 x float], ptr %buf, i64 0, i64 %j
  %v = load float, ptr %q, align 4
  %right = fcmp oeq float %v, 3.0
  br i1 %right, label %next, label %wrong
next:
  %jnext = add nuw nsw i64 %j, 1
  %checked = icmp eq i64 %jnext, 1027
  br i1 %checked, label %runtime, label %check
runtime:
  %abi = call i32 @fminus_rt_abi_version()
  %answers = icmp sgt i32 %abi, 0
  %code = select i1 %answers, i32 42, i32 1
  ret i32 %code
wrong:
  ret i32 1
}
)IR";

    std::string message(llvm::Error error)
    {
        return llvm::toString(std::move(error));
    }
} // namespace

TEST_CASE("an AOT executable links the FMinus runtime and runs", "[smoke][aot]")
{
    std::unique_ptr<llvm::TargetMachine> machine = llvm::cantFail(fminus::hostTargetMachine());

    llvm::LLVMContext context;
    llvm::SMDiagnostic diagnostic;
    std::unique_ptr<llvm::Module> module = llvm::parseAssemblyString(kProgramIR, diagnostic, context);
    if (!module)
    {
        std::string text;
        llvm::raw_string_ostream os(text);
        diagnostic.print("smoke", os);
        FAIL(text);
    }
    fminus::prepareModule(*module, *machine);
    fminus::optimiseModule(*module, *machine, llvm::OptimizationLevel::O3);

    llvm::SmallString<128> directory;
    REQUIRE_FALSE(llvm::sys::fs::createUniqueDirectory("fminus-aot-smoke", directory));
    llvm::SmallString<128> executable(directory);
    llvm::sys::path::append(executable, "program");

    fminus::aot::LinkOptions options;
    options.output = std::string(executable);
    if (llvm::Error error = fminus::aot::emitExecutable(*module, *machine, options))
        FAIL(message(std::move(error)));

    const llvm::StringRef program(executable);
    CHECK(llvm::sys::ExecuteAndWait(program, {program}) == 42);
    CHECK_FALSE(llvm::sys::fs::remove_directories(directory));
}
