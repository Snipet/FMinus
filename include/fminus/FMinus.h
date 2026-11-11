// include/fminus/FMinus.h: FMinus as a C++ library. A Program compiles FMinus source while the host runs, for the CPU
// it runs on, and gives its functions as C function pointers. This is the one header a host includes: it has no LLVM
// in it and needs C++17, and the host links FMinus::FMinus (README.md, "Using FMinus as a library").
//
//   std::string errors;
//   auto program = fminus::Program::compile(source, errors);           // nullptr when it does not compile
//   auto* gain = program->function<void(const float*, float*, int, float)>("gain", errors);
//   gain(in, out, n, 0.5f);                                             // one call a block: the loop is FMinus's
//
// A function's C++ type is its FMinus declaration's C type, and FMinus checks nothing at this boundary: a Signature
// that does not match the declaration is undefined behaviour. Every function has C's calling convention and its own
// name, unmangled. A host that learns a function's signature only as it runs calls it by name instead, through call(),
// which checks the arguments against the declaration (Callable, below).
//
//   FMinus                         C++
//   float, double, int, bool, void float, double, int, bool, void
//   float*, const int*, double*,   the same pointer (restrict is a promise to FMinus's optimiser, not a type)
//   void*, ...
//   float2, float4                 clang's float __attribute__((ext_vector_type(2))), (4)
//   float8                         none: the C ABI passes it in memory, FMinus in registers (docs/simd.md); keep it
//                                  inside FMinus
//   poly float                     the vector of polyLanes() floats: a float4 at 4 lanes (NEON's), and at 8 or more
//                                  none, as float8; keep it inside FMinus too
//   OnePole* (a module's pointer)  void*, or a pointer to a C++ struct with the same fields in the same order (which
//                                  C++ lays out as FMinus does); module() says where each field is. A module itself
//                                  never crosses: FMinus passes and returns modules by pointer only
//
// A module's method is a function too, named as C++ qualifies it, whose first parameter is the instance it runs on:
// `float tick(float x)` in `module OnePole` is function<float(void*, float)>("OnePole::tick", errors).
//
// Threads: compile(), and the first look-up of each function, run LLVM's optimiser and code generator on the calling
// thread. They take milliseconds and allocate, so in a plugin they run off the audio thread, and the pointer is handed
// over (an std::atomic does). Calling a JIT'd function is a plain call; mark the ones the audio thread calls
// [[clang::nonblocking]], and FMinus refuses any that could reach malloc, calloc or free. Different Programs may
// compile and look up on different threads at once.
//
// A program's own threads: its parallel for loops and parallel { } blocks run on FMinus's pool, a thread for each core
// but one, shared by every Program in the process. The first Program with a parallel region starts it when it
// compiles, and the last one's destructor stops it. The thread that calls into the program works on each region too,
// and the region has finished when the statement has. Running one allocates nothing and takes no lock, so a
// nonblocking process() may have them (Options::threads limits them).
//
// Lifetime: a function pointer is valid while its Program lives, and its code stays where it is (codeRanges(), for a
// profiler). Destroy a Program only when no thread can still be running its code.
//
// Parameters: a module's `param float cutoff;` is a value the host sets, read in a method as cutoff[i], at sample i of
// the call. How it reads is the host's choice when it compiles (Options::parameters, Binding): a knob (the value, read
// once when the method starts, and safe to set from any thread), a stream (a buffer of samples, an LFO's say, or null
// for a call in which it holds still at its value), or frozen (a constant compiled in). Module::parameters describes
// each; setValue, setStream and setDefaults write them.
//
//   Program::Options options;
//   options.parameters["Lowpass::cutoff"] = Program::Binding::stream();   // modulated: a sample each
//   auto program = Program::compile(source, errors, options);
//   const Program::Parameter* cutoff = program->parameter("Lowpass::cutoff");
//   Program::setStream(lowpass, *cutoff, lfo);                            // before each call of Lowpass::process
#pragma once

#include "fminus/Config.h"   // IWYU pragma: export (FMINUS_VERSION_STRING, the LLVM this build links)

#include <cstddef>
#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <type_traits>
#include <vector>

namespace fminus
{
    class Program
    {
    public:
        // How a module's parameter reads in its methods, chosen when the program compiles. Every binding has the same
        // slot in an instance (Parameter), so one instance can be run by programs that bind it differently.
        //   Knob    the slot's value, loaded once when the method starts, with an atomic load: the host may set it
        //           from any thread at any time (setValue), and the method sees one value it set, never a torn one.
        //           Held in a register from then on, as fast as a local. The default.
        //   Stream  a float parameter's buffer: cutoff[i] is samples[i], from the pointer the host sets before each
        //           call (setStream). Sample-accurate: an audio-rate signal, an envelope, a smoothed knob. A null
        //           pointer is a flat call: cutoff[i] is then the slot's value (setValue), read as a knob's, so a host
        //           whose smoother is idle passes no buffer, and the method runs its flat copy (Options::knobVersions),
        //           with what it computes from the parameter out of its loop.
        //   Frozen  `value`, compiled in as a constant: a knob that will not move while this program plays. The
        //           optimiser folds it as it folds a constant (a branch on a frozen bool disappears); the host
        //           compiles again to move it.
        struct Binding
        {
            enum class Kind
            {
                Knob,
                Stream,
                Frozen,
            };

            Kind kind = Kind::Knob;
            double value = 0.0;   // a frozen parameter's: an int's truncated toward 0, a bool's true unless 0

            static Binding knob()
            {
                return {};
            }

            static Binding stream()
            {
                return {Kind::Stream, 0.0};
            }

            static Binding frozen(double value)
            {
                return {Kind::Frozen, value};
            }
        };

        // What the program does with subnormal floats and doubles, chosen when it compiles.
        //   Preserve  IEEE's gradual underflow, as C has it: a subnormal is a number like any other. The default.
        //   Flush     flush-to-zero, the mode a plugin's audio thread runs in: a subnormal input reads as 0 and a
        //             subnormal result is 0, each with its sign. LLVM folds and optimises for that mode, and each entry
        //             point, a function marked [[clang::nonblocking]] that no function of the program calls, sets it
        //             when called and restores the caller's on return (FPCR.FZ on arm64, MXCSR's FTZ and DAZ on
        //             x86-64). Writing the register is slow (about 18 ns a call on an M5), so an entry point called
        //             in that mode already writes nothing (under 1 ns): set it around the block, as a host's ScopedFtz
        //             does, and call anything else in it too. The code assumes the mode: run in IEEE's, a subnormal's
        //             result may differ from what the compiler folded. One gap: a float/double conversion of a
        //             subnormal that only LLVM's optimiser finds constant is folded as IEEE's (recipe 33).
        enum class Denormals
        {
            Preserve,
            Flush,
        };

