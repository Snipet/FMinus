# Saturator: tanh, sample by sample and a poly float at a time

`saturator.fm` is a saturator, `y = tanh(drive * x)`: the soft clip of a distortion or a tape stage. It is stateless:
each output sample depends on its input sample alone. So nothing stops a vector from taking several samples at once,
and it is the simplest place to compare a loop over samples with a loop over `poly float`s (docs/simd.md, recipe 9).
`bench.cpp` compiles it through FMinus's library API (`fminus::Program`, as a plugin would), runs it over noise,
compares it with C++, and times it.

```bash
cmake --workflow --preset release                                      # once: also builds LLVM without assertions
build-release/bin/example-saturator                                    # time it here
build-dev/bin/example-saturator -O0                                    # FMinus unoptimised
build-dev/bin/example-saturator --poly-lanes 16                        # a poly float of 16 lanes
build-dev/bin/example-saturator --block 64                             # one call a block of 64
build-dev/bin/example-saturator --check                                # what ctest runs: the comparisons only
build-dev/bin/fminus examples/saturator/saturator.fm --emit asm -o -   # what the JIT generates
```

## The program

Four kernels, two tanh by two loops. The loops:

```c
void saturate(const float* restrict input, float* restrict output, int n, float drive) [[clang::nonblocking]]
{
    for (int i = 0; i < n; ++i)
        output[i] = rational_tanh(drive * input[i]);
}

void saturate_poly(const float* restrict input, float* restrict output, int n, float drive) [[clang::nonblocking]]
{
    int i = 0;
    for (; i + poly_lanes() <= n; i += poly_lanes())
        poly_store(output + i, rational_tanh_poly(drive * poly_load(input + i)));
    const poly bool rest = i + poly_index() < n;
    poly_store(output + i, rational_tanh_poly(drive * poly_load(input + i, rest)), rest);
}
```

The poly loop takes `poly_lanes()` samples a step through `poly_load` and `poly_store`, and the last, partial step
under a mask, which keeps its loads and stores inside the buffer (docs/simd.md, "Loads and stores"). The tanh:

- **`tanhf`**, `<math.h>`'s: `saturate_tanhf` and `saturate_tanhf_poly`. LLVM can only call it. Of a `poly float` it
  is `llvm.tanh.v8f32`, which the backend splits into one call a lane, unless the target has a vector `tanhf`: on
  Apple systems libsystem_m's `_simd_tanh_f4`, for 4 lanes (recipe 8).
- **A rational function**, `rational_tanh`: x times a polynomial of degree 12 in x over one of degree 6 (Eigen's
  fast tanh for float, with its coefficients), x clamped first to ±7.905311, past which tanh is ±1 in float. It is
  multiplies, adds, a divide and a clamp, so of a `poly float` it is one vector instruction each. Evaluated in its
  order, without fused multiply-adds, it is within 3.5e-7 of tanh, and within 6 ulp for |x| above 1e-35. `--check`
  checks the first bound against tanh in double, over noise and over a ramp from -10 to 10.

FMinus has no overloading, as C has none, so `rational_tanh_poly` is the same body again, on `poly float`s.

## What it measures

On this sandbox's Linux machine (an Intel Xeon, Emerald Rapids, with AVX-512), in the dev build, one call for 48000
samples of noise at drive 4. A `poly float` has 8 lanes there: LLVM's cost model prefers 256-bit vectors on Intel's
AVX-512 CPUs. The C++ is the system's clang 18 at `-O2`, for the baseline x86-64, so its vectors are SSE's 4 lanes;
FMinus's JIT compiles for the CPU it runs on.

| ns a sample | FMinus `-O2` | FMinus `-O0` | C++ |
|---|---|---|---|
| `tanhf`, a sample at a time | 15.2 | 17.4 | 15.2 |
| `tanhf`, a `poly float` at a time | 15.2 | 15.3 | |
| rational, a sample at a time | 0.44 | 9.95 | 0.83 |
| rational, a `poly float` at a time | 0.44 | 1.33 | |

Expect noise of a few ns between runs in the `tanhf` lines: this machine is shared.

