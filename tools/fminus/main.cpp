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
//     fminus lowpass.fm --knob-versions 0   no copies of a method for its knobs' values (8 at most by default)
//     fminus lowpass.fm --denormals=flush   subnormals flushed to zero: folded so, and set by the entry points
//     fminus synth.fm --math libm           sinf, cosf, ... as libm's (--math vector, the default: the same result
//                                           wherever computed; --math fast: LLVM's intrinsics, as clang's)
//     fminus synth.fm -I lib                #include "name" looks beside the including file, then in lib/
//     fminus voices.fm --threads 4          a parallel for or parallel { } on 4 threads at most (0, the default:
//                                           one for each core; 1: each region on the calling thread alone)
//     fminus diodetone.cir --emit fm        the FMinus module a circuit netlist generates (fminus::circuit,
//                                           docs/circuits.md); a .cir compiles and runs as that text would
//     fminus play.fm --run dc 1             play.fm's #include "diodetone.cir" reads the module it generates
#include "fminus/Circuit.h"
#include "frontend/Frontend.h"
#include "ir/CodeGen.h"
#include "jit/Jit.h"
#include "runtime/Runtime.h"
#include "support/Aot.h"
#include "support/HostTarget.h"

#include <CLI/CLI.hpp>

#include <llvm/IR/LLVMContext.h>
#include <llvm/IR/Module.h>
#include <llvm/Support/Error.h>
#include <llvm/Support/FileSystem.h>
#include <llvm/Support/InitLLVM.h>
#include <llvm/Support/MemoryBuffer.h>
#include <llvm/Support/Path.h>
#include <llvm/Support/raw_ostream.h>
#include <llvm/Target/TargetMachine.h>

