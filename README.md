# FMinus

A JIT compiler for audio DSP. FMinus source is lexed by Flex and parsed by Bison, then lowered to LLVM IR. LLVM
optimises it for the CPU it is running on, and ORC's LLJIT compiles it, all on the user's machine. For development,
the same IR can also be compiled ahead of time into a native executable.

**Status:** FMinus v0, a deliberately small language that goes all the way through, from source to the JIT, to
expand on. It has functions over `float`, `let`, `return`, arithmetic, calls and a few math builtins (below). The
build, the dependencies and the toolchain's smoke tests are in place around it.

## Quick start

```bash
cmake --workflow --preset dev
```

The first run downloads the pinned sources: LLVM (179 MB), Bison, Flex, CLI11 and Catch2. It builds Bison and Flex
(about a minute) and one LLVM flavor (10–30 minutes, with the output streaming) into the dependency cache, `.deps/`.
Later configures, in this build tree or any other, find them there in seconds. Then it builds FMinus and runs every
test.

## Layout

```
CMakeLists.txt          the project; cmake/ holds the parts
CMakePresets.json       dev, debug, release, asan, tsan, inline, xcode
cmake/
  FMinusPins.cmake        every dependency: version, URL, SHA-256
  FMinusOptions.cmake     options, the deps cache, ccache, fminus_target_defaults()
  FMinusLLVM.cmake        LLVM (cache | inline | system) -> FMinus::LLVM
  FMinusParserTools.cmake Bison and Flex (fetch | system) -> fminus_add_grammar()
  FMinusCLI11.cmake       CLI11 -> CLI11::CLI11, linked into every tool
  FMinusTesting.cmake     Catch2 and lit
  templates/Config.h.in   -> <build>/include/fminus/Config.h
include/fminus/         the library's public API ("fminus/...")
src/                    the fminus library: every .cpp is a source, every grammar is generated in
  support/              integration helpers that came with the skeleton: LLVMInit, HostTarget (the host's
                        TargetMachine and the O0-O3 pipeline), JitRuntime (the runtime, defined in an LLJIT),
                        Aot (object and assembly files, linked executables)
  runtime/              the FMinus runtime: its own library, libfminus_runtime.a, with no LLVM in it; the C
                        functions generated code calls, listed in RuntimeSymbols.def (a placeholder ABI version)
  frontend/             FMinus v0's front end: Lexer.l, Parser.y, Ast.h (the syntax tree), Diagnostics.h,
                        Frontend.h (parse), Scanner.h (the glue between scanner and parser)
  ir/                   CodeGen: the syntax tree to LLVM IR
  jit/                  Jit: LLJIT for FMinus modules (host CPU, pipeline, runtime)
  opt/                  empty: for FMinus's own pass pipelines
tools/<name>/           one executable per directory, with CLI11 for its command line: fminus (the driver),
                        fminus-build and fminus-hostinfo
tests/
  unit/                 your Catch2 tests
  smoke/                the toolchain's checks: LLJIT + O3 on the host, Bison + Flex (smoke/calc/)
  lit/                  lit + FileCheck tests (*.test, *.fm, *.ll)
Scripts/pin-llvm.sh     the pin lines for another LLVM release
docs/                   research notes
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
| CLI11 | 2.7.2 | release source package, header-only, inside each build tree | the build tree |
| Catch2 | 3.16.1 | source archive, built inside each build tree | the build tree |

`.deps/` holds `src/` (extracted sources), `build/` (dependency build trees) and `install/`. FetchContent keeps each
tarball in the build tree that downloaded it (`_deps/<name>-tmp/`), so a second build tree downloads only the small
in-tree dependencies again.
`FMINUS_DEPS_DIR` (CMake or environment variable) moves it, for example to share one cache between checkouts.
Of the llvm-project source, only what `llvm/` builds from is kept after extraction (`llvm/`, `cmake/`, `third-party/`,
`libc/`); `FMINUS_LLVM_KEEP_ALL_SOURCES=ON` keeps it all (delete `.deps/src/llvm-project-*` to extract again). Deleting
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

```
// A soft clipper, in FMinus v0. Comments run to the end of the line.
fn softclip(x: float, drive: float) -> float {
    let y = x * drive;                  // bindings never change
    return y / (1.0 + abs(y));
}

fn chain(x: float) -> float {
    return softclip(half(x), 4.0);      // functions may be called before they are defined
}

