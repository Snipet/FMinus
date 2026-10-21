// tools/fminus/main.cpp: the FMinus compiler driver. FMinus v0 source goes to its syntax tree, LLVM IR, assembly, an
// object file or an executable, or is JIT-compiled and called:
//     fminus gain.fm                        the optimised IR (-O2) on stdout
//     fminus gain.fm -O0                    the IR as the code generator wrote it
//     fminus gain.fm --emit ast             the syntax tree
//     fminus gain.fm --emit asm -o -        this machine's assembly (--emit obj: an object file)
//     fminus gain.fm --emit exe -o gain     an executable that runs the program's main (support/Aot.h)
//     fminus gain.fm --entry                the IR with that executable's C main (also for asm and obj)
//     fminus gain.fm --run gain 0.5 2       JIT-compiles the program, calls gain(0.5, 2) and prints the result
//     fminus unison.fm --poly-lanes 16      as if this machine's vectors held 16 floats (any target's poly float)
//     fminus lowpass.fm --stream Lowpass::cutoff --freeze Lowpass::mode=2
//                                           a parameter read from a buffer, one compiled in (a knob otherwise)
#include "frontend/Frontend.h"
#include "ir/CodeGen.h"
#include "jit/Jit.h"
#include "support/Aot.h"
#include "support/HostTarget.h"

#include <CLI/CLI.hpp>

#include <llvm/IR/LLVMContext.h>
#include <llvm/IR/Module.h>
#include <llvm/Support/Error.h>
#include <llvm/Support/FileSystem.h>
#include <llvm/Support/InitLLVM.h>
#include <llvm/Support/MemoryBuffer.h>
#include <llvm/Support/raw_ostream.h>
#include <llvm/Target/TargetMachine.h>

#include <array>
#include <cstdlib>
#include <format>
#include <memory>
#include <string>
#include <utility>
#include <vector>

namespace
{
    constexpr std::size_t kMaxRunArgs = 8;

    int fail(const std::string& message)
    {
        llvm::errs() << "fminus: " << message << "\n";
        return 1;
    }

    int fail(llvm::Error error)
    {
        return fail(llvm::toString(std::move(error)));
    }

    // Calls the JIT'd Result(float, ...) at `address` with args.size() arguments: Result is float, bool or int.
    template <typename Result, std::size_t... I>
    Result callWith(llvm::orc::ExecutorAddr address, const std::vector<float>& args, std::index_sequence<I...>)
    {
        using Function = Result (*)(decltype(static_cast<void>(I), 0.0f)...);
        return address.toPtr<Function>()(args[I]...);
    }

    template <typename Result, std::size_t... N>
    Result call(llvm::orc::ExecutorAddr address, const std::vector<float>& args, std::index_sequence<N...>)
    {
        Result result{};
        static_cast<void>(
            ((args.size() == N ? (result = callWith<Result>(address, args, std::make_index_sequence<N>{}), true)
                               : false)
             || ...));
        return result;
    }

    // --freeze Lowpass::mode=2: the parameter, and the value it is compiled with.
    bool frozen(const std::string& spec, std::string& name, double& value)
    {
        const std::size_t equals = spec.rfind('=');
        if (equals == std::string::npos || equals == 0)
            return false;
        name = spec.substr(0, equals);
        const std::string number = spec.substr(equals + 1);
        char* end = nullptr;
        value = std::strtod(number.c_str(), &end);
        return !number.empty() && end == number.c_str() + number.size();
    }

    int writeText(const std::string& path, const std::string& text)
    {
        std::error_code ec;
        llvm::raw_fd_ostream out(path, ec, llvm::sys::fs::OF_Text);
        if (ec)
            return fail(llvm::createFileError(path, ec));
        out << text;
        return 0;
    }
} // namespace

