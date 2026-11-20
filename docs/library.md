# Using FMinus as a library

`include/fminus/FMinus.h` is FMinus's C++ API, and the one header a host includes. A `fminus::Program` compiles FMinus
source while the host runs, for the CPU it runs on, and gives its functions as C function pointers
(`examples/hello/main.cpp` is a whole program that does this and no more):

```cpp
#include "fminus/FMinus.h"

std::string errors;
auto program = fminus::Program::compile(source, errors);     // nullptr when it does not compile
if (!program)
    return report(errors);                                    // "program.fm:3:12: error: ...", as clang words it
auto* gain = program->function<void(const float*, float*, int, float)>("gain", errors);
gain(in, out, n, 0.5f);                                       // one call a block: the loop is FMinus's
```

- **Types.** A function's C++ type is its FMinus declaration's C type: `float`, `int`, `bool`, `void`, pointers, and
  `float2` and `float4` as clang's `ext_vector_type`. The C ABI passes a `float8`, and a `poly float` of 8 lanes or
  more, through memory, and FMinus in registers, so keep those inside FMinus. Nothing is checked at the boundary: a
  signature that does not match the declaration is undefined behaviour (a host that learns a signature only as it
  runs calls by name instead, below). The header has the table.
- **Options.** `Program::Options` sets the optimisation level (`-O2` by default, as for `fminus`), a `poly float`'s
  lanes (this machine's by default, which `polyLanes()` reports), and the file name the errors give. `include` is
  where `#include "name"` finds its file: a function from the name and the including file's name to the file's name
  and text, so a host keeps its files where it likes, on disk or in memory (with none, the default, no `#include`
  finds a file). `math` is how `sinf` and the other transcendental builtins compute (`Math::Vector` by default,
  `Libm` or `Fast`), `strictAliasing` whether the optimiser relies on C's strict-aliasing rule (`true`),
  `knobVersions` how many copies of a method knob versioning may make (8; 0 turns it off), `denormals` whether
  subnormals are kept or flushed to zero (`Denormals::Preserve` or `Flush`), `threads` the most threads a parallel
  region runs on (0, the default, is every core; 1 runs each on the calling thread alone), `keepIr` which of the
  program's LLVM IR to keep as text (below), `loopBudget` whether its loops count against a budget (`false`;
  below), `boundsCheck` whether its accesses are checked against their arrays (`false`; below), and `defines` the
  macros for its `#if`s (below). The header says what each costs.
- **Threads.** `compile()` and the first look-up of each function run LLVM's optimiser and code generator on the
  calling thread: they take milliseconds and allocate. Different programs may compile on different threads at once.
  In a plugin, compile off the audio thread and hand the pointer over (below). A program's `parallel for` and
  `parallel { }` run on FMinus's pool, shared by every `Program` in the process: the first with a region starts it
  when it compiles (a thread for each core but one) and the last one's destructor stops it. The thread that calls the
  program works on each region too.
- **Warnings.** `warnings()` is a compiled program's warnings, as clang prints them, one a line with their notes
  (empty when it has none): clang's `-Wswitch`, a `switch` on a value of an enumerated type that misses some of its
  constants, and `-Wunknown-attributes`, an attribute FMinus ignores (below). A program that does not compile has its
  warnings among its `errors`, where they are.
- **Effects.** `nonBlocking(name)` and `nonAllocating(name)` say whether a function is marked
  `[[clang::nonblocking]]` or `[[clang::nonallocating]]`, which FMinus has then checked, so a host can refuse to run
  anything else on its audio thread.
- **Modules.** `module(name)` describes one: its size and alignment, what a host allocates for an instance (zeroed:
  every field starts at 0, and nothing else runs), and each field's name, type and offset, which a C++ struct with the
  same fields in the same order shares. A field's type is FMinus's spelling of it: `"double"` and `"double[8]"` are 8
  bytes an element, C++'s `double`, so a host that reads fields (a trace, an inspector) reads one as a double, never as
  a float. An array's shape is spelled out too: its `element` type and its `extents`, outermost first, `{4, 16}` for a
  `"float[4][16]"`, whose rows lie side by side (C++'s `float[4][16]` at the field's offset), and its `attributes`
  (below). A method is a function whose first parameter is the instance:
  `function<float(void*, float)>("OnePole::tick", errors)`, and `function<double(double)>("seconds", errors)` for one of
  doubles. `examples/voices/` is such a host.
