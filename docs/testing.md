# Testing and debugging

The layers of tests, how to write each, and how to look at what the compiler did when something is off.

## The layers

| Layer | Where | Tests | Write one when |
|---|---|---|---|
| unit (Catch2) | `tests/unit/*.cpp` | the parser through `ast::dump`; the code generator by running JIT'd functions; error messages | you change the language |
| library (Catch2) | `tests/library/*.cpp` | the public API (`fminus/FMinus.h`), in C++17 against `FMinus::FMinus` alone, as a host builds | you change the public API |
| smoke (Catch2) | `tests/smoke/` | the toolchain itself: LLJIT and O3 on the host, an AOT executable, Bison and Flex (the calculator) | rarely: they guard the integration |
| lit + FileCheck | `tests/lit/**` | the tools end to end: `fminus` IR, assembly, runs and errors; `fminus-build`; LLVM's tools | you change output a tool prints |
| examples | `examples/<name>/` (`--check`); `tests/lit/fminus/examples.test` | a real kernel against C++, bit for bit or within a tolerance; the `.fm` examples' results | you change what a kernel or an example computes, or how fast |

```bash
cmake --build --preset dev --target check-fminus   # build what the tests need, then ctest
ctest --preset dev                                 # everything
ctest --preset dev -R calculator                   # tests whose name matches
build-dev/bin/fminus-tests "[codegen]"             # one Catch2 tag
build-dev/bin/fminus-tests "builtins*" -s          # matching test cases, printing every assertion
build-dev/bin/fminus-lit -v tests/lit/fminus       # part of the lit suite; -a prints every test's output
                                                   #   (on Windows: python build-dev\bin\fminus-lit ...)
build-dev/bin/example-biquad --check               # one example's comparison
```

`ctest --preset asan` and `ctest --preset tsan` run the same tests under the sanitizers, after building those
presets. Run them through the preset, not with `ctest --test-dir build-asan`: the preset sets
`ASAN_OPTIONS=detect_container_overflow=0`. FMinus is instrumented and LLVM isn't, and they share `std::vector`
instantiations (LLVM's `df_iterator`, and the `PassConcept` vector a pass callback's `addPass` grows), so without it
the JIT and AOT smoke tests abort with a false "container-overflow" inside LLVM's passes. Running `fminus-tests` by
hand from `build-asan/`, set it yourself. Under tsan, one test skips itself: `ZeroInitTest.cpp`'s check that its dirty
stack reaches a JIT'd function's locals, since ThreadSanitizer's runtime writes the frames in between (below).

## Unit tests (Catch2)

Each file states what it tests in its first line, then has a few helpers and the `TEST_CASE`s. The helpers in
`tests/unit/` are the place to start:

- `parsed(source)` (`FrontendTest.cpp`) returns the dump of the parsed program, or its errors. Compare whole
  strings, so a parser test reads as input → tree.
