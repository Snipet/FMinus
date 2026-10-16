// src/support/Aot.cpp. The build gives it FMINUS_AOT_DRIVER (this tree's C++ compiler), FMINUS_RUNTIME_LIBRARY (the
// path of libfminus_runtime.a) and FMINUS_AOT_SANITIZE (this tree's -fsanitize= list, or empty): src/CMakeLists.txt.
#include "support/Aot.h"

#include <llvm/ADT/SmallString.h>
#include <llvm/IR/LegacyPassManager.h>
#include <llvm/IR/Module.h>
#include <llvm/Support/FileSystem.h>
#include <llvm/Support/FileUtilities.h>
#include <llvm/Support/Program.h>
#include <llvm/Support/raw_ostream.h>
#include <llvm/Target/TargetMachine.h>

#include <cstdlib>
#include <optional>

namespace fminus::aot
{
    llvm::Error emitFile(llvm::Module& module, llvm::TargetMachine& machine, const std::string& path, FileKind kind)
    {
        if (module.getDataLayout() != machine.createDataLayout())
            return llvm::createStringError("emitFile: the module is not set up for this target machine "
                                           "(call prepareModule before optimising)");
        std::error_code ec;
        const llvm::sys::fs::OpenFlags flags =
            kind == FileKind::Assembly ? llvm::sys::fs::OF_Text : llvm::sys::fs::OF_None;
        llvm::raw_fd_ostream file(path, ec, flags);
        if (ec)
            return llvm::createFileError(path, ec);
        {
            // An object writer seeks back into its output: buffer a stream that cannot (stdout, a pipe).
            std::optional<llvm::buffer_ostream> buffer;
            llvm::raw_pwrite_stream* out = &file;
            if (!file.supportsSeeking())
                out = &buffer.emplace(file);

            llvm::legacy::PassManager passes;
            const llvm::CodeGenFileType type =
                kind == FileKind::Object ? llvm::CodeGenFileType::ObjectFile : llvm::CodeGenFileType::AssemblyFile;
            if (machine.addPassesToEmitFile(passes, *out, nullptr, type))
                return llvm::createStringError("emitFile: this target machine cannot emit that kind of file");
            passes.run(module);
        }
        file.flush();   // not close(): "-" is stdout, which raw_fd_ostream never closes
        if (file.has_error())
        {
            const std::error_code error = file.error();
            file.clear_error();   // else the stream's destructor reports a fatal error
            return llvm::createFileError(path, error);
        }
        return llvm::Error::success();
    }

    llvm::Error linkExecutable(const LinkOptions& options)
    {
        std::string driver = FMINUS_AOT_DRIVER;
        if (const char* chosen = std::getenv("FMINUS_LINK_DRIVER"); chosen != nullptr && *chosen != '\0')
            driver = chosen;
        llvm::ErrorOr<std::string> program = llvm::sys::findProgramByName(driver);
        if (!program)
            return llvm::createStringError(program.getError(), "linkExecutable: cannot find the driver " + driver);

        std::vector<std::string> args{*program};
        args.insert(args.end(), options.objects.begin(), options.objects.end());
        if (options.linkRuntime)
        {
            args.emplace_back(FMINUS_RUNTIME_LIBRARY);
            if (const std::string sanitize = FMINUS_AOT_SANITIZE; !sanitize.empty())
                args.push_back("-fsanitize=" + sanitize);
        }
        args.insert(args.end(), options.driverArgs.begin(), options.driverArgs.end());
        args.emplace_back("-o");
        args.push_back(options.output);

        const std::vector<llvm::StringRef> argRefs(args.begin(), args.end());
        std::string message;
        const int status = llvm::sys::ExecuteAndWait(*program, argRefs, std::nullopt, {}, 0, 0, &message);
        if (status != 0)
        {
            std::string command;
            for (const std::string& arg : args)
                command += (command.empty() ? "" : " ") + arg;
            return llvm::createStringError("linkExecutable: `" + command + "` failed (" + std::to_string(status) + ")"
                                           + (message.empty() ? "" : ": " + message));
        }
        return llvm::Error::success();
    }

    llvm::Error emitExecutable(llvm::Module& module, llvm::TargetMachine& machine, LinkOptions options)
    {
        llvm::SmallString<128> object;
        if (const std::error_code ec = llvm::sys::fs::createTemporaryFile("fminus-aot", "o", object))
            return llvm::createStringError(ec, "emitExecutable: cannot create a temporary object file");
        const llvm::FileRemover removeObject(object);
        if (llvm::Error error = emitFile(module, machine, std::string(object), FileKind::Object))
            return error;
        options.objects.insert(options.objects.begin(), std::string(object));
        return linkExecutable(options);
    }
} // namespace fminus::aot
