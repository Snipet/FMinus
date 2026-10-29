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
     │  support/HostTarget    prepareModule (triple, data layout) + optimiseModule (LLVM's O0-O3 pipeline + FMinus's)
     ├──────────────────────────────┐
     ▼                              ▼
 jit/Jit (ORC LLJIT)            support/Aot
 lookup("f") -> address         addEntryPoint -> a C main that runs the program's main
 in this process                emitFile -> object or assembly
                                linkExecutable -> a native executable
```

Each arrow is one function call with a plain value in between, so each stage can be tested on its own and the
stages can be recombined freely. `tools/fminus/main.cpp` strings them together; reading it is the quickest way to
see the whole flow. For a host, the public API strings parse, lower and the JIT together behind one class,
`fminus::Program` (`include/fminus/FMinus.h`, implemented in `src/api/FMinus.cpp`; below).

| Stage | Entry point | Input | Output | Errors |
|---|---|---|---|---|
| parse | `frontend::parse` (`Frontend.h`) | `std::string_view`, and an `IncludeResolver` for its `#include`s | `std::optional<ast::Program>` | `Diagnostics`; stops at the first syntax error, and at an `#include` that finds no file |
| lower | `ir::lower` (`CodeGen.h`) | `const ast::Program&`, `llvm::LLVMContext&` | `std::unique_ptr<llvm::Module>` (null on error) | `Diagnostics`; every error in one run |
| optimise | `prepareModule`, `optimiseModule` (`HostTarget.h`) | `llvm::Module&`, `llvm::TargetMachine&`, level | the module, rewritten | none (the module is valid by now) |
| JIT | `jit::Jit::create`, `add`, `lookup` (`Jit.h`) | module + its context | `llvm::orc::ExecutorAddr` | `llvm::Error` |
| AOT | `aot::addEntryPoint`, `emitFile`, `linkExecutable`, `emitExecutable` (`Aot.h`) | module, target machine | files | `llvm::Error` |

## Diagnostics and locations

`frontend/Diagnostics.h` holds what every stage reports into.

- `SourceLocation` is a line and a column, both counted from 1 (the column counts bytes), and a file: an index into
  the compilation's files, 0 for the program's own.
- `Diagnostics` belongs to one compilation: the program's file, and the files its `#include`s read, each added by
  the scanner (`addFile(name, includedFrom)`) with the location of the `#include` that read it. `error(location,
  message, notes)` records an error, with the notes that explain it (clang's: `[[clang::nonblocking]]`'s errors have
  one for each call down to the `malloc`). `hasErrors()` tells a stage whether to hand over a result. `format()`
  renders `file:line:column: error: message` lines, sorted into source order, each followed by its
  `file:line:column: note: message` lines, wherever they point. A line in an included file comes after clang's include
  stack, `In file included from main.fm:2:` for each `#include` on the way to it, when the line before it was in
  another file.
- The sort matters because the code generator reports in two passes (declarations, then bodies), so its errors
  arrive out of order. Source order is the order the text is read in: an included file's lines sort where its
  `#include` is. `errors()` keeps the order of reporting, for tests that care about it.

Errors are values, not exceptions. The parser stops at the first syntax error (Bison has no error recovery rules
yet), but the code generator carries on after each error with a placeholder value (below). So one run of `fminus`
lists everything wrong with a program that parses.

## Scanning: `frontend/Lexer.l`

Flex generates `Lexer.yy.cpp` and `Lexer.yy.hpp` into `build-*/src/generated/frontend/`. The tokens are C's.

- **Reentrant, prefixed, from memory.** `%option reentrant` keeps all state in a `yyscan_t` handle, so two
  compilations can run at once. `prefix="fminus"` renames Flex's API (`fminuslex_init_extra`, `fminus_scan_bytes`,
  ...), so it can't clash with another scanner in the same binary. `Frontend.cpp` scans a string with
  `fminus_scan_bytes`, never a file or stdin, and so does an `#include`, whose file's text the resolver gives.
- **Locations.** `%option extra-type` gives every scanner a `ScannerState` (`Scanner.h`), which holds Bison's
  `location`. `YY_USER_ACTION` advances it by each match's length. The block at the top of the rules section calls
  `step()` at the start of every token, and newlines call `lines()`.
- **Comments.** `// ...` is skipped by one rule. A block comment switches to the exclusive start condition
  `BLOCK_COMMENT` (`%x`), whose rules skip text and count lines until `*/`. At end of file, it reports clang's
  "unterminated /* comment" at the location where the comment started.
- **Constants.** C's floating constants (`1.5`, `1.`, `.5`, `2e3`, `1.5e-3f`, hex floats like `0x1.8p1`) and integer
  constants (`2`, `010`, `0x1F`) are all one `NUMBER` token. The token keeps its spelling, `f` suffix included, and the
  code generator reads which it is from that: no point and no exponent make an integer constant, an `int`. A floating
  one is rounded once, to `float`. `2f` is clang's "invalid suffix 'f' on integer constant", as in C; in `0x1F`, the
  `F` is a hex digit.
- **Rules.** Flex takes the longest match, and between matches of equal length, the earlier rule. That makes
  `+=` and `++` beat `+`, `&&` beat `&`, and `/*` beat `/`, so `i+++j` is `i++ + j`, as in C. Keywords (`float`,
  `int`, `if`, `restrict`, ...) therefore come before `{IDENTIFIER}`: `float` is a keyword and `floaty` an
  identifier.
- **Errors.** An unexpected character throws `Parser::syntax_error`. The parser catches it and reports it through
  `Parser::error`, like any syntax error.
