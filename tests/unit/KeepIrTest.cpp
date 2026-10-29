// tests/unit/KeepIrTest.cpp: Program::Options::keepIr's texts (include/fminus/FMinus.h) against what the header says
// they are. The lowered IR is the module ir::lower writes, set up for this machine (prepareModule), as LLVM prints it.
// The optimised IR is what the fminus driver prints with --emit ll for the same source and options: the driver, beside
// this test's executable, runs on the program in a file of its own, and the Program compiles it under that file's name,
// which the text's first two lines give. tests/library/KeepIrTest.cpp has the option as a host uses it.
#include "fminus/FMinus.h"
#include "frontend/Diagnostics.h"
#include "frontend/Frontend.h"
#include "ir/CodeGen.h"
#include "support/HostTarget.h"

#include <catch2/catch_test_macros.hpp>

#include <llvm/ADT/SmallString.h>
#include <llvm/ADT/StringRef.h>
#include <llvm/IR/DataLayout.h>
#include <llvm/IR/LLVMContext.h>
#include <llvm/IR/Module.h>
#include <llvm/Support/Error.h>
#include <llvm/Support/ErrorOr.h>
#include <llvm/Support/FileSystem.h>
#include <llvm/Support/MemoryBuffer.h>
#include <llvm/Support/Path.h>
#include <llvm/Support/Program.h>
#include <llvm/Support/raw_ostream.h>
#include <llvm/Target/TargetMachine.h>

#include <memory>
#include <optional>
#include <string>
#include <system_error>
#include <vector>

namespace
{
    // A module whose parameters take each binding, in a loop that knob versioning copies (on), around a math
    // builtin's kernel (tanhf), under flush-to-zero's entry point: most of what the pipeline does to a program.
    const char* const kShaper = "module Shaper\n"
                                "{\n"
                                "    param float drive [[fminus::default(2)]];\n"
                                "    param float mix;\n"
                                "    param bool on;\n"
                                "\n"
                                "    void process(const float* restrict in, float* restrict out, int n)\n"
                                "        [[clang::nonblocking]]\n"
                                "    {\n"
                                "        for (int i = 0; i < n; ++i)\n"
                                "        {\n"
                                "            float wet = on[i] ? tanhf(drive[i] * in[i]) : in[i];\n"
                                "            out[i] = in[i] + mix[i] * (wet - in[i]);\n"
                                "        }\n"
                                "    }\n"
                                "};\n"
                                "\n"
                                "float twice(float x) { return x + x; }\n";

    // A file of the test's own, removed when the test ends.
    struct TemporaryFile
    {
        explicit TemporaryFile(llvm::StringRef suffix)
        {
            REQUIRE(!llvm::sys::fs::createTemporaryFile("fminus-keep-ir", suffix, path));
        }
        ~TemporaryFile()
        {
            (void)llvm::sys::fs::remove(path);   // a temporary file: nothing to do if it is gone already
        }
        TemporaryFile(const TemporaryFile&) = delete;
        TemporaryFile& operator=(const TemporaryFile&) = delete;

        llvm::SmallString<256> path;
    };

    // The fminus driver, built beside this test's executable (bin/), or "" when it is not there.
    std::string driver()
    {
        const std::string self = llvm::sys::fs::getMainExecutable(nullptr, nullptr);
        const llvm::ErrorOr<std::string> found =
            llvm::sys::findProgramByName("fminus", {llvm::sys::path::parent_path(self)});
        return found ? *found : std::string();
    }

    std::string printed(const llvm::Module& module)
    {
        std::string text;
        llvm::raw_string_ostream stream(text);
        module.print(stream, nullptr);
        return text;
    }
} // namespace

TEST_CASE("keepIr's optimised IR is what fminus --emit ll prints for the same source and options, and its lowered IR "
          "the code generator's module",
          "[ir]")
{
    TemporaryFile source("fm");
    TemporaryFile output("ll");
    {
        std::error_code error;
        llvm::raw_fd_ostream file(source.path, error, llvm::sys::fs::OF_Text);
        REQUIRE(!error);
        file << kShaper;
    }

    // The driver's options, and the Program's that say the same.
    const std::string fminus = driver();
    INFO("the fminus driver, beside " << llvm::sys::fs::getMainExecutable(nullptr, nullptr));
    REQUIRE(!fminus.empty());
    const std::vector<llvm::StringRef> arguments{
        fminus,        source.path,         "--emit", "ll",       "-O3", "--freeze", "Shaper::drive=3", "--stream",
        "Shaper::mix", "--denormals=flush", "-o",     output.path};
    fminus::Program::Options options;
    options.fileName = std::string(source.path);
    options.optimisation = 3;
    options.parameters["Shaper::drive"] = fminus::Program::Binding::frozen(3.0);
    options.parameters["Shaper::mix"] = fminus::Program::Binding::stream();
    options.denormals = fminus::Program::Denormals::Flush;
    options.keepIr = fminus::Program::KeepIr::Both;

    std::string message;
    const int status = llvm::sys::ExecuteAndWait(fminus, arguments, std::nullopt, {}, 0, 0, &message);
    INFO(message);
    REQUIRE(status == 0);
    llvm::ErrorOr<std::unique_ptr<llvm::MemoryBuffer>> written = llvm::MemoryBuffer::getFile(output.path, true);
    REQUIRE(written);
    const std::string expected = (*written)->getBuffer().str();

    std::string errors;
    auto program = fminus::Program::compile(kShaper, errors, options);
    INFO(errors);
    REQUIRE(program);
    CHECK(expected.rfind("; ModuleID = '" + options.fileName + "'\n", 0) == 0);
    CHECK(program->optimisedIr() == expected);

    // The lowered IR: the module as ir::lower writes it, for this machine, with the same options (as fminus lowers it).
    auto machine = llvm::cantFail(fminus::hostTargetMachine(llvm::CodeGenOptLevel::Aggressive));
    fminus::ir::Options lowering;
    lowering.polyLanes = fminus::polyLanes(*machine);
    lowering.dataLayout = machine->createDataLayout().getStringRepresentation();
    lowering.parameters["Shaper::drive"] = {fminus::ir::ParamBinding::Kind::Frozen, 3.0};
    lowering.parameters["Shaper::mix"] = {fminus::ir::ParamBinding::Kind::Stream, 0.0};
    lowering.denormals = fminus::ir::Denormals::Flush;
    lowering.targetTriple = machine->getTargetTriple().str();
    fminus::Diagnostics diagnostics(options.fileName);
    const auto parsed = fminus::frontend::parse(kShaper, diagnostics);
    REQUIRE(parsed);
    llvm::LLVMContext context;
    std::unique_ptr<llvm::Module> module = fminus::ir::lower(*parsed, context, diagnostics, lowering);
    INFO(diagnostics.format());
    REQUIRE(module);
    fminus::prepareModule(*module, *machine);
    CHECK(program->loweredIr() == printed(*module));
    CHECK(program->loweredIr() != program->optimisedIr());

    // And the JIT runs the program from the optimised module.
    auto* twice = program->function<float(float)>("twice", errors);
    INFO(errors);
    REQUIRE(twice != nullptr);
    CHECK(twice(2.5f) == 5.0f);
}
