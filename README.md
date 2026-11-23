# FMinus

FMinus is a small language for real-time audio, compiled while your program runs. It is a subset of C, with C's
syntax, C's semantics and clang's error messages, and LLVM compiles it on the machine it runs on, for that machine's
CPU: inside a plugin or any C++ application, from Python, or from the command line. It adds what DSP code needs and C
does not have, and, beyond audio, statements for programs that talk over the network and handle text and JSON.

**Documentation: [snipet.github.io/FMinus](https://snipet.github.io/FMinus/)**, or [docs/](docs/README.md) in this
repository.

## Why FMinus

- **It is C you already know.** Functions, pointers, arrays, `switch`, `defer`, and modules: C++'s structs with
  methods. Where FMinus differs from C, it is on purpose, and [the manual](docs/language.md#where-fminus-differs-from-c)
  says why.
- **It is compiled where it runs.** LLVM optimises each program for the machine in front of it and ORC's JIT compiles
  it in memory. The same program builds into a native executable ahead of time too.
- **It is safe on the audio thread.** A function marked `[[clang::nonblocking]]` is checked by the compiler: nothing it
  calls may allocate or block. A host may also give a call a loop budget, which stops a loop that would run for
  ever, and while you develop, bounds checks stop an index outside its array and say where.
- **DSP is built in.** `float2`, `float4` and `float8` vectors, `poly float` (as wide as the machine's vectors),
  parameters a host turns into knobs and streams, an FFT, and `parallel for`.
- **It reaches beyond audio.** Command-line tools (`int main(string name, int n)`, C's `printf`), tables (CSV rows
  into an array of a module, NumPy's `.npy`), an HTTP client and server (`serve`, `route`, `reply`), a `string` type,
  JSON and XML, files, `within` (a deadline for a block's requests) and `when` (a rising edge). Every URL, route and
  file name is a literal the host can list before anything runs, and a program reaches nothing its host does not
  allow.
- **It fits in your project.** One C++ header, a Python package, a JUCE plugin example, and a compiler from SPICE
  netlists to FMinus modules for analog circuits.

## A taste

A filter, as a host runs it a block at a time:

```c
module Lowpass
{
    param float cutoff [[fminus::range(20, 20000)]] [[fminus::default(1000)]];   // the host's knob
    float rate;
    float state;

    void init(float sampleRate) { rate = sampleRate; }

    void process(const float* in, float* out, int n) [[clang::nonblocking]]    // checked: no heap, no locks
    {
        for (int i = 0; i < n; ++i)
        {
            const float a = 1.0f - expf(-6.2831853f * cutoff[i] / rate);
            state += a * (in[i] - state);
            out[i] = state;
        }
    }
};
```

A gain that a browser or a script sets over HTTP, answered from the audio thread itself:

```c
module Remote
{
    http_server server;
    float gain;

    void init() { server = http_listen(); gain = 1.0f; }   // listens where the host allows: nowhere by default

    void process(float* out, int n) [[clang::nonblocking]]
    {
        serve (server)                                       // wait-free: the requests waiting, between two blocks
        {
            route "PUT /gain" (float value [[fminus::range(0, 1)]])   // ?value=0.5; missing or out of range: 400
            {
                gain = value;
                reply 204;
            }
            route "GET /state"
            {
                reply 200, json { gain };                    // {"gain":0.5}
            }
        }
        for (int i = 0; i < n; ++i)
            out[i] *= gain;
    }
};
```

And a whole program, built into an executable:

```c
// note.fm: the frequency of a MIDI note, in hertz.
float main(float note)
{
    return 440.0f * expf((note - 69.0f) / 12.0f * 0.693147181f);
}
```

```console
$ fminus note.fm -o note
$ ./note 69
440
```

## Getting started

### Build and install

You need CMake 3.30 or later, Ninja, Python 3 and clang: Xcode's command line tools on macOS (Apple silicon), clang
on Linux, and Visual Studio 2022 with its "C++ Clang tools for Windows" on Windows
([building.md](docs/building.md#requirements) has the details). Everything else (LLVM, Bison, Flex and the libraries
FMinus uses) is downloaded at pinned versions and built by the build itself.

```bash
git clone https://github.com/Snipet/FMinus.git
cd FMinus
cmake --workflow --preset release                  # configure, build and test
cmake --install build-release --prefix ~/.local    # or `sudo cmake --install build-release`, into /usr/local
export PATH="$HOME/.local/bin:$PATH"               # if ~/.local/bin is not on your PATH yet
fminus --version
```

The first configure builds LLVM once (10 to 30 minutes, with its output streaming) into a cache, `.deps/`, which every
later build tree finds in seconds. The install is `bin/fminus`, with LLVM inside it, and the runtime it links into the
executables it makes, beside it in `lib/fminus/`: the prefix can be moved anywhere.
[building.md](docs/building.md#installing) covers Windows, prebuilt archives, what an install holds, and uninstalling.

### Use it

`fminus` takes the options a C compiler does:

```bash
fminus note.fm --run main 69           # compile in memory, call main(69) and print the result: 440
fminus note.fm -o note                 # an executable that runs main
fminus -c note.fm                      # an object file, note.o (-S: assembly, note.s)
fminus note.fm                         # the optimised LLVM IR on stdout (-O0: the IR as written)
fminus --version                       # the version, LLVM's, the target and the build's features
```

Running a program in memory needs nothing but `fminus`; making an executable needs a C++ toolchain for its linker
(clang or GCC on Linux, Xcode's command line tools on macOS, Visual Studio on Windows). [programs.md](docs/programs.md)
starts from a first program, and [language.md](docs/language.md) is the language itself.

### Embed it

From C++, a host compiles a program while it runs and calls its functions as C function pointers:

```cpp
#include "fminus/FMinus.h"

std::string errors;
auto program = fminus::Program::compile("float twice(float x) { return 2 * x; }", errors);
auto* twice = program->function<float(float)>("twice", errors);
float y = twice(21.0f);                // 42, a plain call into the compiled code
```

A CMake project adds FMinus to its own build (`add_subdirectory` or FetchContent) and links `FMinus::FMinus`, as
`examples/plugin/` does; [library.md](docs/library.md) covers the API, and `examples/hello/` is a whole host. From
Python, `import fminus` does the same ([python.md](docs/python.md)):

```python
import fminus

program = fminus.compile("float twice(float x) { return 2 * x; }")
print(program.twice(21))               # 42.0
```

## Documentation

| | |
|---|---|
| [programs.md](docs/programs.md) | Writing a whole program, running it, building an executable |
| [language.md](docs/language.md) | The language: every construct, and where it differs from C |
| [library.md](docs/library.md) | FMinus inside a C++ host, and inside a plugin |
| [python.md](docs/python.md) | The Python package, `import fminus` |
| [simd.md](docs/simd.md) | Vectors, the vector math library, and `poly float` |
| [web.md](docs/web.md) | The HTTP client and server, `serve`, `route`, `reply` and `within` |
| [files.md](docs/files.md) | Reading and writing files, and what a host grants |
| [circuits.md](docs/circuits.md) | SPICE netlists compiled to FMinus modules |
| [architecture.md](docs/architecture.md) | The compiler from source text to machine code |
| [extending.md](docs/extending.md) | Growing the language: worked, verified recipes |
| [building.md](docs/building.md) | Presets, options, dependencies, Windows |
| [testing.md](docs/testing.md) | The tests, and looking at what the compiler did |

The [examples](examples/) show each feature in use, from a biquad filter and a wavetable oscillator to a synthesiser a
browser controls and a JUCE plugin.

## Developing FMinus

```bash
cmake --workflow --preset dev          # the everyday build: FMinus with debug info, LLVM with assertions
ctest --preset dev -R jit              # some of the tests
build-dev/bin/fminus-tests "[codegen]" # one Catch2 tag
```

Other presets build the web library (`web`), the whole file interface (`files`), the Python package (`python`), or run
under sanitizers (`asan`, `tsan`): [building.md](docs/building.md#presets) lists them all. Adding a source file, a
test or an example needs no CMake change. To add to the language, start with [architecture.md](docs/architecture.md),
then [extending.md](docs/extending.md), whose recipes each walk through a change from the grammar to the tests.
`editors/vscode/` is a VS Code extension for FMinus's syntax.

```
include/fminus/   the public C++ API: FMinus.h, Circuit.h
src/              the compiler: frontend/ (Flex, Bison), ir/ (to LLVM IR), opt/, jit/, api/; and its libraries:
                  runtime/ (what compiled code calls), web/, circuit/
tools/            fminus (the driver), fminus-build, fminus-hostinfo
packaging/        what an install holds, and the archives CPack makes of it
examples/         one example per directory: a program, and a harness that checks it
python/           the Python package
tests/            unit/ and library/ (Catch2), lit/ (lit and FileCheck), install/, smoke/
docs/             the manual
```

## License

FMinus is free software under the GNU General Public License, version 3 ([LICENSE](LICENSE)). Its dependencies keep
their own licenses, and a binary that includes some of them, as `fminus` and every executable it makes do, must carry
their notices: [licenses.md](docs/licenses.md) says which, and an install carries them all in `share/doc/fminus/`.
