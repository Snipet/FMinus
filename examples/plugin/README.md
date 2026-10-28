# FMinus Filter: a JUCE plugin whose DSP is FMinus

FMinus Filter is an audio effect, AU, VST3 and Standalone on macOS, built with JUCE 8.0.4. It's a resonant
state-variable filter (low-, band- or high-pass) with three controls: cutoff, resonance and mode. Its DSP is an FMinus
program, [`filter.fm`](filter.fm), embedded in the plugin. The processor JIT-compiles it with `fminus::Program` when
the host prepares the plugin, for the CPU it runs on, and calls the result on the audio thread. Its editor is JUCE's
`GenericAudioProcessorEditor`.

| File | |
|---|---|
| `filter.fm` | the DSP: a module `Filter` with four parameters and a `[[clang::nonblocking]]` `process` |
| `Processor.h`, `Processor.cpp` | the `juce::AudioProcessor`: compiles the program, runs it, keeps the parameters and the state |
| `Check.cpp` | `fminus-plugin-check`, a console program that tests the processor headless, through `processBlock` |
| `CMakeLists.txt` | the project: JUCE, FMinus as a subproject, the plugin, the check |

## Building it

It is a CMake project of its own, outside FMinus's build. It uses the LLVM that FMinus's own build installed in its
dependency cache, so build FMinus once first (`cmake --workflow --preset dev` at the checkout's root; the first build of
LLVM takes 10–30 minutes). Then, from the checkout's root:

```bash
cmake -S examples/plugin -B build-plugin -G Ninja
cmake --build build-plugin -j6
```

- **JUCE** comes through FetchContent. It is pinned by commit in `cmake/FMinusPins.cmake` (`FMINUS_JUCE_*`), and the
  first configure clones the repository. If you already have a JUCE 8.0.4 checkout, pass
  `-DFETCHCONTENT_SOURCE_DIR_JUCE=<its path>` to use it. Configure checks the version, the commit (`HEAD`) and the
  tracked files (`git status`) either way: a JUCE at another version or commit, or with a tracked file changed, fails
  there. One that isn't a git checkout gets a warning that only its version can be checked.
- **FMinus** is this checkout, added as a subproject, which builds its library and runtime only. The plugin links
  `FMinus::FMinus` and includes `fminus/FMinus.h`, which has no LLVM in it.
- **LLVM** comes in FMinus's `system` mode, from `<FMINUS_DEPS_DIR>/install/llvm-23.1.3-Release[-assert]-<hash>`.
  `FMINUS_DEPS_DIR` defaults to the checkout's `.deps/`, or to the environment variable of that name. Only a Release
  build of LLVM is taken from there. A Release build of the plugin (the default) takes the flavor without assertions if
  there is one (FMinus's `release` preset builds it), else the one with them (the `dev` preset's); any other build of
  the plugin, the other way round. `-DLLVM_DIR=<install>/lib/cmake/llvm` chooses one by hand, which is also the way to
  use an LLVM of another build type (a Debug one, made with `FMINUS_LLVM_BUILD_TYPE`). With none there, configure
  stops and says how to get one. Bison and Flex come from the same cache too.

The build writes the bundles under `build-plugin/FMinusFilter_artefacts/Release/`: `AU/FMinus Filter.component`,
`VST3/FMinus Filter.vst3` and `Standalone/FMinus Filter.app`. Its last step signs each one ad hoc, over all its files,
so `codesign --verify --strict` accepts it (JUCE signs only when it copies a plugin, which this build leaves off). It
installs nothing. To try the plugin in a DAW, copy the component to `~/Library/Audio/Plug-Ins/Components/` and the VST3
to `~/Library/Audio/Plug-Ins/VST3/` (read the first caveat below before you do). The Standalone runs from where it is
built:

```bash
open "build-plugin/FMinusFilter_artefacts/Release/Standalone/FMinus Filter.app"   # filters the audio input
```

## The check

FMinus's `ctest` doesn't run this project, so it has its own test. Run it either of these ways:

```bash
ctest --test-dir build-plugin                                       # plugin.check
build-plugin/fminus-plugin-check_artefacts/Release/fminus-plugin-check
```

