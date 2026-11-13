# FMinus: notes for Claude sessions

FMinus is the user's own research project: a JIT compiler for audio DSP (Flex + Bison front end, LLVM IR, ORC LLJIT,
compiled on the user's machine). **The user writes the compiler.** Claude wrote FMinus v0 at the user's request
(2026-10-16), and on 2026-10-17 made it a subset of C, with C's syntax and semantics and clang's error wording (the
README lists the deliberate deviations). It goes all the way through (src/frontend, src/ir, src/jit, tools/fminus), for
the user to expand. New features follow C unless the user decides otherwise. Help when asked: explain, review, debug,
fix the build or the toolchain. Do not write or rewrite code under `src/`, `include/` or `tools/fminus/` unless the user
asks for that specific change, and ask before restructuring anything.

- Build: `cmake --workflow --preset dev` (README.md has every preset). Build trees are `build-<preset>/`; the
  dependency cache is `.deps/` (or `FMINUS_DEPS_DIR`). A first configure builds LLVM (10–30 min): say so before
  starting one, and never delete `.deps/` without asking.
- Dependencies come only through FetchContent, pinned in `cmake/FMinusPins.cmake` (version, URL, SHA-256). Keep it
  that way; a new dependency gets a pin line and its own cmake/FMinus*.cmake.
- Adding a source, grammar, tool or test edits no CMake (globs; see README.md "Layout").
- `fminus_target_defaults()` gives every FMinus target C++23, the warnings (`-Werror` unless `FMINUS_WERROR=OFF`) and
  the sanitizers. Dependencies keep their own flags.
- `src/runtime/` is its own library (no LLVM): what generated code calls, JIT'd (`support/JitRuntime.h`) or linked
  into an executable (`support/Aot.h`, `tools/fminus-build`). Every runtime function goes in `RuntimeSymbols.def`.
- `src/circuit/` is its own library too, `fminus_circuit` (no LLVM, no grammar): the circuit compiler the user asked
  for (2026-10-29), SPICE-style netlists to FMinus source (`include/fminus/Circuit.h`, C++17; `docs/circuits.md`, Phase
  1 as built, with what is not built yet at its end; the research in `docs/research/analog-circuits/`). Its linear
  blocks' designs and the method check are `Linear.*`, on doubles at generation and on `Val`s for the text, as
  `Mna.h`'s derivation is. It compiles with `-ffp-contract=off`, so the text it prints is the
  same on every machine; `fminus_lib` links it. A name its emitted module uses goes in `isModuleWord` (Plan.cpp). Its
  self-test, `selfTest()`, needs the JIT, so it lives in `fminus_lib` (`src/api/CircuitSelfTest.cpp`, which compiles
  candidates through `fminus::Program`); its arithmetic stays in `src/circuit/Search.*`, which stays LLVM-free.
- `python/` is the Python package, `import fminus` (the user asked for it, 2026-11-09; `docs/python.md`): an extension
  module on the public API alone (`python/src/`, pybind11, C++17) and the Python around it (`python/fminus/`), built
  only with `FMINUS_BUILD_PYTHON=ON` (the `python` preset; `pyproject.toml` for pip), tested by `python/tests/`
  (unittest; the ctest test `python`). It binds `FMinus.h` only: `docs/python.md` ends with what it leaves out. Its
  wheels are `.github/workflows/wheels.yml`'s (cibuildwheel, set up in `pyproject.toml`; run by hand, and it uploads
  to PyPI only when its `publish` input says so).
- A string literal is a `string` where one is wanted (the syntax steps, below) and otherwise a builtin's argument (the
  user asked for the web research's findings to be built, 2026-11-11; `docs/research/web/`): a format, a JSON Pointer,
  an XML path or a URL, checked and prepared when the program is compiled (`src/ir/Strings.cpp`, `Paths.cpp`,
  `Web.cpp`). The JSON and XML parsers (`FMINUS_JSON`, `FMINUS_XML`, both ON; yyjson, pugixml, fast_float) are compiled
  into the runtime. A feature a build leaves out keeps its builtins as known names, with a "this build leaves out"
  error, and its macro (`__FMINUS_JSON__`, ...) undefined.
