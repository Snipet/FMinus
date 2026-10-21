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
build-dev/bin/example-biquad --check               # one example's comparison
```

`ctest --preset asan` and `ctest --preset tsan` run the same tests under the sanitizers, after building those
presets. Run them through the preset, not with `ctest --test-dir build-asan`: the preset sets
`ASAN_OPTIONS=detect_container_overflow=0`. FMinus is instrumented and LLVM isn't, and they share `std::vector`
instantiations (LLVM's `df_iterator`, for one), so without it the JIT and AOT smoke tests abort with a false
"container-overflow" inside LLVM's passes.

## Unit tests (Catch2)

Each file states what it tests in its first line, then has a few helpers and the `TEST_CASE`s. The helpers in
`tests/unit/` are the place to start:

- `parsed(source)` (`FrontendTest.cpp`) returns the dump of the parsed program, or its errors. Compare whole
  strings, so a parser test reads as input → tree.
- `compile(source, &errors)` (`Compile.h`, which the code generator's tests share: `CodeGenTest.cpp`,
  `ControlFlowTest.cpp`, `IntTest.cpp`, `PointerTest.cpp`, `HeapTest.cpp`, `EffectTest.cpp`, `ArrayTest.cpp`,
  `ModuleTest.cpp`, `VectorTest.cpp`, `PolyTest.cpp`) parses, lowers and JIT-compiles at -O2, and returns the `Jit`, or
  null with the formatted errors. `lookup<float(float)>(*jit, "f")` gives a callable function, and `lookup<void(const
  float*, float*, int)>(*jit, "g")` one that takes a test's buffers (`std::array::data()`). A third parameter, the
  `ir::Options`, compiles at a chosen `poly float` width: `compile(source, &errors, {.polyLanes = 16})`.
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
  Use a tolerance only around libm (`sinf`, `expf`, ...).
- **Error tests compare the whole formatted text**, locations included. A moved column is a real change.
- **Tags**: `[frontend]`, `[codegen]`, `[smoke]`, `[jit]`, `[aot]` (the AOT smoke test, and `AotTest.cpp`'s checks
  of an executable's entry), `[grammar]`, `[simd]` (vectors and the vector math library), `[poly]`, `[int]`,
  `[pointer]`, `[heap]`, `[effects]`. Add new ones freely.
- **Vector functions** are called from C++ through clang's `ext_vector_type`
  (`using float4 = float __attribute__((ext_vector_type(4)));`), which matches FMinus's `float2` and `float4` at the C
  boundary ([simd.md](simd.md)).
- `INFO(errors)` before a `REQUIRE(jit)` prints the compiler's errors when the compile fails, and saves a debugging
  round-trip.

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
| `REQUIRES: arm64` (or `x86_64`) | the host architecture, for assembly checks |
| `REQUIRES: asserts` | LLVM built with assertions |
| `REQUIRES: asan`, `ubsan`, `tsan` | the sanitizer build |
| `REQUIRES: system-darwin` (or `system-linux`) | the host's system (`veclib.fm` needs Apple's vector math library) |
| `%if arm64 %{ ... %}` | a RUN line only on arm64, so the rest of the file still runs elsewhere (`simd.fm`, `poly.fm`) |

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

**Writing IR checks that last:**

- Match the stable parts: names you gave (`%doubled`), opcodes, types, constants.
- Capture the unstable ones (`[[V:%.*]]`) rather than writing `%0`.
- End a `define` line with `{{.*}}`, since attributes come and go.
- Use one `CHECK-LABEL` per function.
- Prefer `CHECK-NOT: call` and similar properties over transcribing the whole function.

## Example harnesses

An example's `--check` compares the FMinus kernel against a C++ version of the same computation and fails beyond
`--tolerance`. ctest runs it. When a change alters what the kernel computes on purpose, through new math or
fast-math flags, change the C++ reference in the same commit. A tolerance is a statement about accuracy, so keep it
as tight as the change allows.

A harness calls FMinus once for a whole run, so the loop over the samples is the kernel's, as `examples/biquad/`
does. The kernel processes a block:

```c
void process(const float* restrict input, float* restrict output, int n, float* restrict state);
```

Its state is the caller's, read at the start of a call and written back at the end, so `--block N` can call it a
block at a time, as a plugin would, and `--check` compares calls of 64 and 61 samples with one call (they must be
bit-identical). A call per sample measures the call more than the kernel: the biquad took 3.88 ns a sample at one call
a sample, and takes 2.66, level with C++, at one call for all of them. When the state's size is the program's to
decide, the program allocates it, in functions the harness calls before and after the run, and marks the kernel
`[[clang::nonblocking]]` so it can't allocate (`examples/delay/`).

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
| Which passes changed what? | `fminus x.fm -O0 -o x.ll` then `opt -passes='default<O2>' -print-changed=quiet -disable-output x.ll` |
| Which passes ran? | `opt -passes='default<O2>' -debug-pass-manager -disable-output x.ll` |
| The IR after one pass | `opt -passes='default<O2>' -print-after=loop-vectorize -disable-output x.ll` |
| Why didn't the loop vectorise? | `opt -passes='default<O2>' -pass-remarks-missed=loop-vectorize -disable-output x.ll` |
| How fast is the machine code? | `llc -O3 -mcpu=apple-m5 x-o2.ll -o x.s`, then `llvm-mca -mcpu=apple-m5` on the kernel's instructions |
| What is this CPU? | `fminus-hostinfo --features` |
| Is the parser's grammar what I think? | `build-dev/src/generated/frontend/Parser.output` (every state, and the conflicts) |

`opt`, `llc`, `llvm-mca` and `llvm-objdump` are in the flavor's `bin/`
(`.deps/install/llvm-23.1.3-Release-assert-*/bin`), and on lit's PATH. `-O0` IR is the input for `opt`: it's what
the code generator wrote, before FMinus's own pipeline. Running `opt -passes='default<O2>'` on it reproduces
`fminus -O2` closely, though not exactly, since `fminus` gives the pipeline its target machine. Add
`-mtriple=$(fminus-hostinfo | sed -n 's/^triple: //p') -mcpu=apple-m5` for the same cost model.

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

**When the JIT can't find a symbol**, the error lists the missing names. A builtin's libm function resolves from
the process. A runtime function needs its `RuntimeSymbols.def` line. An FMinus function needs the exact name, with
no mangling: `jit->lookup("gain")`.