It creates the processor the way a host does and prepares it at 48 kHz, then again at 44.1 kHz. It runs stereo signals
through `processBlock` and exits 1 unless all of these hold:

- The plugin's parameters have filter.fm's ranges and defaults.
- `prepareToPlay` compiles once per sample rate, and not again at the same rate.
- Every output is finite, including at the extremes (resonance 1, the cutoff at 20 Hz and at 20 kHz).
- The low-pass at 1 kHz takes 10 kHz down by at least 20 dB (it measures −42.7 dB) and passes 100 Hz within 1 dB. The
  high-pass does the reverse.
- The resonant band-pass peaks at 1/k at its cutoff, as the analog filter does. A sample rate frozen wrongly would move
  that peak.
- The output is the same, bit for bit, in blocks of 64, 61, 1 and 1000 samples. 1000 is more than `prepareToPlay` was
  told to expect. The parameters move at the same samples in every run, so the smoother's ramps land on the same
  samples too.
- A moved cutoff glides. No step between samples may exceed twice what the input sine moves by itself. `Check.cpp`
  justifies that bound, and a cutoff that stepped without the smoother would exceed it tenfold.
- A new mode crossfades. At resonance 1, through all six switches between the modes, no step may exceed 1.1 times
  what the outputs move by themselves (`Check.cpp` says why). The check also runs the switches with the glide taken out
  of `filter.fm` and expects the bound to fail: it does, by about fourteen times.
- `reset`, which hosts call when they stop or locate, starts the filter from rest. A ring stops at once, and the output
  afterwards is a fresh processor's, bit for bit, even mid-glide. `prepareToPlay` again at the same rate, mid-ring,
  changes nothing, as when JUCE's AU switches to offline rendering. `releaseResources` then `prepareToPlay` starts from
  rest.
- The tail the processor reports, 3 s, covers its longest ring. At 20 Hz and resonance 1 the output falls 60 dB below
  its peak 2.74 s after the input stops.
- One sample that is NaN, an infinity or huge (1e37) doesn't stick. Every output is finite, and 0.2 s later the output
  is within 1e-4 of a clean run's. With `filter.fm`'s test of its input taken out, the test of its states after each
  call still brings the filter back from the next block.
- A state saved and restored into another processor gives the same output, bit for bit.
- A program that doesn't compile is refused, and so is one whose `process` isn't `[[clang::nonblocking]]`. In both
  cases the audio passes through untouched and the error is logged once.

It also prints what `processBlock` costs: 6.8 ns per sample per channel on an M5 (it varies from run to run, and much
more with the machine's load), about 0.07% of a core for stereo at 48 kHz. That includes a sine, a cosine and two
divisions per sample. The sine and cosine are 2.1 ns of it: in FMinus's default math mode each was, when this was
measured, lane 0 of libsystem_m's four-lane function (FMinus's own kernels since: docs/extending.md, recipe 23), which
costs more a call than libm's, in a loop that the filter's recursion keeps from vectorising. Compiled with `Program::Options::math = Libm` the filter takes 4.7 ns and passes the same checks. The
modes' crossfade adds about 0.4 ns. Flush-to-zero costs nothing: `process` finds the mode set (the same 6.8 ns compiled
with `Preserve`).

## How FMinus is used

### The program

```c
module Filter
{
    param float cutoff [[fminus::range(20, 20000)]] [[fminus::default(1000)]];          // Hz
    param float resonance [[fminus::range(0, 1)]] [[fminus::default(0.3)]];             // Q from 0.5 to 25
    param int mode [[fminus::range(0, 2)]];                                              // low-, band-, high-pass
    param float sampleRate [[fminus::range(8000, 384000)]] [[fminus::default(48000)]];  // Hz
    float ic1;       // the integrators' states
    float ic2;
    float lowMix;    // each output's weight in the mix, gliding to the mode's
    float bandMix;
    float highMix;
    int started;     // 0 in a new instance: the weights start at the mode's, without a glide

    void process(float* restrict samples, int n) [[clang::nonblocking]] { ... }
};
```