        // How sinf, cosf, tanf, expf, tanhf and logf compute, chosen when the program compiles (fabsf, sqrtf, floorf,
        // fminf and fmaxf are exact, and the same in every mode, as are the double functions sqrt, fabs, floor, fmin,
        // fmax and fma; a double has no sin of FMinus's: sinf rounds it to a float, and a prototype of C's, `double
        // sin(double x);`, calls the C library's, which is no audio-thread code).
        //   Vector  a float gets one result wherever the program computes it: in a loop's vectorised body or its
        //           remainder, so wherever a block ends; in a float4, a float8 or a poly float of any width; at any
        //           optimisation level; from a frozen parameter or a knob; on arm64 and x86-64 alike. What it computes
        //           is fminus::math's (below): FMinus's own kernels, within 1.56 ulp of the exact result (tanf within
        //           3.35), and loops of them vectorise. The default.
        //   Libm    the C library's sinf, cosf, ..., a lane at a time, so C++'s std::sin of a float, bit for bit, on
        //           every target. Loops of them don't vectorise. (A C++ std::sin and std::cos of one argument are
        //           not sinf and cosf at -O1 and up on Apple systems: clang makes them one __sincosf_stret.)
        //   Fast    LLVM's intrinsics, as clang compiles <math.h>: the vector library where LLVM vectorises, libm
        //           elsewhere, constants folded with the compiling machine's own functions. Usually the fastest, and
        //           a result may change in its last bits with any of the things above.
        enum class Math
        {
            Vector,
            Libm,
            Fast,
        };

        // A file an #include reads: its name, as the error messages give it, and its text. The name is also how the
        // compiler knows a file it has read: each is read once, as if it began with #pragma once, so a second
        // #include of the same name (or of Options::fileName, the program's own) reads nothing.
        struct IncludedFile
        {
            std::string fileName;   // what the error messages call it, and how a second #include of it is known
            std::string source;     // its text
        };

        // The file of `#include "name"` in the file named `from` (Options::fileName, or an IncludedFile's fileName),
        // or nothing when there is none: the error "'name' file not found". It runs on the compiling thread, once for
        // each #include read.
        using IncludeResolver =
            std::function<std::optional<IncludedFile>(std::string_view name, std::string_view from)>;

        // Which of the program's LLVM IR it keeps as text, chosen when it compiles: for a host that shows what FMinus
        // made of a program. The text is LLVM's, a module as LLVM prints it (a .ll file), and the same for the same
        // source and options.
        //   None       nothing is printed or held, and the program compiles as it would without the option. The
        //              default.
        //   Lowered    the IR as the code generator wrote it, set up for this machine (its triple and data layout),
        //              before any pass, LLVM's or FMinus's, has run (loweredIr).
        //   Optimised  the IR the JIT generates the machine code from: Lowered after the pipeline at
        //              Options::optimisation, FMinus's own passes among them. It is what fminus --emit ll prints for
        //              the same source and options, its file named as Options::fileName (optimisedIr).
        //   Both       Lowered and Optimised.
        // To print the optimised IR, compile() runs the optimiser itself, on its calling thread, and the first look-up
        // of a function then runs only the code generator. A text does not change once compile() has returned, so any
        // thread may read it while the Program lives.
        enum class KeepIr
        {
            None,
            Lowered,
            Optimised,
            Both,
        };

        // What a program may reach of the network (Options::web), in a build with the web library (FMINUS_WEB,
        // fminus/Config.h's FMINUS_HAS_WEB; docs/web.md): the policy the library holds the program's http_get and
        // http_post to, checked before each request is sent, at each redirect, and on each connection's address. The
        // default reaches nothing, and nothing a program asks of the library can widen it (the library's policy, not
        // a sandbox: a program's own external functions are not its business; docs/web.md). In a build without the
        // library it is not read: no program there can call http_get.
        //
        // Its server part (WebServerPolicy, `server`), in a build with the web server too (FMINUS_WEB_SERVER,
        // fminus/Config.h's FMINUS_HAS_WEB_SERVER; docs/research/web/server.md): where the program's http_listen may
        // listen, and how. Its default listens nowhere. In a build without the server it is not read.
        //
        // What a listen may be (each with a port of 0 to 65535; 0: the system picks one, which webPort() then says):
        //   "127.0.0.1:8080", "[::1]:8080"   that loopback address (any of 127.0.0.0/8)
        //   "localhost:8080"                 both loopback addresses, one port (::1 left out without IPv6)
        //   "192.168.1.20:8080"              one address of the machine: not loopback, so a token or open is needed
        //   "*:8080"                         every interface: likewise
        // A port browsers refuse (the Fetch standard's bad ports), the unspecified addresses themselves (write "*"),
        // multicast, broadcast, IPv4 inside IPv6 and IPv6 link-local addresses are refused.
        //
        // What reaches the program: the library answers everything else itself, before the program sees it (src/web/
        // Web.h has the nine rules): the request's head within limits; Host the address it arrived on (or localhost on
        // loopback, or one of `hosts`), so a rebound name is refused; Origin and Sec-Fetch-Site, when present, the
        // server's own; no form's content type; a path and a method some route of the program's names (404, 405
        // otherwise); the token, when there is one; a body within maxRequestBytes. No CORS header is ever sent.
        struct WebServerPolicy
        {
            std::string listen;                // where the program may listen (above); empty, the default: nowhere
            std::vector<std::string> hosts;    // names a request's Host may say besides the address ("studio.local"),
                                               // without a port
            // A secret every request to a route must carry, as "Authorization: Bearer <token>": 16 to 256 characters
            // of A-Z, a-z, 0-9, '-' and '_', from the system's random source. Static files are served without it.
            std::string token;
            bool open = false;                 // a listen that is not loopback with no token: the host means it
            struct File
            {
                std::string path;    // "/" or "/app.js": a path a route could name, without a query, each once
                std::string type;    // its Content-Type: printable ASCII with a '/', at most 255 bytes
                std::string bytes;   // its body
            };
            std::vector<File> files;               // static content the library serves itself, to GET and HEAD
            std::int32_t maxConnections = 16;      // open at once (1 to 1024)
            std::int32_t maxPending = 16;          // requests the program has not answered yet (1 to 1024)
            std::int32_t maxRequestBytes = 1 << 20;   // a request's body (0 to 2^30): past it, 413
            std::int32_t maxReplyBytes = 64 << 10;    // a reply's body (0 to 2^24; all slots' at most 2^28 bytes):
                                                      // past it, the reply is HTTP_LIMIT
            std::int32_t requestTimeoutMs = 10000;    // to send a whole request (408 past it), and to take a reply
            std::int32_t replyTimeoutMs = 2000;       // for the program to answer one, before the library answers 503
            std::int32_t idleTimeoutMs = 15000;       // a connection kept alive with nothing on it (each 1 to 86400000)
        };

        struct WebPolicy
        {
            // The URLs a program may reach, as patterns: "https://example.com/presets/*", "https://*.example.com",
            // "http://127.0.0.1:8080/*" (docs/web.md, "The pattern language": a scheme, a host or *.host, a port or *,
            // a path, exact or a prefix ending in *). Empty, the default: no network, every request HTTP_REFUSED.
            std::vector<std::string> allow;
            std::int32_t maxBodyBytes = 16 << 20;   // a response's body at most (and a POST's): past it, HTTP_LIMIT
            std::int32_t timeoutMs = 30000;         // a request, its redirects included: past it, HTTP_TIMEOUT
            std::int32_t maxRedirects = 5;          // redirects a request follows, each checked again (0: none)
            std::int32_t maxRequests = 16;          // requests the program has in flight at once
            std::string caFile;                     // PEM roots to trust instead of the system's; empty: the system's
            // Whether a pattern by name may reach a loopback, private or link-local address (a name that resolves to
            // one is refused otherwise; an address pattern reaches its own address whatever this says).
            bool privateAddresses = false;
            // Names the host resolves itself, "name:port:address", for the program's requests alone (libcurl's
            // CURLOPT_RESOLVE): split DNS, tests.
            std::vector<std::string> resolve;
            // Where the program's server may listen, and how (WebServerPolicy, above): nowhere by default.
            WebServerPolicy server;
        };

