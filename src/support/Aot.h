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

#include <cstdint>
#include <optional>
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
        // libfminus_web.a, libcurl's and its TLS library's archives and the system libraries they need, in a build
        // with the web library (FMINUS_WEB): emitExecutable sets it when the module calls the web library, and leaves
        // it as given otherwise.
        bool linkWeb = false;
    };

    // Links `options.objects` into `options.output` through the compiler driver.
    llvm::Error linkExecutable(const LinkOptions& options);

    // Writes the module's object to a temporary file and links it, ahead of `options.objects`, into `options.output`,
    // with the web library's archives when the module calls a function of it (fminus_web_*).
    llvm::Error emitExecutable(llvm::Module& module, llvm::TargetMachine& machine, LinkOptions options);

    // Makes a program's `float main(float, ...)`, `double main(...)` or `bool main(...)` its executable's entry, its
    // parameters floats and doubles. C's entry is `int main(int, char**)`, so the program's main is renamed fminus.main
    // (a name no FMinus function can have) and made internal, free to be inlined, and a new C main runs it: it reads
    // main's arguments from the command line, each as its type says (strtof, strtod), calls it and prints its result,
    // a float, a double or true or false (fminus_rt_main_args, or fminus_rt_main_double_args for a main with a double
    // parameter; fminus_rt_main_result, fminus_rt_main_double_result, fminus_rt_main_bool_result: runtime/Runtime.h),
    // exiting with 0, or with 2 when the arguments are wrong. A program with a parallel region gets the runtime's pool
    // around the call (fminus_rt_parallel_start and _stop). Call it before prepareModule and optimising. Fails when the
    // module defines no main, or a main that takes anything but floats and doubles or returns anything but a float, a
    // double or a bool.
    //
    // With `loopBudget`, for a module compiled with the loop budget (ir::Options::loopBudget, opt/LoopBudget.h), the C
    // main sets the program's budget to it before the call, and when main runs past it says so on stderr
    // (fminus_rt_main_loop_budget) and exits with 3 instead of printing what main returned (a negative budget is -1,
    // which stops main at its first check). The C main is then a host's code, which the pass leaves alone
    // (kLoopBudgetHostAttribute). For a module compiled with bounds checks (ir::Options::boundsCheck, its flag) that
    // has any, the C main reads the program's record after the call, and when main failed one says where on stderr
    // (opt::boundsMessage's words, fminus_rt_main_bounds) and exits with 4 instead. With either, it calls main through
    // its address passed through an empty inline asm: an indirect call, so main is never inlined into it, and nothing
    // the optimiser finds of main before the passes (that it never returns, say) changes the code after the call.
    //
    // For a module that calls http_get, http_post or http_listen (its web context, ir::kWebContextSymbol), the C main
    // makes the program's context from its environment before the call (fminus_web_main_start, src/web/Web.h:
    // FMINUS_WEB_ALLOW, FMINUS_WEB_CA_FILE, and with the server FMINUS_WEB_LISTEN, FMINUS_WEB_TOKEN, FMINUS_WEB_HOSTS,
    // FMINUS_WEB_ROOT, FMINUS_WEB_OPEN; none set, no network and no server), exiting with 2 when the library refuses
    // the policy, and destroys it after the call (fminus_web_main_stop), so that no request, and no listening socket,
    // outlives main.
    llvm::Error addEntryPoint(llvm::Module& module, std::optional<std::int64_t> loopBudget = std::nullopt);
} // namespace fminus::aot