- **Parameters.** `Options::parameters` binds each parameter by name, `"Lowpass::cutoff"`: `Binding::knob()` (the
  default), `stream()` or `frozen(value)`. `parameter(name)` and `Module::parameters` describe each: its type, binding,
  range and default, and where its slot is in an instance. `setValue` sets a knob from any thread (a relaxed atomic
  store, which a method reads when it next starts), `setStream` points a stream at the call's samples (or, null, makes
  the call flat: every sample is then the value `setValue` set), and `setDefaults` sets each parameter's default.
  `examples/parameters/` is such a host.
- **Attributes.** A field says how a host shows it with attributes after its name, C23's way:
  `float level [[ui::unit("dB"), ui::label("Level")]] [[fminus::range(-60, 0)]];`. `Module::Field::attributes` lists
  them in order, a parameter's too (and its `Parameter::attributes`), each a `Program::Attribute`: its `name` as written
  (`"ui::unit"`) and its `arguments`, each a number (`number`, and `text` as written, `"-60"`) or a string (`isString`,
  `text` its bytes, C's escapes resolved, UTF-8 as the source has it). `findAttribute(attributes, "ui::unit")` is the
  last of a name, or null. FMinus gives meaning to its own, `fminus::range` (a parameter's or a field's of numbers),
  `fminus::default` (a parameter's, or a field's of numbers or `bool`s, for `json_read`), `json::name` and `json::skip`,
  and passes them and every other on, the others unread: a unit, a label, a display hint, whatever the
  host defines. Name a host's in a namespace of its own (`ui::`), as C23 keeps names without one for its standard
  attributes: one without a namespace that is none of those (`[[range(0, 1)]]`) is a warning in `warnings()`, and in
  no `attributes` list. `examples/meter/` labels its readings so ([extending.md](extending.md), recipe 35).
- **Math.** `fminus::math::sin`, `cos`, `exp`, `tanh` and `log` compute what a program in the default math mode does,
  bit for bit, so a C++ reference that calls them agrees with the program at any block size, level and width. They
  are `[[gnu::const]]` for GCC and clang (`FMINUS_MATH_CONST`), so two calls of one argument are one.
- **IR.** For a host that shows what FMinus made of a program, `Options::keepIr` keeps its LLVM IR as text:
  `KeepIr::Lowered`, `Optimised` or `Both` (`None`, the default, prints and holds nothing). `loweredIr()` is the IR as
  the code generator wrote it, set up for this machine, before any pass; `optimisedIr()` is what the JIT generates the
  machine code from, after the pipeline at `optimisation`, and the same text as `fminus --emit ll` prints for the same
  source and options, the file named as `fileName`. Each is empty when not kept. To print the optimised IR,
  `compile()` runs the optimiser itself, and the first look-up then only the code generator.
- **Loop budget.** A program compiled with `Options::loopBudget` counts its loops' iterations and its recursive calls
  against a budget the host sets before each call, `setLoopBudget(n)`, and a call that would loop for ever, or far
  longer than it should, returns early, 0, with every function of the program that called it: `loopBudgetExceeded()`
  says so, and `loopBudgetLeft()` what is left (-1 after a call that ran out; a negative budget is -1). A loop whose
  count is known as it starts is charged it there, any other one an iteration. The checks go in after the optimiser, so
  within its budget a call computes what it would without it, bit for bit, and at the same speed
  ([extending.md](extending.md), recipe 29, has the measurements). A call that ran out leaves the instance
  as its optimised code had it, which need not be any point of the source: re-initialise the instance (or discard it)
  before running the program again. Off by default; unlimited until the host sets it.
- **Bounds checks.** A program compiled with `Options::boundsCheck`, a development build, checks every access whose
  array the compiler sees against the array's extent (an element must be one of its own, a pointer formed from it may
  also point one past its end), and a call whose check fails returns early, 0, with every function of the program that
  called it, before the access: `boundsViolation()` is the first such check since `clearBoundsViolation()`, a
  `BoundsViolation` with the file, line and column, the array as the source names it, its type and extent, and the
  index (`message()`: `echo.fm:14:33: index -188 out of bounds for 'line' of type 'float[512]'`). A pointer the
  program keeps (a variable's, a parameter's: a heap block, a host's buffer) is not checked, nor are an FFT's buffers
  against its plan's size. While the record holds a check, each call of the program returns early, so clear it before
  calling the program again, `init` included. Within its arrays a call computes what it would without the checks, bit
  for bit; a check costs a compare and a branch where the optimiser cannot prove it, and a loop that updates a field
  may then store it every iteration (the measured ones a third to two fifths slower: recipe 30). Off by default, and
  without it the code is the same, byte for byte.
- **Macros.** `Options::defines` defines macros before the program's first line, as C's `-D` does: each name's value,
  `"1"` for a flag as `-D DEBUG` gives it, `""` for none. A program reads them in `#if`, `#elif`, `#ifdef` and
  `#ifndef`, so a development build compiles in the instrumentation that sits in `#ifdef DEBUG` groups (a field, a
  statement, a method, an `#include`), and the build that plays skips those lines unread: they cost nothing, there or
  in the compile, and a module's layout may differ between the two (read `module()` for each). A program's
  `#ifndef LEVEL` / `#define LEVEL 2` / `#endif` is a default the host's value overrides. A name that is no
  identifier, `defined` or a keyword fails `compile()`, and so does a host's macro whose name the program uses in code:
  name them as C names its own (`DEBUG`, `HOST_VERSION`). A host that reads the program's text itself (an editor's
  language service, or its own `#pragma` lines) sees every directive as a line that starts with `#`, and the lines of
  the groups FMinus skips too: it reads both branches, or skips the groups its own defines skip
  ([extending.md](extending.md), recipe 31).