        struct Options
        {
            int optimisation = 2;                  // -O0 to -O3: LLVM's pipeline at that level, as fminus -O<n>
            unsigned polyLanes = 0;                // a poly float's lanes (1, 2, 4, ... 64); 0 is this machine's
            Math math = Math::Vector;              // how the math builtins compute (above), as fminus --math
            std::string fileName = "program.fm";   // what the error messages call the source
            // Where #include "name" finds its file (above). Without one, no #include finds a file: a host decides
            // where the files are, a directory of its own or files it keeps in memory, as fminus reads them from disk
            // (beside the including file, then -I's directories).
            IncludeResolver include;
            // Macros defined before the program's first line, as C's -D defines them (fminus -D NAME[=value]): each
            // name's value, "1" for a flag as -D NAME gives it, or "" for none. A program reads them in #if, #elif,
            // #ifdef and #ifndef, as it reads its own #define's: `#ifdef DEBUG` ... `#endif` around instrumentation
            // compiles it in when the host defines DEBUG, and skips its lines unread when it doesn't. FMinus never
            // substitutes a macro in code: its name there is an error. A name that is no identifier, `defined` or a
            // keyword fails compile.
            std::map<std::string, std::string> defines;
            // Each parameter's binding, by its qualified name ("Lowpass::cutoff"); one not named is a knob. A name
            // that is no parameter of the program, or a stream that is not a float, is a compile error.
            std::map<std::string, Binding> parameters;
            // C's strict-aliasing rule, which the optimiser relies on: the program reads and writes memory only as the
            // type it holds (a float vector as floats). Off, as clang's -fno-strict-aliasing, for a program that
            // reads memory as another type; its loops over a module's buffers may then run several times slower.
            bool strictAliasing = true;
            // Knob versioning, from -O1: a method whose loop branches on a bool knob, an int knob whose fminus::range
            // holds at most 8 integers, or whether a stream is flat (setStream) gets a copy of its code for each of
            // their values, picked when it starts, in which the value is a constant, folded as a frozen one is (an int
            // outside its range runs the method as written). At most this many copies a method; 0 turns it off. The
            // copies cost code size and compile time, and compute what the method does, bit for bit, in the Vector and
            // Libm math modes. Only Math::Fast can move a math function's last bits: a copy's loop may vectorise where
            // the method's did not, making its sinf, expf, ... the vector library's rather than libm's, and a knob's
            // constant that reaches one is folded with the compiling machine's own functions.
            unsigned knobVersions = 8;
            Denormals denormals = Denormals::Preserve;   // subnormals kept, or flushed to zero (above)
            // The most threads a parallel for or parallel { } runs on, the calling thread's among them: 0 is as many
            // as the pool has (one for each core), 1 runs every region on the calling thread alone, in order, and
            // starts no pool. A region runs in its caller's floating-point mode on every thread (Denormals::Flush's
            // flush-to-zero too).
            unsigned threads = 0;
            // Which of the program's LLVM IR to keep as text, for loweredIr() and optimisedIr() (KeepIr, above). None,
            // the default, prints nothing and holds nothing.
            KeepIr keepIr = KeepIr::None;
            // The loop budget, runaway-loop protection: the code counts its loops' iterations and its recursive calls
            // against a budget the host sets before each call (setLoopBudget, below), and a call that runs out returns
            // early, with a flag the host reads (loopBudgetExceeded). Off, the default, the code is as it was.
            bool loopBudget = false;
            // Bounds checks, for development builds: every access whose array the compiler can see is checked against
            // the array's extent, and a call whose check fails returns early, with the check recorded for the host
            // (boundsViolation, below). Off, the default, the code is as it was, and as fast.
            bool boundsCheck = false;
            // The network the program may reach through http_get and http_post, and where its http_listen may listen
            // (WebPolicy, above): none, and nowhere, by default.
            WebPolicy web;
        };

        // Compiles `source`. Returns nullptr when it does not compile, with `errors` holding FMinus's messages, one a
        // line, as clang words them (`program.fm:3:12: error: use of undeclared identifier 'x'`), notes included, and
        // any warnings among them where they are, as clang prints both. `errors` is empty on success, and a program
        // that compiles with warnings has them in warnings().
        static std::unique_ptr<Program> compile(std::string_view source, std::string& errors, const Options& options);
        static std::unique_ptr<Program> compile(std::string_view source, std::string& errors);

        ~Program();
        Program(const Program&) = delete;
        Program& operator=(const Program&) = delete;

        // The function `name`, as a pointer of type Signature*, which must be its declaration's C type (above). It is
        // compiled now, on this thread, if it has not been yet. nullptr, with `errors` set, when the program has no
        // such function.
        template <typename Signature>
        Signature* function(std::string_view name, std::string& errors)
        {
            static_assert(std::is_function_v<Signature>, "Signature is a function type, such as float(float)");
            return reinterpret_cast<Signature*>(address(name, errors));
        }

        // The function's address, or 0 (with `errors` set).
        std::uintptr_t address(std::string_view name, std::string& errors);

        // How many lanes a poly float has in this program: Options::polyLanes, or this machine's vector width in
        // floats (4 with NEON or SSE, 8 with AVX2).
        unsigned polyLanes() const;

        // The program's warnings, one a line as clang prints them, each naming its flag, notes included
        // (`program.fm:7:13: warning: enumeration value 'TRIANGLE' not handled in switch [-Wswitch]`); empty when it
        // has none. A warning is about a program that compiles, and runs as written: a host may show it beside the
        // program. FMinus's warnings are clang's -Wswitch, a switch without a default on a value of an enumerated
        // type that misses some of its constants, and -Wunknown-attributes, a field's or a parameter's attribute
        // without a namespace that C23 does not have, which FMinus ignores (Attribute, below).
        const std::string& warnings() const;

        // The program's LLVM IR as the code generator wrote it, as text, when Options::keepIr kept it (Lowered or
        // Both); empty otherwise.
        const std::string& loweredIr() const;

        // The program's LLVM IR as the JIT generates its machine code from it, optimised, as text, when Options::keepIr
        // kept it (Optimised or Both); empty otherwise.
        const std::string& optimisedIr() const;

        // Whether a declaration of function `name` is marked [[clang::nonblocking]]. FMinus has then checked that it
        // cannot reach malloc, calloc, free or an external function not marked nonblocking itself, so it may run on
        // the audio thread.
        bool nonBlocking(std::string_view name) const;

        // Whether a declaration of function `name` is marked [[clang::nonallocating]] or [[clang::nonblocking]]
        // (which includes it). FMinus has then checked that it cannot reach malloc, calloc or free.
        bool nonAllocating(std::string_view name) const;

