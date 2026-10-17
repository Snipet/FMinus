# Recipes as patches

The recipes of [../extending.md](../extending.md) and [../simd.md](../simd.md), as made and verified: built with
`-Werror` and no warnings, and tested in the dev and asan presets. The base, with recipes 1 and 2 and `main`, has 20
tests; with all six patches it has 32, all passing in both presets. They apply in order (each assumes the ones before
it), with no whitespace errors, to the base they were written against: commit 30c1dc4 with the entry point
(`float main`, `--emit exe`) on top.

| Patch | Recipes | Adds |
|---|---|---|
| `3-bool-logic-conditional.patch` | 3 | `bool` (`true`, `false`), the comparisons, `!`, `&&`, `\|\|` and `?:` |
| `4-if-while-for.patch` | 4 | `if`/`else`, `while`, `for`, `break` and `continue` |
| `5-runtime-function-and-pass.patch` | 5, 6 | a runtime function (`db_to_gain`, by prototype or builtin); an LLVM pass (`CountFlopsPass`) |
| `7-vectors.patch` | 7 | `float2`, `float4`, `float8`, masks, swizzles, literals, reductions; every value carries its FMinus type |
| `8-vector-math-library.patch` | 8 | libsystem_m's vector functions for `float4` math, through `ReplaceWithVeclib` |
| `9-poly-float.patch` | 9 | `poly float`, `poly bool`, `poly_lanes()`, `poly_index()`, `ir::Options`, `polyLanes()`, `--poly-lanes` |

Recipes 1 and 2 (`tanhf` and `%`) have no patch: they are in the base.

```bash
git switch -c try-recipes
git apply docs/recipes/3-bool-logic-conditional.patch   # and so on, in order
cmake --workflow --preset dev
ctest --preset asan                                     # after building the asan preset
```

Their tests go beside the base's: `tests/unit/CodeGenTest.cpp` (tagged `[codegen]`, `[simd]` and `[poly]`),
`tests/unit/FrontendTest.cpp`, and `tests/lit/fminus/` (`flops.fm`, `simd.fm`, `veclib.fm`, `poly.fm`). Under asan,
run the tests through the preset (`ctest --preset asan`): it sets `ASAN_OPTIONS=detect_container_overflow=0`, which
the instrumented FMinus and uninstrumented LLVM need, since they share `std::vector` instantiations. Without it, the
JIT and AOT smoke tests abort with a false "container-overflow" inside LLVM.

Once the compiler moves on, they stop applying, but they remain readable diffs of one way to make each change.
