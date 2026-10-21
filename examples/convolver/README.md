# Convolver: a long FIR filter by FFT

`convolver.fm` convolves its input with an impulse response, a small room's or a cabinet's, by FFT
([docs/extending.md](../../docs/extending.md), recipe 13). It uses overlap-add: the input is gathered a block at a
time, and each block's spectrum is multiplied by the response's. The result goes back into samples and is added onto
what the blocks before it left for the samples to come. `bench.cpp` compiles it through `fminus::Program`, runs noise
through it, compares the output with a direct convolution in double and with the same overlap-add in C++ on the same
FFT, and times all three.

```bash
build-dev/bin/example-convolver                                # 2048 taps, blocks of 256
build-dev/bin/example-convolver --taps 8192 --hop 512          # a longer room
build-dev/bin/example-convolver --block 64                     # process called 64 samples at a time
build-dev/bin/example-convolver --check                        # what ctest runs: the comparisons only
```

## The program

The `Convolver` module makes its plan and its buffers in `init`, off the audio thread, and turns the response into its
spectrum there once, with the inverse FFT's 1/N folded into it. `process` runs on the audio thread, and is
`[[clang::nonblocking]]`: it can't reach `fft_create`, `calloc` or `free`, and FMinus checks that.

```c
void convolve() [[clang::nonblocking]]
{
    ...
    fft_forward(plan, f, x);                                   // the block, padded with zeros
    for (int k = 0; k <= n / 2; ++k)
    {
        const float2 u = x[k];
        const float2 v = h[k];
        x[k] = (float2)(u.x * v.x - u.y * v.y, u.x * v.y + u.y * v.x);   // times the response's spectrum
    }
    fft_inverse(plan, x, r);
    for (int i = 0; i < n; ++i)
        t[i] += r[i];                                          // added onto the tail
    ...
}
```

- **The FFT's size** is the smallest PFFFT takes of at least `block + taps - 1` points, so that a block's convolution
  never wraps around: `fft_size`, a loop that finds a multiple of 32 made of the factors 2, 3 and 5 only. At 2048
  taps and blocks of 256 it's 2304 points, not the power of two above, 4096.
- **The output is `block` samples late**, the time it takes to gather a block. It's the same however the host splits
  its calls of `process`: `--check` compares calls of 64 and 61 samples with one call for the whole run.
- **A host holds only the convolver's address.** `convolver_create` allocates the module on FMinus's heap and runs
  `init`; the host calls `Convolver::process` on it, and `convolver_destroy` at the end.

## Measurements

In the dev build on an M5, 96,000 samples of noise, each time the best of 20 runs:

| Taps | Block | FFT | FMinus | C++, the same | C++, directly | Direct / FMinus |
|---|---|---|---|---|---|---|
| 512 | 64 | 576 | 9.0 ns/sample | 9.6 ns/sample | 44 ns/sample | 4.9× |
| 2048 | 256 | 2304 | 9.6 ns/sample | 10.4 ns/sample | 176 ns/sample | 18× |
| 8192 | 512 | 9216 | 23 ns/sample | 24.5 ns/sample | 685 ns/sample | 30× |

- **FMinus and C++ give the same output, bit for bit**: the same operations in the same order, no fused
  multiply-adds on either side, and the same FFT, the runtime's.
- **Against the direct convolution in double**, the output is within 2.1e-7 of its peak (float's rounding, through two
  FFTs); `--check` allows 1e-6.
- **The direct convolution** is a multiply and an add a tap a sample: its cost grows with the response, where the
  FFT's grows with its logarithm. At 8192 taps the convolver is 30 times faster.
- **FMinus is a little faster than C++** at every size. C++'s copies and adds are `std::copy` and plain loops, and
  FMinus's loops vectorise as well as clang's do.

## Fields, locals, and what LLVM knows

`convolve` reads its module's fields into locals before its loops (`float* t = tail;`). Written the obvious way, with
`tail[i] += result[i]` on the fields themselves, the same code takes 17.3 ns a sample at 2048 taps: 1.6 times C++'s,
and no loop vectorises. A store through a `float*` might, as far as LLVM can tell, change the field `tail` itself, a
pointer in the module, so every iteration loads the field again before it indexes it, and no loop can run on vectors.

Clang knows better about C++'s fields: it marks every load and store with its type (TBAA metadata), and C's aliasing
rules say a `float` store can't change a pointer. FMinus emits no TBAA yet. Until it does, read a field into a local
before a loop that indexes it. The locals cost nothing: they live in registers.

## Where FMinus leaves it

1. **TBAA.** Type-based alias metadata on FMinus's loads and stores, as clang's, would let the obvious code run at the
   locals' speed. It also makes C's strict aliasing FMinus's rule: a `float` written through one pointer can't be read
   as an `int` through another (FMinus has no pointer casts, but a `void*` converts to both).
2. **Partitioned convolution.** The latency is a block, and the FFT is as long as the response. Splitting the response
   into partitions of the block's length (uniformly partitioned overlap-save) keeps both small, and makes a long
   reverb's response cheap at a small block size.
3. **PFFFT's own order.** For convolution, the bins' order doesn't matter: PFFFT's unordered transforms, and its
   `pffft_zconvolve_accumulate`, skip a reorder each way (extending.md, "More of the FFT").
4. **Stereo, or more channels.** One plan serves every channel; a `float4` of four channels' bins would multiply four
   spectra at once.
