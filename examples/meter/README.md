# Level meter: reductions, and the order of a sum

`meter.fm` computes a level meter's two readings of a block: its peak (the largest `|x|`) and its RMS (the square root
of the mean of `x²`). Both reduce a block to one number, which makes them the opposite case to
[`examples/saturator/`](../saturator/README.md): there, every sample is independent, and LLVM's loop vectoriser
widens the plain loop by itself. Here each step of the plain loop waits for the one before, and whether LLVM may
vectorise it depends on whether the order of the steps matters. `bench.cpp` compiles it through FMinus's library API
(`fminus::Program`), takes one reading a block, compares the readings with C++, and times them.

```bash
cmake --workflow --preset release                              # once: also builds LLVM without assertions
build-release/bin/example-meter                                # time it here
build-dev/bin/example-meter --poly-lanes 32                    # a poly float of 32 lanes
build-dev/bin/example-meter --block 256                        # one reading a block of 256
build-dev/bin/example-meter --check                            # what ctest runs: the comparisons only
build-dev/bin/fminus examples/meter/meter.fm --emit asm -o -   # what the JIT generates
```

## The program

Each reading is a loop over the samples, and a loop over `poly float`s:

```c
float rms(const float* restrict input, int n) [[clang::nonblocking]]
{
    float sum = 0;
    for (int i = 0; i < n; ++i)
        sum += input[i] * input[i];
    return n > 0 ? sqrtf(sum / n) : 0;
}

float rms_poly(const float* restrict input, int n) [[clang::nonblocking]]
{
    poly float sum = 0;
    int i = 0;
    for (; i + poly_lanes() <= n; i += poly_lanes())
    {
        const poly float x = poly_load(input + i);
        sum += x * x;
    }
    const poly bool rest = i + poly_index() < n;
    const poly float x = poly_load(input + i, rest);   // 0 in the lanes past n
    sum += x * x;
    return n > 0 ? sqrtf(reduce_add(sum) / n) : 0;
}
```

`peak` and `peak_poly` are the same loops with `fmaxf(largest, fabsf(x))`, and `reduce_max` at the end. In the poly
loops, lane i keeps a partial result over samples i, i + `poly_lanes()`, i + 2 `poly_lanes()`, ..., and the lanes are
combined at the end. The masked last step reads the lanes past `n` as 0, which adds nothing to a sum of squares and
changes no maximum of absolute values.

- **A maximum is the same in any order.** `peak_poly` reads what `peak` does, at every width, bit for bit.
- **A float sum is not.** Float addition rounds, and isn't associative, so `rms_poly`'s last digits depend on the
  width (docs/simd.md, "Results depend on the width"). `--check` checks it at every width from 1 to 64 against a C++
  emulation of that width's order (`rmsInLanes`), exactly.

## What it measures

On this sandbox's Linux machine (an Intel Xeon, Emerald Rapids, with AVX-512), in the dev build, one reading of 48000
samples of noise. A `poly float` has 8 lanes there. The C++ is the system's clang 18 at `-O2`.

| ns a sample | FMinus `-O2` | FMinus `-O0` | C++ | relative error |
|---|---|---|---|---|
| peak, a sample at a time | 0.085 | 3.28 | 2.40 | 0 |
| peak, a `poly float` at a time | 0.262 | 0.452 | | 0 |
| RMS, a sample at a time | 0.607 | 2.43 | 0.607 | 2.0e-6 |
| RMS, a `poly float` at a time | 0.077 | 0.360 | | 1.9e-7 |

The relative error is against the reading in double. The `poly float` loops at other widths (`--poly-lanes`):

| lanes | 1 | 4 | 8 | 16 | 32 | 64 |
|---|---|---|---|---|---|---|
| peak, ns a sample | 2.09 | 0.522 | 0.262 | 0.152 | 0.094 | 0.068 |
| RMS, ns a sample | 0.607 | 0.152 | 0.077 | 0.080 | 0.062 | 0.057 |
| RMS, relative error | 2.0e-6 | 2.3e-7 | 1.9e-7 | 1.2e-7 | 1.2e-7 | 2e-8 |

- **The sum stays scalar unless the program says otherwise.** Vectorising `rms`'s loop would add in another order,
  so LLVM may not, without fast-math's `reassoc`; neither does clang for the C++. Each add waits for the last one:
  0.607 ns a sample in both. `rms_poly` keeps 8 sums side by side, and takes 0.077: 7.9 times faster. It is also
  more accurate, since each partial sum has 6000 terms instead of 48000. `poly float` is `reassoc` for one loop,
  with the order written down, so it can be checked.
- **The maximum is vectorised either way, and the vectoriser unrolls.** The order of `fmaxf`s doesn't matter, and
  FMinus's LLVM vectorises `peak`'s loop by itself, with four vectors of 8 as four running maxima: 32 samples a step.
  `peak_poly` at 8 lanes has one running maximum, each step waiting for the last (on x86, `fmaxf` is three
  instructions, since `maxps` doesn't treat NaN as C does), and is three times slower than the loop it was meant to
  beat. At 32 lanes it is level: on this machine a 32-lane `poly float` is two AVX-512 registers, and so two
  independent maxima of 16 lanes. A wider `poly float` is an unrolled loop.
- **clang 18 does not vectorise the maximum.** The C++ `peak` is a scalar loop, 2.40 ns a sample, with NaN handling
  around every `maxss`. FMinus's LLVM is newer and does vectorise it; on another compiler, expect the C++ line to
  differ.
- **Without the optimiser**, at `-O0`, the poly loops are still vector loops: 7 times faster than the scalar ones.

## Where FMinus leaves it

1. **Several accumulators at the machine's width.** `peak_poly` could keep two or four `poly float` maxima and
   combine them at the end, which is what `--poly-lanes 32` does by accident. A `poly float` array, or a struct of
   them, would let the program say how many.
2. **Fast-math, scoped.** A `reassoc` flag on `rms` alone would let LLVM vectorise the plain loop, in an order of its
   choosing (docs/simd.md, "Where to go next").
3. **A running meter.** A meter's display decays between readings: a one-pole smoother of the readings, or of `x²`
   sample by sample. Sample by sample, that is a recursion, which no width helps, as in the biquad.
