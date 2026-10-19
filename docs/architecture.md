# Architecture

How FMinus turns source text into machine code: every stage, the file that implements it, and what passes from one
stage to the next. [extending.md](extending.md) builds on this to show how to add to each stage.

## The pipeline

```
 source text
     │  frontend/Lexer.l      Flex, reentrant: characters -> tokens with locations
     ▼
 tokens
     │  frontend/Parser.y     Bison, LALR(1) C++ with variants: tokens -> syntax tree
     ▼
 ast::Program                 frontend/Ast.h: functions, statements, expressions (no LLVM in it)
     │  ir/                   ir::lower: syntax tree -> LLVM IR, reporting every semantic error
     ▼
 llvm::Module                 one function per FMinus function, C calling convention
     │  support/HostTarget    prepareModule (triple, data layout) + optimiseModule (LLVM's O0-O3 pipeline)
     ├──────────────────────────────┐
     ▼                              ▼
 jit/Jit (ORC LLJIT)            support/Aot
 lookup("f") -> address         addEntryPoint -> a C main that runs the program's main
 in this process                emitFile -> object or assembly
                                linkExecutable -> a native executable
```

Each arrow is one function call with a plain value in between, so each stage can be tested on its own and the
stages can be recombined freely. `tools/fminus/main.cpp` strings them together; reading it is the quickest way to
see the whole flow.

| Stage | Entry point | Input | Output | Errors |
|---|---|---|---|---|
| parse | `frontend::parse` (`Frontend.h`) | `std::string_view` | `std::optional<ast::Program>` | `Diagnostics`; stops at the first syntax error |
| lower | `ir::lower` (`CodeGen.h`) | `const ast::Program&`, `llvm::LLVMContext&` | `std::unique_ptr<llvm::Module>` (null on error) | `Diagnostics`; every error in one run |
| optimise | `prepareModule`, `optimiseModule` (`HostTarget.h`) | `llvm::Module&`, `llvm::TargetMachine&`, level | the module, rewritten | none (the module is valid by now) |
| JIT | `jit::Jit::create`, `add`, `lookup` (`Jit.h`) | module + its context | `llvm::orc::ExecutorAddr` | `llvm::Error` |
| AOT | `aot::addEntryPoint`, `emitFile`, `linkExecutable`, `emitExecutable` (`Aot.h`) | module, target machine | files | `llvm::Error` |

## Diagnostics and locations

`frontend/Diagnostics.h` holds what every stage reports into.

- `SourceLocation` is a line and a column, both counted from 1 (the column counts bytes).
- `Diagnostics` belongs to one compilation of one file. `error(location, message)` records an error. `hasErrors()`
  tells a stage whether to hand over a result. `format()` renders `file:line:column: error: message` lines, sorted
  into source order.
- The sort matters because the code generator reports in two passes (declarations, then bodies), so its errors
  arrive out of order. `errors()` keeps the order of reporting, for tests that care about it.

Errors are values, not exceptions. The parser stops at the first syntax error (Bison has no error recovery rules
yet), but the code generator carries on after each error with a placeholder value (below). So one run of `fminus`
lists everything wrong with a program that parses.

## Scanning: `frontend/Lexer.l`

Flex generates `Lexer.yy.cpp` and `Lexer.yy.hpp` into `build-*/src/generated/frontend/`. The tokens are C's.

- **Reentrant, prefixed, from memory.** `%option reentrant` keeps all state in a `yyscan_t` handle, so two
  compilations can run at once. `prefix="fminus"` renames Flex's API (`fminuslex_init_extra`, `fminus_scan_bytes`,
  ...), so it can't clash with another scanner in the same binary. `Frontend.cpp` scans a string with
  `fminus_scan_bytes`, never a file or stdin.
- **Locations.** `%option extra-type` gives every scanner a `ScannerState` (`Scanner.h`), which holds Bison's
  `location`. `YY_USER_ACTION` advances it by each match's length. The block at the top of the rules section calls
  `step()` at the start of every token, and newlines call `lines()`.
- **Comments.** `// ...` is skipped by one rule. A block comment switches to the exclusive start condition
  `BLOCK_COMMENT` (`%x`), whose rules skip text and count lines until `*/`. At end of file, it reports clang's
  "unterminated /* comment" at the location where the comment started.
