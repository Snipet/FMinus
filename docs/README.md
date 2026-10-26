# FMinus documentation

| Document | What it covers |
|---|---|
| [architecture.md](architecture.md) | The compiler from source text to machine code: every stage, the file that implements it, what passes between stages, how C's semantics map onto LLVM, and the invariants that keep it extensible |
| [programs.md](programs.md) | Writing a whole program: `float main(...)`, running it in the JIT, building an executable, and how the compiler gives an executable its C entry point |
| [extending.md](extending.md) | How to grow the C subset: the change checklist, the worked and verified recipes (a builtin and `%`; `bool` with comparisons, `&&` `\|\|` and `?:`; `if` `while` `for` `break` `continue`; `int`, `void` and pointers; the heap, and clang's `[[clang::nonblocking]]` checked to keep it off the audio thread; arrays and modules, FMinus's classes; an FFT from a C library built into the runtime; parameters, the host's values, read as a knob, a stream or a frozen constant; then the compiler's own: math that gives a float one result wherever it is computed, knob versioning and flat streams, C's strict aliasing told to LLVM, zero-init elision, what the runtime's calls and loops promise, and subnormals flushed for the audio thread; `#include`, a program in several files; and `parallel for` and `parallel { }`, a program's own threads on a pool, its races refused, all in the compiler now; a runtime function and an LLVM pass, as a patch), the pitfalls, and design notes for what comes next (a type checker, more of arrays, modules, parameters, parallel regions, knob versioning, the math modes and zero-init elision, the host's allocator) |
| [simd.md](simd.md) | SIMD, in detail: the hardware and LLVM's vector model, then the three recipes that are FMinus's SIMD, a guide to their code: `float2`/`float4`/`float8` (OpenCL C's vectors), a vector math library (now the fast math mode), and `poly float`, a vector as wide as the machine's; scalable vectors (SVE), the auto-vectorisers, pitfalls, next steps |
| [building.md](building.md) | Presets, the dependency cache and LLVM flavors, adding sources, tools, examples, tests and dependencies, warnings and sanitizers, Windows, troubleshooting |
| [testing.md](testing.md) | The test layers (Catch2, lit + FileCheck, example harnesses, which call a block kernel once) and how to write each; inspecting and debugging the compiler's output with `fminus`, `opt`, `llc`, `llvm-mca` and lldb |
| [recipes/](recipes/) | the recipes that aren't in the compiler (a runtime function, a pass), as a patch |

To grow the language, read architecture.md, then extending.md, then simd.md. Use building.md and testing.md as
reference. To write programs in it, read programs.md.

Elsewhere:

- [../README.md](../README.md) is the quick reference: the language, using FMinus as a C++ library
  (`include/fminus/FMinus.h`), the commands, the presets.
- [../examples/biquad/README.md](../examples/biquad/README.md) plans the language's growth around a DSP kernel, which
  processes a block of samples in one call.
- [../examples/note/note.fm](../examples/note/note.fm) is a whole program, with a `main`.
- [../examples/](../examples/) shows each feature in use: `waveforms/` (loops), `chord/` (`float4` and the vector
  math library), `unison/` (`poly float`), `comparison/` (a `bool main`), `biquad/` (a module filtering a block a
  call, its cutoff a stream), `delay/` (the heap, and a kernel that can't allocate, in plain C and as a module),
  `saturator/` and `meter/` (`poly float` through a buffer, against plain loops: a stateless tanh, and reductions;
  each a module with a knob), `voices/` (modules, in memory a host of the public API allocates, their controls knobs),
  `convolver/` (the FFT: a convolution by overlap-add), `parameters/` (a filter's cutoff swept through a knob or a
  stream, or frozen), `plugin/` (a JUCE plugin whose DSP is an FMinus program: a stream, knobs and a frozen sample
  rate, compiled in `prepareToPlay`, its own CMake project).
