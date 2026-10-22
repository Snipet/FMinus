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
    fft_forward(plan, frame, bins);                            // the block, padded with zeros
    for (int k = 0; k <= size / 2; ++k)
    {
        const float2 u = bins[k];
        const float2 v = response[k];
        bins[k] = (float2)(u.x * v.x - u.y * v.y, u.x * v.y + u.y * v.x);   // times the response's spectrum
    }
    fft_inverse(plan, bins, result);
    for (int i = 0; i < size; ++i)
        tail[i] += result[i];                                  // added onto the tail
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

## Fields, and what LLVM knows

`convolve`'s loops index the module's fields themselves (`tail[i] += result[i]`), and still read each field once,
before the loop, and run on vectors. FMinus marks every load and store with its type (TBAA metadata, as clang does),
and C's strict-aliasing rule, which FMinus keeps, says a store of a `float` can't change a field that is a pointer
(`tail`) or an int (`size`). Without the marks (`fminus --no-strict-aliasing`, or FMinus before it had them), a store
through a `float*` might, as far as LLVM could tell, change the field `tail` itself, so every iteration loaded the
field again, and no loop vectorised: the same code took 15.6 ns a sample at 2048 taps, against 9.8 with the fields
read into locals first. With the marks, it takes 9.7, as the locals do.

## Where FMinus leaves it

1. **What a call leaves.** `process` loads `ready`, `gathered` and `block` again every sample, because it calls
   `convolve()`, which could change them as far as LLVM knows. A summary of the fields each method writes would keep
   them in registers; FMinus could compute one, since a pointer field is written only by its name (FMinus has no
   pointer to a pointer).
2. **Partitioned convolution.** The latency is a block, and the FFT is as long as the response. Splitting the response
   into partitions of the block's length (uniformly partitioned overlap-save) keeps both small, and makes a long
   reverb's response cheap at a small block size.
3. **PFFFT's own order.** For convolution, the bins' order doesn't matter: PFFFT's unordered transforms, and its
   `pffft_zconvolve_accumulate`, skip a reorder each way (extending.md, "More of the FFT").
4. **Stereo, or more channels.** One plan serves every channel; a `float4` of four channels' bins would multiply four
   spectra at once.