- **Directives.** A line whose first character after spaces is `#` is one rule, `^[ \t]*"#"[^\n]*`, which
  `directive()` (at the end of Lexer.l) reads by hand: `#include "file"` asks the front end's `IncludeResolver`
  (`Frontend.h`) for the file, `#pragma` is ignored, and anything else is an error. An included file is read from a
  buffer of its own (`yy_scan_bytes`, which switches to it); `ScannerState::open` keeps the buffer it interrupted and
  the location at the `#include`, and at the buffer's end `<<EOF>>` switches back, so the parser sees one stream of
  tokens. Each file is read once: `ScannerState::read` holds the names of the files read, the program's own first, and
  an `#include` of one of them reads nothing. Bison's positions carry the file (`api.filename.type` is
  `const SourceFile`, which holds its index in the `Diagnostics`; null is the program's own file), and the parser's
  `at()` turns a location into a `SourceLocation` with `sourceLocation` (`Scanner.h`). Recipe 21 has the details.

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
- **C's precedence.** The `%right`, `%left` and `%precedence` lines give the operators C's precedence and associativity,
  from the loosest at the top to the tightest at the bottom: assignment (right to left), `?:` (right to left), `||`,
  `&&`, equality, relational, additive, multiplicative (`*`, `/` and `%`), unary (`%prec UNARY`: `-`, `!`, `*p`, `&x`, a
  prefix `++`, a cast), and the postfix operators, `.` (a swizzle or a field), `->`, `[`, and `++` and `--` after their
  operand. `*` and `&` are binary or unary by where they stand, as in C's grammar: after an expression `*` is a product,
  at the start of one it is `*p`. The small helpers in the `%code` block, `binary()`, `logical()`, `assign()` and
  `increment()`, keep each operator's rule to one line. The dangling `else` is resolved with `%precedence THEN` below
  `"else"`: an `else` closes the nearest `if`, as in C.
- **Assignment is an expression.** `expr "=" expr` (and `+=`, `++` ...) accepts any expression as its target, as C's
  grammar does in effect. The code generator rejects what isn't an lvalue (a variable, an element `p[i]` or `*p`,
  components of either) with clang's "expression is not assignable". Struct fields can join the check later without
  touching the grammar.
- **Declarations.** `const float a = 1, *p;` is a qualifier, a type and a list of declarators. As in C, a declarator
  carries its own `*` and the qualifiers after it (`* const restrict`), so `float *p, x;` declares a pointer and a
  float. A declarator (`Declarator`, defined in `%code requires`) doesn't know the declaration's type when it
  reduces, so the declaration rule makes one `DeclStmt` per declarator, and `declared()` works out each one's type:
  with a `*`, the first `const` is the pointee's (`const float* p` points to read-only floats), and one after the `*`
  the pointer's own (`float* const p`). That is also why a block item is a *list* of statements.
- **Functions** are a definition (`type name(params) { ... }`) or a prototype (`type name(params);`, in which
  parameter names may be omitted). Both build an `ast::Function`, with `isDefinition` telling them apart. A result or
  a parameter may be a pointer, and `(void)` is C's empty parameter list: the `params` rule turns a single unnamed
  `void` parameter into none.
- **Locations.** `%locations` gives every symbol an `@n`. `at(@n)` turns Bison's `location` into a `SourceLocation`.
  Expressions take the location of their operator or first token, declarations that of their name (as clang points
  at the name), and functions that of their name.
- **Results.** The parser builds into a `ParseContext` (`ParseContext.h`), which holds the program and the
  `Diagnostics`. `Parser::error` (at the end of the file) reports syntax errors with `parse.error detailed`, as
  "syntax error, unexpected X, expecting Y or Z". The expected list appears only when it has at most four tokens.
  LAC (`parse.lac full`) keeps that list accurate.
- **Running it.** `frontend::parse` (`Frontend.cpp`) creates the scanner through an RAII `ScannerHandle` and runs
  `Parser::parse()`. It returns the program, or `std::nullopt` once anything has been reported. The handle also
  deletes the buffers of the `#include`s still being read when a syntax error stopped the parser.

Bison's own diagnostics run at build time: `-Wall -Wcounterexamples` (`FMINUS_BISON_FLAGS`). A conflict fails
nothing, but Bison prints it with an example input, and `Parser.output` lists every state.

## The syntax tree: `frontend/Ast.h`

The tree is plain data. It has no LLVM in it, so other passes (a type checker, a formatter, an interpreter) can work
on it without the code generator.

