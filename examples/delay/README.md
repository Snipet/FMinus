# Feedback delay, its line on the heap

`delay.fm` is a feedback delay (an echo) whose delay line FMinus allocates: a small kernel for the heap, and for
keeping the heap off the audio thread ([docs/extending.md](../../docs/extending.md), recipe 11). It is written as
plain C functions. `echo.fm` is the same echo as a module, its state in fields and its feedback and mix parameters
(recipes 12 and 14). `bench.cpp` compiles both through `fminus::Program` and creates each one's line once, as a
plugin's `prepare()` would. It runs the whole signal through each in one call, compares both with C++, times all
three, and frees the lines.

```bash
build-release/bin/example-delay                                # time it here
build-dev/bin/example-delay --block 64                         # one call a block of 64, as a plugin calls it
build-dev/bin/example-delay --check                            # what ctest runs: the comparisons only
build-dev/bin/example-delay --module my-echo.fm                # another module Echo, against the same C++
```

## The program

What allocates and what runs on the audio thread are separate functions:

```c
float* echo_create(int length)                                   // off the audio thread: allocates
{
    return calloc(length, sizeof(float));
}

void echo_process(float* restrict line, int* restrict position, int length, const float* restrict input,
                  float* restrict output, int n, float feedback, float mix) [[clang::nonblocking]]
{
    int w = *position;
    for (int i = 0; i < n; ++i)
    {
        const float delayed = line[w];
        const float x = input[i];
        line[w] = x + feedback * delayed;
        output[i] = x + mix * delayed;
        if (++w == length)
            w = 0;
    }
    *position = w;
}
```

- **The program decides how much memory it needs**: the line's length depends on the sample rate
  (`echo_length(seconds, rate)`), which only `prepare()` knows. The host calls `echo_create` there, once.
- **The memory starts silent.** FMinus's `calloc` and `malloc` both zero what they return (as every variable FMinus
  declares starts at 0), aligned to 64 bytes. `--check` checks both.
- **`echo_process` cannot allocate.** It is marked `[[clang::nonblocking]]`, so FMinus refuses to compile it if it
  could reach `malloc`, `calloc` or `free`, directly or through any function it calls. Put
  `float* oops = malloc(4);` first in its body, and the program does not compile:

  ```
  delay.fm:42:19: error: function with 'nonblocking' attribute must not call non-'nonblocking' function 'malloc'
  ```

- **The host frees it**: `echo_destroy`, off the audio thread. FMinus's memory is C's `aligned_alloc`'s, so C's
  `free` releases it too (on Windows, `_aligned_malloc`'s, which `_aligned_free` releases).
- **The state is two allocations**, the line and its write position, and the host passes both to every call.
  `echo.fm` holds them together (below).

## The module

```c
module Echo
{
    param float feedback [[fminus::range(0, 0.99)]] [[fminus::default(0.5)]];
    param float mix [[fminus::range(0, 1)]] [[fminus::default(0.5)]];
    float* line;
    int length;     // in whole samples
    int position;   // where the next sample is written

    bool init(float seconds, float rate) { ... }   // off the audio thread: calloc's the line

    void process(const float* restrict input, float* restrict output, int n) [[clang::nonblocking]]
    {
        float* d = line;
        const int size = length;
        int w = position;
        for (int i = 0; i < n; ++i)
        {
            const float delayed = d[w];
            const float x = input[i];
            d[w] = x + feedback[i] * delayed;
            output[i] = x + mix[i] * delayed;
            if (++w == size)
                w = 0;
        }
        position = w;
    }

    void end() { free(line); }                     // off the audio thread
};
```

- **The host holds one instance**, which it allocates (zeroed: `Program::module("Echo")` gives the size and
  alignment), sets to its parameters' defaults (`setDefaults`), and hands to `init` once. Each call of `process` then
  takes only the buffers.
- **The feedback and the mix are knobs**: values the host sets with `setValue`, from any thread, which `process`
  reads once when it starts. `feedback[i]` is the same value at every sample of a call, a register through the loop.
- **The fields go into locals before the loop.** FMinus tells LLVM nothing about which types alias, so a store
  through the line could, as far as LLVM knows, change `line` or `position` themselves, and the loop would load them
  again every sample ([../convolver/README.md](../convolver/README.md), "Fields, locals, and what LLVM knows").

## Measurements

In the dev build on an M5, the functions take 0.68 ns a sample, the module 0.68 ns and the same loop in C++ 0.68 ns
(the best of 20 runs, the median of three), and their outputs are bit-identical, in one call or in blocks of 64 or
61. Each sample is a load, a multiply and an add twice, two stores and a compare: there is no recursion across
samples to wait for (the line is longer than any distance the CPU looks ahead), so it runs at the machine's
throughput. The C++ reference is the same code, on raw pointers. An earlier one kept its line in a `std::vector` and
took 0.95 ns: the examples are compiled with LLVM's definitions, which turn on libc++'s hardening, so its
`operator[]` checked every index.

`delay.fm` is also valid C, and is FMinus's example of it (`echo.fm`'s module and parameters are FMinus's own). With
`<stdlib.h>`, it compiles with `clang -std=c23 -Wall -Wextra -Werror -Wfunction-effects -ffp-contract=off` (C23, for
the attribute), and that build and FMinus's agree bit for bit. Clang's `-Wfunction-effects` finds what FMinus finds:
with a `malloc` in `echo_process`, both report it, in the same words.
