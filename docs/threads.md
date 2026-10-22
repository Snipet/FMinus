# Threads: a design for parallel loops

**Status: a proposal, 2026-10-22. Nothing here is in the compiler.** It is the research the user asked for before
deciding how FMinus gets multithreading: what audio needs from threads, what other languages and DAWs do, and a design
that keeps the programmer out of threading's classic trouble (races, deadlocks, memory ordering, priority inversion,
results that depend on the core count). It names the files each piece would touch, as extending.md's recipes do, and
ends with the questions the user decides.

## The short version

One attribute on a `for` loop, or on a block:

```c
[[fminus::parallel]] for (int v = 0; v < 8; ++v)
    voices[v].render(n);                        // each voice renders its block; the voices run at once
```

The loop means exactly what the same loop means without the attribute. The attribute lets the compiler run the
iterations at once, and the compiler **refuses the attribute** wherever that could change what the loop computes: an
iteration may write only what is its own (locals it declares, the element `a[v]` of an array or buffer, and everything
inside that element), and may read only what no iteration writes, or what is its own. Nothing else is new: no threads,
locks, atomics or channels appear in the language, so a program cannot deadlock, cannot race, and never orders memory
by hand. The runtime (`src/runtime/`) runs the iterations on a pool of real-time threads the host starts off the audio
thread, and runs them in order on the calling thread when there is no pool, with the same result either way: a
program's output does not depend on the core count, on whether a pool exists, or on how the system schedules it. The
cost to the programmer is the shape of the loop (its counter counts from a value to a limit, as Cilk's `cilk_for`
asks) and, now and then, a `restrict` or a buffer moved into the module it belongs to.

What this buys audio: a synth's voices, a plugin's channels, a vocoder's bands, a granular engine's grains, an
oscillator bank's partials all render at once, at the grain where threads pay (a block of work each, tens of
microseconds and up), while samples stay in order and `poly float` keeps the lanes.

## What audio needs from threads

Audio differs from the usual parallel workloads in three ways, and each shapes the design.

- **Deadlines a block at a time.** The host calls `process` every block, 32 to 512 samples, 0.7 to 10 ms, and the
  result must be there when it returns. Work that starts a thread, takes a lock the GUI might hold, allocates or waits
  for a page can miss it. So nothing on the fork-join path may do any of those: the workers exist before the block
  starts, the hand-over is atomics on memory already touched, and the audio thread does its share of the work rather
  than wait idle.
- **State from sample to sample.** A filter's output needs the last one's state, so a block's samples cannot run on
  different cores; `poly float` already covers what can, lane by lane. What is independent is **instances**: voices,
  channels, bands, grains, partials, each a module with its own state that renders a block. That is the grain threads
  want anyway: a voice's block is a few microseconds of work, and a fork-join costs a microsecond or so with warm
  workers (to measure; below), so the loop over voices pays and the loop over samples never would.
- **The same output every time.** A plugin's render must match its real-time playback, a test must match on a 4-core
  laptop and a 16-core desktop, and FMinus already promises that a float's `sinf` is one result wherever it is computed
  (recipe 15). Threads must not break that: the result may not depend on how many threads there are or which took
  which iteration.

Threading's classic hazards, and who takes each one off the programmer here:

| Hazard | Taken by |
|---|---|
| data races | the compiler: an iteration writes only its own, reads only its own or what no iteration writes (below) |
| deadlock, livelock | the language: fork-join only, no locks or waits a program can write, so there is nothing to wait on in a cycle |
| memory ordering | the runtime: the fork publishes what came before it, the join what the iterations wrote; a program never names an ordering |
| thread lifetimes | the runtime: a pool the host starts and stops off the audio thread; the audio thread joins the work |
| load balance, oversubscription | the runtime: one pool a process, iterations handed out in fixed chunks as workers come free; nested parallel loops run in order |
| nondeterminism | the design: a fixed chunking, a reduction combined in chunk order, so the result is the same at any thread count, with or without a pool |
| priority inversion | the runtime: workers at the audio thread's priority (an audio workgroup on macOS, `SCHED_FIFO` on Linux, MMCSS on Windows), and the audio thread works too |
| the floating-point environment | the runtime: a worker computes in the forking thread's mode (flush-to-zero, rounding), which is per-thread state |
| false sharing | the compiler and runtime: an iteration's privates are its own stack frame; a reduction's partials one cache line a chunk |
| reentrancy of what the body calls | the compiler: calls are checked by what they can write; the runtime: its functions are reentrant (the FFT needs a fix, below) |

## Prior art, and what FMinus takes from each

Each row is a way of putting threads in a language or a product, what it asks of the programmer, and what this design
takes from it. The sources are at the end of the note.

