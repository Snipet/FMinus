# Building

FMinus's build: presets, the dependency cache, LLVM flavors, and how to add sources, tools, examples, tests and
dependencies. The build itself is `CMakeLists.txt` and `cmake/FMinus*.cmake`; each module's header comment documents
its options.

## Requirements

macOS on Apple silicon with Xcode's command line tools (clang, make, m4), CMake 3.30 or later, Ninja, and Python 3
for lit. Nothing else needs to be installed: LLVM, Bison, Flex, CLI11 and Catch2 are all fetched and built by the
build itself. The first configure needs the network.

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
| a public header | `include/fminus/` | `"fminus/..."`, for code embedding FMinus |

A grammar outside `src/` (a test's, say) uses the helper directly, as `tests/CMakeLists.txt` does for the
calculator:

```cmake
fminus_add_grammar(<target> PARSER x.y LEXERS x.l [BASE_DIR dir] [OUTPUT_DIR dir] [SCOPE PUBLIC|PRIVATE]
                   [BISON_FLAGS ...] [FLEX_FLAGS ...])
```

Every FMinus target gets `fminus_target_defaults(<target>)`: C++23, the warnings, `-Werror` (`FMINUS_WERROR`), and
the sanitizers.

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
   archive's timestamps.
3. **Include the module** from `CMakeLists.txt`, under the option that needs it, and link its target where it's used.
4. **Note it** in README.md's dependency table and its License section.

## Warnings, sanitizers, tools

- **Warnings.** `-Wall -Wextra -Wpedantic -Wshadow -Wnon-virtual-dtor -Woverloaded-virtual -Wcast-align
  -Wnull-dereference -Wdouble-promotion -Wformat=2 -Wimplicit-fallthrough`, with `-Werror` unless
  `-DFMINUS_WERROR=OFF`. `-Wswitch` (in `-Wall`) is what makes extending the AST safe. `-Wdouble-promotion` catches a
  `float` silently widened to `double`, a common DSP slip.
- **Sanitizers.** `FMINUS_SANITIZERS` is a list of `address`, `undefined` and `thread` (address and thread don't
  combine). AOT executables are linked with the same sanitizers, because the runtime library is instrumented too.
  RealtimeSanitizer is not available in Apple's clang.
- **ccache or sccache** is used when installed.
- **Examples' reference code** is built with `-ffp-contract=off` (`examples/CMakeLists.txt`): FMinus's IR fuses
  nothing, so the C++ reference must not either, or the outputs stop matching bit for bit.

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