- **Calls by name.** A host that learns a function's signature only as it runs (a console that calls the function
  its user names, a test runner, a tool that tries a program's function) calls it by name: `callable(name, errors)`
  gives a `Callable`, its parameters and result as values (each parameter's name, its type as FMinus writes it, its
  `kind`, and for a pointer what it points to), and `call(callable, {Value::of(0.5f), Value::of(3)})` checks the
  arguments against them and calls the function on the calling thread, returning a `CallResult`: a `status`, the
  `value` it returned, and `message()`, one line in FMinus's words. A wrong name, a type `call()` does not pass, a
  function whose code does not link, the wrong number of arguments, an argument of another type (it converts nothing:
  `Value::scalar(kind, number)` does, as `setValue` would), a `const` array for a pointer the function writes
  through, a null or misaligned instance, or another program's: each is a status and a message, never undefined
  behaviour. It passes `bool`, `int`, `float` and `double`, and pointers to the host's memory (`Value::array(buffer)`,
  `Value::instance(memory, module)` for a method's `this` or an `Osc*`, with the program's own `Module`), and returns
  a scalar or nothing. The first `callable()` of a function compiles a thunk, a few instructions that load each
  argument as its parameter's type and call the function as C does (about 2 ms; it allocates, so off the audio
  thread); a `call()` then adds about 15 ns to the function, and no lock. The loop budget and bounds checks stop a
  call as any, and say so (`OutOfBudget`, `OutOfBounds`) without allocating, so on the audio thread too: set the
  budget and clear the record before each call, and serialise calls from several threads, as the budget and the
  record are the program's ([extending.md](extending.md), recipe 36).
- **Profiling.** A host's sampling profiler, which interrupts the thread that runs the program now and then and reads
  its program counter, maps each sample to a function with `codeRanges(errors)`: every range of the program's machine
  code, sorted by address, each a `CodeRange` with its `address` and `size`, the `symbol` it has in the object, the
  program's `function` it is the code of (`"Osc::tick"`) and that function's `module` (`"Osc"`), its `kind` (a
  function's code, an entry point's or a parallel region's, a thunk, or FMinus's own), and the functions the optimiser
  inlined into it (`inlined`), whose samples are its. `codeRangeAt(ranges, pc)` finds a sample's range (a binary
  search: no allocation, no lock). `codeRanges` links the program if no function has been looked up yet, so after
  `compile()` the list is whole, and the code stays where it says while the program lives: take it once, off the audio
  thread. Code that is not the program's, the runtime's, the C library's or the host's, is in no range
  ([extending.md](extending.md), recipe 37, and a reference sampler in `tests/library/ProfileTest.cpp`).
- **The web.** In a build with `FMINUS_WEB`, `Options::web` is what the program may reach through `http_get` and
  `http_post`: URL patterns, a body limit, a timeout, a redirect limit, a CA file; none by default, so every request is
  `HTTP_REFUSED`. `webRequests()` lists every URL the program names, with its method and where it is, before anything
  runs; `stopWeb()` cancels what is in flight and wakes a thread in `http_wait`; the destructor frees every request
  ([web.md](web.md)). With `FMINUS_WEB_SERVER` too, `Options::web.server` is where the program's
  `http_listen` may listen (`"localhost:0"`, ...; nowhere by default), with a token, the names a `Host` may say, the
  static files the library serves for it (a page), and its limits; `webRoutes()` lists every route the program names,
  before anything runs, a route statement's with its parameters (name, type, path or query, range, default); `webPort()` is the port it listens on; and `stopWeb()` and the destructor close its server,
  waking a thread in `http_accept`.
