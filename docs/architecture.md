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
     │  ir/CodeGen.cpp        ir::lower: syntax tree -> LLVM IR, reporting every semantic error
     ▼
 llvm::Module                 one function per FMinus function, C calling convention
     │  support/HostTarget    prepareModule (triple, data layout) + optimiseModule (LLVM's O0-O3 pipeline)
     ├──────────────────────────────┐
     ▼                              ▼
 jit/Jit (ORC LLJIT)            support/Aot
 lookup("f") -> address         emitFile -> object or assembly
 in this process                linkExecutable -> a native executable
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
| AOT | `aot::emitFile`, `linkExecutable`, `emitExecutable` (`Aot.h`) | module, target machine | files | `llvm::Error` |

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

Flex generates `Lexer.yy.cpp` and `Lexer.yy.hpp` into `build-*/src/generated/frontend/`.

- **Reentrant, prefixed, from memory.** `%option reentrant` keeps all state in a `yyscan_t` handle, so two
  compilations can run at once. `prefix="fminus"` renames Flex's API (`fminuslex_init_extra`, `fminus_scan_bytes`,
  ...), so it can't clash with another scanner in the same binary. `Frontend.cpp` scans a string with
  `fminus_scan_bytes`, never a file or stdin.
- **Locations.** `%option extra-type` gives every scanner a `ScannerState` (`Scanner.h`), which holds Bison's
  `location`. `YY_USER_ACTION` advances it by each match's length. The block at the top of the rules section calls
  `step()` at the start of every token, and newlines call `lines()`.
- **Rules.** Flex takes the longest match, and between matches of equal length, the earlier rule. Keywords
  (`"fn"`, `"let"`, ...) therefore come before `{IDENTIFIER}`: `fn` is a keyword and `fnord` an identifier. A
  number's token keeps its spelling (a `std::string`), so the code generator can round it once, to the right type.
- **Errors.** An unexpected character throws `Parser::syntax_error`. The parser catches it and reports it through
  `Parser::error`, like any syntax error.

`Scanner.h` is the glue between the scanner and the parser:

- the scanner's function is declared under a name of its own, `nextToken`, and Flex defines it through `YY_DECL`;
- the `yylex` that the parser calls is an inline function that forwards to it.

The indirection exists because the prefixed scanner `#define`s `yylex` to `fminuslex` inside its own files. A
function actually named `yylex` would then be defined under one name and called under another.

## Parsing: `frontend/Parser.y`

Bison generates `Parser.tab.cpp`, `Parser.tab.hpp`, `location.hh` and the report `Parser.output`. Its header
comment holds the grammar in EBNF.

- **C++ with variants.** `api.value.type variant` lets each symbol carry a real C++ type (`%token <std::string>
  IDENTIFIER`, `%nterm <ast::ExprPtr> expr`). `api.value.automove` moves `$1`, `$2`, ... into the action, which is
  what lets move-only types such as `std::unique_ptr` pass through. As a consequence, an action may use each `$n` only
  once. `api.token.constructor` gives the scanner `Parser::make_FN(location)` and friends.
- **Locations.** `%locations` gives every symbol an `@n`. `at(@n)` turns Bison's `location` into a
  `SourceLocation`. Expressions take the location of their operator or first token, statements and functions that
  of their first keyword.
- **Precedence.** The `%left`, `%nonassoc` and `%precedence` lines resolve the expression grammar's ambiguity, from
  the loosest binding at the top to the tightest at the bottom. `%prec NEG` gives unary minus its own level.
- **Results.** The parser builds into a `ParseContext` (`ParseContext.h`), which holds the program and the
  `Diagnostics`. `Parser::error` (at the end of the file) reports syntax errors with `parse.error detailed`: "syntax
  error, unexpected X, expecting Y or Z". LAC (`parse.lac full`) keeps that expected list accurate.
- **Running it.** `frontend::parse` (`Frontend.cpp`) creates the scanner through an RAII `ScannerHandle` and runs
  `Parser::parse()`. It returns the program, or `std::nullopt` once anything has been reported.

Bison's own diagnostics run at build time: `-Wall -Wcounterexamples` (`FMINUS_BISON_FLAGS`). A conflict fails
nothing, but Bison prints it with an example input, and `Parser.output` lists every state.

## The syntax tree: `frontend/Ast.h`

The tree is plain data. It has no LLVM in it, so other passes (a type checker, a formatter, an interpreter) can work
on it without the code generator.