        // An attribute of a field or a parameter, written after its name, C23's way:
        // `float level [[ui::unit("dB")]] [[fminus::range(-60, 0)]];` has two. FMinus's own have a meaning,
        // fminus::range (a parameter's, or a field's of numbers) and fminus::default (a parameter's), and a clang::
        // one, a function's, is refused on a field. Every other is the host's, passed on as written for the host to
        // decide what it means, a unit, a label or a display hint: FMinus checks only that its numbers are numbers
        // (docs/extending.md, recipe 35). One exception: a name without a namespace that is none of C23's standard
        // attributes (deprecated, maybe_unused and the rest), `[[range(0, 1)]]`, is ignored with clang's warning
        // (warnings()), and is in no list a host reads. Name a host's attributes in a namespace of its own.
        struct Attribute
        {
            // An argument: a number, or a string literal (C's, its escapes resolved, adjacent ones joined).
            struct Argument
            {
                bool isString = false;   // a string's; a number's otherwise
                double number = 0;       // a number's value (0 for a string)
                std::string text;        // a string's bytes (UTF-8 as the source has them); a number as written
                                         // ("-60", "2e4", "0x10")
            };

            std::string name;                  // as written, its namespace and all: "ui::unit", "fminus::range"
            std::vector<Argument> arguments;   // in order; none for `[[ui::hidden]]`
        };

        // The last of `attributes` named `name` ("ui::unit"), or nullptr: the one that counts when a field has two.
        static const Attribute* findAttribute(const std::vector<Attribute>& attributes, std::string_view name) noexcept;

        // A module's parameter, `param float cutoff [[fminus::range(20, 20000)]] [[fminus::default(1000)]];`. Its
        // slot in an instance is the C struct { const float* stream; T value; }, T a float, an int32_t or a bool (a
        // byte), whatever its binding: the stream's pointer at streamOffset, the value at valueOffset.
        struct Parameter
        {
            std::string module;                   // "Lowpass"
            std::string name;                     // "cutoff"
            std::string type;                     // "float", "int" or "bool"
            Binding binding;                      // as this program reads it
            std::size_t streamOffset = 0;         // in bytes, from the instance's address
            std::size_t valueOffset = 0;
            std::optional<double> minimum;        // fminus::range(minimum, maximum): what the host's control spans;
            std::optional<double> maximum;        // FMinus neither clamps to it nor checks it
            std::optional<double> defaultValue;   // fminus::default(value): what setDefaults sets
            std::vector<Attribute> attributes = {};   // all of them, in order, these two too: `[[ui::unit("Hz")]]`
                                                      // (but none FMinus ignored: Attribute)
        };

        // A module of the program (FMinus's class), as a host sees it. An instance is `size` bytes, aligned to
        // `alignment` and zeroed, since every field starts at 0: C's aligned_alloc and std::memset (an alignment of at
        // least alignof(std::max_align_t), and the size rounded up to a multiple of it, suit every system's), or, on
        // Windows, whose C runtime has no aligned_alloc, _aligned_malloc, which only _aligned_free releases; or a
        // value-initialised C++ struct with the same fields. A method then runs on its address (above). Nothing else
        // is needed to make one: FMinus runs no constructor. A module with parameters wants them set too (setDefaults).
        struct Module
        {
            struct Field
            {
                std::string name;
                std::string type;          // as FMinus writes it: "float", "double", "float4", "float[16]",
                                           // "double[8]", "float[4][16]", "Osc", "Osc *"; a parameter's "param float"
                                           // (its slot). A "double" is 8 bytes, C++'s double, aligned to 8: a host
                                           // that reads fields by type reads one as a double, not as a float
                                           // (docs/extending.md, recipe 33)
                std::size_t offset = 0;    // in bytes, from the instance's address
                std::size_t size = 0;
                std::string element = {};   // an array's elements' type ("float", "Osc"); empty for no array
                // An array's shape: how many elements each dimension holds, outermost first: {16} for "float[16]",
                // {4, 16} for "float[4][16]", whose 4 rows of 16 lie side by side, size / 4 bytes each (C's layout:
                // element [i][j] is at offset + (i * 16 + j) * sizeof(float)). Empty for a field that is no array
                // (docs/extending.md, recipe 34).
                std::vector<std::size_t> extents = {};
                // Its attributes, in order: `float level [[ui::unit("dB")]];`'s, a parameter's (its Parameter has them
                // too), but none FMinus ignored (Attribute). Empty for a field that has none.
                std::vector<Attribute> attributes = {};
            };

            std::string name;
            std::size_t size = 0;
            std::size_t alignment = 1;
            std::vector<Field> fields;              // in declaration order
            std::vector<std::string> methods;       // as function() takes them: "OnePole::tick"
            std::vector<Parameter> parameters;      // the fields that are parameters, in declaration order
        };

        // The module `name`, or nullptr when the program has none.
        const Module* module(std::string_view name) const;

        // Every module of the program, in declaration order.
        const std::vector<Module>& modules() const;

        // The parameter `name`, qualified ("Lowpass::cutoff"), or nullptr when the program has none.
        const Parameter* parameter(std::string_view name) const;

        // Sets a parameter's value in `instance`, an instance of its module: a relaxed atomic store, from any thread
        // at any time, which a method reads when it next starts. A float's rounded, an int's truncated toward 0 (and
        // clamped to an int's range), a bool's true unless 0. A frozen parameter's methods do not read it, and a
        // stream's read it only in a call whose stream is null (setStream). No allocation, no lock: the audio thread
        // may call it.
        static void setValue(void* instance, const Parameter& parameter, double value) noexcept;

        // The value setValue last set (a relaxed atomic load), as a double.
        static double value(const void* instance, const Parameter& parameter) noexcept;

        // Points a stream parameter at its samples for the next call: as many as the call reads, which a method that
        // processes n samples, reading cutoff[i] for i below n, is n. The thread that calls the method sets it,
        // before the call; the method reads it when it starts. nullptr makes the call flat: every cutoff[i] is then
        // the slot's value, as setValue last set it, and no sample is read. A knob's or a frozen parameter's methods
        // do not read it.
        static void setStream(void* instance, const Parameter& parameter, const float* samples) noexcept;

        // Sets each of the module's parameters that has an fminus::default to it (setValue), in `instance`.
        static void setDefaults(void* instance, const Module& module) noexcept;