- `src/web/` is its own library too, `fminus_web` (no LLVM), built only with `FMINUS_WEB=ON` (OFF by default; the `web`
  preset; `docs/web.md`): the HTTP(S) client behind the `http_*` builtins, on libcurl and Mbed TLS (Schannel on
  Windows), which `cmake/FMinusWeb.cmake` builds at configure time into the deps cache. Every function generated code
  calls goes in `WebSymbols.def`. A program's requests reach only what `Program::Options::web` allows (none by
  default), and no test may leave loopback. Its macOS and Windows code has never been built or run: say so before
  relying on it, and `docs/web.md` ("What is verified where") lists what has.
- The web library's server (the user asked for it, 2026-11-11; `docs/research/web/server.md`) is `FMINUS_WEB_SERVER`
  (OFF by default, needs `FMINUS_WEB`; the `web` preset turns it on): llhttp, pinned, compiled into `fminus_web`, and
  FMinus's own sockets (`src/web/Server.cpp`, `Sockets*.cpp`). A program listens only where
  `Program::Options::web.server` says (nowhere by default), and no test may bind anything but a loopback address. What
  a `[[clang::nonblocking]]` server builtin calls stays wait-free in the library's code, proved by `HeapCalls` and
  seccomp's strict mode (`tests/unit/WebServerTest.cpp`, and through the JIT in `WebServeTest.cpp`). Its sockets on
  macOS and Windows have never been built.
- The file interface (the user asked for it, 2026-11-11; `docs/files.md`, the research in `docs/research/files/`) is
  part of the runtime (`src/runtime/Files*.cpp`; `Files.h` is the host's half) and the compiler (`src/ir/Files.cpp`):
  `FMINUS_FILES` (ON), `FMINUS_FILES_WRITE` and `FMINUS_FILES_ANYWHERE` (OFF, need `FMINUS_FILES`), and the ceilings
  `FMINUS_FILES_TYPES` and `FMINUS_FILES_MAX_BYTES`, which no policy raises; the `files` preset turns writing and
  anywhere on. A build without an option compiles none of the file interface's code it gates. A program reaches only
  the folders `Program::Options::files` maps (none by default), every operation through one function
  (`Context::State::reach`), which never trusts a path's text: it walks a segment at a time beneath the folder's
  handle, follows no link, and compares each directory by identity with the denied ones. No test may touch anything
  outside a directory it made (beside `bin/`, or lit's `%t`), except `/dev/null` and `/proc` named as what must be
  refused. Its macOS and Windows code has been compiled (the Wheels workflow builds it on both, 2026-11-12) and never
  run: say so before relying on it.
- The syntax for networked programs (the user asked for the research, 2026-11-12, and for its five steps to be built,
  2026-11-13; `docs/research/syntax/`, `docs/extending.md`'s recipes 44 to 51): C's `defer` and `if` and `switch` with a
  declaration, typed handles (`http_request`, `json_doc`, ...), and FMinus's own `serve`, `route`, `reply`, `within` and
  `when`; a `string` type, a read-only view (text is read as a value and made only in sinks: nothing joins two strings
  or builds one), with `char` arrays to keep text in; `{name}` holes in URLs, file names and routes; a JSON literal, an
  argument and never a value, written by the runtime's writer (`src/runtime/JsonWrite.cpp`), and `json_read`, each by a
  shape table that `src/ir/Shapes.cpp` makes. A new keyword goes in `isFMinusKeyword` (`src/circuit/Emit.cpp`) too: a
  test checks that list against the scanner. A call that the loop budget or a bounds check stops runs no deferred
  statement and no `reply`; a `within` it stops abandons its scope (`src/opt/EarlyReturn.cpp`). A runtime function
  handed an array of pointers reads memory that is not its arguments': say so in `kRuntimeFacts`. No preset builds a
  tree without JSON, XML or the file interface, nor the web library without its server, nor the server without JSON:
  build those after a change to anything a feature gates (`docs/testing.md`, "Builds that leave features out").
- The smoke tests (`tests/smoke/`, `tests/lit/smoke/`) prove the toolchain. Keep them passing until the user retires
  them.
- `docs/` is the manual (architecture, extending, building, testing; `docs/recipes/` patches). Keep it in step with
  the code it describes, and re-verify a recipe before changing it (apply, build, test).
- Commit, push or tag only when the user asks.