- **Constants.** C's floating constants (`1.5`, `1.`, `.5`, `2e3`, `1.5e-3f`, hex floats like `0x1.8p1`) and integer
  constants (`2`) are all one `NUMBER` token. The token keeps its spelling, `f` suffix included, so the code generator
  can round it once, to the right type. `2f` is clang's "invalid suffix 'f' on integer constant", as in C.
- **Rules.** Flex takes the longest match, and between matches of equal length, the earlier rule. That makes
  `+=` beat `+`, and `/*` beat `/`. Keywords (`float`, `bool`, `if`, `poly`, ...) therefore come before `{IDENTIFIER}`:
  `float` is a keyword and `floaty` an identifier.
- **Errors.** An unexpected character throws `Parser::syntax_error`. The parser catches it and reports it through
  `Parser::error`, like any syntax error.

`Scanner.h` is the glue between the scanner and the parser:

- the scanner's function is declared under a name of its own, `nextToken`, and Flex defines it through `YY_DECL`;
- the `yylex` that the parser calls is an inline function that forwards to it.

The indirection exists because the prefixed scanner `#define`s `yylex` to `fminuslex` inside its own files. A
function actually named `yylex` would then be defined under one name and called under another.

## Parsing: `frontend/Parser.y`

Bison generates `Parser.tab.cpp`, `Parser.tab.hpp`, `location.hh` and the report `Parser.output`. The grammar, a
subset of C's, is written out as EBNF in the file's header comment.

- **C++ with variants.** `api.value.type variant` lets each symbol carry a real C++ type (`%token <std::string>
  IDENTIFIER`, `%nterm <ast::ExprPtr> expr`). `api.value.automove` moves `$1`, `$2`, ... into the action, which is
  what lets move-only types such as `std::unique_ptr` pass through. As a consequence, an action may use each `$n` only
  once. `api.token.constructor` gives the scanner `Parser::make_FLOAT(location)` and friends.
- **C's precedence.** The `%right`, `%left` and `%precedence` lines give the operators C's precedence and
  associativity, from the loosest at the top to the tightest at the bottom: assignment (right to left), `?:` (right
  to left), `||`, `&&`, equality, relational, additive, multiplicative (`*`, `/` and `%`), unary (`%prec UNARY`), and
  the swizzle's `.`. The small helpers in the `%code` block, `binary()`, `logical()` and `assign()`, keep each
  operator's rule to one line. The dangling `else` is resolved with `%precedence THEN` below `"else"`: an `else` closes
  the nearest `if`, as in C.
- **Assignment is an expression.** `expr "=" expr` (and `+=` ...) accepts any expression as its target, as C's
  grammar does in effect. The code generator rejects what isn't a variable with clang's "expression is not
  assignable". Array elements and struct fields can join the check later without touching the grammar.
- **Declarations.** `const float a = 1, b;` is a qualifier, a type and a list of declarators. A declarator
  (`Declarator`, defined in `%code requires`) doesn't know the declaration's type when it reduces, so the declaration
  rule makes one `DeclStmt` per declarator. That is also why a block item is a *list* of statements.
- **Functions** are a definition (`type name(params) { ... }`) or a prototype (`type name(params);`, in which
  parameter names may be omitted). Both build an `ast::Function`, with `isDefinition` telling them apart.
- **Locations.** `%locations` gives every symbol an `@n`. `at(@n)` turns Bison's `location` into a `SourceLocation`.
  Expressions take the location of their operator or first token, declarations that of their name (as clang points
  at the name), and functions that of their name.
- **Results.** The parser builds into a `ParseContext` (`ParseContext.h`), which holds the program and the
  `Diagnostics`. `Parser::error` (at the end of the file) reports syntax errors with `parse.error detailed`, as
  "syntax error, unexpected X, expecting Y or Z". The expected list appears only when it has at most four tokens.
  LAC (`parse.lac full`) keeps that list accurate.
- **Running it.** `frontend::parse` (`Frontend.cpp`) creates the scanner through an RAII `ScannerHandle` and runs
  `Parser::parse()`. It returns the program, or `std::nullopt` once anything has been reported.

