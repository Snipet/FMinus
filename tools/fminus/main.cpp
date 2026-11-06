// tools/fminus/main.cpp: the FMinus compiler driver. FMinus v0 source goes to its syntax tree, LLVM IR, assembly, an
// object file or an executable, or is JIT-compiled and called:
//     fminus gain.fm                        the optimised IR (-O2) on stdout
//     fminus gain.fm -O0                    the IR as the code generator wrote it
//     fminus gain.fm --emit ast             the syntax tree
//     fminus gain.fm --emit asm -o -        this machine's assembly (--emit obj: an object file)
//     fminus gain.fm --emit exe -o gain     an executable that runs the program's main (support/Aot.h)
//     fminus gain.fm --entry                the IR with that executable's C main (also for asm and obj)
//     fminus gain.fm --run gain 0.5 2       JIT-compiles the program, calls gain(0.5, 2) and prints the result (its
//                                           parameters bools, ints, floats and doubles; its result one of those, or
//                                           nothing)
//     fminus sum.fm --run scale 1,2,3 3 0.5 an array for a pointer parameter, its elements separated by commas,
//                                           printed after the call when the function may have written to it
//     fminus unison.fm --poly-lanes 16      as if this machine's vectors held 16 floats (any target's poly float)
//     fminus lowpass.fm --stream Lowpass::cutoff --freeze Lowpass::mode=2
//                                           a parameter read from a buffer, one compiled in (a knob otherwise)
//     fminus lowpass.fm --knob-versions 0   no copies of a method for its knobs' values (8 at most by default)
//     fminus lowpass.fm --denormals=flush   subnormals flushed to zero: folded so, and set by the entry points
//     fminus synth.fm --math libm           sinf, cosf, ... as libm's (--math vector, the default: the same result
//                                           wherever computed; --math fast: LLVM's intrinsics, as clang's)
//     fminus synth.fm -I lib                #include "name" looks beside the including file, then in lib/
//     fminus synth.fm -D DEBUG -D LEVEL=2   macros for #if, #ifdef and #ifndef, as clang's -D (DEBUG is 1)
//     fminus voices.fm --threads 4          a parallel for or parallel { } on 4 threads at most (0, the default:
//                                           one for each core; 1: each region on the calling thread alone)
//     fminus spin.fm --run spin 3 --loop-budget 1000000
//                                           the loop budget: the call may take a million iterations of its loops,
//                                           and stops early past them (also for --emit: the IR's checks, an
//                                           executable's main)
//     fminus taps.fm --run tap 20 --bounds-check
//                                           bounds checks: an index outside its array stops the call, which says
//                                           where (also for --emit, as the loop budget)
//     fminus diodetone.cir --emit fm        the FMinus module a circuit netlist generates (fminus::circuit,
//                                           docs/circuits.md); a .cir compiles and runs as that text would
//     fminus play.fm --run dc 1             play.fm's #include "diodetone.cir" reads the module it generates
//     fminus pedal.cir --self-test --emit fm   the self-test chooses the netlist's open configuration (on --threads
//                                           threads), and the module is generated with its answer; --notes prints
//                                           the plan
//     fminus pedal.cir --probes --nodes --emit fm   the module with its probe fields (Options::probes), and the
//                                           netlist's nodes as a probe sees them (Result::nodes) on stderr
#include "fminus/Circuit.h"
#include "frontend/Frontend.h"
#include "ir/CodeGen.h"
#include "jit/Call.h"
#include "jit/Jit.h"
#include "opt/BoundsCheck.h"
#include "opt/LoopBudget.h"
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

#include <algorithm>
#include <array>
#include <atomic>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <exception>
#include <format>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

namespace
{
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

    // --run's argument for a parameter: a scalar in its slot, or an array the call points to, its elements as the
    // parameter's type has them (each 4 or 8 bytes, in storage aligned to 8).
    struct RunArgument
    {
        fminus::jit::CallSlot slot;
        fminus::ast::Kind element = fminus::ast::Kind::Void;   // an array's elements' type
        std::size_t count = 0;                                  // and how many it has
        std::vector<std::uint64_t> storage;
        bool printed = false;   // a pointer the function may write through: printed after the call
    };

