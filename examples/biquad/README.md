# Biquad with per-sample coefficients

`biquad.fm` is a biquad low-pass filter whose coefficients are recomputed every sample: the RBJ "Audio EQ Cookbook"
low-pass, in transposed direct form II, with the cutoff swept every sample. It is a small, performance-critical
kernel to grow FMinus around. `bench.cpp` JIT-compiles it and runs it over noise in one call. It compares the result
with a C++ version of the same formulas, and times both.

```bash
cmake --workflow --preset release                              # once: also builds LLVM without assertions
build-release/bin/example-biquad                               # time it here
build-dev/bin/example-biquad -O0                               # FMinus unoptimised
build-dev/bin/example-biquad --block 64                        # one call a block of 64, as a plugin calls it
build-dev/bin/example-biquad --check                           # what ctest runs: the comparisons only
build-dev/bin/fminus examples/biquad/biquad.fm --emit asm -o -  # what the JIT generates
```

## Where FMinus leaves it

With `int`, `void` and pointers ([docs/extending.md](../../docs/extending.md), recipe 10), the kernel filters a whole
block in one call, as a plugin's process function does:

```c
void lowpass(const float* restrict input, const float* restrict cutoff, float* restrict output, int n, float q,
             float rate, float* restrict state)
{
    float s1 = state[0];
    float s2 = state[1];
    for (int i = 0; i < n; ++i)
    {
        const float4 k = lowpass_coefficients(cutoff[i], q, rate);   // (b0, b1, a1, a2)
        const float x = input[i];
        const float y = k.x * x + s1;
        s1 = k.y * x - k.z * y + s2;
        s2 = k.x * x - k.w * y;
        output[i] = y;
    }
    state[0] = s1;
    state[1] = s2;
}
```

The harness makes one call for the whole signal, so the loop over the samples is FMinus's. The filter's state,
`(s1, s2)`, is the harness's: read once, kept in registers through the loop, and written back at the end. So the next
call carries on where this one stopped, and calling it a block at a time (`--block 64`) gives the same output.
`--check` checks that too, with blocks of 64 and of 61, which end mid-way through the signal. `restrict` promises that
the four buffers don't overlap. Here it changes no instruction: the state is in locals already, and the loop can't
vectorise across samples, since each one needs the last one's state. It is true, and free; in a loop that can
vectorise, it removes the check for overlap (recipe 10).

In the dev build on an M5 (expect a few hundredths of noise between runs):

| Kernel | FMinus | C++ | FMinus / C++ |
|---|---|---|---|
| three calls a sample, `lowpass_y`, `lowpass_s1` and `lowpass_s2`, each recomputing the coefficients | 6.35 ns | 2.65 ns | 2.40× |
| one call a sample, returning `(y, s1, s2, 0)` in a `float4` | 3.88 ns | 2.65 ns | 1.46× |
| one call for all 48000 samples, the loop in FMinus | 2.66 ns | 2.65 ns | 1.00× |

The C++ version inlines everything into one loop, and FMinus now does the same: the coefficients are inlined into the
loop, `2 * q` is hoisted out of it, and the state lives in registers. The 1.2 ns a sample that the per-sample calls
cost was the call itself, and the arguments and the `float4` result crossing the C ABI: everything LLVM could not see
across.

That is 1.46×, not more, because what is left is the filter, and its critical path doesn't shrink. Each sample's `y`
needs `s1`, which needs the last `y`: an add, a multiply, a subtract and an add, one after another, about 2.3 ns a
sample on an M5. Out-of-order execution runs the next sample's `sinf`, `cosf` and four divides alongside that chain,
so they cost little. C++ stand-ins measure it: holding the coefficients for 16 samples instead of recomputing them
saves 0.4 ns of 2.8, and fused multiply-adds (`-ffp-contract=fast`) save nothing. Four channels in the lanes of a
`float4` take 1.44 ns a channel's sample: twice the throughput, since four independent recursions run side by side.

At `-O2` the two outputs are bit-identical. Both compute the sine and cosine of the same angle with one call to
Apple's `__sincosf_stret`, and neither fuses a multiply and an add: the harness is built with `-ffp-contract=off`, and
FMinus's IR carries no `contract` flags. At `-O0`, FMinus calls `sinf` and `cosf` separately instead, and the outputs
differ by about 3e-6. Float's own error, against the same filter in double, is about 7e-6.

`biquad.fm` is also valid C. It builds its `float4`s with C's literal, `(float4){a, b, c, d}`, not OpenCL C's
`(float4)(a, b, c, d)`, which C reads as a cast of a comma expression (docs/simd.md, "Literals"). `restrict` is C99's,
and `pi(void)` is C's empty parameter list. With `typedef float float4 __attribute__((ext_vector_type(4)));` and
`<math.h>`, it compiles with `clang -std=c17 -Wall -Wextra -Werror -ffp-contract=off`, and that build and FMinus's
agree bit for bit, state included.

## A path through it

Each step brings FMinus closer to the C you would write for this filter. Each one changes `biquad.fm` and the
harness's `Kernel` a little. After each one, run `example-biquad` and read the IR and assembly.
[docs/extending.md](../../docs/extending.md) and [docs/simd.md](../../docs/simd.md) have worked recipes for the pieces
FMinus has so far.

1. **File-scope constants** (`const float pi = 3.14159265f;`) replace `float pi(void)`. That means a top-level
   declaration, folded into its uses or kept as a global constant.
2. ~~**Several results**~~: done with a `float4` return, which also computes `sinf` and `cosf` once a sample. A
   `struct coefficients` would name the fields; through the C calling convention, a struct of four floats is a
   homogeneous floating-point aggregate, returned in `s0` to `s3`.
3. ~~**State**~~: done with a pointer to two floats the harness owns (recipe 10). A `struct biquad { float s1, s2; }`
   passed by pointer would name them, and file-scope `static` variables are the other way, though not for a plugin,
   where every instance needs its own.
4. ~~**Integers and buffers**~~: done (recipe 10). `void lowpass(const float* input, ..., int n, ...)` with a `for`
   loop over the block, and the harness makes one call. The per-sample calls were costing 1.2 ns a sample, and the
   comparison with C++ is fair now: level.
5. **Vectors across channels**, the next big win: four channels at once, their samples, states and coefficients one
   `float4` each (`const float4* input`, `float4* state`), or as many as the machine holds with `poly float`.
   Channels are lane-independent, so the output stays the same at any width, and `sinf`/`cosf` of a `float4` is one
   call to libsystem_m's vector functions (recipe 8). In C++ it doubles the throughput, to 1.44 ns a channel's sample.
6. **Cheaper transcendentals**: `sinf` and `cosf` written in FMinus, as polynomials with range reduction (`floorf`,
   `fminf`, `fmaxf`, comparisons and `?:` exist already). Accuracy becomes something to trade, and the harness prints
   the error against double. For one channel it buys little (above), but it counts once step 5 makes the
   transcendentals the bottleneck.
7. **Control-rate coefficients**: computed every 16 or 32 samples and interpolated in between. An algorithmic win, but
   for one channel a small one: holding them for 16 samples saves 0.4 ns of 2.8.
8. **Contraction and fast-math**: C's `#pragma STDC FP_CONTRACT ON`, or flags per function (`contract`, `reassoc`,
   `afn`), let AArch64 fuse multiplies and adds. The output stops being bit-identical with C++, so `--tolerance`
   starts to matter. On an M5 it doesn't shorten this recursion.

Keep `example-biquad --check` passing (ctest runs it) as the kernel moves from the harness into FMinus. Its C++
version is the reference to change in step with the FMinus one.