- **Nodes.** `Expr` and `Stmt` each pair a `SourceLocation` with a `std::variant` of node kinds (`NumberExpr`,
  `VariableExpr`, `UnaryExpr`, `BinaryExpr`, `CallExpr`; `LetStmt`, `ReturnStmt`). Children are owned:
  `ExprPtr = std::unique_ptr<Expr>`. `Function` and `Program` are aggregates of those.
- **Exhaustive walks.** Code that walks the tree uses `std::visit` with an `Overloaded{...}` set of lambdas and no
  catch-all. Enums (`BinaryOp`, `Type`) are walked with `switch` and no `default:`. So adding a node kind or an
  enumerator breaks the build at every walk that doesn't handle it, through `std::visit`'s exhaustiveness and
  `-Wswitch` under `-Werror`. Keep it that way.
- **`ast::dump`** prints the tree as s-expressions (`fminus --emit ast`). It is the parser's test oracle.

## Code generation: `ir/CodeGen.cpp`

`ir::lower(program, context, diagnostics)` creates a module named after the file. A `Lowering` object then runs two
passes over the program:

1. **Declare.** Every function gets an `llvm::Function` with external linkage, its FMinus name and the C calling
   convention (`float` is `f32`), so C++ calls the JIT'd code as `float(float, ...)` and functions may call each other
   in any order. A name already used by a builtin or another function is reported here.
2. **Define.** Each body is lowered with an `llvm::IRBuilder<>` into an `entry` block. Parameters and `let`s are
   bound in `scope_`, a name-to-`llvm::Value*` map. Since bindings never change, a binding is just the SSA value of
   its expression; no memory is involved. `return` emits `ret`. Afterwards `llvm::verifyFunction` checks the
   function, and anything it rejects is reported as an internal error, because the IR is wrong rather than the
   program.

Expressions lower recursively (`lower(const ast::Expr&)` returns an `llvm::Value*`):

- **Numbers** go through `llvm::APFloat::convertFromString` straight to IEEE single, so they are rounded once. A
  literal out of range for `float` is an error.
- **Arithmetic** becomes `fneg`, `fadd`, `fsub`, `fmul` and `fdiv` with no fast-math flags, so it is IEEE exact.
- **Calls** first try `kBuiltins`, a table from name to LLVM intrinsic (`abs` to `llvm.fabs`, `sin` to `llvm.sin`,
  ...), declared with `Intrinsic::getOrInsertDeclaration`, then the program's own functions. The argument count is
  checked against the callee.
- **On an error**, the lowering reports it and returns `llvm::PoisonValue` of the expected type. The IR stays
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
  the host's vector width and instruction costs. This is the place to plug in FMinus's own passes (extending.md,
  recipe 6).
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
  process symbols, such as libm's `sinf` behind the `sin` builtin, through LLJIT's default process-symbol link order.
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

The `fminus` driver emits objects and assembly. `fminus-build` takes LLVM IR all the way to an executable.

## The runtime: `src/runtime/`

The runtime is a separate static library, `libfminus_runtime.a` (`FMinus::Runtime`), of plain C functions that
generated code calls. It contains no LLVM.

- `Runtime.h` declares the functions as `extern "C"`.
- `RuntimeSymbols.def` lists them, one `FMINUS_RUNTIME_SYMBOL(name)` each.
- `support/JitRuntime.cpp` expands that list into absolute symbols in a JIT (`defineRuntimeSymbols`).
- An executable gets the same functions by linking the archive.

Because the JIT and executables share one runtime, the two compile the same program to the same behaviour.

## Tools, examples, tests

| Target | Source | Uses |
|---|---|---|
| `fminus` | `tools/fminus/main.cpp` | parse, lower, then `--emit ast/ll/asm/obj` or `--run` in the JIT |
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
- **Every function is verified.** An invalid function is the compiler's bug: it is reported as an internal error
  instead of crashing in a later pass.
- **Modules are prepared before they are optimised.** The pipeline's decisions depend on the triple and the data
  layout.
- **Generated functions have the C calling convention and external linkage.** That is the contract with C++
  callers, the JIT's `lookup` and the linker. A new type at the boundary needs its C ABI attributes (extending.md,
  recipe 3: `bool` is `zeroext`).
- **The runtime stays free of LLVM and real-time safe.** It runs inside the audio thread's call tree.
