// src/ir/CodeGen.h: FMinus v0's syntax tree to LLVM IR.
#pragma once

#include "frontend/Ast.h"
#include "frontend/Diagnostics.h"

#include <map>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace llvm
{
    class LLVMContext;
    class Module;
} // namespace llvm

namespace fminus::ir
{
    // How a program reads a module's parameter (`param float cutoff;`), the host's choice when it compiles:
    //   Knob     its value slot, read once a call with an atomic load where a method starts: the host writes it
    //            whenever it likes, from any thread (the default)
    //   Stream   the samples its stream slot points to, cutoff[i] the i-th: the host sets the pointer before each call
    //   Frozen   `value`, a constant compiled in, so the code around it folds (a frozen `mode` picks one branch)
    // Whatever the binding, the parameter's slot in the module is the same: { const float* stream; T value; }, T the
    // parameter's type (bool a byte). So a program compiled with other bindings runs on the same instances.
    struct ParamBinding
    {
        enum class Kind
        {
            Knob,
            Stream,
            Frozen,
        };
        Kind kind = Kind::Knob;
        double value = 0.0;   // a frozen parameter's, converted to its type as C converts
    };

    // What a program does with subnormal floats (Denormals.cpp), the host's choice when it compiles:
    //   Preserve  IEEE's gradual underflow, as C has it: a subnormal is a number like any other (the default)
    //   Flush     flush-to-zero, as a plugin host usually runs: a subnormal input reads as 0, a subnormal result is 0,
    //             each with its sign. Every function the program defines says so to LLVM (denormal_fpenv), so its
    //             folding agrees with the hardware, and each entry point (a [[clang::nonblocking]] function that no
    //             function of the program calls) sets the floating-point mode on entry and restores it on return.
    //             Whatever else a host calls, it calls with the mode set itself.
    enum class Denormals
    {
        Preserve,
        Flush,
    };

    // How sinf, cosf, tanf, expf, tanhf and logf compute, scalar or vector (ir/Math.cpp lowers them, runtime/Math.cpp
    // computes them). fabsf, sqrtf, floorf, fminf and fmaxf are exact, so they are LLVM's intrinsics in every mode.
    //   Vector   FMinus's own kernels (runtime/MathKernels.h), which give a float the same result wherever it is: in
    //            a scalar loop or its vectorised body, a float4 or a poly float of any width, at any optimisation
    //            level, a constant or not, on any target. Each is one sequence of IEEE operations, which every lane
    //            computes alike: inline from -O1 (opt/MathKernels.h), and the runtime's functions at -O0
    //            (runtime/Runtime.h, fminus_rt_sinf and fminus_rt_sin_f4). Loops of them still vectorise. The
    //            default.
    //   Libm     libm's sinf, cosf, ... (through the runtime, so that nothing folds them), a lane at a time: C++'s
    //            std::sin of a float, bit for bit, everywhere. Loops of them don't vectorise.
    //   Fast     LLVM's intrinsics (llvm.sin, ...), as clang has them: LLVM folds constants with the host's double
    //            functions, pairs sinf and cosf into one sincos call, and the vectorisers and ReplaceWithVeclib use the
    //            target's vector math library (support/HostTarget.h), so a result can depend on where a block ends,
    //            on the optimisation level, on a poly float's width and on whether a parameter is frozen.
    // The module records the mode in its kMathFlag (support/HostTarget.h), which tells optimiseModule whether to give
    // the pipeline the vector math library: only Fast has it.
    enum class Math
    {
        Vector,
        Libm,
        Fast,
    };

    struct Options
    {
        // The lanes of a poly float: the target's vector register width in floats (support/HostTarget.h: polyLanes
        // asks the TargetMachine). 4 is NEON's and SSE's; any power of two from 1 to 64 lowers, wider ones in parts.
        unsigned polyLanes = 4;

        // How the transcendental builtins compute (above).
        Math math = Math::Vector;

        // The target's data layout (TargetMachine::createDataLayout), which a module's layout and sizeof follow. Empty
        // is LLVM's default, which agrees with the 64-bit targets FMinus runs on for every type it has.
        std::string dataLayout{};

