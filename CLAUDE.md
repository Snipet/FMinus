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
- The smoke tests (`tests/smoke/`, `tests/lit/smoke/`) prove the toolchain. Keep them passing until the user retires
  them.
- `docs/` is the manual (architecture, extending, building, testing; `docs/recipes/` patches). Keep it in step with
  the code it describes, and re-verify a recipe before changing it (apply, build, test).
- Commit, push or tag only when the user asks.
