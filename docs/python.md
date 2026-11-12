# FMinus from Python

`import fminus` is FMinus as a Python package: it compiles FMinus programs while Python runs, for the machine it runs
on, and calls their functions with Python's numbers and buffers. It is the C++ library's public API
(`include/fminus/FMinus.h`, [README.md](../README.md#using-fminus-as-a-library)) as Python objects, and no more: a
host like any other, written against `FMinus::FMinus` alone.

```python
import array
import fminus

program = fminus.compile("""
    void gain(const float* restrict in, float* restrict out, int n, float g)
    {
        for (int i = 0; i < n; ++i)
            out[i] = g * in[i];
    }
""")
samples = array.array("f", [1, 2, 3])
out = array.array("f", [0, 0, 0])
program.gain(samples, out, len(samples), 0.5)      # one call a block: the loop is FMinus's
print(out)                                         # array('f', [0.5, 1.0, 1.5])

program = fminus.load("examples/parameters/parameters.fm")
```

Contents: [building it](#building-it), [compiling](#compiling), [calling functions](#calling-functions),
[modules](#modules-and-instances), [parameters](#parameters), [the safety options](#the-safety-options),
[threads](#threads), [what else a Program has](#what-else-a-program-has), [how it is built](#how-it-is-built),
[wheels and PyPI](#wheels-and-pypi), [what is not there](#what-is-not-there).

## Building it

The package is part of FMinus's build, off by default: `FMINUS_BUILD_PYTHON=ON` builds it. It needs what FMinus needs
([building.md](building.md)) and Python 3.9 or later with its development headers (`python3-dev` on Debian and
Ubuntu; python.org's and Homebrew's installs have them). Two ways:

**In a build tree**, to work on it. The `python` preset is `dev` with the package on:

```bash
cmake --workflow --preset python
```

It configures, builds and tests in `build-python/` (the package's own tests run under ctest as the test `python`), and
leaves the package in `build-python/python/fminus/`. Put its parent on `PYTHONPATH` to import it:

```bash
PYTHONPATH=build-python/python python3 python/examples/sweep.py
```

Any other tree takes the option too (`cmake --preset release -DFMINUS_BUILD_PYTHON=ON`), except `asan` and `tsan`:
an instrumented extension module needs the sanitizer's runtime loaded before Python's own, which the build does not set
up, so configure refuses the two together. `-DPython_EXECUTABLE=<path>` picks another Python than the first on `PATH`,
a virtual environment's say; the module is built for that one.

**With pip**, to use it:

```bash
CC=clang CXX=clang++ pip install .
```

`pyproject.toml` has pip run the same CMake build (through scikit-build-core, which pip fetches), with the tests, tools
and examples off, in Release, and install the package into the Python that runs pip. LLVM comes as it does for every
build: from the deps cache, `.deps/` in the checkout or `FMINUS_DEPS_DIR`, where a first configure builds it (10–30
minutes; `pip install -v .` shows it building). `CMAKE_ARGS` passes more settings to CMake
(`CMAKE_ARGS="-DFMINUS_LLVM_MODE=system -DLLVM_DIR=..."`).

Either way the package is two files, `fminus/__init__.py` and the extension module beside it
(`_fminus.cpython-<version>-<platform>.so`, or `.pyd` on Windows), which holds FMinus, its runtime and LLVM, linked
statically: about 120 MB with the `python` preset's debug information, 60 MB without (pip's Release build; 24 MB as
a wheel, which is compressed).

## Compiling

`fminus.compile(source, **options)` compiles a string, and `fminus.load(path, **options)` a file (UTF-8). Each returns
a `Program`, or raises `fminus.CompileError`, whose message is FMinus's errors, one a line, as clang words them:

```python
try:
    fminus.compile("float f() { return x; }", file_name="synth.fm")
except fminus.CompileError as error:
    print(error)                 # synth.fm:1:20: error: use of undeclared identifier 'x'
```

A program that compiles with warnings has them in `program.warnings` (a string, empty when there are none).

The options are keyword arguments, `fminus::Program::Options`' fields by Python's names; the header says what each
costs.

| Option | Default | What it is |
|---|---|---|
| `file_name` | `"program.fm"`; `load()`: the path | what the error messages call the source |
| `optimisation` | `2` | 0 to 3: LLVM's pipeline at that level, as `fminus -O<n>` |
| `poly_lanes` | `0` | a `poly float`'s lanes (1, 2, 4, ... 64); 0 is this machine's (`program.poly_lanes` says) |
| `math` | `"vector"` | how `sinf`, `cosf`, ... compute: `"vector"` (FMinus's kernels, the same bits everywhere), `"libm"` or `"fast"` |
| `include` | `None` | where `#include "name"` finds a file the disk does not have (below) |
| `include_dirs` | `()` | directories `#include` looks in, after the including file's own |
| `defines` | `None` | macros for the program's `#if` and `#ifdef`: `{"DEBUG": True, "LEVEL": 2}` (`True` is a flag, `"1"`, as `-D DEBUG`) |
| `parameters` | `None` | how each module parameter reads, by its qualified name: `{"Lowpass::cutoff": "stream"}` ([below](#parameters)) |
| `strict_aliasing` | `True` | whether the optimiser relies on C's strict-aliasing rule |
| `knob_versions` | `8` | the most copies of a method knob versioning makes; 0 turns it off |
| `denormals` | `"preserve"` | `"preserve"` (IEEE's subnormals) or `"flush"` (flushed to zero) |
| `threads` | `0` | the most threads a parallel region runs on; 0 is every core, 1 the calling thread alone |
| `keep_ir` | `"none"` | which LLVM IR to keep as text: `"lowered"`, `"optimised"` or `"both"` (`program.lowered_ir`, `program.optimised_ir`) |
| `loop_budget` | `False` | count loops against a budget ([below](#the-safety-options)) |
| `bounds_check` | `False` | check each access against its array ([below](#the-safety-options)) |

A string option that is none of its values is a `ValueError`.

**`#include`.** `load()` reads a program's includes from disk as the `fminus` driver does: beside the including file,
then in each of `include_dirs` in turn. `compile()` does the same when it is given `include_dirs` (a relative name is
then relative to `file_name`'s directory, which for the default is the current one), and with neither option no
`#include` finds a file. `include` is for files that are not on disk, and is asked first: a function of the name and
the including file's name that returns the file's text (the file is then named as the `#include` names it), or
`(file_name, text)` to name it otherwise, or `None` for no such file. An exception it raises is `compile()`'s.

```python
library = {"gain.fm": "float gain(float x) { return 3 * x; }"}
program = fminus.compile('#include "gain.fm"\nfloat run(float x) { return gain(x); }',
                         include=lambda name, including: library.get(name))
```

A circuit netlist (`#include "pedal.cir"`, which the driver generates a module from) is not read: see
[what is not there](#what-is-not-there).

## Calling functions

A program's functions are its attributes: `program.gain` is a `fminus.Function`, and calling it calls the machine
code. `program.function("gain")` is the same, for a name in a string or one that an attribute of `Program`'s own hides
(`function`, `module`, `warnings`, ...), and a method is named as C++ qualifies it, `program.function("Osc::tick")`,
its first argument the instance. A name the program does not define is an `AttributeError` (from `function()`, a
`fminus.Error`), and so is a function `Program::call()` does not pass: one that takes or returns a vector, a `poly
float`, or returns a pointer.

```python
>>> program.gain
<fminus.Function void gain(const float *in, float *out, int n, float g)>
>>> program.gain.signature, program.gain.result_type
('void gain(const float *in, float *out, int n, float g)', 'void')
>>> [(p.name, p.type) for p in program.gain.parameters]
[('in', 'const float *'), ('out', 'float *'), ('n', 'int'), ('g', 'float')]
```

Every call goes through `Program::call()` (README.md, "Calls by name"), which checks each argument against the
declaration, so a wrong call is a `TypeError` with FMinus's message, never undefined behaviour. Arguments are
positional. What crosses:

| FMinus | Python gives | Python gets |
|---|---|---|
| `float`, `double` | a `float`, an `int`, or anything with `__float__` (a NumPy scalar); a `float`'s is rounded to one | a `float` |
| `int` | an `int`, or anything with `__index__` (a NumPy integer); never a `float`. One that does not fit 32 bits is an `OverflowError` | an `int` |
| `bool` | a `bool`, or an `int` (0 or not) | a `bool` |
| `void` | | `None` |
| `float*`, `int*`, `double*` | a buffer of C floats, ints or doubles: an `array.array` (`"f"`, `"i"`, `"d"`), a NumPy array (`float32`, `int32`, `float64`), a `memoryview`, a ctypes array. `None` is a null pointer | |
| `const float*`, ... | the same, and a read-only buffer too | |
| `void*` | any buffer, an instance, or `None` | |
| `Osc*` (a module's pointer) | an `Instance` of that module, of the same `Program` | |

A buffer is lent for the call: FMinus reads and writes Python's memory in place, and nothing is copied. It must be
one block (C-contiguous: a NumPy slice with a step is refused), of exactly the pointer's element type (`float64` is no
`float*`: convert with `astype(numpy.float32)`), and writable unless the pointer is to `const`.

**A pointer has no extent.** C passes none, and FMinus checks no access through one, with bounds checks or without: a
function that is passed a buffer is passed its length too, and a length longer than the buffer reads or writes past
it, as it would from C++. That is the one way a call from Python can corrupt the process, so pass `len(buffer)`.

A `Function` keeps its `Program` alive, so `twice = fminus.compile(source).twice` is safe to keep and call.

## Modules and instances

`program.module("Lowpass")` is a `fminus.Module`, the module's description: `name`, `size` and `alignment` (an
instance's, in bytes), `methods` (their names, `"Lowpass::tick"`), and `fields` and `parameters`, lists of records.
`program.modules` is every module, in declaration order.

`module.instance()` (or `module()`) makes a `fminus.Instance`: memory the object owns, aligned and zeroed as the
module says, with each parameter at its `fminus::default`. Its methods are its attributes, bound to it:

```python
program = fminus.compile("""
    module OnePole
    {
        float state;
        float tick(float x) { state += 0.5f * (x - state); return state; }
    };
""")
pole = program.module("OnePole").instance()
pole.tick(1)          # 0.5
pole.tick(1)          # 0.75: its state stays in the instance
pole.reset()          # as it was made: every field 0, every parameter at its default
```

A function that takes a module's pointer takes the instance (`program.state_of(pole)`), and so does a method reached
through the program, `program.function("OnePole::tick")(pole, 1)`. An instance of another `Program`'s module, even one
compiled from the same source, is refused: the layout is the program's own.

An instance is a buffer too: `memoryview(instance)` is its bytes, writable, laid out as `module.fields` says (each
field's `name`, `type` as FMinus writes it, `offset` and `size` in bytes, an array's `element` type and `extents`, and
its `attributes`). That is how Python reads a field:

```python
state = next(f for f in pole.module.fields if f.name == "state")
memoryview(pole)[state.offset:state.offset + state.size].cast("f")[0]        # the float
numpy.frombuffer(pole, dtype=numpy.float32, count=1, offset=state.offset)    # or as an array, in place
```

## Parameters

A module's `param float cutoff;` is a value the host sets. An instance's parameters are its items, by name:

```python
lowpass["cutoff"] = 440        # lowpass.set("cutoff", 440): safe from any thread, read when a method next starts
lowpass["cutoff"]              # lowpass.get("cutoff")
lowpass.set_defaults()         # each parameter with an fminus::default, back to it
```

How a parameter reads is chosen when the program compiles, with `parameters=`: `"knob"` (the default: the value, read
once when a method starts), `"stream"` (a buffer, a value a sample), or a number, the value it is frozen at, compiled
in as a constant. A stream's samples are a buffer of floats that the instance holds until another is set:

```python
program = fminus.load("examples/parameters/parameters.fm", parameters={"Filter::cutoff": "stream"})
filter_ = program.module("Filter").instance()
filter_.set_stream("cutoff", cutoff)       # as many floats as the next calls read
filter_.process(samples, out, n)
filter_.set_stream("cutoff", None)         # flat: every sample is the parameter's value
```

`python/examples/sweep.py` is this program run whole: an LFO writes the cutoff a sample, and the filter reads it.
`set_stream` on a parameter that is not a stream in this program is a `fminus.Error`, and a name that is no parameter
a `KeyError`. Each record of `module.parameters` has the parameter's `name`, `module`, `type`, `binding` (`"knob"`,
`"stream"` or `"frozen"`, and `frozen`, its value), `minimum`, `maximum` and `default` (`None` where the source gives
none), and its `attributes`, each a record of a `name` (`"ui::unit"`) and its `arguments`, floats and strings.

## The safety options

Both are the library's (README.md, "Loop budget" and "Bounds checks"); from Python, a call they stop raises.

**`loop_budget=True`** counts the program's loops against a budget, and a call that runs out returns early and raises
`fminus.LoopBudgetError`. Set the budget before each call it limits: what is left carries over.

```python
program = fminus.compile("float settle(float x) { while (x > 1) x = x * 0.5f; return x; }", loop_budget=True)
program.set_loop_budget(1000)
program.settle(float("inf"))          # fminus.LoopBudgetError: 'settle' ran past its loop budget and returned early
program.loop_budget_exceeded, program.loop_budget_left     # (True, -1)
```

**`bounds_check=True`** checks each access whose array the compiler can see, and a call whose check fails returns
before the access and raises `fminus.BoundsError`:

```python
program = fminus.compile("float tap(int i) { float taps[16]; taps[i] = 1; return taps[i]; }",
                         bounds_check=True, file_name="taps.fm")
program.tap(20)      # fminus.BoundsError: 'tap' stopped: taps.fm:1:40: index 20 out of bounds for 'taps' of type
                     # 'float[16]'
program.bounds_violation        # a record: file, line, column, array, type, extent, index, width, message
program.clear_bounds_violation()
```

The record holds the first check that failed until it is cleared, and while it does every call of the program stops
and raises again: clear it, then `reset()` the instances the stopped call ran on, as the library's hosts do. Neither
option checks a host's buffer, which has no extent ([above](#calling-functions)).

## Threads

The GIL is released while a function runs, so other Python threads run meanwhile, a program's `parallel for` and
`parallel { }` have the machine's cores, and several Python threads may call one program at once. As from C++, each
needs an instance of its own, and the buffers of a running call are not to be resized or written from elsewhere (a
buffer lent for a call cannot be resized: Python refuses while it is exported). The loop budget and the bounds record
are the program's, one of each, so calls that use them are serialised. Compiling holds the GIL.

## What else a Program has

- `program.address("gain")`: the function's address, an `int`, for ctypes, cffi or Numba: a C function of its
  declaration's type, valid while the `Program` lives. Nothing checks a call through it, and from Python it is no
  faster: a call of a `Function` takes about 0.3 µs before the function's own time (measured on the x86-64 machine
  below; a ctypes call of the address takes the same), so process a block a call, as a plugin does.
- `function.nonblocking`, `function.nonallocating` (and `program.nonblocking(name)`): whether it is marked
  `[[clang::nonblocking]]` or `[[clang::nonallocating]]`, which FMinus has then checked.
- `program.code_ranges()`: where each function's machine code is, a record a range (`address`, `size`, `kind`, `symbol`,
  `function`, `module`, `inlined`), for a profiler.
- `fminus.__version__`, `fminus.llvm_version`.

## How it is built

| File | What it is |
|---|---|
| `python/src/Module.cpp` | the extension module `fminus._fminus`, written with pybind11 in C++17 against `fminus/FMinus.h`: `Program`, `Function`, `Module` and `Instance`, the conversion of arguments, the errors |
| `python/fminus/__init__.py` | the package: `compile()` and `load()` with their keyword arguments, and the files `#include` reads from disk |
| `python/tests/test_*.py` | its tests, unittest's (nothing to install); the NumPy ones skip where NumPy is missing |
| `python/examples/sweep.py` | an example, which the tests run |
| `python/CMakeLists.txt` | builds the module and copies the package into `<build>/python/fminus/`; its install rules are what pip installs |
| `cmake/FMinusPython.cmake` | finds Python and fetches pybind11 (pinned in `cmake/FMinusPins.cmake`) |
| `pyproject.toml` | `pip install .`: the CMake build, run by scikit-build-core; and cibuildwheel's settings, for the wheels |
| `.github/workflows/wheels.yml` | builds the wheels and the source distribution, and uploads them to PyPI ([below](#wheels-and-pypi)) |

Adding a source, a Python file or a test edits no CMake, as elsewhere. With the option on, every FMinus library is
compiled as position-independent code, since the module is a shared library that holds them. On Linux the module
exports its initialiser and none of LLVM's symbols, which stay its own, so they meet no other LLVM in the process
(Numba's, say).

A `Program` is held by a shared pointer, and each `Function`, `Module` and `Instance` made from it holds it too, so it
lives while any of them does. A `Function` holds the program's `Callable`, an `Instance` its memory (`operator new`,
aligned) and the buffers its streams point into. The module's records (a field, a parameter, a range) are
`types.SimpleNamespace`s, built when asked for.

To bind something more of the header: add the method to its class in `Module.cpp` (a `.def(...)`), a keyword to
`compile()` in `__init__.py` if it is an option (and its place in `_compile`'s arguments), a test, and a line here.

## Wheels and PyPI

A wheel is the package built: `pip install fminus` unpacks one in seconds, where the source distribution compiles
FMinus, and LLVM first, on the machine that installs it. The Wheels workflow (`.github/workflows/wheels.yml`) builds
both, and uploads them.

**What it builds.** cibuildwheel, configured in `pyproject.toml` (`[tool.cibuildwheel]`), builds a wheel for each
CPython from `requires-python` up (not the free-threaded ones, as the module asks for the GIL) on each platform, and
runs `python/tests/` against each as it is installed, with NumPy where it has a wheel:

| Platform | Runner | Wheel | LLVM |
|---|---|---|---|
| `linux-x64` | `ubuntu-latest` | `manylinux_2_28_x86_64` (glibc 2.28 or later: RHEL 8, Ubuntu 20.04, Debian 10) | built in the container, then cached |
| `linux-arm64` | `ubuntu-24.04-arm` | `manylinux_2_28_aarch64` | the same; only when named, as FMinus had not been built there |
| `macos-arm64` | `macos-latest` | `macosx_14_0_arm64` | the Deps workflow's archive |
| `windows-x64` | `windows-latest` | `win_amd64` | the Deps workflow's archive |

Each wheel is a whole build of FMinus, pip's own (`pyproject.toml`), so a platform's job compiles FMinus once for each
CPython, seven times today (about two minutes each on four cores, and 24 MB a wheel on Linux). LLVM comes from the deps
cache as for any build, which the job keeps between runs (`actions/cache`, under a key of `cmake/FMinusPins.cmake` and
the two modules that build the dependencies). On Linux the wheels are built in manylinux's container, so that they run
on any distribution of its C library or later, and LLVM, Bison and Flex are built there too, the first time (about half
an hour on four cores). The image's compiler is GCC, and FMinus's runtime needs clang (its vector types and builtins),
so the build installs the image's own clang (`manylinux-install-clang`), which uses GCC's C++ library. The Deps
workflow's Linux archive does not do: it is built on Arch Linux, against a newer C library than a wheel may ask for.
musllinux (Alpine) has no wheel: it would be another LLVM, built against musl.

**Running it.** Actions > Wheels > Run workflow, on any branch. `platforms` names the ones to build (empty: all but
`linux-arm64`), `pythons` narrows the CPythons (`cp312-*` builds one a platform, to try a change), and `publish` says
where the files go: `nowhere` (the default: they are the run's artifacts, for 7 days), `testpypi` or `pypi`, both
from `main` only. To build this machine's wheels by hand, `pipx run cibuildwheel` in the checkout (on Linux it needs
Docker, and `FMINUS_DEPS_DIR=/host/<an absolute path>` keeps LLVM for the next run).

**Setting up PyPI**, once. The upload uses Trusted Publishing: PyPI takes GitHub's word for which workflow is
uploading, and no token is stored anywhere. On pypi.org, under Your account > Publishing, add a pending publisher (the
project does not exist until its first upload): project `fminus`, owner `Snipet`, repository `FMinus`, workflow
`wheels.yml`, environment `pypi`. The same on test.pypi.org, a separate site with its own account, with the
environment `testpypi`. GitHub makes the two environments the first time the workflow names them; a required reviewer
on `pypi` (the repository's Settings > Environments) makes each upload wait for an approval.

**A release.** Raise the version, `project(FMinus VERSION ...)` in `CMakeLists.txt`, which is the package's: PyPI keeps
a version's files for good, and takes no second file of a name, even after a deletion. Run the workflow to
`testpypi` first and install from there
(`pip install --index-url https://test.pypi.org/simple/ --extra-index-url https://pypi.org/simple/ fminus`), then run
it to `pypi`.

## What is not there

- **The circuit compiler** (`fminus/Circuit.h`): `fminus.circuit.generate` and the self-test are not bound, and
  `load()` does not turn an `#include "x.cir"` into a module as the driver does. Generate the module with
  `fminus x.cir --emit fm` and load that.
- **Vectors at the boundary**: a function that takes or returns `float2`, `float4`, `float8` or a `poly float` has no
  `Function` (the library's `call()` passes none); pass buffers and load vectors inside FMinus.
- **A list of a program's functions.** The library lists a module's methods (`module.methods`) but not the functions
  outside modules, so `dir(program)` does not show them.
- **`fminus::math`** (the kernels as C++ functions), and executables (`fminus-build`'s side, `support/Aot.h`).
- **Some wheels.** None for musllinux (Alpine), Intel Macs, 32-bit systems or free-threaded CPython: pip builds the
  source distribution there, LLVM and all.
- **Its dependencies' licence texts in a wheel.** A wheel holds LLVM and PFFFT, linked in, and was compiled with
  pybind11's headers, and carries FMinus's `LICENSE` alone. Their licences ask a binary for their notices (README.md,
  "License"): add the three texts to the package (`license-files` in `pyproject.toml`) before the first upload.
- **Runs elsewhere.** Built and tested on Linux (x86-64, CPython 3.13, clang 18). The Wheels workflow's Linux x86-64
  build has been run in its container for one CPython, and that wheel tried on Debian 13 too. Its macOS, Windows and
  Linux arm64 jobs build the same sources and have not run, nor has the workflow itself on GitHub.
