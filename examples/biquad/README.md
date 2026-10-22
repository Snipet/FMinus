# Biquad with per-sample coefficients

`biquad.fm` is a biquad low-pass filter whose coefficients are recomputed every sample: the RBJ "Audio EQ Cookbook"
low-pass, in transposed direct form II, with the cutoff swept every sample. It is a small, performance-critical
kernel to grow FMinus around: a module whose cutoff is a parameter. `bench.cpp` compiles it through `fminus::Program`
and runs it over noise in one call. It compares the result with a C++ version of the same formulas, and times both.

```bash
cmake --workflow --preset release                              # once: also builds LLVM without assertions
build-release/bin/example-biquad                               # time it here
build-dev/bin/example-biquad -O0                               # FMinus unoptimised
build-dev/bin/example-biquad --block 64                        # one call a block of 64, as a plugin calls it
build-dev/bin/example-biquad --check                           # what ctest runs: the comparisons only
build-dev/bin/fminus examples/biquad/biquad.fm --stream Biquad::cutoff --freeze Biquad::rate=48000 --emit asm -o -
                                                               # what the JIT generates, bound as the harness binds it
```

## Where FMinus leaves it

The filter is a module with parameters ([docs/extending.md](../../docs/extending.md), recipes 12 and 14): its state
is two fields, and what a host sets is three parameters, which `process` reads a sample at a time:

```c
module Biquad
{
    param float cutoff [[fminus::range(20, 20000)]] [[fminus::default(1000)]];   // Hz
    param float q [[fminus::range(0.1, 20)]] [[fminus::default(0.7071)]];
    param float rate [[fminus::range(8000, 384000)]] [[fminus::default(48000)]];   // Hz
    float s1;
    float s2;

    void process(const float* restrict input, float* restrict output, int n) [[clang::nonblocking]]
    {
        for (int i = 0; i < n; ++i)
        {
            const float4 k = lowpass_coefficients(cutoff[i], q[i], rate[i]);   // (b0, b1, a1, a2)
            const float x = input[i];
            const float y = k.x * x + s1;
            s1 = k.y * x - k.z * y + s2;
            s2 = k.x * x - k.w * y;
            output[i] = y;
        }
    }
};
```

How each parameter reads is the harness's choice when it compiles, through `fminus::Program::Options::parameters`:
the cutoff is a stream, the harness's buffer of one cutoff a sample, so the sweep is sample-accurate; `q` is a knob,
which `setValue` sets before the first call; the sample rate is frozen at `--rate`, compiled in as a constant, since
it changes only when the host starts again. The harness allocates the module, as a plugin does each instance, and
makes one call for the whole signal, so the loop over the samples is FMinus's. The state carries over from call to
call in the module, so calling it a block at a time (`--block 64`) gives the same output; `--check` checks that too,
with blocks of 64 and of 61, which end mid-way through the signal.

The state lives in registers through the loop, and goes back to its fields once, at the end. That needs LLVM to know
that nothing else the loop reads or writes is a field: `input` and `output` are `restrict`, and the stream's samples are
in an alias scope of their own (recipe 14), so none of them can be `s1` or `s2`. `restrict` changes nothing else here:
the loop can't vectorise across samples, since each one needs the last one's state.

In the dev build on an M5 (expect a few hundredths of noise between runs):

| Kernel | FMinus | C++ | FMinus / C++ |
|---|---|---|---|
| three calls a sample, `lowpass_y`, `lowpass_s1` and `lowpass_s2`, each recomputing the coefficients | 6.35 ns | 2.65 ns | 2.40× |
| one call a sample, returning `(y, s1, s2, 0)` in a `float4` | 3.88 ns | 2.65 ns | 1.46× |
| one call for all 48000 samples, the loop in FMinus | 2.66 ns | 2.65 ns | 1.00× |
| the module `Biquad`, its cutoff a stream, its rate frozen: one call for all 48000 samples | 2.65 ns | 2.65 ns | 1.00× |

The first three rows are the kernel as FMinus grew: calls a sample, then one function over the block, its state's
pointer and the cutoff's buffer arguments. The last is the module, which costs what the function did. The C++ version
inlines everything into one loop, and FMinus does the same: the coefficients are inlined into the loop, `2 * q` is
hoisted out of it, and the state lives in registers. The 1.2 ns a sample that the per-sample calls
cost was the call itself, and the arguments and the `float4` result crossing the C ABI: everything LLVM could not see
across.