- **Nodes.** `Expr` and `Stmt` each pair a `SourceLocation` with a `std::variant` of node kinds:
  - expressions: `NumberExpr`, `VariableExpr`, `BoolExpr`, `UnaryExpr` (with `*p` and `&x`), `BinaryExpr`,
    `LogicalExpr`, `CallExpr`, `AssignExpr`, `IncrementExpr`, `CastExpr`, `IndexExpr` (`p[i]`), `ConditionalExpr`,
    `ConstructExpr` (a vector literal), `MemberExpr` (a swizzle, `v.xy`, or a module's field, `m.y` and `p->y`),
    `MethodCallExpr` (`m.tick(x)`), `ThisExpr`;
  - statements: `DeclStmt`, `ExprStmt`, `ReturnStmt`, `BlockStmt`, `EmptyStmt`, `IfStmt`, `WhileStmt`, `ForStmt`,
    `BreakStmt`, `ContinueStmt`.
- **Types.** A `Type` is a `Kind` and, for a pointer, what it points to and whether that is `const`; for an array,
  its elements and how many; for a module, which (a pointer to its `Module`). The kinds are `void`, `float`, `bool`,
  `int`, the vectors `float2`, `float4`, `float8`, their lane masks `bool2`, `bool4`, `bool8`, `poly float` and
  `poly bool`, whose width the target decides, `Pointer`, `Module` and `Array`. `Type::Float` and the rest name the
  types that are only a kind, and a `Kind` converts to its `Type`, so `type == Type::Float` reads as it says;
  `Type::pointerTo(Kind::Float, true)` is `const float*`, `Type::arrayOf(Kind::Float, 16)` is `float[16]`. Helpers
  answer the questions the rules ask: `lanes`, `isVector`, `isMask`, `isPoly`, `isPointer`, `isModule`, `isArray`,
  `pointee`, `samePointee`, `canPointTo`, `isArithmetic`, `vectorOf`, `maskOf`, `arithmeticOf` ([simd.md](simd.md)).
- **Modules.** A `Module` is a name, its `Field`s (a name and a type each; a parameter's, `param float cutoff;`, is
  marked, with its attributes) and its methods' names. Its methods are `Function`s of the program, named
  `OnePole::tick`, with `module` set and a first parameter, `this`, that the parser adds. The program holds its modules
  by `std::unique_ptr`, so a `Type` may point to one. A module's name is a type from its declaration on: the scanner
  returns it as `MODULE_NAME` (`ParseContext.h`), as a C compiler does a typedef's, so `OnePole* p;` is a declaration
  and `a * b;` a product.
- **Ownership.** Children are owned: `ExprPtr = std::unique_ptr<Expr>`, and a `BlockStmt` holds a
  `std::vector<Stmt>`. `Param`, `Function` (definition or prototype) and `Program` are aggregates of those.
- **Exhaustive walks.** Code that walks the tree uses `std::visit` with an `Overloaded{...}` set of lambdas and no
  catch-all. Enums (`UnaryOp`, `BinaryOp`, `AssignOp`, `IncrementOp`, `Kind`) are walked with `switch` and no
  `default:`; a switch over a type is a switch over `type.kind`. So adding a
  node kind or an enumerator breaks the build at every walk that doesn't handle it, through `std::visit`'s
  exhaustiveness and `-Wswitch` under `-Werror`. Keep it that way.
- **`ast::dump`** prints the tree as s-expressions, one statement per line (`fminus --emit ast`). It is the parser's
  test oracle.

## Code generation: `src/ir/`

`ir::lower(program, context, diagnostics, options)` (`CodeGen.h`) creates a module named after the file.
`options.polyLanes` is a `poly float`'s width, which the driver gets from the target machine (`polyLanes`, below), and
`options.parameters` the host's binding of each module parameter: a knob, a stream or a frozen constant. The rest are
the host's choices too: `math`, how the transcendental builtins compute; `strictAliasing`, whether loads and stores
carry TBAA tags; `knobVersions`, knob versioning's budget; `denormals`, subnormals kept or flushed to zero, and
`targetTriple`, which says what register an entry point sets then; `threads`, the most threads a parallel region runs
on. `ir::lower` writes two module flags,
`fminus.math` (the mode) and `fminus.knob-versions` (the budget), for the optimiser, where only the module arrives. A
`Lowering` object then lays the modules out, runs two passes over the program, and checks the functions' effects after
them; under `Denormals::Flush` it then marks every definition flushing and wraps the entry points (`flushDenormals`),
which needs every call known. `Lowering` is one class (`Lowering.h`, internal to `src/ir/`, in `fminus::ir::detail`),
but its members are defined across the directory by what they lower:

| File | Lowering's members for |
|---|---|
| `CodeGen.cpp` | `ir::lower()`, the constructor, and the passes: where the parts combine |
| `Types.cpp` | FMinus's types as LLVM's (`typeOf`, `lanes`), the conversions (`convert`, `convertPointer`, `commonType`), errors and poison values |
| `Functions.cpp` | declarations and prototypes (`declare`), libm's prototypes' attributes (`describeLibm`), bodies and parameters (`define`), stack slots and scopes (`find`, which captures a variable for a parallel region, and `lookup`, which does not) |
| `Statements.cpp` | blocks, declarations, `return`, `if`, `while`, `for`, `break` and `continue` (none of which leaves a parallel region); a loop's `llvm.loop.mustprogress` when its condition is not a constant expression (`isConstant`, `mustProgress`) |
| `Expressions.cpp` | the expression `std::visit` (`lower`), constants, operators (`binary`, `arithmetic`), `&&` and `\|\|`, `?:`, casts, assignment, `++` and `--` |
| `Calls.cpp` | the builtin tables, calls, reductions, `poly_lanes` and `poly_index`, `poly_load` and `poly_store`, the heap's `malloc`, `calloc` and `free`, the FFT's `fft_*` (`lowerFft`, on the runtime's), and `ir::builtins()`; the heap's and the FFT's runtime calls through `callRuntime`, whose declaration says what the function does (`kRuntimeFacts`, `describeRuntime`) |
| `Math.cpp` | `sinf`, `cosf`, `tanf`, `expf`, `tanhf` and `logf` in the vector and libm math modes: calls of the vector mode's math intrinsics (`fminus.sin.f32`, `fminus.sin.v2f32`, `fminus.sin.v4f32`) or the libm mode's runtime function, a float's or four lanes' (`lowerMath`), declared with their attributes and vector variant (`mathFunction`), and the names the runtime keeps for them, which a program that calls one may not declare (`checkMathNames`) |
| `Vectors.cpp` | vector literals in both forms, swizzles, `?:` with a vector condition |
| `Memory.cpp` | lvalues (`lvalue`, `read`, `write`): variables, elements (`p[i]`, `*p`), fields, components of any of them; arrays (a name that stands for a pointer, a list); `&x`; pointer arithmetic |
| `Modules.cpp` | each module's struct type (`layOut`), fields (`field`), `this`, method calls (`callMethod`), copies; parameters, read by their binding (`lowerParameter`, `atEntry`), a stream's null test, value, base and stride (a flat call, read with no branch; the stride's `opaqueUnit`), their domains for knob versioning (`tagDomain`), and their streams' alias scope (`scopeStreams`) |
| `Effects.cpp` | `[[clang::nonallocating]]` and `[[clang::nonblocking]]`: each function's calls (recorded by `lowerCall`), checked once every body is lowered |
| `Parallel.cpp` | `parallel for` and `parallel { }`: what would race refused (`Walker`, `checkParallelFor`, `checkParallelBlock`, and whether a method changes its module, `changesModule`), each region's body a function of the runtime's task type (`beginRegion`, `endRegion`), and what the body uses of the function around it captured into a context, by value or by reference (`capture`, `captured`, `capturedParameter`, `outerValue`) |
| `Tbaa.cpp` | C's strict aliasing, told to LLVM: FMinus's tree of four types and each access's tag (`accessTag`, `tag`), which every load and store calls |
| `Denormals.cpp` | `Denormals::Flush`: `DenormalFolder` (`Denormals.h`, the builder's constant folder, which flushes as the mode does), every definition marked `denormal_fpenv(preservesign)`, and each entry point wrapped to set the mode and call its body through an address LLVM cannot see into (`flushDenormals`, `wrapEntry`) |

The header declares every member under the name of its file, so it doubles as the map. A new node goes in the file its
kind belongs to, and the compiler finds the rest: `lower`'s `std::visit` in `Expressions.cpp` and the statement visit in
`Statements.cpp` fail to compile until they handle it.

**Before the passes, the modules.** `layOut` gives every module an LLVM struct type of its fields, in order, laid out
as a C struct (`%OnePole = type { float, float }`): opaque ones first, so a field may point to any module, then each
body, so a field may hold a module declared before it. A module's `sizeof` and alignment are the data layout's,
`options.dataLayout` (the driver's and the API's come from the target machine). A parameter's element is its slot,
`{ ptr, float }` (or `i32`, or a `bool`'s `i8`): the stream's pointer and the value, the same under every binding, so
one instance runs under programs that bind it differently. A method reads a knob's value, or a stream's pointer, once,
in its entry block, so both are registers through its loops (extending.md, recipe 14). With a stream's pointer it
reads the slot's value and whether the pointer is null, a flat call, and picks where the samples are, a base and a
stride: the pointer and 1, or, flat, the slot's value field and 0. `cutoff[i]` loads from `base + i * stride` and
selects the value when the stream is flat, with no branch, so a loop over a stream vectorises even where knob
versioning makes it no copy: LLVM's loop vectoriser checks that the stride is 1, and a flat call runs the scalar loop,
a load and a select a sample. The 1 is an empty inline asm's result (`opaqueUnit`), so that LLVM keeps the
multiplication: given a stride it can see is 0 or 1, InstCombine makes `i * stride` a select, and the loop stays
scalar. A bool knob's read, an int knob's when its `fminus::range` has integer ends and holds at most 8 values, and the
stream's test carry the values they can take (`!fminus.domain`), for knob versioning (recipe 16), which makes the stride
a constant again in each copy. Any other int knob is left untagged, and no method is versioned on it.

**Pass 1, declare.** Every function name gets one `llvm::Function`, with external linkage, its C name and the C
calling convention (`float` is `float`, `int` is `i32`, a pointer is `ptr`, a `void` result is `void`). C++ therefore
calls JIT'd code as `float(float, ...)` or `void(const float*, float*, int)`, and functions may call each other in any
order. The attributes are clang's: a `bool` is `zeroext`, and a `restrict` pointer `noalias`. The rules for a name's
prototypes and definition are C's:

- a prototype and a definition must agree ("conflicting types for 'f'");
- a second definition is "redefinition of 'f'";
- a prototype of a builtin, as `<math.h>` has, declares nothing new, but a definition of a builtin is refused;
- a function with prototypes and no definition stays external, and is resolved when the code is linked. The JIT
  looks in the process, so `float atanf(float x);` calls libm's. On Darwin, whose libm sets no `errno`, such a
  prototype of a math function of libm's gets LLVM's attributes for it without `errno` (`memory(none)` for `atanf`),
  after pass 2 (`describeLibm`; extending.md, recipe 19).

**Pass 2, define.** Each definition's body is lowered with an `llvm::IRBuilder<DenormalFolder>`, which folds constants
as `ConstantFolder` does, and under `Denormals::Flush` as flushing hardware computes them:

- **Variables are C's.** Every parameter and local gets a stack slot: an `alloca` at the top of the entry block
  (`allocate`), which is where mem2reg looks. Reads are loads, writes are stores, and a parameter's incoming value is
  stored to its slot (`x.addr`, as clang names it). So `fminus -O0` reads like `clang -O0`, and from `-O1` mem2reg
  promotes the slots back into SSA values. A declaration without an initialiser stores 0 (a pointer, null). C leaves
  such a variable indeterminate, and reading it undefined.
- **Scopes are C's.** `scopes_` is a stack of name-to-slot maps. Parameters and the body's own declarations share the
  outermost scope, so redeclaring a parameter is an error. Each `{ ... }` pushes a scope, in which an outer name may
  be shadowed. A name is looked up innermost first.
- **Lvalues** (`Memory.cpp`) are what a program reads, assigns and takes the address of: a variable (its slot), an
  element a pointer reaches (`p[i]`, a `getelementptr inbounds` on the index sign-extended to 64 bits, as clang's;
  `*p`, the pointer itself), a module's field (`m.y`, `p->y`, or `y` in a method: a struct GEP from the module's
  address), or components of any of them (`v.x`, `p[i].zw`). `lvalue()` finds one, `read()` loads it and `write()`
  stores to it (for components: load the vector, insert the lanes, store it back). A variable's value is a `read`, and
  so is `p[i]`'s; so a new kind of lvalue joins every use at once. Two kinds are never loaded whole: an array's `read`
  is a pointer to its first element (C's decay), and a module's is its address, which `write()` and a declaration copy
  with a memcpy, as clang copies a struct. Every load and store of a value carries its type's TBAA tag (`Tbaa.cpp`);
  the copies and a declaration's zeroing, bytes of any type, carry none, nor do the stores that start a parameter's or
  a declaration's slot, which mem2reg removes.
- **Assignment** (`lowerAssign`) checks its target: it must be an lvalue ("expression is not assignable") that isn't
  `const` ("cannot assign to variable 'k' with const-qualified type 'const float'"; through a `const float*`, "read-only
  variable is not assignable"). It stores the value, and returns it as the expression's value, so `a = b = 0` works. A
  compound assignment reads, combines and writes, and `++` and `--` are `+= 1` and `-= 1`.
- **Statements after a `return`** are accepted, as C accepts them. They still lower, so their errors are found, into
  a fresh block named `unreachable` that nothing branches to, and the optimiser deletes it. The terminator check is
  `hasTerminator()`, not `getTerminator()`; see extending.md's pitfalls.
- **The function's end.** A `void` function that reaches its end returns there (`ret void`), as `return;` does. In any
  other, if the last block can still be reached from the entry (`reachable`, a depth-first walk: having predecessors
  isn't enough, because dead code can branch), the function falls off its end. That is "non-void function 'g' does
  not return a value", an error as in C++. Otherwise the block gets an `unreachable` terminator.
- **Verification.** `llvm::verifyFunction` checks each function, and anything it rejects is reported as an internal
  error, because the IR is wrong rather than the program.

Expressions lower recursively. `lower(const ast::Expr&)` returns a `Value`: the IR and the expression's FMinus type.
The rules read the FMinus type, never the IR's, because two FMinus types can share an LLVM type (a `poly float` is
`<4 x float>` on NEON, as a `float4` is). `convert(value, type, location, use)` applies C's implicit conversions and
OpenCL C's (a scalar broadcasts to a vector), and reports clang's message for the place a value meets its type
(`Use`: initialising, assigning, returning, passing, a condition, a logical operand, a cast). Between scalars they are
C's: `sitofp` and `fptosi` between `int` and `float`, `zext` from `bool`, `!= 0` to `bool`. A pointer converts only
to `bool` and to the same pointer with `const` added (`convertPointer`).

- **Constants**: an integer constant is an `i32`, read in its base (`010` is octal) and refused when it doesn't fit
  in an `int`. A floating one goes through `llvm::APFloat::convertFromString`, after the `f` suffix is dropped,
  straight to IEEE single, so it is rounded once. Hex floats are exact. A constant too large for `float` is clang's
  "magnitude of floating-point constant too large for type 'float'".
- **Arithmetic** (`binary`, then `arithmetic`) works in the type C's usual arithmetic conversions give the operands
  (`commonType`): `int` when both are integers (a `bool` computes as an `int`), `float` when either is a float, a
  vector when either is one. On `float`s it is `fneg`, `fadd`, `fsub`, `fmul`, `fdiv` and `frem` with no fast-math
  flags, so it is IEEE exact, as clang does with `-ffp-contract=off`. `frem` is `%`, with `fmodf`'s semantics; AArch64
  has no instruction for it, so it becomes a call to `fmodf`. On `int`s it is clang's: `add nsw`, `sub nsw`, `mul
  nsw` (signed overflow is undefined, which lets LLVM widen a loop counter), `sdiv` and `srem`, and signed `icmp`s.
  With a pointer, it is pointer arithmetic (`pointerArithmetic`): `p + i` is a `getelementptr`, `p - q` LLVM's
  `CreatePtrDiff` (the addresses' difference divided by the element's size), and comparisons are unsigned `icmp`s of
  the addresses.
- **Calls** first try the reductions (`reduce_add`, `reduce_min`, `reduce_max`), `poly_lanes` and `poly_index`, and
  `poly_load` and `poly_store` (vector loads and stores at a float's alignment, masked with a `poly bool`), then
  `kBuiltins`, a table from `<math.h>`'s float names to LLVM intrinsics (`fabsf` to `llvm.fabs`, `sinf` to
  `llvm.sin`, ...), declared with `Intrinsic::getOrInsertDeclaration` at the arguments' type, so `sinf` of a `float4`
  is `llvm.sin.v4f32`. That is the fast math mode's lowering, and the exact builtins' (`fabsf`, `sqrtf`, `floorf`,
  `fminf`, `fmaxf`) in every mode; in the vector and libm modes `sinf`, `cosf`, `tanf`, `expf`, `tanhf` and `logf` are
  calls of functions LLVM does not know (`Math.cpp`), which it can neither fold nor pair, so a float gets one result
  wherever it is computed (extending.md, recipe 15): the vector mode's math intrinsics, which become FMinus's kernels
  at the end of the pipeline (below), or the libm mode's. Then `kAllocations`, C's `malloc`, `calloc` and `free`,
  which call the runtime's functions (below) with 64-bit sizes and give back a `void*`. Then they try the program's
  functions. Argument counts are checked with clang's "too many/too few arguments to function call". Every call of the
  heap or of a program function is recorded for the effects' check. The heap's and the FFT's runtime functions are
  declared through `callRuntime`, with the attributes that say what each does (an allocator of zeroed memory, a
  transform that reads one buffer and writes another; recipe 19), unless the program declares a function of that name
  itself. The math builtins' names are kept: a program that calls `sinf` may not declare a function named
  `fminus_rt_sinf`, `fminus_rt_sin_f4` or `fminus_rt_libm_sinf` (each builtin has its three), in any math mode, the fast
  one's too, so that whether a program compiles does not depend on the mode. `checkMathNames` reports it at the call,
  with a note at the declaration, so no function of the program gets a runtime function's attributes or becomes a vector
  variant.
- **Effects** (`Effects.cpp`) run after every body: a function marked `[[clang::nonallocating]]` or
  `[[clang::nonblocking]]` (C23's syntax, after its parameters) is checked call by call, as clang's
  `-Wfunction-effects` checks it, down through the program's unmarked functions; a call that could reach the heap
  (or, nonblocking, an unmarked external function) is an error, with a note for each step down.
- **Parallel regions** (`Parallel.cpp`) are checked, then lowered into a function of their own each,
  `<function>.parallel.<n>`, `void(ptr context, i32 begin, i32 end)`, which runs a loop's iterations or a block's
  statements from `begin` to `end - 1`. Where the statement was, the function fills the context, a frame of its own,
  with what the body found of it (a variable it only reads, copied; one it changes, an array or a module, its address;
  what a method read of its parameters where it started), and calls `fminus_rt_parallel_for` (extending.md, recipe
  22).
- **Control flow** is blocks named as clang names them: `land.rhs` and `lor.rhs` for `&&` and `||`, `cond.true`,
  `cond.false` and `cond.end` for `?:`, `if.then`, `while.cond`, `for.inc`, ... A `?:` with a vector condition is a
  `select` instead, lane by lane. A loop whose condition is not a constant expression carries clang's
  `llvm.loop.mustprogress` on its branches back, as C11 allows (recipe 19).
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
  the host's vector width and instruction costs. SLP's cost threshold is 10 rather than LLVM's 0 (LLVM's
  `-slp-threshold`, set once for the process unless the host set it), so that it packs independent recurrences, a
  stereo filter's channels, into one vector only where that pays, a math builtin's two-lane kernel ([simd.md](simd.md),
  "The auto-vectorisers"). It is also
  where FMinus's own passes (`src/opt/`) join the pipeline,
  through its extension points, each in a block of its own:
  - **Knob versioning** (`KnobVersioningPass`, extending.md recipe 16), at the early simplification point
    (`registerPipelineEarlySimplificationEPCallback`), from `-O1`: after the first SimplifyCFG, SROA and EarlyCSE,
    before IPSCCP and the inliner, so each method is versioned on its own knobs. Its budget is the module flag
    `fminus.knob-versions` (8 when there is none), spent on the streams' tests first. Each copy gets locals of its
    own, their lifetimes marked, so the frame does not grow with the copies. A function it has versioned carries the
    attribute `fminus-knob-versions`, and the pass leaves it alone when it runs again (`fminus-build -O3` on FMinus's
    optimised IR, say).
  - **Zero-init elision** (`ZeroInitElisionPass`, recipe 18), at the end of each function's simplification
    (`registerScalarOptimizerLateEPCallback`): after the inliner and the loop passes, before the loop vectoriser. From
    `-O1`: LLVM's `-O0` pipeline calls this point's callback too, and the pass stays out of it, so `fminus -O0` shows
    every memset the lowering wrote.
  - **The vector math library**, only for a module whose flag `fminus.math` is `"fast"`, or that has none (IR written
    by hand): on Apple systems, libsystem_m's table in the target library analysis, and `ReplaceWithVeclib` last, so
    `sinf` of a `float4` is one call to `_simd_sin_f4` ([simd.md](simd.md), recipe 8). The vector and libm modes'
    calls are of functions neither knows.
  - **The math kernels** (`MathKernelsPass`, `src/opt/MathKernels.h`, extending.md recipe 24), last
    (`registerOptimizerLastEPCallback`), after the vectorisers, at every level: each call of a math intrinsic
    (`fminus.sin.f32`, `.v2f32`, `.v4f32`, the vector mode's) becomes FMinus's kernel, inline, built from the template
    the runtime's functions instantiate (`runtime/MathKernels.h`), from `-O1`; at `-O0` a call of the runtime's
    function. Then EarlyCSE, InstSimplify and SimplifyCFG tidy the functions it changed: sine and cosine of one angle
    share their reduction, and a constant's kernel folds, flushing as the function does (`DenormalFolder`). An
    argument of sin, cos or tan past 2^16 takes an unlikely branch to the runtime's function, through a
    `preserve_allcc` wrapper (`fminus.large.sin`) so that the loop around it spills nothing.

  The flags travel with the module, so the JIT, `fminus` and executables get them without a new argument. Recipe 6
  adds a pass the same way.
- **`polyLanes(machine)`** asks the machine's cost model how many floats its vector registers hold, the width LLVM's
  vectorisers use: a `poly float`'s lanes (4 on NEON).
- **`codeGenLevel(level)`** maps an optimisation level to the code generator's level.

## The JIT: `jit/Jit.h`

`jit::Jit` wraps ORC's `LLJIT`:

- **`create(level, modules)`** builds an LLJIT for the host CPU, defines the FMinus runtime in it, and installs an IR
  transform layer that runs `optimiseModule` on each module before code generation. LLJIT optimises nothing by
  itself. With `Modules::Optimised` it installs none, and generates code from each module as it comes: one optimised
  already, at the JIT's level (below, "The IR as text").
- **`add(module, context)`** sets the module up for the JIT's target and hands it over, with the `LLVMContext` it
  lives in, as a `ThreadSafeModule`. Nothing is compiled yet.
- **`lookup(name)`** compiles what the symbol needs, on the calling thread, and returns its address. The caller casts
  the address to a function pointer (`address.toPtr<float (*)(float)>()`).
- **Symbol resolution.** JIT'd code finds the FMinus runtime's functions by address (`defineRuntimeSymbols`), the
  math builtins' among them in the vector and libm modes, and process symbols through LLJIT's default process-symbol
  link order. These include libm's `sinf` and libsystem_m's `_simd_sin_f4` behind the `sinf` builtin in the fast mode,
  and any external function a prototype declares (`float atanf(float x);`).
- **Lifetimes.** The `Jit` owns its `TargetMachine` and `LLJIT` (destroyed in reverse order), and the `LLJIT` owns
  every module added. Function pointers stay valid as long as the `Jit` lives.

Compilation is lazy: it runs the first time a symbol is looked up. In a plugin, look up on a background thread and
hand the pointer to the audio thread.

**The IR as text** (`Program::Options::keepIr`). A host that shows what FMinus made of a program asks `Program::compile`
to keep its IR, printed as LLVM prints a module: as `ir::lower` wrote it, after `prepareModule` (`loweredIr()`), and
optimised (`optimisedIr()`). To print the optimised module, `compile` makes the calls `fminus --emit ll` makes,
`prepareModule` and `optimiseModule`, on a machine made as the JIT's is, and creates its `Jit` with
`Modules::Optimised`: the text is the module the JIT generates code from, and the same as `fminus --emit ll` prints for
the same source, options and file name (`tests/unit/KeepIrTest.cpp` runs the driver and compares). The optimiser then
runs in `compile`, and the first look-up runs only the code generator. Off, the default, nothing is printed and the
module goes to the JIT unoptimised, as before. The lowered IR is not what `fminus -O0` prints: `-O0`'s pipeline still
makes the vector math mode's intrinsics (`fminus.exp.f32`) calls of the runtime's functions (`fminus_rt_expf`), and in
the fast mode, on Apple systems, a `float4`'s `llvm.sin.v4f32` a call of the vector library's `_simd_sin_f4`.

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
  the result through `fminus_rt_main_result` (or `fminus_rt_main_bool_result`, as `true` or `false`); a program with a
  parallel region gets the runtime's pool around the call (`fminus_rt_parallel_start`, `_stop`). It runs before
  `prepareModule` and `optimiseModule`, so the entry is optimised with the rest. [programs.md](programs.md) shows the
  IR.

The `fminus` driver emits objects, assembly and executables (`--emit exe`, which adds the entry; `--entry` adds it to
the other outputs). `fminus-build` takes hand-written LLVM IR, which brings its own `main`, to an executable.

## The runtime: `src/runtime/`

The runtime is a separate static library, `libfminus_runtime.a` (`FMinus::Runtime`), of plain C functions that
generated code calls. It contains no LLVM, and it holds the FFT: PFFFT's objects (`cmake/FMinusFft.cmake`) are in it.

- `Runtime.h` declares the functions as `extern "C"`: a placeholder ABI version, the command line and result of an
  executable (`fminus_rt_main_args`, `fminus_rt_main_result`), and the heap that `malloc`, `calloc` and `free` call
  (`fminus_rt_malloc`, `fminus_rt_calloc`, `fminus_rt_free`: 64-byte aligned, zeroed, on C's `aligned_alloc`, or on
  Windows, which has none, `_aligned_malloc`), the FFT behind the `fft_*` builtins (`fminus_rt_fft_create`, ...,
  `Fft.cpp`: a plan holds PFFFT's setup and buffers of its own, so a transform never allocates), and the math
  builtins' functions (`Math.cpp`). Those are the vector mode's `fminus_rt_sinf` ... `fminus_rt_logf` and their four
  lanes, `fminus_rt_sin_f4` ... `fminus_rt_log_f4`, whose lanes are each the scalar function of that lane: FMinus's
  own kernels (`MathKernels.h`, extending.md recipe 23), one sequence of IEEE operations each, written once as a
  template that the scalar and the four-lane function both instantiate and `MathKernelsPass` builds as IR, the same on
  every target (a program calls the functions at `-O0`, and for sin, cos or tan past 2^16); and the libm mode's
  `fminus_rt_libm_sinf` and its kin. `fminus::math` in the public header calls the vector mode's. And the
  pool behind `parallel for` and `parallel { }` (`Parallel.cpp`): `fminus_rt_parallel_for` runs a region's body over
  its iterations on the pool's threads and the caller's, one region at a time (a region that finds the pool held, or
  that a chunk of another starts, runs on its caller's thread alone), in the caller's floating-point mode, allocating
  nothing and taking no lock; `fminus_rt_parallel_start` and `fminus_rt_parallel_stop` count its users, the first
  making a thread for each core but one, the last joining them.
- `RuntimeSymbols.def` lists them, one `FMINUS_RUNTIME_SYMBOL(name)` each.
- `support/JitRuntime.cpp` expands that list into absolute symbols in a JIT (`defineRuntimeSymbols`).
- An executable gets the same functions by linking the archive.

Because the JIT and executables share one runtime, the two compile the same program to the same behaviour.

## Tools, examples, tests

| Target | Source | Uses |
|---|---|---|
| `fminus` | `tools/fminus/main.cpp` | parse, lower, then `--emit ast/ll/asm/obj/exe` or `--run` in the JIT; a `.cir` input, or an `#include` of one, is the text `fminus::circuit::generate` writes (`--emit fm` prints it) |
| `fminus-build` | `tools/fminus-build/main.cpp` | LLVM IR to exe/obj/asm/ll (HostTarget + Aot) |
| `fminus-hostinfo` | `tools/fminus-hostinfo/main.cpp` | the triple, CPU, features and data layout the JIT targets |
| `example-biquad` | `examples/biquad/bench.cpp` | biquad.fm's module, its cutoff a stream, against C++, compared and timed, through `fminus::Program` |
| `example-delay` | `examples/delay/bench.cpp` | delay.fm's functions and echo.fm's module, their lines allocated by the program, against C++, through `fminus::Program` |
| `example-saturator` | `examples/saturator/bench.cpp` | saturator.fm's tanh, plain and `poly float` loops, against C++, through `fminus::Program` |
| `example-meter` | `examples/meter/bench.cpp` | meter.fm's peak and RMS, plain and `poly float` loops, against C++, through `fminus::Program` |
| `example-voices` | `examples/voices/bench.cpp` | voices.fm's synth, in memory the host allocates, against a C++ mirror of its modules, through `fminus::Program` |
| `example-convolver` | `examples/convolver/bench.cpp` | convolver.fm's FIR by FFT, against C++ and a direct convolution, through `fminus::Program` |
| `example-parameters` | `examples/parameters/bench.cpp` | parameters.fm's filter, its cutoff a knob, a stream or frozen, against C++, through `fminus::Program` |
| `example-hello` | `examples/hello/main.cpp` | FMinus as a library at its smallest: a program compiled from a string, through `fminus::Program` |
| `example-circuits` | `examples/circuits/*.cpp` | the circuit compiler's modules through `fminus::Program` against the double reference and a 64x one: the research's targets (`--check`), its accuracy tables (`--tables`) and its cost table |
| (not built here) | `examples/plugin/` | FMinus Filter, a JUCE plugin, and its check `fminus-plugin-check`: a CMake project of its own, which `examples/CMakeLists.txt` skips, built with `cmake -S examples/plugin -B build-plugin` ([building.md](building.md)) |
| `fminus-tests` | `tests/unit/`, `tests/smoke/` | Catch2 |
| `fminus-library-tests` | `tests/library/` | Catch2, in C++17, against `FMinus::FMinus` alone: the public API as a host uses it |
| lit suite | `tests/lit/` | `fminus`, `fminus-build`, FileCheck, LLVM's tools |

## Libraries and include roots

| Target | What | Include roots it gives |
|---|---|---|
| `fminus_runtime` (`FMinus::Runtime`) | `src/runtime/**`, and PFFFT's objects (`fminus_pffft`) | none (generated code calls it by name) |
| `fminus_circuit` (`FMinus::Circuit`, `libfminus_circuit.a`) | `src/circuit/**`: the circuit compiler | `include/` (`"fminus/Circuit.h"`); C++17; no LLVM; `-ffp-contract=off`, so its double folding prints the same text on every target; `fminus_lib` links it publicly |
| `fminus_lib` (`FMinus::FMinus`, `libfminus.a`) | every other `.cpp` under `src/`, and the generated scanner and parser | `include/` (`"fminus/FMinus.h"`), `<build>/include` (`fminus/Config.h`); C++17; LLVM linked, not included |
| `FMinus::Internal` | `fminus_lib` for this tree's tools, examples and tests | also `src/` (`"frontend/Ast.h"`, ...), `<build>/src/generated/` (`"frontend/Parser.tab.hpp"`), LLVM's headers; C++23 |
| `FMinus::LLVM` | LLVM's headers (as system headers) and the libraries of `FMINUS_LLVM_COMPONENTS` | LLVM's |

`include/fminus/FMinus.h` is the public API: `fminus::Program` compiles a source (parse, lower for the machine's
`poly float` width, add the module to a `jit::Jit`), looks its functions up as C function pointers, and says which are
marked `nonblocking` or `nonallocating`; it describes each module and its parameters, sets a parameter's value or
stream in an instance, and keeps the program's IR as text when the host asks (`Options::keepIr`). `fminus::math`
gives C++ the math builtins as the default math mode computes them. It has no LLVM in it and needs C++17, so
`FMinus::FMinus` gives its users neither LLVM's include directories and definitions nor FMinus's C++23
(`fminus_target_defaults` makes a target's standard its own), only the libraries to link. The tree's tools, examples
and tests use the stages directly, through `FMinus::Internal`.

## Circuits: `src/circuit/`

The circuit compiler ([circuits.md](circuits.md)) is a front end to FMinus, not a part of its pipeline: it turns a
SPICE-style netlist into FMinus source, which then goes through the stages above like any other text. It is its own
library, with no LLVM in it, so a host's include resolver (or `tools/fminus`'s) can call it.

| Stage | Entry point | Input | Output |
|---|---|---|---|
| parse | `circuit::parse` (`Netlist.h`) | the netlist's text | `Netlist`: its elements, models, pots and directives, as written, with their lines and columns |
| plan | `circuit::plan` (`Plan.h`) | `Netlist`, `Options` | `Plan`: diode pairs as ports, pots as variable branches, the checks, a chain of blocks cut at ideal op amps, the configuration, the oversampled region |
| equations | `assemble`, `choosePivots` (`Mna.h`) | a block of the plan | `System` (its stamps, sources and readouts) and `Pivots` (the elimination order, checked over a grid of rates and pots) |
| derivation | `solve<T>`, `derive<T>` (`Mna.h`) | `System`, `Pivots`, the symbols as `T` | each readout as a linear form in the sources, then the module's coefficients |
| text | `deriveBlock`, `emit` (`Emit.h`) | the plan and each block's derivation on `Val`s | the module's FMinus text |

`generate()` (`Generate.cpp`) runs them; any error stops it before the text. `Messages` (`Netlist.h`) collects every
stage's errors, warnings and notes and formats them as clang does. `Reference.h` and `Analysis.h` are the double
reference and the measurements that `examples/circuits` and the tests check the emitted code with.

The invariant: every emitted coefficient is `derive<Val>` of the same derivation the reference runs as
`derive<double>` (and a linear block's analog response as `solve<std::complex<double>>`), over the same system and
pivots, so the float code computes what the reference computes, operation for operation.

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
  layout, and on the module flags the code generator writes (`fminus.math`, `fminus.knob-versions`).
- **Every load and store of a value is tagged with its type** (`tag`, `Tbaa.cpp`), unless strict aliasing is off. An
  untagged access is safe, since it may alias anything, but a lowering that forgets the tag slows the loops around it;
  `TbaaTest.cpp` walks a program that makes every kind of access and fails on one untagged. A tag that is wrong is a
  miscompile.
- **The runtime's declarations say only what is true of its code.** A runtime function that a builtin calls goes in
  `RuntimeSymbols.def`, and its facts in `kRuntimeFacts` (`Calls.cpp`, through `callRuntime`: the heap's and the
  FFT's) or in `mathFunction` (`Math.cpp`: the math builtins'). They change with its code: an attribute that is not
  true is a miscompile (extending.md, recipes 15 and 19).
- **Generated functions have the C calling convention, C names and external linkage.** That is the contract with C++
  callers, the JIT's `lookup` and the linker. A method's name is C++'s qualified one, `OnePole::tick`, with `this` as
  its first parameter: a symbol no C function can have, and what a host looks it up by. A new type at the boundary needs
  its C ABI attributes (extending.md, recipe 3: `bool` is `zeroext`; recipe 10: a `restrict` pointer is `noalias`;
  simd.md: `float8` goes through memory). The one exception is an executable's `main`, which `addEntryPoint` renames and
  makes internal. (Under `Denormals::Flush` an entry point's definition becomes the internal `<name>.body`, behind a
  wrapper that keeps the name, the type and the C ABI attributes. The wrapper calls the body through its address
  passed through an empty inline asm, so that no pass can move the call across the writes of the control register;
  with its address taken, the body keeps the C calling convention too. The call is indirect: under branch-target
  enforcement (Arm BTI, x86 CET IBT) the body would need a landing pad, and neither the JIT's pages nor a binary linked
  from FMinus's objects, which carry no BTI or IBT property, is enforced today; a link that forces the property on
  (`-z force-bti`, say) would be.)
- **Semantics follow C unless a deviation is documented.** The README lists the deviations: `float` and `int` are the
  only arithmetic types, an unsuffixed `1.5` is a `float`, pointers point to six types and convert only through `void*`
  or by adding `const`, the heap's memory is aligned and zeroed, `%` works on floats, calls may precede definitions,
  uninitialised variables are 0, falling off a non-void function's end is an error, a broken function effect is an
  error, C's strict aliasing is a little stricter (allocated memory keeps one type, there is no character type, a vector
  is its elements' type), the math builtins' results are their math mode's (by default one result for a float wherever
  it is computed), subnormals are flushed if the host asks, threads are `parallel` regions, structured and checked for
  what would race, and `main` returns a `float` or a `bool` and takes floats. As in C11 under clang, a loop whose
  condition is not a constant may be assumed to end, and a heap block the program never uses may be removed.
- **The runtime stays free of LLVM and real-time safe.** It runs inside the audio thread's call tree. The pool's
  `fminus_rt_parallel_for` is too: it waits for nothing but the chunks it handed out, never for the pool, and its
  system calls, one for each sleeping thread the region has a seat for, wake threads without blocking; making and
  joining threads is `fminus_rt_parallel_start`'s and `_stop`'s, off the audio thread.
