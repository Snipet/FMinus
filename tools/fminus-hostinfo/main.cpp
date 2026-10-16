// tools/fminus-hostinfo/main.cpp: prints what the JIT targets on this machine: the LLVM in use, the process's triple,
// the host CPU LLVM detects, the CPU features code is generated for (every one with --features) and the data layout
// LLJIT would use. The features are the target machine's, not host detection's: on Apple silicon LLVM detects the CPU
// model (apple-m5, ...) but no feature list, and the model implies the features.
// It came with the project skeleton as a diagnostic (and a lit smoke test, tests/lit/smoke/hostinfo.test), and shows
// a tool's command line through CLI11.
#include "fminus/Config.h"
#include "support/LLVMInit.h"

#include <CLI/CLI.hpp>

#include <llvm/Config/llvm-config.h>
#include <llvm/ExecutionEngine/Orc/JITTargetMachineBuilder.h>
#include <llvm/IR/DataLayout.h>
#include <llvm/MC/MCSubtargetInfo.h>
#include <llvm/Support/Error.h>
#include <llvm/Support/InitLLVM.h>
#include <llvm/Support/raw_ostream.h>
#include <llvm/Target/TargetMachine.h>

#include <string>
#include <vector>

int main(int argc, char** argv)
{
    llvm::InitLLVM initLLVM(argc, argv);   // stack traces on a crash

    CLI::App app{"Prints what FMinus's JIT targets on this machine.", "fminus-hostinfo"};
    app.set_version_flag("--version", FMINUS_VERSION_STRING);
    bool listFeatures = false;
    app.add_flag("-f,--features", listFeatures, "List every CPU feature the JIT enables");
    CLI11_PARSE(app, argc, argv);

    fminus::initializeLLVM();

    auto builder = llvm::orc::JITTargetMachineBuilder::detectHost();
    if (!builder)
    {
        llvm::errs() << "fminus-hostinfo: " << llvm::toString(builder.takeError()) << "\n";
        return 1;
    }
    auto machine = builder->createTargetMachine();
    if (!machine)
    {
        llvm::errs() << "fminus-hostinfo: " << llvm::toString(machine.takeError()) << "\n";
        return 1;
    }

    // The features code generation uses: the CPU model's, plus what host detection added.
    const llvm::MCSubtargetInfo& subtarget = (*machine)->getMCSubtargetInfo();
    std::vector<std::string> features;
    for (const llvm::SubtargetFeatureKV& feature : subtarget.getAllProcessorFeatures())
    {
        if (subtarget.getFeatureBits().test(feature.Value))
            features.emplace_back(feature.key());
    }
    llvm::outs() << "fminus " FMINUS_VERSION_STRING " (LLVM " LLVM_VERSION_STRING ", " FMINUS_LLVM_MODE
                 << (FMINUS_LLVM_ASSERTIONS ? ", assertions" : "") << ")\n";
    llvm::outs() << "triple: " << builder->getTargetTriple().str() << "\n";
    llvm::outs() << "cpu: " << builder->getCPU() << "\n";
    llvm::outs() << "features: " << features.size() << (listFeatures ? "" : " (--features lists them)") << "\n";
    if (listFeatures)
    {
        for (const std::string& feature : features)
            llvm::outs() << "  " << feature << "\n";
    }
    llvm::outs() << "data layout: " << (*machine)->createDataLayout().getStringRepresentation() << "\n";
    return 0;
}