- `compile(source, &errors)` (`Compile.h`, which the code generator's tests share: `CodeGenTest.cpp`,
  `ControlFlowTest.cpp`, `IntTest.cpp`, `PointerTest.cpp`, `HeapTest.cpp`, `EffectTest.cpp`, `ArrayTest.cpp`,
  `ModuleTest.cpp`, `FftTest.cpp`, `ParamTest.cpp`, `VectorTest.cpp`, `PolyTest.cpp`, `ParallelTest.cpp`, and the
  compiler's own: `VersionTest.cpp`, `TbaaTest.cpp`, `ZeroInitTest.cpp`, `AttributeTest.cpp`, `DenormalTest.cpp`)
  parses, lowers and JIT-compiles at -O2, and returns the `Jit`, or null with the formatted errors.
  `lookup<float(float)>(*jit, "f")` gives a callable function, and `lookup<void(const float*, float*, int)>(*jit, "g")`
  one that takes a test's buffers (`std::array::data()`). A third parameter, the `ir::Options`, compiles at a chosen
  `poly float` width, `compile(source, &errors, {.polyLanes = 16})`, in another math mode (`{.math =
  fminus::ir::Math::Fast}`), or with the parameters bound (`options.parameters["Amp::gain"] = {Kind::Stream, 0.0}`,
  `ParamTest.cpp`); the tests of knob versioning, TBAA and denormals set `knobVersions`, `strictAliasing` and
  `denormals` the same way, to compare a program with itself under both settings. `MathTest.cpp` compiles through
  `fminus::Program`, as a host does, at each level, width and binding, and its test of the names the runtime reserves
  for the math builtins (extending.md, recipe 15) through `ir::lower` alone too, as `fminus --emit obj` meets a program:
  the JIT would refuse a program's definition of a runtime symbol's name anyway.
- `errorsOf(source)` returns the errors of a program that must fail.

```cpp
TEST_CASE("operators have C's precedence and associativity", "[frontend]")
{
    CHECK(parsed("float f(float a, float b) { return -a * -(a + b) + +b; }")
          == "(function float f ((float a) (float b))\n  (return (+ (* (- a) (- (+ a b))) (+ b))))\n");
}

TEST_CASE("functions compute what they say", "[codegen]")
{
    auto jit = compile("float half(float x) { return x / 2; }");
    REQUIRE(jit);
    CHECK(lookup<float(float)>(*jit, "half")(3.0f) == 1.5f);
}
```

- **Compare floats exactly** where the arithmetic is exact or IEEE-deterministic; FMinus fuses and reorders nothing.
  That includes the math builtins: in the default math mode, compare `sinf`, `expf`, ... with `fminus::math::sin`,
  `exp`, ..., which compute them as a program does, bit for bit; in the libm mode, with `std::` (but not a `std::sin`
  and `std::cos` of one argument, which clang pairs into one `__sincosf_stret` from `-O1`). Use a tolerance only in
  the fast mode, around LLVM's intrinsics.
- **Error tests compare the whole formatted text**, locations included. A moved column is a real change.
- **Tags**: `[frontend]`, `[codegen]`, `[smoke]`, `[jit]`, `[aot]` (the AOT smoke test, and `AotTest.cpp`'s checks
  of an executable's entry), `[grammar]`, `[simd]` (vectors and the vector math library), `[poly]`, `[int]`,
  `[pointer]`, `[heap]`, `[effects]`, `[math]` (the math modes), `[versions]` (knob versioning and flat streams),
  `[tbaa]`, `[zeroinit]`, `[attributes]` and `[progress]` (the runtime's attributes and loop progress),
  `[denormals]`, `[parallel]` (parallel regions, and `[runtime]` among them for the pool on its own). Add new ones
  freely.
- **A test that something reads 0** must not run on a fresh stack, which is 0 already. `ZeroInitTest.cpp` calls each
  function on a dirty one: `dirtyStack()` fills 64 KB below the caller with 1e6, and `padded()` calls the function
  from below a kilobyte of padding, so a wrongly removed zeroing reads 1e6. A test of a hand-written local that
  nothing zeroes checks the harness itself (all 256 of its elements read 1e6); it is skipped under tsan.
- **Vector functions** are called from C++ through clang's `ext_vector_type`
  (`using float4 = float __attribute__((ext_vector_type(4)));`), which matches FMinus's `float2` and `float4` at the C
  boundary ([simd.md](simd.md)).
- `INFO(errors)` before a `REQUIRE(jit)` prints the compiler's errors when the compile fails, and saves a debugging
  round-trip.
- **Parallel regions** run alone, on the calling thread, unless the pool is started: `ParallelTest.cpp`'s `Pool`
  starts it for a test, as `Program::compile` does for a host. A region's result is checked against the same program
  compiled with `threads = 1`, bit for bit, and a test that a region used several threads tries a few times, since a
  busy machine (ctest's `-j`) may not run a pool thread before the caller has done the work. What the pool's threads
  cost is their CPU time, which POSIX's clocks give whatever else the machine runs (the process's less the caller's:
  `CLOCK_PROCESS_CPUTIME_ID`, `CLOCK_THREAD_CPUTIME_ID`), where wall time would not. Run them under `tsan` too: the
  pool is what it checks.

## lit and FileCheck

A lit test is a file with `RUN:` lines. lit runs each line in its own shell (pipes, `not`, `env`), and a test
passes when every line exits 0. FileCheck reads the test file itself for `CHECK:` lines and matches them, in order,
against its input. FMinus files put both in `//` comments.

```c
// The code generator's IR, as written and optimised.
// RUN: fminus %s -O0 | FileCheck %s
// RUN: fminus %s -O2 | FileCheck %s --check-prefix=OPT
// RUN: not fminus %s --run gain 1 2>&1 | FileCheck %s --check-prefix=ARITY

float gain(float x, float g)
{
    return x * g;
}

// CHECK-LABEL: define float @gain(float %x, float %g){{.*}} {
// CHECK:         [[X:%.*]] = load float, ptr %x.addr
// CHECK-NEXT:    [[G:%.*]] = load float, ptr %g.addr
// CHECK-NEXT:    [[P:%.*]] = fmul float [[X]], [[G]]
// CHECK-NEXT:    ret float [[P]]
// OPT-NOT:     alloca
// ARITY: fminus: --run: 'gain' takes 2 arguments, not 1
```

| Directive | Meaning |
|---|---|
| `CHECK: text` | `text` appears, after the previous match |
| `CHECK-NEXT:` | on the very next line |
| `CHECK-SAME:` | on the same line as the previous match |
| `CHECK-LABEL:` | a section boundary: later checks match only up to the next label, so one function's checks can't match in another |
| `CHECK-NOT:` | does not appear between the surrounding matches |
| `CHECK-DAG:` | these lines appear, in any order |
| `{{regex}}` | a regular expression inside a literal line |
| `[[NAME:regex]]`, `[[NAME]]` | capture, then reuse: value names like `%0` change, so capture them |
| `--check-prefix=X` | use `X:` lines instead of `CHECK:`, so one file can check several runs |

| Substitution | |
|---|---|
| `%s` | this test file |
| `%t` | a temporary path for this test (`%t.o`, `%t.ll`, ...) |
| `%S` | this test's directory |
| `%host_triple` | the JIT's target triple |

| Feature | |
|---|---|
| `REQUIRES: arm64` (or `x86_64`) | the host architecture, for assembly checks, and IR checks that hang on its cost model (`slp.fm`) (Windows names them `ARM64` and `AMD64`, so these skip there) |
| `REQUIRES: asserts` | LLVM built with assertions |
| `REQUIRES: asan`, `ubsan`, `tsan` | the sanitizer build |
| `REQUIRES: system-darwin` (or `system-linux`) | the host's system (`veclib.fm`, which runs `--math fast`, needs Apple's vector math library) |
| `%if arm64 %{ ... %}` | a RUN line only on arm64, so the rest of the file still runs elsewhere (`simd.fm`, `poly.fm`); `%if system-darwin && arm64 %{ ... %}` for Apple's assembly (`math.fm`) |

On PATH: `fminus`, `fminus-build`, `fminus-hostinfo`, then `FileCheck`, `not`, `count`, `opt`, `llc`, `llvm-mca`,
`llvm-objdump`, ... from the LLVM flavor. To check an error, send stderr to FileCheck and expect failure:
`not fminus %s 2>&1 | FileCheck %s`. To check a program, build it and run it (`tests/lit/fminus/main.fm`):

```c
// RUN: fminus %s --emit exe -o %t
// RUN: %t 3 4 | FileCheck %s --check-prefix=RESULT
// RUN: not %t 3 2>&1 | FileCheck %s --check-prefix=ARITY
```

A check prefix can't be `RUN` (FileCheck refuses it), and lit's `%t` names a file per test, so several executables
take suffixes: `%t.none`, `%t.note`.

A test that needs files of its own keeps them in an `Inputs/` directory beside it, which `lit.cfg.py` excludes, so lit
never runs them as tests. RUN lines name them through `%S`: `include.fm` runs `fminus %s -I %S/Inputs/include/lib` and
includes `"Inputs/include/pole.fm"`, beside itself.

**Writing IR checks that last:**

- Match the stable parts: names you gave (`%doubled`), opcodes, types, constants.
- Capture the unstable ones (`[[V:%.*]]`) rather than writing `%0`.
- End a `define` line with `{{.*}}`, since attributes come and go.
- Don't match a load or store to the end of its line, nor a metadata number: every load, and every store but those
  that start a parameter's or a declaration's stack slot (Tbaa.cpp), carries `!tbaa`, before `!alias.scope` and
  `!noalias` (`params.fm` accepts `!tbaa {{![0-9]+}}`), and every module has two flags, `!0` and `!1`
  (`fminus.knob-versions` and `fminus.math`), so the rest of its metadata starts at `!2`.
- Use one `CHECK-LABEL` per function.
- Prefer `CHECK-NOT: call` and similar properties over transcribing the whole function.
- Check loop metadata at `-O0`, where the lowering wrote it: passes that duplicate a latch do not always copy it
  (`progress.fm`).

## Example harnesses

An example's `--check` compares the FMinus kernel against a C++ version of the same computation and fails beyond
`--tolerance`. ctest runs it. When a change alters what the kernel computes on purpose, through new math or
fast-math flags, change the C++ reference in the same commit. A tolerance is a statement about accuracy, so keep it
as tight as the change allows.

A reference's `sinf`, `expf` and `tanhf` are `fminus::math`'s, which compute what the default math mode does, so the
outputs agree bit for bit where they used to differ in their last bits. The biquad's is C++'s at every optimisation
level (its tolerance stays); the saturator's four kernels must give the same output as each other in any blocks and
at any width, and C++'s; the parameters example's frozen filter must be the knobs' at the same values, bit for bit;
the convolver and the echo, their fields now indexed in their loops (TBAA), are still C++'s bit for bit. The harnesses
that take `--math` (`example-biquad`, `-saturator`, `-parameters`) time the other modes; the saturator's and the
parameters' `--check` expect the default.

A harness calls FMinus once for a whole run, so the loop over the samples is the kernel's, as `examples/biquad/`
does. The kernel is a module's method that processes a block, on an instance the harness allocates:

```c
module Biquad
{
    param float cutoff;   // ... and q, and the rate: the host's
    float s1;
    float s2;
    void process(const float* restrict input, float* restrict output, int n) [[clang::nonblocking]];
};
```

Its state is the instance's fields, which carry over from one call to the next, so `--block N` can call it a block at a
time, as a plugin would, and `--check` compares calls of 64 and 61 samples with one call (they must be bit-identical). A
harness sets the parameters to values other than their `fminus::default`s, at least in `--check`, so a parameter that
never reaches its slot fails it. (`examples/delay/delay.fm` has the same shape in plain C: a function, its state the
caller's, through a pointer.) A call per sample measures the call more than the kernel: the biquad took 3.88 ns a sample
at one call a sample, and 2.66, level with C++, at one call for all of them, before the vector math mode (extending.md,
recipe 15: about 4.6 there, and 2.9 in the libm mode). When the state's size is the program's to decide, the program
allocates it, in functions the harness calls before and after the run, and marks the kernel `[[clang::nonblocking]]` so
it can't allocate (`examples/delay/`).

Write the C++ reference as the kernel is written, on raw pointers. The examples compile with LLVM's definitions,
which turn on libc++'s hardening, so a `std::vector`'s `operator[]` in the reference's loop checks every index: the
delay's first reference was 35 % slower than FMinus for that alone.

## Looking at what the compiler did

| Question | Command |
|---|---|
| Did it parse the way I think? | `fminus x.fm --emit ast` |
| What did my lowering write? | `fminus x.fm -O0` |
| What did LLVM make of it? | `fminus x.fm` (-O2), `-O3` |
| What does the CPU run? | `fminus x.fm --emit asm -o -` |
| What does it return? | `fminus x.fm --run f 1 2` |
| What did knob versioning do? | `fminus x.fm` against `fminus x.fm --knob-versions 0` |
| Which of LLVM's passes changed what? | `fminus x.fm -O0 -o x.ll` then `opt -passes='default<O2>' -print-changed=quiet -disable-output x.ll` (without FMinus's own passes: below) |
| Which passes ran? | `opt -passes='default<O2>' -debug-pass-manager -disable-output x.ll` |
| The IR after one pass | `opt -passes='default<O2>' -print-after=loop-vectorize -disable-output x.ll` |
| Why didn't the loop vectorise? | `opt -passes='default<O2>' -pass-remarks-missed=loop-vectorize -disable-output x.ll` |
| How fast is the machine code? | `llc -O3 -mcpu=apple-m5 x-o2.ll -o x.s`, then `llvm-mca -mcpu=apple-m5` on the kernel's instructions |
| What is this CPU? | `fminus-hostinfo --features` |
| Is the parser's grammar what I think? | `build-dev/src/generated/frontend/Parser.output` (every state, and the conflicts) |

`opt`, `llc`, `llvm-mca` and `llvm-objdump` are in the flavor's `bin/`
(`.deps/install/llvm-23.1.3-Release-assert-*/bin`), and on lit's PATH. `-O0` IR is the input for `opt`: it's what the
code generator wrote, every memset included, since FMinus's own passes, knob versioning and zero-init elision
(extending.md, recipes 16 and 18), run from `-O1`. The one exception is the fast math mode's vector library, which on
Apple systems replaces a `float4`'s `llvm.sin.v4f32` with `_simd_sin_f4` at `-O0` too (simd.md, recipe 8).
`opt -passes='default<O2>'` runs LLVM's pipeline without FMinus's passes, and without what `fminus` gives the pipeline:
the host's CPU (`opt` builds a generic target machine for the IR's triple), and in the fast math mode the vector
library. So it reproduces `fminus -O2` closely only where those passes find nothing: it makes no copies of a method
whose loop a knob steers or reads a stream (no `"fminus-knob-versions"` attribute; a stream's loop that vectorises does
so behind the loop vectoriser's check that the stream's stride is 1, `ident.check`), and keeps the memsets of local
arrays that `fminus` removes. Add `-mtriple=$(fminus-hostinfo | sed -n 's/^triple: //p') -mcpu=apple-m5` for the same
cost model.

For `llvm-mca`, keep only the kernel's instructions: cut the function body out of the assembly and leave out calls
and branches. It reports the throughput of one iteration (IPC, the block's reciprocal throughput) on the CPU's
model.

**In a debugger.** `lldb build-debug/bin/fminus -- x.fm -O0` steps through the front end and the code generator.
Break in `Lowering::lower` to watch an expression lower. `value->dump()` and `module.dump()` print IR from inside
lldb (`expr value->dump()`); LLVM is built with `LLVM_ENABLE_DUMP`, so they work without assertions too. To step
into LLVM itself, use the `inline` preset, which builds LLVM with debug information in the tree.

**When the verifier complains** ("internal error: the IR of 'f' is invalid: ..."), the message names the rule
broken. The module is discarded, so `fminus` prints no IR. To see it, print the function just before the check in
`Lowering::define` (`llvmFunction->print(llvm::errs())`), or break there in lldb and `expr llvmFunction->dump()`. A
missing terminator, a phi that doesn't match its predecessors, and a use of a value that doesn't dominate it are
the usual three (extending.md, the pitfalls).

**When the JIT can't find a symbol**, the error lists the missing names. A builtin's math function is the runtime's
in the default and libm math modes, and in the fast mode libm's or libsystem_m's, which resolve from the process. A
runtime function needs its `RuntimeSymbols.def` line. An FMinus function needs the exact name, with
no mangling: `jit->lookup("gain")`.