        // The loop budget (Options::loopBudget): runaway-loop protection, for a program that might loop for ever, or
        // for far longer than a block may take, on the audio thread. Its code counts down the budget: a loop whose
        // count the optimiser knows when it starts (a for over a block, a vectorised loop) is charged that count there,
        // before it runs; any other loop (a while on the data) one at each branch back to its start; and a function
        // that can call itself again one at each call. When the count would go below zero, the function sets the flag
        // and returns at once, 0, null or nothing, and so does each of the program's functions that called it, back to
        // the host: no caller goes on with a value that was never computed. Before then, a call computes what it would
        // without the budget, bit for bit: the checks go in after the optimiser has made every decision, and only count
        // and branch. A loop with a count costs a subtraction and a branch where it starts, any other a subtraction and
        // a branch an iteration, in registers, and a call a load and a store of the budget (an atomic min, below, in
        // what a parallel region's threads run; docs/extending.md, recipe 29, has the measurements). What a call takes
        // depends on the optimised code: a vectorised loop counts once a vector of samples, and a loop the optimiser
        // unrolled or removed counts what it became.
        //
        // A call that ran out leaves the instance and what it wrote of the output as the optimised code had them when
        // it stopped, which need not be any point of the source: the optimiser keeps a module's field in a register
        // through a loop and stores it after the loop, sinks and merges stores, so some of the call's writes may be
        // missing, out of order with the source, or out of step with each other (a write index advanced past its wrap,
        // say). A host that runs the program again first re-initialises the instance as it made it (zeroes it, then
        // setDefaults() and its own parameter values and streams), or discards it.
        //
        //   Program::Options options;
        //   options.loopBudget = true;
        //   auto program = Program::compile(source, errors, options);
        //   ...
        //   program->setLoopBudget(10'000'000);           // on the audio thread, before each call
        //   process(instance, in, out, n);
        //   if (program->loopBudgetExceeded())            // it returned early: silence it, report it, and
        //       ...                                       // re-initialise the instance before the next call
        //
        // One budget a Program, which every thread that runs it shares. A parallel region's threads each count from
        // where their part starts, and write back what they have left as a minimum, so that what is left never goes up
        // during a call: each thread takes at most the budget, the region at most its thread count times it, and a
        // part that starts after one ran out stops at its first check. What is left after a region is the least any
        // part left (the flag is what says one ran out). A loop that runs for ever only through undefined behaviour (an
        // int that overflows past its end, which C lets the optimiser assume never happens) is charged the count the
        // optimiser assumed. The program compiles with its machine code, as compile() returns (the first look-up of a
        // function only finds it). Unlimited until the host sets it. Each of these is a relaxed atomic load or store,
        // no allocation and no lock: the audio thread may call them. In a program compiled without the budget they
        // read and write a budget nothing counts down, and loopBudgetExceeded() is false.
        //
        // Sets the budget for the next call, `backEdges` branches back (a loop with a count is charged them all as it
        // starts; at 0 the first branch back stops the call; a negative budget is -1, which stops the call at its
        // first check), and clears the flag. The host sets it before each call it limits; otherwise what is left
        // carries over to the next call.
        void setLoopBudget(std::int64_t backEdges) noexcept;

        // What is left of the budget: what was set less what the calls since have taken; -1 once a call has run out
        // (never less, so that however many calls follow without setLoopBudget, none of the program's counts wraps).
        std::int64_t loopBudgetLeft() const noexcept;

        // Whether a call since setLoopBudget ran out of the budget and returned early.
        bool loopBudgetExceeded() const noexcept;

        // Bounds checks (Options::boundsCheck), for development builds: an index outside its array reads or writes
        // whatever lies beside it (another field, the host's memory), which corrupts the host or crashes it, perhaps
        // much later. Compiled with the checks, every access whose array the compiler can see is checked against the
        // array's extent: a[i], m.taps[i], p->taps[i] and voices[v] (an array of modules), *(a + i), a pointer formed
        // from one (a + i, &a[i], which may point one past its end, as C allows), and a poly_load or poly_store through
        // one (each lane it reaches, or with a mask each lane the mask takes): of a local array, a module's field, a
        // constant table. A pointer the program keeps has no extent the compiler knows, and its accesses are not
        // checked: one a variable, a field or a parameter holds (`float* p = buf;`, then p[i]), what a function
        // returns, a heap block (malloc's), a host's buffer. Nor are fft_forward's and fft_inverse's buffers: the
        // transform reads and writes as many elements as its plan has, which only the running program knows, so a
        // buffer shorter than the plan is overrun as it would be without the checks.
        //
        // When a check fails, the function records it, unless the record holds one already (the first is kept), and
        // returns at once, 0, null or nothing, and so does each of the program's functions that called it, back to the
        // host, as with the loop budget: no caller goes on with a value that was never computed, and the access never
        // happens. What the call leaves in the instance and the output is whichever of the optimised code's stores had
        // run, which need not be any point of the source (the loop budget's paragraph, above): a host that runs the
        // program again first clears the record (clearBoundsViolation, below), then re-initialises the instance, or
        // discards it. In that order: a method such as init is a call too, which a record still holding the check
        // would stop partway.
        //
        //   Program::Options options;
        //   options.boundsCheck = true;                   // while developing; the build that plays has none
        //   auto program = Program::compile(source, errors, options);
        //   ...
        //   process(instance, in, out, n);
        //   if (auto failed = program->boundsViolation()) // it returned early: silence it, and off the audio thread
        //   {
        //       report(failed->message());                // "synth.fm:12:9: index 16 out of bounds for 'line' of
        //                                                 // type 'float[16]'"
        //       program->clearBoundsViolation();          // first: then init runs whole
        //       init(instance);
        //   }
        //
        // A check costs a compare and a branch where the optimiser cannot prove the access within the array (a ring
        // buffer's index; a loop over the array that its own condition keeps within it has none), and a load and a
        // branch after each call of a function with checks that inlining leaves. The optimiser treats a failed check as
        // a trap until the last pass makes it a return, so a loop with one keeps a field it updates in memory, a store
        // each iteration (docs/extending.md, recipe 30, has the measurements), and a build with the checks computes
        // what it would without them until a check fails. A build without them is the code it was. The program
        // compiles with its machine code, as compile() returns (the first look-up of a function only finds it).
        //
        // One record a Program, which every thread that runs it shares. While it holds a check, each function of the
        // program returns early as soon as a call it makes of a function with checks (or one that calls such a
        // function) returns, however it got there: the callee runs its own body, unless its own calls stop it, and
        // the caller returns 0, null or nothing after it, though its own indexes are fine. Calls the optimiser
        // inlined are no calls, so a function whose code is all inlined runs to its end. In a parallel region, the
        // first part to fail is recorded; each of the others stops at a failed check of its own or that way, or else
        // runs to its end, and the function that ran the region returns when the region ends. So clear the record
        // before the program runs again, re-initialising the instance included. Each of these is a relaxed atomic
        // load or store, no allocation and no lock: the audio thread may call them. In a program compiled without the
        // checks there is never a violation.
        //
        // A bounds check that failed: where in the source the access is, which array, and the element it reached.
        // The strings are the Program's, valid while it lives.
        struct BoundsViolation
        {
            std::string_view file;     // the source file, as errors name it (Options::fileName, or an #include's)
            int line = 0;              // the access's line in that file, from 1
            int column = 0;            // and its column, from 1, in bytes, as the errors count it
            std::string_view array;    // the array as the source names it ("buf", "m.taps", "voices[v].line"), or
                                       // empty when it has no name
            std::string_view type;     // its type, as FMinus writes it: "float[16]"
            std::int64_t extent = 0;   // its elements
            std::int64_t index = 0;    // the element the access reached, from the array's first
            // How many elements from `index` the access reaches: 1 for an element (a[i], *p, p->y), a poly_load's or
            // poly_store's lanes, and 0 for a pointer formed (a + i, &a[i]), which may also be one past the end.
            int width = 1;

            // What it says, in UBSan's words with the array's name and where it is: "synth.fm:12:9: index 16 out of
            // bounds for 'line' of type 'float[16]'", "pointer offset 17 ...", "4 elements from index 14 ...". It
            // allocates: off the audio thread.
            std::string message() const;
        };

        // The first check that failed since clearBoundsViolation (or since the program compiled), or nothing.
        std::optional<BoundsViolation> boundsViolation() const noexcept;

        // Clears the record, so that the next call runs whole: before any call of the program, init's included.
        void clearBoundsViolation() noexcept;

