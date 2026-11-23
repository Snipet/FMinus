# The experiment

The sources and results of the one experiment that [Desktop applications in FMinus, drawn by FunkGui](../README.md)
ran (its §12). Research code, kept as it was run on 2026-11-23: not part of FMinus's build (no CMake of the tree
reaches `docs/`), not tested by its suite, and `host.cpp` is not wrapped to 120 columns. It ran on Linux x86-64
(Ubuntu 24.04, clang 18.1.3) against FMinus at `19cbb91` and FunkGui at `b7bc1cd` (v0.27.0), and nowhere else.

| File | What it is |
|---|---|
| `ui.fm` | the binding: enumerations, eight prototypes of C functions the host defines, two helpers |
| `counter.fm` | the application: a counter, three buttons, a slider, the arrow keys, its accessibility items. `-D V2` is the same program after an edit |
| `host.cpp` | a `funkgui::Panel` that forwards to the program; the eight C functions over `funkgui::Canvas`; a scripted run on `funkgui::HeadlessHost`; a PNG encoder on zlib. With `APP_JIT` it compiles `counter.fm` itself and then reloads it as its edit |
| `CMakeLists.txt` | FunkGui (without JUCE) and FMinus as subprojects; `counter-jit` and `counter-aot` |
| `counter-jit.out`, `counter-aot.out` | one run of each, as printed |
| `counter-rest.png`, `counter-moving.png`, `counter-reloaded.png` | three of the frames `counter-jit` wrote, drawn by FunkGui's `SoftRaster` at 2 device pixels a logical one |

## Running it

It needs a FunkGui checkout, a built `fminus` (to compile `counter.fm` ahead of time), zlib's headers, and the LLVM
that FMinus's own build left in `.deps/`, so that the subproject does not build LLVM again:

```bash
cmake --workflow --preset release                       # FMinus itself: bin/fminus, and LLVM in .deps/
git clone https://github.com/Snipet/FunkGui.git ../FunkGui
llvm=$(ls -d "$PWD"/.deps/install/llvm-*-Release-* | head -1)
cmake -S docs/research/desktop/experiments -B ../desktop-experiment -G Ninja -DCMAKE_BUILD_TYPE=Release \
      -DCMAKE_C_COMPILER=clang -DCMAKE_CXX_COMPILER=clang++ \
      -DFUNKGUI_DIR="$PWD/../FunkGui" -DFMINUS_EXE="$PWD/build-release/bin/fminus" \
      -DFMINUS_LLVM_MODE=system -DLLVM_DIR="$llvm/lib/cmake/llvm" -DFMINUS_DEPS_DIR="$PWD/.deps"
cmake --build ../desktop-experiment --target counter-aot counter-jit
mkdir -p ../desktop-experiment/out
../desktop-experiment/counter-aot ../desktop-experiment/out
../desktop-experiment/counter-jit docs/research/desktop/experiments ../desktop-experiment/out
```

`counter-aot` builds in a minute or two (FunkGui's core, once). `counter-jit` also compiles FMinus's library in
that tree, a few minutes more. Each prints what the `.out` files hold and writes its frames as PNGs into the
directory it is given.

Any LLVM flavor in `.deps/install/` will do for the subproject. The `CMakeLists.txt` has a branch for macOS's linker
(`-dead_strip` for `--gc-sections`) that has never been run: the experiment ran on Linux only, and `host.cpp`
exports its `ui_*` functions for the JIT the way ELF does it.

## Reading the output

```
jit:                   counter.fm compiled and linked in 78.0 ms (-O2)
jit-1-rest              43 primitives, 0 missing glyphs, geometry 9c685e45af10bec6, text b7df00428ff91004, 720x400 px 91afce4f5f4500b9
```

A frame's line is what FunkGui's tests pin: how many primitives the Canvas recorded, the fingerprint of its geometry
and of its text (`funkgui::fingerprint`), and a hash of the rasterised picture. `counter-jit.out` and
`counter-aot.out` have the same three lines for the same three frames, and the PNG files of the two runs are the same
bytes: the JIT's code and the object file's draw the same thing.

The times vary from run to run. The report's ranges are from eight runs of each on the idle machine; the `.out`
files are one run, from the slow end (1.00 and 1.07 µs a frame, where the median was 0.7).

## What the reload does

`counter-jit` compiles `counter.fm` a second time with `V2` defined while the first program's instance is live, which
stands for an edit: one field more in `App` (`total`), a taller window, a wider slider. `carryOver` in `host.cpp`
walks the new module's fields (`fminus::Program::module`), copies each from the old instance's field of the same
name and type, a nested module field by field, and leaves the rest at 0. Then it calls the program's `app_relayout`,
since positions are computed and not state. `counter-reloaded.png` is the result after one more click: the count went
on from 15 to 21 by the step the slider had been dragged to, and the new field counted that click alone.