The filter is a state-variable filter integrated by the trapezoidal rule. Zavalishin calls this topology-preserving;
the form, with the integrators' states held as "equivalent currents", is Andrew Simper's. Chamberlin's filter in
`examples/parameters/` holds only up to about a sixth of the sample rate. This one is stable at every cutoff below the
Nyquist frequency, which a cutoff control that reaches 20 kHz needs. FMinus has no `tanf`, so its `g = tan(pi fc / fs)`
is a sine over a cosine. `process` filters a channel's samples in place.

Two more things in `process` keep the filter from clicking or sticking, and both work a sample at a time, so the
output doesn't depend on how the host splits it into blocks:

- **A new mode crossfades.** At a high resonance the three outputs are large and a quarter or half a period apart, so
  switching between them at once would jump the output by up to twice their amplitude. Instead, each output's weight in
  the mix moves a little every sample and gets all the way in 20 ms. The step per sample is `50 / sampleRate`, which
  the frozen rate makes a constant. A new instance starts at its mode, without a glide.
- **A bad input sample is taken as 0.** That covers a NaN, an infinity, or anything of 1e30 or more. Otherwise it would
  make the integrators' states NaN, or overflow them, and every later output would be NaN. The test is on the input, off
  the chain that carries the states from one sample to the next, so it costs next to nothing. The same test on the
  states sits on that chain, and makes `processBlock` about 30% slower in the libm math mode (6.1 ns a sample a channel
  against 4.7); in the default mode it costs nothing measurable (6.7 against 6.8). If a state goes bad anyway, say
  through a parameter out of its range, which this processor never sets, a test after the loop restarts the filter from
  rest for the next call.

The processor chooses each parameter's binding when it compiles (`Program::Options::parameters`):

| Parameter | Binding | The processor |
|---|---|---|
| `cutoff` | `Binding::stream()` | fills a buffer from a `juce::SmoothedValue` (a 20 ms multiplicative ramp), one value per sample, and passes it with `setStream` before each call |
| `resonance` | a knob (the default) | `setValue` from the parameter, once per block |
| `mode` | a knob, an `int` | `setValue` from the choice's index, once per block; `filter.fm` crossfades its outputs over 20 ms itself |
| `sampleRate` | `Binding::frozen(rate)` | nothing: the host's rate is compiled in as a constant, and a new rate compiles again |

A moved cutoff therefore glides a sample at a time, never a block at a time, and a new mode crossfades. The two show
the two places a control can be smoothed: in the host, which smooths the cutoff and streams it, or in the program,
which keeps the mix's weights in its instance. The plugin's parameters mirror the program's: the same names, ranges
and defaults, which the check compares.

It compiles with `Program::Options::denormals = Flush`: `processBlock` runs with flush-to-zero (JUCE's
`ScopedNoDenormals`), so the program is folded for that mode, and `process`, an entry point, finds the mode set and
writes nothing ([docs/extending.md](../../docs/extending.md), recipe 20).

### The processor, thread by thread

- **`prepareToPlay`**, off the audio thread, compiles `filter.fm` when the rate has changed since the last compile.
  FMinus's optimiser and code generator run there and take milliseconds. Before the processor accepts the program, it
  checks for the module, the parameters with their types, and `process` marked `nonBlocking("Filter::process")`.
  FMinus has then checked that `process` can't reach `malloc`, `calloc`, `free` or an external function that isn't
  nonblocking. The processor refuses anything else. It looks up `Filter::process`, which compiles it to machine code,
  also here. It then makes one instance of the module per channel, as `Program::Module` describes one: aligned,
  zeroed, and set to its defaults with `setDefaults`. It also allocates the cutoff's buffer. If the program fails or
  is refused, the error goes to `juce::Logger` once. When neither the program nor the channels have changed, it keeps
  the instances, and the filter's state in them. JUCE's AU prepares again in mid-stream when a host switches offline
  rendering on or off, and the filter carries on without a click.
- **`processBlock`**, on the audio thread, sets the knobs, fills the cutoff's buffer, then runs `process` on each
  channel in place. It runs the block in parts if a host sends more samples than it announced. It doesn't allocate,
  lock or log. Without a program, it returns and the audio passes through as it came.
