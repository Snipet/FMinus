# Biquad with per-sample coefficients

`biquad.fm` is a biquad low-pass filter whose coefficients are recomputed every sample: the RBJ "Audio EQ Cookbook"
low-pass, in transposed direct form II, with the cutoff swept every sample. It is a small, performance-critical
kernel to grow FMinus around. `bench.cpp` JIT-compiles it and runs it over noise. It compares the result with a C++
version of the same formulas, and times both.

```bash
cmake --workflow --preset release                              # once: also builds LLVM without assertions
build-release/bin/example-biquad                               # time it here
build-dev/bin/example-biquad -O0                               # FMinus unoptimised
build-dev/bin/example-biquad --check                           # what ctest runs: the comparison only
build-dev/bin/fminus examples/biquad/biquad.fm --emit asm -o -  # what the JIT generates
```

## Where FMinus leaves it

FMinus has no state, buffers or structs yet, but it has `float4`. So one sample is one call, `lowpass`, which returns
the output and the filter's two new states together, `(y, s1, s2, 0)`, and computes the coefficients, `sinf` and
`cosf` included, once. The harness calls it as clang's `ext_vector_type`: a `float4` crosses the C ABI in one
register ([docs/simd.md](../../docs/simd.md)).

```c
float4 lowpass(float x, float s1, float s2, float cutoff, float q, float rate)
{
    const float4 k = lowpass_coefficients(cutoff, q, rate);   // (b0, b1, a1, a2)
    const float y = k.x * x + s1;
    return (float4)(y, k.y * x - k.z * y + s2, k.x * x - k.w * y, 0.0f);
}
```

In the dev build on an M5, FMinus takes 3.9 ns a sample and C++ takes 2.65 ns (1.46×); expect a few tenths of noise
between runs. Before `float4`, one sample was three calls, `lowpass_y`, `lowpass_s1` and `lowpass_s2`, each
recomputing the coefficients: 6.35 ns, 2.40× C++. The C++ version inlines everything into one loop; closing the rest
of the gap is the point of the features below.

At `-O2` the two outputs are bit-identical. Both compute the sine and cosine of the same angle with one call to
Apple's `__sincosf_stret`, and neither fuses a multiply and an add: the harness is built with `-ffp-contract=off`, and
FMinus's IR carries no `contract` flags. At `-O0`, FMinus calls `sinf` and `cosf` separately instead, and the outputs
differ by about 3e-6. Float's own error, against the same filter in double, is about 7e-6.

`biquad.fm` is also valid C. It builds its `float4`s with C's literal, `(float4){a, b, c, d}`, not OpenCL C's
`(float4)(a, b, c, d)`, which C reads as a cast of a comma expression (docs/simd.md, "Literals"). With
`typedef float float4 __attribute__((ext_vector_type(4)));` and `<math.h>`, it compiles with `clang -std=c17 -Wall
-Wextra -Werror -ffp-contract=off`, and that build and FMinus's agree bit for bit.

## A path through it

Each step brings FMinus closer to the C you would write for this filter. Each one changes `biquad.fm` and the
harness's `Kernel` a little. After each one, run `example-biquad` and read the IR and assembly.
[docs/extending.md](../../docs/extending.md) and [docs/simd.md](../../docs/simd.md) have worked recipes for the pieces
FMinus has so far.

1. **File-scope constants** (`const float pi = 3.14159265f;`) replace `float pi()`. That means a top-level
   declaration, folded into its uses or kept as a global constant.
2. ~~**Several results**~~: done with a `float4` return, which also computes `sinf` and `cosf` once a sample. A
   `struct coefficients` would name the fields; through the C calling convention, a struct of four floats is a
   homogeneous floating-point aggregate, returned in `s0` to `s3`.
3. **State**: a state struct the harness owns and passes by pointer
   (`float process(struct biquad* state, float x, float cutoff)`), or file-scope `static` variables. The whole TDF-II
   step becomes one call that returns only `y`.
4. **Integers and buffers**: `void process(const float* input, const float* cutoff, float* output, int n)`, with a
   `for` loop (FMinus has comparisons and loops now). With the per-sample calls gone, the time shows what the calls
   were costing, and the comparison with C++ becomes fair.
5. **Cheaper transcendentals**: `sinf` and `cosf` written in FMinus, as polynomials with range reduction (`floorf`,
   `fminf`, `fmaxf`, comparisons and `?:` exist already). Accuracy becomes something to trade, and the harness prints
   the error against double.
6. **Contraction and fast-math**: C's `#pragma STDC FP_CONTRACT ON`, or flags per function (`contract`, `reassoc`,
   `afn`), let AArch64 fuse multiplies and adds. The output stops being bit-identical with C++, so `--tolerance`
   starts to matter.
7. **Control-rate coefficients**: computed every 16 or 32 samples and interpolated in between. An algorithmic win
   that only state and loops can express.
8. **Vectors across channels**: four channels at once, their states and coefficients one `float4` each, or as many
   as the machine holds with `poly float`. Channels are lane-independent, so the output stays the same at any width.
   This needs state (step 3) first: one call would have to return three `float4`s. Then `sinf`/`cosf` of a `float4`
   is one call to libsystem_m's vector functions (recipe 8). A loop over a block (step 4) lets the vectoriser help
   too.

Keep `example-biquad --check` passing (ctest runs it) as the kernel moves from the harness into FMinus. Its C++
version is the reference to change in step with the FMinus one.