int main(int argc, char** argv)
{
    llvm::InitLLVM initLLVM(argc, argv);   // stack traces on a crash

    CLI::App app{"The FMinus compiler: FMinus v0 source to its syntax tree, LLVM IR, assembly, an object file or an "
                 "executable, or JIT-compiled and run.",
                 "fminus"};
    std::string input;
    std::string output = "-";
    std::string emit = "ll";
    int level = 2;
    std::string run;
    std::vector<float> runArgs;
    bool entry = false;
    unsigned polyLanes = 0;
    std::vector<std::string> streams;
    std::vector<std::string> freezes;
    app.add_option("input", input, "FMinus source file ('-' for stdin)")->required();
    app.add_option("args", runArgs, "The arguments of --run's function");
    app.add_option("-o,--output", output, "The output file ('-' is stdout)")->capture_default_str();
    app.add_option("--emit", emit, "What to write: ast, ll, asm, obj, or exe (an executable that runs main)")
        ->check(CLI::IsMember({"ast", "ll", "asm", "obj", "exe"}))
        ->capture_default_str();
    app.add_option("-O", level, "Optimisation level, 0 to 3")->check(CLI::Range(0, 3))->capture_default_str();
    app.add_option("--run", run, "JIT-compile, call this function with the arguments and print its result");
    app.add_option("--poly-lanes", polyLanes, "A poly float's lanes: 1, 2, 4, ... 64 (by default, this machine's)")
        ->check(CLI::IsMember({1u, 2u, 4u, 8u, 16u, 32u, 64u}));
    app.add_flag("--entry", entry, "Add the C main an executable starts in, which runs the program's main (always "
                                   "with --emit exe)");
    app.add_option("--stream", streams, "Read a float parameter (Module::name) from a buffer the host points it at")
        ->allow_extra_args(false);
    app.add_option("--freeze", freezes, "Compile a parameter in as a constant: Module::name=value")
        ->allow_extra_args(false);
    CLI11_PARSE(app, argc, argv);
    if (emit == "exe" && run.empty() && output == "-")
        return fail("--emit exe: an executable needs a file name (-o)");

    const std::array<llvm::OptimizationLevel, 4> levels{llvm::OptimizationLevel::O0, llvm::OptimizationLevel::O1,
                                                        llvm::OptimizationLevel::O2, llvm::OptimizationLevel::O3};
    const llvm::OptimizationLevel optimisation = levels[static_cast<std::size_t>(level)];

    // Source -> syntax tree.
    llvm::ErrorOr<std::unique_ptr<llvm::MemoryBuffer>> source = llvm::MemoryBuffer::getFileOrSTDIN(input);
    if (!source)
        return fail("cannot read " + input + ": " + source.getError().message());
    fminus::Diagnostics diagnostics(input == "-" ? "<stdin>" : input);
    std::optional<fminus::ast::Program> program = fminus::frontend::parse((*source)->getBuffer(), diagnostics);
    if (!program)
    {
        llvm::errs() << diagnostics.format();
        return 1;
    }
    if (emit == "ast" && run.empty())
        return writeText(output, fminus::ast::dump(*program));

    // The target: it decides how many lanes a poly float has.
    auto machine = fminus::hostTargetMachine(fminus::codeGenLevel(optimisation));
    if (!machine)
        return fail(machine.takeError());
    fminus::ir::Options options;
    options.polyLanes = polyLanes != 0 ? polyLanes : fminus::polyLanes(**machine);
    options.dataLayout = (*machine)->createDataLayout().getStringRepresentation();
    for (const std::string& name : streams)
        options.parameters[name] = {fminus::ir::ParamBinding::Kind::Stream, 0.0};
    for (const std::string& spec : freezes)
    {
        std::string name;
        double value = 0.0;
        if (!frozen(spec, name, value))
            return fail("--freeze " + spec + ": not Module::name=value");
        options.parameters[name] = {fminus::ir::ParamBinding::Kind::Frozen, value};
    }

    // Syntax tree -> LLVM IR.
    auto context = std::make_unique<llvm::LLVMContext>();
    std::unique_ptr<llvm::Module> module = fminus::ir::lower(*program, *context, diagnostics, options);
    if (!module)
    {
        llvm::errs() << diagnostics.format();
        return 1;
    }

    // LLVM IR -> JIT -> a call.
    if (!run.empty())
    {
        const fminus::ast::Function* function = nullptr;
        for (const fminus::ast::Function& candidate : program->functions)
        {
            if (candidate.name == run)
                function = &candidate;
        }
        if (function == nullptr)
            return fail("--run: " + input + " has no function '" + run + "'");
        if (function->params.size() != runArgs.size())
            return fail(std::format("--run: '{}' takes {} arguments, not {}", run, function->params.size(),
                                    runArgs.size()));
        if (runArgs.size() > kMaxRunArgs)
            return fail(std::format("--run: at most {} arguments", kMaxRunArgs));
        for (const fminus::ast::Param& param : function->params)
        {
            if (param.type != fminus::ast::Type::Float)
                return fail(std::format("--run: '{}' takes a parameter of type '{}'; --run passes floats only", run,
                                        fminus::ast::typeName(param.type)));
        }

        auto jit = fminus::jit::Jit::create(optimisation);
        if (!jit)
            return fail(jit.takeError());
        if (llvm::Error error = (*jit)->add(std::move(module), std::move(context)))
            return fail(std::move(error));
        auto address = (*jit)->lookup(run);
        if (!address)
            return fail(address.takeError());
        // Through a pointer of the function's own type: a bool or an int comes back in a general register (a bool
        // zero-extended), a float in a vector register. std::format prints a bool as true or false.
        constexpr auto counts = std::make_index_sequence<kMaxRunArgs + 1>{};
        switch (function->returnType.kind)
        {
        case fminus::ast::Kind::Float:
            llvm::outs() << std::format("{}\n", call<float>(*address, runArgs, counts));
            break;
        case fminus::ast::Kind::Bool:
            llvm::outs() << std::format("{}\n", call<bool>(*address, runArgs, counts));
            break;
        case fminus::ast::Kind::Int:
            llvm::outs() << std::format("{}\n", call<int>(*address, runArgs, counts));
            break;
        case fminus::ast::Kind::Void:
        case fminus::ast::Kind::Float2:   // a vector stays inside FMinus, or goes to a C++ host (docs/simd.md)
        case fminus::ast::Kind::Float4:
        case fminus::ast::Kind::Float8:
        case fminus::ast::Kind::Bool2:
        case fminus::ast::Kind::Bool4:
        case fminus::ast::Kind::Bool8:
        case fminus::ast::Kind::PolyFloat:
        case fminus::ast::Kind::PolyBool:
        case fminus::ast::Kind::Pointer:
        case fminus::ast::Kind::Module:   // never a result (the code generator refused it)
        case fminus::ast::Kind::Array:
            return fail(std::format("--run: '{}' returns '{}'; --run prints floats, bools and ints only", run,
                                    fminus::ast::typeName(function->returnType)));
        }
        return 0;
    }

    // LLVM IR -> optimised for this machine -> IR, assembly, an object file or an executable.
    if (entry || emit == "exe")
    {
        if (llvm::Error error = fminus::aot::addEntryPoint(*module))
            return fail(std::move(error));
    }
    fminus::prepareModule(*module, **machine);
    fminus::optimiseModule(*module, **machine, optimisation);
    if (emit == "exe")
    {
        if (llvm::Error error = fminus::aot::emitExecutable(*module, **machine, {.output = output}))
            return fail(std::move(error));
        return 0;
    }
    if (emit == "ll")
    {
        std::string text;
        llvm::raw_string_ostream os(text);
        module->print(os, nullptr);
        return writeText(output, text);
    }
    const auto kind = emit == "asm" ? fminus::aot::FileKind::Assembly : fminus::aot::FileKind::Object;
    if (llvm::Error error = fminus::aot::emitFile(*module, **machine, output, kind))
        return fail(std::move(error));
    return 0;
}
