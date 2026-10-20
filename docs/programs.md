# Writing a program

An FMinus program can be a library of functions for a host to call, which is how a plugin uses it, or a whole
program that starts in `main`, like a C program. This page is about the second: writing `main`, running it in the
JIT, building an executable, and how the compiler gives an executable its entry point.

## A first program

`examples/note/note.fm`:

```c
// The frequency of a MIDI note, in hertz: equal temperament, A4 (note 69) at 440 Hz.
float frequency(float note)
{
    const float ln2 = 0.693147181f;
    return 440 * expf((note - 69) / 12 * ln2);   // 440 * 2^((note - 69) / 12)
}

float main(float note)
{
    return frequency(note);
}
```

Run it in the JIT, as any function:

```bash
build-dev/bin/fminus examples/note/note.fm --run main 69     # prints 440
```

Or build an executable and run that:

```bash
build-dev/bin/fminus examples/note/note.fm --emit exe -o note
./note 69                                                    # 440
./note 60                                                    # 261.62555
./note                                                       # ./note: 'main' takes 1 argument, not 0  (status 2)
./note C4                                                    # ./note: 'C4' is not a number             (status 2)
```

On Windows, `-o note` writes `note.exe` (clang-cl, the linking driver there, adds the extension), and `note 69` runs it.

## `main`

A program's entry is a function named `main` that returns a `float` or a `bool` and takes any number of `float`
parameters, including none:

```c
float main() { return 0.5f; }                                // ./prog prints 0.5
float main(float a, float b) { return sqrtf(a * a + b * b); } // ./prog 3 4 prints 5
bool main(float a, float b) { return a == b; }                // ./prog 2 2 prints true (examples/comparison/)
```

The executable:

- **reads one command-line argument per parameter**, as C's `strtof` reads it. Decimal and hex floats, `inf` and `nan`
  are numbers; `C4` and `1.5x` are not.
- **calls `main`** with them;
- **prints its result** and a newline on stdout, as `fminus --run` prints a result: a float as the shortest decimal
  that reads back as the same float (`0.6666667`, `440`, `inf`), a `bool` as `true` or `false`;
