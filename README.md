# FMinus

A JIT compiler for audio DSP. FMinus source is lexed by Flex and parsed by Bison, then lowered to LLVM IR. LLVM
optimises it for the CPU it is running on, and ORC's LLJIT compiles it, all on the user's machine. For development,
the same IR can also be compiled ahead of time into a native executable.

**Status:** FMinus v0, a deliberately small subset of C that goes all the way through, from source to the JIT, to expand
on. It has functions over `float`, `int` and `bool`, and `void` ones; declarations, assignments, blocks, `if`, `while`,
`for`, `break`, `continue` and `return`; arithmetic with `%`, `++` and `--`, casts, comparisons, `!`, `&&`, `||` and
`?:`; calls and `<math.h>`'s float functions (below). It has pointers, `const` and `restrict` ones, so a function can
take a C++ host's buffers and process a whole block in one call, C's arrays, and C's heap (`malloc`, `calloc`, `free`),
with clang's `[[clang::nonblocking]]` checked so that the code on the audio thread can't reach it. It has modules, its
classes: fields and methods, as C++'s structs with member functions, which hold each other and arrays of each other and
live wherever their host puts them, and parameters, the values a plugin's host sets, which a method reads a sample at a
time and the host compiles as knobs, as streams or as constants. It has an FFT, real and complex, whose plans are made
off the audio thread and whose transforms run on it (PFFFT, compiled into FMinus's runtime library). It has SIMD: OpenCL
C's `float2`, `float4` and `float8`, and `poly float`, a vector as wide as the machine's. It has threads of its own,
structured: `parallel for` spreads a loop's iterations over the machine's cores and `parallel { }` runs a block's
statements at once, each finished where it ends, with the races the compiler can see refused, on a pool of threads that
a `[[clang::nonblocking]]` function may use. A program with a `float main(...)` also builds into an executable. Its
compiler has optimisations of its own, from `-O1`: a method gets a copy of its code for each value of a knob that steers
its loop, and a local's zeroing (an array's, or a module's overwritten whole) is removed where nothing reads the zeros.
It tells LLVM C's strict-aliasing rule, that a loop whose condition is not a constant may be assumed to end, and what
the runtime's functions do. Its math builtins give a float one result wherever it is computed, and a host may have
subnormals flushed to zero. `examples/plugin/` is a JUCE plugin whose DSP is an FMinus program. The build, the
dependencies and the toolchain's smoke tests are in place around it.

Documentation: [docs/](docs/README.md). It covers the architecture, writing a program with `main`, a guide to
extending the compiler with worked recipes, SIMD (vector types, a vector math library, and vectors as wide as the
machine's), the build, and testing and debugging.

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
CMakePresets.json       dev, debug, release, asan, tsan, inline, xcode
cmake/
  FMinusPins.cmake        every dependency: version, URL, SHA-256
  FMinusClangCl.cmake     Windows: clang-cl, found before project() (Visual Studio's, PATH's, LLVM's)
  FMinusOptions.cmake     options, the deps cache, ccache, fminus_target_defaults()
  FMinusLLVM.cmake        LLVM (cache | inline | system) -> FMinus::LLVM
  FMinusParserTools.cmake Bison and Flex (fetch | system; WinFlexBison on Windows) -> fminus_add_grammar()
  FMinusCLI11.cmake       CLI11 -> CLI11::CLI11, linked into every tool
  FMinusTesting.cmake     Catch2 and lit
  templates/Config.h.in   -> <build>/include/fminus/Config.h
include/fminus/         the library's public API ("fminus/..."): FMinus.h, fminus::Program (below)
src/                    the fminus library: every .cpp is a source, every grammar is generated in
  api/                  the public API (include/fminus/FMinus.h) on the pipeline
  support/              integration helpers that came with the skeleton: LLVMInit, HostTarget (the host's
                        TargetMachine and the O0-O3 pipeline, with FMinus's passes, and the vector math library in
                        the fast math mode), JitRuntime (the runtime, defined in an LLJIT), Aot (object and
                        assembly files, linked executables)
  runtime/              the FMinus runtime: its own library, libfminus_runtime.a, with no LLVM in it; the C
                        functions generated code calls, listed in RuntimeSymbols.def (a placeholder ABI version,
                        an executable's command line and result, the heap, the FFT, Fft.cpp, on PFFFT's objects,
                        which the library holds, and the math builtins' functions, Math.cpp)
  frontend/             FMinus v0's front end: Lexer.l, Parser.y, Ast.h (the syntax tree), Diagnostics.h,
                        Frontend.h (parse), Scanner.h (the glue between scanner and parser)
  ir/                   the syntax tree to LLVM IR: CodeGen.h (ir::lower), Lowering.h (the code generator's class
                        and its parts' map), CodeGen.cpp (the passes), Types, Functions, Statements, Expressions,
                        Calls, Math (the math modes), Vectors, Memory, Modules, Effects, Tbaa (strict aliasing's
                        tags) and Denormals (flush-to-zero; with Denormals.h) .cpp
  jit/                  Jit: LLJIT for FMinus modules (host CPU, pipeline, runtime)
  opt/                  FMinus's own LLVM passes: KnobVersioning (a method's copies for its knobs' values) and
                        ZeroInitElision (a local's zeroing, removed where nothing reads it)
tools/<name>/           one executable per directory, with CLI11 for its command line: fminus (the driver),
                        fminus-build and fminus-hostinfo
examples/<name>/        one example per directory: an FMinus program, and a harness if it has .cpp files
                        (bin/example-<name>):
  biquad/               a biquad module with per-sample coefficients, its cutoff a stream, against C++, compared
                        and timed; a plan to grow it
  note/                 a whole program with a main: a MIDI note's frequency (docs/programs.md)
  comparison/           a main that returns a bool: whether its two arguments are equal
  waveforms/            band-limited sawtooth and square waves by additive synthesis: for, while, if, break, continue
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
  hello/                FMinus as a library at its smallest: a main that JIT-compiles a program from a string
  plugin/               FMinus Filter, a JUCE plugin (AU, VST3, Standalone) whose DSP is an FMinus program: its
                        own CMake project, built apart from FMinus's (cmake -S examples/plugin -B build-plugin)
tests/
  unit/                 your Catch2 tests
  library/              the public API as a host uses it: in C++17, against FMinus::FMinus alone
  smoke/                the toolchain's checks: LLJIT + O3 on the host, Bison + Flex (smoke/calc/)
  lit/                  lit + FileCheck tests (*.test, *.fm, *.ll)
editors/vscode/         a VSCode extension: syntax highlighting for .fm files (its README installs it)
Scripts/pin-llvm.sh     the pin lines for another LLVM release
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
| CLI11 | 2.7.2 | release source package, header-only, inside each build tree | the build tree |
| Catch2 | 3.16.1 | source archive, built inside each build tree | the build tree |
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
| program | function definitions, `float name(float a, const float b) { ... }` or `void name(...)`, and prototypes, `float name(float);`; `f(void)` is C's empty list; after the parameters, clang's `[[clang::nonallocating]]` or `[[clang::nonblocking]]`, checked: the function may not reach the heap (nor, nonblocking, an external function not marked so); modules, `module OnePole { float y; float tick(float x) { return y += x; } };`, whose fields are declarations without initialisers and whose methods are functions that see the fields by name (`this` is the module's address); a module's parameters, `param float cutoff [[fminus::range(20, 20000)]] [[fminus::default(1000)]];` (a `float`, `int` or `bool`), which the host sets and a method reads as `cutoff[i]`, its value at sample i of the call |
| declarations | `float x = e;`, `const float k = e;`, `float* p = out;`, several at once (`float a = 1, b;`; in `float *p, x;` only `p` is a pointer, as in C); arrays, `float h[16];` and `int k[3] = {1, 2, 3};` (the elements left out are 0); modules, `OnePole lp;` and `OnePole b = a;` (a copy); block-scoped, shadowing allowed in an inner block |
| statements | `{ ... }`, `e;`, `return e;` and `return;`, `;`, `if (c) s else s` (an `else` closes the nearest `if`), `while (c) s`, `for (init; c; step) s` (its declarations are the loop's), `break;`, `continue;`; a `while` or `for` whose condition is not a constant expression may be assumed to end, as in C11 (6.8.5p6), and `while (1)` and `for (;;)` may run for ever; FMinus's `parallel for (int i = first; i < end; ++i) s`, its iterations at once over the machine's cores, and `parallel { s1 s2 ... }`, its statements at once, each finished, all of it, where it ends (below) |
| expressions | constants, names, calls, `( )`, unary `-` `+` `!`, `++` and `--` (before or after), casts `(int)x` `(float)x` `(bool)x`, `* / %`, `+ -`, `< <= > >=`, `== !=`, `&&`, `\|\|` (both short-circuit), `?:` (only the branch taken runs), and `= += -= *= /= %=` (right to left; an assignment's value is the one stored), with C's precedence; pointers' `p[i]`, `*p`, `&x`, `p + i`, `p - q` and comparisons; an array's `a[i]` (its name stands for a pointer to its first element); a module's fields `m.y` and `p->y`, its methods `m.tick(x)` and `p->tick(x)`, and `this`; `sizeof(type)`, an `int`; vector literals, OpenCL C's `(float4)(a, b, c, d)` and C's `(float4){a, b}` (the lanes left out are 0), and swizzles `v.wzyx`, `v.s0` |
| constants | C's: floating ones are floats (`1.5`, `1.`, `.5`, `2e3`, `1.5e-3f`, hex floats `0x1.8p1`, `0x1p-3f`), integer ones ints (`2`, `010` in octal, `0x1F` in hex); `true` and `false` |
| comments | `// ...` and `/* ... */` |
| directives | `#include "file"`, the file read where the line is, as in C, and once only (as if every file began with `#pragma once`), found where the host says: `fminus` looks beside the including file, then in `-I`'s directories; `#pragma`, ignored |
| types | `float` (IEEE single precision), `int` (32 bits, signed: C's `/` and `%`, overflow undefined) and `bool` (C23's: 0 or 1, computed as an int; a number is `x != 0` as a bool); `void`, a function's result; pointers to `float`, `int`, `float2`, `float4`, `float8` and modules, `const` (`const float*`, read-only through it) and `restrict` (C99's promise that nothing else reaches it), and `void*` (an address with no type, which converts to and from any pointer); arrays of `float`, `int`, the float vectors and modules; modules, by name (`OnePole`), from their declaration on; `float2`, `float4`, `float8` (OpenCL C's vectors, lane by lane, scalars broadcast); `poly float` and `poly bool`, as many lanes as the machine's vectors ([docs/simd.md](docs/simd.md)) |
| builtins | `fabsf sqrtf floorf fminf fmaxf` (LLVM intrinsics, exact) and `sinf cosf expf logf tanhf` (by the math mode: the runtime's, with one result for a float wherever it is computed, by default), on floats and vectors; `malloc calloc free` (C's, with `int` sizes); `reduce_add reduce_min reduce_max` (a vector to a float); `poly_lanes` (an int) and `poly_index`; `poly_load poly_store` (a poly float from or to a `float*`, with an optional mask); the FFT: `fft_create(n)` and `fft_create_complex(n)` (a plan, a `void*`; they allocate), `fft_forward(plan, in, out)` and `fft_inverse(plan, in, out)` (real when one side is floats, complex when both are `float2`s; unnormalised), `fft_destroy(plan)` |

Where FMinus differs from C, it is on purpose:

- **`float` and `int` are the arithmetic types**: no `double`, `long`, `unsigned` or narrower integers. In C an
  unsuffixed `1.5` is a `double`, and `x * 1.5` is computed in double; in FMinus it is a float. Write the `f` suffix
  (`1.5f`) and a program means the same in both. Every example here does. An integer constant is an `int`, as in C, so
  `1 / 2` is 0; one too large for an `int` is an error, where C would make it a `long`.
- **Pointers point to `float`, `int`, `float2`, `float4`, `float8`, `void` or a module, one level deep**: no pointers
  to pointers or to arrays, no `bool*`, no null pointer constant (`0` is an int; an uninitialised pointer is null) and
  no casts between pointer types. Converting a pointer to one of another type (other than through a `void*`), or
  dropping its `const`, is an error, where C warns.
- **Arrays are C's, one dimension deep**: of `float`, `int`, the float vectors or modules (no arrays of arrays, of
  pointers or of `bool`), their size an integer constant, and zeroed like every variable. An initialiser list may be
  shorter than the array, never longer (C only warns).
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
  whole call) or frozen (a constant). `fminus::range` and `fminus::default` are for the host; FMinus checks them
  against each other only. From `-O1`, a method whose loop branches on a `bool` knob or an `int` knob whose range
  holds at most 8 values, or whose loop reads a stream, gets a copy of its code for each value (for a stream, null or
  not), which computes what the method does ([docs/extending.md](docs/extending.md), recipe 16). A stream that gets no
  copies (a method's fourth stream, under the default budget of 8 copies) still reads with no branch: its loop
  vectorises when the stream has samples, and runs a sample at a time when it is flat.
- **The heap's memory is aligned to 64 bytes and zeroed**, `malloc`'s too, as everything FMinus declares starts at 0;
  sizes are `int`s (C's are `size_t`s), so one block holds at most 2 GB. A call that breaks a function's
  `[[clang::nonallocating]]` or `[[clang::nonblocking]]` is an error, where clang only warns
  (`-Wfunction-effects`). As with clang's `malloc`, LLVM may remove a block the program never uses, and a removed
  block does not fail: a test of it against null is then false.
- **The FFT is FMinus's own**, as C has none: builtins on PFFFT, which take sizes of 2, 3 and 5 only, multiples of 32
  points for a real plan and of 16 for a complex one (`fft_create` is null for any other). A plan is made and freed off
  the audio thread (the effects count `fft_create`, `fft_create_complex` and `fft_destroy` as allocations), and a
  transform allocates nothing. Bins are `float2`s, (re, im), and neither direction scales: inverse(forward(x)) is n
  times x, as in FFTW and PFFFT. A plan is opaque: a program hands it to the FFT's builtins, and reads or writes
  nothing through it.
- **Memory is read and written as the type it holds**, C's strict-aliasing rule, which FMinus tells LLVM as clang
  does (TBAA): an `int` read as a `float` through a `void*` is undefined. FMinus is stricter in three places: a float
  vector or `poly float` is its elements' type, `float`; allocated memory keeps one type at each place until it is
  freed (C lets a store give it a new one); and there is no character type, so only a module's copy and a declaration's
  zeroing read or write bytes of any type. `--no-strict-aliasing` or `Program::Options::strictAliasing = false` turns
  the rule off, as clang's `-fno-strict-aliasing` does.
- **`sinf`, `cosf`, `expf`, `logf` and `tanhf` give a float one result wherever it is computed**: in a scalar loop or
  its vectorised body, in a vector of any width, at any optimisation level, from a constant or not. C promises no
  such thing. By default they are libsystem_m's vector functions on Apple systems (within about 3 ulp of the exact
  result, where libm's are within 1), and libm's elsewhere; `fminus::math` computes the same in C++.
  `Program::Options::math` (`fminus --math`) chooses `Libm`, libm's results bit for bit, or `Fast`, LLVM's intrinsics
  as clang compiles `<math.h>`. The runtime keeps three names for each of them (`fminus_rt_sinf`, `fminus_rt_sin_f4`
  and `fminus_rt_libm_sinf` for `sinf`): a program that calls one may not declare a function of any of its names, in
  any math mode. The call is then an error, with a note at the declaration.
- **Subnormals are C's unless the host asks for flush-to-zero** (`Program::Options::denormals`,
  `fminus --denormals=flush`): then a subnormal input reads as 0 and a subnormal result is 0, each with its sign, in
  folding as at run time, and each entry point (a `[[clang::nonblocking]]` function the program does not call) sets
  the mode on entry and restores the caller's on return. C's floats underflow gradually; clang's
  `-fdenormal-fp-math=preserve-sign` is the same choice.
- **Vectors are OpenCL C's**, which C has none of: `float2`, `float4`, `float8`, their literals and swizzles, and math
  functions that take them. A comparison of vectors gives a lane mask (OpenCL C gives 0 or -1 in an `int` vector), and
  `?:` with a vector condition picks each lane, so both sides run. Literals come in OpenCL C's form,
  `(float4)(a, b, c, d)`, and in C's, `(float4){a, b, c, d}` (with clang's `ext_vector_type`). They differ: C reads
  `(float4)(...)` as a cast of a comma expression. `poly float`, a vector as wide as the machine's, is FMinus's own
  ([docs/simd.md](docs/simd.md)).
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
- **A function may be called before its definition** without a prototype, because the compiler declares every
  function first. A prototype with no definition declares an external C function, as in C: the JIT resolves it in
  the process (`float atanf(float x);` calls libm's), and the linker resolves it in an executable. On Apple systems,
  whose libm sets no `errno`, a prototype of libm's function says it writes no memory, as clang's `<math.h>` says
  there, so a call on a loop's invariants is made once, before the loop ([docs/extending.md](docs/extending.md),
  recipe 19).
- **A declaration without an initialiser starts at 0**, and a pointer at null. C leaves the variable indeterminate.
  From `-O1`, the optimiser removes a local's zeroing where nothing reads the zeros: an array's that loops overwrite,
  and a module's that a copy of another overwrites. `-O0` keeps every one ([docs/extending.md](docs/extending.md),
  recipe 18).
- **A non-void function that can reach its end is an error**, as in C++. Clang only warns in C.
- **The preprocessor is `#include "file"` alone**: no macros, no `#if`, and no `#include <...>`, as FMinus has no
  system headers. They are errors, and `#pragma` is ignored, as C ignores the pragmas it doesn't know. Each file is
  read once, as if it began with `#pragma once`: two files that both include a third (a module both use) read it the
  first time only, where C would define everything in it twice. A directive's errors are clang's, `extra tokens at end
  of #include directive` too, which clang only warns of ([docs/extending.md](docs/extending.md), recipe 21).
- **Arithmetic is IEEE single precision**, with no contraction into fused multiply-adds (clang's
  `-ffp-contract=off`), and subnormals as IEEE has them unless the host flushes them (above).

- **`main` returns a `float` or a `bool` and takes floats.** C's returns an `int` and takes `argc` and `argv`. An
  FMinus executable reads `main`'s arguments from its command line and prints its result, a float or `true` or
  `false` ([docs/programs.md](docs/programs.md)).

Loops make progress as C11 says (6.8.5p6), not as C++ does: a `while` or `for` whose condition is not a constant
expression and which runs for ever without effects is undefined, so LLVM may delete such a loop whose results go
unused, while `while (1)` and `for (;;)` may run for ever. A function as a whole is not assumed to make progress, as a
C++ one is.

A v0 program with `f` suffixes, with prototypes for calls ahead of a definition, with vectors only in C's form (braces,
given clang's `ext_vector_type`), and without `%` on floats, `poly`, modules, `parallel` or `main`, is therefore also a
valid C program with the same meaning, given `<math.h>` (`<stdlib.h>` for the heap; C23 for the attributes) and
`-ffp-contract=off`. The example above and `examples/delay/delay.fm` compile as C under `-Wall -Wextra -Werror`, the
latter with clang's `-Wfunction-effects` too. One with modules is C++ instead, spelled `struct` for `module` and
`__restrict` for `restrict`, as long as no module has parameters, which are FMinus's own: `examples/voices/voices.fm`'s
modules but `Synth` compile so, with `-Wno-perf-constraint-implies-noexcept` (C++ wants a nonblocking function
`noexcept` too). The other examples use modules with parameters, `poly`, `parallel`, FMinus's own builtins (the
convolver's FFT) or a `main`; hello's one function is C.

Every FMinus function becomes an external LLVM function of the same name with the C calling convention (a `float` is
LLVM's `float`, an `int` an `i32`, a pointer a `ptr`), so C++ calls JIT'd code through a pointer of the matching type:
`float(float, ...)`, or `void(const float*, float*, int)` for a function that processes a block of samples in one
call. A method is one too, named `OnePole::tick`, whose first parameter is the module's address: `float(void*,
float)`. A module is an LLVM struct of its fields, laid out as C lays out a struct. The code generator reports every
error it finds, in source order and in clang's words, as `file:line:column: error: message`. The parser stops at the
first syntax error.

```bash
build-dev/bin/fminus chain.fm                      # the optimised IR (-O2)
build-dev/bin/fminus chain.fm -O0                  # the IR as the code generator wrote it
build-dev/bin/fminus chain.fm --emit ast           # the syntax tree, as s-expressions
build-dev/bin/fminus chain.fm --emit asm -o -      # this machine's assembly (--emit obj: an object file)
build-dev/bin/fminus chain.fm --run chain 1        # JIT-compiles, calls chain(1), prints 0.33333334
build-dev/bin/fminus note.fm --emit exe -o note    # an executable that runs main: ./note 69 prints 440
build-dev/bin/fminus unison.fm --poly-lanes 16     # compile as for a machine with 16-float vectors (AVX-512)
build-dev/bin/fminus lp.fm --stream Lowpass::cutoff --freeze Lowpass::mode=2   # parameters: a stream, a constant
build-dev/bin/fminus lp.fm --knob-versions 0       # no copies of a method for its knobs' values (8 at most by default)
build-dev/bin/fminus synth.fm --math libm          # sinf, cosf, ... as libm's (vector, the default; fast: LLVM's own)
build-dev/bin/fminus lp.fm --denormals=flush       # subnormals flushed to zero: folded so, and set by entry points
build-dev/bin/fminus lp.fm --no-strict-aliasing    # no TBAA: for a program that reads memory as another type
build-dev/bin/fminus synth.fm -I lib               # #include "name": beside the including file, then in lib/
build-dev/bin/fminus voices.fm --threads 4         # a parallel region on 4 threads at most (0: every core; 1: alone)
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
   `ControlFlowTest.cpp`, `IntTest.cpp`, `PointerTest.cpp`, `HeapTest.cpp`, `EffectTest.cpp`, `ArrayTest.cpp`,
   `ModuleTest.cpp`, `FftTest.cpp`, `VectorTest.cpp`, `PolyTest.cpp`, `ParallelTest.cpp`, which share `Compile.h`),
   IR or errors in
   `tests/lit/fminus/*.fm` (`// RUN:` lines, checked by FileCheck).

`examples/biquad/` is a kernel to grow the language around. It is a biquad low-pass module with coefficients recomputed
every sample, its cutoff a stream, which filters a block a call, with a harness that calls it once, compares it with C++
and times both, and a plan of features in its README. `examples/delay/` is a feedback delay whose line the program
allocates, with a kernel marked `[[clang::nonblocking]]`, written as plain C and as a module. `examples/saturator/`
(tanh) and `examples/meter/` (peak and RMS) each run a loop over samples against a loop over `poly float`s, and time
both. `examples/voices/` is a synth of eight voices in modules, its controls knobs, which a host of the public API
allocates and runs. `examples/convolver/` convolves with an impulse response by FFT. `examples/ensemble/` renders
sixteen voices at once with a `parallel for`, and checks that every core gives what one thread does, bit for bit.
`examples/parameters/` sweeps a filter's cutoff with an LFO, through a knob once a call or a stream a sample, and
freezes a fixed one.

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
  signature that does not match the declaration is undefined behaviour. The header has the table.
- **Options.** `Program::Options` sets the optimisation level (`-O2` by default, as for `fminus`), a `poly float`'s
  lanes (this machine's by default, which `polyLanes()` reports), and the file name the errors give. `include` is
  where `#include "name"` finds its file: a function from the name and the including file's name to the file's name
  and text, so a host keeps its files where it likes, on disk or in memory (with none, the default, no `#include`
  finds a file). `math` is how `sinf` and the other transcendental builtins compute (`Math::Vector` by default,
  `Libm` or `Fast`), `strictAliasing` whether the optimiser relies on C's strict-aliasing rule (`true`),
  `knobVersions` how many copies of a method knob versioning may make (8; 0 turns it off), `denormals` whether
  subnormals are kept or flushed to zero (`Denormals::Preserve` or `Flush`), and `threads` the most threads a parallel
  region runs on (0, the default, is every core; 1 runs each on the calling thread alone). The header says what each
  costs.
- **Threads.** `compile()` and the first look-up of each function run LLVM's optimiser and code generator on the
  calling thread: they take milliseconds and allocate. Different programs may compile on different threads at once.
  In a plugin, compile off the audio thread and hand the pointer over (below). A program's `parallel for` and
  `parallel { }` run on FMinus's pool, shared by every `Program` in the process: the first with a region starts it
  when it compiles (a thread for each core but one) and the last one's destructor stops it. The thread that calls the
  program works on each region too.
- **Effects.** `nonBlocking(name)` and `nonAllocating(name)` say whether a function is marked
  `[[clang::nonblocking]]` or `[[clang::nonallocating]]`, which FMinus has then checked, so a host can refuse to run
  anything else on its audio thread.
- **Modules.** `module(name)` describes one: its size and alignment, what a host allocates for an instance (zeroed:
  every field starts at 0, and nothing else runs), and each field's name, type and offset, which a C++ struct with the
  same fields in the same order shares. A method is a function whose first parameter is the instance:
  `function<float(void*, float)>("OnePole::tick", errors)`. `examples/voices/` is such a host.
- **Parameters.** `Options::parameters` binds each parameter by name, `"Lowpass::cutoff"`: `Binding::knob()` (the
  default), `stream()` or `frozen(value)`. `parameter(name)` and `Module::parameters` describe each: its type, binding,
  range and default, and where its slot is in an instance. `setValue` sets a knob from any thread (a relaxed atomic
  store, which a method reads when it next starts), `setStream` points a stream at the call's samples (or, null, makes
  the call flat: every sample is then the value `setValue` set), and `setDefaults` sets each parameter's default.
  `examples/parameters/` is such a host.
- **Math.** `fminus::math::sin`, `cos`, `exp`, `tanh` and `log` compute what a program in the default math mode does,
  bit for bit, so a C++ reference that calls them agrees with the program at any block size, level and width. They
  are `[[gnu::const]]` for GCC and clang (`FMINUS_MATH_CONST`), so two calls of one argument are one.
- **Lifetime.** A function pointer is valid while its `Program` lives.

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

## Executables (ahead of time)

For development, a module can also be compiled ahead of time for this machine, into a native executable you can run
under lldb or Instruments, or into an object or assembly file to read. Its machine code is what LLJIT generates for the
same module, since both use the TargetMachine and pipeline in `support/HostTarget.h`.

An FMinus program with a `float main(...)` builds straight into one. `fminus::aot::addEntryPoint` renames the
program's `main` to `fminus.main` and adds the C `main` an executable starts in, which reads the arguments and prints
the result ([docs/programs.md](docs/programs.md)):

```bash
build-dev/bin/fminus note.fm --emit exe -o note             # then ./note 69
build-dev/bin/fminus note.fm --entry                        # the IR with that C main (--emit asm, obj too)
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
ahead-of-time code call the same runtime. In the JIT, `fminus::defineRuntimeSymbols` defines every function listed in
`src/runtime/RuntimeSymbols.def` by its address. Under the `asan` and `tsan` presets, the sanitizer runtimes are linked
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
  `ubsan`, `tsan`, `arm64`, `x86_64`, `system-darwin`, `system-linux`, `system-windows`. `.fm` is a placeholder
  extension for FMinus sources: change it in `tests/lit/lit.cfg.py`.

## Presets

| Preset | FMinus | LLVM | For |
|---|---|---|---|
| `dev` | RelWithDebInfo | Release + assertions | everyday work |
| `debug` | Debug | Release + assertions | stepping through FMinus |
| `release` | Release | Release | measuring |
| `asan` | Debug + ASan + UBSan | Release + assertions | memory errors in FMinus's code |
| `tsan` | RelWithDebInfo + TSan | Release + assertions | compiling on a background thread, the parallel regions' pool |
| `inline` | RelWithDebInfo | in-tree, -O2 -g, assertions | stepping into LLVM |
| `xcode` | Xcode project | Release + assertions | Xcode's debugger and Instruments |

Every preset builds in `build-<preset>/` and exports `compile_commands.json`; `.clangd` reads `build-dev`'s. Each
preset has a workflow (`cmake --workflow --preset <name>`) except `xcode`, which is built from Xcode
(`cmake --preset xcode && open build-xcode/FMinus.xcodeproj`). ccache or sccache is used when installed. On Windows,
`asan`, `tsan` and `xcode` are not available.

## Handy

- `build-dev/bin/fminus-hostinfo [--features]`: the triple, CPU, features and data layout the JIT targets here.
- VSCode: `python3 editors/vscode/package.py`, then `code --install-extension build-vscode/fminus-0.9.0.vsix`, for
  FMinus syntax highlighting, lit's `RUN:` and `CHECK:` lines included
  ([editors/vscode/README.md](editors/vscode/README.md)).
- `build-dev/bin/fminus`: FMinus source to its tree, IR, assembly or an object, or run in the JIT (above).
- `build-dev/bin/fminus-build`: LLVM IR to an executable, object, assembly or optimised IR (above).
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
- **Size and symbols.** Static LLVM adds tens of megabytes. Export only the plugin's entry points (an
  exported-symbols list), so LLVM's symbols cannot clash with another copy of LLVM in the host process.
- **Bit-exactness across machines.** FMinus fuses and reassociates nothing, and in its default math mode a float's
  `sinf` is the same at any width, level or block size. But the JIT tunes code for the CPU it runs on, the math
  functions are libsystem_m's on Apple systems and libm's elsewhere, and a `poly float`'s width orders its float sums
  ([docs/simd.md](docs/simd.md)). Decide what a "same output" test means early.

## License

FMinus is free software under the GNU General Public License, version 3 (`LICENSE`). Its dependencies keep their own
licenses: LLVM (Apache 2.0 with LLVM exceptions), PFFFT (FFTPACKv5's BSD-style license: a binary that includes it, as
anything linking FMinus's runtime does, reproduces its copyright notice, conditions and disclaimer, from its
`LICENSE.txt`, in its documentation), Catch2 (BSL-1.0) and CLI11 (BSD-3-Clause). JUCE, which only `examples/plugin/`
uses, is under the AGPLv3 or JUCE's commercial license; that example's README says what that means for a plugin built
from it. Bison and Flex run only at build time; the code they generate is covered by Bison's skeleton exception and
Flex's permissive license. On Windows they are WinFlexBison's ports, under the same licenses (its build scripts are
GPL-3.0-or-later).