That is 1.46×, not more, because what is left is the filter, and its critical path doesn't shrink. Each sample's `y`
needs `s1`, which needs the last `y`: an add, a multiply, a subtract and an add, one after another, about 2.3 ns a
sample on an M5. Out-of-order execution runs the next sample's `sinf`, `cosf` and four divides alongside that chain,
so they cost little. C++ stand-ins measure it: holding the coefficients for 16 samples instead of recomputing them
saves 0.4 ns of 2.8, and fused multiply-adds (`-ffp-contract=fast`) save nothing. Four channels in the lanes of a
`float4` take 1.44 ns a channel's sample: twice the throughput, since four independent recursions run side by side.

At every optimisation level the two outputs are bit-identical. FMinus's `sinf` and `cosf` are its runtime's (its
default math mode), which give an angle one result wherever they are called, and the harness's C++ calls the same
functions, `fminus::math::sin` and `cos`; neither side fuses a multiply and an add: the harness is built with
`-ffp-contract=off`, and FMinus's IR carries no `contract` flags. (With `--math fast`, LLVM's intrinsics, FMinus calls
Apple's `__sincosf_stret` once a sample at `-O2` but `sinf` and `cosf` separately at `-O0`, and the two levels'
outputs differ by about 3e-6.) Float's own error, against the same filter in double, is about 7e-6.

`biquad.fm` is no longer C: a module and its parameters are FMinus's own. Its free functions, `pi` and
`lowpass_coefficients`, still are, and `examples/delay/delay.fm` is the example kept in plain C, which clang compiles
and checks with `-Wfunction-effects`.

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
3. ~~**State**~~: done, first with a pointer to two floats the harness owned (recipe 10), now as the fields of a
   module, `Biquad` (recipe 12), in memory the harness allocates for each instance.
4. ~~**Controls**~~: done with parameters (recipe 14): the cutoff a stream, `q` a knob, the rate frozen. Freezing the
   rate makes it a constant divisor, and saves nothing: each sample still divides `2 pi cutoff` by 48000, since one
   multiplier, `2 pi / rate`, would round differently (it needs `reassoc`, or the source written as
   `cutoff * (2 pi / rate)`). Freezing `q` would fold `2 * q`, which a knob already hoists out of the loop.
5. ~~**Integers and buffers**~~: done (recipe 10). `void lowpass(const float* input, ..., int n, ...)` with a `for`
   loop over the block, and the harness makes one call. The per-sample calls were costing 1.2 ns a sample, and the
   comparison with C++ is fair now: level.
6. **Vectors across channels**, the next big win: four channels at once, their samples, states and coefficients one
   `float4` each (`const float4* input`, four channels' state in two `float4` fields), or as many as the machine holds
   with `poly float`. Channels are lane-independent, so the output stays the same at any width, and `sinf`/`cosf` of a
   `float4` is one call to libsystem_m's vector functions (recipe 8). In C++ it doubles the throughput, to 1.44 ns a
   channel's sample.
7. **Cheaper transcendentals**: `sinf` and `cosf` written in FMinus, as polynomials with range reduction (`floorf`,
   `fminf`, `fmaxf`, comparisons and `?:` exist already). Accuracy becomes something to trade, and the harness prints
   the error against double. For one channel it buys little (above), but it counts once step 6 makes the
   transcendentals the bottleneck.
8. **Control-rate coefficients**: computed every 16 or 32 samples and interpolated in between. An algorithmic win, but
   for one channel a small one: holding them for 16 samples saves 0.4 ns of 2.8.
9. **Contraction and fast-math**: C's `#pragma STDC FP_CONTRACT ON`, or flags per function (`contract`, `reassoc`,
   `afn`), let AArch64 fuse multiplies and adds. The output stops being bit-identical with C++, so `--tolerance`
   starts to matter. On an M5 it doesn't shorten this recursion.

Keep `example-biquad --check` passing (ctest runs it) as the kernel moves from the harness into FMinus. Its C++
version is the reference to change in step with the FMinus one.
