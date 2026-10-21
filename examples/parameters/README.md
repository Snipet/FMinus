# Parameters: a filter swept by an LFO

`parameters.fm` has two modules with parameters, the values a host sets ([docs/extending.md](../../docs/extending.md),
recipe 14). `Lfo` writes a cutoff a sample, `centre * 2^(depth * sin(2 pi phase))`. `Filter` is Chamberlin's
state-variable filter, a low-pass, a band-pass and a high-pass from one pair of integrators. `bench.cpp` compiles the
file three ways through `fminus::Program` and runs noise through the filter, the LFO sweeping its cutoff, or not. It
compares each output with the same modules written in C++, and times them.

```bash
build-dev/bin/example-parameters                   # calls of 64 samples
build-dev/bin/example-parameters --block 256       # longer calls: a coarser staircase once a call
build-dev/bin/example-parameters --check           # what ctest runs: the comparisons only
```

## The program

```c
module Filter
{
    param float cutoff [[fminus::range(20, 8000)]] [[fminus::default(1000)]];          // Hz
    param float damping [[fminus::range(0.1, 2)]] [[fminus::default(0.5)]];           // 1 / Q: below 1, a peak
    param int shape [[fminus::range(0, 2)]];                                           // low-, band-, high-pass
    param float sampleRate [[fminus::range(8000, 384000)]] [[fminus::default(48000)]];
    float low;
    float band;

    void process(const float* restrict in, float* restrict out, int n) [[clang::nonblocking]]
    {
        const float w = 3.14159265f / sampleRate[0];
        for (int i = 0; i < n; ++i)
        {
            const float f = 2 * sinf(w * cutoff[i]);
            low += f * band;
            const float high = in[i] - low - damping[i] * band;
            band += f * high;
            out[i] = shape[i] == 0 ? low : shape[i] == 1 ? band : high;
        }
    }
};
```

Nothing in it says how a parameter arrives: `cutoff[i]` is the cutoff at sample i, and the host decides, when it
compiles, what that is. The harness's three programs:

| Program | `Options::parameters` | The host, each call |
|---|---|---|
| knobs | nothing: every parameter is a knob | runs the LFO into a buffer, `setValue(filter, cutoff, buffer[0])`, runs the filter |
| the cutoff a stream | `"Filter::cutoff"`: `Binding::stream()` | runs the LFO into a buffer, `setStream(filter, cutoff, buffer)`, runs the filter |
| frozen | each of the filter's parameters: `Binding::frozen(its default)` | runs the filter |

The first is a plugin that reads its controls once a block, the second one that modulates its cutoff a sample at a
time, the third one that has compiled its fixed settings in. A host builds its list of controls from the program, not
from a copy of it: `Module::parameters` gives each parameter's type, range and default (the harness prints them), and
`setDefaults` sets an instance to the defaults. The C++ reference takes its values from there too.

## Measurements

In the dev build on an M5, 96,000 samples of noise, each time the best of 20 runs, the median of three:

| | calls of 64 | calls of 256 | C++, calls of 64 |
|---|---|---|---|
| swept a sample: the cutoff a stream | 5.7 ns/sample | 6.0 ns/sample | 5.6 ns/sample |
| swept once a call: the cutoff a knob | 5.6 ns/sample | 5.9 ns/sample | 5.7 ns/sample |
| fixed: knobs | 3.3 ns/sample | 3.3 ns/sample | 3.3 ns/sample |
| fixed: frozen | 3.3 ns/sample | 3.4 ns/sample | |

(The swept rows include the LFO, a `sinf` and an `expf` a sample.)

- **Every output is C++'s, bit for bit**, under each binding: the same operations in the same order, and the same
  libm. The sample-accurate output is also the same in calls of 61, of 256, or one call for the whole run, which
  `--check` compares; the once-a-call output isn't, since its cutoff moves where the calls start. The frozen filter's
  output is the knobs' at the same values, bit for bit: LLVM folds the coefficient, 2 sin(pi 1000 / 48000), to the
  float libm's `sinf` gives.
- **A sweep a sample costs a sine a sample, and it barely shows.** As a knob, the cutoff is a register, so LLVM moves
  `2 * sinf(w * cutoff[i])` out of the loop: one sine a call. As a stream it stays in the loop. But the coefficients
  don't depend on the filter's state, so the processor computes them ahead while the recursion waits: 1–3% over the
  knob's time.
- **The difference is in the sound.** Swept once a call, the cutoff is a staircase, a step every call (zipper noise):
  the output is 0.0068 of its peak away from the sample-accurate one in calls of 64, and 0.034 in calls of 256.
- **A knob is as fast as a constant.** It is loaded once, when the method starts, and is a register from then on.
  Frozen, the filter loses its sine (computed at compile time), its `damping` load, and the branches on `shape`, which
  leave the loop one block. None of that shows here: the filter is bound by its recursion, each sample waiting for the
  last's `low` and `band`, a chain of dependent multiplies and adds. Freezing pays where a constant changes the code
  that matters: a loop count that unrolls, a branch whose arms are long, a choice between two loops that vectorise.

## Where FMinus leaves it

1. **A smoothed knob.** Between a knob's step a call and a stream's buffer, a binding that ramps from the last call's
   value to this call's across the call would remove the staircase at a knob's price.
2. **Freezing while it plays.** A host that freezes a setting compiles again, off the audio thread, and swaps the
   function pointers: every binding has the same slots, so the instance, with its state, runs under the new program
   as it ran under the old (`tests/unit/ParamTest.cpp` runs one instance under three programs).
3. **Modulation inside FMinus.** Here the host passes the LFO's buffer to the filter. A program could connect them
   itself, a voice module calling the LFO into a buffer and the filter on it, if a method could hand a stream to a
   module it holds.