        // Each parameter's binding, by its qualified name ("Lowpass::cutoff"); a parameter not listed is a knob.
        std::map<std::string, ParamBinding> parameters{};

        // C's strict-aliasing rule, told to LLVM: every load and store of memory carries its type's TBAA tag
        // (Tbaa.cpp), so a store of a float cannot change an int or a pointer, and a loop over a module's buffer loads
        // the module's fields once. Off, clang's -fno-strict-aliasing, nothing is tagged, and LLVM assumes any store
        // may change anything: for a program that reads memory as a type other than the one it holds.
        bool strictAliasing = true;
        // Knob versioning's budget (opt/KnobVersioning.h), from -O1: at most this many copies of a method's body, one
        // for each combination of the values of the int and bool knobs (and the streams' flat tests) its loops branch
        // on, so that each copy folds them as constants. 0 turns it off. It travels with the module, as the module flag
        // "fminus.knob-versions", to wherever the module is optimised. The copies compute what the method does, bit
        // for bit, in the Vector and Libm math modes; only Math::Fast can move a math call's last bits, where a copy
        // vectorises a loop the method did not, or folds a knob's constant into a call (opt/KnobVersioning.h).
        unsigned knobVersions = 8;
        // Subnormals preserved or flushed to zero (Denormals, above).
        Denormals denormals = Denormals::Preserve;

        // The target's triple (TargetMachine::getTargetTriple), which says how an entry point sets the floating-point
        // mode under Denormals::Flush: FPCR on arm64, MXCSR on x86-64; and whether libm sets errno, which a prototype
        // of libm's function then says it writes (on Darwin it sets none: Functions.cpp, describeLibm). Empty is this
        // process's, which is what the JIT and an executable built here run on.
        std::string targetTriple{};

        // The most threads a parallel for or parallel { } runs on, the calling thread's among them (Parallel.cpp):
        // 0 is as many as the runtime's pool has, 1 runs each region on the calling thread alone. Each region hands
        // it to fminus_rt_parallel_for (runtime/Runtime.h), which also runs every region alone until the pool starts.
        unsigned threads = 0;

        // The loop budget, runaway-loop protection (opt/LoopBudget.h): every branch back to the start of a loop, and
        // every call of a function that can call itself again, counts down the program's budget,
        // fminus_rt_loop_budget (runtime/Runtime.h), and a call that runs out returns early with its flag set. The
        // module defines the budget, unlimited until its host sets it, and records the option as the module flag
        // "fminus.loop-budget", for the pass at the end of the pipeline, wherever the module is optimised. A program
        // that declares a function of the budget's name is then an error. Off by default: the code is as it was.
        bool loopBudget = false;

        // Bounds checks, for development builds (Bounds.cpp, opt/BoundsCheck.h): every access whose array the code
        // generator can see (a[i], m.taps[i], voices[v], *(a + i), a pointer formed from one, a poly_load through one)
        // is checked against the array's extent, and a call whose check fails returns early, with the check recorded
        // in the program's record, fminus_rt_bounds (runtime/Runtime.h). The module defines the record, describes
        // each check in its named metadata "fminus.bounds.sites", and records the option as the module flag
        // "fminus.bounds-check", for the pass at the end of the pipeline, wherever the module is optimised. A program
        // that declares a function of the record's name is then an error. Off by default: the code is as it was.
        bool boundsCheck = false;
    };

    // Lowers `program` into a new module in `context`, named after the diagnostics' file. Every FMinus function becomes
    // an external LLVM function of the same name with the C calling convention (float is f32, double f64), so C++
    // calls the JIT'd or linked code as float(float, ...). Returns nullptr when it reported errors into `diagnostics`; it reports every
    // error it finds, not only the first.
    std::unique_ptr<llvm::Module> lower(const ast::Program& program, llvm::LLVMContext& context,
                                        Diagnostics& diagnostics, const Options& options = {});

