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

Or build an executable and run that, as with a C compiler:

```bash
build-dev/bin/fminus examples/note/note.fm -o note             # or --emit exe -o note
./note 69                                                    # 440
./note 60                                                    # 261.62555
./note                                                       # ./note: 'main' takes 1 argument, not 0  (status 2)
./note C4                                                    # ./note: 'C4' is not a number             (status 2)
```

An installed `fminus` ([building.md](building.md), "Installing") does the same from anywhere: `fminus note.fm -o note`.
On Windows, `-o note` writes `note.exe` (clang-cl, the linking driver there, adds the extension), and `note 69` runs it.

## `main`

A program's entry is a function named `main` that returns a `float`, a `double` or a `bool` and takes any number of
`float` and `double` parameters, including none:

```c
float main() { return 0.5f; }                                // ./prog prints 0.5
float main(float a, float b) { return sqrtf(a * a + b * b); } // ./prog 3 4 prints 5
bool main(float a, float b) { return a == b; }                // ./prog 2 2 prints true (examples/comparison/)
double main(double seconds, float rate)                       // ./prog 3600 48000 prints 3599.999983778296
{
    double t = 0;
    for (int i = 0; i < (int)(seconds * rate); ++i)
        t += 1.0 / rate;                                      // C's double 1.0, and a double quotient: t is one
    return t;
}
```

The executable:

- **reads one command-line argument per parameter**, as C's `strtof` reads a float and `strtod` a double. Decimal and
  hex floats, `inf` and `nan` are numbers; `C4` and `1.5x` are not.
- **calls `main`** with them;
- **prints its result** and a newline on stdout, as `fminus --run` prints a result: a float as the shortest decimal
  that reads back as the same float (`0.6666667`, `440`, `inf`), a double as the shortest that reads back as the same
  double (`0.6666666666666666`), a `bool` as `true` or `false`;
