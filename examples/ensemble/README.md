# Sixteen voices at once

`ensemble.fm` is a synth of sixteen voices, each a band-limited sawtooth (its partials summed, up to 32), which renders
a block into a buffer of its own. The voices don't touch each other, so a `parallel for` spreads them over the
machine's cores, and the mix after it has every voice's block; the mix itself is two loops, the left channel's and the
right's, which a `parallel { }` runs at once ([docs/extending.md](../../docs/extending.md), recipe 22). `bench.cpp` is
a host of FMinus's public API alone (`fminus/FMinus.h`): it compiles the program twice, its regions on every core and
on the calling thread alone (`Program::Options::threads = 1`), plays a chord through each, compares the two outputs bit
for bit, and times both.

```bash
build-dev/bin/example-ensemble                       # time it, in blocks of 512
build-dev/bin/example-ensemble --block 64            # in blocks of 64
build-dev/bin/example-ensemble --threads 4           # the regions on 4 threads at most
build-dev/bin/example-ensemble --check               # what ctest runs: the comparisons only
```

## The program

```c
module Ensemble
{
    param float gain [[fminus::range(0, 1)]] [[fminus::default(0.1)]];
    param float rate [[fminus::range(8000, 384000)]] [[fminus::default(48000)]];
    Voice voices[16];
    ...
    void process(float* restrict left, float* restrict right, int n) [[clang::nonblocking]]
    {
        parallel for (int v = 0; v < 16; ++v)   // each voice, its own block: an element each
            voices[v].render(n);
        parallel                                // the two channels at once: each writes its own buffer
        {
            for (int i = 0; i < n; ++i) { ... left[i] = gain[i] * sum; }    // the even voices
            for (int i = 0; i < n; ++i) { ... right[i] = gain[i] * sum; }   // the odd ones
        }
    }
};
```

Each iteration of the `parallel for` calls `render` on its own voice, an element of `voices`, which the compiler lets
it do; `render` changes its voice, so an iteration that called it on one voice outside the array (`solo.render(n)`)
would be an error: every iteration would change it at once. The block's two statements each write their own channel
through its pointer and read the voices' buffers, which neither changes. `gain` is a knob: `process` reads it once,
where it starts, and both statements see that value.

## What it measures

On an M5 (four performance cores, six efficiency cores), at -O2, a second of audio (48,000 samples), the median of
three runs, each against its own run on one thread:

| Blocks of | one thread | 4 threads | 10 threads |
|---|---|---|---|
| 512 samples | 384 ns/sample | 130 ns/sample (3.0×) | 84 ns/sample (4.6×) |
| 64 samples | 372 ns/sample | 174 ns/sample (2.1×) | 123 ns/sample (3.1×) |

The outputs are bit-identical in every case: a voice computes on any thread what it would on the caller's, and the
mix adds the voices in the same order. Smaller blocks gain less, since each region costs its synchronisation, a few
microseconds, whatever its size (recipe 22's measurements).

`--check` fails when the two programs' outputs differ at all, when blocks of 61 samples change the output, when a
channel is silent, or when `process` is not `[[clang::nonblocking]]`.