    // The builtins a program can call, with their parameter counts: <math.h>'s float functions (fabsf, sqrtf,
    // sinf, ...), lowered to LLVM intrinsics, or the transcendental ones to the runtime's functions (Math), then its
    // double functions that are instructions (sqrt, fabs, floor, fmin, fmax, fma).
    struct BuiltinInfo
    {
        std::string_view name;
        unsigned arity;
    };
    std::vector<BuiltinInfo> builtins();

    // The web library's builtins (Calls.cpp, Web.cpp; a build with FMINUS_WEB). A module that calls http_get,
    // http_post or http_listen defines the program's web context, this global, a pointer, null until its host sets it
    // (a null context reaches nothing and listens nowhere: src/web/Web.h), and every such call passes it to the library.
    // A program that declares a function of the name is an error then.
    inline constexpr const char* kWebContextSymbol = "fminus_web_context";
    // What every name of the web library's begins with (its C functions, web/WebSymbols.def, and the context's
    // global): in every build, a function of a program cannot be named so (Functions.cpp).
    inline constexpr std::string_view kWebPrefix = "fminus_web_";

    // One call of http_get or http_post, as the code generator recorded it in the module's named metadata
    // "fminus.web.requests", in the order it lowered them: its method, "GET" or "POST"; its URL literal as a pattern,
    // each %d a "*" and %% a "%"; and where the call is (its file as the errors name it, its line and column).
    struct WebRequest
    {
        std::string method;
        std::string pattern;
        std::string file;
        int line = 0;
        int column = 0;
    };
    std::vector<WebRequest> webRequests(const llvm::Module& module);

    // One call of http_is, as the code generator recorded it in the module's named metadata "fminus.web.routes", in
    // the order it lowered them (WebServer.cpp; a build with FMINUS_WEB_SERVER): its route literal's method ("GET",
    // "POST", "PUT", "PATCH" or "DELETE") and its path as a pattern, each %d a "*"; and where the call is (its file as
    // the errors name it, its line and column).
    //
    // A route statement's (Serve.cpp) is recorded there too, where its `route` is, with `statement` set and its
    // parameters: each one's name, its type ("int", "float", "bool", "string" or "http_request"), what fills it
    // ("path", a hole; "query", the query's field of its name; "request", the request itself), and its fminus::range
    // and fminus::default when it has them (a string's default its text).
    struct WebRouteParameter
    {
        std::string name;
        std::string type;
        std::string source;
        std::optional<double> minimum;
        std::optional<double> maximum;
        std::optional<double> defaultValue;
        std::optional<std::string> defaultText = std::nullopt;   // a string's
    };
    struct WebRoute
    {
        std::string method;
        std::string pattern;
        std::string file;
        int line = 0;
        int column = 0;
        bool statement = false;
        std::vector<WebRouteParameter> parameters = {};
    };
    std::vector<WebRoute> webRoutes(const llvm::Module& module);

    // The file interface's builtins (Calls.cpp, Files.cpp; a build with FMINUS_FILES). A module that calls file_read,
    // file_list, file_write or file_append defines the program's file context, this global, a pointer, null until its
    // host sets it (a null context reaches no file: runtime/Files.h), and every such call passes it to the runtime. A
    // program that declares something of the name is an error then.
    inline constexpr const char* kFilesContextSymbol = "fminus_rt_files_context";
    // What every name of the file interface's begins with (its C functions, runtime/RuntimeSymbols.def, an executable's
    // start and stop, and the context's global): in every build, a function or a constant of a program cannot be named
    // so (Functions.cpp, Constants.cpp).
    inline constexpr std::string_view kFilesPrefixes[] = {"fminus_rt_file_", "fminus_rt_files_"};

    // One call of file_read, file_list, file_write or file_append, as the code generator recorded it in the module's
    // named metadata "fminus.files", in the order it lowered them (Files.cpp): its operation ("read", "list", "write"
    // or "append"); its name as a pattern, each %d a "*" and %% a "%" (a listing's own "*" stays one); and where the
    // call is (its file as the errors name it, its line and column).
    struct FileUse
    {
        std::string operation;
        std::string pattern;
        std::string file;
        int line = 0;
        int column = 0;
    };
    std::vector<FileUse> files(const llvm::Module& module);
} // namespace fminus::ir