- **exits with 0**, or with **2** after a message on stderr when the arguments are wrong: too few, too many, or one
  that is not a number. (`fminus` itself exits with 1 when a program doesn't compile.)

This is where FMinus departs from C. C's `main` returns an `int`, the exit status, takes `argc` and `argv`, and
prints through `printf`. FMinus has no strings and no I/O, so the executable does the command line and the printing
for it, and `main` stays a plain function over floats. It has `int` and pointers now (extending.md, recipe 10), but
`argv` is a `char**`, which needs `char` and pointers to pointers, so C's `int main(int argc, char** argv)` waits for
them ("Where to go next").

`main` is otherwise an ordinary function:

- other functions may call it;
- `fminus --run main ...` calls it in the JIT, as any function. `--run` takes floats on its command line, so it calls
  functions whose parameters are all `float`, and prints a `float`, `bool` or `int` result;
- a host program finds it with `jit->lookup("main")`, as `float (*)(float)` or `bool (*)(float, float)`;
- a program without one compiles, runs in the JIT, and emits IR, assembly and objects as before. Only an executable
  needs it.

| Problem | Message |
|---|---|
| `--emit exe` and no `main` (or only a prototype) | fminus: an executable needs a main function, such as float main(float x) |
| a `main` with another type, such as `float main(bool b)` or `float main(float4 v)` | fminus: 'main' must return float or bool and take only float parameters |
| `--run` of a function with a `bool` parameter | fminus: --run: 'f' takes a parameter of type 'bool'; --run passes floats only |
| `--emit exe` without `-o` | fminus: --emit exe: an executable needs a file name (-o) |

## The commands

```bash
build-dev/bin/fminus prog.fm --emit exe -o prog         # an executable, at -O2
build-dev/bin/fminus prog.fm --emit exe -O0 -o prog     # unoptimised, for the debugger
build-dev/bin/fminus prog.fm --entry                    # the IR, with the entry the executable would get
build-dev/bin/fminus prog.fm --entry --emit asm -o -    # its assembly
build-dev/bin/fminus prog.fm --entry --emit obj -o prog.o
build-dev/bin/fminus prog.fm --run main 3 4             # no executable: the JIT calls main
```

`--entry` adds the entry point to whatever `--emit` writes, so an object file can be linked by hand
(`c++ prog.o build-dev/lib/libfminus_runtime.a -o prog`; on Windows,
`clang-cl prog.o build-dev\lib\fminus_runtime.lib`). `--emit exe` always adds it.

## How an executable gets its entry

C's entry point is `int main(int argc, char** argv)`. FMinus's `main` is `float main(float, ...)` or
`bool main(float, ...)`, under the same name. So before the module is optimised, `fminus::aot::addEntryPoint`
(`src/support/Aot.h`) does three things:

1. **It renames the program's `main` to `fminus.main`** and gives it internal linkage. No FMinus function can have
   that name, since an identifier can't contain a dot. Internal linkage lets the inliner fold it into its caller.
2. **It adds a C `main`**, which reads the arguments into a stack array through the runtime, calls `fminus.main`, and
   prints the result through the runtime:

```llvm
; fminus hypot.fm --entry -O0 (float main(float a, float b))
define internal float @fminus.main(float %a, float %b) {
  ...
}

define i32 @main(i32 %argc, ptr %argv) {
entry:
  %args = alloca [2 x float], align 4
  %status = call i32 @fminus_rt_main_args(i32 %argc, ptr %argv, i32 2, ptr %args)
  %0 = icmp eq i32 %status, 0
  br i1 %0, label %run, label %usage

run:
  %1 = getelementptr inbounds [2 x float], ptr %args, i32 0, i32 0
  %2 = load float, ptr %1, align 4
  %3 = getelementptr inbounds [2 x float], ptr %args, i32 0, i32 1
  %4 = load float, ptr %3, align 4
  %result = call float @fminus.main(float %2, float %4)
  call void @fminus_rt_main_result(float %result)
  ret i32 0

usage:
  ret i32 %status
}
```

3. **It verifies the new function**, and refuses a module without a `main` definition or with a `main` of another
   type. The check is on the LLVM function's type, so it covers every type the language gains: a result must be
   `float` or `i1` (a `bool`), and every parameter `float`, so a `float4` (`<4 x float>`) is refused wherever it is.

For a `bool main`, the result goes to `fminus_rt_main_bool_result` instead, declared and called with `zeroext` on its
parameter: the C ABI passes a `bool` zero-extended, as FMinus's own `bool` parameters are (extending.md, recipe 3).

```llvm
; fminus examples/comparison/comparison.fm --entry -O0
  %result = call i1 @fminus.main(float %2, float %4)
  call void @fminus_rt_main_bool_result(i1 zeroext %result)
```

The runtime functions are in `src/runtime/Runtime.cpp`, so they are in `libfminus_runtime.a`, which every executable
links:

```cpp
    // An executable's start (fminus::aot::addEntryPoint), before it calls the program's main. Reads main's `count`
    // arguments from the command line into `args`, each as C's strtof reads it, and returns 0; or tells stderr what is
    // wrong (the wrong number of arguments, or one that is not a number) and returns 2, the executable's exit status.
    std::int32_t fminus_rt_main_args(std::int32_t argc, char** argv, std::int32_t count, float* args) noexcept;

    // An executable's end: prints main's result and a newline on stdout as `fminus --run` prints a result, the
    // shortest decimal that reads back as the same float.
    void fminus_rt_main_result(float result) noexcept;

    // The same for a main that returns bool: prints true or false.
    void fminus_rt_main_bool_result(bool result) noexcept;
```

`fminus_rt_main_result` writes with `std::to_chars` into a buffer on the stack: no allocation, no exceptions, and the
same digits as the driver's `std::format("{}")`, which prints a `bool` as `true` or `false` too. All three are listed
in `RuntimeSymbols.def` like every runtime function, although only executables call them.

The order in the driver matters. The entry is added **before** `prepareModule` and `optimiseModule`, so the pipeline
optimises it with the rest: at `-O2`, `fminus.main` and the functions it calls are inlined into `main`, and the
argument array is read straight from the stack:

```llvm
; fminus examples/note/note.fm --entry -O2
define i32 @main(i32 %argc, ptr %argv) local_unnamed_addr {
entry:
  %args = alloca [1 x float], align 4
  %status = call i32 @fminus_rt_main_args(i32 %argc, ptr %argv, i32 1, ptr nonnull %args)
  %0 = icmp eq i32 %status, 0
  br i1 %0, label %run, label %common.ret

common.ret:
  ret i32 %status

run:
  %1 = load float, ptr %args, align 4
  %2 = fadd float %1, -6.900000e+01
  %3 = fdiv float %2, 1.200000e+01
  %4 = fmul float %3, f0x3F317218
  %5 = call float @llvm.exp.f32(float %4)
  %6 = fmul float %5, 4.400000e+02
  call void @fminus_rt_main_result(float %6)
  br label %common.ret
}
```

Then `aot::emitExecutable` writes the object to a temporary file and links it with the compiler driver the build tree
was configured with, against `libfminus_runtime.a` (and, in the `asan` and `tsan` presets, the sanitizer runtimes).
That is the same path `fminus-build` takes for hand-written IR; README.md's "Executables" has the details.

## In the debugger

An executable built at `-O0` keeps every function, so lldb can stop in them and show their registers. FMinus emits no
debug information yet, so lldb shows assembly rather than source lines:

```
$ lldb -- ./note 69
(lldb) breakpoint set -n frequency
Breakpoint 1: where = note`frequency, address = 0x0000000100000538
(lldb) run
* thread #1, stop reason = breakpoint 1.1
    frame #0: 0x0000000100000538 note`frequency
(lldb) bt
  * frame #0: 0x0000000100000538 note`frequency
    frame #1: 0x00000001000005b8 note`fminus.main + 28
    frame #2: 0x00000001000005f8 note`main + 52
(lldb) register read s0
      s0 = 69
(lldb) finish
(lldb) register read s0
      s0 = 440
```

The argument and the result are in `s0`, as the C calling convention puts the first float. At `-O2` the functions are
inlined into `main` and there is nothing to break on but `main` itself. Source-level debugging needs `llvm::DIBuilder`
([extending.md](extending.md), "Debug information").

## Tests

- `tests/lit/fminus/main.fm` builds an executable and runs it: a result, too few arguments, an argument that isn't a
  number, a `main` with no parameters (from stdin), a program with no `main`, `--emit exe` without `-o`, the IR of
  `--entry`, and `examples/note/note.fm`, built and run. The same `main` also runs through `--run`, with the same
  output.
- `tests/unit/AotTest.cpp` checks the IR `addEntryPoint` leaves (an internal `fminus.main`, an external
  `i32 main(i32, ptr)`, the two runtime declarations) and what it refuses: no `main`, a prototype only, an `i1`
  result, a vector parameter, a variadic `main`.

## Where to go next

- **Printing.** A runtime function `fminus_rt_print(float x)` that writes `x` and returns it, made a builtin `print`
  (recipe 5's pattern), would let a program trace values in a loop. It belongs to executables and debugging: inside a
  plugin, nothing on the audio thread may write to a stream.
- **An exit status.** With `int` (recipe 10), `int main(...)` could return the status as C does: `addEntryPoint`
  would return the result from C's `main` instead of printing it.
- **Strings and buffers.** With `char` and pointers to pointers, `main` could take C's `argc` and `argv`, and runtime
  functions could read and write audio files into buffers (recipe 10's pointers), so an FMinus program processes a
  file from the command line.
- **Debug information**, so lldb shows FMinus source.
