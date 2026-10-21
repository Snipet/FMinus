# Feedback delay, its line on the heap

`delay.fm` is a feedback delay (an echo) whose delay line FMinus allocates: a small kernel for the heap, and for
keeping the heap off the audio thread ([docs/extending.md](../../docs/extending.md), recipe 11). `bench.cpp`
JIT-compiles it and creates the line once, as a plugin's `prepare()` would. It runs the whole signal through the line
in one call, compares the result with C++, times both, and frees the line.

```bash
build-release/bin/example-delay                                # time it here
build-dev/bin/example-delay --block 64                         # one call a block of 64, as a plugin calls it
build-dev/bin/example-delay --check                            # what ctest runs: the comparisons only
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
  delay.fm:41:19: error: function with 'nonblocking' attribute must not call non-'nonblocking' function 'malloc'
  ```

- **The state is two allocations**, the line and its write position, and the host passes both to every call. A
  module would hold them together, with the functions as its methods (recipe 12, and `examples/voices/`).
- **The host frees it**: `echo_destroy`, off the audio thread. FMinus's memory is C's `aligned_alloc`'s, so C's
  `free` releases it too.

In the dev build on an M5, FMinus takes 0.70 ns a sample and the same loop in C++ 0.70 ns, and their outputs are
bit-identical. Each sample is a load, a multiply and an add twice, two stores and a compare: there is no recursion
across samples to wait for (the line is longer than any distance the CPU looks ahead), so it runs at the machine's
throughput. The C++ reference is the same code, on raw pointers. An earlier one kept its line in a `std::vector` and
took 0.95 ns: the examples are compiled with LLVM's definitions, which turn on libc++'s hardening, so its
`operator[]` checked every index.

`delay.fm` is also valid C. With `<stdlib.h>`, it compiles with `clang -std=c23 -Wall -Wextra -Werror
-Wfunction-effects -ffp-contract=off` (C23, for the attribute), and that build and FMinus's agree bit for bit. Clang's
`-Wfunction-effects` finds what FMinus finds: with a `malloc` in `echo_process`, both report it, in the same words.