        // The web (a build with FMINUS_WEB; docs/web.md). A program reaches the network only through http_get and
        // http_post, whose URLs are string literals, so the host knows every one before it runs anything: it may show
        // them, ask its user, or refuse to run a program that names a host it does not allow. Each request is held to
        // Options::web when it is made (its %d filled in), at each redirect, and on each connection's address.
        //
        //   options.web.allow = {"https://example.com/presets/*"};
        //   auto program = Program::compile(source, errors, options);
        //   for (const Program::WebRequest& request : program->webRequests())
        //       show(request.method, request.pattern, request.line);       // "GET https://example.com/presets/*.json"
        //
        // The program's requests are its context's, which the Program holds: one I/O thread for the process drives
        // every request, started by the first and joined when the last context goes. Destroying the Program stops its
        // context (below) and frees every request it made, closed or not; so does a program that never closes what
        // it opens leak nothing past the Program.
        //
        // One call of http_get or http_post in the program's source: its method ("GET" or "POST"), its URL as a
        // pattern (each %d a "*", each %% a "%"), and where the call is, in every call site's order of lowering.
        struct WebRequest
        {
            std::string method;
            std::string pattern;
            std::string file;   // as errors name it (Options::fileName, or an #include's)
            int line = 0;
            int column = 0;
        };

        // Every call of http_get and http_post the program makes, or none (and none in a build without the web
        // library, where a program cannot make one).
        const std::vector<WebRequest>& webRequests() const;

        // The web server (a build with FMINUS_WEB_SERVER). A program listens with http_listen(), which takes no
        // argument: where is Options::web.server's, never the program's. It takes the requests that reach it with
        // http_next (wait-free: on the audio thread, between blocks) or http_accept (which blocks), tells them apart
        // with route literals, http_is(r, "PUT /osc/%d/gain"), and answers each once. Every route is a literal, so the
        // host knows them all before anything runs, and the library answers 404 and 405 itself for a path or a method
        // no route names: only a request the program declared reaches its code.
        //
        //   options.web.server.listen = "localhost:0";
        //   auto program = Program::compile(source, errors, options);
        //   for (const Program::WebRoute& route : program->webRoutes())
        //       show(route.method, route.pattern, route.line);              // "PUT /osc/*/gain"
        //   program->function<void()>("init", errors)();                    // which calls http_listen()
        //   open("http://localhost:" + std::to_string(program->webPort()) + "/");
        //
        // One call of http_is in the program's source: its route's method ("GET", "POST", "PUT", "PATCH" or "DELETE";
        // a GET route answers HEAD too), its path as a pattern (each %d a "*"), and where the call is, in every call
        // site's order of lowering.
        struct WebRoute
        {
            std::string method;
            std::string pattern;
            std::string file;   // as errors name it (Options::fileName, or an #include's)
            int line = 0;
            int column = 0;
        };

        // Every call of http_is the program makes, or none (and none in a build without the web server).
        const std::vector<WebRoute>& webRoutes() const;

        // The port the program's server listens on (1 to 65535) once the program has called http_listen; 0 before,
        // when its policy has no listen or the address could not be bound, once stopWeb() has closed it, and in a
        // build without the web server. Takes a lock: not for the audio thread.
        int webPort() const noexcept;

        // Stops the program's network for good: every request in flight ends as HTTP_CANCELLED before this returns,
        // waking every thread in http_wait, and every request the program starts from then on ends so at once. What
        // completed stays readable until it is closed or the Program destroyed. A host that shuts a program down while
        // a thread of its own is in the program (a loader in http_wait) calls this, joins that thread, then destroys
        // the Program: destroying a Program while a thread still runs its code is undefined, as ever. Any thread may
        // call it, any number of times; it waits for the I/O thread (briefly): not on the audio thread. The
        // destructor calls it. It closes the program's server too: its sockets and every connection, every request
        // the program has not answered given up (its reply HTTP_CANCELLED), every thread in http_accept woken (null),
        // and http_port HTTP_CANCELLED from then on; an http_listen after it never listens.
        void stopWeb() noexcept;

        // Calling a function by name, for a host that learns a function's signature as it runs rather than when it is
        // built (function<Signature>, above, is for one it knows): a console that calls the function its user names, a
        // test runner, a tool that tries one of a program's functions on values it chooses. callable() describes the
        // function, its parameters and its result, as values; call() checks the host's arguments against them, calls
        // the function with them on the calling thread, and returns its result. What goes wrong is a value too, a
        // CallResult with a status and a message: a name that is no function, a type call() does not pass, an argument
        // of the wrong type, the wrong number of them, never undefined behaviour.
        //
        //   using Value = fminus::Program::Value;
        //   std::string errors;
        //   const Program::Callable* softclip = program->callable("softclip", errors);   // nullptr: errors says why
        //   Program::CallResult result = program->call(*softclip, {Value::of(0.5f), Value::of(2.0f)});
        //   if (result.ok())
        //       show(result.value.number);              // the float it returned, as a double
        //   else                                        // with Value::of(2): "argument 2 of 'softclip' is 'int', not
        //       show(result.message());                 // 'float' (its parameter 'drive')"
        //
        // What call() passes: C's scalars, bool, int, float and double (an enumerated type's as its int or bool), and
        // pointers to int, float, double or a module, or void*, which point to the host's memory (Value::array,
        // Value::instance); it returns a scalar or nothing. A function that takes or returns anything else (a vector,
        // a poly float, a pointer to a vector, a pointer as its result) has no Callable: callable() says which of its
        // types call() does not pass. Each argument's type must be its parameter's, as call() converts nothing (a host
        // whose numbers are all doubles converts each with Value::scalar). A pointer must also not drop a const (a
        // const float* to a float*), must be aligned to its elements' type, and a module's instance must be one of
        // this Program's Module (below). A pointer's extent is the host's to know: C passes none, and FMinus checks no
        // access through one, bounds checks or not (a parameter written `float x[16]` is a float*, as C has it). A
        // function that is passed a buffer is passed its length too, as C's are, and must not read past it.
        //
        // The instance: a method runs on one, its first parameter, `this`, which is Value::instance(memory, module), an
        // instance the host made as Module says (zeroed, aligned, its parameters set), never null. What the method
        // changes stays in it for the next call, by name or through a pointer. A function that is no method runs on its
        // arguments and what they point to, as FMinus has no variables outside functions but its constants. The module
        // is the host's word for how the memory is laid out, which call() cannot see, so it must be this Program's own
        // (module() gives it): another Program's, of the same name, is refused even when it was compiled from the same
        // source. A host that keeps an instance across a recompile, knowing its layout is the same, passes it with the
        // new Program's Module.
        //
        // Threads: callable() compiles, on the calling thread, the code that calls the function, the first time it is
        // asked for it: a thunk of a few instructions, which loads each argument as the function's parameter type and
        // calls it as C does, through the target's calling convention. That takes a millisecond or so, allocates and
        // takes the Program's lock (and compiles the program's code, if no function was looked up yet), so it runs off
        // the audio thread. Its Callable is the Program's, the same one each time, valid while the Program lives: a
        // host keeps the pointer, as it keeps a function's, and asks a recompiled Program for its own. A Callable
        // cannot be copied, so the one call() is given is the one callable() described. call() through a Callable runs
        // the function on the calling thread, any thread, on several at once (each with an instance of its own: a
        // method's changes are as atomic as its C++ call's), and adds no lock and no allocation to what the function
        // does, unless the host's call is wrong (Mismatch: its message allocates) or the function takes more than 16
        // arguments. A call the loop budget or a bounds check stops allocates nothing: its error is empty, and
        // CallResult::message() words it, off the audio thread. call(name, ...) looks the Callable up first, as
        // callable() does.
        //
        // The safety options apply as they do to a call through a pointer: in a program compiled with the loop budget
        // the call counts against it, and one that runs out returns early, Status::OutOfBudget, with
        // loopBudgetExceeded() set; with bounds checks, a check that fails stops the call, Status::OutOfBounds, with
        // boundsViolation() set. Each status is the Program's state after the call, so set the budget (setLoopBudget)
        // and clear the record (clearBoundsViolation) before each call, and re-initialise an instance a stopped call
        // ran on (the loop budget's paragraph, above). The budget and the record are the Program's, one of each, which
        // every thread's calls share: a status is the call's own only when no other call of the Program overlaps it.
        // With several threads calling at once, one call's failure is every overlapping call's status (and a call that
        // returns after another's failed check returns early too, as the bounds checks' paragraph says), and one
        // thread's setLoopBudget or clearBoundsViolation is the others' too. A host that calls a program compiled with
        // either option from several threads serialises those calls, or reads the statuses as the program's.

