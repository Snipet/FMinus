# Recipes as patches

Most of the recipes in [../extending.md](../extending.md) and [../simd.md](../simd.md) are in the compiler now:

- recipes 1 and 2 (`tanhf`, `%`);
- recipe 3 (`bool`, comparisons, `!`, `&&`, `||`, `?:`);
- recipe 4 (`if`, `while`, `for`, `break`, `continue`);
- recipes 7 to 9 (`float2`/`float4`/`float8`, the vector math library, `poly float`);
- recipe 10 (`int`, `void` and pointers);
- recipe 11 (the heap, `void*` and `sizeof`, and `[[clang::nonblocking]]`);
- recipe 12 (arrays and modules);
- recipe 13 (the FFT, on PFFFT in the runtime);
- recipe 14 (parameters: knobs, streams and frozen constants);
- recipe 15 (math that gives a float one result: the vector, libm and fast math modes);
- recipe 16 (knob versioning, and flat streams);
- recipe 17 (C's strict aliasing, told to LLVM as TBAA);
- recipe 18 (zero-init elision);
- recipe 19 (the runtime's attributes, and loops that must make progress);
- recipe 20 (denormal control: subnormals flushed to zero);
- recipe 21 (`#include`, each file read once);
- recipe 22 (`parallel for` and `parallel { }`, a program's own threads, on the runtime's pool);
- recipe 23 (math kernels of FMinus's own, the same in every lane and on every target);
- recipe 24 (the math kernels inline).

Their code is in `src/`, and their tests in `tests/`.

One patch is left, for the two recipes that aren't part of the language:

| Patch | Recipes | Adds |
|---|---|---|
| `5-runtime-function-and-pass.patch` | 5, 6 | a runtime function (`db_to_gain`, by prototype or builtin); an LLVM pass (`CountFlopsPass`, which records each function's floating-point operations as an attribute) |

It applies, with no whitespace errors, to the compiler as it is, with all of the above. The suite then has 186 tests,
which pass in the dev and asan presets.

```bash
git switch -c try-recipes
git apply docs/recipes/5-runtime-function-and-pass.patch
cmake --workflow --preset dev
ctest --preset asan                                     # after building the asan preset
```

Its tests go beside the others: `tests/unit/CodeGenTest.cpp` and `tests/lit/fminus/flops.fm`. The pass gives every
function an attribute (`"fminus-flops"`), so the patch also changes `tests/lit/fminus/denormals.fm`, whose checks name
the functions' attribute groups: two functions that shared one no longer do when their counts differ. Under asan, run
the tests through the preset (`ctest --preset asan`): it sets `ASAN_OPTIONS=detect_container_overflow=0`, which the
instrumented FMinus and uninstrumented LLVM need, since they share `std::vector` instantiations. Without it, the JIT and
AOT smoke tests abort with a false "container-overflow" inside LLVM.

Once the compiler moves on, the patch stops applying, but it remains a readable diff of one way to make the change.