    // `text` read as a value of `kind` (bool, int, float or double), as CLI11 reads one, into the start of `out`.
    bool readScalar(const std::string& text, fminus::ast::Kind kind, unsigned char* out)
    {
        switch (kind)
        {
        case fminus::ast::Kind::Bool:
        {
            bool value = false;
            if (!CLI::detail::lexical_cast(text, value))
                return false;
            const std::uint8_t byte = value ? 1 : 0;
            std::memcpy(out, &byte, 1);
            return true;
        }
        case fminus::ast::Kind::Int:
        {
            std::int32_t value = 0;
            if (!CLI::detail::lexical_cast(text, value))
                return false;
            std::memcpy(out, &value, sizeof value);
            return true;
        }
        case fminus::ast::Kind::Float:
        {
            float value = 0;
            if (!CLI::detail::lexical_cast(text, value))
                return false;
            std::memcpy(out, &value, sizeof value);
            return true;
        }
        case fminus::ast::Kind::Double:
        {
            double value = 0;
            if (!CLI::detail::lexical_cast(text, value))
                return false;
            std::memcpy(out, &value, sizeof value);
            return true;
        }
        default:
            return false;
        }
    }

    // What --run passes `type` as: bool, int, float and double, and pointers to int, float and double (an array).
    bool runPasses(const fminus::ast::Type& type)
    {
        using fminus::ast::Kind;
        if (type.kind == Kind::Pointer)
        {
            if (fminus::ast::hasRows(type))
                return false;
            const Kind element = fminus::ast::pointee(type).kind;
            return element == Kind::Int || element == Kind::Float || element == Kind::Double;
        }
        return type.kind == Kind::Bool || type.kind == Kind::Int || type.kind == Kind::Float
               || type.kind == Kind::Double;
    }

    const char* withArticle(fminus::ast::Kind kind)
    {
        switch (kind)
        {
        case fminus::ast::Kind::Bool:
            return "a bool";
        case fminus::ast::Kind::Int:
            return "an int";
        case fminus::ast::Kind::Double:
            return "a double";
        default:
            return "a float";
        }
    }