- **Files.** In a build with `FMINUS_FILES` (the default), `Options::files` is what the program may reach through
  `file_read`, `file_list`, `file_write` and `file_append`: its folders (a name, a directory, whether it may be written,
  its types), a deny-list compared by identity, the types allowed, absolute names (`anywhere`, with
  `FMINUS_FILES_ANYWHERE`), one hard link only, and the limits and quotas; none by default, so every name is
  `FILE_REFUSED`. `compile()` checks it against the build's ceilings every time and opens the folders' directories once,
  when the program names a file; `files()` lists every name the program uses, with its operation and where it is,
  before anything runs; the destructor frees every file the program still holds and closes the folders
  ([files.md](files.md)).
- **Lifetime.** A function pointer, and a `Callable`, is valid while its `Program` lives, and the code stays where
  `codeRanges()` says. A `Callable` is the program's own: the host keeps the pointer `callable()` gives, as it keeps a
  function's, never a copy.

`include/fminus/Circuit.h` is the circuit compiler's: `fminus::circuit::generate(netlist, "pedal.cir")` turns a
SPICE-style netlist into the text of one FMinus module, which `Program::compile` compiles like any other source, its
errors in clang's words as FMinus's are; `set()` takes the pots, then the ramped switches' positions
(`Result::switches`). A host that serves `#include` generates in its resolver for a `.cir` name
([circuits.md](circuits.md) has the lambda). A netlist that leaves its configuration open plays a
provisional one until `fminus::circuit::selfTest`, which takes seconds and runs on the host's threads, answers; the
host stores the answer (`Choice::text()`) under the netlist's key and generates again with it. It is C++17 with no
LLVM too, and comes with `FMinus::FMinus` (its library, `libfminus_circuit.a`, links into `libfminus.a`'s users; the
self-test's JIT side is in `libfminus.a`).

The header has no LLVM in it and needs C++17. In CMake, a host links `FMinus::FMinus`, and gets FMinus's include
directories and libraries (LLVM's too, linked statically, and the FFT inside the runtime's), but neither LLVM's headers
nor FMinus's C++23. As a
subproject, FMinus builds its library and runtime only (its tests, tools and examples are on only at the top level):

```cmake
include(FetchContent)
FetchContent_Declare(FMinus GIT_REPOSITORY https://github.com/Snipet/FMinus.git GIT_TAG <commit> SYSTEM)
set(FMINUS_WERROR OFF)               # warnings in FMinus's sources are not errors in the host's build
FetchContent_MakeAvailable(FMinus)
target_link_libraries(my_plugin PRIVATE FMinus::FMinus)
```

A subproject gets its LLVM as FMinus's own build does (Dependencies, above): by default it builds a flavor into its
deps cache. To reuse one an FMinus checkout has built, set `FMINUS_LLVM_MODE` to `system`, `LLVM_DIR` to
`<checkout>/.deps/install/llvm-<version>-<flavor>/lib/cmake/llvm` and `FMINUS_DEPS_DIR` to `<checkout>/.deps` (for its
Bison and Flex) before `FetchContent_MakeAvailable`. Calling into FMinus's internals instead (`"frontend/Frontend.h"`,
`"jit/Jit.h"`, ...) takes `FMinus::Internal`, which adds `src/`, LLVM's headers and C++23.

## Notes for running inside a plugin

These are constraints a plugin runs into. `examples/plugin/`, a JUCE plugin, meets them, and its README says how:

