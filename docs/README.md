# FMinus documentation

| Document | What it covers |
|---|---|
| [architecture.md](architecture.md) | The compiler from source text to machine code: every stage, the file that implements it, what passes between stages, how C's semantics map onto LLVM, and the invariants that keep it extensible |
| [programs.md](programs.md) | Writing a whole program: `float main(...)`, running it in the JIT, building an executable, and how the compiler gives an executable its C entry point |
| [extending.md](extending.md) | How to grow the C subset: the change checklist, the worked and verified recipes (a builtin and `%`, now in the base; `bool` with comparisons, `&&` `\|\|` and `?:`; `if` `while` `for` `break` `continue`; a runtime function; an LLVM pass), the pitfalls, and design notes for what comes next (integers, pointers, structs, state) |
| [simd.md](simd.md) | SIMD, in detail: the hardware and LLVM's vector model, then three verified recipes: `float2`/`float4`/`float8` (OpenCL C's vectors), a vector math library, and `poly float`, a vector as wide as the machine's; scalable vectors (SVE), the auto-vectorisers, pitfalls, next steps |
| [building.md](building.md) | Presets, the dependency cache and LLVM flavors, adding sources, tools, examples, tests and dependencies, warnings and sanitizers, troubleshooting |
| [testing.md](testing.md) | The test layers (Catch2, lit + FileCheck, example harnesses) and how to write each; inspecting and debugging the compiler's output with `fminus`, `opt`, `llc`, `llvm-mca` and lldb |
| [recipes/](recipes/) | the recipes of extending.md and simd.md as patches, in order |

To grow the language, read architecture.md, then extending.md, then simd.md. Use building.md and testing.md as
reference. To write programs in it, read programs.md.

Elsewhere:

- [../README.md](../README.md) is the quick reference: the language, the commands, the presets.
- [../examples/biquad/README.md](../examples/biquad/README.md) plans the language's growth around a DSP kernel.
- [../examples/note/note.fm](../examples/note/note.fm) is a whole program, with a `main`.