Bison's own diagnostics run at build time: `-Wall -Wcounterexamples` (`FMINUS_BISON_FLAGS`). A conflict fails
nothing, but Bison prints it with an example input, and `Parser.output` lists every state.

## The syntax tree: `frontend/Ast.h`

The tree is plain data. It has no LLVM in it, so other passes (a type checker, a formatter, an interpreter) can work
on it without the code generator.

- **Nodes.** `Expr` and `Stmt` each pair a `SourceLocation` with a `std::variant` of node kinds:
  - expressions: `NumberExpr`, `VariableExpr`, `BoolExpr`, `UnaryExpr`, `BinaryExpr`, `LogicalExpr`, `CallExpr`,
    `AssignExpr`, `ConditionalExpr`, `ConstructExpr` (a vector literal), `SwizzleExpr`;
  - statements: `DeclStmt`, `ExprStmt`, `ReturnStmt`, `BlockStmt`, `EmptyStmt`, `IfStmt`, `WhileStmt`, `ForStmt`,
    `BreakStmt`, `ContinueStmt`.
- **Types.** `Type` is `float`, `bool`, the vectors `float2`, `float4`, `float8`, their lane masks `bool2`, `bool4`,
  `bool8`, and `poly float` and `poly bool`, whose width the target decides. Helpers answer the questions the rules
  ask: `lanes`, `isVector`, `isMask`, `isPoly`, `vectorOf`, `maskOf`, `arithmeticOf` ([simd.md](simd.md)).
- **Ownership.** Children are owned: `ExprPtr = std::unique_ptr<Expr>`, and a `BlockStmt` holds a
  `std::vector<Stmt>`. `Param`, `Function` (definition or prototype) and `Program` are aggregates of those.
- **Exhaustive walks.** Code that walks the tree uses `std::visit` with an `Overloaded{...}` set of lambdas and no
  catch-all. Enums (`UnaryOp`, `BinaryOp`, `AssignOp`, `Type`) are walked with `switch` and no `default:`. So adding a
  node kind or an enumerator breaks the build at every walk that doesn't handle it, through `std::visit`'s
  exhaustiveness and `-Wswitch` under `-Werror`. Keep it that way.
- **`ast::dump`** prints the tree as s-expressions, one statement per line (`fminus --emit ast`). It is the parser's
  test oracle.

## Code generation: `src/ir/`

`ir::lower(program, context, diagnostics, options)` (`CodeGen.h`) creates a module named after the file.
`options.polyLanes` is a `poly float`'s width, which the driver gets from the target machine (`polyLanes`, below). A
`Lowering` object then runs two passes over the program. `Lowering` is one class (`Lowering.h`, internal to `src/ir/`,
in `fminus::ir::detail`), but its members are defined across the directory by what they lower:

| File | Lowering's members for |
|---|---|
| `CodeGen.cpp` | `ir::lower()`, the constructor, and the two passes: where the parts combine |
| `Types.cpp` | FMinus's types as LLVM's (`typeOf`, `lanes`), the conversions (`convert`, `commonType`), errors and poison values |
| `Functions.cpp` | declarations and prototypes (`declare`), bodies and parameters (`define`), stack slots and scopes |
| `Statements.cpp` | blocks, declarations, `return`, `if`, `while`, `for`, `break` and `continue` |
| `Expressions.cpp` | the expression `std::visit` (`lower`), constants, operators, `&&` and `\|\|`, `?:`, assignment |
| `Calls.cpp` | the builtin tables, calls, reductions, `poly_lanes` and `poly_index`, and `ir::builtins()` |
| `Vectors.cpp` | vector literals in both forms, swizzles read and assigned, `?:` with a vector condition |

The header declares every member under the name of its file, so it doubles as the map. A new node goes in the file its
kind belongs to, and the compiler finds the rest: `lower`'s `std::visit` in `Expressions.cpp` and the statement visit in
`Statements.cpp` fail to compile until they handle it.

**Pass 1, declare.** Every function name gets one `llvm::Function`, with external linkage, its C name and the C
calling convention (`float` is `f32`). C++ therefore calls JIT'd code as `float(float, ...)`, and functions may call
each other in any order. The rules for a name's prototypes and definition are C's:

