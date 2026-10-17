# FMinus documentation

| Document | What it covers |
|---|---|
| [architecture.md](architecture.md) | The compiler from source text to machine code: every stage, the file that implements it, what passes between stages, how C's semantics map onto LLVM, and the invariants that keep it extensible |
| [extending.md](extending.md) | How to grow the C subset: the change checklist, six worked and verified recipes (a builtin, `%`, `bool` with comparisons, `&&` `\|\|` and `?:`, `if` `while` `for` `break` `continue`, a runtime function, an LLVM pass), the pitfalls, and design notes for what comes next (integers, pointers, structs, state) |
| [building.md](building.md) | Presets, the dependency cache and LLVM flavors, adding sources, tools, examples, tests and dependencies, warnings and sanitizers, troubleshooting |
| [testing.md](testing.md) | The test layers (Catch2, lit + FileCheck, example harnesses) and how to write each; inspecting and debugging the compiler's output with `fminus`, `opt`, `llc`, `llvm-mca` and lldb |
| [recipes/](recipes/) | extending.md's recipes as patches, in order |

To grow the language, read architecture.md, then extending.md. Use building.md and testing.md as reference.
Elsewhere:

- [../README.md](../README.md) is the quick reference: the language, the commands, the presets.
- [../examples/biquad/README.md](../examples/biquad/README.md) plans the language's growth around a DSP kernel.