| Prior art | What it does | What FMinus takes |
|---|---|---|
| **OpenMP**, `#pragma omp parallel for` with `private`, `shared` and `reduction` clauses | The loop runs in parallel because the pragma says so; the programmer promises there is no race, and the compiler checks nothing. A pragma is ignorable, so the program is C without it. | The surface: one annotation on an ordinary `for`, a reduction recognised in the loop. Not the trust: the pragma's clauses are what FMinus's checker infers and verifies. |
| **Cilk** (MIT, Intel Cilk Plus, now **OpenCilk**): `cilk_for`, `cilk_spawn`, `cilk_sync`, `cilk_scope` | Fork-join parallelism with *serial semantics*: delete the keywords (the *serial elision* of Cilk-5, OpenCilk's *serial projection*) and the program is a C program that computes what the parallel one may compute; a race-free program must match it. `cilk_for` constrains its loop to a counted one (one control variable declared in the initialiser, a termination value and an increment that do not change between iterations, no assignment to the variable in the body) and runs the iterations on a work-stealing runtime. Races are still the programmer's (Cilk ships a race detector, Cilksan). **Tapir** (Schardl, Moses and Leiserson, PPoPP 2017) puts fork-join into LLVM's IR itself, three instructions, `detach`, `reattach` and `sync`, so LLVM's passes optimise across the fork; it lives in OpenCilk's fork of LLVM (3.0 of 2025 on LLVM 19, the 4.0 candidates on LLVM 21), and upstream LLVM has none of it. | The loop's shape, the serial semantics as the specification, and the principle that no runtime (or one worker) gives the same program. FMinus adds what Cilk leaves to the detector: the checker. Outlining before LLVM, since FMinus builds on upstream LLVM, where there is no parallel IR. |
| **CPLEX**, WG14's C Parallel Language Extensions study group (2013–2017), ISO/IEC TS 21938-1:2019 | Cilk's constructs in C's reserved spelling: `_Task _Block { ... }` (a block whose tasks are joined at its end), `_Task _Spawn f(x);` (a call run as a task) and `_Task _Call` on a function that may spawn. Published as a technical specification in 2019, then abandoned when its sponsors moved on; a second part, vector parallelism, never left draft. | The precedent for the `_Parallel` spelling, and the lesson that C itself never settled this. |
| **ISPC** `foreach`, **Fortran** `do concurrent` (2008; `local` and `shared` locality since 2018) | The programmer declares a loop's iterations independent and the compiler may vectorise or (Fortran, on some compilers) thread them. ISPC says the elements of a `foreach` run in no particular order and reading another element's result is undefined; Fortran forbids leaving the construct and calling an impure procedure from it, and flang's documentation calls those rules "necessary, not sufficient" for running it in parallel. | The idea that independence is a property of the loop the programmer states once. FMinus checks the statement, which neither of these does. |
| **Deterministic Parallel Java** (Bocchino, Adve et al., OOPSLA 2009) | A type-and-effect system: every method declares the *regions* of memory it reads and writes, arrays can be partitioned by index so that `a[i]` is its own region, and the compiler proves that parallel branches do not interfere. Programs are deterministic by construction. | The whole idea, in miniature: an element indexed by the loop's counter is the iteration's own region, and a method's effects are read off its declaration. FMinus can do it without region annotations because it has no globals, no pointers to pointers and a `restrict` it already trusts, so the roots are few and visible. |
| **Rust** (`Send`/`Sync`, Rayon's `par_iter_mut`) | Ownership and borrowing make a data race a compile error; a parallel iterator hands each iteration exclusive access to its element. | The same disjointness, enforced by the type system there and by the loop's index here. |
| **Halide**, **Futhark** | Pure computations whose *schedule* (parallel, vectorised, tiled) is separate and cannot change the result. | The rule that a parallel directive is a permission, never a change of meaning. |
| **FAUST** (Grame): `-omp`, `-sch`, `-pl`, `-g`, still in its manual | A block-diagram language whose compiler extracts parallelism from the signal graph itself, as a DAG of vector loops over a block, and schedules it with OpenMP (LAC 2009) or its own work-stealing scheduler (LAC 2010); the programmer writes no threads at all. Their measurements made vectorisation (`-vec`) the first win, found "simple DSP effects are often faster in scalar mode", and that "vector sizes of 512 or 1024 samples usually give the best results" for the parallel modes: 10 to 20 ms of audio a block, far above a plugin's. | The warning about grain, and the model for what a later FMinus could do automatically on top of the checker: a module graph scheduled by the compiler. The explicit loop is the step before it. |
| **SOUL** (ROLI, dormant since 2022), **StreamIt** (MIT, 2002), synchronous dataflow (Lee & Messerschmitt, 1987) | A program is a graph of processors or actors with fixed rates; the runtime or compiler schedules them, across cores where it can, with no threads in the language. SOUL's processors "are written as single-threaded actors" and its overview promises "No race conditions. No system calls.", with the graph left to the runtime's "auto-parallelisation tricks"; synchronous dataflow's fixed rates give a schedule at compile time. | The target (audio as instances rendering blocks) and the confirmation that threads can be invisible; FMinus keeps C's imperative shape and gets there by checking instead of by a graph. |
| **DAWs** (Ableton Live, REAPER) and **Max/MSP**'s `poly~` | Parallelism across tracks, devices and voices, never across a block's samples: Live "assigns any independent segments of an audio path that can be processed in parallel to separate threads" while "all audio along the same signal path must be processed in chronological order", so one heavy plugin fills one core; REAPER's *anticipative FX processing* renders unarmed tracks ahead of the play head (about 200 ms) on worker threads, at the cost of a slower response to a live change; `poly~`'s `parallel` attribute "divides the voices among the available processors", worth it from signal vectors of 32 samples up. | The grain: an instance's block is the unit of work; and the across-blocks pipeline as later work. |
| **oneTBB**, `parallel_deterministic_reduce` | A reduction that "executes the same set of split and join operations no matter how many threads participate in execution and how tasks are mapped to the threads", on a fixed partitioning, so its result is the same at any thread count; the specification adds that "the result might differ from that obtained with an equivalent sequential (linear) algorithm". | FMinus's fixed chunking and in-order combination, and the same honest caveat. |
| **Apple audio workgroups** (macOS 11 and iOS 14; WWDC 2020, "Meet Audio Workgroups"; JUCE 7.0.8's `AudioWorkgroup`) | A real-time thread calls `os_workgroup_join` once on the device's workgroup (`kAudioDevicePropertyIOThreadOSWorkgroup`, or the one a plugin host hands over through its render context, which "may change from one render call to the next"; JUCE's `AudioProcessor::audioWorkgroupContextChanged`), so the scheduler treats the workers with the audio thread as one unit with its deadline; `os_workgroup_max_parallel_threads` says how many to make. Apple's own advice in the session: "If you're wondering if you should create your own realtime threads, the answer is probably no ... measure everything." | The runtime's priority model on macOS, the host API that hands the workgroup over, and the advice, which is why the example measures before it recommends. |
| **Real-time audio practice** ("Real-time 101", Rowland & Renn-Giles, ADC 2019; Bencina, "Time waits for nothing", 2011; Doumler, "Using locks in real-time audio processing, safely", 2020) | No locks, allocation or system calls on the audio thread; a wait on a mutex "not only blocks the audio thread but also leads to priority inversion"; hand data over with atomics and single-producer queues; where a wait is unavoidable, spin in stages (Doumler's lock spins about 25 ns, then 400 ns with pauses, then yields by the millisecond). | The runtime's rules, one by one, and the shape of its workers' wait. |

## The design

### Spelling

Three spellings fit a subset of C; the attribute is the recommendation.

```c
[[fminus::parallel]] for (int v = 0; v < 8; ++v) ...     // 1. a C23 attribute on the statement
parallel for (int v = 0; v < 8; ++v) ...                 // 2. a keyword of FMinus's own, as poly is
_Parallel for (int v = 0; v < 8; ++v) ...                // 3. a keyword in C's reserved spelling, as CPLEX's _Task _Spawn
```

1. **`[[fminus::parallel]]`** is C23 syntax: C23 lets an attribute specifier sequence stand before a statement, a
   `for` among them, and a C compiler ignores an attribute it does not know (clang with `-Wno-unknown-attributes`). So
   a program with parallel loops stays a valid C program with the same meaning, run in order, which is the README's
   test for every feature so far: `#define`-free serial elision. It is also what an attribute is for in C: a
   permission the compiler may use without changing what the program computes. FMinus already gives attributes teeth
   (`[[clang::nonblocking]]` is an error when broken, `fminus::range` and `fminus::default` are the host's), so one
   more checked attribute in the `fminus::` namespace reads as the language already does. The grammar gains one
   place for `attributes`, before a `for` or a block; `[` starts no expression, so there is no conflict.
2. **`parallel for`** reads best and stands out, as `poly float` does, and FMinus has taken lowercase keywords for
   its own concepts before (`module`, `param`, `poly`). It costs an identifier (`parallel` names things in DSP:
   parallel filters) and the C story: a `#define parallel` makes such a program C again, which is Cilk's serial
   elision exactly.
3. **`_Parallel`** is how C11 spells new keywords (`_Atomic`, `_Thread_local`) and how WG14's CPLEX group spelled
   Cilk's (`_Task _Spawn f(x);`, `_Task _Block { ... }`); it costs no identifier, but it is the ugliest, and CPLEX,
   published as a technical specification in 2019, was abandoned rather than folded into C.

The rest of this note writes `[[fminus::parallel]]`; everything in it holds for the other two spellings.

### Meaning

`[[fminus::parallel]] for (init; condition; step) body` computes what `for (init; condition; step) body` computes,
with one exception, the reductions below, and under these conditions, each an error when broken:

- **A countable loop.** `init` declares one `int` counter, `int v = start;` `condition` compares it with a limit,
  `v < limit`, `<=`, `>` or `>=`; `step` is `++v`, `v++`, `--v`, `v--`, `v += c` or `v -= c`. The start, the limit
  and `c` are loop-invariant (they read nothing the body writes), and `c` is not 0. The trip count is then computed
  once, before anything runs, as Cilk's `cilk_for` computes its (a 64-bit count, so `v += c` cannot overflow it). The
  body does not assign the counter.
- **No way out but the end.** `break` and `return` in the body are errors (`'break' cannot leave a parallel loop`);
  `continue` ends the iteration, as it does in C. A nested ordinary loop may `break` out of itself.
- **An iteration writes only its own, and reads only its own or what nothing writes.** The ownership rules, next.
- **Calls stay inside what the compiler can see**: the builtins, the program's own functions and methods (checked by
  what they can write), and no external function at first (an extension below lets C23's `[[reproducible]]` ones in).

A block takes the attribute too, for different work in parallel:

```c
[[fminus::parallel]]
{
    left.process(inL, outL, n);      // one task
    right.process(inR, outR, n);     // another: writes nothing the first reads or writes
}
```

Each statement of the block is a task, and the same rules apply with "task" for "iteration": what a task writes, no
other task reads or writes. The block means what the block means in C, its statements run in order. This is Cilk's
`spawn`/`sync` for the case audio has, a handful of unlike things to do at once (a stereo pair, two effects of a
chain that do not feed each other), without a `sync` the programmer can misplace. A parallel loop inside a task, or
a task that calls a method with a parallel loop in it, runs its iterations in order (nested parallelism, below).

### Ownership: the rules the compiler checks

Everything in the body that reads or writes memory has a **root**, the object or pointer it reaches memory through:
a local or parameter of the enclosing function, an array or module declared there, a field of `this` in a method, a
pointer value, or a local the body declares. The rules are about roots and indexes.

**Private.** A variable the body declares is the iteration's own: a `float`, an array, a module, a pointer. Each
iteration has its own copy (its own stack frame), so it may do anything with it.

**Owned.** An lvalue is *owned* by the iteration when it is

- `B[idx]`, where `B` is a *stable base* and `idx` an *owned index*;
- a field, element or component of an owned lvalue: `voices[v].osc.phase`, `voices[v].out[i]` for any `i` (the
  whole of `voices[v]` is this iteration's), `frames[v].x`;
- `*p`, `p[j]`, `p->f` for any `j`, where `p` is an *owned pointer*: `&owned`, or a pointer the body declares and
  initialises from an owned pointer; `p + k` is not owned (it may reach another iteration's element).

An *owned index* is `v`, `v + k` or `v - k` with `k` loop-invariant: injective in `v`, so two iterations never name
the same element. Each stable base is written through one index form per loop: `out[v] = x; out[v + 1] = y;` is an
error (iteration `v`'s second element is iteration `v + 1`'s first), and so is `out[2 * v]` with `out[2 * v + 1]` in
the first version, where `frames[v] = (float2)(x, y)` does the job. (A later version can admit a strided family,
`c * v + m` for `0 <= m < c`, once the checker reasons about ranges.)

A *stable base* is a root that denotes one object or one fixed pointer for the whole loop: an array or module
declared in the enclosing function; an array or module field of `this`; a `restrict` pointer parameter, or a
`restrict` pointer local declared before the loop, that the body does not assign. A pointer without `restrict` is not
stable, because C lets it alias anything of its type: it may be read through (rule B) but not written through, and
the error says why: `pointer 'out' may alias what the loop writes: declare it restrict`.

**Rule A, writes.** Every store in the body goes to a private or an owned lvalue. A store to anything else is an
error that says what to do: `variable 'mix' is assigned inside a parallel loop: declare it in the loop, or reduce it
with +=`; `field 'filled' is assigned inside a parallel loop: an iteration may write only its own element, such as
voices[v]`; `cannot assign to the counter 'v' of a parallel loop`.

**Rule B, reads.** Every load in the body is from a private, an owned lvalue, or a *shared* lvalue: one whose root no
iteration writes. For a stable base, "no iteration writes it" is per base: reading `coefficients[j]` is fine when the
loop writes only `voices[v]`. For a pointer without `restrict`, it is per type: the body may read through
`const float* in` only when the loop writes no float through any base but privates (because `in` could point at one),
and writing `voices[v]` writes floats. In practice every example's pointer parameters are `restrict` already, and
the error says to add it.

**Rule C, calls.** FMinus has no globals, no statics and no pointers to pointers, so **a function can write only through
its non-`const` pointer parameters and, a method, `this`**: its effects are read off its declaration. A call in the
body is allowed when

- every argument to a non-`const` pointer parameter is an owned pointer (`&voices[v]`, `voices[v].buffer`, an owned
  pointer local), and a method's object is an owned lvalue or a private (`voices[v].render(n)`; `filter.tick(x)` on a
  field of the enclosing `this` is an error: `method 'OnePole::tick' may write 'filter', which every iteration shares`);
- every argument to a `const` pointer parameter is readable by rule B (passing `in` is reading through `in`);
- the callee is a builtin whose writes are known: `poly_store(p, ...)` writes through `p`, so `p` is owned; the math
  builtins write nothing; `fft_forward(plan, in, out)` writes `out`, and the plan is read-only once the runtime is
  fixed (below). `malloc`, `calloc` and `free` are thread-safe system calls and stay out of nonblocking functions as
  they do now.
- the callee is defined in the program. An external function (a prototype without a definition) could write anything:
  an error at first. The extension: C23's `[[reproducible]]` (effectless and idempotent) and `[[unsequenced]]`
  (stateless and independent too) on the prototype would admit it under the same argument rules, which is what the
  standard made them for: C23's "effectless" allows a function's stores only to objects based on its pointer
  parameters, each reached through one pointer, which is rule A's shape with `restrict`'s promise (to read
  against the standard's own words in 6.7.13.8 before building on it; the summary here is cppreference's).

Knobs and streams need no rule. A method reads a knob once where it starts (an unordered atomic load, recipe 14), so
every iteration sees the one value, captured into the fork; a stream is read-only through the call; a frozen
parameter is a constant. A method called from the body on an owned module reads its own knobs at its own entry, as it
does now.

Two things the rules are not: they are not a general alias analysis, and they do not try to be complete. They admit
the shapes audio writes (an element a voice, a field a channel, a `restrict` buffer a task) and refuse the rest with a
message that names the fix. Where the checker is wrong it is wrong on the safe side: the loop does not compile. That
is the deal that keeps races out of the language instead of out of the careful programmer's code.

### Reductions, and what "the same result" means

A scalar the body combines into, and does nothing else with, is a reduction:

```c
float peak = 0;
[[fminus::parallel]] for (int v = 0; v < voices; ++v)
    peak = fmaxf(peak, bands[v].level());        // or: sum += ...; product *= ...; count += 1;
```

The recognised forms are `acc += e`, `acc -= e`, `acc *= e`, `acc = fminf(acc, e)` and `acc = fmaxf(acc, e)`, for an
`int`, a `float` or a float vector, where `e` does not read `acc` and nothing else in the body reads or writes `acc`.
Each chunk of iterations (below) combines into a private copy that starts at the operator's identity (`-0.0f` for a
sum, 1 for a product, `+inf` and `-inf` for `fminf` and `fmaxf`), and after the join the chunks' partials are combined
into `acc` in chunk order, by the calling thread. Several reductions in one loop are fine.

Floating-point addition rounds differently in each order, so the question is which order. The design fixes it:

- The iterations are cut into **chunks whose number depends on the trip count alone**, `chunks = min(count, P)` with
  `P` a constant of the design (64, say), each chunk `ceil(count / chunks)` iterations from a fixed start. Workers
  take chunks as they come free, but which worker runs a chunk changes nothing: the partials live in slots indexed by
  the chunk, not the thread.
- So the result is **the same at any thread count, with or without a pool, on every machine**, which is the promise
  threads most often break. `max` and `min` are exact in any order; so is an `int` sum.
- For a loop of **at most `P` iterations, every chunk is one iteration**, its partial is that iteration's contribution
  exactly (`-0 + x` is `x`, `1 * x` is `x`), and combining the chunks in order is the serial loop's own order: the
  result is **bit-identical to the loop without the attribute**. A loop of more than `P` iterations sums its chunks in
  order, which the serial loop does not, and differs in the last bits, as a `poly float`'s `reduce_add` differs from a
  scalar sum today ([simd.md](simd.md), "Results depend on the width"). Audio's parallel loops are over voices,
  channels and bands, so the exact case is the common one.

The alternative, for the user to weigh, is **no float reductions** in a parallel loop at all: the attribute is then
strictly ignorable, bit for bit, and a program that wants a sum writes each iteration's result to its own element and
sums them in a plain loop after, which is what a synth's mix does anyway (the example below), and what makes the mix's
order the same as before. The recommendation is the fixed-shape reduction, because it is more deterministic than
`poly` already is, and because refusing `sum += x` sends people to the clumsier shape for the few cases that are not a
mix. An "array reduction", `out[i] += voices[v].sample(i)` for every `i`, is the one audio most wants and the one
this version does not do (it needs a private copy of `out` a chunk, so a known size): the per-voice buffer and the
in-order mix are the idiom, and the error says so.

### Nested parallelism

A parallel loop inside an iteration of another (a voice whose `render` has one, over its partials) runs its
iterations in order on the worker that reached it. The runtime knows it is inside a job (a thread-local flag) and
runs the body inline; no pool slot is taken, nothing waits. The result is the same, by design, and the outer loop
already has the cores busy. A work-stealing runtime (Cilk's) would let the inner loop spread when the outer has fewer
iterations than cores; that is a later change to the runtime alone, since the language says nothing about it.

### What `nonblocking` means with threads

`fminus_rt_parallel_for` waits for its own iterations and nothing else: no lock, no allocation, and with a pool
whose workers are spinning (a block in progress) no system call. It is therefore allowed in a `[[clang::nonblocking]]`
function, as a transform is, and the body's calls are checked as the function's own are (they are: the body is lowered
inside the function's definition, so `calls_` records them). Two honest limits go in the documentation: when a pool's
workers have parked (the host has been idle), the first fork of the next block wakes them, which is a system call of a
few microseconds unless the host uses the resume/pause calls below; and a worker preempted mid-chunk holds the join
until it runs again, which is why the workers run at the audio thread's priority and why there is no fix for a host
that denies it. Clang's RealtimeSanitizer, if it is ever wired in (extending.md, "A run-time check"), would see the
wake as a system call; the resume/pause mode avoids it.

## How it fits the compiler

By stage, as architecture.md lays them out. The sizes are guesses from the recipes that resemble each part.

### Tokens, grammar, tree

- `Lexer.l`: nothing new for the attribute spelling (the keyword spellings add one rule before `{IDENTIFIER}`).
- `Parser.y`: `statement: attributes "for" ... | attributes block`, reusing the `attributes` nonterminal functions
  have; an attribute on any other statement is an error in the lowering, as an unknown attribute on a function is now.
  The grammar comment at the top gains the line.
- `Ast.h`: `ForStmt` and `BlockStmt` gain `std::vector<Attribute> attributes` (empty today), so every visit still
  compiles, and `dump` prints `(parallel-for ...)` for the test oracle. No new node: a parallel loop *is* a `for`.

### The checker

A pass over the body before it is lowered, in a file of its own, `src/ir/Parallel.cpp`, as `Effects.cpp` is for the
effects. It needs what the lowering has at that point: `scopes_` to resolve a name to its binding and type (a
`restrict` parameter is `Param::isRestrict`; a local's `DeclStmt::isRestrict` is kept today "and changes nothing",
so this is where it starts to mean something), `function_->module` for the fields of `this`, `functions_` for a
callee's parameter types, the builtin tables for the builtins' writes. It walks the statements and expressions with
`std::visit` and no catch-all, as every walk does, and classifies each lvalue (`AssignExpr` and `IncrementExpr`
targets, `poly_store`'s pointer, `&e`, a method call's object, each pointer argument) as private, owned, shared or
refused, reporting the first error of each kind at the operator, in clang's place. It is a syntactic analysis on
access paths: what it cannot classify it refuses.

Two things it depends on. It wants expression types for bases (`is this name a restrict pointer? a module field?`),
which the lowering computes as it lowers (there is no type checker yet: extending.md, "A type checker"); for the
bases the rules admit (names, `this` fields, `B[idx]`, member paths) the types follow from the bindings alone, so the
checker can compute them itself, and a type checker, when it comes, replaces that. And it wants to know which locals
the body writes to tell loop-invariant expressions: a first walk collects the write set, a second checks.

### Lowering: the fork and the body

`lowerFor` sees the attribute and lowers the loop as a fork-join instead of `for.cond`/`for.body`/`for.inc`:

1. **The body becomes a function**, `Synth::process.par0` (a name no FMinus function can have, as `fminus.main`
   is), with internal linkage, whose parameters are **the captured values**: every variable the body reads from the
   enclosing scope by value (a `float`, an `int`, a knob already in a register, `this`, a pointer), and every
   stable base by address (an array's or module's slot). A `restrict` pointer keeps `noalias` on its parameter,
   which matters: when the inliner later folds this function into its caller (below), it turns `noalias` parameters
   into `!alias.scope` metadata, as it does for a stream's pointer today (recipe 14), and the body's loops still
   vectorise across the buffer. The chunk's own loop, `for (i = begin; i < end; ++i)`, is lowered inside it with the
   builder redirected (`scopes_` pushed with the captures' slots, `loops_` with the iteration's end as `continue`'s
   target and no `break` target). A reduction gets a private slot, started at the identity, and stored to
   `partials[chunk]` at the end.
2. **A trampoline with the runtime's signature**, `void(const void* ctx, i64 begin, i64 end, i32 chunk)`, unpacks a
   context struct the forking function built on its stack (`{ ptr this, ptr out, float k, i32 n, ptr partials }`)
   and calls the body. It is `alwaysinline` into nothing (the runtime calls it through a pointer), but the body is
   `alwaysinline` into *it*, which is how the `noalias` parameters become scoped-alias metadata. (Passing the
   captures through a struct and loading them in the body would lose `restrict`.)
3. **The fork**: `call void @fminus_rt_parallel_for(ptr %ctx, i64 %count, ptr @Synth::process.par0.tramp, i32 %chunks)`
   through `callRuntime`, declared with what is true of it (`kRuntimeFacts`): it calls back into the module and
   reaches whatever `ctx` reaches, so it gets no memory attributes beyond `nounwind` (and *not* `nocallback`); the
   call is then also the barrier the enclosing function needs, since LLVM assumes it reads and writes what the
   captures point to. After the call, the reductions' partials are combined in chunk order, and the loop is done.
4. **The chunk loop's accesses carry `llvm.access.group`**, and the loop `llvm.loop.parallel_accesses`: LLVM's own
   way of being told that "no loop-carried memory dependence exist" between a loop's accesses (LangRef), which the
   checker has just proved. The loop vectoriser then skips its dependence analysis for the loop and can run the
   chunk's iterations in lanes where it would otherwise see a possible dependence, so the attribute also helps a
   loop LLVM could not vectorise by itself, with no pool at all. It is not an aliasing claim (two iterations'
   accesses may still alias, as the metadata's author points out), and every access of the loop must carry a group
   or LLVM ignores it: the body's calls to functions that are not inlined keep it from applying. (Nothing else in
   FMinus asserts independence to LLVM today; this is the one place a proof exists.)
5. **`llvm.loop.mustprogress`** goes on the chunk loop as recipe 19 puts it on any counted loop.

A parallel block lowers the same way with a function per task and `count = tasks`, each chunk one task.

LLVM has an outliner of its own for this, the OpenMP IRBuilder (`llvm/Frontend/OpenMP/OMPIRBuilder.h`, which flang
uses through MLIR): `createParallel` emits the region inline through callbacks and `finalize` cuts it out with
`CodeExtractor`. It hands the result to libomp's `__kmpc_fork_call`, so it would tie FMinus's runtime to libomp's ABI
and bring LLVM's shape of parallel region rather than this one's. The lowering above outlines in the code generator,
where it knows each capture and its `restrict`, and calls a runtime that stays free of LLVM.

What the other passes make of it. **Knob versioning** (recipe 16) versions a method on its knobs' loads at its entry;
the outlined body receives a knob as a plain parameter, so a branch on it inside the body is not versioned (the body
is inlined into the trampoline, not into the method). Carrying `!fminus.domain` over as a `range` attribute on the
parameter, and letting the pass version the body on it, closes that; it is the pass's first case of versioning on an
argument. **Zero-init elision** (recipe 18) runs per function and sees the body's privates as it sees any local.
**TBAA** (recipe 17) is unchanged: the body's loads and stores carry their tags. **Flush-to-zero** (recipe 20) wraps
entry points to set the mode on the calling thread; a worker is another thread, so the runtime carries the mode over
(below), and the body needs no wrapper. **Effects** (recipe 11): the fork is allowed in nonblocking functions, and the
body's calls are the function's.

### The runtime

A new source, `src/runtime/Threads.cpp`, and its functions in `Runtime.h` and `RuntimeSymbols.def`. Generated code
calls one of them; the rest are the host's.

```cpp
// What generated code calls: runs body(ctx, begin, end, chunk) for `chunks` chunks of [0, count), on the pool's
// workers and the calling thread, and returns when every chunk is done. In order on the calling thread when there is
// no pool, or when already inside a chunk (a nested parallel loop). Allocates nothing, takes no lock, and makes no
// system call while the pool's workers are awake.
void fminus_rt_parallel_for(const void* ctx, std::int64_t count,
                            void (*body)(const void* ctx, std::int64_t begin, std::int64_t end, std::int32_t chunk),
                            std::int32_t chunks) noexcept;

// The host's, off the audio thread: a pool of `workers` threads (0: one fewer than the machine's cores), shared by
// every program in the process and counted, so start and stop pair. join_workgroup hands the workers the host's
// os_workgroup_t on macOS (Core Audio's, through the device, or the one a plugin host provides) and does nothing
// elsewhere.
std::int32_t fminus_rt_threads_start(std::int32_t workers) noexcept;
void fminus_rt_threads_stop() noexcept;
void fminus_rt_threads_join_workgroup(void* workgroup) noexcept;

// Optional, around a block: resume wakes the workers to spin until pause, so no fork inside the block makes a system
// call; without them, workers spin for a while after each fork and then park.
void fminus_rt_threads_resume() noexcept;
void fminus_rt_threads_pause() noexcept;
```

How it works inside, and why each choice:

- **One pool a process** (a binary, since the runtime is linked into each plugin), shared by every program and every
  instance, counted by `start`/`stop`. Two instances of a plugin in one host each starting a pool of its own would
  put twice the cores' worth of real-time threads on the machine, which is the oversubscription DAWs warn plugin
  authors about. A fixed ring of job slots (64, say) lets several audio threads fork at once; a worker takes chunks
  from any job.
- **The calling thread works too.** It takes chunks like a worker, then waits for the rest by spinning: it is a
  real-time thread with nothing better to do, and the wait is bounded by the longest chunk.
- **Chunks, not iterations, are the unit**, handed out by an atomic counter per job; `chunks = min(count, P)` is
  fixed by the trip count, which is what makes a reduction's order fixed (above).
- **Workers wait two ways.** After a fork they spin on the job generation for a bounded time (a block's worth, a
  millisecond or two), so a block's second and third forks find them awake at the cost of a few atomic reads, then
  park on a semaphore (`ulock`/futex) until the next fork wakes them (the one system call, from the audio thread, at
  the first fork of a block). The host that calls `resume` and `pause` around its block moves that call to the block's
  edge, where it already makes others.
- **Priority follows the audio thread.** On macOS the right thing is an audio workgroup: the workers join the host's
  `os_workgroup_t`, so the scheduler treats them as one real-time unit with the audio thread and its deadline (JUCE
  exposes it to a plugin). Without one, the time-constraint policy at the audio thread's parameters. On Linux
  `SCHED_FIFO` at the caller's priority, if permitted (rtkit, PipeWire's `module-rt`), and silently not otherwise;
  on Windows MMCSS's "Pro Audio" class. Nothing in the language changes with any of this, which is why it belongs
  in the runtime.
- **The floating-point environment travels.** C11 says it outright (7.6): "The floating-point environment has
  thread storage duration", and a new thread starts with its creator's environment as it was when the thread was
  made, so a pool started before the host set flush-to-zero would never see it. The fork therefore reads the
  forking thread's control register (FPCR on arm64, MXCSR on x86-64), stores it in the job, and a worker sets its
  own to match before its first chunk of the job (18 ns when it differs, nothing when it does not). So a program
  compiled under `Denormals::Flush` computes in that mode on every thread, and under a host's rounding mode too, where
  it would otherwise be a silent difference between a worker's result and the audio thread's.
- **Stacks.** A body's privates are stack arrays, and a module's method may have a large one; a secondary thread's
  default stack is small on macOS (512 KB). Workers get 8 MB, the main thread's size.
- **Nothing allocates after `start`.** Job slots, chunk counters and partials' slots are in the pool or on the
  forking thread's stack; `start` touches it all.
- **No pool, no change.** Without `start`, or after `stop`, every parallel loop runs in order on the calling thread,
  and so do `fminus --run`, the tests and an executable, unless `--threads N` asks `addEntryPoint`'s `main` to start a
  pool before `fminus.main` and stop it after.
- **One fix the runtime needs first.** An FFT plan (`Fft.cpp`) holds three buffers of its own, `work`, `in` and
  `out`, so that a transform never allocates; two iterations transforming through one plan at once would race on
  them. PFFFT's setup itself "is read-only so it can safely be shared by multiple concurrent threads" (its header),
  and takes a null `work` to use the stack, "probably the best strategy for small FFTs, say for N < 16384". So a
  transform should take its scratch from the stack up to a bound, and from a per-thread arena the pool allocates at
  `start` above it, and keep the plan read-only once made; `kRuntimeFacts`'s description of a transform (reads its
  plan and input, writes its output) then stays true across threads.

### The host's side, and the public API

A plugin changes nothing to be correct and three lines to be fast: `fminus::threads::start()` in `prepareToPlay`
(counted, so several instances share the pool), `stop()` in `releaseResources`, and on macOS
`joinWorkgroup(workgroup)` when the host hands one over (`AudioProcessor::audioWorkgroupContextChanged` in JUCE).
`resume()`/`pause()` around `processBlock` are the optional refinement. `include/fminus/FMinus.h` gets these as free
functions in `fminus::threads`, with the comment block saying what each costs and which thread may call it, as the
header does for everything else; `Program` is untouched, since the pool is the process's. `examples/plugin/` would
use them, and `examples/voices/` becomes the measured example (below).

### JIT and ahead of time

Nothing changes in `jit/Jit.cpp`: the new runtime functions are defined by address like every other
(`RuntimeSymbols.def`), and an executable links them from `libfminus_runtime.a`. The outlined body is an internal
function of the module, compiled with it; the runtime calls it through the pointer the fork passes, so LLJIT has
materialised it before any worker can reach it. The runtime library gains a dependency on the platform's threads
(pthreads, `os_workgroup`, MMCSS), with no LLVM in it still.

## Numbers to expect, and the example to measure them on

A fork-join on warm workers costs cache-line transfers and a few atomics: a microsecond or so on an M-series or a
recent x86, more across sockets, to measure first (phase 1). Waking parked workers is dearer: the wake itself is a
system call of a microsecond or a few (Linux measurements of a futex wake and the woken thread's return run from 1
to 4 µs under `SCHED_FIFO`), but on a loaded machine the woken thread's latency is tens of microseconds (Bela's
developers measured 20 to 40 µs waking their audio thread, and spun instead), which is why the workers spin inside
a block and park between. So a parallel loop pays when the block's work is tens of microseconds and up, and the gain approaches the core count only
when each iteration is a few microseconds or more. For `examples/voices/`: eight voices at 64 samples a block, about
5 ns a voice-sample, is 2.5 µs of work, which no fork can speed up; 64 voices at 256 samples is 80 µs, which eight
cores could bring near 10 µs plus the fork. The example should say both, as the biquad's README says what inlining
did and did not buy.

The example: `voices.fm` with a `Voice::render(int n)` that writes the voice's block into a buffer of its own,
`float out[512];` a field (a buffer indexed by `v` from inside the module is owned by `voices[v]`), the voice loop
parallel, and the mix a plain loop in the old order, so the output is **bit-identical** to today's `voices.fm`, which
`bench.cpp` already checks against C++ and across block sizes. With `--voices 64 --block 256` it times the loop with
no pool, with a pool, and the old sample-major loop, and `--check` runs all three and compares. The plugin runs its two
channels in a parallel block.

```c
module Voice
{
    Osc osc;
    OnePole filter;
    Envelope envelope;
    float out[512];                              // this voice's block; the host keeps n at most 512

    void render(int n) [[clang::nonblocking]]
    {
        for (int i = 0; i < n; ++i)
            out[i] = envelope.tick() * filter.tick(osc.tick());
    }
};

    // Synth::process
    [[fminus::parallel]] for (int v = 0; v < 8; ++v)   // each iteration writes voices[v] and nothing else
        voices[v].render(n);
    for (int i = 0; i < n; ++i)                        // the mix, in the order the sample-major loop summed it
    {
        float mix = 0;
        for (int v = 0; v < 8; ++v)
            mix += voices[v].out[i];
        out[i] = gain[i] * mix;
    }
```

## Tests

- **lit** (`tests/lit/fminus/parallel.fm`): the IR shape at `-O0` (the outlined function and its `noalias`
  parameters, the trampoline, the `fminus_rt_parallel_for` call and its declaration, the partials' combination), at
  `-O2` (the body inlined into the trampoline, `!alias.scope` on the buffer's accesses, `llvm.loop.parallel_accesses`,
  a vectorised chunk loop where there is one); `--run` with and without `--threads`, the same output; every error's
  wording.
- **unit** (`tests/unit/ParallelTest.cpp`, on `Compile.h`): each ownership rule, accepted and refused: a private, an
  owned element and its fields, a `restrict` and a plain pointer, a reduction of each form, a method on an owned module
  and on a shared field, a counter assigned, a `break`, a nested parallel loop, a parallel block with and without
  overlap; the result of a reduction of more than `P` iterations, the same at 1, 3 and 8 workers.
- **library** (`tests/library/ThreadsTest.cpp`, under the `tsan` preset too): start, fork from two threads at once,
  stop; a program's output identical with no pool, one worker and many; the FP mode carried to workers (a program
  compiled with `Flush`, its result on every chunk); the FFT from several iterations through one plan.
- **the examples**: `example-voices --check` with its three loops; `fminus-plugin-check` with the parallel block.
- **smoke**: nothing; the toolchain is unchanged.

## In what order

1. **The runtime alone**: `Threads.cpp`, its tests in C++, measured (fork cost warm and cold, on the machines at hand);
   the FFT's scratch moved off the plan. No language change yet, so it ships on its own.
2. **The loop**: the attribute in the grammar and the tree, the checker without reductions, the outlining and the
   fork, the IR tests and the error tests, no pool in `fminus` yet (every loop runs inline). The language is complete
   and deterministic at this point; threads are a runtime detail it does not yet use.
3. **Threads reach programs**: `fminus::threads` in the public API, `--threads` for `--run` and executables, the FP
   environment, the voices example measured, the plugin's workgroup.
4. **Reductions, and `llvm.loop.parallel_accesses`.**
5. **The parallel block.**
6. Later, each a design of its own: knob versioning across the fork; a work-stealing runtime for nested loops;
   external functions through C23's `[[reproducible]]`; the strided index family; an array reduction with a known
   size; work across blocks (a stage rendered one block ahead on a worker, as REAPER's anticipative FX does, at a
   block of latency: an attribute on a method whose instance is lent to the pool until its next call); and background
   work that is not real time (a sample loaded, a spectrum analysed for a display), which is a different problem, the
   host's today through knobs and streams, and could become a method the pool runs at low priority with its result
   handed over as a knob is.

## For the user to decide

1. **The spelling**: `[[fminus::parallel]]`, `parallel`, or `_Parallel`. The note recommends the attribute for the C
   story; `parallel` reads better.
2. **Float reductions**: fixed-shape (deterministic, exact up to `P` iterations, otherwise `poly`-like) or none
   (strictly ignorable attribute, the two-loop idiom for sums). The note recommends fixed-shape; `P` is to choose.
3. **How strict rule B is about plain pointers.** The sound rule refuses a read through a non-`restrict` pointer
   whenever the loop writes anything of its type. It is right, and it will annoy someone once; the alternative, to
   trust that a pointer parameter does not alias a module's fields, is what C programmers assume and what a host
   could break.
4. **The pool's owner**: the process (recommended: shared by instances), or a `Program`, or the host's own threads
   through a callback the runtime calls to run a chunk (what a DAW with its own scheduler might want: an API for
   later, not instead).
5. **Nested loops in order** at first, or a work-stealing runtime from the start. In order is a page of runtime; work
   stealing is a project.
6. **Whether `main` programs start a pool by default** (`--threads` opt-in, recommended, so `--run` stays what it is),
   and whether `fminus --run` should take `--threads` for the lit tests.

## Sources

Read for this note. Some were reachable only as search summaries from this session's container, and are marked so.

- PFFFT's header, `pffft.h` (Julien Pommier): a setup "is read-only so it can safely be shared by multiple concurrent
  threads"; a null `work` uses the stack, "probably the best strategy for small FFTs, say for N < 16384".
  https://bitbucket.org/jpommier/pffft/raw/master/pffft.h
- C23 attributes on statements: cppreference's C statement pages show an optional attribute specifier sequence before
  `while`, `do` and `for` since C23 (https://cppreference.com/c/language/statements); the WG14 papers behind the
  grammar, N2739 (attributes before a primary block) and N2937 (primary and secondary blocks):
  https://www.open-std.org/jtc1/sc22/WG14/www/docs/n2739.pdf, https://www.open-std.org/JTC1/SC22/wg14/www/docs/n2937.pdf
  (summaries).
- C23 `[[reproducible]]` and `[[unsequenced]]`: WG14 N2956 (Gustedt), https://open-std.org/JTC1/SC22/WG14/www/docs/n2956.htm,
  and cppreference, https://cppreference.com/c/language/attributes/unsequenced (summaries).
- Cilk's `cilk_for` constraints (one control variable declared in the initialiser, a termination value and an
  increment that do not change between iterations, no assignment to the control variable in the body): Intel Cilk Plus
  reference, "cilk_for", and the OpenCilk Language Extension Specification,
  https://cilk.mit.edu/docs/OpenCilkLanguageExtensionSpecification.htm (summaries). Serial elision: Frigo, Leiserson
  and Randall, "The Implementation of the Cilk-5 Multithreaded Language", PLDI 1998; OpenCilk's glossary,
  https://www.opencilk.org/doc/reference/glossary/serial-projection.
- Tapir: Schardl, Moses and Leiserson, "Tapir: Embedding Fork-Join Parallelism into LLVM's Intermediate
  Representation", PPoPP 2017 (slides: https://llvm.org/devmtg/2017-02-04/tapir-llvm.pdf); the instructions in
  OpenCilk's `llvm/include/llvm/IR/Instructions.h` at tag `opencilk/v3.0`, and none in upstream's; OpenCilk's
  releases, https://api.github.com/repos/OpenCilk/opencilk-project/releases (3.0 on LLVM 19.1.7, 4.0-rc2 on 21.1.3).
- CPLEX: WG14 N1862, N1966, N2017 and N2170 (the technical specification's text; N2131 and N2138 quote its
  spellings), https://www9.open-std.org/jtc1/sc22/wg14/www/docs/n2170.pdf; its status on cppreference's
  "Experimental C features" page, https://cppreference.com/c/experimental (summaries).
- LLVM's OpenMP IRBuilder: `llvm/lib/Frontend/OpenMP/OMPIRBuilder.cpp` (`createParallel`, `finalize`, its
  `CodeExtractor`), and MLIR's `OpenMPToLLVMIRTranslation.cpp`, flang's path into it. `llvm.access.group` and
  `llvm.loop.parallel_accesses`: LangRef, https://llvm.org/docs/LangRef.html#llvm-loop-parallel-accesses-metadata;
  Michael Kruse on what the metadata does not say, https://lists.llvm.org/pipermail/llvm-dev/2020-May/141593.html.
- FAUST: the compiler options in its manual, https://faustdoc.grame.fr/manual/options/; Orlarey, Fober and Letz,
  "Adding Automatic Parallelization to Faust", LAC 2009; Letz, Orlarey and Fober, "Work Stealing Scheduler for
  Automatic Parallelization in Faust", LAC 2010, https://hal.archives-ouvertes.fr/hal-02158924 (its measurements as
  summaries).
- Apple: WWDC 2020 session 10224, "Meet Audio Workgroups", https://developer.apple.com/videos/play/wwdc2020/10224/;
  `os_workgroup_join`, https://developer.apple.com/documentation/os/os_workgroup_join; `AudioWorkIntervalCreate`;
  `kAudioDevicePropertyIOThreadOSWorkgroup`; "Adding parallel real-time threads to audio workgroups",
  https://developer.apple.com/documentation/audiotoolbox/adding-parallel-real-time-threads-to-audio-workgroups.
  JUCE: `juce_AudioWorkgroup.h` and `AudioProcessor::audioWorkgroupContextChanged`, added in JUCE 7.0.8.
- Real-time practice: Rowland and Renn-Giles, "Real-time 101", parts I and II, ADC 2019 (and farbot,
  https://github.com/hogliux/farbot); Bencina, "Real-time audio programming 101: time waits for nothing", 2011;
  Doumler, "Using locks in real-time audio processing, safely", 2020,
  https://timur.audio/using-locks-in-real-time-audio-processing-safely. Priorities: Windows MMCSS,
  https://learn.microsoft.com/en-us/windows/win32/procthread/multimedia-class-scheduler-service; Linux `SCHED_FIFO`
  through `pthread_setschedparam` (or rtkit); macOS `thread_policy_set` with `THREAD_TIME_CONSTRAINT_POLICY`.
  Wake-up latency: a futex wake measured on LKML, https://lkml.rescloud.iu.edu/2110.1/08298.html; Bela's forum,
  https://forum.bela.io/d/7275-underruns-without-fully-utilizing-cpu.
- Deterministic reductions: oneTBB's specification of `parallel_deterministic_reduce`,
  https://oneapi-spec.uxlfoundation.org/specifications/oneapi/latest/elements/onetbb/source/algorithms/functions/parallel_deterministic_reduce_func;
  Ahrens, Demmel and Nguyen, "Efficient Reproducible Floating Point Summation and BLAS", UCB/EECS-2015-229, on
  why the order matters and what reproducible does not mean (more accurate).
- Race freedom by construction: Bocchino et al., "A Type and Effect System for Deterministic Parallel Java",
  OOPSLA 2009; ISPC's user guide, https://ispc.github.io/ispc.html (`foreach`, data races within a gang); flang's
  notes on `do concurrent`, https://flang.llvm.org/docs/DoConcurrent.html; Ragan-Kelley et al., "Halide", PLDI
  2013; the Futhark book, https://futhark-book.readthedocs.io/en/latest/language.html; the Rust book, chapter 16;
  SOUL's overview, https://github.com/soul-lang/SOUL/blob/master/docs/SOUL_Overview.md; Thies, Karczmarek and
  Amarasinghe, "StreamIt", CC 2002; Lee and Messerschmitt, "Synchronous Data Flow", Proc. IEEE 1987.
- DAWs and Max: Ableton's "Multi-core CPU handling", https://help.ableton.com/hc/en-us/articles/209067649; REAPER's
  audio buffering preferences, https://www.cockos.com/wiki/index.php/Preferences_Audio_Buffering; `poly~`'s
  reference, https://docs.cycling74.com/reference/poly~/.
- The floating-point environment is per thread: C11 7.6 (cppreference, https://cppreference.com/c/numeric/fenv), and
  the kernels' per-thread FPU state (Linux x86 `struct fxregs_state`'s `mxcsr`, arm64 `user_fpsimd_state`'s `fpcr`).
- libm is reentrant: Linux man-pages, `sin(3)`, "MT-Safe".
