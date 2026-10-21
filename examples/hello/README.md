# Hello, FMinus

`main.cpp` is FMinus as a library at its smallest: a C++ `main` that JIT-compiles a one-line FMinus program from a
string and calls its function through a C function pointer.

```bash
build-dev/bin/example-hello                                    # twice(21) = 42
```

It includes `fminus/FMinus.h` and nothing else of FMinus's. A project of your own links `FMinus::FMinus` (README.md,
"Using FMinus as a library", has the CMake).