    // An element of an array --run passed, as --run prints a result: the shortest decimal that reads back as it.
    std::string element(fminus::ast::Kind kind, const unsigned char* at)
    {
        switch (kind)
        {
        case fminus::ast::Kind::Int:
        {
            std::int32_t value = 0;
            std::memcpy(&value, at, sizeof value);
            return std::format("{}", value);
        }
        case fminus::ast::Kind::Double:
        {
            double value = 0;
            std::memcpy(&value, at, sizeof value);
            return std::format("{}", value);
        }
        default:
        {
            float value = 0;
            std::memcpy(&value, at, sizeof value);
            return std::format("{}", value);
        }
        }
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

    // What the circuit compiler said of the .cir files the program reads (the input and its #includes): its messages,
    // whether one failed, and each Result by the generated file's name, so that FMinus's messages on the generated
    // text can be re-cited at the netlist's lines. The include resolver cannot carry errors, so it writes them here,
    // and main prints them after the parse.
    struct CircuitLog
    {
        std::string messages;
        bool failed = false;
        std::map<std::string, fminus::circuit::Result> results;   // "x.cir.fm" -> what generated it
    };

    // --self-test's threads: SelfTestOptions::parallel on std::threads (0: one for each core), the calling thread one
    // of them.
    std::function<void(int, const std::function<void(int)>&)> threadPool(unsigned threads)
    {
        return [threads](int count, const std::function<void(int)>& job)
        {
            unsigned n = threads != 0 ? threads : std::max(1u, std::thread::hardware_concurrency());
            n = std::min(n, static_cast<unsigned>(std::max(count, 1)));
            std::atomic<int> next{0};
            std::exception_ptr failure;
            std::atomic<bool> failed{false};
            auto work = [&]
            {
                for (int i = next++; i < count; i = next++)
                {
                    try
                    {
                        job(i);
                    }
                    catch (...)
                    {
                        if (!failed.exchange(true))
                            failure = std::current_exception();
                    }
                }
            };
            std::vector<std::thread> pool;
            for (unsigned k = 1; k < n; ++k)
                pool.emplace_back(work);
            work();
            for (std::thread& t : pool)
                t.join();
            if (failure)
                std::rethrow_exception(failure);
        };
    }

    // How a .cir is generated: plainly, or after its self-test (whose messages and answer go to the log too).
    struct CircuitSettings
    {
        bool selfTest = false;
        unsigned threads = 0;
        bool notes = false;
        bool probes = false;   // Options::probes
        bool nodes = false;    // print Result::nodes
    };

    // Result::nodes, a line each, cited at each node's first mention: "pedal.cir:12:8: node 3 'n1': rate 4, dc 4.5 V,
    // readable" (or "not readable: <why>").
    std::string nodeLines(const fminus::circuit::Result& result, const std::string& name)
    {
        std::string out;
        for (std::size_t k = 0; k < result.nodes.size(); ++k)
        {
            const fminus::circuit::Node& n = result.nodes[k];
            out += std::format("{}:{}:{}: node {} '{}': rate {}, dc {:.9g} V, {}\n", name, n.line, n.column, k, n.name,
                               n.rate, n.dc, n.readable ? std::string("readable") : "not readable: " + n.why);
        }
        return out;
    }

    // `text` of the netlist `name`, generated as `settings` say; its messages go to `log`.
    fminus::circuit::Result generateCircuit(std::string_view text, const std::string& name,
                                            const CircuitSettings& settings, CircuitLog& log)
    {
        fminus::circuit::Options options;
        options.probes = settings.probes;
        std::string selfTestWarnings;
        if (settings.selfTest)
        {
            fminus::circuit::SelfTestOptions test;
            test.parallel = threadPool(settings.threads);
            const fminus::circuit::SelfTestResult tested = fminus::circuit::selfTest(text, name, options, test);
            if (!tested.errors.empty())
            {
                log.messages += tested.errors + tested.warnings;
                log.failed = true;
                return {};
            }
            log.messages += tested.notes;
            selfTestWarnings = tested.warnings;
            options.chosen = tested.choice;
        }
        fminus::circuit::Result result = fminus::circuit::generate(text, name, options);
        log.messages += result.errors + result.warnings;
        if (settings.selfTest)
        {
            // the self-test's own warnings (a block that rings), which generate does not repeat
            std::size_t at = 0;
            const std::string& warnings = selfTestWarnings;
            while (at < warnings.size())
            {
                const std::size_t end = std::min(warnings.find('\n', at), warnings.size());
                const std::string line = warnings.substr(at, end - at + 1);
                if (result.warnings.find(line) == std::string::npos)
                    log.messages += line;
                at = end + 1;
            }
        }
        if (settings.notes)
            log.messages += result.notes;
        if (settings.nodes)
            log.messages += nodeLines(result, name);
        log.failed = log.failed || !result.errors.empty();
        log.results[name + ".fm"] = result;
        return result;
    }

    // FMinus's messages, with each line that cites a module generated from a netlist ("x.cir.fm:12:8: error: ...")
    // re-cited at the netlist's line it came from (Result::lines): "x.cir:4:1: error: ... (in the module generated
    // from x.cir, line 12)".
    std::string recited(const std::string& messages, const CircuitLog& log)
    {
        std::string out;
        std::size_t at = 0;
        while (at < messages.size())
        {
            const std::size_t end = std::min(messages.find('\n', at), messages.size());
            std::string line = messages.substr(at, end - at);
            at = end + 1;
            for (const auto& [generated, result] : log.results)
            {
                if (line.rfind(generated + ":", 0) != 0)
                    continue;
                const std::size_t lineEnd = line.find(':', generated.size() + 1);
                const std::size_t columnEnd =
                    lineEnd == std::string::npos ? std::string::npos : line.find(':', lineEnd + 1);
                if (columnEnd == std::string::npos)
                    break;
                const int number = std::atoi(line.substr(generated.size() + 1, lineEnd - generated.size() - 1).c_str());
                const std::string netlist = generated.substr(0, generated.size() - 3);
                const std::string base = std::string(llvm::sys::path::filename(netlist));
                const int from = number >= 1 && static_cast<std::size_t>(number) <= result.lines.size()
                                     ? result.lines[static_cast<std::size_t>(number) - 1]
                                     : 0;
                line = netlist + (from > 0 ? ":" + std::to_string(from) + ":1" : std::string()) +
                       line.substr(columnEnd) + " (in the module generated from " + base + ", line " +
                       std::to_string(number) + ")";
                break;
            }
            out += line + "\n";
        }
        return out;
    }

    // Where #include "name" finds its file, as C's quoted form looks: beside the including file, then in each of
    // `directories` in turn (an absolute name is itself). A file is named as found (the directory and the name, its
    // ./ and ../ removed), except one read already by another name: the first name it was found by, so that the
    // scanner reads it once (the program's own file, `input`, is read already). A .cir file is a circuit netlist: its
    // text is the module fminus::circuit generates, named <file>.fm (so a second #include of it reads nothing), and
    // what the generator says goes to `log`.
    class DiskIncludes
    {
    public:
        DiskIncludes(std::vector<std::string> directories, const std::string& input, CircuitLog* log,
                     CircuitSettings settings)
            : directories_(std::move(directories)), log_(log), settings_(settings)
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
                // generated once, so a second #include says nothing again (the scanner reads it once anyway)
                auto [it, first] = generated_.try_emplace(found);
                if (first)
                    it->second = generateCircuit((*file)->getBuffer(), found, settings_, *log_).source;
                return fminus::frontend::IncludedFile{found + ".fm", it->second};
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
        std::map<std::string, std::string> names_;       // a file's real path, and the name it goes by
        std::map<std::string, std::string> generated_;   // a .cir's name, and the module generated from it
        CircuitLog* log_;                                // main's: the resolver is copied into a std::function
        CircuitSettings settings_;
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
    std::vector<std::string> runArgs;   // read as the function's parameters' types (an array's: a, b, ...)
    bool entry = false;
    unsigned polyLanes = 0;
    std::vector<std::string> streams;
    std::vector<std::string> freezes;
    bool noStrictAliasing = false;
    unsigned knobVersions = fminus::ir::Options{}.knobVersions;
    std::string denormals = "preserve";
    std::string math = "vector";
    std::vector<std::string> includeDirectories;
    std::vector<std::string> defineSpecs;
    unsigned threads = 0;
    std::optional<std::int64_t> loopBudget;
    bool boundsCheck = false;
    CircuitSettings circuitSettings;
    app.add_option("input", input, "FMinus source file ('-' for stdin)")->required();
    app.add_option("args", runArgs,
                   "The arguments of --run's function: a bool, an int, a float or a double each, or an array's "
                   "elements separated by commas (1,2,3) for a pointer");
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
    app.add_option("-D", defineSpecs,
                   "Define a macro for #if, #ifdef and #ifndef: NAME (its value 1) or NAME=value, as clang's -D")
        ->allow_extra_args(false);
    app.add_option("--threads", threads,
                   "The most threads a parallel for or parallel { } runs on, the calling thread's among them (0: one "
                   "for each core; 1: each region on the calling thread alone); and --self-test's threads")
        ->capture_default_str();
    app.add_option("--loop-budget", loopBudget,
                   "Compile with the loop budget, runaway-loop protection: a loop's iterations count (as it starts, "
                   "for a loop whose count is known), and each call of a recursive function, and a call that takes more "
                   "than this many returns early (--run's call, an executable's main; --emit ll and asm show the checks)")
        ->check(CLI::NonNegativeNumber);
    app.add_flag("--bounds-check", boundsCheck,
                 "Compile with bounds checks, for development: an access outside an array the compiler can see (a[i], "
                 "m.taps[i], a + i, &a[i]) stops the call, which says where, the array and the index (--run's call, "
                 "an executable's main; --emit ll and asm show the checks)");
    app.add_flag("--self-test", circuitSettings.selfTest,
                 "Run the self-test of each circuit netlist (.cir) the program reads and generate it with the answer: "
                 "it chooses the configuration the netlist leaves open (seconds; docs/circuits.md)");
    app.add_flag("--notes", circuitSettings.notes, "Print the circuit compiler's notes on each netlist (its plan)");
    app.add_flag("--probes", circuitSettings.probes,
                 "Generate each circuit netlist's module with its probe fields (docs/circuits.md, \"Probes\"): the "
                 "same output");
    app.add_flag("--nodes", circuitSettings.nodes,
                 "Print each circuit netlist's nodes as a probe sees them: rate, DC volts, whether it can be read");
    CLI11_PARSE(app, argc, argv);
    circuitSettings.threads = threads;
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
    CircuitLog circuits;
    if (isNetlist(input))
    {
        const fminus::circuit::Result circuit = generateCircuit(text, input, circuitSettings, circuits);
        llvm::errs() << circuits.messages;
        circuits.messages.clear();
        if (circuits.failed)
            return 1;
        if (emit == "fm" && run.empty())
            return writeText(output, circuit.source);
        text = circuit.source;
        sourceName = input + ".fm";   // FMinus's messages cite the lines --emit fm prints, re-cited at the netlist's
    }
    else if (emit == "fm")
        return fail("--emit fm: " + input + " is not a circuit netlist (.cir)");
    fminus::frontend::Defines defines;
    for (const std::string& spec : defineSpecs)
    {
        const std::size_t equals = spec.find('=');
        const std::string name = spec.substr(0, equals);
        if (const std::optional<std::string> wrong = fminus::frontend::macroNameError(name))
            return fail("-D " + spec + ": " + *wrong);
        defines[name] = equals == std::string::npos ? "1" : spec.substr(equals + 1);
    }
    fminus::Diagnostics diagnostics(sourceName);
    std::optional<fminus::ast::Program> program = fminus::frontend::parse(
        text, diagnostics, DiskIncludes(includeDirectories, diagnostics.fileName(), &circuits, circuitSettings),
        defines);
    llvm::errs() << circuits.messages;
    if (circuits.failed)
        return 1;
    if (!program)
    {
        llvm::errs() << recited(diagnostics.format(), circuits);
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
    options.loopBudget = loopBudget.has_value();
    options.boundsCheck = boundsCheck;

    // Syntax tree -> LLVM IR.
    auto context = std::make_unique<llvm::LLVMContext>();
    std::unique_ptr<llvm::Module> module = fminus::ir::lower(*program, *context, diagnostics, options);
    if (!module)
    {
        llvm::errs() << recited(diagnostics.format(), circuits);
        return 1;
    }
    llvm::errs() << recited(diagnostics.format(), circuits);   // its warnings, as clang prints them, if any

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
        for (const fminus::ast::Param& param : function->params)
        {
            if (!runPasses(param.type))
                return fail(std::format("--run: '{}' takes a parameter of type '{}'; --run passes bools, ints, floats, "
                                        "doubles and arrays of ints, floats and doubles only",
                                        run, fminus::ast::typeName(param.type)));
        }
        const std::optional<fminus::jit::CallType> result = fminus::jit::callType(function->returnType);
        if (!result || *result == fminus::jit::CallType::Pointer)   // a vector stays inside FMinus, or goes to a C++
            return fail(std::format("--run: '{}' returns '{}'; --run prints bools, ints, floats and doubles only",
                                    run, fminus::ast::typeName(function->returnType)));   // host (docs/simd.md)
        // Each argument read as its parameter's type, as CLI11 reads a number of that type; an array's elements each
        // so, in storage that lives until the call has returned.
        std::vector<RunArgument> arguments(runArgs.size());
        std::vector<fminus::jit::CallType> parameters;
        for (std::size_t i = 0; i < runArgs.size(); ++i)
        {
            const fminus::ast::Type type = function->params[i].type;
            RunArgument& argument = arguments[i];
            parameters.push_back(*fminus::jit::callType(type));
            if (!fminus::ast::isPointer(type))
            {
                if (!readScalar(runArgs[i], type.kind, argument.slot.bytes))
                    return fail(std::format("--run: '{}' is not {}", runArgs[i], withArticle(type.kind)));
                continue;
            }
            argument.element = fminus::ast::pointee(type).kind;
            argument.printed = !type.pointeeConst;
            const std::size_t size = argument.element == fminus::ast::Kind::Double ? 8 : 4;
            std::vector<std::string> texts;
            for (std::size_t at = 0; at <= runArgs[i].size();)
            {
                const std::size_t comma = std::min(runArgs[i].find(',', at), runArgs[i].size());
                texts.push_back(runArgs[i].substr(at, comma - at));
                at = comma + 1;
            }
            argument.count = texts.size();
            argument.storage.assign((argument.count * size + 7) / 8, 0);
            for (std::size_t k = 0; k < texts.size(); ++k)
            {
                auto* at = reinterpret_cast<unsigned char*>(argument.storage.data()) + k * size;
                if (!readScalar(texts[k], argument.element, at))
                    return fail(std::format("--run: '{}' is not {}, an element of the array '{}'", texts[k],
                                            withArticle(argument.element), runArgs[i]));
            }
            argument.slot = fminus::jit::CallSlot::of(static_cast<void*>(argument.storage.data()));
        }

        // What each bounds check's number means (opt/BoundsCheck.h), read before the JIT takes the module.
        const std::vector<fminus::opt::BoundsSite> sites = fminus::opt::boundsSites(*module);
        // A program with a parallel region runs it on the runtime's pool, started for the call (runtime/Runtime.h).
        const RuntimePool pool(module->getFunction("fminus_rt_parallel_for") != nullptr && threads != 1);
        auto jit = fminus::jit::Jit::create(optimisation);
        if (!jit)
            return fail(jit.takeError());
        if (llvm::Error error = (*jit)->add(std::move(module), std::move(context)))
            return fail(std::move(error));
        if (auto address = (*jit)->lookup(run); !address)
            return fail(address.takeError());
        // The thunk that calls it by its signature (jit/Call.h), as Program::call does.
        auto thunk = fminus::jit::callThunk(**jit, run, *result, parameters);
        if (!thunk)
            return fail(thunk.takeError());
        // The program's loop budget (opt/LoopBudget.h), set for the call, as a host sets it.
        fminus_rt_loop_budget_state* budget = nullptr;
        if (loopBudget)
        {
            auto found = (*jit)->lookup(fminus::opt::kLoopBudgetSymbol);
            if (!found)
                return fail(found.takeError());
            budget = found->toPtr<fminus_rt_loop_budget_state*>();
            budget->left = *loopBudget;
            budget->stopped = 0;
        }
        // The program's bounds checks' record, read after the call, as a host reads it.
        const fminus_rt_bounds_state* record = nullptr;
        if (boundsCheck)
        {
            auto found = (*jit)->lookup(fminus::opt::kBoundsSymbol);
            if (!found)
                return fail(found.takeError());
            record = found->toPtr<const fminus_rt_bounds_state*>();
        }
        // Through the thunk: each argument goes where the target's convention puts its type, a bool zero-extended, and
        // the result comes back as its type. A bool prints as true or false, and a float or a double as the shortest
        // decimal that reads back as it; then each array the function may have written, as it is now.
        std::vector<fminus::jit::CallSlot> slots;
        for (const RunArgument& argument : arguments)
            slots.push_back(argument.slot);
        fminus::jit::CallSlot out;
        (*thunk)(slots.data(), &out);
        switch (*result)
        {
        case fminus::jit::CallType::Float:
            llvm::outs() << std::format("{}\n", out.as<float>());
            break;
        case fminus::jit::CallType::Double:
            llvm::outs() << std::format("{}\n", out.as<double>());
            break;
        case fminus::jit::CallType::Bool:
            llvm::outs() << std::format("{}\n", out.as<std::uint8_t>() != 0);
            break;
        case fminus::jit::CallType::Int:
            llvm::outs() << std::format("{}\n", out.as<std::int32_t>());
            break;
        case fminus::jit::CallType::Void:
        case fminus::jit::CallType::Pointer:   // refused above
            break;
        }
        for (std::size_t i = 0; i < arguments.size(); ++i)
        {
            const RunArgument& argument = arguments[i];
            if (!argument.printed)
                continue;
            const std::size_t size = argument.element == fminus::ast::Kind::Double ? 8 : 4;
            std::string line = function->params[i].name.empty() ? std::format("argument {}", i + 1)
                                                                 : function->params[i].name;
            line += " = {";
            for (std::size_t k = 0; k < argument.count; ++k)
                line += (k == 0 ? "" : ", ")
                        + element(argument.element,
                                  reinterpret_cast<const unsigned char*>(argument.storage.data()) + k * size);
            llvm::outs() << line << "}\n";
        }
        // A check that failed, on stderr: what the call printed is what it returned early, and the status is 4.
        if (record != nullptr && record->site > 0 && static_cast<std::size_t>(record->site) <= sites.size())
        {
            llvm::errs() << std::format("fminus: --run: '{}' stopped: {}\n", run,
                                        fminus::opt::boundsMessage(sites[static_cast<std::size_t>(record->site) - 1],
                                                                   record->index));
            return 4;
        }
        // What the call took of its budget, on stderr; when it ran out, what it printed is what it returned early,
        // and the status is 3.
        if (budget != nullptr)
        {
            if (budget->stopped != 0)
            {
                llvm::errs() << std::format("fminus: --run: '{}' ran past its loop budget of {} and returned early\n",
                                            run, *loopBudget);
                return 3;
            }
            llvm::errs() << std::format("fminus: --run: '{}' took {} of its loop budget of {}\n", run,
                                        *loopBudget - budget->left, *loopBudget);
        }
        return 0;
    }

    // LLVM IR -> optimised for this machine -> IR, assembly, an object file or an executable.
    if (entry || emit == "exe")
    {
        if (llvm::Error error = fminus::aot::addEntryPoint(*module, loopBudget))
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