- **Executable memory.** Under the hardened runtime, macOS needs an entitlement for memory written at run time and
  then executed: `com.apple.security.cs.allow-jit` (for `MAP_JIT`) or `allow-unsigned-executable-memory`. A plugin
  runs in its host's process and has only the host's entitlements, so check each target DAW early. Unsigned test
  executables (this repository's) are unaffected.
- **Real time.** LLJIT compiles lazily, on the thread that looks a symbol up. Compile and look up on a background
  thread (the plugin does both in `prepareToPlay`), hand the function pointer to the audio thread atomically, and keep
  JIT'd code free of allocation and locks: mark the functions the audio thread calls `[[clang::nonblocking]]`, and
  FMinus refuses any that could reach `malloc` or `free` ([extending.md](extending.md), recipe 11). A host
  that runs its audio thread with flush-to-zero can tell the compiler so (`Program::Options::denormals = Flush`): the
  code is then folded for that mode, and each entry point sets it if the caller has not
  ([extending.md](extending.md), recipe 20). The plugin does: its `processBlock` flushes (JUCE's
  `ScopedNoDenormals`), and it compiles with `Flush`, so its `process` finds the mode set and writes nothing.
- **Printing.** A program's `printf` and `eprintf` write the process's own C streams, stdout and stderr, which a
  plugin's host may not show or may have closed ([programs.md](programs.md), "Printing"). Both allocate, in the
  effects' terms, so a `[[clang::nonblocking]]` function cannot print: trace on a background thread, or from a function
  the audio thread does not call.
- **Parallel regions on the audio thread.** A `parallel for` or `parallel { }` runs on FMinus's pool and the audio
  thread together, in the audio thread's floating-point mode, and allocates and locks nothing. The audio thread starts
  on its own share at once; of the pool threads that slept (no region for 50 µs), it wakes only as many as the region
  has seats for (a region of two iterations, one), a system call each that does not block; at the end it spins for the
  chunks other threads are still running. The pool's threads run at the
  user-interactive quality of service on macOS: they are not real-time threads, and join no audio workgroup, so the
  system may run them late, and the audio thread then takes their chunks itself. A region costs about 4 µs on an M5
  with every core (under 2 µs on two) against a plain call: worth it when its iterations take tens of microseconds
  together, a synth's voices, not a block's samples. A host whose other audio threads run regions too gets one region
  on the pool at a time; the others run on their own threads. `Program::Options::threads` limits or turns them off.
- **Loops that never end.** A program stuck in a loop holds the audio thread, and the host with it. Compile with
  `Program::Options::loopBudget`, set the budget before each call (a generous one: many times what a block takes), and
  after it check `loopBudgetExceeded()`: the call returned early, and the plugin can silence the program, report it,
  and re-initialise the instance (or discard it) before the next call. The budget is a count of iterations, not a clock
  ([extending.md](extending.md), recipe 29).
- **An index outside its array.** While a program is written, compile it with `Program::Options::boundsCheck`, and
  after each call check `boundsViolation()`: the call returned early, before the access, and the plugin can silence
  the program, show `message()` (off the audio thread: it allocates), then `clearBoundsViolation()`, and only then
  re-initialise the instance (or discard it): while the record holds the check, `init` would return partway, as any
  call would. The checks see only arrays the program declares: a delay line in a heap block (`calloc`'s) is not
  checked, and one in a field array is. Compile the build that plays without it, which is as fast as ever
  ([extending.md](extending.md), recipe 30).
- **The network.** In a build with `FMINUS_WEB`, a program's requests reach only what `Program::Options::web` allows:
  none by default (the library's policy, not a sandbox: [web.md](web.md)). A sandboxed host needs the
  network client entitlement itself; the library opens no listening socket unless the host gives the program's server
  a listen (`Options::web.server`), so by default there is no firewall prompt. A plugin that serves listens inside its
  host application's sandbox and under its firewall rules: macOS's App Sandbox wants the network server entitlement
  even for loopback, which an Audio Unit may not have, so a failed listen is an ordinary outcome (`http_port` says
  `HTTP_TRANSPORT`, and the program carries on without its page), and a listen that is not loopback asks the user
  (from the research's sources: not tried); destroy
  every `Program` with a web context before the plugin is unloaded, which joins the library's I/O thread (and can wait
  for a DNS lookup in flight, up to the system resolver's timeout: not on the UI thread); and a loader thread in
  `http_wait` is woken by `Program::stopWeb()` ([web.md](web.md)).
- **Files.** A program reads only folders `Program::Options::files` maps (none by default: the file builtins' policy,
  not a sandbox, [files.md](files.md)). Give it directories the plugin owns the meaning of, its bundle's
  resources, its own folder in the user's library, a folder the user chose; a sandboxed host can give only what it
  has, and an access the sandbox refuses is `FILE_DENIED`. Every file builtin that touches the disk is refused in a
  `[[clang::nonblocking]]` function, so a program loads on the plugin's loader thread and plays from memory; nothing
  is mapped, no thread is started, and the folders' handles close with the `Program`.
- **Size and symbols.** Static LLVM adds tens of megabytes. Export only the plugin's entry points (an
  exported-symbols list), so LLVM's symbols cannot clash with another copy of LLVM in the host process.
- **Bit-exactness across machines.** FMinus fuses and reassociates nothing, and in its default math mode a float's
  `sinf` is the same at any width, level or block size, and on arm64 and x86-64 alike (its own kernels). But the JIT
  tunes code for the CPU it runs on, the libm and fast math modes' functions are the system's, and a `poly float`'s
  width orders its float sums ([simd.md](simd.md)). Decide what a "same output" test means early.
