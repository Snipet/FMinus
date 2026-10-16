// tools/fminus-build/main.cpp: LLVM IR (.ll or .bc) to an executable, object file, assembly or optimised IR for this
// machine, through the helpers FMinus's own driver can use (support/HostTarget.h, support/Aot.h). It is for
// development: pipe the compiler's IR through it, run the result under lldb or Instruments, or read its assembly.
//     fminus-build kernel.ll -o kernel             an executable linked with the FMinus runtime (the IR needs a main)
//     fminus-build kernel.ll --emit asm -o -       this machine's assembly, after -O3
//     fminus-build kernel.ll --emit ll -O2 -o -    the optimised IR
// It came with the project skeleton.
#include "support/Aot.h"
#include "support/HostTarget.h"

#include <CLI/CLI.hpp>

#include <llvm/IR/LLVMContext.h>
#include <llvm/IR/Module.h>
#include <llvm/IR/Verifier.h>
#include <llvm/IRReader/IRReader.h>
#include <llvm/Support/Error.h>
#include <llvm/Support/FileSystem.h>
#include <llvm/Support/InitLLVM.h>
#include <llvm/Support/SourceMgr.h>
#include <llvm/Support/raw_ostream.h>
#include <llvm/Target/TargetMachine.h>

#include <array>
#include <memory>
#include <string>
#include <vector>

int main(int argc, char** argv)
{
    llvm::InitLLVM initLLVM(argc, argv);   // stack traces on a crash

    CLI::App app{"Compiles LLVM IR for this machine: an executable (the default), an object, assembly or optimised IR.",
                 "fminus-build"};
    std::string input;
    std::string output;
    std::string emit = "exe";
    int level = 3;
    std::vector<std::string> linkArgs;
    bool noRuntime = false;
    app.add_option("input", input, "LLVM IR, textual (.ll) or bitcode (.bc)")->required()->check(CLI::ExistingFile);
    app.add_option("-o,--output", output, "The output file; '-' is stdout (not for exe)")->required();
    app.add_option("--emit", emit, "What to write: exe, obj, asm or ll")
        ->check(CLI::IsMember({"exe", "obj", "asm", "ll"}))
        ->capture_default_str();
    app.add_option("-O", level, "Optimisation level, 0 to 3")->check(CLI::Range(0, 3))->capture_default_str();
    app.add_option("--link-arg", linkArgs, "An argument for the linking driver, e.g. --link-arg=-lm (repeatable)");
    app.add_flag("--no-runtime", noRuntime, "Do not link libfminus_runtime.a into the executable");
    CLI11_PARSE(app, argc, argv);
    if (emit == "exe" && output == "-")
    {
        llvm::errs() << "fminus-build: an executable needs a file name (-o)\n";
        return 2;
    }

    const std::array<llvm::OptimizationLevel, 4> optimisation{llvm::OptimizationLevel::O0, llvm::OptimizationLevel::O1,
                                                              llvm::OptimizationLevel::O2, llvm::OptimizationLevel::O3};
    const std::array<llvm::CodeGenOptLevel, 4> codegen{llvm::CodeGenOptLevel::None, llvm::CodeGenOptLevel::Less,
                                                       llvm::CodeGenOptLevel::Default,
                                                       llvm::CodeGenOptLevel::Aggressive};
    const auto index = static_cast<std::size_t>(level);

    auto fail = [](llvm::Error error) {
        llvm::errs() << "fminus-build: " << llvm::toString(std::move(error)) << "\n";
        return 1;
    };

    auto machine = fminus::hostTargetMachine(codegen[index]);
    if (!machine)
        return fail(machine.takeError());

    llvm::LLVMContext context;
    llvm::SMDiagnostic diagnostic;
    std::unique_ptr<llvm::Module> module = llvm::parseIRFile(input, diagnostic, context);
    if (!module)
    {
        diagnostic.print("fminus-build", llvm::errs());
        return 1;
    }
    fminus::prepareModule(*module, **machine);
    if (llvm::verifyModule(*module, &llvm::errs()))
    {
        llvm::errs() << "fminus-build: " << input << " is not valid IR\n";
        return 1;
    }
    fminus::optimiseModule(*module, **machine, optimisation[index]);

    llvm::Error result = [&]() -> llvm::Error {
        if (emit == "ll")
        {
            std::error_code ec;
            llvm::raw_fd_ostream out(output, ec, llvm::sys::fs::OF_Text);
            if (ec)
                return llvm::createFileError(output, ec);
            module->print(out, nullptr);
            return llvm::Error::success();
        }
        if (emit == "asm")
            return fminus::aot::emitFile(*module, **machine, output, fminus::aot::FileKind::Assembly);
        if (emit == "obj")
            return fminus::aot::emitFile(*module, **machine, output, fminus::aot::FileKind::Object);
        fminus::aot::LinkOptions options;
        options.output = output;
        options.driverArgs = linkArgs;
        options.linkRuntime = !noRuntime;
        return fminus::aot::emitExecutable(*module, **machine, std::move(options));
    }();
    if (result)
        return fail(std::move(result));
    return 0;
}
