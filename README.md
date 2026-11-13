# FMinus

A JIT compiler for audio DSP. FMinus source is lexed by Flex and parsed by Bison, then lowered to LLVM IR. LLVM
optimises it for the CPU it is running on, and ORC's LLJIT compiles it, all on the user's machine. For development,
the same IR can also be compiled ahead of time into a native executable.

**Status:** FMinus v0, a deliberately small subset of C that goes all the way through, from source to the JIT, to expand
on. It has functions over `float`, `double`, `int` and `bool`, and `void` ones; declarations, assignments, blocks, `if`,
`while`, `for`, `switch` with its `case` and `default` labels, `break`, `continue` and `return`; C's enumerations, named
`int` constants, and a C file's const objects at the top level, tables and scalars that the functions after them share;
arithmetic with `%`, `++` and `--`, C's bitwise operators on `int`, casts, comparisons, `!`, `&&`, `||` and `?:`; calls
and `<math.h>`'s float functions (below) and its exact double ones, and `hash` and `randf`, white noise from a counter,
the same bits wherever they are computed. It has pointers, `const` and `restrict` ones, so a function can take a C++
host's buffers and process a whole block in one call, C's arrays, and C's heap (`malloc`, `calloc`, `free`), with
clang's `[[clang::nonblocking]]` checked so that the code on the audio thread can't reach it. It has modules, its
classes: fields and methods, as C++'s structs with member functions, which hold each other and arrays of each other and
live wherever their host puts them, and parameters, the values a plugin's host sets, which a method reads a sample at a
time and the host compiles as knobs, as streams or as constants. It has an FFT, real and complex, whose plans are made
off the audio thread and whose transforms run on it (PFFFT, compiled into FMinus's runtime library), and a JSON parser
and an XML parser (yyjson and pugixml, compiled in too): a program parses a document its host hands it, a preset or a
wavetable pack, off the audio thread, and reads its numbers by path, each correctly rounded, on it or off it. Built
with `FMINUS_WEB` (off by default), it has an HTTP(S) client too, on libcurl: a program fetches a preset or a table from
a URL its host allows, and the client reaches no other, every URL a literal the host can list before anything runs
([docs/web.md](docs/web.md)); and with `FMINUS_WEB_SERVER` too (also off by default), a server: a program listens where
its host says, nowhere by default, and its audio thread takes requests and answers them between blocks, wait-free, for
a page with knobs and meters, its routes literals the host can list and the library refusing what none names. It has
files ([docs/files.md](docs/files.md)): a program lists a folder and reads whole files into memory off the audio thread,
by literal names (a folder its host maps, then a path inside it) that the host can list before anything runs, and
reaches nothing its host did not grant (no files by default), each directory opened beneath the last with no link
followed and compared by identity with the denied ones; built with `FMINUS_FILES_WRITE` (off by default) it saves
files too, each replaced whole or not at all. It has SIMD: OpenCL C's `float2`, `float4` and `float8`, and `poly float`, a vector as wide as the machine's. It has threads of
its own, structured: `parallel for` spreads a loop's iterations over the machine's cores and `parallel { }` runs a
block's statements at once, each finished where it ends, with the races the compiler can see refused, on a pool of
threads that a `[[clang::nonblocking]]` function may use. A program with a `float main(...)` also builds into an
executable. Its compiler has optimisations of its own, from `-O1`: a method gets a copy of its code for each value of a
knob that steers its loop, and a local's zeroing (an array's, or a module's overwritten whole) is removed where nothing
reads the zeros. It tells LLVM C's strict-aliasing rule, that a loop whose condition is not a constant may be assumed to
end, and what the runtime's functions do. Its math builtins give a float one result wherever it is computed, and a host
may have subnormals flushed to zero, give a program a loop budget, so that a call that would loop for ever returns early
instead of holding the audio thread, and compile it with bounds checks while it is written, so that an index outside its
array stops the call and says where. A program's instrumentation can sit in C's conditionals, `#ifdef DEBUG` ...
`#endif`, compiled in when the host defines `DEBUG` and skipped, unread, when not. A host's sampling profiler can map
each sample to the function, method and module whose machine code it fell in. `examples/plugin/` is a JUCE plugin whose
DSP is an FMinus program, and `import fminus` is the library as a Python package, which compiles programs and calls them
with Python's numbers and buffers. The build, the dependencies and the toolchain's smoke tests are in place around it.

Documentation: [docs/](docs/README.md). It covers the architecture, writing a program with `main`, a guide to
extending the compiler with worked recipes, SIMD (vector types, a vector math library, and vectors as wide as the
machine's), the Python package, the web library, the file interface, the build, and testing and debugging.

## Quick start

```bash
cmake --workflow --preset dev
```

The first run downloads the pinned sources: LLVM (179 MB), Bison, Flex, CLI11 and Catch2. It builds Bison and Flex
(about a minute) and one LLVM flavor (10–30 minutes, with the output streaming) into the dependency cache, `.deps/`.
Later configures, in this build tree or any other, find them there in seconds. Then it builds FMinus and runs every
test.

On Windows, install Visual Studio's "C++ Clang tools for Windows" and run the same command, best from its "x64 Native
Tools Command Prompt". The build uses clang-cl there, and builds WinFlexBison, the same Bison and Flex ported to
Windows, in place of their `./configure && make` ([docs/building.md](docs/building.md#windows)).

## Layout

```
CMakeLists.txt          the project; cmake/ holds the parts
CMakePresets.json       dev, debug, release, asan, tsan, inline, python, web, files, xcode
pyproject.toml          pip install .: the Python package, built by this CMake build
cmake/
  FMinusPins.cmake        every dependency: version, URL, SHA-256
  FMinusClangCl.cmake     Windows: clang-cl, found before project() (Visual Studio's, PATH's, LLVM's)
  FMinusOptions.cmake     options, the deps cache, ccache, fminus_target_defaults()
  FMinusLLVM.cmake        LLVM (cache | inline | system) -> FMinus::LLVM
  FMinusParserTools.cmake Bison and Flex (fetch | system; WinFlexBison on Windows) -> fminus_add_grammar()
  FMinusFastFloat.cmake   fast_float, the numbers of the parsers, the web server and string_float (every build)
                          -> fminus_fast_float
  FMinusJson.cmake        yyjson, the JSON parser, compiled into the runtime (FMINUS_JSON) -> fminus_yyjson
  FMinusXml.cmake         pugixml, the XML parser, compiled into the runtime (FMINUS_XML) -> fminus_pugixml
  FMinusWeb.cmake         libcurl and Mbed TLS, built at configure time into the deps cache, for the web library
                          (FMINUS_WEB) -> fminus_web_curl, fminus_web_mbedtls
  FMinusLlhttp.cmake      llhttp, the web server's request parser, compiled into fminus_web (FMINUS_WEB_SERVER)
                          -> fminus_llhttp
  FMinusCLI11.cmake       CLI11 -> CLI11::CLI11, linked into every tool
  FMinusTesting.cmake     Catch2 and lit
  FMinusPython.cmake      Python and pybind11, for the Python package (FMINUS_BUILD_PYTHON)
  templates/Config.h.in   -> <build>/include/fminus/Config.h
include/fminus/         the library's public API ("fminus/..."): FMinus.h, fminus::Program (below); Circuit.h,
                        fminus::circuit::generate (SPICE-style netlists to FMinus source) and its self-test,
                        fminus::circuit::selfTest (docs/circuits.md)
src/                    the fminus library: every .cpp is a source, every grammar is generated in
  api/                  the public API (include/fminus/FMinus.h) on the pipeline; CircuitSelfTest.cpp, the circuit
                        compiler's self-test (it compiles candidates through fminus::Program)
  support/              integration helpers that came with the skeleton: LLVMInit, HostTarget (the host's
                        TargetMachine and the O0-O3 pipeline, with FMinus's passes, and the vector math library in
                        the fast math mode), JitRuntime (the runtime, defined in an LLJIT), Aot (object and
                        assembly files, linked executables)
  runtime/              the FMinus runtime: its own library, libfminus_runtime.a, with no LLVM in it; the C
                        functions generated code calls, listed in RuntimeSymbols.def (a placeholder ABI version,
                        an executable's command line and result, the heap, the FFT, Fft.cpp, on PFFFT's objects,
                        which the library holds, the math builtins' functions, Math.cpp, on FMinus's math
                        kernels, MathKernels.h, bytes to numbers, Bytes.cpp, with a format's text, Format.h, the
                        JSON parser, Json.cpp, on yyjson's objects, and the XML parser, Xml.cpp, on pugixml's,
                        which the library holds too, the paths they read, Tables.h, numbers from text,
                        Numbers.cpp, on fast_float, and the file interface (FMINUS_FILES; docs/files.md): Files.h,
                        the host's half (the policy, its check, the context), Files.cpp (names, quotas, handles,
                        reach, every operation's one way in, an executable's context), FilesSystem.h, what it asks
                        of the system, FilesPosix.cpp and FilesWindows.cpp)
  frontend/             FMinus v0's front end: Lexer.l, Parser.y, Ast.h (the syntax tree), Diagnostics.h,
                        Frontend.h (parse), Scanner.h (the glue between scanner and parser), Preprocessor.h (macros,
                        and the expressions of #if)
  ir/                   the syntax tree to LLVM IR: CodeGen.h (ir::lower), Lowering.h (the code generator's class
                        and its parts' map), CodeGen.cpp (the passes), Types, Functions, Statements, Expressions,
                        Constants (integer constant expressions, array sizes, enumerations, the top level's
                        constants), Calls, Strings (string literals as builtins' arguments, and formats), Text (the
                        type string: a literal as one, ==, a switch on one, the string_* builtins), Paths
                        (JSON Pointers and XML paths, prepared into tables of steps), Shapes (a type's shape, what
                        the runtime's JSON functions walk: json_read's table of a module), Web (a URL literal's checks,
                        the program's web context, the status codes' names), WebServer (a route literal's checks,
                        the route table, a reply's format), Files (a file's name checked against the rules and the
                        build's ceilings, the program's file context, the status codes' names), Math
                        (the math modes), Hash (hash and randf), Vectors, Memory, Modules, Effects, Tbaa (strict aliasing's tags),
                        Denormals (flush-to-zero; with Denormals.h) and Bounds (bounds checks) .cpp
  jit/                  Jit: LLJIT for FMinus modules (host CPU, pipeline, runtime); Call: the thunks that call a
                        function by its signature (Program::call, fminus --run)
  opt/                  FMinus's own LLVM passes: KnobVersioning (a method's copies for its knobs' values),
                        ZeroInitElision (a local's zeroing, removed where nothing reads it), MathKernels (the
                        math builtins' kernels, inline), BoundsCheck (a failed bounds check made a return) and
                        LoopBudget (runaway-loop protection), with EarlyReturn, what the last two share
  web/                  the web library (FMINUS_WEB; docs/web.md): its own library, libfminus_web.a, with no LLVM in
                        it: the C functions the http_ builtins call (Web.h, WebSymbols.def), one I/O thread on
                        libcurl's multi interface, the host's policy (Policy.cpp: the patterns, the URL rules, the
                        address check), the system's root certificates (Roots*.cpp), an executable's context (Main.cpp);
                        and its server (FMINUS_WEB_SERVER): Server.cpp (the slots, the connections on llhttp, the
                        rules a request meets), ServerPolicy.cpp, ServerRoot.cpp (a directory as static files),
                        Sockets*.cpp (a file a system), Handle.h (the kind every handle carries)
  circuit/              the circuit compiler: its own library, libfminus_circuit.a, with no LLVM in it (Netlist,
                        Plan, Dc (the DC operating point), Mna, Laws, Linear (matched designs, the method check),
                        Emit, Halfbands, Generate; Reference and Analysis, the double reference and the measurements
                        its checks use; Search, the self-test's arithmetic, and Runners, the FMinus that drives a
                        module over buffers); docs/circuits.md
tools/<name>/           one executable per directory, with CLI11 for its command line: fminus (the driver),
                        fminus-build and fminus-hostinfo
examples/<name>/        one example per directory: an FMinus program, and a harness if it has .cpp files
                        (bin/example-<name>):
  biquad/               a biquad module with per-sample coefficients, its cutoff a stream, against C++, compared
                        and timed; a plan to grow it
  note/                 a whole program with a main: a MIDI note's frequency (docs/programs.md)
  comparison/           a main that returns a bool: whether its two arguments are equal
  waveforms/            band-limited sawtooth and square waves by additive synthesis: for, while, if, break,
                        continue, a switch on an enumeration, and a constant of the top level
  noise/                white noise from a count of samples (randf), and a voice's source of its own (hash)
  chord/                a chord's four notes in a float4: one vector expf, literals, swizzles, reductions
  unison/               detuned sawtooth voices, one a lane of a poly float: a strip-mined loop with a mask
  delay/                a feedback delay whose line the program allocates, run by a [[clang::nonblocking]] kernel:
                        as plain C functions, and as a module with knobs
  saturator/            tanh(drive * x), a loop over samples against one over poly floats (poly_load, poly_store);
                        the drive a knob
  meter/                a block's peak and RMS, by plain loops and by poly floats' reductions; a meter module whose
                        held peak falls by a knob
  voices/               eight synth voices in modules, in memory a host of the public API allocates, its controls
                        knobs, against C++
  convolver/            a long FIR filter by FFT (overlap-add), against C++ and a direct convolution
  parameters/           a filter's cutoff swept by an LFO through a knob or a stream, or frozen, against C++
  wavetable/            a wavetable oscillator whose tables, gain and interpolation come in a JSON or an XML
                        document its host hands it (json_floats, json_base64_floats, xml_floats, the choices),
                        against C++
  fetch/                the wavetable's pack fetched from a loopback server its host allows (http_get, http_wait,
                        Options::web; a build with FMINUS_WEB)
  remote/               a synthesiser a browser controls: its gain, pitch and meter served from its nonblocking
                        process (http_listen, http_next, http_is, the replies), its page static content the host
                        hands over; driven over loopback by the library's own client (a build with
                        FMINUS_WEB_SERVER)
  kit/                  a drum kit whose sounds are files: a folder its host grants listed and loaded off the audio
                        thread, played from a nonblocking process, and a take saved (file_list, file_read_nth,
                        file_write, Options::files)
  trigger/              an envelope a gate triggers, its note-on found by `when` across the host's blocks, against an
                        if that runs on every sample the gate is high
  settings/             a synth's settings read from a JSON document in one call (json_read): ranges, defaults, an
                        enumeration by name, an array of modules, a json::name and a json::skip
  hello/                FMinus as a library at its smallest: a main that JIT-compiles a program from a string
  plugin/               FMinus Filter, a JUCE plugin (AU, VST3, Standalone) whose DSP is an FMinus program: its
                        own CMake project, built apart from FMinus's (cmake -S examples/plugin -B build-plugin)
  circuits/             the circuit compiler's netlists (.cir): the research's examples and Phase 1's (switches,
                        matched linear blocks, a linear-only region, a block that rings), checked against their
                        targets, the accuracy tables and the costs reprinted from emitted code (--check, --tables)
python/                 the Python package, `import fminus` (FMINUS_BUILD_PYTHON; docs/python.md): src/, its
                        extension module, on the public API alone (pybind11, C++17); fminus/, the Python around it
                        (compile, load); tests/ (unittest) and examples/
tests/
  unit/                 your Catch2 tests
  library/              the public API as a host uses it: in C++17, against FMinus::FMinus alone
  smoke/                the toolchain's checks: LLJIT + O3 on the host, Bison + Flex (smoke/calc/)
  lit/                  lit + FileCheck tests (*.test, *.fm, *.ll); lit/circuit/ the circuit compiler's
editors/vscode/         a VSCode extension: syntax highlighting for .fm files (its README installs it)
Scripts/pin-llvm.sh     the pin lines for another LLVM release
Scripts/pin-web.sh      the pin lines for other libcurl, Mbed TLS and llhttp releases (the web library's)
docs/                   the manual: architecture, programs, extending, SIMD, building, testing; recipes/ as patches
```

Adding a source, a header, a grammar, a tool or a test edits no CMake: the globs pick it up when you build again.

## Dependencies

Everything comes through CMake's FetchContent, pinned by hash in `cmake/FMinusPins.cmake` (JUCE by its git commit).
Nothing is taken from the system unless an option asks for it.

| Dependency | Version | How | Cached in |
|---|---|---|---|
| LLVM (llvm/ only) | 23.1.3 | source release, built and installed once per flavor | `.deps/install/llvm-<ver>-<flavor>` |
| GNU Bison | 3.8.2 | source tarball, `./configure && make` at configure time | `.deps/install/bison-3.8.2` |
| Flex | 2.6.4 | source tarball, `./configure && make` at configure time | `.deps/install/flex-2.6.4` |
| WinFlexBison (Windows, in place of the two above) | 2.5.25 and fixes: Bison 3.8.2, Flex 2.6.4 | source archive of a commit, built with CMake at configure time | `.deps/install/winflexbison-2.5.25-a243a1c` |
| PFFFT | marton78's fork, commit aa16fd3 (2026-09-29) | source archive of a commit; two C files compiled into `libfminus_runtime.a` | `.deps/src/pffft-<commit>` |
| fast_float (`FMINUS_JSON` or `FMINUS_XML`, both on by default, or `FMINUS_WEB_SERVER`) | 8.3.3 (2026-10-31) | source archive of the release's tag; headers, compiled into `libfminus_runtime.a` (and `libfminus_web.a`, for the server's query numbers) | `.deps/src/fast_float-<version>` |
| yyjson (`FMINUS_JSON`, on by default) | 0.13.0 (2026-09-08) | source archive of the release's tag; its reader, one C file, compiled into `libfminus_runtime.a` | `.deps/src/yyjson-<version>` |
| pugixml (`FMINUS_XML`, on by default) | 1.16 (2026-06-16) | source archive of the release's tag; one C++ file, without XPath, exceptions or the STL, compiled into `libfminus_runtime.a` | `.deps/src/pugixml-<version>` |
| libcurl (`FMINUS_WEB`, off by default) | 8.23.0 (2026-10-14) | the release's tarball from curl.se; built with its own CMake at configure time, static, HTTP and HTTPS only, into `libfminus_web.a`'s dependencies | `.deps/install/curl-<version>-<flavor>` |
| Mbed TLS (`FMINUS_WEB`, macOS and Linux) | 4.1.1, the 4.1 LTS branch | the release's tarball (a GitHub release asset); built with its own CMake at configure time, static, threading on | `.deps/install/mbedtls-<version>-<flavor>` |
| llhttp (`FMINUS_WEB_SERVER`, off by default) | 9.4.3 (2026-07-30) | source archive of the release's tag, `release/v9.4.3` (it holds the generated C); three C files compiled into `libfminus_web.a`, hidden, every lenient flag off | `.deps/src/llhttp-<version>` |
| CLI11 | 2.7.2 | release source package, header-only, inside each build tree | the build tree |
| Catch2 | 3.16.1 | source archive, built inside each build tree | the build tree |
| pybind11 (the Python package only, `FMINUS_BUILD_PYTHON`) | 3.1.0 | PyPI's source distribution, header-only, inside each build tree | the build tree |
| JUCE (`examples/plugin/` only) | 8.0.4, commit 51d11a2 | git clone of the pinned commit, by the plugin's own build | the plugin's build tree |

`.deps/` holds `src/` (extracted sources), `build/` (dependency build trees) and `install/`. FetchContent keeps each
tarball in the build tree that downloaded it (`_deps/<name>-tmp/`), so a second build tree downloads only the small
in-tree dependencies again.
`FMINUS_DEPS_DIR` (CMake or environment variable) moves it, for example to share one cache between checkouts.
Of the llvm-project source, only what `llvm/` builds from is kept after extraction: `llvm/` without its regression
tests (`llvm/test`), `cmake/`, `third-party/` and `libc/`: about 300 MB of the 2.4 GB it unpacks to.
`FMINUS_LLVM_KEEP_ALL_SOURCES=ON` keeps it all (delete `.deps/src/llvm-project-*` to extract again). Deleting
`.deps/build/` frees most of the space once everything is installed, and deleting `.deps/` entirely is safe: the next
configure rebuilds what it needs.

**LLVM modes** (`FMINUS_LLVM_MODE`):

- `cache` (default): LLVM is built in its own tree with its own flags; FMinus's warnings and sanitizers never reach it.
  A *flavor* is LLVM's build type, assertions, targets and options; its fingerprint is in the install's name. `dev`,
  `debug`, `asan` and `tsan` share one flavor (Release with assertions), and `release` uses another (no assertions).
- `inline`: LLVM is a subproject of the build tree. Ninja tracks it, and a debugger steps into it with its sources
  (the `inline` preset adds `-g`). Every inline build tree builds its own copy.
- `system`: `-DLLVM_DIR=<prefix>/lib/cmake/llvm`, for an LLVM you already have.

Other options: `FMINUS_LLVM_TARGETS` (default `host`; for example `AArch64;X86` to cross-compile),
`FMINUS_LLVM_COMPONENTS` (what `FMinus::LLVM` links), `FMINUS_LLVM_TOOLS` (FileCheck, not, count, opt, llc, llvm-as,
llvm-dis, llvm-link, llvm-mca, llvm-objdump, llvm-config), `FMINUS_LLVM_EXTRA_OPTIONS` (more `NAME=VALUE` settings,
part of the flavor), `FMINUS_LLVM_BUILD_TYPE` (cache mode). `FMINUS_PARSER_TOOLS_MODE=system` uses Bison ≥ 3.8 and
Flex ≥ 2.6 from PATH instead; macOS's own `/usr/bin/bison` is 2.3, which is too old.

## Grammars

`fminus_add_grammar(<target> PARSER x.y LEXERS x.l ...)` runs Bison and Flex at build time and adds the outputs to the
target. Under `src/` this happens on its own: every directory holding a `.y` is a grammar, with the `.l` files beside
it. Outputs keep their path relative to `src/`:

| Source | Generated | Include as |
|---|---|---|
| `src/frontend/Parser.y` | `Parser.tab.cpp`, `Parser.tab.hpp`, `location.hh`, `Parser.output` (Bison's report) | `"frontend/Parser.tab.hpp"` |
| `src/frontend/Lexer.l` | `Lexer.yy.cpp`, `Lexer.yy.hpp` (the scanner's API) | `"frontend/Lexer.yy.hpp"` |

Bison runs with `-Wall -Wcounterexamples` (`FMINUS_BISON_FLAGS`), so a conflict comes with an example input.
`tests/smoke/calc/` is a working pattern to copy. It has a Bison C++ parser with variants, token constructors,
locations and detailed errors, and a reentrant, prefixed Flex scanner reading from memory. `CalcScanner.h` is the glue
between the two: `YY_DECL`, the scanner's state and the `yylex` the parser calls.

## The language (v0)

FMinus v0 is a subset of C: C's syntax, and C's semantics where it has them.

```c
/* A soft clipper, in FMinus v0. */
float half(float x);                     // a prototype: C needs one to call half before its definition (FMinus doesn't)

float softclip(float x, const float drive)
{
    const float y = x * drive;           // const: never assigned
    float out = y / (1.0f + fabsf(y));   // <math.h>'s float functions are builtins
    out *= 0.5f;                         // locals and parameters are variables, as in C
    return out;
}

float chain(float x)
{
    return softclip(half(x), 4.0f);      // a function may be called before it is defined
}

float half(float x) { x /= 2; return x; }
```

| | |
|---|---|
| program | function definitions, `float name(float a, const float b) { ... }` or `void name(...)`, and prototypes, `float name(float);`; `f(void)` is C's empty list; after the parameters, clang's `[[clang::nonallocating]]` or `[[clang::nonblocking]]`, checked: the function may not reach the heap (nor, nonblocking, an external function not marked so); modules, `module OnePole { float y; float tick(float x) { return y += x; } };`, whose fields are declarations without initialisers and whose methods are functions that see the fields by name (`this` is the module's address); a module's parameters, `param float cutoff [[fminus::range(20, 20000)]] [[fminus::default(1000)]];` (a `float`, `int` or `bool`), which the host sets and a method reads as `cutoff[i]`, its value at sample i of the call; enumerations and constants (below) |
| declarations | `float x = e;`, `const float k = e;`, `float* p = out;`, several at once (`float a = 1, b;`; in `float *p, x;` only `p` is a pointer, as in C); arrays, `float h[16];` and `int k[3] = {1, 2, 3};` (the elements left out are 0), `float buf[ROWS * COLS];` (a size is C's integer constant expression: an enumeration constant names one, a `const int` does not) and `float t[] = {1, 2, 3};` (its list's length), and a `const` one of constants, `const float t[4] = {1, 2, 3, 4};`, stored once (up to 1 MiB), a constant of the program that no call fills; 2-D arrays, `float grid[4][16];`, rows side by side (C's arrays of arrays), and their lists, rows braced or not, as C places them (`float g[2][3] = {{1, 2}, {3}};`, `int k[][2] = {1, 2, 3};`, two rows), the same everywhere a 1-D array goes; at the top level, constants, C's file-scope const objects: `const float table[ROWS * COLS] = {...};` and `const float gain = 0.5f;`, their initialisers constants (numbers, enumeration constants, `sizeof`, other constant scalars of the top level, `hash` and `randf` of them, an element's address, operators, casts, `?:`, `&&` and `\|\|`), each stored once (up to 1 MiB), which every function and method declared after it reads (an `#include`d file's after the `#include`); modules, `OnePole lp;` and `OnePole b = a;` (a copy); block-scoped, shadowing allowed in an inner block; C's enumerations, `enum Wave { SAW, SQUARE = 4, TRIANGLE };` (`SAW` is 0, `TRIANGLE` 5, each an `int` constant), declared at the top level (the functions and modules after it see its constants), in a module (its methods too) or in a block, a name each in the scope a variable would have, and the names of its type after its list, as C declares them (`enum Wave { SAW, SQUARE } wave;`: a local, a field, or a constant at the top level); C23's fixed underlying type, `enum Wave : int { ... }` or `: bool`, whose constants must fit it, and which declares the tag ahead of its constants too (`enum Wave : int;`) |
| statements | `{ ... }`, `e;`, `return e;` and `return;`, `;`, `if (c) s else s` (an `else` closes the nearest `if`), `while (c) s`, `for (init; c; step) s` (its declarations are the loop's), `break;`, `continue;`, `switch (v) { case 1: s ... default: s }` (C's: an `int` or `bool` value, its cases integer constant expressions, which fall through to the next until a `break`; `case` and `default` are C23's labels, so a declaration may follow one), and a switch on a `string`, `switch (s) { case "saw": ... }`, its cases string literals, the first whose bytes equal the value's taken (WG14 N3895's, with lengths for NULs); C2y's declaration in an `if`'s or a `switch`'s head, `if (int n = count(); n > 0) s else s` and `if (json_doc doc = json_parse(data, bytes)) s` (whose condition is `doc`), its names in scope to the end of the whole statement, the `else` too; `defer s` (or `_Defer s`, ISO/IEC TS 25755's), whose statement runs when control leaves the block the `defer` is in, at its end or by `return`, `break` or `continue`, the last reached first (a `return`'s value is computed before); FMinus's `when (c) s`, which runs `s` each time `c` becomes true (true now, and false the last time this `when` was reached, or never reached: `if (c && !was) s; was = c;`), its state a `bool` of the method's module instance, so a method's statement, which in a loop over samples fires on each rising edge; a `while` or `for` whose condition is not a constant expression may be assumed to end, as in C11 (6.8.5p6), and `while (1)` and `for (;;)` may run for ever; FMinus's `parallel for (int i = first; i < end; ++i) s`, its iterations at once over the machine's cores, and `parallel { s1 s2 ... }`, its statements at once, each finished, all of it, where it ends (below); FMinus's `serve (server) { route "POST /note/{n}" (int n [[fminus::range(0, 127)]]) { ... reply 204; } ... }`, the requests waiting for its routes, each route's parameters filled from the path's holes and the query and checked (404, 400), and `reply`, which answers and leaves the route, a JSON literal `json { "gain": gain }` among its forms, in a build with the web server (below) |
| expressions | constants, names, calls, `( )`, unary `-` `+` `!` `~`, `++` and `--` (before or after), casts `(int)x` `(float)x` `(double)x` `(bool)x`, `* / %`, `+ -`, `<< >>`, `< <= > >=`, `== !=`, `&`, `^`, `\|` (the bitwise ones on `int`s), `&&`, `\|\|` (both short-circuit), `?:` (only the branch taken runs), and `= += -= *= /= %= &= \|= ^= <<= >>=` (right to left; an assignment's value is the one stored), with C's precedence; pointers' `p[i]`, `*p`, `&x`, `p + i`, `p - q` and comparisons; an array's `a[i]` (its name stands for a pointer to its first element), and a 2-D array's `g[i][j]` and its rows, `g[i]`, each a 1-D array, a `float*` wherever one goes; a module's fields `m.y` and `p->y`, its methods `m.tick(x)` and `p->tick(x)`, and `this`; `sizeof(type)`, an `int` (a type past the greatest `int`, a module over 2 GB, is an error), arrays' types too, `sizeof(float[4][16])`; vector literals, OpenCL C's `(float4)(a, b, c, d)` and C's `(float4){a, b}` (the lanes left out are 0), and swizzles `v.wzyx`, `v.s0`; C's string literals, a `string` where one is wanted (an initialiser, an assignment, an argument, a result, an operand of `==` and `!=`, a `case`) and a builtin's literal argument (below); `==` and `!=` of two strings, by content; a `char` array's `name[i]`, an `int` from 0 to 255 |
| constants | C's: floating ones are floats (`1.5`, `1.`, `.5`, `2e3`, `1.5e-3f`, hex floats `0x1.8p1`, `0x1p-3f`), or, without the `f`, doubles in arithmetic that has a double (below), integer ones ints (`2`, `010` in octal, `0x1F` in hex); `true` and `false`; enumeration constants, ints; integer constant expressions (a case's value, an enumerator's, an array's size) are C's, folded as FMinus computes |
| attributes | C23's `[[...]]`: after a function's parameters, clang's (above); after a parameter's or a field's name, before an array's sizes, FMinus's `fminus::range(min, max)` (a parameter's, or a field's of numbers: what a host's control or display spans, and the values `json_read` fills a field with) and `fminus::default(value)` (a parameter's, or a field's of numbers or `bool`s: what `json_read` sets the field to when a document does not fill it, which does not initialise it), `json::name("name")` (the JSON member a field is read as) and `json::skip` (a field `json_read` leaves alone), and any other, the host's, which FMinus passes on as written (FMinus's own too): `float level [[ui::unit("dB")]] [[fminus::range(-60, 0)]];`, `float taps [[ui::hidden]] [16];` (but a name without a namespace that C23 does not have, `[[range(0, 1)]]`, is clang's warning, and ignored); their names identifiers, keywords and modules' names among them, as C23 reads them; their arguments numbers and C's string literals (C's escapes; adjacent literals are one) |
| comments | `// ...` and `/* ... */` |
| directives | `#include "file"`, the file read where the line is, as in C, and once only (as if every file began with `#pragma once`), found where the host says: `fminus` looks beside the including file, then in `-I`'s directories; C's conditionals, `#if`, `#ifdef`, `#ifndef`, `#elif`, `#else` and `#endif` (and C23's `#elifdef` and `#elifndef`), whose skipped groups are never scanned, on macros that `#define NAME value` and `#undef` make, or the host (`fminus -D NAME[=value]`), read in their conditions only, and FMinus's own, `__FMINUS_JSON__`, `__FMINUS_XML__`, `__FMINUS_WEB__`, `__FMINUS_WEB_SERVER__`, `__FMINUS_FILES__` and `__FMINUS_FILES_WRITE__`, defined (to 1) when the build has the JSON parser, the XML parser, the web library, its server, the file interface or its writing (`FMINUS_JSON`, `FMINUS_XML`, `FMINUS_WEB`, `FMINUS_WEB_SERVER`, `FMINUS_FILES`, `FMINUS_FILES_WRITE`), which neither a program nor the host may define or undefine; `#pragma`, ignored |
| types | `float` (IEEE single precision), `double` (IEEE double precision: for accumulators and slow state), `int` (32 bits, signed: C's `/` and `%`, overflow undefined) and `bool` (C23's: 0 or 1, computed as an int; a number is `x != 0` as a bool); `void`, a function's result; `string`, FMinus's: a read-only view of bytes something else owns, a pointer and a length (C's `struct { const char* data; int size; }`), no NUL after it and no encoding checked, compared by content, read by the `string_*` builtins, empty (null, 0) until set, and a literal one for ever; `char`, an array's element and a pointer's pointee only (`char name[32];`, `const char* p`), a byte read as an `int` from 0 to 255, where a program keeps text (`string_copy`, `string_of`, `char name[8] = "abc";`); pointers to `float`, `double`, `int`, `float2`, `float4`, `float8`, `char` and modules, `const` (`const float*`, read-only through it) and `restrict` (C99's promise that nothing else reaches it), and `void*` (an address with no type, which converts to and from any pointer); the handles of the runtime's objects, `http_response` (a request the program made), `http_request` (one it received), `http_server`, `json_doc`, `xml_doc`, `file_content`, `file_listing` and `fft_plan`, each C's opaque handle (`typedef struct fminus_json_doc* json_doc;`): what a builtin gives and others take, a `void*` converted to one and back, as C converts it, and one of another kind refused; compared with `==` and `!=`, tested as a condition (null is false), and kept in arrays; no arithmetic, element or field, and no pointer to one; arrays of `float`, `double`, `int`, the float vectors, `char`, `string`, modules and handles, one or two dimensions deep; modules, by name (`OnePole`), from their declaration on; `enum Wave`, an `int`, after its enumeration; `float2`, `float4`, `float8` (OpenCL C's vectors, lane by lane, scalars broadcast); `poly float` and `poly bool`, as many lanes as the machine's vectors ([docs/simd.md](docs/simd.md)) |
| builtins | `fabsf sqrtf floorf fminf fmaxf` (LLVM intrinsics, exact) and `sinf cosf tanf expf logf tanhf` (by the math mode: the runtime's, with one result for a float wherever it is computed, by default), on floats and vectors (a double argument rounds to a float, as C's prototype converts it); `sqrt fabs floor fmin fmax fma` (C's double functions, exact, on doubles only; `fma` rounds once); `malloc calloc free` (C's, with `int` sizes); `reduce_add reduce_min reduce_max` (a vector to a float); `poly_lanes` (an int) and `poly_index`; `poly_load poly_store` (a poly float from or to a `float*`, with an optional mask); the FFT: `fft_create(n)` and `fft_create_complex(n)` (a plan, an `fft_plan`; they allocate), `fft_forward(plan, in, out)` and `fft_inverse(plan, in, out)` (real when one side is floats, complex when both are `float2`s; unnormalised), `fft_destroy(plan)`; `hash(n)` (an `int` mixed into a well-spread one) and `randf(n)` (a float uniform in [-1, 1) from it: white noise, `out[i] = randf(start + i)`); bytes to numbers, `bytes_floats(data, bytes, out, cap)` (raw little-endian float32s, as a program's own server sends them) and `bytes_pcm16(data, bytes, out, cap)` (little-endian 16-bit PCM, scaled to [-1, 1)), into floats: how many values the `bytes` bytes at `data` hold, the first `cap` of them written (a null `data` or a negative size is none), wait-free, so a nonblocking function may call them; JSON (`FMINUS_JSON`, below): `json_parse(data, bytes)` (a document, a `json_doc`, null for a text that is no JSON, over 64 MiB or nested deeper than 1024) and `json_free(doc)`, which allocate, and the reads, wait-free, at a path, a JSON Pointer literal with places for ints (`"/osc/%d/detune", k`): `json_float`, `json_double`, `json_int` and `json_bool` (the value, or their last argument when there is none of that kind), `json_count` (an array's or an object's size), `json_floats(doc, path, out, cap)` (an array of numbers, or of rows of them: how many, the first `cap` written), `json_choice(doc, path, "a", "b", ...)` (which literal a string equals, or -1), `json_base64_floats(doc, path, out, cap)` (a base64 string's float32s) and `json_read(doc, path, ..., &module)` (the module filled from the object there, field by field under each field's name in one walk of it, and whether every field without a default was filled); XML (`FMINUS_XML`, below), the same way: `xml_parse(data, bytes)` (an `xml_doc`, null for a text that is no XML document, over 64 MiB or nested deeper than 1024) and `xml_free(doc)`, which allocate, and the reads, wait-free, at a path, a small part of XPath with places for ints (`"/preset/osc[%d]/@detune", k`: child steps by name, an index **from 0**, a last `@attribute`), of an attribute's value or an element's text: `xml_float`, `xml_double`, `xml_int` and `xml_bool` (the value, or their fallback), `xml_count` (how many elements the last step matches), `xml_floats(doc, path, out, cap)` (a list of numbers, separated by white space or commas) and `xml_choice(doc, path, "a", "b", ...)`; the web (`FMINUS_WEB`, below; [docs/web.md](docs/web.md)): `http_get(url, ...)` and `http_post(url, data, bytes, content_type, ...)` (a request, an `http_response`, at once; the URL a literal whose `%d`s are the ints after every other argument, the content type a literal) and `http_cancel(r)` and `http_close(r)`, which allocate, `http_wait(r, timeout_ms)` (the status, once done or the time is up), which blocks (refused in a nonblocking function, allowed in a nonallocating one), and the reads, wait-free: `http_status(r)` (0 while pending, the HTTP status, or a failure, `HTTP_TRANSPORT`, `HTTP_TIMEOUT`, `HTTP_CANCELLED`, `HTTP_REFUSED`, `HTTP_LIMIT`, `HTTP_CERTIFICATE`, `HTTP_INVALID`, `HTTP_FAILED`, names a program uses without declaring them), `http_size(r)` and `http_body(r)` (a `const void*`, until `http_close`; of a request received too), `http_progress(r)`; a null handle does nothing and reads as a failed request; the web server (`FMINUS_WEB_SERVER`, below; [docs/web.md](docs/web.md), "The server"): `http_listen()` (the program's server, an `http_server`, where its host's policy says: nowhere by default) and `http_close(server)`, which allocate, `http_accept(server, timeout_ms)` (a request, an `http_request`, waiting for one), which blocks, and, wait-free, `http_port(server)` (its port, or `HTTP_REFUSED`, `HTTP_TRANSPORT`, `HTTP_CANCELLED`), `http_next(server)` (the oldest request waiting, or null), `http_is(r, "PUT /osc/%d/gain")` (whether the request is the route, a literal checked when compiled), `http_arg(r, i)` (a number its `%d` took), `http_query_float(r, "name", fallback)` and `http_query_int`, `http_size(r)` and `http_body(r)` (its body), and the replies, `http_reply(r, status)`, `http_reply_bytes(r, status, data, bytes, "type")` and `http_reply_text(r, status, "type", "format", ...)` (C's `printf` of `%d`, `%g`, `%.Ng` and `%%`); the file interface (`FMINUS_FILES`, on by default, below; [docs/files.md](docs/files.md)): `file_read("folder/name.ext", ...)` (the file read whole into memory, a `file_content`; the name a literal, a folder the host maps and a path inside it, whose `%d`s are the ints after every other argument), `file_list("folder/*.ext", ...)` (a snapshot of a folder's files that fit, in their names' order, a `file_listing`), `file_read_nth(list, i)` (a `file_content`) and `file_close(h)` (of either), which allocate, and the reads, wait-free: `file_status(h)` (`FILE_OK`, or `FILE_MISSING`, `FILE_REFUSED`, `FILE_DENIED`, `FILE_LIMIT`, `FILE_FAILED`, `FILE_INVALID`, names a program uses without declaring them), `file_size(f)` and `file_data(f)` (a `const void*`, until `file_close`), `file_count(list)`; with `FMINUS_FILES_WRITE`, `file_write("folder/name.ext", data, bytes, ...)` (the whole file, replaced in one step or not at all) and `file_append(...)`, which allocate, each giving a status; a null handle reads as no file. Each handle a builtin takes may be a `void*`, as before FMinus had handles, and a JSON or XML read's document a `const void*` too; a string's, every one wait-free, in every build: `string_size(s)` (its length in bytes), `string_byte(s, i)` (byte `i` as 0 to 255, or -1 outside it), `string_slice(s, from, to)` (the bytes from `from` up to `to`, each end clamped to the string; empty when they cross), `string_find(s, what)` (where `what` first occurs, or -1; an empty `what` at 0), `string_starts(s, with)` and `string_ends(s, with)`, `string_float(s, fallback)` and `string_int(s, fallback)` (the whole string read as a number as a query's is: no `+`, finite, an `int` that fits; else the fallback), `string_data(s)` (its bytes' address, a `const void*`), `string_copy(out, cap, s)` (its first `cap` bytes at most into a `char` array, and its length) and `string_of(data, size)` (a view of `size` bytes at `data`, a `const char*` or a `void*`: `string_of(http_body(r), http_size(r))` reads a body as text; a null `data` or a negative size is the empty string) |

Where FMinus differs from C, it is on purpose:

- **Floating constants are floats first**: `float`, `double` and `int` are the arithmetic types (no `long`, `unsigned`
  or narrower integers), and in C an unsuffixed `1.5` is a `double`, so `x * 1.5` of a float `x` is computed in
  double; in FMinus the constant is a float, and so is the product, as FMinus has always computed it. Unless the
  arithmetic the constant is part of has a double: then it is C's double, and the arithmetic computes as C's does, every
  step of it. A constant's arithmetic is the expression around it as far as `+`, `-`, `*`, `/`, `%`, unary `+` and `-`,
  the arms of `?:` and parentheses reach, and it has a double when one of its operands is one, when it is compared with
  one, or when its value goes to one: a `double` it initialises or is assigned to, a parameter, a result or a cast. So
  `double t = x * 0.1;` is C's double product, `double third = 1.0 / 3.0;` C's third, `d *= 0.999` multiplies by C's
  0.999 and `d < 0.1` compares with C's 0.1, while `float y = x * 0.1;` is a float product, and a program without
  doubles means what it meant before they came, bit for bit. Where an arithmetic has no double, its float result may
  differ from C's in its last bits: write the `f` suffix (`1.5f`) and a program means the same in both. Every example
  here does. An integer constant is an `int`, as in C, so `1 / 2` is 0; one too large for an `int` is an error, where C
  would make it a `long` ([docs/extending.md](docs/extending.md), recipe 33).
- **Pointers point to `float`, `double`, `int`, `float2`, `float4`, `float8`, `char`, `void` or a module, one level deep**: no
  pointers to pointers or to arrays, no `bool*`, no null pointer constant (`0` is an int; an uninitialised pointer is
  null) and no casts between pointer types. Converting a pointer to one of another type (other than through a `void*`),
  or dropping its `const`, is an error, where C warns. A 2-D array's name stands for a pointer to its first row, as in
  C (`float (*)[16]`): indexed, dereferenced, moved and compared as C's, but no variable, parameter or field holds one,
  and `&g[i]` is an error.
- **Arrays are C's, one or two dimensions deep**: of `float`, `double`, `int`, the float vectors, `char`, strings or modules (no arrays
  of three dimensions, of pointers or of `bool`), each size an integer constant expression from 1 to 2^30, and 2^30
  elements in all (no variable length arrays, and no zero-length ones, GNU's), and zeroed like every variable. An
  initialiser list may be shorter than the array, never longer, and a brace around one element (a 1-D array's, or
  inside a row) is an error too (C only warns of both). A 2-D array is no parameter: C's would be a pointer to its
  first row, so a function takes a row (`float*`), or the module that holds the array. A `const` one whose list is
  constants (and of 1 MiB or less) is stored once, a constant of the program, as C stores a `static const` one: every
  call reads the same elements as C's would, but from one object, so its address is the same in every call, nested
  ones too, where C gives each call its own, and two equal tables may be one object
  ([docs/extending.md](docs/extending.md), recipes 26 and 34).
- **The handle types are keywords**, where C's would be a header's typedefs (`typedef struct fminus_json_doc*
  json_doc;`): no variable or function may be named `json_doc`, as none may be named `float4`. A handle is C's pointer
  to a struct the program never sees, and does what C lets one do, but for `<`, `<=`, `>` and `>=`, which C allows
  between two of a kind and FMinus refuses (their order means nothing), and arrays of them, which FMinus has though it
  has no other arrays of pointers. A `void*` still converts to any handle, as in C, so a program written with `void*`
  before the handles came compiles as it did, unchecked as it was; and a JSON or XML read still takes a `const void*`
  document, which C would refuse for a typed one (the read only reads it). A handle is no parameter declared as an
  array (that would be a pointer to one), and `call()` and `--run` treat one as they treat a `void*`
  ([docs/extending.md](docs/extending.md), recipe 44).
- **`defer` is ISO/IEC TS 25755's**, a Technical Specification beside C, not yet C itself: `_Defer`, and `defer`, which
  the TS encourages, both keywords (FMinus substitutes no macros, so `<stddefer.h>`'s way is not open). It is a
  rewrite when the program is compiled, as clang's `-fdefer-ts` makes it: each exit of a block holds the code of the
  deferred statements of the blocks it leaves, with no flag or list at run time. A call that the loop budget or a
  bounds check stops returns at once, as it always has, and runs no deferred statement on the way, as the TS says of
  `exit` and a signal: those checks are made in the optimised code, where the blocks are gone
  ([docs/extending.md](docs/extending.md), recipe 44).
- **`if` and `switch` take C2y's declaration in their head** (N3356, as N3580 refines it), which C23 does not have.
  FMinus has no storage classes, so N3580's constraint on them has nothing to check.
- **The top level holds no variables**, only constants: a declaration there is `const`, its initialiser constants, a
  constant of the program, as C's file-scope const object is (an LLVM constant global of its name, of 1 MiB at
  most), which no call fills or copies. A program's state lives in its modules, each instance's own. C has
  file-scope variables; what it takes for a constant initialiser FMinus takes too, and a constant scalar of the top
  level, which C17 leaves to the compiler (6.6p10) and clang accepts (`const int M = N * 2;`), but not a table's
  element (`table[1]`), which clang also accepts, nor what C leaves undefined, which clang refuses for `1 / 0` but
  folds to a value for `(int)1e10f` and `INT_MIN / -1`. The global is the program's own, as a `static const` one is
  a C file's: no symbol of its name, for a call by that name or a host to find. A constant is no integer constant
  expression, in C17 as in FMinus: an array's size, a case's value or an enumerator's is named by an enumeration
  constant, C's idiom (`enum { ROWS = 17, COLS = 12 };`). A constant scalar reads as its value, as clang reads one,
  with no load ([docs/extending.md](docs/extending.md), recipe 28).
- **Modules are FMinus's own**, C++'s structs with member functions, and less: no constructors (a module's fields
  start at 0 wherever it lives, and take no initialisers: a method such as `init` sets them), no const methods, no
  inheritance, overloading, access control or static members. A module goes to a function and back by pointer only
  (C++ would copy it), and `?:` does not choose between two of them. Its name is a type from its declaration on, as a
  C struct's is, so a module holds only modules declared before it (and pointers to any). A method is named as C++
  qualifies it, `OnePole::tick`, in errors and to a host.
- **Parameters are FMinus's own**, as C has nothing like them: `param` fields of a module, which the host sets and its
  methods only read, as `cutoff[i]`. How one reads is the host's choice at compile time: a knob (its value, loaded
  atomically once when the method starts), a stream (a `float` a sample, from a buffer the host points it at, which
  nothing the method writes may overlap, as with `restrict`, or, when the pointer is null, the slot's value, for the
  whole call) or frozen (a constant). `fminus::range` and `fminus::default` are for the host; FMinus checks them against
  each other only, and passes on any other attribute, as a field's (below). From `-O1`, a method whose loop branches on
  a `bool` knob or an `int` knob whose range holds at most 8 values, or whose loop reads a stream, gets a copy of its
  code for each value (for a stream, null or not), which computes what the method does
  ([docs/extending.md](docs/extending.md), recipe 16). A stream that gets no copies (a method's fourth stream, under the
  default budget of 8 copies) still reads with no branch: its loop vectorises when the stream has samples, and runs a
  sample at a time when it is flat.
- **A field's attributes are the host's**: C23 ignores an attribute it does not know, and clang warns; FMinus passes a
  field's or a parameter's on to the host as written (`Module::Field::attributes`), checking only that its numbers
  are numbers, for the host to decide what `[[ui::unit("dB")]]` means. Its own namespaces, `fminus::` and `json::`, are
  checked (an unknown name is an error), and so is clang's, whose attributes are a function's; a function's unknown
  attribute, a local's and a constant's, and one after an array's size (C23's, for the array's type) are errors too.
  A name without a namespace is C's: one of C23's standard attributes goes to the host, and any other,
  `[[range(0, 1)]]`, is clang's warning (`unknown attribute 'range' ignored [-Wunknown-attributes]`, with a note,
  "did you mean 'fminus::range'?"), and goes nowhere. A string literal is C's, and an attribute's argument or a
  builtin's (below) ([docs/extending.md](docs/extending.md), recipes 35 and 50).
- **`string` is FMinus's own, and `char` only an element**: text is a read-only view, a pointer and a length over
  bytes something else owns (C would need `typedef struct { const char* data; int size; } string;` and a function for
  `==`), where C's is a `char*` that a NUL ends. A piece of a request's path or of a JSON document has no NUL after it,
  so a C string would be a copy, a length a scan, and every overrun undefined; a view reads what the runtime already
  holds, allocates nothing, and every operation on it is wait-free, allowed in a `[[clang::nonblocking]]` function.
  Nothing joins or builds one: text is made only where it goes (a builtin's format). `==` and `!=` compare two by
  content; `<`, `+`, `[]`, a cast and a test as a condition are errors (`string_size(s) > 0` asks whether one is
  empty). **A string lives as long as what it points into**, the rule `http_body`'s pointer has: one over a `char`
  array as long as the array, a literal for ever, and, as the builtins that give them come, one that came from a
  request until the request's reply, one from a document until `json_free` or `xml_free`, and one from a listing
  until its `file_close`. The compiler does not check it. A string is no `param`, no vector's lane, and no
  parameter or result of `main` or of a function the program declares and does not define (C's ABI passes a struct
  by each platform's own rules); `call()` and `--run` pass none. The builtins that give text from a request (its
  path, its query, a header), a document or a listing, and a string as a route's parameter, in a `{name}` hole or in
  a JSON literal, are the next step's. `char` is a byte an array holds or a pointer points to, never a variable, a
  parameter or a result, and an element reads as an `int` from 0 to 255 on every machine (C
  leaves a plain `char`'s sign to the implementation; FMinus's is unsigned, as clang's `-funsigned-char` makes it); a
  store keeps the low 8 bits. A `char` array is read and written as `char`s only: FMinus's `char` has none of C's
  licence to alias other types. `string`, `char` and `when` are keywords ([docs/extending.md](docs/extending.md),
  recipe 46).
- **`when` is FMinus's own**, as C has no state but what a program declares: `when (c) s` is `if (c && !was) s;
  was = c;`, `c` computed once and `was` stored before `s` runs (so a `return`, `break` or `continue` in `s` does not
  arm it again), `was` a `bool` the compiler keeps in the instance of the method's module, after the declared fields,
  one for each `when`. So it is a method's statement (anywhere else an error that says where its state would live);
  `sizeof` a module grows by its `when`s' bools, which `module()` lists no field for but its size and alignment count,
  and a copy of the module copies them; it is refused in a `parallel for`'s iteration and a `parallel { }` block's
  statement, where it would change the module from several threads, as a method that changes its module is; a
  `poly bool` condition is an error (a `when` is one decision). It allocates and waits for nothing, so a
  `[[clang::nonblocking]]` method may hold one ([docs/extending.md](docs/extending.md), recipe 47).
- **A string literal is a `string` where one is wanted, and otherwise an argument, never a value.** It is a value of
  type `string` as an initialiser, an assignment's value, an argument or a result of the program's own function, an
  operand of `==` or `!=`, an arm of a `?:` that gives a string, or a `case` of a switch on a string; its bytes are a
  constant of the program, C's escapes resolved and adjacent literals one, with no NUL counted (`"a\0b"` is three
  bytes). It is an attribute's argument, and the argument of a builtin's string parameter, which gets its bytes and
  their length and takes only a literal (a `string` variable there is an error). Anywhere else, where C would have a
  `char` array, it is an error, in clang's words and at clang's column where C converts one with a warning or refuses
  it (`passing 'char[2]' to parameter of incompatible type 'float'`), and in FMinus's, at the literal, where C would
  take it silently (as a `void*`, a `bool`, an operand). `char name[8] = "abc";` is C's: the literal's bytes, the rest
  0, an error if they do not fit. A builtin's format, a literal with places for ints, is checked as clang checks
  `printf`'s, but takes only `%d` (an `int`, an enumeration's or a `bool`) and `%%`, and its mistakes, clang's
  warnings, are errors ([docs/extending.md](docs/extending.md), recipe 38). A path into a JSON document is such a
  format and a JSON Pointer too, and one into an XML document such a format and an XML path, each prepared when the
  program is compiled; a malformed one is an error at its column.
- **JSON is FMinus's own**, as C has none: builtins on yyjson ([docs/extending.md](docs/extending.md), recipe 39).
  `json_parse` reads RFC 8259's JSON and nothing else (no comments, trailing commas, `NaN` or byte order mark; its
  strings' UTF-8 checked), and gives no document for more than 64 MiB of text, or for arrays and objects nested deeper
  than 1024 (`FMINUS_JSON_MAX_BYTES` and `FMINUS_JSON_MAX_DEPTH` in `fminus/Config.h`). A read never fails: a null
  document is an empty one, and a value that is absent or not of the read's kind gives its fallback. A number read as
  a `float` is the nearest float to its digits, read straight from them (never a `double` narrowed, which is now and
  then one bit off) and in no locale; `json_int` gives the fallback unless the number is an integer an `int` holds
  (`7.0` is 7, but `1.5` is not truncated and `3e9` not saturated). Parsing and freeing allocate, and are refused in a
  `[[clang::nonallocating]]` or `[[clang::nonblocking]]` function; every read is wait-free in the runtime, and allowed
  in both. Wait-free is not cheap: a read's time is bounded by the document, not by a constant. A step looks through
  an object's members, or an array of arrays or objects, up to the one it names (an index into an array of numbers
  and strings goes straight there), and a value is read whole, every digit of a number and every character of a
  base64 string, so one read of a large hostile document can take milliseconds, and a `%d` loop over n members or
  elements takes time in n². Read a document in `init`, into arrays, and keep the audio thread to those. There is no way to get a string itself out of a document: `json_choice` says which of the program's
  literals it equals. `json_read(doc, path, ..., &module)` fills a module from the object at a path in one walk of
  its members, by a table of the module's fields the compiler makes once a type ([docs/extending.md](docs/extending.md),
  recipe 50): each field from the member of its name, or of its `[[json::name("...")]]`, and only from a value of its
  kind inside its `fminus::range` (an `int` from an integer an `int` holds, an enumeration from its constant's name
  as written, `"SAW"`, or its number, a `char` array from a string that fits with room for a 0, an array from as many
  elements as fit); a member no field names is ignored. A field not filled is set to its `fminus::default`, or left
  as it was, and the result says whether every field without a default was filled. **A field's `fminus::default`
  does not initialise it**: a module's fields start at 0, as C's do, and the default is what `json_read` sets when a
  document does not fill the field. **A `string` field is not filled** (it would point into the document, which the
  module outlives), nor a `param` (its value is the host's), a pointer, a handle, a poly type or a field with
  `[[json::skip]]`; an array of modules the document does not have is left as it was, its elements' defaults unset.
  It is wait-free, as the other reads are, and costs a walk of the object, each member compared with the fields not
  yet named.
- **XML is FMinus's own too**, on pugixml, made as the JSON builtins are ([docs/extending.md](docs/extending.md),
  recipe 40): the same fallbacks, effects, limits (`FMINUS_XML_MAX_BYTES`, `FMINUS_XML_MAX_DEPTH`) and null
  document. **An index in an XML path counts from 0**, as C's arrays and JSON Pointer's do, where XPath's counts from
  1: `"/pack/table[0]"` is the first table, which XPath writes `/pack/table[1]`. The path is a small part of XPath:
  child steps by element name from the root, `[n]` after a name, and a last `@attribute`, with no axes, predicates,
  wildcards or functions; a name matches as written, its prefix too (`"/preset/fm:depth"`), since there are no
  namespaces. `xml_parse` is a reader, not a validator: it refuses what is not well formed, but takes invalid UTF-8,
  for one, though not a character reference to U+0000 (`&#0;`), which would cut a value short. It skips a DOCTYPE,
  so no entity a document declares is ever expanded and none outside it fetched (a "billion laughs" document is
  inert); it expands XML's predefined and character references. A value is an
  attribute's, or an element's first text (CDATA too; text either side of a comment is one), without the white space
  around it; numbers are XML Schema's (`+1.5`, `007`, `INF`, `NaN`), read as JSON's are; a bool is `true`, `false`,
  `1` or `0`. A read costs what a JSON read does: an element is found among its parent's children, an attribute among
  its element's, and a value is read whole.
- **The web is FMinus's own**, as C has none: builtins on libcurl, in a build with `FMINUS_WEB` ([docs/web.md](docs/web.md),
  and [docs/extending.md](docs/extending.md), recipe 41). A URL is a string literal, a format whose ints come after
  every other argument, as `printf`'s do, and is checked where it is written: printable ASCII with no space or
  backslash (no CR or LF), `http` or `https`, no credentials. Its requests reach only what its host allows
  (`Program::Options::web`: none by default), and the host can list every URL before anything runs
  (`Program::webRequests()`); the library checks each request, each redirect and each connection's address. A request
  is an `http_response` that comes back at once; failures are statuses (`HTTP_REFUSED`, `HTTP_TIMEOUT`, ...), never traps.
  `http_wait` is a builtin that blocks: refused in a `[[clang::nonblocking]]` function, allowed in a
  `[[clang::nonallocating]]` one. A program that uses a table on the block it arrived in gives up determinism: fetch in
  `init`, then play.
- **So is its server**, in a build with `FMINUS_WEB_SERVER` too ([docs/web.md](docs/web.md), "The server", and
  [docs/extending.md](docs/extending.md), recipe 42). C has no callbacks a program could be handed a request through,
  nor do FMinus programs, so a program **takes** its requests (`http_next`, wait-free, on the audio thread between
  blocks; `http_accept`, which blocks, on a thread of its host's) and answers each with a reply builtin. A route is a
  string literal, a method and a path whose `%d`s are numbers of the request's path, checked where it is written and
  listed for the host (`Program::webRoutes()`); a reply's text is a format of C's `printf`, `%d`, `%g`, `%.Ng` and `%%`,
  checked as clang checks `printf`'s and written the same in every locale. Where it listens is the host's
  (`Program::Options::web.server`: nowhere by default), never the program's: `http_listen()` takes no argument. The
  library answers 404 and 405 for what no route names, and checks `Host`, `Origin`, `Sec-Fetch-Site`, the token and
  every limit before a request reaches the program. A program never sees a request's path as text, nor a header.
- **`serve`, `route` and `reply` are FMinus's own statements** over those builtins, as `parallel` is ([docs/web.md](
  docs/web.md), "serve, route and reply", and [docs/extending.md](docs/extending.md), recipe 45): a `serve` takes the
  requests waiting for its own routes only (wait-free; with a timeout it blocks), a route's parameters are typed and
  filled from its literal's holes, `{name}`, and the query (what does not fit is 404 or 400 before its block runs), and
  `reply` answers the route's request and leaves the route, which every path through the route must reach, so a request
  a serve takes is answered once. Two routes one request could match are an error unless one is more specific (Go's
  `ServeMux` rule). `serve`, `route`, `reply` and `json` are keywords: nothing may be named so.
- **A JSON literal is an argument, never a value**, as a format is: `json { "gain": gain, "taps": [a, b] }`, a
  `reply`'s body, which the compiler makes a format of the reply's and the library writes into the request's buffer, a
  float's and a double's numbers the shortest digits that read back the same, a NaN or an infinity `null`. FMinus has
  no JSON value, and C no literal of the kind.
- **Files are FMinus's own too**, in place of C's `<stdio.h>`, in a build with `FMINUS_FILES` (on by default;
  [docs/files.md](docs/files.md), and [docs/extending.md](docs/extending.md), recipe 43). A program never names a path
  of the machine: a name is a string literal, a folder its host maps and a path inside it (`"presets/%d.json"`, its
  ints after every other argument), checked where it is written (no `..`, no hidden file, no device name of Windows,
  nothing Windows would read otherwise, an extension the literal writes) and listed for the host before anything runs
  (`Program::files()`). It reaches only the folders its host grants (`Program::Options::files`: none by default), and
  the runtime decides from what it opened, not from the name's text: each directory opened beneath the last, no link
  followed, each compared by identity with the denied ones, the file a regular one within the size limit. A file is
  read whole into memory, never mapped, and every builtin that touches the disk is refused in a
  `[[clang::nonblocking]]` function; reading a handle is wait-free. Failures are statuses (`FILE_REFUSED`,
  `FILE_MISSING`, ...), never traps. Writing (`FMINUS_FILES_WRITE`) and absolute names (`FMINUS_FILES_ANYWHERE`) are off
  by default, and a build can limit the types and the size of any file it touches, which no host can raise.
- **The heap's memory is aligned to 64 bytes and zeroed**, `malloc`'s too, as everything FMinus declares starts at 0;
  sizes are `int`s (C's are `size_t`s), so one block holds at most 2 GB, and `sizeof` of a larger type (a module of
  big arrays) is an error. A call that breaks a function's `[[clang::nonallocating]]` or `[[clang::nonblocking]]` is
  an error, where clang only warns (`-Wfunction-effects`). As with clang's `malloc`, LLVM may remove a block the
  program never uses, and a removed block does not fail: a test of it against null is then false.
- **The FFT is FMinus's own**, as C has none: builtins on PFFFT, which take sizes of 2, 3 and 5 only, multiples of 32
  points for a real plan and of 16 for a complex one (`fft_create` is null for any other). A plan is made and freed off
  the audio thread (the effects count `fft_create`, `fft_create_complex` and `fft_destroy` as allocations), and a
  transform allocates nothing. Bins are `float2`s, (re, im), and neither direction scales: inverse(forward(x)) is n
  times x, as in FFTW and PFFFT. A plan is opaque: a program hands it to the FFT's builtins, and reads or writes
  nothing through it.
- **`hash` and `randf` are FMinus's own**, as C has nothing like them (its `rand` keeps a state, and gives the C
  library's numbers): `hash(n)` mixes an `int` into a well-spread one (Chris Wellons's lowbias32 on its 32 bits, a
  bijection), and `randf(n)` is the hash's top 24 bits as a float uniform in [-1, 1) (k / 2^23, k from -2^23 to
  2^23 - 1). Each is a few integer instructions inline, with no state: the same `n` gives the same bits wherever it is
  computed, in every lane of a vectorised loop, at every level, in every math mode and on every machine, so white noise
  is `out[i] = randf(start + i)`, about three instructions a sample on arm64. A program declares no function of
  either name ([docs/extending.md](docs/extending.md), recipe 25).
- **Memory is read and written as the type it holds**, C's strict-aliasing rule, which FMinus tells LLVM as clang
  does (TBAA): an `int` read as a `float` through a `void*` is undefined. FMinus is stricter in three places: a float
  vector or `poly float` is its elements' type, `float`; allocated memory keeps one type at each place until it is
  freed (C lets a store give it a new one); and FMinus's `char` is a type of its own, which aliases nothing else, so
  only a module's copy and a declaration's zeroing read or write bytes of any type (and `string_byte`, which reads what
  a string views, whatever it is). `--no-strict-aliasing` or `Program::Options::strictAliasing = false` turns
  the rule off, as clang's `-fno-strict-aliasing` does.
- **`sinf`, `cosf`, `tanf`, `expf`, `logf` and `tanhf` give a float one result wherever it is computed**: in a scalar
  loop or its vectorised body, in a vector of any width, at any optimisation level, from a constant or not, on any
  machine. C promises no such thing. By default they are FMinus's own kernels, one sequence of IEEE operations each,
  within 1.56 ulp of the exact result (tan 3.35, exp 1.05, log 0.99; libm's are within 1), inline from `-O1`;
  `fminus::math` computes the same in C++ ([docs/extending.md](docs/extending.md), recipes 23 and 24).
  `tanf` became a builtin with the kernels: a prototype of it, `float tanf(float x);`, which called libm's, now calls
  the builtin, as a prototype of any builtin does, and a program's own function named `tanf` is an error ("'tanf' is a
  builtin and cannot be redefined"): rename it. `Program::Options::math` (`fminus --math`) chooses `Libm`, libm's
  results bit for bit, or `Fast`, LLVM's intrinsics as clang compiles `<math.h>`. The runtime keeps three names for
  each of them (`fminus_rt_sinf`, `fminus_rt_sin_f4` and `fminus_rt_libm_sinf` for `sinf`): a program that calls one
  may not declare a function of any of its names, in any math mode. The call is then an error, with a note at the
  declaration.
- **A double's math is C's double functions that are instructions**: `sqrt`, `fabs`, `floor`, `fmin`, `fmax` and
  `fma`, exact (one rounding each, `fma`'s too), the same in every math mode, and on doubles only (a vector argument is
  an error). FMinus has no double `sin`, `cos`, `tan`, `exp`, `log` or `tanh`: its kernels are a float's, and `sinf(d)`
  rounds the double to a float first, as C's prototype converts it. A program that wants the C library's declares it,
  `double sin(double x);`, as any external function, off the audio thread's guarantees (the error for a call without
  the prototype says so). `sqrt` and the other five became builtins with the type, as `tanf` did: a program's own
  function of one of their names is an error, and a prototype of one that is not C's (`float sqrt(float x);`)
  conflicts.
- **Subnormals are C's unless the host asks for flush-to-zero** (`Program::Options::denormals`,
  `fminus --denormals=flush`): then a subnormal input reads as 0 and a subnormal result is 0, each with its sign, a
  float's and a double's, in folding as at run time (but for a float/double conversion of a subnormal that only
  LLVM's optimiser finds constant, which it folds as IEEE's: docs/extending.md, recipe 33), and each entry point (a
  `[[clang::nonblocking]]` function the program does not call) sets the mode on entry and restores the caller's on
  return. C's floats underflow gradually; clang's `-fdenormal-fp-math=preserve-sign` is the same choice.
- **Vectors are OpenCL C's**, which C has none of: `float2`, `float4`, `float8`, their literals and swizzles, and math
  functions that take them. A comparison of vectors gives a lane mask (OpenCL C gives 0 or -1 in an `int` vector), and
  `?:` with a vector condition picks each lane, so both sides run. Literals come in OpenCL C's form,
  `(float4)(a, b, c, d)`, and in C's, `(float4){a, b, c, d}` (with clang's `ext_vector_type`). They differ: C reads
  `(float4)(...)` as a cast of a comma expression. `poly float`, a vector as wide as the machine's, is FMinus's own
  ([docs/simd.md](docs/simd.md)). A double is no lane: a double meeting a vector is clang's error for the truncation
  (`v * d`, `float4 v = d;`), there is no `double4` or `poly double`, and a vector literal's double rounds to its lane's
  float, as the literal's type says (`(float4)(d)`).
- **Threads are FMinus's own, and structured**, as OpenMP's `parallel for` and `sections` are, where C has a library
  (C11's `<threads.h>`: threads a program makes, joins and locks around). `parallel for (int i = first; i < end; ++i) s`
  runs its iterations at once, spread over the machine's cores, and `parallel { a = f(x); b = g(y); }` its statements,
  each a task; either has finished, every iteration or statement, where it ends. A loop counts up by one from an `int`
  it declares (`i < end` or `i <= end`; `++i`, `i++` or `i += 1`), and its bounds are computed once, before it starts,
  where C tests a `for`'s condition before each iteration. The compiler refuses what it can see would race: an iteration
  that assigns a variable from outside the loop (every iteration would), assigns its own loop variable, or calls a
  method that changes its module on one; two statements of a block of which one changes a variable that the other uses;
  `break` and `return` out of either, and a declaration as a block's statement. What an iteration reaches through an
  index or a pointer (`out[i]`, `voices[v].render(n)`) is its own to keep apart from the others', as with OpenMP: the
  compiler sees which variable, not which element. There are no reductions: an iteration writes its own element, and the
  code after the loop combines them. A program without races computes what it would on one thread, bit for bit. A
  program's regions run on one pool for the process, a thread for each core but one, which the first `Program` with a
  region starts when it compiles (the calling thread works too); a region allocates nothing and takes no lock, so a
  `[[clang::nonblocking]]` `process` may have one, and every thread runs it in its caller's floating-point mode.
  `parallel` is a keyword: nothing can be named `parallel`; and a program with a region may not declare a function named
  `fminus_rt_parallel_for`, the runtime's ([docs/extending.md](docs/extending.md), recipe 22).
- **`%` works on floats**, with `fmodf`'s semantics: the result has the sign of the dividend, so `-7.5 % 2` is
  `-1.5`. C allows `%` on integers only; HLSL allows it on floats. `%=` goes with it.
- **A shift is defined for every count and value**, as OpenCL C defines it: `x << n` and `x >> n` shift by `n`'s low
  five bits (`n & 31`: `x << 32` is `x`, `x << -1` is `x << 31`), `<<` drops the bits shifted out of two's complement
  (`-1 << 1` is -2, `1 << 31` is the least `int`), and `>>` shifts the sign in (`-8 >> 1` is -4). C leaves a count
  below 0 or from 32 on undefined, and a left shift of a negative value or into the sign; a negative value's right
  shift is implementation-defined (clang's is arithmetic, as FMinus's). So a program means the same on every machine,
  which LLVM could not promise of a shift it may take for poison, and a scalar shift costs nothing for it (the shift
  instructions of arm64 and x86-64 take the count's low five bits themselves). `&`, `|`, `^` and `~` are C's; `+`,
  `-` and `*` on `int` still overflow as C's do, undefined. With no `unsigned`, a logical shift right is an arithmetic
  one and a mask: `(x >> 17) & 0x7FFF` ([docs/extending.md](docs/extending.md), recipe 25).
- **A function may be called before its definition** without a prototype, because the compiler declares every
  function first. A prototype with no definition declares an external C function, as in C: the JIT resolves it in
  the process (`float atanf(float x);` calls libm's), and the linker resolves it in an executable. On Apple systems,
  whose libm sets no `errno`, a prototype of libm's function says it writes no memory, as clang's `<math.h>` says
  there, so a call on a loop's invariants is made once, before the loop ([docs/extending.md](docs/extending.md),
  recipe 19).
- **Enumerations are C's, a little smaller**: `enum Wave` is `int` whatever its constants are (or C23's fixed type,
  `int` or `bool`), where GCC and clang make it an `unsigned int` when none is negative, and its constants are `int`s
  in an `int`'s range, where C23 lets a constant past it give the enumeration a wider type. A case's value or an
  enumerator's that overflows an `int` is an error, where clang warns. Messages name the type `'int'`, where clang
  names it `'enum Wave'` ([docs/extending.md](docs/extending.md), recipes 27 and 32).
- **A declaration without an initialiser starts at 0**, and a pointer at null, and so does a variable whose
  declaration a switch's jump to a label passes (`case 1:` after `case 0: float t = x;`). C leaves the variable
  indeterminate.
  From `-O1`, the optimiser removes a local's zeroing where nothing reads the zeros: an array's that loops overwrite,
  and a module's that a copy of another overwrites. `-O0` keeps every one ([docs/extending.md](docs/extending.md),
  recipe 18).
- **A non-void function that can reach its end is an error**, as in C++. Clang only warns in C. As in clang's check,
  a constant condition takes one branch: the end after a `while (1)` or `for (;;)` that only a `break` would leave, or
  after `if (1) return x;`, is not reached, and needs no return. A constant of the top level folds there too
  (`while (RUN)` with `const int RUN = 1;`), and a parameter never does, whatever the host binds it to, so a program
  compiles the same with a knob, a stream or a frozen value. clang also folds a block's `const` variable, `&&`, `||`
  and `?:` there; FMinus does not (`while (1 && 1)` asks for a return).
- **The preprocessor is `#include "file"`, the conditionals and macros that only they read.** A macro, the program's
  (`#define LEVEL 2`) or the host's, is read where C reads it as a condition, in `#if`'s and `#elif`'s expressions and
  by `#ifdef`, `#ifndef` and `defined`: its name in code is an error, never a substitution (for a constant, an `enum`
  or a `const`), so a program FMinus compiles means the same in C, and nothing is rewritten before the parser reads
  it. A function-like macro, a macro named as a keyword, `#error`, `#line` and `#include <...>` (FMinus has no system
  headers) are errors, and `#pragma` is ignored, as C ignores the pragmas it doesn't know. An `#if`'s arithmetic is
  64-bit and defined for every operand, as FMinus's code is (a shift takes its count's low six bits). Each file is
  read once, as if it began with `#pragma once`: two files that both include a third (a module both use) read it the
  first time only, where C would define everything in it twice. A directive is one line, which its `#` begins (a
  comment before the `#` is an error, where C reads the directive) and on which a `/*` comment in it ends, in a
  skipped group too. A directive's errors are clang's, `extra tokens at end of #endif directive` and a macro
  redefined with another value too, which clang only warns of ([docs/extending.md](docs/extending.md), recipes 21 and
  31).
- **Arithmetic is IEEE single precision**, with no contraction into fused multiply-adds (clang's
  `-ffp-contract=off`), and subnormals as IEEE has them unless the host flushes them (above).

- **`main` returns a `float` or a `bool` and takes floats.** C's returns an `int` and takes `argc` and `argv`. An
  FMinus executable reads `main`'s arguments from its command line and prints its result, a float or `true` or
  `false` ([docs/programs.md](docs/programs.md)).

Loops make progress as C11 says (6.8.5p6), not as C++ does: a `while` or `for` whose condition is not a constant
expression and which runs for ever without effects is undefined, so LLVM may delete such a loop whose results go
unused, while `while (1)` and `for (;;)` may run for ever. A function as a whole is not assumed to make progress, as a
C++ one is. A host that would rather a loop did not run for ever compiles with a loop budget
(`Program::Options::loopBudget`, `fminus --loop-budget n`): no change to the language, a limit on what a call takes.

An index outside its array is undefined, as in C. A host that would rather it stopped the call compiles with bounds
checks (`Program::Options::boundsCheck`, `fminus --bounds-check`), for a development build: every access whose array
the compiler sees (`a[i]`, `m.taps[i]`, `voices[v]`, `a + i`) is checked against the array's extent, and a call whose
check fails returns early, with where it failed recorded for the host. No change to the language either: a program
within its arrays computes the same with the checks and without.

A v0 program with `f` suffixes (on the floating constants of each arithmetic that has no double: one that has one
means C's double without it), with prototypes for calls ahead of a definition, with vectors only in C's form (braces,
given clang's `ext_vector_type`), and without `%` on floats, `poly`, modules, `parallel`, `main`, FMinus's own builtins
(`hash`, `randf`, the FFT), a shift that C leaves undefined or a negative value in a variable of an enumeration with no
negative constant (which GCC and clang make unsigned), is therefore also a valid C program with the same meaning, given
`<math.h>` (`<stdlib.h>` for the heap; C23 for the attributes and a declaration after a label) and `-ffp-contract=off`.
The example above and `examples/delay/delay.fm` compile as C under `-Wall -Wextra -Werror`, the latter with clang's
`-Wfunction-effects` too. One with modules is C++ instead, spelled `struct` for `module` and `__restrict` for
`restrict`, as long as no module has parameters, which are FMinus's own: `examples/voices/voices.fm`'s modules but
`Synth` compile so, with `-Wno-perf-constraint-implies-noexcept` (C++ wants a nonblocking function `noexcept` too). The
other examples use modules with parameters, `poly`, `parallel`, FMinus's own builtins (the convolver's FFT, the noise's
`randf`) or a `main`; hello's one function is C.

Every FMinus function becomes an external LLVM function of the same name with the C calling convention (a `float` is
LLVM's `float`, a `double` its `double`, an `int` an `i32`, a pointer a `ptr`), so C++ calls JIT'd code through a
pointer of the matching type: `float(float, ...)`, or `void(const float*, float*, int)` for a function that processes a
block of samples in one call. A method is one too, named `OnePole::tick`, whose first parameter is the module's address:
`float(void*, float)`. A module is an LLVM struct of its fields, laid out as C lays out a struct. The code generator
reports every error it finds, in source order and in clang's words, as `file:line:column: error: message`, and each
once: an operand with an error of its own (an undeclared name) is no operand for an operator, a subscript, `*`, `&`, a
call or a conversion to complain about, as clang's recovery has it, so `*(p + y)` is the one error about `y`. The parser
stops at the first syntax error. FMinus has two of clang's warnings. `-Wswitch`: a `switch` without a `default` on a
value of an enumerated type, whose cases miss some of its constants (`warning: enumeration value 'NOISE' not handled in
switch [-Wswitch]`). A value that folds to a constant (a constant of the top level) gets none, as in clang, which says
"no case matching constant switch condition" there instead; FMinus does not. And `-Wunknown-attributes`: a field's or a
parameter's attribute without a namespace that C23 does not have, which FMinus ignores (`warning: unknown attribute
'range' ignored [-Wunknown-attributes]`). The program still compiles; `fminus` prints the warnings with the errors, in
source order, as clang does.

```bash
build-dev/bin/fminus chain.fm                      # the optimised IR (-O2)
build-dev/bin/fminus chain.fm -O0                  # the IR as the code generator wrote it
build-dev/bin/fminus chain.fm --emit ast           # the syntax tree, as s-expressions
build-dev/bin/fminus chain.fm --emit asm -o -      # this machine's assembly (--emit obj: an object file)
build-dev/bin/fminus chain.fm --run chain 1        # JIT-compiles, calls chain(1), prints 0.33333334
build-dev/bin/fminus clock.fm --run seconds 48000  # a function of doubles: --run passes and prints them
build-dev/bin/fminus mix.fm --run blend 0.5 3 true # bools, ints, floats and doubles, in any mix and number
build-dev/bin/fminus dsp.fm --run scale 1,-2,0.5 3 2   # an array for a pointer, printed after the call: x = {2, -4, 1}
build-dev/bin/fminus note.fm --emit exe -o note    # an executable that runs main: ./note 69 prints 440
build-dev/bin/fminus unison.fm --poly-lanes 16     # compile as for a machine with 16-float vectors (AVX-512)
build-dev/bin/fminus lp.fm --stream Lowpass::cutoff --freeze Lowpass::mode=2   # parameters: a stream, a constant
build-dev/bin/fminus lp.fm --knob-versions 0       # no copies of a method for its knobs' values (8 at most by default)
build-dev/bin/fminus synth.fm --math libm          # sinf, cosf, ... as libm's (vector, the default; fast: LLVM's own)
build-dev/bin/fminus lp.fm --denormals=flush       # subnormals flushed to zero: folded so, and set by entry points
build-dev/bin/fminus lp.fm --no-strict-aliasing    # no TBAA: for a program that reads memory as another type
build-dev/bin/fminus synth.fm -I lib               # #include "name": beside the including file, then in lib/
build-dev/bin/fminus synth.fm -D DEBUG -D LEVEL=2  # macros for #if and #ifdef, as clang's -D (-D DEBUG is 1)
build-dev/bin/fminus voices.fm --threads 4         # a parallel region on 4 threads at most (0: every core; 1: alone)
build-dev/bin/fminus osc.fm --run f 1 --loop-budget 1000000   # the loop budget: a call that would loop for ever
                                                   # returns early (status 3); stderr says what it took
build-dev/bin/fminus taps.fm --run tap 20 --bounds-check   # bounds checks: an index outside its array stops the
                                                   # call (status 4); stderr says where, the array and the index
build-dev/bin/fminus pedal.cir --emit fm           # the FMinus module a circuit netlist generates (docs/circuits.md)
build-dev/bin/fminus pedal.cir --self-test --emit fm   # the self-test chooses the netlist's open configuration first
                                                   # (--threads its threads), and prints a line of directives to paste
                                                   # and its warnings (a block that rings at Nyquist)
build-dev/bin/fminus pedal.cir --notes -o /dev/null    # the circuit compiler's plan: blocks, rates, solvers or linear
                                                   # forms (matched, Woodbury), the method check, latency
build-dev/bin/fminus pedal.cir --probes --nodes --emit fm   # the module with its probe fields, and its nodes as a
                                                   # probe sees them (rate, DC volts, readable or why not)
```

How it fits together: `frontend::parse` (Lexer.l, then Parser.y) gives an `ast::Program`, and `ir::lower` (in
`src/ir/`, where CodeGen.cpp runs the parts that `Lowering.h` lists) gives an `llvm::Module`. From there, `jit::Jit`
optimises and compiles it in memory, or `support/Aot.h` writes it to a file.

**Adding a construct**, say comparisons and `?:`. [docs/extending.md](docs/extending.md) works through this one and
the others it builds on, all the way to C's `if`, `while` and `for`, and [docs/simd.md](docs/simd.md) through vector
types:

1. Its tokens: a rule in `src/frontend/Lexer.l` (keywords before `{IDENTIFIER}`), a `%token` in `Parser.y`.
2. Its grammar: a rule in `Parser.y`, with C's precedence if it is an operator. Bison's report is
   `build-dev/src/generated/frontend/Parser.output`, and `-Wcounterexamples` shows an input for each conflict.
3. Its node: an alternative of a `std::variant` in `src/frontend/Ast.h`, or an enumerator. Every `std::visit` and
   every `switch` over it now fails to compile until it handles the new node: `dump` in `Ast.cpp`, and the lowering
   in `src/ir/`, in the file its kind belongs to (statements in `Statements.cpp`, vectors in `Vectors.cpp`, ...;
   `Lowering.h` has the list).
4. Its tests: a dump in `tests/unit/FrontendTest.cpp`, a result in the code generator's tests (`CodeGenTest.cpp`,
   `ControlFlowTest.cpp`, `SwitchTest.cpp`, `EnumTest.cpp`, `IntTest.cpp`, `BitwiseTest.cpp`, `PointerTest.cpp`,
   `HeapTest.cpp`, `EffectTest.cpp`, `ArrayTest.cpp`, `TableTest.cpp`, `TopLevelTest.cpp`, `ModuleTest.cpp`,
   `FftTest.cpp`, `HashTest.cpp`, `VectorTest.cpp`, `PolyTest.cpp`, `ParallelTest.cpp`, `DoubleTest.cpp`,
   `Array2DTest.cpp`, `FieldAttributeTest.cpp`, `StringTest.cpp`, `BytesTest.cpp`, `JsonTest.cpp`, `XmlTest.cpp`,
   which share `Compile.h`), IR or errors in `tests/lit/fminus/*.fm`
   (`// RUN:` lines, checked by FileCheck).

`examples/biquad/` is a kernel to grow the language around. It is a biquad low-pass module with coefficients recomputed
every sample, its cutoff a stream, which filters a block a call, with a harness that calls it once, compares it with C++
and times both, and a plan of features in its README. `examples/delay/` is a feedback delay whose line the program
allocates, with a kernel marked `[[clang::nonblocking]]`, written as plain C and as a module. `examples/saturator/`
(tanh) and `examples/meter/` (peak and RMS) each run a loop over samples against a loop over `poly float`s, and time
both. `examples/voices/` is a synth of eight voices in modules, its controls knobs, which a host of the public API
allocates and runs. `examples/convolver/` convolves with an impulse response by FFT. `examples/ensemble/` renders
sixteen voices at once with a `parallel for`, and checks that every core gives what one thread does, bit for bit.
`examples/parameters/` sweeps a filter's cutoff with an LFO, through a knob once a call or a stream a sample, and
freezes a fixed one. `examples/wavetable/` loads a wavetable pack from a JSON or an XML document its host hands it, off
the audio thread, and plays it on the audio thread.

`examples/plugin/` is FMinus inside a plugin: FMinus Filter, a JUCE effect (AU, VST3 and Standalone on macOS) whose
DSP, a state-variable filter, is an FMinus program that its processor compiles in `prepareToPlay`, with the cutoff a
stream from a smoother, resonance and mode knobs (the program crossfades a new mode itself), and the sample rate frozen
at the host's. The processor refuses a `process` that isn't `[[clang::nonblocking]]`, clears the filter on a host's
`reset` through an atomic flag, and each bundle exports only its entry points, LLVM's symbols kept inside. It is a
CMake project of its own (`cmake -S examples/plugin -B build-plugin`), on JUCE 8.0.4 (pinned by commit in
`cmake/FMinusPins.cmake`) and the LLVM that FMinus's own build installed in `.deps/`, with a headless check,
`fminus-plugin-check`, which its README says how to run.

Each variable is a stack slot (an `alloca` in the entry block), read with loads and written with stores, so
`fminus -O0` reads like `clang -O0`. From `-O1`, mem2reg promotes the slots back into registers.

## Using FMinus as a library

`include/fminus/FMinus.h` is FMinus's C++ API, and the one header a host includes. A `fminus::Program` compiles FMinus
source while the host runs, for the CPU it runs on, and gives its functions as C function pointers
(`examples/hello/main.cpp` is a whole program that does this and no more):

```cpp
#include "fminus/FMinus.h"

std::string errors;
auto program = fminus::Program::compile(source, errors);     // nullptr when it does not compile
if (!program)
    return report(errors);                                    // "program.fm:3:12: error: ...", as clang words it
auto* gain = program->function<void(const float*, float*, int, float)>("gain", errors);
gain(in, out, n, 0.5f);                                       // one call a block: the loop is FMinus's
```

- **Types.** A function's C++ type is its FMinus declaration's C type: `float`, `int`, `bool`, `void`, pointers, and
  `float2` and `float4` as clang's `ext_vector_type`. The C ABI passes a `float8`, and a `poly float` of 8 lanes or
  more, through memory, and FMinus in registers, so keep those inside FMinus. Nothing is checked at the boundary: a
  signature that does not match the declaration is undefined behaviour (a host that learns a signature only as it
  runs calls by name instead, below). The header has the table.
- **Options.** `Program::Options` sets the optimisation level (`-O2` by default, as for `fminus`), a `poly float`'s
  lanes (this machine's by default, which `polyLanes()` reports), and the file name the errors give. `include` is
  where `#include "name"` finds its file: a function from the name and the including file's name to the file's name
  and text, so a host keeps its files where it likes, on disk or in memory (with none, the default, no `#include`
  finds a file). `math` is how `sinf` and the other transcendental builtins compute (`Math::Vector` by default,
  `Libm` or `Fast`), `strictAliasing` whether the optimiser relies on C's strict-aliasing rule (`true`),
  `knobVersions` how many copies of a method knob versioning may make (8; 0 turns it off), `denormals` whether
  subnormals are kept or flushed to zero (`Denormals::Preserve` or `Flush`), `threads` the most threads a parallel
  region runs on (0, the default, is every core; 1 runs each on the calling thread alone), `keepIr` which of the
  program's LLVM IR to keep as text (below), `loopBudget` whether its loops count against a budget (`false`;
  below), `boundsCheck` whether its accesses are checked against their arrays (`false`; below), and `defines` the
  macros for its `#if`s (below). The header says what each costs.
- **Threads.** `compile()` and the first look-up of each function run LLVM's optimiser and code generator on the
  calling thread: they take milliseconds and allocate. Different programs may compile on different threads at once.
  In a plugin, compile off the audio thread and hand the pointer over (below). A program's `parallel for` and
  `parallel { }` run on FMinus's pool, shared by every `Program` in the process: the first with a region starts it
  when it compiles (a thread for each core but one) and the last one's destructor stops it. The thread that calls the
  program works on each region too.
- **Warnings.** `warnings()` is a compiled program's warnings, as clang prints them, one a line with their notes
  (empty when it has none): clang's `-Wswitch`, a `switch` on a value of an enumerated type that misses some of its
  constants, and `-Wunknown-attributes`, an attribute FMinus ignores (below). A program that does not compile has its
  warnings among its `errors`, where they are.
- **Effects.** `nonBlocking(name)` and `nonAllocating(name)` say whether a function is marked
  `[[clang::nonblocking]]` or `[[clang::nonallocating]]`, which FMinus has then checked, so a host can refuse to run
  anything else on its audio thread.
- **Modules.** `module(name)` describes one: its size and alignment, what a host allocates for an instance (zeroed:
  every field starts at 0, and nothing else runs), and each field's name, type and offset, which a C++ struct with the
  same fields in the same order shares. A field's type is FMinus's spelling of it: `"double"` and `"double[8]"` are 8
  bytes an element, C++'s `double`, so a host that reads fields (a trace, an inspector) reads one as a double, never as
  a float. An array's shape is spelled out too: its `element` type and its `extents`, outermost first, `{4, 16}` for a
  `"float[4][16]"`, whose rows lie side by side (C++'s `float[4][16]` at the field's offset), and its `attributes`
  (below). A method is a function whose first parameter is the instance:
  `function<float(void*, float)>("OnePole::tick", errors)`, and `function<double(double)>("seconds", errors)` for one of
  doubles. `examples/voices/` is such a host.
- **Parameters.** `Options::parameters` binds each parameter by name, `"Lowpass::cutoff"`: `Binding::knob()` (the
  default), `stream()` or `frozen(value)`. `parameter(name)` and `Module::parameters` describe each: its type, binding,
  range and default, and where its slot is in an instance. `setValue` sets a knob from any thread (a relaxed atomic
  store, which a method reads when it next starts), `setStream` points a stream at the call's samples (or, null, makes
  the call flat: every sample is then the value `setValue` set), and `setDefaults` sets each parameter's default.
  `examples/parameters/` is such a host.
- **Attributes.** A field says how a host shows it with attributes after its name, C23's way:
  `float level [[ui::unit("dB"), ui::label("Level")]] [[fminus::range(-60, 0)]];`. `Module::Field::attributes` lists
  them in order, a parameter's too (and its `Parameter::attributes`), each a `Program::Attribute`: its `name` as written
  (`"ui::unit"`) and its `arguments`, each a number (`number`, and `text` as written, `"-60"`) or a string (`isString`,
  `text` its bytes, C's escapes resolved, UTF-8 as the source has it). `findAttribute(attributes, "ui::unit")` is the
  last of a name, or null. FMinus gives meaning to its own, `fminus::range` (a parameter's or a field's of numbers),
  `fminus::default` (a parameter's, or a field's of numbers or `bool`s, for `json_read`), `json::name` and `json::skip`,
  and passes them and every other on, the others unread: a unit, a label, a display hint, whatever the
  host defines. Name a host's in a namespace of its own (`ui::`), as C23 keeps names without one for its standard
  attributes: one without a namespace that is none of those (`[[range(0, 1)]]`) is a warning in `warnings()`, and in
  no `attributes` list. `examples/meter/` labels its readings so ([docs/extending.md](docs/extending.md), recipe 35).
- **Math.** `fminus::math::sin`, `cos`, `exp`, `tanh` and `log` compute what a program in the default math mode does,
  bit for bit, so a C++ reference that calls them agrees with the program at any block size, level and width. They
  are `[[gnu::const]]` for GCC and clang (`FMINUS_MATH_CONST`), so two calls of one argument are one.
- **IR.** For a host that shows what FMinus made of a program, `Options::keepIr` keeps its LLVM IR as text:
  `KeepIr::Lowered`, `Optimised` or `Both` (`None`, the default, prints and holds nothing). `loweredIr()` is the IR as
  the code generator wrote it, set up for this machine, before any pass; `optimisedIr()` is what the JIT generates the
  machine code from, after the pipeline at `optimisation`, and the same text as `fminus --emit ll` prints for the same
  source and options, the file named as `fileName`. Each is empty when not kept. To print the optimised IR,
  `compile()` runs the optimiser itself, and the first look-up then only the code generator.
- **Loop budget.** A program compiled with `Options::loopBudget` counts its loops' iterations and its recursive calls
  against a budget the host sets before each call, `setLoopBudget(n)`, and a call that would loop for ever, or far
  longer than it should, returns early, 0, with every function of the program that called it: `loopBudgetExceeded()`
  says so, and `loopBudgetLeft()` what is left (-1 after a call that ran out; a negative budget is -1). A loop whose
  count is known as it starts is charged it there, any other one an iteration. The checks go in after the optimiser, so
  within its budget a call computes what it would without it, bit for bit, and at the same speed
  ([docs/extending.md](docs/extending.md), recipe 29, has the measurements). A call that ran out leaves the instance
  as its optimised code had it, which need not be any point of the source: re-initialise the instance (or discard it)
  before running the program again. Off by default; unlimited until the host sets it.
- **Bounds checks.** A program compiled with `Options::boundsCheck`, a development build, checks every access whose
  array the compiler sees against the array's extent (an element must be one of its own, a pointer formed from it may
  also point one past its end), and a call whose check fails returns early, 0, with every function of the program that
  called it, before the access: `boundsViolation()` is the first such check since `clearBoundsViolation()`, a
  `BoundsViolation` with the file, line and column, the array as the source names it, its type and extent, and the
  index (`message()`: `echo.fm:14:33: index -188 out of bounds for 'line' of type 'float[512]'`). A pointer the
  program keeps (a variable's, a parameter's: a heap block, a host's buffer) is not checked, nor are an FFT's buffers
  against its plan's size. While the record holds a check, each call of the program returns early, so clear it before
  calling the program again, `init` included. Within its arrays a call computes what it would without the checks, bit
  for bit; a check costs a compare and a branch where the optimiser cannot prove it, and a loop that updates a field
  may then store it every iteration (the measured ones a third to two fifths slower: recipe 30). Off by default, and
  without it the code is the same, byte for byte.
- **Macros.** `Options::defines` defines macros before the program's first line, as C's `-D` does: each name's value,
  `"1"` for a flag as `-D DEBUG` gives it, `""` for none. A program reads them in `#if`, `#elif`, `#ifdef` and
  `#ifndef`, so a development build compiles in the instrumentation that sits in `#ifdef DEBUG` groups (a field, a
  statement, a method, an `#include`), and the build that plays skips those lines unread: they cost nothing, there or
  in the compile, and a module's layout may differ between the two (read `module()` for each). A program's
  `#ifndef LEVEL` / `#define LEVEL 2` / `#endif` is a default the host's value overrides. A name that is no
  identifier, `defined` or a keyword fails `compile()`, and so does a host's macro whose name the program uses in code:
  name them as C names its own (`DEBUG`, `HOST_VERSION`). A host that reads the program's text itself (an editor's
  language service, or its own `#pragma` lines) sees every directive as a line that starts with `#`, and the lines of
  the groups FMinus skips too: it reads both branches, or skips the groups its own defines skip
  ([docs/extending.md](docs/extending.md), recipe 31).
- **Calls by name.** A host that learns a function's signature only as it runs (a console that calls the function
  its user names, a test runner, a tool that tries a program's function) calls it by name: `callable(name, errors)`
  gives a `Callable`, its parameters and result as values (each parameter's name, its type as FMinus writes it, its
  `kind`, and for a pointer what it points to), and `call(callable, {Value::of(0.5f), Value::of(3)})` checks the
  arguments against them and calls the function on the calling thread, returning a `CallResult`: a `status`, the
  `value` it returned, and `message()`, one line in FMinus's words. A wrong name, a type `call()` does not pass, a
  function whose code does not link, the wrong number of arguments, an argument of another type (it converts nothing:
  `Value::scalar(kind, number)` does, as `setValue` would), a `const` array for a pointer the function writes
  through, a null or misaligned instance, or another program's: each is a status and a message, never undefined
  behaviour. It passes `bool`, `int`, `float` and `double`, and pointers to the host's memory (`Value::array(buffer)`,
  `Value::instance(memory, module)` for a method's `this` or an `Osc*`, with the program's own `Module`), and returns
  a scalar or nothing. The first `callable()` of a function compiles a thunk, a few instructions that load each
  argument as its parameter's type and call the function as C does (about 2 ms; it allocates, so off the audio
  thread); a `call()` then adds about 15 ns to the function, and no lock. The loop budget and bounds checks stop a
  call as any, and say so (`OutOfBudget`, `OutOfBounds`) without allocating, so on the audio thread too: set the
  budget and clear the record before each call, and serialise calls from several threads, as the budget and the
  record are the program's ([docs/extending.md](docs/extending.md), recipe 36).
- **Profiling.** A host's sampling profiler, which interrupts the thread that runs the program now and then and reads
  its program counter, maps each sample to a function with `codeRanges(errors)`: every range of the program's machine
  code, sorted by address, each a `CodeRange` with its `address` and `size`, the `symbol` it has in the object, the
  program's `function` it is the code of (`"Osc::tick"`) and that function's `module` (`"Osc"`), its `kind` (a
  function's code, an entry point's or a parallel region's, a thunk, or FMinus's own), and the functions the optimiser
  inlined into it (`inlined`), whose samples are its. `codeRangeAt(ranges, pc)` finds a sample's range (a binary
  search: no allocation, no lock). `codeRanges` links the program if no function has been looked up yet, so after
  `compile()` the list is whole, and the code stays where it says while the program lives: take it once, off the audio
  thread. Code that is not the program's, the runtime's, the C library's or the host's, is in no range
  ([docs/extending.md](docs/extending.md), recipe 37, and a reference sampler in `tests/library/ProfileTest.cpp`).
- **The web.** In a build with `FMINUS_WEB`, `Options::web` is what the program may reach through `http_get` and
  `http_post`: URL patterns, a body limit, a timeout, a redirect limit, a CA file; none by default, so every request is
  `HTTP_REFUSED`. `webRequests()` lists every URL the program names, with its method and where it is, before anything
  runs; `stopWeb()` cancels what is in flight and wakes a thread in `http_wait`; the destructor frees every request
  ([docs/web.md](docs/web.md)). With `FMINUS_WEB_SERVER` too, `Options::web.server` is where the program's
  `http_listen` may listen (`"localhost:0"`, ...; nowhere by default), with a token, the names a `Host` may say, the
  static files the library serves for it (a page), and its limits; `webRoutes()` lists every route the program names,
  before anything runs, a route statement's with its parameters (name, type, path or query, range, default); `webPort()` is the port it listens on; and `stopWeb()` and the destructor close its server,
  waking a thread in `http_accept`.
- **Files.** In a build with `FMINUS_FILES` (the default), `Options::files` is what the program may reach through
  `file_read`, `file_list`, `file_write` and `file_append`: its folders (a name, a directory, whether it may be written,
  its types), a deny-list compared by identity, the types allowed, absolute names (`anywhere`, with
  `FMINUS_FILES_ANYWHERE`), one hard link only, and the limits and quotas; none by default, so every name is
  `FILE_REFUSED`. `compile()` checks it against the build's ceilings every time and opens the folders' directories once,
  when the program names a file; `files()` lists every name the program uses, with its operation and where it is,
  before anything runs; the destructor frees every file the program still holds and closes the folders
  ([docs/files.md](docs/files.md)).
- **Lifetime.** A function pointer, and a `Callable`, is valid while its `Program` lives, and the code stays where
  `codeRanges()` says. A `Callable` is the program's own: the host keeps the pointer `callable()` gives, as it keeps a
  function's, never a copy.

`include/fminus/Circuit.h` is the circuit compiler's: `fminus::circuit::generate(netlist, "pedal.cir")` turns a
SPICE-style netlist into the text of one FMinus module, which `Program::compile` compiles like any other source, its
errors in clang's words as FMinus's are; `set()` takes the pots, then the ramped switches' positions
(`Result::switches`). A host that serves `#include` generates in its resolver for a `.cir` name
([docs/circuits.md](docs/circuits.md) has the lambda). A netlist that leaves its configuration open plays a
provisional one until `fminus::circuit::selfTest`, which takes seconds and runs on the host's threads, answers; the
host stores the answer (`Choice::text()`) under the netlist's key and generates again with it. It is C++17 with no
LLVM too, and comes with `FMinus::FMinus` (its library, `libfminus_circuit.a`, links into `libfminus.a`'s users; the
self-test's JIT side is in `libfminus.a`).

The header has no LLVM in it and needs C++17. In CMake, a host links `FMinus::FMinus`, and gets FMinus's include
directories and libraries (LLVM's too, linked statically, and the FFT inside the runtime's), but neither LLVM's headers
nor FMinus's C++23. As a
subproject, FMinus builds its library and runtime only (its tests, tools and examples are on only at the top level):

```cmake
include(FetchContent)
FetchContent_Declare(FMinus GIT_REPOSITORY https://github.com/Snipet/FMinus.git GIT_TAG <commit> SYSTEM)
set(FMINUS_WERROR OFF)               # warnings in FMinus's sources are not errors in the host's build
FetchContent_MakeAvailable(FMinus)
target_link_libraries(my_plugin PRIVATE FMinus::FMinus)
```

A subproject gets its LLVM as FMinus's own build does (Dependencies, above): by default it builds a flavor into its
deps cache. To reuse one an FMinus checkout has built, set `FMINUS_LLVM_MODE` to `system`, `LLVM_DIR` to
`<checkout>/.deps/install/llvm-<version>-<flavor>/lib/cmake/llvm` and `FMINUS_DEPS_DIR` to `<checkout>/.deps` (for its
Bison and Flex) before `FetchContent_MakeAvailable`. Calling into FMinus's internals instead (`"frontend/Frontend.h"`,
`"jit/Jit.h"`, ...) takes `FMinus::Internal`, which adds `src/`, LLVM's headers and C++23.

## Using FMinus from Python

`import fminus` is the same library as a Python package ([docs/python.md](docs/python.md) is its manual). It is off by
default: the `python` preset builds and tests it (it is `dev` with `FMINUS_BUILD_PYTHON=ON`, and needs Python 3.9 or
later with its development headers), and `pip install .` installs it.

```bash
cmake --workflow --preset python                                    # build-python/python/fminus/
PYTHONPATH=build-python/python python3 python/examples/sweep.py
CC=clang CXX=clang++ pip install .                                  # or: into this Python
```

```python
import array
import fminus

program = fminus.load("examples/parameters/parameters.fm")         # or fminus.compile(source)
filter_ = program.module("Filter").instance()                      # zeroed, its parameters at their defaults
filter_["cutoff"] = 440                                            # a knob
samples = array.array("f", [1.0] * 256)                            # or a NumPy float32 array
out = array.array("f", bytes(4 * 256))
filter_.process(samples, out, len(samples))                        # one call a block: the loop is FMinus's
```

A program that does not compile raises `fminus.CompileError`, with FMinus's errors as clang words them. A program's
functions are its attributes and a module's methods its instances', each called through `Program::call()`, so every
argument is checked against the declaration, and a wrong one is a `TypeError`: numbers convert to the parameter's type,
and a pointer takes a buffer of its element type (an `array.array`, a NumPy array, a `memoryview`), lent for the call
and never copied. `Program::Options` are `compile()`'s and `load()`'s keyword arguments (`optimisation=3`,
`parameters={"Filter::cutoff": "stream"}`, `defines={"DEBUG": True}`, `bounds_check=True`, ...), and a call that the
loop budget or a bounds check stops raises `fminus.LoopBudgetError` or `fminus.BoundsError`. The GIL is released while
a function runs. A pointer has no extent, as in C: a length longer than its buffer reads or writes past it. The circuit
compiler and vectors at the boundary are not bound yet (the manual lists what is not there).

The Wheels workflow (`.github/workflows/wheels.yml`, run by hand) builds the package's wheels, one for each CPython on
Linux, macOS and Windows, with its source distribution, and uploads them to PyPI, so that `pip install fminus` builds
nothing ([docs/python.md](docs/python.md#wheels-and-pypi) has the one-time setup on PyPI and the steps of a release).

## Executables (ahead of time)

For development, a module can also be compiled ahead of time for this machine, into a native executable you can run
under lldb or Instruments, or into an object or assembly file to read. Its machine code is what LLJIT generates for the
same module, since both use the TargetMachine and pipeline in `support/HostTarget.h`.

An FMinus program with a `float main(...)` builds straight into one (or a `double` or `bool` one, whose parameters are
floats and doubles). `fminus::aot::addEntryPoint` renames the program's `main` to `fminus.main` and adds the C `main` an
executable starts in, which reads the arguments as their types say and prints the result
([docs/programs.md](docs/programs.md)):

```bash
build-dev/bin/fminus note.fm --emit exe -o note             # then ./note 69
build-dev/bin/fminus note.fm --entry                        # the IR with that C main (--emit asm, obj too)
build-dev/bin/fminus osc.fm --emit exe --loop-budget 1000000 -o osc   # main stops past the budget, and exits with 3
build-dev/bin/fminus taps.fm --emit exe --bounds-check -o taps         # main stops at a failed check, exits with 4
```

`fminus-build` builds LLVM IR written by hand, which brings its own C `main`:

```bash
build-dev/bin/fminus-build kernel.ll -o kernel              # an executable (the IR needs a main)
build-dev/bin/fminus-build kernel.ll --emit asm -o -        # this machine's assembly, after -O3
build-dev/bin/fminus-build kernel.ll --emit ll -O2 -o -     # the optimised IR; --emit obj for an object file
build-dev/bin/fminus-build kernel.ll -o kernel --link-arg=-framework --link-arg=Accelerate
```

From code, it is `fminus::aot::emitFile`, `linkExecutable`, `emitExecutable` and `addEntryPoint` in `src/support/Aot.h`.
Linking goes through the C++ compiler driver this build tree was configured with (Xcode's clang; `$FMINUS_LINK_DRIVER`
overrides it), which brings the SDK, libSystem and the ad-hoc signature arm64 needs; on Windows, clang-cl, which
names an executable `note.exe` when asked for `note`. It links this tree's `libfminus_runtime.a`, so JIT'd and
ahead-of-time code call the same runtime. An executable whose program calls the web library links `libfminus_web.a`,
libcurl, Mbed TLS and the system libraries they need too (and one that does not, none of them), and reads the URLs it
may reach from `FMINUS_WEB_ALLOW` when it starts: none by default ([docs/web.md](docs/web.md)); one that calls
`http_listen` reads where it may listen from `FMINUS_WEB_LISTEN` (and with it `FMINUS_WEB_TOKEN`, `FMINUS_WEB_HOSTS`,
`FMINUS_WEB_ROOT`, a directory of static files, and `FMINUS_WEB_OPEN`): nowhere by default. One whose program names a
file reads its folders from `FMINUS_FILES_READ` and `FMINUS_FILES_WRITE` (and with them `FMINUS_FILES_DENY`,
`FMINUS_FILES_TYPES` and `FMINUS_FILES_ANYWHERE`) when it starts: none by default ([docs/files.md](docs/files.md)). In
the JIT,
`fminus::defineRuntimeSymbols` defines every function listed in `src/runtime/RuntimeSymbols.def` by its address (and,
with the web library, `src/web/WebSymbols.def`'s). Under the `asan` and `tsan` presets, the sanitizer runtimes are linked
too. This is a development feature: inside a plugin, FMinus only ever JITs.

## Tests

```bash
cmake --build --preset dev --target check-fminus    # build what the tests need, then ctest
ctest --preset dev -R jit                           # some of them
build-dev/bin/fminus-tests "[smoke]"                # Catch2 directly (tags, --benchmark-samples, ...)
build-dev/bin/fminus-lit -v tests/lit               # lit directly; a source path works as it is
```

- **Catch2** (`tests/unit/`, `tests/smoke/`): one executable, `fminus-tests`; ctest runs each `TEST_CASE` on its own.
  `tests/library/` is a second one, `fminus-library-tests`: the public API, built in C++17 against `FMinus::FMinus`
  alone, as a host builds.
  The smoke tests cover the JIT, an executable linked ahead of time and run, and the calculator grammar.
  Catch2's `BENCHMARK` is there for timing JIT'd kernels.
- **lit** (`tests/lit/`): a file with `RUN:` lines, checked by FileCheck. `tests/lit/fminus/` drives `fminus`. FMinus's
  tools and LLVM's are on PATH; `%host_triple` is substituted; the features for `REQUIRES:` are `asserts`, `asan`,
  `ubsan`, `tsan`, `arm64`, `x86_64`, `avx` (an x86-64 host whose JIT targets AVX), `system-darwin`,
  `system-linux`, `system-windows`, and `json`, `xml`, `web`, `web-server`, `files`, `files-write` and
  `files-anywhere` (a build with `FMINUS_JSON`, `FMINUS_XML`, `FMINUS_WEB`, `FMINUS_WEB_SERVER`, `FMINUS_FILES`,
  `FMINUS_FILES_WRITE` or `FMINUS_FILES_ANYWHERE`), and `files-types` (a build whose `FMINUS_FILES_TYPES` lists types,
  which `%files_types` substitutes). `.fm` is a placeholder extension for FMinus sources: change it in
  `tests/lit/lit.cfg.py`.

## Presets

| Preset | FMinus | LLVM | For |
|---|---|---|---|
| `dev` | RelWithDebInfo | Release + assertions | everyday work |
| `debug` | Debug | Release + assertions | stepping through FMinus |
| `release` | Release | Release | measuring |
| `asan` | Debug + ASan + UBSan | Release + assertions | memory errors in FMinus's code |
| `tsan` | RelWithDebInfo + TSan | Release + assertions | compiling on a background thread, the parallel regions' pool |
| `inline` | RelWithDebInfo | in-tree, -O2 -g, assertions | stepping into LLVM |
| `python` | RelWithDebInfo | Release + assertions | the Python package (`dev` with `FMINUS_BUILD_PYTHON=ON`) |
| `web` | RelWithDebInfo | Release + assertions | the web library and its server (`dev` with `FMINUS_WEB=ON` and `FMINUS_WEB_SERVER=ON`; builds libcurl and Mbed TLS into the deps cache, and fetches llhttp) |
| `files` | RelWithDebInfo | Release + assertions | the whole file interface (`dev` with `FMINUS_FILES_WRITE=ON` and `FMINUS_FILES_ANYWHERE=ON`; fetches nothing) |
| `xcode` | Xcode project | Release + assertions | Xcode's debugger and Instruments |

Every preset builds in `build-<preset>/` and exports `compile_commands.json`; `.clangd` reads `build-dev`'s. Each
preset has a workflow (`cmake --workflow --preset <name>`) except `xcode`, which is built from Xcode
(`cmake --preset xcode && open build-xcode/FMinus.xcodeproj`). ccache or sccache is used when installed. On Windows,
`asan`, `tsan` and `xcode` are not available.

## Handy

- `build-dev/bin/fminus-hostinfo [--features]`: the triple, CPU, features and data layout the JIT targets here.
- VSCode: `python3 editors/vscode/package.py`, then `code --install-extension build-vscode/fminus-0.22.0.vsix`, for
  FMinus syntax highlighting, lit's `RUN:` and `CHECK:` lines included
  ([editors/vscode/README.md](editors/vscode/README.md)).
- `build-dev/bin/fminus`: FMinus source to its tree, IR, assembly or an object, or run in the JIT (above).
- `build-dev/bin/fminus-build`: LLVM IR to an executable, object, assembly or optimised IR (above).
- `build-dev/bin/fminus examples/circuits/diodetone.cir --emit fm`: an analog circuit's netlist as the FMinus module the
  circuit compiler generates ([docs/circuits.md](docs/circuits.md)); `--self-test` lets the self-test choose what the
  netlist leaves open, `--notes` prints the plan, `--probes` adds the probe fields and `--nodes` lists the nodes a
  probe reads; `build-dev/bin/example-circuits --check` checks the research's examples against its targets, and the
  self-test against the configurations they ship.
- The LLVM tools of the flavor in use are in `.deps/install/llvm-…/bin`: `opt -passes='default<O3>' -S`,
  `llc -mcpu=native`, `llvm-mca` (throughput of a kernel's machine code), `llvm-objdump -d`.
- LLVM is built with `LLVM_ENABLE_DUMP=ON`: `value->dump()` and `module->dump()` work without assertions too.

## Notes for running inside a plugin

These are constraints a plugin runs into. `examples/plugin/`, a JUCE plugin, meets them, and its README says how:

- **Executable memory.** Under the hardened runtime, macOS needs an entitlement for memory written at run time and
  then executed: `com.apple.security.cs.allow-jit` (for `MAP_JIT`) or `allow-unsigned-executable-memory`. A plugin
  runs in its host's process and has only the host's entitlements, so check each target DAW early. Unsigned test
  executables (this repository's) are unaffected.
- **Real time.** LLJIT compiles lazily, on the thread that looks a symbol up. Compile and look up on a background
  thread (the plugin does both in `prepareToPlay`), hand the function pointer to the audio thread atomically, and keep
  JIT'd code free of allocation and locks: mark the functions the audio thread calls `[[clang::nonblocking]]`, and
  FMinus refuses any that could reach `malloc` or `free` ([docs/extending.md](docs/extending.md), recipe 11). A host
  that runs its audio thread with flush-to-zero can tell the compiler so (`Program::Options::denormals = Flush`): the
  code is then folded for that mode, and each entry point sets it if the caller has not
  ([docs/extending.md](docs/extending.md), recipe 20). The plugin does: its `processBlock` flushes (JUCE's
  `ScopedNoDenormals`), and it compiles with `Flush`, so its `process` finds the mode set and writes nothing.
- **Parallel regions on the audio thread.** A `parallel for` or `parallel { }` runs on FMinus's pool and the audio
  thread together, in the audio thread's floating-point mode, and allocates and locks nothing. The audio thread starts
  on its own share at once; of the pool threads that slept (no region for 50 µs), it wakes only as many as the region
  has seats for (a region of two iterations, one), a system call each that does not block; at the end it spins for the
  chunks other threads are still running. The pool's threads run at the
  user-interactive quality of service on macOS: they are not real-time threads, and join no audio workgroup, so the
  system may run them late, and the audio thread then takes their chunks itself. A region costs about 4 µs on an M5
  with every core (under 2 µs on two) against a plain call: worth it when its iterations take tens of microseconds
  together, a synth's voices, not a block's samples. A host whose other audio threads run regions too gets one region
  on the pool at a time; the others run on their own threads. `Program::Options::threads` limits or turns them off.
- **Loops that never end.** A program stuck in a loop holds the audio thread, and the host with it. Compile with
  `Program::Options::loopBudget`, set the budget before each call (a generous one: many times what a block takes), and
  after it check `loopBudgetExceeded()`: the call returned early, and the plugin can silence the program, report it,
  and re-initialise the instance (or discard it) before the next call. The budget is a count of iterations, not a clock
  ([docs/extending.md](docs/extending.md), recipe 29).
- **An index outside its array.** While a program is written, compile it with `Program::Options::boundsCheck`, and
  after each call check `boundsViolation()`: the call returned early, before the access, and the plugin can silence
  the program, show `message()` (off the audio thread: it allocates), then `clearBoundsViolation()`, and only then
  re-initialise the instance (or discard it): while the record holds the check, `init` would return partway, as any
  call would. The checks see only arrays the program declares: a delay line in a heap block (`calloc`'s) is not
  checked, and one in a field array is. Compile the build that plays without it, which is as fast as ever
  ([docs/extending.md](docs/extending.md), recipe 30).
- **The network.** In a build with `FMINUS_WEB`, a program's requests reach only what `Program::Options::web` allows:
  none by default (the library's policy, not a sandbox: [docs/web.md](docs/web.md)). A sandboxed host needs the
  network client entitlement itself; the library opens no listening socket unless the host gives the program's server
  a listen (`Options::web.server`), so by default there is no firewall prompt. A plugin that serves listens inside its
  host application's sandbox and under its firewall rules: macOS's App Sandbox wants the network server entitlement
  even for loopback, which an Audio Unit may not have, so a failed listen is an ordinary outcome (`http_port` says
  `HTTP_TRANSPORT`, and the program carries on without its page), and a listen that is not loopback asks the user
  (from the research's sources: not tried); destroy
  every `Program` with a web context before the plugin is unloaded, which joins the library's I/O thread (and can wait
  for a DNS lookup in flight, up to the system resolver's timeout: not on the UI thread); and a loader thread in
  `http_wait` is woken by `Program::stopWeb()` ([docs/web.md](docs/web.md)).
- **Files.** A program reads only folders `Program::Options::files` maps (none by default: the file builtins' policy,
  not a sandbox, [docs/files.md](docs/files.md)). Give it directories the plugin owns the meaning of, its bundle's
  resources, its own folder in the user's library, a folder the user chose; a sandboxed host can give only what it
  has, and an access the sandbox refuses is `FILE_DENIED`. Every file builtin that touches the disk is refused in a
  `[[clang::nonblocking]]` function, so a program loads on the plugin's loader thread and plays from memory; nothing
  is mapped, no thread is started, and the folders' handles close with the `Program`.
- **Size and symbols.** Static LLVM adds tens of megabytes. Export only the plugin's entry points (an
  exported-symbols list), so LLVM's symbols cannot clash with another copy of LLVM in the host process.
- **Bit-exactness across machines.** FMinus fuses and reassociates nothing, and in its default math mode a float's
  `sinf` is the same at any width, level or block size, and on arm64 and x86-64 alike (its own kernels). But the JIT
  tunes code for the CPU it runs on, the libm and fast math modes' functions are the system's, and a `poly float`'s
  width orders its float sums ([docs/simd.md](docs/simd.md)). Decide what a "same output" test means early.

## License

FMinus is free software under the GNU General Public License, version 3 (`LICENSE`). Its dependencies keep their own
licenses: LLVM (Apache 2.0 with LLVM exceptions), PFFFT (FFTPACKv5's BSD-style license: a binary that includes it, as
anything linking FMinus's runtime does, reproduces its copyright notice, conditions and disclaimer, from its
`LICENSE.txt`, in its documentation), yyjson (MIT: a binary that includes it, as anything linking FMinus's runtime
built with `FMINUS_JSON` does, as it is by default, carries its copyright notice and permission notice, from its
`LICENSE`), pugixml (MIT: a binary that includes it, as anything linking FMinus's runtime built with `FMINUS_XML`
does, as it is by default, carries its copyright notice and permission notice, from its `LICENSE.md`), fast_float
(Apache-2.0, MIT or BSL-1.0, at the user's choice; under BSL-1.0, a binary that includes it, as
anything linking FMinus's runtime built with `FMINUS_JSON` or `FMINUS_XML` does, needs no notice), libcurl (the curl
license, MIT-like: a binary that includes it, as anything linking FMinus built with `FMINUS_WEB` does, carries its
copyright notice and permission notice, from its `COPYING`), Mbed TLS (Apache-2.0 or GPL-2.0-or-later, at the user's
choice; under Apache-2.0, a binary that includes it, as anything linking FMinus built with `FMINUS_WEB` does on macOS
and Linux, carries a copy of the license, from its `LICENSE`), llhttp (MIT: a binary that includes it, as anything
linking FMinus built with `FMINUS_WEB_SERVER` does, carries its copyright notice and permission notice, from its
`LICENSE`), Catch2 (BSL-1.0),
CLI11 (BSD-3-Clause) and pybind11 (BSD-3-Clause; the Python package only). JUCE, which only `examples/plugin/` uses, is
under the AGPLv3 or JUCE's commercial license; that example's README says what that means for a plugin built from it.
Bison and Flex run only at build time; the code they generate is covered by Bison's skeleton exception and Flex's
permissive license. On Windows they are WinFlexBison's ports, under the same licenses (its build scripts are
GPL-3.0-or-later).
