# Recipes as patches

Most of the recipes in [../extending.md](../extending.md) and [../simd.md](../simd.md) are in the compiler now:

- recipes 1 and 2 (`tanhf`, `%`);
- recipe 3 (`bool`, comparisons, `!`, `&&`, `||`, `?:`);
- recipe 4 (`if`, `while`, `for`, `break`, `continue`);
- recipes 7 to 9 (`float2`/`float4`/`float8`, the vector math library, `poly float`);
- recipe 10 (`int`, `void` and pointers);
- recipe 11 (the heap, `void*` and `sizeof`, and `[[clang::nonblocking]]`).

Their code is in `src/`, and their tests in `tests/`.

One patch is left, for the two recipes that aren't part of the language:

| Patch | Recipes | Adds |
|---|---|---|
| `5-runtime-function-and-pass.patch` | 5, 6 | a runtime function (`db_to_gain`, by prototype or builtin); an LLVM pass (`CountFlopsPass`, which records each function's floating-point operations as an attribute) |

It applies, with no whitespace errors, to the compiler as it is, with all of the above. The suite then has 66 tests,
which pass in the dev and asan presets.

```bash
git switch -c try-recipes
git apply docs/recipes/5-runtime-function-and-pass.patch
cmake --workflow --preset dev
ctest --preset asan                                     # after building the asan preset
```

Its tests go beside the others: `tests/unit/CodeGenTest.cpp` and `tests/lit/fminus/flops.fm`. Under asan, run the
tests through the preset (`ctest --preset asan`): it sets `ASAN_OPTIONS=detect_container_overflow=0`, which the
instrumented FMinus and uninstrumented LLVM need, since they share `std::vector` instantiations. Without it, the JIT
and AOT smoke tests abort with a false "container-overflow" inside LLVM.

Once the compiler moves on, the patch stops applying, but it remains a readable diff of one way to make the change.
