# FMinus

A JIT compiler for audio DSP. FMinus source is lexed by Flex and parsed by Bison, then lowered to LLVM IR. LLVM
optimises it for the CPU it is running on, and ORC's LLJIT compiles it, all on the user's machine. For development,
the same IR can also be compiled ahead of time into a native executable.

**Status:** FMinus v0, a deliberately small subset of C that goes all the way through, from source to the JIT, to expand
on. It has functions over `float`, `int` and `bool`, and `void` ones; declarations, assignments, blocks, `if`, `while`,
`for`, `break`, `continue` and `return`; arithmetic with `%`, `++` and `--`, casts, comparisons, `!`, `&&`, `||` and
`?:`; calls and `<math.h>`'s float functions (below). It has pointers, `const` and `restrict` ones, so a function can
take a C++ host's buffers and process a whole block in one call, C's arrays, and C's heap (`malloc`, `calloc`,
`free`), with clang's `[[clang::nonblocking]]` checked so that the code on the audio thread can't reach it. It has
modules, its classes: fields and methods, as C++'s structs with member functions, which hold each other and arrays of
each other and live wherever their host puts them. It has SIMD: OpenCL C's `float2`, `float4` and `float8`, and `poly
float`, a vector as wide as the machine's. A program with a `float main(...)` also builds into an executable. The
build, the dependencies and the toolchain's smoke tests are in place around it.

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
                        TargetMachine and the O0-O3 pipeline), JitRuntime (the runtime, defined in an LLJIT),
                        Aot (object and assembly files, linked executables)
  runtime/              the FMinus runtime: its own library, libfminus_runtime.a, with no LLVM in it; the C
                        functions generated code calls, listed in RuntimeSymbols.def (a placeholder ABI version,
                        and an executable's command line and result)
  frontend/             FMinus v0's front end: Lexer.l, Parser.y, Ast.h (the syntax tree), Diagnostics.h,
                        Frontend.h (parse), Scanner.h (the glue between scanner and parser)
  ir/                   the syntax tree to LLVM IR: CodeGen.h (ir::lower), Lowering.h (the code generator's class
                        and its parts' map), CodeGen.cpp (the passes), Types, Functions, Statements, Expressions,
                        Calls, Vectors, Memory, Modules and Effects .cpp
  jit/                  Jit: LLJIT for FMinus modules (host CPU, pipeline, runtime)
  opt/                  empty: for FMinus's own pass pipelines
tools/<name>/           one executable per directory, with CLI11 for its command line: fminus (the driver),
                        fminus-build and fminus-hostinfo
examples/<name>/        one example per directory: an FMinus program, and a harness if it has .cpp files
                        (bin/example-<name>):
  biquad/               a biquad with per-sample coefficients, one float4 call a sample, against C++, compared and
                        timed; a plan to grow it
  note/                 a whole program with a main: a MIDI note's frequency (docs/programs.md)
  comparison/           a main that returns a bool: whether its two arguments are equal
  waveforms/            band-limited sawtooth and square waves by additive synthesis: for, while, if, break, continue
  chord/                a chord's four notes in a float4: one vector expf, literals, swizzles, reductions
  unison/               detuned sawtooth voices, one a lane of a poly float: a strip-mined loop with a mask
  delay/                a feedback delay whose line the program allocates, run by a [[clang::nonblocking]] kernel
  saturator/            tanh(drive * x), a loop over samples against one over poly floats (poly_load, poly_store)
  meter/                a block's peak and RMS, by plain loops and by poly floats' reductions
  voices/               eight synth voices in modules, in memory a host of the public API allocates, against C++
  hello/                FMinus as a library at its smallest: a main that JIT-compiles a program from a string
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

Everything comes through CMake's FetchContent, pinned by hash in `cmake/FMinusPins.cmake`. Nothing is taken from the
system unless an option asks for it.

| Dependency | Version | How | Cached in |
|---|---|---|---|
| LLVM (llvm/ only) | 23.1.3 | source release, built and installed once per flavor | `.deps/install/llvm-<ver>-<flavor>` |
| GNU Bison | 3.8.2 | source tarball, `./configure && make` at configure time | `.deps/install/bison-3.8.2` |
| Flex | 2.6.4 | source tarball, `./configure && make` at configure time | `.deps/install/flex-2.6.4` |
| WinFlexBison (Windows, in place of the two above) | 2.5.25 and fixes: Bison 3.8.2, Flex 2.6.4 | source archive of a commit, built with CMake at configure time | `.deps/install/winflexbison-2.5.25-a243a1c` |
| CLI11 | 2.7.2 | release source package, header-only, inside each build tree | the build tree |
| Catch2 | 3.16.1 | source archive, built inside each build tree | the build tree |

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
| program | function definitions, `float name(float a, const float b) { ... }` or `void name(...)`, and prototypes, `float name(float);`; `f(void)` is C's empty list; after the parameters, clang's `[[clang::nonallocating]]` or `[[clang::nonblocking]]`, checked: the function may not reach the heap (nor, nonblocking, an external function not marked so); modules, `module OnePole { float y; float tick(float x) { return y += x; } };`, whose fields are declarations without initialisers and whose methods are functions that see the fields by name (`this` is the module's address) |
| declarations | `float x = e;`, `const float k = e;`, `float* p = out;`, several at once (`float a = 1, b;`; in `float *p, x;` only `p` is a pointer, as in C); arrays, `float h[16];` and `int k[3] = {1, 2, 3};` (the elements left out are 0); modules, `OnePole lp;` and `OnePole b = a;` (a copy); block-scoped, shadowing allowed in an inner block |
| statements | `{ ... }`, `e;`, `return e;` and `return;`, `;`, `if (c) s else s` (an `else` closes the nearest `if`), `while (c) s`, `for (init; c; step) s` (its declarations are the loop's), `break;`, `continue;` |
| expressions | constants, names, calls, `( )`, unary `-` `+` `!`, `++` and `--` (before or after), casts `(int)x` `(float)x` `(bool)x`, `* / %`, `+ -`, `< <= > >=`, `== !=`, `&&`, `\|\|` (both short-circuit), `?:` (only the branch taken runs), and `= += -= *= /= %=` (right to left; an assignment's value is the one stored), with C's precedence; pointers' `p[i]`, `*p`, `&x`, `p + i`, `p - q` and comparisons; an array's `a[i]` (its name stands for a pointer to its first element); a module's fields `m.y` and `p->y`, its methods `m.tick(x)` and `p->tick(x)`, and `this`; `sizeof(type)`, an `int`; vector literals, OpenCL C's `(float4)(a, b, c, d)` and C's `(float4){a, b}` (the lanes left out are 0), and swizzles `v.wzyx`, `v.s0` |
| constants | C's: floating ones are floats (`1.5`, `1.`, `.5`, `2e3`, `1.5e-3f`, hex floats `0x1.8p1`, `0x1p-3f`), integer ones ints (`2`, `010` in octal, `0x1F` in hex); `true` and `false` |
| comments | `// ...` and `/* ... */` |
| types | `float` (IEEE single precision), `int` (32 bits, signed: C's `/` and `%`, overflow undefined) and `bool` (C23's: 0 or 1, computed as an int; a number is `x != 0` as a bool); `void`, a function's result; pointers to `float`, `int`, `float2`, `float4`, `float8` and modules, `const` (`const float*`, read-only through it) and `restrict` (C99's promise that nothing else reaches it), and `void*` (an address with no type, which converts to and from any pointer); arrays of `float`, `int`, the float vectors and modules; modules, by name (`OnePole`), from their declaration on; `float2`, `float4`, `float8` (OpenCL C's vectors, lane by lane, scalars broadcast); `poly float` and `poly bool`, as many lanes as the machine's vectors ([docs/simd.md](docs/simd.md)) |
| builtins | `fabsf sqrtf sinf cosf expf logf floorf fminf fmaxf tanhf` (LLVM intrinsics, on floats and vectors); `malloc calloc free` (C's, with `int` sizes); `reduce_add reduce_min reduce_max` (a vector to a float); `poly_lanes` (an int) and `poly_index`; `poly_load poly_store` (a poly float from or to a `float*`, with an optional mask) |

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
- **The heap's memory is aligned to 64 bytes and zeroed**, `malloc`'s too, as everything FMinus declares starts at 0;
  sizes are `int`s (C's are `size_t`s), so one block holds at most 2 GB. A call that breaks a function's
  `[[clang::nonallocating]]` or `[[clang::nonblocking]]` is an error, where clang only warns
  (`-Wfunction-effects`).
- **Vectors are OpenCL C's**, which C has none of: `float2`, `float4`, `float8`, their literals and swizzles, and math
  functions that take them. A comparison of vectors gives a lane mask (OpenCL C gives 0 or -1 in an `int` vector), and
  `?:` with a vector condition picks each lane, so both sides run. Literals come in OpenCL C's form,
  `(float4)(a, b, c, d)`, and in C's, `(float4){a, b, c, d}` (with clang's `ext_vector_type`). They differ: C reads
  `(float4)(...)` as a cast of a comma expression. `poly float`, a vector as wide as the machine's, is FMinus's own
  ([docs/simd.md](docs/simd.md)).
- **`%` works on floats**, with `fmodf`'s semantics: the result has the sign of the dividend, so `-7.5 % 2` is
  `-1.5`. C allows `%` on integers only; HLSL allows it on floats. `%=` goes with it.
- **A function may be called before its definition** without a prototype, because the compiler declares every
  function first. A prototype with no definition declares an external C function, as in C: the JIT resolves it in
  the process (`float atanf(float x);` calls libm's), and the linker resolves it in an executable.
- **A declaration without an initialiser starts at 0**, and a pointer at null. C leaves the variable indeterminate.
- **A non-void function that can reach its end is an error**, as in C++. Clang only warns in C.
- **Arithmetic is IEEE single precision**, with no contraction into fused multiply-adds (clang's
  `-ffp-contract=off`).

- **`main` returns a `float` or a `bool` and takes floats.** C's returns an `int` and takes `argc` and `argv`. An
  FMinus executable reads `main`'s arguments from its command line and prints its result, a float or `true` or
  `false` ([docs/programs.md](docs/programs.md)).

A v0 program with `f` suffixes, with prototypes for calls ahead of a definition, with vectors only in C's form (braces,
given clang's `ext_vector_type`), and without `%` on floats, `poly`, modules or `main`, is therefore also a valid C
program with the same meaning, given `<math.h>` (`<stdlib.h>` for the heap; C23 for the attributes) and
`-ffp-contract=off`. The example above, `examples/biquad/biquad.fm` and `examples/delay/delay.fm` all compile as C
under `-Wall -Wextra -Werror`, the last with clang's `-Wfunction-effects` too. One with modules is C++ instead, spelled
`struct` for `module` and `__restrict` for `restrict` (`examples/voices/voices.fm` compiles so).

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
   `ModuleTest.cpp`, `VectorTest.cpp`, `PolyTest.cpp`, which share `Compile.h`), IR or errors in
   `tests/lit/fminus/*.fm` (`// RUN:` lines, checked by FileCheck).

`examples/biquad/` is a kernel to grow the language around. It is a biquad low-pass with coefficients recomputed
every sample, which filters a block a call, with a harness that calls it once, compares it with C++ and times both,
and a plan of features in its README. `examples/delay/` is a feedback delay whose line the program allocates, with a
kernel marked `[[clang::nonblocking]]`. `examples/saturator/` (tanh) and `examples/meter/` (peak and RMS) each run a
loop over samples against a loop over `poly float`s, and time both. `examples/voices/` is a synth of eight voices in
modules, which a host of the public API allocates and runs.

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
  lanes (this machine's by default, which `polyLanes()` reports), and the file name the errors give.
- **Threads.** `compile()` and the first look-up of each function run LLVM's optimiser and code generator on the
  calling thread: they take milliseconds and allocate. Different programs may compile on different threads at once.
  In a plugin, compile off the audio thread and hand the pointer over (below).
- **Effects.** `nonBlocking(name)` and `nonAllocating(name)` say whether a function is marked
  `[[clang::nonblocking]]` or `[[clang::nonallocating]]`, which FMinus has then checked, so a host can refuse to run
  anything else on its audio thread.
- **Modules.** `module(name)` describes one: its size and alignment, what a host allocates for an instance (zeroed:
  every field starts at 0, and nothing else runs), and each field's name, type and offset, which a C++ struct with the
  same fields in the same order shares. A method is a function whose first parameter is the instance:
  `function<float(void*, float)>("OnePole::tick", errors)`. `examples/voices/` is such a host.
- **Lifetime.** A function pointer is valid while its `Program` lives.

The header has no LLVM in it and needs C++17. In CMake, a host links `FMinus::FMinus`, and gets FMinus's include
directories and libraries (LLVM's too, linked statically), but neither LLVM's headers nor FMinus's C++23. As a
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
| `tsan` | RelWithDebInfo + TSan | Release + assertions | compiling on a background thread |
| `inline` | RelWithDebInfo | in-tree, -O2 -g, assertions | stepping into LLVM |
| `xcode` | Xcode project | Release + assertions | Xcode's debugger and Instruments |

Every preset builds in `build-<preset>/` and exports `compile_commands.json`; `.clangd` reads `build-dev`'s. Each
preset has a workflow (`cmake --workflow --preset <name>`) except `xcode`, which is built from Xcode
(`cmake --preset xcode && open build-xcode/FMinus.xcodeproj`). ccache or sccache is used when installed. On Windows,
`asan`, `tsan` and `xcode` are not available.

## Handy

- `build-dev/bin/fminus-hostinfo [--features]`: the triple, CPU, features and data layout the JIT targets here.
- VSCode: `python3 editors/vscode/package.py`, then `code --install-extension build-vscode/fminus-0.5.0.vsix`, for
  FMinus syntax highlighting, lit's `RUN:` and `CHECK:` lines included
  ([editors/vscode/README.md](editors/vscode/README.md)).
- `build-dev/bin/fminus`: FMinus source to its tree, IR, assembly or an object, or run in the JIT (above).
- `build-dev/bin/fminus-build`: LLVM IR to an executable, object, assembly or optimised IR (above).
- The LLVM tools of the flavor in use are in `.deps/install/llvm-…/bin`: `opt -passes='default<O3>' -S`,
  `llc -mcpu=native`, `llvm-mca` (throughput of a kernel's machine code), `llvm-objdump -d`.
- LLVM is built with `LLVM_ENABLE_DUMP=ON`: `value->dump()` and `module->dump()` work without assertions too.

## Notes for running inside a plugin

These are constraints the research will run into, not things the skeleton handles:

- **Executable memory.** Under the hardened runtime, macOS needs an entitlement for memory written at run time and
  then executed: `com.apple.security.cs.allow-jit` (for `MAP_JIT`) or `allow-unsigned-executable-memory`. A plugin
  runs in its host's process and has only the host's entitlements, so check each target DAW early. Unsigned test
  executables (this repository's) are unaffected.
- **Real time.** LLJIT compiles lazily, on the thread that looks a symbol up. Compile and look up on a background
  thread, hand the function pointer to the audio thread atomically, and keep JIT'd code free of allocation and locks:
  mark the functions the audio thread calls `[[clang::nonblocking]]`, and FMinus refuses any that could reach
  `malloc` or `free` ([docs/extending.md](docs/extending.md), recipe 11).
- **Size and symbols.** Static LLVM adds tens of megabytes. Export only the plugin's entry points (an
  exported-symbols list), so LLVM's symbols cannot clash with another copy of LLVM in the host process.
- **Bit-exactness across machines.** Host-tuned code can differ in its last bits between CPUs (FMA contraction,
  vector widths, fast-math). Decide what a "same output" test means early.

## License

FMinus is free software under the GNU General Public License, version 3 (`LICENSE`). Its dependencies keep their own
licenses: LLVM (Apache 2.0 with LLVM exceptions), Catch2 (BSL-1.0) and CLI11 (BSD-3-Clause). Bison and Flex run only
at build time; the code they generate is covered by Bison's skeleton exception and Flex's permissive license. On
Windows they are WinFlexBison's ports, under the same licenses (its build scripts are GPL-3.0-or-later).
