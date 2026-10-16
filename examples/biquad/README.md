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

FMinus v0 has no state, no loops and a single result per function. So one sample is three calls from the harness
(`lowpass_y`, `lowpass_s1`, `lowpass_s2`), and each call recomputes the coefficients, `sin` and `cos` included. The
C++ version inlines all of it into one loop. In the dev build on an M5, FMinus takes 7.1 ns a sample and C++ takes
2.9 ns (2.4×). Closing that gap is the point of the features below.

At `-O2` the two outputs are bit-identical. Both compute the sine and cosine of the same angle with one call to
Apple's `__sincosf_stret`, and neither fuses a multiply and an add: the harness is built with `-ffp-contract=off`,
and FMinus's IR carries no `contract` flags. At `-O0`, FMinus calls `sinf` and `cosf` separately instead, and the
outputs differ by about 3e-6. Float's own error, against the same filter in double, is about 7e-6.

## A path through it

Each step changes `biquad.fm` and the harness's `Kernel` a little. After each one, run `example-biquad` and read the
IR and assembly.

1. **Constants** (`const pi = 3.14159...;`) replace `fn pi()`: a top-level declaration, folded or kept as a global.
2. **Several results** (a struct or a tuple): `fn lowpass(cutoff, q, rate) -> Coefficients` computes `sin` and `cos`
   once a sample instead of three times. Returning a struct through the C calling convention is a topic of its own.
3. **State** (instance variables, or `inout` parameters) makes the whole TDF-II step one call:
   `process(x, cutoff) -> float`. The harness then keeps only the buffers.
4. **Integers, comparisons, loops and buffers**:
   `fn process(input: buffer, cutoff: buffer, output: buffer, n: int)`. With the per-sample calls gone, the time
   shows what the calls were costing, and the comparison with C++ becomes fair.
5. **Cheaper transcendentals**: `sin` and `cos` written in FMinus, as polynomials with range reduction (`floor`,
   `min` and `max` exist already). Accuracy becomes something to trade, and the harness prints the error against
   double.
6. **Fast-math flags** (`contract`, `reassoc`, `afn`, per function or per block) let AArch64 fuse multiplies and
   adds. The output stops being bit-identical with C++, so `--tolerance` starts to matter.
7. **Control-rate coefficients**: computed every 16 or 32 samples and interpolated in between. An algorithmic win
   that only state and loops can express.
8. **Vectors**: several channels at once, or the coefficient math for a whole block. That needs a loop for the
   vectoriser (step 4), and a vector `sin`/`cos`, for example Accelerate's vForce through TargetLibraryInfo's
   vector library.

Keep `example-biquad --check` passing (ctest runs it) as the kernel moves from the harness into FMinus. Its C++
version is the reference to change in step with the FMinus one.