fn half(x: float) -> float { return x / 2; }
```

| | |
|---|---|
| program | functions: `fn name(param: float, ...) -> float { statement* }` |
| statements | `let name = expr;` and `return expr;` (a function ends with a return) |
| expressions | numbers (`1`, `0.5`, `.5`, `2e3`), names, calls, `+ - * /`, unary `-`, parentheses |
| types | `float` (IEEE single precision) |
| builtins | `abs sqrt sin cos exp log floor min max` (LLVM intrinsics) |

Every FMinus function becomes an external LLVM function of the same name with the C calling convention, so C++
calls JIT'd code as `float(float, ...)`. The code generator reports every error it finds, in source order, as
`file:line:column: error: message`; the parser stops at the first syntax error.

```bash
build-dev/bin/fminus chain.fm                      # the optimised IR (-O2)
build-dev/bin/fminus chain.fm -O0                  # the IR as the code generator wrote it
build-dev/bin/fminus chain.fm --emit ast           # the syntax tree, as s-expressions
build-dev/bin/fminus chain.fm --emit asm -o -      # this machine's assembly (--emit obj: an object file)
build-dev/bin/fminus chain.fm --run chain 1        # JIT-compiles, calls chain(1), prints 0.6666667
```

How it fits together: `frontend::parse` (Lexer.l, then Parser.y) gives an `ast::Program`, and `ir::lower`
(ir/CodeGen.cpp) gives an `llvm::Module`. From there, `jit::Jit` optimises and compiles it in memory, or
`support/Aot.h` writes it to a file.

**Adding a construct**, say comparisons and `if`:

1. Its tokens: a rule in `src/frontend/Lexer.l` (keywords before `{IDENTIFIER}`), a `%token` in `Parser.y`.
2. Its grammar: a rule in `Parser.y`, with precedence if it is an operator. Bison's report is
   `build-dev/src/generated/frontend/Parser.output`, and `-Wcounterexamples` shows an input for each conflict.
3. Its node: an alternative of the `std::variant` in `src/frontend/Ast.h` (a new type: an enumerator of `ast::Type`).
   Every `std::visit` over that variant now fails to compile until it handles the new node: `dump` in `Ast.cpp` and
   the lowering in `src/ir/CodeGen.cpp`.
4. Its tests: a dump in `tests/unit/FrontendTest.cpp`, a result in `tests/unit/CodeGenTest.cpp`, IR or errors in
   `tests/lit/fminus/*.fm` (`// RUN:` lines, checked by FileCheck).

Bindings are SSA values because they never change. Assignment and loops need storage: allocas in the entry block,
which the pipeline's mem2reg turns back into SSA.

## Executables (ahead of time)

For development, a module can also be compiled ahead of time for this machine, into a native executable you can run
under lldb or Instruments, or into an object or assembly file to read. Its machine code is what LLJIT generates for the
same module, since both use the TargetMachine and pipeline in `support/HostTarget.h`.

```bash
build-dev/bin/fminus-build kernel.ll -o kernel              # an executable (the IR needs a main)
build-dev/bin/fminus-build kernel.ll --emit asm -o -        # this machine's assembly, after -O3
build-dev/bin/fminus-build kernel.ll --emit ll -O2 -o -     # the optimised IR; --emit obj for an object file
build-dev/bin/fminus-build kernel.ll -o kernel --link-arg=-framework --link-arg=Accelerate
```

From code, it is `fminus::aot::emitFile`, `linkExecutable` and `emitExecutable` in `src/support/Aot.h`. Linking goes
through the C++ compiler driver this build tree was configured with (Xcode's clang; `$FMINUS_LINK_DRIVER` overrides
it), which brings the SDK, libSystem and the ad-hoc signature arm64 needs. It links this tree's `libfminus_runtime.a`,
so JIT'd and ahead-of-time code call the same runtime. In the JIT, `fminus::defineRuntimeSymbols` defines every
function listed in `src/runtime/RuntimeSymbols.def` by its address. Under the `asan` and `tsan` presets, the
sanitizer runtimes are linked too. This is a development feature: inside a plugin, FMinus only ever JITs.

## Tests

```bash
cmake --build --preset dev --target check-fminus    # build what the tests need, then ctest
ctest --preset dev -R jit                           # some of them
build-dev/bin/fminus-tests "[smoke]"                # Catch2 directly (tags, --benchmark-samples, ...)
build-dev/bin/fminus-lit -v tests/lit               # lit directly; a source path works as it is
```

- **Catch2** (`tests/unit/`, `tests/smoke/`): one executable, `fminus-tests`; ctest runs each `TEST_CASE` on its own.
  The smoke tests cover the JIT, an executable linked ahead of time and run, and the calculator grammar.
  Catch2's `BENCHMARK` is there for timing JIT'd kernels.
- **lit** (`tests/lit/`): a file with `RUN:` lines, checked by FileCheck. `tests/lit/fminus/` drives `fminus`. FMinus's tools and LLVM's are on PATH;
  `%host_triple` is substituted; the features for `REQUIRES:` are `asserts`, `asan`, `ubsan`, `tsan`, `arm64`, `x86_64`.
  `.fm` is a placeholder extension for FMinus sources: change it in `tests/lit/lit.cfg.py`.

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
(`cmake --preset xcode && open build-xcode/FMinus.xcodeproj`). ccache or sccache is used when installed.

## Handy

- `build-dev/bin/fminus-hostinfo [--features]`: the triple, CPU, features and data layout the JIT targets here.
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
  thread, hand the function pointer to the audio thread atomically, and keep JIT'd code free of allocation and locks.
- **Size and symbols.** Static LLVM adds tens of megabytes. Export only the plugin's entry points (an
  exported-symbols list), so LLVM's symbols cannot clash with another copy of LLVM in the host process.
- **Bit-exactness across machines.** Host-tuned code can differ in its last bits between CPUs (FMA contraction,
  vector widths, fast-math). Decide what a "same output" test means early.

## License

FMinus is free software under the GNU General Public License, version 3 (`LICENSE`). Its dependencies keep their own
licenses: LLVM (Apache 2.0 with LLVM exceptions), Catch2 (BSL-1.0) and CLI11 (BSD-3-Clause). Bison and Flex run only
at build time; the code they generate is covered by Bison's skeleton exception and Flex's permissive license.