        // A value call() passes to a function, or one it returns: a scalar, or a pointer to the host's memory.
        struct Value
        {
            enum class Kind : std::uint8_t
            {
                Void,   // none: a void function's result, or a call that did not run
                Bool,
                Int,
                Float,
                Double,
                Pointer,
            };

            Kind kind = Kind::Void;
            double number = 0;          // a Bool's (0 or 1), an Int's, a Float's or a Double's value: a double holds
                                        // each exactly
            void* pointer = nullptr;    // a Pointer's: the host's first element, or its instance
            Kind element = Kind::Void;  // a Pointer's elements: Int, Float or Double, or Void for a module's instances
            const Module* module = nullptr;   // a Pointer to a module's instances: the module
            bool readOnly = false;      // a Pointer through which the function may only read: a const float* takes it,
                                        // a float* does not

            static Value of(bool value) noexcept
            {
                return scalarOf(Kind::Bool, value ? 1.0 : 0.0);
            }

            static Value of(int value) noexcept
            {
                return scalarOf(Kind::Int, static_cast<double>(value));
            }

            static Value of(float value) noexcept
            {
                return scalarOf(Kind::Float, static_cast<double>(value));
            }

            static Value of(double value) noexcept
            {
                return scalarOf(Kind::Double, value);
            }

            // Not a pointer, which would convert to a bool: Value::array or Value::instance says what it points to.
            template <typename T>
            static Value of(T* value) = delete;

            // A scalar of `kind` (Bool, Int, Float or Double) from a number, converted as setValue converts a
            // parameter's value: a float's rounded, an int's truncated toward 0 (and clamped to an int's range, NaN 0),
            // a bool's true unless 0. Any other kind gives a Void value, which no parameter takes.
            static Value scalar(Kind kind, double number) noexcept;

            // The host's elements, as a function's `int*`, `float*` or `double*` takes them (or its `const` one, or
            // `void*`), from the first; a const one only as its `const float*` or `const void*` does.
            static Value array(int* elements) noexcept
            {
                return pointerTo(Kind::Int, elements, false);
            }

            static Value array(const int* elements) noexcept
            {
                return pointerTo(Kind::Int, const_cast<int*>(elements), true);
            }

            static Value array(float* elements) noexcept
            {
                return pointerTo(Kind::Float, elements, false);
            }

            static Value array(const float* elements) noexcept
            {
                return pointerTo(Kind::Float, const_cast<float*>(elements), true);
            }

            static Value array(double* elements) noexcept
            {
                return pointerTo(Kind::Double, elements, false);
            }

            static Value array(const double* elements) noexcept
            {
                return pointerTo(Kind::Double, const_cast<double*>(elements), true);
            }

            // An instance of `module` the host made (Module, above), or the first of an array of them: a method's
            // `this`, or a function's `Osc*` (or `const Osc*`, or `void*`).
            static Value instance(void* memory, const Module& module) noexcept
            {
                Value value = pointerTo(Kind::Void, memory, false);
                value.module = &module;
                return value;
            }

        private:
            static Value scalarOf(Kind kind, double number) noexcept
            {
                Value value;
                value.kind = kind;
                value.number = number;
                return value;
            }

            static Value pointerTo(Kind element, void* pointer, bool readOnly) noexcept
            {
                Value value;
                value.kind = Kind::Pointer;
                value.pointer = pointer;
                value.element = element;
                value.readOnly = readOnly;
                return value;
            }
        };

        // A function as call() calls it: its name and its signature, as values. The Program's own, which callable()
        // gives as a pointer: it cannot be copied or moved, so the Callable call() is given is the one callable()
        // described, and what call() checks is the function's signature. A host copies the fields it shows.
        struct Callable
        {
            // One of its parameters, which call() checks an argument against (not a module's Parameter, above).
            struct Param
            {
                std::string name;   // as declared: "x"; "this" for a method's instance; empty for one with no name
                std::string type;   // as FMinus writes it: "float", "const float *", "Osc *", "void *"
                Value::Kind kind = Value::Kind::Float;     // Bool, Int, Float, Double or Pointer
                Value::Kind element = Value::Kind::Void;   // a Pointer's elements: Int, Float or Double; Void for a
                                                           // module's instances, or for void*, which takes any
                const Module* module = nullptr;            // a Pointer to a module's instances: the module
                bool readOnly = false;                     // a Pointer to const: it takes a const Value::array too
            };

            std::string name;                         // as callable() takes it: "softclip", "Osc::tick"
            const Module* module = nullptr;           // a method's module: its `this` is parameters[0]
            Value::Kind result = Value::Kind::Void;   // Void, Bool, Int, Float or Double
            std::string resultType;                   // as FMinus writes it: "float", "void"
            std::vector<Param> parameters;            // in order, a method's `this` first

            Callable() = default;   // none of a Program's, which call() refuses
            Callable(const Callable&) = delete;
            Callable& operator=(const Callable&) = delete;
            Callable(Callable&&) = delete;
            Callable& operator=(Callable&&) = delete;
            ~Callable() = default;

        private:
            friend class Program;
            const void* owner_ = nullptr;   // the Program's
            std::uintptr_t thunk_ = 0;      // the code that calls it (src/jit/Call.h)
        };

        // What a call by name did: whether it ran, and its result, or why not.
        struct CallResult
        {
            enum class Status : std::uint8_t
            {
                Ok,            // the function ran and returned: `value` is its result
                NoFunction,    // the program defines no function of the name (it may declare one: a prototype alone)
                Unsupported,   // the function takes or returns a type call() does not pass (above)
                LinkError,     // the function is the program's, but its code did not link (a function it calls is
                               // declared and defined nowhere), or the code that calls it did not compile: LLVM's
                               // words, "'g' cannot be called: Failed to materialize symbols: ..."
                Mismatch,      // the arguments are not the function's: their number, a type, a null instance, a pointer
                               // not aligned, an instance of another Program's module, a Callable of another Program's
                OutOfBudget,   // it ran out of the loop budget and returned early (loopBudgetExceeded)
                OutOfBounds,   // a bounds check failed, and it returned early (boundsViolation)
            };

            Status status = Status::Ok;
            // The function's result, of its result type (Void for a void function); after OutOfBudget or OutOfBounds,
            // what it returned early, 0, false or nothing; Void when it did not run.
            Value value;
            // Why it did not run, one line, as FMinus words its errors: "'softclip' takes 2 arguments, not 3", "the
            // program has no function 'softclp'". Empty when it is Ok, and after OutOfBudget or OutOfBounds, which a
            // call on the audio thread meets, and so allocate nothing: message() words them.
            std::string error;
            // After OutOfBounds, the check that failed, as boundsViolation() had it when the call returned (its strings
            // are the Program's, valid while it lives); nothing otherwise.
            std::optional<BoundsViolation> violation;