- **`reset`** is what hosts call to clear tails: the AU's `Reset` (on stop and locate, and from JUCE when the AU's
  bypass is switched) and the VST3's `setProcessing(false)`. Neither holds a lock that `processBlock` holds, and either
  may come while it runs. So `reset` only sets an atomic flag. At its next block, `processBlock` puts each instance back
  as new (`memset`, then `setDefaults`) and sets the cutoff's smoother to the parameter without a glide.
  `releaseResources` sets the same flag.
- **The tail** is reported as 3 s, the longest the filter rings, to 60 dB down: at 20 Hz and resonance 1 (Q 25), the
  envelope `exp(-w t / 2Q)` takes `6.91 * 2Q / w`, 2.75 s. It is a constant, the worst case, because a tail that
  followed the parameters would need `updateHostDisplay`, which stays off the audio thread.
- **The hand-over** uses plain members, because `prepareToPlay` and `processBlock` never run at once. What keeps them
  apart also orders the processor's writes before the audio thread's reads. That is either the host's contract (an AU
  renders only once initialised, and JUCE prepares it in `Initialize`; a VST3 processes only while active, and JUCE
  prepares it in `setActive`) or a lock. JUCE prepares the AU again under the processor's callback lock when offline
  rendering is switched, and the AU's render takes the same lock. The Standalone's `AudioProcessorPlayer` holds its own
  lock around both. In FL Studio, JUCE's VST3 wrapper holds a mutex around `setActive` and `process`. `Processor.cpp`
  says more. The program is destroyed and recompiled only in `prepareToPlay`, so its code is never running then. A
  plugin that compiled while audio played, for example to freeze a setting once it stops moving, would hand the new
  program over through an atomic pointer. It would destroy the old one only after the audio thread let go of it
  (`examples/parameters/README.md`, "Freezing while it plays").

## Caveats

These are the points from FMinus's README, "Notes for running inside a plugin", as they apply here:

- **Executable memory.** A JIT writes machine code and then runs it. Under the hardened runtime, macOS allows that only
  with `com.apple.security.cs.allow-jit` (for `MAP_JIT`) or `com.apple.security.cs.allow-unsigned-executable-memory`.
  A plugin runs in its host's process with the host's entitlements, so check every DAW you target before relying on
  it. The bundles built here are signed ad hoc, without the hardened runtime, so the Standalone and the check are
  unaffected.
- **Size.** LLVM is linked into each bundle statically. With the linker's `-dead_strip` (on here), a bundle is about
  87 MB on the LLVM flavor with assertions: 46 MB of it is code and 41 MB is its symbol table. Linking with `-Wl,-x`
  as well drops the local symbols (and with them function names in lldb and Instruments) and brings a bundle to about
  47 MB. The flavor without assertions leaves LLVM's assertion code out as well.
- **Symbols.** The AU and the VST3 each export only their entry points: `_JuceAUFactory` and `_FMinusFilterAUFactory`,
  or `_GetPluginFactory`, `_bundleEntry` and `_bundleExit`. Each has its own `-exported_symbols_list`
  (`build-plugin/exports-*.txt`), and `nm -gU` on a bundle's binary shows the list. If LLVM's symbols were exported,
  many of them weak, macOS would merge them with another LLVM's in the same process (another such plugin, or the host's
  own). The Standalone is its own process and isn't restricted.
- **Same output.** The JIT tunes code for the CPU it runs on. The check's "bit for bit" compares runs on one machine;
  another CPU may differ in the last bits.
- **Licences.** JUCE is dual-licensed, under the AGPLv3 or JUCE's commercial licence. The GPL-3.0 and the AGPLv3 each
  allow combining code under the other (their section 13), so this example, under FMinus's GPL-3.0, uses JUCE under
  the AGPLv3. A plugin built from it and distributed comes under the AGPLv3's terms too, unless you hold a JUCE
  licence. The plugin also links FMinus's runtime, which contains PFFFT, so a binary you distribute reproduces PFFFT's
  copyright notice, conditions and disclaimer (its `LICENSE.txt`) in its documentation (FMinus's README, "License").