- a prototype and a definition must agree ("conflicting types for 'f'");
- a second definition is "redefinition of 'f'";
- a prototype of a builtin, as `<math.h>` has, declares nothing new, but a definition of a builtin is refused;
- a function with prototypes and no definition stays external, and is resolved when the code is linked. The JIT
  looks in the process, so `float atanf(float x);` calls libm's.

**Pass 2, define.** Each definition's body is lowered with an `llvm::IRBuilder<>`:

- **Variables are C's.** Every parameter and local gets a stack slot: an `alloca` at the top of the entry block
  (`allocate`), which is where mem2reg looks. Reads are loads, writes are stores, and a parameter's incoming value is
  stored to its slot (`x.addr`, as clang names it). So `fminus -O0` reads like `clang -O0`, and from `-O1` mem2reg
  promotes the slots back into SSA values. A declaration without an initialiser stores 0. C leaves such a variable
  indeterminate, and reading it undefined.
- **Scopes are C's.** `scopes_` is a stack of name-to-slot maps. Parameters and the body's own declarations share the
  outermost scope, so redeclaring a parameter is an error. Each `{ ... }` pushes a scope, in which an outer name may
  be shadowed. A name is looked up innermost first.
- **Assignment** (`lowerAssign`) checks its target: it must be a variable ("expression is not assignable") that isn't
  `const` ("cannot assign to variable 'k' with const-qualified type 'const float'"). It stores the value, and returns
  it as the expression's value, so `a = b = 0` works. A compound assignment loads, combines and stores.
- **Statements after a `return`** are accepted, as C accepts them. They still lower, so their errors are found, into
  a fresh block named `unreachable` that nothing branches to, and the optimiser deletes it. The terminator check is
  `hasTerminator()`, not `getTerminator()`; see extending.md's pitfalls.
- **The function's end.** If the last block can still be reached from the entry (`reachable`, a depth-first walk:
  having predecessors isn't enough, because dead code can branch), the function falls off its end. That is "non-void
  function 'g' does not return a value", an error as in C++. Otherwise the block gets an `unreachable` terminator.
- **Verification.** `llvm::verifyFunction` checks each function, and anything it rejects is reported as an internal
  error, because the IR is wrong rather than the program.

Expressions lower recursively. `lower(const ast::Expr&)` returns a `Value`: the IR and the expression's FMinus type.
The rules read the FMinus type, never the IR's, because two FMinus types can share an LLVM type (a `poly float` is
`<4 x float>` on NEON, as a `float4` is). `convert(value, type, location, use)` applies C's implicit conversions and
OpenCL C's (a scalar broadcasts to a vector), and reports clang's message for the place a value meets its type
(`Use`: initialising, assigning, returning, passing, a condition, a logical operand).

- **Constants** go through `llvm::APFloat::convertFromString`, after the `f` suffix is dropped, straight to IEEE
  single, so they are rounded once. Hex floats are exact. A constant too large for `float` is clang's "magnitude of
  floating-point constant too large for type 'float'".
- **Arithmetic** becomes `fneg`, `fadd`, `fsub`, `fmul`, `fdiv` and `frem` with no fast-math flags, so it is IEEE
  exact, as clang does with `-ffp-contract=off`. `frem` is `%`, with `fmodf`'s semantics; AArch64 has no instruction
  for it, so it becomes a call to `fmodf`.
- **Calls** first try the reductions (`reduce_add`, `reduce_min`, `reduce_max`) and `poly_lanes` and `poly_index`,
  then `kBuiltins`, a table from `<math.h>`'s float names to LLVM intrinsics (`fabsf` to `llvm.fabs`, `sinf` to
  `llvm.sin`, ...), declared with `Intrinsic::getOrInsertDeclaration` at the arguments' type, so `sinf` of a `float4`
  is `llvm.sin.v4f32`. Then they try the program's functions. Argument counts are checked with clang's "too many/too
  few arguments to function call".
- **Control flow** is blocks named as clang names them: `land.rhs` and `lor.rhs` for `&&` and `||`, `cond.true`,
  `cond.false` and `cond.end` for `?:`, `if.then`, `while.cond`, `for.inc`, ... A `?:` with a vector condition is a
  `select` instead, lane by lane.
