// src/support/Aot.h: ahead-of-time compilation, for development. It writes a module's machine code to an object or
// assembly file, and links objects into a native executable with the FMinus runtime (runtime/Runtime.h).
//
// An executable built this way runs under lldb and Instruments like any program. For the same module, TargetMachine
// (support/HostTarget.h) and pipeline, its code is what LLJIT would generate. Linking runs the C++ compiler driver this
// build tree was configured with (Xcode's clang on macOS; $FMINUS_LINK_DRIVER overrides it) against this tree's
// libfminus_runtime.a, so it only works on a development machine: FMinus inside a plugin only ever JITs. An integration
// piece that came with the project skeleton.
#pragma once

#include <llvm/Support/Error.h>

#include <string>
#include <vector>

namespace llvm
{
    class Module;
    class TargetMachine;
} // namespace llvm

namespace fminus::aot
{
    enum class FileKind
    {
        Object,
        Assembly,
    };

    // Generates machine code for `module`, which prepareModule set up for `machine`, into `path` ("-": stdout).
    llvm::Error emitFile(llvm::Module& module, llvm::TargetMachine& machine, const std::string& path, FileKind kind);

    struct LinkOptions
    {
        std::string output;                      // the executable
        std::vector<std::string> objects{};      // object files and archives, in link order
        std::vector<std::string> driverArgs{};   // to the driver as they are: -framework Accelerate, -lm, -v, ...
        bool linkRuntime = true;                 // libfminus_runtime.a (and this tree's sanitizer runtimes)
    };

    // Links `options.objects` into `options.output` through the compiler driver.
    llvm::Error linkExecutable(const LinkOptions& options);

    // Writes the module's object to a temporary file and links it, ahead of `options.objects`, into `options.output`.
    llvm::Error emitExecutable(llvm::Module& module, llvm::TargetMachine& machine, LinkOptions options);

    // Makes a program's `float main(float, ...)` or `bool main(float, ...)` its executable's entry. C's entry is
    // `int main(int, char**)`, so the program's main is renamed fminus.main (a name no FMinus function can have) and
    // made internal, free to be inlined, and a new C main runs it: it reads main's arguments from the command line,
    // calls it and prints its result, a float or true or false (fminus_rt_main_args, fminus_rt_main_result,
    // fminus_rt_main_bool_result: runtime/Runtime.h), exiting with 0, or with 2 when the arguments are wrong. Call it
    // before prepareModule and optimising. Fails when the module defines no main, or a main that takes anything but
    // floats or returns anything but a float or a bool.
    llvm::Error addEntryPoint(llvm::Module& module);
} // namespace fminus::aot