- **The library call is the cost.** glibc has no vector `tanhf`, so 8 lanes are 8 calls, at 15 ns a sample either
  way. The rational function takes 0.44 ns, 35 times less, and is within 3.5e-7 of tanh (glibc's `tanhf`, about
  1e-7 here). That is 3.5 ns a step of 8 samples, for some 25 vector instructions and no dependency from one step to
  the next.
- **At `-O2`, the two loops compile to the same code.** The sample-at-a-time loop has no dependency from one
  sample to the next, and `restrict` rules out overlap, so LLVM's loop vectoriser widens it to 8 lanes itself, clamp
  and divide included. `poly float` gives that by construction: at `-O0`, with no vectoriser, the poly loop is 7.5
  times faster than the scalar one. Where the vectoriser gives up (a sum, as in `examples/meter/`), the poly loop is
  the one that stays vector.
- **The width is the machine's, and it matters.** `--poly-lanes 4` (SSE's, NEON's) gives 0.84 ns for the poly loop:
  half the throughput, and the same as C++'s 4-lane SSE. `--poly-lanes 16` gives 0.37, in AVX-512's whole `zmm`
  registers. At 8 and at 16 lanes, the masked tail is a `vmovups` under an AVX-512 mask register (`k1`).
  `--poly-lanes 1` is a scalar loop that the vectoriser leaves alone, since its operations are on vectors of one
  lane: 4.0 ns.
- **FMinus and C++ agree bit for bit**, for the rational function (the same operations in the same order, no
  contraction on either side) and for `tanhf` (the same glibc function). The rational poly kernel gives the scalar
  one's output at every width from 1 to 64, and every kernel gives the same output in blocks of 64 and 61 as in one
  call, which `--check` checks.

On an Apple Silicon Mac, the `tanhf` lines are vector code too: `_simd_tanh_f4` replaces the call a lane in the 4-lane
poly loop, and the loop vectoriser uses it for the scalar loop's body. It differs from `tanhf` in the last bits: by
at most 3 ulp, for about 170 million of the 4.3 billion floats, over every finite float on an M5. FMinus and C++ then
differ by up to 6e-8 (`--check`'s tolerance is 1e-6). And the plain loop's output depends on where a block ends: its
body meets the vector function and its remainder the scalar one, so moving the end of a block moves samples from one
to the other. `--check` allows the two `tanhf` kernels those 3 ulp across block sizes (it measures 2 here), and still
holds the rational kernels, FMinus's own arithmetic, to the same output bit for bit.

## Where FMinus leaves it

1. **Overloading.** `rational_tanh` and `rational_tanh_poly` are one function written twice. OpenCL C's
   `__attribute__((overloadable))` or C11's `_Generic` would make one name for both; a generic `poly` parameter that
   a scalar argument instantiates would make one body.
2. **Fused multiply-adds.** Each step of the two polynomials is a multiply and then an add. With contraction (C's
   `#pragma STDC FP_CONTRACT ON`, or a `contract` flag on the function), each becomes one `vfmadd` on x86 or `fmla` on
   NEON: shorter, and more accurate. The output then differs from C++'s built with `-ffp-contract=off`, so
   `--tolerance` starts to matter.
3. **A cheaper divide.** The divide is the loop's slowest instruction. `arcp` (fast-math) would allow a reciprocal
   estimate refined by a Newton step in its place (clang's `-mrecip`), trading the last bits for speed.
4. **A vector `tanhf` on Linux.** glibc's libmvec (2.35 and later) has `_ZGVdN8v_tanhf` (AVX2, 8 lanes) and
   `_ZGVeN16v_tanhf` (AVX-512), but LLVM's libmvec table (`-fveclib=libmvec`) lists no `tanhf`, and FMinus registers
   Apple's library only (`src/support/HostTarget.cpp`). Mapping `llvm.tanh.v8f32` to it, and resolving libmvec in the
   JIT, would make the `tanhf` lines vector code too.
5. **Oversampling.** A saturator adds harmonics that alias. Running it at 2× or 4× the sample rate, with a half-band
   filter on each side, is the usual fix, and makes the kernel's speed matter four times over.
