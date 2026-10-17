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

## Where v0 leaves it

FMinus v0 is a subset of C with no state, no loops and a single result per function. So one sample is three calls
from the harness (`lowpass_y`, `lowpass_s1`, `lowpass_s2`), and each call recomputes the coefficients, `sinf` and
`cosf` included. The C++ version inlines all of it into one loop. In the dev build on an M5, FMinus takes 6.3 ns a
sample and C++ takes 2.7 ns (2.4×); expect a few tenths of noise between runs. Closing that gap is the point of the
features below.

`biquad.fm` is also valid C: every constant carries the `f` suffix, and it compiles with `clang -std=c17 -Wall
-Wextra -Werror` (with `<math.h>`). At `-O2` the two outputs are bit-identical. Both compute the sine and cosine of
the same angle with one call to Apple's `__sincosf_stret`, and neither fuses a multiply and an add: the harness is
built with `-ffp-contract=off`, and FMinus's IR carries no `contract` flags. At `-O0`, FMinus calls `sinf` and `cosf`
separately instead, and the outputs differ by about 3e-6. Float's own error, against the same filter in double, is
about 7e-6.

## A path through it

Each step brings FMinus closer to the C you would write for this filter. Each one changes `biquad.fm` and the
harness's `Kernel` a little. After each one, run `example-biquad` and read the IR and assembly.
[docs/extending.md](../../docs/extending.md) has worked recipes for the first pieces: comparisons and `?:`, `if`,
`while` and `for`, runtime functions, and passes.

1. **File-scope constants** (`const float pi = 3.14159265f;`) replace `float pi()`. That means a top-level
   declaration, folded into its uses or kept as a global constant.
2. **Several results**: a `struct` return, `struct coefficients lowpass(float cutoff, float q, float rate)`,
   computes `sinf` and `cosf` once a sample instead of three times. Returning a struct through the C calling
   convention is a topic of its own.
3. **State**: a state struct the harness owns and passes by pointer
   (`float process(struct biquad* state, float x, float cutoff)`), or file-scope `static` variables. The whole TDF-II
   step becomes one call.
4. **Integers, comparisons, loops and buffers**:
   `void process(const float* input, const float* cutoff, float* output, int n)`, with a `for` loop. With the
   per-sample calls gone, the time shows what the calls were costing, and the comparison with C++ becomes fair.
5. **Cheaper transcendentals**: `sinf` and `cosf` written in FMinus, as polynomials with range reduction (`floorf`,
   `fminf` and `fmaxf` exist already). Accuracy becomes something to trade, and the harness prints the error
   against double.
6. **Contraction and fast-math**: C's `#pragma STDC FP_CONTRACT ON`, or flags per function (`contract`, `reassoc`,
   `afn`), let AArch64 fuse multiplies and adds. The output stops being bit-identical with C++, so `--tolerance`
   starts to matter.
7. **Control-rate coefficients**: computed every 16 or 32 samples and interpolated in between. An algorithmic win
   that only state and loops can express.
8. **Vectors**: several channels at once, or the coefficient math for a whole block.
   [docs/simd.md](../../docs/simd.md) has verified recipes for the pieces: `float4` (recipe 7), so four channels'
   state and coefficients sit in one register each; a vector `sinf`/`cosf` from libsystem_m through LLVM's
   vector library (recipe 8; Accelerate's mapping also replaces `fabsf` and `sqrtf` with calls, so it is the worse
   choice); and `poly float` (recipe 9), as many channels or voices as the machine's registers hold. Channels are
   lane-independent, so the output stays the same at any width. A loop over a block (step 4) lets the vectoriser
   help too.

Keep `example-biquad --check` passing (ctest runs it) as the kernel moves from the harness into FMinus. Its C++
version is the reference to change in step with the FMinus one.