- **On an error**, the lowering reports it, in clang's words, and returns `llvm::PoisonValue`. The IR stays
  well-formed, the next error is still found, and the module is thrown away at the end: `lower` returns null when
  `diagnostics.hasErrors()`.

## Optimisation: `support/HostTarget.h`

- **`hostTargetMachine(level)`** returns a `TargetMachine` for this CPU and its features
  (`JITTargetMachineBuilder::detectHost`). On Apple silicon, that means a CPU model such as `apple-m5`, whose features
  the model implies; `fminus-hostinfo --features` lists them. It also registers the host target the first time
  (`support/LLVMInit.h`).
- **`prepareModule(module, machine)`** sets the module's triple and data layout. Do it before optimising: the passes
  read both.
- **`optimiseModule(module, machine, level)`** runs `PassBuilder::buildPerModuleDefaultPipeline(level)`, which is
  clang's `-O<n>` pipeline, with loop and SLP vectorisation on. It passes the target machine, so the cost models know
  the host's vector width and instruction costs. On Apple systems it also gives the pipeline libsystem_m's vector
  math library, and runs `ReplaceWithVeclib` last, so `sinf` of a `float4` is one call to `_simd_sin_f4`
  ([simd.md](simd.md), recipe 8). This is the place to plug in FMinus's own passes (extending.md, recipe 6).
- **`polyLanes(machine)`** asks the machine's cost model how many floats its vector registers hold, the width LLVM's
  vectorisers use: a `poly float`'s lanes (4 on NEON).
- **`codeGenLevel(level)`** maps an optimisation level to the code generator's level.

## The JIT: `jit/Jit.h`

`jit::Jit` wraps ORC's `LLJIT`:

- **`create(level)`** builds an LLJIT for the host CPU, defines the FMinus runtime in it, and installs an IR
  transform layer that runs `optimiseModule` on each module before code generation. LLJIT optimises nothing by
  itself.
- **`add(module, context)`** sets the module up for the JIT's target and hands it over, with the `LLVMContext` it
  lives in, as a `ThreadSafeModule`. Nothing is compiled yet.
- **`lookup(name)`** compiles what the symbol needs, on the calling thread, and returns its address. The caller casts
  the address to a function pointer (`address.toPtr<float (*)(float)>()`).
- **Symbol resolution.** JIT'd code finds the FMinus runtime's functions by address (`defineRuntimeSymbols`), and
  process symbols through LLJIT's default process-symbol link order. These include libm's `sinf` behind the `sinf`
  builtin, and any external function a prototype declares (`float atanf(float x);`).
- **Lifetimes.** The `Jit` owns its `TargetMachine` and `LLJIT` (destroyed in reverse order), and the `LLJIT` owns
  every module added. Function pointers stay valid as long as the `Jit` lives.

Compilation is lazy: it runs the first time a symbol is looked up. In a plugin, look up on a background thread and
hand the pointer to the audio thread.

## Ahead of time: `support/Aot.h`

The same module can be written to disk instead, for development:

- **`emitFile(module, machine, path, Object | Assembly)`** runs the code generator (still the legacy pass manager's
  `addPassesToEmitFile`). The path `-` means stdout. Object writers seek, so a non-seekable stream is buffered.
- **`linkExecutable(options)`** runs the C++ compiler driver the build tree was configured with (`$FMINUS_LINK_DRIVER`
  overrides it), with the objects, this tree's `libfminus_runtime.a` and, under `asan` or `tsan`, the sanitizer
  runtimes.
- **`emitExecutable(module, machine, options)`** combines the two through a temporary object file.
- **`addEntryPoint(module)`** makes a program's `float main(float, ...)` or `bool main(float, ...)` the executable's
  entry. C's entry is `int main(int, char**)`, so it renames the program's `main` to `fminus.main` (internal, so the
  inliner can fold it) and adds a C `main` that reads the arguments through `fminus_rt_main_args`, calls it, and prints
  the result through `fminus_rt_main_result` (or `fminus_rt_main_bool_result`, as `true` or `false`). It runs before
  `prepareModule` and `optimiseModule`, so the entry is optimised with the rest. [programs.md](programs.md) shows the
  IR.