- **exits with 0**, or with **2** after a message on stderr when the arguments are wrong: too few, too many, or one
  that is not a number. (`fminus` itself exits with 1 when a program doesn't compile.)

This is where FMinus departs from C. C's `main` returns an `int`, the exit status, takes `argc` and `argv`, and
prints through `printf`. FMinus has no I/O (and its strings are read-only views), so the executable does the command
line and the printing for it, and `main` stays a plain function over floats. It has `int` and pointers now
(extending.md, recipe 10), and `char` as an array's element (recipe 46), but `argv` is a `char**`, which needs pointers
to pointers, so C's `int main(int argc, char** argv)` waits for
them ("Where to go next").

`main` is otherwise an ordinary function:

- other functions may call it;
- `fminus --run main ...` calls it in the JIT, as any function. `--run` reads each argument as its parameter's type,
  a `bool`, `int`, `float` or `double` in any mix (or an array, `1,2,3`, for a pointer), and prints a `float`,
  `double`, `bool` or `int` result, through the thunk a host's `Program::call` uses
  ([extending.md](extending.md), recipe 36);
- a host program finds it with `jit->lookup("main")`, as `float (*)(float)` or `bool (*)(float, float)`;
- a program without one compiles, runs in the JIT, and emits IR, assembly and objects as before. Only an executable
  needs it.

| Problem | Message |
|---|---|
| an executable (`-o prog`, `--emit exe`) and no `main` (or only a prototype) | fminus: an executable needs a main function, such as float main(float x) |
| a `main` with another type, such as `float main(bool b)` or `float main(float4 v)` | fminus: 'main' must return float, double or bool and take only float and double parameters |
| `--run` of a function with a `float4` parameter | fminus: --run: 'f' takes a parameter of type 'float4'; --run passes bools, ints, floats, doubles and arrays of ints, floats and doubles only |
| `--run f 1.5` of `float f(int n)` | fminus: --run: '1.5' is not an int |
| `--emit exe` without `-o` | fminus: --emit exe: an executable needs a file name (-o) |
| no C++ toolchain where `fminus` looks | fminus: linkExecutable: no C++ compiler to link with: tried /usr/bin/clang++ (the compiler FMinus was built with), c++ (on PATH), clang++ (on PATH). ... |

## The commands

`fminus`'s command line is a C compiler's where the two meet (`tools/fminus/main.cpp` has all of it):

| Command | What it writes |
|---|---|
| `fminus prog.fm` | the optimised IR on stdout, as it always has |
| `fminus prog.fm -o prog` | an executable that runs `main`: with no `--emit`, `-c`, `-S` or `-emit-llvm`, `-o`'s name says what to write |
| `fminus prog.fm -o prog.ll` (`.bc`, `.s` or `.asm`, `.o` or `.obj`) | LLVM IR, bitcode, assembly or an object file, by the extension, in any case |
| `fminus pedal.cir -o pedal.fm` | the FMinus module a circuit netlist generates (`--emit fm`; [circuits.md](circuits.md)) |
| `fminus prog.fm -o /dev/null` | the IR: a device or a pipe gets what it got before |
| `fminus -c prog.fm` | an object file, `prog.o` in the current directory (clang's `-c`; with `-o`, that file) |
| `fminus -S prog.fm` | this machine's assembly, `prog.s` |
| `fminus -S -emit-llvm prog.fm` | LLVM IR, `prog.ll` (`-c -emit-llvm`: bitcode, `prog.bc`; `-emit-llvm` alone: the IR on stdout) |
| `fminus prog.fm --emit ast` | the syntax tree (`--emit ll`, `bc`, `asm`, `obj`, `exe`, `fm`, whatever `-o`'s name; on stdout without `-o`, but `exe`) |
| `fminus prog.fm --run main 3 4` | nothing: the JIT calls `main` and prints its result |
| `fminus --version` | FMinus's version, its LLVM's, the target, the build's features (`+json +xml -web ...`) and where it is |
| `fminus --print-runtime` | what an executable is linked with, and through ([building.md](building.md), "Installing") |

Two of `--emit`, `-c`, `-S` and `-emit-llvm` that ask for different outputs are an error (`fminus: -c and -S ask for
different outputs (an object file, assembly)`), and so is a `.fm` name for what is not a netlist. Before `-o` read its
name, `fminus prog.fm -o prog` wrote the IR to `prog`; a script that relied on that names the file `.ll` or passes
`--emit ll`.

```bash
build-dev/bin/fminus prog.fm -o prog                    # an executable, at -O2 (--emit exe -o prog: the same)
build-dev/bin/fminus prog.fm -O0 -o prog                # unoptimised, for the debugger
build-dev/bin/fminus prog.fm --entry                    # the IR, with the entry the executable would get
build-dev/bin/fminus prog.fm --entry --emit asm -o -    # its assembly
build-dev/bin/fminus prog.fm --entry -c -o prog.o       # its object (--entry --emit obj -o prog.o)
build-dev/bin/fminus prog.fm --run main 3 4             # no executable: the JIT calls main
build-dev/bin/fminus prog.fm --loop-budget 1000000 -o prog   # main may take a million branches back
build-dev/bin/fminus prog.fm --bounds-check -o prog          # main's arrays' indexes checked
```

With `--loop-budget n` the program counts its loops against a budget (extending.md, recipe 29), and the entry sets it
to n before it calls `main`: a `main` that runs past it returns early, and the executable says so on stderr (`'main'
ran past its loop budget of 1000000 and returned early`) and exits with 3 instead of printing a result.

With `--bounds-check` each access whose array the compiler sees is checked against the array's extent (extending.md,
recipe 30): when one fails, `main` returns early, before the access, and the executable says where on stderr
(`prog.fm:4:13: index 7 out of bounds for 'a' of type 'float[4]': 'main' returned early`) and exits with 4 instead of
printing a result.

`--entry` adds the entry point to whatever `--emit` writes, so an object file can be linked by hand
(`c++ prog.o build-dev/lib/libfminus_runtime.a -o prog`, or with an installed `fminus`'s runtime,
``c++ prog.o "$(fminus --print-runtime | sed -n 's/^runtime: //p')" -o prog``; on Windows,
`clang-cl prog.o build-dev\lib\fminus_runtime.lib`). An executable always has it.

## How an executable gets its entry

C's entry point is `int main(int argc, char** argv)`. FMinus's `main` is `float main(float, ...)`, `double main(...)` or
`bool main(...)`, its parameters floats and doubles, under the same name. So before the module is optimised,
`fminus::aot::addEntryPoint` (`src/support/Aot.h`) does three things:

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
   `float`, `double` or `i1` (a `bool`), and every parameter `float` or `double`, so a `float4` (`<4 x float>`) is
   refused wherever it is.

For a `bool main`, the result goes to `fminus_rt_main_bool_result` instead, declared and called with `zeroext` on its
parameter: the C ABI passes a `bool` zero-extended, as FMinus's own `bool` parameters are (extending.md, recipe 3).

```llvm
; fminus examples/comparison/comparison.fm --entry -O0
  %result = call i1 @fminus.main(float %2, float %4)
  call void @fminus_rt_main_bool_result(i1 zeroext %result)
```

A `main` with a `double` among its parameters reads them all into doubles, each as its type says (`strtod` for a
double, `strtof` for a float, which the double holds exactly), through `fminus_rt_main_double_args`, whose fourth
argument is a string of their kinds, a character each; the entry rounds a float's back, exactly. A `double main`
prints through `fminus_rt_main_double_result`. A `main` of floats keeps `fminus_rt_main_args`, its code unchanged:

```llvm
; fminus clock.fm --entry -O0 (double main(double seconds, float rate))
@main.kinds = private unnamed_addr constant [3 x i8] c"df\00", align 1
  ...
  %args = alloca [2 x double], align 8
  %status = call i32 @fminus_rt_main_double_args(i32 %argc, ptr %argv, i32 2, ptr @main.kinds, ptr %args)
  ...
  %5 = fptrunc double %4 to float
  %result = call double @fminus.main(double %2, float %5)
  call void @fminus_rt_main_double_result(double %result)
```

The runtime functions are in `src/runtime/Runtime.cpp`, so they are in `libfminus_runtime.a`, which every executable
links:

```cpp
    // An executable's start (fminus::aot::addEntryPoint), before it calls the program's main. Reads main's `count`
    // arguments from the command line into `args`, each as C's strtof reads it, and returns 0; or tells stderr what is
    // wrong (the wrong number of arguments, or one that is not a number) and returns 2, the executable's exit status.
    std::int32_t fminus_rt_main_args(std::int32_t argc, char** argv, std::int32_t count, float* args) noexcept;

    // The same for a main with a double among its parameters: reads each of its `count` arguments as `kinds` says, a
    // character a parameter: 'd' as C's strtod reads a double, 'f' as strtof reads a float (stored widened, exactly,
    // which the entry rounds back), into `args`.
    std::int32_t fminus_rt_main_double_args(std::int32_t argc, char** argv, std::int32_t count, const char* kinds,
                                            double* args) noexcept;

    // An executable's end: prints main's result and a newline on stdout as `fminus --run` prints a result, the
    // shortest decimal that reads back as the same float.
    void fminus_rt_main_result(float result) noexcept;

    // The same for a main that returns double: the shortest decimal that reads back as the same double.
    void fminus_rt_main_double_result(double result) noexcept;

    // The same for a main that returns bool: prints true or false.
    void fminus_rt_main_bool_result(bool result) noexcept;
```

`fminus_rt_main_result` and `fminus_rt_main_double_result` write with `std::to_chars` into a buffer on the stack: no
allocation, no exceptions, and the same digits as the driver's `std::format("{}")`, which prints a `bool` as `true` or
`false` too. All five are listed in `RuntimeSymbols.def` like every runtime function, although only executables call
them.

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
  %exp = fcmp ogt float %4, 8.900000e+01
  %exp1 = select i1 %exp, float 8.900000e+01, float %4
  ...
  %exp25 = call float @llvm.fma.f32(float %exp14, float %exp21, float %exp21)
  %exp26 = fmul float %exp25, %exp24
  %5 = fmul float %exp26, 4.400000e+02
  call void @fminus_rt_main_result(float %5)
  br label %common.ret
}
```

Then `aot::emitExecutable` writes the object to a temporary file and links it through a C++ compiler driver against
`libfminus_runtime.a` (and, in the `asan` and `tsan` presets, the sanitizer runtimes): `aot::findToolchain` finds both,
the runtime beside an installed `fminus` (`../lib/fminus`) or in a build tree's own tree, and the driver from
`FMINUS_LINK_DRIVER`, the compiler FMinus was built with, or the system's `c++` or `clang++` (`clang-cl` on Windows),
as `fminus --print-runtime` prints ([building.md](building.md), "Installing"). The object is the system linker's to
take as it is: on Linux it is position-independent, in the small code model, the code the JIT generates there
(`support/HostTarget.h`, `hostFileTargetMachine`), so an executable links with no text relocations and no warning.
That is the same path `fminus-build` takes for hand-written IR; "Executables, in detail" (below) has the details.

In the `-O2` listing, `expf` is the default math mode's ([language.md](language.md) has the modes): FMinus's own
kernel, inline, `%exp` to `%exp26`, 27 instructions of fused multiply-adds, IEEE arithmetic and bit operations. At `-O0`
it is a call of the runtime's `fminus_rt_expf`, which computes the same. The runtime keeps that name for itself, with
`fminus_rt_exp_f4` and `fminus_rt_libm_expf`, as it keeps three for each of `sinf`, `cosf`, `tanf`, `logf` and `tanhf`:
a program that calls one of these builtins may not declare a function of any of its three names, in any math mode. The
program at the top of this page, with `float fminus_rt_expf(float x) { return x; }` added as its first line, is refused
at the call:

```
note.fm:6:18: error: 'expf' reserves 'fminus_rt_expf' for the runtime's function, and this program declares a function of that name: rename it
note.fm:1:7: note: 'fminus_rt_expf' declared here
```

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
- `tests/lit/fminus/command-line.fm` checks the command line: each of `-o`'s extensions, any other name an executable,
  run; `-c`, `-S` and `-emit-llvm`, and the files they name after the input; `/dev/null`; the options that disagree;
  `--version`; `--print-runtime`, and the errors when `FMINUS_RUNTIME_DIR` or `FMINUS_LINK_DRIVER` names nothing; and,
  on Linux, that an executable links with no warning from the system's linker.
- The ctest test `install` (`tests/install/InstallTest.cmake`) installs the build, moves the prefix, and runs the
  installed `fminus` from there: `--version`, `--print-runtime` (it must name the moved install's runtime, not the
  build tree's), `--run`, and `note.fm -o note`, run; with the web library, an executable that calls it too.
- `tests/unit/AotTest.cpp` checks the IR `addEntryPoint` leaves (an internal `fminus.main`, an external
  `i32 main(i32, ptr)`, the two runtime declarations) and what it refuses: no `main`, a prototype only, an `i1`
  result, a vector parameter, a variadic `main`.

## Executables, in detail

A module can also be compiled ahead of time for this machine, into a native executable to run as any program runs (and
under lldb or Instruments), or into an object or assembly file to read or link. Its code comes from the same pipeline
the JIT's does (`support/HostTarget.h`), and on Linux from the same code model and relocations, so an object is the
JIT's code there; on macOS and Windows the object keeps the JIT's TargetMachine's settings.

An FMinus program with a `float main(...)` builds straight into one (or a `double` or `bool` one, whose parameters are
floats and doubles). `fminus::aot::addEntryPoint` renames the program's `main` to `fminus.main` and adds the C `main` an
executable starts in, which reads the arguments as their types say and prints the result (above):

```bash
fminus note.fm -o note                                      # then ./note 69 (--emit exe says the same)
fminus note.fm --entry                                      # the IR with that C main (-S and -c too)
fminus osc.fm --loop-budget 1000000 -o osc                  # main stops past the budget, and exits with 3
fminus taps.fm --bounds-check -o taps                       # main stops at a failed check, and exits with 4
```

`fminus-build`, a development tool (a build tree's, and installed only with `--component devtools`), builds LLVM IR
written by hand, which brings its own C `main`:

```bash
build-dev/bin/fminus-build kernel.ll -o kernel              # an executable (the IR needs a main)
build-dev/bin/fminus-build kernel.ll --emit asm -o -        # this machine's assembly, after -O3
build-dev/bin/fminus-build kernel.ll --emit ll -O2 -o -     # the optimised IR; --emit obj for an object file
build-dev/bin/fminus-build kernel.ll -o kernel --link-arg=-framework --link-arg=Accelerate
```

From code, it is `fminus::aot::emitFile`, `linkExecutable`, `emitExecutable` and `addEntryPoint` in `src/support/Aot.h`.
Linking goes through a C++ compiler driver (`aot::findToolchain`: `$FMINUS_LINK_DRIVER`, else the compiler FMinus was
built with if it is there, else `c++` or `clang++` on PATH), which brings the system's C and C++ libraries, and on macOS
the SDK, libSystem and the ad-hoc signature arm64 needs; on Windows, clang-cl, which names an executable `note.exe` when
asked for `note`. It links the runtime FMinus's own build made, `libfminus_runtime.a`: an installed `fminus`'s beside it
(`../lib/fminus`), a build tree's from that tree, or `$FMINUS_RUNTIME_DIR`'s, as `fminus --print-runtime` says
([building.md](building.md), "Installing"), so JIT'd and ahead-of-time code call the same runtime. An executable whose
program calls the web library links `libfminus_web.a`, libcurl, Mbed TLS and the system libraries they need too (and one
that does not, none of them), and reads the URLs it may reach from `FMINUS_WEB_ALLOW` when it starts: none by default
([web.md](web.md)); one that calls `http_listen` reads where it may listen from `FMINUS_WEB_LISTEN` (and with it
`FMINUS_WEB_TOKEN`, `FMINUS_WEB_HOSTS`, `FMINUS_WEB_ROOT`, a directory of static files, and `FMINUS_WEB_OPEN`): nowhere
by default. One whose program names a file reads its folders from `FMINUS_FILES_READ` and `FMINUS_FILES_WRITE` (and with
them `FMINUS_FILES_DENY`, `FMINUS_FILES_TYPES` and `FMINUS_FILES_ANYWHERE`) when it starts: none by default
([files.md](files.md)). In the JIT, `fminus::defineRuntimeSymbols` defines every function listed in
`src/runtime/RuntimeSymbols.def` by its address (and, with the web library, `src/web/WebSymbols.def`'s). Under the
`asan` and `tsan` presets, the sanitizer runtimes are linked too. Inside a plugin, FMinus only ever JITs: an executable
is for a program that runs on its own.

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