#include <array>
#include <cstdlib>
#include <format>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
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

    // The runtime's pool (runtime/Parallel.cpp), for a program with a parallel region: started while it lives.
    struct RuntimePool
    {
        explicit RuntimePool(bool start) : started(start)
        {
            if (started)
                fminus_rt_parallel_start();
        }
        ~RuntimePool()
        {
            if (started)
                fminus_rt_parallel_stop();
        }
        RuntimePool(const RuntimePool&) = delete;
        RuntimePool& operator=(const RuntimePool&) = delete;

        bool started;
    };

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

    // Whether a file's name ends in .cir, in any case: a circuit netlist (fminus::circuit).
    bool isNetlist(std::string_view name)
    {
        if (name.size() < 4)
            return false;
        std::string tail(name.substr(name.size() - 4));
        for (char& c : tail)
            c = static_cast<char>(c >= 'A' && c <= 'Z' ? c - 'A' + 'a' : c);
        return tail == ".cir";
    }

    // What the circuit compiler said of the .cir files #include read: its messages, and whether one failed. The
    // include resolver cannot carry errors, so it writes them here, and main prints them after the parse.
    struct CircuitLog
    {
        std::string messages;
        bool failed = false;
    };

    // Where #include "name" finds its file, as C's quoted form looks: beside the including file, then in each of
    // `directories` in turn (an absolute name is itself). A file is named as found (the directory and the name, its
    // ./ and ../ removed), except one read already by another name: the first name it was found by, so that the
    // scanner reads it once (the program's own file, `input`, is read already). A .cir file is a circuit netlist: its
    // text is the module fminus::circuit generates, named <file>.fm (so a second #include of it reads nothing), and
    // what the generator says goes to `log`.
    class DiskIncludes
    {
    public:
        DiskIncludes(std::vector<std::string> directories, const std::string& input, CircuitLog* log)
            : directories_(std::move(directories)), log_(log)
        {
            if (input != "<stdin>")
                remember(input);
        }

        std::optional<fminus::frontend::IncludedFile> operator()(std::string_view name, std::string_view from)
        {
            std::vector<llvm::SmallString<256>> candidates;
            if (llvm::sys::path::is_absolute(name))
                candidates.emplace_back(name);
            else
            {
                llvm::SmallString<256> beside(from == "<stdin>" ? llvm::StringRef()
                                                                : llvm::sys::path::parent_path(from));
                llvm::sys::path::append(beside, name);
                candidates.push_back(beside);
                for (const std::string& directory : directories_)
                {
                    llvm::SmallString<256> inside(directory);
                    llvm::sys::path::append(inside, name);
                    candidates.push_back(inside);
                }
            }
            for (llvm::SmallString<256>& candidate : candidates)
            {
                llvm::sys::path::remove_dots(candidate, true);
                if (llvm::sys::fs::is_directory(candidate))
                    continue;
                llvm::ErrorOr<std::unique_ptr<llvm::MemoryBuffer>> file = llvm::MemoryBuffer::getFile(candidate, true);
                if (!file)
                    continue;
                std::string found = remember(std::string(candidate));
                if (!isNetlist(found))
                    return fminus::frontend::IncludedFile{found, (*file)->getBuffer().str()};
                const fminus::circuit::Result circuit = fminus::circuit::generate((*file)->getBuffer(), found);
                log_->messages += circuit.errors + circuit.warnings;
                log_->failed = log_->failed || !circuit.errors.empty();
                return fminus::frontend::IncludedFile{found + ".fm", circuit.source};
            }
            return std::nullopt;
        }

    private:
        // The name the file at `path` goes by: the first it was found by.
        std::string remember(const std::string& path)
        {
            llvm::SmallString<256> real;
            if (llvm::sys::fs::real_path(path, real))
                return path;
            return names_.try_emplace(std::string(real), path).first->second;
        }

        std::vector<std::string> directories_;
        std::map<std::string, std::string> names_;   // a file's real path, and the name it goes by
        CircuitLog* log_;                            // main's: the resolver is copied into a std::function
    };

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
    bool noStrictAliasing = false;
    unsigned knobVersions = fminus::ir::Options{}.knobVersions;
    std::string denormals = "preserve";
    std::string math = "vector";
    std::vector<std::string> includeDirectories;
    unsigned threads = 0;
    app.add_option("input", input, "FMinus source file ('-' for stdin)")->required();
    app.add_option("args", runArgs, "The arguments of --run's function");
    app.add_option("-o,--output", output, "The output file ('-' is stdout)")->capture_default_str();
    app.add_option("--emit", emit,
                   "What to write: ast, ll, asm, obj, exe (an executable that runs main), or fm (the FMinus module a "
                   "circuit netlist, .cir, generates)")
        ->check(CLI::IsMember({"ast", "ll", "asm", "obj", "exe", "fm"}))
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
    app.add_flag("--no-strict-aliasing", noStrictAliasing,
                 "Emit no type-based alias metadata (clang's -fno-strict-aliasing): for a program that reads memory "
                 "as a type other than the one it holds");
    app.add_option("--knob-versions", knobVersions,
                   "At most this many copies of a method whose loop branches on an int or bool knob or a flat stream, "
                   "one for each value (0: none; from -O1)")
        ->capture_default_str();
    app.add_option("--denormals", denormals,
                   "Subnormal floats: preserve (IEEE's, as in C) or flush (to zero: the code is folded for that mode, "
                   "and each entry point, a nonblocking function nothing calls, sets it)")
        ->check(CLI::IsMember({"preserve", "flush"}))
        ->capture_default_str();
    app.add_option("--math", math,
                   "How sinf, cosf, tanf, expf, tanhf and logf compute: vector (one result wherever computed, on any "
                   "machine: FMinus's own kernels), libm (the C library's, a lane at a time) or fast (LLVM's "
                   "intrinsics)")
        ->check(CLI::IsMember({"vector", "libm", "fast"}))
        ->capture_default_str();
    app.add_option("-I", includeDirectories,
                   "Where #include \"name\" looks after the including file's directory, in order (clang's -I)")
        ->allow_extra_args(false);
    app.add_option("--threads", threads,
                   "The most threads a parallel for or parallel { } runs on, the calling thread's among them (0: one "
                   "for each core; 1: each region on the calling thread alone)")
        ->capture_default_str();
    CLI11_PARSE(app, argc, argv);
    if (emit == "exe" && run.empty() && output == "-")
        return fail("--emit exe: an executable needs a file name (-o)");

    const std::array<llvm::OptimizationLevel, 4> levels{llvm::OptimizationLevel::O0, llvm::OptimizationLevel::O1,
                                                        llvm::OptimizationLevel::O2, llvm::OptimizationLevel::O3};
    const llvm::OptimizationLevel optimisation = levels[static_cast<std::size_t>(level)];

    // Source -> syntax tree. A circuit netlist (.cir) is first the FMinus text fminus::circuit generates from it.
    llvm::ErrorOr<std::unique_ptr<llvm::MemoryBuffer>> source = llvm::MemoryBuffer::getFileOrSTDIN(input);
    if (!source)
        return fail("cannot read " + input + ": " + source.getError().message());
    std::string text = (*source)->getBuffer().str();
    std::string sourceName = input == "-" ? "<stdin>" : input;
    if (isNetlist(input))
    {
        const fminus::circuit::Result circuit = fminus::circuit::generate(text, input);
        llvm::errs() << circuit.errors << circuit.warnings;
        if (!circuit.errors.empty())
            return 1;
        if (emit == "fm" && run.empty())
            return writeText(output, circuit.source);
        text = circuit.source;
        sourceName = input + ".fm";   // FMinus's messages cite the lines --emit fm prints
    }
    else if (emit == "fm")
        return fail("--emit fm: " + input + " is not a circuit netlist (.cir)");
    CircuitLog circuits;
    fminus::Diagnostics diagnostics(sourceName);
    std::optional<fminus::ast::Program> program =
        fminus::frontend::parse(text, diagnostics, DiskIncludes(includeDirectories, diagnostics.fileName(), &circuits));
    llvm::errs() << circuits.messages;
    if (circuits.failed)
        return 1;
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
    options.strictAliasing = !noStrictAliasing;
    options.knobVersions = knobVersions;
    options.math = math == "libm"   ? fminus::ir::Math::Libm
                   : math == "fast" ? fminus::ir::Math::Fast
                                    : fminus::ir::Math::Vector;
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
    options.denormals = denormals == "flush" ? fminus::ir::Denormals::Flush : fminus::ir::Denormals::Preserve;
    options.targetTriple = (*machine)->getTargetTriple().str();   // which register an entry point sets
    options.threads = threads;

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

        // A program with a parallel region runs it on the runtime's pool, started for the call (runtime/Runtime.h).
        const RuntimePool pool(module->getFunction("fminus_rt_parallel_for") != nullptr && threads != 1);
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
        std::string ir;
        llvm::raw_string_ostream os(ir);
        module->print(os, nullptr);
        return writeText(output, ir);
    }
    const auto kind = emit == "asm" ? fminus::aot::FileKind::Assembly : fminus::aot::FileKind::Object;
    if (llvm::Error error = fminus::aot::emitFile(*module, **machine, output, kind))
        return fail(std::move(error));
    return 0;
}