The `fminus` driver emits objects, assembly and executables (`--emit exe`, which adds the entry; `--entry` adds it to
the other outputs). `fminus-build` takes hand-written LLVM IR, which brings its own `main`, to an executable.

## The runtime: `src/runtime/`

The runtime is a separate static library, `libfminus_runtime.a` (`FMinus::Runtime`), of plain C functions that
generated code calls. It contains no LLVM.

- `Runtime.h` declares the functions as `extern "C"`: a placeholder ABI version, and the command line and result of
  an executable (`fminus_rt_main_args`, `fminus_rt_main_result`).
- `RuntimeSymbols.def` lists them, one `FMINUS_RUNTIME_SYMBOL(name)` each.
- `support/JitRuntime.cpp` expands that list into absolute symbols in a JIT (`defineRuntimeSymbols`).
- An executable gets the same functions by linking the archive.

Because the JIT and executables share one runtime, the two compile the same program to the same behaviour.

## Tools, examples, tests

| Target | Source | Uses |
|---|---|---|
| `fminus` | `tools/fminus/main.cpp` | parse, lower, then `--emit ast/ll/asm/obj/exe` or `--run` in the JIT |
| `fminus-build` | `tools/fminus-build/main.cpp` | LLVM IR to exe/obj/asm/ll (HostTarget + Aot) |
| `fminus-hostinfo` | `tools/fminus-hostinfo/main.cpp` | the triple, CPU, features and data layout the JIT targets |
| `example-biquad` | `examples/biquad/bench.cpp` | biquad.fm in the JIT against C++, compared and timed |
| `fminus-tests` | `tests/unit/`, `tests/smoke/` | Catch2 |
| lit suite | `tests/lit/` | `fminus`, `fminus-build`, FileCheck, LLVM's tools |

## Libraries and include roots

| Target | What | Include roots it gives |
|---|---|---|
| `fminus_runtime` (`FMinus::Runtime`) | `src/runtime/**` | none (generated code calls it by name) |
| `fminus_lib` (`FMinus::FMinus`, `libfminus.a`) | every other `.cpp` under `src/`, and the generated scanner and parser | `include/` (`"fminus/..."`), `<build>/include` (`fminus/Config.h`) |
| `FMinus::Internal` | `fminus_lib` for this tree's tools, examples and tests | also `src/` (`"frontend/Ast.h"`, ...) and `<build>/src/generated/` (`"frontend/Parser.tab.hpp"`) |
| `FMinus::LLVM` | LLVM's headers (as system headers) and the libraries of `FMINUS_LLVM_COMPONENTS` | LLVM's |

`include/fminus/` is meant for a public API, for a plugin to call once FMinus has one. Today everything goes through
`FMinus::Internal`.

## Invariants worth keeping

- **The syntax tree stays free of LLVM.** Lowering is the only place the two meet.
- **Walks stay exhaustive.** Never add a `default:` or a catch-all lambda to a walk over the tree: they turn "the
  compiler lists what to update" into silent bugs.
- **Lowering reports and continues.** Return a poison value of the expected type, never a null pointer, and let
  `lower` discard the module at the end.
- **Every variable lives in an entry-block `alloca`.** Allocate through `allocate()`, whatever block the builder is
  in, and let mem2reg do the SSA construction.
- **Every function is verified.** An invalid function is the compiler's bug: it is reported as an internal error
  instead of crashing in a later pass.
- **Modules are prepared before they are optimised.** The pipeline's decisions depend on the triple and the data
  layout.
- **Generated functions have the C calling convention, C names and external linkage.** That is the contract with
  C++ callers, the JIT's `lookup` and the linker. A new type at the boundary needs its C ABI attributes (extending.md,
  recipe 3: `bool` is `zeroext`; simd.md: `float8` goes through memory). The one exception is an executable's
  `main`, which `addEntryPoint` renames and makes internal.
- **Semantics follow C unless a deviation is documented.** The README lists the deviations: `float` is the only
  arithmetic type, constants are `float`, `%` works on floats, calls may precede definitions, uninitialised variables
  are 0, falling off a non-void function's end is an error, and `main` returns a `float` or a `bool` and takes floats.
- **The runtime stays free of LLVM and real-time safe.** It runs inside the audio thread's call tree.
