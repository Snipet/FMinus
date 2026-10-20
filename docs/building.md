# Building

FMinus's build: presets, the dependency cache, LLVM flavors, and how to add sources, tools, examples, tests and
dependencies. The build itself is `CMakeLists.txt` and `cmake/FMinus*.cmake`; each module's header comment documents
its options.

## Requirements

macOS on Apple silicon with Xcode's command line tools (clang, make, m4), CMake 3.30 or later, Ninja, and Python 3
for lit. Nothing else needs to be installed: LLVM, Bison, Flex, CLI11 and Catch2 are all fetched and built by the
build itself. The first configure needs the network.

On Windows: Visual Studio 2022 or later (or its Build Tools) with the "Desktop development with C++" workload and
its "C++ Clang tools for Windows" (clang-cl; "C++ Clang Compiler for Windows" under Individual components), CMake 3.30
or later, Ninja, and Python 3. Configure and build from a developer prompt for x64: "x64 Native Tools Command Prompt
for VS" in the Start menu, or Visual Studio's `Common7\Tools\Launch-VsDevShell.ps1 -Arch amd64` in PowerShell. Its
environment names the C runtime's and the Windows SDK's headers and libraries; a plain prompt, or the x86 one, is
refused at configure time. The commands are the same; [Windows](#windows) has what differs.

## Presets

```bash
cmake --workflow --preset dev        # configure, build, test
cmake --preset dev                   # configure only
cmake --build --preset dev           # build
ctest --preset dev                   # test
```

| Preset | Build type | LLVM | Sanitizers | For |
|---|---|---|---|---|
| `dev` | RelWithDebInfo | Release + assertions | none | everyday work |
| `debug` | Debug | Release + assertions | none | stepping through FMinus in lldb |
| `release` | Release | Release, no assertions | none | timing (examples/biquad) |
| `asan` | Debug | Release + assertions | address, undefined | memory errors in FMinus's code |
| `tsan` | RelWithDebInfo | Release + assertions | thread | compiling on another thread |
| `inline` | RelWithDebInfo | built inside the tree, -O2 -g | none | stepping into LLVM itself |
| `xcode` | (multi-config) | Release + assertions | none | Xcode's debugger and Instruments |

Each preset builds in `build-<preset>/`, exports `compile_commands.json` (`.clangd` reads `build-dev`'s), and has a
workflow, except `xcode`: run `cmake --preset xcode`, then `open build-xcode/FMinus.xcodeproj`. Sanitizers
instrument FMinus's own code only; LLVM stays uninstrumented, which is why the `asan` test preset sets
`detect_container_overflow=0`.

## The dependency cache

Everything built from a dependency lives in `.deps/` (or wherever `FMINUS_DEPS_DIR` points, as a CMake or
environment variable), shared by every build tree of the checkout:

```
.deps/
  src/llvm-project-23.1.3/     extracted, then pruned to what llvm/ builds from (about 300 MB)
  src/bison-3.8.2/  src/flex-2.6.4/
  build/<flavor>/              LLVM's build tree: about 1 GB, safe to delete once installed
  install/<flavor>/            LLVM: headers, static libraries, CMake package, tools, lit
  install/bison-3.8.2/  install/flex-2.6.4/
  src/winflexbison-2.5.25-a243a1c/  install/winflexbison-2.5.25-a243a1c/     Windows, in place of Bison and Flex
```

The cache saves work only: deleting any part of it is safe, and the next configure rebuilds what is missing.
FetchContent keeps each downloaded tarball in the build tree that fetched it (`_deps/<name>-tmp/`). CLI11 and
Catch2 are small and built inside every build tree.

## LLVM

`cmake/FMinusLLVM.cmake` provides LLVM in one of three modes (`FMINUS_LLVM_MODE`):

- **`cache`**, the default. The first configure that asks for a *flavor* not yet built builds and installs it, which
  takes 10–30 minutes with the output streaming. A flavor is LLVM's build type, assertions, targets and options,
  and its install directory is named after a fingerprint of them, for example
  `llvm-23.1.3-Release-assert-b92e05cbab`. Every later configure finds the install in seconds. The `dev`, `debug`,
  `asan`, `tsan` and `xcode` presets share one flavor; `release` builds a second, without assertions.
- **`inline`**. LLVM is a subproject of the build tree: Ninja tracks it, and a debugger steps into it with its
  sources. Every such build tree builds its own copy.
- **`system`**. An LLVM you have already, through `-DLLVM_DIR=<prefix>/lib/cmake/llvm`. An install from this cache
  works, and its lit is found too.

Options (each changes the flavor):

| Option | Default | |
|---|---|---|
| `FMINUS_LLVM_ASSERTIONS` | ON except in Release | LLVM's assertions: slower compiles, earlier failures on bad IR |
| `FMINUS_LLVM_BUILD_TYPE` | Release | LLVM's own build type (cache mode) |
| `FMINUS_LLVM_TARGETS` | `host` | `LLVM_TARGETS_TO_BUILD`, for example `AArch64;X86` |
| `FMINUS_LLVM_TOOLS` | FileCheck not count opt llc llvm-as llvm-dis llvm-link llvm-mca llvm-objdump llvm-config | built and installed with the flavor |
| `FMINUS_LLVM_EXTRA_OPTIONS` | (empty) | more `NAME=VALUE` LLVM settings; a value cannot contain `;` |
| `FMINUS_LLVM_COMPONENTS` | Core OrcJIT Passes IRReader Support TargetParser AllTargets* | what `FMinus::LLVM` links (doesn't change the flavor) |
| `FMINUS_LLVM_KEEP_ALL_SOURCES` | OFF | keep all of llvm-project's sources, not only what `llvm/` builds from |

**Bumping LLVM:** `Scripts/pin-llvm.sh 23.1.4` prints the new version and SHA-256 lines from the GitHub release.
Paste them into `cmake/FMinusPins.cmake` and configure; a new flavor builds. LLVM's C++ API changes between
releases, so expect compile errors in `src/` and the tests. `getTerminator()` in LLVM 23 (extending.md) is an
example of a semantic change that compiles fine.

**Adding an LLVM component:** when code needs a library that `FMinus::LLVM` doesn't link (`undefined symbol:
llvm::...` at link time), add its component to `FMINUS_LLVM_COMPONENTS` in `cmake/FMinusLLVM.cmake`:
`MCJIT`, `ObjCARCOpts`, `Coroutines`, ... `llvm-config --components` lists them, and it is in every flavor's
`bin/`.

## Bison and Flex

`cmake/FMinusParserTools.cmake` builds GNU Bison 3.8.2 and Flex 2.6.4 from source at the first configure (about a
minute), and provides `fminus_add_grammar`. `FMINUS_PARSER_TOOLS_MODE=system` uses Bison 3.8 or later and Flex 2.6
or later from PATH instead; macOS's own `/usr/bin/bison` is 2.3, too old for the C++ skeleton.

Their release tarballs build with `./configure && make`, which Windows has no shell or make for. On Windows the build
fetches [WinFlexBison](https://github.com/lexxmark/winflexbison) instead: the same Bison 3.8.2 and Flex 2.6.4,
ported to Windows as `win_bison.exe` and `win_flex.exe`, with m4 built in. It is a CMake project, built at the first
configure with this tree's C compiler and installed in the deps cache like the others. The pin is a commit after
release 2.5.25: in 2.5.25, two of the tools running at once (Ninja runs the parser's and the scanner's together)
could write the same temporary file. `FMINUS_PARSER_TOOLS_MODE=system` finds `win_bison` and `win_flex` on PATH too.

Bison runs with `-Wall -Wcounterexamples` (`FMINUS_BISON_FLAGS`). Its report, `Parser.output`, is in
`build-dev/src/generated/frontend/`. The generated parser and scanner are compiled with the project's warnings,
except one: Bison's own C++ skeleton sets a counter it never reads (`yynerrs_`), so `-Wunused-but-set-variable` is
off for generated parsers.

## Adding things

Every source list is a glob, so adding a file never edits CMake. Configure picks a new file up on the next build
(the globs use `CONFIGURE_DEPENDS`).

| To add | Put it in | Becomes |
|---|---|---|
| a compiler source or header | `src/<dir>/` | part of `libfminus.a`; include it as `"<dir>/File.h"` |
| a grammar | `src/<dir>/X.y` (one per directory), scanners beside it as `*.l` | generated into the library: `"<dir>/X.tab.hpp"`, `"<dir>/Y.yy.hpp"` |
| a runtime function | `src/runtime/` (and a line in `RuntimeSymbols.def`) | part of `libfminus_runtime.a` |
| a tool | `tools/<name>/*.cpp` | the executable `bin/<name>`, linked with `FMinus::Internal` and CLI11 |
| an example | `examples/<name>/*.cpp`, and its FMinus sources | `bin/example-<name>`, told its directory as `FMINUS_EXAMPLE_DIR`, with a ctest `example.<name>` running `--check` |
| a unit test | `tests/unit/*.cpp` | part of `fminus-tests`; each `TEST_CASE` is a ctest test |
| a lit test | `tests/lit/**/*.{fm,ll,test}` | part of the lit suite |
| a public header | `include/fminus/` | `"fminus/..."`, for code embedding FMinus (FMinus.h: C++17, no LLVM) |
| a test of the public API | `tests/library/*.cpp` | part of `fminus-library-tests` (C++17, `FMinus::FMinus` alone) |

A grammar outside `src/` (a test's, say) uses the helper directly, as `tests/CMakeLists.txt` does for the
calculator:

```cmake
fminus_add_grammar(<target> PARSER x.y LEXERS x.l [BASE_DIR dir] [OUTPUT_DIR dir] [SCOPE PUBLIC|PRIVATE]
                   [BISON_FLAGS ...] [FLEX_FLAGS ...])
```

Every FMinus target gets `fminus_target_defaults(<target> [CXX_STANDARD <n>])`: C++23 (or C++<n>), the warnings,
`-Werror` (`FMINUS_WERROR`), and the sanitizers. The standard is the target's own and never reaches its users, except
through an interface library (`FMinus::Internal`'s C++23): `FMinus::FMinus` asks its users for C++17, what the public
header needs.

## Adding a dependency

Every dependency comes through FetchContent, pinned by version, URL and SHA-256. That's the project's rule.

1. **Pin it** in `cmake/FMinusPins.cmake`: `FMINUS_<NAME>_VERSION`, `_URL` and `_SHA256`. A GitHub release asset
   shows its digest (`gh api repos/<owner>/<repo>/releases/tags/<tag> --jq '.assets[] | [.name, .digest]'`). For a
   source archive, hash a first download and check it against something the project publishes, such as a release
   asset or a signature.
2. **Fetch it** in a module of its own, `cmake/FMinus<Name>.cmake`. `cmake/FMinusCLI11.cmake` is the template for a
   CMake project:

   ```cmake
   set(CLI11_BUILD_TESTS OFF)            # the dependency's own options, before MakeAvailable
   FetchContent_Declare(CLI11
     URL "${FMINUS_CLI11_URL}"
     URL_HASH SHA256=${FMINUS_CLI11_SHA256}
     DOWNLOAD_EXTRACT_TIMESTAMP TRUE
     SYSTEM                              # its headers are system headers: our warnings stay out of them
     EXCLUDE_FROM_ALL)                   # built only when something links it
   FetchContent_MakeAvailable(CLI11)
   ```

   An autotools tarball builds once into the deps cache: see `_fminus_build_autotool` in
   `cmake/FMinusParserTools.cmake`. Its `DOWNLOAD_EXTRACT_TIMESTAMP TRUE` matters there, because make compares the
   archive's timestamps. A tool that is a CMake project of its own builds there the same way, with CMake:
   `_fminus_build_winflexbison`, beside it.
3. **Include the module** from `CMakeLists.txt`, under the option that needs it, and link its target where it's used.
4. **Note it** in README.md's dependency table and its License section.

## Warnings, sanitizers, tools

- **Warnings.** `-Wall -Wextra -Wpedantic -Wshadow -Wnon-virtual-dtor -Woverloaded-virtual -Wcast-align
  -Wnull-dereference -Wdouble-promotion -Wformat=2 -Wimplicit-fallthrough`, with `-Werror` unless
  `-DFMINUS_WERROR=OFF`. `-Wswitch` (in `-Wall`) is what makes extending the AST safe. `-Wdouble-promotion` catches a
  `float` silently widened to `double`, a common DSP slip. With clang-cl, `/W4` stands for `-Wall -Wextra`
  ([Windows](#windows)).
- **Sanitizers.** `FMINUS_SANITIZERS` is a list of `address`, `undefined` and `thread` (address and thread don't
  combine). AOT executables are linked with the same sanitizers, because the runtime library is instrumented too.
  RealtimeSanitizer is not available in Apple's clang. The sanitizers are not set up for clang-cl, on Windows.
- **ccache or sccache** is used when installed.
- **Examples' reference code** is built with `-ffp-contract=off` (`examples/CMakeLists.txt`): FMinus's IR fuses
  nothing, so the C++ reference must not either, or the outputs stop matching bit for bit.

## Windows

FMinus builds on Windows with clang-cl, clang's driver for MSVC's command line, through the same presets and
commands (`cmake --workflow --preset dev`), except `asan`, `tsan` and `xcode`. What differs:

- **The compiler.** When no compiler was chosen (`CC`, `CXX`, `-DCMAKE_CXX_COMPILER`), configure finds clang-cl
  for C and C++ (`cmake/FMinusClangCl.cmake`): first in the developer prompt's own Visual Studio, whose clang-cl
  matches its C++ library, then on PATH, then in LLVM's installer's directory. It first checks that the shell is an
  x64 developer prompt, which a build needs at every link (LLVM's, half an hour in, first). FMinus needs clang: its
  tests and examples use clang's `ext_vector_type`, which FMinus's `float2`, `float4` and `float8` match. MSVC's own
  `cl` is refused at configure time, with the fix. A Visual Studio generator, instead of the presets' Ninja, takes
  `-T ClangCL`.
- **Bison and Flex** come from WinFlexBison, built with CMake ([Bison and Flex](#bison-and-flex)).
- **The C runtime.** Every configuration, Debug included, uses the release C runtime (`/MD`:
  `CMAKE_MSVC_RUNTIME_LIBRARY` is `MultiThreadedDLL`), which LLVM's libraries are built against too: the debug one
  (`/MDd`) has other iterators and another allocator, and the two do not link together. Catch2 and an inline LLVM
  follow it, and a cache-mode flavor records it.
- **Warnings.** clang-cl reads `-Wall` as cl's `/Wall`, which is clang's `-Weverything`, so FMinus's list starts from
  `/W4`, which is clang's `-Wall -Wextra`; the rest are clang's own flags, as elsewhere. `_CRT_SECURE_NO_WARNINGS` and
  `_CRT_NONSTDC_NO_WARNINGS` turn off the C runtime's deprecations of standard functions such as `getenv` and of
  POSIX names such as `fileno`. The examples' `-ffp-contract=off` goes through clang-cl as
  `/clang:-ffp-contract=off`.
- **Sanitizers** are not set up for clang-cl: `FMINUS_SANITIZERS` is an error.
- **Executables.** `fminus --emit exe` and `fminus-build` link through clang-cl, which adds `.exe` to a name without
  an extension (`-o note` writes `note.exe`) and needs the developer prompt's `LIB` to find the C runtime. The Windows
  C runtime defines `printf` inline in `<stdio.h>`, so IR that calls it needs
  `--link-arg=legacy_stdio_definitions.lib` (`tests/lit/smoke/build-exe.ll`).
- **The heap.** The Windows C runtime has no `aligned_alloc`. FMinus's `malloc` and `calloc` come from
  `_aligned_malloc` there, and only `_aligned_free` releases that: a host frees FMinus's memory with `_aligned_free`
  or FMinus's `free`, and FMinus's `free` cannot release the host's `malloc`'s (`src/runtime/Runtime.h`).
- **lit** runs through Python, `python build-dev\bin\fminus-lit -v tests\lit` (ctest does that itself). The tests of
  Apple's arm64 assembly (`REQUIRES: arm64`) and of its vector math library (`system-darwin`) are skipped; Windows
  names its architectures `AMD64` and `ARM64`, and its system `system-windows`.

## Troubleshooting

| Symptom | Fix |
|---|---|
| The first configure sits in "building LLVM" for a long time | it is building a flavor (10–30 minutes, once); the output streams |
| A configure builds LLVM again unexpectedly | an option that changes the flavor changed (assertions, targets, deployment target, extra options); the old flavor stays in `.deps/install/` until you delete it |
| `Could NOT find BISON: Found unsuitable version "2.3", but required is at least "3.8"` | `FMINUS_PARSER_TOOLS_MODE=system` found macOS's own bison: use the default `fetch`, or put Homebrew's bison first on PATH |
| `ld: undefined symbol: llvm::...` | a missing LLVM component: add it to `FMINUS_LLVM_COMPONENTS` |
| lit says "run FMinus's lit tests through bin/fminus-lit" | lit was started directly; use `build-dev/bin/fminus-lit` (or ctest), which loads the build tree's site configuration |
| clangd can't find headers | configure the dev preset once (`cmake --preset dev`), or point `.clangd` at another build tree |
| `asan` reports container-overflow inside LLVM | run the asan test preset (`ctest --preset asan`), which sets `ASAN_OPTIONS=detect_container_overflow=0` |
| An example's `--check` fails after a change in fast-math or contraction | the C++ reference no longer computes the same thing: change it in step, or raise `--tolerance` |
| Windows: `FMinus: on Windows, configure from a Visual Studio developer prompt for x64` | open "x64 Native Tools Command Prompt for VS" from the Start menu, `cd` to the checkout, delete `build-<preset>/`, and run the command again |
| Windows: `FMinus: no clang-cl` | add "C++ Clang tools for Windows" to Visual Studio (Visual Studio Installer, Modify), open a new developer prompt, delete `build-<preset>/`, and configure again |
| Windows: `CMake 3.30 or higher is required` in a developer prompt | the prompt puts Visual Studio's own CMake first on PATH; update Visual Studio, or run a newer CMake by its full path |
| Windows: `FMinus: ... is MSVC's cl; FMinus builds with clang` | install Visual Studio's "C++ Clang tools for Windows", and configure a fresh build tree (delete `build-<preset>/`) from a developer prompt |
| Windows: `WinFlexBison ...: build failed` | the log it names has the compiler's error; WinFlexBison builds with this tree's C compiler, so the same prompt and clang-cl |
| Windows: `LNK2038: mismatch detected for 'RuntimeLibrary'` | something asked for the debug C runtime (`/MDd`); FMinus and LLVM use the release one, `/MD`, in every configuration ([Windows](#windows)) |
| Windows: `--emit exe` fails with `LNK1104: cannot open file 'msvcrt.lib'` | run from an x64 developer prompt, whose `LIB` names the C runtime's and the Windows SDK's libraries |
