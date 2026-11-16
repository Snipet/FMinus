// src/support/Aot.h: ahead-of-time compilation. It writes a module's machine code to an object or assembly file, and
// links objects into a native executable with the FMinus runtime (runtime/Runtime.h): what `fminus prog.fm -o prog`
// does, in a build tree and in an install (docs/building.md, "Installing").
//
// An executable built this way runs under lldb and Instruments like any program. For the same module, TargetMachine
// (support/HostTarget.h) and pipeline, its code is what LLJIT would generate (on macOS, but for how it addresses
// memory: HostTarget.h). Linking runs a C++ compiler driver (findToolchain: $FMINUS_LINK_DRIVER, else the
// compiler FMinus was built with, Xcode's clang on macOS, if it is there, else the system's) against the runtime's
// archives, which an installed fminus finds beside itself and a build tree's in its tree. A machine that links
// executables needs a C++ toolchain; one that only JITs needs nothing (FMinus inside a plugin only ever JITs). An
// integration piece that came with the project skeleton.
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

    // What linkExecutable links with, and how it found each part: what `fminus --print-runtime` prints.
    //
    // The runtime's archives: in the directory $FMINUS_RUNTIME_DIR names, when it is set (and nowhere else); else in
    // the installed layout beside the running executable, <bin>/../lib/fminus (FMINUS_INSTALL_RUNTIME_RELATIVE: what
    // `cmake --install` lays out, packaging/CMakeLists.txt), so that a prefix can be moved or unpacked anywhere; else
    // in the build tree this FMinus was built in (its lib/, and the deps cache's libcurl and Mbed TLS), if it is still
    // there. The C++ compiler driver that links: $FMINUS_LINK_DRIVER, when it is set (and nothing else); else the one
    // this FMinus was built with (FMINUS_AOT_DRIVER), if it is there; else the system's, by name on PATH: c++, then
    // clang++ (clang-cl on Windows).
    struct Toolchain
    {
        std::string runtime;                    // libfminus_runtime.a (fminus_runtime.lib); empty: not found
        std::string runtimeFrom;                // how it was found
        std::vector<std::string> runtimeTried;  // where it was looked for, in order
        // In a build with the web library (FMINUS_WEB): libfminus_web.a and libcurl's and its TLS library's archives,
        // from the same place as the runtime, in link order, and then the system libraries they need, as the driver
        // takes them (-pthread; -framework Security, ...; ws2_32.lib, ...).
        std::vector<std::string> web;
        std::string driver;                     // the driver's path; empty: none found
        std::string driverFrom;                 // how it was found
        std::vector<std::string> driverTried;   // what was looked for, in order
    };

    // Looks for the runtime and the driver, as linkExecutable does each time it links.
    Toolchain findToolchain();

    struct LinkOptions
    {
        std::string output;                      // the executable
        std::vector<std::string> objects{};      // object files and archives, in link order
        std::vector<std::string> driverArgs{};   // to the driver as they are: -framework Accelerate, -lm, -v, ...
        bool linkRuntime = true;                 // libfminus_runtime.a (and the build's sanitizer runtimes)
        // libfminus_web.a, libcurl's and its TLS library's archives and the system libraries they need, in a build
        // with the web library (FMINUS_WEB): emitExecutable sets it when the module calls the web library, and leaves
        // it as given otherwise.
        bool linkWeb = false;
    };

    // Links `options.objects` into `options.output` through the compiler driver findToolchain finds, with the runtime's
    // archives it finds. Fails, naming where it looked, when it finds no driver, or no runtime when it is to link one.
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
    // outlives main. For a module that names a file (file_read, file_list, file_write or file_append: its file context,
    // ir::kFilesContextSymbol), the same before the web's, from FMINUS_FILES_READ, FMINUS_FILES_WRITE,
    // FMINUS_FILES_DENY, FMINUS_FILES_TYPES and FMINUS_FILES_ANYWHERE (fminus_rt_files_main_start, runtime/Runtime.h;
    // neither a folder nor anywhere set, no files), and destroyed after it (fminus_rt_files_main_stop), every file the
    // program holds freed.
    llvm::Error addEntryPoint(llvm::Module& module, std::optional<std::int64_t> loopBudget = std::nullopt);
} // namespace fminus::aot
