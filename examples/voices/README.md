# Eight voices in modules

`voices.fm` is a small synth: eight voices, each a sawtooth through a one-pole low-pass under a linear envelope,
written in modules, FMinus's classes ([docs/extending.md](../../docs/extending.md), recipe 12). `bench.cpp` is a host
of FMinus's public API alone (`fminus/FMinus.h`). It compiles the program, allocates the synth in its own memory, plays
a score of notes through it, compares the result with the same modules written as C++ structs, and times both.

```bash
build-dev/bin/example-voices                                   # time it
build-dev/bin/example-voices --block 64                        # process called a block of 64 at a time
build-dev/bin/example-voices --check                           # what ctest runs: the comparisons only
```

## The program

A module is fields, and methods that see them by name:

```c
module OnePole
{
    float y;
    float k;

    void init(float cutoff, float rate)
    {
        k = 1 - expf(-6.28318531f * cutoff / rate);
    }

    float tick(float x) [[clang::nonblocking]]
    {
        y += k * (x - y);
        return y;
    }
};
```

and modules hold modules, and arrays of them:

```c
module Voice
{
    Osc osc;
    OnePole filter;
    Envelope envelope;

    float tick() [[clang::nonblocking]]
    {
        return envelope.tick() * filter.tick(osc.tick());
    }
};

module Synth
{
    Voice voices[8];
    float rate;
    ...
    void process(float* restrict out, int n) [[clang::nonblocking]]
    {
        for (int i = 0; i < n; ++i)
        {
            float mix = 0;
            for (int v = 0; v < 8; ++v)
                mix += voices[v].tick();
            out[i] = 0.125f * mix;
        }
    }
};
```

- **Nothing runs when a module is made.** Its fields start at 0 wherever it lives: a local, a field, an element of an
  array, FMinus's heap (`calloc(1, sizeof(Synth))`), or memory a host gives it. A method such as `init` sets them.
- **The host allocates the synth.** `fminus::Program::module("Synth")` gives its size and alignment (260 bytes,
  aligned to 4), and each field's offset. `bench.cpp` allocates that much, zeroes it, and calls the methods on it:
  `Synth::init` once, as a plugin's `prepare()` would; then `Synth::note_on`, `Synth::note_off` and
  `Synth::process`, each a C function whose first parameter is the synth's address.
- **The layout is C's.** A module's fields are laid out as a C struct's, so a C++ struct with the same fields in the
  same order is the same layout, and a host can read a field or mirror a module. `--check` checks it, against the C++
  structs in `bench.cpp`.
- **`process` cannot allocate.** It is marked `[[clang::nonblocking]]`, and so is every method it calls, which FMinus
  checks as it checks a function (recipe 11). A nonblocking method that could reach `calloc` names the method in
  clang's words: `function with 'nonblocking' attribute must not call non-'nonblocking' function 'Line::grow'`.
- **A module goes to a function by pointer.** `Voice::tick` calls `osc.tick()` on its own field; a function taking a
  voice takes a `Voice*`. Inside a method, `this` is the module's address, and `tick()` is `this->tick()`.

In the dev build on an M5, FMinus and C++ are level: 7.2 to 7.6 ns a sample each (eight voices), in either order from
run to run, with bit-identical outputs. Each voice's filter is a recursion across samples, but the eight voices'
recursions are independent, so out-of-order execution runs them side by side.

`voices.fm` is also C++, apart from two spellings: with `-Dmodule=struct -Drestrict=__restrict` and `<math.h>`, it
compiles with `clang++ -std=c++20 -Wall -Wextra -Werror -Wfunction-effects -Wno-perf-constraint-implies-noexcept`
(C++ wants a nonblocking function `noexcept` too, which FMinus has no use for).