            bool ok() const noexcept
            {
                return status == Status::Ok;
            }

            // What went wrong, one line: `error`, or what stopped the call, "'spin' ran past its loop budget and
            // returned early", "'tap' stopped: taps.fm:3:16: index 20 out of bounds for 'taps' of type 'float[16]'";
            // empty when it is Ok. It allocates, so a host on the audio thread keeps the CallResult and words it
            // elsewhere, while the Program lives.
            std::string message() const;

        private:
            friend class Program;
            const Callable* function_ = nullptr;   // the Callable that ran, for message()
        };

        // The function `name` (a method as "Osc::tick") as call() calls it, its thunk compiled now, on this thread, if
        // it has not been yet; nullptr, with `errors` set, when the program defines no such function, call() cannot
        // pass one of its types, or its code does not link (CallResult::Status, above). A function the program only
        // declares (`float atanf(float x);`, the C library's) is none of its own, and has no Callable.
        const Callable* callable(std::string_view name, std::string& errors);

        // Calls `function`, one of this Program's Callables, with `count` arguments, checked against its parameters.
        CallResult call(const Callable& function, const Value* arguments, std::size_t count);
        CallResult call(const Callable& function, const std::vector<Value>& arguments);

        // Looks the function up (callable(), which compiles its thunk the first time), then calls it.
        CallResult call(std::string_view name, const std::vector<Value>& arguments);

        // Profiling: where the program's machine code is, for a sampling profiler in the host, which interrupts a
        // thread now and then (a timer's signal, or the thread suspended and its registers read), takes its program
        // counter, and counts the samples that fall in each function's code: a profile of the program as it runs, per
        // function, per method and per module, with nothing added to its code.
        //
        //   std::vector<Program::CodeRange> ranges = program->codeRanges(errors);   // once, off the audio thread
        //   ...
        //   if (const Program::CodeRange* range = Program::codeRangeAt(ranges, pc))   // each sample, on the
        //       ++samples[range->function];                                         // sampler's thread: "Osc::tick"
        //
        // A range is the machine code's, as the optimiser left it, not the source's. A function the optimiser inlined
        // into its caller (a method that a process loop calls, from -O1) runs inside the caller's code, so its samples
        // are the caller's, whose range lists it in `inlined`; its own range, which the program keeps for the host's
        // calls, takes samples only when something calls it. At -O0 nothing is inlined, and each function's samples are
        // its own. A parallel region's body is a range of its own (Kind::Parallel), run by the pool's threads and the
        // caller's: a sampler of the calling thread alone sees only its share. Code that is not the program's has no
        // range: the runtime's (the pool, an FFT, fminus_rt_sinf where a math kernel is called rather than inlined),
        // the C library's (Math::Libm's sinf, a prototype's function) and the host's own, which the host names from
        // its own symbols (dladdr), or charges to the program's function that called it by walking the stack.
        //
        // One function's machine code: the bytes [address, address + size).
        struct CodeRange
        {
            // What the code is.
            enum class Kind : std::uint8_t
            {
                Function,     // a function's code, as the program defines it: "Osc::tick"; under Denormals::Flush, an
                              // entry point's body, "process.body"
                EntryPoint,   // under Denormals::Flush, an entry point's few instructions that set the floating-point
                              // mode around its body and hand the caller's back: "process"
                Parallel,     // a parallel for's or a parallel { }'s body: "run.parallel.1"
                Thunk,        // the code call() calls the function through: "fminus.call.softclip"
                Support,      // FMinus's own, no function's: a math kernel's path for large arguments,
                              // "fminus.large.sin"
            };

            std::uintptr_t address = 0;   // its first byte, in this process
            std::size_t size = 0;         // its bytes from there; on macOS and Windows, whose objects give no
                                          // sizes, to the next range's start, its padding included
            Kind kind = Kind::Function;
            std::string symbol;           // the code's name in the program's object: "Osc::tick", "run.parallel.1"
            std::string function;         // the program's function it is the code of, as function() names it:
                                          // "Osc::tick", "run" for "run.parallel.1"; empty for Support
            std::string module;           // the function's module, for a method: "Osc"; empty otherwise
            // The program's functions the optimiser inlined into this code, whose samples are this range's: "Osc::tick"
            // in "Osc::process". Read from the calls, counted, before the optimiser and after: a function is inlined
            // when this code calls it fewer times after than it did before, with the calls each function inlined into
            // it brought, even if it still calls it at another call. A call the optimiser deleted as dead, or merged
            // with another, looks the same as one it inlined. Sorted; empty when there are none.
            std::vector<std::string> inlined = {};
        };

        // Every range of the program's machine code, sorted by address, none overlapping another: each function the
        // program defines, each parallel region, FMinus's own, and the thunk of each Callable made so far. The
        // program's code is linked now, on this thread, if no function has been looked up yet (as address() links
        // it), so the list after compile() is the program's whole. Empty, with `errors` set, when its code does not
        // link (CallResult's LinkError). The code stays where the list says while the Program lives (the JIT neither
        // moves nor frees it), so a host takes the list once, after compile() and its callable()s, and a sampler reads
        // it from any thread without a lock. It is the host's copy, made on each call: it allocates, off the audio
        // thread.
        std::vector<CodeRange> codeRanges(std::string& errors);

        // The range of `ranges` (codeRanges(), sorted by address) that holds `address`, a sampled program counter, or
        // nullptr when none does: a binary search, with no allocation and no lock, which a sampler's thread calls for
        // each sample.
        static const CodeRange* codeRangeAt(const std::vector<CodeRange>& ranges, std::uintptr_t address) noexcept;

    private:
        struct Impl;
        explicit Program(std::unique_ptr<Impl> impl);

        std::unique_ptr<Impl> impl_;
    };

    // sinf, cosf, tanf, expf, tanhf and logf as a program compiled in the default math mode (Program::Math::Vector)
    // computes them, wherever it does: a C++ reference that calls these gets a program's results bit for bit, on any
    // machine. They are FMinus's own kernels, the same IEEE operations on every target: `sin`, `cos` and `tanh` within
    // 1.56 ulp of the exact result, `tan` within 3.35, `exp` within 1.05 and `log` within 0.99 (libm's are within 1;
    // docs/extending.md, recipe 23). None allocates or locks: the audio thread may call them. Each depends on its
    // argument alone and does nothing else, which FMINUS_MATH_CONST tells GCC and clang (their const attribute), so
    // that two calls of one argument are one, as they are of std::sin.
#if defined(__GNUC__) || defined(__clang__)
#define FMINUS_MATH_CONST [[gnu::const]]
#else
#define FMINUS_MATH_CONST
#endif
    namespace math
    {
        FMINUS_MATH_CONST float sin(float x) noexcept;
        FMINUS_MATH_CONST float cos(float x) noexcept;
        FMINUS_MATH_CONST float tan(float x) noexcept;
        FMINUS_MATH_CONST float exp(float x) noexcept;
        FMINUS_MATH_CONST float tanh(float x) noexcept;
        FMINUS_MATH_CONST float log(float x) noexcept;
    } // namespace math
} // namespace fminus
