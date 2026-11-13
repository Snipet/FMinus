# Extending the compiler

How to add to FMinus. First, how a change moves through the compiler, and the few rules that keep that easy. Then
thirty-two worked recipes. Eleven grow the C subset from a one-line builtin to C's control flow, a custom LLVM pass,
`int`, `void` and pointers, the heap with the attributes that keep it off the audio thread, arrays and modules, FMinus's
classes, an FFT from a C library built into the runtime, and parameters, the values a plugin's host sets. Six more are
about the compiler rather than the language: math that gives a float one result wherever it is computed, knob versioning
and flat streams, C's strict aliasing told to LLVM, zero-init elision, what the runtime's calls and loops promise, and
subnormals flushed for the audio thread. Two more grow the language again: `#include`, which splits a program into
files, and `parallel for` and `parallel { }`, a program's own threads. Two return to the math: kernels of FMinus's own,
the same in every lane and on every target, and those kernels inline. The last four are for DSP's everyday code: C's
bitwise operators with a hash and white noise from a counter, constant tables stored once, `switch` and enumerations,
and constants at the top level with arrays sized by constant expressions. The last two are for the host: a loop budget,
which stops a program that would loop for ever before it holds the audio thread, and bounds checks for development
builds, which stop a call before an index leaves its array. Then one returns to C's preprocessor: its conditionals,
`#if` and `#ifdef`, which compile a program's instrumentation in or out. Then one is for whoever reads the errors: one
error for each mistake, enumerated types as C has them, and FMinus's first warning, clang's `-Wswitch`. The next adds
C's `double`, for accumulators and slow state, and decides what an unsuffixed floating constant is when FMinus has two
floating types. Then one adds C's arrays of arrays, 2-D arrays, with C's initialiser lists for them and a bounds check
for each dimension. The next is for the host again: attributes on a module's fields, a unit, a label or a range, which
FMinus passes on to the host, with C's string literals for their arguments. The next two are for the host too: calling a
function by name, its signature learnt as the host runs, through a thunk the JIT compiles for it, its arguments checked
and its errors values; and where the JIT put each function's machine code, which a sampling profiler maps its samples
to, with what the optimiser inlined into it. Recipe 38 is the first step towards a web library and parsers of JSON and
XML: C's string literals as builtins' arguments, formats with places for ints, checked as clang checks `printf`'s, bytes
decoded into floats, a builtin that blocks, and the build options a builtin can need; recipe 39 is the JSON parser
on it, a document read by paths the compiler prepares, each number correctly rounded; recipe 40 is the XML parser,
made the same way; recipe 41 is the web library, an HTTP(S) client a program reaches only under its host's policy;
recipe 42 is its server, a program's requests taken on the audio thread, wait-free, its routes literals the host
lists before anything runs; and recipe 43 is the file interface, files a program reaches only through the folders its
host maps, each directory opened beneath the last and compared by identity with the denied ones; and recipe 44 is C's
own again, the first step of a report on syntax for networked programs: a type for each kind of handle, TS 25755's
`defer`, and C2y's `if` and `switch` with a declaration; and recipe 45 is its second step, FMinus's own: `serve`,
`route` and `reply`, statements over the server's builtins, and a JSON literal a reply writes. Recipe 46 is the
same report's text: FMinus's type `string`, a read-only view of bytes, and `char` to keep one; and recipe 47 its
`when`, a statement that runs once each time its condition becomes true. Recipe 48 is where text goes and comes from:
`{name}` holes in addresses, a route's `string` parameters, a request's, a document's and a listing's text, strings in a
JSON literal, which the runtime's JSON writer now writes, and `%s` in a reply; recipe 49 is the report's `within`, a
deadline for the requests a block starts, every one of them ended where the block ends; recipe 50 its `json_read`, a
module filled from a JSON document in one walk, by a table of the module's fields the compiler makes; and recipe 51 the
report's last piece, a JSON literal of arrays, vectors, enumerations by name and modules, from the same table, written
as a reply's, a POST's and a file's bytes. Then the pitfalls
those recipes ran into, and design notes for what comes next. [simd.md](simd.md) has recipes 7 to 9, for vectors.

Recipes 1 to 4 are in the compiler now (`tanhf` and `%`; `bool` and its operators; `if`, `while` and `for`), and so are
simd.md's 7 to 9, recipe 10 (`int`, `void` and pointers), recipe 11 (the heap), recipe 12 (arrays and modules), recipe
13 (the FFT), recipe 14 (parameters), recipes 15 to 20 (the math modes, knob versioning, TBAA, zero-init elision, the
runtime's attributes and loop progress, denormal control), recipe 21 (`#include`), recipe 22 (`parallel for` and
`parallel { }`), recipes 23 and 24 (math kernels of FMinus's own, and inline), recipe 25 (the bitwise operators, `hash`
and `randf`), recipe 26 (constant tables), recipe 27 (`switch` and `enum`), recipe 28 (constants at the top level, and
sizes of constant expressions), recipe 29 (the loop budget), recipe 30 (bounds checks), recipe 31 (`#if` and `#ifdef`),
recipe 32 (one error a mistake, enumerated types, `-Wswitch`), recipe 33 (`double`), recipe 34 (2-D arrays), recipe 35
(field attributes), recipe 36 (calls by name), recipe 37 (code ranges, for a profiler), recipe 38 (string literals as
builtins' arguments, bytes to numbers, and the build options of builtins), recipe 39 (the JSON parser, and paths
the compiler prepares), recipe 40 (the XML parser), recipe 41 (the web library), recipe 42 (its server), recipe 43
(the file interface), recipe 44 (typed handles, `defer`, and `if` and `switch` with a declaration), recipe 45
(`serve`, `route`, `reply` and the JSON literal), recipe 46 (`string` and `char`), recipe 47 (`when`), recipe 48 (holes, and text from requests, documents and listings), recipe 49 (`within`), recipe 50 (`json_read`) and recipe 51 (a JSON literal of modules, and its sinks), so their sections describe how the code you have got there: read a section beside the files it
names. Recipes 5 and 6, a runtime function and a pass, are not part of the language; they live in [recipes/](recipes/)
as a patch, so you can apply them to see them work. None of these are sketches: each was applied to this repository, built with `-Werror`, and
tested in the dev and asan presets, every test passing (recipes 38 to 51 in the release preset only: each says where).
[architecture.md](architecture.md) describes the stages they touch.

FMinus follows C. When a recipe has a semantic choice to make, it makes C's: conversions, short-circuiting, which
`if` an `else` belongs to, what `continue` does in a `for`. And it reports errors in clang's words. The README lists
the few deliberate deviations.

## How a change moves through the compiler

Most language features touch the same places, in this order:

| # | Where | What to add |
|---|---|---|
| 1 | `src/frontend/Lexer.l` | a rule for each new token. Keywords go before `{IDENTIFIER}`; `macroNameError` reads a name with the scanner itself, so a new keyword is refused as a macro's name with nothing more to add. A new keyword or type's word goes into `isFMinusKeyword` (`src/circuit/Emit.cpp`) too, the circuit compiler's list of the words it must not emit as a name ([circuits.md](circuits.md)): `CircuitTest.cpp` reads the scanner's words from the parser's tokens and the handle types and fails until it has each. |
| 2 | `src/frontend/Parser.y` | the tokens (`%token`), their precedence if they are operators, and the grammar rules with their actions |
| 3 | `src/frontend/Ast.h` | a node: an alternative of `Expr::node` or `Stmt::node`, or an enumerator of `UnaryOp`, `BinaryOp`, `AssignOp`, `IncrementOp`, `Kind` (a type's) |
| 4 | `src/frontend/Ast.cpp` | its `dump` |
| 5 | `src/ir/` | its conversions and checks, and its lowering to LLVM IR, in the file its kind belongs to (`Lowering.h` lists them: types, functions, statements, expressions, calls, the math modes, vectors, memory, modules, effects, parallel regions, TBAA, denormals); a load or store of memory gets its type's tag (`tag`, recipe 17); a statement that holds code walks it in `Parallel.cpp`'s `Walker` too |
| 6 | `src/runtime/` | if it needs C++ at run time, a runtime function (recipe 5); if a builtin calls it, its facts: in `kRuntimeFacts`, through `callRuntime` (recipe 19), or for a math builtin's, in `mathFunction` (`Math.cpp`, recipe 15) |
| 7 | `src/jit/Call.cpp`, `src/support/Aot.cpp` | for a new type: whether a call by name, `Program::call` and `--run`, passes and returns it (`callType`'s `switch`, recipe 36), and whether an executable's `main` may take or return it (`addEntryPoint`) |
| 8 | `tests/` | a dump in `FrontendTest.cpp`; results in `CodeGenTest.cpp`, `ControlFlowTest.cpp`, `SwitchTest.cpp`, `EnumTest.cpp`, `IntTest.cpp`, `BitwiseTest.cpp`, `PointerTest.cpp`, `HeapTest.cpp`, `EffectTest.cpp`, `ArrayTest.cpp`, `TableTest.cpp`, `TopLevelTest.cpp`, `ModuleTest.cpp`, `FftTest.cpp`, `HashTest.cpp`, `VectorTest.cpp`, `PolyTest.cpp`, `ParallelTest.cpp`, `DoubleTest.cpp`, `Array2DTest.cpp`, `FieldAttributeTest.cpp`, `StringTest.cpp`, `BytesTest.cpp`, `JsonTest.cpp`, `XmlTest.cpp`, `HandleTest.cpp`,
`DeferTest.cpp`, `SelectionDeclarationTest.cpp`, `ServeTest.cpp`, `StringTypeTest.cpp`, `WhenTest.cpp`, `HoleTest.cpp`, `DocumentTextTest.cpp`, `WithinTest.cpp` or `JsonReadTest.cpp` (their helpers are in `Compile.h`; a runtime function that must not allocate is counted with `HeapCalls.h`); IR or errors in `tests/lit/fminus/` |
| 9 | `examples/` | a program that uses it, and a line in `tests/lit/fminus/examples.test` that runs it |
| 10 | `editors/vscode/syntaxes/fminus.tmLanguage.json` | a new keyword, type or builtin, so VSCode highlights it (its README says where) |
| 11 | `README.md` | the language table, and the deviations from C if any |

Steps 3 to 5 need no searching. Once the node exists, the build fails at every walk that doesn't handle it yet: a
`std::visit` without a matching lambda fails to compile, and a `switch` without the new enumerator trips `-Wswitch`,
an error under `-Werror`. Fix the errors in order and the feature is wired through. `std::visit` names only the
first alternative it can't handle, so after you fix one, the next appears. The same goes for step 7's `switch`: when
recipe 7 added `Float2` to `Bool8`, the build stopped in the driver until `--run` said what to do with a vector (that
`switch` is `jit::callType`'s now, recipe 36), and when recipe 10 added `Kind::Pointer`, it stopped in `typeOf`,
`typeName` and the driver. Steps 9 and 10 are the ones nothing reminds you of.

Each feature has an example to read beside its recipe: `examples/comparison/` for `bool` (recipe 3),
`examples/waveforms/` for every statement (recipes 4 and 27) and a constant of the top level (recipe 28),
`examples/chord/` for `float4` and the vector math library (recipes 7 and 8), `examples/unison/` for `poly float`
(recipe 9), `examples/biquad/` for a block of samples a call, through pointers (recipe 10), `examples/delay/` for state
on the heap and a kernel that can't allocate (recipe 11), `examples/saturator/` and `examples/meter/` for `poly float`
through a buffer, against plain loops (recipes 9 and 10), `examples/convolver/` and `examples/delay/echo.fm` for loops
over a module's fields (recipe 17), `examples/ensemble/` for `parallel for` and `parallel { }` (recipe 22), and
`examples/wavetable/` for a document a host hands a program, read by path (recipes 39 and 40), `examples/fetch/` for
a request a program makes (recipe 41), and several at once in a `within` (recipe 49), `examples/remote/` for the requests a program answers (recipes 42 and 45), and
`examples/kit/` for the files a program reads and writes (recipe 43), `examples/trigger/` for `when` (recipe
47), `examples/presets/` for holes and a route's `string` parameter (recipe 48) and a module read from a request's
body and written back as JSON (recipe 51), and `examples/settings/` for a module read from a JSON document (recipe
50).

## Ground rules

- **Let the compiler find the places.** Never write `default:` in a `switch` over an AST enum, nor a catch-all
  lambda (`[](const auto&)`) in a `std::visit`. Either one switches off the errors that list what a new node still
  needs.
- **Keep the syntax tree free of LLVM.** Nodes hold names, spellings, enums and children. Types, values and checks
  belong to the passes.
- **Do what C does.** When in doubt, write the C program, compile it with `clang -O0 -S -emit-llvm`, and compare
  with `fminus file.fm -O0`. The block names here are clang's (`land.rhs`, `cond.true`, `for.inc`, ...), so the two
  read alike.
- **Report and continue.** In the code generator, `error(location, message)` records an error, in clang's wording.
  Then return a poison value of the expected type (`poison(ast::Type::Float)`), never a null pointer, so lowering
  goes on and finds the next error. `lower` throws the module away at the end. A check of an operand's type tests the
  operand's `failed` first (recipe 32): an operand with an error of its own gets no second one, and what you return
  for it is a `recovery()`, failed in turn. Never return a failed value that no error is behind.
- **Leave the IR valid.** Every block ends in exactly one terminator (`ret`, `br`). A phi lists every predecessor
  exactly once. Allocas belong in the entry block (`allocate()`). `verifyFunction` runs on every function and reports
  a violation as an internal error.
- **Look at the IR twice.** `fminus file.fm -O0` shows what your lowering wrote: stack slots, loads and stores, as at
  clang's `-O0`. `fminus file.fm` (-O2) shows what LLVM made of it. Most lowering bugs are obvious at `-O0`, and most
  performance questions are answered at `-O2`.
- **Test both halves.** New syntax gets a dump test, new semantics a codegen test (run in the JIT), and new errors a
  test that checks their text and location.

## Recipe 1: a builtin (`tanhf`)

*In the base since commit 30c1dc4.*

A builtin is a row in `kBuiltins` (`src/ir/Calls.cpp`): a `<math.h>` name, the LLVM intrinsic it lowers to (over
`float`), and its parameter count.

```cpp
            Builtin{"fmaxf", llvm::Intrinsic::maxnum, 2}, Builtin{"tanhf", llvm::Intrinsic::tanh, 1},
```

That is the whole change. `lowerCall` already declares the intrinsic (`Intrinsic::getOrInsertDeclaration`),
checks the argument count, and refuses a definition that tries to reuse the name. A prototype of it, as `<math.h>`
would have, is accepted. LLVM lowers `llvm.tanh.f32` to a call to libm's `tanhf`, which the JIT resolves from the
process.

(This is the row as recipe 1 wrote it, and how the fast math mode still lowers it. Recipe 15 gave a transcendental
builtin a fourth field, the stem of its runtime functions, `Builtin{"tanhf", llvm::Intrinsic::tanh, 1, "tanh"}`: in
the default mode `tanhf` is a call of the runtime's `fminus_rt_tanhf`, which gives a float one result wherever it is
computed. An exact builtin, such as `fabsf`, still needs only the three.)

Before the row, a program could already call `tanhf` C's way, through a prototype, `float tanhf(float x);`: an
external function, which the JIT finds in libm. (Any `<math.h>` function that isn't a builtin still works that way,
`atanf` for one.) The builtin makes it part of the language: no declaration is needed, and the call is the intrinsic
that LLVM's passes treat uniformly. `IntrinsicEnums.inc`, in the LLVM install's `include/llvm/IR/`, lists the
intrinsics. An exact one is one row: `copysignf` (`copysign`), or `fmaf` (`fma`, three parameters). A transcendental
one, `exp2f`, is not since recipe 15: a bare row would be LLVM's intrinsic in every math mode, which LLVM folds with
the host's double functions, so it needs a stem and its runtime functions too (`fminus_rt_exp2f`, `fminus_rt_exp2_f4`,
`fminus_rt_libm_exp2f`), as recipe 15 gives the others, and recipe 23 gave `tanf`. `powf`, of two arguments, also
needs `lowerMath` and `mathFunction` to take two.

Update the count in `CodeGenTest.cpp` (`builtins().size()`), then test the value, in the builtins' test case:

```cpp
                       "float h(float x) { return tanhf(x); }\n");
    ...
    CHECK(lookup<float(float)>(*jit, "h")(0.5f) == fminus::math::tanh(0.5f));   // the default math mode (MathTest.cpp)
    CHECK(fminus::ir::builtins().size() == 11);
```

The test compared with `std::tanh` until recipe 15; `fminus::math::tanh` is what the default mode computes.

## Recipe 2: an operator (`%` and `%=`)

*In the base since commit 30c1dc4.*

Floating-point remainder, with `fmodf`'s semantics: the result takes the sign of the dividend. C allows `%` on
integers only, so this is FMinus's first extension beyond C; HLSL allows it on floats. It binds like `*` and `/`, and
it gets its compound assignment.

**Tokens** (`Lexer.l`, beside the other operators; `%=` is longer, so it wins over `%`):

```lex
"%="                        { return Parser::make_PERCENT_ASSIGN(yyextra->where); }
...
"%"                         { return Parser::make_PERCENT(yyextra->where); }
```

**Grammar** (`Parser.y`): declare the tokens, put them on their precedence levels, and add the rules.

```yacc
%token PLUS "+" MINUS "-" STAR "*" SLASH "/" PERCENT "%"
%token ASSIGN "=" PLUS_ASSIGN "+=" MINUS_ASSIGN "-=" STAR_ASSIGN "*=" SLASH_ASSIGN "/=" PERCENT_ASSIGN "%="
...
%right "=" "+=" "-=" "*=" "/=" "%="
%left "+" "-"
%left "*" "/" "%"
...
    | expr "%" expr                         { $$ = binary(@2, ast::BinaryOp::Remainder, $1, $3); }
...
    | expr "%=" expr                        { $$ = assign(@2, ast::AssignOp::Remainder, $1, $3); }
```

**Nodes** (`Ast.h`): an enumerator in each of `BinaryOp` and `AssignOp`.

```cpp
        Remainder,   // fmodf's: the sign of the dividend (C has % for integers only; HLSL has it for floats too)
```

Build now, and the compiler lists the rest of the work:

```
src/frontend/Ast.cpp: error: enumeration value 'Remainder' not handled in switch [-Werror,-Wswitch]    (BinaryOp)
src/frontend/Ast.cpp: error: enumeration value 'Remainder' not handled in switch [-Werror,-Wswitch]    (AssignOp)
src/ir/Expressions.cpp: error: enumeration value 'Remainder' not handled in switch [-Werror,-Wswitch]  (arithmetic)
src/ir/Expressions.cpp: error: enumeration value 'Remainder' not handled in switch [-Werror,-Wswitch]  (compound)
```

**The dump** gets a case in each `opName`: `"%"` and `"%="`. **The lowering** needs two cases:

```cpp
                case ast::BinaryOp::Remainder:
                    return builder_.CreateFRem(lhs, rhs);
...
                case ast::AssignOp::Remainder:
                    return ast::BinaryOp::Remainder;
```

A compound assignment is `x = x op v`, so `compoundOperator` maps `%=` to `%`, and `lowerAssign` does the rest.

**Tests.** The precedence shows in the dump (`FrontendTest.cpp`): `a + b % a * b` is `(+ a (* (% b a) b))`. The
values show in the JIT, in a test case of their own in `CodeGenTest.cpp`:

```cpp
    auto jit = compile("float f(float x, float y) { return x % y * 2; }\n"
                       "float wrap(float phase) { phase %= 1; return phase; }\n");
    REQUIRE(jit);
    CHECK(lookup<float(float, float)>(*jit, "f")(7.5f, 2.0f) == 3.0f);    // (7.5 % 2) * 2: % binds as * and /
    CHECK(lookup<float(float, float)>(*jit, "f")(-7.5f, 2.0f) == -3.0f);  // the sign of the dividend
    CHECK(lookup<float(float)>(*jit, "wrap")(2.25f) == 0.25f);
```

`frem` has no AArch64 instruction, so it becomes a call to `fmodf`. Check with `fminus file.fm --emit asm -o -`.

## Recipe 3: `bool`, comparisons, `!`, `&&`, `||` and `?:`

*In the compiler since commit 660d5f8. Example: `examples/comparison/comparison.fm`, a `bool main`.*

This recipe adds the expression half of C's control flow:

- C23's `bool`, with `true` and `false`;
- the comparisons, `<`, `<=`, `>`, `>=`, `==` and `!=`;
- `!`, `&&` and `||`;
- the conditional operator, `c ? a : b`.

```c
float sign(float x) { return x > 0 ? 1 : x < 0 ? -1 : 0; }
bool in_range(float x) { return x >= 0 && x <= 1; }
```

It brings three firsts: a second type with C's conversions; a C ABI detail; and control flow inside an expression,
which means basic blocks and phis.

### Tokens and grammar

`Lexer.l`: the keywords go with the others, before `{IDENTIFIER}`. The operators can go in any order, because Flex
takes the longest match, so `<=` wins over `<` and `!=` over `!`.

```lex
"bool"                      { return Parser::make_BOOL(yyextra->where); }
"true"                      { return Parser::make_TRUE(yyextra->where); }
"false"                     { return Parser::make_FALSE(yyextra->where); }
...
"<="                        { return Parser::make_LE(yyextra->where); }
">="                        { return Parser::make_GE(yyextra->where); }
"=="                        { return Parser::make_EQ(yyextra->where); }
"!="                        { return Parser::make_NE(yyextra->where); }
"&&"                        { return Parser::make_AND(yyextra->where); }
"||"                        { return Parser::make_OR(yyextra->where); }
"<"                         { return Parser::make_LT(yyextra->where); }
">"                         { return Parser::make_GT(yyextra->where); }
"!"                         { return Parser::make_NOT(yyextra->where); }
"?"                         { return Parser::make_QUESTION(yyextra->where); }
":"                         { return Parser::make_COLON(yyextra->where); }
```

`Parser.y`: C's precedence levels, in C's order, between assignment and the arithmetic ones. `?:` is right
associative, so `a ? b : c ? d : e` nests to the right. Equality binds more loosely than the relational operators,
and both are left associative, as in C.

```yacc
%token FLOAT "float" CONST "const" RETURN "return" BOOL "bool" TRUE "true" FALSE "false"
%token LT "<" LE "<=" GT ">" GE ">=" EQ "==" NE "!=" NOT "!" AND "&&" OR "||" QUESTION "?" COLON ":"
...
%right "=" "+=" "-=" "*=" "/=" "%="
%right "?" ":"
%left "||"
%left "&&"
%left "==" "!="
%left "<" "<=" ">" ">="
%left "+" "-"
...
type
    : "float"                               { $$ = ast::Type::Float; }
    | "bool"                                { $$ = ast::Type::Bool; }
    ;
...
    | "!" expr %prec UNARY                  { $$ = ast::makeExpr(at(@1), ast::UnaryExpr{ast::UnaryOp::Not, $2}); }
    | "true"                                { $$ = ast::makeExpr(at(@1), ast::BoolExpr{true}); }
    | "false"                               { $$ = ast::makeExpr(at(@1), ast::BoolExpr{false}); }
    | expr "<" expr                         { $$ = binary(@2, ast::BinaryOp::Less, $1, $3); }
    ...                                     (<=, >, >=, ==, != alike)
    | expr "&&" expr                        { $$ = logical(@2, ast::LogicalOp::And, $1, $3); }
    | expr "||" expr                        { $$ = logical(@2, ast::LogicalOp::Or, $1, $3); }
    | expr "?" expr ":" expr                { $$ = ast::makeExpr(at(@2), ast::ConditionalExpr{$1, $3, $5}); }
```

`logical()` is a helper beside `binary()` in the `%code` block. The precedence lines resolve every conflict: after
`a ? b : c`, a `+` is shifted (`+` binds tighter than `:`), but an `=` reduces first. So `(a ? b : c) = d` is the
code generator's "expression is not assignable", as in C.

### Nodes

`Ast.h`: a type, an operator, six comparisons, and three nodes.

```cpp
    // The value types. float is IEEE single precision (LLVM's float). bool is C23's: 0 or 1, which converts to float
    // as 0 or 1, and to which a float converts as `x != 0` (a NaN is true).
    enum class Type
    {
        Float,
        Bool,
    };
    ...
        Not,      // !x: a bool
    ...
        Less,        // the comparisons give bool; with a NaN, all but != are false
        LessEqual,
        Greater,
        GreaterEqual,
        Equal,
        NotEqual,
    };

    // && and ||: C's, so the right operand runs only when the left does not decide.
    enum class LogicalOp
    {
        And,
        Or,
    };
    ...
    // `true` or `false`.
    struct BoolExpr
    {
        bool value = false;
    };
    ...
    struct LogicalExpr
    {
        LogicalOp op = LogicalOp::And;
        ExprPtr lhs;
        ExprPtr rhs;
    };

    // `condition ? thenValue : elseValue`: C's, so only one branch runs. Two floats or two bools give that type;
    // a float and a bool give float.
    struct ConditionalExpr
    {
        ExprPtr condition;
        ExprPtr thenValue;
        ExprPtr elseValue;
    };
```

The build then fails at every walk:

- `Ast.cpp`: the `BinaryOp` and `UnaryOp` switches, `typeName`, and the expression `std::visit`;
- `src/ir/`: `typeOf` (`Types.cpp`), and the `UnaryOp` switch, `arithmetic` and the expression `std::visit`
  (`Expressions.cpp`).

Bison reports no conflicts. The dump is mechanical:

- an `opName` case per operator;
- `"true"` and `"false"`;
- `(&& a b)`, `(|| a b)` and `(?: c a b)`.

### C's conversions

With two types, values have to meet. C's rules are short:

- **bool to float:** a `bool` used arithmetically is 0 or 1. C goes through `int`; with `float` the only arithmetic
  type, it goes straight to `float`. (Since recipe 10 added `int`, a `bool` computes as an `int`, as in C.)
- **float to bool:** any scalar used as a condition is compared with 0. `x` as a condition means `x != 0`, and since
  that comparison is unordered, a NaN is true, as in C.
- **Where they apply:** a value is converted wherever C converts it: operands, assignments, initialisers, arguments,
  returns, conditions.

One helper does both directions:

```cpp
            // C's implicit conversions between the two types: bool to float is 0 or 1, and float to bool is `x != 0`,
            // so a NaN is true (an unordered comparison). Every value is a float or an i1.
            llvm::Value* convert(llvm::Value* value, ast::Type type)
            {
                llvm::Type* target = typeOf(type);
                if (value->getType() == target)
                    return value;
                if (type == ast::Type::Bool)
                    return builder_.CreateFCmpUNE(value, llvm::ConstantFP::get(value->getType(), 0.0), "tobool");
                return builder_.CreateUIToFP(value, target, "conv");
            }
```

(This is the helper as recipe 3 wrote it. Recipe 7 grew it into `convert(Value, type, location, use)`: the lowered
value carries its FMinus type, and a failed conversion reports clang's message for where it happened. See simd.md,
"Every value carries its FMinus type".)

Then every place that consumes a value converts it to what it expects:

```cpp
                            // C's usual arithmetic conversions, with float the only arithmetic type: a bool is 0 or 1.
                            llvm::Value* lhs = convert(lower(*binary.lhs), ast::Type::Float);
                            llvm::Value* rhs = convert(lower(*binary.rhs), ast::Type::Float);
...
                                   builder_.CreateRet(convert(lower(*ret.value), function_->returnType));
...
                for (std::size_t i = 0; i < args.size(); ++i)
                    args[i] = convert(args[i], callee.params[i].type);
```

The same goes for initialisers, assignments (`Binding` gains the variable's type), and the operands of `-`, `+` and
`!`. `function_` (the function being lowered) comes back for the return. These are implicit conversions, so there
are no type errors to report: `x + (a < b)` and `bool truthy(float x) { return x; }` are valid C, and they work.
Once types multiply, a semantic pass replaces this scheme (see "Where to go next").

### `bool` at the C boundary

FMinus functions are called from C++ through the C calling convention. In that convention a `bool` is passed and
returned zero-extended, and the IR has to say so, or a C++ caller reads garbage in the upper bits. Clang marks such
parameters and returns `zeroext`, and `declare` does the same:

```cpp
                llvm::Function* llvmFunction =
                    llvm::Function::Create(type, llvm::Function::ExternalLinkage, function.name, module_);
                // The C ABI passes and returns bool zero-extended: say so, or a C++ caller reads garbage bits.
                for (std::size_t i = 0; i < function.params.size(); ++i)
                {
                    if (function.params[i].type == ast::Type::Bool)
                        llvmFunction->addParamAttr(static_cast<unsigned>(i), llvm::Attribute::ZExt);
                }
                if (function.returnType == ast::Type::Bool)
                    llvmFunction->addRetAttr(llvm::Attribute::ZExt);
```

Every new type that crosses into C++ raises the same question: integers narrower than 32 bits, structs (in
registers or through memory, by size), vectors. The answer is whatever clang emits for the equivalent C declaration.

### Comparisons

LLVM has ordered and unordered float comparisons. Ordered ones are false when either side is NaN; unordered ones
are true. C's operators use ordered comparisons except for `!=`. The cases go into `arithmetic`, after the
operands' conversion to `float`:

```cpp
                // Ordered comparisons: false when either side is NaN. != is unordered: true then.
                case ast::BinaryOp::Less:
                    return builder_.CreateFCmpOLT(lhs, rhs, "cmp");
                ...
                case ast::BinaryOp::NotEqual:
                    return builder_.CreateFCmpUNE(lhs, rhs, "cmp");
```

`!` converts its operand to `bool` and flips it: `builder_.CreateNot(convert(operand, ast::Type::Bool), "lnot")`.

### `&&` and `||`: short-circuiting

In C, `a && b` evaluates `b` only when `a` is true. That's observable: `x > 0 && (ran = 1)` assigns only for a
positive `x`. So the operator is control flow. The left side branches either to a block that evaluates the right
side, or straight to the join block, where a phi picks the answer:

```cpp
            // a && b and a || b, as clang lowers them: b runs only when a does not decide (land.rhs, lor.rhs), and a
            // phi joins the two ways out.
            llvm::Value* lowerLogical(const ast::LogicalExpr& logical)
            {
                const bool isAnd = logical.op == ast::LogicalOp::And;
                llvm::Value* lhs = convert(lower(*logical.lhs), ast::Type::Bool);
                llvm::BasicBlock* lhsEnd = builder_.GetInsertBlock();
                llvm::Function* function = lhsEnd->getParent();
                llvm::LLVMContext& context = module_.getContext();
                llvm::BasicBlock* rhsBlock =
                    llvm::BasicBlock::Create(context, isAnd ? "land.rhs" : "lor.rhs", function);
                llvm::BasicBlock* endBlock =
                    llvm::BasicBlock::Create(context, isAnd ? "land.end" : "lor.end", function);
                if (isAnd)
                    builder_.CreateCondBr(lhs, rhsBlock, endBlock);
                else
                    builder_.CreateCondBr(lhs, endBlock, rhsBlock);

                builder_.SetInsertPoint(rhsBlock);
                llvm::Value* rhs = convert(lower(*logical.rhs), ast::Type::Bool);
                llvm::BasicBlock* rhsEnd = builder_.GetInsertBlock();   // lowering b may have added blocks
                builder_.CreateBr(endBlock);

                builder_.SetInsertPoint(endBlock);
                llvm::PHINode* phi = builder_.CreatePHI(builder_.getInt1Ty(), 2, isAnd ? "land" : "lor");
                phi->addIncoming(builder_.getInt1(!isAnd), lhsEnd);   // && is false when a is; || is true when a is
                phi->addIncoming(rhs, rhsEnd);
                return phi;
            }
```

`lhsEnd` and `rhsEnd` matter. Lowering an operand can leave the builder in a different block from the one it
started in, for example when that operand is itself an `&&`. The phi must name the blocks that actually branch to
the join block.

### `?:`: one branch runs

C evaluates only the branch taken, so `x > 0 ? (t = 1) : (t = 2)` assigns once. A `select` would evaluate both
operands, so the conditional needs blocks too. Its type is known only once both branches are lowered: two `bool`s
give `bool`, anything else `float`. So the conversions go at the end of each branch's block, before its branch to the
join:

```cpp
            // c ? a : b, as clang lowers it: only the branch taken runs, so it takes blocks and a phi (a select would
            // run both). SimplifyCFG turns a small one without side effects into a select at -O1 and above.
            llvm::Value* lowerConditional(const ast::ConditionalExpr& conditional)
            {
                llvm::Value* condition = convert(lower(*conditional.condition), ast::Type::Bool);
                llvm::Function* function = builder_.GetInsertBlock()->getParent();
                llvm::LLVMContext& context = module_.getContext();
                llvm::BasicBlock* trueBlock = llvm::BasicBlock::Create(context, "cond.true", function);
                llvm::BasicBlock* falseBlock = llvm::BasicBlock::Create(context, "cond.false", function);
                llvm::BasicBlock* endBlock = llvm::BasicBlock::Create(context, "cond.end", function);
                builder_.CreateCondBr(condition, trueBlock, falseBlock);

                // Each branch may end in another block than it started in (a nested ?:): the phi needs the last. Both
                // are lowered before their type is known, then converted to it at their ends.
                builder_.SetInsertPoint(trueBlock);
                llvm::Value* thenValue = lower(*conditional.thenValue);
                llvm::BasicBlock* thenEnd = builder_.GetInsertBlock();
                builder_.SetInsertPoint(falseBlock);
                llvm::Value* elseValue = lower(*conditional.elseValue);
                llvm::BasicBlock* elseEnd = builder_.GetInsertBlock();

                const bool bothBool = thenValue->getType()->isIntegerTy(1) && elseValue->getType()->isIntegerTy(1);
                const ast::Type type = bothBool ? ast::Type::Bool : ast::Type::Float;
                builder_.SetInsertPoint(thenEnd);
                thenValue = convert(thenValue, type);
                builder_.CreateBr(endBlock);
                builder_.SetInsertPoint(elseEnd);
                elseValue = convert(elseValue, type);
                builder_.CreateBr(endBlock);

                builder_.SetInsertPoint(endBlock);
                llvm::PHINode* phi = builder_.CreatePHI(typeOf(type), 2, "cond");
                phi->addIncoming(thenValue, thenEnd);
                phi->addIncoming(elseValue, elseEnd);
                return phi;
            }
```

At `-O0` you see the blocks as clang writes them. At `-O2`, SimplifyCFG has flattened them into `select`s, because
these operands have no side effects. For DSP code that is usually what you want: no branch to mispredict.

```llvm
; float both(float x, float y) { return x > 0 && y > 0 ? x : y; }     fminus both.fm -O0 (the slots elided)
  %cmp = fcmp ogt float %x1, 0.000000e+00
  br i1 %cmp, label %land.rhs, label %land.end
land.rhs:
  %cmp3 = fcmp ogt float %y2, 0.000000e+00
  br label %land.end
land.end:
  %land = phi i1 [ false, %entry ], [ %cmp3, %land.rhs ]
  br i1 %land, label %cond.true, label %cond.false
cond.true:
  %x4 = load float, ptr %x.addr, align 4, !tbaa !2
  br label %cond.end
cond.false:
  %y5 = load float, ptr %y.addr, align 4, !tbaa !2
  br label %cond.end
cond.end:
  %cond = phi float [ %x4, %cond.true ], [ %y5, %cond.false ]
  ret float %cond

; fminus both.fm -O2
  %cmp = fcmp ogt float %x, 0.000000e+00
  %cmp3 = fcmp ogt float %y, 0.000000e+00
  %land = select i1 %cmp, i1 %cmp3, i1 false
  %cond = select i1 %land, float %x, float %y
  ret float %cond
```

### Tests

The recipe's tests (in `tests/unit/ControlFlowTest.cpp`) check:

- the values of `magnitude` and `sign`;
- `bool` returned to C++ (`lookup<bool(float)>`);
- NaN in a comparison, and NaN as a condition;
- `bool` as 0 and 1 in arithmetic;
- that `&&` and `||` skip their right side;
- that `?:` runs one branch;
- the not-assignable conditional.

One existing test changes, though nothing about it broke. `bool` is a new way to start a function, so Bison's
top-level message gains a token: "expecting end of file or float" becomes "expecting end of file or float or bool"
(`FrontendTest.cpp`; see the pitfalls).

## Recipe 4: `if`, `else`, `while`, `for`, `break` and `continue`

*In the compiler. Example: `examples/waveforms/waveforms.fm`, band-limited waves by additive synthesis, which uses
every statement.*

C's statements for control flow:

```c
float sum_to(float n)
{
    float total = 0;
    for (float i = 1; i <= n; i += 1)
        total += i;
    return total;
}
```

### Tokens and the dangling `else`

`Lexer.l` gets six keywords: `if`, `else`, `while`, `for`, `break` and `continue`. The grammar takes the statements
as C writes them:

```yacc
    | "if" "(" expr ")" statement %prec THEN
                                            { $$ = ast::Stmt{at(@1), ast::IfStmt{$3, ast::own($5), nullptr}}; }
    | "if" "(" expr ")" statement "else" statement
                                            { $$ = ast::Stmt{at(@1), ast::IfStmt{$3, ast::own($5), ast::own($7)}}; }
    | "while" "(" expr ")" statement        { $$ = ast::Stmt{at(@1), ast::WhileStmt{$3, ast::own($5)}}; }
    | "for" "(" for_init optional_expr ";" optional_expr ")" statement
                                            { $$ = ast::Stmt{at(@1), ast::ForStmt{$3, $4, $6, ast::own($8)}}; }
    | "break" ";"                           { $$ = ast::Stmt{at(@1), ast::BreakStmt{}}; }
    | "continue" ";"                        { $$ = ast::Stmt{at(@1), ast::ContinueStmt{}}; }
    ;

// The first clause of a for: a declaration (its ; included), an expression and ;, or just ;.
for_init
    : declaration                           { $$ = $1; }
    | expr ";"                              { $$.push_back(ast::Stmt{at(@1), ast::ExprStmt{$1}}); }
    | ";"                                   {}
    ;

optional_expr
    : %empty                                { $$ = nullptr; }
    | expr                                  { $$ = $1; }
    ;
```

Without the `%prec THEN`, Bison reports C's classic ambiguity. This is its real output (`-Wcounterexamples`):

```
Parser.y: warning: 1 shift/reduce conflict [-Wconflicts-sr]
Parser.y: warning: shift/reduce conflict on token "else" [-Wcounterexamples]
  Example: "if" "(" expr ")" "if" "(" expr ")" statement . "else" statement
  Shift derivation
    statement
    `-> 30: "if" "(" expr ")" statement
                              `-> 31: "if" "(" expr ")" statement . "else" statement
  Reduce derivation
    statement
    `-> 31: "if" "(" expr ")" statement                             "else" statement
                              `-> 30: "if" "(" expr ")" statement .
```

In `if (a) if (b) s; else t;`, the `else` could close either `if`. C says it closes the nearest one, which is the
shift. Two precedence declarations say so: the rule without an `else` gets the lower precedence `THEN`, so an
`else` arriving after it is shifted.

```yacc
// The dangling else: in `if (a) if (b) s; else t;` the else could close either if. Giving it more precedence than the
// shorter rule (THEN) makes Bison shift it, so it closes the nearest if, as in C.
%precedence THEN
%precedence "else"
```

### Nodes

A branch and a loop body are single statements (often a block), owned through `StmtPtr = std::unique_ptr<Stmt>`.
`ast::own(stmt)` boxes one in a grammar action.

```cpp
    using StmtPtr = std::unique_ptr<Stmt>;

    // `if (condition) thenBranch [else elseBranch]`. A float condition is `!= 0`, as in C.
    struct IfStmt
    {
        ExprPtr condition;
        StmtPtr thenBranch;
        StmtPtr elseBranch;   // null without an else
    };

    struct WhileStmt
    {
        ExprPtr condition;
        StmtPtr body;
    };

    // `for (init; condition; step) body`. The init's declarations are scoped to the loop; a missing condition is
    // true, so the loop runs until a break or a return.
    struct ForStmt
    {
        std::vector<Stmt> init;   // declarations, an expression statement, or nothing
        ExprPtr condition;        // null when omitted
        ExprPtr step;             // null when omitted
        StmtPtr body;
    };

    struct BreakStmt
    {
    };

    struct ContinueStmt
    {
    };
```

These join `Stmt::node`. The build fails at the two statement walks, the dump and the lowering. The dump prints a
statement's sub-statements one level further in.

### Lowering

Three small helpers come first:

```cpp
            // A sub-statement (a branch, a loop body) has a scope of its own, as in C.
            void lowerScoped(const ast::Stmt& stmt)
            {
                scopes_.emplace_back();
                lowerStatement(stmt);
                scopes_.pop_back();
            }

            llvm::BasicBlock* newBlock(const char* name)
            {
                return llvm::BasicBlock::Create(module_.getContext(), name, builder_.GetInsertBlock()->getParent());
            }

            // Branches to `target` unless the block already ends (a return, a break, a continue in it).
            void fallThrough(llvm::BasicBlock* target)
            {
                if (!builder_.GetInsertBlock()->hasTerminator())
                    builder_.CreateBr(target);
            }
```

`fallThrough` handles control that never reaches the end of a branch or a body. `if (c) return 1;` must not get a
second terminator after its `ret`.

**`if`**: if.then and if.else (when there is an else) join at if.end. When both branches return, if.end has no
predecessors. The statements after it then lower into dead code. The function's end is unreachable too, so no
"does not return a value" error is reported. `reachable()`, a depth-first walk from the entry, already handles that.
(Recipe 32 branches on a constant condition one way, so the end after `while (1)` is unreachable too.)

```cpp
            void lowerIf(const ast::IfStmt& branch)
            {
                llvm::Value* condition = convert(lower(*branch.condition), ast::Type::Bool);
                llvm::BasicBlock* thenBlock = newBlock("if.then");
                llvm::BasicBlock* elseBlock = branch.elseBranch ? newBlock("if.else") : nullptr;
                llvm::BasicBlock* endBlock = newBlock("if.end");
                builder_.CreateCondBr(condition, thenBlock, elseBlock != nullptr ? elseBlock : endBlock);

                builder_.SetInsertPoint(thenBlock);
                lowerScoped(*branch.thenBranch);
                fallThrough(endBlock);
                if (elseBlock != nullptr)
                {
                    builder_.SetInsertPoint(elseBlock);
                    lowerScoped(*branch.elseBranch);
                    fallThrough(endBlock);
                }
                builder_.SetInsertPoint(endBlock);
            }
```

**Loops**: `loops_` is a stack of `{breakTarget, continueTarget}` pairs, pushed around each body. A `break` branches to
the innermost loop's exit, and a `continue` to its next iteration. With no loop around them, they are clang's
errors: "'break' statement not in loop or switch statement" and "'continue' statement not in loop statement". (Recipe
27 renames it `breakables_`, whose `Breakable`s are switches and parallel regions too.)

```cpp
                               [&](const ast::BreakStmt&) {
                                   if (loops_.empty())
                                       error(stmt.location, "'break' statement not in loop or switch statement");
                                   else
                                       builder_.CreateBr(loops_.back().breakTarget);
                               },
```

`while` is while.cond → while.body → while.cond, until while.end. `continue` goes to while.cond.

```cpp
            void lowerWhile(const ast::WhileStmt& loop)
            {
                llvm::BasicBlock* condBlock = newBlock("while.cond");
                llvm::BasicBlock* bodyBlock = newBlock("while.body");
                llvm::BasicBlock* endBlock = newBlock("while.end");
                builder_.CreateBr(condBlock);

                builder_.SetInsertPoint(condBlock);
                builder_.CreateCondBr(convert(lower(*loop.condition), ast::Type::Bool), bodyBlock, endBlock);

                builder_.SetInsertPoint(bodyBlock);
                loops_.push_back({endBlock, condBlock});
                lowerScoped(*loop.body);
                loops_.pop_back();
                fallThrough(condBlock);

                builder_.SetInsertPoint(endBlock);
            }
```

`for` has one more block, for.inc, for the step. `continue` goes there, so the step still runs, as in C. The init's
declarations live in a scope around the whole loop, so `i` is gone after it. The body has a scope of its own inside
that, so the body may redeclare `i`; in C that is legal, and in C++ it isn't. A missing condition branches straight
into the body.

```cpp
            void lowerFor(const ast::ForStmt& loop)
            {
                scopes_.emplace_back();
                lowerStatements(loop.init);
                llvm::BasicBlock* condBlock = newBlock("for.cond");
                llvm::BasicBlock* bodyBlock = newBlock("for.body");
                llvm::BasicBlock* incBlock = newBlock("for.inc");
                llvm::BasicBlock* endBlock = newBlock("for.end");
                builder_.CreateBr(condBlock);

                builder_.SetInsertPoint(condBlock);
                if (loop.condition)
                    builder_.CreateCondBr(convert(lower(*loop.condition), ast::Type::Bool), bodyBlock, endBlock);
                else
                    builder_.CreateBr(bodyBlock);   // no condition: until a break or a return

                builder_.SetInsertPoint(bodyBlock);
                loops_.push_back({endBlock, incBlock});
                lowerScoped(*loop.body);
                loops_.pop_back();
                fallThrough(incBlock);

                builder_.SetInsertPoint(incBlock);
                if (loop.step)
                    lower(*loop.step);
                builder_.CreateBr(condBlock);

                builder_.SetInsertPoint(endBlock);
                scopes_.pop_back();
            }
```

`for (;;) { }` never reaches for.end. A function that ends with it therefore needs no `return`, again because the end
isn't reachable.

(These are the two functions as recipe 4 wrote them. Recipe 19 keeps the branch each enters its loop by, and when the
condition is not a constant expression, `mustProgress` marks the branches back with `llvm.loop.mustprogress`, as
clang does in C11: such a loop may be assumed to end. That is the `!llvm.loop` on the branches back below.)

### The loop, before and after

```llvm
; fminus sum.fm -O0: the four blocks, as clang names them (the loads and stores elided)
for.cond:                                         ; preds = %for.inc, %entry
  br i1 %cmp, label %for.body, label %for.end
for.body:                                         ; preds = %for.cond
  br label %for.inc
for.inc:                                          ; preds = %for.body
  br label %for.cond, !llvm.loop !5
for.end:                                          ; preds = %for.cond
  ret float %total6

; fminus sum.fm -O1: the slots became phis, and the loop was rotated (the test moved to the bottom)
for.body:                                         ; preds = %entry, %for.body
  %i.09 = phi float [ %1, %for.body ], [ 1.000000e+00, %entry ]
  %total.08 = phi float [ %0, %for.body ], [ 0.000000e+00, %entry ]
  %0 = fadd float %total.08, %i.09
  %1 = fadd float %i.09, 1.000000e+00
  %cmp = fcmp ugt float %1, %n
  br i1 %cmp, label %for.end, label %for.body, !llvm.loop !2
```

### Tests

The recipe's tests (in `tests/unit/ControlFlowTest.cpp`) check:

- a `for` with a declared counter;
- that an `else` binds to the nearest `if`;
- that `continue` still runs the step;
- that `break` leaves only the inner loop, in a `for (;;)`;
- a `while`;
- an `if`/`else` that returns in both branches;
- an endless `for (;;) { }`;
- an `if` in dead code after a `return`.

The errors: `break` and `continue` outside a loop, the counter used after its loop, and a function that can still
fall off its end.

## Recipe 5: a runtime function (`db_to_gain`)

*Patch: [recipes/5-runtime-function-and-pass.patch](recipes/5-runtime-function-and-pass.patch) (with recipe 6), on
top of the compiler as it is.*

Some operations are better written in C++ than generated: lookup tables, approximations, calls to the platform. The
FMinus runtime is where they go. Generated code calls a runtime function by its symbol. The JIT resolves the symbol
through `defineRuntimeSymbols`, and an executable links `libfminus_runtime.a`.

**The function** (`src/runtime/Runtime.h`, `Runtime.cpp`, `RuntimeSymbols.def`):

```cpp
    // Decibels to a linear gain: 10^(db / 20). FMinus code declares it (`float fminus_rt_db_to_gain(float db);`), or
    // calls it as the builtin db_to_gain (ir/Calls.cpp).
    float fminus_rt_db_to_gain(float db) noexcept;
```

```cpp
extern "C" float fminus_rt_db_to_gain(float db) noexcept
{
    return std::pow(10.0f, db / 20.0f);
}
```

```
FMINUS_RUNTIME_SYMBOL(fminus_rt_db_to_gain)
```

**C's way: a prototype.** With the function in the runtime, a program can already call it. Declare it and call it,
as C calls any external function. The compiler doesn't change:

```c
float fminus_rt_db_to_gain(float db);
float f(float db) { return fminus_rt_db_to_gain(db) * 2; }
```

**The language's way: a builtin name.** To make it part of FMinus, under a name of its own and with no prototype,
add a second table beside `kBuiltins`, from the FMinus name to the runtime's symbol. The type goes in `Lowering.h`,
beside `Builtin`, since `declare` (in `Functions.cpp`) needs it too; the table and its lookup go in `Calls.cpp`:

```cpp
    // src/ir/Lowering.h
    // A function of the FMinus runtime (runtime/Runtime.h), float(float, ...), callable with no prototype (Calls.cpp):
    // JIT'd code finds it through support/JitRuntime.h, an executable links libfminus_runtime.a.
    struct RuntimeFunction
    {
        std::string_view name;
        std::string_view symbol;
        unsigned arity;
    };
    ...
    const RuntimeFunction* findRuntimeFunction(std::string_view name);   // db_to_gain, or nullptr

    // src/ir/Calls.cpp
    constexpr std::array kRuntimeFunctions{
        RuntimeFunction{"db_to_gain", "fminus_rt_db_to_gain", 1},
    };
```

`lowerCall` checks it after the builtins. `getOrInsertFunction` declares the external function once per module:

```cpp
        if (const RuntimeFunction* runtime = findRuntimeFunction(call.callee))
        {
            if (args.size() != runtime->arity)
                return wrongArity(location, call, runtime->arity);
            for (const Value& arg : args)
                irArgs.push_back(convert(arg, ast::Type::Float, location, Use::Passing));
            std::vector<llvm::Type*> params(runtime->arity, builder_.getFloatTy());
            llvm::FunctionCallee callee = module_.getOrInsertFunction(
                runtime->symbol, llvm::FunctionType::get(builder_.getFloatTy(), params, false));
            return {builder_.CreateCall(callee, irArgs), ast::Type::Float};
        }
```

`declare` (`Functions.cpp`) refuses the name for any function of the program, prototype or definition, the way it
refuses to redefine a builtin.

Keep the `.def` line in step with the header. Without it, the JIT in this repository's tools still finds the
function, but only by accident. LLJIT also searches the process's own symbols, and the tools link the runtime
statically and export it. A plugin that hides its symbols or dead-strips the unused ones would fail at lookup.
Runtime functions run inside the audio thread's call tree. Keep them free of allocation, locks and system calls,
and `noexcept`.

The declaration above, from `getOrInsertFunction`, is bare: LLVM takes the function to read and write any memory,
which is always safe. The builtins' runtime functions say what they do (`kRuntimeFacts`, recipe 19, and the math
builtins' `mathFunction`, recipe 15); this one could say `nounwind willreturn nosync nofree memory(errnomem: write)`,
which is true of it (`std::pow` may set `errno`).

## Recipe 6: an LLVM pass of your own (`CountFlopsPass`)

*Patch: [recipes/5-runtime-function-and-pass.patch](recipes/5-runtime-function-and-pass.patch).*

`src/opt/` holds FMinus's own LLVM passes. Three are in the compiler, registered straight in `optimiseModule`: knob
versioning (recipe 16, at the early simplification extension point), zero-init elision (recipe 18, at the scalar
optimiser's late one) and the math kernels (recipe 24, at the optimiser's last one). The example here counts a
function's floating-point operations after optimisation and records the count as a string attribute,
`"fminus-flops"="3"`. That's handy for watching what LLVM leaves of a kernel; the biquad's coefficient math, for
instance.

**A pass** is a class with a `run` method (the new pass manager's `PassInfoMixin`):

```cpp
// src/opt/CountFlops.h
    struct CountFlopsPass : llvm::PassInfoMixin<CountFlopsPass>
    {
        llvm::PreservedAnalyses run(llvm::Function& function, llvm::FunctionAnalysisManager& analyses);
    };
```

```cpp
// src/opt/CountFlops.cpp
    llvm::PreservedAnalyses CountFlopsPass::run(llvm::Function& function, llvm::FunctionAnalysisManager&)
    {
        unsigned flops = 0;
        for (const llvm::BasicBlock& block : function)
        {
            for (const llvm::Instruction& instruction : block)
            {
                const bool arithmetic =
                    llvm::isa<llvm::BinaryOperator, llvm::UnaryOperator, llvm::FCmpInst>(instruction);
                if (arithmetic && instruction.getOperand(0)->getType()->isFPOrFPVectorTy())
                    ++flops;
            }
        }
        function.addFnAttr("fminus-flops", std::to_string(flops));
        return llvm::PreservedAnalyses::all();   // a string attribute changes nothing an analysis computed
    }
```

Return `PreservedAnalyses::all()` only when nothing an analysis depends on changed. A transforming pass returns
`PreservedAnalyses::none()`, or the set it really kept.

**Plugging it in.** LLVM's default pipeline has extension points: callbacks registered on the `PassBuilder` before it
builds the pipeline. `src/opt/Passes.cpp` registers FMinus's passes, and `optimiseModule` calls it, so the JIT,
`fminus` and the AOT path all run them:

```cpp
// src/opt/Passes.cpp
    void registerPasses(llvm::PassBuilder& passes)
    {
        // Last, so it counts what the optimiser left. Other extension points: registerPipelineStartEPCallback (before
        // everything), registerPeepholeEPCallback (after each instruction combining), registerVectorizerStartEPCallback
        // (just before the loop vectoriser), ...
        passes.registerOptimizerLastEPCallback(
            [](llvm::ModulePassManager& modules, llvm::OptimizationLevel, llvm::ThinOrFullLTOPhase) {
                modules.addPass(llvm::createModuleToFunctionPassAdaptor(CountFlopsPass()));
            });
    }
```

```cpp
// src/support/HostTarget.cpp, in optimiseModule
        llvm::PassBuilder passes(&machine, tuning);
        opt::registerPasses(passes);
```

The callback receives the optimisation level, so a pass can run only at some levels. The `O0` pipeline invokes this
extension point too: `fminus file.fm -O0` also shows the attribute.

**Testing it** with lit (`tests/lit/fminus/flops.fm`):

```c
// RUN: fminus %s -O2 | FileCheck %s
float softclip(float x, float drive)
{
    const float y = x * drive;
    return y / (1.0f + fabsf(y));
}
// CHECK: define float @softclip({{.*}} #[[ATTRS:[0-9]+]]
// CHECK: attributes #[[ATTRS]] = { {{.*}}"fminus-flops"="3"
```

A pass that adds an attribute to every function changes every `define` line. The IR checks in
`tests/lit/fminus/codegen.fm` end their `define` lines with `{{.*}} {`, so they tolerate it. Write new checks the
same way.

## Recipes 7 to 9: SIMD

[simd.md](simd.md) has three more recipes, in the compiler too, with the hardware and LLVM's vector model they rest
on:

- **Recipe 7, `float2`, `float4` and `float8`**: OpenCL C's vector types. Lane-wise operators with scalars broadcast,
  literals (`(float4)(a, b, c, d)`), swizzles read and written (`v.wzyx`, `v.xy = ...`), comparisons that give lane
  masks, `?:` as a lane-wise select, the builtins overloaded on vectors, and ordered reductions (`reduce_add`,
  `reduce_min`, `reduce_max`). Every lowered expression starts carrying its FMinus type beside its IR (`Value`),
  instead of the code generator reading types off LLVM's.
- **Recipe 8, a vector math library**: `sinf` of a `float4` becomes one call to libsystem_m's `_simd_sin_f4`,
  through LLVM's target library information and `ReplaceWithVeclib`, about three times faster than four calls to
  `sinf`. Why not Accelerate, measured. Since recipe 15 it is the fast math mode's; the default mode reaches the same
  library through the runtime, with one result for a float wherever it is computed.
- **Recipe 9, `poly float`**: a vector as wide as the target's registers (4 floats on NEON, 16 on AVX-512), chosen by
  LLVM's cost model, with rules that keep a program's meaning the same at every width, `poly_lanes()` and
  `poly_index()`, and what does change with the width (the order of float sums).

## Recipe 10: `int`, `void` and pointers

*In the compiler. Example: `examples/delay/delay.fm`, a feedback delay that processes a block of samples in one call,
in plain C; harness and measurements in its README. (`examples/biquad/` did this too, before its state became a
module's, recipe 12.)*

A DSP kernel works on buffers. To take a C++ host's buffers and loop over them, a function needs pointers to reach the
samples, an `int` to count and index them, and a way to return nothing:

```c
void gain(const float* restrict in, float* restrict out, int n, float g)
{
    for (int i = 0; i < n; ++i)
        out[i] = in[i] * g;
}
```

C++ calls it as `void (*)(const float*, float*, int, float)`, once for a whole block, and the loop is FMinus's: at
`-O2`, LLVM's loop vectoriser runs it four `<4 x float>` multiplies at a time on NEON (`tests/lit/fminus/pointers.fm`).

The recipe went in as five steps, each of which builds and passes the whole suite: the type becomes a struct, every
store goes through lvalues, then `int`, `void` and pointers. The sections follow them.

### A type is a kind, and a pointee

Until recipe 10, `ast::Type` was an enum: every type was one name. A pointer is two, itself and what it points to, so
the enum became `Kind`, and `Type` a struct around it (`Ast.h`):

```cpp
    struct Type
    {
        Kind kind = Kind::Float;
        Kind pointee = Kind::Void;   // a pointer's: what it points to
        bool pointeeConst = false;   // a pointer's: what it points to is read-only through it (const float*)

        constexpr Type() = default;
        constexpr Type(Kind kind) : kind(kind) {}   // implicit: Kind::Float is the type float

        static constexpr Type pointerTo(Kind pointee, bool isConst)
        {
            Type type(Kind::Pointer);
            type.pointee = pointee;
            type.pointeeConst = isConst;
            return type;
        }

        friend constexpr bool operator==(Type, Type) = default;

        static const Type Void, Float, Bool, Int, Float2, Float4, Float8, Bool2, Bool4, Bool8, PolyFloat, PolyBool;
    };

    inline constexpr Type Type::Void{Kind::Void};
    inline constexpr Type Type::Float{Kind::Float};
```

The static members keep `ast::Type::Float` meaning what it did, so `poison(ast::Type::Float)` and
`type == ast::Type::Bool` compiled unchanged, here and in the recipes above. What changed is every `switch`: it
switches over `type.kind`, with `case ast::Kind::Float:`. The defaulted `==` compares every field, so `float*` and
`const float*` are different types, as in C, and a pointer never equals `Type::Float`. A module type, recipe 12, is
one more kind and a pointer to the module's declaration, and an array one more and a length.

`typeName` returns a `std::string` now, since a pointer's name is built: clang's spelling, `'const float *'`.

### Lvalues: one path for every store

Before pointers, the only thing a program could assign was a variable, or components of one, and two functions knew
it: `assignable`, for a variable, and `assignComponents`, for a swizzle. Pointers add `p[i]` and `*p`, and `++`, `--`
and `&` need the same knowledge. So it moved into one place, `Memory.cpp`, around one struct (`Lowering.h`):

```cpp
        struct LValue
        {
            llvm::Value* address = nullptr;   // the variable's stack slot, or the element's address
            ast::Type type;                   // what a read gives, and what a write converts to
            bool isConst = false;             // a const variable, or an element through a const T*
            std::string name;                 // the variable's; empty for an element
            ast::Type whole;                  // the type of what is at `address`
            std::vector<int> components;      // for components, the lanes of `whole` they are
        };
```

`isLValue(expr)` says whether an expression names one: a variable, `p[i]`, `*p`, or components of any of those.
`lvalue(expr, location)` finds it, reporting what is wrong with it (an undeclared name, a component a vector lacks).
`read` loads it; `write` converts a value to its type and stores it, loading the vector and inserting the lanes for
components; `writable` refuses a `const` one and duplicate components. An assignment is then:

```cpp
    Lowering::Value Lowering::lowerAssign(SourceLocation location, const ast::AssignExpr& assign)
    {
        Value value = lower(*assign.value);
        if (!isLValue(*assign.target))
        {
            error(location, "expression is not assignable");
            return poison(ast::Type::Float);
        }
        const std::optional<LValue> target = lvalue(*assign.target, location);
        if (!target || !writable(location, *target))
            return poison(ast::Type::Float);
        if (assign.op != ast::AssignOp::Assign)
        {
            value = compound(location, assign.op, read(*target), value);
            if (value.ir == nullptr)
                return poison(target->type);
        }
        return {write(location, *target, value, Use::Assigning), target->type};
    }
```

Components of components came for free: in `v.xy.x = 1`, the inner lvalue's lanes are `{0, 1}`, and `.x` picks the
first of them, so `lvalue` composes the two into `{0}`. A variable's value is a `read` too (`readLValue`), and so is
`p[i]`'s, so each new kind of lvalue below was one more case in `isLValue` and `lvalue`, and every use had it.

### `int`

`int` is C's on every target FMinus runs on: 32 bits, signed, `i32` in LLVM. The scanner gains the keyword, hex
integer constants (`0[xX]{HEXDIGITS}`), and `++` and `--`. It still returns one `NUMBER` token for every constant,
and the code generator reads the kind from the spelling: no point and no exponent (for hex, no `p`) make an integer
constant.

```cpp
    Lowering::Value Lowering::lowerNumber(SourceLocation location, const ast::NumberExpr& number)
    {
        const bool hex = number.spelling.size() > 1 && number.spelling[0] == '0'
                         && (number.spelling[1] == 'x' || number.spelling[1] == 'X');
        if (number.spelling.find_first_of(hex ? ".pP" : ".eEfF") == std::string::npos)
            return {lowerInteger(location, number.spelling, hex), ast::Type::Int};
```

`lowerInteger` reads it in its base with `std::from_chars`: hex after `0x`, octal after a leading `0`, so `010` is 8
and `08` is clang's "invalid digit '8' in octal constant". A constant that doesn't fit in an `int` is an error. C
would make it a `long`, which FMinus doesn't have, so `INT_MIN` is written `-2147483647 - 1`, as `<limits.h>` does.

This is a change of meaning. Before recipe 10, `2` named a float, so `1 / 2` was 0.5; now it is 0, as in C. Nothing in
the tree relied on it, since every example writes its floats with an `f`. And `x * 2` compiles to the IR it always
did: the `int` 2 converts to `float`, `x`'s type, and `IRBuilder`'s constant folder turns `sitofp i32 2 to float`
into `float 2.0` as it builds it.

**The usual arithmetic conversions.** `commonType` grows C's rule for scalars: two integers meet as an `int`, and an
`int` meeting a `float` meets it as a `float`. A `bool` takes part as an `int` (`arithmeticOf(Type::Bool)` is
`Type::Int`, C's integer promotion), so `true + true` is the `int` 2. Before, it was the float 2, which converts to the
same thing everywhere but in a division.

```cpp
        if (ast::isVector(ta) || ast::isVector(tb))
            type = ast::isVector(ta) ? ta : tb;
        else
            type = ta == ast::Type::Int && tb == ast::Type::Int ? ast::Type::Int : ast::Type::Float;
```

The operators moved into one function, `binary`, which the binary expressions, the compound assignments and `++`
share. It converts both operands to the common type and calls `arithmetic`, which now takes that type to choose the
instructions:

```cpp
        if (type == ast::Type::Int)
        {
            switch (op)
            {
            case ast::BinaryOp::Add:
                return builder_.CreateNSWAdd(lhs, rhs);
            case ast::BinaryOp::Subtract:
                return builder_.CreateNSWSub(lhs, rhs);
            case ast::BinaryOp::Multiply:
                return builder_.CreateNSWMul(lhs, rhs);
            case ast::BinaryOp::Divide:
                return builder_.CreateSDiv(lhs, rhs);
            case ast::BinaryOp::Remainder:
                return builder_.CreateSRem(lhs, rhs);
            case ast::BinaryOp::Less:
                return builder_.CreateICmpSLT(lhs, rhs, "cmp");
```

These are clang's. `sdiv` and `srem` truncate toward zero, as C's `/` and `%` do, and dividing by zero is undefined
(AArch64's `sdiv` gives 0; x86's `idiv` traps). The `nsw` ("no signed wrap") flags say what C says, that signed
overflow is undefined, and they matter to the optimiser. In `s += p[i + k]`, the address needs `i + k` as 64 bits.
With `nsw`, `sext(i + k)` is `sext(i) + sext(k)`: LLVM sign-extends `k` once, before the loop, and counts `i` in 64
bits. Without it, `i + k` might wrap, so a `sext` stays in every iteration of the vectorised loop. To see it, put
that loop in a file and compare `fminus sum.fm -O0 | opt -passes='default<O2>' -S` with the same after `sed 's/
nsw//g'` (LLVM's `opt` is in `.deps/install/llvm-…/bin`). A language that wants wrapping, as `-fwrapv` gives C,
drops the flags and pays that.

Conversions between scalars are C's, in `convert`: `sitofp` from `int` to `float` (exact up to 2^24, rounded beyond),
`fptosi` back (toward zero; out of range, undefined), `zext` from `bool`, and `icmp ne 0` to `bool`. `i *= 1.5f`
computes a float and truncates it into `i`, as C does. Casts are the same conversions, written out: a rule
`"(" cast_type ")" expr %prec UNARY`, for `float`, `int` and `bool`, a `CastExpr`, and `lowerCast`, which is
`convert` with `Use::Casting` for its error ("cannot cast from 'float4' to 'int'").

**`++` and `--`** are C's, before or after their operand. The prefix rules take `%prec UNARY`. The postfix ones sit
with the swizzle's `.` at the tightest level, `%precedence "." "++" "--" "["`, so `-i++` is `-(i++)` and `*p++` is
`*(p++)`, as in C. `++x` is `x += 1`, and `x++` is too, with the value from before:

```cpp
        const Value before = read(*target);
        const Value one{builder_.getInt32(1), ast::Type::Int};
        const Value after =
            binary(location, decrement ? ast::BinaryOp::Subtract : ast::BinaryOp::Add, before, one);
        llvm::Value* stored = write(location, *target, after, Use::Assigning);
        return prefix ? Value{stored, target->type} : before;
```

Since it goes through `binary` and `write`, it works on anything `+= 1` does: an `int` (`add nsw i32 %i, 1`, as
clang's `%inc`), a float, every lane of a vector, one component (`v.x--`), a `bool` (which `++` makes true, as in C),
and, below, a pointer.

`poly_lanes()` is an `int` now, as a count should be. Programs that used it as a float still work, since it converts.

### `void`

`Kind::Void` is a function's result type and nothing else's: a `void` parameter is clang's "argument may not have
'void' type", and a `void` variable "variable has incomplete type 'void'". `f(void)` is C's empty parameter list: the
`params` rule turns a single unnamed `void` parameter into none. `return;` is a `ReturnStmt` with no value, and
`lowerReturn` checks each kind of function against the other's form ("non-void function 'f' should return a value",
"void function 'g' should not return a value"). A `void` function that reaches its end returns there (`ret void`).

A call of a `void` function has no value, and the type rules say so. `convert` refuses `void` either way, so `float x
= f();` is "initializing 'float' with an expression of incompatible type 'void'". `commonType` refuses anything that
isn't arithmetic (`isArithmetic`: neither `void` nor a pointer), so `f() + 1` is "invalid operands to binary
expression ('void' and 'int')". `poison(Type::Void)` has no IR at all, since LLVM has no values of type `void`, and
nothing reads it: every use of a `void` value is one of these errors first.

### Pointers

**Declarations.** As in C, the `*` belongs to the declarator, not the type: `float *p, x;` is a pointer and a float.
`Parser.y` has a `pointer` rule, nothing or a `*` with the qualifiers after it, and every declarator, parameter and
result takes one. Where `const` lands is C's rule, in one helper:

```cpp
        inline Declared declared(bool isConst, ast::Type type, const Pointer& pointer)
        {
            if (!pointer.isPointer)
                return {type, isConst, false};
            return {ast::Type::pointerTo(type.kind, isConst), pointer.isConst, pointer.isRestrict};
        }
```

`const float* p` is a pointer to read-only floats, and `p` itself can move; `float* const p` is the reverse. The first
`const` is part of the type, and the second the variable's, kept where a variable's `const` always was (`isConst`).

**What a pointer may point to**: `float`, `int`, `float2`, `float4` and `float8` (`canPointTo`), one level deep. The
others wait for a reason:

- `bool*`: in a register FMinus's `bool` is an `i1`, but C keeps a `bool` in a byte. Through a pointer it would need
  `i8` in memory, and a `zext` on every store and a `trunc` on every load, as clang emits.
- `poly float*`: `p[1]` would sit `poly_lanes()` floats along, a different place on every target. With buffers, a
  `poly float` comes from a `float*` at a strip-mined index instead, with a masked load for the tail: `poly_load` and
  `poly_store` ([simd.md](simd.md), "Loads and stores").
- Pointers to pointers, `void*`, null constants and casts between pointer types: nothing needs them yet.

A declaration of any other is "FMinus has pointers to float, double, int, float2, float4, float8, void and modules, not
to 'bool'" (as recipes 11, 12 and 33 left it). The variable is then bound as a `float*`, so its uses report nothing
more.

**Elements.** `p[i]` and `*p` are lvalues (`IndexExpr`, and a `UnaryExpr` with `Dereference`), and both end in
`element`:

```cpp
        llvm::Value* address = pointer.ir;
        if (index)
        {
            llvm::Value* offset = convert(*index, ast::Type::Int, location, Use::Initializing);   // a bool: 0 or 1
            offset = builder_.CreateSExt(offset, builder_.getInt64Ty(), "idxprom");
            address = builder_.CreateInBoundsGEP(typeOf(type), pointer.ir, offset, "arrayidx");
        }
        return LValue{address, type, pointer.type.pointeeConst, "", type, {}};
```

That is clang's IR to the name: the index sign-extended to 64 bits (`idxprom`), since it is a signed `int` and
addresses are 64 bits, then a `getelementptr inbounds` of the element type (`arrayidx`). LLVM's pointers are opaque
(`ptr`), so the element type rides on the GEP and on each load and store, never on the pointer. `inbounds` is C's
promise that the address stays inside the object the pointer points into (or one past its end). As in C, `i[p]` is
`p[i]`: `lvalue` swaps the two when only the index is a pointer. An element through a `const T*` is read-only:
"read-only variable is not assignable", clang's words for it.

**Pointer arithmetic** (`pointerArithmetic`, which `binary` calls when either operand is a pointer) is C's:

- `p + i` and `i + p` are a GEP, as `p[i]`'s address is (`add.ptr`); `p - i` negates the index first (`idx.neg`);
- `p - q`, for two pointers of one type, is how many elements apart they are: `CreatePtrDiff`, the difference of
  the two addresses (`ptrtoaddr`, LLVM 23's; clang's `-O0` still writes `ptrtoint`) divided exactly by the element's
  size (`sdiv exact`), truncated to an `int` (C's is a `ptrdiff_t`, a `long`);
- comparisons of two pointers of one type are unsigned `icmp`s of the addresses (`icmp ult ptr`);
- anything else is "invalid operands to binary expression ('float *' and 'int')", or clang's own words for two
  pointers of different types ("comparison of distinct pointer types", "are not pointers to compatible types").

Since `++` and `+=` go through `binary`, `p++` and `p += 4` work too, and `*p++` reads and moves on, as in C.

**Conversions** (`convertPointer`). A pointer converts to `bool`, whether it is not null (`icmp ne ptr %p, null`), so
`if (p)` and `!p` work. It converts to a pointer to the same type with `const` added, `float*` to `const float*`,
which changes nothing in the IR. Nothing else converts. C converts a pointer to one of another type, or drops a
`const`, with only a warning. FMinus makes both errors, in the warning's words: "incompatible pointer types assigning
to 'float *' from 'int *'", "initializing 'float *' with an expression of type 'const float *' discards qualifiers".

**`&x`** (`lowerAddressOf`) is an lvalue's address, a pointer to its type, `const` when the lvalue is. For a variable,
that is its stack slot. A slot whose address escapes is one mem2reg leaves in memory; after inlining, SROA usually
promotes it after all. `&v.x` is clang's "address of vector element requested", and `&(x + 1)` "cannot take the
address of an rvalue of type 'float'".

**`restrict`** is C99's promise that, while the function runs, what a pointer reaches is reached through no other
pointer. On a parameter it becomes LLVM's `noalias`, as clang does it, in `declare`:

```cpp
            if (function.params[i].isRestrict)
                llvmFunction->addParamAttr(static_cast<unsigned>(i), llvm::Attribute::NoAlias);
```

It is what lets the optimiser keep a value in a register across a store through another pointer, and vectorise a
loop without first checking that its buffers don't overlap. On `gain` above, LLVM 23 vectorises either way, but
without `restrict` it computes `%diff.check` first, the distance between `in` and `out`, and runs the scalar loop when
they overlap; `pointers.fm` checks both. A `restrict` on a local pointer is accepted and changes nothing.

**At the C boundary** a pointer is an address in a general register (`x0` to `x7` on AArch64), and an `int` the low
half of one (`w0` to `w7`), with no attributes needed; floats travel separately, in `s0` to `s7`. So the C++
function pointer type only has to name the same parameters in the same order. `--run` passed floats only then, and
refused an `int` or pointer parameter; it printed an `int` result. Since recipe 36 it passes ints, and arrays for
pointers, through a thunk of the function's signature.

### Tests

- `tests/unit/IntTest.cpp`: division and remainder toward zero, octal and hex constants, `1 / 2` as 0, the
  conversions both ways, casts, `++` and `--` in both positions on ints, floats, vectors, a component and a `bool`,
  and the errors: an octal 8, a constant too large, `++` of an rvalue and of a `const`, a cast of a vector.
- `tests/unit/PointerTest.cpp`: a buffer from C++ through `gain`; state the host owns, carried across two calls
  (two blocks of two give one block of four); `*p++` over an `int*`; `float4*` frames with components assigned;
  a pointer as a loop counter, `p - q`, negative indices, `i[p]`, `&x` of a local, null; and the errors, each in
  clang's words.
- `tests/unit/CodeGenTest.cpp`: `void` functions, `f(void)`, and `void`'s errors. `VectorTest.cpp`: components of
  components, assigned.
- `tests/unit/FrontendTest.cpp`: the dumps, `(pre++ i)`, `(cast int x)`, `([] p i)`, `(* p)`, `(& x)`, `(return)`,
  and `(const float * restrict in)` for a parameter.
- `tests/lit/fminus/int.fm`: the IR of `int` arithmetic and conversions, as clang -O0 writes it, and `--run` of an
  `int` function. `tests/lit/fminus/pointers.fm`: the IR of `p[i]`, `noalias`, and the vectorised loop with and
  without `restrict`.

## Recipe 11: the heap, kept off the audio thread

*In the compiler. Example: `examples/delay/delay.fm`, a feedback delay whose line the program allocates; harness and
measurements in its README.*

A kernel's memory is often its own business. A delay line's length depends on the sample rate, a table's on the
range it covers, and the program that uses the memory is the one that knows how much it needs. So FMinus has C's
heap: `malloc`, `calloc` and `free`, with `void*` and `sizeof` to go with them. But an audio callback must never
allocate: the system's allocator takes a lock and can fault in a page, and either one can miss a deadline. So FMinus
also has clang's function effects, `[[clang::nonallocating]]` and `[[clang::nonblocking]]`, and checks them: a kernel
marked with one cannot reach the heap, however far down its calls go.

The pattern is three functions: one that allocates, one that runs a block, and one that frees.

```c
float* echo_create(int length)                     // off the audio thread: prepare(), once
{
    return calloc(length, sizeof(float));
}

void echo_process(float* restrict line, int* restrict position, int length, const float* restrict input,
                  float* restrict output, int n, float feedback, float mix) [[clang::nonblocking]]
{
    ...                                            // the audio thread: every block, no allocation
}

void echo_destroy(float* line, int* position)      // off the audio thread: once, at the end
{
    free(line);
    free(position);
}
```

All of it is C23. Clang compiles the file with `<stdlib.h>`, its `-Wfunction-effects` checks the same attribute, and
the two builds agree bit for bit (`examples/delay/README.md`). The recipe went in as three steps, each of which builds
and passes the suite: notes in diagnostics, the heap, and the effects.

### `malloc`, `calloc` and `free`

They are builtins, as `<math.h>`'s functions are: a call is lowered without a prototype (one, as `<stdlib.h>` has,
declares nothing new), and a definition of one is refused. Each calls a function of the runtime, `src/runtime/`
(`Calls.cpp`):

```cpp
    constexpr std::array kAllocations{
        Allocation{"malloc", "fminus_rt_malloc", 1},
        Allocation{"calloc", "fminus_rt_calloc", 2},
        Allocation{"free", "fminus_rt_free", 1},
    };
```

`lowerAllocation` converts the sizes to `int` (C's are `size_t`, which FMinus doesn't have), widens them to 64 bits
for the call, and gives back a `void*`:

```cpp
        std::vector<llvm::Value*> sizes;
        for (const Value& arg : args)
        {
            llvm::Value* size = convert(arg, ast::Type::Int, location, Use::Passing);
            sizes.push_back(builder_.CreateSExt(size, builder_.getInt64Ty(), "size"));
        }
        llvm::CallInst* allocate = callRuntime(runtime, builder_.getPtrTy(), sizes);
        allocate->setName(allocation.name);
        return {allocate, voidPointer};
```

`callRuntime`, which the FFT's builtins call too (recipe 13), declares the runtime's function from its arguments'
types, and since recipe 19 gives the declaration what is true of it: an allocator of zeroed memory, aligned to 64.

The runtime functions join `RuntimeSymbols.def`, so the JIT defines them by address and an executable links them
from `libfminus_runtime.a`, as recipe 5's `db_to_gain` would be. What they do is FMinus's choice:

```cpp
    void* memory = std::aligned_alloc(static_cast<std::size_t>(kAlignment), static_cast<std::size_t>(bytes));
    if (memory != nullptr)
        std::memset(memory, 0, static_cast<std::size_t>(bytes));
    return memory;
```

- **Aligned to 64 bytes**: a cache line, and the widest vector there is (AVX-512's `poly float`). A `float8` loads from
  it at its natural alignment, and two lines never share a cache line.
- **Zeroed**, `malloc`'s memory too. C leaves `malloc`'s indeterminate; FMinus starts everything at 0, its variables
  already, so a delay line starts silent. Write `calloc` for C's meaning everywhere. Zeroing touches every page, so
  the audio thread never takes a page fault on the memory later, as it could on fresh pages from the system.
- **Null** when a count or size is not positive, when the block would be more than 16 GiB (a mistake, which zeroing
  would turn into the machine's whole memory; ASan's allocator aborts on such a request), or when the memory isn't
  there. A block the program never uses is another matter: LLVM may remove it, as it may C's, and a removed block
  does not fail, so a test of it against null is false (recipe 19).
- **C's memory**: `aligned_alloc`'s blocks are `free`'s, so a C++ host may free what FMinus allocated, and FMinus may
  free the host's `malloc`'s (`HeapTest.cpp` does both). Not on Windows, whose C runtime has no `aligned_alloc`: there
  the blocks come from `_aligned_malloc`, which only `_aligned_free` releases, so `fminus_rt_free` calls that, a host
  frees FMinus's memory with it (or with FMinus's `free`), and FMinus's `free` cannot take the host's `malloc`'s.

### `void*` and `sizeof`

`malloc` returns an address with no type, and C converts a `void*` to and from any other pointer without a cast. So
`float* p = calloc(n, sizeof(float));` needs no cast in C, and none in FMinus. `canPointTo` now includes `Kind::Void`,
and `convertPointer` accepts the conversion either way, `const` still refused where it would be dropped:

```cpp
            const bool compatible = ast::samePointee(value.type, type) || value.type.pointee == ast::Kind::Void
                                    || type.pointee == ast::Kind::Void;
```

A `void*` has no elements, so `element()` refuses `v[i]` ("subscript of pointer to incomplete type 'void'") and `*v`
("indirection not permitted on operand of type 'void *'"), and `pointerArithmetic` refuses `v + 1` ("arithmetic on a
pointer to void"). It compares with any pointer, and in `?:` meets any pointer as a `void*`, as in C.

`sizeof(type)` is a constant `int` (C's is a `size_t`): `sizeOf` in `Types.cpp` knows each kind's size on the 64-bit
targets FMinus runs on, as clang's `ext_vector_type` lays them out. A `float4` is 16, a pointer 8, and a `poly float`
whatever the target's vectors hold: 16 bytes with NEON, 64 with AVX-512. The grammar takes a type, with a declarator's
`*`: `"sizeof" "(" qualifier type pointer ")"`. C's `sizeof expr` is not there: its operand is not evaluated, and
FMinus finds an expression's type only by lowering it.

### The effects

The attributes are C23's syntax, after a function's parameter list, where clang puts its function attributes:

```yacc
attributes
    : %empty                                {}
    | attributes "[" "[" attribute_list "]" "]"
                                            { $$ = $1; for (ast::Attribute& a : $4) $$.push_back(std::move(a)); }
    ;
```

The tree keeps them as written (`ast::Attribute`, a name such as `clang::nonblocking`), and `Effects.cpp` gives them
their meaning, clang's:

- **`nonallocating`**: the function calls no `malloc`, `calloc` or `free`;
- **`nonblocking`**: nor anything else that might wait, which in FMinus means an external function (a prototype the
  program doesn't define) whose own prototype isn't marked `nonblocking`.

A function's effect is the strongest any of its declarations states. The check needs every body's calls, so it runs
last. `lowerCall` records each call of an allocation builtin or a program function into `calls_` as it lowers it,
and `checkEffects` goes through the calls of every marked function. `unsafe` decides one, following calls down:

```cpp
        if (findAllocation(callee) != nullptr)
            return std::vector<Diagnostic>{};
        const auto found = functions_.find(callee);
        if (found == functions_.end() || effectOf(callee) >= required)
            return std::nullopt;   // undeclared (an error of its own), or marked: its own definition is checked
```

A call is fine when it goes to a function marked with the effect (or the stronger one), since that function's own
definition is checked. A function of the program without the mark is fine when its body is, by the same rule: clang
infers an effect through the definitions it sees, and an FMinus program's are all seen. An external function without
the mark is never fine; its definition isn't there to look at. Recursion is fine: a function already being looked into
adds nothing that the first look won't find. `<math.h>`'s builtins are fine (clang trusts its own, too), so `sinf`
works in a nonblocking kernel.

Each call that breaks the effect is an error at the call, in clang's words (clang only warns; FMinus refuses), with a
note for every step down to what breaks it:

```
effects.fm:19:18: error: function with 'nonblocking' attribute must not call non-'nonblocking' function 'helper'
effects.fm:12:5: note: function cannot be inferred 'nonblocking' because it calls non-'nonblocking' function 'grow'
effects.fm:7:12: note: function cannot be inferred 'nonblocking' because it calls non-'nonblocking' function 'calloc'
```

Notes are new in `Diagnostics`: an error carries them (`Diagnostic::notes`), and `format()` prints them right after
it, wherever they point. The errors sort by their own locations, and the notes stay with their error.

What the check does not see is what happens inside external functions that say they are nonblocking (it takes their
word, as clang does) and inside the JIT'd code LLVM generates (calls to `fmodf` for `%`, and to the math functions,
the runtime's or the vector math library's, which allocate nothing). At run time, clang's RealtimeSanitizer
(`-fsanitize=realtime`) checks the same attribute in C++; FMinus has no run-time check.

### Tests

- `tests/unit/HeapTest.cpp`: memory from `calloc` and `malloc`, aligned and zeroed, freed by C++ and by FMinus, and
  the host's freed by FMinus; null for sizes that aren't positive or overflow (the blocks returned to the host, which
  tests them: recipe 19); `sizeof` of each kind, `poly float`'s at 4 and 16 lanes; a delay line's state across two
  blocks; the errors (a redefined `malloc`, `void*`'s, `sizeof(void)`, a `const` pointer to `free`).
- `tests/unit/EffectTest.cpp`: a nonblocking kernel calling `sinf`, a marked prototype, and unmarked functions that
  don't allocate, recursion included; the errors and their notes, clang's, for an inferred function, an external one,
  `free`, a merely nonallocating prototype under `nonblocking`, and an unknown attribute.
- `tests/unit/FrontendTest.cpp`: the dumps of attributes and `sizeof`, and notes printed after their error.
- `tests/lit/fminus/heap.fm`: the IR of `calloc` and `free`, and an executable that allocates. `effects.fm`: the notes,
  from the command line.

## Recipe 12: arrays and modules

*In the compiler. Example: `examples/voices/voices.fm`, eight synth voices in modules, in memory a host of the public
API allocates; harness and measurements in its README.*

A DSP kernel is state and the code that runs on it. Recipe 11's delay keeps its state in two allocations, and every
call passes both, by position, to functions that only share a prefix. A module gathers them: fields, and methods that
see the fields by name. It is FMinus's class, C++'s struct with member functions. Arrays come with it, since a module's
state is often a history, a table or a set of voices:

```c
module OnePole
{
    float y;
    float k;

    void init(float cutoff, float rate)
    {
        k = 1 - expf(-6.2831853f * cutoff / rate);
    }

    float tick(float x) [[clang::nonblocking]]
    {
        return y += k * (x - y);
    }
};

module Bank
{
    OnePole filters[4];   // modules hold modules, and arrays of them
    float history[8];

    float tick(int i, float x)
    {
        history[i] = x;
        return filters[i].tick(x);
    }
};
```

What a module is, beside C++'s, follows from where it runs:

- **Nothing runs when one is made.** Its fields start at 0, as every variable FMinus declares does, wherever it lives:
  a local, a field, an array, FMinus's heap (`calloc(1, sizeof(Bank))`), or memory a host gives it. There are no
  constructors and no field initialisers: a method such as `init` sets the fields. So a host allocates a module with
  nothing but its size and alignment (`fminus::Program::module`), and memory it zeroes is a valid instance.
- **A module goes to a function and back by pointer.** `a = b` copies one, but a parameter or a result of a module
  type is an error. C++ would copy it, which a kernel that was meant to change it never wants; the C ABI passes an
  aggregate this size through memory anyway.
- **A method is a function named as C++ qualifies it**, `OnePole::tick`, whose first parameter is `this`: what a host
  calls (`function<float(void*, float)>("OnePole::tick", errors)`), and what errors say, as clang does.
- **No const methods, inheritance, overloading or access control** yet. A method is not called on a module that is
  read-only where it is called (clang's "'this' argument to member function 'tick' has type 'const OnePole', but
  function is not marked const").

Arrays are C's: one dimension of `float`, `int`, the float vectors or modules, zeroed, with C's initialiser lists, a
name that stands for a pointer to the first element, and parameters that are pointers. Everything goes through the
stages in order: a type that names a declaration, the scanner, the grammar, a layout pass, and the lowering of
fields, methods and copies.

### A type that names a declaration

A module type is one kind, and which module. `Type` gains a pointer to the module's declaration, and an array's
length (`Ast.h`):

```cpp
    struct Type
    {
        Kind kind = Kind::Float;
        Kind pointee = Kind::Void;        // a pointer's: what it points to; an array's: what its elements are
        bool pointeeConst = false;        // a pointer's: what it points to is read-only through it (const float*)
        const Module* module = nullptr;   // a module type's, and a pointer's or an array's whose pointee is one
        int count = 0;                    // an array's: how many elements
        ...
        static constexpr Type pointerTo(Type pointee, bool isConst);   // a kind, or a module type
        static constexpr Type moduleType(const Module* module);
        static constexpr Type arrayOf(Type element, int count);
    };
```

It stays a literal type, so `Type::Float` and the rest are still constants, and the defaulted `==` still compares every
field: two modules are two types, and `float[4]` is not `float[8]`. `pointerTo` takes a `Type` now, not a `Kind`, and
`pointee()` gives back the module with the kind, so a `OnePole*` knows its module. Everything that built a pointer
from a kind (`pointerTo(type.kind, ...)`) lost the module that way, which crashed the first program that used one
(Pitfalls). `samePointee(a, b)` replaces comparing two pointers' `pointee` kinds.

The modules themselves are `std::vector<std::unique_ptr<Module>>` in the `Program`, so a `Type` may point to one: the
modules stay where they are while the parser adds more, and when the program moves.

### The scanner learns the types

`OnePole lp;` and `OnePole* p;` are declarations, and `a * b;` is a product, and an LALR(1) parser cannot tell `T * p`
from `a * b` by its tokens. C has the same problem with typedef names, and C compilers solve it the same way: the
scanner knows which names are types. A module's name is a type from its declaration on, so the parse context keeps the
modules declared so far, and the scanner returns those names as `MODULE_NAME`, a token whose value is the type
(`ParseContext.h`, `Lexer.l`):

```lex
{IDENTIFIER}                {
                                std::string name(yytext, yyleng);
                                if (const fminus::ast::Module* module = yyextra->context->moduleNamed(name))
                                    return Parser::make_MODULE_NAME(fminus::ast::Type::moduleType(module),
                                                                    yyextra->where);
                                return Parser::make_IDENTIFIER(std::move(name), yyextra->where);
                            }
```

The grammar registers a module as soon as its name is read, in a mid-rule action, so its own fields can point to it
(`Osc* next;`):

```yacc
module
    : "module" module_name                  <ast::Module*>{
                                                auto module = std::make_unique<ast::Module>();
                                                module->location = at(@2);
                                                module->name = $2;
                                                $$ = module.get();
                                                parseContext.moduleNames.emplace($$->name, $$);   // the first stays
                                                parseContext.program.modules.push_back(std::move(module));
                                                parseContext.currentModule = $$;
                                            }
      "{" module_body "}" ";"               { ... }
```

The price is C's: a module is declared before it is used. Before its declaration its name is an identifier, and
`M m;` there is two identifiers in a row, a syntax error. A module's methods may call each other in any order, as
functions may, since the code generator declares every function before it lowers any.

### The grammar

`type` takes a `MODULE_NAME`, so every declaration, parameter, result and `sizeof` takes a module with no rule of its
own. A module's body is declarations, which become its fields, and functions, which become its methods. The method
rule shares its start with a field's declarator, `qualifier type pointer IDENTIFIER`, and the next token, `(`,
decides, so Bison has no conflict to resolve. The action names the method and gives it `this` before its parameters:

```yacc
    | qualifier type pointer IDENTIFIER "(" params ")" attributes block
                                            {
                                                ast::Module* module = parseContext.currentModule;
                                                ...
                                                ast::Function method{at(@4), module->name + "::" + name, $6,
                                                                     result.type, $9, true, $8, module};
                                                const ast::Type self =
                                                    ast::Type::pointerTo(ast::Type::moduleType(module), false);
                                                method.params.insert(method.params.begin(),
                                                                     ast::Param{at(@4), "this", self, true, false});
```

A method is an `ast::Function` among the program's, with `module` set, so declaring it, lowering it, checking its
effects and looking it up in the JIT are a function's, unchanged. `this` is a keyword, so no parameter of the program
can be called that, and the parameter can't be assigned.

A swizzle and a field are written alike, `v.x` and `m.y`, and the parser can't tell them apart without the base's
type. So `SwizzleExpr` became `MemberExpr`, with an `arrow` flag for `p->y`, and the code generator decides by the
base's type. `m.tick(x)` and `p->tick(x)` are `MethodCallExpr`s, and `this` a `ThisExpr`. An array is a declarator
with a size after its name, `"[" NUMBER "]"`, and an initialiser list; a parameter declared as one is a pointer, as in
C. The size was the parser's to check (a positive integer constant), since the `Type` holds it; recipe 28 makes it any
integer constant expression, which the code generator works out.

### A struct type per module

Before any function is declared, `layOut` (`Modules.cpp`) makes every module's LLVM struct type: opaque ones first, so
a field may point to any module, then each body in order, so a field may hold, by value, a module declared before. A
field that holds its own module, or a later one, is "field has incomplete type", as in C:

```cpp
        for (const std::unique_ptr<ast::Module>& module : program.modules)
            structs_.emplace(module.get(), llvm::StructType::create(module_.getContext(), module->name));
        ...
                else if (byValue && !complete.contains(type.module))
                {
                    error(field.location, "field has incomplete type " + quoted(ast::typeName(type))
                                              + ": a module holds only modules declared before it (and pointers)");
```

```llvm
%OnePole = type { float, float }
%Bank = type { [4 x %OnePole], [8 x float] }
```

The fields are laid out as a C struct's, padding where the data layout puts it, so a C++ struct with the same fields
in the same order is the same layout. That is what lets a host mirror a module, or read one of its fields. A module's
`sizeof` and alignment are the data layout's (`sizeOf` and `alignOf` in `Types.cpp`), so lowering needs the target's:
`ir::Options::dataLayout`, which the driver and the public API fill from the TargetMachine. Left empty, LLVM's default
agrees with the 64-bit targets FMinus runs on for every type it has.

### Fields, and `this`

A field is an lvalue, so it joins `lvalue()` (`Memory.cpp`), and with it reading, assignment, `++` and `&`. `field()`
finds it by name and gives its address, a getelementptr from the module's:

```cpp
        llvm::Value* at = builder_.CreateStructGEP(structs_.at(&module), address, index, name);
        return LValue{at, found->type, isConst, "", found->type, {}};
```

The module's address comes from `m.y` (an lvalue of a module type), `p->y` (a pointer to one, read-only for a `const
OnePole*`), or a bare `y` in a method. That last is C++'s lookup: the scopes first, the parameters and locals, then the
module's members, through `this`. In `lowerCall`, a method of the module is tried before anything else, so `tick(x)`
in a method is `this->tick(x)`, and a method named `sinf` would hide the builtin there, as a member hides a global in
C++.

`this` is the method's first parameter, and its attributes are clang's for C++'s: never null, aligned, and as big as
the module. They tell the optimiser that every field can be loaded at any point, which lets it hoist a field's load out
of a loop:

```llvm
define float @"OnePole::tick"(ptr nonnull align 4 dereferenceable(8) %this, float %x) {
  ...
  %this1 = load ptr, ptr %this.addr, align 8, !tbaa !2
  %k = getelementptr inbounds nuw %OnePole, ptr %this1, i32 0, i32 1
```

### Methods, and their effects

`callMethod` passes the module's address first, then the arguments, converted to the parameters' types. Arity counts
without `this`, so a wrong one reads as clang's would. It records the call by its qualified name, which is all the
effects need: `Effects.cpp` infers through a method's body as through a function's, so a nonblocking method that
could reach `calloc` is refused, with the note that says where, in clang's words (clang's `-Wfunction-effects` names
`'Line::grow'` too):

```
test.fm:5:50: error: function with 'nonblocking' attribute must not call non-'nonblocking' function 'Line::grow'
test.fm:4:31: note: function cannot be inferred 'nonblocking' because it calls non-'nonblocking' function 'calloc'
```

### A module's value is its address

LLVM can load and store a struct whole, but clang copies a C struct with a memcpy, and FMinus does as clang does: a
`Value` of a module type holds the module's address (`valueTypeOf` is a `ptr` for it). `read()` does not load it;
`write()` copies it with a memcpy, as clang's assignment does, and so does a declaration from another module:

```llvm
  call void @llvm.memset.p0.i64(ptr align 4 %local, i8 0, i64 8, i1 false)
  call void @llvm.memset.p0.i64(ptr align 4 %copy, i8 0, i64 8, i1 false)
  call void @llvm.memcpy.p0.p0.i64(ptr align 4 %copy, ptr align 4 %local, i64 8, i1 false)
```

Each declaration of an array or a module starts with a memset of 0, the aggregate form of every variable's zero.
From `-O1`, recipe 18 removes one where nothing can read a byte of it: an array's that loops overwrite, and a
module's that a copy overwrites, as `copy`'s memcpy does here, unless a call then takes `copy`'s address. The
same representation decides what is an error. `convert` refuses a module to anything but its own type; a module as a
parameter or a result is refused in `declare()`, which then declares a `ptr` there, so the IR stays valid while the
other errors are found; `?:` refuses two modules, since its phi would join two addresses where a copy was meant (choose
between pointers, `c ? &a : &b`).

### Arrays: a name that stands for a pointer

An array is a variable whose `read()` is no load: it is a getelementptr to the first element, and the pointer to it
is what an expression gets, const when the array is:

```cpp
        if (ast::isArray(source.type))
            return {builder_.CreateConstInBoundsGEP2_64(typeOf(source.type), source.address, 0, 0, "arraydecay"),
                    ast::Type::pointerTo(ast::pointee(source.type), source.isConst)};
```

That one line is C's decay, and with it `a[i]`, `a + 1`, `float* p = a;` and passing `a` to a `float*` parameter all
work through recipe 10's pointers, unchanged. `writable()` refuses an array ("array type 'float[4]' is not
assignable"), `&a` is refused (FMinus has no pointers to arrays), and `checkType` keeps an array's elements to what a
pointer can point to, since the decayed pointer must be one. `initializeArray` stores a list's elements in order, after
the memset; one longer than the array is an error, where C only warns.

### Modules to a host

The public API describes each module (`Program::module`, `include/fminus/FMinus.h`): its size and alignment, and each
field's name, type, offset and size, read from the data layout before the module goes to the JIT. A host allocates an
instance, zeroes it, and calls methods on it; `examples/voices/bench.cpp` does exactly that, checks that its C++
mirror of the modules has the same layout, and plays a score through both, bit for bit.

### Tests

- `tests/unit/ArrayTest.cpp`: arrays zeroed, indexed, initialised from lists, decayed to pointers and compared as
  them, of ints and vectors, declared afresh in a loop, a C++ host's buffer as an array parameter; the errors, the
  sizes' included.
- `tests/unit/ModuleTest.cpp`: a module's methods run on a C++ host's struct, against C++; modules holding modules and
  arrays of them, methods calling methods with and without `this`, `this` passed on; sizes with padding, copies,
  modules on the heap, pointers to a module's own kind; the effects through methods; the errors, clang's where clang
  has them.
- `tests/unit/FrontendTest.cpp`: the dumps of arrays and of modules, and a module's name used before its declaration.
- `tests/library/ProgramTest.cpp`: `module()`'s sizes, alignments, offsets and methods, and a module in memory the host
  allocates, through the public API alone.
- `tests/lit/fminus/modules.fm` and `arrays.fm`: the IR, at `-O0` and `-O2`.
- `example.voices`: the voices example's `--check`.

## Recipe 13: the FFT, a C library inside the runtime

*In the compiler. Example: `examples/convolver/`, a long FIR filter by overlap-add, against C++ and a direct
convolution; measurements in its README. Tests: `tests/unit/FftTest.cpp`, `tests/lit/fminus/fft.fm`.*

Spectral processing needs an FFT: fast convolution, analysis, a phase vocoder. Writing a fast one is a project of its
own, so FMinus takes a library's, and builds it into the runtime. A program makes a plan off the audio thread, runs
transforms on it there, and frees it at the end:

```c
module Spectrum
{
    fft_plan plan;                                             // a void* before recipe 44: either works

    void init(int n) { plan = fft_create(n); }                 // allocates: off the audio thread

    void process(const float* restrict in, float2* restrict bins) [[clang::nonblocking]]
    {
        fft_forward(plan, in, bins);                           // n floats into n/2 + 1 bins; allocates nothing
    }

    void end() { fft_destroy(plan); }
};
```

A real plan transforms n floats into n/2 + 1 bins and back; a complex plan, `fft_create_complex(n)`, n `float2`s into
n and back. A bin is a `float2`, (re, im), as FFTW's `fftwf_complex` and C++'s `std::complex<float>` lay one out.
Neither direction scales, as in FFTW and PFFFT: inverse(forward(x)) is n times x, and a program scales once, often in a
window it multiplies by anyway.

The library is PFFFT, marton78's fork of Julien Pommier's. On an M5, against FFTW 3.3.10 built with NEON (both single
precision, one thread, the median of five runs):

| N | real, FFTW_MEASURE | real, PFFFT | complex, FFTW_MEASURE | complex, PFFFT |
|---|---|---|---|---|
| 256 | 100 / 96 ns | 84 / 81 ns | 127 / 124 ns | 163 / 166 ns |
| 1024 | 443 / 425 ns | 336 / 333 ns | 636 / 636 ns | 787 / 774 ns |
| 4096 | 1899 / 1831 ns | 1537 / 1513 ns | 3116 / 3110 ns | 3697 / 3583 ns |
| 960 | 427 / 435 ns | 333 / 331 ns | 658 / 658 ns | 788 / 776 ns |

(forward / inverse). PFFFT is 15–25% faster on real transforms, audio's usual case, from 256 points up; FFTW is 15–35%
faster on complex ones. PFFFT makes a plan in microseconds, with two allocations, and never touches the heap in a
transform. FFTW's planner takes 10–70 ms a size, allocates tens of thousands of times and isn't thread-safe, and some
of its plans allocate in every transform (an inverse real one asked to preserve its input does). PFFFT is two C files,
and compiles into the runtime on every target with SIMD (NEON, SSE); FFTW is an autotools build at configure time. So
PFFFT, whose price is its sizes: products of 2, 3 and 5, a multiple of 32 points for a real plan and of 16 for a
complex one. Apple's vDSP is 2–3 times faster than both from about 512 points, but it is a system framework, not a
library that can be built in.

### The library, inside the runtime

`cmake/FMinusFft.cmake` fetches PFFFT once, pinned in `cmake/FMinusPins.cmake` like every other dependency, into the
deps cache (`.deps/src/pffft-<commit>`, marked with its hash, as LLVM's source is). It compiles two of its files,
`pffft.c` and `pffft_common.c`, into an object library, and never adds PFFFT's own CMake (its tests, its benchmarks,
the FFT libraries it compares itself with):

```cmake
add_library(fminus_pffft OBJECT
  "${FMINUS_PFFFT_SOURCE_DIR}/src/pffft.c" "${FMINUS_PFFFT_SOURCE_DIR}/src/pffft_common.c")
target_compile_definitions(fminus_pffft PRIVATE PFFFT_STATIC_DEFINE PFFFT_ENABLE_NEON=1 _USE_MATH_DEFINES)
set_target_properties(fminus_pffft PROPERTIES C_VISIBILITY_PRESET hidden POSITION_INDEPENDENT_CODE ON)
```

- **NEON has to be asked for.** PFFFT uses SSE on x86-64 by itself, but NEON only with `PFFFT_ENABLE_NEON`; without
  it, an arm64 build is scalar and says so only in a `#pragma message`. The NEON header checks for ARM itself, so the
  define is safe on every target, and in a universal binary.
- **Its symbols are hidden.** A plugin that links FMinus exports none of them, so it can't meet another copy of PFFFT
  in the host's process.
- **It keeps its own flags.** FMinus's warnings and sanitizers stay out of it, as they stay out of LLVM.

`src/CMakeLists.txt` puts the objects into the runtime library, `$<TARGET_OBJECTS:fminus_pffft>`, so
`libfminus_runtime.a` holds the FFT: a host links nothing more for it, and neither does an executable that
`fminus --emit exe` links (`fft.fm` runs one).

### The runtime's plan

`src/runtime/Fft.cpp` gives Runtime.h's seven functions, `fminus_rt_fft_create` to `fminus_rt_fft_inverse_complex`, on
PFFFT. A plan holds PFFFT's setup and three buffers, aligned for PFFFT's SIMD and touched when the plan is made, so a
transform never allocates or faults a page in, and the caller's buffers need neither PFFFT's alignment nor its
formats:

- **PFFFT packs a real transform's bins** into n floats: bins 0 and n/2, which are real, share the first pair, as
  F(0) + i F(n/2). The forward transform moves bin n/2 to the end and zeroes the two imaginary parts. Every other bin
  is where FMinus wants it, so an aligned output takes PFFFT's directly, and the transform costs what PFFFT's own call
  does. The inverse packs the bins back into the plan's buffer (its input is `const`), about 10% over PFFFT's call at
  1024 points.
- **A buffer that isn't on 16 bytes** goes through the plan's own: SSE's aligned loads need them (NEON's are only
  faster), and a program's `float x[1024]` is aligned to 4.
- **A null plan, or a plan of the other kind, does nothing**, so a size `fft_create` refused costs a program a silent
  output, not a crash. A size past 2^26 is refused before PFFFT sees it, since `pffft_new_setup` asserts there.

### The builtins

`fft_create`, `fft_create_complex`, `fft_destroy`, `fft_forward` and `fft_inverse` are builtins, as `malloc` is: a
table in `Calls.cpp`, `kFfts`, and `lowerFft`, which calls the runtime's functions through `callRuntime` (declared
from the arguments' IR types, and since recipe 19 with what each does: a transform reads its plan and its input,
writes its output, and touches nothing of the program's besides). The transforms are overloaded on their buffers, as
a math builtin is on its arguments:

```cpp
        const Value& decides = forward ? args[1] : args[2];   // the floats' side
        const bool real = pointsTo(decides, ast::Kind::Float);
```

A forward transform of floats is the real one, of `float2`s the complex one; an inverse that gives floats is real.
Each buffer then converts as a parameter of its type would, `const` added to the input's, so a wrong pointer is
clang's "incompatible pointer types passing ..." and a read-only output is "... discards qualifiers". Anything else is
"no matching function for call to 'fft_forward'", with the two forms it takes.

The effects need one change. `allocates(name)` is what they refuse a nonallocating function: the heap's builtins,
and the three that make and free a plan. A transform is fine in a nonblocking one, so `Spectrum::process` above
compiles, and a `process` that made its plan itself the first time it ran would not:

```
test.fm:7:20: error: function with 'nonblocking' attribute must not call non-'nonblocking' function 'fft_create'
```

The runtime's functions join `RuntimeSymbols.def`, so the JIT defines them by address, as it does the heap's.

### Tests

- `tests/unit/FftTest.cpp`: real and complex transforms at 64 and 1024 points against a double-precision DFT (within
  2e-6 of the largest bin), and their round trips, n times the input; PFFFT's sizes, accepted and refused; buffers a
  float off 16 bytes, bit for bit as aligned ones; null plans and plans of the other kind; the effects and the errors.
- `tests/lit/fminus/fft.fm`: the runtime calls in the IR, and a program that finds a sine's bin through the JIT and as
  an executable linked with `libfminus_runtime.a`.

## Recipe 14: parameters, the host's values

*In the compiler. Example: `examples/parameters/`, a filter whose cutoff an LFO sweeps through a knob or a stream, or
fixed and frozen, against C++; measurements in its README. Tests: `tests/unit/ParamTest.cpp`,
`tests/library/ProgramTest.cpp`, `tests/lit/fminus/params.fm`.*

A plugin's controls are values its host owns: a knob the user turns while the audio plays, an automation lane, a
modulation source. A kernel reads them, and C has no good way to say how. A field the host writes from another
thread is a data race; `volatile` makes every read a load from memory, in every iteration of the loop; a buffer
argument per control makes every caller pass one. A parameter is a field of a module that the host sets, and that its
methods read like a buffer:

```c
module Lowpass
{
    param float cutoff [[fminus::range(20, 20000)]] [[fminus::default(1000)]];   // Hz
    param bool bypass;
    float y;

    void process(const float* restrict in, float* restrict out, int n) [[clang::nonblocking]]
    {
        const float w = 6.2831853f / 48000;   // w * cutoff: a one-pole's coefficient, well below the Nyquist
        for (int i = 0; i < n; ++i)
            out[i] = bypass[i] ? in[i] : (y += w * cutoff[i] * (in[i] - y));
    }
};
```

`cutoff[i]` is the cutoff at sample i of the call. Where that comes from is the host's choice when it compiles the
program, not the program's (`Program::Options::parameters`, by qualified name; `fminus --stream` and `--freeze`):

| Binding | `cutoff[i]` is | Costs | For |
|---|---|---|---|
| a knob (the default) | the value the host set, read once when the method starts, atomically | a load a call | a control that moves now and then, set from any thread |
| a stream | `samples[i]`, from the pointer the host set before the call; with a null pointer, the slot's value, for the whole call (recipe 16) | a load a sample; a flat call, a knob's in its copy (recipe 16) | modulation, automation, a smoothed control: sample-accurate |
| frozen | a constant | nothing: the optimiser folds it | a setting that won't move while this program plays: a sample rate, a mode |

So one source serves a plugin whose cutoff is a plain knob, one that modulates it, and one that compiles again with
the settings that stopped moving frozen in. A knob is `volatile` in the sense that matters: the method never assumes
the value it read last call, and always sees one the host set. It is not `volatile` in the sense that costs: within a
call it is one value, in a register. A control that the host knows will hold still for a long time is a candidate for
freezing; one that moves inside a block wants to be a stream.

### Tokens and grammar

`param` is a keyword (`Lexer.l`), and a module member of its own (`Parser.y`): a type, a name and attributes, with
no initialiser, since the host, not the program, gives a parameter its value:

```
module_member
    : "param" type IDENTIFIER attributes ";"
                                            {
                                                $$.fields.push_back(ast::Field{at(@3), $3, $2, false, false, true,
                                                                               $4});
                                            }
    | declaration                           ...
```

A parameter is a `Field` with `isParam` set and its attributes, so it is laid out, copied and zeroed as a field is.
Attributes gain arguments: `attribute_args` is a list of numbers, each with a leading `-` if it has one, kept as
written (`"-60"`, `"2e4"`) so the tree's dump shows them as the source does; `ast::attributeNumber` reads one, as C
reads a constant. (Recipe 35 adds strings, and attributes on every field.)

### One slot for every binding

A parameter's place in its module is a slot, `{ const float* stream; T value; }`, a `float`, an `int` or a `bool`'s
byte for `T`, whatever the binding (`layOut`, `Modules.cpp`):

```cpp
                if (field.isParam)
                {
                    checkParameter(*module, field);
                    elements.push_back(
                        llvm::StructType::get(module_.getContext(), {builder_.getPtrTy(), slotTypeOf(type)}));
                    continue;
                }
```

The same layout under every binding means one instance runs under several programs. A host that freezes a setting
compiles a new program, then swaps the function pointers, and the instance's state (a filter's history, a delay's
line) carries on; a host that compiles a program with a stream and one without can run either on the same instance.
A stream is a `float` only: a gate or a mode a sample is something else (events, below).

`checkParameter` checks the rest: the type and the binding (an `int` stream is "parameter 'mode' is an int: only a
float parameter can be a stream"); `checkAttributes`, the attributes. `fminus::range(minimum, maximum)` takes two
numbers in order, `fminus::default(value)` one, inside the range if there is one, and another name of FMinus's
namespace is unknown; any other attribute is the host's, which FMinus passes on (recipe 35). A binding that names no
parameter of the program is an error too, at 1:1, as an option that names nothing would be. The range and the
default are for the host: FMinus checks them against each other, and neither clamps to them nor checks a value the
host sets.

### Reading one

`cutoff[i]` is a subscript, and the code generator looks at it before the general one: `parameterOf` says whether the
base is a parameter of the method's own module, named as a field is (a name no local or parameter of the method has),
or as `this->cutoff`. `lowerParameter` then lowers it by its binding:

- **Frozen**, it is a constant: the float, rounded; a `bool`'s `value != 0`; an `int`'s C conversion, toward 0, and the
  end of an int's range past it.
- **A knob** is the slot's value, which `atEntry` loads once, where the method starts:

  ```cpp
        llvm::BasicBlock& entry = function->getEntryBlock();
        llvm::IRBuilder<> at(&entry, entry.getFirstInsertionPt());
        llvm::Value* slot =
            at.CreateStructGEP(structs_.at(&module), function->getArg(0), field, parameter.name + ".slot");
        ...
            llvm::LoadInst* load = at.CreateAlignedLoad(value, at.CreateStructGEP(slotType, slot, 1), ...);
            load->setAtomic(llvm::AtomicOrdering::Unordered);
  ```

  `Unordered` is LLVM's weakest atomic ordering: the load can't tear, and it reads a value some store wrote, so the
  host's store (`setValue`, a relaxed `std::atomic_ref` store) is not a data race, from whichever thread it comes. On
  arm64 and x86-64 it is a plain load. It is in the entry block, from `this` itself (the method's first argument, not
  its stack slot), so every `cutoff[i]` in the method reads the same value, and the value is a register through the
  loops, which then vectorise as they would on a constant.

  Unordered, not monotonic (C++'s `memory_order_relaxed`), because LLVM may move and merge an unordered load, and
  neither a monotonic one. That decides what happens when the method is inlined into another's loop, a voice's
  `filter.tick()` called a sample from its synth's `process`: the knob's load is then in the loop. Unordered, GVN takes
  it out, once for the synth's call, and the loop vectorises; monotonic, it stays, a load every sample. Four such
  methods in a chain ran 6.4 times slower monotonic (0.378 ns a sample, against 0.059 unordered or with plain fields).
  A knob is then read once a call of the method that runs the loop, which a relaxed load was already allowed to see.
- **A stream** loads its pointer at the method's start the same way (`atEntry(field, true)`), and with it the slot's
  value, as a knob's, and `%cutoff.flat`, whether the pointer is null. From that test it chooses there where the
  samples are: a base, the pointer or, when the stream is flat, the slot's value field, and a stride, 1 or 0.
  `cutoff[i]` has no branch: it is a load from base + i * stride, as `p[i]` is from `p`, and a select of the value when
  the stream is flat. From `-O1`, knob versioning gives a method whose loop reads the stream a copy for each answer, as
  the budget allows, in which the read is the load alone, or the value (recipe 16).

A parameter is the host's to write: `cutoff[0] = 1` and `&cutoff[0]` are "parameter 'cutoff' is the host's: a method
reads it, and cannot assign it or take its address" (`lvalue`, `Memory.cpp`), and `cutoff` without its sample is
"parameter 'cutoff' is read as cutoff[i], in a method of 'Lowpass'" (`field`), which is also what reading one through
another instance, `other->cutoff[i]`, gives: a knob's once-a-call value belongs to the call of its own instance's
method. (A subscript whose base was already reported adds no "subscripted value is not a pointer" of its own.)

### A stream is restrict

A stream's samples are the method's to read, and nothing the method writes overlaps them: the host promises it, as
`restrict` promises it of an argument. LLVM has to be told. A stream's pointer is loaded from the module, not passed
as a `noalias` argument, so for all LLVM knows, the stream could be the module's own field `y`, and `y += ...` would
then go back to memory every sample, to be read again as the next sample of the cutoff.

`scopeStreams` tells it the way the inliner tells it about an inlined `restrict` argument: one alias scope for the
method's streams, the streams' loads in it (`!alias.scope`), every other access to memory outside it (`!noalias`), and
the scope declared where the method starts (`llvm.experimental.noalias.scope.decl`), so that a copy of the method
inlined into a loop gets a scope for each call. In `params.fm`, the one-pole's state stays in a register (in the
method's copy for a stream with samples, recipe 16; each access also carries its type's tag, recipe 17):

```llvm
for.body:
  %cutoff = load float, ptr %cutoff.at, align 4, !tbaa !8, !alias.scope !2
  %2 = load float, ptr %arrayidx, align 4, !tbaa !8, !noalias !2
  ...
  store float %5, ptr %arrayidx13, align 4, !tbaa !8, !noalias !2
  br i1 %exitcond.not, label %for.cond.common.ret.loopexit19_crit_edge, label %for.body, !llvm.loop !11

for.cond.common.ret.loopexit19_crit_edge:
  store float %5, ptr %y, align 8, !tbaa !8, !noalias !2         ; y, once, after the loop
```

### The host's side

`include/fminus/FMinus.h` gives each binding (`Program::Binding::knob()`, `stream()`, `frozen(value)`), and describes
each parameter in `Module::parameters`: its type and binding, where its slot's pointer and value are in an instance,
and its range and default. Four functions write them, none of which allocates or locks:

```cpp
fminus::Program::Options options;
options.parameters["Lowpass::cutoff"] = fminus::Program::Binding::stream();
auto program = fminus::Program::compile(source, errors, options);
const fminus::Program::Module& lowpass = *program->module("Lowpass");
const fminus::Program::Parameter& cutoff = *program->parameter("Lowpass::cutoff");
const fminus::Program::Parameter& bypass = *program->parameter("Lowpass::bypass");
fminus::Program::setDefaults(instance, lowpass);              // each parameter at its fminus::default

// The audio thread, each block: the modulation into a buffer, the buffer as the cutoff.
lfo(modulation, n);
fminus::Program::setStream(instance, cutoff, modulation);
process(instance, in, out, n);

// Any thread, any time: the next call reads it.
fminus::Program::setValue(instance, bypass, 1.0);
```

`setValue` converts as the binding's constant does (a float rounded, an int truncated, a bool unless 0) and stores
with `std::atomic_ref`; `value` reads it back. The header still needs only C++17: the atomics are in `FMinus.cpp`.
`setStream(instance, cutoff, nullptr)` makes the next call flat: every `cutoff[i]` is then the value `setValue` last
set, and no sample is read (recipe 16).

### Tests

- `tests/unit/ParamTest.cpp`: a knob read once when its method starts, so a write during the call is not what it
  reads; streams a sample at a time, a one-pole whose coefficient is one against C++ bit for bit; frozen floats, ints
  (toward 0, and the ends) and bools; one instance under a knob's, a stream's and a frozen program; parameters inside
  another module, in an array of them; the errors.
- `tests/unit/FrontendTest.cpp`: the dump of parameters and their attributes, and the syntax errors.
- `tests/library/ProgramTest.cpp`: `Module::parameters` and `parameter()`, offsets, ranges and defaults; `setValue`'s
  conversions, `setDefaults` and `setStream`; one instance under two programs; the binding errors; a knob set from
  another thread, every block showing one value whole.
- `tests/lit/fminus/params.fm`: the slots, the knob's unordered load at the start and the vectorised loop (and out of
  the loop of a method it is inlined into), the stream's alias scope and its state kept out of memory (and, with
  recipe 16, `Gain::process`'s copy for a stream with samples vectorised), the frozen parameters folded (a frozen
  `bool` turns a loop into a `memcpy`), and the driver's options.
- `example.parameters`: the parameters example's `--check`.

## Recipe 15: math that gives a float one result

*In the compiler. Examples: the C++ references of the harnesses whose kernels call a math builtin (biquad, saturator,
parameters, voices) call `fminus::math`, and `example-biquad`, `-saturator` and `-parameters` take `--math`. Tests:
`tests/unit/MathTest.cpp` (`[math]`), `tests/library/ProgramTest.cpp`, `tests/lit/fminus/math.fm` and
`math-names.fm`; `veclib.fm` is now the fast mode's.*

After recipe 8, the last bits of a math builtin's result depended on more than its argument:

- **Where a block ended.** The loop vectoriser runs a loop's body on `_simd_tanh_f4` and its remainder on libm's
  `tanhf`, so moving the end of a block moved samples from one to the other. The saturator's `--check` had to allow
  3 ulp across block sizes.
- **The optimisation level.** At `-O2` the backend pairs `sinf` and `cosf` of one angle into Apple's
  `__sincosf_stret`, whose sine is not always `sinf`'s; at `-O0` it calls the two. The biquad's `-O0` output was 3e-6
  from its `-O2` one.
- **A `poly float`'s width.** A 4-lane `sinf` was `_simd_sin_f4`; a `float8`, or 8 lanes or more, was scalarised to
  `sinf` a lane.
- **Freezing.** LLVM folds `llvm.sin` of a constant with the compiling machine's double `sin`, rounded, which is an ulp
  from libm's `sinf` for about 1% of arguments. A frozen parameter could move an output against the same value as a
  knob.

A host meets all four. It compares a plugin with a C++ reference, renders in buffers of any size, compiles for the
machine it is on, and freezes settings that stop moving, and none of that should move a sample. C and clang promise
nothing of the kind (clang's `-fveclib` has every one of these), so the promise is FMinus's. It is also one FMinus can
keep cheaply: it compiles on the machine the code runs on, and knows the library there.

### The idea, and its premise

A vector function is *lane-independent* when lane k of its result depends on lane k of its argument alone. A float
then gets the same result in any lane, beside any neighbours, and so in lane 0 of a vector of four copies of itself.
So the scalar function is lane 0 of the vector function of a splat, every vector call is the vector function four
lanes at a time, and LLVM never sees `sinf` at all: it can neither fold it, nor pair it, nor hand it to another
library.

The premise, as this recipe first met it, was a library's, and was measured. libsystem_m's `_simd_exp_f4`,
`_simd_tanh_f4` and `_simd_log_f4` were lane-independent, checked exhaustively on an M5: every float, in every lane,
beside awkward neighbours (NaN, infinities, huge and subnormal values). `_simd_sin_f4` and `_simd_cos_f4` were not. A
vector took a fast path when every lane was finite and at most 1e6 in magnitude, and a slow path for all four lanes
otherwise; the two paths disagreed on about 37% of arguments, by up to 3 ulp, so one large lane changed its three
neighbours' results. So the runtime guarded sin and cos: a vector went to libsystem_m whole only when every lane was
within 2^16, and lane by lane otherwise, and `MathTest.cpp` checked the premise again on every run. Elsewhere than Apple
systems the functions were libm's, a lane at a time. Recipe 23 replaced the library with kernels of FMinus's own,
lane-independent by construction and the same on every target, which retired the premise, the guard and its test.

### The runtime: `src/runtime/Math.cpp`

Fifteen functions (eighteen with recipe 23's `tanf`), in `Runtime.h` and `RuntimeSymbols.def`: the vector mode's
`fminus_rt_sinf` to `fminus_rt_logf`, their four lanes, `fminus_rt_sin_f4` to `fminus_rt_log_f4`, and the libm mode's
`fminus_rt_libm_sinf` to `fminus_rt_libm_logf`. A four-lane vector is `fminus_rt_float4`, GCC's and clang's
`vector_size(16)` float vector, which the C ABI passes in one register as it does FMinus's `<4 x float>`. Since recipe
23 the vector mode's are one kernel each, run on a float and on four:

```cpp
extern "C" float fminus_rt_sinf(float x) noexcept
{
    Scalar o;
    return fminus::kernels::sin(o, x);
}
...
extern "C" fminus_rt_float4 fminus_rt_sin_f4(fminus_rt_float4 x) noexcept
{
    Four o;
    return fminus::kernels::sin(o, x);
}
```

As this recipe first wrote them, on Apple systems, the scalar function was lane 0 of libsystem_m's four-lane function
of four copies of its argument (`_simd_sin_f4(splat(x))[0]`), and the four-lane one handed libsystem_m a whole vector
only within the guard's limit. The libm mode's functions are libm's on every target. The functions go through the
runtime even where a library could be called straight, so that the generated IR is the same on every target, and the
functions `MathTest.cpp` checks are the ones programs call.

### The code generator: `src/ir/Math.cpp`

A transcendental builtin's row in `kBuiltins` gains the stem of its runtime functions. The exact ones (`fabsf`,
`sqrtf`, `floorf`, `fminf`, `fmaxf`) have none, and are their intrinsics in every mode:

```cpp
        Builtin{"sinf", llvm::Intrinsic::sin, 1, "sin"},   Builtin{"cosf", llvm::Intrinsic::cos, 1, "cos"},
```

`lowerCall` converts the arguments as before, then hands a builtin with a stem to `lowerMath`, unless the mode is
fast. In the vector mode:

- a `float` is one call of `fminus_rt_sinf`;
- a `float4`, or a `poly float` of 4 lanes, is one call of `fminus_rt_sin_f4`;
- a `float8`, or a `poly float` of 8 to 64 lanes, is cut into four-lane pieces (`shufflevector`), a call each, joined
  in order (`llvm::concatenateVectors`);
- a `float2`, or a `poly float` of 1 or 2 lanes, is padded to four lanes with zeros, which keep sin and cos on the fast
  path, and its own lanes are taken back (since recipe 24, a two-lane call, or the float's).

So no vector is scalarised to libm, and every lane of every width is the scalar function of that lane. In the libm
mode every lane is a call of `fminus_rt_libm_sinf`.

Since recipe 24 the vector mode's two calls are of FMinus's math intrinsics, `fminus.sin.f32` and `fminus.sin.v4f32`,
which stand for the runtime's two functions until the end of the pipeline: a pass then replaces each call with the
kernel itself, inline, or at `-O0` with the call of the runtime's function that this section describes. The intrinsics
are declared as below, with the same attributes and the same variant (`_ZGV_LLVM_N4v_fminus.sin.f32(fminus.sin.v4f32)`,
and a two-lane one), and the IR below is what the vectorisers see.

`mathFunction` declares each runtime function once a module, with the attributes LLVM gives `llvm.sin` itself:
`memory(none) nounwind willreturn speculatable nosync nofree nocallback nocreateundeforpoison`. LICM takes a call out
of a loop, GVN merges two calls of one argument, and an unused one goes; but nothing evaluates one, since LLVM does not
know the function. The scalar function also names its vector variant, in LLVM's vector function ABI:

```llvm
declare float @fminus_rt_sinf(float) local_unnamed_addr #2
attributes #2 = { ... memory(none) "vector-function-abi-variant"="_ZGV_LLVM_N4v_fminus_rt_sinf(fminus_rt_sin_f4)" }
```

`_ZGV`, the `LLVM` ISA, `N` for unmasked, `4` lanes, `v` for one vector parameter, then the two names. That is what
the loop and SLP vectorisers look for (`VFABI::getVectorVariantNames`, `VFDatabase`), with no entry in the target
library information. So a scalar loop of `sinf` still vectorises, into four-lane calls, with scalar ones in its
remainder:

```llvm
vector.body:                                      ; fminus math.fm -O2: sines()
  %4 = tail call <4 x float> @fminus_rt_sin_f4(<4 x float> %wide.load)
  %5 = tail call <4 x float> @fminus_rt_sin_f4(<4 x float> %wide.load17)
  ...
for.body:
  %18 = tail call float @fminus_rt_sinf(float %17)
```

The variant has to be declared in the module when the vectorisers look for it, and the module simplification
pipeline's `GlobalDCE` removes a declaration nothing uses before they run. So it goes into `llvm.compiler.used`
(`llvm::appendToCompilerUsed`), as LLVM's own `InjectTLIMappings` keeps a vector library's declarations.

A builtin's three names (`sinf`'s `fminus_rt_sinf`, `fminus_rt_sin_f4` and `fminus_rt_libm_sinf`) are the runtime's, and
a program that calls the builtin may not declare a function of any of them itself, a definition or a prototype, of any
type. Such a function would be the one the call reaches, or the one LLVM takes for the runtime's: it would get the
runtime's attributes, which are false of a program's code (a call of one that could loop forever was removed, as
`willreturn`), and the four-lane name would be the scalar function's vector variant, which the loop vectoriser calls
with a `<4 x float>` whatever the program's function takes (it asserted on one that took an `int`). So `lowerCall` runs
`checkMathNames` before any mode's lowering, and each call of the builtin is an error, with a note at the program's
function:

```
math-names.fm:18:18: error: 'sinf' reserves 'fminus_rt_sin_f4' for the runtime's function, and this program declares a function of that name: rename it
math-names.fm:10:5: note: 'fminus_rt_sin_f4' declared here
```

Every mode refuses all three names, the fast mode too, which calls none of them, so that whether a program compiles
does not depend on its mode. Only the names of the builtins a program calls are refused: a program that declares
`fminus_rt_sinf` and calls only `cosf` is left alone, as `callRuntime` leaves a program's own heap and FFT functions
(recipe 19), and a module's method of such a name (`M::fminus_rt_sin_f4`) has another name. So `mathFunction` only
ever finds the runtime's own declaration.

### The option, and the optimiser

`ir::lower` records the mode in a module flag, `fminus.math` (`kMathFlag`, `HostTarget.h`): `"vector"`, `"libm"` or
`"fast"`, with LLVM's `Error` behaviour, so two modules of different modes cannot be linked. `optimiseModule` gives the
pipeline recipe 8's vector library (`DarwinLibSystemM` and `ReplaceWithVeclib`) only for `"fast"`, or for a module
without the flag (hand-written IR, which `fminus-build` takes). The other modes' calls are the runtime's, which
neither knows; the library would only vectorise a loop of a prototype's libm function (`float atanf(float x);`), and
give it one result in the loop's body and another in its remainder. The flag travels with the module, so the JIT,
`fminus` and executables need no new argument. (Since recipe 24 `optimiseModule` also runs `MathKernelsPass` last, at
every level and whatever the flag: only the vector mode's calls are of its intrinsics.)

| `Program::Options::math`, `fminus --math` | `sinf` ... `logf` are | One result for a float? | Constants folded? | Loops of them vectorise? | Error |
|---|---|---|---|---|---|
| `Vector`, `vector` (the default) | FMinus's own kernels, the same on every target (recipe 23), inline from `-O1` (recipe 24) | yes | yes, to the kernels' results (since recipe 24) | yes, into four-lane kernels | up to 1.56 ulp, `tanf` 3.35 (recipe 23) |
| `Libm`, `libm` | the runtime's wrappers of libm's, a lane at a time | yes | no | no | libm's (within 1 ulp) |
| `Fast`, `fast` | LLVM's intrinsics, as recipe 8 left them | no | yes, with the host's double functions | yes, into `_simd_*_f4` | either |

`ir::Options::math` (`ir::Math`) is the same choice. The fast mode is the compiler before this recipe: the same IR, the
same pipeline, and the module flag, but for the runtime's math names, which it refuses too (above).

### What changes for programs and hosts

- **Results move in their last bits.** A scalar `sinf` in the default mode was libm's (within 1 ulp of the exact
  result); it became the vector function's lane (within about 3), the same as a vector call's. `waveforms.fm`'s
  `main 0.25 1000` was 0.48676, where libm gives 0.4867599 (`examples.test` accepts both). Recipe 23's kernels moved
  them once more, to within 1.56 ulp, and the same on every target.
- **C++ references** call `fminus::math::sin` ... `log` (`include/fminus/FMinus.h`), which compute what the default
  mode does, and agree bit for bit at any block size, level, width and binding. They are `[[gnu::const]]` for GCC and
  clang (`FMINUS_MATH_CONST`): without it an opaque function is called every time, and the biquad's reference, which
  recomputes its sine and cosine for each coefficient, went from 2.65 to 16.5 ns a sample.
- **A frozen parameter's `sinf`** is no longer folded. It is a call, which LICM takes out of the loop, once a method
  call, and the output is the knob's bit for bit (`examples/parameters/`'s `--check` now checks it). (Recipe 24 folds
  it again, to the kernel's result, the knob's still.)
- **Vectors of other widths are faster**: a `float8` `sinf` was eight `sinf` calls, and is two four-lane ones.
- **Scalar calls that can't vectorise are slower**: each computes four lanes of a vector function, whose latency is two
  to six times libm's (Measurements). (Recipe 23's kernels compute one lane, and recipe 24 puts them inline.)
- **A prototype's libm function** (`atanf`, `powf`) gets the vector library only in the fast mode, and only where its
  calls write no memory: on Apple systems (recipe 19).
- **Elsewhere than Apple systems** the vector mode's results were libm's, the four-lane functions a loop over libm,
  until recipe 23 made them FMinus's own everywhere.
- **The runtime** has fifteen more functions, which a host that hides or strips symbols must keep
  (`RuntimeSymbols.def`, which `defineRuntimeSymbols` reads).
- **Their names are reserved.** A program that calls `sinf` and declares a function named `fminus_rt_sinf`,
  `fminus_rt_sin_f4` or `fminus_rt_libm_sinf` (or one of another builtin's three that it calls) is an error, in every
  mode: rename the function.

### Tests

- `tests/unit/MathTest.cpp`, the premise and the promise. Each four-lane function against its scalar one, lane by
  lane, over 10^6 vectors of mixed arguments: phases, audio's ranges, zeros of both signs, subnormals, infinities, NaN
  payloads, the floats either side of the limits, and any float at all (and, until recipe 23, libsystem_m's own
  lane-independence, where the runtime relied on it). Then, a test case a function, seven kernels (a loop that
  vectorises, a recursion that doesn't, a parameter's function, `float2`, `float4`, `float8` and `poly float` code),
  at `-O0` and `-O2`, at 1 to 64 lanes, the parameter a knob and frozen, in one call and in blocks of 61: every output
  `fminus::math`'s, bit for bit. The libm mode runs the same matrix against C++'s `std::`, and the fast mode is checked
  as it was. Compiling the promise in the fast mode makes them fail. Then
  the reserved names: five programs that declare one (the four-lane name of an `int` and of a `float`, a prototype of
  it, the scalar name with a body that may loop forever, the libm name), each an error with its note in every mode,
  through the code generator and the public API; and a program's own prototypes of the three, called by name beside a
  loop of `cosf`, left alone.
- `tests/library/ProgramTest.cpp`: through the public API in C++17, a block of all five against `fminus::math` in the
  default mode and against `std::` in the libm mode, at `-O0` and `-O2`.
- `tests/lit/fminus/math.fm`: each mode's IR (the vectorised loop and its remainder, `float2`, `float4`, `float8` and
  16- and 1-lane poly calls, a constant not folded, a frozen parameter's call out of the loop, the attributes and the
  flag, no `_simd_` outside the fast mode; since recipe 24 the kernels inline, a constant folded to the kernel's result
  and the frozen parameter's kernel gone from the loop), the assembly (two calls where the fast mode has one
  `__sincosf_stret`), `--run` at `-O0` and `-O2`, and `--math`'s error.
- `tests/lit/fminus/math-names.fm`: a program that defines `fminus_rt_sin_f4` of another type and calls `sinf` in a
  loop, which LLVM asserted on: the error and its note for an object file at `-O2` in each mode, and for `--run` in the
  default mode.
- Changed: `veclib.fm` runs `--math fast`; `VectorTest.cpp`'s recipe 8 test compiles in the fast mode, and its other
  `sinf` check is exact against `fminus::math`; `CodeGenTest.cpp` and `ModuleTest.cpp` compare with `fminus::math`.

### Measurements

The dev build on an M5, with other work running, so about ±10%. Nanoseconds a sample, the best of three runs; the fast
mode is the compiler before this recipe:

| Kernel | fast | vector | libm |
|---|---|---|---|
| biquad, `-O2` (a sine and a cosine a sample, in a recursion) | 2.72 | 4.63 | 2.88 |
| saturator: `tanhf` a sample at a time (vectorised) | 0.66 | 0.66 | 2.58 |
| parameters: swept a sample (the LFO's `sinf` and `expf` a sample) | 5.84 | 8.31 | 5.98 |

Where code is vector, the vector mode costs nothing: the saturator's loops run as fast as before, and now give the same
output as each other and as C++, in any blocks. Where a scalar call is on a recursion's critical path, it costs. A
call computes four lanes, its latency about 11–16 ns against libm's 2–10, so the biquad pays about 70% and the LFO
about 40%. The libm mode is as consistent, costs the biquad about 6% (the `__sincosf_stret` pairing it gives up), and
is about four times slower where the code is vector. So the default stays the vector mode, and the libm mode is the
one to recommend for a program of scalar recursive kernels (filters with coefficients a sample, LFOs, envelopes):
consistent too, and C++'s `std::` bit for bit.

Since recipe 24, with the kernels inline, the vector mode costs these programs little or nothing: the biquad 3.29 ns
(libm 3.05, fast 2.72), the swept parameters 7.4 ns where libm takes 7.5 and fast 7.3 (one session, on a busier
machine than the table's), and the saturator's `tanhf` loops 0.58 ns a sample (the examples' READMEs). The libm mode
stays the one whose results are C++'s `std::`.

## Recipe 16: knob versioning, and flat streams

*In the compiler. Code: `src/opt/KnobVersioning.h` and `KnobVersioning.cpp`, registered in `optimiseModule`
(`src/support/HostTarget.cpp`); the tags and the flat stream in `src/ir/Modules.cpp` (`tagDomain`, `atEntry`,
`opaqueUnit`, `lowerParameter`). Tests: `tests/unit/VersionTest.cpp` (`[versions]`), `tests/lit/fminus/versions.fm`,
and one check each in `params.fm` and `zeroinit.fm`.*

A mode is a knob that a loop branches on (`rational_tanh` is a function of the program, a ratio of polynomials):

```c
module Shape
{
    param int mode [[fminus::range(0, 2)]];   // a gain, a soft clip, a hard clip
    param float gain;

    void process(const float* restrict in, float* restrict out, int n) [[clang::nonblocking]]
    {
        for (int i = 0; i < n; ++i)
            out[i] = mode[i] == 0 ? gain[i] * in[i]
                   : mode[i] == 1 ? rational_tanh(gain[i] * in[i])
                                  : fminf(fmaxf(gain[i] * in[i], -1), 1);
    }
};
```

Recipe 14 reads `mode` once, where the method starts, so the branch on it goes the same way every sample; but LLVM
keeps it in the loop. At `-O2` it if-converts the loop and vectorises it, so every mode pays for the tanh: about
0.41 ns a sample, whichever is set. Taking the branch out of the loop means a copy of the loop for each way it goes,
which LLVM 23 makes only at `-O3` (`SimpleLoopUnswitchPass(/* NonTrivial */ Level == O3)`, to a cost of 50), and
FunctionSpecialization copies a function for a constant argument, never for a load.

FMinus knows three things LLVM doesn't. A knob is one value a call, so a copy picked where the method starts, by that
value, runs the whole call as the method would have. A knob's values are few and known: a `bool`'s two, an `int`'s
`fminus::range`. And the host decides how much code it will pay for. So the method gets one copy of its body for each
value, picked where it starts:

```llvm
; fminus versions.fm: Drive::process, a one-pole whose mode shapes its step
entry:
  %0 = getelementptr inbounds nuw i8, ptr %this, i64 8
  %mode = load atomic i32, ptr %0 unordered, align 8, !tbaa !2, !fminus.domain !5
  %cmp43 = icmp sgt i32 %n, 0
  switch i32 %mode, label %for.cond.preheader [          ; any other mode: the method as written
    i32 0, label %for.cond.mode0.preheader
    i32 1, label %for.cond.mode1.preheader
    i32 2, label %for.cond.mode2.preheader
  ]
```

In each copy `mode` is a constant, which the rest of the pipeline folds as it folds a frozen parameter: the shaper's
mode 0 is a vectorised gain, about 6.5 times faster. A stream gets the same treatment, through a new meaning for its
null pointer (below).

### What a knob can be: `!fminus.domain`

`atEntry` tags each knob's read with the values the knob can take:

| Parameter | The tagged value | Its domain |
|---|---|---|
| `param bool on` | `%on.on = icmp ne i8 %on, 0` | `!{i1 false, i1 true}` |
| `param int mode [[fminus::range(0, 2)]]` | `%mode = load atomic i32 ...` | `!{i32 0, i32 1, i32 2}` |
| a stream (below) | `%cutoff.flat = icmp eq ptr %cutoff.stream, null` | `!{i1 false, i1 true}` |
| a float, an int without a range, one whose ends are not integers, or one of more than 8 values | none | |

- **A hint, not a promise.** LLVM's own `!range` would make a value outside it poison, and a host may set any int
  (`setValue` never clamps to a range). `!fminus.domain` is FMinus's own kind, which LLVM ignores. A value outside it
  runs the method's original body, so a copy only ever runs with the value it was made for, and a wrong domain costs
  speed, never correctness.
- **A list, not a range**, so that an `enum`'s values can be one later.
- **At most 8 values** (`kLargestDomain`, `Modules.cpp`): a wider int's copies would cost more than its branch.

### The budget

`ir::Options::knobVersions` (8 by default) caps the copies of one method: the product of the domains it is versioned
on. 0 turns versioning off. A host sets it in `Program::Options::knobVersions`, and the driver takes
`--knob-versions N`. `ir::lower` writes it into the module as a module flag, `fminus.knob-versions` (behaviour `Min`),
where the pass reads it: the JIT optimises a module later, in its IR transform layer, where only the module arrives, so
the flag keeps `optimiseModule`'s signature as it was. A module without the flag (hand-written IR) gets 8.

The pass chooses greedily: first each stream's test for null, then the knobs in the order they are declared (their
slots' order, read from the load's offset from `this`). Each is taken while the product still fits, and one that does
not is skipped, not the end of the choice:

```cpp
        std::ranges::stable_sort(knobs, {}, [](const Knob& knob) { return std::pair(!testsStream(knob), knob.order); });
        std::vector<Knob> chosen;
        std::uint64_t copies = 1;
        for (const Knob& knob : knobs)
        {
            if (copies * knob.domain.size() > budget)
                continue;
            copies *= knob.domain.size();
            chosen.push_back(knob);
        }
```

So three ints over 0..3 and then a `bool` get 4 copies under a budget of 4, 8 (the first int and the bool) under 8,
and 16 (two ints) under 16. Streams come first because a test left out costs more than a knob left out. Uncopied, a
stream's read is a load at a stride the loop vectoriser checks is 1 before its vector loop, and a select of the value
(below): a stream with samples still vectorises, but a flat one runs LLVM's scalar loop, a load and a select a sample,
where its copy would have taken its work out of the loop. A knob left out leaves the method as it was. An int's
original body, for the values outside its domain, comes on top of the budget.

### The pass

**Where.** `optimiseModule` registers it, in a block of its own, at the early simplification extension point, from
`-O1`:

```cpp
        passes.registerPipelineEarlySimplificationEPCallback(
            [](llvm::ModulePassManager& modulePasses, llvm::OptimizationLevel level, llvm::ThinOrFullLTOPhase) {
                if (level != llvm::OptimizationLevel::O0)
                    modulePasses.addPass(llvm::createModuleToFunctionPassAdaptor(opt::KnobVersioningPass()));
            });
```

That is after the pipeline's first SimplifyCFG, SROA and EarlyCSE, and before IPSCCP and the inliner. Before the
inliner, so that each method is versioned on its own knobs, and the whole pipeline then simplifies each copy. After
SROA, because at the pipeline's very start the IR is the code generator's `-O0` form, where `const int m = mode[0];`
stores the knob to a stack slot, and the knob would not reach its branch through SSA values.

**Which knobs.** `KnobVersioningPass::run` takes each tagged instruction of the entry block, and keeps those that steer
a loop (`steersLoop`): whose value reaches, through casts, comparisons, logic on bools and `freeze`, the condition of a
branch, a switch or a select in a block that `LoopInfo` puts in a loop. A stream's test also steers a loop through
where its samples are: its base and its stride are selects made where the method starts, outside every loop, and when
a loop uses one (`addressesStream`: a select of a pointer, or of the stride's unit), the loop reads the stream. A
stream read only before the loop (`const float g = a[0];`) uses its base and stride there alone, and its read, a select
on the test outside the loop too, is data to the loop: it steers nothing, and leaves the budget to the knobs that do.
A knob that decides once, before the loop, or is data in it (`out[i] = in[i] * mode[i]`), gains nothing from a copy,
and is left alone. So is a function marked `optnone`, `optsize` or `minsize`, and one the pass has versioned already
(below).

**The copies** (`version`):

1. Split the entry block after the last chosen knob's read. What comes before (the knobs' reads, the streams' pointers
   and values, the streams' alias scope declaration) stays in the entry block, shared by every copy. Two things move
   first. What the entry block computes from a chosen knob before the last one's read (a stream's base and stride, which
   its test selects, when a chosen knob's read follows them: another stream's test, or an int's or a bool's read, of a
   parameter the method reads before the stream, since `atEntry` puts each parameter's reads at the top of the entry
   block) moves after it, into the body, so that each copy computes it with its own values (`sinkDerived`); an
   instruction that touches memory, or that the entry block still needs where it is, stays. And the static allocas after
   the split move up: an alloca outside the entry block is dynamic, and SROA no longer promotes it.
2. The body is every block the split's second half reaches. For each combination of the knobs' values, clone it block by
   block, with each knob mapped to its value:

   ```cpp
                for (std::size_t k = 0; k < knobs.size(); ++k)
                {
                    map[knobs[k].value] = values[k];
                    suffix += label(knobs[k], *values[k]);
                }
                ...                                    // each local cloned and mapped (below)
                std::vector<llvm::BasicBlock*> copy;
                for (llvm::BasicBlock* block : body)
                {
                    llvm::BasicBlock* clone = llvm::CloneBasicBlock(block, map, suffix, &function);
                    map[block] = clone;
                    copy.push_back(clone);
                }
                llvm::remapInstructionsInBlocks(copy, map);
   ```

   `CloneBasicBlock` copies each instruction's metadata, so the copies' accesses keep their TBAA tags, `!alias.scope`
   and `!noalias`. A scope declared inside the body (an inlined call's, or hand-written IR's) is cloned for each copy
   (`cloneAndAdaptNoAliasScopes`), and a loop's `!llvm.loop` is made anew for each, since no two loops may share one.
   The copies' names carry their values: `for.body.mode0`, `%arrayidx.cutoff.flat1`.

   Each copy has locals of its own: the entry block's static allocas that only the body uses are cloned for each copy,
   beside the original, and mapped as the knobs are (`%tmp.invert0.mode0`). A local the copies shared would be stored
   in one copy and loaded in all, so no copy's store would dominate its loads, and zero-init elision (recipe 18), which
   needs a local's memset to dominate its loads, would remove none. Each copy's locals, and the original body's, are
   marked live from the copy's start to its returns (`llvm.lifetime.start` and `llvm.lifetime.end`, `markLifetimes`):
   no two copies run at once, so the code generator gives their locals one stack slot (stack colouring), and the frame
   does not grow with the copies. Seven copies of a 16 KB array take a 16 KB frame; unmarked, they took 112 KB.

   In each copy a stream's stride, whose test is now a constant, becomes a constant too: 0 where the stream is flat, and
   1, the opaque unit's value, where it has samples (`foldUnits`; flat streams, below). So the copy for a stream's
   samples reads them a sample after another, as the method did before a stream could be flat.
3. Pick the copy where the entry block branched to the body (`choose`): a switch on each int, whose default is the
   original body, and a branch on each bool, nested, the first knob's values slowest.
4. When every chosen knob is a bool, no value falls back, and the original body would be dead. It becomes the first
   combination's copy instead, so a stream's copy for samples is the method as it was, with its names.

The function gets `"fminus-knob-versions"="3"`, its number of copies, which the tests read, and which the pass looks
for first: it leaves a function that has it alone. FMinus's optimised IR, optimised again (`fminus-build -O3`, say),
still has its knobs tagged, and an int's fallback still branches on one, but it is not versioned twice. Everything
happens inside the one function, so it keeps its name, its attributes, its callers and its effects: a
`[[clang::nonblocking]]` method calls nothing in its copies that it did not call before.

### Flat streams

A stream is a buffer of samples a call (recipe 14). A host often has nothing to put in it: a smoother that has settled,
an automation lane that holds still. Before, it filled a buffer with one value anyway, and the method computed the
stream's per-sample work (a biquad's coefficients) for each sample. Now a null stream is a flat call:

```cpp
fminus::Program::setValue(instance, cutoff, 1000.0);         // the value a flat call reads
fminus::Program::setStream(instance, cutoff, nullptr);       // this call is flat: cutoff[i] is 1000 for every i
process(instance, in, out, n);
```

Before, a null stream was a null dereference. `atEntry(field, true)` now loads, where the method starts, the stream's
pointer, the slot's value with a knob's unordered atomic load (`%cutoff.value`), and the test `%cutoff.flat`, tagged as
a bool is. From the test it chooses there where the samples are: the base, `%cutoff.base`, the stream's pointer or,
when it is null, the slot's value field; and the stride, `%cutoff.stride`, 1 or 0. `flatStreams_` keeps the test, the
value, the base and the stride for the method's reads. Each `cutoff[i]` is then a load from base + i * stride, in the
streams' alias scope, and a select of the value when the stream is flat, with no branch:

```llvm
; fminus versions.fm -O0 --stream Biquad::cutoff
entry:
  ...
  %cutoff.stream = load ptr, ptr %0, align 8, !tbaa !6, !noalias !18
  %1 = getelementptr inbounds nuw { ptr, float }, ptr %cutoff.slot, i32 0, i32 1
  %cutoff.value = load atomic float, ptr %1 unordered, align 4, !tbaa !8, !noalias !18
  %cutoff.flat = icmp eq ptr %cutoff.stream, null, !fminus.domain !14
  %cutoff.base = select i1 %cutoff.flat, ptr %1, ptr %cutoff.stream
  %cutoff.unit = call i64 asm "", "=r,0"(i64 1) #4                 ; 1, which LLVM cannot see
  %cutoff.stride = select i1 %cutoff.flat, i64 0, i64 %cutoff.unit
  ...
for.body:
  ...
  %cutoff.offset = mul nuw nsw i64 %idxprom, %cutoff.stride
  %cutoff.at = getelementptr inbounds float, ptr %cutoff.base, i64 %cutoff.offset
  %cutoff = load float, ptr %cutoff.at, align 4, !tbaa !8, !alias.scope !18
  %cutoff.read = select i1 %cutoff.flat, float %cutoff.value, float %cutoff
```

The load is always valid: a flat stream's reads all load the value field, and the select discards what they load. So
it does not matter that they are plain loads, which the host's thread may race (in LLVM a plain load that races a store
reads an undefined value, not undefined behaviour), nor that they are in the streams' alias scope though the field is
no stream's: a store the method made through a pointer cast from `this` could reach the field unseen, and change only
what the select discards.

The stride's 1 is `%cutoff.unit`, an empty inline asm statement whose result is its operand, in the same register
(`opaqueUnit`, with the constraints `"=r,0"`, `kUnitConstraints`): it emits no instruction and touches no memory, but
LLVM cannot see that it is 1. A stride that LLVM can see is 0 or 1 does not last: InstCombine makes `i * stride` a
select of 0 and `i`, and the loop vectoriser then sees no strided load, so a loop over a stream that knob versioning did
not copy would stay scalar. Opaque, the stride stays a multiplication, and the vectoriser versions the loop on it
itself: it checks at run time that the stride is 1 and runs its vector loop, or, for a flat stream, its scalar loop, a
load and a select a sample. Two kinds of loop rely on it, besides every loop over a stream when versioning is off: one
over more streams than the budget takes (`versions.fm`'s `Mix`: five streams, three tests chosen, the other two read at
a checked stride in each copy), and the caller's loop that a method without a loop of its own, which the pass gives no
copies, is inlined into: the stream's base and stride leave the loop with the rest of its setup, and the loop vectorises
(`versions.fm`'s `Rack`, which calls `Amp::apply` a sample). An asm statement is not speculatable, so where the inlined
call sits under a condition in the loop, LICM leaves the unit in the loop.

In `Biquad` the test steers the loop, so the method gets two copies. In the flat one the read is `%cutoff.value`, a
register, and LICM takes everything computed from it out of the loop; in the other it is the load alone, at a stride of
1, and the loop is the method as it was:

```llvm
; fminus versions.fm --stream Biquad::cutoff
entry:
  ...
  br i1 %cutoff.flat, label %for.cond.cutoff.flat1.preheader, label %for.cond.preheader

for.body.cutoff.flat1.lr.ph:                      ; the flat copy, before its loop
  %1 = fmul float %cutoff.value, f0x40C90FDB
  %2 = fdiv float %1, 4.800000e+04
  %3 = tail call float @fminus_rt_cosf(float %2)
  %4 = tail call float @fminus_rt_sinf(float %2)
  ...                                             ; and five more divides
for.body:                                         ; the stream's copy: as before
  %cutoff.at = getelementptr inbounds nuw [4 x i8], ptr %cutoff.stream, i64 %indvars.iv
  %cutoff = load float, ptr %cutoff.at, align 4, !tbaa !6, !alias.scope !38
```

### What it changes for programs and hosts

- **Options.** `Program::Options::knobVersions` and `ir::Options::knobVersions` (8), and `fminus --knob-versions N`.
- **Streams.** `setStream(instance, parameter, nullptr)` makes a call flat: every `cutoff[i]` is then the slot's value,
  as `setValue` last set it, and no sample is read.
- **Bit for bit.** Each copy computes what the method computes with its value: the tests' outputs are identical,
  versioned or not, for every value in a domain and for an int outside it. In the vector and libm math modes that
  holds for math calls too (recipe 15). In the fast mode two things can still move a math call's last bit: a copy's
  loop may vectorise a `sinf` that the method's kept scalar, and a knob's constant may reach one, which LLVM then folds
  with the host's double functions.
- **Code size and compile time** grow with the copies (below). A method that no knob steers is unchanged:
  `examples/voices/` has float knobs only.
- **The IR.** Every module has the flag; `fminus -O0` shows the tags, and each stream's base, unit and stride; the
  copies' names carry their values. Where the unit is still used (at `-O0`, for a stream the pass gave no copies, in an
  int's fallback), its asm statement shows in `--emit asm` as an `InlineAsm Start` and `InlineAsm End` comment, with no
  instruction of its own.
- **Knob versioning off** (`knobVersions = 0`), or a stream the budget leaves out, costs a flat stream its copy: a loop
  over a stream with samples still vectorises, on the vectoriser's check that its stride is 1, but a flat one runs
  LLVM's scalar loop, a load and a select a sample, with its work still in the loop. Four streams in one loop
  (`out[i] = in[i] * a[i] + b[i] * c[i] + d[i]`) took about 0.10 ns a sample with samples in all four, and 0.42 with the
  fourth, the one a budget of 8 leaves out, flat (one run each, on a loaded machine). The pass takes the streams' tests
  first, in the order they are declared, so the k-th stream a method's loops read gets its copies from a budget of 2^k:
  where a flat stream runs in a vectorisable loop, give the budget that much.

### Tests

- `tests/unit/VersionTest.cpp`: a mode and a bool over a shaper, whose 6 copies give the unversioned output for every
  combination, bit for bit, and C++'s; an int outside its range (-1, 3, 7, `INT_MIN`, `INT_MAX`) runs the original body;
  the budget from 0 to 200 over three ints and a bool; a stream's test chosen before an int over 0..7, its loads still
  vectorised; what is not versioned (a knob that decides before the loop, one that is data in it, a float, a wide int,
  an int without a range or with one whose end is not an integer, a method without a loop, a frozen parameter); a
  nonblocking biquad that stays nonblocking and calls nothing new; an FIR whose length is a knob, with a local array,
  valid at `-O1` to `-O3`; a null stream that reads the slot's value at `-O0` and `-O2`, versioned or not; a stream read
  only before the loop, not versioned on, so the mode that steers the loop gets its 3 copies; and the pass alone on
  hand-written IR: a scope declared in the body and a loop ID, one of each per copy; the pass run twice, the second time
  leaving the function as it was; and two streams whose bases and strides alone reach the loop, versioned on, each copy
  with its own, constant. Then five streams in one loop under budgets of 0, 2 and 8, giving 1, 2 and 8 copies, each
  stream read four samples at a time somewhere in the method (`vectorStreams`), and every combination of flat streams
  against C++, bit for bit; a stream read in a method without a loop, inlined into its caller's loop, which vectorises,
  at budgets 8 and 0; and a local array in 6 copies, each copy's own, in the entry block with its lifetime marked,
  zeroed in no copy at `-O1` to `-O3` (the fallback keeps its memset at `-O3`), the outputs the unversioned method's and
  C++'s.
- `tests/lit/fminus/versions.fm`: the tags and the branch-free flat lowering at `-O0` (base, unit, stride, the load
  and its select), the module flag; at `-O2` the switch, one loop a mode with no trace of the mode, the original body
  still switching; no copies for a method no knob steers; the budget at 8 and 64; `-O1`; `--knob-versions 0`, with no
  function versioned; the stream's copy as before, at a stride of 1 with no select, and the flat copy with its
  coefficients before its loop; five streams, eight copies and the other two streams' strides checked, and the same
  loop unversioned (`Mix`); and a stream read in a method without a loop, inlined into a vectorised loop (`Rack`).
- `tests/lit/fminus/zeroinit.fm`'s `Shaped`: a local array in each copy of a method versioned on a bool and a mode,
  none zeroed at `-O1` to `-O3`, and at `-O1` and `-O2` no memset in the method at all, its fallback included.
- `tests/lit/fminus/params.fm`: `Gain::process`, its gain a stream and `on` a bool, is now versioned on both, and its
  copy for `on` with samples vectorises: the check reads a `<4 x float>` load in the streams' scope.

### Measurements

The dev build on an M5, with five other builds running, so about ±10% and more; the best of 30 runs, measured before
recipe 15, with LLVM's intrinsics for `sinf` and `cosf`, and before a stream's read lost its branch: the biquad's
unversioned rows branched on `%cutoff.flat` a sample, where its read is now a load and a select. "Unversioned" is
`knobVersions = 0`. Every pair gave the same output, bit for bit.

| Kernel | Versioned | Unversioned | |
|---|---|---|---|
| the shaper above, mode 0 (a gain), calls of 256 | 0.062 ns/sample | 0.407 | 6.5× |
| the shaper, mode 5 (outside the range: the original body) | 0.407 | 0.407 | |
| a biquad, its cutoff flat, calls of 64 and 256 | 2.29–2.36 | 2.72–2.76 | 15% |
| the same, its cutoff samples | 2.70–2.76 | 2.73–2.77 | the same: this copy is the old code |
| an FIR of 8 taps, its length a knob over 1..8 | 1.02 (frozen: 1.02) | 1.60 | 36% less time |
| `examples/parameters`' filter, `shape` over 0..2 | 3.44–3.49 | 3.43–3.45 | latency-bound: nothing |

The copies cost code and compile time. The shaper's `__TEXT` grows from 960 bytes to 2072, and its `Program::compile`
from about 6 ms to 12; `examples/parameters/` from 416 bytes to 756 (and 5.5 ms against 3.7).

## Recipe 17: C's strict aliasing, told to LLVM (TBAA)

*In the compiler. Code: `src/ir/Tbaa.cpp`, and the loads and stores in `Memory.cpp`, `Calls.cpp` and `Modules.cpp`.
Examples: `examples/convolver/convolver.fm` and `examples/delay/echo.fm`, which now index their fields in their loops.
Tests: `tests/unit/TbaaTest.cpp` (`[tbaa]`), `tests/library/StrictAliasingTest.cpp`, `tests/lit/fminus/tbaa.fm`.*

A method's loop over its module's buffers is the most ordinary code in DSP:

```c
module Acc
{
    float* tail;
    float* result;
    int size;

    void add() [[clang::nonblocking]]
    {
        for (int i = 0; i < size; ++i)
            tail[i] += result[i];
    }
};
```

Without help, LLVM runs it badly. `tail[i] += ...` stores a `float` through a pointer that came out of the module, and
for all LLVM knows that pointer points at the module itself: at `tail`, `result` or `size`. So after every store it
loads all three fields again, and a loop that reloads its own bounds and bases can't run on vectors. At `-O2` every
iteration was this:

```llvm
for.body:
  %1 = load ptr, ptr %result, align 8
  %2 = load float, ptr %arrayidx, align 4
  %3 = load ptr, ptr %this, align 8
  ...
  store float %5, ptr %arrayidx8, align 4
  ...
  %6 = load i32, ptr %size, align 8
```

C has the answer, and clang gives it to LLVM: the strict-aliasing rule. An object is accessed only through an lvalue
of its own type, so a store of a `float` cannot change an `int` or a pointer. Clang tags every load and store with its
type (`!tbaa` metadata), and LLVM's type-based alias analysis (`TypeBasedAA`, in every default pipeline) reads the
tags. FMinus now does the same. With the tags, the fields are loaded once, before the loop, and the loop vectorises,
with a run-time check that the two buffers don't overlap:

```llvm
iter.check:
  %result = getelementptr inbounds nuw i8, ptr %this, i64 8
  %1 = load ptr, ptr %result, align 8, !tbaa !5
  %2 = load ptr, ptr %this, align 8, !tbaa !5
  ...
vector.body:
  %wide.load = load <4 x float>, ptr %4, align 4, !tbaa !7, !alias.scope !9
  ...
  store <4 x float> %12, ptr %8, align 4, !tbaa !7, !alias.scope !12, !noalias !9
```

It is about 4.8 times faster at 64 elements and 7.3 times at 512. The convolver and the echo no longer copy their
fields into locals before their loops: written the obvious way, they now run as fast as the locals did.

### The rule

The rule is a language decision, not only an optimisation: TBAA is sound only for programs that keep it. FMinus keeps
C's (C11 6.5p6-7, the effective type), with FMinus's types in it:

- **Memory is read and written as the type it holds.** A `float` through `float` lvalues, an `int` through `int`s, a
  `bool` through `bool`s, a pointer through pointers (of any pointee, `void*` too). A float vector or a `poly float`
  counts as its elements' type, `float`: a `float4*` over a float buffer, `poly_load` and `poly_store`, and the FFT's
  `float2` bins all read and write float memory, and floats read them back. Lane masks count as `bool`s.
- **Anything else is undefined**: an `int` read as a `float` through a `void*`, or a module's memory reached as floats
  through one (`void* v = this; float* f = v; f[0] = 1;`).
- **Modules and arrays are their fields and elements.** Each is accessed as its own type. A module's copy (`a = b`) is
  a byte copy, which may read and write any type, as a `char` copy may in C.

FMinus is stricter than C in three places:

- **Allocated memory keeps one type at each place**, from its first access until it is freed. C lets a store give
  memory from `malloc` a new effective type; in FMinus that is undefined. Free it and allocate again to change its
  type.
- **There is no character type**, so nothing but a module's copy (and a declaration's zeroing) reads or writes bytes of
  any type. C's `char` lvalues may alias anything; FMinus has none to offer.
- **Vectors are their elements' type**, the OpenCL C reading: C has no vectors, and a float vector must reach the floats
  it covers.

**Turning it off**, as clang's `-fno-strict-aliasing` does: `fminus --no-strict-aliasing`,
`Program::Options::strictAliasing = false` or `ir::Options::strictAliasing = false`. Nothing is then tagged, and the IR
is what FMinus wrote before this recipe, byte for byte (checked for every example and lit test at `-O2`). It is for a
program that does read memory as another type; its loops over a module's buffers then run as slowly as they did.

### A tree of four types

LLVM's TBAA metadata is a tree of type nodes, and each access is tagged with a node: two accesses whose types are
different nodes, neither an ancestor of the other, don't alias. FMinus's tree is a root with four scalar types under
it, in clang's style and with clang's names:

```llvm
!4 = !{!"FMinus TBAA"}                 ; the root
!3 = !{!"int", !4, i64 0}
!6 = !{!"any pointer", !4, i64 0}
!8 = !{!"float", !4, i64 0}            ; float, float2, float4, float8, poly float
!2 = !{!3, !3, i64 0}                  ; a tag: base type, access type, offset (here int's)
```

and `bool`'s node, for `bool` and the lane masks, when a program uses one.

- **Its own root.** Tags from two trees are taken to alias, so FMinus's rule never reaches C++ code, should the two
  meet in one module.
- **One pointer type.** FMinus has no pointer to a pointer, so a pointer in memory is only ever reached as itself, and
  telling a `float*` field from an `int*` one would buy nothing.
- **Scalar tags.** Each tag's base type is its access type, at offset 0. Struct-path tags (clang's `%struct.S` nodes
  with a field offset) would also tell two `float` fields of one module apart; BasicAA already does that when both are
  reached from the same `this`, and no measured loop needed more. No tag is marked immutable: memory a `const float*`
  reads may change through another pointer.

`Tbaa.cpp` builds the nodes with `llvm::MDBuilder`. The context uniques metadata, so asking again for a type's tag gives
the same node, and the tree appears once a module, made of the nodes the program uses:

```cpp
    llvm::MDNode* Lowering::accessTag(ast::Type type)
    {
        const char* name = tbaaTypeName(type.kind);
        if (name == nullptr)
            return nullptr;
        llvm::MDBuilder md(module_.getContext());
        llvm::MDNode* access = md.createTBAAScalarTypeNode(name, md.createTBAARoot("FMinus TBAA"));
        return md.createTBAAStructTagNode(access, access, 0);
    }
```

`tbaaTypeName` is a `switch` over `ast::Kind` with no `default:`, so a new kind of type stops the build there until it
says which node it belongs to.

### Where the tags go

Every load and store of a value the code generator writes calls `tag(access, type)` with the FMinus type of what is in
memory:

| Where | What | Tagged as |
|---|---|---|
| `read()`, `write()`, `Memory.cpp` | a variable, `p[i]`, `*p`, a field; components load and store the whole vector | its type (`whole`) |
| `initializeArray`, `Memory.cpp` | `float c[4] = {1, 2};`, each element's store | the element's type |
| `lowerPolyMemory`, `Calls.cpp` | `poly_load`, `poly_store`, and their masked forms | `float` |
| `atEntry`, `Modules.cpp` | a knob's value, a stream's pointer and value | the parameter's type; `any pointer` |
| `lowerParameter`, `Modules.cpp` | a stream's sample, `cutoff[i]` | `float`, beside its alias scope |
| `self()`, `Modules.cpp` | `this`, from its stack slot | `any pointer` |

A masked load or store is a call, and LLVM reads a call's `!tbaa` as it reads a load's (`TypeBasedAAResult::
getModRefInfo`), so the masked forms carry it too. The loop vectoriser keeps the scalar tag on the vector loads and
stores it makes, which is why a float vector is a `float` access in the first place. A stream's sample keeps its alias
scope (recipe 14): the two kinds of metadata say different things, and LLVM uses both.

What stays untagged is safe, since an untagged access may alias anything: a module's copy (a `memcpy`) and a
declaration's zeroing (a `memset`), bytes of whatever types; and the stores that start a stack slot, a parameter's
(`Functions.cpp`) and a scalar declaration's (`Statements.cpp`), which mem2reg removes from `-O1` on. A unit test walks
a lowered program that uses every kind of access, and fails on any other untagged load or store, so a new lowering
that forgets `tag` shows up there.

### What LLVM does with it, and what it still can't

- **The fields leave the loop.** LICM and GVN hoist a pointer or int field's load past `float` stores, and the loop
  vectorises (`tbaa.fm`'s `Acc::add`, the convolver's four loops in `convolve`).
- **An int field written in a loop of float stores stays in step.** The echo's `position` is loaded once, before the
  loop. Its store stays in the loop, untagged: SimplifyCFG speculates `if (++position == length) position = 0;` into
  one store of a `select`, and drops the tag doing it. It costs nothing measurable here.
- **A call still clobbers everything.** `Convolver::process` loads `ready`, `gathered` and `block` again every sample,
  because it calls `convolve()` (not inlined), which could write them as far as LLVM knows. TBAA says nothing about
  calls; a summary of the fields each method writes would.

### For programs and hosts

- **Programs** that keep the rule compute what they did, bit for bit: every example's `--check` still passes, the
  convolver and the echo bit-identical to their C++ references. A loop over a module's buffers may index its fields
  directly; the advice to copy fields into locals first is obsolete.
- **Hosts** get `Program::Options::strictAliasing` (on by default). Nothing changes at the boundary: the host's own C++
  is compiled apart from the JIT's code, and what a host writes into an instance (`setValue`, `setDefaults`,
  `setStream`) is written as the types the program reads.
- **The IR.** Every load and store prints a tag: a check that matches such a line to its end, or a metadata number,
  changes (`params.fm`'s stream checks accept `!tbaa {{![0-9]+}}` before `!alias.scope` and `!noalias`).

### Tests

- `tests/lit/fminus/tbaa.fm`: at `-O0`, the tags on a module's field loads, its buffer's loads and stores, a knob of
  each type, a bool variable, an `int*`'s store and load, an array's list, a `float4` through its pointer and through
  components, `poly_load` and `poly_store` masked and not; the `memset` and a module copy's `memcpy` with none; the tree
  node by node. At `-O2`, `Acc::add` with its field loads before the loop and a `vector.body`, and a float's store that
  can't change an int, so `count[0] - before` folds to `ret i32 0`. A stream's pointer and samples. And
  `--no-strict-aliasing`: no `!tbaa`, the fields loaded in the loop, no `vector.body`.
- `tests/unit/TbaaTest.cpp`, every program under both settings, at `-O2`, against C++: `Acc::add` with a gain and a
  counted int field, at sizes 0 to 513, over two buffers, one buffer, and two overlapping by a float; a delay whose
  write position is a pointer field it moves while storing floats through it; floats and float vectors over one
  memory, at 4 and 8 lanes; the FFT's `float2` bins in `calloc`'d memory, written as floats and as `float2`s; a
  module's copy among tagged field accesses; and the structural check above, which also counts exactly four types.
- `tests/library/StrictAliasingTest.cpp`: `Program::Options::strictAliasing` defaults to on, and off gives the same
  result, in memory the host owns.

### Measurements

The dev build on an M5, while five other builds ran, so about ±10%:

- **`Acc::add`**, ahead of time (`fminus --emit obj`, linked with a C harness, the best of 15): 0.33 ns an element
  before, 0.069 with the tags at 64 elements (4.8×) and 0.046 at 512 (7.3×). `--no-strict-aliasing` is the before.
- **The convolver** (`example-convolver`, 2048 taps, blocks of 256, the best of 20 runs): its fields indexed in the
  loops took 15.6 ns a sample before and 9.7 with the tags, the same as with its fields in locals (9.8), and C++ 10.5.
  With the tags, `convolve` vectorises the same four loops the locals did.
- **The echo** (`example-delay --module`): its fields indexed took 2.10 ns a sample before, 0.70 with the tags, as the
  plain-C functions and C++ do.
- **Everything else**: the optimised IR of every other example is the same with the tags stripped as without the rule.
  They had been written around the gap, or had no loop it touched.

## Recipe 18: zero-init elision, an optimisation of FMinus's own

*In the compiler. Pass: `src/opt/ZeroInitElision.h` and `ZeroInitElision.cpp`, registered in `optimiseModule`
(`src/support/HostTarget.cpp`). Tests: `tests/unit/ZeroInitTest.cpp` (`[zeroinit]`), `tests/lit/fminus/zeroinit.fm`.*

FMinus starts every variable at 0, where C leaves a local indeterminate (the README's deviations). For an array that is
a memset where it is declared (`zero()`, `Memory.cpp`), and the memset runs every time the declaration does: once a
call, or once an iteration for an array declared in a loop body. A scratch buffer pays for it on every block:

```c
module Smooth
{
    float y;
    void process(const float* restrict in, float* restrict out, int n) [[clang::nonblocking]]
    {
        float tmp[256];                   // zeroed, all 1 KB, every call
        for (int i = 0; i < n; ++i)
            tmp[i] = in[i] * 0.5f;        // ... then overwritten, as far as n
        for (int i = 0; i < n; ++i)
        {
            y += 0.01f * (tmp[i] - y);    // ... before anything reads it, no further than n
            out[i] = y;
        }
    }
};
```

LLVM's DSE removes a memset that later stores are known to overwrite, but only when it sees that they cover it: here
they cover `n * 4` bytes, `n` known only at run time, so the memset stays at every level. Only a constant bound helps,
and only at `-O3`, which unrolls the loop fully. Clang never has the problem, since C does not zero the array; under
`-ftrivial-auto-var-init=zero` it has, and LLVM's `MoveAutoInitPass` then moves the zeroing towards its uses, but never
removes it. The zeroing is FMinus's rule, so removing it where nothing can see it is FMinus's optimisation: a pass of
its own, which says a memset is dead when no load can read a byte it stored.

### The proof

`ZeroInitElisionPass` is a function pass (`PassInfoMixin`, recipe 6). For each local, an entry-block static `alloca`,
it first follows the address through `getelementptr`s and collects what the function does with it: loads, stores,
memsets, memset.patterns, memcpys and memmoves into it, comparisons of addresses and lifetime markers. Any other use (a
call's argument, a phi, a store of the address itself, a memcpy out of it) is an escape, and the local keeps its
memsets. This scan asks for no analysis, so a function without a memset of a local costs nothing more.

Then it looks for a **writer**, something that writes every byte of `[0, end)`:

- a **loop** that stores element i on every iteration i, counting from 0 by one element: the store's pointer, as a
  SCEV less the local's address, is `{0,+,size}<loop>`, its block dominates the latch, and the latch is the loop's only
  exit. It writes `size * (count + 1)` bytes, `count` being its exact backedge-taken count;
- a **fill**: a memset, memset.pattern, memcpy or memmove whose destination is the local's start, which LLVM's loop
  idiom pass makes of `for (...) t[i] = in[i];` and `for (...) t[i] = 1.5f;`.

A memset M can go when some writer proves three things, about the function as it is:

1. **Order.** M dominates every load, and none is reachable from M without completing the writer. The search walks
   blocks from just after M, and stops at M again, at the loop's exit block or at the fill:

   ```cpp
                    for (llvm::BasicBlock* next : llvm::successors(&current))
                    {
                        if (next == writer.exit || (skipping && &current == writer.guard && next == writer.skip))
                            continue;
                        if (queued.insert(next).second)
                            work.push_back(next);
                    }
   ```

   A load inside the writer's loop is found (a read before the write, `t[i] = t[i] + x`), and so is one on a path that
   goes around it.
2. **Stability.** The values `end` is computed from have, at each load, the values the writer saw. A leaf of `end`'s
   SCEV whose definition dominates M is never defined again between M and a load; any other must pass the same search
   from its definition. That covers a bound read from a module's field after the declaration (`i < count`).
3. **Range.** Every load reads within `[0, end)`. A load in a loop entered after M, at `{start,+,step}<loop>`, reads
   furthest on its last iteration; the pass evaluates it there (`evaluateAtIteration`) and asks ScalarEvolution whether
   that, plus the load's size, is at most `end` (`isKnownPredicateAt`). For the usual shape both are `4 * (zext n)`.

**The zero-trip case.** A rotated loop sits under a guard, `if (n > 0)`, whose other edge goes around it. At `-O2` jump
threading has usually sent that edge past the reading loop too. At `-O1` it joins the loop's exit, and the reading loop
has a guard of its own, `n > 0` again. The pass then tries a second proof: the skipping edge is a stop as well, and
every load reachable from it must be known to need the guard's condition, which a skip, taken only when `n <= 0`, rules
out. With `n == 0` nothing is read, and nothing needs the zeros.

**The ranges are exact.** An access outside the local is undefined behaviour, so in an execution without any, every
offset the writer and the loads use lies inside it. The pass checks, in 128-bit arithmetic, that the writer's largest
length and each load's furthest offset stay below 2^62, so no 64-bit SCEV it compares wraps. The top of
`ZeroInitElision.cpp` has the argument that the three proofs are enough, in a paragraph.

### Where it runs

```cpp
        passes.registerScalarOptimizerLateEPCallback(
            [](llvm::FunctionPassManager& functionPasses, llvm::OptimizationLevel level) {
                if (level != llvm::OptimizationLevel::O0)
                    functionPasses.addPass(opt::ZeroInitElisionPass());
            });
```

The scalar-optimiser-late extension point is the end of the function simplification pipeline, which runs in the
inliner's CGSCC walk, on each function after its callees are inlined into it. By then the loop passes have rotated the
loops, made their induction variables canonical, and turned copy and fill loops into memcpys and memset.patterns; at
`-O2` and `-O3` LICM's loop adaptor has just put every loop into loop-simplify form, so the analyses the pass asks for
are fresh, and DSE has removed what it can (`-O1` has no DSE, and the pass removes a memset of a local nothing reads
too). The loop vectoriser has not run yet: it would split a store loop into a vector loop and a remainder, a shape the
pass does not prove. A method's memset goes before the method is inlined into its caller.

`-O0` has no simplification pipeline, but `buildO0DefaultPipeline` still runs the passes registered at this extension
point, so the callback adds the pass from `-O1` only. At `-O0` every memset stays, and `fminus x.fm -O0` shows exactly
what the lowering wrote. The pass would prove little there anyway: a loop's index is loaded from its stack slot on
every iteration, which ScalarEvolution cannot count, so no loop is a writer.

### What it changes

- **For programs**, nothing they can observe: an element nothing wrote still reads 0. A local array whose reads all
  follow a writer no longer pays for its zeroing.
- **For hosts**, nothing: no option. The pass is semantics-preserving, so there is nothing to choose. `fminus -O0`
  shows every memset the code generator writes.

What it leaves, by design or for later:

- **A partial cover keeps the whole memset.** `for (i < n - 1)` writes and `for (i < n)` reads: element `n - 1` needs
  its 0, and so, as far as the pass knows, does every other.
- **A writer is one loop, or one fill.** Two loops that each write half, a loop writing two elements an iteration, or a
  nest (`t[j * n + i]`) keep the memset; so do reads two loops deep (`t[i + j]`).
- **A store loop must not break early, nor store conditionally**, and must start at element 0.
- **A module written field by field.** A module declared as a local is zeroed by a memset too, and the pass takes it as
  any local: its memset goes when a copy of a whole module (`M b = a;`, or `b = a` before any read of `b`) writes it,
  a fill like any other. Its fields written one by one are stores at constant offsets, which no writer covers; SROA and
  DSE already handle them.

### Tests

- `tests/unit/ZeroInitTest.cpp`: the memset gone, at `-O1`, `-O2` and `-O3`, for `Smooth`, the same method inlined into
  a loop over four voices, a bound read from a field, a constant bound, a copy and a fill, an array declared in a
  loop, and one declared before a loop whose iterations write and read; their results against the same filter with
  the buffer a field, bit for bit, at every block size from 0 to 256. Then every case that must keep its memset, run
  through the JIT at each level: writes one short, a read before the write, a conditional store, a `break`, every other
  element, from element 1, reads to `i <= n` or to `t[n]`, `t[0]` after a loop that may not run, an array an FFT reads
  whole; and `ArrayTest.cpp`'s zeroed arrays again. Last, the pass alone on hand-written IR.
- Every call runs on a **dirty stack**: `dirtyStack()` fills 64 KB below the caller with 1e6, and `padded()` calls the
  function from below a kilobyte of padding, so a wrongly removed memset reads 1e6, not a lucky 0. A test of a
  hand-written local nothing zeroes checks that the harness works: all 256 of its elements read 1e6 (skipped under
  ThreadSanitizer, whose runtime writes that stack). A pass that removed every memset fails 204 checks.
- `tests/lit/fminus/zeroinit.fm`: the IR at `-O0` (every memset) and `-O1` to `-O3`: no memset in each covered function,
  with the memcpy and memset.pattern left as the writers, and the memset kept in each uncovered one, an array passed to
  an external C function and to the FFT included; and `Shaped`, a method that knob versioning copies, whose copies'
  arrays (`%tmp.invert0.mode0`, ...) lose their memsets.
- `tests/unit/VersionTest.cpp`: a local array in each of 6 copies of a versioned method, each copy's own, with no
  memset left in any copy at `-O1` to `-O3`, nor in the fallback but at `-O3` (recipe 16).

### Measurements

`zeroinit.fm`'s `Smooth`, built by `fminus --emit obj` (`-O2`) and timed by a C harness, the best of 7 runs of 20
million samples, on an M5 with other builds running (about ±10%):

| Block size | before | after | the array a field instead |
|---|---|---|---|
| 16 | 2.33 ns/sample | 1.68 (28% less) | 1.67–1.71 |
| 64 | 1.93 | 1.71 (12%) | 1.69–1.73 |
| 256 | 1.76 | 1.68 (5%) | 1.68 |

With the memset gone, a local array costs what a field does: the difference was the 1 KB memset. A `float t[64]` written
and summed by constant-bound loops went from about 14 ns a call to 10. Compile times did not move beyond the noise.

## Recipe 19: what the runtime's calls and loops promise

*In the compiler. Code: `src/ir/Calls.cpp` (`kRuntimeFacts`, `describeRuntime`, `callRuntime`),
`src/ir/Functions.cpp` (`describeLibm`) and `src/ir/Statements.cpp` (`isConstant`, `mustProgress`). Tests:
`tests/unit/AttributeTest.cpp` (`[attributes]`, `[progress]`), `tests/lit/fminus/runtime-attributes.fm`,
`tests/lit/fminus/libm-prototypes.fm`, `tests/lit/fminus/progress.fm`; `HeapTest.cpp` and `heap.fm` changed.*

LLVM optimises what it can prove, and it proves little about a call to a function it can't see, or about a loop it
can't count. FMinus knows three things LLVM doesn't:

- **What the runtime's functions do.** It wrote them (`src/runtime/`). It declared them bare,
  `declare void @fminus_rt_fft_forward_real(ptr, ptr, ptr)`, so LLVM had to assume that an FFT may read and write every
  field of every module and all of the heap, and that `calloc` is just some function that returns a pointer.
- **That Apple's libm sets no errno.** A prototype, `float asinf(float x);`, calls libm's function, which LLVM takes to
  write `errno`, as C allows it to; so a call of it never left its loop, even on a still dial.
- **That its loops end.** C11 says an implementation may assume it (6.8.5p6), and clang tells LLVM so. FMinus said
  nothing.

Each is a fact on a declaration or a branch, and none adds a pass: LLVM's own passes do the rest.

### The runtime's declarations

Every call of the heap's and the FFT's runtime functions goes through `callRuntime`, the heap's too now
(`lowerAllocation`), which declares the function from its arguments' types and gives the declaration its attributes
(the math builtins' functions have theirs from `mathFunction`, recipe 15):

```cpp
        llvm::FunctionCallee function =
            module_.getOrInsertFunction(name, llvm::FunctionType::get(result, types, false));
        if (auto* declared = llvm::dyn_cast<llvm::Function>(function.getCallee());
            declared != nullptr && !functions_.contains(name))
            describeRuntime(*declared);
```

The test of `functions_` matters. A program may declare a function with the runtime's name itself (recipe 5 calls
`fminus_rt_db_to_gain` through a prototype): its types may not be the runtime's, and a definition would be the
program's code, of which the runtime's facts are not true. So the program's declaration stays as the program wrote it.
A math builtin's functions are the other way round: a program that calls the builtin may not declare a function of
their names at all (`checkMathNames`, recipe 15), since its calls of the builtin must reach the runtime's.

`describeRuntime` looks the function up in `kRuntimeFacts`, which names each of the heap's and the FFT's runtime
functions and its kind (`Allocation`, `Release`, `Plan`, `PlanRelease`, `Transform`), and sets that kind's attributes
through `llvm::Function`'s own setters; it leaves any other function bare. Every one of the ten is `noexcept`, so
`nounwind`, and comes back, so `willreturn`:

| Function | Attributes | Why it is true |
|---|---|---|
| `fminus_rt_fft_forward_real` and the other three transforms | `memory(argmem: readwrite, inaccessiblemem: readwrite) nofree nosync`; the plan and `in` `readonly captures(none)`, `out` `writeonly captures(none)` | `Fft.cpp` reads the plan and `in`, writes `out`, and keeps none of the pointers. Its other memory is PFFFT's setup and the plan's buffers, which only the plan points to. It allocates, frees and waits for nothing |
| `fminus_rt_malloc`, `fminus_rt_calloc` | `allockind("alloc,zeroed") allocsize(0)` (calloc's `allocsize(0,1)`) `"alloc-family"="fminus_rt_malloc" memory(inaccessiblemem: readwrite, errnomem: write)`; the result `noalias noundef align 64` | `aligned_alloc(64, ...)` and a `memset` to 0, or null: how LLVM declares C's `calloc`, with `zeroed` for `malloc` too, since FMinus's is |
| `fminus_rt_free` | `allockind("free") "alloc-family"="fminus_rt_malloc" memory(argmem: readwrite, inaccessiblemem: readwrite)`; the pointer `allocptr captures(none)` | C's `free`, as LLVM declares it |
| `fminus_rt_fft_create`, `_create_complex` | `memory(inaccessiblemem: readwrite, errnomem: write)`; the result `noalias noundef` | a fresh plan, or null |
| `fminus_rt_fft_destroy` | `memory(argmem: readwrite, inaccessiblemem: readwrite)`; the plan `captures(none)` | reads the plan, frees it and its own memory |

What is left out matters as much, since an attribute that is not true is a miscompile:

- **No `nonnull`.** A transform on a null plan does nothing, and the heap's results and a plan may be null.
- **No `noalias` on an FFT's buffers.** `Runtime.h` says `out` must not overlap `in`, but a program that transforms in
  place gets what PFFFT gives it, as before. `readonly` and `writeonly` stay true even then.
- **No allocator attributes on `fft_create`.** Its result is fresh, but it is no allocator LLVM may remove: its null
  says that PFFFT takes no plan of that size, every time. Given `allockind`, LLVM removes a plan that is made, tested
  and destroyed, and takes the test for a plan made: `fft_create(7)`, tested against null inside FMinus, would then be
  a plan. `AttributeTest.cpp` runs it.

The rule this rests on is that **a plan's memory is the runtime's**: `inaccessiblemem` is memory no FMinus code
reaches. A program could convert a plan to a module pointer whose fields line up with the runtime's `Plan` and read
the buffers' addresses out, but that reads an object as a type it is not, which C leaves undefined (recipe 17). A
program hands a plan to the FFT's builtins, and reads or writes nothing through it.

`runtime-attributes.fm` shows what LLVM does with them:

- **A module's fields survive a transform.** A method that reads a field, transforms into a local array and reads the
  field again reads it once: the call writes only its output and the plan's own memory. A transform into the module's
  own field is read again after, as it must be.
- **The heap's blocks are fresh, and zeroed.** `float* p = malloc(n * 4); p[0] = 2; return p[0] + p[1];` (with its
  `free`) is `ret float 2.0`, and the block goes. A `calloc` and `free` with nothing between them go too.
- **A store just before a `free` is dead.**

**Allocation elision is C's freedom, and it changes one thing.** LangRef lets LLVM remove a call of a function with
`allockind` and serve the block from a "virtual" allocator, which does not fail. Clang's `malloc` has the same
attributes, so a C program gets the same: C doesn't spell the freedom out, as C++ does ([expr.new]), but nothing in it
says an unused allocation must happen, or fail. So a block that the program only tests and frees may be removed, and
the test is then false, where the runtime would have given null:

```c
bool refused() { float* p = calloc(-1, 4); float* none; const bool r = p == none; free(p); return r; }
```

`fminus file.fm --run refused` prints `true` at `-O0` and `false` at `-O2`. A block the program keeps, stores into a
module or returns is made, and is null when the runtime refuses it, as `HeapTest.cpp` now checks from the host.

### libm's prototypes, where libm sets no errno

A prototype with no definition calls the process's C function of its name (README.md): `float asinf(float x);` calls
libm's. LLVM knows libm's functions by name and type (`TargetLibraryInfo`), and InferFunctionAttrs, in the pipeline,
gives their declarations what C promises of them: `nounwind willreturn nofree nosync`, and `memory(errnomem: write)`,
since C lets a math function report a domain or range error in `errno`. A call that writes memory stays where it is:
LICM leaves it in its loop, so does each of knob versioning's copies, and the loop vectoriser gives up on the loop. So
in a method whose cutoff is a still dial, `tanf(pi * cutoff[i] / rate)`, while `tanf` was a prototype's libm call
(recipe 23 made it a builtin), ran every sample, where the builtins' `sinf(w) / cosf(w)` ran once a call.

Apple's libm writes no `errno`: its `math_errhandling` is `MATH_ERREXCEPT` (2), and none of thirty calls at their domain
and range errors (`logf(0)`, `expf(1000)`, `powf(0, -1)`, `acosf(2)`, `sqrtf(-1)`, `fmodf(1, 0)`, ...) sets it, even
compiled with `-fmath-errno`. That is why clang defaults to `-fno-math-errno` on Apple systems, and declares
`<math.h>`'s functions `memory(none)` there. `describeLibm` (`Functions.cpp`) does the same for FMinus, after the
definitions. For each prototype that no definition followed, whose name and type are a math function's that
`TargetLibraryInfo` knows and the target has, it runs LLVM's own inference (`inferNonMandatoryLibFuncAttrs`) and takes
`errno` out of what the function may write:

| Prototype | LLVM's attributes | On Darwin |
|---|---|---|
| `asinf`, `powf`, `atan2f`, `exp2f`, `log10f`, `ldexpf`, ... | `memory(errnomem: write)` | `memory(none)` |
| `remquof`, which writes its quotient | `memory(argmem: write, errnomem: write)` | `memory(argmem: write)` |
| `roundf`, `ceilf`, `truncf`, ..., which set no `errno` anywhere | `memory(none)` | the same |
| a name libm doesn't have, a type that isn't libm's (`float atanf(int x)`), a definition | none | none |

Which libm is in the triple (`ir::Options::targetTriple`, the process's when empty). Linux's glibc and Windows' CRT do
set `errno`: there the declarations stay as the program wrote them, and LLVM infers what C allows. Other C functions
that write `errno`, the allocators, also reach memory of their own (`inaccessiblemem`), and keep it: only a function
whose memory, without `errno`, is at most its arguments' loses `errno`.

What LLVM then does, which is what clang does with `<math.h>` on Apple systems:

- **Calls move.** A call on a loop's invariants is made once, before the loop, in every copy knob versioning makes.
  A loop of calls on its samples vectorises: in the vector and libm math modes into `llvm.asin.v2f32` and its kin,
  which the backend makes into one `asinf` call a lane, the same function, so every result is still libm's; in the
  fast mode into libsystem_m's `_simd_asin_f4` (recipe 8), with that mode's caveat that a block's vectorised body and
  its remainder may differ in their last bits (recipe 15: "a prototype's libm function gets the vector library only in
  the fast mode").
- **Two calls become arithmetic.** LLVM's library-call simplifier rewrites `powf(x, 2)` as `x * x`, and
  `powf(x, 0.5)` as a square root (with `-0` and `-inf` handled), only for a call that touches no memory. Both are
  correctly rounded, and libsystem_m's `powf` is not always: over every float, it differs from `x * x` for 13,758,450
  inputs and from the square root for 13,621,093 (0.3% each). In [1, 2) it is the farther from the exact result in
  53,514 of its 53,936 differences from `x * x`, and in all 44,211 from `sqrtf`. So those two calls move by an ulp, for
  those inputs, toward the exact result. (The rewrites that need no such permission ran before too: `powf(x, -1)` to
  `1 / x`, `powf(2, x)` to `exp2f(x)`, and constants folded with the host's double functions.)

On an effect with a one-pole on `tanf(3.14159265f * cutoff[i] / 48000)` and a gain of `powf(10, level[i] / 20)`, its
two parameters streams left flat (a still dial, as a plugin host compiles them), at `-O2` and flushing: 87.0
instructions a sample before, 16.1 after, and 8.6 ns against 5.4 (an M5 under a load average of 47; the minimum of 14
runs), the output the same. With the streams moving, the calls stay in the loop, as their arguments change every sample.
(That was measured while `tanf` was a prototype's call of libm; since recipe 23 it is a builtin, whose call writes no
memory on every system, and `powf` is the one of the two that this recipe moves.)

### Loops must make progress

C11 6.8.5p6 lets an implementation assume that a loop ends when its controlling expression is not a constant
expression and it does no I/O, touches no volatile object and does no synchronising or atomic operation. A loop with a
constant condition (`while (1)`, or `for (;;)`, whose omitted condition counts as a nonzero constant) may run for ever.
Clang (`CodeGenFunction::checkIfLoopMustProgress`) marks the loops that must end with metadata on their branches back
to the header, and FMinus does the same: `lowerWhile` and `lowerFor` call `mustProgress` on their condition block when
the condition is not a constant expression, and it marks the branches:

```cpp
            llvm::Metadata* operands[] = {
                nullptr, llvm::MDNode::get(context, llvm::MDString::get(context, "llvm.loop.mustprogress"))};
            llvm::MDNode* loop = llvm::MDNode::getDistinct(context, operands);
            loop->replaceOperandWith(0, loop);   // a loop ID names itself, so that it is this loop's alone
            for (llvm::User* user : header->users())
            {
                auto* branch = llvm::dyn_cast<llvm::Instruction>(user);
                if (branch != nullptr && branch != entry && branch->isTerminator())
                    branch->setMetadata(llvm::LLVMContext::MD_loop, loop);
            }
```

Every branch back to the condition block but the one into the loop gets the ID: a `while`'s fall-through and its
`continue`s share it, as a loop's latches must for `Loop::getLoopID` to find it, and a `for`'s only branch back is
`for.inc`'s. `isConstant` decides what a constant expression is, on the syntax tree, as C11 6.6 does: numbers, `true`
and `false`, `sizeof`, and operators, casts, `?:` and vector literals whose operands all are, in a `std::visit` with a
lambda for each kind of expression. Three choices follow from C:

- **A variable is never a constant expression, `const` or not**, as in C (C++ differs).
- **Nor is a parameter, whatever its binding.** A frozen one folds to a constant in the IR, but the program means the
  same as with a knob, so `while (enabled[0])` gets the metadata under every binding.
- **`1 || x` is not one**, as C reads 6.6, though clang folds it; and `while (1.5f)` is one, where clang, which asks for
  an integer, marks it. Neither is a loop anyone writes.

The rest of 6.8.5p6 needs no test: FMinus has no `volatile`, its only atomics are knob reads, once a call where a
method starts, and I/O happens in external functions, which LLVM takes to have any effect unless it knows them. No
function is marked `mustprogress`, which C does not imply (C++ does).

So a loop with a non-constant condition that runs for ever without effects is undefined, as in C, and LLVM may delete a
loop whose results go unused without proving that it ends. In `progress.fm`, the Collatz steps, which no one has proved
end, are deleted at `-O2`, and the same body under `while (1)` is kept, an infinite loop:

```c
void steps(int n)
{
    while (n != 1)
        n = n % 2 == 0 ? n / 2 : 3 * n + 1;
}
```

### What it changes for hosts and programs

No option: both are C's semantics. For a program, an unused block may be elided, and an unused loop with no effects
deleted. For a runtime function, its facts must stay true of its code, and change with it: the heap's and the FFT's in
`kRuntimeFacts`, the math builtins' in `mathFunction` (`Math.cpp`). Recipe 5's `db_to_gain`, declared there with
`getOrInsertFunction`, is bare, which is safe; it could say what is true of it, `nounwind willreturn nosync nofree
memory(errnomem: write)` (`std::pow` may set `errno`).

### Tests

- `tests/lit/fminus/runtime-attributes.fm`: every runtime declaration and its attributes at `-O0`; at `-O2`, a field
  read once across a transform, the transform's output read again, an unused `calloc` and `free` removed, a block's
  unwritten floats read as 0, a removed block's test against null folded to false, and `fft_create`'s call and test
  kept.
- `tests/lit/fminus/libm-prototypes.fm`: on Darwin, libm's prototypes `memory(none)` (`remquof`'s `memory(argmem:
  write)`) at `-O0`, a prototype of another type, an unknown name and a definition bare, and at `-O2` a loop's `asinf`
  and `powf` of its invariants before the loop, which vectorises; elsewhere, the declarations bare and the calls in
  the loop. (Its example was `tanf` until recipe 23 made `tanf` a builtin; `asinf` is one of libm's that LLVM takes to
  write `errno`, as `tanf` was, where `cbrtf`, which LLVM already takes to touch no memory, would test nothing.)
- `tests/lit/fminus/progress.fm`: the metadata on a `while`, on a `for`'s `for.inc`, on a `while`'s two branches back,
  and on a loop whose condition reads a `const` variable; none on `while (1)`, `for (;;)`, `while (true)`,
  `for (; 2 > 1 && !0;)` and `while (1.5f)`, and no function marked `mustprogress`; at `-O2`, two unused loops deleted
  and the infinite ones kept.
- `tests/unit/AttributeTest.cpp`, in the JIT, what must not change: a field read across a transform, a module's own
  spectrum as the transform's output, a store into the input just before a transform, a round trip through two
  transforms; `fft_create(7)` still null inside FMinus; a block reads 0 until written; loops with a `continue`, a `?:`,
  `while (1)` with a `break` and `for (;;)` with a `return`; a program's own prototype of `fminus_rt_free`, with other
  types; libm's prototypes lowered for Apple, Linux and Windows triples, each declaration's memory, and a loop of
  `asinf` and `powf` against C++'s calls of libm, bit for bit.
- Changed: `HeapTest.cpp`'s refused sizes return their blocks to the host, which tests them for null;
  `heap.fm`'s `declare` lines carry their attributes.

### Measurements

None of FMinus's kernels speeds up: every example's audio path compiles to the same `-O2` IR but for the loop
metadata, and the convolver runs at about 9.9 ns a sample before and after (2048 taps, blocks of 256). Its
transforms write through pointers loaded from its own fields, which may point into the module as far as LLVM knows,
so a field read after one is read again anyway. What changed is the functions that allocate or free (`echo_create`,
`Echo::init`, ...), on which LLVM now infers `noalias`, `noundef` and `dereferenceable_or_null`, and the IR in the lit
tests: a field read across a transform into a local, a scratch block a program could do without, a loop nobody reads.

## Recipe 20: subnormals, flushed for the audio thread

*In the compiler. Code: `src/ir/Denormals.cpp` and `Denormals.h`. Tests: `tests/unit/DenormalTest.cpp`
(`[denormals]`), `tests/library/DenormalsTest.cpp`, `tests/lit/fminus/denormals.fm`,
`tests/lit/fminus/denormals-arm64.fm`.*

C's floats underflow gradually. Below the least normal float, 2^-126 (about 1.18e-38), come the subnormals, down to
2^-149, and a product too small for a normal float lands among them instead of on 0. FMinus follows C, and that stays
the default. Audio has two reasons not to want it:

- **A subnormal can be slow.** On an M5 a chain of multiplies on one runs about 13% slower, two `float4` chains 34%;
  on x86 an operation on one can take a microcode assist of a hundred cycles or more (Intel's figure).
- **A silent filter never reaches 0.** A one-pole `y += k * (0 - y)` decays into the subnormals and stops where
  `k * y` rounds to nothing: at 7e-44, for ever, every sample on a subnormal.

So a plugin host runs its audio thread with flush-to-zero: a subnormal input reads as 0 and a subnormal result is 0,
each keeping its sign (AArch64's FPCR.FZ; x86's MXCSR.FTZ for results and DAZ for inputs), set around each block by a
scoped guard. FMinus neither set that mode nor told LLVM about it, and LLVM assumes IEEE's: it folds a frozen
parameter's `1e-30 * 1e-10` to the subnormal 1e-40, where the same code reading the value from a knob computes 0 in
the host. The option makes the mode part of the program:

| `Program::Options::denormals`, `fminus --denormals=` | Subnormals | The mode |
|---|---|---|
| `Preserve`, `preserve` (the default) | numbers, as in C: IEEE's gradual underflow | the host's, untouched; LLVM assumes IEEE's |
| `Flush`, `flush` | an input reads as 0 and a result is 0, with its sign | assumed by every function, and set by each entry point |

Under Flush a program means what C means under clang's `-fdenormal-fp-math=preserve-sign` with the mode set, which
clang's `-ffast-math` arranges on Linux for a whole executable by linking `crtfastmath.o`. FMinus has no process to
start, so it sets the mode per entry point instead. Preserve changes nothing at all: the IR of every example and lit
program is the same, byte for byte, at `-O0`, `-O2` and `-O3`, as before the option.

`ir::Options` gains `denormals` and `targetTriple` (`CodeGen.h`): which register an entry point sets, FPCR on arm64 and
MXCSR on x86-64, since lowering never sees a TargetMachine. The driver and `Program::compile` pass the machine's
triple; empty is this process's. On a target whose mode FMinus cannot set, Flush is one error at 1:1: `cannot flush
subnormals to zero on riscv64: FMinus sets the floating-point mode on arm64 and x86-64`. `Lowering::lower` runs
`flushDenormals` last, after `checkEffects`, since it needs every call. It has three parts.

### The attribute: every definition assumes the mode

LLVM 23's function attribute `denormal_fpenv` (an `llvm::DenormalFPEnv`, which replaced the `"denormal-fp-math"`
string) holds the mode a function runs in: output mode, then input mode, each `ieee`, `preservesign`, `positivezero` or
`dynamic`. Flush is `preservesign`, both ways: AArch64's FZ (with FPCR.AH clear, as macOS and Linux leave it) and x86's
FTZ with DAZ. It prints as `denormal_fpenv(preservesign)`, and `flushDenormals` gives it to every function the program
defines:

```cpp
        constexpr llvm::DenormalFPEnv kFlushing{llvm::DenormalMode::getPreserveSign()};
        ...
            definition->addFnAttrs(llvm::AttrBuilder(module_.getContext()).addDenormalFPEnvAttr(kFlushing));
```

Declarations get none: a prototype's body is someone else's. The attribute sets no hardware. What it changes is what
LLVM may assume: `ConstantFoldFPInstOperands` flushes a subnormal operand to a signed zero before it folds, and a
subnormal result after, reading the mode from the instruction's function; InstSimplify and InstCombine fold through it.
All the program's functions share one mode, so the inliner inlines among them as before.

### Folding as it is built: `DenormalFolder`

The attribute is not enough. `IRBuilder<>` folds an operation on two constants as it builds it, with `ConstantFolder`,
in IEEE arithmetic: there is no instruction yet, so no function to read a mode from. A frozen parameter is a constant
(recipe 14), so `k[i] * 1e-10f` with `k` frozen at 1e-30 is built as 1e-40 before any attribute can stop it. Leaving
the constants unfolded would not help: at `-O0` nothing folds the IR again, and AArch64's GlobalISel folds what reaches
it in IEEE arithmetic whatever `denormal_fpenv` says.

So the code generator's builder is `llvm::IRBuilder<DenormalFolder>` (`Lowering.h`). `DenormalFolder` holds a
`ConstantFolder` (which is `final`) and forwards every fold to it, except, under Flush, the ones that see subnormals:

```cpp
    llvm::Value* DenormalFolder::FoldBinOp(llvm::Instruction::BinaryOps opcode, llvm::Value* lhs,
                                           llvm::Value* rhs) const
    {
        if (!flush_ || !isFloatArithmetic(opcode))
            return folder_.FoldBinOp(opcode, lhs, rhs);
        ...
        a = flushed(a);
        b = flushed(b);
        ...
        auto* result = llvm::dyn_cast_or_null<llvm::Constant>(folder_.FoldBinOp(opcode, a, b));
        return result != nullptr ? flushed(result) : nullptr;
    }
```

`FoldBinOpFMF` is the same, and `FoldCmp` flushes an `fcmp`'s operands, so `1e-40f > 0` folds to false, as FCMP and
UCOMISS compute it flushing. That is `ConstantFoldFPInstOperands` under preservesign, step for step, so a constant
folds the same whether the code generator or LLVM folds it, at any level. `fneg` and casts are left alone: a sign flip
does not flush, and a subnormal is 0 as an `int` either way. Under Preserve the folder is `ConstantFolder`, call for
call.

### Entry points: the wrappers

An entry point is a function marked `[[clang::nonblocking]]`, what a host calls on its audio thread, that no function of
the program calls. "Called" is one set, from the calls the effects' check already records (`calls_`). `wrapEntry`
renames the definition to `<name>.body`, makes it internal and `noinline`, and creates `<name>` again as a wrapper with
the same type and the same C ABI attributes (`zeroext`, `noalias`, `this`'s `nonnull`), so a host's function pointer
type does not change. The wrapper calls the body through its address, passed through an empty inline asm statement.
On arm64:

```llvm
define float @"Lowpass::tick"(ptr nonnull align 4 dereferenceable(8) %this, float %x) #1 {   ; denormal_fpenv(dynamic)
entry:
  %fpcr = call i64 @llvm.aarch64.get.fpcr()
  %fpcr.fz = or i64 %fpcr, 16777216                           ; FZ, bit 24
  %flushing = icmp eq i64 %fpcr, %fpcr.fz                     ; the caller's mode flushes already
  %body = call ptr asm "", "=r,0"(ptr @"Lowpass::tick.body") #3   ; nounwind willreturn memory(none)
  br i1 %flushing, label %run, label %flush
run:
  %result = call float %body(ptr nonnull align 4 dereferenceable(8) %this, float %x)
  ret float %result
flush:
  call void @llvm.aarch64.set.fpcr(i64 %fpcr.fz)
  %flushed = call float %body(ptr nonnull align 4 dereferenceable(8) %this, float %x)
  call void @llvm.aarch64.set.fpcr(i64 %fpcr)
  ret float %flushed
}
```

On x86-64 it is the same with `llvm.x86.sse.stmxcsr` and `ldmxcsr`, through two stack slots, setting FTZ (bit 15) and
DAZ (bit 6). Every return of the body is covered, since the body returns to the wrapper, which restores the register
before it returns on the path that set it, so the register comes back exactly as it was. Each call of the body carries
the body's return and parameter attributes, since an indirect call has no callee for the code generator to read
`zeroext` from, and none of its function attributes, which would tell LLVM that it touches no memory. Nothing in the
wrapper can block. Four details carry the design:

- **The body stays behind a call.** In LLVM's default floating-point environment arithmetic depends on no register, so
  it may move across `llvm.aarch64.set.fpcr`, which is ordered only against other side effects. A body inlined into its
  wrapper could compute before the mode is set. The body is `noinline`, and the wrapper is `denormal_fpenv(dynamic)`,
  into which the inliner refuses to inline a `preservesign` function anyway.
- **A call LLVM cannot see into.** A plain call of the body is not enough either. LLVM orders a call by what it knows
  of the callee, and of a body that touches no memory (`x * 1e-30f * 1e-10f`, `tanhf(x) * 1e-10f`) FunctionAttrs finds
  `memory(none)`: a call ordered against nothing, which LLVM may move as it may an `fmul`. At `-O1` InstCombine sank
  such a call below the write that restores the caller's mode, into the block of the `ret` that used its result, and
  the body ran in an IEEE host's mode (the Sink pass would move it too). Through the asm statement's result, which LLVM
  never looks into, the call is of a function LLVM cannot see, which may read and write any memory
  (`CallBase::getMemoryEffects` reads a callee's attributes only when the callee is a `Function`). The register's
  writes are the same to LLVM: `set.fpcr` is `IntrNoMem` with `IntrHasSideEffects`, which TableGen makes
  `MemoryEffects::unknown()`, and `ldmxcsr` declares no memory effects. No pass reorders two such calls (InstCombine
  sinks no call that may write memory, SimplifyCFG hoists none past a side effect, GVN gives each its own value number
  and does no PRE of it, EarlyCSE and LICM move none), and in machine code every call is a scheduling barrier and is
  never moved.
- **Writes only when needed.** Reading FPCR costs nothing; writing it costs about 4 ns even when the value does not
  change, and setting FZ then clearing it about 18 ns a call. A host that flushes already, around its whole block, pays
  a compare and a call; one that doesn't pays the two writes, once a call.
- **Called functions are not entry points.** `OnePole::tick`, nonblocking and called a sample at a time from
  `process`, runs in the mode `process` set. A host might call it too, a sample at a time, and a wrapper would then
  write the register every sample. So a function the program calls keeps its plain definition, and a host that calls it
  (or anything else that is not an entry point, an `init`) sets the mode around the call itself.

The asm statement costs little at run time: the body's address in a register (two instructions on arm64, ADRP and an
LDR from the GOT or an ADD), and an indirect call. On arm64 that call is a tail call from `-O1` on the path of a caller
that flushes already (`BR` to the register, not `B` to the body); on x86-64 it is a plain call there, since the two
MXCSR stack slots escape into `stmxcsr` and `ldmxcsr`, and LLVM marks no call after an escape `tail`. What LLVM no
longer sees costs it some optimisation, never a result: the body, whose address escapes, keeps C's calling convention
rather than `fastcc`; a constant result of the body is no longer folded into the wrapper (a frozen `Tiny::scaled` in
`denormals.fm`, whose body returns 0, still calls it); and the wrapper is not inferred `nounwind` or `willreturn`. One
caveat: under branch-target enforcement (Arm's BTI, x86's CET IBT) the target of an indirect call needs a landing pad,
which the body does not have. Neither the JIT's code nor FMinus's object files, which ask for no such enforcement, are
enforced today; a link that forces enforcement onto objects without it (`-z force-bti`, `-z force-ibt`) would be.

Ahead of time, `fminus --denormals=flush --emit exe` on a nonblocking `main` works as it is: `addEntryPoint` renames the
wrapper and adds the C `main`, which inlines the wrapper, never the body.

### What it changes for programs and hosts

- **Semantics.** Under Flush a subnormal input of an arithmetic operation or comparison reads as a zero of its sign,
  and a subnormal result is a zero of its sign, in every function of the program, and constants fold the same way.
  `fneg` and `fabsf` keep a subnormal (they are sign-bit operations), as do loads, stores and moves.
- **The contract.** Every function assumes the flushing mode; only entry points establish it. A host that calls
  anything else calls it with the mode set, or a subnormal's result may differ from what the compiler folded. The
  pattern to recommend is the usual one, FTZ (and DAZ) around the whole block, which also makes every entry point's
  check free.
- **Driver and API.** `fminus prog.fm --denormals=flush` (with `--run` and `--emit` too);
  `options.denormals = fminus::Program::Denormals::Flush;` before `Program::compile`.
- **Known limits.** Arm's FZ flushes a result that is tiny before rounding, LLVM's folding one that is subnormal after
  rounding: they differ only for results that round up to 2^-126 (`(1 - 2^-24) * FLT_MIN` is 0 under FZ, and folds to
  `FLT_MIN`); x86 detects tininess after rounding, as LLVM does. In the fast math mode, LLVM folds `sinf` and the others
  of constants with the host's double functions whatever the mode. A constant operation that first appears inside
  SelectionDAG is folded there in IEEE arithmetic; none was seen in FMinus's programs. With FPCR.AH set, FZ no longer
  flushes inputs, and macOS and Linux leave it clear. An entry point reaches its body by an indirect call, so a link
  that enforces branch targets (BTI, CET's IBT) would need a landing pad in the body, which FMinus does not emit.

### Tests

- `tests/unit/DenormalTest.cpp`, against C++ computing in the same mode (a `ScopedMode` guard that sets FPCR or MXCSR,
  as a host's would): an entry point gives 0 for `1e-30f * 1e-10f`, -0 for its negative, 0 for a subnormal operand;
  the caller's mode comes back from every return, under an IEEE host and a flushing one (on x86-64, MXCSR without its
  six exception flags, which the arithmetic itself sets: FPCR has none); Preserve keeps subnormals
  bit for bit; a one-pole sticks at a subnormal under Preserve and at a normal number under Flush, bit for bit with
  C++ in each mode; a nonblocking function the program calls runs in the host's mode; entry points whose bodies touch
  no memory (a product, a `tanhf`, a comparison, a `float4`), flushing at `-O0` to `-O3` under an IEEE host and a
  flushing one, each body `memory(none)` after the pipeline and its wrapper still calling it through the asm's result,
  never directly; a frozen parameter's results equal a knob's at run time, at `-O0` to `-O3`; the IR (every definition
  `preservesign`, each wrapper `dynamic` with its body internal and `noinline`, called twice through the asm and never
  directly, nothing under Preserve); x86-64's wrapper; the error on riscv64; and `DenormalFolder` itself.
- `tests/library/DenormalsTest.cpp`: `Program::Options::denormals` from C++17, and an entry point that reads no memory
  running flushing at each optimisation level.
- `tests/lit/fminus/denormals.fm`: the attribute and the wrappers at `-O0`, each calling its body through the asm's
  result, and after the pipeline; the frozen product folded to 0 at `-O0` and `-O2` and to `9.999940e-41` under
  Preserve, every `define` carrying the attribute, `--run` at `-O0` to `-O3`, and the bad value. `denormals-arm64.fm`
  (`REQUIRES: arm64`): the wrapper's IR block by block, and its machine code: from `-O1`, MRS, ORR, the body's address,
  CMP, the tail call `BR` when the caller flushes already, and MSR, BLR, MSR otherwise; at `-O0`, through the stack, a
  `BLR` with no write on the first path, and MSR, BLR, MSR on the other.

### Measurements

The M5, the dev build, with other builds running (about ±10%). The wrapper, a call through the JIT's pointer: `x + 1`
takes about 0.5 ns under Preserve, 19 ns under Flush in an IEEE host (two FPCR writes), and 0.7–1.1 ns in a host
that flushes already. A first version without the check of the caller's mode wrote twice always, about 7 ns a call
even in a flushing host. Calling the body through its address cost nothing measurable: through an object file at `-O2`,
the best of 7 runs of 20 million calls, `x + 1` took 18.2–18.8 ns in an IEEE host against 18.6–18.9 with the direct
call, and 0.70 ns in a flushing one against 1.02–1.17. Three programs written for the measurement, and not kept in the
repository: a chain of multiplies, two `float4` chains and a one-pole, each a nonblocking function of 300 million
iterations, run by `fminus --run` (its 28 ms of compilation subtracted):

| Program | Preserve | Flush |
|---|---|---|
| a chain of multiplies, normal | 0.70 ns/iteration | 0.70 ns |
| the same, subnormal (1e-39) | 0.79 ns (+13%), stays 1e-39 | 0.70 ns, 0 |
| two `float4` chains, subnormal (1e-40) | 0.93 ns (+34%) | 0.70 ns, 0 |
| a one-pole from 1, k = 0.01 | 1.74 ns, stuck at 7e-44 (subnormal) | 1.73 ns, stuck at 1.17e-36 (normal) |

Flush-to-zero removes the subnormal penalty; the one-pole, whose time is its dependency chain, runs at the same speed
either way, and its state under Flush is a normal number but still not 0. Flushing removes the slowdown, not the
residue. Compile time is unchanged.

## Recipe 21: `#include`, a program in several files

*In the compiler. Code: `src/frontend/Lexer.l` (the directive rule, `<<EOF>>` and `directive`), `Scanner.h`,
`Frontend.h`, `Diagnostics.h`; `Program::Options::include` (`include/fminus/FMinus.h`); `-I` (`tools/fminus/main.cpp`).
Tests: `tests/unit/IncludeTest.cpp` (`[include]`), `tests/library/IncludeTest.cpp`, `tests/lit/fminus/include.fm` (with
`Inputs/include/`).*

A module worth reusing, a one-pole or an oscillator, wants a file of its own, and C's way to use one is the
preprocessor's `#include "file"`: the file's text is read in place of the line. FMinus does what C does there and no
more. (It had no macros and no conditional compilation then, which a language with modules, `const` and parameters
needs less than C does; recipe 31 added the conditionals, with macros that only they read.) Two choices are its own,
both because FMinus has no headers:

- **Each file is read once**, as if every file began with `#pragma once`. In C, a header without a guard included
  twice declares everything twice: harmless for prototypes, an error for definitions. FMinus's files hold definitions
  (a module and its methods), so a second `#include` of a file reads nothing. That settles the diamond, `a.fm` and
  `b.fm` both including `pole.fm`, and the cycle: the program's own file counts as read, so a file that includes it
  gets nothing.
- **Where a name is found is the host's.** `Program::Options::include` is a function from the name and the including
  file's name to the file, its name (as the messages give it) and its text, or nothing. A plugin can keep its files in
  memory; `fminus` looks where C's quoted form does, beside the including file, then in each `-I` directory.

| Line | What it does | Its errors (clang's words) |
|---|---|---|
| `#include "file"` | reads the file there, once | `'file' file not found`, `empty filename`, `expected "FILENAME" or <FILENAME>`, `extra tokens at end of #include directive` (clang warns), `#include nested too deeply` (200 files) |
| `#include <file>` | an error: FMinus has no system headers | |
| `#pragma ...` | nothing, as C ignores the pragmas it doesn't know; `#pragma once` is what every file does anyway | |
| `#` | nothing: C's null directive | |
| `#define`, `#if`, `#ifdef`, ... | an error then; recipe 31's now | |
| `#error`, `#line`, `#warning`, `#embed` | an error: `#error is not supported: FMinus's directives are #include, #pragma, #define, #undef and the conditionals (...)` | |
| any other `#name` | `invalid preprocessing directive` | |

A directive is a line whose first character, after spaces, is `#`, followed by spaces and comments only. A `#`
anywhere else is still `unexpected character '#'`. An `#include` may be any line, inside a function's body too, as in
C. A missing file stops the parse there, as clang's fatal error does.

### One buffer a file

The directive is one scanner rule, `^[ \t]*"#"[^\n]*`: Flex's `^` matches at a line's start, a buffer's first line
too. `directive()`, in Lexer.l's last section, reads the line by hand: the name, the quoted file, then nothing but
spaces and comments. For an `#include` it asks the resolver, skips a file read already, registers a new one, and hands
it back to the rule's action, which uses Flex's own way of reading another input:

- `yy_scan_bytes` makes a buffer of the file's text (a copy) and switches to it, and the scan goes on in it from the
  same action. `ScannerState::open` keeps, for each file being read, its buffer, the buffer it interrupted (Flex's
  current buffer, `YY_CURRENT_BUFFER`), and the location at the `#include`.
- At a buffer's end, `<<EOF>>` deletes it, switches back with `yy_switch_to_buffer` and restores the location. Only the
  program's own end returns `YYEOF`. The parser never knows: it sees one stream of tokens.
- A block comment ends with its file: `<BLOCK_COMMENT><<EOF>>` is `unterminated /* comment`, as in C.
- When a syntax error stops the parser inside an included file, `ScannerHandle`'s destructor deletes the buffers
  still open, the innermost (Flex's current) first.

### Locations that know their file

`SourceLocation` gains `file`, an index into the compilation's files (0 the program's own), and `Diagnostics` holds the
files: `addFile(name, includedFrom)` registers one with the location of its `#include`.

Bison's positions carry a file already, `position::filename`, a `const std::string*` by default.
`%define api.filename.type {const fminus::frontend::SourceFile}` makes it a pointer to a `SourceFile`
(`ParseContext.h`), which holds the index. `ScannerState::files` is a `std::deque` of them, so the pointers stay valid
as files are added, and the program's own file is null. Locations then flow through the parser unchanged (Bison's
`YYLLOC_DEFAULT` copies the first symbol's start), and `at()` reads the index through `sourceLocation` (`Scanner.h`).
The code generator needs nothing new: its errors take the nodes' locations, which carry their file.

Bison's `location.hh` compares the two ends' files with `!=` in its `operator<<`. FMinus never prints a location, but
that template's body is checked where it is defined, so `SourceFile` needs an `operator==` (defaulted).

### Messages in reading order

`format()` sorts its errors into the order the text is read: for each location, the line and column of each `#include`
on the way to it, from the program's file in, then its own, compared as a sequence. An included file's errors sort
where its `#include` is. Before a line in an included file comes clang's include stack, outermost first, when the line
before it was in another file:

```
In file included from bad.fm:2:
In file included from sub/c.fm:1:
lib/d.fm:3:16: error: use of undeclared identifier 'z'
bad.fm:3:27: error: use of undeclared identifier 'y'
```

### Where the files are found

`frontend::IncludeResolver` (`Frontend.h`) and its public twin, `Program::IncludeResolver`, are
`std::function<std::optional<IncludedFile>(std::string_view name, std::string_view from)>`. The scanner asks for every
`#include` it reads, a file read already too, and knows a file by the name the resolver gives it: only the resolver can
tell that `../pole.fm` and `pole.fm` are one file. With no resolver, every `#include` is `file not found`.

`fminus`'s `DiskIncludes` looks beside the including file, then in each `-I` directory in order; an absolute name is
itself. A file found is named by its path with `./` and `../` removed, and one found again by another path (its real
path the same) goes by the first name, so the scanner reads it once; the program's own file is known from the start.
Standard input has no directory, so its names start from the current one.

### Tests

- `tests/unit/IncludeTest.cpp`: the dump with a file read where its `#include` is, a module's name a type after it,
  an `#include` in a function's body, spaces and comments around the directive, and lines counted on after it; each
  file once (the diamond, a file included twice, the program's own file included by itself and by another), with the
  resolver asked for every `#include` with the including file's name; pragmas ignored; `file not found` at the name,
  with no resolver too; errors in included files after their include stack, an unterminated comment and a missing file
  inside one; `format()`'s order and stacks over three files with notes; every directive error; and the depth limit.
- `tests/library/IncludeTest.cpp`: `Program::Options::include` from C++17, a host's files in memory: a module from an
  included file run through the JIT, an included file's error, a file not found, and the default without a resolver.
- `tests/lit/fminus/include.fm`: `fminus` with `-I`, a file beside the test and one in the `-I` directory that
  includes the first again, the test including itself, `--run` and `--emit ast` (one `Pole`), `file not found` without
  `-I`, and the include stack.

A program without directives scans as before: the suite's other tests are unchanged.

## Recipe 22: `parallel for` and `parallel { }`, a program's own threads

*In the compiler. Code: `src/frontend/Lexer.l` and `Parser.y` (`parallel`), `Ast.h` (`ParallelForStmt`,
`ParallelBlockStmt`); `src/ir/Parallel.cpp` (the checks, the outlined bodies, the captures), `Functions.cpp` (`find`
captures, `lookup` does not), `Statements.cpp` (`break`, `continue` and `return` in a region), `Modules.cpp`
(`atEntry` in a region), `Denormals.cpp`, `Calls.cpp` (`RuntimeKind::Parallel`); `src/runtime/Parallel.cpp` (the
pool); `Program::Options::threads` (`include/fminus/FMinus.h`), `--threads` (`tools/fminus/main.cpp`),
`addEntryPoint` (`src/support/Aot.cpp`). Tests: `tests/unit/ParallelTest.cpp` (`[parallel]`),
`tests/library/ParallelTest.cpp`, `tests/lit/fminus/parallel.fm` (with `Inputs/parallel/`). Example:
`examples/ensemble/`.*

A synth's voices are independent: each renders its own buffer from its own state, and only the mix after them needs
them all. A machine has ten cores, and a block's voices run on one. C's threads are a library, C11's `<threads.h>`:
threads a program makes, joins and locks around, none of which an audio thread may do. FMinus's are in the language,
and structured, as OpenMP's `parallel for` and `sections` are: a region's threads join where the region ends, so no
thread outlives a statement, and the program never names one.

```c
module Synth
{
    Voice voices[16];
    void process(float* restrict left, float* restrict right, int n) [[clang::nonblocking]]
    {
        parallel for (int v = 0; v < 16; ++v)    // each voice on whichever core is free
            voices[v].render(n);
        mix(left, right, n);                     // every voice has rendered
    }
};
```

```c
parallel
{
    l = left.tick(x);     // the two filters at once
    r = right.tick(y);    // the block ends when both have
}
```

Three decisions shape the rest. The threads are made once, when the program compiles, never on the audio thread: a
region hands work to a pool of them. A region never waits for the pool: when another holds it, a region runs on its
own thread, and so does a region inside a region. And what the compiler can see would race is an error, before
anything runs.

### Tokens, grammar and nodes

`parallel` is a keyword, before `{IDENTIFIER}` in Lexer.l, and starts two statements in Parser.y, with no conflict
(nothing else starts with it):

```yacc
statement
    : ...
    | "parallel" "for" "(" for_init optional_expr ";" optional_expr ")" statement
    | "parallel" block
    ;
```

`ParallelForStmt` has `ForStmt`'s fields: the grammar takes any `for`, and the code generator checks the counting
form, so that a wrong one gets a sentence that says what to write rather than a syntax error. `ParallelBlockStmt`
holds the block's statements, each a task. They dump as `(parallel-for (...) cond step body)` and `(parallel ...)`.

### A region is a function of its own

`lowerParallelFor` and `lowerParallelBlock` (`Parallel.cpp`) put the region's body into a function,
`<function>.parallel.<n>`, internal, of the runtime's task type `void(ptr context, i32 begin, i32 end)`: a loop's
runs the iterations `begin` to `end - 1`, a block's its statements `begin` to `end - 1` (a `switch` on each one's
number). Where the statement was, the function fills the context and calls the runtime:

```llvm
define void @scale(ptr %out, i32 %n, float %k) {
entry:
  %scale.parallel.1.context = alloca [16 x i8], align 8
  ...
  %k2 = load float, ptr %k.addr, align 4, !tbaa !5
  %0 = getelementptr inbounds i8, ptr %scale.parallel.1.context, i64 0
  store float %k2, ptr %0, align 4                         ; k, which the body reads: a copy
  %out3 = load ptr, ptr %out.addr, align 8, !tbaa !7
  %1 = getelementptr inbounds i8, ptr %scale.parallel.1.context, i64 8
  store ptr %out3, ptr %1, align 8                         ; out, a pointer: a copy of it
  call void @fminus_rt_parallel_for(ptr @scale.parallel.1, ptr %scale.parallel.1.context, i32 0, i32 %n1, i32 0)
  ret void
}

define internal void @scale.parallel.1(ptr noalias readonly captures(none) %context, i32 %begin, i32 %end) {
entry:
  %out.addr = alloca ptr, align 8                          ; the body's own slots, as a parameter's
  %k.addr = alloca float, align 4
  %i = alloca i32, align 4
  %k.at = getelementptr inbounds i8, ptr %context, i64 0
  %k = load float, ptr %k.at, align 4
  store float %k, ptr %k.addr, align 4
  %out.at = getelementptr inbounds i8, ptr %context, i64 8
  %out = load ptr, ptr %out.at, align 8
  store ptr %out, ptr %out.addr, align 8
  br label %start
start:
  store i32 %begin, ptr %i, align 4                        ; for (int i = begin; i < end; ++i) body
  br label %parallel.cond
  ...
}
```

That is the source `void scale(float* out, int n, float k) { parallel for (int i = 0; i < n; ++i) out[i] *= k; }` at
`-O0`. At `-O2` mem2reg makes the copies registers, and the body's loop vectorises as any loop over a range does. The
last argument is `Options::threads`, the most threads the region may use.

- **Captures, as the body finds them.** A variable of the function around the region is found by `find()`, which
  captures it (`capture()`) when the binding it finds lives in a scope below the region's own (`Region::base`): it
  makes the region's binding, in the region's outermost scope, where the next look finds it. A capture takes the next
  place in the context (at its type's alignment) and its load where the body starts. When the region ends
  (`endRegion`), the function around it fills the context at the statement's place: by then every capture is known.
  `lookup()` finds a binding without capturing, for the checks and for `parameterOf`, which only asks whether a local
  hides a parameter.
- **By value, or by reference.** A variable the region only reads is copied, into a slot of the body's own: its
  loads, inside the body's loops, read memory nothing else writes. An array or a module goes by reference, its address
  (its value is its address anyway), and so does a variable the region assigns or takes the address of, which the
  checks' walk lists (`Region::byReference`). `Binding::slot` became an `llvm::Value*` for that: a captured
  reference's slot is the address the context holds.
- **A method's own reads.** `this` is a variable (`self()` finds it). What a method reads of a parameter where it
  starts, `atEntry`'s knob values and a stream's pointer, test, base and stride, is the method's to read, once, so
  that every iteration sees what the call does: in a region, `atEntry` returns the region's copy of the method's read
  (`capturedParameter`), and the region's stream loads get the body's own alias scope (`scopeStreams`).
- **Regions in regions.** A region inside another captures from the one around it; when it ends, `find()` in the one
  around it captures there in turn. Each region sets aside the parameter reads of the code around it
  (`Region::entryLoads`, `flatStreams`, `streamReads`) and the builder's place (`Region::resume`), and gives them back.
- **The rest of the function goes on as before.** The body's calls are recorded under the function's name, so a
  `[[clang::nonblocking]]` `process` can't reach `malloc` through its region; the runtime's call itself is no call of
  the program's (the pool allocates nothing). Under `Denormals::Flush` the body is marked flushing, as every definition
  is, and runs in its caller's mode, which the runtime sets on each thread. LLVM knows of `fminus_rt_parallel_for`
  only that it does not throw (`RuntimeKind::Parallel` in `Calls.cpp`): the body may read and write anything its
  context reaches, and need not return. Its name is the runtime's, as the math builtins' are (recipe 15): a program
  with a region that declares a function `fminus_rt_parallel_for` is told to rename it, with a note at the
  declaration.

### What would race

`check()` is a walk (`Walker`, in `Parallel.cpp`) of the region before it lowers: every access the code makes to what
it did not declare itself, as a place and a kind. A place is a variable from outside the region (`Outer`), a field of
the method's module through `this` (`Self`), or one of the region's own (`Local`, not recorded); with the fields
followed from it (`m.y.z`), and whether an index was (`buffer[i]`, `voices[v].y`): an element. A kind is a read, a
write (`=`, `+=`, `++`), an address (`&x`, an array's name as a pointer, `this` as a value) or a call of a method on
the place.

| In a `parallel for` | refused | why |
|---|---|---|
| `total += out[i];` (`total` from outside) | every iteration of the parallel for assigns 'total' | every iteration would, at once |
| `level = 2;` (a field) | the same, for 'level' | |
| `filter.tick(x);` | ... calls 'OnePole::tick' on 'filter', which changes it | `tick` writes its module |
| `bump();` (a method of the module) | ... calls 'Synth::bump', which changes the module it is called on ('this') | |
| `i = 3;` | cannot assign 'i', the loop variable of a parallel for | each iteration has its own |
| `break;`, `return;` | 'break' cannot leave a parallel for; cannot return from a parallel for | the others run on |
| `out[i] = f(in[i]);`, `voices[v].render(n);`, `*p += 1;`, `continue;` | allowed | an element, through an index or a pointer, is the program's to keep apart |

In a `parallel { }`, a statement that changes a place (a write, an address, a call that changes its module) and
another that uses it, the same place or one inside the other (`m` and `m.y`, not `m.x` and `m.y`), are an error at
the later one, with a note at the earlier: `the statements of a parallel block run at once: this one uses 'a', which
another changes`. So is a declaration as a statement (it would end with the statement), and `break`, `continue` and
`return` out of one.

Whether a method changes its module (`changesModule`) is worked out once, for every method, the first time a region
asks: the same walk over each method's body, whose fields are `Self`. One that writes a field, an element of an array
field too, or takes the address of one or of the module, changes it; then, until nothing changes, so does one that
calls such a method on its module or on a module among its fields. What a method writes through a pointer field is
not its module's. `gain()`, which only reads `k`, may run in every iteration; `tick()` may not, on one module.

What is not checked is what C with OpenMP does not check either: elements, which iteration reaches which (the compiler
sees `out[i]`, not that `i` is the loop's own), and memory through pointers. A program that races there gets what
threads give it.

### The pool

`runtime/Parallel.cpp` runs one region at a time, on one pool for the process: a thread for each core but one, made by
the first `fminus_rt_parallel_start` and joined by the last `fminus_rt_parallel_stop`. `Program::compile` starts it
when the module calls `fminus_rt_parallel_for` (and `threads` is not 1), and `~Program` stops it; `fminus --run` and an
executable's `main` start it around the call. A host of `jit::Jit` or of an object file starts it itself, or every
region runs on the calling thread, in order, which computes the same.

A call publishes the region (the task, the context, the range, a chunk's size, the caller's floating-point mode), then
the cursor, `generation << 32 | next`, which releases them; bumps `signal`, which idle threads watch; and takes chunks
itself. A chunk is about a quarter of a thread's share, taken with a compare-and-swap of the cursor as each thread
comes free, so a slow core holds up only its own. When none is left, the caller closes the cursor and spins until the
threads still inside have finished. A pool thread counts itself `inside` before it reads anything of the region, and
reads it only while the cursor is open: the region's caller writes the next region only when no thread is inside.
Both are sequentially consistent, which is what makes the two meet: a thread counted before the caller looks is waited
for, and one counted after finds the cursor closed.

A call runs alone, on its own thread, when there is no pool, when `threads` is 1, for one iteration, inside a chunk of a
region the pool runs (a nested region: so it never waits for itself), and when another caller holds the pool (a
host's other audio thread). It allocates nothing and takes no lock.

Each pool thread copies the caller's control register (FPCR on arm64, MXCSR on x86-64) while it runs chunks, so an
iteration computes on any thread what it would on the caller's: flush-to-zero under `Denormals::Flush`, the rounding
mode, everything. A pool thread that took a seat in a region, or that a caller woke for one, spins on `signal` for
50 µs, so the next region in the same block starts at once; then it sleeps (`std::atomic::wait`), on a word of its own,
its bed, so that a caller can wake one thread and no other. A region has `seats` places for pool threads, its
`threads` less the caller, and no more than its iterations less one. The caller counts the threads still awake, which
will look at the region themselves, and wakes only as many sleepers as the seats left, the first beds first, so the
same few threads take the small regions: a system call each, which does not block, and it starts on its chunks
without waiting for them. A thread that finds no seat (more were awake than the region seats) spins only for what was
left of its 50 µs, and sleeps. Sleeping and waking meet as joining and closing do: the thread marks its bed asleep and
then looks at `signal` again; the caller bumps `signal` and then looks at the beds, and wakes a bed marked asleep with
a compare-and-swap; both sequentially consistent, so either the caller wakes the thread or the thread sees the region.
The stop wakes every bed the same way. On macOS the threads run at the user-interactive quality of service.

### What it changes for programs and hosts

- `parallel` is a keyword: a program that named something `parallel` must rename it.
- A program whose iterations write only their own elements, and read only what none writes, computes what it would on
  one thread, bit for bit, whatever the number of threads and the order they run in: the tests compare every result
  against `threads = 1`.
- A region is a call, a context, and its synchronisation: about 4 µs on an M5 with every core taking part, and under
  2 µs with two (below), against 0.05 µs run alone. A region is worth it when its iterations take tens of microseconds
  together, a synth's voices, not a block's samples.
- `Program::Options::threads` (`fminus --threads`) is the most threads a region uses, the caller's among them: 1 turns
  the regions into plain loops on the calling thread, and starts no pool.
- Knob versioning copies a method with a region as any other; the region's body is not copied, and reads the copy's
  constant through the context.

### Tests

- `tests/unit/ParallelTest.cpp`: through the JIT with the pool started, every iteration once over ranges empty,
  reversed, negative and long, `<=`, `continue`, and a block's statements, a block of two loops, a grid of nested
  loops; a synth whose voices render in a region (and each voice's own region inside it), against the same program
  with `threads = 1`, bit for bit, state and all; a knob, a stream (flat and not) and the method's variables read in a
  region; `Denormals::Flush` flushing on every thread; every error's text and place, and the code that passes. And the
  pool from C++: every chunk once over many ranges, several threads at most `threads` of them, the caller's mode on
  every thread, a region in a region on its own thread, two callers at once, 20,000 small regions, and none of the
  pool's threads left after the last stop; and, paced as an audio callback runs them with every pool thread asleep
  before each, that a region of two iterations costs the pool's threads under 150 µs of CPU time (one thread's spin,
  where waking all of them cost each its 50 µs), and that a region with work for every thread still runs on several
  (on five threads or more, and not on Windows, which has no POSIX clocks).
- `tests/library/ParallelTest.cpp`: `Program::compile` of a synth whose nonblocking `process` has a `parallel for` and
  a `parallel { }`, with a knob and a stream, against `threads = 1`, bit for bit; two Programs run from two threads at
  once, three times over.
- `tests/lit/fminus/parallel.fm`: the dump, the `-O0` IR (the context, the call, the bodies, the block's `switch`), the
  result through `--run`, with `--threads 1`, and from an executable; `Inputs/parallel/races.fm`'s errors.

Run them under `tsan` too (`ctest --preset tsan -R parallel`): the pool's protocol is what ThreadSanitizer checks, and
the stress test is there for it.

### Measurements

On an M5 (four performance cores, six efficiency cores), `fminus --run`, the median of three runs:

| Program | 1 thread | 2 | 4 | 10 (every core) |
|---|---|---|---|---|
| 16 voices of 32 partials, 4,096 samples a block, 500 blocks | 2.63 s | 1.41 s | 0.87 s | 0.63 s (4.2×) |
| 200,000 regions of 16 iterations that do nothing | 0.01 s | 0.28 s | 0.57 s | 0.78 s |

The voices scale with the performance cores, and less with the efficiency cores, which take fewer chunks. The empty
regions are the cost of the synchronisation alone, which grows with the threads that take part: a chunk's
compare-and-swap and the count of who is inside travel between the cores' caches. It depends on where the system puts
the threads, too: a run whose two or four threads all landed on one cluster's cores took 0.09 s.

Paced as an audio callback runs them, one region every 1,333 µs (64 samples at 48 kHz), so that every pool thread
sleeps before each: a host of the runtime alone, 2,000 regions a run, three runs of each (two of the four-iteration
one), on an M5 under a load average of about 130 from other builds. Before, a caller woke every sleeping thread for
every region, and each spun its 50 µs after it, whether it found a seat or not; now it wakes only those the region
seats:

| Region | The pool's CPU a block, before | now | The caller's time in it (median), before | now |
|---|---|---|---|---|
| 2 iterations of 5 µs (one seat) | 519 to 534 µs | 60 µs | 18 to 19 µs | 8 to 10 µs |
| 4 iterations of 10 µs (three seats) | 519 to 539 µs | 192 µs | 24 to 25 µs | 18 to 20 µs |
| 16 iterations of 20 µs (nine seats, every thread) | 575 to 595 µs | 593 to 622 µs | 53 to 100 µs | 61 to 70 µs |

A region that seats every thread wakes them as before, with a system call each where one woke them all; within this
machine's noise, it costs the same.

## Recipe 23: math kernels of FMinus's own

*In the compiler. Code: `src/runtime/MathKernels.h` (the kernels), `src/runtime/Math.cpp` (the runtime's functions on
them), and the runtime's `-ffp-contract=off` (`src/CMakeLists.txt`); `tanf`'s row in `kBuiltins` (`src/ir/Calls.cpp`).
Tests: `tests/unit/MathTest.cpp` (`[math]`, and `[exhaustive]` on demand). Fits: `Scripts/fit-math-kernels.py`.*

Recipe 15's vector mode keeps its promise, one result for a float wherever it is computed, and it kept it by trusting a
library: libsystem_m's vector functions were lane-independent (or made so, for sin and cos), so a scalar `sinf` could
be lane 0 of the four-lane function of four copies of its argument. That had three costs. Every scalar call computed
four lanes to keep one. The promise held on Apple systems only: elsewhere the mode was libm's, a lane at a time, so a
program's last bits depended on the machine. And a macOS whose library changed would change them; MathTest.cpp could
only notice. This recipe gives the vector mode functions of FMinus's own, lane-independent by construction and the same
on every target. Recipe 24 then puts them inline, which is where the time goes.

### One sequence of operations, written once

Each function is a template over the operations it uses (`MathKernels.h`):

```cpp
    template <class O>
    typename O::F sin(O& o, typename O::F x)
    {
        using F = typename O::F;
        const F a = o.abs(x);
        F t;
        const F r = reduce(o, a, t);
        const F result = sinOfQuadrant(o, r, o.bits(t), o.bits(x));
        return o.large(o.gt(a, o.c(kLargeArgument)), x, result, Large::Sin);
    }
```

`O` is `Scalar`, whose `F` is a float and whose `o.fma(a, b, c)` is one fused multiply-add, or `Vector<4>`, whose `F`
is clang's four-float vector and whose operations are each one operation a lane (and recipe 24's `IROps`, whose
operations are IR instructions). The runtime's `fminus_rt_sinf` is `sin<Scalar>`, and `fminus_rt_sin_f4` is
`sin<Vector<4>>`. A lane goes through the same IEEE operations as the scalar
function, in the same order, so it gets the same result, whatever its neighbours hold: it follows from how the
functions are written, and MathTest.cpp only confirms it. What makes it hold on every target:

- **IEEE's basic operations only**: add, subtract, multiply, divide and fused multiply-add, each rounded once to
  nearest, comparisons, an int's conversion to a float, and bit operations (abs, the sign, the exponent field). No
  libm, and no instruction whose result is an approximation (NEON's FRECPE and x86's RCPPS differ). An x86-64 computes
  each of them as an arm64 does.
- **Fused multiply-adds where the kernel says so, and nowhere else.** `o.fma` is the only place a multiply and an add
  are fused: each operation is a call of its own, and the runtime compiles with `-ffp-contract=off`, as the examples'
  C++ does. An x86-64 without FMA (before Haswell) computes `fmaf` in its C library, which C requires to round
  correctly: the same bits, more slowly.
- **No branches.** The special cases (NaN, infinities, zeros, the ends of a function's range) are selects, so every
  lane runs every operation. The one exception is an argument of sin, cos or tan past `kLargeArgument` (below).
- **A whole number without a rounding instruction.** Adding 1.5 × 2^23 (`kRound`) to a float under 2^22 in magnitude
  rounds it to an integer (to even, as IEEE's rounding does), which the sum's low bits hold, and subtracting it again
  gives the integer as a float: two adds, where SSE2 has no rounding instruction, and the integer's bits come with
  them, for the quadrant and for exp's power of two.

### The kernels

| Function | Argument reduced | Approximated | Then |
|---|---|---|---|
| `sinf`, `cosf` | \|x\| = q π/2 + r, r in [-π/4, π/4]: q by `kRound`, r by three fused multiply-adds with π/2 in three floats (Cody and Waite), the first exact, so nothing cancels | sin r = r + r³ P(r²), P of degree 2, and cos r = 1 + r² Q(r²), Q of degree 3 | one of the two by q's parity (cos: q + 1's), the sign by q's second bit (and x's, for sin) |
| `tanf` | as sin's | sin r and cos r, as above | sin r / cos r for an even q, -cos r / sin r for an odd one: one divide; x's sign |
| `expf` | x = k ln 2 + r, r in [-ln2/2, ln2/2]: k by `kRound`, r by two fused multiply-adds | e^r = 1 + r + r² Q(r), Q of degree 4, in Estrin's form | × 2^k in two multiplies, so a subnormal result is rounded once |
| `logf` | x = 2^e m, m in [√½, √2), by its bits (a subnormal x scaled by 2^23 first) | log(1 + f) = f + f² P(f), f = m - 1, P of degree 9, Estrin's form | + e ln 2 in two fused multiply-adds |
| `tanhf` | none | below atanh ½ (0.549), \|x\| + \|x\|³ T(x²), T of degree 4; above, (1 - u) / (1 + u) with u = e^-2\|x\| by exp's polynomial | x's sign |

The polynomials are minimax fits, from Remez's exchange in 60-digit decimal arithmetic, with float coefficients:
`Scripts/fit-math-kernels.py` fits them again (it needs no package). Each one's own error is under 0.1 ulp; the rest
of a function's error is the rounding of its operations. Horner's form takes the fewest operations; Estrin's (pairs of
coefficients joined by powers of the argument) halves the chain of dependent fused multiply-adds, for the two long
polynomials.

**Arguments past 2^16.** Three floats of π/2 hold the reduction to a float's precision a little past 2^16. Past
`kLargeArgument`, 2^16, and for an infinity, sin, cos and tan reduce out of line, exactly (`largeKernel`): |x| = m 2^s,
with m a 24-bit integer, times the 128 bits of 2/π that start where its lowest bit meets them, is |x| 2/π mod 4 to 64
bits past the point, in integer multiplies (Payne and Hanek's reduction, `reduceLarge`); then the same polynomials. A
vector takes that path for its lanes past the limit, and the other lanes keep their result. A sine at 2^16 radians has
already lost its phase to float's spacing (2^-7 there), so audio does not meet it; a phase that grows without wrapping
does, after 23 seconds of 440 Hz, and then pays for the exact reduction, correct and slower.

### Accuracy, and the special values

Every float through every kernel, against the exact result (the double function, whose own error, 2^-53, does not show
at this scale); the old vector mode, libsystem_m's functions on an M5 with macOS 27, over every fourth float:

| Function | Largest error | Not correctly rounded | Recipe 15's vector mode (Apple) |
|---|---|---|---|
| `sinf` | 1.56 ulp | 7.9% | 2.73 ulp, 17.8% |
| `cosf` | 1.56 ulp | 8.0% | 2.70 ulp, 63.2% |
| `expf` | 1.05 ulp | 0.40% | 1.79 ulp, 2.1% |
| `tanhf` | 1.52 ulp | 0.78% | 2.78 ulp, 4.0% |
| `logf` | 0.99 ulp | 0.28% | 0.82 ulp, 0.32% |
| `tanf` | 3.35 ulp | 22.2% | none (`_simd_tan_f4`, the fast mode's: 3.90 ulp, 25.8%) |

libm's are within 1 ulp. What is left in sin and cos is the reduced argument's own rounding: near a result of 1/2, r's
last bit is worth nearly an ulp of the result. tan has it twice over, since tan's slope is 2 at a quadrant's edge, and
the quotient adds its own rounding and its terms'. A double-float r (its rounding error carried in a second float) would
take it out, at about twice the reduction's work in a vector; the kernels do not.

The special values are C's (Annex F): sin, tan and tanh keep a zero's sign; cos(±0) and exp(±0) are 1; sin, cos and tan
of an infinity are NaN; exp(+∞) is +∞ and exp(-∞) is +0, and exp's last finite result is at 88.7228317 and its last
nonzero one at -103.972076, as the exact function rounds; tanh(±∞) is ±1; log(±0) is -∞, log(+∞) is +∞, and log of
anything negative is NaN; and a NaN gives a NaN. A NaN's payload is not kept.

**Flush-to-zero.** Under `Denormals::Flush` (recipe 20), a subnormal argument reads as its signed zero and a subnormal
result is a zero, and a constant must fold to what the processor computes. LLVM folds an add, subtract, multiply,
divide or comparison flushing, and a fused multiply-add not (`ConstantFoldScalarCall`). So a kernel reads a subnormal
argument only where both give the same result, and an odd function's last operation, the one that gives x for a tiny
x, is an add (sin, tanh) or a divide (tan), never an fma: sin(r) is `r + (r z) P(z)`. Each kernel run with FPCR.FZ set
gives, for every float, what it gives without it of the argument flushed, flushed. Where Arm and x86 flush differently
(Arm by the result before rounding, x86 after), only an exact result just below 2^-126 tells them apart, and no
kernel's last operation makes one (exp's last multiply is exact for a normal result).

### `tanf`, a builtin

A filter in the topology-preserving form (an SVF, a ladder, a one-pole) needs g = tan(π fc / fs), every sample while
its cutoff moves. With five transcendental builtins it was `sinf(w) / cosf(w)`: two calls, a divide, and two
reductions of one angle. `tanf` is a builtin now, the way recipes 1 and 15 add one: a row with its stem,
`Builtin{"tanf", llvm::Intrinsic::tan, 1, "tan"}`; three runtime functions, `fminus_rt_tanf`, `fminus_rt_tan_f4` and
`fminus_rt_libm_tanf` (in `Runtime.h` and `RuntimeSymbols.def`), whose names a program that calls it may not declare;
`fminus::math::tan`; and the builtin count in `CodeGenTest.cpp`, a row in `MathTest.cpp`'s table and the VSCode
grammar's list. In the fast mode it is `llvm.tan` (LLVM 19 added it): libm's `tanf` for a float, and on Apple systems
libsystem_m's `_simd_tan_f4` for a `float4`. Its kernel is sin's and cos's reduction and polynomials, and a divide.

### What it changes for programs and hosts

- **Every program's results move in their last bits, once.** The vector mode's sines are now FMinus's: `waveforms.fm`'s
  `main 0.25 1000` is 0.4867599, libm's, where libsystem_m's gave 0.48676.
- **`tanf` is a builtin.** A program's prototype of it, `float tanf(float x);`, calls the builtin now, as a prototype
  of any builtin does; a program's own function named `tanf` is an error ("'tanf' is a builtin and cannot be
  redefined"): rename it.
- **The default mode's results are the same on every machine**: an x86-64 computes what an arm64 does, where before
  it computed libm's, a lane at a time. (Neither Linux nor Windows has been run with it.)
- **`fminus::math` is the kernels**, so a C++ reference still agrees with a program bit for bit; `fminus::math::tan`
  is new, with `tanf`.
- **The runtime has three more functions**, `tanf`'s, which a host that hides or strips symbols must keep
  (`RuntimeSymbols.def`).
- **Nothing depends on the system's library**: the premise test, the `kSinCosLimit` guard, and the declarations of
  libsystem_m's functions are gone from the vector mode (the fast mode still uses them, recipe 8).
- **A call costs about what it did.** Out of line, a scalar kernel computes one lane, not four, but makes its fifteen
  or so constants again on every call (Measurements).

### Tests

`tests/unit/MathTest.cpp`:

- each lane of the four-lane functions against the scalar function, over a million vectors of mixed arguments (phases,
  audio's ranges, zeros of both signs, subnormals, infinities, NaN payloads, the floats around 2^16 and around 2^22 π/2,
  and any float at all), and the scalar function as lane 0 of four copies;
- each function's largest error over every 127th float, against its bound (the table above), and above 0.5 ulp;
- the special values, exp's edges among them;
- under flush-to-zero (FPCR.FZ, or MXCSR's FTZ and DAZ), every seventh subnormal of either sign, the least normals,
  exp's subnormal results and every 4099th float, scalar and in lanes, against the flushed argument's result, flushed.

A hidden test, `fminus-tests "[exhaustive]"`, runs the bounds, the lanes and flush-to-zero over every float, on every
core (about ten minutes on an M5); run it after changing a kernel. The promise's tests (recipe 15's) run unchanged.

### Measurements

The M5, the dev build, with other builds running (load averages of 90 to 180, so times are unreliable; instruction
counts are not). Each runtime function called from C++, instructions a float:

| Function | libsystem_m, scalar | kernel, scalar | libsystem_m, four lanes | kernel, four lanes |
|---|---|---|---|---|
| `sinf` | 66 | 75 | 17.0 | 22.8 |
| `cosf` | 69 | 75 | 17.8 | 22.8 |
| `expf` | 72 | 68 | 17.0 | 17.0 |
| `tanhf` | 73 | 96 | 17.3 | 23.3 |
| `logf` | 97 | 90 | 23.3 | 22.3 |

A scalar call of libsystem_m computed four lanes behind a splat; the kernel computes one, but materialises its constants
in every call, a third of its instructions. Apple's four-lane functions are leaner than the kernels' (sin and cos
compute both polynomials). Fifteen audio programs (recipe 24 has them) take 3% to 29% more instructions a sample than
with libsystem_m, the most where `tanhf` is called a sample at a time. Recipe 24 inlines the kernels: their constants
leave the loops, and the call with them.

## Recipe 24: the math kernels inline

*In the compiler. Code: `src/opt/MathKernels.h` and `MathKernels.cpp` (the pass, and the kernels as IR), registered in
`optimiseModule` (`src/support/HostTarget.cpp`); the math intrinsics in `src/ir/Math.cpp` (`mathFunction`). Tests:
`tests/unit/MathTest.cpp` (`[math]`), `tests/lit/fminus/math.fm`, `versions.fm` and `examples.test`.*

Recipe 23's kernels are FMinus's own, and out of line they cost about what libsystem_m's calls did. A scalar `sinf` in
a loop is a call: every live float in a caller-saved register is spilled around it, and the kernel makes its fifteen or
so constants again each time. Audio's loops mostly carry state (a filter's integrators, an envelope, a phase), so the
loop vectoriser leaves them scalar, and every sample pays. This recipe puts the kernel itself where the call was, as
IR, built from the same template the runtime instantiates: the backend schedules it with the code around it, keeps its
constants in registers across the loop, and two kernels of one angle share their work. The results do not change: the
IR computes the runtime's operations in the runtime's order.

### FMinus's math intrinsics

Inlined in the code generator, a kernel would be thirty instructions and a branch where LLVM's passes look for one
call: LICM could hoist it out of a loop only piece by piece, GVN would merge two of one argument only if it merged all
thirty pairs, and the loop vectoriser gives up on a loop whose body has a call behind a branch (a large argument's).
So the code generator still emits a call, of a function no module defines, FMinus's *math intrinsic* (`mathFunction`,
`src/ir/Math.cpp`):

```llvm
  %0 = call float @fminus.sin.f32(float %x)                  ; a float's sinf
  %1 = call <4 x float> @fminus.sin.v4f32(<4 x float> %v)    ; a float4's

declare float @fminus.sin.f32(float) #1
; #1: nounwind speculatable willreturn memory(none) ..., and its two- and four-lane variants (below, Two lanes):
;     "vector-function-abi-variant"="_ZGV_LLVM_N2v_fminus.sin.f32(fminus.sin.v2f32),_ZGV_LLVM_N4v_fminus.sin.f32(...)"
```

They have the attributes recipe 15 gave the runtime's functions, so LLVM does with them what it does with `llvm.sin`
but evaluate them: LICM takes one out of a loop, GVN merges two of one argument, and the vectorisers widen a loop of
them into four-lane calls. Their names have a dot, which no FMinus identifier has, so no program can declare one, and a
call of one that reached the linker would fail loudly. The runtime's three names stay reserved (recipe 15's
`checkMathNames`): the pass calls them.

### The pass

`MathKernelsPass` (`src/opt/MathKernels.cpp`) runs last, at `registerOptimizerLastEPCallback`, after the vectorisers,
and at every level, since nothing else defines the intrinsics. From `-O1` it replaces each call with its kernel; at
`-O0` with a call of the runtime's function of that width (`fminus_rt_sinf`, `fminus_rt_sin_f4`), so `fminus -O0`
still reads like `clang -O0`. Then the intrinsics' declarations go, and their entries in `llvm.compiler.used`.

The kernel is `runtime/MathKernels.h`'s template instantiated on `IROps`, whose operations are IRBuilder's: `o.fma` is
`llvm.fma`, `o.add` an `fadd` with no fast-math flags, `o.select` a `select`, `o.bits` a `bitcast`, all on a `float` or
on the call's vector type, whatever its width. Each instruction is named after the kernel, so it reads as one:

```llvm
define float @wave(float %phase) {                      ; return sinf(6.28318531f * phase);
entry:
  %0 = fmul float %phase, f0x40C90FDB
  %sin = call float @llvm.fabs.f32(float %0)
  %sin2 = call float @llvm.fma.f32(float %sin, float f0x3F22F983, float f0x4B400000)   ; 2/pi, kRound
  %sin3 = fsub float %sin2, f0x4B400000                                                 ; the quadrant q
  %sin4 = call float @llvm.fma.f32(float %sin3, float f0xBFC90FDB, float %sin)         ; |x| - q pi/2, in three
  ...
  %sin26 = select i1 %sin24, float %sin25, float %sin21                                 ; the sign
  %sin27 = fcmp ogt float %sin, 6.553600e+04
  br i1 %sin27, label %sin.large, label %sin.join, !prof !2                            ; unlikely
sin.large:
  %sin28 = call preserve_allcc float @fminus.large.sin(float %0)
  br label %sin.join
sin.join:
  %sin29 = phi float [ %sin26, %entry ], [ %sin28, %sin.large ]
  ret float %sin29
}
```

Four details carry it:

- **A large argument's branch.** `o.large`, the one branch in sin, cos and tan, splits the block where the kernel is: an
  unlikely branch to a block that calls the runtime's scalar function for each lane past 2^16, a `select` keeping the
  other lanes' results, and a phi. The block calls `fminus.large.sin`, an internal function of the `preserve_all`
  calling convention that calls `fminus_rt_sinf` and saves every register that call may change. With a call of C's
  convention there, every value live across the loop around the branch (the kernels' constants among them) had to
  survive a call that may change 24 of the 32 vector registers, and the register allocator spilled them and reloaded
  them every iteration: tremolo's `process` had 72 loads from the stack, most of them in its loops, and has 13 with the
  wrapper. Neither function is speculatable, so no pass hoists the call out of its branch. The branch costs a compare
  and a branch never taken.

  On x86-64 with AVX the wrapper saves `ymm1`-`ymm15` and restores them before it returns, and LLVM's vzeroupper
  insertion (`X86InsertVZeroUpper.cpp`) then put a `vzeroupper` between the restores and the `ret`, zeroing the upper
  halves it had just restored. The caller of an 8-lane kernel keeps the 256-bit vector in a register across the
  calls, so when any lane was past 2^16, an infinity or a NaN, the upper four lanes of each vector came out as the
  kernel's result for 0: `cosf` gave 1 there (on arm64, and at 4 lanes, nothing changed). So there the wrapper's
  `target-features` are the machine's and `-vzeroupper` (a function's features replace the machine's, so it names
  them all), and LLVM puts no `vzeroupper` in it; the one LLVM put before the runtime's call is right, since it lets
  the runtime's SSE code run without the cost of upper halves in use, so the wrapper calls `llvm.x86.avx.vzeroupper`
  there itself:

  ```llvm
  define internal preserve_allcc float @fminus.large.sin(float %0) #4 {
  entry:
    call void @llvm.x86.avx.vzeroupper()
    %1 = call float @fminus_rt_sinf(float %0)
    ret float %1
  }
  attributes #4 = { cold ... memory(none) "target-features"="+avx,+avx2,...,-vzeroupper" }
  ```

  `MathKernelsPass` takes the target machine for this (`HostTarget.cpp` passes it). `preserve_all` is FMinus's only
  convention other than C's, and `fminus.large.<sin|cos|tan>` the only functions that have it.
- **Folding.** The builder is the code generator's `DenormalFolder` (recipe 20), flushing as the function computes,
  and after the pass EarlyCSE, InstSimplify and SimplifyCFG run on the functions it changed. A constant's kernel folds
  to a constant: `sinf(0.5f)` is `ret float f0x3EF57744`, the runtime's result, and a frozen parameter's `sinf` leaves
  its loop entirely. LLVM folds `llvm.fma` without flushing, which recipe 23's kernels are written for: under
  `Denormals::Flush` a constant folds to what the processor computes, flushing.
- **Sharing.** EarlyCSE merges identical instructions, so `sinf(w)` and `cosf(w)`, the reduction and both polynomials,
  are one kernel and two selects: a filter's `sinf(w) / cosf(w)` costs one reduction.
- **Every width.** The pass builds the kernel at whatever width the call has: a float's, two lanes' or four lanes'
  (below; a `float8` or a wide `poly float` is cut into fours, as recipe 15 did), the vectorisers' as the code
  generator's.

### Two lanes

A stereo effect runs two recursions side by side, one a channel (a drive's two one-poles, a filter's two states), so
the loop vectoriser leaves its loop alone, and each channel's `tanhf` is a scalar kernel. The SLP vectoriser can pair
the two channels' work into two-lane vectors, but pairs a call only at a width that has a vector variant, and the
scalar intrinsic named only the four-lane one. So it names the two-lane one too, `fminus.sin.v2f32`, declared and kept
like the other (`mathFunction`), and `lowerMath` calls it for a `float2` and a two-lane `poly float`, which recipe 15
padded to four; a one-lane `poly float` is the float's call. The pass builds a two-lane kernel as any other, and at
`-O0` calls the runtime's scalar function a lane, since the runtime has no two-lane function (none is needed: nothing
but the pass calls one). The drive's loop, at `-O2`, is one recursion of two lanes:

```llvm
  %1 = phi <2 x float> [ %0, %for.body.lr.ph ], [ %9, %for.body ]   ; left and right
  ...
  %tanh = call <2 x float> @llvm.fabs.v2f32(<2 x float> %6)
  ...
```

A lane computes what a float does, so nothing changes but the time: every program measured gives the same output as
with recipe 24's four-lane variant alone, and the drive takes 125 instructions a sample where it took 153, and 21.8
cycles where it took 30.3 (-28%). The other fourteen programs run the same code, but for the three-band EQ, which
takes 13% more instructions to compile. Whether SLP pairs two calls is its cost model's choice: in the drive it took
the two whole recursions; in the shaper it paired nothing.

### Shorter chains

Inline, a kernel's latency is on the path of any recursion through it (a ladder filter's tanh), so the kernels were
rewritten where that shortens the chain and leaves every result as it was (checked over every float against recipe
23's):

- **The sign** (`o.flipSign`): for a float, a negation chosen, which stays in a floating-point register, where an
  exclusive or of the sign bit would take it to an integer register and back; for a vector, the exclusive or, two
  operations where a select would be three.
- **exp's scale**: 2^k1 (1 + s) is one fused multiply-add, `fma(s, 2^k1, 2^k1)`, which rounds as `1 + s` does, scaled,
  so it gives that sum's rounding, one operation sooner.
- **tanh**: |x| clamped by one `minnum` at 43.5, and the -2 of e^-2|x| folded into log2(e)'s constant, exactly.
- **log's special cases**: one select, of a value chosen off the polynomial's path, by an integer range test of the
  scaled argument's bits (never subnormal, so the bits tell the cases apart as comparisons would).

### What it changes for programs and hosts

- **No result changes.** The kernels inline compute the runtime's operations, so every float gets recipe 23's result,
  in every lane, at every level (MathTest.cpp checks them against the runtime over every 127th float, and the hidden
  `[exhaustive]` test over every float).
- **Programs are faster**: of fifteen measured, twelve call a math builtin every sample, and take 10% to 48% fewer
  instructions a sample and up to 31% fewer cycles, but one, tremolo, 20% more cycles (Measurements); a vectorised loop
  of the builtins runs about twice as fast.
- **Two channels can share a kernel** (Two lanes): the SLP vectoriser pairs a stereo pair's calls into one two-lane
  kernel where its cost model finds it worth it, and a `float2` is two lanes, no longer four.
- **Constants fold, exactly.** A frozen parameter's `sinf`, `expf`, ... folds to the runtime's result; recipe 15's
  vector mode could only call it. A frozen copy of a program can stay in the vector mode now, bit for bit.
- **Compilation takes longer**: every call is now its kernel in the backend, in every knob-versioned copy and in every
  piece a vectorised loop is cut into (its interleaved body, its epilogue, its remainder). The fifteen programs
  measured take 39% more instructions to compile (Measurements).
- **The IR from `-O1` has no call of the vector mode's functions** but a large argument's, through `fminus.large.sin`;
  at `-O0` it has the runtime's calls, as before. Programs that call no math builtin are compiled as they were.
- **The runtime's functions remain**: they are `fminus::math`, `-O0`'s code, and the large arguments' path.

### Tests

- `tests/unit/MathTest.cpp`: the promise's matrix (recipe 15's) now runs the kernels inline from `-O1`, against
  `fminus::math`, bit for bit; every 127th float through a program's kernel of a float, a `float2` and a `float4` at
  `-O2`, against the runtime's function; and constants: fifteen arguments where flushing shows (subnormals of either
  sign, the least normals, exp's subnormal results), each a literal's kernel, folded under `Denormals::Preserve` and
  `Flush`, against what the processor computes in that mode. The `[exhaustive]` test runs the inline kernels too.
- `tests/lit/fminus/math.fm`: the kernels in the vectorised loop and its remainder, their branch for a large argument
  and `fminus.large.sin` (`preserve_allcc`, not speculatable, the branch unlikely); `turn`'s cosine and sine sharing
  their reduction; `tanf`'s divide; `float4`, `float8`, `float2` (two lanes, no padding) and 16- and 1-lane
  `poly float` kernels; a drive's two channels' `tanhf` paired into one two-lane kernel; a constant folded to
  `0x3EF57744`; the frozen parameter's sine gone from its loop; `-O0`'s runtime calls; no intrinsic left at either
  level.
- `tests/lit/fminus/math-large.fm`: `sinf`, `cosf` and `tanf` of 4-, 8- and 16-lane `poly float`s with a lane past
  2^16, an infinity or a NaN, each lane against the scalar function, JIT'd at `-O1` to `-O3` and as an executable; the
  wrappers' IR; and on x86-64 with AVX (the lit feature `avx`) their features and their machine code: one
  `vzeroupper`, before the runtime's call, and none between the restores and the `ret`. On such a machine the
  `MathTest.cpp` matrix's 8-lane `poly float` runs fail without the fix too.
- `versions.fm` and `examples.test` check the kernels where they checked the runtime's calls.

### Measurements

The M5, the dev build (LLVM with assertions), with other agents' builds running (load averages of 4 to 50); each
figure is the best of 18 runs, the two compilers alternating, and the instruction counts (`proc_pid_rusage`) do not
move with the load, where the cycles move by a few percent. First each function, in a loop over a buffer (vectorised:
four lanes a kernel) and in a dependent chain (`x = f(x) * 0.5f + 0.75f`, a step's latency), in cycles, against
recipe 15's libsystem_m calls:

| Function | Loop: libsystem_m | Loop: inline | Chain: libsystem_m | Chain: inline |
|---|---|---|---|---|
| `sinf` | 3.43 a sample (17.3 instructions) | 1.78 (9.0) | 49.4 a step (68) | 45.3 (30) |
| `cosf` | 3.48 (18.0) | 1.77 (8.9) | 52.4 (71) | 45.1 (29) |
| `expf` | 3.78 (17.3) | 1.55 (7.2) | 52.6 (74) | 41.7 (29) |
| `tanhf` | 4.80 (17.5) | 2.27 (11.8) | 70.9 (75) | 55.7 (39) |
| `logf` | 4.26 (23.5) | 2.41 (11.3) | 73.7 (119) | 52.2 (39) |

Then fifteen programs as a synthesizer plugin built on FMinus compiles and runs them (`Denormals::Flush`, every float
parameter a stream, knob versioning 8; instruments as 16 voices in 16-sample calls, effects in 256-sample blocks, every
stream flat): its nine example patches, its template voice and effect, and four effects of its library of modules,
which are not in this repository. Per sample (per voice-sample for an instrument):

| Program | Instructions | Cycles |
|---|---|---|
| saw and filter (a sine and cosine a sample for its filter, two `expf`) | 315 → 173 (-45%) | 53.4 → 36.8 (-31%) |
| FM bell | 179 → 94 (-48%) | 28.0 → 20.8 (-26%) |
| SVF swept by an LFO (library) | 346 → 198 (-43%) | 72.9 → 56.3 (-23%) |
| modular voice (the template) | 596 → 464 (-22%) | 96.1 → 74.4 (-23%) |
| shaper: tanh, asymmetric, one-pole tone (library) | 317 → 189 (-40%) | 63.5 → 50.7 (-20%) |
| drive (`tanhf` of each channel in a one-pole's loop) | 253 → 153 (-39%) | 37.5 → 30.3 (-19%) |
| pluck | 45 → 40 (-10%) | 6.8 → 5.9 (-13%) |
| ladder swept by an LFO (library; tanh in its feedback) | 544 → 396 (-27%) | 149.8 → 130.9 (-13%) |
| sine voice | 113 → 76 (-32%) | 14.0 → 12.8 (-9%) |
| three-band EQ, each band designed a sample (library) | 164 → 120 (-27%) | 29.1 → 26.6 (-9%) |
| the template effect | 346 → 257 (-26%) | 53.7 → 52.3 (-3%) |
| filter, echo, gain and pan | within 3% | within 3% |
| tremolo | 102 → 59 (-42%) | 17.9 → 21.4 (+20%) |

Where the math is off a recursion's path the gain is the instructions'; where it is on it (the ladder's tanh, the sine
voice's oscillator) the chain's. Tremolo is slower: its LFO's phase is a field, which LLVM leaves in memory there,
loaded and stored every sample, and with the kernel inline that round trip through memory costs more than it did beside
a call. With the phase in a local, both take about 16.5 cycles a sample; with samples in its parameters' streams, the
kernel inline is 9% faster. The kernel's branch for a large argument costs about 2 of tremolo's 21 cycles.

Compilation, instructions to compile and link each program (`Program::compile` and its first look-ups): 39% more over
the fifteen, from none (echo, which calls no math builtin) to +114% (the shaper, eight `tanhf`s a sample in each
knob-versioned copy of its loop); in time, for example, the modular voice 120 ms to 154 ms in this build.

## Recipe 25: bits: C's bitwise operators, and `hash` and `randf`

*In the compiler. Code: the tokens in `src/frontend/Lexer.l`, their rules and precedence in `Parser.y`,
`UnaryOp::Complement` and five `BinaryOp`s and `AssignOp`s in `Ast.h` (their spellings in `Ast.cpp`); in `src/ir/`,
`Expressions.cpp` (`isBitwise`, `binary`, `arithmetic`, `lowerAssign`), `Hash.cpp` (`hash` and `randf`), and their
names in `Calls.cpp` and `Functions.cpp`. Tests: `tests/unit/BitwiseTest.cpp` (`[bitwise]`), `FrontendTest.cpp`'s
precedence, `tests/unit/HashTest.cpp` (`[hash]`), `tests/lit/fminus/bitwise.fm`, `bitwise-errors.fm` and `hash.fm`.
Example: `examples/noise/`.*

An `int`'s bits were out of reach: FMinus had none of `&`, `|`, `^`, `~`, `<<` and `>>`, and `&` was only the address.
A flag word, a power of two's mask, a table index wrapped with `& (n - 1)`: each took a division or a loop. And noise:
the generators that need nothing but shifts, xors and multiplies (xorshift, a PCG, a counter's hash) could not be
written, so white noise took one with a division and a remainder a sample (Park and Miller's minimal standard, by
Schrage's method), whose every state waits on the last. A loop of it runs a sample at a time. This recipe adds C's
operators, and two builtins that make noise a function of a sample's index.

### Tokens and precedence

Ten tokens join Lexer.l: `|`, `^`, `~`, `<<`, `>>` and `&=`, `|=`, `^=`, `<<=`, `>>=` (`&` was there). Flex takes the
longest match, so `&&` stays one token and `a&&b` is a logical and, as C's maximal munch has it; `<<=` is one token,
not `<<` and `=`. Parser.y gives them C's levels, between `&&` and equality and between relational and additive:

```
%right "=" "+=" "-=" "*=" "/=" "%=" "&=" "|=" "^=" "<<=" ">>="
%right "?" ":"
%left "||"
%left "&&"
%left "|"
%left "^"
%left "&"
%left "==" "!="
%left "<" "<=" ">" ">="
%left "<<" ">>"
%left "+" "-"
```

So `a & b == c` is `a & (b == c)`, as in C (and as clang warns), `a << 1 + 2` is `a << 3`, and `~a == b` is
`(~a) == b`. `&` is unary or binary by where it stands, as `-` is: the unary rule takes `%prec UNARY`, the binary one
`"&"`'s level, and Bison needs nothing else (`-Wcounterexamples` finds no conflict). `~` is a unary operator like `!`.
Each binary rule is one line through `binary()` or `assign()`, and the nodes are enumerators: `UnaryOp::Complement`,
`BinaryOp::BitAnd`, `BitOr`, `BitXor`, `ShiftLeft`, `ShiftRight`, and the same five of `AssignOp`. `-Wswitch` then
lists every switch that needs them: the dump's spellings, the unary operators' lowering, `arithmetic`'s float half,
`compoundOperator`, and `isConstant` (Statements.cpp), for which `~` of a constant expression is one, as in C.

### Ints only

C's bitwise operators take integers, and FMinus's one integer type is `int`; a `bool` takes part as an `int`, as C's
integer promotions say (`~true` is -2). `binary()` checks the operands before anything else, so a pointer gets the same
message as a float, and the result type of a refused operator is `int`, what the expression would have had:

```cpp
        if (isBitwise(op) && (ast::arithmeticOf(lhs.type) != ast::Type::Int
                              || ast::arithmeticOf(rhs.type) != ast::Type::Int))
        {
            error(location, "invalid operands to binary expression (" + quoted(ast::typeName(lhs.type)) + " and "
                                + quoted(ast::typeName(rhs.type)) + ")");
            return poison(ast::Type::Int);
        }
```

`~` of a float is clang's "invalid argument type 'float' to unary expression". A vector is refused as a float is, since
FMinus has no integer vectors ([simd.md](simd.md)). A compound assignment, `x &= 1` of a float `x`, reports its
operator's error and stores nothing (`lowerAssign`): storing the poison `int` into a `float*` would only add an error
clang does not give. An operand that reported an error of its own, the undeclared `y` of `a & y`, lowers to a poison
`float`, which would draw the message too, about a float the program does not have; so the binary case of `lower`,
`~`'s and `lowerAssign` note the error count before they lower their operands, and when it has grown they give a
poison `int` and say nothing more. (Recipe 32 gives every expression that count, `Value::failed`, and every operator
the rule.) A poison value is no test for this: `(int)3e10f` folds to one with no error, and
`(float)((int)3e10f) | 1` is still refused (bitwise-errors.fm).

### Shifts defined for every count

C leaves `x << n` and `x >> n` undefined for a count below 0 or from 32 on, and `x << n` for a negative `x` or one
that shifts a 1 into the sign; a negative `x`'s `>>` is implementation-defined. LLVM's `shl` and `ashr` are poison for
such a count, and a `shl nsw` (which clang does not emit, though it could) for such an `x`, and poison spreads: one
sample's bad shift could become any value, or let the optimiser delete the code around it. A JIT whose output must be
the same on every machine wants none of that, so FMinus defines them, as OpenCL C does:

```cpp
            case ast::BinaryOp::ShiftLeft:
                return builder_.CreateShl(lhs, builder_.CreateAnd(rhs, 31, "shl.mask"), "shl");
            case ast::BinaryOp::ShiftRight:
                return builder_.CreateAShr(lhs, builder_.CreateAnd(rhs, 31, "shr.mask"), "shr");
```

- **The count is its low five bits** (`n & 31`), clang's own lowering for OpenCL C (it names the `and` `shl.mask`):
  `x << 32` is `x`, `x << -1` is `x << 31`. A constant count folds its mask. A variable one costs nothing on a
  scalar: arm64's `lsl` and `asr` by a register, and x86-64's `shl` and `sar`, take the count's low five bits
  themselves, and the backend drops the `and` (bitwise.fm's ASM). A vector shift by a variable count keeps it, one
  `and` a vector.
- **`<<` is a `shl` without `nsw` or `nuw`**: the bits shifted out are lost, a negative value's too, as two's
  complement has them (`-1 << 1` is -2, `1 << 31` the least `int`). C++20 defines it so.
- **`>>` is an `ashr`**: the sign shifts in (`-8 >> 1` is -4), as clang's does.

`&`, `|` and `^` are `and`, `or` and `xor`, and `~x` an `xor` with -1, as clang writes them. `+`, `-` and `*` on `int`
keep their `nsw`: an `int`'s overflow is still undefined, which is what lets LLVM widen a loop's counter. With no
`unsigned`, a logical shift right is an arithmetic one and a mask, `(x >> 17) & 0x7FFF`, and a hash or generator that
multiplies must not overflow an `int`: the reason `hash` is a builtin.

### `hash` and `randf`

`hash(n)` mixes an `int`, a counter or a seed, into a well-spread one; `randf(n)` is a float uniform in [-1, 1) from the
same bits. White noise is then a function of a sample's index, `out[i] = randf(start + i)`: no state but where a block
starts, the same samples whatever the block size, and a loop of it vectorises. The mixer is Chris Wellons's lowbias32,
from his hash prospector (public domain), on the int's 32 bits as C would hold them in a `uint32_t`:

```c
x ^= x >> 16; x *= 0x7feb352d; x ^= x >> 15; x *= 0x846ca68b; x ^= x >> 16;
```

His search found it among the least biased 32-bit hashes of two multiplies: a flipped input bit flips each output bit
close to half the time (within 0.002 of a half, over a million random ints, which is the measurement's own noise). Each
step is a bijection, so two ints never give one hash; `hash(0)` is 0. `Hash.cpp` emits it as instructions, `lshr` for
C's unsigned shift and `mul` without `nsw` for its unsigned product, which wraps. `randf` takes the hash's top 24 bits
as a signed int k (an `ashr` by 8), from -2^23 to 2^23 - 1, and gives k / 2^23 (`sitofp`, then `fmul` by 0x1p-23): 2^24
values, evenly spaced from -1 to 1 - 2^-23, each as likely, their mean -2^-24.

Every step is exact: integer arithmetic, a conversion of 24 bits to a float, which holds them, and a product by a power
of two, which only moves the exponent and never reaches the subnormals. So a builtin made of them gives the same bits
in every lane of a vectorised loop and its remainder, at `-O0` and `-O2`, constant-folded or not, in every math mode,
with subnormals flushed or not, and on every target, with nothing to keep in step: the math kernels needed intrinsics
LLVM cannot see into, a pass and three runtime functions to promise that (recipes 15, 23 and 24), and these need none.
Being plain instructions, they are pure to LLVM: it folds, hoists, merges or removes a call of them as it does an `add`,
and the loop vectoriser widens them into `<4 x i32>` operations, NEON's `ushr`, `eor` and `mul.4s`, with the product by
2^-23 folded into the conversion (`scvtf.4s ..., #23`).

The names are FMinus's own, as `reduce_add` is: a program's definition or prototype of `hash` or `randf` is "'hash' is
a builtin and cannot be redefined". The argument converts as it would to a parameter of type `int` (a float is
truncated toward zero); a pointer or a vector is clang's "passing 'float *' to parameter of incompatible type 'int'".
Two sources that must not repeat each other take two seeds through `hash`: `randf(seed ^ count)` with
`seed = hash(voice)`. An `int` counts 2^31 samples, 12.4 hours at 48 kHz; a source that may run longer wraps its count
before `++` would overflow (`examples/noise/noise.fm`).

### What it changes for programs and hosts

- **Programs gain** the operators and the two builtins; nothing that compiled before compiles differently, except a
  program with a function named `hash` or `randf`, which must rename it.
- **A shift never makes a program undefined**, and means the same on every machine.
- **Hosts need nothing**: the builtins call no runtime function, so there is no symbol to keep and no table to add.

### Tests

- `tests/unit/BitwiseTest.cpp`: `&`, `|`, `^` and `~` against C++'s over pairs of edge values (0, ±1, the least and
  greatest `int`, bit patterns), `bool` operands; the shifts by 0, 1, 31, 32, 33, -1, -32 and the extremes, against
  FMinus's definition in C++, at run time and folded; the compound assignments on locals, elements, fields, bools and
  through pointers; `&` as the address, `&&` and `a&~b`; mixed precedence computed; clang's errors and their places.
- `FrontendTest.cpp`: the dumps of `a | b & c`, `a << 1 + 2`, `~a == b`, `a & b == c` and the unary and binary `&`.
- `tests/unit/HashTest.cpp`: golden values of both (a change to them changes every program's noise), the reference over
  28,000 inputs; the same bits from a vectorised loop, a module's counter in blocks of 1, 3, 64 and 959, in each math
  mode, at `-O0` and `-O2`, flushed or not; a million values' mean, variance and range, and each hash bit set half the
  time; the arity, argument and redefinition errors.
- `tests/lit/fminus/bitwise.fm`: the IR (`and`, `xor -1`, `shl.mask`, `shl` without flags, `ashr`), a constant count
  folded, the dump, and arm64's `lsl` by a register with no `and`; `bitwise-errors.fm`: the errors, one each.
- `tests/lit/fminus/hash.fm`: the instructions inline with no call at `-O0`; the noise loop's `<4 x i32>` vector body in
  every math mode and flushed; a constant folded; `--run`'s values; arm64's `scvtf.4s` by 23.
- `examples.test` runs `examples/noise/`: a second of noise's RMS, the same at `-O0` in the libm mode.

### Measurements

The noise loop, `out[i] = randf(start + i)` at `-O2` on arm64: 49 instructions a step of sixteen samples (four
vectors of four), so 3.1 a sample, all of them integer vector instructions but the four conversions and the stores. A
generator whose state waits on the last sample's cannot vectorise at all.

## Recipe 26: constant tables, stored once

*In the compiler. Code: `src/ir/Memory.cpp` (`declareConstArray`), called by `declareVariable` (`Statements.cpp`), and
`capture` (`Parallel.cpp`). Tests: `tests/unit/TableTest.cpp` (`[table]`), `tests/lit/fminus/tables.fm`.*

A wavetable, a window, a set of filter coefficients: `const float t[16] = {...};` in a function was an array on the
stack, zeroed and filled on every call, as every local array is (recipe 12). Nothing can write a `const` array, so
every call filled it with the same values: at `-O2`, a 16-float table read once a call cost 40 instructions, 35 of them
building the constants and storing them, for one load. Clang stores a `static const` local once, as a private constant
global, and gives a plain local with a constant initialiser a private global to `memcpy` from (`@__const.f.t`), whose
copy InstCombine removes when the array is only read. FMinus's const array is only ever read, so it goes straight to
the global: 5 instructions, an address and a load.

### When an array is a table

`declareVariable` hands a declaration to `declareConstArray` when it is `const`, an array of what FMinus can point to
but a module, and has no `= expr` (an array's is an error). `declareConstArray` lowers the list's elements, in order,
each converted to the element type as `initializeArray` converts it, and looks at what came out:

- **Every element a constant** (`llvm::Constant`): a number, `sizeof`, operators and casts of them, and a builtin of
  constants that folds (`hash(3)`, `poly_lanes()`); `?:`, `&&` and `||` branch even on constants (recipe 3), so a list
  that holds one is not. The array is a global: `private unnamed_addr constant`, aligned as the array, named as clang
  names a local's, `__const.<function>.<name>` (LLVM adds `.1` for a second of one name), and the name is bound to it
  instead of a stack slot.
- **Anything else** (a variable, a call, a knob): a local, as before, with its list's values, computed once, stored
  after the zeroing.
- **Over 1 MiB** (`kMaxTableBytes`: 262,144 floats, 65,536 `float4`s), whatever its list: a local too. A global's
  every byte is built in the compiler's memory, its zeros too: the JIT's object holds them in a read-only section
  (LLVM leaves a constant's zeros out of BSS), and JITLink copies them, so a table costs about twice its size to
  compile, where a stack slot costs nothing until it is called. An array may have 2^30 elements: `fminus --run` of a
  program with a 256 MB table (a two-element list) peaked at 573 MB with the table a global, and at 37 MB with it on
  the stack.

A binding holds an address whichever it is, so every read goes through `lvalue()` and `read()` unchanged: the name
stands for a `const T*`, an element through it is read-only, and none can be written. A parallel region reads a table
where it is, as the function around it does, rather than capturing its address into its context (`capture`).

### Its contents, bit for bit

The global holds what the stack's array would: the same lowered and converted values (folded by the same
`DenormalFolder`, so under `Denormals::Flush` too), then zeros. The zeros take clang's shapes (`EmitArrayConstant`):
up to the last element that is not zero, then, for eight zeros or more, one `zeroinitializer`, the two parts a packed
struct (`<{ [2 x i32], [30 x i32] }>`, laid out as the array), so a long table with a short list costs no constant per
element; an array without a list is a `zeroinitializer`. From `-O2` a constant index reads the constant itself.

The one thing C could tell apart is the address. Each call of a C function has its own local array, so nested calls
of one function hold two; a table is one object for every call, and `unnamed_addr` lets LLVM merge two equal tables
into one, so a comparison of pointers into two of them (`p == q`) can find them the same. The README lists it with the
deviations.

### What it changes for programs and hosts

- **Results do not change**, bit for bit; a const array of constants costs no time where it is declared. Recipe 28's
  tables at the top level take the same shapes (`tableContents`).
- **A large table takes its size in the JIT's memory**, once, where it took it on the stack, on every call: a table of
  100,000 floats with a short list is 400 KB of `__const` in the object, zeros included. One over 1 MiB stays on the
  stack, as before.

### Tests

- `tests/unit/TableTest.cpp`: tables of every element type and shape (a full list, a short one, eight zeros, none, a
  100,000-element one; floats with -0, subnormals, a NaN and conversions from ints; ints from shifts, `~0` and
  `hash(3)`; `float4`s) against the same arrays on the stack, bit for bit, subnormals preserved and flushed; a table in
  a loop, through a `const float*`, in nested calls, in two scopes of one name, in a `parallel for` and in a
  knob-versioned method; a list with a variable, a call's side effects in order, and `?:` on the stack; the errors.
- `tests/lit/fminus/tables.fm`: the globals and their shapes, functions with no alloca, memset or store for them, a list
  that is not constants on the stack, a region's context without the table, a 1 MiB table and one 16 bytes larger on
  the stack, and `-O2`'s folded reads.

## Recipe 27: `switch` and `enum`

*In the compiler. Code: the keywords in `src/frontend/Lexer.l`; in `Parser.y`, the labels (`label`, `statement`,
`unlabeled`), `switch`, the enumerations (`enumeration`, `enumerators`) and `enum Tag` as a type (`type`,
`cast_type`); the tags' scopes in `ParseContext.h`; `SwitchStmt`, `CaseStmt`, `DefaultStmt`, `Enumerator`, `Enum`,
`EnumStmt` and `Program::enums` in `Ast.h`, their dumps in `Ast.cpp`. In `src/ir/`: `Statements.cpp` (`lowerSwitch`,
`lowerLabel`, `bypass`, the `Breakable` stack), `Constants.cpp` (C's integer constant expressions, `integerConstant`
and `fold`; the enumerations, `declareEnum` and `enumerator`), `CodeGen.cpp` (the file's enumerations among the
definitions), the constants read in `Expressions.cpp` and refused as lvalues in `Memory.cpp`, and `Parallel.cpp`.
Tests: `tests/unit/SwitchTest.cpp` (`[switch]`), `tests/unit/EnumTest.cpp` (`[enum]`), `FrontendTest.cpp`,
`tests/lit/fminus/switch.fm`, `switch-errors.fm`, `enum.fm` and `enum-errors.fm`. Example: `examples/waveforms/`'s
`wave`.*

An oscillator picks its waveform, a filter its response, a voice its stage: an `int` whose values a program could only
name in comments, tested by a chain of `if`s. This recipe adds C's two answers: `switch`, which jumps to the statement
of a value, and enumerations, named `int` constants, which a case label can be. Both are C's, with C23's labels, and
with clang's errors.

### Tokens and grammar

Four keywords join Lexer.l: `switch`, `case`, `default` and `enum`. A keyword in an attribute is still a word there,
as C23 and C++ read one (`[[gnu::const]]`), so `[[fminus::default(1000)]]` parses as before: `attribute_word` takes an
identifier or `default` after the `::`.

A label is C23's: a block item of its own, so a declaration may follow one (`case 0: float t = 2 * x;`, an error before
C23) and one may end a block. Where a statement is expected, a label is the start of a labelled statement. C23 writes
it so, and so does Parser.y:

```yacc
block_item
    : declaration | enumeration | unlabeled | label
    ;
statement
    : unlabeled                             { $$ = $1; }
    | label statement                       { $$ = $1; ... the label's body = $2 }
    ;
label
    : "case" expr ":"                       { $$ = ast::Stmt{at(@1), ast::CaseStmt{$2, nullptr}}; }
    | "default" ":"                         { $$ = ast::Stmt{at(@1), ast::DefaultStmt{nullptr}}; }
    ;
```

`unlabeled` is what `statement` was, and `switch "(" expr ")" statement` one more of its rules. A block never asks for
a `statement`, so after `case 1:` in one there is nothing to choose: Bison finds no conflict. `case a ? b : c:` is a
`?:` up to its last colon. In the tree, a label in a block is a `CaseStmt` or `DefaultStmt` without a body, and the
items after it are its block's; `if (x) case 1: s` gives the label a body, `s`.

An enumeration is declared on its own, at the top level, in a module or in a block:
`qualifier "enum" enum_tag "{" enumerators ","? "}" ";"`. It starts with `qualifier`, as a declaration and a function
do, so the three share their first reduction and `enum Mode {` against `enum Mode m;` is the next token's choice, `{`
or not. `enum Mode`, without the braces, is a type in `type` and in `cast_type`: the type `int`, once a tag of the name
is visible. C's `enum Mode { A } m;`, a variable declared with its enumeration, is a syntax error here (recipe 32 takes
it, and C23's fixed underlying type).

### `switch`

`lowerSwitch` is clang's: the value, an `int` (a `bool` promoted to one, as C promotes it), goes to LLVM's `switch`,
whose default is the end, `sw.epilog`, and the body is lowered after it. Each label is a block, `sw.bb` or
`sw.default`, into which the code before it falls (`fallThrough`), and `lowerLabel` adds it to the instruction as it
meets it: a case's value and block (`addCase`), or the default (`setDefaultDest`). So labels may be anywhere in the
body, in a block or a loop inside it too, as C allows; jumping into a loop gives LLVM a loop with two entries, which it
takes as it is. The code before the first label is reached by nothing, and lowers into an `unreachable` block, as code
after a `return` does. `switch.fm`'s `pick` at `-O0`:

```llvm
  switch i32 %x1, label %sw.default [
    i32 0, label %sw.bb
    i32 1, label %sw.bb2
    i32 14, label %sw.bb3
    i32 -3, label %sw.bb4
  ]
```

`break` and `continue` find their targets on recipe 4's stack, renamed `Breakable`: a loop, a switch or a parallel
region, innermost last. `break` leaves the innermost loop or switch; `continue` skips the switches and goes to the
innermost loop's next iteration; a label belongs to the innermost switch, and one met inside a parallel region before
a switch is an error ("'case' cannot jump into a parallel for"), since the region's body is a function of its own.

A jump to a label can pass declarations: `case 0: float t = 2 * x; ... break; case 1: ... t ...`, where `t` is in
scope at `case 1` but its declaration never ran. C allows the jump, and the variable is indeterminate; FMinus starts
every variable at 0, so `bypass` makes the switch's edge to such a label go through `sw.zero`, a block that stores 0 in
each variable of the body's scopes still in scope there (a memset for an array or a module; an enumeration constant
and a const table hold nothing to zero), then branches to the label. Falling into the label from above runs the
declaration as written. mem2reg and dead-store elimination remove the zeros nothing reads.

LLVM makes the most of it from `-O1`: a switch of constants becomes a table (`pick` is a range check and one load), a
switch of two values a `select`, and one on a knob in a method's loop gets knob versioning's copies (recipe 16), one
for each value of its domain, with no switch left in them. SimplifyCFG makes a switch of a chain of `if`s on one `int`
too, so the code is the same either way: `switch` is for the program's reader.

The errors are clang's: "statement requires expression of integer type ('float' invalid)", "duplicate case value
'1'" and, for two names of one value, "duplicate case value: 'A' and 'B' both equal '1'" (a name being the case's
value written as an enumeration constant, casts aside, as clang's `IgnoreParenCasts` has it), "multiple default
labels in one switch", each with "previous case defined here" at the label before, and "'case' statement not in switch
statement". A value with an error of its own (an undeclared name) gets no second one.

### Integer constant expressions

A case's value, and an enumerator's, is an integer constant expression, which a C compiler works out where it is
written: C11 6.6p6's integer constants, enumeration constants, `sizeof`, and floating constants only as the immediate
operands of casts (`(int)2.5f`), combined by the unary operators but `*` and `&`, the binary ones, `&&`, `||`, `?:` and
casts to `int` or `bool`. `integerConstant` (`Constants.cpp`) folds one, over the tree, before anything is lowered:
IRBuilder's folding would not do, since `&&`, `||` and `?:` branch even on constants (recipe 3), an `add nsw` of two
constants folds to the wrapped value without a word, and a division by zero to poison.

`fold` gives each node a value and a type, `int`, `bool` or `float`, and computes as FMinus computes at run time
(`arithmetic`, recipe 25): `/` and `%` truncate toward zero, a shift takes its count's low five bits, `>>` shifts the
sign in. So `case 1 << 33:` is `case 2:`, as `1 << 33` is 2 anywhere else. A `float` is carried by type only, with the
place of its floating constant (`Folded::floating`): its value is never needed, since a `float` can only make the
expression no constant. Where C makes the value an error, it is one, with clang's words:

- a variable, const or not, a call, an assignment, `p[i]` or `*p`: "expression is not an integer constant expression",
  at the operand (C++ would take a `const int`; C does not, nor does FMinus's `isConstant`);
- a `float`-typed whole: "integer constant expression must have integer type, not 'float'"; a floating constant
  elsewhere in an `int` (`1.5f < 2`, `(int)(0.5f + 1)`): no integer constant expression;
- an `int` that overflows (C11 6.6p4: a constant expression's value is in its type's range): "overflow in expression;
  result is -2147483648 with type 'int'", an error where clang warns;
- a division by zero, and the least `int` divided by -1: no integer constant expression, with a note, "division by
  zero is undefined" (clang's warning) or the value out of range.

The last two are errors only where the expression is evaluated: `0 && 1 / 0` is the constant 0, and so is the arm of
a `?:` its condition does not pick, since what is not evaluated has no value to be out of range, as clang has it
(`IK_ICEIfUnevaluated`). What is no constant at all is reported wherever it is, as clang does: `0 && f()` is none.

### Enumerations

`declareEnum` gives each constant its value, its own or the one before it plus 1 (the first's 0), and binds it from
just after its definition, as C11 6.2.1p7 says, so `enum { A = 1, B = A + 1 };` is 1 and 2, and an inner
`enum { A = A + 50 };` reads the outer `A`. A constant past the greatest `int` is clang's "ISO C restricts enumerator
values to range of 'int' (2147483648 is too large)": FMinus has no larger type for it to take. A value in error makes
the constant the next one's after all, as clang counts it, so the rest still count and its uses report nothing more.

C gives enumeration constants and variables one name space, so the constants live where the variables do, in the code
generator's scopes: a `Binding` is a variable (its slot) or a constant (its value, `enumerator`, and no slot), and
each knows where it was declared, for the notes. In a block, a constant is the block's, a variable of an inner block
hides it, and one of the same scope is clang's "redefinition of 'A' as different kind of symbol" when the constant
came first, a plain "redefinition of 'A'" when the variable did (two constants: "redefinition of enumerator 'A'"),
each with "previous definition is here". At the top level and in a module it is the file's, `fileScope_`, as a C
struct's enumeration constants are: `CodeGen.cpp` declares each enumeration in its place among the file's declarations
(since recipe 28, as it lays out each module and defines each constant), so a function sees the constants declared
before it, and a module's methods, which follow the module, see its own. A constant named as a function or a module of
the file is a redefinition too, reported at whichever comes second (`fileNameClash`): "as different kind of symbol",
but for a constant after a function, which clang calls a plain redefinition. A name in a method is looked up as C++
looks one up in a member function (`enumerator`, on `named` since recipe 28): the blocks' scopes, then the module's
fields and methods, then the file's.

The scanner learns none of this, unlike a module's name (recipe 12). A module's name is a type, which the grammar must
know to tell `T * p` from `a * b`; an enumeration constant is an expression wherever it stands, so the grammar never
needs to know one, and a variable may hide it, which a token of its own would make a syntax error. Only the tags are the
parser's, since `enum Mode` must name a declared one: `ParseContext::tags` is a scope for each block being parsed (the
rule `block` opens one at its `{` and closes it at its `}`), and the file's, where a module's tags go too. A second
`enum Mode { }` in one scope is "redefinition of 'Mode'"; `enum Mode` without a visible tag is clang's "ISO C forbids
forward references to 'enum' types", as FMinus declares no enumeration ahead of its constants (C23 does, with a fixed
underlying type, and since recipe 32 so does FMinus).

A constant reads as an `i32` constant (`lower`), so a const table of constants is stored once (recipe 26), a case
label takes one, and a loop whose condition is one (`while (RUNNING)`) is a constant expression, which may run for ever
(`isConstant`, now `Lowering`'s, recipe 19). It is no lvalue (`isLValue`): `A = 1` and `A++` are "expression is not
assignable", and `&A` "cannot take the address of an rvalue of type 'int'". A parallel region reads one where it is,
capturing nothing, and the race walker takes an outer constant for no variable. `enum Mode` names `int`, where C's
enumerated type is compatible with an integer type of the compiler's choosing (GCC's and clang's is `unsigned int` when
no constant is negative, `int` otherwise): a local, a parameter, a result, a module's
`param enum Mode mode [[fminus::range(0, 2)]]`, `sizeof(enum Mode)` (4) and `(enum Mode)x` all mean `int`, and messages
say `'int'`. (Recipe 32's `int` carries which enumeration it is, for `-Wswitch`, and is still `int` to every rule.)

### What it changes for programs and hosts

- **Programs gain** `switch`, labels and enumerations. `switch`, `case`, `default` and `enum` are keywords: a program
  with a variable, a parameter or a function of one of those names renames it, and the circuit compiler refuses them as
  a netlist's names (`isFMinusKeyword`). `[[fminus::default(...)]]` is unchanged. Nothing else compiles differently.
- **A jump past a declaration** finds the variable 0, where C leaves it indeterminate (the README's deviations).
- **Hosts need nothing**: an enumeration holds no memory, so no module's layout changes, and a `param enum Mode` is a
  `param int` to a host, its range and default as written.

### Tests

- `tests/unit/SwitchTest.cpp`: fall-through, `break` and `default` (first, in the middle, none), two labels on one
  statement, a label ending a block (C23), a body that is one labelled statement, a `bool`'s switch; a switch in a loop
  (`break` leaves the switch, `continue` goes to the loop) and a switch in a switch; labels inside a loop and an `if`
  of the body, a jump into the loop, and the declarations a jump passes (a float, a `float4`, an array) read as 0;
  case values of constant arithmetic (shifts by 33 and of negatives, the least `int`, `/` and `%`, unevaluated
  divisions by zero, `sizeof`, casts of floating constants); a switch in a `parallel for`, `continue` in it; clang's
  errors, every one.
- `tests/unit/EnumTest.cpp`: values implicit, explicit, negative and extreme, a trailing comma, constants in a const
  table, arithmetic, an index and a call; `enum Wave` as a parameter, a result, a cast and `sizeof`; the scopes (a
  method sees its module's and the file's, a field hides a file's constant, a block's constant hidden by an inner
  variable, a value reading the outer name); case labels of constants and the duplicates' names; a `parallel for`
  reading constants from three scopes; the errors.
- `FrontendTest.cpp`: the dumps of switches, labels as items and as statements, `case x ? 1 : 2:`, `fminus::default`;
  enumerations where they are among functions and modules, `enum Wave` as a type everywhere, and the tags' errors.
- `tests/lit/fminus/switch.fm`: the `-O0` IR (the switch's cases, the blocks falling into each other, `sw.epilog`,
  `sw.zero`), `-O2`'s table and select, the dump, `--run`; `switch-errors.fm`, each error once.
- `tests/lit/fminus/enum.fm`: constants folded (no load), the table a constant global, the cases, a constant loop
  without `llvm.loop.mustprogress`, the dump, `--run`; `enum-errors.fm`, each error once. `CircuitTest.cpp` checks
  the keywords are refused as names, and `examples.test` runs `examples/waveforms/`'s `wave`.

## Recipe 28: constants at the top level, and sizes of constant expressions

*In the compiler. Code: in `src/frontend/Parser.y`, a declaration among the `items` (`Program::constants`) and an
array's size an expression (`array_size`); `Constant`, `DeclStmt::size`, `Field::size` and `Field::enums`, and
`Param::size` in `Ast.h`, their dumps in `Ast.cpp`. In `src/ir/`: `CodeGen.cpp` (the file in source order),
`Constants.cpp` (`sized`, `checkSizes`, `defineConstant`, `atTopLevel`, `fileNameClash`), `Modules.cpp`
(`prepareModules`, `layOut` where the module is, `fieldType`), `Functions.cpp` (`describeThis`, `named`),
`Memory.cpp` (a top-level constant as an lvalue, `tableContents`, a const array's element assigned), `Expressions.cpp`
(a constant scalar read as its value, `?:`, `&&` and `||` on a constant at the top level, `sizeof` of a module not yet
laid out) and `Statements.cpp` (a local's size); `module()`'s field types in `src/api/FMinus.cpp`. Tests:
`tests/unit/TopLevelTest.cpp` (`[toplevel]`), `FrontendTest.cpp`, `tests/lit/fminus/toplevel.fm` and
`toplevel-errors.fm`. Example: `examples/waveforms/`'s `PI`.*

An instrument's tables are the program's, not a function's: a 17-by-12 table of partials that two functions and a
module's method read, the sizes of a bank of filters, a window. Recipe 26 stores a function's const table once, but it
is that function's: a second function needs a copy of its list, and a module's method a field filled by `init`. And a
size was a number: `float table[204]`, with the stride, 12, written again in every index. This recipe adds C's two
answers. A declaration at the top level declares constants, C's file-scope const objects, which every function and
method after them reads; and an array's size is any integer constant expression, so `ROWS * COLS` names it, with
enumeration constants (recipe 27):

```c
enum { ROWS = 17, COLS = 12 };
const float PARTIALS[ROWS * COLS] = {1, 0.5f, 0.25f};   // the rest are 0
const float GAIN = 0.5f;

float partial(int row, int k) { return PARTIALS[row * COLS + k] * GAIN; }
```

### The grammar

`items` takes a `declaration`, as a block and a module's body do. A declaration and a function share their start,
`qualifier type pointer IDENTIFIER`, and the next token decides, `(` or not, as between a module's field and its
method, so Bison finds no conflict. Each declarator becomes a `Constant`, its `DeclStmt` and how many functions,
modules and enumerations come before it, which `dump` prints where it is: `(decl const float[(* ROWS COLS)] PARTIALS
{1 0.5f 0.25f})`.

An array's size was `"[" NUMBER "]"`, which the parser checked and put in the `Type`. It is an expression now,
`array_size: "[" expr "]" | "[" "]"`, kept as written (`DeclStmt::size`, `Field::size`, `Param::size`), and the
array's `Type` has a count of 0 in the tree: an enumeration constant's value is the code generator's (recipe 27), so
every size is. `[]` takes its size from the list, `float t[] = {1, 2, 3}`, as C's does. A module's body is parsed into
its fields, methods and enumerations apart, so a field records how many of its module's enumerations come before it
(`Field::enums`): its size sees those, and not one declared after it, as in C. The dump shows a size as written,
`int[0x10]`.

### Sizes

`sized` (`Constants.cpp`) works one out with recipe 27's `fold`: enumeration constants, `sizeof`, operators (the
bitwise ones too), casts and `?:`, an `int` or a `bool`, from 1 to 2^30. Its errors are clang's, where clang has them:

- a `float`: "size of array has non-integer type 'float'";
- no constant: "expression is not an integer constant expression", at the operand that is none. C would make a local
  array sized by a variable a variable length array, and refuse one at the top level or in a struct; FMinus has none.
  A `const int` is no constant expression, in C17 as here (C23's `constexpr` object would be): a size is named by an
  enumeration constant, C's idiom, `enum { TAPS = 64 };`;
- negative: "'buf' declared as an array with a negative size"; an overflow, recipe 27's "overflow in expression";
- 0, a GNU extension in C: "zero size arrays are not allowed"; over 2^30: "array is too large: FMinus's arrays have at
  most 2^30 elements" (both FMinus's, as before);
- `[]` without a list: "definition of variable with array type needs an explicit size or an initializer", and in a
  module "field has incomplete type 'float[]'" (C's flexible array member is not FMinus's).

After an error the array has one element, so its uses report nothing more. A local's size is worked out in
`declareVariable`, with its block's enumerations in scope; a field's in `layOut`; a parameter's, `float x[N]`, a
pointer whose size C still checks, in `checkSizes`, where the function is among the file's declarations, with the
parameters before it in scope as variables (C99's `float x[n]` is a variable length parameter, which FMinus refuses as
any size that is no constant). The tree stays as the parser made it: `layOut` keeps each field's type, its count
worked out, in `fieldTypes_` (`fieldType`), and `module()` (the public API) names a field's array from the LLVM
struct's.

### The file in order

The code generator laid out every module, declared every function and then lowered each body, declaring the
enumerations before each. Now a field's size may name an enumeration constant, so a module's layout waits for the
enumerations before it, and a constant is seen by what follows it. Pass 1 still declares every function, so calls may
come before definitions; pass 2 (`CodeGen.cpp`) goes through the file in source order, as C's file scope does:

```cpp
                    if (constant != nullptr && constant->position <= position && constant->enums <= enums
                        && constant->modules <= modules)
                        defineConstant(*constant);              // before the enumerations and modules after it
                    else if (enumeration != nullptr && enumeration->module == nullptr
                             && enumeration->position <= position && enumeration->modules <= modules)
                        declareEnum(*enumeration);              // before the modules after it
                    else if (module != nullptr && module->position <= position)
                        layOut(*module, enums);                 // its own enumerations among its fields
```

each before function `position`, which comes next: its parameters' sizes are checked, and its body lowered. A
module's own enumerations are declared among its fields (`Field::enums`), and the rest after them, for its methods. A
method's `this` was `nonnull align dereferenceable(size)` from pass 1, which needs the module's size; `describeThis`
gives it when `layOut` has it. A module's `sizeof` before its layout is done, in its own fields' sizes or its own
enumerations' values, is C's "invalid application of 'sizeof' to an incomplete type 'M'" (`prepareModules` makes the
struct types opaque before pass 1, and `isOpaque` says which are not laid out yet); its methods see it whole.

### Constants

`defineConstant` makes a constant an LLVM global of its name, `constant`, aligned as its type: `@PARTIALS = private
constant ...`. A table's contents are recipe 26's (`tableContents`): the list's values, then zeros, eight or more of
them one `zeroinitializer`; a scalar without an initialiser is 0, and a table without a list all zeros.

The global is private, as a C file's `static const` object is internal (clang gives a plain const object external
linkage). Only the program's own code reads it, and a symbol of its name would be found by whatever looks one up by
name: the calls LLVM's backend makes to a library's functions, whatever the program calls them (a float `%` calls
`fmodf`, a zeroed array `memset` or `bzero`, `expf` of a variable in the fast math mode `expf`), which would jump into
the constant's data; and a host, `Program::address("GAIN")`, which would have returned the data's address as a
function's. A private global's symbol is local to its object (`L`-prefixed on Mach-O), so neither finds it. Within the
module two kinds of name would still find it, since the code generator declares their functions by name: a builtin's
(`sinf`, `malloc`, `randf`), which C's file scope holds already ("redefinition of 'sinf' as different kind of symbol",
clang's), and a runtime function's (`RuntimeSymbols.def`; FMinus's "'fminus_rt_malloc' is the name of a function of
FMinus's runtime, which the code it generates calls: rename the constant"). Either is refused, and its global left
unnamed.

The initialiser is lowered as a function's would be, into a function of no use (private, `fminus.top-level`, deleted
after, with any declaration only it used), with `function_` a function of no name and no module (`topLevel_`): the same
`lower`, `convert` and `DenormalFolder`, so a constant holds what a function's array of the same list holds, bit for
bit, its subnormals flushed when the host flushes them. Every value must then be an `llvm::Constant` that holds no
poison or undef (`undefined`), or it is clang's "initializer element is not a compile-time constant", at the first that
is not. What C leaves undefined folds to poison: `1 / 0` and `LEN % 0`, which clang refuses too, and `INT_MIN / -1`,
`(int)1e10f` and `(int)(0.0f / 0.0f)`, which clang folds to values (-2147483648, 2147483647 and 0). Read as its value,
with no load, a poison constant would make poison of every function that read it. What folds is a constant: numbers,
enumeration constants, `sizeof`, operators and casts of them, `hash` and `randf` of them, a constant scalar of the top
level (below), the address of a table's element (`&PARTIALS[COLS]`, a constant expression). What the program would
compute as it runs is not: a call (`sinf(1)` too, which every math mode lowers to a call), a table's element (a load),
a heap block. `?:`, `&&` and `||` branch even on constants (recipe 3); at the top level (`atTopLevel`), where nothing
runs, a constant condition is the value it picks, so `N > 2 ? 0.25f : 4` is a constant there, as in C. In a function
they branch as before, so a local table holding one stays on the stack (recipe 26).

FMinus's own errors: a declaration at the top level that is not `const` ("FMinus has no variables at the top level:
declare 'state' const, a constant of the program, or keep it in a module"), since a program's state lives in modules,
each instance's own; a module ("a module is no constant of the top level: its fields start at 0, and only its methods
set them"); and a table over `kMaxTableBytes`, recipe 26's 1 MiB ("array is too large for a constant: FMinus's
constants of the top level hold at most 1 MiB, and this one holds 268435456 bytes"). A local table over it stays on
the stack; a top-level one has nowhere else to go, and the JIT's object would hold every byte of it, its zeros too, in
the host's process, at about twice its size (recipe 26's measure): one line, `const float4 BIG[1 << 24];`, would cost
half a gigabyte. The constant's name goes into `fileScope_`, beside the enumeration constants: a second of the name is
"redefinition of 'A'", and so is an enumeration constant after it, with its note; one before it, a function or a
module of the name is "redefinition of 'A' as different kind of symbol" (`fileNameClash`, which `declareEnum`
shares), reported at the later.

### Reading one

`lvalue` finds a constant after the blocks' scopes and, in a method, the module's fields and methods (`named`, C++'s
lookup in a member function), so a local or a field of the name hides it. A table decays to a pointer as any array:
`getelementptr inbounds float, ptr @PARTIALS, i64 %idxprom` and a load, one object for every function, method and
call, with no copy. A parallel region reads it where it is: it is in no function's scope, so nothing captures it. A
scalar reads as its value, with no load, as clang reads a const object whose initialiser is a constant even at `-O0`
(`fmul float %0, 5.000000e-01`); `&GAIN` is the global's address. So a constant scalar is a constant in another's
initialiser (`const int M = N * 2;`, which C17 leaves to the compiler, 6.6p10, and clang takes), but in a constant
expression it is a variable, as in C: a size, a case's value or an enumerator's.

Assigning to one is clang's error: "cannot assign to variable 'GAIN' with const-qualified type 'const float'", and,
for an element of a const array its variable names, a local's too, "cannot assign to variable 'PARTIALS' with
const-qualified type 'const float[204]'" (it was "read-only variable is not assignable", which stays for an element
through a `const float*`).

### What it changes for programs and hosts

- **Programs gain** top-level constants, sizes of constant expressions and `[]` sized by a list. Every program that
  compiled compiles to the same IR, with four differences a test may see: a module's dump lists its enumerations among
  its fields, in source order; a size's dump is as written; an element of a const array assigned is clang's message;
  and a module's enumeration can no longer take the module's own `sizeof` (C's incomplete type).
- **Hosts**: a top-level constant is no symbol, in the JIT's object or in one `--emit obj` writes: `Program::address`
  finds functions only, as before. `module()` reports an array field's size as it was worked out.

### Tests

- `tests/unit/TopLevelTest.cpp`: the reporting program's case, a 17-by-12 table read by two functions, a module's
  method, a `parallel for` and a pointer into it, through an enumeration's stride, against C++, with one constant
  global and no stack slot or store for it; tables of floats (subnormals, -0, a NaN, `?:`, a constant scalar's value),
  ints (`hash(3)`, `&&`, `||`), `float4`s, zeros and `[]` against the same list on the stack, bit for bit, preserved
  and flushed; scalars of every type, an element's address and `&GAIN`; C's scopes (a local and a field hide a
  constant, a method of a module before it does not see it, an `#include`d table after its `#include` and not before);
  sizes of enumeration constants, operators, `sizeof`, casts and `?:` in fields (against a C++ struct's layout),
  locals, a block's enumeration, tables and parameters; constants named as the library functions LLVM calls (`fmodf`,
  `memset`, `bzero`), run in every math mode at `-O0` and `-O2`, and no symbol for `Program::address`; clang's errors
  (a builtin's name, what C leaves undefined), and FMinus's (a runtime function's name, a table over 1 MiB), and a
  size's error with no second error for its list.
- `FrontendTest.cpp`: the dumps of sizes as written, `[]`, a module's enumerations among its fields, and constants
  where they are among functions, modules and enumerations.
- `tests/lit/fminus/toplevel.fm`: the private globals (an `#include`d one first), functions and a method with no
  alloca, memset or store for them, a scalar read as its value, `-O2`'s loads and folded reads (and the tables only
  those read gone), `--run`, the dump; `toplevel-errors.fm`, the errors from the command line.

## Recipe 29: the loop budget, runaway-loop protection

*In the compiler. Code: `src/opt/LoopBudget.h` and `.cpp` (`LoopBudgetPass`, and the budget's global,
`loopBudgetGlobal`); `ir::Options::loopBudget` and its module flag in `src/ir/CodeGen.h` and `.cpp`; the pipeline's last
pass in `src/support/HostTarget.cpp`; `Program::Options::loopBudget`, `setLoopBudget`, `loopBudgetLeft` and
`loopBudgetExceeded` in `include/fminus/FMinus.h` and `src/api/FMinus.cpp`; `fminus --loop-budget n` in
`tools/fminus/main.cpp`; an executable's in `addEntryPoint` (`src/support/Aot.cpp`), with
`fminus_rt_loop_budget_state` and `fminus_rt_main_loop_budget` in `src/runtime/`. Tests:
`tests/unit/LoopBudgetTest.cpp` (`[budget]`), `tests/library/LoopBudgetTest.cpp` and
`tests/lit/fminus/loop-budget.fm`.*

A program that never stops holds the thread that called it. In a plugin that is the audio thread, and the host waits
for its block: the whole session freezes, and the only way out is to kill the host. It takes one ordinary bug. This
oscillator wraps its phase in a loop, and an infinite frequency (a knob read as a division by zero upstream, say) makes
the phase infinite, and infinity less one is infinity:

```c
module Osc
{
    param float frequency [[fminus::default(440)]];
    float phase;

    void process(float* restrict out, int n) [[clang::nonblocking]]
    {
        const float step = frequency[0] / 48000.0f;
        for (int i = 0; i < n; ++i)
        {
            phase = phase + step;
            while (phase >= 1.0f)
                phase = phase - 1.0f;
            out[i] = phase;
        }
    }
};
```

C has no answer, and a host has no safe one: a thread cannot be stopped from outside without leaving whatever it held.
So the program stops itself. Compiled with the loop budget, it counts its own work against a budget the host sets
before each call, and a call that runs out returns early, with a flag the host reads:

```cpp
fminus::Program::Options options;
options.loopBudget = true;                                    // off by default
auto program = fminus::Program::compile(source, errors, options);
...
program->setLoopBudget(10'000'000);                           // on the audio thread, before each call
process(instance, out, n);
if (program->loopBudgetExceeded())                            // it returned early: silence it, report it
    ...
```

The counting is a pass of FMinus's own, the last of the pipeline, so nothing the optimiser decides depends on it: a call
that stays within its budget computes what it would without it, bit for bit, and runs at the same speed (Measurements,
below).

### What counts

- **A loop whose count is known where it starts** is charged it there, before it runs. That is most of a DSP program's
  loops: a `for` over a block, a vectorised loop and its remainder, a loop over a module's voices or a filter's taps.
- **Any other cycle** counts one at each branch back to its start: a `while` on the data, a loop that leaves through a
  `break`, the phase wrap above.
- **A function that can call itself again** counts one each time it starts, so a recursion that runs long without a
  loop runs out too.

The count is the optimised code's, not the source's. A rotated loop branches back once less than it iterates, a
vectorised loop once a vector of samples, an unrolled loop once a group of iterations, and a loop the optimiser
removed not at all. `halvings(1024)`, a `while` that halves its argument, takes 10 at `-O0` and 9 at `-O2`;
`sum(100)`, a `for` of a float sum, 100 at `-O0` and 24 at `-O2` on arm64, where LLVM interleaves it by four
(`loop-budget.fm`). The budget is a safety net, not a meter: set it to many times what a block should take (a block of
a thousand samples whose voices loop over a hundred partials takes a few hundred thousand at most).

### Where the checks go: last

`LoopBudgetPass` is a module pass, registered at `OptimizerLast` after the math kernels (`HostTarget.cpp`), and runs at
every level (`isRequired`). It acts on a module whose flag `fminus.loop-budget` is 1, which `ir::lower` writes when
`ir::Options::loopBudget` is set, so the option travels with the module to wherever it is optimised, as the math mode
does (recipe 15).

Why last. The code generator could have put a check in every loop it lowers, but LLVM's loop vectoriser leaves alone a
loop that can leave from its middle on a condition it computes (a `for` over a block with a `break` on the data stays
scalar at `-O2`), so the check would have cost every vectorised loop its vectorisation, and a math builtin's loop most
of its speed. After the pipeline,
every decision has been made: the loops that vectorise have, the copies knob versioning makes are made, the kernels are
inline. The pass adds a counter and branches, and no floating-point operation moves.

Which functions it changes, from the module's call graph:

- a function **counts** when it has a cycle, or can call itself again: through its own calls, or through a function it
  hands a runtime function (a parallel region's body, which `fminus_rt_parallel_for` runs). Its cycles are
  `CycleInfo`'s rather than `LoopInfo`'s, irreducible ones included: FMinus's structured code makes none, but a pass
  might, and every closed path through a function passes through some cycle's header from inside that cycle, so
  instrumenting each header's branches back leaves no path that can go round uncounted;
- a function **may stop** when it counts, or calls one that may stop. Each such call is followed by a check of the flag.

In a function that counts, the count is a stack slot, written as the code generator writes a variable, and
`PromoteMemToReg` makes it a register once every check is in place: the function reads the budget once where it starts,
and writes it back before it returns and before each call that may stop (and reads it again after). A loop without a
count:

```llvm
while.body:
  %0 = fmul nnan float %x.addr.08, 5.000000e-01
  ...
  br i1 %cmp, label %while.body.budget, label %while.end

while.body.budget:                                ; on the branch back
  %budget.left = sub i64 %budget.slot.0, 1
  %budget.out = icmp slt i64 %budget.left, 0
  br i1 %budget.out, label %budget.stop, label %while.body, !prof !5   ; unlikely
```

On arm64 that is a `SUB` and a `TBZ` on its way back, beside the loop's own test. A loop with a count gets nothing in
it: ScalarEvolution's count of its branches back (`getBackedgeTakenCount`), expanded in its preheader, is charged
there, and a vectorised loop's is a shift (`plan` and `countOf`):

```llvm
vector.ph:
  %n.vec = and i32 %conv, 2147483644
  %0 = add nsw i32 %n.vec, -4
  %1 = zext i32 %0 to i64
  %2 = lshr i64 %1, 2
  %budget.left = sub i64 %budget.start, %2
  %budget.out = icmp slt i64 %budget.left, 0
  br i1 %budget.out, label %budget.stop, label %vector.body, !prof !5
```

A loop gets a count when ScalarEvolution knows an exact one for all its exits (a `break` on the data has none), loop
invariant, of at most 64 bits and 12 terms, with no recurrence of an outer loop in it (a triangle's inner loop, `for (j
= 0; j < i; ++j)`, whose count changes every outer iteration, would make the expander add an induction variable to the
outer loop), and safe to compute where the loop starts (no division by what may be zero). Any other counts a branch
back at a time, and so does every loop at `-O0`, whose counters live in memory where ScalarEvolution cannot follow
them. A loop the pass charged runs all of its iterations or none: the charge fails before the first.

Why charge it rather than count. The first version counted every loop at its branches back, and paid for it where it
hurts, in the inner loops: a synth's loop over its eight voices, a sample at a time (`examples/voices/`), took 9% longer
at `-O2`, since its body is two dozen instructions and the check added two, an iteration. And a vectorised loop of
`tanhf` (`examples/saturator/`), one block with its own branch back, became two blocks with a branch each; the code
generator scheduled and allocated the two differently (spilling less, interleaving less), and the loop took 9% longer,
with almost no more work in it. Folding the check into the loop's own branch (`br (%done | %out)`, kept one branch by
`!unpredictable`, without which SelectionDAG splits an `or` of conditions into two branches) got the `tanhf` loop to 4%
and made the voices' 14%. Charged before it starts, a counted loop is the code it was.

The price is one assumption. ScalarEvolution's count may rest on what C leaves undefined: `for (int i = 0; i != n; i +=
2)` with an odd `n` never ends, through an `int` that overflows, and C lets the optimiser assume it does not (the loop
is marked `llvm.loop.mustprogress`, recipe 19, and the `add` is `nsw`). Such a loop is charged the count the optimiser
assumed, and runs as the machine code runs it. Most do end: the vectoriser's trip count and IndVarSimplify's rewritten
exit test follow the same count.

### Stopping, and returning

When the count would go below zero the function writes -1 for what is left, sets the flag and returns at once, a zero
of its type, null or nothing:

```llvm
budget.stop:
  store atomic i64 -1, ptr @fminus_rt_loop_budget monotonic, align 8
  store atomic i32 1, ptr getelementptr inbounds ({ i64, i32 }, ptr @fminus_rt_loop_budget, i32 0, i32 1) monotonic, align 4
  ret float 0.000000e+00
```

and every function of the program that called it returns too, at once, with nothing written: after each call of a
function that may stop, the flag decides.

```llvm
  store atomic i64 %budget.left, ptr @fminus_rt_loop_budget monotonic, align 8
  %5 = call i32 @calls(i32 %4, float %x)
  %budget.stopped5 = load atomic i32, ptr getelementptr inbounds (...) monotonic, align 4
  %6 = icmp ne i32 %budget.stopped5, 0
  br i1 %6, label %budget.unwind, label %if.end.budget, !prof !6
...
budget.unwind:
  ret i32 0
```

So no caller goes on with a value that was never computed. But what the program leaves in memory, its modules' state and
what it wrote of the output, is whichever of the optimised code's stores had run, and that need not be any point of the
source. The oscillator above, at `-O2`, keeps `phase` in a register through the block's loop (LICM promotes it,
`%phase.promoted`) and stores it after the loop: stopped in the wrap of a sample, it has written the samples before and
left `phase` as it was before the call. The optimiser sinks and merges stores too, so some of a stopped call's writes
may be missing, out of order with the source, or out of step with each other. A ring buffer that writes `buf[w]`,
advances `w`, loops, then wraps `w` at `size` may be left with `w == size`, and the next call would write past the
buffer (unless the program is compiled with bounds checks, recipe 30, which stop the call there). So a host that runs
the program again first re-initialises the instance as it made it (zeroes it, then `setDefaults` and its own
parameters), or throws it away; `LoopBudgetTest.cpp` shows the oscillator's case.

A stop writes -1, not its count, which a charge may have taken far below zero. `left` is then never below -1: the
host sets it to -1 or more (`setLoopBudget` makes a negative budget -1), a stop writes -1, and every other write is of
a count that passed its check or of what was read. So a function's count starts at -1 or more, a charge takes at most
`INT64_MAX` (`countOf` charges no count that may be larger: a loop over bytes, written by hand, could take 2^64 - 1),
and no subtraction wraps, however many calls run out without the host setting the budget again. The first version
wrote its count: a loop `for (float* p = a; p != e; ++p)` over a range written one short, `e` one below `a`, is
charged (e - a - 4) / 4 unsigned, 2^62 - 2; two calls took `left` near `INT64_MIN`, and the third's charge wrapped it
positive and let the loop write through memory.

The calls checked are direct calls of the program's functions that may stop, and calls of a runtime function handed
one (a parallel region's body: the region's threads each count, and the caller checks after the region). Inline asm is
no call, and FMinus has no function pointers: its only indirect call is an entry point's call of its body under
`Denormals::Flush` (recipe 20), whose wrapper restores the host's floating-point mode and returns what the body
returned, which is right as it is.

A function that may stop no longer promises what it did, and the pass says so: it reads and writes memory that is no
argument's (the budget: `memory(readwrite, argmem: none, ...)` from `memory(none)`), it returns (`noreturn` goes, as
a loop that never ended was what made it), and its result may be 0 or null (`nonnull`, `dereferenceable`, `range`,
`nofpclass` and a parameter's `returned` go, from it and from its calls). No pass of the optimiser runs after it, but
the code generator reads some of these, and an attribute that is false is undefined behaviour waiting.

### The budget, the program's own

The budget is a global of the program, `fminus_rt_loop_budget`, the `{ int64_t left; int32_t stopped; }` of
`runtime/Runtime.h`'s `fminus_rt_loop_budget_state`, defined unlimited until the host sets it:

```llvm
@fminus_rt_loop_budget = dso_local global { i64, i32 } { i64 9223372036854775807, i32 0 }, align 8
```

Three reasons for a global of the module rather than one of the runtime's or the host's. Each `Program` has its own, so
two programs count apart. It lives in the JIT's own memory, beside the code that reads it, as a constant table does, so
the code reaches it as it reaches its own data, with nothing in the host's address space to resolve. And an object
`fminus --emit obj` writes defines it too, for its host to declare (`extern "C" fminus_rt_loop_budget_state
fminus_rt_loop_budget;`) and set.

`ir::lower` defines it, not the pass: LLJIT takes a module's symbols when it is added, before its IR transform layer
optimises it, and a symbol a pass at the end of the pipeline made would be one the JIT does not know (the pass makes
it only for IR written by hand, for `fminus-build`). `Program::compile` then looks it up, which compiles the module, so
with the budget, compile() generates the machine code and the first look-up of a function only finds it. A program that
declares a function of its name is an error while the budget is on ("the loop budget reserves 'fminus_rt_loop_budget'
for its counter, and this program declares a function of that name: rename it"), and nothing to it without.

Every access the program makes is a relaxed (monotonic) atomic, as are the host's (`std::atomic_ref`), so a parallel
region's threads may share it without a data race. Each part of a region (a chunk of a `parallel for`, which the pool
hands out as threads come free) counts from what `left` held where it started. A function a region's threads may run,
the region's body and every function it reaches, writes its count back with an `atomicrmw min` rather than a store,
so `left` never goes up while the program runs:

```llvm
parallel.end:
  %4 = atomicrmw min ptr @fminus_rt_loop_budget, i64 %budget.slot.0 monotonic, align 8
  ret void
```

With a store, a part that read `left` before another ran out could store its own positive count over that one's -1
when it returned (a quick chunk on a thread the system had paused, say), and every part after it would start with a
fresh budget: a region's spinning chunks could each take a whole budget. With the min, a part that starts after a stop
reads -1 and stops at its first check, and each thread takes at most the budget, so the region takes at most its
thread count times it, which is what a safety net needs. What is left after the region is the least any part left. The
flag is shared, and the caller sees it after the region. Every other function stores: no other thread writes `left`
while it runs, and the min would cost an atomic read-modify-write at each call and return (`LDSMIN` on arm64, a
compare-and-swap loop on x86-64).

### The host's side

- `setLoopBudget(n)` sets `left` to n (a negative n to -1, which stops a call at its first check) and clears the flag;
  the host calls it before each call it limits. What is left carries over otherwise: after a call that ran out, the
  next stops at its first check.
- `loopBudgetExceeded()` reads the flag, `loopBudgetLeft()` what is left (what was set less what the calls since took,
  -1 once one ran out), each a relaxed load: the audio thread may call them.
- After a call that ran out, the host silences the program and re-initialises the instance before it runs it again, or
  throws it away (Stopping, and returning, above).
- A program compiled without the option reads and writes a budget nothing counts down: `loopBudgetExceeded()` is
  false.
- `fminus --run f ... --loop-budget n` sets it for the call and says on stderr what the call took, or that it ran past
  it (status 3, and what it printed is what it returned early). `fminus --emit exe --loop-budget n` makes the C main a
  host: it sets the budget before main and, when main runs out, says so on stderr (`fminus_rt_main_loop_budget`) and
  exits with 3. That main is the host's, so the pass leaves it alone (`kLoopBudgetHostAttribute`), and it calls the
  program's `main` through its address passed through an empty inline asm (`call ptr asm "", "=r,0"(ptr
  @fminus.main)`, as `Denormals::Flush`'s wrapper calls its body, recipe 20). Until the pass puts the program's checks
  in, a `main` whose loop never ends never returns, and touches no budget: called directly, FunctionAttrs marked it
  `noreturn`, and SimplifyCFG deleted everything after its call, the flag's check, the message and the `ret 3`, for an
  `unreachable`; the stopped call returned into it and trapped (status 133 on arm64) from `-O1` up. An indirect call
  carries none of `main`'s attributes, and no inliner inlines it. The C main's own accesses of the budget are volatile
  besides.

### What it changes for programs and hosts

- **Off, the default**, nothing: the module has no flag, no global and no check (`loop-budget.fm`'s `OFF`).
- **On**, a call within its budget computes what it does without it, bit for bit, at every level (`LoopBudgetTest.cpp`
  compares kernels of each kind, and the measurements below every example's); a program cannot name a function
  `fminus_rt_loop_budget`; compile() generates the machine code; the IR shows the checks and the attributes above. A
  call that runs out leaves its instance as the optimised code had it, which the host re-initialises.
- **What it does not catch**: a loop charged a count it will not keep (undefined behaviour, above); a recursion deeper
  than the stack, which overflows it before a larger budget runs out; time spent outside the program, in an external
  function it calls; and a loop whose iterations are each slow, which a count of iterations does not see as time. A
  budget of iterations is no clock, and a host that wants one sets the budget from what its blocks take.

### Tests

- `tests/unit/LoopBudgetTest.cpp`, in the JIT at each level: a loop that never ends returns early with the flag, and
  `left` at -1; unlimited until set, what is left carried to the next call, and a `while`'s exact count at `-O0`; a
  counted (vectorised) loop charged before it starts, all of it or none, its charge exactly what it takes; -1 left
  after a stop, so that a pointer loop charged 2^62 - 2 stops however many calls run out; a loop over bytes (IR by
  hand), whose count may not fit an `int64_t`, counted a branch back at a time; a loop's stores to its output there up
  to the branch back where it stopped; a stopped oscillator's `phase` not stored at all, at no point of the source; a
  recursion's calls counted (`fib(10)`'s 177 at `-O0`), and every caller returning 0 when the deepest runs out; kernels
  of each kind (a vectorised loop of `tanhf` and `sinf`, a recurrence of a `float4`'s coefficients, nested loops,
  `while` loops in a `for`) bit for bit with and without the budget; the reserved name.
- `tests/library/LoopBudgetTest.cpp`, through `Program`: the oscillator above, its infinite frequency stopped, the flag
  and -1 left, the next call stopped too, and run again on an instance made again; a negative budget is -1; bit for
  bit; a program without the budget; a `parallel for` whose iterations spin, on the pool and on one thread; an entry
  point under `Denormals::Flush` that stops and hands the host's mode back; the checks in `optimisedIr()`.
- `tests/lit/fminus/loop-budget.fm`: the IR at `-O0` (the global, a branch back's check, the stop, a recursion's entry,
  a call's flag and the unwind, the attributes, the flag) and at `-O2` (a counted loop's charge), nothing without the
  option; `--run`'s counts and messages, an executable's main and its call through the laundered address, a `while
  (1)` main built at `-O2` that exits with 3, the reserved name; and `Inputs/loop-budget/region.fm`, a region's body
  and what it calls writing back with `atomicrmw min`, a function only the caller runs with a store.

### Measurements

On an M5, at `-O2`, blocks of 256 samples, the host setting the budget before each call, the best of 300 runs of 65536
samples each, the two alternating, with the machine busy with other builds (its noise is about a percent):

| Kernel | Off (ns a sample) | On | Change | Output |
|---|---|---|---|---|
| a gain, vectorised | 0.170 | 0.171 | +0.7% | identical |
| `examples/saturator/` `process_tanhf` (a vectorised loop of the kernel) | 1.463 | 1.464 | 0.0% | identical |
| `process` (a rational tanh) | 1.173 | 1.173 | 0.0% | identical |
| `process_poly` | 1.203 | 1.202 | -0.1% | identical |
| `examples/biquad/` (a recurrence, its cutoff a stream) | 11.227 | 11.243 | +0.1% | identical |
| `examples/voices/` (eight voices a sample) | 16.098 | 16.069 | -0.2% | identical |
| `examples/convolver/` (2048 taps by FFT) | 30.143 | 30.204 | +0.2% | identical |
| `examples/delay/` (an echo) | 1.300 | 1.301 | 0.0% | identical |
| a phase wrap, two `while` loops a sample | 44.365 | 43.865 | -1.1% | identical |
| a 32-tap FIR, nested loops | 13.500 | 13.500 | 0.0% | identical |

At `-O1` and `-O3` the same, within a percent, but for the gain, the cheapest kernel there is, a load, a multiply and a
store a sample: with the budget it ran at 0.17 ns a sample in every measure, without it at 0.17 or at 0.15 by level
and block size, so in two of them the budget was 13% slower (`-O1` in blocks of 256, 0.148 to 0.167) and 14% (`-O2` in
blocks of 1024, 0.145 to 0.166). Its vectorised loop is the same ten instructions either way, 24 bytes further on for
the charge ahead of it: this is where the loop lands, which a loop running at well under a cycle a sample feels. A call
itself costs about a nanosecond more (the budget's load through the GOT, the charges, the store): the biquad and the
echo in blocks of 16 samples, 0.1% and 0.9%. Counting every loop at its branches back, the first version, cost the
voices 9% and the `tanhf` loop 9% (above). At `-O0`, where every loop counts a branch at a time and every value between
blocks is in memory, a loop of a few instructions takes up to twice as long (the gain 2.2 ns a sample to 4.5, the FIR
82 to 128, the convolver 130 to 171); `-O0` is for reading the IR. Compiling takes a few percent longer (the pass's
analyses, and the code it adds): the saturator 58 ms to 63, the convolver 98 to 114, under the same load. None of
these kernels has a parallel region: a function a region's threads run writes its count back with an atomic min
(`LDSMIN` on arm64) at each call and return instead of a store, which these numbers do not measure.

## Recipe 30: bounds checks, for development builds

*In the compiler. Code: `src/ir/Bounds.cpp` (the checks, and the extents values carry; `Lowering::Extent` in
`src/ir/Lowering.h`), with the accesses in `src/ir/Memory.cpp` (`read`, `element`, `lvalue`, `lowerAddressOf`,
`pointerArithmetic`), `src/ir/Vectors.cpp` (a module's field), `src/ir/Modules.cpp` (`lowerMethodCall`, `p->tick()`)
and `src/ir/Calls.cpp` (`poly_load`, `poly_store`);
`src/opt/BoundsCheck.h` and `.cpp` (`BoundsCheckPass`, the record's global, the checks' descriptions and their
messages); `src/opt/EarlyReturn.h` and `.cpp`, what it shares with the loop budget's pass; `ir::Options::boundsCheck`
and its module flag in `src/ir/CodeGen.h` and `.cpp`; the pass in `src/support/HostTarget.cpp`;
`Program::Options::boundsCheck`, `boundsViolation`, `clearBoundsViolation` and `BoundsViolation` in
`include/fminus/FMinus.h` and `src/api/FMinus.cpp`; `fminus --bounds-check` in `tools/fminus/main.cpp`; an
executable's in `addEntryPoint` (`src/support/Aot.cpp`), with `fminus_rt_bounds_state` and `fminus_rt_main_bounds` in
`src/runtime/`. Tests: `tests/unit/BoundsCheckTest.cpp` (`[bounds]`), `tests/library/BoundsCheckTest.cpp` and
`tests/lit/fminus/bounds-check.fm`.*

An index past an array's end, or before its start, reads or writes whatever lies beside the array: another field of
the module, the stack, the host's memory. C calls it undefined, and in a plugin it corrupts the host or crashes it,
often much later and somewhere else. It takes one ordinary bug. This echo's delay is a parameter, and its read position
wraps once, so a delay longer than the line reads before it:

```c
module Echo
{
    param int delay [[fminus::range(1, 512)]] [[fminus::default(100)]];
    float line[512];
    int at;

    void process(const float* restrict in, float* restrict out, int n) [[clang::nonblocking]]
    {
        for (int i = 0; i < n; ++i)
        {
            int back = at - delay[i];
            if (back < 0)
                back += 512;
            const float y = line[back];           // a delay of 700: line[-188]
            line[at] = in[i] + 0.5f * y;
            out[i] = in[i] + y;
            at = at == 511 ? 0 : at + 1;
        }
    }
};
```

Compiled with bounds checks, every access whose array the compiler can see is checked against the array's extent, and
a call whose check fails returns early, with the check recorded for the host:

```cpp
fminus::Program::Options options;
options.boundsCheck = true;                                   // off by default; a development build's
auto program = fminus::Program::compile(source, errors, options);
...
process(instance, in, out, n);
if (auto failed = program->boundsViolation())                 // it returned early: silence it
{
    report(failed->message());                                // off the audio thread: "echo.fm:14:33: index -188 out
                                                              // of bounds for 'line' of type 'float[512]'"
    program->clearBoundsViolation();                          // first, so that init runs whole
    init(instance);
}
```

The checks are for a program while it is written: a host compiles with them while its author tries the program out,
and without them for the build that plays, which is the code it was, byte for byte (Measurements, below).

### What is checked

The code generator checks where it can see the array: an array's name stands for a pointer to its first element
(recipe 12), and that pointer carries the array's extent, its element count, its name as the source writes it and its
type (`Lowering::Extent`, created in `read` where the array decays). What is made from it carries the extent on:

- **an element**, `a[i]`, `*p`, `p->y`, and the module a method is called on through one, `p->tick()`: one of the
  array's own, its index from 0 to the count less one (width 1);
- **a pointer formed**, `a + i`, `p - i`, `&a[i]`: any element or one past the last, as C allows (C11 6.5.6p8), so
  `a + 4` and `&a[4]` of a `float a[4]` are fine, an end to compare against, and `a + 5` and `a - 1` fail (width 0);
- **a `poly_load` or `poly_store`** through one: every lane it reaches, the poly width from the index (width: the
  lanes), or, with a mask, each lane the mask takes, the first outside reported (`checkLanes`).

The arrays are every array the program declares: a local, a module's field (`taps[k]` in a method, `m.taps[k]`,
`p->taps[k]`), an array of modules and an element's field (`voices[v].render()`, `(voices + v)->render()`,
`voices[v].taps[k]`), a constant table (recipe 26, and the top level's, recipe 28), an array a parallel region uses of
the function around it. The index is the element's from the array's first: `(a + 3)[i]` checks `3 + i`.

What carries no extent is not checked: a pointer a variable, a field or a parameter holds (`float* p = buf;`, then
`p[i]`), what a function returns, what `?:` chooses, a `void*`. So the heap's blocks and a host's buffers, which only
reach a program through such pointers, are not checked: their extents are known only at run time, and keeping them
would change the module's layout or a pointer's size. A constant of the top level runs nowhere (its initialiser is
lowered into a function that is deleted, recipe 28), so `const float* const p = &table[6];` is no check either. And
`fft_forward` and `fft_inverse` check none of their buffers, though they may carry extents: the runtime reads and
writes as many elements as the plan has (recipe 13), and the plan's size is known only when the program runs, so a
`float buf[8]` handed to `fft_forward(fft_create(64), buf, bins)` is overrun, with the checks or without.

### Where the checks go: first

The loop budget's checks go in last, after every decision, because they must not change one (recipe 29). These go in
first, where the access is, as clang's `-fsanitize=array-bounds` with `-fsanitize-trap` puts them, because the
optimiser must know of them: a failed check is the end of the call, and nothing after it may run, or be moved before
it. The check is an unsigned comparison of the element's index, widened to an `i64` (a negative one is a huge unsigned
number), against the extent, and a branch, likely in bounds, past a call of `fminus.bounds.stop` that never returns:

```llvm
  %idxprom = sext i32 %1 to i64
  %bounds.in = icmp ult i64 %idxprom, 512
  br i1 %bounds.in, label %bounds.ok, label %bounds.fail, !prof !13   ; likely

bounds.ok:
  %arrayidx = getelementptr inbounds float, ptr %arraydecay, i64 %idxprom
  ...
bounds.fail:
  call void @fminus.bounds.stop(i32 1, i64 %idxprom)
  unreachable
```

`fminus.bounds.stop` is `noreturn nounwind cold`, and writes only memory the program cannot reach (`memory
(inaccessiblemem: write)`): to the optimiser a trap that loads of the program's own memory may move across, so a loop's
loads of a module's fields still leave the loop. Its arguments are the check's number, from 1, and the index. The
optimiser then does what it does with any trap: it removes a check it can prove (in a `for` over the array whose own
condition is the extent, `i <u 16` folds away from `-O1`), keeps the others, and moves no store past one, so the access
a failed check guards never happens, and nor does anything after it. A check of an index that is a constant within the
array is no check at all (`a[3]` costs nothing); one that is a constant outside it fails whenever it runs, and is a
branch too (on `false`), so that what follows stays reachable to the code generator's own rules: a non-void function
that can reach its end is the same error with the checks as without.

A trap can bound a loop: in `while (s < n) { s += 1.0f + a[i & 7]; i = i + 1; }` over a `float a[4]`, `i` cannot reach
4, so the optimiser unrolls the loop into four iterations, with no loop left for the loop budget to count. And a loop
whose check the optimiser turns into a trip count, `out[i] = table[i + offset]` with `offset` a field, still vectorises:
its vector loop runs to the earlier of `n` and `1024 - offset`, and the remainder checks. What a check does cost is the
loop's freedom with a field it updates: `at` in the echo above is stored each iteration, where without the check LICM
keeps `at` in a register and stores it after the loop. Sinking the store to the loop's exits would add a store on the
failed check's path, where the iteration that failed had stored nothing, which LLVM may not do to memory another thread
could see. A loop like the echo's is then a third slower (the comb in Measurements).

Each check's description goes into the module's named metadata, `!fminus.bounds.sites`, in the order of the numbers:
its file (an `#include`'d file's name, as errors give it), line and column (a subscript's `[`, a pointer's `+` or `-`,
a dereference's `*`, a `poly_load`'s name), the array as written, its type, its extent and the access's width:

```llvm
!fminus.bounds.sites = !{!3, !4}
!3 = !{!"echo.fm", i32 14, i32 33, !"line", !"float[512]", i64 512, i32 1}
!4 = !{!"echo.fm", i32 15, i32 17, !"line", !"float[512]", i64 512, i32 1}
```

The array as written is a variable's name, a field's (`m.taps`, `p->taps`, `this->taps`, or `taps` in a method), an
element's index when it is a name or a number (`voices[v].taps`; `voices[...]` otherwise), and empty when there is
none. A copy of a function, knob versioning's (recipe 16) or the inliner's, keeps its checks' numbers, so a check means
the same wherever it runs.

### Stopping, and returning

`BoundsCheckPass` is a module pass, registered at `OptimizerLast` after the math kernels and before the loop budget
(`HostTarget.cpp`), and runs at every level (`isRequired`), for a module whose flag `fminus.bounds-check` is 1. It
makes each stop a branch to its function's one stop block, which records the check, unless the record holds one
already, and returns, a zero of its type, null or nothing (the echo's `process` at `-O2`, its two checks' stops):

```llvm
bounds.stop:
  %bounds.site = phi i32 [ 1, %bounds.fail ], [ 2, %bounds.fail23 ]
  %bounds.index = phi i64 [ %idxprom, %bounds.fail ], [ %idxprom20, %bounds.fail23 ]
  %bounds.first = cmpxchg ptr @fminus_rt_bounds, i32 0, i32 %bounds.site monotonic monotonic, align 8
  %bounds.won = extractvalue { i32, i1 } %bounds.first, 1
  br i1 %bounds.won, label %bounds.record, label %bounds.return
bounds.record:
  store atomic i64 %bounds.index, ptr getelementptr inbounds ({ i32, i32, i64 }, ptr @fminus_rt_bounds, i32 0, i32 2) monotonic, align 8
  br label %bounds.return
bounds.return:
  ret void
```

and every function of the program that called it returns too: after each call of a function that may stop (one that
has a check, or calls one that may, or hands one to a runtime function, a parallel region's body), it reads the record,
and returns at once if it holds a check:

```llvm
  %0 = call float @pick(float %i1)
  %bounds.recorded = load atomic i32, ptr @fminus_rt_bounds monotonic, align 8
  %1 = icmp ne i32 %bounds.recorded, 0
  br i1 %1, label %bounds.unwind, label %entry.bounds, !prof !12   ; unlikely
bounds.unwind:
  ret float 0.000000e+00
```

After inlining few calls are left (at `-O2` a module's `process` is usually one function), and each costs a load and a
branch. A callee that always failed a check was `noreturn` to the optimiser, which left `unreachable` after its calls;
the record is always set when such a callee returns, so the read after the call always unwinds into the return. The
functions it changes, and the calls of them, get their attributes adjusted to what is now true of them (they return,
their result may be 0 or null, they write memory that is no argument's: `mayReturnEarly`, `src/opt/EarlyReturn.h`,
which the loop budget's pass uses too, with `reachedFunctions`, which calls run which functions), and carry
`"fminus-bounds-check"`, so that a second run leaves them alone. The pass runs before the loop budget's, so that the
budget's count is written back on these returns too (`BoundsCheckTest.cpp` has both in one program).

The first failure is kept: the site is a compare-and-swap from 0, and the index is written only by the call that won
it, so a parallel region's threads may fail at once and the record still holds one check and its own index. While the
record holds a check, every read after a call unwinds, however the call got there: a function returns as soon as a
call it makes of a function that may stop returns, though its own indexes are fine. The callee runs its own body,
unless its own calls stop it, and a function whose code is all inlined, with no such call left, runs to its end, as
does a parallel region's part with none (the function that ran the region returns when the region ends). So the host
clears the record before the program runs again, and before anything else it calls, re-initialising the instance
included: an `init` called while the record still holds the check returns after its first such call, the instance half
set up, and nothing says so, since the first failure is kept (`tests/library/BoundsCheckTest.cpp` runs both orders).
What the call leaves in the instance and the output is whichever of the optimised code's stores had run, as with the
loop budget: everything the source did before the failed access may not have been stored yet (a field kept in a
register), and nothing after it was. A host clears the record, then re-initialises the instance, or throws it away.

### The record, the program's own

The record is a global of the program, `fminus_rt_bounds`, the `{ int32_t site; int32_t reserved; int64_t index; }` of
`runtime/Runtime.h`'s `fminus_rt_bounds_state`, defined empty:

```llvm
@fminus_rt_bounds = dso_local global { i32, i32, i64 } zeroinitializer, align 8
```

for the loop budget's reasons (recipe 29): each `Program` has its own, it lives beside the code that writes it, and an
object `fminus --emit obj` writes defines it for its host. `ir::lower` defines it, with the module flag, before any
function, since LLJIT learns a module's symbols when it is added; `Program::compile` looks it up, which compiles the
module, and reads the checks' descriptions from the module's metadata before the JIT takes it. A program that declares
a function of its name is an error while the checks are on ("the bounds checks reserve 'fminus_rt_bounds' for their
record, and this program declares a function of that name: rename it"), and nothing without. The site 0 is no check,
so a record of zeros is empty.

### The host's side

- `boundsViolation()` is the first check that failed since `clearBoundsViolation()` (or since the program compiled), or
  nothing: a `BoundsViolation` with the file, line and column, the array, its type and extent, the index and the
  access's width, its strings the `Program`'s own (`std::string_view`s, valid while it lives). It allocates nothing, and
  its loads are relaxed atomics: the audio thread may call it. `message()` words it, as UBSan words its own with the
  array's name and where it is: `index 16 out of bounds for 'line' of type 'float[16]'`, `pointer offset 17 out of
  bounds ...`, `4 elements from index 14 out of bounds ...`; it allocates, so a host calls it off the audio thread.
- `clearBoundsViolation()` empties the record, a relaxed store: after the host has silenced the program and read the
  violation, and before its next call of the program, re-initialising the instance included (a method such as `init`
  is a call too, and the record would stop it partway): clear, then re-initialise the instance (or throw it away).
- A program compiled without the checks never has a violation.
- `fminus --run f ... --bounds-check` says where on stderr when a check fails (`fminus: --run: 'f' stopped: ...`), with
  status 4, and what it printed is what the call returned early. `fminus --emit exe --bounds-check` gives the C main
  the checks' messages as two tables of strings, the text before the index and the text after it, by number; after
  `main` it reads the record, and when it holds a check prints the message (`fminus_rt_main_bounds`) and exits with 4.
  It calls `main` through its laundered address, as with the loop budget (recipe 29): a `main` that always fails a
  check is `noreturn` until the pass, which would delete what follows a direct call.

### What it changes for programs and hosts

- **Off, the default**, nothing: no extent is made, no check, no record, no metadata and no flag, and the IR is the
  same, byte for byte, as before the option existed (every example and every lit test's program, at `-O0`, `-O2` and
  `-O3`; `BoundsCheckTest.cpp` checks that the module has none of them).
- **On**, a call within its arrays computes what it does without the checks, bit for bit (`BoundsCheckTest.cpp` and
  `tests/library/BoundsCheckTest.cpp` compare a comb filter and the echo above); a program cannot name a function
  `fminus_rt_bounds`; compile() generates the machine code. A call whose check fails returns early, its instance as the
  optimised code had it, which the host re-initialises once it has cleared the record.
- **What it does not catch**: an access through a pointer the program keeps (above), a heap block's or a host
  buffer's; `fft_forward`'s and `fft_inverse`'s buffers shorter than their plan (above); a stream parameter's
  `cutoff[i]` past the samples the host gave; and an array of arrays, which FMinus does not have.

### Tests

- `tests/unit/BoundsCheckTest.cpp`, in the JIT at each level: an index outside its array stops the call and records the
  check and the index (8, -1, 10^9); each check described where it is, the array as written (`taps`, `voices`,
  `voices[v].taps`, `p->taps`, a `float4[2]`'s `w`, a pointer's `+`, a dereference's `*`); a pointer formed one past the
  end and no further, and the element it reaches; a stop returning through every caller (`-O0`, where the calls stay)
  with no write after the calls; the first failure kept, and while it is, a call returning once its first call of a
  function with checks returns; a module's ring buffer, an array of modules, what lies past a field's array untouched;
  a method called through a pointer into an array of modules (`(voices + n)->bump()`, `(&voices[n])->bump()`), checked
  as an element, so that one past the end is formed but not called on; a `poly_load`'s and a `poly_store`'s lanes, a
  masked one's only those taken; a parallel region's part failing, the code after the region not run; a loop the
  optimiser proves within its array with no check; a comb filter bit for bit with and without; the loop budget and the
  checks together, the budget's count written back on a check's return; nothing of the checks without the option; a
  constant index outside its array, and the same errors with and without; the reserved name.
- `tests/library/BoundsCheckTest.cpp`, through `Program`: the echo above, its long delay stopped and reported (file,
  line, column, array, type, extent, index, width, message), the first failure kept, cleared and run again; an `init`
  run at `-O0` before the record is cleared stopping after its first call, the instance half set up, and run whole
  after; an `#include`'d file's check named by that file; bit for bit, the instance too; a program without the
  checks, and the checks in `optimisedIr()`; an entry point under `Denormals::Flush` that stops and hands the host's
  mode back.
- `tests/lit/fminus/bounds-check.fm`: the IR at `-O0` (the record, a check, the stop block, a call's read of the record
  and the unwind, the attribute, the descriptions) and at `-O2` (a loop with no check left), nothing without the option;
  `--run`'s messages for an element, an index below 0, a call, a pointer formed; an executable's main, its C main's
  read of the record, a `main` that always fails built at `-O2`; the reserved name.

### Measurements

On the same M5, blocks of 256 samples, the best of 40 runs of 65536 samples each, the two alternating, with the machine
busy with other builds (ns a sample):

| Kernel | Off | On | Change | Output |
|---|---|---|---|---|
| a gain over the host's buffers (no array: no check) | 0.061 | 0.061 | 0% | identical |
| a comb on a ring buffer in a field, `line[at]`, `at` a field | 0.70 | 0.93 | +32% | identical |
| a wavetable, `table[(int)phase]` | 3.16 | 3.16 | 0% | identical |
| `out[i] = in[i] + 0.5f * table[i + offset]`, vectorised either way | 0.059 | 0.060 | +2% | identical |
| a 32-tap FIR, its history `history[at]` and `history[(at - k) & 31]` | 8.25 | 11.6 | +40% (at `-O3`: -7%) | identical |
| eight voices of an array of modules, each a wavetable | 12.08 | 11.73 | -3% | identical |

The same at `-O1` and `-O3`, within a few percent, but for the FIR, which `-O3` unrolls whole either way and runs at 4.7
and 4.4. The comb and the FIR pay for the field each updates every sample, a store each iteration instead of one after
the loop (Where the checks go, above); the wavetable's and the voices' checks hide in their loops' latency, and the gain
has none. Compiling takes as long, within the noise: `examples/voices/`, whose program has seven checks, 11.4 ms without
them and 11.7 with. These are development builds; without the option nothing is checked, and nothing costs anything.

## Recipe 31: conditional compilation, `#if` and `#ifdef`

*In the compiler. Code: `src/frontend/Lexer.l` (the start condition `SKIPPED` and its rules, the directive rule in it
too, the `#` rules, the end of a comment and of a file, the macro check in `{IDENTIFIER}`, and `directive`,
`conditional`, `define`, `macroName`, `isKeyword` and `macroNameError` in its last section),
`src/frontend/Preprocessor.h` and `.cpp` (`Macros`, `withoutComments`, `evaluate`), `src/frontend/Scanner.h`
(`Conditional`, and `ScannerState`'s `macros`, `conditionals`, `expanded` and `lineStart`) and `Frontend.h` (`Defines`,
`parse`'s `defines`); `Program::Options::defines` (`include/fminus/FMinus.h`, `src/api/FMinus.cpp`); `fminus -D`
(`tools/fminus/main.cpp`). Tests: `tests/unit/ConditionalTest.cpp` (`[conditional]`),
`tests/library/ConditionalTest.cpp` and `tests/lit/fminus/conditional.fm`.*

A program being written carries instrumentation: a meter's peak, a counter of the voices stolen, a field a host shows
while its author tunes a filter. It wants to be there while the program is written and gone from the build that plays,
without being commented in and out by hand. C's way is the preprocessor's conditionals, and FMinus now has them:

```c
module Meter
{
    float y;
#ifdef DEBUG
    float peak;   // the loudest |x| so far: a debug build's
#endif

    float tick(float x)
    {
        y = 0.5f * x;
#if defined(DEBUG) && LEVEL >= 2
        peak = fmaxf(peak, fabsf(x));
#endif
        return y;
    }
};
```

`DEBUG` and `LEVEL` are macros: the program's own (`#define LEVEL 2`) or the host's, `Program::Options::defines`, as
`fminus -D DEBUG` and C's `-D` define them. Without `DEBUG`, the lines between are skipped: the scanner never reads
them, the parser never sees them, and the module is the one without them, field and all.

### Why C's conditionals, and not a `debug { }` block

Two designs serve "instrumentation that compiles out". One is a statement of FMinus's own, `debug { ... }`, compiled
when the host sets a flag and dropped otherwise: a keyword, a grammar rule and an option, smaller to build than
anything here. But it serves statements only. The field that holds the peak, a method only a debug build has, the
`#include` of a module that draws a scope, a host's own `#pragma` lines: none of these is a statement, and each place
would want a rule of its own (the top level, a module's members). It reserves `debug`, a word programs use as a name.
And it is not C: a program with one is no longer a C program, and a C compiler reads it only through a macro,
`#define debug if (0)`, which is a statement again.

The conditionals act on lines, before the parser sees a token, so one mechanism covers every place a line can be.
They are C's: FMinus gains no syntax, a C compiler reads a program's conditionals as they are, and an editor's
language service that lexes the text itself, as a plugin's code editor may, already reads a directive as one line
that starts with `#`. Their cost is the scanner's: a start condition, a stack of conditionals and an evaluator for
`#if`'s expressions. FMinus keeps that small by keeping one thing out, macro substitution: a macro is a condition, read
in `#if`, `#elif`, `#ifdef`, `#ifndef` and `defined`, and its name in code is an error. So no text is ever rewritten
before the parser reads it, a reader sees the code that runs, and a program FMinus compiles means the same in C.

### What a program writes

| Line | What it does | Its errors (clang's words) |
|---|---|---|
| `#define NAME` or `#define NAME value` | defines a macro: its value the rest of the line, comments gone and each run of spaces one space (C's comparison), empty for none | `macro name missing`, `macro name must be an identifier`, `'defined' cannot be used as a macro name`, `'NAME' macro redefined` (another value: clang warns); FMinus's own: `'float' cannot be used as a macro name: it is one of FMinus's keywords`, `function-like macros are not supported: ...` |
| `#undef NAME` | forgets it (one that isn't is nothing) | `macro name missing`, `macro name must be an identifier`, `'defined' cannot be used as a macro name`, `extra tokens at end of #undef directive` (clang warns) |
| `#if expression` | reads the group up to the next conditional when the expression is not 0 | the expression's (below) |
| `#ifdef NAME`, `#ifndef NAME` | `#if defined(NAME)`, and `#if !defined(NAME)` | `macro name missing`, `macro name must be an identifier`, `extra tokens at end of #ifdef directive` |
| `#elif expression`, and C23's `#elifdef NAME` and `#elifndef NAME` | the next group, when no group before it was read and its condition holds; after a group read, its condition is never computed | `#elif without #if`, `#elif after #else` |
| `#else` | the last group, when no other was read | `#else without #if`, `#else after #else`, `extra tokens at end of #else directive` (clang warns) |
| `#endif` | the end of the conditional | `#endif without #if`, `extra tokens at end of #endif directive` (clang warns), and at the file's end, `unterminated conditional directive` at the `#if` left open |
| a macro's name in code | an error, never a substitution | `macro expansion is not supported: 'NAME' is a macro, which FMinus reads only in #if, #elif, #ifdef and #ifndef (for a constant, declare an enum or a const)` |

The keywords are refused as macros because C would substitute one in code, where FMinus reads it as the keyword:
`#define float double` would mean one thing to clang and another to FMinus. `macroNameError` asks the scanner itself
(`isKeyword` reads the name with a scanner of its own and looks at the token), so the keywords it refuses are the
rules before `{IDENTIFIER}`, a keyword added later too. `#error`, `#warning`, `#line` and `#embed` are still errors
(`#error is not supported: FMinus's directives are ...`).

A directive is one line, as before, which its `#` begins, after spaces. Two rules keep FMinus from reading a line
otherwise than C, in a skipped group too, where a difference would be silent (the other group compiled):

- **A `/*` comment that starts in a directive's line must end on it** (`a directive's /* comment must end on its
  line`), where C would carry the directive on to the comment's end and read the lines between as the comment.
  `directive()` checks every directive's line first, whatever its name and in any group: a `#pragma`, a skipped
  group's `#include`, an inert `#if`, an `#elif` never computed. A quote's text is no comment, as in C (a host's
  `#pragma panel label x "a /* b"` is fine), and a quote left open runs to the line's end, as clang reads one. A
  comment between the `#` and the name is spaces, as in C: `#/* note */else` is an `#else`.
- **No comment comes before a directive's `#`** (`a comment before a directive's '#' is not supported: a directive
  begins its line, after spaces only`). C reads `/* note */ #else` as a directive; the directive rule's `^` cannot
  match after the comment, so FMinus refuses the `#` there, in code and in a skipped group, instead of reading it as
  text. `ScannerState::lineStart` says whether only spaces and comments come before the scanner on its line: a line's
  end sets it, text clears it (in `SKIPPED`, its text rule; in `INITIAL`, every call of the scanner after the first,
  which follows a token), and a block comment keeps it as it was where the comment began, as C reads a `#` after one:
  `x /* a` / `b */ #else` is text in a skipped group, as in C.

### Expressions

An `#if`'s expression is C's integer constant expression of the preprocessor (C17 6.10.1), computed in 64-bit
integers, C's `intmax_t`. `evaluate` (`Preprocessor.cpp`) lexes the line into C's preprocessing tokens, expands its
macros, and parses what results by precedence climbing over C's ten levels of binary operators, with `?:` and the
unary operators by recursive descent, computing as it goes:

- **Operands.** Integers as FMinus writes them, decimal, octal and hex, with no suffix (FMinus has no `unsigned` or
  `long`): `1u` is `invalid suffix 'u' on integer constant`, `1.5` `floating point literal in preprocessor
  expression`, and past 2^63-1 `integer literal is too large to be represented in a signed integer type`. `defined
  NAME` and `defined(NAME)`, 1 when NAME is a macro. `true` and `false`, 1 and 0, as in C23. Any other name, a
  keyword too, is 0, as in C.
- **Macros** expand to their values' tokens, in turn, C's way: as text (`#define TWO 1 + 1` makes `TWO * 2` 3), and
  never inside their own value (`#define A A` makes `A` the name `A`, so 0). `defined`'s operand is never expanded. A
  token from a macro's value is at fault at the macro's name: `#define X 2.5` then `#if X` points at the `X`.
- **Operators**: `! ~ - +`, `* / %`, `+ -`, `<< >>`, the comparisons, `& ^ |`, `&& ||` and `?:`, with C's precedence.
  `&&`, `||` and `?:` compute only what C computes: `#if 0 && 1 / 0` is no error, `#if 1 / 0` is `division by zero in
  preprocessor expression`.
- **Arithmetic is defined for every operand**, as FMinus's code is: `+ - *` and unary `-` wrap, the least integer
  divided by -1 is itself (its remainder 0), and a shift takes its count's low six bits (OpenCL C's rule for a 64-bit
  integer, as FMinus shifts an `int` by its count's low five), the right one arithmetic. clang warns of an overflow
  there; FMinus does not (its warnings are `-Wswitch`, recipe 32, and `-Wunknown-attributes`, recipe 35), and the
  result is defined.
- **Limits**, so that a program cannot hang the compiler or exhaust its stack: 256 levels of parentheses, unary
  operators and `?:` (clang's bracket depth), 256 macros expanding inside one another, 65536 tokens after expansion
  (`#define B A A` doubles a line's tokens with each such macro), and the work of expanding, counted across all of a
  compilation's conditions: each macro expanded counts one and the bytes of its value, at most 1048576 in all
  (`macro expansion is too long: the program's conditions expand more than 1048576 bytes of macros`). The tokens
  alone would not do: a macro of nothing (`#define C0`, a flag's form) adds none, and `C1` a thousand `C0`s, `C2` a
  thousand `C1`s and `C3` a thousand `C2`s make `#if C3 + 1` a billion expansions; and a line under any bound of its
  own can be repeated. A real program's conditions expand a few bytes each, and the bound costs at most some tens of
  milliseconds to reach.

The errors are clang's, at clang's columns: `expected value in expression`, `expected ')' in preprocessor expression`,
`token is not a valid binary operator in a preprocessor subexpression`, `expected end of line in preprocessor
expression`, `invalid token at start of a preprocessor expression`, `expected ':'`, `macro name missing`, `missing ')'
after 'defined'`. clang adds a note, `to match this '('`; FMinus's directive errors have none.

### Skipped groups: a start condition

The scanner reads a skipped group in a start condition of its own, `SKIPPED`, exclusive, which has the directive rule
(`<INITIAL,SKIPPED>^[ \t]*"#"[^\n]*`) and rules that only count: spaces, text up to a `/`, a `#` or the line's end, a
`/`, a `#` inside a line (an error after only comments, above), a `//` comment, line ends. The group's text is never
matched against FMinus's tokens, so it need not be FMinus at all: `#if 0` around a paragraph of prose is fine, as in C.
Two things are still read, as C's translation phases read them before the directives:

- **Block comments.** `<INITIAL,SKIPPED>"/*"` enters `BLOCK_COMMENT` from either, and its `*/` goes back to `SKIPPED`
  or `INITIAL`, whichever `ScannerState::skipping()` says. So a `/*` in a skipped group hides an `#endif` inside the
  comment, as in C, and one left open is `unterminated /* comment`.
- **The conditionals' lines.** In a skipped group, `directive()` reads only a directive's name, once it has checked
  that the line's comments end on it: `#if`, `#ifdef` and `#ifndef` open a conditional, and `#elif`, `#else` and
  `#endif` close a group; any other line, an unknown directive, an `#include` or a `#define`, is text, as C's skipped
  groups have it. After each directive the rule's action sets the start condition from the stack:
  `BEGIN(yyextra->skipping() ? SKIPPED : INITIAL)`.

A `Conditional` (`Scanner.h`) is one `#if` on the stack, to its `#endif`: where its name is, for `unterminated
conditional directive`; its file; whether a group has been read (`taken`: the groups after it are skipped, their
conditions never computed); whether its `#else` has been; and whether the group being read is skipped. One that opens
in a skipped group is `inert`, taken and skipped from the start: its `#elif`s are never computed and its lines' extra
tokens never read, though `#else after #else` is still clang's error there. Because the scanner keeps counting lines in
`SKIPPED` (each rule steps the location, as the whitespace rules do), every location after a skipped group is right.

### Conditionals and files

C's conditionals end in the file they begin in. `Conditional::file` is `ScannerState::open`'s size where it opened:
an `#else`, `#elif` or `#endif` whose innermost conditional is another file's is `#endif without #if`, and the
`<<EOF>>` rules, the program's end and an included file's, refuse a conditional the ending file opened (`SKIPPED`'s
own `<<EOF>>` always does: the scanner can only be skipping in the file being read, since a skipped `#include` reads
nothing). Macros are the compilation's, as C's: one an included file defines is defined after its `#include`, in the
including file too.

### The host's side

`Program::Options::defines` is a map from a macro's name to its value, defined before the program's first line, as
`fminus -D NAME[=value]` defines it (`-D DEBUG` is `{"DEBUG", "1"}`, `-D EMPTY=` is `""`). `compile()` refuses a name
that `macroNameError` refuses (`fminus::Program: defines: '1X': macro name must be an identifier`), and `fminus` too
(`fminus: -D 1X: ...`). A program that has a default for one writes C's idiom, which the host's value overrides:

```c
#ifndef LEVEL
#define LEVEL 2
#endif
```

A program's `#define` of a host's macro to another value is `'LEVEL' macro redefined`, as two of the program's own
would be. A host's macro whose name the program uses in code is an error at that name, so a host that defines `DEBUG`
for every program refuses one that has a variable named `DEBUG`: name a host's macros as C names its own (upper case,
a prefix of the host's).

A host that reads a program's text itself sees what FMinus sees, line by line: every directive is a line that starts
with `#`. The lines of a group FMinus skips are there too, so a host that reads its own `#pragma` lines (a plugin's
panel layout, say) or indexes declarations (an editor's language service) either reads both branches, which an editor
does well enough, or skips the groups that `#ifdef`, `#ifndef` and `defined` on its own defines skip.

### What it changes for programs and hosts

- **Off, nothing.** A program without these directives scans as before, and the code of one whose groups are skipped
  is the code of the program without their lines: a skipped group costs no compile time beyond counting its lines, and
  nothing at run time. The suite's other tests are unchanged, but for the messages of the directives FMinus still
  lacks, which now list these.
- **Programs** that used `#define`, `#if` and the rest were refused before; now they compile, unless they substitute a
  macro in code, which stays an error with a message that says why. One that FMinus took before is refused now: a
  `#pragma` whose `/*` comment runs on past its line, which C would read as one comment with the lines after it.
- **The playing build and the debug build differ in their modules' layouts** when a field is conditional: a host that
  allocates instances reads `Program::module()` for each build, as it always should.

### Tests

- `tests/unit/ConditionalTest.cpp`: each conditional choosing its group (`#ifdef` and `#ifndef`, an `#elif` chain, C23's
  `#elifdef` and `#elifndef`, nested conditionals, a skipped group's own `#else`); a module's field and a method's
  statement compiled in and out; a skipped group never scanned (prose, an unknown directive, an `#include` the resolver
  is never asked for, a `#define` never made), a block comment hiding an `#endif`, a `#` inside a line, lines counted
  through a skipped group; expressions (precedence, every operator, octal and hex, wrapping, shifts past 63, the least
  integer divided by -1, `true` and `false`, names that are 0, short-circuiting and `?:`, macros expanded as text, in
  turn and not inside themselves, `defined`'s operand unexpanded); the host's defines (a redefinition, the `#ifndef`
  idiom, a bad name) and `macroNameError` over every keyword; a macro's name in code, the host's too, and a skipped
  group's names unread; conditionals ending in their own file, macros crossing files, a skipped `#include`; every
  error above at clang's column, and the limits, nested empty macros and a line repeated past the compilation's
  bound among them; a directive's line: a `#` after a comment that begins its line refused, in code, in a skipped
  group and in an included file, and after text read as C reads it; a `/*` left open in a `#pragma`, a skipped
  group's directive, an inert conditional, an `#elif` never computed, an `#include` and a null directive; quotes in
  a `#pragma`; and names near a keyword that are a macro's.
- `tests/library/ConditionalTest.cpp`: `Options::defines` from C++17, a meter whose field and statement compile in with
  `DEBUG` (its module's fields, its result) and out without it, the program's `LEVEL` default overridden by the host's;
  a bad name, a keyword, and a host's macro used in code.
- `tests/lit/fminus/conditional.fm`: `fminus -D`, the meter's results with and without `DEBUG` and with `LEVEL=1`, its
  syntax tree with and without the field, a skipped group of prose, an error inside a group read only with `-D
  BROKEN`, a `-D` whose name the code uses, and a bad name.

## Recipe 32: one error a mistake, enumerated types, and the first warning

*In the compiler. Code: `Lowering::Value::failed` (`src/ir/Lowering.h`), set in `lower` and by `recovery`, and read by
`binary`, `recovered`, the unary operators and `lowerConditional` (`Expressions.cpp`), `lvalue`, `readLValue` and
`lowerAddressOf` (`Memory.cpp`), `lowerMember` and `lowerSelect` (`Vectors.cpp`), `lowerMethodCall` and
`lowerParameter` (`Modules.cpp`) and `convert`, `poison` and `recovery` (`Types.cpp`); `branchOn`, `folds` and
`isConstant` (`Statements.cpp`); `sizeOf` (`Types.cpp`) and `lowerSizeof` (`Expressions.cpp`); `declareEnum`
(`Constants.cpp`); the enumerations' rules in `Parser.y` (`enumeration`, `enum_head`, `enum_base`, `enum_declarators`,
and `underlyingType`, `sameTag`, `declaredEnum`, `forwardEnum`, `enumType`, `declarationsOf`, `addFields` and
`addConstants`), `Tag` in `ParseContext.h`, `Type::enumeration`, `Expr::opening` and `Enum`'s `id`, `fixed` and
`underlying` in `Ast.h`; `lowerSwitch`, `unhandled` and `begin` (`Statements.cpp`);
`Diagnostics::warning` (`src/frontend/Diagnostics.h`), `Program::warnings` (`include/fminus/FMinus.h`), and the
warnings `fminus` prints (`tools/fminus/main.cpp`). Tests: `tests/unit/RecoveryTest.cpp` (`[recovery]`),
`EnumTest.cpp`, `SwitchTest.cpp`, `FrontendTest.cpp`, `tests/library/ProgramTest.cpp`, and
`tests/lit/fminus/recovery-errors.fm`, `enum-switch.fm` and `large-modules.fm`.*

Whoever reads FMinus's errors (a person at an editor, a plugin's status line, a model fixing its own program) reads the
first one and acts on it, and an error that only follows from it sends them looking for a second mistake that isn't
there. This recipe makes each mistake one error, as clang's recovery does; it takes back three errors that C would not
give and one wrapped number; it gives enumerations what C gives them and recipe 27 left out; and it adds FMinus's first
warning, clang's `-Wswitch`, which finds the case a switch over an enumeration forgot.

### An expression with an error of its own

An undeclared name lowers to a poison float, so that the lowering goes on and finds the program's other errors. A
float is a fine operand for most operators, but not for all: `*(p + y)` with no `y` was three errors, the name, `+`'s
"invalid operands to binary expression ('float *' and 'float')" and `*`'s "indirection requires pointer operand",
where clang gives one. Recipe 25 stopped the cascade for the bitwise operators by counting errors around their
operands; this recipe gives the count to every expression. `lower` counts the errors its expression reports, and the
`Value` it returns says whether there were any:

```cpp
    Lowering::Value Lowering::lower(const ast::Expr& expr)
    {
        const std::size_t reported = diagnostics_.errors().size();
        Value value = std::visit(/* ... each node ... */, expr.node);
        value.failed = value.failed || diagnostics_.errors().size() != reported;
        return value;
    }
```

`failed` is clang's "contains errors": the expression, or a part of it, reported an error. It is set only where an
error is behind it: by `lower`'s count, and by `recovery()`, the stand-in for what an operand's error kept from being
computed, or a declaration's (a function declared to return a `bool *`, refused at its declaration, since FMinus has
no pointers to `bool`: an operator on its result reports nothing more). `poison()`, the plain stand-in, is not failed: a
`?:` of two `void` calls has no value and no error, and each of its uses must report one, as a `void` call's do,
"initializing 'float' with an expression of incompatible type 'void'". A failed value with no error behind it would be
taken silently everywhere, and the program would compile on a poison. What takes a failed value says nothing more
about it:

- **An operator** (`binary`) gives a `recovery()` of the type it would likely have had (`recovered`): an `int` for a
  bitwise one, a `bool` for a comparison (a vector operand's lane mask), otherwise the other operand's type as it
  computes, a pointer staying one, so that `p + y` is a `float*` and what uses it reports nothing either. The unary
  operators likewise.
- **A subscript, `*`, `->` and `&`** (`lvalue`, `lowerAddressOf`): a base or an operand that failed is no "subscripted
  value is not a pointer", no "indirection requires pointer operand" and no "cannot take the address of an rvalue". A
  pointer whose index failed still gives its element, of its type.
- **A conversion** (`convert`): a failed value converts to any type, silently, as a poison of it. So initialising,
  assigning, passing, returning and testing one report nothing more: `float* q = y;` is the one error about `y`.
- **`?:`, a member, a method, a builtin's argument**: the same.

An operator whose operands are fine but which is in error itself, `p + q` of two pointers, reports its error, and its
result is failed in turn: `*(p + q)` is one error. The rule needs the count, not the IR: `(int)3e10f` folds to a poison
with no error, and `(float)((int)3e10f) | 1` is still refused (recipe 25).

### A constant condition takes one branch

A non-void function that can reach its end is an error (recipe 4), and `reachable()` follows the CFG from the entry.
`while (1)` branched on `i1 true` to its body or its end, so its end looked reachable, and a function that ended in
one needed a `return` that could never run, where clang's check, which folds constant conditions, asks for none.
`branchOn` is clang's: a condition that folds to a constant branches to the one block it picks, so `while (1)`,
`while (1.5f)`, `for (; RUNNING;)` with an enumeration constant, `while (RUN)` with `const int RUN = 1;` at the top
level, and `if (1)` reach what follows only through a `break`, or the other branch. The branch not taken is still
lowered, for its errors, and the optimiser deletes it.

Whether the condition folds is the tree's to say, `folds()`: a constant expression (`isConstant`), or one that also
reads a constant of the top level that is no array, which `lower` reads as its initialiser's value, as clang folds a
const object's. The value is the IR's, which folds such a test against 0 to an `i1`. The IR alone would not do: a
frozen parameter's read lowers to a constant too, and `if (on[0]) return x;` at the end of a function would compile
frozen and be refused with a knob, so the same source's errors would hang on the host's bindings. A parameter is no
constant to `folds`, whatever its binding. `&&`, `||` and `?:` branch even on constants (recipe 3), so `while (1 &&
1)` still asks for a return, and so does a `const` of a block, which clang folds and FMinus reads from its slot. A
check that may fail (Bounds.cpp) keeps its conditional branch, on `false` for a constant index: the code after it must
stay reachable (Pitfalls).

### `sizeof` past an `int`, and an enumeration constant named as a builtin

`sizeOf` gave an `unsigned`, the data layout's 64-bit size truncated: a module of `float4 lanes[1073741824]`, 16 GiB,
was 0 to `sizeof`, and a copy of one was a `memcpy` of nothing. It gives a `std::uint64_t` now, C's `size_t`, which a
copy's `memcpy`, a declaration's `memset` and a method's `dereferenceable` take whole. FMinus's `sizeof` is an `int`,
so a size past the greatest one is an error, `'Huge' is too large for sizeof: 17179869200 bytes, and FMinus's sizeof
is an 'int'`, not a negative or wrapped `int`.

C's file scope has `<math.h>`'s functions in it, and clang refuses `enum { sinf };` after its `#include`:
"redefinition of 'sinf'" (an enumeration constant after a function is a plain redefinition to clang, recipe 27).
`declareEnum` says so of a constant of the file named as one of FMinus's builtins, as recipe 28 refuses a constant of
the top level. In a block, the constant is the block's own, as a variable of the name is.

### Enumerations: names of their type, and C23's fixed underlying type

C declares variables with an enumeration, `enum Mode { SAW, SQUARE } mode;`, and recipe 27 refused it. `enumeration`
now ends with `enum_declarators`, a declaration's declarators or none, and gives an `EnumDeclaration`: the enumeration,
then the names, as `declaration`'s DeclStmts of its type (`declarationsOf`). Each place takes them as it takes a
declaration: a block's statements after the `EnumStmt`, a module's fields (`addFields`) after the module's enumeration,
so a field's array may be sized by its own enumeration's constant (`enum { LEN = 4 } count, buf[LEN];`), and the top
level's constants (`addConstants`), which must be `const`, as every constant there. The tag is a type from the closing
brace on, as in C, so the names may use it: `enum_head`, the enumeration up to its brace, reduces there and declares
the tag before the declarators are read.

C23 gives an enumeration a fixed underlying type, `enum Mode : int { ... }`, which its constants must fit and which is
the type `enum Mode` names, and which lets a tag be declared ahead of its constants, `enum Mode : int;` (a type from
there on, complete). FMinus's integer types are `int` and `bool`, and `enum_base` takes either; another type is clang's
"non-integral type 'float' is an invalid underlying type". `ParseContext`'s tags are `Tag`s now: where, the
enumeration's id, the fixed type if any, and whether the constants are defined. A declaration ahead, `forwardEnum`,
declares a tag in the innermost scope; the definition after it, `declaredEnum`, finds it there and takes its id. Two
declarations of a tag in one scope must agree, in clang's words with a note at the first ("enumeration redeclared with
different underlying type 'bool' (was 'int')", "enumeration previously declared with fixed underlying type"); a second
definition is the "redefinition of 'Mode'" it was; `enum : int;` is "unnamed enumeration must be a definition", and
`enum Mode;` without a fixed type C's forward reference, unless the tag is already defined, which it then names, as C
allows. `declareEnum` checks the values against the fixed type, as clang does: an implicit one past it,
"enumerator value 2 is not representable in the underlying type 'bool'", an explicit one, "enumerator value is not
representable in the underlying type 'bool'". `--emit ast` shows the type after the tag, `(enum Mode : int (SAW))`.

### The enumerated type

`-Wswitch` (below) needs to know that a switch's value is of an enumerated type, and recipe 27 made `enum Mode` the
type `int`. Recipe 27's Where to go next proposed a `Kind` of its own, which every switch over a type's kind would then
list, and every rule for an `int` would have to take. This recipe keeps `enum Mode` an `int` (or a `bool`), and gives
`ast::Type` which enumeration it is beside it, `enumeration`, the `Enum`'s id, which the parser numbers:

```cpp
        int enumeration = 0;   // an enumerated type's Enum::id, or a pointer's or an array's whose pointee is one

        friend constexpr bool operator==(Type a, Type b)   // every field but `enumeration`
        {
            return a.kind == b.kind && a.pointee == b.pointee && a.pointeeConst == b.pointeeConst
                   && a.module == b.module && a.count == b.count;
        }
```

`==` ignores it, so every rule that asks whether a type is an `int` still says yes, every message still says `'int'`,
and nothing else in the compiler changes. A value keeps it where it keeps its type and C keeps the enumerated type: a
variable, a parameter, a field, a result, a cast, an assignment, `++`, an element, `*p` (`pointerTo`, `arrayOf` and
`pointee` carry it). It goes where C promotes: `arithmeticOf` gives a plain `int`, so `mode + 1`, `-mode` and
`c ? a : b` have none, and an enumeration constant is an `int`, as in C before C23.

### `-Wswitch`, and FMinus's warnings

clang warns of a switch without a `default` on a value of an enumerated type whose cases miss some of its constants:

```
waves.fm:14:13: warning: enumeration values 'TRIANGLE' and 'NOISE' not handled in switch [-Wswitch]
waves.fm:14:13: note: add missing switch cases
```

`declareEnum` keeps each enumeration's constants by its id (`enumerations_`), and `lowerSwitch`, after the body, hands
`unhandled` the value's enumeration and the switch's cases. It says nothing when the switch has a `default`, when a case
was in error (no constant, or a duplicate: `Switch::erroneous`), when the value folds to a constant (`folds()`, as
`branchOn` asks: `const enum Shape { SAW, SQUARE } shape = SQUARE;` at the top level is one, and clang says instead
"no case matching constant switch condition '1'", a warning FMinus does not give), or when the enumeration has no
constants yet (one declared ahead, whose list comes after the switch, as clang finds it incomplete). Otherwise it lists
the values no case has, by each value's first constant and in order of value, as clang does (`enum { B = 1, A = 0, A2 =
0 }` misses `'A'` and `'B'`), in clang's four forms: one, two, three, and "4 enumeration values not handled in switch:
'B', 'C', 'D'...". The warning is at where the value begins, as clang places it, with clang's note: `begin` gives
`m->mode` at `m`, and `(e)` at its `(`, which the tree keeps on the expression inside the parentheses
(`Expr::opening`, the outermost one's), since it has no node for them.

A warning is a `Diagnostic` too, in `Diagnostics::warnings()`, apart from the errors, so that `errors().size()`, which
the code generator's counts read, counts errors only. `format()` prints both in source order, each with its severity,
as clang prints them. `fminus` prints a compiled program's warnings to stderr and goes on; `Program::warnings()` holds
them for a host, and a program that does not compile has them among its `errors`. clang's other `-Wswitch`, "case value
not in enumerated type", is not FMinus's (Where to go next), and there is no flag to turn a warning off or into an
error.

### What it changes for programs and hosts

- **Programs** that FMinus refused and C accepts now compile: an enumeration with names after its list, C23's fixed
  type and a tag declared ahead, a non-void function ending in a loop only a `break` leaves, or in `if (1) return`. Two
  it took are refused: `sizeof` of a type past 2 GB, and an enumeration constant of the file named as a builtin.
- **Error messages** are fewer: a program with an undeclared name, say, gets one error where it got two or three. A
  host that counts a program's errors counts fewer; one that matches them by text finds the first unchanged.
- **Warnings** are new: `Program::warnings()`, empty for a program with none, and lines that begin `warning:` among a
  failed program's `errors`. A host that shows `errors` shows them as clang does; one that reads it line by line takes
  `warning:` and `note:` lines beside `error:` ones. The header's API is otherwise unchanged.
- **IR** changes only where a condition is a constant, `while (1)`'s `br label %while.body` where it had
  `br i1 true, ...` (the same code at `-O1` and up), and where a module is past 4 GB: its copies and zeroing were
  `memcpy`s and `memset`s of the size's low 32 bits.

### Tests

- `tests/unit/RecoveryTest.cpp`: an undeclared name, or a call of no function, under every construct above, one error
  each, at clang's column; an operator's own error still reported, and its result taking nothing more; a `?:` of two
  `void` calls as an initialiser, a result, an operand, an argument, a pointer, `*`'s and `-`'s operand, an assigned
  value, a subscript and a condition, each clang's error, and as a statement, run; functions that end after
  `while (1)`, `for (;;)`, `while (RUNNING)`, `while (1.5f)`, `if (1)`, `if (0) else`, nested ones and a constant of
  the top level, run, and those whose end is reached (a `break`, `while (n)`, `if (0)`, `for (; 0;)`) still refused,
  with a dead branch's error; a parameter's `if` and `while` refused alike as a knob, a stream and frozen; `sizeof`
  past the greatest `int` (2 GiB + 4, 16 GiB) and of 2^31 - 4 bytes; builtins' names as the file's enumeration
  constants, and one in a block.
- `EnumTest.cpp`: names declared after a list, run: a local, a pointer to it, an array of the type, a module's field
  and a field sized by its own enumeration's constant, a const table and a constant at the top level; C23's fixed
  types, run: `int` declared ahead and used before its list, `bool` taking any number as a bool, `sizeof` of each, one
  declared ahead in a block; the values' errors against `bool` and `int`.
- `SwitchTest.cpp`: `-Wswitch`'s four forms and their notes from a program that runs; values by their first name in
  order of value; every source of an enumerated value (a variable, a field in a method and through a pointer, a host's
  parameter, a result, a cast, an assignment, an element, `*p`, C23's `bool`, an enumeration without a tag), each at its
  column, and a parenthesised one at its `(`; and none for a default, every constant, an `int`'s cast, arithmetic,
  `-e`, `?:`, a constant, an `int`, an enumeration declared ahead whose list comes later, a constant of the top level,
  and a case in error.
- `FrontendTest.cpp`: the trees of names after a list at the top level, in a module and in a block; fixed types and
  a tag declared ahead; each tag error, with its note.
- `tests/library/ProgramTest.cpp`: `Program::warnings()` of a program that runs, empty for one with none, and a failed
  program's `errors` with its warning first, in source order.
- `tests/lit/fminus/recovery-errors.fm`: every cascade above in one function, one error each;
  `enum-switch.fm`: `fminus` printing the warning and its note, a field's switch at its column, and running the
  program; `large-modules.fm`: a 16 GiB module's `memcpy`, `memset` and `dereferenceable` at 64 bits, and its `sizeof`
  refused (`-D SIZEOF`). `enum.fm`'s constant loop branches to its body alone.

## Recipe 33: `double`, for accumulators and slow state

*In the compiler. Code: `Kind::Double`, `Type::Double` and `isFloating` (`src/frontend/Ast.h`), `typeName`
(`Ast.cpp`), the `double` keyword (`Lexer.l`) and its rules in `Parser.y` (`type`, `cast_type`, and `poly double`
refused); `typeOf`, `convert`, `commonType`, `sizeOf` and `checkType` (`src/ir/Types.cpp`); `lower`, `lowerAs`,
`lowerWith`, `hasDouble`, `staticType`, `lowerConditional`, `lowerNumber`, `lowerCast` and `lowerAssign`
(`Expressions.cpp`), and `lowerAs` where a value goes somewhere typed: `lowerReturn` and `declareVariable`
(`Statements.cpp`), `initializeArray` and `declareConstArray` (`Memory.cpp`), `defineConstant` and `fold`
(`Constants.cpp`), `lowerCall` (`Calls.cpp`) and `lowerMethodCall` (`Modules.cpp`); `kBuiltins`' double functions and
`kLibmDoubles` (`Calls.cpp`), and a double builtin's prototype (`declare`, `Functions.cpp`); `lowerConstruct`
(`Vectors.cpp`); the `"double"` tag (`Tbaa.cpp`); `DenormalFolder::FoldCast` (`Denormals.h`, `Denormals.cpp`);
`addEntryPoint` (`src/support/Aot.cpp`) and `fminus_rt_main_double_args` and `fminus_rt_main_double_result`
(`src/runtime/Runtime.cpp`); `--run` (`tools/fminus/main.cpp`); `isFMinusKeyword` (`src/circuit/Emit.cpp`). Tests:
`tests/unit/DoubleTest.cpp` (`[double]`), `FrontendTest.cpp`, `AotTest.cpp`, `tests/library/DoubleTest.cpp`, and
`tests/lit/fminus/double.fm` and `double-errors.fm`.*

A float has 24 bits, and some of a program's state needs more. An engine's coolant warms by a millionth of a degree a
block: at 90 degrees, half a float's ulp is 3.8e-6, so `t += 1e-6f` rounds back to 90 every time and the temperature
never moves. A clock counted in seconds a sample at a time, at 48 kHz, reads 57.8 after a minute, two seconds behind. A
one-pole's coefficient a hair below 1 is a different filter rounded to a float. C's answer is `double`, and this recipe
adds it: C's IEEE double precision, for accumulators, clocks and slow state, beside the floats that a block's audio is.

### The type

`double` is a kind of its own, `Kind::Double`, LLVM's `double`: 8 bytes, aligned to 8 by the data layout, in a module's
struct as C lays one out (a `float` then a `double` puts the double at offset 8). It goes wherever a float goes but
into a vector: a variable, a parameter, a result, a field, an array's element (`canPointTo` takes it, so `double*` and
`double t[8]` are C's), a constant of the top level and a table stored once. Its TBAA tag is clang's, `"double"`, a
leaf of its own beside `"float"`, so a store of a double changes no float. `sizeof(double)` is 8.

A double is no vector lane: FMinus's vectors are floats, and there is no `double4` or `poly double` (the parser refuses
the second by name). A double meeting a vector is clang's error for the truncation its broadcast would be,
`cannot convert between scalar type 'double' and vector type 'float4' as implicit conversion would cause truncation`,
from `commonType`; `float4 v = d;` is `initializing 'float4' with an expression of incompatible type 'double'`, from
`convert`. A vector literal names its lanes' type, so there a double rounds to a float, as a cast would:
`(float4)(d)` and `(float4){d, d}` are OpenCL C's and clang's. LLVM's vectorisers still put doubles side by side in a
loop over a `double` array: two lanes in a NEON register, four in an AVX one.

### C's conversions

`convert` gains four instructions: `fpext` from a float (exact), `fptrunc` to a float (rounded to nearest, to infinity
past the greatest float), `sitofp` and `uitofp` from an int and a bool, and `fptosi` to an int, truncating, as from a
float. A double is `x != 0` as a bool, `fcmp une` as a float's is. `commonType` is C's usual arithmetic conversions: two
ints are an int, a double anywhere is a double, a float anywhere else a float, so `f + i + d` adds `f + i` in float,
then widens it, as C does. What C refuses of a double, FMinus refuses in clang's words: a bitwise operator
(`invalid operands to binary expression ('double' and 'int')`), `~`, a switch's value, a subscript (`array subscript is
not an integer`) and an integer constant expression (`size of array has non-integer type 'double'`). `%` is `fmod`'s,
as it is `fmodf`'s on floats. A module's parameter stays a float, an int or a bool: its slot is what a host's
`setValue` writes and a stream's buffer is floats.

### Floating constants: floats first

The decision is what an unsuffixed `0.1` is. C says a double, so `x * 0.1` of a float `x` widens `x`, multiplies in
double and rounds the product back. FMinus has always said a float, and every program written for it, its examples
and every host's, computes `x * 0.1` as a float product. Taking C's rule would change those programs' bits wherever a
double product rounds to another float than the float product (rarely, but not never), and their speed everywhere: an
`fpext` and an `fptrunc` around every multiply by a constant, and a loop of them vectorised at half the lanes. GLSL
keeps float first and spells a double constant `0.1lf`, which C cannot read.

FMinus keeps float first, and lets a constant be C's double where its arithmetic has a double in it. A constant's
arithmetic is the expression around it as far as `+`, `-`, `*`, `/`, `%`, unary `+` and `-`, the arms of `?:` and
parentheses reach. It has a double when one of its operands is a double, when the two sides of a comparison it is in
have one, or when its value goes to one: a `double` variable it initialises or is assigned to (or whose value it
changes, `d *= 0.999`), a parameter, a result or a cast. Then every unsuffixed constant in it is the double nearest its
spelling, and the arithmetic computes as C's does, step for step: the same types at each operator, so the same bits.
Where it has no double, it computes as it always did:

| Expression | FMinus | C |
|---|---|---|
| `float y = x * 0.1;` | a float product | a double product, rounded to float |
| `double y = x * 0.1;` | a double product | the same |
| `double third = 1.0 / 3.0;` | C's third | the same |
| `x * 0.1 * d` | `(double)x * 0.1 * d` | the same |
| `d < 0.1`, `d == 0.1` | compared with C's 0.1 | the same |
| `d *= 0.999;`, `t += 1.0 / rate;` (a float `rate`) | C's 0.999; a double quotient | the same |
| `c ? 0.1 : d` | C's 0.1 | the same |
| `sinf(x * 0.1)`, `(float)(x * 0.1)` | a float product | a double product, rounded to float |
| `(int)(x * 0.1)` | a float product | a double product |

So a program without doubles means what it meant before they came, bit for bit, and one with doubles means C's
wherever its constants meet them. What differs from C is what always did: a constant in arithmetic of floats whose
value stays a float (or becomes an int).

The code generator decides it in `lower`. Each call takes a flag the arithmetic around it set, `doubleArithmetic_`,
for that call only: `+`, `-`, `*`, `/` and `%`, a sign and `?:`'s arms pass theirs on to their operands, and every
other expression's parts start an arithmetic of their own. At the top of an arithmetic there is no flag, and
`hasDouble` looks for a double among its operands:

```cpp
    // FMinus's floating constants are floats first ... unless the arithmetic it is part of has a double.
    Lowering::Value Lowering::lowerAs(const ast::Expr& expr, ast::Type type)
    {
        return lowerWith(expr, type == ast::Type::Double || hasDouble(expr));
    }

    bool Lowering::hasDouble(const ast::Expr& expr) const
    {
        if (std::holds_alternative<ast::NumberExpr>(expr.node))
            return false;
        // a sign, + - * / %, and ?:'s arms: the same arithmetic
        ...
        return staticType(expr) == std::optional<ast::Type>(ast::Type::Double);
    }
```

The operands have to be known before anything is lowered: in `x * 0.1 * d` the constant comes before `d`, and the
code generator lowers left to right, the order a program's side effects happen in, which no constant may change. So
`staticType` works an operand's type out from the declarations alone, as lowering it would give it: a variable's (or a
field's by name in a method), `m.y` and `p->y`, `a[i]` and `*p`, a call's result (a function's, a method's, a double
builtin's), a cast's, an assignment's or an increment's target, `p + i`, and a `?:` whose arms agree. A comparison's
two sides are one arithmetic, so `d < 0.1` asks both. Where a value goes is known before it is lowered too: a
declaration's type, the function's result, a parameter's (`lowerCall` looks the callee up by name first, a method's
too), a cast's, an assignment's target (`staticType` again), an array's element. Those call `lowerAs(expr, type)`, and
`lower(expr)` is `lowerAs(expr, void)`. A constant whose arithmetic has a double reaches `lowerNumber` with the flag
set, and reads its spelling into `APFloat::IEEEdouble()` instead of `IEEEsingle()`: one rounding, from the decimal, as
clang's. A double's constant past a float's range is no error (`1e300`); a float's still is.

Integer constants stay ints, so `d + 1 / 2` adds 0, as C does. An integer constant expression's floating constant is
the float it always was (`case 1.5:` is "not 'float'"); `fold` takes a cast to `double` as it takes one to `float`.

### The math

C's double functions that are one instruction are builtins: `sqrt`, `fabs`, `floor`, `fmin`, `fmax` and `fma`, each
an LLVM intrinsic on `double` (`llvm.sqrt.f64`, ..., `llvm.fma.f64`), exact and the same in every math mode, a
`Builtin` whose `type` is `Kind::Double`. They take doubles only: an argument converts to one (a numeric argument is
C's double, `fmax(d, 0.5)`), and a vector is an error. `fma` rounds once, as C's does; FMinus still contracts nothing
else into one. Their names became builtins as `tanf` did, so a program's own `floor` is "a builtin and cannot be
redefined", and a prototype must be C's, `double sqrt(double x);` (a `float sqrt(float x);` "conflicts").

The transcendental ones are not: FMinus's kernels (recipe 23) are fitted to a float's 24 bits, and a double's would
need fits, tests and ulp tables of their own. `sinf(d)` takes its double as C's prototype converts it, rounded to a
float, and computes in float. A program that wants the C library's double `sin` declares it, `double sin(double x);`,
as any external function (on Apple systems `describeLibm` marks it as writing no memory, as it marks `powf`); a call
without the prototype is clang's error with a note that says so:

```
synth.fm:9:28: error: call to undeclared function 'sin'
synth.fm:9:28: note: FMinus's sinf computes in float, and rounds a double argument to one; a prototype, 'double sin(double x);', calls the C library's on doubles
```

That leaves the C library on the audio path, which the float functions avoid: keep a double's transcendentals off the
audio thread, or compute them in float.

### Hosts, `--run` and executables

A function of doubles is C's to a host: `program->function<double(double)>("seconds", errors)`. A module's double field
is `"double"` (or `"double[8]"`) in `Module::Field::type`, 8 bytes an element at its offset, and a host that reads
fields by their type (a trace, an inspector) reads it as C++'s `double`; one that knew only `"float"`, `"int"` and
`"bool"` finds no match for it, and so never reads it as a float. `fminus --run` passed every argument as a float or
every one as a double, as the function's parameters were, and refused a mix (recipe 36's thunk passes each as its own
type), and prints a double as the shortest decimal that reads back as it. An executable's
`main` may return a double and take doubles beside floats: its C `main` reads them through `fminus_rt_main_double_args`,
each as its kind says (`strtod` or `strtof`, whose float the double holds exactly), and prints through
`fminus_rt_main_double_result`; a `main` of floats keeps the functions and the code it had. Both are new runtime
symbols, so a runtime built before them links no program that uses them, and every program it linked still links.
`--emit obj` and `asm` need nothing: a double is LLVM's own.

### Subnormals

Under `Denormals::Flush`, a double's subnormals flush as a float's do: AArch64's FPCR.FZ and x86's MXCSR FTZ and DAZ
apply to both, and LLVM's `denormal_fpenv` attribute covers every type. The code generator's folder flushes the two
conversions as well (`FoldCast`): `(float)(double)1e-40` folds to 0, as FCVT and CVTSD2SS give it flushing. LLVM's own
folding does not flush a conversion (`ConstantFoldCastOperand` ignores the function's mode), so an `fpext` or
`fptrunc` of a subnormal that only the optimiser finds constant folds as IEEE's, where the hardware would give 0: the
same kind of gap as LLVM's `x * 1.0` to `x`, which leaves a subnormal `x` that a flushing multiply would have zeroed.

### What it changes for programs and hosts

- **Programs** without doubles compile to the same IR, bit for bit: no constant changes type, and no conversion is
  added. `double` is a keyword, and `sqrt`, `fabs`, `floor`, `fmin`, `fmax` and `fma` are builtins: a program that
  named something so renames it (the circuit compiler refuses them as a netlist's names too).
- **Messages** that list FMinus's types name `double` (`FMinus has pointers to float, double, int, ...`), `--run`'s
  say it passes and prints doubles, and `main`'s that it may return and take them.
- **Hosts** get `"double"` fields, aligned to 8, and `ir::builtins()` six more names.

### Tests

- `tests/unit/DoubleTest.cpp`: constants float first, as they were (`x * 0.1`, `1.0 / 3.0`, `x < 0.1` and a cast to
  float, all floats); C's doubles where the arithmetic has one, against C++'s own doubles: a result, an initialiser, a
  float operand, an operand after the constant, a comparison, a compound assignment, a `?:`'s arm, a cast, a parameter,
  a table's list, `1e300 * 10`, and a float's `1e39` still refused; the usual arithmetic conversions and casts both
  ways, a NaN as a bool, `%`, `++` and `--`; a float's slow state stuck at 90 where a double's moves; a module's double
  fields and array, a top-level constant and table, `double*` both ways and `sizeof`; the six builtins against
  `<cmath>`, `fma`'s one rounding against the product's two, `sqrtf` of a double, and libm's `sin` by prototype;
  subnormals flushed in folding, in a computation and in a conversion; and the errors above, in clang's words.
- `FrontendTest.cpp`: `double` as a parameter, a pointer's pointee, an array's element and a cast; `poly double`.
- `AotTest.cpp`: a `double main(double, float)`'s entry, its readers and printer.
- `tests/library/DoubleTest.cpp`: `module()`'s description of double fields (type, offset, size, alignment), the host
  reading and writing them, a function of doubles; doubles under the loop budget, bounds checks (`'taps' of type
  'double[8]'`) and `#ifdef`.
- `tests/lit/fminus/double.fm`: the IR (`fpext`, the double constant, the `"double"` tag beside `"float"`), `--run`'s
  results and refusal, an executable's `main` of a double and a float, its entry, and `--emit obj`;
  `double-errors.fm`: each error once, and `poly double`.

## Recipe 34: 2-D arrays, C's arrays of arrays

*In the compiler. Code: `Type::rowKind` and `rowCount`, `pointerTo`, `arrayOf`, `pointee`, `hasRows`, `elementOf` and
`samePointee` (`src/frontend/Ast.h`), `Brace`, `DeclStmt::rowSize` and `braces`, `Field::rowSize` and `SizeofExpr`'s
sizes (`Ast.h`); `typeName`, `declared` and `list` (`Ast.cpp`); `array_sizes`, `initializer_list`, `initializers`,
`initializer`, `declaredName`, `setSizes`, `setList` and `append`, the parameter's rule and `sizeof`'s (`Parser.y`);
`checkType` (`src/ir/Types.cpp`); `sized`, `arraySize`, `declaredArray`, `floatsLike` and `defineConstant`
(`Constants.cpp`); `element`, `lvalue`, `placeList`, `elementAt`, `initializeArray`, `declareConstArray`, `tableOf` and
`pointerArithmetic` (`Memory.cpp`); `declareVariable` (`Statements.cpp`); `layOut` (`Modules.cpp`); `lowerSizeof` and
`lowerConditional` (`Expressions.cpp`); `Module::Field::element` and `extents` (`include/fminus/FMinus.h`,
`src/api/FMinus.cpp`). Tests: `tests/unit/Array2DTest.cpp` (`[array2d]`), `BoundsCheckTest.cpp`, `FrontendTest.cpp`,
`tests/library/Array2DTest.cpp`, and `tests/lit/fminus/array2d.fm` and `array2d-errors.fm`.*

A bank of wavetables is four tables of 256 samples; a reverb's delay network is eight lines of 2048; a mixer's gains
are a matrix. Recipe 12's arrays are one dimension deep, so a program wrote either `float bank[4 * 256]` and indexed it
as `bank[t * 256 + i]`, or a module of one row, `module Row { float v[256]; }; Row bank[4];`, and `bank[t].v[i]`.
Both work, and both hide the shape: the bounds checks see one array of 1024 floats, a host sees 1024 floats, and a
reader sees arithmetic. C's answer is an array of arrays, and this recipe adds it: `float bank[4][256];`, rows side by
side, `bank[t][i]` an element and `bank[t]` a row, a `float*` wherever one goes.

### The type: a row beside the element

C's `float[4][256]` is an array of 4 `float[256]`s, and its name stands for a pointer to its first row, a
`float (*)[256]`. A `Type` is a value: a kind, and for an array or a pointer the kind of its elements (`pointee`), with
a module or an enumeration beside it. Making it nest, an element that is a `Type` of its own, would make every type a
tree and every comparison a walk, for arrays that go two levels deep. So a row is described where an element's kind is:
`pointee` is `Kind::Array`, and two fields say what the row is.

```cpp
        Kind rowKind = Kind::Void;   // when the pointee is a row (Kind::Array): what the row's elements are
        int rowCount = 0;            // and how many it has (0 in the syntax tree, as `count`)
```

`arrayOf(arrayOf(float, 256), 4)` is `float[4][256]`: an array of 4, its pointee `Kind::Array`, its rows of 256
floats. `pointerTo(float[256])` is the pointer its name stands for, `float (*)[256]`. `pointee()` of either gives the
row back, an array again, so everything that asks "what is an element of this" gets the row, and the row's own
`pointee()` is a float. `elementOf` goes straight to the float, and `hasRows` says whether a type has rows. `==` and
`samePointee` compare the row too, so `float (*)[256]` is not `float (*)[16]`.

The choice pays for itself where nothing was written. Every rule that tests a pointer's pointee (`pointee ==
Kind::Module` for `p->y`, `canPointTo(pointee)`, `pointee == kind` for `poly_load`'s float pointer, `samePointee` for
an assignment) sees `Kind::Array`, which none of them takes, so a pointer to rows is refused everywhere a float pointer
or a module pointer goes, in the words those rules already had: `float* p = grid;` is "incompatible pointer types
initializing 'float *' with an expression of type 'float (*)[16]'", clang's. The places that take a row say so:
`element()`, which gives the row an index reaches, and `pointerArithmetic`, which moves a pointer to rows by rows.

### A row decays, and its elements are indexed

Nothing new lowers `grid[i][j]`. `grid` is read as any array is (`read()`, Memory.cpp): a pointer to its first
element, which is now its first row. `grid[i]` is `element()` on that pointer: a GEP of the row type, `[16 x float]`,
by `i`, and an lvalue of the row's type, an array. Reading an array decays it, so the row stands for a pointer to its
first float, and `[j]` is a float's element:

```cpp
        const bool row = ast::isArray(type);   // a pointer to rows: the element is a row
        if (!ast::canPointTo(type.kind) && !row)
            return std::nullopt;
```

```llvm
; float g[2][3]; ... g[r][c]   (fminus -O0)
  %arraydecay = getelementptr inbounds [2 x [3 x float]], ptr %g, i64 0, i64 0
  %arrayidx = getelementptr inbounds [3 x float], ptr %arraydecay, i64 %idxprom
  %arraydecay4 = getelementptr inbounds [3 x float], ptr %arrayidx, i64 0, i64 0
  %arrayidx8 = getelementptr inbounds float, ptr %arraydecay4, i64 %idxprom7
```

InstCombine folds the four into one address, and the loop vectoriser sees what it sees for a 1-D array: a loop over a
row's floats, at a constant stride. `for (v ...) for (i ...) gain[v][i] *= k;` over a `float gain[8][64]` field
vectorises its inner loop at the target's width, as `for (i ...) flat[i] *= k;` does over a `float flat[512]`.

The rest follows from decay. `grid[i]` goes wherever a `float*` does (a parameter, `poly_load(grid[i] + j)`,
`float* row = grid[i];`); `*grid` is the first row, `grid + 2` the third, and `(grid + 2) - grid` is 2. A row is no
lvalue to store to ("array type 'float[16]' is not assignable"), and an element of a const table names its variable
when it is assigned (`lvalue` looks through `t[i]` to `t`: "cannot assign to variable 't' with const-qualified type
'const float[2][3]'"). A module's field, a constant of the top level, a local, a const table and an array a parallel
region captures are arrays as before, so each of them may be a 2-D array too; so is an array of modules,
`Voice voices[2][8];`, whose `voices[i][j].tick()` is a method's call on a module, and whose row, `voices[i]`, is a
`Voice*`.

### What a program cannot have: a pointer to rows

No variable, field or parameter holds a `float (*)[16]`. C writes one `float (*p)[16]`, which FMinus's declarators do
not parse (they have one `*`, before the name), and `&grid[i]` is refused as `&` of any array is ("FMinus has pointers
to ..., not to 'float[16]'"). A parameter declared as a 2-D array would be one too: C's `void f(float g[4][16])` takes a
`float (*)[16]`. The parser refuses it ("FMinus has no parameters of 2-D arrays, which would be pointers to rows: pass
a row (a float*), or the module that holds the array"), for a reason of its own: a parameter's type is fixed in the
first pass (`declare`, Functions.cpp), before the file's enumerations are declared, and a row's size, `[COLS]`, is an
integer constant expression of them. A function takes a row, or the module whose method reaches the array by name.

### Sizes, and `sizeof` of an array type

A declarator takes one or two sizes (`array_sizes`): `[4][16]`, the rows' last, in `DeclStmt::rowSize` (and
`Field::rowSize`), as written. A third is "FMinus's arrays have at most two dimensions". `sized` (Constants.cpp) works
the row's size out first (`arraySize`, the integer constant expression's rules), since C does not let `[]` leave it out
("array has incomplete element type 'float[]'"), and then the count of rows: written, or as many as the list starts.
The elements in all, rows times their size, are at most 2^30, as a 1-D array's are. A row whose size is in error, or
missing, makes the declaration's one error, as it makes clang's type invalid: the row is then as wide as the list's
widest braced row, and the array has its rows as written, or as many as the list reaches with that row when it reaches
more, so neither the list (its excess) nor the count (the size a list gives) reports more, as a 1-D array whose size is
in error takes its list's length. A field without either size, `float d[][]`, is the same error, not "field has
incomplete type".

`sizeof` takes an array type now, C's abstract declarator: `sizeof(float[4][16])` is 256, `sizeof(int[N])` four times
N. Its sizes are expressions too (`SizeofExpr::size` and `rowSize`), so `lowerSizeof` sizes the type as a declaration's.
`sizeof(float[])` is C's incomplete type. FMinus still has no `sizeof` of an expression (Where to go next).

### Initialiser lists: C's placement

A 2-D array's list may brace its rows, `{{1, 2}, {3}}`, or not, `{1, 2, 3}`, or mix the two, and C says where each
value goes: a braced list starts a row, from the row's first element, and its values fill that row; a value outside a
brace goes to the next element, crossing into the next row as it needs (C's brace elision); after a braced row, the next
value starts the next row. So `float g[3][3] = {{1}, 2, 3, 4, {5}};` is rows `1 0 0`, `2 3 4` and `5 0 0`.

The tree keeps a list as its values in order (`DeclStmt::list`, as before), and the braces among them beside it
(`ast::Brace`: where its `{` is, which values it holds, how deep it is). Recipe 12's code reads `list` as it always
did, and a 1-D array's list has no braces. The grammar builds both bottom up: a value is an item, and a braced list is
an item whose values move into the list around it, its brace first, one level deeper than its own braces:

```yacc
initializer
    : expr                                  { $$.items.push_back($1); }
    | initializer_list                      {
                                                InitList list = $1;
                                                $$.braces.push_back(ast::Brace{at(@1), 0, list.items.size(), 1});
                                                for (ast::Brace& brace : list.braces)
                                                    ++brace.depth;
                                                append($$, std::move(list));
                                            }
    ;
```

`placeList` (Memory.cpp) then places the values as C does, for every array, a 1-D one's too: each value and the element
it goes to, counted from the first, row by row, and how far the list reaches (what sizes `[]`). Where C only warns,
FMinus refuses, in clang's words, at the value or the brace: an excess value ("excess elements in array initializer",
at the first value too many, clang's place, which recipe 12 put at the declaration), and a brace around one element
("braces around scalar initializer": a 1-D array's, one inside a row, or one that would start mid-row, where C would
take it as one element's).
`initializeArray`, `declareConstArray` and `defineConstant` store each value at its element (`elementAt`: a GEP of
`0, row, column`).

A const table of constants is stored once, as recipe 26's are, row by row: `tableOf` makes each row a table of its own
(`tableContents`, clang's shapes), then the rows a table of rows. When a row has eight zeros or more at its end it is a
struct, as a 1-D table's is, and the rows go side by side in a packed struct instead, the rows after the last one given
a single `zeroinitializer`. The bytes are the array's either way: a row's size is a multiple of its elements'
alignment, so no padding comes between rows.

```llvm
@kGrid = private constant [2 x [3 x float]] [[3 x float] [float 1.0, float 2.0, float 3.0], [3 x float] [float 4.0, float 0.0, float 0.0]]
@sparse = private constant <{ [1 x [4 x i32]], [15 x [4 x i32]] }> <{ [1 x [4 x i32]] [[4 x i32] [i32 1, i32 0, i32 0, i32 0]], [15 x [4 x i32]] zeroinitializer }>
```

### Bounds checks, a dimension at a time

Recipe 30 checks an access against the extent its pointer carries, made where an array decays. A 2-D array decays
twice, so it is checked twice: `grid[i]` against the rows (`index 4 out of bounds for 'grid' of type 'float[4][16]'`),
and `grid[i][j]` against the row (`index 16 out of bounds for 'grid[i]' of type 'float[16]'`), each named as the source
names it (`spelled`), so the message says which dimension. That is UBSan's `-fsanitize=array-bounds`, which checks
each subscript of a C array of arrays against its own bound: `grid[0][16]` is out of bounds though `grid[1][0]` lies
there. A row is checked as an element wherever it is used, even to make a pointer of it (`float* end = grid[4];` is
out), and a pointer formed from a row may point one past the row's end, no further. A loop whose conditions keep both
indexes in their bounds has no check, and vectorises as it does without them.

### Hosts

A module's 2-D field is `"float[4][16]"` in `Module::Field::type`, and two new members spell its shape out:
`element`, `"float"`, and `extents`, `{4, 16}`, outermost first (`{16}` for a `"float[16]"`, empty for a field that is
no array). The rows lie side by side, as C++'s `float[4][16]` at the field's offset, so element `[i][j]` is at
`offset + (i * 16 + j) * sizeof(float)`. A host that counted a field's elements by multiplying the numbers in its type's
brackets counts 64, and reads them in order, rows one after another.

### What it changes for programs and hosts

- **Programs** without 2-D arrays compile to the same IR. A list's excess value is reported where it is, not at the
  declaration, and a brace inside a 1-D array's list is "braces around scalar initializer", not a syntax error.
- **Messages** name the new types as clang does: `float[4][16]`, `float (*)[16]`.
- **Hosts** get `Module::Field::element` and `extents`, and fields typed `"float[4][16]"`.

### Tests

- `tests/unit/Array2DTest.cpp`: 2-D locals zeroed and indexed, a row passed as a `float*` and moved as one, the rows
  side by side, the name a pointer to rows (`*(g + 2)`, `(g + 2) - g`, a comparison), a `?:` of rows, `sizeof` of array
  types, a fresh array each declaration; a module's 2-D fields of modules, doubles (a constant C's double), `float4`s
  and ints, copied with their module, and its `sizeof`; lists braced, flat, mixed, `{}` rows and `[]` sized by a list;
  the tables' shapes in the IR (a plain table of rows, trailing zero rows, a row a struct, a local table, a local filled
  at run time) and their values; loops over both dimensions vectorised; a `parallel for` over rows, a local captured,
  a switch's jump past one; and the errors above, in clang's words.
- `BoundsCheckTest.cpp`: each dimension checked at every level, `'gain'` and `'gain[v]'` in the messages, a row checked
  as an element, a pointer formed from a row.
- `FrontendTest.cpp`: the dumps of 2-D declarations, fields, braced lists and `sizeof(float[4][16])`; a third dimension,
  a 2-D parameter and `sizeof(float*[2])` refused.
- `tests/library/Array2DTest.cpp`: `module()`'s shape of 2-D fields (type, element, extents, offset, size) and of the
  others (a 1-D field, arrays of modules of one and two dimensions, a scalar, a pointer), the host reading and writing
  rows, voices sharing one bank of tables; 2-D arrays under the loop budget, bounds checks (`BoundsViolation`'s array,
  type and extent for each dimension) and `#if`.
- `tests/lit/fminus/array2d.fm`: the IR of a 2-D local, its list and a table, the vectorised loop (with
  `--bounds-check` too, which leaves it no check), `--run` and `--bounds-check`'s messages for each dimension, the
  dump; `array2d-errors.fm`: each error once (a row without its size, whose list then reports nothing more), and the
  parser's.

## Recipe 35: attributes on fields, for the host

*In the compiler. Code: `AttributeArg`, `Attribute`, `DeclStmt::attributes` and `Field::attributes`
(`src/frontend/Ast.h`); `stringLiteral`, `attributeText`, `attributeNumber` and `isIgnoredAttribute` (`Ast.cpp`); the
`STRING` token, its rules and `stringValue`, and the skipped groups' string rule (`Lexer.l`); `attributes`,
`attribute_specifiers`, `attribute_specifier`, `attribute_word`, `attribute_arg`, `string_literal`, `declarator`,
`declarator_sizes`, `named`, `setSizes` and `addFields` (`Parser.y`); `checkAttributes` (`src/ir/Modules.cpp`) and
`refuseAttributes` (`Statements.cpp`, from `declareVariable` and `defineConstant`, and `Parallel.cpp`, from
`lowerParallelFor`); `Program::Attribute`, `findAttribute`, `Module::Field::attributes` and
`Parameter::attributes` (`include/fminus/FMinus.h`, `src/api/FMinus.cpp`); the meter example's labels
(`examples/meter/`); the VS Code grammar's `strings`. Tests: `tests/unit/FieldAttributeTest.cpp` (`[fieldattr]`),
`FrontendTest.cpp`, `tests/library/FieldAttributeTest.cpp`, and `tests/lit/fminus/field-attributes.fm` and
`field-attributes-errors.fm`.*

A host shows a program's state: a meter's level, an engine's speed, a filter's cutoff. Recipe 14 gave a parameter what
a host's control needs, a range and a default; a field, which a host reads for a display, a trace or an inspector, had
nothing, so a host that wanted a field's unit or label kept a table of its own beside the program, or read its
comments. C23 has a place for such things: an attribute after a declarator's name appertains to what it declares, and
an implementation ignores one it does not know. This recipe lets a field carry attributes, takes C's string literals
as their arguments, and passes the ones FMinus does not know to the host, which decides what they mean:

```c
module Meter
{
    param float decay [[fminus::range(0, 1)]] [[fminus::default(0.9)]] [[ui::label("Decay")]];
    float level [[ui::unit("dB"), ui::label("Level")]] [[fminus::range(-60, 0)]];
    float taps [[ui::hidden]] [16];
};
```

### Where they go: after the name

C23 has attribute specifiers in several places, and the place says what one appertains to: after a declarator's name,
the entity it declares; after an array's size, the array's type; at the start of a declaration, every entity it
declares. FMinus takes the first, where a parameter's have always been. So `float taps [[ui::hidden]] [16];` is the
field's, and `float taps[16] [[ui::hidden]]` is clang's error, "'ui::hidden' attribute cannot be applied to types",
with where the attribute goes; the start of a declaration is a syntax error. Each declarator has its own (`float a
[[x]], b;` gives `a` one and `b` none), and so do the names an enumeration declares after its list.

The declarator is a rule that locals, fields and constants share, and the attributes come in there. After the name, a
`[` may begin an attribute or an array's size, and Bison must decide at that token whether a rule ends. With a rule of
its own, `declared_name: pointer IDENTIFIER attributes`, and `declarator: declared_name array_sizes`, it must choose at
the `[` after an attribute between ending `declared_name` (a size follows) and reading another attribute: a
shift/reduce conflict. So each alternative of `declarator` spells out `pointer IDENTIFIER attributes` (`named` builds
the `Declarator`), and the two `[`s are shifts in one state, which the next token tells apart (`[[` is an attribute's,
anything else a size's). `attributes` may be empty, and its empty list is reduced before any `[`, on every path. After
the sizes, though, a `[` may be a third size's, so `declarator_sizes` reads attributes there as
`attribute_specifiers`, one or more, for `setSizes` to report.

An attribute's namespace and its name are identifiers, and C23 reads a keyword there as one (`[[ui::enum("a", "b")]]`,
`fminus::default`), so each is an `attribute_word`: `IDENTIFIER`, `MODULE_NAME` (the scanner makes a module's name a
type, so `[[meter::label("Peak")]]` in a program with a module `meter` is that module's name) or any keyword, spelt
out one by one, a vector type's too. A namespace is any of these, and the host's to choose, so a program's own names
must not take it from the host. A new keyword goes in `attribute_word` too: `FrontendTest.cpp` reads each of the
parser's tokens spelt as an identifier from its table (`Parser::symbol_name`) and checks it there.

### Strings

C23's attribute arguments are balanced tokens, anything at all. FMinus took numbers, and now takes strings too: C's
string literals, with C's escapes (the simple ones, `\n`, `\"`, `\\`; octal and hex bytes, `\101` and `\x41`; universal
character names, `µ`, in UTF-8), adjacent literals joined, `"Out" "put"` (`string_literal`). A literal is one
token, `STRING`, whose value is its characters: the scanner's `stringValue` resolves the escapes, and an escape that
is wrong is clang's error at the escape, "unknown escape sequence '\q'" (which clang only warns of), "hex escape
sequence out of range", "\x used with no following hex digits", "incomplete universal character name"; a line that
ends inside one is "missing terminating '"' character". FMinus has no `char` and no string type: a string where an
expression goes was a syntax error, "unexpected string literal", until recipe 38 made a literal an expression to the
parser, which the code generator takes only as a builtin's argument. A skipped group of an `#if` reads strings as strings,
as C does: a `/*` inside one opens no comment, which would swallow the `#endif`.

The tree keeps each argument as written, an `AttributeArg`: a number's spelling, or a string's characters with
`isString`. The dump writes a string back as a C literal (`stringLiteral`: a quote, a backslash and a control character
escaped), so `--emit ast` reads as the source does.

### What FMinus checks, and what it passes on

`checkAttributes` runs on each field as its module is laid out (`layOut`), a parameter's too:

| Attribute | Where | What FMinus does |
|---|---|---|
| `fminus::range(min, max)` | a parameter, or a field of numbers: `float`, `double`, `int`, a float vector, or an array of them | two numbers, the minimum below the maximum: what a host's control or display spans. FMinus neither clamps to it nor checks it |
| `fminus::default(value)` | a parameter | one number, inside the range: what `setDefaults` sets. On a field it is an error: a field starts at 0, and a method such as `init` sets it (since recipe 50, a field of numbers or `bool`s takes one too: what `json_read` sets the field to when a document does not fill it, which still does not initialise it) |
| another `fminus::` name | | "unknown attribute": the namespace is FMinus's |
| `clang::nonblocking`, `clang::nonallocating` | | "only applies to functions"; another `clang::` name is unknown, as it is on a function |
| a name without a namespace that is none of C23's standard attributes | | ignored, with clang's warning: "unknown attribute 'range' ignored [-Wunknown-attributes]", and a note, "did you mean 'fminus::range'?", for `range` (and `default` on a parameter). The host never sees it |
| any other | | the host's, kept as written. Its numbers must be numbers C writes ("invalid number '08'"; an integer too large for `unsigned long long`, which clang refuses, is none) |

The last row is C23's rule with one change: an implementation ignores an attribute it does not know, and FMinus,
which does not know `ui::unit`, passes it on to the host, which may. On a parameter, such an attribute was an error
before this recipe; now only an unknown name in FMinus's namespace is. C's standard attributes, `[[deprecated]]` and
the rest (`isIgnoredAttribute` has their names, `__deprecated__` too), go to the host as well, as FMinus gives them no
meaning; a host names its own in a namespace (`ui::`), since C keeps the names without one for its standard. Any other
name without a namespace is no one's, so FMinus warns of it as clang does and drops it (`attributesOf` in
`src/api/FMinus.cpp` asks `isIgnoredAttribute` too): `[[range(20, 20000)]]` is far more likely `fminus::range` without
its namespace than anything a host reads, and passed on quietly, it would leave the parameter with no range and no
message saying why. `attributeNumber` reads a number as C writes it, and learned a hex float's `f` suffix here
(`0x1p-3f`, which it refused before; in `0x1F` the `F` is a digit). An integer's digits must fit `unsigned long long`,
C's widest integer type: `strtoull` would saturate `99999999999999999999` and hand the host 2^64 - 1 beside its
spelling, so `errno`'s `ERANGE` makes it no number.

`checkAttributes` runs on a field once `layOut` has worked out its type: `fminus::range` then names the type as
declared (`'Osc[4]'`, not the tree's unsized `'Osc[0]'`), and on a field whose type was an error of its own (`void`,
an array of `bool`, `float x[]`) it reports nothing more, as recipe 32's rule has it. A parameter's runs first, as
its type needs no working out.

A local's attributes, and a constant's at the top level, have no host to go to: `refuseAttributes` reports the first,
"'ui::unit' attribute only applies to a module's fields and parameters". A `parallel for`'s counter is a local too,
which `lowerParallelFor` binds itself rather than through `declareVariable`, so it calls `refuseAttributes` as well. A
function's are as recipe 11 made them: `clang::nonblocking` and `clang::nonallocating`, any other an error.

The attributes change no code: the module's layout, its methods and its IR are the same as without them
(`FieldAttributeTest.cpp` compares the IR).

### The host's side

`Program::Module::Field::attributes` lists a field's in order, each a `Program::Attribute`: its `name` as written,
namespace and all (`"ui::unit"`), and its `arguments`, each an `Argument` with `isString`, `number` (a number's value,
0 for a string) and `text` (a string's bytes, UTF-8 as the source has them, or a number's spelling, `"-60"`). Two in
one `[[ ]]` are two entries, as two `[[ ]]`s are. A parameter's are its field's, and its `Parameter::attributes` too,
so a host that builds its dials from `parameters` finds a dial's unit beside its range.
`Program::findAttribute(attributes, name)` gives the last of a name, or null: the one that counts when a field says it
twice, as the last `fminus::range` does.

```cpp
using fminus::Program;
for (const Program::Module::Field& field : program->module("Meter")->fields)
{
    if (const Program::Attribute* unit = Program::findAttribute(field.attributes, "ui::unit"))
        show(field.name, unit->arguments.at(0).text);   // "level", "dB"
}
```

The meter example (`examples/meter/`) labels its two readings, `[[meter::label("Peak")]]`, and gives them a scale,
`[[fminus::range(0, 1)]]`; its harness reads both as a host would, and names the readings by their labels in its
report. The safety options and the conditionals leave a field's attributes alone: a program compiled with the loop
budget or bounds checks has the same, and a field in an `#ifdef DEBUG` group has its own when the host defines
`DEBUG`.

### What it changes for programs and hosts

- **Programs** without attributes on fields compile to the same IR. A parameter's attribute outside FMinus's and clang's
  namespaces, an error before, is the host's, or, without a namespace and none of C's standard ones, a warning. A `"` in
  code was an unexpected character, and now begins a string; in a skipped group, a `/*` inside a string no longer opens
  a comment.
- **Messages** are the ones above, in clang's words, and FMinus has a second warning, `-Wunknown-attributes`.
- **Hosts** get `Program::Attribute`, `findAttribute`, `Module::Field::attributes` and `Parameter::attributes`, all
  additions: every member that was there is as it was. The VS Code grammar (0.15.0) colours any namespace's attribute
  names, and strings with their escapes, an unknown one marked.

### Tests

- `tests/unit/FieldAttributeTest.cpp`: a module's IR the same with and without attributes, and its method's results;
  each error above, in clang's words, a `parallel for`'s counter's among them, and one error for a field whose type
  is its own error; the warning and its notes, and C's standard attributes with none; the numbers C writes, a hex
  float with its suffix and the widest integers among them, and those too large; `fminus::range` on each type of
  numbers; a string that is no number, even `"12"`; strings in an `#if`'s skipped group and in its kept one.
- `FrontendTest.cpp`: the dumps of fields' attributes (in each declarator, before the sizes, after an enumeration's
  list, a parameter's strings, adjacent literals), a local's and a constant's, and C's escapes written back; every
  keyword of the parser's, a vector type and a module's name as an attribute's namespace and name; the scanner's errors
  for strings, and the parser's for an attribute after a size and a string where an attribute's name goes.
- `tests/library/FieldAttributeTest.cpp`: `Module::Field::attributes` and `Parameter::attributes` as a host reads them
  (names, numbers with their values and spellings, strings with their escapes resolved and their UTF-8, two in one
  `[[ ]]`, none), `findAttribute`'s last and null, an ignored attribute in `warnings()` and in no list, and the same
  under the loop budget, bounds checks and `#ifdef`.
- `tests/lit/fminus/field-attributes.fm`: the dump, and the IR (at `-O0`, and at `-O2` with bounds checks and the loop
  budget) with no trace of them; `field-attributes-errors.fm`: each error once, the scanner's and the parser's, and
  the warning of a program that compiles.
- `example.meter` (`examples/meter/bench.cpp --check`): the readings' labels and scales, read as a host reads them.

## Recipe 36: calling a function by name, for the host

*In the compiler. Code: `CallType`, `CallSlot`, `Thunk`, `thunkName`, `callThunk` and `callType` (`src/jit/Call.h`,
`Call.cpp`); `Program::Value`, `Program::Callable`, `Program::CallResult` (and its `message`), `callable` and `call`
(`include/fminus/FMinus.h`), with `kindOf`, `describeCall`, `mismatch`, `slotOf` and `resultOf`
(`src/api/FMinus.cpp`); `--run`'s `RunArgument`, `readScalar` and `runPasses` (`tools/fminus/main.cpp`). Tests:
`tests/library/CallTest.cpp` and `tests/unit/CallThunkTest.cpp` (`[call]`), and `tests/lit/fminus/run.fm`.*

A host calls a JIT'd function through a C++ pointer of its type, `function<float(float, float)>("softclip", errors)`,
a type its own code names when it is built. Some hosts learn the type only as they run: a console that calls the
function its user names, with the arguments the user typed; a test runner that reads its cases from a file; a tool
that tries one of a program's functions on values it chooses, as a chat's assistant might. C++ calls a pointer only
through a type it compiled, so such a host has nothing to call it with, and a pointer of a guessed type that does not
match the declaration is undefined behaviour. `fminus --run` met the same problem and answered it with templates: one
instantiation of the call for each count of arguments up to 8, of floats or of doubles. That cannot grow to four
scalar types and pointers in any order (five types in each of 8 places is 390,625 parameter lists). This recipe compiles
the call instead, one small function for each function a host calls, and gives the host an API that checks its arguments
against the declaration and returns the result, or what went wrong, as a value:

```cpp
using Value = fminus::Program::Value;
std::string errors;
const fminus::Program::Callable* softclip = program->callable("softclip", errors);   // nullptr: errors says why
fminus::Program::CallResult result = program->call(*softclip, {Value::of(0.5f), Value::of(4.0f)});
if (result.ok())
    show(result.value.number);   // 0.6666667, the float it returned, as a double
else
    show(result.message());      // with Value::of(4): "argument 2 of 'softclip' is 'int', not 'float' (its
                                 // parameter 'drive')"
```

### The thunk: one C++ type for every signature

`jit::callThunk` compiles, for a function such as `float softclip(float x, float drive)`, a function of one C++ type,
`void(const CallSlot* arguments, CallSlot* result)`. A `CallSlot` is 8 bytes aligned to 8, with the value at its start
in its own type: a bool a byte, an int 4 bytes, a float 4, a double and a pointer 8. The thunk loads each argument from
its slot as the function's parameter type, calls the function, and stores the result in the result's slot. In outline,
in LLVM IR:

```llvm
define void @"fminus.call.softclip"(ptr noalias %arguments, ptr noalias %result) {
entry:
  %slot = getelementptr inbounds i8, ptr %arguments, i64 0
  %arg = load float, ptr %slot, align 8
  %slot1 = getelementptr inbounds i8, ptr %arguments, i64 8
  %arg2 = load float, ptr %slot1, align 8
  %0 = call float @softclip(float %arg, float %arg2)
  store float %0, ptr %result, align 8
  ret void
}
declare float @softclip(float, float)
```

The call is LLVM's to lower, so the target's calling convention is LLVM's to get right: AAPCS64 and Apple's variant of
it on arm64, System V's and Windows' on x86-64, which register each argument takes (System V and AAPCS64 count general
and vector registers apart, Windows by position), and which go on the stack once those run out (Apple's packs them by
size). A bool is the one type whose declaration says more than its IR type: the code generator declares one `zeroext`
(recipe 3), as clang does, so the thunk declares and calls the function the same way, and reads the byte in its slot as
`!= 0`, as C++ reads a bool, so a slot whose other bytes hold anything still passes 0 or 1. A bool result goes back as a
byte, 0 or 1.

The thunk is a module of its own (`thunkName`'s `fminus.call.` and the function's name, which no FMinus name can be),
added to the program's JIT beside the program's module, rather than a function in that module. So the program's code and
its IR are what they were: `optimisedIr()` is still the text `fminus --emit ll` prints, a host that never calls by name
compiles no thunk, and a host that calls one function pays for one thunk, the first time (1.5 to 2.5 ms on an M5, LLVM's
pipeline and code generator on a module of one function). `callThunk` looks nothing up itself: its callers look the
function up first, so that the function's own module is compiled and found, and the thunk's reference to it resolves
when the thunk links. A second thunk of one function in one JIT is LLVM's duplicate definition, which
`Program::callable` avoids by compiling each once, under the Program's lock, and keeping it.

`jit::callType` says how a thunk passes each of FMinus's types: `Void` (a result only), `Bool`, `Int`, `Float`,
`Double` and `Pointer`, an enumerated type's as its `int` or `bool`, and nothing for a vector, a `poly` type, a module
or an array. Its `switch` has no `default:`, so a new `Kind` stops the build there until it says how it crosses, the
role the driver's `switch` over `--run`'s result type had (step 7 of the table above).

### What a host sees: `Value`, `Callable`, `CallResult`

A `Program::Value` is an argument or a result: a `kind` (`Void`, `Bool`, `Int`, `Float`, `Double` or `Pointer`), a
`number` for a scalar (a double holds each of the four exactly), and for a pointer its address, its elements' kind, the
module whose instances it points to, and whether the function may only read through it. The host makes one with
`Value::of(0.5f)`, `of(2)`, `of(true)` or `of(0.5)`, `Value::array(buffer)` (an `int*`, `float*` or `double*`, or its
`const` one) and `Value::instance(memory, module)`; `of` of a pointer is deleted, since C++ would otherwise convert it
to a bool.

`callable(name, errors)` gives the function as a `Callable`: its `name`, its module if it is a method, its `result` and
`resultType`, and each parameter's `name`, `type` as FMinus writes it (`"const float *"`), `kind`, and for a pointer its
`element`, `module` and `readOnly`. `compile()` describes every function the program declares (`describeCall`), and
`callable()` compiles the thunk the first time it is asked for one. The `Callable` is the Program's own, in its map, and
`callable()` gives a pointer to it: it cannot be copied or moved, so the one `call()` is given is the one `callable()`
described, not a copy whose `parameters` a host edited (a method's `this` dropped, say, which the thunk would still load
from the first slot), nor a copy kept past its Program, which a new Program whose `Impl` took the old one's address
would take for its own. A host keeps the pointer while the Program lives, as it keeps a function's (one kept longer
dangles, as a function's does), and copies the fields it shows. One it makes itself is no Program's, and `call()`
refuses it. A function that takes or returns a type no thunk passes, or a pointer to a vector or to rows, or that
returns a pointer, has no `Callable`, and the error says which type ("'broadcast' returns 'float4'; call() returns
nothing, or a bool, an int, a float or a double"). Nor does a function the program only declares,
`float atanf(float x);`: it is the C library's, not the program's, and LLJIT's `lookup` looks in the program's own
`JITDylib`, not in the process its prototypes link to. A function the program defines whose code does not link has
none either: `compile()` links nothing unless a safety option needs its code, so
`float g(float x) { return nowhere(x); }`, beside a prototype of `nowhere` that nothing defines, compiles, and its
look-up fails ("'g' cannot be called: Failed to materialize symbols: ..."). `call(name, ...)` tells these apart by the
function, not by the message: `NoFunction` for a name the program does not define, `Unsupported` for a type,
`LinkError` for a definition that did not link.

`call(callable, arguments)` checks the arguments (`mismatch`), packs them into slots (`slotOf`: on the stack, past 16 on
the heap), calls the thunk, and returns a `CallResult`: its `status`, the `value` the function returned, the `error`,
one line in FMinus's words of why it did not run, and after a failed bounds check the `violation` it met. `message()`
words any status, the `error` or what stopped the call, and is empty when the status is `Ok`. `call(name, arguments)`
looks the `Callable` up first.

### What `call()` checks

| The host passes | For a parameter of type | What `call()` does |
|---|---|---|
| the wrong number of arguments | | `Mismatch`: "'softclip' takes 2 arguments, not 1" |
| a `Value` of another kind: `of(2)` | `float` | `Mismatch`: "argument 2 of 'softclip' is 'int', not 'float' (its parameter 'drive')". `call()` converts nothing: a host whose numbers are all doubles (a JSON tool's) converts each with `Value::scalar(param.kind, number)`, as `setValue` converts a parameter's value |
| `array(const float*)` | `float *` | `Mismatch`: "is 'const float *', not 'float *'", C's "discards qualifiers". A `float*` for a `const float *` is fine |
| `array(int*)` | `float *` | `Mismatch`: "is 'int *', not 'float *'" |
| any pointer | `void *`, `const void *` | taken, as C converts it (a `const` one only for a `const void *`) |
| any pointer but a `const` one | a handle, `json_doc` and the rest (recipe 44) | taken, as for a `void *` |
| `instance(memory, module)`, `module` this Program's `Osc` (`program->module("Osc")`) | `Osc *` | taken: the module is the host's word for how the memory is laid out, which `call()` cannot see |
| an instance of another Program's `Osc`, of the same source or not | `Osc *` | `Mismatch`: "is an instance of another program's module 'Osc'". Its own fields may match this one's while a module it holds by value, in an array or through a pointer does not (two `Line`s of one size, their fields swapped), and comparing layouts deeply would be a second definition of a module's type; a host that keeps an instance across a recompile, knowing its layout is the same, passes it with the new Program's `Module` |
| a null instance | a method's `this` | `Mismatch`: "is a null 'Osc *': a method runs on an instance". Another pointer may be null, as in C |
| a pointer not aligned to its elements (4 bytes for an `int` or a `float`, 8 for a `double`, the module's alignment for an instance) | | `Mismatch`: "is not aligned to 4 bytes, as a 'float *' must be". The function's loads assume the alignment, as C's do |
| a `Callable` of another Program's, or one the host made | | `Mismatch`: "'softclip' is not a Callable of this program's: callable() gives them". A copy does not compile |

An extent is not checked, nor passed: C passes none, a parameter written `float x[16]` is a `float*` whose size C
checks and then ignores, and bounds checks see no host's buffer (recipe 30). A function that is passed a buffer is
passed its length, as C's are; the host makes the buffer that long.

### Where the call runs

`call()` runs the function on the calling thread, any thread, and several threads may call at once: the thunk is a
plain call, and what `call()` adds is a compare for each argument, the slots, and two loads after the call (the bounds
record and the loop budget's flag). On an M5, with other builds running, a call of `softclip` through its `Callable`
took about 15 ns, against 1 ns through its pointer, and `call(name, ...)`, which finds the `Callable` in a map each
time, about 28 ns. It takes no lock, and allocates only for more than 16 arguments, or when the host's call is wrong
(a `Mismatch`'s message; `call(name, ...)`'s failures too, as `callable()` allocates). A call the loop budget or a
bounds check stops allocates nothing, as it may be the audio thread's: its `error` is empty, the result keeps the
`violation` (whose strings are the Program's), and `message()` words it later, on another thread. `callable()` takes
the Program's lock and allocates, and compiles the thunk the first time, so it runs off the audio thread, as a
function's first look-up does.

A method runs on an instance, its `this`: an instance the host made as `Module` says (zeroed, aligned, its parameters
set with `setDefaults` and `setValue`), and what the method changes stays in it for the next call, by name or through
a pointer. Two threads that call methods at once give each its own instance, as with any C++ call. A function that is
no method runs on its arguments and what they point to: FMinus has no variables outside functions but its constants.

### The safety options

A call by name is a call: in a program compiled with the loop budget it counts against the budget, and one that runs
out returns early, `Status::OutOfBudget`, with `loopBudgetExceeded()` set and the value it returned early (0); with
bounds checks, a check that fails stops it, `Status::OutOfBounds`, with the record's check as the result's `violation`,
which `message()` words ("'tap' stopped: program.fm:17:16: index 4 out of bounds for 'taps' of type 'float[4]'"), a 2-D
array's dimension by dimension. The bounds record is read first, since a check that failed is why a call that also ran
out stopped where it did. Each status is the Program's state after the call, so a host sets the budget and clears the
record before each call, as for a call through a pointer, and re-initialises an instance a stopped call ran on.

The budget and the record are the Program's, one of each, which every thread's calls share, so a status is the call's
own only when no other call of the Program overlaps it. Two threads calling at once share one failure: the call that
ran whole returns the other's `OutOfBounds` (and a call that returns after another's failed check returns early too,
the bounds checks' rule), and one thread's `setLoopBudget` or `clearBoundsViolation` is the other's too, which may
clear a record before the call that set it reads it. A host that calls a program compiled with either option from
several threads serialises those calls, or reads the statuses as the program's; without the options there is no
shared state, and calls overlap freely.

### `--run`

`fminus --run` calls through the same thunk (`callThunk`, after its own look-up of the function), so it passes what
`call()` passes from a command line: each argument read as its parameter's type, as CLI11 reads one (`true`, `0`, `-3`,
`0.5`), in any mix and number, and for a pointer to an int, a float or a double an array, its elements separated by
commas (`1,-2,0.5`), which it prints after the call when the function may have written to it:

```bash
$ fminus run.fm --run scale 1,-2,0.5 3 2
x = {2, -4, 1}
```

A function of no result runs and prints its arrays. A module's instance is a host's to make, so `--run` refuses an
`Osc *` parameter, and a method; "--run: '1.5' is not an int" and "'x' is not a float, an element of the array '1,x'"
say which argument it could not read. What it prints for a float or a double is as before, the shortest decimal that
reads back as it.

### What it changes for programs and hosts

- **Programs** compile to the same IR and the same code. The thunks are modules of their own, made only when a host
  calls by name.
- **Messages**: `--run` passes ints, bools, mixed types, more than 8 arguments and arrays, so its refusals of them are
  gone; its refusals of the types it does not pass name the ones it does ("--run passes bools, ints, floats, doubles
  and arrays of ints, floats and doubles only"; "--run prints bools, ints, floats and doubles only").
- **Hosts** get `Program::Value`, `Callable`, `CallResult`, `callable()` and `call()`, all additions: every member that
  was there is as it was.

### Tests

- `tests/library/CallTest.cpp`: each scalar type and a mix, bit for bit as the function's own pointer computes them;
  twenty arguments of the four types (on the stack, and in heap slots); arrays written and read, `void*` taking any
  pointer, and a `const` array, another element type and a misaligned pointer refused; a method on the host's instance,
  which keeps its state and reads a knob the host sets, through a function's `Osc *` too, a null instance, another
  program's refused, of the same source too, and taken with this program's `Module`; another program's `Delay` whose
  fields match but whose `Line` has its fields swapped, refused; the `Callable`s' descriptions; each error as a status
  and a message (no function, a builtin, a prototype alone, a definition that does not link, a vector, a pointer result,
  a pointer to a vector, the count, a type, a `Callable` of another program's or the host's own, and none copied, at
  compile time); `Value::scalar`'s conversions; the loop budget and bounds checks stopping a call, with no message until
  `message()` words it (a double in a budgeted loop, a 2-D array's dimension), and a call after the record is cleared;
  four threads calling one `Callable` on instances of their own, and asking `callable()` for it at once; the IR kept as
  text unchanged by a call, and an `#ifdef`'s function.
- `tests/unit/CallThunkTest.cpp`: each type in its slot and back, a bool's byte read as C++ reads one whatever its
  other bytes hold, a bool result as 0 or 1, twelve arguments past the vector registers, a second thunk's error, and
  `callType`'s map.
- `tests/lit/fminus/run.fm`: `--run` of a mix of a float, an int and a bool, of arrays written (printed) and read (not
  printed), of a function of no result, of twelve arguments, with bounds checks and the loop budget, and its refusals;
  `int.fm`, `double.fm` and `main.fm`: `--run` of ints, of a double beside a float, and of a bool, which it refused
  before; `simd.fm` and `poly.fm`: its refusals of vectors, reworded.

## Recipe 37: where the code is, for a sampling profiler

*In the compiler. Code: `Jit::Code`, `code`, `Calls`, `directCalls`, `Inlined`, `inlinedCalls`, `recordInlining` and
`Linker`, with `CodeLog`, `keepNamesOfKeptCode`, `LinkedCode`, `LoadedCode` and `callersFirst` (`src/jit/Jit.h`,
`Jit.cpp`);
`Program::CodeRange`, `codeRanges` and `codeRangeAt` (`include/fminus/FMinus.h`), with `attributed`
(`src/api/FMinus.cpp`). Tests:
`tests/unit/CodeRangesTest.cpp` and `tests/library/ProfileTest.cpp` (`[profile]`).*

A program that takes too long on the audio thread does not say where the time goes. A host can find out without
changing the program: a sampling profiler interrupts the thread that runs it a few thousand times a second, reads
where it was, its program counter, and counts the samples that fall in each function. The program runs as it always
does, with nothing added to its code, and the counts are the share of its time each function takes. What such a
profiler lacks for JIT'd code is the map from an address to a function: the code has no file on disk and no symbol
table the system's tools read (`dladdr` finds nothing in it), and its addresses change each time the program compiles.
This recipe writes the map down as the JIT links each object, and gives it to the host with each function's module and
what the optimiser inlined into it:

```cpp
std::string errors;
std::vector<fminus::Program::CodeRange> ranges = program->codeRanges(errors);   // after compile(), off the audio thread
...
if (const fminus::Program::CodeRange* range = fminus::Program::codeRangeAt(ranges, pc))   // each sample, on the
    ++samples[range->function];                                                         // sampler's thread
```

### Where the linker puts the code

LLJIT compiles a module to an object file in memory, then links it into memory of its own with one of two linkers,
which it picks by the target (`LLJITBuilderState::prepareForConstruction`). JITLink, on macOS and Linux, turns the
object into a graph of blocks and symbols, gives them addresses, applies the fix-ups, and runs the passes a plugin
adds at each step. `LinkedCode` is such a plugin: its pass runs after the fix-ups, when every symbol has its final
address, and writes down each symbol with a name in an executable section, its address and its size. It adds a pass
before the graph is pruned too. Pruning removes each symbol that is not live and keeps each block a live symbol
reaches, and an ELF object's call of a local function (a parallel region's body, an entry point's) names its section's
symbol and an offset, `.text + 0x1c0`, not the function's: the function's code stays, in the section's block, but its
name would go (on Linux the first run of the tests found `run`, and not `run.parallel.1` or `fminus.large.sin`).
`keepNamesOfKeptCode` walks the blocks the live symbols reach, as the pruning does, and marks live each named symbol of
an executable section in one of them, so nothing is linked that would not be; a Mach-O object's calls name their
functions, and there it changes nothing. The link can
still fail after that pass (finalising the memory), so its list waits, keyed by the link's
`MaterializationResponsibility`, until the plugin hears that the link was emitted (`notifyEmitted`), and is dropped if
it failed. RuntimeDyld, Windows' (LLJIT picks it for COFF), runs no passes; it tells a `JITEventListener` of each object
it has loaded, with the object and where it put each section. `LoadedCode` reads the object's symbol table, its
functions, sizes them with `object::computeSymbolSizes`, and places each at its section's load address plus its offset
in the section. Both write into a `CodeLog` the `Jit` shares with them, and `Jit::code()` copies it, sorted by address.

The object's names are the linker's, with the target's global prefix (macOS's `_`: `_Osc::tick`). `code()` gives the
module's, without it, and drops the symbols that are no function of the module: the assembler's labels (macOS's
`ltmp0` at the start of each section, which has no prefix), ELF's local labels (`.L`) and AArch64 ELF's mapping
symbols (`$x`). The sizes are the object's: an ELF object gives each function's (`st_size`), and a Mach-O or COFF object
none, so each runs to the next symbol, the padding before it included (what the next function's alignment leaves, if
anything). No program counter lands in padding, so a sample finds its function either way.

The tests check both linkers on every system: `Jit::create` takes a `Linker`, and `Linker::RuntimeDyld` gives the JIT
RuntimeDyld wherever it runs, made as LLJIT makes Windows' (its `createObjectLinkingLayer`, with COFF's two flags when
the target is COFF), so the path Windows takes runs on macOS and Linux too. The two do not link quite the same code:
LLJIT gives JITLink the small code model, and leaves RuntimeDyld the target's for a JIT, the large one on x86-64 and on
AArch64 but Windows', in which code that names an address (a global's, a constant's, a function's) is not the same. On
an M5, `run`, which passes its parallel region's address to the pool, is 8 bytes longer. Where the code is the same, the
two lists give each function the same place and size, but the last, which RuntimeDyld runs to the end of its section;
the tests check that with functions that name no address.

### What the optimiser did: `inlined`

A range is the machine code's, not the source's. From `-O1` the optimiser inlines a small function into its caller,
`Osc::tick` into `Osc::process`'s loop, so the samples of tick's work fall in process's code. FMinus emits no debug
information, which would say of each instruction which inlined call it came from (a `DILocation`'s `inlinedAt`); a
profile by source line waits for it. What can be said cheaply is which functions a range holds. `Jit::directCalls`
lists, for each function a module defines, the functions of the module it calls by name, and how many calls of each it
makes. The JIT reads them before the optimiser and after (its IR transform, around `optimiseModule`), and `inlinedCalls`
compares the two. Had the optimiser inlined nothing, F would call G after as often as before, and as often again as each
function inlined into F called G; each of those calls missing after is inlined into F, and brings G's own calls with it.
So G is listed when F calls it fewer times than that, even if F still calls it at another call:
`out[i] = g(out[i], 0) + g(out[i], mode)` inlines g where its mode is 0, whose path is short, and calls it where the
mode is the caller's. The walk takes each function after all that call it on the way from F (a reverse post-order, which
a recursion's call back up does not wait for), so its count is whole when it is read. A callee each of whose calls F
still makes keeps its code, and the walk goes no further through it. A call the optimiser deleted as dead (behind a
frozen `bool`, say), or merged with another, looks the same as one it inlined, and is listed too; one it copied,
unrolling a loop, can hide one it inlined. A program compiled with `keepIr` is optimised by `Program::compile` itself,
which reads the calls around its own `optimiseModule` and hands the JIT their `inlinedCalls` (`recordInlining`). On an
M5 the two reads and the walk take under 0.1 ms for a circuit's module of 3,300 instructions
(`examples/circuits/screamer.cir`), against 30 to 50 ms of `-O2`'s pipeline.

An inlined function keeps its own code too, as every function of the program is external (the host may call it), so
`Osc::tick`'s range is still there; it takes samples only when something calls it, and nothing in the program then
does. At `-O0` nothing is inlined, and each function's samples are its own: a host that wants to see the time a
function's own work takes profiles a build at `-O0` beside the one that plays (the shares differ from it, as `-O0`'s
code is slower in its own ways).

### Which function a symbol is

The code generator and the JIT name what they make after the function it belongs to, with a `.`, which no FMinus name
has, before what it is:

| Symbol | `kind` | `function` |
|---|---|---|
| `Osc::tick` | `Function` | `Osc::tick`, and `module` `Osc` |
| `process`, when there is a `process.body` | `EntryPoint`: under `Denormals::Flush`, an entry point is a few instructions that set the mode around a call of its body and hand the caller's back (recipe 20) | `process` |
| `process.body` | `Function` | `process` |
| `run.parallel.1` | `Parallel`: a region's body, which the pool's threads and the caller run (recipe 22) | `run` |
| `fminus.call.softclip` | `Thunk`: what `call()` calls the function through (recipe 36) | `softclip` |
| `fminus.large.sin` | `Support`: FMinus's own, a math kernel's path for an argument past its range (recipe 23) | none |

`attributed` reads a symbol so; `codeRanges` adds a method's `module` (from its `Callable`), and reads each name of
`inlined` the same way, listing each of the program's functions once, never the range's own.

### What a host does with it

`codeRanges(errors)` links the program first, if no function has been looked up yet (a look-up of one of its functions
links its whole module, so it looks up the first the program defines), so the list after `compile()` is whole; a thunk
joins it when `callable()` makes it. A program whose code does not link has none, and `errors` says why (LLVM's
words, as for `call()`'s `LinkError`). The list is the host's copy, sorted by address, the ranges apart. The JIT neither
moves nor frees what it has linked, so the code stays where the list says while the Program lives: the host takes the
list once, off the audio thread, and its sampler reads it on its own thread with `codeRangeAt`, a binary search with no
allocation and no lock.

Sampling is the host's: FMinus takes no samples itself. `tests/library/ProfileTest.cpp` has a reference sampler for
macOS and Linux. A thread sends the thread that runs the program `SIGPROF` every 200 µs (`pthread_kill`), and the
handler stores the program counter the kernel saved in its context (`uc_mcontext`: on macOS
`__darwin_arm_thread_state64_get_pc`, which also undoes arm64e's pointer authentication, or `__rip`; on Linux
`gregs[REG_RIP]` or `pc`) into memory set aside before, as a signal handler may neither allocate nor lock. When it has
400, it stops, and the samples are mapped and counted. On Windows a sampler suspends the thread instead
(`SuspendThread`, then `GetThreadContext`'s `Rip`), as macOS's `thread_get_state` can, and the test does not run there.
ThreadSanitizer holds a signal back until the thread next runs instrumented code, which the JIT's never is, so the test
does not run under it either. What it found on an M5, in six runs of 400 samples, for `spin`, which calls `Work::slow`
(a loop of 3n iterations) and `Work::fast` (the same loop, n), each call longer than the time between two samples, so
that the samples do not fall in step with the calls:

| | `-O0` | `-O2` |
|---|---|---|
| `Work::slow` | 72–75% | 0 |
| `Work::fast` | 25–28% | 0 |
| `spin` | 0 | 99.75–100%, its range's `inlined` `Work::fast` and `Work::slow` |

All but one sample fell in the program's code (the one, at `-O2`, in the test's own loop around `spin`). Code that is
not the program's has no range, and `codeRangeAt` gives null for it: the runtime's (the pool, an FFT, `fminus_rt_sinf`
where a math kernel is called rather than inlined), the C library's (`Math::Libm`'s `sinf`, a prototype's function)
and the host's own. A host names those from its own symbols (`dladdr`), or charges them to the program's function that
called them by walking the stack. A parallel region runs on the pool's threads as well as the caller's, so a sampler of
the calling thread sees its share of the region only.

### What it changes for programs and hosts

- **Programs** compile to the same code. The JIT writes down each function's name, address and size as it links, and
  reads a module's calls twice around the optimiser.
- **Hosts** get `Program::CodeRange`, `codeRanges()` and `codeRangeAt()`, all additions: every member that was there is
  as it was.
- **The JIT** (`jit/Jit.h`) gets `code()`, `directCalls`, `inlinedCalls`, `recordInlining`, and a `Linker` for
  `create`, whose default is LLJIT's choice, as before.

### Tests

- `tests/unit/CodeRangesTest.cpp`: through JITLink and RuntimeDyld, with and without `Denormals::Flush`, each function's
  code at the address its look-up gives, sized, sorted and apart, nothing before a look-up, no global prefix and no
  assembler's label, an entry point and its body, a parallel region's body and FMinus's own `fminus.large.sin`;
  functions that name no address, placed and sized alike by both linkers, the last at least as large through
  RuntimeDyld; what `-O2` inlined (tick into process, both into run, `shape` into the parallel region) and `-O0`
  nothing; a method inlined at one call and called at another, listed in its caller's code; `inlinedCalls`'s walk
  (through a callee, stopping at a kept one, recursion, a function the optimiser removed) and its counts (a callee
  inlined at one of two calls, a call copied, a diamond, counts past a `size_t`'s range); a thunk's code once it is
  made.
- `tests/library/ProfileTest.cpp`: each range's kind, function and module, at the address `address()` gives, the list
  whole after `compile()` with no look-up, `codeRangeAt` inside, at the edges and outside every range, the same ranges
  again with a thunk's added after `callable()`; the same inlining when `compile()` optimises (`keepIr`), and none at
  `-O0`; a program whose code does not link, and one with no function; and the reference sampler at `-O0` and `-O2`.

## Recipe 38: string literals as builtins' arguments, bytes to numbers, and the build options of builtins

*In the compiler. Code: `StringExpr` (`src/frontend/Ast.h`, its dump in `Ast.cpp`); `StringToken`, `appendString`
and `string_literal` as an expression (`Parser.y`), `stringValue`'s offsets (`Lexer.l`); `src/ir/Strings.cpp`
(`scanFormat`, `stringConstant`, `formatArguments`, `refuseString`, `notAValue`); `Feature`, `isBuilt`, `notBuilt`,
`RuntimeEffect`, `Param`, `RuntimeBuiltin`, `kRuntimeBuiltins`, `findRuntimeBuiltin`, `blocks` and
`lowerRuntimeBuiltin` (`src/ir/Lowering.h`, `Calls.cpp`), `unsafe` (`Effects.cpp`), a prototype of one (`declare`,
`Functions.cpp`); `predefinedMacros` and `predefinedMacroError` (`src/frontend/Preprocessor.h`, `.cpp`, with
`macroNameError` and `#undef` in `Lexer.l`, and `parse` in `Frontend.cpp`); `src/runtime/Bytes.cpp`, `Format.h`,
`Numbers.h` and `Numbers.cpp`; `FMINUS_JSON` and `FMINUS_XML` (`cmake/FMinusOptions.cmake`), `FMINUS_HAS_JSON` and
`FMINUS_HAS_XML` (`cmake/templates/Config.h.in`), `cmake/FMinusFastFloat.cmake` and its pin. Tests:
`tests/unit/StringTest.cpp` (`[strings]`), `BytesTest.cpp` (`[bytes]`), `NumbersTest.cpp` (`[numbers]`),
`ConditionalTest.cpp` and `FrontendTest.cpp`, and `tests/lit/fminus/strings.fm`, `strings-errors.fm` and `bytes.fm`.
Built with `-Werror` and tested in the `release` preset on Linux x86-64 only, every test passing but the one the
machine it was written on cannot run (the smoke test of LLVM's own `opt`, which failed there before this recipe too);
not yet in `dev` or `asan`, nor on macOS or Windows.*

A program that loads an impulse response from a server, or reads a preset's cutoff from a JSON file, needs to say
which: a URL, a key, a path. FMinus has no strings, and [research/web/README.md](research/web/README.md) weighs four
ways to give it enough of them; it recommends the smallest, which this recipe builds: a string literal is legal as the
argument of a builtin that takes one, and nowhere else. It is the step everything in that report stands on, and it
comes with the scaffolding the parsers will need: a format with places for ints (`"/osc/%d/detune"`), checked as clang
checks `printf`'s; a third class for the effects, a builtin that blocks; the build options that decide whether a
build has the JSON and XML parsers, with what a host and a program see of them; and fast_float, the parsers' reader of
numbers. No JSON or XML builtin came with it (recipe 39 adds JSON's, and recipe 40 XML's). Two builtins that need no
parser prove the way from the source to the runtime and back, and are what the report wants for a payload a program's own server sends:

```c
module Loader
{
    float ir[65536];
    int length;

    // data: `bytes` bytes the host hands over (a file's, a response's): little-endian float32s
    void load(const void* data, int bytes) { length = bytes_floats(data, bytes, ir, 65536); }
};
```

### A literal is an expression to the parser

Recipe 35 gave the scanner C's string literals, a `STRING` token whose value is the literal's characters, its escapes
resolved. The parser took them only as an attribute's arguments, so a string anywhere else was "unexpected string
literal". Now `expr` takes one too, `string_literal`, the same rule an attribute's argument reads (adjacent literals
are one), as C's grammar does: the grammar keeps no new conflict (Bison reports none), and every program it accepts is
C's. The tree's node, `StringExpr`, holds the bytes and where each one is spelt: the scanner's token carries each
byte's offset in the token (`StringToken`, from `stringValue`), at its character or at its escape's backslash, and
`appendString` turns them into locations, so that an error about a byte of a format, three literals and two lines
later, is at that byte. A NUL is a byte like any other: `"a\0b"` is three.

### Where a literal may go: a builtin's string parameter

The code generator decides. FMinus has no type for a literal as C has it (C's is a `char` array), so a `StringExpr` is
a value nowhere (but a `string` where one is wanted, since recipe 46): `lower()` reports one wherever it goes, "FMinus
has no char arrays here: a string literal is a 'string' where one is wanted, or the argument of a builtin's string
parameter" (before recipe 46, "FMinus has no strings: ..."), at the literal. Where C converts a char array with a warning, or refuses it, the error is
clang's, where clang reports it (`refuseString`, given the place): an argument of the program's function or a
builtin's numeric parameter (a math function's float, a size's, a plan's length's, `hash`'s and `randf`'s int), an
initialiser, an assignment and a `return` know the type they want, so `float x = "dB";` is "initializing 'float' with
an expression of incompatible type 'char[3]'" at the declarator's `x`, `y = "a"` "assigning to ..." at the `=`,
`k("x")` of an `int` parameter "incompatible pointer to integer conversion passing 'char[2]' to parameter of type
'int'" at the literal, and a `float*` parameter's "incompatible pointer types ...". A subscript is clang's "array
subscript is not an integer", at the `[`, and a literal for a format's `%d` its "format specifies type 'int' but the
argument has type 'char *'". After the literal's error, a builtin whose argument it was says nothing more
(`reduce_add("a")`, `fft_forward(plan, "a", out)`): the argument's error says why, as with every failed value.
Where C would take the array silently (as a `void*`, a `bool`, in a condition or an operand), clang has no words to
borrow, and FMinus's say what it lacks. In an integer constant expression, a case label's, a literal is "expression is
not an integer constant expression", as in C.

A builtin that takes a literal says so in its table, `kRuntimeBuiltins` (`Calls.cpp`): each parameter is a `Param`, an
`int`, bytes (a `const void*`, which any pointer converts to), floats it writes (a `float*`), a string or a format. A
call of one is lowered before the generic path (after a method of the module of the same name, which comes first, as
it does for every call), so its arguments are lowered as its parameters say. A string or a format must be a literal,
"argument to 'name' must be a string literal" (clang's words for a builtin's constant argument), and becomes
`stringConstant`'s private constant, its bytes and no NUL after them, as clang's `.str` is but for the NUL, passed as
its address and its length in bytes:

```llvm
@.str = private unnamed_addr constant [14 x i8] c"/osc/%d/detune", align 1
  %format.ints = alloca [1 x i32], align 4
  store i32 %k1, ptr %0, align 4, !tbaa !2
  %__fminus_string_hash = call i32 @fminus_rt_string_hash(ptr @.str, i32 14, ptr %format.ints, i32 1)
```

So the runtime never looks for an end (no `strlen`, and a NUL inside is data), and a program that does this means what
it would in C, where the builtin would be a function of a `const char*` and a length. A prototype of a builtin with
its own types, `int bytes_floats(const void* data, int bytes, float* out, int cap);`, declares nothing new, as
`<math.h>`'s do; one of a builtin that takes a string cannot be written, since FMinus has no `char`, and is "conflicting
types". A definition is refused, as any builtin's is.

When this recipe was written no builtin of the language took a string (recipe 39's `json_` builtins do now). Two
names C reserves for the implementation, `__fminus_string_hash(format, ...)` and `__fminus_string_hash_blocking`, are
scaffolding: the 32-bit FNV-1a hash of the text a format and its ints make, which the tests compute again in C++, and
so prove every byte's way to the runtime (escapes, adjacent literals, a NUL, a place for an int that spans two
literals). They stay for now: the tests of formats and of the blocking class are written against them, and nothing
else blocks yet (`http_wait` will).

### Formats: places for ints, checked as clang checks `printf`'s

With literals only, a program still needs `"/osc/2/detune"` in a loop over oscillators, or preset 17's URL. The C way
that needs no string type is a format with `int` arguments: `Param::Format` is a literal with places for ints, and the
call gives an `int` for each place after it. `scanFormat` (`Strings.cpp`) is the one check every family of builtins
will use: `%d` is a place, `%%` a percent sign, and anything else after a `%` an error, in clang's words for `printf`'s
and at the byte clang's caret is on (`formatArguments` turns an offset into its location). It reads a specifier as
clang does (`ParsePrintfSpecifier`): flags, a width, a precision and a length modifier (`h hh l ll q j z t L I I32 I64
w`), then the conversion:

| Mistake | Error, at |
|---|---|
| `"%y"`, `"%5y"`, `"%lq"` | "invalid conversion specifier 'y'" (or `'q'`): no such conversion in C, clang's words at the conversion, with a note: "a format here takes %d, an int, and %%, a percent sign" |
| `"%f"`, `"%5d"`, `"%ld"`, `"%s"` | "invalid conversion specifier 'f'" (`'5d'`, `'ld'`, `'s'`), at the byte after the `%`, and the note: C's specifiers that are not a plain `%d`, which clang takes and FMinus has no meaning for, in clang's words for one it would not take |
| `"ab%"`, `"%q"`, `"%5"`, `"%l"` | "incomplete format specifier", at the `%`: the text ends before a conversion (`q` is a length modifier, BSD's `long long`) |
| fewer ints than places | "more '%' conversions than data arguments", at the `d` of the first place without one |
| more ints than places | "data argument not used by format string", at the first one left over (none when the format had an error: clang counts an int an invalid place was to take; nor when one left over has an error of its own, which is its one error). One left over is no type error, whatever it is: no place reads it |
| a float, a pointer, a literal | "format specifies type 'int' but the argument has type 'float'" (`'char *'` for a literal), at the argument |

An `int`, an enumeration's value and a `bool` (0 or 1) are what `%d` reads, as C promotes them for a variadic
argument; FMinus converts nothing else, where C would pass a `float` as a `double` and leave `%d` undefined. The ints
go in an array of the function's frame (tagged as ints, for TBAA), with their count, and the runtime gets the literal
as written: it makes the text itself, `fminus::runtime::expandFormat` (`Format.h`), which trusts no byte of it (a `%d`
past the last int is nothing). The host sees the pattern, `"https://example.com/presets/%d.json"`, which is what the
report's permissions need (its §8). A family whose builtins want the path prepared when the program is compiled (the
report's §6.4) builds its own table from `scanFormat`'s places, as JSON's does (recipe 39, `scanPath`).

### `bytes_floats` and `bytes_pcm16`

`bytes_floats(data, bytes, out, cap)` reads `bytes` bytes at `data` as little-endian IEEE float32s, and
`bytes_pcm16(data, bytes, out, cap)` as little-endian signed 16-bit samples, each times 1/32768 (exact: a 16-bit
integer and a power of two), to [-1, 1). Each returns how many values the data holds (`bytes / 4`, or `/ 2`: a partial
value at the end is none) and writes the first of them, at most `cap`, so a call with a cap of 0 asks how many there
are. A null `data` or a negative size is no data, and a null `out` or a cap that is not positive takes none. Each value
is assembled from its bytes one by one (`runtime/Bytes.cpp`), so the data needs no alignment and the machine's byte
order does not matter, and a float's bits are copied as they are, a NaN's payload too.

Each is a loop bounded by `cap` over memory it is given: no allocation, no lock, no system call, so the effects class
them wait-free and a nonblocking function may call them. Their declarations say what is true of `Bytes.cpp`
(`kRuntimeFacts`, `RuntimeKind::Decode`, recipe 19): `memory(argmem: readwrite) nofree nosync nounwind willreturn`,
`data` `readonly captures(none)`, `out` `writeonly captures(none)`. Neither is `nonnull` (both may be null) nor
`noalias`: a program may decode in place, `out` at `data`'s own address. `bytes_floats` takes four bytes to four, so
it goes first to last; `bytes_pcm16` takes two bytes to four, so it writes its last value first, and every sample is
still there when its turn comes (`BytesTest.cpp`, "decode in place", calls both on a buffer of their own). The format scaffolding's (`RuntimeKind::ReadText`) reads its two pointers
and nothing else: `memory(argmem: read)`. Bounds checks (recipe 30) do not look into a call's output, as they do not
into the FFT's: `cap` is the program's promise.

### The effects' third class: a builtin that blocks

`Effects.cpp` had two classes of builtin: one that allocates (`allocates(name)`, refused in a nonallocating function
and so in a nonblocking one) and every other, trusted, as clang trusts its own. A web library has a third: `http_wait`
waits for a response, which a nonblocking function may not do, and allocates nothing, which a nonallocating one may.
Each runtime builtin now names its class, `RuntimeEffect::WaitFree`, `Blocks` or `Allocates`, and `unsafe` asks
`blocks(name)` after `allocates(name)`: a call of one that blocks is an error in a nonblocking function, in clang's
words, with the notes down to it, and fine in a nonallocating one. Nothing in the language blocked yet;
`__fminus_string_hash_blocking`, the scaffolding's second name, is classed so for the tests. (Recipe 41's `http_wait`
is the first real one; the scaffolding stays, for builds without the web library: recipe 41 says why.)

### The build options a builtin can need

`FMINUS_JSON` and `FMINUS_XML` (`cmake/FMinusOptions.cmake`), both on by default, decide whether a build has the JSON
and the XML parser. What a build has, it says three ways:

- **To a host:** `FMINUS_HAS_JSON` and `FMINUS_HAS_XML` in the generated `fminus/Config.h`, 1 or 0, for `#if`.
- **To a program:** `__FMINUS_JSON__` and `__FMINUS_XML__`, macros FMinus defines, to 1, before the program's first
  line, only in a build with the feature (`predefinedMacros`, `Frontend.cpp`'s `parse`), so one source can test it
  with `#ifdef` (recipe 31) and compile either way. Their names are C's for the implementation's (two underscores),
  so no program's own can meet them. A program's `#define` or `#undef` of one, and a host's (`-D`,
  `Program::Options::defines`), is an error in every build, with the feature or without it: "'__FMINUS_JSON__' cannot
  be used as a macro name: FMinus defines it in a build with the JSON parser (FMINUS_JSON)" (`macroNameError` and
  `#undef`, through `predefinedMacroError`). Clang only warns of a predefined macro redefined; FMinus's conditionals
  would then answer for a feature the build does not have.
- **To a call:** a builtin's table entry names the option it needs (`Feature`), and stays in the table whatever the
  build has, so a call of one this build leaves out is an error that names the option, `'json_float' needs the JSON
  parser, which this build of FMinus leaves out (FMINUS_JSON=OFF)` (`notBuilt`), not "call to undeclared function",
  and its name is still a builtin's, which no program may define.

No builtin needed an option when this recipe was written, so the last was tested through `notBuilt` and `isBuilt`
themselves, against `Config.h`; recipe 39's `json_` builtins are the first that do.

### fast_float

The report chose fast_float to read the parsers' numbers: text straight to a correctly rounded `float` (reading a
`double` and narrowing it rounds twice, and is now and then one bit off), the same in every locale (C's `strtof` reads
`"0,5"` where the locale is German), and about five times faster than `strtod`. It is pinned like every other
dependency, at 8.3.3 (2026-10-31). Its release has a single-header asset, `fast_float.h`, made by a script from the
tag; no checkout reproduces that file's bytes, so a digest of it could only be taken on trust from the release page,
and nothing here could build what it pins. The tag's archive is GitHub's generated one, whose bytes `git archive
--prefix=fast_float-8.3.3/ v8.3.3 | gzip -n` reproduces, as the WinFlexBison pin's do: the pin is that archive, and
its digest the one computed from the tag.

`cmake/FMinusFastFloat.cmake` follows `FMinusFft.cmake`: fetched once into the deps cache (`.deps/src/fast_float-8.3.3`,
marked with its hash), its own CMake never added, an interface library of its `include/` directory as a system one, so
FMinus's warnings stay out of it. The runtime links it when `FMINUS_JSON` or `FMINUS_XML` is on; with both off, the
module returns before it fetches anything. One file of the runtime includes it, `Numbers.cpp`, inside `#pragma GCC
visibility push(hidden)` after the standard headers it uses, so its inline functions' copies are hidden: a plugin that
links FMinus exports none of them. Being headers, its code is compiled in that file, so a sanitizer preset instruments
it, where PFFFT's own objects keep their flags. Nothing calls it but the runtime's own reader (and recipe 39 adds
`readDouble` beside it, which the JSON parser calls too), `fminus::runtime::readFloat(text, length, syntax, value)`
(`Numbers.h`): the bytes the number at the start of a text takes (0 for none), in JSON's syntax or C's, an overflow an
infinity and an underflow a zero, as IEEE rounds them.

### What it changes for programs and hosts

- **Programs** without string literals in code compile to the same IR. A literal where a value goes was a syntax
  error, and is now the code generator's, at the literal, in clang's words where C has them. `bytes_floats` and
  `bytes_pcm16` are new builtins, so a program's function of either name is now an error; so are the reserved
  `__fminus_` names, and `__FMINUS_JSON__` and `__FMINUS_XML__` as macros.
- **Hosts** get `FMINUS_HAS_JSON` and `FMINUS_HAS_XML` in `fminus/Config.h`. A host's define of a predefined macro is
  refused. The runtime has three more functions (`RuntimeSymbols.def`), which the JIT defines and an executable links.
- **The build** fetches fast_float (156,846 bytes) on a first configure with either option on, as it is by default,
  into the deps cache.
- **The VS Code grammar** (0.16.0) colours a string literal in code as in an attribute, with a format's `%d` and `%%`,
  and the two new builtins.

### Tests

- `tests/unit/StringTest.cpp`: through the JIT, a literal's bytes and length in the runtime (a NUL, an empty literal,
  adjacent literals, escapes, `%d` with negative and extreme ints and `%%`, a place across two literals, an
  enumeration's and a `bool`'s value), in a nonblocking function; `scanFormat`'s places and errors, a specifier read as
  clang reads one (flags, width, precision, length modifier: incomplete at the `%`, invalid at the conversion); each
  format error at its column (in a second literal, at an escape); each misplaced literal's error, clang's and FMinus's,
  at clang's column (the declarator, the `=`, the literal), a builtin's int parameter and a subscript among them, and
  no second error from the builtin it was given to; the effects'
  three classes, and a blocking builtin refused in a nonblocking function, through an unmarked one too, and allowed in a
  nonallocating one; `notBuilt`'s messages, and `isBuilt` and the predefined macros against `Config.h`; a builtin's
  prototype, conflicting ones and a definition.
- `tests/unit/BytesTest.cpp`: float32s' every bit (one, minus zero, infinity, the least subnormal, a NaN's payload) at
  three alignments; counts, caps, partial values, null data and output, negative sizes; PCM16's scaling, exact, through
  the JIT and from C++; both decoding in place, a cap short of the count too; the parameters' conversions and errors.
- `tests/unit/NumbersTest.cpp` (with either option): `readFloat`'s rounding (a decimal where reading a `double` and
  narrowing it is wrong), overflow and underflow, JSON's syntax against C's, a comma, and its bounds.
- `ConditionalTest.cpp`: the predefined macros as `Config.h` says, and their `#define`, `#undef` and host define refused
  in every build; `FrontendTest.cpp`: a literal as an expression in the dump, and where each of its bytes is spelt.
- `tests/lit/fminus/strings.fm`: the constants and the call at `-O0`, the declaration's attributes, the dump, and a
  program through the JIT and as an executable; `strings-errors.fm`: each error once, from the command line, and the
  macros'; `bytes.fm`: the calls and their attributes, and a program that decodes through the JIT and as an
  executable.

## Recipe 39: a JSON parser, and paths the compiler prepares

*In the compiler. Code: `src/runtime/Json.cpp` (the engine and every `fminus_rt_json_` function), `readDouble` and
`readInt` (`src/runtime/Numbers.h`, `.cpp`), the functions' declarations and the tables' layout (`Runtime.h`) and their
lines in `RuntimeSymbols.def`; `src/ir/Paths.cpp` (`scanPath`, `preparedPath`, `preparedChoices`); the `json_` entries
of `kRuntimeBuiltins`, `Param`'s `Document`, `Handle`, `Float`, `Double`, `Bool`, `Path` and `Choices`, `Result`,
`paramType`, `resultType`, `lowerRuntimeBuiltin` and `formatInts`, and the facts `RuntimeKind::Document`,
`DocumentRelease`, `Read`, `ReadChoice` and `ReadInto` (`src/ir/Lowering.h`, `Calls.cpp`), a prototype of one
(`declare`, `Functions.cpp`); `cmake/FMinusJson.cmake`, the yyjson pin, `FMINUS_JSON_MAX_BYTES` and
`FMINUS_JSON_MAX_DEPTH` (`cmake/FMinusOptions.cmake`, `Config.h.in`), the runtime's lines in `src/CMakeLists.txt`.
Tests: `tests/unit/JsonTest.cpp` (`[json]`) with `HeapCalls.h` and `.cpp`, `NumbersTest.cpp`, and
`tests/lit/fminus/json.fm`, `json-errors.fm` and `json-off.fm` (lit's new `json` feature); the example,
`examples/wavetable/`. Built with `-Werror` and tested in the `release` preset on Linux x86-64 only, every test passing
but the one this machine cannot run (the smoke test of LLVM's own `opt`, which failed here before recipe 38 too), and
in a second tree configured with `-DFMINUS_JSON=OFF`, with the same result; not yet in `dev` or `asan` (so neither
yyjson nor the new runtime code has run under a sanitizer), nor on macOS or Windows.*

Recipe 38 gave programs string literals; this one gives them something to say with them. A host hands a program a
document, a preset or a wavetable pack, and the program reads what it needs by path, off the audio thread or on it,
each number correctly rounded:

```c
json_doc doc = json_parse(data, bytes);                    // allocates; null for a text that is no JSON
float cutoff = json_float(doc, "/filter/cutoff", 1000.0f); // a JSON Pointer (RFC 6901); the fallback when absent
int   voices = json_int(doc, "/master/voices", 8);
int   wave   = json_choice(doc, "/osc/%d/wave", k, "sine", "saw", "square");   // which literal, or -1
int   n      = json_floats(doc, "/tables/%d", t, row, 2048);                   // how many; at most 2048 written
json_free(doc);
```

It is the report's recommendation, [research/web/README.md](research/web/README.md) §4.3, §6.1, §6.3 and §6.4's
first level, with the answers its §10 recommends: an embedded parser (yyjson's reader) with fast_float for the
numbers, paths prepared when the program is compiled, and `json_int` giving the fallback for a number that is no
`int`, never a truncated or saturated one.

### The engine: yyjson's reader, in the runtime

yyjson 0.13.0 (2026-09-08, MIT) is two C files that read a document in one pass, without recursion, into a flat array
of 16-byte values, validating all of RFC 8259 and its strings' UTF-8. It is pinned as fast_float is: its release has
no files of its own, so the pin is GitHub's archive of the tag, whose bytes `git archive --prefix=yyjson-0.13.0/
0.13.0 | gzip -n` reproduces, and its digest the one computed from the tag. `cmake/FMinusJson.cmake` follows
`FMinusFft.cmake`, and does nothing at all when `FMINUS_JSON` is off: fetched once into the deps cache
(`.deps/src/yyjson-0.13.0`, marked with its hash), its own CMake never added, one file, `src/yyjson.c`, compiled as an
object library with its own flags (none of FMinus's warnings or sanitizers), whose objects go into
`libfminus_runtime.a`. Two details matter:

- **Its symbols are hidden only if `yyjson_api` is defined.** yyjson's header marks every function
  `visibility("default")` unless the macro is defined already, so the C visibility preset alone would leave them
  exported from a plugin. The module defines it empty, with the rest of `FMINUS_YYJSON_DEFINITIONS`, which `Json.cpp`
  reads the header with too (inside `#pragma GCC visibility push(hidden)`, as `Numbers.cpp` includes fast_float).
  `readelf` on `yyjson.c.o` shows every global symbol `HIDDEN`.
- **What FMinus does not use is compiled out:** the writer, the incremental reader, files, yyjson's own JSON Pointer
  and Patch, every extension of JSON (comments, trailing commas, `NaN`, JSON5: `YYJSON_DISABLE_NON_STANDARD`), and its
  conversion of numbers (`YYJSON_DISABLE_FAST_FP_CONV`). A depth limit is compiled in,
  `YYJSON_READER_DEPTH_LIMIT=FMINUS_JSON_MAX_DEPTH`. The object's code is 54 KB on x86-64 (`size`'s `text`).

`json_parse` reads with one flag, `YYJSON_READ_NUMBER_AS_RAW`: every number stays its text, and a builtin converts it
when it reads it, with fast_float (below). That takes conversion out of parsing, and keeps the C library's `strtod`,
which yyjson would otherwise fall back to and which reads the locale, from ever running: the object still refers to
it (yyjson's own number reader, which FMinus never calls, does), but no path from `json_parse` reaches it. yyjson is
known to `Json.cpp` alone. Everything else, the prepared paths, the fallbacks, the promises to the audio thread, is
the runtime's own, so another engine (the report's §6.4 level 2, a parser of the program's own paths) would change
that file and nothing else.

### The builtins

| Builtin | Gives | Effect |
|---|---|---|
| `json_parse(const void* data, int bytes)` | a document, a `json_doc` (recipe 44; a `void*` before it): null for a text that is no JSON, larger than `FMINUS_JSON_MAX_BYTES` (64 MiB), or nested deeper than `FMINUS_JSON_MAX_DEPTH` (1024) | allocates |
| `json_free(json_doc doc)` | frees it (null: nothing) | allocates |
| `json_float(doc, path, ..., float fallback)`, `json_double(..., double fallback)` | the number at the path, correctly rounded; past the type's range an infinity, below its least subnormal a zero | wait-free |
| `json_int(doc, path, ..., int fallback)` | the number, when it is an integer an `int` holds | wait-free |
| `json_bool(doc, path, ..., bool fallback)` | `true` or `false` | wait-free |
| `json_count(doc, path, ...)` | an array's elements or an object's members; else 0 | wait-free |
| `json_floats(doc, path, ..., float* out, int cap)` | how many numbers an array of numbers, or of arrays of numbers row after row, holds; the first `cap` written; 0 and nothing written when any element is something else | wait-free |
| `json_choice(doc, path, ..., "a", "b", ...)` | the index of the first literal a string equals, byte for byte; else -1 | wait-free |
| `json_base64_floats(doc, path, ..., float* out, int cap)` | a base64 string's bytes as little-endian float32s, as `bytes_floats` reads them; 0 for no base64 | wait-free |

`...` is the path's ints. Every read gives its fallback (or 0, or -1) when the document is null, the path reaches
nothing, or the value is not of its kind, so a program never checks before it reads: the wavetable example checks
once, that every table had its size. A null document is an empty one, as a null FFT plan does nothing. There is no
way to get a string out: `json_choice` turns one into an index, an enumeration's value, which is all FMinus can hold.

Their table entries are `RuntimeBuiltin`s, as recipe 38's are, with more `Param`s (a read's document a `json_doc`,
`Param::JsonRead`, which a `const void*` converts to too, json_free's `Param::JsonDoc`, the fallbacks a `float`, a
`double` or a `bool`) and a `Result`, so `lowerRuntimeBuiltin` returns what each gives. (Before recipe 44 a document was
a `const void*` and json_free's handle a `void*`; a program that says so still compiles.) A `bool` goes to the runtime
as an `int`, 0 or 1, both ways: C's `bool` in a register is an ABI detail (`zeroext i1`) that an `int` avoids. A
prototype with the builtin's own types (`json_doc json_parse(const void* data, int bytes);`, or `void*` for the
document, as before) declares nothing new; `paramType`, `otherType` and `resultType` are what `declare` compares it
with.

### Paths, prepared when the program is compiled

A path is a format (recipe 38: `%d` an int, `%%` a percent sign) and a JSON Pointer: empty for the whole document, or
reference tokens, each after a `/`, in which `~1` is a `/` and `~0` a `~`. `Param::Path` lowers it with `scanPath`
(`Paths.cpp`), not as a string: each token becomes its pieces, a run of the literal's bytes (its escapes undone) or the
place of an int, in a private constant table the runtime walks:

```llvm
; "/osc/%d/de~1tune": three steps; "osc" is bytes 0 to 2, the second step the first int, "de/tune" bytes 3 to 9
@.path = private unnamed_addr constant { i32, i32, [9 x i32], [10 x i8] }
         { i32 9, i32 10, [9 x i32] [i32 1, i32 0, i32 3, i32 1, i32 0, i32 -1, i32 1, i32 3, i32 7],
           [10 x i8] c"oscde/tune" }, align 4
  %json_float = call float @fminus_rt_json_float(ptr %doc1, ptr @.path, ptr %format.ints, i32 1, float 1.5)
```

So the runtime never reads a path's text, and a call costs what walking the document costs. A `%d` is the int's
decimal text in its token: an array's index, or a member's name, or part of one (`"/settings/osc_%d_level"`, Vital's
shape). A token is an array index only as RFC 6901 writes one (`0`, or digits from 1 with no leading zero), so a
negative int, `01` and `-` reach nothing. In an object, the first member of the name is taken. `json_choice`'s
literals are a second table of the same layout, their lengths and their bytes (`preparedChoices`). Each table is one
block, reached through one argument, so a read touches only argument memory and the document's (below). The runtime
trusts no word of a table: a count or a piece past its end, or an int's place past the ints given, reaches nothing.

A path that is no JSON Pointer is an error at its own byte. The format's errors are recipe 38's, at clang's columns:

| Mistake | Error, at |
|---|---|
| `"osc"` | "a JSON Pointer must be empty or begin with '/'", at its first byte |
| `"/a~2"` | "unknown escape sequence '~2' in a JSON Pointer: '~0' is '~' and '~1' is '/'", at the `~` |
| `"/a~"` | "incomplete escape sequence '~' at the end of a JSON Pointer: ...", at the `~` |
| `"/osc/%f"`, too few or too many ints, a `float` for `%d` | recipe 38's format errors |
| a path, or a choice, that is not a literal | "argument to 'json_choice' must be a string literal", at it |
| `json_choice(doc, "/w")` | "too few arguments to function call, expected at least 3, have 2" |

clang has no words for a JSON Pointer, so the first three are FMinus's, in the shape of clang's own for C's escapes.
`json_choice`'s ints end at its first string literal, after which every argument must be one.

### Numbers

A builtin reads a number's text with `Numbers.h`: `readFloat` straight to the nearest `float` (never a `double`
narrowed: `1.00000005960464477550` is the float after 1, where the double, exactly halfway, narrows to 1), `readDouble`
to the nearest `double`, both fast_float's, with JSON's syntax, in no locale. A test sets a German locale, where the C
library's `strtod("0.5")` is 0, and reads the same numbers. `readInt` is FMinus's own, exact: the number is an integer
when its last nonzero digit weighs at least 1, and an `int` holds it when it is within the `int` range. So `7.0`,
`700e-2` and `0.07e2` are 7, `-0` is 0, `0e99999999999` is 0, and `1.5`, `3e9`, `2147483648` and
`2147483647.0000000001` give the fallback: no truncation, no saturation, and no rounding through a `double`, which would
make the last of those an `int`.

### What the runtime promises the audio thread

The effects class `json_parse` and `json_free` as allocating (refused in a nonallocating function, and so in a
nonblocking one) and every read as wait-free (allowed in both). The reads keep the promise: they walk a finished
document, which nothing writes once `json_parse` returns, through yyjson's inline accessors, with no allocation, no
lock and no system call, in steps the document's size bounds. Their declarations say what is true of `Json.cpp`
(recipe 19):

```llvm
declare float @fminus_rt_json_float(ptr readonly captures(none), ptr readonly captures(none),
                                    ptr readonly captures(none), i32, float) #2
attributes #2 = { nofree nosync nounwind willreturn memory(argmem: read, inaccessiblemem: read) }
```

`ReadInto`'s (`json_floats`, `json_base64_floats`) have `argmem: readwrite` and `out` `writeonly`. A document is
opaque, as a plan is: what it points to, its values and its copy of the text, only it points to, so a read touches
inaccessible memory there and argument memory elsewhere, and LLVM may share two reads of one document with nothing
between them that writes. `json_parse` reads its text (`argmem: read`; yyjson copies what it needs, so the program may
change its bytes after) and writes inaccessible memory and errno, as `fft_create` does, with a `noalias` result;
`json_free` is `fft_destroy`'s. Neither is an allocator LLVM may remove: a null document means the text is no JSON.

The tests prove the runtime's half, not only the effects'. `HeapCalls` (`tests/unit/HeapCalls.h`) counts the calls of
the C heap's functions a thread makes: `fminus-tests` defines `malloc`, `free` and their kin, each counting and then
calling glibc's own (`__libc_malloc`), and since the runtime is linked into the same executable, its yyjson calls them
too. A hundred rounds of every read of the example's preset, through the JIT, make no call, where one `json_parse` and
`json_free` make at least two. Then a child process enters seccomp's strict mode, in which any system call but `read`,
`write` and `exit` kills it (a contended lock's `futex`, an allocator's `mmap` or `brk`), runs every read, and writes
their sum back: the parent's sum (`tests/unit/StrictMode.h`, the helper every such proof uses: [testing.md](testing.md),
"A wait-free proof"). The counter is there on Linux with glibc and no sanitizer; seccomp on Linux, in every build.

### What a read costs

Wait-free is a promise about waiting, not about time: a read's steps are bounded by the document, and the document
is the server's. A step into an object looks through its members up to the one it names (yyjson keeps no index of
them); one into an array of arrays or objects steps over the elements before its index (into a flat array, of
numbers and strings, it goes straight there); and a value is read whole, every digit of a number (fast_float) and
every character of a base64 string (checked before a byte is written). Measured on the release runtime, a 100,000
member object's missing member took 0.6 ms to look for, the last of 100,000 objects in an array 0.9 ms, the number of
a 60 MB `0.000...1` 89 ms, and a loop of `json_float(doc, "/%d", i)` over the 100,000 members 45 s: a `%d` loop over n
is n² steps. A block of 64 samples at 48 kHz is 1.33 ms.

So the reads are for a document's few known values, or for the time before the audio: a program reads a document in
its `init`, into arrays, and its `process` reads those, as the wavetable example does (`examples/wavetable/`). Nothing
in the compiler can tell a cheap read from a dear one, since that is the document's doing; the effects allow them all,
as clang's allow a nonblocking function a loop of a million steps. Recipe 40's reads cost the same way.

### Input from the network

A document may come from anywhere, so every limit is the parser's, not the program's: at most 64 MiB of text (refused
before yyjson sees it), at most 1024 levels of arrays and objects (refused by yyjson, which never recurses: a million
`[`s are refused, not a stack overflow), sizes in `int`s with the count of values bounded by the text's size, and
memory about 16 bytes a value plus a copy of the text. A read checks every word of a prepared table against its
header, and base64 is checked whole before a byte of it is written. The tests parse RFC 8259's edges (the empty text,
a truncated one, a NUL inside and after, a byte order mark, invalid UTF-8 of four kinds, numbers JSON does not write,
huge and tiny exponents), nesting at the limit and past it, the size limit to the byte, and a 10 MB array read whole.

### Without `FMINUS_JSON`

`FMinusJson.cmake` returns before it fetches anything, so nothing of yyjson is fetched or compiled; `Json.cpp` compiles
to nothing (`#if FMINUS_HAS_JSON`), `RuntimeSymbols.def` lists no `json_` function, and the runtime library has no
JSON symbol. The compiler still knows every `json_` name (recipe 38's `Feature`): a call is `'json_parse' needs the
JSON parser, which this build of FMinus leaves out (FMINUS_JSON=OFF)`, and `__FMINUS_JSON__` is undefined, so a
program can `#ifdef` its loader out. `JsonTest.cpp`'s test of the names passes both ways, and lit's `json` feature
(read from the build tree's `fminus/Config.h`) runs `json.fm` and `json-errors.fm` in a build with the parser and
`json-off.fm` in one without. A second tree configured with `-DFMINUS_JSON=OFF` was built and its whole suite run.

### What it changes for programs and hosts

- **Programs** gain ten builtins, so a program's function of any of their names is an error, in every build.
- **Hosts** get `FMINUS_JSON_MAX_BYTES` and `FMINUS_JSON_MAX_DEPTH` in `fminus/Config.h`. The runtime has ten more
  functions in a build with the parser, which the JIT defines and an executable links: `libfminus_runtime.a` grows by
  yyjson's objects and `Json.cpp`'s (from 219 KB to 312 KB on Linux x86-64, the release preset, with fast_float's
  `Numbers.cpp` in both). A binary that links it carries yyjson's MIT notice (README, "License").
- **The build** fetches yyjson (1,641,525 bytes) on a first configure with `FMINUS_JSON` on, as it is by default.
- **The VS Code grammar** colours the ten builtins.

### Tests

- `tests/unit/JsonTest.cpp`: RFC 8259's valid and invalid documents, the null and empty inputs, a copy kept of the
  text; the depth at its limit and past it (arrays, and objects of arrays), a million unclosed `[`s, a value 1023
  levels down, the size limit to the byte; a 10 MB array counted and read; `json_float` on zeros, exponents, the
  double-rounding hard cases, overflow, underflow, the least subnormal and the largest float, and every other kind
  (its fallback); `json_double`; a comma-decimal locale (skipped without one); `json_int` on integers written every way
  and on everything that is not one; `json_bool`, `json_count` and `json_choice` on each kind, a NUL and UTF-8 in a
  string; `json_floats` flat, in rows, with a short cap, a cap of 0, a null `out`, and on every array it refuses;
  `json_base64_floats` padded, unpadded, with a cap, and on every string it refuses; paths with escapes, the root,
  empty names, a NUL, `%%`, and `%d` in and out of range, negative, `INT_MIN`, as a name or part of one; a null
  document and malformed tables; a program through the JIT, every builtin and fallback; every read allocating nothing
  and making no system call; the effects; each path error at its column; the parameters' conversions and the
  arities; prototypes; and each builtin's name, compiled or refused as the build has it.
- `tests/unit/NumbersTest.cpp`: `readDouble` and `readInt` beside `readFloat`.
- `tests/lit/fminus/json.fm`: the tables and calls at `-O0`, the declarations' attributes, and a program that parses a
  document (its bytes in ints) and reads it through the JIT and as an executable; `json-errors.fm`: each error once,
  from the command line; `json-off.fm`: the option's error, in a build without it.
- `examples/wavetable/`: a module that loads a wavetable pack from JSON in `init` and plays it in a nonblocking
  `process`, bit for bit against C++, in one call and in blocks; ctest's `example.wavetable`.

## Recipe 40: an XML parser, made as the JSON one is

*In the compiler. Code: `src/runtime/Xml.cpp` (the engine and every `fminus_rt_xml_` function), `src/runtime/Tables.h`
(a prepared table's reading, which `Json.cpp` had, now shared by both), `NumberSyntax::Xml` and `readInt`'s syntax
(`src/runtime/Numbers.h`, `.cpp`), the functions' declarations and the XML path's layout (`Runtime.h`) and their lines
in `RuntimeSymbols.def`; `src/ir/Paths.cpp` (`scanXmlPath`, `preparedXmlPath`); the `xml_` entries of
`kRuntimeBuiltins`, `Param::XmlPath` and its lines in `lowerRuntimeBuiltin`, and the `xml_` facts, which are the JSON
reads' kinds (`src/ir/Lowering.h`, `Calls.cpp`); `cmake/FMinusXml.cmake`, the pugixml pin, `FMINUS_XML_MAX_BYTES` and
`FMINUS_XML_MAX_DEPTH` (`cmake/FMinusOptions.cmake`, `Config.h.in`), the runtime's lines in `src/CMakeLists.txt`.
Tests: `tests/unit/XmlTest.cpp` (`[xml]`), `NumbersTest.cpp`, and `tests/lit/fminus/xml.fm`, `xml-errors.fm` and
`xml-off.fm` (lit's new `xml` feature); the example, `examples/wavetable/`, which loads the same pack from XML too.
Built with `-Werror` and tested in the `release` preset on Linux x86-64 only, every test passing but the one this
machine cannot run (the smoke test of LLVM's own `opt`, as for recipes 38 and 39), in a second tree configured with
`-DFMINUS_XML=OFF` and in a third with both parsers off, with the same result; not yet in `dev` or `asan` (so neither
pugixml nor the new runtime code has run under a sanitizer), nor on macOS or Windows.*

The second family of recipe 39's kind. A host that keeps its presets as XML hands the program the document, and the
program reads it as it would read JSON:

```c
xml_doc xml = xml_parse(data, bytes);                             // allocates; null for a text that is no XML
float c  = xml_float(xml, "/preset/filter/@cutoff", 1000.0f);     // an attribute; the fallback when absent
int   k  = xml_count(xml, "/preset/osc");                         // how many <osc> the preset has
int   w  = xml_choice(xml, "/preset/osc[%d]/@wave", i, "sine", "saw");   // the i-th <osc>, from 0
int   m  = xml_floats(xml, "/pack/table[%d]", t, row, 2048);      // an element's text as a list of numbers
xml_free(xml);
```

It is the report's recommendation, [research/web/README.md](research/web/README.md) §4.3, §6.2 and §6.3, with the
answer to its §10's seventh question settled so the two families agree: **an index counts from 0**, as C's arrays,
JSON Pointer's and so `json_float`'s do, where XPath's counts from 1. `"/pack/table[0]"` is the first table, which
XPath writes `/pack/table[1]`. Everything else is recipe 39's design, and on purpose the same: paths prepared when the
program is compiled, the same fallbacks, the same effects, a null document an empty one, the same tables, the same
number reader, the same off build.

### The engine: pugixml, in the runtime

pugixml 1.16 (2026-06-16, MIT) is one C++ file that parses a document into a tree, in a loop that does not recurse.
It is pinned as yyjson is, by GitHub's archive of its tag (`v1.16`), whose bytes `git archive --prefix=pugixml-1.16/
v1.16 | gzip -n` reproduces; the release also ships a tarball of its own, but the tag's archive is the one whose digest
could be computed here from what it holds. `cmake/FMinusXml.cmake` is `FMinusJson.cmake` again, and does nothing when
`FMINUS_XML` is off: fetched once into the deps cache, its own CMake never added, `src/pugixml.cpp` compiled as an
object library with its own flags and its own C++ standard (it is written for any), whose objects go into
`libfminus_runtime.a`. What FMinus does not use is compiled out, `FMINUS_PUGIXML_DEFINITIONS`, which `Xml.cpp` reads
the header with too: XPath (`PUGIXML_NO_XPATH`), exceptions (`PUGIXML_NO_EXCEPTIONS`: a failure, out of memory too, is
a status) and the STL (`PUGIXML_NO_STL`). Its symbols are hidden by the C++ visibility preset, inline ones too
(`VISIBILITY_INLINES_HIDDEN`), and `Xml.cpp` includes the header inside `#pragma GCC visibility push(hidden)`;
`readelf` on `pugixml.cpp.o` shows every global symbol `HIDDEN`, and `Xml.cpp.o` exports the nine `fminus_rt_xml_`
functions and nothing else. The object's code is 69 KB on x86-64 (`size`'s `text`). pugixml's file loading and its
number conversions are still in the object (they are member functions of its classes), so it refers to `fopen` and
`strtod`; no path from what `Xml.cpp` calls reaches either.

`xml_parse` checks the size first (`FMINUS_XML_MAX_BYTES`, 64 MiB), then parses a copy of the text
(`load_buffer`, which never writes the program's bytes), as UTF-8 (a byte order mark is skipped), with
`parse_cdata | parse_escapes | parse_eol | parse_wconv_attribute | parse_merge_pcdata`: elements, attributes, text and
CDATA; the five predefined references and character references expanded; line ends made line feeds and an
attribute's white space spaces; comments and processing instructions dropped, the text either side of one merged. Then
it walks the tree once, by its parent and sibling links, with no recursion, and refuses a document with more than one
root element (which pugixml, lenient, takes) or an element deeper than `FMINUS_XML_MAX_DEPTH` (1024, the root at 1):
pugixml has no depth limit of its own. It is a reader, not a validator, as the report says: what XML forbids and does
no harm may parse (invalid UTF-8, an attribute's name twice, of which the first counts, text after the root element).
One thing XML forbids would do harm, and is refused: a character reference to U+0000 (`&#0;`, `&#x00;`, and
`&#4294967296;`, since pugixml keeps a reference's value in 32 bits). pugixml expands it to a NUL, and a value, a
C string to pugixml, would end there: `<v>1.5&#0;junk</v>` would read as 1.5, and `cubic&#0;evil` match the choice
`"cubic"`. Before parsing, `xml_parse` looks for one anywhere in the text (`nulReference`, one `memchr` pass); only
when there is one does it parse the text once more with no reference expanded and look in every value, so that one
in a comment or a CDATA section, which is text, is still allowed, and the memory is still one tree's at a time.

**Entities are inert by construction.** pugixml reads a DOCTYPE only to skip it (`parse_doctype` off, so not even its
node is kept), and expands only the predefined and character references. An entity the document declares is never
expanded, and an external one never fetched: a reference to either stays in the text as written. So "billion laughs"
is a short document whose root's text is `&lol9;`, and `<!ENTITY secret SYSTEM "file:///...">` reads nothing. The
tests prove it, not only for the result: an internal entity, billion laughs (whose parse makes three heap calls), an
external general entity naming a real file, one naming a URL, and an external parameter entity are parsed and read in
a child process under a seccomp filter that kills it on `open`, `openat`, `socket`, `connect` or `execve`; it exits
normally, and a second child, which only `fopen`s the file under the same filter, is killed by `SIGSYS`, which shows
the filter works.

### The builtins

| Builtin | Gives | Effect |
|---|---|---|
| `xml_parse(const void* data, int bytes)` | a document, an `xml_doc` (recipe 44; a `void*` before it): null for a text that is no XML document (malformed, no root element or two), larger than 64 MiB, or nested deeper than 1024 | allocates |
| `xml_free(xml_doc doc)` | frees it (null: nothing) | allocates |
| `xml_float(doc, path, ..., float fallback)`, `xml_double(..., double fallback)` | the value as a number, correctly rounded | wait-free |
| `xml_int(doc, path, ..., int fallback)` | the value when it is an integer an `int` holds | wait-free |
| `xml_bool(doc, path, ..., bool fallback)` | `true` for `true` or `1`, `false` for `false` or `0` | wait-free |
| `xml_count(doc, path, ...)` | how many elements the last step matches: the parent's children of that name; with an index, 1 or 0; for an attribute, 1 or 0 | wait-free |
| `xml_floats(doc, path, ..., float* out, int cap)` | how many numbers the value lists, separated by white space or a comma; the first `cap` written; 0 and nothing written when anything in it is not a number | wait-free |
| `xml_choice(doc, path, ..., "a", "b", ...)` | the index of the first literal the value equals, byte for byte; else -1 | wait-free |

A path names an attribute, whose value is read, or an element, whose text is read: its first text child, a CDATA
section's or not, or the empty text when it has none (an element of children only has none). XML's white space around
a value is not part of it, so `<gain> 0.5 </gain>` is 0.5 and `xml_choice` compares `" saw "` as `saw`. Numbers are XML
Schema's, read by `readFloat` and `readDouble` with `NumberSyntax::Xml`: C's general syntax with a `+` allowed
(`+1.5`, `007`, `.5`, `1.`, `INF`, `-INF`, `NaN`), the bytes after it none. `readInt` gained a syntax: XML's allows a
`+`, leading zeros and digits on either side of the point, and is otherwise recipe 39's exact rule (`7.0` is 7, `1.5`
and `3e9` give the fallback). A bool is XML Schema's four words, nothing else (`TRUE`, `yes`, `2`: the fallback). A
list is numbers each followed by white space, or a comma with any white space around it, or the end: an empty item, a
comma at either end, or a number run into a word is not a list, and `xml_floats` writes nothing. There is no
`xml_base64_floats`: a table in XML is a list.

Their `kRuntimeBuiltins` entries are the JSON ones' with `Param::XmlPath` for `Param::Path`; `lowerRuntimeBuiltin`
prepares that parameter with `preparedXmlPath` and treats it as a format otherwise, so its ints, their checks and the
choices are the same code. The runtime functions have the JSON functions' facts (`Document`, `DocumentRelease`, `Read`,
`ReadChoice`, `ReadInto`), since they do the same with memory.

### Paths, prepared when the program is compiled

An XML path is a format, as a JSON Pointer is, and a small part of XPath: `/` and the root element's name, then child
steps by element name, each with an index or none, and a last step that may be an attribute's name after `@`. A step
with no index takes the first child of the name; an intermediate one too (XPath would take them all). A name matches
as written, its prefix and colon among it: there are no namespaces, so `"/preset/fm:depth"` finds `<fm:depth>`
whatever `fm` is bound to, and a default namespace changes nothing. `%d` may stand in a name (`"/osc_%d_level"`, the
int in decimal) or be the index (`"[%d]"`), and `%%` is a percent sign. `scanXmlPath` prepares it into a table of the
JSON path's layout, each step its kind (an element, with an index, with an int's index, an attribute), its name's
pieces as a token's are, and its index:

```llvm
; "/p/a[%d]": two steps; "p" is byte 0; "a" is byte 1, with the first int as its index
@.xmlpath.1 = private unnamed_addr constant { i32, i32, [9 x i32], [2 x i8] }
              { i32 9, i32 2, [9 x i32] [i32 0, i32 1, i32 0, i32 1, i32 2, i32 1, i32 1, i32 1, i32 0],
                [2 x i8] c"pa" }, align 4
  %xml_float = call float @fminus_rt_xml_float(ptr %doc, ptr @.xmlpath.1, ptr %format.ints, i32 1, float -1.0)
```

The runtime reads it through `Tables.h`, which is `Json.cpp`'s table reading moved to a header both include (in an
unnamed namespace, so each file keeps its own copy and exports none): it trusts no word, and a negative index, one past
the children, an int's place past the ints, an attribute before the last step, or an unknown kind reaches nothing.
What XPath has beyond this is an error at its first byte, since no XML name holds it; clang has no words for XPath,
so the messages are FMinus's:

| Mistake | Error, at |
|---|---|
| `"preset"`, `""` | "an XML path must begin with '/' and the root element's name", at its first byte (an empty one's quote) |
| `"/@a"` | "the first step of an XML path is the root element's name, not an attribute", at the `@` |
| `"/a//b"`, `"/a/"` | "expected an element's name after '/' in an XML path", at what follows the `/`, or the `/` at the end |
| `"/a/@"` | "expected an attribute's name after '@' in an XML path" |
| `"/a/@b/c"` | "an attribute must be the last step of an XML path", at the `/` |
| `"/a/@b[0]"` | "an attribute takes no index in an XML path", at the `[` |
| `"/a[x]"`, `"/a[-1]"`, `"/a[@n='1']"` | "expected an index in '[ ]' in an XML path: digits, from 0, or %d", at the byte |
| `"/a[1"` | "expected ']' in an XML path", at the byte that is not one, or the `[` at the end |
| `"/a[2147483648]"` | "an index in an XML path must be at most 2147483647", at its first digit |
| `"/a[0]b"` | "expected '/' after ']' in an XML path" |
| `"/a/*"`, `"/a/text()"`, `"/a/b c"` | "unexpected '*' in an XML path: a step is a name, an index '[n]' after it, or a last '@' and an attribute's name" |
| `"/osc[%f]"`, too few or too many ints, a `float` for `%d` | recipe 38's format errors (and, for the first, the index's) |

### What the runtime promises the audio thread

The same as recipe 39's, kept the same way. The effects class `xml_parse` and `xml_free` as allocating and every read
as wait-free. A read walks a finished tree, which nothing writes once `xml_parse` returns, through pugixml's
accessors, which only follow pointers and read its strings: no allocation, no lock, no system call, and steps the
document's size bounds (an element found among its parent's children, an attribute among its element's, a name's
length by `strlen` within the document). `xml_floats` reads the list twice, to check it and then to write the first
`cap`, stopping there. They cost what recipe 39's do ("What a read costs"): 100,000 siblings' last took 2.2 ms to find,
a missing attribute among 100,000 0.7 ms, and a loop of `xml_float(doc, "/r/b[%d]", i)` over them 42 s. `XmlTest.cpp`
proves it as `JsonTest.cpp` does: a hundred rounds of every read of the preset,
through the JIT, make no heap call (`HeapCalls`), where a parse and a free make some, and a child under seccomp's strict
mode runs every read and writes their sum back.

### Without `FMINUS_XML`

`FMinusXml.cmake` returns before it fetches anything, `Xml.cpp` compiles to nothing, `RuntimeSymbols.def` lists no
`xml_` function, and a call of one is `'xml_parse' needs the XML parser, which this build of FMinus leaves out
(FMINUS_XML=OFF)`; `__FMINUS_XML__` is undefined. A tree configured with `-DFMINUS_XML=OFF` (yyjson and fast_float
still fetched, for JSON) and one with both options off (nothing of pugixml, yyjson or fast_float fetched or compiled)
were built and their whole suites run; `nm` on each one's `libfminus_runtime.a` shows no `xml` or `pugi` symbol.

### What it changes for programs and hosts

- **Programs** gain nine builtins, so a program's function of any of their names is an error, in every build.
- **Hosts** get `FMINUS_XML_MAX_BYTES` and `FMINUS_XML_MAX_DEPTH` in `fminus/Config.h`. The runtime has nine more
  functions in a build with the parser: `libfminus_runtime.a` grows by pugixml's objects and `Xml.cpp`'s (from 313 KB
  with the JSON parser alone to 453 KB with both, on Linux x86-64, the release preset). A binary that links it carries
  pugixml's MIT notice (README, "License"). Memory for a document is pugixml's tree, a few dozen
  bytes a node, plus the copy of the text: a hostile 64 MiB of `<a/>` is about 16 million nodes, 1.2 GB at its peak,
  where a 64 MiB JSON array of zeros is about half that. The depth limit is checked once pugixml has built the tree
  (it has no limit of its own), so a text past it costs its tree before it is refused, unlike yyjson's, which stops
  at the limit: 64 MiB of unclosed `<a>`s is the worst there is, about 1.5 GB at its peak and 0.7 s, then null.
- **The build** fetches pugixml (588,574 bytes) on a first configure with `FMINUS_XML` on, as it is by default.
- **The VS Code grammar** colours the nine builtins.

### Tests

- `tests/unit/XmlTest.cpp`: well-formed documents (a declaration, a byte order mark, DOCTYPEs with and without an
  internal subset, comments and processing instructions around and inside the root, references, CDATA, prefixes) and
  documents that are not (nothing, no root, two roots, unclosed, mismatched, unquoted, after the root); the null and
  empty inputs; a copy kept; the depth at its limit and past it, a million unclosed elements, 200,000 closed, a value
  1024 levels down; the size limit to the byte; the entity attacks, confined; a reference to U+0000 in a value
  refused, in every form, and allowed in a comment or CDATA; CDATA, mixed text, a comment between texts, references
  in text and attributes, white space around values, prefixed and default namespaces, a DOCTYPE's default attribute
  (not applied); a 10 MB document of about a hundred lists of 10,000 numbers each, in text and in attributes, read
  whole and summed exactly; `xml_float` on XML Schema's numbers, the double-rounding hard cases,
  overflow, underflow, and everything that is not one number; `xml_double`; a comma-decimal locale (skipped without
  one); `xml_int`, `xml_bool`, `xml_count` and `xml_choice` on each kind of node; `xml_floats` with each separator, in
  text, CDATA and attributes, with a short cap, a cap of 0, a null `out`, and on every list it refuses; paths with
  indices from 0, the root's index, attributes, prefixes, and `%d` in and out of range, negative, `INT_MIN` and
  `INT_MAX`, in a name and as an index; a null document and malformed tables; a program through the JIT; every read
  allocating nothing and making no system call; the effects; the prepared tables; each path error at its column (each
  column recounted by hand); the parameters' conversions and the arities; prototypes; each builtin's name in both
  builds.
- `tests/unit/NumbersTest.cpp`: XML's syntax in `readFloat` and `readInt`.
- `tests/lit/fminus/xml.fm`: the tables and calls at `-O0`, the declarations' attributes, and a program that parses a
  document and reads it through the JIT and as an executable; `xml-errors.fm`: the errors from the command line;
  `xml-off.fm`: the option's error, in a build without it.
- `examples/wavetable/`: `initXml` loads the same pack from XML (three tables as text, one as an attribute's list),
  and `--check` compares its output with the JSON loader's and C++'s, bit for bit, and refuses five broken XML
  documents.

## Recipe 41: the web library, an HTTP(S) client under the host's policy

*In the compiler. Code: `src/web/` (the library, `fminus_web`: `Web.h` and `Web.cpp`, the C functions and the I/O
thread, and `Context::stop`; `Policy.h` and `Policy.cpp`, the pattern language, the URL rules and the address check;
`Roots*.cpp`; `Main.cpp`, an executable's context; `WebSymbols.def`); the `http_` entries of `kRuntimeBuiltins`,
`Feature::Web`, `Param::Url`, `Param::ContentType`, `Result::Bytes` and their lines in `lowerRuntimeBuiltin`, and the
`fminus_web_` facts (`src/ir/Lowering.h`, `Calls.cpp`); `src/ir/Web.cpp` (a URL's checks, the requests' metadata, the
context global, the status codes' names) and its hooks in `named` (`Functions.cpp`) and the undeclared-identifier
errors (`Memory.cpp`, `Constants.cpp`); `__FMINUS_WEB__` (`Preprocessor.cpp`); `Program::Options::web`,
`webRequests()` and `stopWeb()` (`include/fminus/FMinus.h`, `src/api/FMinus.cpp`); the JIT's symbols
(`support/JitRuntime.cpp`), an executable's context and libraries (`support/Aot.cpp`, `Aot.h`); the driver's
`--web-allow` and `--web-ca-file` (`tools/fminus/main.cpp`); `cmake/FMinusWeb.cmake`, the curl and Mbed TLS pins,
`FMINUS_WEB` and `FMINUS_HAS_WEB`, the `web` preset, the library's and the link list's lines in `src/CMakeLists.txt`;
`Scripts/pin-web.sh`, the pins' lines for the next releases (its libcurl half run here; GitHub's API, which its Mbed TLS
half reads, refuses this machine).
Tests: `tests/unit/WebTest.cpp` (`[web]`: the library against a loopback server, and programs through the JIT and as
executables), `tests/unit/HttpTest.cpp` (the builtins, in every build), `tests/lit/fminus/web.fm`, `web-errors.fm` and
`web-off.fm` (lit's new `web` feature); the example, `examples/fetch/`. Built with `-Werror` and tested in the
`release` preset on Linux x86-64 only, in a tree with `-DFMINUS_WEB=ON` and in the default tree without it, every test
passing but the one this machine cannot run (the smoke test of LLVM's own `opt`, as for recipes 38 to 40); the
library's own tests under GCC's ThreadSanitizer and AddressSanitizer, outside the presets, and `stop()` racing
`close`, waits and new requests under both, in a harness of its own (libcurl and Mbed TLS uninstrumented); not yet in
`dev`, `web` or `asan`, nor on macOS or Windows. [web.md](web.md) is its manual.*

The third family of recipe 38's kind, and the one the strings were for. A program fetches what it plays, off the audio
thread, from a URL its host allows:

```c
http_response r = http_get("https://example.com/presets/%d.json", k);   // at once; refused unless allowed
if (http_wait(r, 10000) == 200)                                 // blocks: not in a nonblocking function
{
    json_doc doc = json_parse(http_body(r), http_size(r));
    gain = json_float(doc, "/gain", 1.0f);
    json_free(doc);
}
http_close(r);
```

It is the report's recommendation, [research/web/README.md](research/web/README.md) §4.1, §4.4, §5, §7 and §8, with
its §10's answers as it gives them: literals only for strings (B), HTTP(S) GET and POST first with no WebSocket and no
server, libcurl with Mbed TLS (Schannel on Windows) bundled and pinned, `FMINUS_WEB` off by default, and the name
"web".

### The library: its own, beside the runtime

`fminus_web` is a static library of its own (`src/web/`, globbed as `src/circuit/` is), never part of
`fminus_runtime`: the runtime is what every host and every executable links, and networking brings a thread that must
be joined before a plugin unloads, system libraries and a TLS stack, which a host that wants none of them should not
have in its binary at all. `fminus_lib` links it only with the option on. libcurl and Mbed TLS are built at configure
time into the deps cache, as Bison and Flex are (`cmake/FMinusWeb.cmake`): fetched once, configured, built and
installed with their own CMake into `install/<name>-<version>-<flavor>`, the flavor a hash of the compiler and the
arguments, then found there with `find_package` (each `<name>_DIR` cleared first, so a tree never keeps an install of
another flavor that an earlier configure cached). libcurl is HTTP and HTTPS only; both are hidden and
position-independent. [web.md](web.md) has the rest: one I/O thread on libcurl's multi interface, a context a host's
policy, the checks at start, at each redirect and on each connection.

`Context::stop()` is this recipe's addition to the library: the flag first, under the context's mutex, so a request
started after it is cancelled where it starts (and one queued before it, where its hop begins, on the I/O thread);
then a cancel of each request still pending, queued under the mutex and waited for. `http_close` now waits for every
command queued before it is freed (`Engine::posted`, a barrier): a cancel `stop()` queued for a request that ended
first would otherwise reach the I/O thread after the request was freed.

### The builtins

| Builtin | Parameters | Result | Effect |
|---|---|---|---|
| `http_get` | `Url` | `Pointer` | allocates |
| `http_post` | `Url`, `Bytes`, `Int`, `ContentType` | `Pointer` | allocates |
| `http_wait` | `Handle`, `Int` | `Int` | blocks |
| `http_status`, `http_size`, `http_progress` | `Handle` | `Int` | wait-free |
| `http_body` | `Handle` | `Bytes`, a `const void*` | wait-free |
| `http_cancel`, `http_close` | `Handle` | `Void` | allocates |

Their entries in `kRuntimeBuiltins` name `Feature::Web`, so a build without the library knows every name and refuses
a call of one with `notBuilt`'s error, and their effect class, which `Effects.cpp` reads with no change: recipe 38
built the third class, `Blocks`, for `http_wait`, which is the first real builtin in it. Two new parameter kinds:

- **`Param::Url`** is a format whose ints come **last**, after every other argument, as `printf`'s do
  (`http_post(url, data, bytes, type, k)`), where a path's follow it (`json_float(doc, path, k, fallback)`).
  `lowerRuntimeBuiltin` computes where the ints start (`intsAt`) and does not shift the other parameters past them
  when the format is a URL (`trailing`). Before its text, a URL passes the program's web context, loaded from the
  module's global (`webContext`), so `http_get(url, ...)` calls `fminus_web_http_get(context, text, length, ints,
  count)` with no hidden parameter in the table. Then `checkUrl`: no byte a URL cannot hold (a control byte, CR and
  LF among them, a space, a backslash, anything not ASCII), a scheme of `http` or `https`, and no `@` before the path.
  Each is an error at its byte, of a request the library would refuse anyway: what the compiler can see, it says
  where it is written.
- **`Param::ContentType`** is a string: its text and length, no ints, checked by `checkContentType` (printable ASCII,
  at most 255 bytes).

And a result, **`Result::Bytes`**, a `const void*`: `http_body`'s, which the program may only read.

A null handle is the library's to handle: every function takes null and does nothing (`http_status` gives
`HTTP_INVALID`). There is no null pointer constant in FMinus, but an `http_response` parameter or field may be one,
and one declared without an initialiser is.

### What the library's functions touch, told to LLVM

`kRuntimeFacts` gives each of them a kind (recipe 19), and each kind attributes that are true of `Web.cpp`:

| Kind | Functions | Attributes |
|---|---|---|
| `WebStart` | `get`, `post` | `nounwind`, `willreturn`; `memory(argmem: readwrite, inaccessiblemem: readwrite, errnomem: write)`; the URL, the ints, the body and the type `readonly captures(none)` (the context's pointer is captured: the request keeps it); `noundef` result, not `noalias` (a refused request with no context is one shared request) |
| `WebWait`, `WebEnd` | `wait`; `cancel`, `close` | `nounwind`, `memory(argmem: readwrite, inaccessiblemem: readwrite)`, the request `captures(none)`; no `willreturn`, as each waits for another thread |
| `WebRead` | `status`, `size`, `body`, `progress` | `nounwind`, `willreturn`, `nofree`, `memory(argmem: readwrite, inaccessiblemem: readwrite)`, `captures(none)` |

The reads are the case to get right. Each is an acquire load of what the I/O thread publishes, so it is **not**
`readonly`, nor `nosync` (an acquire load synchronises with the store that published): were LLVM told a read only
reads, it could merge two calls in a polling loop, or move one out of the loop, and a `process` that polls for a late
table would never see it arrive. Saying they may write the request's memory (opaque to the program, as an FFT plan's
is) keeps every call where the program wrote it, and still tells LLVM that none of them touches a module's fields.

### The program's context, and what the host sees

**The context** is a global of the module, `fminus_web_context` (`ir::kWebContextSymbol`), a pointer, null until the
host sets it, defined at the first request a program starts (`webContext`), exactly as the loop budget's
`fminus_rt_loop_budget` is (recipe 29). `Program::compile` creates the program's `web::Context` from `Options::web`
when the program has the global and the host allowed something, looks the global up (which compiles the module, as
the budget's look-up does) and stores the context's handle in it. A null context reaches nothing, in the library, so
the default policy costs no context at all. The program cannot name the global (a function of its name is an error),
nor the library's functions: nothing it asks of the library can widen the policy. (A program's own external
functions are another matter, and not the library's: [web.md](web.md), "The policy is the library's, not a sandbox".)

**The URLs** are literals, so the code generator records each call's in the module's named metadata,
`fminus.web.requests`: its method, its pattern (each `%d` a `*`, each `%%` a `%`), its file, line and column
(`recordWebRequest`), as the bounds checks record their sites (recipe 30). `ir::webRequests` reads them back, and
`Program::webRequests()` returns them, before any of the program's code has run.

**The status codes** are names a program uses without declaring them: `named()`, C++'s lookup in a method, falls back
past the file's scope to `webStatus`, a static binding for each name with an enumeration constant's value, so
`HTTP_REFUSED` folds, labels a `case` and sizes an array as an enumerator does, and a program's own declaration of
the name, in a block or at the file's scope, is found first. In a build without the library `webStatus` finds
nothing, and the two places that report an undeclared identifier (`lvalue`, `fold`) ask `unbuiltWebStatus` first, so
`HTTP_REFUSED` is the error that names the option. *Why not macros:* FMinus substitutes none in code (recipe 31). *Why
not an enumeration of a type of its own:* a program could not name the type.

### Lifetime

`~Program` (its `Impl`'s destructor) stops the context, then destroys it, before the JIT goes: every request
cancelled and freed, every thread in the library's `http_wait` woken and out of it, the I/O thread joined if it was the
last context. A thread still inside the program's code is the host's to stop first: `Program::stopWeb()` wakes one in
`http_wait` with `HTTP_CANCELLED`, the host joins it, and only then destroys the `Program`, as it must for any thread
running a program's code.

### The driver and executables

Neither has a host, so each takes its policy from the person who runs it, default-deny: `fminus --run` from
`--web-allow PATTERN` (repeatable) and `--web-ca-file`, checked by `web::checkPolicy` before anything compiles, and an
executable from its environment, `FMINUS_WEB_ALLOW` and `FMINUS_WEB_CA_FILE`, when it starts. `addEntryPoint` gives
the C `main` of a module with the context global a call of `fminus_web_main_start(&fminus_web_context, argv[0])`
before the program's `main` (a non-zero result, a refused policy, is the exit status, 2, its message already on
stderr), and `fminus_web_main_stop` after it. Those two make a context from the environment, so no program may reach
them: `WebSymbols.def` lists them under `FMINUS_WEB_MAIN_SYMBOL`, which `Constants.cpp` reads (the names stay reserved)
and `JitRuntime.cpp` does not (a JIT never defines them), and `declare` (`Functions.cpp`) refuses a function of a
program, a prototype or a definition, whose name begins `fminus_web_` (`ir::kWebPrefix`), in every build, so that a
program cannot call the library past its literals and `webRequests()`. `emitExecutable` links the web library's archives only when the module
declares a `fminus_web_` function: `src/CMakeLists.txt` writes the list (`libfminus_web.a`, then `FMINUS_WEB_LINK`
from `FMinusWeb.cmake`: libcurl's and Mbed TLS's archives and the system libraries libcurl's own CMake links on the
platform) into `generated/support/WebLink.inc`, which `linkExecutable` includes. An executable that makes no request
is linked as before.

### The scaffolding stays

Recipe 38 made `__fminus_string_hash` and `__fminus_string_hash_blocking` to prove a literal's way to the runtime and
the blocking class before any real builtin needed them, and said they would go when builtins of their own came. They
stay: the default build has no web library, so `http_wait` cannot carry a blocking call through the effects there,
and a build with both parsers off has no other format. `StringTest.cpp` and `strings*.fm` keep testing both in every
build; `HttpTest.cpp` and `web*.fm` test the real ones where the library is.

### What it changes for programs and hosts

- **Programs** that use none of it compile to the same IR. `http_get` and its kin are new builtins in every build, so a
  program's function of one of those names is an error; a program's own `HTTP_REFUSED` hides FMinus's. `__FMINUS_WEB__`
  cannot be defined.
- **Hosts** get `Program::Options::web` (default: no network), `webRequests()` and `stopWeb()`, in every build (the
  policy is not read without the library), and `FMINUS_HAS_WEB` in `fminus/Config.h`. A host linking `FMinus::FMinus`
  in a build with the library links `fminus_web`, and through it libcurl and Mbed TLS.
- **The build** is unchanged by default: nothing is fetched or built with the option off. On, a first configure
  downloads and builds libcurl and Mbed TLS (a minute or two).
- **The VS Code grammar** (0.17.0) colours the `http_` builtins and the status codes' names.

### Tests

- `tests/unit/WebTest.cpp`: the library at its own level (patterns, the address check, every
  status against a loopback server it starts, redirects, limits, HTTPS with its own certificate authority, contexts
  destroyed with requests in flight and threads waiting); then programs, against the same server: one fetches a JSON
  body through the JIT under `Options::web` and reads its numbers, and the same as an executable, run with
  `FMINUS_WEB_ALLOW`, without it, with a pattern that does not match and with one the library refuses (exit 2); the
  policy refusing through `http_status` (a URL, and a redirect, not allowed, and nothing sent); a `Program` destroyed
  with requests in flight, and stopped with a thread in `http_wait` inside the program's code.
- `tests/unit/HttpTest.cpp`, in every build: the effects of each builtin, through the table and through the effects'
  errors; each builtin and status name known, with the option's error in a build without it; then, with it, the
  status codes' values against `Web.h`'s, a URL's errors at their columns, `webRequests()` for four call sites in two
  files, `Options::web` checked by `compile()`, and the default policy refusing, and a null handle, through the JIT.
- `tests/lit/fminus/web.fm`: the calls and the declarations' attributes at `-O0`, the metadata, `--run` with no
  pattern and with one that does not match (refused, no request made), a bad pattern, `--web-allow` with `--emit`, an
  executable with and without `FMINUS_WEB_ALLOW`; `web-errors.fm`: each error once; `web-off.fm`: each name's error
  and `--web-allow`'s in a build without the library.
- `examples/fetch/`: `--check` serves the wavetable example's pack on 127.0.0.1, fetches it through the program, plays
  it, compares the samples with the same pack loaded directly (identical), and checks a missing pack (404) and the
  default policy (refused, no request).

## Recipe 42: the web server, a program's requests taken on the audio thread

*In the compiler. Code: the server in `src/web/` (`Server.h` and `Server.cpp`, the slots, the connections and the
rules; `ServerPolicy.cpp`, the server's policy and its classifiers; `ServerRoot.cpp`, a directory as static files;
`Sockets.h`, `SocketsPosix.cpp` and `SocketsWindows.cpp`; `Handle.h`, the kind every handle carries; and in `Web.h`,
`Web.cpp`, `Policy.*`, `Main.cpp` and `WebSymbols.def`, their hooks); the server's entries of `kRuntimeBuiltins`,
`Feature::WebServer`, `Param::Route`, `Param::QueryName`, `Param::ReplyFormat`, the `Server*` kinds of
`kRuntimeFacts` and their lines in `lowerRuntimeBuiltin` (`src/ir/Lowering.h`, `Calls.cpp`); `src/ir/WebServer.cpp` (a
route's check, its metadata and the route table, a query's name, a reply's format) and `finishWebRoutes`'s call in
`CodeGen.cpp`; `__FMINUS_WEB_SERVER__` (`Preprocessor.cpp`); `Program::WebServerPolicy`, `webRoutes()` and
`webPort()` (`include/fminus/FMinus.h`, `src/api/FMinus.cpp`); the driver's `--web-listen`, `--web-token`,
`--web-host`, `--web-root` and `--web-open` (`tools/fminus/main.cpp`); `cmake/FMinusLlhttp.cmake`, llhttp's pin,
`FMINUS_WEB_SERVER` and `FMINUS_HAS_WEB_SERVER`, the `web` preset, the server's lines in `src/CMakeLists.txt` and
`cmake/FMinusFastFloat.cmake`; llhttp's line in `Scripts/pin-web.sh`.
Tests: `tests/unit/WebServerTest.cpp` (`[server]`: the engine from C++), `tests/unit/HttpServerTest.cpp` (the builtins,
in every build), `tests/unit/WebServeTest.cpp` (programs that serve, through the JIT and as executables),
`tests/library/WebPolicyTest.cpp`'s server case (the host API in C++17, in every build),
`tests/lit/fminus/web-server.fm`, `web-server-errors.fm` and `web-server-off.fm` (lit's new `web-server` feature); the
example, `examples/remote/`. Built with `-Werror` and tested in the `release` preset on Linux x86-64 only, in three
trees: `-DFMINUS_WEB=ON -DFMINUS_WEB_SERVER=ON`, `-DFMINUS_WEB=ON` alone, and the default, every test passing but the
one this machine cannot run (the smoke test of LLVM's own `opt`, as for recipes 38 to 41); the engine and its test under
GCC's AddressSanitizer with UndefinedBehaviorSanitizer and under ThreadSanitizer, outside the presets, with a race
harness and a mutation fuzzer; not yet in `dev`, `web` or `asan`, nor on macOS or Windows, nor with IPv6. [web.md](
web.md), "The server", is its manual, and [research/web/server.md](research/web/server.md) the report it builds.*

The reverse of recipe 41: a request comes in, and the program answers it. FMinus has no callbacks, so the program
**takes** its requests, and since taking, reading and answering one are wait-free, its audio thread may do it between
two blocks:

```c
void process(float* out, int n) [[clang::nonblocking]]
{
    for (http_request r = http_next(server); r; r = http_next(server))   // the requests waiting, or none
    {
        if (http_is(r, "POST /osc/%d/gain"))                         // a route literal, checked here
        {
            gain[http_arg(r, 0) & 7] = http_query_float(r, "value", 1.0f);
            http_reply(r, 204);
        }
        else if (http_is(r, "GET /state"))
            http_reply_text(r, 200, "application/json", "{\"level\":%g}", level);
    }
    // ... the block
}
```

It is the report's recommendation as its section 0 chose it: the program serves, by builtins (its shape A), llhttp for
the request's head and FMinus's own code for everything else, the LAN only with a token or the host's `open`, static
content the host's, a reply's text C's `printf` in part, no CORS ever, no TLS yet, and an option of its own,
`FMINUS_WEB_SERVER`, off by default.

### The engine, in the library, behind its own option

Nothing of the language is in the engine, and it was built first, tested from C++ before any builtin existed
(`WebServerTest.cpp`). It lives in `fminus_web`, beside the client, for one reason: **the I/O thread**. A plugin
should start as few threads as it can and join each before it unloads, and `fminus_web` has one already, waiting in
`curl_multi_poll`, which takes descriptors beside libcurl's own. So a server's listening sockets and its connections
are more descriptors in the same wait: two new commands on the same queue (`Serve`, `Unserve`), and after libcurl's
transfers each turn, `Servers::turn()` accepts, reads, parses, writes and frees, then `descriptors()` rebuilds the
array the next wait takes. A socket is closed only in a turn or a detach, and the array is rebuilt after each, so a
closed descriptor never reaches curl, which would not report it.

**llhttp** (9.4.3, pinned; `cmake/FMinusLlhttp.cmake`) is built as yyjson is (recipe 39): fetched once into the deps
cache, its three generated C files an object library whose objects `src/CMakeLists.txt` puts into `fminus_web`, its
own flags kept, its symbols hidden, its CMake never added. Every lenient flag stays off, and every limit is the
server's own, counted in its callbacks: the head is fed at most what is left of its 16 KiB, fields are counted as they
come, the parser is paused at the end of each message so one request at a time is read, and what FMinus does not serve
is refused at the first callback that shows it (another version, a method outside the seven, any
`Transfer-Encoding`, so llhttp's chunked machinery is never entered). fast_float reads a query's numbers, in
`Server.cpp` alone, hidden as `Numbers.cpp` hides it (`cmake/FMinusFastFloat.cmake` now fetches it for the server too).

**The option** is `FMINUS_WEB_SERVER` (`cmake/FMinusOptions.cmake`), which configure refuses without `FMINUS_WEB`,
and `fminus/Config.h`'s `FMINUS_HAS_WEB_SERVER`. With it off, each of the server's sources is guarded by that macro
and compiles to nothing, nothing is fetched, and `libfminus_web.a` has no llhttp, listen or root symbols.

**The slots** are §4.3 of the report: a fixed table, `maxPending` of them, each with a reply buffer of
`maxReplyBytes`, all made when the server starts. A slot's state is one atomic 64-bit word, a sequence number above 8
bits of phase (free, published, claimed, answered, abandoned, done, closed): the I/O thread publishes with a store,
`http_next` claims the oldest published slot with a compare-and-swap (at most twice the slots' number of tries, so it
stays wait-free), a reply publishes with another, and the I/O thread sends and frees. A request the library gives up on
while the program holds it (its client left, its reply timed out) goes to abandoned, which the program's reply turns to
done, returning `HTTP_CANCELLED`, so a request never changes under the thread that holds it; until that reply the slot
stays the program's, so a request never answered holds its slot until the server closes (and `maxPending` of them
leave every later request a 503). While any slot is out the
I/O thread turns once a millisecond instead of being woken, since waking it is a system call.

**Handles that know their kind** (`Handle.h`). A request made, a request received and a server are all `void*` to a
program, and recipe 41's functions took any pointer as a request. Each kind now begins with a 32-bit word of its own,
which every C function reads first (`as<T>`): `http_status` of a server is `HTTP_INVALID`, `http_size` and `http_body`
of a received request read its body, `http_close` of a server closes it. The client's request derives from `Handle`
and nothing else of it changed: its tests pass as they were.

### The builtins' table rows

| Builtin | Parameters | Result | Effect |
|---|---|---|---|
| `http_listen` | none | `Pointer` | allocates |
| `http_port` | `Handle` | `Int` | wait-free |
| `http_next` | `Handle` | `Pointer` | wait-free |
| `http_accept` | `Handle`, `Int` | `Pointer` | blocks |
| `http_is` | `Handle`, `Route` | `Bool` | wait-free |
| `http_arg` | `Handle`, `Int` | `Int` | wait-free |
| `http_query_float`, `http_query_int` | `Handle`, `QueryName`, `Float` or `Int` | `Float` or `Int` | wait-free |
| `http_reply` | `Handle`, `Int` | `Int` | wait-free |
| `http_reply_bytes` | `Handle`, `Int`, `Bytes`, `Int`, `ContentType` | `Int` | wait-free |
| `http_reply_text` | `Handle`, `Int`, `ContentType`, `ReplyFormat` | `Int` | wait-free |

Their entries name `Feature::WebServer`, so `isBuilt` asks `FMINUS_HAS_WEB_SERVER` and `notBuilt` says `'http_listen'
needs the web server, which this build of FMinus leaves out (FMINUS_WEB_SERVER=OFF)` in a build without it.
`http_size`, `http_body` and `http_close` keep their recipe 41 rows (`Feature::Web`): they now take the server's
handles too, which is the library's business, not the table's. `Effects.cpp` needs nothing: the three classes are
recipe 38's, and `http_accept` is the second real builtin in `Blocks`. `http_is` returns the library's 0 or 1 as an
`i32`, as `Result::Bool` already did for the parsers' `json_bool`.

`http_listen()` takes no argument in the language, and three in the library: `lowerRuntimeBuiltin` replaces its
arguments with `listenArguments()`, the program's web context (recipe 41's global, which `http_listen` now defines
too) and two nulls and a zero for the route table, which is not known yet (below).

Three new parameter kinds, each a string literal (`isString`, as recipe 38 made them):

- **`Param::Route`**: checked by `checkRoute`, recorded by `recordWebRoute`, passed as a string is (its text and
  length).
- **`Param::QueryName`**: checked by `checkQueryName` (letters, digits, `_`, `-` and `.`, 1 to 64 bytes), passed as a
  string.
- **`Param::ReplyFormat`**: a format, like `Param::Format`, whose values follow it, but whose places are `printf`'s
  `%d`, `%g` and `%.Ng` rather than ints alone: so it has its own scan (`scanReplyFormat`) and its own arguments
  (`replyArguments`, `replyValues`), below.

### The route literal: its check, its record and the table

`scanRoute` reads a literal as the library will match it: a method of `GET`, `POST`, `PUT`, `PATCH` or `DELETE`, one
space, and a path of `/` and segments of the characters rule 2 lets a request's path hold, with `%d` its only
conversion. It refuses, each at its byte, what could never match a request, since a route that silently never matches
is a bug the compiler can see: `HEAD` and `OPTIONS` (a `GET` route answers `HEAD`, and the library answers `OPTIONS`),
an empty, `.` or `..` segment (a request's path never has one), `?` (a route is a path alone), `%%` (no request's path
holds a `%`), more than 8 `%d` (the library keeps 8), two `%d` together or one before a digit (each takes all the
digits there are, so the second could never have any). `checkRoute` reports the first, with the note `a route takes
%d, a decimal number of the path` for an unknown conversion, as recipe 38's format does.

**The record** is recipe 41's again: `recordWebRoute` adds the call's method, its path as a pattern (each `%d` a `*`),
its file, line and column to the module's named metadata `fminus.web.routes`, which `ir::webRoutes` reads back and
`Program::webRoutes()` returns.

**The table** is what makes the library answer 404 and 405 itself: every route literal of the program, each once,
handed to the library by `http_listen`, so that only a request some route names reaches the program's code. It cannot
be made where `http_listen` is lowered, since an `http_is` may come after it (in a later function, or later in the
same one). So `lowerRuntimeBuiltin` keeps each listen call (`webListens_`) and each route's text (`webRoutes_`), and
`finishWebRoutes()`, which `lower` calls once every function is lowered and before the effects are checked, makes two
private constants, `fminus.web.routes` (`[N x ptr]`, each a private constant of the literal's bytes) and
`fminus.web.route.lengths` (`[N x i32]`), and patches every listen call's last three arguments. A program with no route
passes null, null and 0, and its server answers 404 to everything that is not a file. The library trusts no byte of the
table: a literal that is not a route there never matches.

### The reply's format

`printf` is the format a person reading a reply expects, and the one clang already has words for. `scanReplyFormat`
reads `%d`, `%g`, `%.Ng` (N from 1 to 17: `%.9g` reads back as the same float, `%.17g` as the same double) and `%%`, and
calls anything else clang's `invalid conversion specifier`, plus one message of its own, for a precision out of range.
`replyArguments` then checks each value against its place, in clang's words (`format specifies type 'double' but the
argument has type 'int'`, `more '%' conversions than data arguments`, `data argument not used by format string`), and
converts it as C passes `printf`'s arguments: an `int` or a `bool` to a double exactly (`sitofp`), a float widened
(`fpext`), a double as it is. A `%g`'s argument is lowered as a double parameter's is (`lowerAs`), so `0.1` is C's
double `0.1`. `replyValues` stores them into an array of doubles in the function's frame, and the call passes its
address and count after the format's text and length.

*Why doubles, and not the ints and floats apart:* one array of one type is one parameter, and every value an `int`, a
`float` or a `double` can hold is a double exactly, so nothing is lost; the library reads `%d`'s back as C converts a
double to an int (saturated, NaN 0, which the compiler's check makes unreachable). It writes the text with
`std::to_chars` into the slot's buffer: the same in every locale, and no allocation.

### What the library's functions touch, told to LLVM

| Kind | Functions | Attributes |
|---|---|---|
| `ServerListen` | `listen` | `nounwind`, `willreturn`; argument and inaccessible memory read and written, other memory read (the literals the table points to), errno read and written; both table pointers `readonly captures(none)`; `noundef` result, not `noalias` (the shared refused server, or the context's server again) |
| `WebRead` | `port`, `arg` | recipe 41's reads: `nofree`, argument and inaccessible memory, `captures(none)`; not `readonly` |
| `ServerTake` | `next` | `nofree`, `willreturn`, argument and inaccessible memory read and written, `noundef` result: two calls are two requests |
| `ServerWait` | `accept` | the same, without `willreturn`: `http_accept(server, -1)` may wait for ever |
| `ServerMatch` | `is` | as a take, and the route `readonly captures(none)`: it writes the numbers it took into the request, which `http_arg` reads |
| `ServerQuery` | `query_float`, `query_int` | as a read, and the name `readonly captures(none)` |
| `ServerReply` | `reply`, `reply_bytes`, `reply_text` | `nofree`, `willreturn`, the request `captures(none)`, every other pointer `readonly captures(none)`; memory left as it is: any |

The replies are the case to get right this time. A reply hands the request back: the I/O thread sends it and reuses the
slot's memory, the request's body among it, which the program reads through `http_body`'s pointer. LLVM must not move
a load of that body past the reply, and the only way to tell it so is a call that may write any memory. So a reply's
declaration leaves its memory effects alone; what it costs is a little optimisation around a reply, and the narrower
attributes would be a miscompile. The lit test checks both ways: `http_next` and `http_port` stay inside a polling loop
at `-O2`, and a body's load after a reply is a second load. A JIT test checks the polling loop too, with a request that
arrives 50 ms into a loop of two billion looks.

### The host's side

`Program::Options::web.server` is a `WebServerPolicy` (`FMinus.h`, C++17, in every build), which `compile()` copies
into `web::ServerPolicy` only under `FMINUS_HAS_WEB_SERVER` and checks with recipe 41's `web::checkPolicy`, whose
server part (`compileServerPolicy`, `ServerPolicy.cpp`) refuses the first mistake (`fminus::Program: web:
server.listen '0.0.0.0:8080': 0.0.0.0 is the unspecified address: write '*' for every interface`). A program now gets
a context when its host allowed a pattern **or** gave a listen, so one may listen and reach nothing. `webRoutes()`
returns the metadata as `webRequests()` does. `webPort()` asks the context (`Context::port()`, new, under its lock),
and is 0 whenever the port is not positive. `stopWeb()` and `~Program` needed nothing new: `Context::stop()` and the
context's destructor close the server (detached from the I/O thread, its sockets closed, every thread in `http_accept`
woken and waited for).

### The driver and executables

Recipe 41's way, again: the driver's options (`--web-listen ADDR`, and with it `--web-token`, `--web-host`,
`--web-root`, `--web-open`), for `--run` only, each refused without `--web-listen`, a policy mistake named by the
option it is in; and an executable's environment, read by `fminus_web_main_start` (`Main.cpp`): `FMINUS_WEB_LISTEN`,
and with it `FMINUS_WEB_TOKEN`, `FMINUS_WEB_HOSTS`, `FMINUS_WEB_ROOT` and `FMINUS_WEB_OPEN`, a mistake the exit status
2 with the variable named. `main_start` now makes a context when either `FMINUS_WEB_ALLOW` or `FMINUS_WEB_LISTEN` says
something, and `addEntryPoint` needed nothing new: `http_listen` defines the context global, which is what it looks
for. A root directory is read by the library's `readServerRoot` (`ServerRoot.cpp`), once, for both: regular files only,
no link followed (on POSIX each directory and file is opened beneath the directory that listed it, with `openat` and
`O_NOFOLLOW`, so a directory swapped for a link mid-walk cannot lead out of the root), hidden names and unservable paths
skipped with a note, an `index.html` at its directory's path too,
the type from a table of thirteen extensions, and limits (256 files, 16 MiB, 4096 entries).

### What it changes for programs and hosts

- **Programs** that use none of it compile to the same IR. The eleven names are builtins in every build (a program's
  function of one of them is an error); `__FMINUS_WEB_SERVER__` cannot be defined.
- **Hosts** get `Options::web.server` (default: nowhere), `webRoutes()` and `webPort()`, in every build, and
  `FMINUS_HAS_WEB_SERVER` in `fminus/Config.h`. Nothing listens unless a host gives an address.
- **The build** is unchanged by default; `FMINUS_WEB=ON` alone is the client of recipe 41, fetching nothing more. With
  the server on, a first configure fetches llhttp's 40 KB archive.
- **The VS Code grammar** (0.18.0) colours the server's builtins and a reply format's `%g` and `%.Ng`.

### Tests

- `tests/unit/WebServerTest.cpp` (`[server]`), compiled only with the option: the policy (every refusal, every form
  accepted) and its classifiers (loopback, the bad ports, a servable path, `Host` and `Origin`, the LAN and IPv6 cases
  among them, judged and never bound); handle kinds crossed with every function; the route matcher; `%g` against
  `snprintf` over 20 values and 4 formats; then the engine over loopback, from a raw socket and from the library's own
  client: each rule and its status, a request a byte at a time, two in one write, keep-alive (with `Connection:
  close`, HTTP/1.0 and the idle limit), connections dropped mid-request and mid-reply, a reply over the limit, two
  clients and four takers, closing with idle connections, a held request and threads in `http_accept(-1)`; and the
  proof that the wait-free functions are: `HeapCalls` counts no allocation, and children under seccomp's strict mode
  make the same calls and send back the expected sums.
- `tests/unit/HttpServerTest.cpp`, in every build: each builtin's effect through the table and through the effects'
  errors; each name's error in a build without the server; then, with it, the route errors at their columns, the reply
  format's and the query name's, the route table (each literal once, a route lowered after the listen), the
  declarations' attributes, `webRoutes()` over five call sites and an `#include`, and `Options::web.server` checked by
  `compile()`.
- `tests/unit/WebServeTest.cpp`: a module served through the JIT, its `process` taking requests a host thread drives
  every millisecond while another `Program`'s client posts and gets (a gain set and read back as JSON, a table posted
  as bytes and read back, 404 and 405 from the library, the host's file); a polling loop that sees a late request; a
  loader in `http_accept` woken by `stopWeb()`; a `Program` destroyed with its server open; an executable started with
  `FMINUS_WEB_LISTEN`, `FMINUS_WEB_ROOT` and `FMINUS_WEB_TOKEN` and driven over loopback, and refused without them;
  `readServerRoot`, and a directory exchanged with a link to elsewhere, over and over, while it reads; and the
  wait-free proof again, of a `[[clang::nonblocking]]` handler the JIT compiled (`HeapCalls`, seccomp's strict mode).
- `tests/lit/fminus/web-server.fm`: the calls, the table and the declarations' attributes at `-O0`, the polling loops
  and the body's reload at `-O2`, `--run` without a listen (`HTTP_REFUSED`), with one (the program asks itself and
  answers on the same thread), with a token (401), and an executable with and without `FMINUS_WEB_LISTEN`;
  `web-server-errors.fm`: each error once, and the driver's; `web-server-off.fm`: each name's error and the driver's
  options in a build without the server.
- `examples/remote/`: `--check` listens on 127.0.0.1, drives the program with the library's client, and checks that a
  gain and a note sent over HTTP are heard in the output, and that the default policy listens nowhere.

## Recipe 43: the file interface, files a program reaches only through its host's folders

*In the compiler. Code: the runtime's part, `src/runtime/Files.h` (the host's half: `fminus::files::Policy`,
`checkPolicy`, `Context`, the status codes' values), `Files.cpp` (names, policies, contexts, quotas, handles, `reach`,
the C functions, an executable's start and stop), `FilesSystem.h`, `FilesPosix.cpp` and `FilesWindows.cpp` (the
operating system's part), and their lines in `Runtime.h` and `RuntimeSymbols.def`; the `file_` entries of
`kRuntimeBuiltins`, `Feature::Files`, `Feature::FilesWrite`, `Param::FileName`, `Param::FilePattern`, the `File*` kinds of
`kRuntimeFacts` and their lines in `lowerRuntimeBuiltin` (`src/ir/Lowering.h`, `Calls.cpp`); `src/ir/Files.cpp` (a
name's check, `scanFileName`, its record, the context global, the status codes' names) and its hooks in `named`
(`Functions.cpp`), the reserved names (`Functions.cpp`, `Constants.cpp`, `ir::kFilesPrefixes` in `CodeGen.h`) and the
undeclared-identifier errors (`Memory.cpp`, `Constants.cpp`); `__FMINUS_FILES__` and `__FMINUS_FILES_WRITE__`
(`Preprocessor.cpp`); `Program::FilePolicy`, `Options::files`, `FileUse` and `files()` (`include/fminus/FMinus.h`,
`src/api/FMinus.cpp`); an executable's context (`support/Aot.cpp`); the driver's `--files-read`, `--files-write`,
`--files-deny`, `--files-types` and `--files-anywhere` (`tools/fminus/main.cpp`); `FMINUS_FILES`, `FMINUS_FILES_WRITE`,
`FMINUS_FILES_ANYWHERE`, `FMINUS_FILES_TYPES` and `FMINUS_FILES_MAX_BYTES` (`cmake/FMinusOptions.cmake`,
`cmake/templates/Config.h.in`, the root `CMakeLists.txt`'s feature loop) and the `files` preset.
Tests: `tests/unit/FilesTest.cpp` (`[runtime][files]`: the runtime from C++, before any builtin),
`tests/unit/FileBuiltinsTest.cpp` (the builtins, the host and programs, in every build), `tests/library/FilesPolicyTest.cpp`
(the host API in C++17, in every build), `tests/lit/fminus/files.fm`, `files-errors.fm`, `files-write.fm`,
`files-anywhere.fm`, `files-off.fm`, `files-write-off.fm`, `files-anywhere-off.fm` and `files-types.fm` (lit's new
features `files`, `files-write`, `files-anywhere` and `files-types`); the example, `examples/kit/`. Built with `-Werror`
and tested in the `release` preset on Linux x86-64 only, in four trees: the default (reading), one with
`FMINUS_FILES_WRITE` and `FMINUS_FILES_ANYWHERE`, one with `FMINUS_FILES=OFF` and one with the web library and its
server, every test passing but the one this machine cannot run (the smoke test of LLVM's own `opt`, as for recipes 38
to 42); the runtime and its test under GCC's AddressSanitizer with UndefinedBehaviorSanitizer and under
ThreadSanitizer, outside the presets, with a race harness and a fuzzer of names; not yet in `dev`, `files` or `asan`,
nor on macOS or Windows, whose code was written and never compiled for them (`FilesWindows.cpp` was type-checked on
Linux against stub headers written from the SDK's declarations, which found a `HANDLE` passed where an integer was
taken: a default Windows build would not have compiled). [files.md](files.md) is its manual, and
[research/files/README.md](research/files/README.md) the report it builds.*

The fourth family of recipe 38's kind, and the first that is part of every build. A program loads what it plays from a
folder its host maps, off the audio thread, and plays it from memory:

```c
int load()                                               // off the audio thread
{
    file_listing kit = file_list("kit/*.f32");           // the folder "kit" is wherever the host says
    const int status = file_status(kit);
    for (int i = 0; i < file_count(kit) && i < 8; ++i)
    {
        file_content f = file_read_nth(kit, i);          // the whole file, in memory: refused in a nonblocking function
        length[i] = bytes_floats(file_data(f), file_size(f), table[i], 4096);
        file_close(f);
    }
    file_close(kit);
    return status;
}
```

It is the report's recommendation as its section 0 chose it: a folder the host maps and a path inside it, never a
path of the machine; no files by default; reading in the default build, writing and absolute names off; every link
refused; types by extension, an allow-list; listings in the first version; the code in the runtime library; and no
decoder (files give bytes).

### The runtime: a part of it, behind three switches

Nothing here needs a dependency, so it is not a library of its own as the web's is: it is more of `fminus_runtime`
(`src/runtime/Files*.cpp`, globbed), which an executable links already. It was built first, and tested from C++ before
any builtin existed (`FilesTest.cpp`), as recipe 42's engine was.

**The switches compile code out**, not merely turn it off. `FMINUS_FILES` guards every file of it (`#if
FMINUS_HAS_FILES` around the whole), so a build without it has no `fminus_rt_file` symbol and imports no `openat`;
`FMINUS_FILES_WRITE` guards everything that creates, writes or renames (so a reading build imports no `write`,
`renameat`, `unlinkat` or `fsync`: no bug can make it write), and `FMINUS_FILES_ANYWHERE` the `realpath` that absolute
names need. The other two options are not switches but ceilings, values in `fminus/Config.h` (`FMINUS_FILES_TYPES`, a
string, and `FMINUS_FILES_MAX_BYTES`), which configure checks (`cmake/FMinusOptions.cmake`: each type 1 to 16 letters
and digits, lowered and deduplicated; the bytes 1 to 2147483646) and both the runtime and the compiler read. The
parsers' build-time limits (`FMINUS_JSON_MAX_BYTES`) were the model.

**The host's half is C++, the program's C.** `Files.h` has `Policy` (the report's §6 struct), `checkPolicy` (every
refusal a message naming the field: `folders: 'kit': write is set, and this build of FMinus leaves out writing files
(FMINUS_FILES_WRITE=OFF)`) and `Context`, which opens each folder's directory once and holds it, holds each denied path
that exists, and keeps the quotas and the handles under one mutex. `Runtime.h` has the ten C functions generated code
calls, with the context's handle first and the name's literal, length, ints and count after it, as the web library's
`get` takes its URL.

**Handles know their kind**, as recipe 42 made the web library's: a 32-bit word first, `FMFf` for a file and `FMFl`
for a listing, which every function reads before anything else. After it come the three fields the wait-free reads
load (its status, its size or count, its data), which never change once the handle is returned until it is closed. A
failure is not allocated: one shared handle per kind and status (`failures[2][6]`, `constinit`), never counted against
`maxOpen`, which `file_close` recognises by its having no owner. So `file_read` never returns null, even when memory
runs out. A closed handle's memory is not freed but kept by its context for the next handle (`spare`), its kind a third
word, `FMFx`: a second close, or a read after the close, finds that word on any allocator, where freed memory's old kind
word could still pass for a handle's (a reviewer's double close: harmless with glibc, whose free list overwrites the
word, and a double free in the context's list of handles with an allocator that does not).

### One way in: `reach`

The report's lesson from Node's and Deno's advisories was that a check left off one operation is the bug, again and
again. So every operation goes through one function, `Context::State::reach`, which takes the name's text with its
`%d`s filled in and returns the directory its file is in, opened, or why not:

1. **The name, in full** (`parseName`): the report's grammar with strict UTF-8, 1024 bytes and 16 segments, the extension
   the literal wrote. The compiler checked the literal; the runtime trusts none of it.
2. **The folder** by its exact name, or an absolute name only with `FMINUS_HAS_FILES_ANYWHERE`, the policy's `anywhere`
   and an operation that reads. A write needs the folder's `write`.
3. **The type**: the folder's list, else the policy's, and always the build's.
4. **The deny-list's identities now**: the pinned ones, what is at each path (`os::lookUp`), and everything a look has
   ever found at one (`seen`, at most 4096, past which everything is refused). A path that cannot be looked up, for a
   reason other than absence, refuses everything: it fails closed.
5. **The folder and its ancestors** compared with them (`os::ancestors`: `..` opened beneath each directory to the
   root, only when there is a deny-list).
6. **The walk**, one segment a call (`os::openDirectory`), each opened beneath the last without following a link and
   compared with the denied identities; then the deny-list's paths looked at again (`settled`), and any change since 4
   refuses the operation. `read` and `list` look once more after their own call.

A deny path with nothing at it when the context is made has no identity to pin. The first version looked it up at each
open and forgot what it found, and a reviewer read a denied directory made there afterwards, by renaming it (every
time) and by exchanging it with a public one while names were walked (some 9,000 times in 30 seconds, through the deny
path's own name too). Remembering what was found, and refusing an operation during which what is at a deny path
changed, closes both; what is left, a directory made there and moved away before any operation looked, takes someone
who can write in the folder. A second name for an entry is the other trap the walk meets: FAT and NTFS also find an
entry by its 8.3 short name (`SSH~1` for `.ssh`), so on any file system but the native ones of Linux and macOS each
segment must be one its directory lists (`os::nameListed`), and on Windows the opened object's long name must be the
segment (`GetFinalPathNameByHandleW`).

Then the operation's own call of the system's layer: `os::readFile` (look with `fstatat` before opening, so only a
regular file is ever opened; open without blocking or following; look again with `fstat`, and compare; read to the limit
plus one byte), `os::listDirectory`, `os::replaceFile` (after the context's first replacement in a directory has removed
the temporary files dead writes left there, an hour old: `os::sweepTemporaries`) or `os::appendFile`. `FilesSystem.h` is
the whole of what differs between systems, a dozen functions over an `os::Dir` (a descriptor, or a `HANDLE`) and an
`os::Identity`: `FilesPosix.cpp` for Linux and macOS (`O_PATH` on Linux, `O_NOFOLLOW_ANY` on macOS), `FilesWindows.cpp`
on `NtCreateFile` with `RootDirectory` and `OBJ_DONT_REPARSE`. Each segment the layer is given is checked once more
(`plain`: never empty, `.`, `..`, nor holding a separator or a NUL), the last word before the kernel's: cap-std's
critical bug of 2026 was a walk that handed the kernel a final segment with a trailing slash.

*Why not `openat2` with `RESOLVE_BENEATH | RESOLVE_NO_SYMLINKS`*, which does the walk in one call on Linux 5.6 and later:
the walk must stop at each directory anyway, to compare it with the denied ones, and a container's seccomp profile may
refuse the call (Docker's did, before 20.10). One code path on every kernel is the one that gets tested.

### The builtins' table rows

| Builtin | Parameters | Result | Effect |
|---|---|---|---|
| `file_read` | `FileName` | `Pointer` | allocates |
| `file_list` | `FilePattern` | `Pointer` | allocates |
| `file_read_nth` | `Handle`, `Int` | `Pointer` | allocates |
| `file_close` | `Handle` | `Void` | allocates |
| `file_status`, `file_size`, `file_count` | `Handle` | `Int` | wait-free |
| `file_data` | `Handle` | `Bytes`, a `const void*` | wait-free |
| `file_write`, `file_append` | `FileName`, `Bytes`, `Int` | `Int` | allocates |

The reads name `Feature::Files`, the two writes `Feature::FilesWrite`, so `isBuilt` asks `FMINUS_HAS_FILES` or
`FMINUS_HAS_FILES_WRITE`, and `notBuilt` says which option is missing: in a build with no interface at all, even
`file_write` gets the interface's message (`'file_write' needs the file interface, ...`), since that is what a program
lacks first. `Effects.cpp` needs nothing: no builtin here blocks in the effects' sense (a disk's wait is the allocating
class's, refused in both kinds of function, as `http_get` is), and the reads are wait-free.

**`Param::FileName` and `Param::FilePattern`** are recipe 41's `Param::Url` again, with a check of their own: a format
whose ints come last (`trailing`), so `file_write("takes/take%d.f32", data, bytes, n)` lowers to
`fminus_rt_file_write(context, text, length, ints, count, data, bytes)`, the ints before the other parameters in the C
call though after them in the builtin's. Before the text, the program's file context, loaded from the module's global
(`filesContext`).

### The name literal: its check and its record

`scanFileName` (`src/ir/Files.cpp`) reads a literal as the runtime will read the name, each `%d` as the one digit it
writes at least (so a name's length is counted at its least, and `COM%d` is a device name), and reports each mistake at
its byte: the folder's characters, an empty segment, a control character or one of `\ : * ? " < > |` (C1 controls
among them), bad UTF-8, a leading `.` or space and a trailing `.` or space, a device name of Windows in any case and
spelling, an extension of 1 to 16 letters and digits that the literal writes, 255 bytes a segment, 16 segments, 1024
bytes, and for a listing one `*`, in the file part, before the extension. Each is an error where the runtime would
refuse anyway: what the compiler can see, it says where it is written.

**The build's ceilings are its arguments** (`FileNameRules`): `FMINUS_FILES_TYPES`, `FMINUS_FILES_ANYWHERE`, whether
the target is Windows (from the triple), and whether the call writes. So a type the build cannot touch, an absolute
name in a build without them, the other system's form of one, and an absolute name written are compile errors, and a
unit test can give `scanFileName` the rules of any build, Windows's among them, without building one.

**The record** is recipe 41's: `recordFileUse` adds the operation (the builtin's name after `file_`), the pattern (each
`%d` a `*`, each `%%` a `%`), the file, line and column to the named metadata `fminus.files`, which `ir::files` reads
back and `Program::files()` returns, before any of the program's code has run.

**The status codes** are recipe 41's too: `named()` falls back to `fileStatus`, after `webStatus`, so `FILE_REFUSED`
folds and labels a `case`, and a program's own declaration hides it; in a build without the interface,
`unbuiltFileStatus` makes the name the option's error.

### What the runtime's functions touch, told to LLVM

| Kind | Functions | Attributes |
|---|---|---|
| `FileOpen` | `read`, `list` | `nounwind`, `willreturn`; `memory(argmem: readwrite, inaccessiblemem: readwrite, errnomem: readwrite)`; the name and the ints `readonly captures(none)` (the context's pointer is captured: the handle keeps it); `noundef` result, not `noalias` (a failure is a shared handle) |
| `FileNth` | `read_nth` | the same, the listing `captures(none)` |
| `FileWrite` | `write`, `append` | as `FileOpen`, and the bytes `readonly captures(none)`; no result attribute |
| `FileRead` | `status`, `size`, `data`, `count` | `nounwind`, `willreturn`, `nofree`, `nosync`, `memory(argmem: read, inaccessiblemem: read)`, the handle `readonly captures(none)` |
| `FileClose` | `close` | `nounwind`, `willreturn`, the handle `captures(none)`; memory left as it is: any |

The reads are the opposite of recipe 41's. A web request's fields change under the program, so its reads must not be
merged; a file handle's never change once it is returned, so they are `readonly` and `nosync`, and LLVM merges two
`file_size` calls of one handle with nothing between (`files.fm` checks it at `-O2`). A `file_read` between them is a
write to the context's inaccessible memory, and the read after it is made again, which is right: nothing promises the
two handles are different. `file_close` is recipe 42's reply again: it frees a file's bytes, which the program reads
through `file_data`'s pointer, and only a call that may write any memory keeps a load of them from moving past it.

### The host's side

`Program::FilePolicy` (`FMinus.h`, C++17, in every build) is `Policy` as the host writes it. `compile()` copies it into
`files::Policy` only under `FMINUS_HAS_FILES` and checks it with `checkPolicy` on every compile, whether or not the
program names a file (`fminus::Program: files: <message>`), so a host learns of a policy its build cannot honour at
once. The context is made only when the program has the context global and the policy grants a folder or `anywhere`,
which is when the folders' directories are opened (a missing one fails `compile()`); otherwise the global stays null,
and the runtime refuses every name. The `Impl` destructor resets the context before the JIT goes: every handle the
program still holds is freed and the folders are closed. No function or constant of a program may be named
`fminus_rt_file_*` or `fminus_rt_files_*` (`kFilesPrefixes`), in every build, so a program reaches the runtime only
through its literals and `files()`, and never an executable's `fminus_rt_files_main_start`, which would make a context
from the process's environment.

### The driver and executables

Recipe 41's way again, default-deny: `fminus --run` builds a `files::Policy` from `--files-read` and `--files-write`
(`NAME=DIR`, or a `DIR` named after its last segment), `--files-deny`, `--files-types` and `--files-anywhere`, checks it
with `checkPolicy` before anything compiles, makes the context after the JIT (so it is destroyed first) and stores its
handle in the global; a mistake is named by the option it is in (`filesOptionOf`). An executable's C `main`
(`addEntryPoint`) calls `fminus_rt_files_main_start(&fminus_rt_files_context, argv[0])` before the web's start, so a
mistake in either stops it before any request is made, returning its non-zero result (2, with the variable and the
reason on stderr), and `fminus_rt_files_main_stop` after the web's stop. `RuntimeSymbols.def` lists those two through
`FMINUS_FILES_MAIN_SYMBOL`, which names nothing by default (so `JitRuntime.cpp` defines neither) and which
`Constants.cpp` defines (so `kRuntimeNames` reserves them), as `WebSymbols.def`'s `FMINUS_WEB_MAIN_SYMBOL` does. An
executable links nothing new: the code is in `libfminus_runtime.a`.

### What it changes for programs and hosts

- **Programs** that name no file compile to the same IR. The ten names are builtins in every build (a program's
  function of one of them is an error), and so are the seven `FILE_` names, which a program's own declaration hides;
  no function or constant may be named `fminus_rt_file_*` or `fminus_rt_files_*`. `__FMINUS_FILES__` and
  `__FMINUS_FILES_WRITE__` cannot be defined.
- **Hosts** get `Options::files` (default: no files) and `files()`, in every build, and `FMINUS_HAS_FILES` and its kin
  in `fminus/Config.h`. Nothing is read unless a host maps a folder.
- **The build** is unchanged in what it fetches: nothing. The default build compiles the reading half into the runtime;
  `-DFMINUS_FILES=OFF` compiles none of it.
- **The VS Code grammar** (0.19.0) colours the `file_` builtins and the `FILE_` names.

### Tests

- `tests/unit/FilesTest.cpp` (`[runtime][files]`), compiled with `FMINUS_FILES` on every system but Windows, its
  seccomp case and its swap race on Linux alone: each trap built for real in a directory the test makes beside `bin/`
  and removes (links of every kind and place, hard links with and without `singleLink`, a FIFO that must not hang, a
  directory where a file was named, a file that grows while it is read, a file one byte over the limit, a folder
  renamed after the context was made, the deny-list reached by another spelling, through a link, made afterwards and
  renamed afterwards, a directory made at a denied path afterwards and then renamed, or exchanged with a public one
  while names are walked, a denied path that cannot be looked up), a second name a file system answers to
  (`os::nameListed` alone, then every name of every case's kind checked so: `os::checkEveryName`, a test hook), closed
  handles read and closed twice, the temporary files of writers killed half way and their sweep, over 70 names refused
  and 22 allowed, types in every
  letter case, every reason `checkPolicy` refuses, the limits and quotas, listings (their order and what they skip),
  absolute names with the deny-list on the real path and `/dev/null` and `/proc` refused, writes failed at each of their
  six steps (`os::failNextReplaceAt`, a test hook compiled only with writing, since the tests run as root and
  permissions cannot fail them), two contexts writing one file while a reader checks it is never torn, a directory
  swapped for a link while names are walked, `FILE_DENIED` from a forked child that gives up root, an executable's
  environment, and the wait-free proof (`HeapCalls`, and children under seccomp's strict mode).
- `tests/unit/FileBuiltinsTest.cpp`, in every build: each builtin's effect through the table and through the effects'
  errors (directly and through a called function); each name's error in a build without the interface or its writing;
  then, with it, the status codes against `Files.h`'s values, the name errors at their columns, `scanFileName` given
  the ceilings of other builds (types, absolute names, Windows), `files()` over several call sites and an `#include`,
  `Options::files` checked by `compile()`, the reserved names, the JIT's symbols; a kit listed, loaded and played
  through the JIT (and a table saved and compared, with writing), the same as an executable with `FMINUS_FILES_READ`,
  absolute names, the wait-free proof through a `[[clang::nonblocking]]` function the JIT compiled, and a `Program`'s
  destructor closing its folders (counted in `/proc/self/fd`) and freeing what its program held.
- `tests/library/FilesPolicyTest.cpp`: `Options::files` and `files()` from a C++17 host.
- `tests/lit/fminus/files.fm`: the calls, the context global and the declarations' attributes at `-O0`, the reads
  merged and the closes kept at `-O2`, the metadata, `--run` with `--files-read` in both spellings, `--files-types` and
  `--files-deny`, an executable with and without `FMINUS_FILES_READ`, and the driver's mistakes; `files-errors.fm`: each
  name error once; `files-write.fm` and `files-anywhere.fm`: writing and absolute names through the driver and an
  executable; `files-off.fm`, `files-write-off.fm` and `files-anywhere-off.fm`: each in the build it describes;
  `files-types.fm`: in a build whose `FMINUS_FILES_TYPES` lists types (`%files_types`).
- `examples/kit/`: `--check` writes four drum sounds into a folder of its own, plays them through the program and
  compares the samples with C++ (identical), and checks that a denied subfolder, a type not allowed and the default
  policy read nothing, and, with writing, that a take is saved byte for byte and refused in a folder granted for
  reading alone.

## Recipe 44: typed handles, `defer`, and `if` and `switch` with a declaration

*In the compiler. Code: the handles, the `HANDLE_TYPE` token and its eight words (`src/frontend/Lexer.l`,
`Parser.y`), `Kind::Handle`, `ast::Handle`, `Type::handle`, `Type::handleType`, `isHandle`, `canBeElement`,
`handleName` and `handleStruct` (`src/frontend/Ast.h`, `Ast.cpp`), their type, size, tag and conversions
(`src/ir/Types.cpp`, `Tbaa.cpp`), their operators and the incomplete type's errors (`handleOperands` and `lvalue` in
`Memory.cpp`, `binary` and `lowerConditional` in `Expressions.cpp`, `lowerMethodCall` in `Modules.cpp`), the builtins'
handle `Param`s and `Result`s, `otherType` and the plan's type (`src/ir/Lowering.h`, `Calls.cpp`), a builtin's
prototype (`Functions.cpp`), and a call by name (`jit::callType`, `src/jit/Call.cpp`). `defer`: its two tokens and its
rule, `DeferStmt` and its dump, `Diagnostics::mark` and `rewind` (`src/frontend/Diagnostics.h`), and its lowering in
`src/ir/Statements.cpp` (`lowerBlock`, `openDefers`, `closeDefers`, `reachDefer`, `runDefers`, `lowerDeferred`,
`deferAround`; `Deferred`, `DeferScope` and `Breakable::Kind::Defer` in `Lowering.h`), the function body's block
(`Functions.cpp`), a parallel block's refusal and the race checks' walk (`Parallel.cpp`), and the host's records left
alone while a deferred statement is lowered again (`relowering_`: `Web.cpp`, `WebServer.cpp`, `Files.cpp`). `if` and
`switch` with a declaration: `selection_head` and `Declarator::comma` (`Parser.y`), `IfStmt::init` and
`SwitchStmt::init`, their lowering (`lowerIf`, `lowerSwitch`) and their walk (`Parallel.cpp`). Tests:
`tests/unit/HandleTest.cpp`, `DeferTest.cpp`, `SelectionDeclarationTest.cpp`, three cases of `FrontendTest.cpp`,
`tests/library/HandleTest.cpp`, `tests/lit/fminus/handles.fm`, `defer.fm` and `selection-declarations.fm`. The
examples that hold handles say their types (`remote/`, `fetch/`, `kit/`, `wavetable/`, `convolver/`). Built with
`-Werror` and tested in the `release` preset on Linux x86-64 only, in two trees, the default and one with the web
library, its server, file writing and absolute names, every test passing but the one this machine cannot run (the smoke
test of LLVM's own `opt`, as for recipes 38 to 43); not yet in `dev` or `asan`, nor on macOS or Windows.
[research/syntax/README.md](research/syntax/README.md) is the report; this is its first step, its §3.*

Three of C's features, none of them FMinus's invention and none needing a change to the runtime: a type for each kind
of handle, C's opaque handles in place of `void*`; ISO/IEC TS 25755's `defer`; and C2y's declaration in an `if`'s or a
`switch`'s head. Together they make a loader's cleanup one line a resource, on every path:

```c
module Loader
{
    float gain;

    int load(int preset)                                         // off the audio thread
    {
        http_response r = http_get("https://example.com/presets/%d.json", preset);
        defer http_close(r);                                     // on every way out of load
        if (http_wait(r, 10000) != 200)
            return -1;
        if (json_doc doc = json_parse(http_body(r), http_size(r)))   // C2y: the condition is doc
        {
            defer json_free(doc);
            gain = json_float(doc, "/gain", 1.0f);
            return 0;
        }
        return -2;                                               // doc is out of scope here
    }
};
```

`http_size(doc)` there would be an error where it was `HTTP_INVALID` at run time, and the program would mean the same to
a C compiler that has the TS and C2y's declarations (clang's `-fdefer-ts`, and its `-std=c2y` once a release carries
them), given a header of the builtins' prototypes and eight typedefs.

### Handles: a kind of type, eight of them

**Each word is a type, as `float4` is**: `http_response` (a request the program made), `http_request` (one it
received), `http_server`, `json_doc`, `xml_doc`, `file_content`, `file_listing` and `fft_plan`. The scanner returns each
as `HANDLE_TYPE`, a token whose value is its `ast::Type`, as `VECTOR_TYPE`'s is; `type` takes it, and so does
`attribute_word`, where C23 reads a keyword as a name. Being rules of the scanner, they are keywords that no macro may
be named (`macroNameError` reads a name with the scanner), and no variable either: of the tree's 786 programs the
report measured, none used one (its §2).

**In the tree, a handle is a kind with a field beside it**: `Kind::Handle`, and `Type::handle`, an `ast::Handle`, which
says which. Eight kinds would have been eight cases in every `switch` over a kind; one kind and a field are one, and
`-Wswitch` found each place that needed it (`typeOf`, `sizeOf`, `tbaaTypeName`, `typeName`, `callType`). The field
rides along wherever a kind does: `pointerTo`, `arrayOf` and `pointee` carry it, `==` and `samePointee` compare it, so an
array of `json_doc` is not an array of `xml_doc`. `isArithmetic` is false for a handle, so every operator that takes
numbers refuses one with its own error, and `canPointTo` stays without it: a handle is a pointer, and FMinus has no
pointers to pointers.

**C's handle is a pointer to an incomplete struct**, `typedef struct fminus_json_doc* json_doc;`, and FMinus's does what
C lets one do, in clang's words where it refuses:

| The program writes | FMinus |
|---|---|
| `json_doc d = json_parse(...)`, `void* v = d;`, `d = v;` | a `ptr`, as the `void*` was; a `void*` converts to any handle and any handle to a `void*` (`convertPointer`) |
| `xml_doc x = d;`, `xml_float(d, ...)` | C's warning, FMinus's error: "incompatible pointer types initializing 'xml_doc' with an expression of type 'json_doc'" |
| `d = c;` of a `const void* c` | "... discards qualifiers", as C has it |
| `d == e`, `d != v`, `if (d)`, `!d`, `d && e`, `c ? d : v` | a pointer comparison, a test against null, a `void*` when `?:` meets one |
| `d < e` | "invalid operands to binary expression ('json_doc' and 'json_doc')": C orders two pointers of a kind, FMinus refuses, since their order means nothing |
| `d + 1`, `d++`, `d - e` | "arithmetic on a pointer to an incomplete type 'struct fminus_json_doc'" |
| `d[0]`, `*d`, `d->size` | "subscript of pointer to incomplete type ...", "incomplete type ... where a complete type is required", "incomplete definition of type ..." |
| `json_doc* p;`, `&d`, a parameter `json_doc docs[]` | "FMinus has pointers to float, ..., not to 'json_doc'": a pointer to a pointer |
| `json_doc docs[8];`, a field `http_response pending[4][4];` | an array of `ptr`s, zeroed, `docs[i]` a handle |

`handleStruct` names the struct as C's typedef would, for the four messages that name it. The arrays are the one
place a pointer to a handle exists: an array's name stands for a pointer to its first element, which `element()` and
`pointerArithmetic` (`Memory.cpp`) now take for a handle, though no variable can hold one. `checkType`'s message for an
array of what cannot be an element names handles among what can, and `void*` stays what cannot ("FMinus has no arrays
of pointers").

**In memory a handle is a pointer**: `typeOf` is `ptr`, `sizeOf` 8, and its TBAA tag is "any pointer" (recipe 17), so
a field that was a `void*` and is now an `http_server` aliases as it did. A declaration without an initialiser stores
null, as a pointer's does.

**The builtins say which handle each takes and gives.** `Param::Handle` and `Param::Document` became a parameter for
each kind (`Response`, `Request`, `Server`, `JsonDoc`, `XmlDoc`, `FileContent`, `FileListing`) and three for the
builtins that take either of two: `Message` (`http_size` and `http_body`: a request made or received), `Closable`
(`http_close`: a request made or a server) and `FileHandle` (`file_status` and `file_close`); `Result::Pointer` became a
result for each kind. `paramType` gives the first of an either-parameter's two, `otherType` the second, and
`lowerRuntimeBuiltin` passes the argument as whichever it is; a third kind is C's error with both named ("passing
'json_doc' to parameter of type 'http_response' or 'http_request'"). A JSON or XML read's document is
`Param::JsonRead` or `XmlRead`: a document, which a `const void*` converts to as well, since that is what a document was
before (the read only reads it: C would declare it `const struct fminus_json_doc*`). The FFT's plan, outside the table
(`lowerFft`), is an `fft_plan`. A prototype of a builtin may say the handle, or `void*` as before, or either of an
either-parameter's two (`declare`, `Functions.cpp`). Nothing in the runtime or the web library changed: their functions
take and give pointers, as they did, and still check a handle's kind, for one that reaches them as a `void*`.

**The host sees what it saw for a `void*`.** `jit::callType` passes a handle as a pointer, so `call()` describes a
handle parameter as a `Pointer` to `Void` that takes any non-`const` pointer, `--run` refuses one as it refuses a
`void*`, and a handle result is no value `call()` returns; `Module::Field` gives a handle field's type by its name
(`"json_doc"`, `"json_doc[4]"`, element `"json_doc"`). The documentation's hosts change nothing.

### `defer`: a rewrite when the program is compiled

The TS's rules, which FMinus takes whole where it has what they speak of (it has no `goto` and no `longjmp`):

| | TS 25755 |
|---|---|
| Form | `_Defer` and a statement, its deferred block; FMinus spells it `defer` too, which the TS encourages |
| When | reaching it runs nothing; the statement runs when control leaves the enclosing block, at its end or by `return`, `break` or `continue` |
| Block | the nearest: a compound statement, or an `if`'s or a loop's body without braces, which C11 6.8p3 makes a block of its own, so `for (...) defer f(i);` runs `f` at the end of each iteration |
| Order | the reverse of the order reached; leaving several blocks, the innermost's first |
| Values | nothing captured: the statement reads its variables when it runs; a `return`'s value is computed and converted first |
| Refused | `return`, and a `break` or `continue` that would leave the statement; a `switch` jumping into the statement, or past a `defer` of a block it jumps into |

**The grammar is one rule**, `unlabeled: defer statement`, with `defer: "defer" | "_Defer"` (two tokens, so that an
attribute keeps the spelling it was written in). The TS says an unlabeled statement; clang parses any statement and
reports a `case` label there as the jump it is, and so does FMinus. `DeferStmt` holds the statement, and dumps as
`(defer s)`.

**Each block keeps the defers reached in it.** `lowerBlock` (a `{ }`), `lowerScoped` (a branch, a loop's body, a
switch's) and `define` (a function's body) open a `DeferScope` and close it, as they open and close a scope of names.
`reachDefer` records a `Deferred`: the statement, and what it will see wherever it is lowered, the names of its block
declared so far (a copy of the innermost scope) and the loops and switches around it (how many). An exit lowers what
it leaves: `runDefers(from)` walks the `DeferScope`s from the innermost down to `from`, each one's defers last first,
and `lowerDeferred` lowers each statement there, in its own block, with the scopes after its defer's set aside and its
block's scope as it was at the defer, so `{ defer f(x); float x = 2; }` reads the `x` outside. A block's end runs its
own when control reaches it (`closeDefers`); `return` runs all of the function's, after its value is converted
(`lowerReturn`); `break` and `continue` run those of the blocks inside their target, which each `Breakable` now knows
(`defers`: how many `DeferScope`s were open where it began). With no `goto`, every path to an exit has reached the
same defers, so this needs no flag and no list at run time: the code of each deferred statement is at each exit, as
clang emits it.

```llvm
if.then:                                    ; return -1; after defer close_(1); defer close_(2);
  call void @close_(i32 2)
  call void @close_(i32 1)
  ret i32 -1
if.end:                                     ; return k;
  %k2 = load i32, ptr %k.addr, align 4, !tbaa !2
  call void @close_(i32 2)
  call void @close_(i32 1)
  ret i32 %k2
```

At `-O2`, LLVM merges the copies when it can (`tests/lit/fminus/defer.fm`).

**Lowered once a place, reported once.** A deferred statement is lowered at every exit of its block, and FMinus checks
as it lowers, so a mistake in it would be reported at each. The first lowering of each `DeferStmt` (`deferLowered_`)
reports its errors, records its calls for the effects (`calls_`) and its literals for the host (URLs, routes, files);
each later one is the same code again, so `lowerDeferred` drops what it reported (`Diagnostics::mark` and `rewind`) and
the calls it recorded, and `relowering_` keeps the three recorders from recording again. A defer whose statement no exit
lowered, because its block ended in an error's jump, is lowered once anyway, for its errors, in a block nothing reaches
(`closeDefers`). The effects need nothing else: a deferred call is a call of its function, at the place it is written,
so `defer free(p);` in a `[[clang::nonblocking]]` function is the error `free(p);` is, once.

**The constraints are clang's errors** (`-fdefer-ts`, clang 22's `err_jump_out_of_defer_stmt` and
`err_switch_into_protected_scope`). A deferred statement being lowered is a `Breakable` of its own, `Kind::Defer`, at
the defer's place, on top of the loops and switches around its defer (those after it are set aside too):

| The program writes | FMinus, at |
|---|---|
| `defer return;` | "cannot return from a defer statement", the `return` |
| `for (;;) { defer break; }`, `defer continue;` | "cannot break out of a defer statement", "cannot continue loop outside of enclosing defer statement" (a loop inside the deferred statement may break and continue as it likes) |
| `defer break;` with no loop or switch at all | "'break' statement not in loop or switch statement", which clang checks first |
| `switch (k) { defer f(); case 1: ... }` | "cannot jump from switch statement to this case label", the label, and a note, "jump bypasses defer statement", at each defer reached in a block of the switch's body that holds the label, the last first |
| `switch (k) { default: defer { case 1: ...; } }` | the same error, its note "jump enters a defer statement" |
| `parallel { defer f(); ... }` | FMinus's: each statement of the block is a task, which would run it as soon as it began |

`lowerLabel` finds the second kind of jump from the `DeferScope`s open since its switch began (`Switch::defers`); the
first from the `Breakable` around the label. A `defer` inside a statement's braces in a `parallel { }`, or in a
`parallel for`'s body, is that task's or that iteration's, and runs at its end, by `continue` too; `return` and `break`
out of a region were errors already, and run nothing. The race checks walk a deferred statement as part of the code
around it (`Parallel.cpp`'s `Walker`).

**What a deferred statement ending in an error's `return` leaves**: a block that already ends, which the exit's own
`ret` or `br` must not follow. `runDefers` and `lowerDeferred` move on to a fresh block when it does, as
`lowerStatement` does after a `return`, or the verifier reports a terminator in the middle of a block.

**The loop budget and bounds checks are not exits of a block.** A call that one of them stops returns at once, as it
did, and runs no deferred statement on the way, as the TS says of `exit`, an abort and a signal. They could not: both are
made in the optimised code (recipes 29 and 30), the budget's checks put at the loops' branches back after every other
pass, a failed bounds check's trap made a return there too, and each caller made to return after the call; by then the
blocks the deferred statements belonged to are gone, merged and moved, and code to run there would change what the
optimiser decided, which those passes are built not to do. So what such a statement would have released (a request,
a document) stays held: a stopped call is a program's bug, whose instance its host re-initialises or discards (recipe
29). Whether to change that is open. A `within` block's scope is the one thing such a call must not leave behind,
since it is the thread's and outlives the call: those ways out abandon it (recipe 49).

### `if` and `switch` with a declaration

C2y's (N3356, as N3473 and N3580 refine it), and C++17's before it: the head of an `if` or a `switch` may declare,
`if (int n = count(); n > 0)`, and its names are in scope from their declaration to the end of the whole statement,
the `else` branch included. Its third form, one declarator with an initialiser, `if (json_doc doc = json_parse(...))`,
is the second with the declared name for its condition, which N3580 says it is "exactly as if" written:
`if (json_doc doc = json_parse(...); doc)`.

**The grammar** is `selection_head`, in the parentheses of `if` and `switch` beside the plain expression:
`declaration expr` (the declaration ends at its `;`, and may declare several names), or `qualifier type declarators`,
whose action keeps the first declarator and makes its name the condition, a `VariableExpr` at the name. Bison takes it
with no conflict (428 states, against 400 before this recipe and 408 with the handles and `defer`), because after
`qualifier type declarators` the next token decides: `)` ends the third form, `;` a declaration, `,` another
declarator. Three more alternatives exist only for clang's errors, each also without a conflict:

| The program writes | clang's error, at |
|---|---|
| `if (int a = 1, b = 2)` | "multiple declarations are not allowed", the comma (`Declarator::comma`, which `declarators` now records) |
| `if (int c)` | "variable declaration in condition must have an initializer", the name |
| `if (int d = 1; int e = 2)` | "expected expression", the second declaration |
| `if (x = 1; x)`, `if (; x)` | "first clause in condition must be a declaration", the clause |

N3580's constraint, on storage-class specifiers, has nothing to check: FMinus has none.

**The tree** keeps the declarations as `IfStmt::init` and `SwitchStmt::init` (empty for a plain head), the condition as
the condition, and dumps them before it: `(if (decl int n (call count x)) (> n 0) ...)`. **The lowering** opens a scope
around the whole statement, lowers the declarations there, then the statement as before, and closes it: a name of the
head is gone after the statement ("use of undeclared identifier"), and one of a branch's block may shadow it. A
switch's head is declared before its jump, outside the body's scopes, so no label's `sw.zero` zeroes it (recipe 27). No
`DeferScope` is needed: a `defer` in a branch is the branch's, and has run before the statement ends.

### What it changes for programs and hosts

- **Programs** that say `void*` for a handle compile as they did, to the same IR, unchecked as they were. Error
  messages that named a builtin's `void *` parameter name its handle now (`passing 'float' to parameter of
  incompatible type 'json_doc'`), the FFT's overloads say `fft_plan`, and the message for an array of what FMinus
  cannot keep in one lists handles. `defer`, `_Defer` and the eight handle words are keywords: a program that named a
  variable so must rename it (none in the tree did).
- **Hosts** see a handle where they saw a `void*`, by its name.
- **The VS Code grammar** (0.20.0) colours `defer`, `_Defer` and the handle types.

### Tests

- `tests/unit/HandleTest.cpp` (`[handles]`): through the JIT, an `fft_plan` in a local, a parameter, a result, a
  field, a 1-D and a 2-D array, null until set, compared and tested, chosen by `?:`, and to and from a `void*`; with
  the JSON parser, a `json_doc` read and freed, and the same program with `void*` and `const void*` documents and
  prototypes of the builtins written both ways; the errors of the table above, each once at its column, a `void*`
  array still refused, the wrong kind in a builtin's argument, an either-parameter's message, and a prototype that
  conflicts.
- `tests/unit/DeferTest.cpp` (`[defer]`): the order within a block and across nested ones, TS 25755's examples 2, 4
  and 5, a `return` whose value a deferred statement then changes (and a conversion before it), `break` and `continue`
  from loops and from a switch's block, an unreached `defer`, an unbraced `if`'s and loop's body, a deferred block that
  declares and reads the names its defer saw, a `defer` closing a `json_doc` on every path of a function with three
  exits, a `parallel for` iteration's, each constraint's error at its column, an error and a nonblocking function's
  `free` reported once though lowered at several exits, and a non-void function's missing `return` still found.
- `tests/unit/SelectionDeclarationTest.cpp` (`[selection]`): both forms for `if` and `switch`, several names, the
  `else` branch's view, shadowing, an `fft_plan` as the declared variable, a `defer` in a branch, and the names gone
  after the statement.
- `tests/unit/FrontendTest.cpp`: each form's dump, the handle words as types, as attributes' names and as no macro's
  name, `defer` with a label and with a declaration, and clang's errors for C2y's refusals.
- `tests/library/HandleTest.cpp`: a handle field and arrays of them as `module()` describes them, and a handle
  parameter as `callable()` describes it and `call()` passes it.
- `tests/lit/fminus/handles.fm`, `defer.fm` and `selection-declarations.fm`: the IR at `-O0` (a handle's tag, the
  copies of a deferred call at each exit, a declaration stored before its condition) and at `-O2`, and errors.

## Recipe 45: `serve`, `route` and `reply`, and the JSON literal of scalars

*In the compiler. Code: the four keywords (`src/frontend/Lexer.l`, `Parser.y`: `serve_items`, `route`,
`route_params`, `route_param`, the `json_*` rules, and the words in `attribute_word`), `ServeStmt`, `Route`,
`RouteParam`, `ReplyStmt`, `JsonExpr` and `JsonValue` and their dump (`src/frontend/Ast.h`, `Ast.cpp`); `src/ir/Serve.cpp`
(`lowerServe`, `checkRouteParameters`, `lowerRoute`, `fillParameter`, `lowerReply`, `lowerJsonReply`, `jsonFormat`,
`innermost`), `Breakable::Kind::Route`, `RouteState`, `RouteLiteral` and `RouteHoles` (`Lowering.h`); a route's holes,
its record and the overlap check in `src/ir/WebServer.cpp` (`scanRouteHoles`, `routeIndex`, `recordRouteStatement`,
`checkRouteOverlaps`, called from `CodeGen.cpp`); `lowerRuntimeCall`, the builtins' argument checks for a call whose
first value is given (`Calls.cpp`), and the facts of `fminus_web_http_next_in`, `_accept_in`, `_route` and
`_query_text` (`kRuntimeFacts`); `return`, `break`, `continue` and a `case` label in a route (`Statements.cpp`); the
walks (`Parallel.cpp`, `Constants.cpp`, `Expressions.cpp`); `ir::WebRoute::parameters` and `statement`
(`src/ir/CodeGen.h`), `Program::RouteParameter` and `Program::WebRoute`'s two new members
(`include/fminus/FMinus.h`, `src/api/FMinus.cpp`); and in the library, a reply's format's `%r`, `%R` and `%b`
(`src/web/Server.cpp`, `Web.h`). Tests: `tests/unit/ServeTest.cpp`, a case of `FrontendTest.cpp`,
`tests/lit/fminus/serve.fm` and `serve-errors.fm`, and `web-server-off.fm`'s serve. The example `examples/remote/` is
written with the statements. Built with `-Werror` and tested in the `release` preset on Linux x86-64 only, in two trees,
the default and one with the web library, its server, file writing and absolute names, every test passing but the one
this machine cannot run (the smoke test of LLVM's own `opt`, as for recipes 38 to 44), and since also in the three
trees that leave features out ([testing.md](testing.md), "Builds that leave features out"), one of them with
`FMINUS_WEB` and not its server; not in `dev` or `asan`, nor on macOS or Windows.
[research/syntax/README.md](research/syntax/README.md) is the report, §5 and §6.1; this is its second step.
[web.md](web.md), "serve, route and reply" and "The JSON literal", is the manual.*

FMinus's own syntax, the first since `parallel`: three statements that put the server's builtins (recipe 42) under a
loop and a chain the compiler writes, and a literal that writes JSON without a JSON value. What a program wrote by hand
in recipe 42 is now:

```c
void process(float* out, int n) [[clang::nonblocking]]
{
    serve (server)
    {
        route "POST /note/{note}" (int note [[fminus::range(0, 127)]])   // 404 for /note/x, 400 for /note/128
        {
            pitch = 440.0f * expf(0.05776227f * (float)(note - 69));
            reply 204;
        }
        route "GET /state"
        {
            reply 200, json { gain, pitch, level, changes };
        }
    }
    // ... the block
}
```

and three mistakes that compiled before do not: a request taken and never answered (a route that can end without a
reply), a number read from the wrong `%d` (a hole is its parameter), and two routes that would both take a request
(an overlap).

### Tokens, grammar, nodes

`serve`, `route`, `reply` and `json` are keywords, each a token of its own and a word of `attribute_word`, where C23
reads a keyword as a name. The rules are the report's appendix D's, with three changes, each without a conflict (488
states, against 428 before):

```
unlabeled   : ... | "serve" "(" arg_list ")" "{" serve_items "}" | "reply" arg_list ";" | "reply" ";"
serve_items : %empty | serve_items route | serve_items block_item          the last an error: only a route stands there
route       : "route" string_literal route_params attributes block
route_param : qualifier type pointer IDENTIFIER attributes                 a function's parameter, with C23's attributes
expr        : ... | "json" json_object | "json" json_array
json_member : string_literal ":" json_value | IDENTIFIER
```

A non-route in a serve's braces parses (so the error is FMinus's words, not Bison's), and `reply;` is clang's "expected
expression". A route keeps its literal as written (`{name}` and all), where it begins, its parameters (whether a list
was written, `()` included, for the dump), the attributes after them, its block's items and where its closing brace is
(Bison's location of the block, whose end column is one past the brace): the missing reply's error is there. A
`JsonValue` is an expression, or an object (its keys, each a `StringExpr` with its bytes' places, and its values), or an
array; a bare name is its own key, a `VariableExpr` its value. The dump writes `(serve s 100 (route "PUT /x/{k}" ((int
k)) ...))`, `(reply 200 (json {"gain" gain "taps" [a b]}))`.

### A route's literal

`scanRouteHoles` reads a route statement's literal into the literal the library takes, each `{name}` a `%d`, with the
offset in the source of each byte, and the holes' names: a `%` is an error ("%d is http_is's spelling"), a hole is a C
identifier in braces, never right after another nor before a digit, at most 8. Then the rest is `scanRoute`'s (recipe
42), its errors moved back to the source's bytes and their "%d" said as "{name}". `checkRouteParameters` checks what a
literal's holes and a parameter list must agree on: each parameter an `int`, a `float`, a `bool` or an `http_request`
(one), each name once; a hole's an `int` with no default, another's name one a query may have (64 bytes at most); the
attributes `checkAttributes` checks on a module's parameter (recipe 35), built into a `Field` for it, and an `int`'s
default an integer, a `bool`'s 0 or 1; each hole a parameter's, and a different one's; no attribute after the list. It
returns the parameters' description for the host, which `recordRouteStatement` puts into the route's entry of
`fminus.web.routes` (a sixth operand, a tuple a parameter: name, type, source, and a range's ends and a default as
empty or one-number tuples), which `ir::webRoutes` reads back and `Program::webRoutes()` returns: `statement` and
`parameters`, both empty for an `http_is` call's, which keeps its five operands.

A good literal's library text joins the route table (`routeIndex`, the one `http_is` uses: each text once, so its
index is its position), and its index the serve's **route set**, one bit an index in 64-bit words, a private constant
`serve.routes`, which `fminus_web_http_next_in` and `_accept_in` take with its number of words (the library's track,
the commit before recipe 44: a take of the oldest request published for a route in the set).

### What a serve lowers to

```llvm
  %serve.take = call ptr @fminus_web_http_next_in(ptr %0, ptr @serve.routes, i32 1)
  store ptr %serve.take, ptr %serve.request, align 8, !tbaa !18
  br label %serve.cond

serve.cond:                                       ; preds = %serve.next, %entry
  %request = load ptr, ptr %serve.request, align 8, !tbaa !18
  %serve.none = icmp eq ptr %request, null
  br i1 %serve.none, label %serve.end, label %serve.body

serve.body:                                       ; preds = %serve.cond
  %request2 = load ptr, ptr %serve.request, align 8, !tbaa !18
  %route = call i32 @fminus_web_http_route(ptr %request2)
  switch i32 %route, label %serve.default [
    i32 0, label %serve.route
    i32 1, label %serve.route10
    i32 2, label %serve.route21
  ]

serve.route:                                      ; route "POST /gain" (float value [[fminus::range(0, 1)]])
  %request3 = load ptr, ptr %serve.request, align 8, !tbaa !18
  %value.text = call ptr @fminus_web_http_query_text(ptr %request3, ptr @.str, i32 5, ptr null)
  %value.missing = icmp eq ptr %value.text, null
  br i1 %value.missing, label %param.missing, label %param.read

param.missing:                                    ; no default: 400
  br label %serve.bad

param.read:
  %1 = call float @fminus_web_http_query_float(ptr %request3, ptr @.str, i32 5, float +qnan)
  store float %1, ptr %value, align 4, !tbaa !21
  %value.number = fcmp ord float %1, %1
  br i1 %value.number, label %param.done, label %serve.bad

param.done:                                       ; its range, as doubles
  ...
  br i1 %value.inrange, label %param.fits, label %serve.bad

param.fits:                                       ; the block: gain = value; changes += 1; reply 204;
  ...
  %http_reply = call i32 @fminus_web_http_reply(ptr %request8, i32 204)
  %7 = icmp eq i32 %http_reply, -5
  %8 = icmp eq i32 %http_reply, -7
  %reply.refused = or i1 %7, %8
  br i1 %reply.refused, label %reply.refused9, label %reply.done
  ...
serve.bad:                                        ; a parameter that does not fit
  %request37 = load ptr, ptr %serve.request, align 8, !tbaa !18
  %33 = call i32 @fminus_web_http_reply(ptr %request37, i32 400)
  br label %serve.next

serve.next:                                       ; where every reply goes on to
  %serve.take38 = call ptr @fminus_web_http_next_in(ptr %0, ptr @serve.routes, i32 1)
  store ptr %serve.take38, ptr %serve.request, align 8, !tbaa !18
  br label %serve.cond, !llvm.loop !27
```

The server and the timeout are computed once, before the first take. The take is `fminus_web_http_accept_in` the first
time when there is a timeout, and `next_in` after (the serve waits once, then handles every request waiting). The
`switch` is on `fminus_web_http_route`, which also loads the route's places into the request, so a hole's parameter is
`http_arg(request, k)` for its hole's place `k`, with no `http_is`. Its default, a request published for a route of no
case, cannot happen (a request taken by a set is one of its routes'), and is answered 500 all the same; a route whose
literal was in error has no case, and its block is lowered where nothing reaches it, for its errors. The branch back
carries `llvm.loop.mustprogress`, as a `for` over `http_next` does, and the loop budget counts it as any loop.

**A parameter** is a stack slot, bound `const` in the route's outermost scope, the one its block's items are in (a
function's parameters' and body's way: a local of the same name is a redefinition). A query's is found or not with
`fminus_web_http_query_text` (null when absent), then read: an `int` by `http_query_int` twice, with the fallbacks 0 and
1, which a value cannot both give (a malformed one gives each its fallback); a `float` by `http_query_float` with a NaN
for its fallback, which a value never is (the library reads finite numbers only); a `bool` by its bytes, compared in
the code: `true`, `false`, `1` or `0`. A missing one takes its default, or goes to `serve.bad`; one outside its range
(compared as doubles, which hold every `int` and `float` exactly) too. `serve.bad` answers 400, and the serve goes on.

**A reply** loads the request, and calls the builtin of its form through `lowerRuntimeCall`, `lowerRuntimeBuiltin`'s
body with the call's arguments as a list of expressions and the first one, the request, given as a value: so each
form's arguments are checked by the very code that checks the builtin's, in its words. The form is the values' (one;
a string literal second; a JSON literal second; four), and none fits is FMinus's error, which lists the forms. Then
`HTTP_LIMIT` or `HTTP_INVALID` (the request still the route's) answers 500; the deferred statements of the route's
blocks run (`runDefers` from the route's first defer scope, `RouteState::defers`: after the reply is sent, so a defer
cannot free what a `reply 200, data, ...` sends); and it branches to `serve.next`. `routes_` holds the route being
lowered, and a `Breakable` of `Kind::Route` marks it among the loops and switches around: `break` and `continue` that
would cross it, a `return` (unless a parallel region is nearer), a `case` label that a switch outside would jump to,
and a `serve` are errors at their keywords; a `reply` whose innermost of route, deferred statement and region is no
route is one too ("cannot reply from a defer statement", "'reply' statement not in a route").

**Every path must reach a reply.** After its block, a route whose block can still fall through ends with a branch to
`serve.next`, and an error at its closing brace when its start reaches that branch through its own blocks: a search of
the CFG that stops at `serve.next` and `serve.bad`, so a constant condition's branch is one way, as for a function's
end (recipe 4's `branchOn`). It cannot be `depth_first`: the serve's own blocks (`serve.bad`, `serve.next`, `serve.end`)
are not finished yet, and LLVM 23's successors of an unfinished block read past its end (Pitfalls). The message is
clang's for a function, said of a route: "route 'GET /a' does not reply in all control paths", or "... does not reply"
when no reply was lowered in it.

**Effects** need one line: a serve with a timeout records a call of `http_accept` at the `serve`, so a nonblocking
function refuses it in the words it refuses `http_accept` in. A route's block is the function's, its calls the
function's calls.

### Overlap

`checkRouteOverlaps` runs once every function is lowered, before the route table is made, over every route literal
the program recorded, a route statement's or an `http_is` call's (`routeLiterals_`, in the order lowered, none from a
deferred statement lowered again). Each path is read as segments of items, a byte that is no digit or a run of digits
(its literal digits and whether a hole ends it: `1{x}` is one run), since a hole takes all the digits there are. Two
routes of a method, with as many segments, each pair of segments of the same shape, meet when every pair of runs can
take one number (a literal one `12` and a hole's `1{x}`: yes; `12` and `{x}`: yes; `1{x}` and `2{y}`: no). When they
meet: the same requests both ways is the same route (legal for two `http_is` calls, an error otherwise); neither's
requests all the other's is an error with a request both match (`GET /a/1/y`); one more specific is legal only if the
library ranks it first (its route ranking, the first segment whose number of holes differs deciding), since a request
goes to the route the library ranks first, and two that rank the same go to the first in its table. One error a
literal, at the second, its note at the first. The check reads a hole as any number of digits: two routes that differ
only past ten digits, or past an int's range, are called overlapping, which is the safe side.

### The JSON literal

A `JsonExpr` lowered as a value is an error ("FMinus has no JSON values: a JSON literal is only a reply's body"), as a
string literal is, at its `json`; `isConstant`, `fold` and the walks know it. A reply's literal is `jsonFormat`'s: the
object's and array's punctuation and the keys (escaped as RFC 8259 asks, `"`, `\` and the control characters, each `%`
doubled for the format; UTF-8 checked, its byte the error) are the format's text, and each value a place, its value
lowered in order and converted to a double, as a reply's format takes them: `%r` a `float` (widened), `%R` a `double`,
`%d` an `int`, `%b` a `bool`. A pointer, a handle, a vector, a string or an enumeration's value is an error saying
what a value may be (a string's, a literal or a `string`, lowered as a string is wanted so that either is one error:
"... in this version, and not yet a string", the next step's; an enumeration's: "cast it to int for its number", since
step 4 writes its constant's name: an `int` now would change what a program's replies say then). The call is `fminus_web_http_reply_text` with
`application/json`; of `reply 200, json { gain, "pitch": (double)pitch, "changes": changes, "on": changes > 0 };`
(`tests/lit/fminus/serve.fm`):

```llvm
@.str.2 = private unnamed_addr constant [43 x i8] c"{\22gain\22:%r,\22pitch\22:%R,\22changes\22:%d,\22on\22:%b}", align 1
  ...
  %http_reply_text = call i32 @fminus_web_http_reply_text(ptr %request23, i32 200, ptr @.str.1, i32 16, ptr @.str.2, i32 43, ptr %format.values, i32 4)
```

(Since recipe 48 a JSON literal is the runtime's JSON writer's, written into the request's own buffer, and the three
conversions below are gone from the library: what follows is how this recipe made it.)

The library's formatter learned the three: `%r` narrows the double back to the float it was widened from and writes
`std::to_chars(float)` with no precision, the shortest digits that read back as that float (`0.1f` is `0.1`), `%R` the
same of the double, a NaN or an infinity `null`; `%b` `true` or `false`. No format a program writes can hold them:
`scanReplyFormat` refuses every conversion but `%d`, `%g`, `%.Ng` and `%%`. `std::to_chars` allocates nothing and makes
no system call, and the wait-free proof covers it (`ServeTest.cpp`).

### What it changes for programs and hosts

- **Programs** that use none of it compile to the same IR, but for two route literals of `http_is` calls one request
  could match with neither more specific, which are now an error (the same literal twice is not). `serve`, `route`,
  `reply` and `json` are keywords: of the tree's programs, `examples/remote/remote.fm`'s method and function `serve`,
  `web-server-off.fm`'s function and three test programs' functions were named `serve`, and were renamed.
- **Hosts** see a route statement in `webRoutes()` as an `http_is` call's entry, its `statement` set and its
  `parameters` listed; `Program::RouteParameter` is new. Nothing else of the API changed.
- **The library** writes three conversions it did not, which only the compiler's formats hold.
- **The VS Code grammar** (0.21.0) colours the four words and a route's holes.

### Tests

- `tests/unit/ServeTest.cpp` (`[serve]`): through the JIT, a module whose nonblocking `process` serves, driven by raw
  loopback requests from the test: a float, an int and a bool from the query (each missing, malformed and out of range
  a 400; a default), an int from a hole (a 404 for no number, a 400 out of range), the request itself, two holes filled
  out of the parameters' order, the more specific of two routes taking what both match, the bytes, text and JSON forms,
  a reply's deferred statement run after it, a status the library refuses answered 500; the JSON literal's text (a
  float's and a double's shortest digits, NaN and infinities `null`, `-0`, nesting, empty objects and arrays, escaped
  and UTF-8 keys, `%` in a key), a literal over `maxReplyBytes` answered 500; two serves on two threads, each leaving the
  other's request waiting, a serve with a timeout waking for a late request and waiting its time for none;
  `webRoutes()` with parameters; the wait-free proof of a serve with four parameters and each reply form, `HeapCalls`
  and seccomp's strict mode; the missing reply on each shape of the missing return's tests; every error at its column,
  the overlap errors with their notes; and, without the web server, a serve's error. Since recipes 46 and 47, where
  they meet: a route in a nonblocking method with two `when`s, a `switch` on a string and a `char` array, each edge
  once and nothing allocated; the missing reply through a `when` and a `switch` on a string, with and without a
  `default`; a `string` or `char` parameter, a string in a JSON literal, in a reply and as a serve's argument, each one
  error at its column.
- `tests/unit/FrontendTest.cpp`: the dump, a non-route in a serve's braces, `reply;`, the words as no names and no
  macros; and a route's block holding a `when`, a `string` and a `char` array.
- `tests/lit/fminus/serve.fm`: the loop and the switch at `-O0`, the facts of the four new declarations, the take kept
  in the loop at `-O2`, the dump, and the driver asking its own server and answering with a JSON literal;
  `serve-errors.fm`: errors and their notes (and, since recipes 46 and 47, the errors where they meet strings and
  `when`); `serve-when.fm`: a route's `when`s, each a bool of the module's after the method's own, and a `switch` on
  a string in a route; `web-server-off.fm`: a serve in a build without the server.
- `examples/remote/`: `--check` as before, through `serve` and a JSON literal, its routes now five.

## Recipe 46: `string`, a read-only view of bytes, and `char` to keep one

*In the compiler. Code: the two words, `char` and `string` (`CHAR` and `STRING_TYPE` in `src/frontend/Lexer.l` and
`Parser.y`, in `type` and `attribute_word`), `Kind::Char`, `Kind::String`, `Type::Char`, `Type::String`,
`canPointTo`, `canBeElement` and `pointsToView` (`src/frontend/Ast.h`, `typeName` in `Ast.cpp`), their LLVM types,
sizes, conversions and checks (`typeOf`, `sizeOf`, `convert`, `checkType`: `src/ir/Types.cpp`), their tags
(`Tbaa.cpp`), a char element read and written as a byte (`element`, `read`, `write`, `lowerAddressOf` in
`Memory.cpp`), the string itself (`src/ir/Text.cpp`: `stringValue`, `loadString`, `storeString`, `store`,
`stringsEqual`, `stringOperands`, `lowerLiteralOperand`, `lowerText`, `textCase`, `compareTexts`, `initializeChars`), a
literal wanted as a string (`stringWanted_` and `lowerAs`, `Expressions.cpp`; `refuseString`, `Strings.cpp`), a switch
on one (`Switch::text` and the rest, `lowerSwitch` and `lowerLabel` in `Statements.cpp`), a char array sized by its
literal (`declaredArray`, `Constants.cpp`), an external function's refusal (`CodeGen.cpp`), the builtins' table rows
and `Param::Text`, `Param::Chars`, `Param::CharData` and `Result::String` (`Lowering.h`, `Calls.cpp`), the runtime's
half (`src/runtime/Text.cpp`, `Runtime.h`, `RuntimeSymbols.def`), and a call by name's refusal (`jit::callType`).
fast_float is now fetched for every build (`cmake/FMinusFastFloat.cmake`), since `string_float` reads numbers in every
build. Tests: `tests/unit/StringTypeTest.cpp`, a case of `FrontendTest.cpp`, `tests/library/StringTypeTest.cpp`,
`tests/lit/fminus/string-type.fm`. Built with `-Werror` and tested in the `release` preset on Linux x86-64 only, in
two trees, the default and one with the web library, its server, file writing and absolute names, every test passing
but the one this machine cannot run (the smoke test of LLVM's own `opt`); not yet in `dev` or `asan`, nor on macOS or
Windows. [research/syntax/README.md](research/syntax/README.md) is the report; this is the first part of its step 3,
its §4.1. Holes in addresses, and the builtins that give strings from a request, a document or a listing, are a later
step's.*

Text, as a value the program reads and never builds:

```c
module Presets
{
    char name[32];                         // a program's own bytes, to keep one
    int size;

    int pick(string wanted)                // a view: a pointer and a length, passed in two registers
    {
        size = string_copy(name, 32, wanted);
        switch (string_of(name, size < 32 ? size : 32))
        {
        case "warm":
            return 1;
        case "bright":
            return 2;
        default:
            return string_starts(wanted, "user-") ? string_int(string_slice(wanted, 5, 99), -1) : 0;
        }
    }
};
```

### The type: a pointer and a length

**`string` is a kind, as `float` is** (`Kind::String`), and a keyword. Its LLVM type is the literal struct
`{ ptr, i32 }`: a value, as a float is, in a register pair, never an address (a module's value is its address; a
string's is not). That is what lets a function take and return one with no memory: LLVM passes the struct's two members
as two arguments, the pointer then the length, and returns them in two registers. On x86-64 and arm64 that is what C
does with `struct { const char* data; int size; }` while registers remain, but no ABI promises it, and Windows' C
returns such a struct through memory: so `FMinus.h` promises a host nothing for a string, `call()` and `--run` refuse
one (`jit::callType` gives none, as for a vector), and a prototype the program declares and never defines may not take
or return one (`CodeGen.cpp`, after the declarations: "an external function cannot take or return a 'string' yet").
In memory it is C's struct, 16 bytes aligned to 8, so a module's string field is where a C++ host's struct would put
it, and `module()` names its type `"string"`.

**Its memory is its two members.** `loadString` loads the pointer, tagged as a pointer, and the length, tagged as an
int, and builds the pair; `storeString` stores them back. `read()` and `write()` call them for a variable, a field or
an element, and an array's list and a const table store through `store`, so every string in memory is reached as what
it is. An array of strings is one an array's name points into, as one of handles is (`pointsToView`): no variable can
hold a pointer to a string, and `string* p` is the pointer error it always was.

**A literal is a string where one is wanted.** `lowerAs(expr, Type::String)` sets `stringWanted_`, which the next
`lower()` takes, as `doubleArithmetic_` passes a double's arithmetic on: the literal then lowers to a constant pair, its
bytes the private constant a builtin's literal is (`stringConstant`, no NUL) and their count. So an initialiser, an
assignment, an argument, a result and an array's list take one with no rule of their own, and a constant of the top
level, `const string NAME = "saw";`, falls out of the constants' code (the pair is a constant; `string_size("four")`
folds to 4 too). A `?:` passes the wish to its arms (`c ? "on" : "off"`). `refuseString`, which every place that
converts a literal calls, gives the pair when the type is a string. Anywhere else a literal is what it was, and its
error says what FMinus lacks there: "FMinus has no char arrays here: a string literal is a 'string' where one is
wanted, or the argument of a builtin's string parameter". A builtin's literal parameter (a URL, a path, a format)
still takes a literal only: a `string` variable there is "argument to 'http_get' must be a string literal".

**`==` and `!=` compare by content**: the same length, then the same bytes, through the runtime's
`fminus_rt_string_equal`, declared as reading its arguments only (so LLVM may merge two calls and move one out of a
loop). A literal beside a string is one (`lowerLiteralOperand` lowers the other operand first, which a literal's lack of
code makes safe), and two literals are two strings. Every other operator is clang's error for a struct
("invalid operands to binary expression ('string' and 'string')"), a cast is "cannot cast from 'string' to 'int'", a
subscript is "subscripted value is not a pointer", and a string as a condition is clang's "statement requires
expression of scalar type ('string' invalid)" with a note that names the test to write, `string_size(s) > 0`.

**A switch on a string** has no LLVM `switch`: its value's block ends in a placeholder (`unreachable`) while the body is
lowered, each `case` records its literal and its block (`textCase`: duplicates are clang's error with its note), and
then `compareTexts` puts the comparisons where the placeholder was, in the labels' order, each a call and a branch to
its label's block or to the next comparison (`sw.next`), the last to the default's block or past the switch. The
semantics are WG14 N3895's with lengths for NULs: the case whose bytes equal the value's. Fallthrough, `break`,
`default`, a jump that passes a declaration (`bypass`) and one past a `defer` work as for an integer switch, since the
labels are the same code. A case that is no literal is "case value of a switch on a 'string' must be a string literal".

### The builtins

They are rows of `kRuntimeBuiltins`, so a prototype of one, `int string_size(string s);`, declares nothing new, and a
definition is refused, as for every builtin. `Param::Text` takes any string (a literal among them); `Param::Chars` and
`Param::CharData` a `char*` and a `const char*`, to which a `void*` and a `const void*` convert as to any pointer.
A row with no symbol is made inline (`lowerText`): `string_size` and `string_data` are an `extractvalue`;
`string_slice` clamps both ends with `smin` and `smax` and moves the pointer with a plain `getelementptr` (an empty
string's null takes 0); `string_byte` reads behind a branch, never outside the string, untagged (a string may view
bytes of any type); `string_of` selects the empty string for a null pointer or a negative size; `string_starts` and
`string_ends` call the comparison on the string's first or last bytes, or on the whole string when it is the shorter.
The others are the runtime's, each a pointer and a length a string: `string_find` (a `std::string_view` find),
`string_float` and `string_int` (the web server's rules for a query's number: fast_float's general format or
`std::from_chars`, the whole string, finite, no `+`), and `string_copy` (a `memmove` of at most `cap` bytes, and the
length). None allocates, locks or waits: every one is `RuntimeEffect::WaitFree`, which the effects let a
`[[clang::nonblocking]]` function call, and the runtime's are described as reading their arguments only (`TextRead`,
`TextCopy` in `describeRuntime`). A string reaches a runtime function as two arguments, its pointer and its length; no
runtime function returns one.

### `char`, to keep one

`Kind::Char` is a byte, `i8`, and only an element and a pointee: `checkType` refuses a `char` variable, parameter,
result or field ("FMinus has no 'char' variables, parameters or results"), but not `sizeof(char)`, which is 1. An
element is an lvalue whose type is `int` and whose `whole` is `char` (`element`): `read` loads the byte with its tag and
widens it with `zext`, so it is 0 to 255 on every machine, and `write` stores the int's low 8 bits and gives back what
the byte then holds, as C's assignment does. `&name[i]` is a `char*` (`lowerAddressOf` reads `whole`). The tag is
`"char"`, a leaf of FMinus's tree as `"int"` is: FMinus's `char` aliases nothing else, where C's may alias anything.
`char name[8] = "abc";` copies the literal's bytes into the zeroed array (`initializeChars`), clang's error if they do
not fit (C's NUL is counted only where it fits); `char name[] = "abc";` has C's four elements (`declaredArray`).

### What it changes for programs and hosts

- **Programs**: `string`, `char` and the eleven builtins' names are taken: a variable or a function so named must be
  renamed (none in the tree was). A literal where a string is wanted now compiles; every other literal error is as it
  was, its words for what FMinus lacks changed. Messages that list the types an array or a pointer may have name `char`
  and `string`.
- **Hosts** see a string field as `"string"`, 16 bytes, and a char array as `"char[32]"`; a function that takes or
  returns a string is none `call()` passes, and none a C++ signature should name.
- **Builds** fetch fast_float whatever their options (it was fetched only with the parsers or the server).
- **The VS Code grammar** (0.22.0) colours `string`, `char` and the builtins.

### Tests

- `tests/unit/StringTypeTest.cpp` (`[string-type]`): a literal in each place it may go (an initialiser, an assignment,
  an argument, a result, a `?:`'s arms, a constant of the top level and an array of them); `==` and `!=` (lengths that
  differ, an empty literal against the empty string, a NUL among the bytes, a view of a char array); a switch with
  fallthrough, a default, none, adjacent literals and a declaration in its head; each builtin at its edges (empty, out
  of range, negative, crossing ends, a cap of 0, numbers with a `+`, a space, an overflow, `nan`, `inf`); char arrays
  (0 to 255, a store's low 8 bits, `&name[i]`, `*p`, through a `void*`, 2-D, a module's, a copy); strings in fields,
  1-D and 2-D arrays and a `parallel for`; a nonblocking function calling every builtin, which allocates nothing
  (`HeapCalls`); and the errors, each once at its column.
- `tests/unit/FrontendTest.cpp`: the words as types and in a dump, and no variable or macro of either name.
- `tests/library/StringTypeTest.cpp`: a string field, an array of them and a char array as `module()` describes them,
  and `callable()` refusing a string.
- `tests/lit/fminus/string-type.fm`: the IR at `-O0` (the two members' loads and tags, the comparison, a switch's
  chain, a char's `zext` and tag) and at `-O2`, `--run`'s refusal, and errors.

## Recipe 47: `when`, a statement that runs once each time its condition becomes true

*In the compiler. Code: the `WHEN` token and its rule (`src/frontend/Lexer.l`, `Parser.y`), `WhenStmt` and its dump
(`src/frontend/Ast.h`, `Ast.cpp`), its state's fields (`layOut` and `collectWhens` in `src/ir/Modules.cpp`,
`whenFields_` in `Lowering.h`), its lowering (`lowerWhen`, `Statements.cpp`), and its refusal in a parallel region and
its part in whether a method changes its module (`Access::Kind::When`, `checkParallelFor`, `checkParallelBlock`,
`changesModule`: `Parallel.cpp`). The example: `examples/trigger/`. Tests: `tests/unit/WhenTest.cpp`, a case of
`FrontendTest.cpp`, `tests/library/WhenTest.cpp`, `tests/lit/fminus/when.fm` and two lines of `examples.test`. Built
and tested as recipe 46 was. [research/syntax/README.md](research/syntax/README.md) is the report; this is its §7.2.*

The synchronous languages' `await` reduced to one bit, for the audio thread, where waiting means looking again next
block: `when (c) s` is `if (c && !was) s; was = c;`, `c` computed once.

```c
module Envelope
{
    float level;
    void process(const float* gate, float* out, int n) [[clang::nonblocking]]
    {
        for (int i = 0; i < n; ++i)
        {
            when (gate[i] > 0.5f)   // the note starts on the sample the gate rises, once
                level = 1.0f;
            out[i] = level;
            level *= 0.999f;
        }
    }
};
```

**The grammar** is one rule, `unlabeled: "when" "(" expr ")" statement`, which adds no conflict (438 states, against
428 before recipe 46): with no `else`, nothing follows the statement to decide between. `WhenStmt` holds the condition
and the statement and dumps as `(when c s)`.

**Its state is a field the compiler adds.** `layOut` gives a module's struct, after its declared fields, a bool for each
`when` of its methods, in source order (`collectWhens` walks a method's statements, every kind of them, for `-Wswitch`'s
sake a visit with no default, and a serve's routes' blocks, which are the method's), and records each one's field in
`whenFields_`. The module's size and alignment count
them, so `sizeof` grows and a host allocating by `module()`'s size holds them, but `module()` lists the fields the
program declared only (`FMinus.cpp` walks the tree's fields). Every instance starts at 0, as its fields do, and a copy
copies the bools with the rest.

**The lowering** is `lowerWhen`: the condition converted as an `if`'s is (so a `poly bool` is clang's error, "statement
requires expression of scalar type"), then the bool's address from `this` (`self()`), its load, the store of the
condition, and `c && !was` as an `and` and an `xor`, then `when.then` and `when.end`, as an `if` without an `else`.
The store comes before the statement, so leaving it by `return`, `break` or `continue` does not arm it again. Optimised,
in a loop over samples, LLVM keeps the bool in a register through the loop, loaded before it and stored after it: each
sample costs a compare and a logical operation (`tests/lit/fminus/when.fm`).

**Where it may be.** A function that is no method has no instance: "a 'when' keeps its state, one bool, in the instance
of the module whose method it is in, and 'f' is no method". In a parallel region its store would race: the region's
walk (`Walker`) records a `when` as an access of its own kind to `this`, which `checkParallelFor` and
`checkParallelBlock` refuse in their words for a method that changes its module ("every iteration of the parallel for
runs this 'when', whose state is the module's ('this'), and the iterations run at once"), and `changesModule` counts as
a change, so a method that holds one is a method that changes its module (a `parallel for` that calls it on `this` is
refused too). It allocates and waits for nothing, so a nonblocking method may hold one, and the effects need no rule.

### Tests

- `tests/unit/WhenTest.cpp` (`[when]`): a rising edge fires once, and again after a false; a condition that stays true
  does not; the first reach with a true condition fires; two whens and two instances keep their own state, in the
  bools after the fields where a C++ struct finds them; a loop over samples fires on each edge, a copy of the module
  copies its state; a `return`, `continue` and `break` in the statement do not arm it again; the refusals.
- `tests/unit/FrontendTest.cpp`: the dump, and `when` as no variable's name.
- `tests/library/WhenTest.cpp`: the module's size counts the state, `module()` lists no field for it, and an instance
  the host allocates and zeroes works.
- `tests/lit/fminus/when.fm`: the IR at `-O0` (the load, the store, the test) and at `-O2` (the state a register
  through the loop), `sizeof`, and the errors; `examples.test` runs `examples/trigger/`.

## Recipe 48: holes, and text from requests, documents and listings

*In the compiler, and a little of the web library. Code: an address's holes, `scanHoles` and `HoleScan`
(`src/ir/Strings.cpp`, `Lowering.h`), filled where a call's literal is (`lowerHoles`, `Filled`, `formatTexts` and the
address branch of `lowerRuntimeCall`, `src/ir/Calls.cpp`), checked for a URL (`checkUrlHoles`, `Web.cpp`) and a file's
name (`FileNameRules::holes`, `spelled`, `checkFileName`, `Files.cpp`), and recorded with a `*` each
(`recordWebRequest`, `recordFileUse`), and read for the race checks (`Walker::holes`, `Parallel.cpp`); a route's
`string` parameters (`scanRouteHoles` and `scanRoute` with their texts,
the overlap check's `isText`, `sample` and `rank`, `checkHeaderName`, `WebServer.cpp`; `checkRouteParameters`,
`textDefaultOf` and `fillParameter`, `Serve.cpp`), and their default in the host's list
(`ir::WebRouteParameter::defaultText`, `Program::RouteParameter::defaultText`, `include/fminus/FMinus.h`,
`src/api/FMinus.cpp`); the builtins that give text (`http_header`, `http_path`, `http_query_string`, `json_string`,
`xml_string`, `file_name`: their rows, `Param::HeaderName`, and `textResult`, `Calls.cpp`) and their facts
(`ServerText`, `ReadString`, `FileEntry`; the `_s` functions' memory beyond their arguments); the JSON literal through
the runtime's writer (`lowerJsonReply`, `jsonDescribe`, `JsonPlan`, `Serve.cpp`; `JsonWrite` and `ServerBuffer` facts);
`%s` in a reply's format (`scanReplyFormat`, `replyArguments`, `WebServer.cpp`); and in the library,
`fminus_web_http_reply_text_s` and the formatter without `%r`, `%R` and `%b` (`src/web/Server.cpp`, `Web.h`,
`WebSymbols.def`). The example `examples/presets/`. Tests: `tests/unit/HoleTest.cpp`, `DocumentTextTest.cpp`, cases of
`ServeTest.cpp`, `HttpServerTest.cpp`, `WebServerTest.cpp` and `FrontendTest.cpp`, `tests/lit/fminus/holes.fm`,
`holes-web.fm`, `holes-errors.fm`, `serve-strings.fm` and `string-reads.fm`. Built with `-Werror` and tested in the
`release` preset on Linux x86-64 only, in two trees, the default and one with the web library, its server, file writing
and absolute names, every test passing but the one this machine cannot run (the smoke test of LLVM's own `opt`), and
since also in the three trees that leave features out ([testing.md](testing.md), "Builds that leave features out"),
without `FMINUS_JSON` and with the web library and not its server among them; not in `dev` or `asan`, nor on macOS or
Windows. [research/syntax/README.md](research/syntax/README.md) is the report, §4.2, §4.3, §5.2 and §6.1; this is the
rest of its step 3. The libraries' halves came first, in the two commits before recipe 44 and the one before recipe 46
(`fminus_web_http_text`, `_path`, `_header`; `fminus_rt_json_text`, `_xml_text`, `_file_name`; the `_s` functions;
`fminus_rt_json_write`, `fminus_web_http_reply_buffer` and `_reply_written`).*

Text, recipe 46's view, now goes somewhere and comes from somewhere: into an address by a hole, out of a request, a
document and a listing, and into a JSON literal or a reply. Nothing is built: a string is read, and written only where
it goes.

```c
route "PUT /presets/{name}" (string name, http_request q)      // a whole segment of the path
{
    if (!string_starts(http_header(q, "content-type"), "application/json"))
        reply 415;
    if (file_write("presets/{name}.json", http_body(q), http_size(q)) != FILE_OK)   // ".hidden": FILE_INVALID
        reply 400, json { "error": "no such name", "name": name };
    reply 204;
}
```

### Holes

**`scanHoles` reads an address literal** (a URL, a file's name, a listing's pattern) into the literal the library takes:
each `{name}` a `%d` (whose `d` becomes an `s` once its variable is known to be a string), `{{` and `}}` a brace, `%d`
and `%%` as they were, and for each byte of the result the offset of the byte of the literal it stands for, so every
later check reports at the source's byte. It refuses a brace alone, an empty hole, a name that is no C identifier, and
a `%d` beside a hole, the error at the second kind ("a literal names its values by holes, {name}, or by %d after it,
not both"). A literal with no hole goes the way it always went, its braces unescaped; one with holes takes no ints after
it ("a literal with holes takes no values after it").

**`lowerHoles` fills them**: each hole a `VariableExpr` at its `{`, lowered as any name is (so an unknown one is clang's
"use of undeclared identifier", and a field, a parameter, a constant and an enumerator are found as in an expression);
an `int`, a `bool` or an enumeration's value converted to an `int`, a `string` kept as its pair; anything else "hole {x}
has type 'float': a hole takes an int, a bool, an enumeration's value or a string". The format's own errors are
`scanFormat`'s on the literal with every hole a `%d`. Then the checks of the literal's kind run on the filled literal:
`checkUrl` as ever, and `checkUrlHoles`, no hole before the port (the scheme's and the host's bytes are what the host's
list names), a string's never in it; `checkFileName` with `holes`, which reads a `%s` as a letter and refuses a hole in
the folder or the extension, its messages' `%d` said as "hole" (a device name "this segment", since its bytes are no
longer the source's). The call is the builtin's own function when every hole is a number's, and its `_s` function when
one is a string's: the strings after the ints, in two arrays of the frame, their addresses and their lengths
(`formatTexts`), as `runtime/Format.h`'s `expandPlaces` takes them.

```llvm
@.str = private unnamed_addr constant [20 x i8] c"presets/%s/%d-%d.txt", align 1
  %file_read = call ptr @fminus_rt_file_read_s(ptr %files.context, ptr @.str, i32 20, ptr %format.ints, i32 2,
                                               ptr %format.texts, ptr %format.lengths, i32 1)
```

What a string may become is the library's to check, where the bytes are: a URL's string percent-encoded (a path segment
it makes `.`, `..` or empty `HTTP_INVALID` before anything is sent), a file name's joining its segment and the name then
checked by the rules a literal's segments keep (`FILE_INVALID`). Nothing about enforcement changed: the filled URL meets
the policy, the filled name `reach`. The host's lists keep their form, a `*` a hole. A hole's variable is no node of
the tree, so the race checks' walk (`Parallel.cpp`'s `Walker::holes`) scans the literal again and records each as read
where its call is: a statement of a parallel block that names one in a hole races with one that changes it, as a `%d`'s
argument would.

### A route's `string` parameters

A route's literal had holes already (recipe 45); `scanRouteHoles` now takes the names of its string parameters, whose
holes must be a whole segment ("a string's hole is a whole segment of the path"), each a `%s` of the library's table,
and `scanRoute` takes a `%s` when it is a route statement's (an `http_is` literal still refuses one, as `%s` there is
clang's invalid conversion). The overlap check reads a `%s` segment as one that takes any segment: within another only
when that is one too, ranked after any number of number holes, as the library ranks it (track 2's note). A parameter
`string` is filled by `fminus_web_http_text` of its place (a hole's) or by `fminus_web_http_query_text` (the query's
field: its bytes as sent, nothing decoded), into its slot as its two members; missing is its default,
`[[fminus::default("text")]]` (a string literal's pair), or a 400. A `string` has no range ("'fminus::range' does not
apply to a string parameter"), and its default is a string ("the default of a string parameter is a string literal"):
`checkAttributes`' rules (`Modules.cpp`), where a field's are too, and where a `string` field takes no default at all
(recipe 50).
The metadata's tuple of a parameter has a seventh operand, a string's default or none, which `ir::webRoutes` reads into
`defaultText`, a member added after the others to `Program::RouteParameter`.

### Text from a request, a document, a listing

No runtime function returns a struct (C's ABI for one differs by platform), so a builtin whose result is a `string` and
which has a symbol, a `Result::String` row, is called with one more argument, the address of an `int` of the frame,
where the function writes the length of the bytes whose address it returns; `textResult` loads it and makes the pair.
Such a row's `Param::Text` is its fallback, never passed: chosen member by member, with two `select`s, when the address
is null (nothing there, or not text). So `json_string(doc, "/name", "none")` is

```llvm
  %json_string.data = call ptr @fminus_rt_json_text(ptr %doc, ptr @.path, ptr null, i32 0, ptr %json_string.length)
  %json_string.size = load i32, ptr %json_string.length, align 4, !tbaa !5
  %json_string.none = icmp eq ptr %json_string.data, null
  %1 = select i1 %json_string.none, ptr @.str, ptr %json_string.data
  %2 = select i1 %json_string.none, i32 4, i32 %json_string.size
```

| Builtin | The library's function | Its address is valid |
|---|---|---|
| `http_header(q, "name")` | `fminus_web_http_header`, the name a literal checked as an HTTP token (`checkHeaderName`) | until the reply |
| `http_path(q)` | `fminus_web_http_path` | until the reply |
| `http_query_string(q, "name", fallback)` | `fminus_web_http_query_text` | until the reply |
| `json_string(doc, path, ..., fallback)` | `fminus_rt_json_text` | until `json_free` |
| `xml_string(doc, path, ..., fallback)` | `fminus_rt_xml_text` | until `xml_free` |
| `file_name(list, i)` | `fminus_rt_file_name` (null outside the listing: the empty string) | until `file_close` |

Each is wait-free, so a nonblocking function may call it, and each is described as a read that writes its last
argument: the request's (`ServerText`, now any of the four functions: its pointer arguments read but the last, which it
writes), the document's (`ReadString`, `Read`'s with argument 4 written) and the listing's (`FileEntry`, `FileRead`'s
with its length written). Their result is neither `noalias` nor `nonnull`, and the memory it points into is freed or
reused by a call whose memory is any (the reply, `json_free`, `file_close`), so a load through it stays before that.

### The JSON literal, through the runtime's writer

Recipe 45 made a JSON literal a format of the reply's text, with three conversions of the library's own. A string
cannot go that way (it must be escaped), and the next step writes arrays and modules, so **every** JSON literal of a
reply is now the runtime's JSON writer's (`fminus_rt_json_write`, track 3's), writing straight into the request's own
reply buffer. `jsonDescribe` builds the literal's description as track 3's note lays it out: its keys and punctuation
bytes, collected while no value comes (`JsonPlan::pending`) and flushed as an operation `0, offset, count`, and each
value an operation `1, slot, shape`, its value lowered in the order written into `JsonPlan::values` with its first
slot: a `float` 1, a `double` 2, an `int` 3, a `bool` 4 (a byte), a `string` 5 (its pointer and its length, two slots).
A key is escaped by the compiler as before (`jsonKey`, without the `%` it doubled for a format); a string value by the
writer, as RFC 8259 asks, bytes above 0x7F as they are. Then `lowerJsonReply` stores the values into an `[n x i64]` of
the frame, makes the description a private constant laid out as a path's table, and calls

```llvm
  %reply.buffer = call ptr @fminus_web_http_reply_buffer(ptr %request, ptr %reply.capacity)
  %reply.room = load i32, ptr %reply.capacity, align 4, !tbaa !23
  %json.bytes = call i32 @fminus_rt_json_write(ptr %reply.buffer, i32 %reply.room, ptr @.json, ptr %json.values)
  %http_reply_written = call i32 @fminus_web_http_reply_written(ptr %request, i32 200, i32 %json.bytes, ptr @.str.1, i32 16)
```

`reply_written` says `HTTP_LIMIT` for a text past the capacity, which the route answers 500 as before. Every output
recipe 45's tests pin is the same, byte for byte: the writer's numbers are `std::to_chars` with no precision, as `%r`
and `%R` were. `%r`, `%R` and `%b` are gone from the library's formatter (a `%` that begins none of its places is
itself). An enumeration's value, an array, a vector and a module stay refused, each an error that says a later version
writes it (the writer has their shapes; the next step uses them). (Since recipe 51 they are written, `jsonDescribe` is
`Shapes.cpp`'s `describeJson` on `appendShape`, and a reply's literal is a call of `http_reply_json`.) The writer is part of `FMINUS_JSON`: in a build
without it a JSON literal is "a JSON literal needs the JSON writer, which this build of FMinus leaves out
(FMINUS_JSON=OFF)".

### `%s` in a reply

`scanReplyFormat` takes a plain `%s` (`Place::text`); `replyArguments` lowers its value as a string is wanted (a literal
is one), "format specifies type 'string' but the argument has type 'int'" otherwise, and gives the strings apart from
the doubles. With one, the call is `fminus_web_http_reply_text_s`, new in the library: the strings after the values in
two arrays, as the `_s` functions take them, each written as it is; a string that is not one (a negative length, a null
address with bytes) `HTTP_INVALID`. `fminus_web_http_reply_text` is that function with no strings, its results as they
were (a `%s` there is itself). It is wait-free, in `WebSymbols.def`, and inside both proofs (`HeapCalls` and seccomp's
strict mode, `WebServerTest.cpp`).

### What the runtime's functions touch, told to LLVM

- **An `_s` function reads memory no argument points to**: each string's bytes, through the address an array of the
  frame holds. `argmem` alone would let LLVM drop a store to a `char` array whose view is such a string, so
  `describeRuntime` adds `otherMemOnly(Ref)` to every `_s` function whose memory is not any already (`memory(read,
  argmem: readwrite, ...)`).
- **`fminus_rt_json_write`** reads any memory and writes only its arguments' (`memory(read, argmem: readwrite)`): a
  value's slot may hold a string's address. Nofree, nosync, its buffer writeonly, its description and values readonly.
- **`fminus_web_http_reply_buffer`** is `WebRead`'s kind, writing its capacity; `_reply_written` and `_reply_text_s` are
  replies, their memory any, so the writer's stores into the buffer stay before the publish.

### What it changes for programs and hosts

- **Programs**: a brace in an address literal is a hole now, `{{` one brace (no literal in the tree had one). A route
  may take a `string`; the messages that listed a route parameter's types name it, and a hole's says "a segment of the
  path or a number in one". A JSON literal may hold strings; `http_header`, `http_path`, `http_query_string`,
  `json_string`, `xml_string` and `file_name` are taken names. A reply's `%s` is a place. A program that uses none of
  it compiles to the same IR, but for a JSON literal, which is now the writer's calls, and needs `FMINUS_JSON`.
- **Hosts** see a hole as a `*`, a `string` route parameter as `"string"`, and its default in `defaultText`. The
  Python package does not bind `webRoutes()` (docs/python.md), so nothing there changes.
- **The library**'s formatter lost `%r`, `%R` and `%b`, which only the compiler's formats held, and gained
  `fminus_web_http_reply_text_s`.
- **The VS Code grammar** (0.24.0) colours the six builtins, `%s`, and `{{` and `}}` as a brace.

### Tests

- `tests/unit/HoleTest.cpp` (`[holes]`): `scanHoles` itself; each mistake of a file's name and of a URL at its byte;
  `files()` and `webRequests()` with a `*` a hole; through the JIT, files read, listed and written by names with holes
  of each type, and every name a string makes wrong `FILE_INVALID`; a URL's string percent-encoded byte by byte (all
  256), a path segment `..`, `.` or empty `HTTP_INVALID`, a `/` `HTTP_REFUSED`, one program asking another on loopback.
- `tests/unit/ServeTest.cpp`: a `string` parameter from the path and the query, its default, a header, the path, a
  query's string, and the overlap's ranking; the host's list; the wait-free proof with them, and a JSON literal written
  in place; a JSON literal's strings and their escapes, `%s`; the errors where `string` meets `serve`; and, in a build
  without `FMINUS_JSON`, the JSON literal's error.
- `tests/unit/DocumentTextTest.cpp`: `json_string`, `xml_string` and `file_name` at their edges, a nonblocking function
  calling every wait-free builtin of the step, their errors, and the reads proved wait-free through the JIT.
- `tests/unit/HttpServerTest.cpp`: `%s` in `scanReplyFormat` and its errors. `tests/unit/WebServerTest.cpp`: the
  formatter's `%r`, `%R`, `%b` and `%s` as themselves, `fminus_web_http_reply_text_s`, and the wait-free proof with it.
- `tests/unit/FrontendTest.cpp`: a route's `string` parameters and an address's holes in the dump.
- `tests/lit/fminus/holes.fm` and `holes-web.fm`: the IR of each kind of hole at `-O0` and `-O2`, the facts of the `_s`
  functions, the metadata, and runs; `holes-errors.fm`: every error once at its column; `serve-strings.fm`: a route's
  strings, the request's text, a JSON literal's string and `%s`, run on loopback; `string-reads.fm`: the documents' and
  the listing's text; `serve.fm`, `serve-errors.fm`: the writer's calls, the changed messages.
- `examples/presets/`: `--check` on loopback, in a directory it makes.

## Recipe 49: `within`, a deadline for the requests a block starts

*In the compiler and the web library. Code: the `WITHIN` token and its rule (`src/frontend/Lexer.l`, `Parser.y`, and
the word in `attribute_word`), `WithinStmt` and its dump (`src/frontend/Ast.h`, `Ast.cpp`); its lowering,
`src/ir/Within.cpp` (`lowerWithin`, `lowerWithinEnd`), on `defer`'s mechanism (`Deferred::within` in `Lowering.h`,
`lowerDeferred`, `closeDefers` and `lowerLabel`'s note in `Statements.cpp`); its effects (`kWithinCall`, and the words
in `Effects.cpp`); the facts of `fminus_web_http_within_begin` and `_end` (`RuntimeKind::WebScope` and
`kRuntimeFacts`, `Calls.cpp`); the walks (`Parallel.cpp`'s `Walker`, `Modules.cpp`'s `collectWhens`); the passes' ways
out (`returnEarly` and `abandonScopes`, `src/opt/EarlyReturn.h` and `.cpp`, which `BoundsCheck.cpp` and `LoopBudget.cpp`
call); and in the library, `fminus_web_http_within_abandon` and the records it reads (`State::Holder`, `holderOf`,
`abandoned`, `dropAbandoned`: `src/web/Web.h`, `Web.cpp`, `WebSymbols.def`). Tests: `tests/unit/WithinTest.cpp`, a
case of `FrontendTest.cpp`, `tests/lit/fminus/within.fm`, `within-errors.fm` and `within-off.fm`; `examples/fetch/`
(`countPacks`, and `Live`'s `when`). Built with `-Werror` and tested in the `release` preset on Linux x86-64 only, in two
trees, the default and one with the web library, its server, file writing and absolute names, every test passing but
the one this machine cannot run (the smoke test of LLVM's own `opt`); the library's part and the JIT's tests of it also
under GCC's ThreadSanitizer and AddressSanitizer, outside the presets; not in `dev` or `asan`, nor on macOS or Windows.
[research/syntax/README.md](research/syntax/README.md) is the report, its §7.1; [web.md](web.md), "within", is the
manual. The library's begin and end came first, in the commit before recipe 44, tested from C++.*

Structured concurrency's one rule, for requests: a child lives in a lexical block, and the block does not end before
it. FMinus had it for threads (`parallel { }`); a program that fetched eight packs had to wait for each by hand, and
one it forgot kept running past the code that needed it.

```c
http_response r[MOST];
within (10000)                                   // all of them at once; none is still running after the block
{
    for (int k = 0; k < packs; ++k)
        r[k] = http_get("https://example.com/packs/%d.json", k);
}
// here every r[k] has its final status: 200, a failure, or HTTP_TIMEOUT
```

### Tokens, grammar, node

`within` is a keyword, a token of its own and a word of `attribute_word`. The rule is the report's appendix D's, one
line: `unlabeled: "within" "(" expr ")" block`, braces required, so `within (10) f();` is Bison's "unexpected
identifier, expecting {". No conflict (504 states, against 498 before): after the `)` only a `{` can follow. The node
holds the milliseconds and its body as a `BlockStmt` at its brace, so a walk that knows blocks walks it with nothing
new, and it dumps as `(within ms\n  (block ...))`. `await`, which the report reserved only in its prototype, stays an
identifier: FMinus has no `async`, and a word reserved for a feature that is not built would break programs for nothing.

### What a within lowers to

The library's half (`src/web/Web.h`, "deadline scopes") is a scope of the calling thread under the program's context:
`within_begin(context, ms)` opens one, which the thread's `http_get` and `http_post` join and its `http_wait`,
`http_accept` and `_accept_in` read; `within_end(context)` waits for its requests until the deadline, ends the rest as
`HTTP_TIMEOUT`, and closes it. The statement pairs them on every way out, which is `defer`'s problem solved once
already (recipe 44): `lowerWithin` computes the milliseconds (an `int`, converted as an argument is: a `float` is
truncated, a pointer is clang's error), begins the scope, keeps whether it began in a slot, and opens a `DeferScope` of
its own around the braces, whose one `Deferred` is the end: no statement, and `within`, the slot. Each way out that
leaves that scope (`closeDefers` at the brace, `runDefers` for `return`, `break`, `continue` and a `reply`) calls
`lowerDeferred`, which for a within's end lowers `lowerWithinEnd`: the end, when the scope began.

```llvm
  %web.context = load ptr, ptr @fminus_web_context, align 8
  %within.begun = call i32 @fminus_web_http_within_begin(ptr %web.context, i32 %0)
  %within.open = icmp eq i32 %within.begun, 0
  store i1 %within.open, ptr %within.slot, align 1, !tbaa !8
  ...
if.then:                                          ; return seen; after defer seen = http_status(r);
  %seen5 = load i32, ptr %seen, align 4, !tbaa !3
  %http_status = call i32 @fminus_web_http_status(ptr %r6)
  store i32 %http_status, ptr %seen, align 4, !tbaa !3
  %within.opened = load i1, ptr %within.slot, align 1, !tbaa !8
  br i1 %within.opened, label %within.end, label %within.done
within.end:
  %web.context7 = load ptr, ptr @fminus_web_context, align 8
  %within.ended = call i32 @fminus_web_http_within_end(ptr %web.context7)
  br label %within.done
within.done:
  ret i32 %seen5
```

So the order is `defer`'s: a `return`'s value first, then the block's own deferred statements (their `DeferScope` is
inside the within's), then the end; a `reply` sends first and ends the scope after, as a route's deferred statements
run after its reply (recipe 45), so the client has its answer while the scope's requests end. The slot is for a begin
that failed (no memory: no scope open), whose end must not end the enclosing scope in its place; at `-O2` it is a
register. The end's result is no value of the program's: each request's status says what ended it. The end has no
errors of its own, so `closeDefers` lowers none in an unreachable block for its errors, and a switch that would jump
into the block, past the begin, is `lowerLabel`'s "cannot jump from switch statement to this case label", its note at
the within: "jump enters a 'within' block". The walks need a line each: the race checks walk the milliseconds and the
block, and `collectWhens` walks the block, so a `when` inside one has its state.

The declarations (recipe 19): `within_begin` is `RuntimeKind::WebScope`, `willreturn`, `memory(argmem: readwrite,
inaccessiblemem: readwrite, errnomem: write)`, the context `captures(none)` (it allocates as the list of scopes grows);
`within_end` is `WebWait`'s, with no `willreturn` (it waits for a deadline and the I/O thread). It changes the statuses
of the scope's requests, which the program reads again with `http_status`, a `WebRead`, never merged.

### Effects and errors

A within allocates where it begins and waits where it ends. It is recorded among the function's calls as one callee of
its own, `kWithinCall` (a keyword, so no function of a program has its name), which `unsafe` refuses under either
effect, each in words of its own, at the `within`:

| The program writes | FMinus, at the `within` |
|---|---|
| a within in a `[[clang::nonblocking]]` function | "function with 'nonblocking' attribute must not call non-'nonblocking' function 'http_wait'": the words `http_wait` is refused in, as a `serve` with a timeout is refused in `http_accept`'s |
| in a `[[clang::nonallocating]]` one | clang's words for an allocation that is no call: "function with 'nonallocating' attribute must not allocate or deallocate memory" |
| a call of a function that holds one | the call's error, its note at the within: "function cannot be inferred 'nonallocating' because it allocates or deallocates memory" (or "... calls non-'nonblocking' function 'http_wait'") |
| a `case` label inside its block | "cannot jump from switch statement to this case label", note "jump enters a 'within' block" |
| `within (p) { }` of a `float* p` | "passing 'float *' to parameter of incompatible type 'int'" |
| any within, in a build without `FMINUS_WEB` | "'within' needs the web library, which this build of FMinus leaves out (FMINUS_WEB=OFF)", once; its block is lowered plainly, for its own errors |

One record, not two (a wait and an allocation), so a nonblocking function has one error a within, not two (recipe 32).
A `parallel` region inside a block runs its body on other threads: their requests are not the block's (what the
calling thread runs of the region is, so which ones is the pool's choice: web.md says so), and a within inside a
region's body is its thread's own, which ends there (a region's body is left no other way).

### A block a stopped call abandons

A call the loop budget or a bounds check stops leaves every block at once, without reaching its end, and runs no
deferred statement (recipes 29, 30 and 44: the checks are made in the optimised code, where the blocks are gone). For
`defer` that is accepted. For a within it is not: the scope is found by the thread's id, so one begun and never ended
stays the thread's. Every later wait of the thread would be bounded by its deadline, long past (`http_wait(r, 50)`
returns at once), every later within would take it as its enclosing scope's (its deadline already past), and a later
thread the system gives the same id would inherit it. Tested: with the abandon below taken out, `WithinTest.cpp`'s
`http_wait(r, 50)` after a stopped call returns in 0 ms, and a new `within (150)` ends at once.

**The library gains an abandon**, `fminus_web_http_within_abandon(context)`: every scope the calling thread has open
under the context is dropped at once. It waits for nothing: the scopes' requests go on as requests started outside
any scope do, under the policy's limits, and are freed with the context when the program never closes them (it never
saw them: the stopped call wrote nothing). It must be cheap and must not block, since an audio thread's way out may
come to it; the scopes are a vector under the context's mutex, so it cannot touch them. Instead a thread that begins a
scope has a **record** of the context's (`State::Holder`): its thread's id, a count of its scopes open, and a
generation that each scope copies when it begins. The records are a list pushed under the mutex (a record whose thread
has none open is taken by the next thread that begins one, so the list never holds more than the most threads that had
scopes at once) and never freed before the context, so the abandon walks it without a lock: for its thread's record,
when it has scopes open, it moves the generation on and stores the count 0, in that order, and returns how many. Only
the record's own thread writes those two, so no store races another; a begin that then takes the record, seeing 0 with
an acquire load, sees the new generation. Every scope begun under an older generation is **abandoned**: `innermost`
(which every request's start, every wait and every begin asks for the thread's scope) skips it, and the next begin or
end of any thread drops it from the vector under the mutex. Another thread's scopes are never touched. The steps are a
walk of the records, a few atomic loads, and two stores when there is something to abandon: no lock, no allocation, no
system call, which `WithinTest.cpp` proves as the tree proves a wait-free builtin, under `HeapCalls` (0 calls) and in a
child under seccomp's strict mode (its result arrives), for a context with no record, a thread with two scopes open,
one with none while another thread holds one, and a null context.

**The passes call it**, but only where it can matter. A scope belongs to the frame of the function that called
`within_begin`: after inlining, the function that holds the call. When a call stops, that function either stops itself
(its stop block, the budget's `budget.stop` or the checks' `bounds.stop`) or returns after the call of the function
that did (its `budget.unwind` or `bounds.unwind`), on the scope's own thread, and every function between it and the
host returns too, so every scope the thread has open belongs to a frame that is going. So `returnEarly`, which both
passes call to make each of those blocks' return, first calls `abandonScopes`, which in a function that calls
`within_begin` loads the program's context and calls the abandon:

```llvm
budget.stop:
  store atomic i64 -1, ptr @fminus_rt_loop_budget monotonic, align 8
  store atomic i32 1, ptr getelementptr inbounds ({ i64, i32 }, ptr @fminus_rt_loop_budget, i32 0, i32 1) monotonic, align 4
  %web.context12 = load ptr, ptr @fminus_web_context, align 8
  %within.abandoned13 = call i32 @fminus_web_http_within_abandon(ptr %web.context12)
  ret float 0.000000e+00
```

Why not once, where a stop is first taken? Because a thread can stop without taking one: a parallel region's part
stops on another thread, and the region's caller, the thread with the scope, only unwinds; and while the record of the
bounds checks holds a failure (from another thread's call of the same program, say), every function returns as soon
as a call of one that may stop returns, without any check of its own failing. So it is each unwinding function's way
out, and of those, only a function that begins a scope needs it: one that begins none has no scope of its own, and the
function whose scope it is will pass its own way out after it. A function that holds no call that may stop cannot be
cut short, and ends its scope at its brace. A program without a within has no call of `within_begin`, and the passes
leave it as it was, byte for byte (`within.fm` checks `spin` and `plain`, and every lit test that pins the passes'
IR is unchanged). The declaration says what the abandon touches: `nofree nounwind willreturn memory(argmem: readwrite,
inaccessiblemem: readwrite)`, the context `captures(none)`; a function that calls it has `inaccessiblemem` added to
its memory effects where they did not have it.

A better way would have needed no call on the stop paths: a scope that knows its frame (a stack address the block
passed to the begin) could be found dead by the thread's next library call, when that call is made from a frame above
it. But a host calls the same function again from the same depth, whose frame is at the same address, and a thread
the system gave a gone thread's id could be at any depth: no address test tells a dead scope from a live one. The
abandon is what makes "after a stopped call, the thread has no scope" true.

### What it changes for programs and hosts

- **Programs**: `within` is a keyword; `examples/wavetable/wavetable.fm`'s local of that name is `fraction` now, and no
  other program in the tree used it. A program without a within compiles to the same IR, with or without the passes.
- **Hosts**: nothing new in `FMinus.h`. `stopWeb()` and `~Program` wake a thread at a within's end as they wake one in
  `http_wait`; a call stopped inside a block leaves its requests running until they end or the program goes.
- **The library**: one function more, `fminus_web_http_within_abandon`, in `WebSymbols.def`; a thread's first begin
  under a context makes its record (32 bytes), kept for the context's life.
- **The VS Code grammar** (0.23.0) colours `within`.

### Tests

- `tests/unit/WithinTest.cpp` (`[within]`), through the JIT at `-O0` and `-O2`, against a small HTTP/1.1 server of its
  own on 127.0.0.1 (`/ok`, `/after/N`, `/slow`, `/pack/K.json`): six requests at once all 200 at the brace in about
  one's time; a slow one among them `HTTP_TIMEOUT` at the deadline, the others 200; no deadline; one already passed; a
  request started in a function the block calls; `http_wait(r, -1)` inside returning at the deadline with 0; nested
  blocks (the inner ends with the outer's deadline, an outer request still pending after the inner end); `return` with
  a `defer` before the end, `continue` and `break`; `stopWeb()` waking a thread at the brace; with the server, a reply
  inside a block received before its scope ends and a serve bounded by the deadline; the report's script (eight packs
  in one within, then each read); where it meets recipe 50, a module `json_read` fills from each pack after the block,
  and one a method fills through `this` inside a block; where it meets recipe 48, requests whose URLs have holes (an
  `int`'s in the port and the path, a `string`'s from a `char` array in the path), started through the library's `_s`
  entry points, all 200 at the brace and the held one `HTTP_TIMEOUT` at the deadline; a block the loop budget
  (`[budget]`) and one a bounds check (`[bounds]`) stop with a
  request pending, then on the same thread `http_wait(r, 50)` waiting its 50 ms, a new within with its own deadline and
  its promise, and the program destroyed with the connection closed and the I/O thread gone; a stop on a thread with no
  scope leaving another thread's scope whole; a program without a within, with both passes, has no abandon. The
  library's abandon alone: what it abandons (all of the thread's, at once), what it leaves (their requests, which
  complete or wait their own time; another thread's scope), threads that come and go, threads racing begins, ends and
  abandons, and its wait-free proof. The errors, each once at its column; without the library, its one error.
- `tests/unit/FrontendTest.cpp`: the dump, braces required, `within` as no variable's or macro's name, `await` an
  identifier.
- `tests/lit/fminus/within.fm`: the IR at `-O0` (the begin, the slot, the end at each way out after the block's
  defer, three ends for a loop's `continue`, `break` and brace), the declarations' attributes, the abandon on the loop
  budget's ways out of the functions that begin a scope and not of those that begin none, no abandon with bounds
  checks and none failing at `-O2`, the dump, and `--run` with no network; `within-errors.fm`: the effects, the jump,
  the milliseconds; `within-off.fm`: a build without the library.
- `examples/fetch/`: `--check` fetches four packs at once in `countPacks`'s within (three on the server), and plays
  `Live` until its `when` swaps the pack's first table in, silence before and the table after, the `when` fired once.

## Recipe 50: `json_read`, a module filled from a JSON document by a table of its fields

*In the compiler. Code: `json_read`'s row of `kRuntimeBuiltins`, `Param::Module`, its argument in `lowerRuntimeCall`,
and the facts of `fminus_rt_json_module`, `RuntimeKind::ReadModule` (`src/ir/Calls.cpp`, `Lowering.h`);
`src/ir/Shapes.cpp` (`ShapeTable`, `ShapeUse` and `ShapeProblem` in `Lowering.h`; `appendShape`, `appendField`,
`leftOut`, `jsonName`, `shapeConstant`, `moduleShape`, `lowerFilled`, `checkJsonAttribute`, `checkJsonNames`,
`moduleShapes_`); a field's `fminus::default`, the `json::` attributes' place among the others and the notes for
`[[name]]`, `[[skip]]` and `[[default]]` (`checkAttributes` and `layOut`, `src/ir/Modules.cpp`); the comment of
`Program::Attribute` (`include/fminus/FMinus.h`). The runtime's half, `fminus_rt_json_module` and the shapes'
format (`src/runtime/Json.cpp`, `JsonShapes.h`, `Runtime.h`), came with the libraries' commit before recipe 46 and is
unchanged. Tests: `tests/unit/JsonReadTest.cpp`, cases of `FieldAttributeTest.cpp` and
`tests/library/FieldAttributeTest.cpp`, `tests/lit/fminus/json-read.fm`, `json-read-errors.fm`, a line of
`json-off.fm`, and `field-attributes-errors.fm`'s changed lines. The example: `examples/settings/`. Built with `-Werror`
and tested in the `release` preset on Linux x86-64 only, in two trees, the default and one with the web library, its
server, file writing and absolute names, every test passing but the one this machine cannot run (the smoke test of
LLVM's own `opt`), and since also in the three trees that leave features out ([testing.md](testing.md), "Builds that
leave features out"), two of them without `FMINUS_JSON`; not in `dev` or `asan`, nor on macOS or Windows.
[research/syntax/README.md](research/syntax/README.md) is the report, §6.2; this is the reading half of its step 4.*

Five `json_float`s walk a document five times, and say nothing about which value was missing. A module says what it
wants once, by its fields, and the compiler knows them:

```c
enum Wave { SINE, SAW, SQUARE };

module Preset
{
    float gain [[fminus::range(0, 1)]];
    float cutoff [[fminus::range(20, 20000)]] [[fminus::default(1000)]];
    float q [[json::name("resonance")]] [[fminus::default(0.707)]];
    enum Wave wave;                                // "SAW", or 1
    char name[16];
    int loads [[json::skip]];                      // the program's own

    bool load(json_doc doc, int k) [[clang::nonblocking]]
    {
        return json_read(doc, "/presets/%d", k, this);   // false: gain, wave or name not filled
    }
};
```

### The builtin

`bool json_read(json_doc doc, "pointer", ..., Module* into)` is a row of `kRuntimeBuiltins` beside `json_floats`:
`Param::JsonRead`, `Param::Path` (so its path and its ints are every read's: `""` the root, its errors recipe 39's) and
`Param::Module`, the last argument, a pointer to the module it fills. `lowerFilled` (`Shapes.cpp`) lowers that one:
`&preset`, `this`, a parameter `Preset* p`, an array's name (its first element); the module's type's table
(`moduleShape`) goes to the runtime first, then the address, after the path's table and its ints:

```llvm
@json.shape.Preset = private unnamed_addr constant { i32, i32, [70 x i32], [41 x i8] } { i32 70, i32 41, ... }, align 4
  %json_read = call i32 @fminus_rt_json_module(ptr %doc1, ptr @.path, ptr %format.ints, i32 1,
                                               ptr @json.shape.Preset, ptr %p)
  %tobool = icmp ne i32 %json_read, 0
```

The result is the runtime's, 1 when every field without a default was filled, as a `bool` (`Result::Bool`). Since no
one C type takes a pointer to any module, `paramType` has none for `Param::Module`, so a prototype of `json_read` cannot
be written, as one of any builtin that takes a path cannot.

| The program writes | FMinus, at |
|---|---|
| `json_read(doc, "", x)`, `x` a `float*` (a `void*`, a module itself, a row) | "'json_read' fills a module: its last argument must be a pointer to one, not 'float *'", the argument |
| `json_read(doc, "", "m")` | the same, "not a string literal" |
| `json_read(doc, "", f)`, `f` a `const Fine*` | C's "passing 'const Fine *' to parameter of type 'Fine *' discards qualifiers", the call, as every passing error |
| `json_read(doc, "", n)` of a module with no field it fills | "'json_read' has nothing to fill in 'Notes': it fills no parameter, string, pointer, handle, poly type or field with [[json::skip]], and 'Notes' has no other field", the call |
| a module nested past 16 levels, of more than 1024 fields to fill, or past an int32's offsets | "'json_read' cannot fill 'M': ...", the call: the runtime would refuse its table |

### The table: a type made into words

The runtime's half came first, with its format (`runtime/Runtime.h`, "shapes", and `JsonShapes.h`, which checks a
table whole before it reads a byte by it). `Shapes.cpp` is the compiler's: **`appendShape(type, use, table, depth)`
appends a type's shape**, a value of it in memory, to a `ShapeTable`, its words and the bytes its names are offsets
into:

| Type | Words |
|---|---|
| `float`, `double`, `int`, `bool` | 1, 2, 3, 4 |
| an enumerated type's value | 7, its size (4, or 1 for `enum E : bool`), its constants' count, and each constant's value and name, as written, the order declared (`enumerations_`) |
| `float2`, `float4`, `float8` | 11, its lanes |
| `char name[n]` | 6, n |
| an array (a 2-D one an array of rows) | 9, its count, its elements' stride (the data layout's), its element's shape |
| a module | 12, its fields' count, each field's record (`appendField`) |
| a `string`, for writing only (`ShapeUse::Write`) | 5 |
| a pointer, a handle, a poly type (its width is the target's), a `char` array of no byte; for reading, a string | none (`ShapeProblem::NoShape`) |

A field's record is its size in words, its JSON name (an offset and a length), its offset (the struct layout's), its
flags, its range and its default when it has them, each a double as two words, and its shape. A range goes in when the
field's values are numbers (`float`, `double`, `int` or an enumeration's, through its arrays and a vector's lanes),
a default when they are numbers or `bool`s: what the runtime takes. **`appendField` leaves a field out** when
`leftOut` says so (a `param`, whose value is the host's: a program never writes one; a field with `[[json::skip]]`)
or its type has no shape, and the module's count is the fields it kept; the bools of the module's `when`s follow the
declared fields and are no field of the tree. A problem other than no shape (too deep, too many fields, too large) is
the whole table's. The handoff's worked example is the test's (`JsonReadTest.cpp`, "a module's table, word by word"):

```
module Filter { float cutoff [[fminus::range(20, 20000)]] [[fminus::default(1000)]]; float q; };
20, 7,                                                            words, bytes
12, 2,                                                            a module of 2 fields
12, 0, 6, 0, 3, 0, 1077149696, 0, 1087604736, 0, 1083129856, 1,  "cutoff" at 0, range 20..20000, default 1000, float
6, 6, 1, 4, 0, 1                                                  "q" at 4, no flags, float
"cutoffq"
```

**One table a type.** `moduleShape` makes the module's at the first call that names it, `json.shape.Preset`, a private
constant laid out as a path's table (`shapeConstant`), keeps it in `moduleShapes_`, and every later call passes the
same one; a nested module's shape is inside its outer one's, as the format has it. The writer of a JSON literal,
`fminus_rt_json_write`, takes the same shapes: since recipe 48 it writes every JSON literal, from a description
`jsonDescribe` (`Serve.cpp`) makes of the literal's numbers, bools and strings, whose shapes are these words' first
five kinds; a later step's literal of a module appends the module's shape with `ShapeUse::Write`, the same words but
for a string field, which it takes and the reader leaves out. (Recipe 51 made it so: one function, `appendShape`,
makes every shape of a literal's values too.)

### What it reads

The runtime's rules, pinned by `tests/unit/JsonModuleTest.cpp` and, through the JIT, by `JsonReadTest.cpp`: each field
from the member of its JSON name, byte for byte, the first of two of one name; a member no field names ignored; a
field filled only by a value of its kind inside its range (an `int` from an integer an `int` holds, an enumeration from
its constant's name as written or an integer, a `char` array from a string that fits with room for its 0, the rest
zeroed, an array or a vector from as many elements as fit, a nested module from an object by its own fields); a field
not filled set to its default, a nested module to its fields', or left as it was; a null document, a path that reaches
nothing, or no object there, nothing filled and the defaults set. An array of modules the document does not have is
left as it was, its elements' defaults unset: it has no default of its own, and counts as missing.

### The attributes

**`json::name` and `json::skip`** make `json::` FMinus's namespace (until now any `json::` attribute was the host's, as
every namespace FMinus does not know; no program or test in the tree used one). `checkAttributes` hands a module's
field's and parameter's to `checkJsonAttribute`, in source order among the others; a route's parameter's are not
handed over (`member`), and stay `Serve.cpp`'s "unknown attribute" there. Both still go to the host, as written,
with the field's others. Two fields under one JSON name are found when the module is laid out (`checkJsonNames`):
every field a literal or `json_read` could name counts, a parameter and a string among them, but not a pointer, a
handle or a skipped field.

**`fminus::default` on a field** is any field's whose values are numbers or `bool`s, a parameter's as before: on a field
that is no `param` it means what `json_read` sets the field to when a document does not fill it, and nothing else. **It
does not initialise the field**: a module's fields start at 0, as C's do, and `fresh()` of a module with defaults reads
0. An enumeration's is its constant's number. It is checked against the field's range as a parameter's is, and an
`int`'s must be an integer, a `bool`'s 0 or 1, as a route parameter's must. `[[fminus::range]]` on such a field also
means a value outside it does not fill it. A `string` field takes no default, and the error says why (`json_read`
leaves it out, so the default would never be used), while a route's `string` parameter takes a string literal
(recipe 48): one function checks both (`checkAttributes`), and a number's default is no string, nor a string's a
number, for a field and a parameter alike.

| The program writes | FMinus, at |
|---|---|
| `[[json::name]]`, `[[json::name("a", "b")]]` | clang's "'json::name' attribute takes one argument", the attribute |
| `[[json::name(1)]]` | clang's "'json::name' attribute requires a string" |
| `[[json::name("")]]` | "'json::name' takes a name of one byte or more: the JSON member's the field is read and written as" |
| `[[json::name("\xff")]]` | "'json::name' takes UTF-8 text, as a JSON member's name is, and byte 1 of the name is no part of any" |
| `[[json::skip(1)]]` | clang's "'json::skip' attribute takes no arguments" |
| `[[json::rename("x")]]` | "unknown attribute 'json::rename': a field takes json::name("name") and json::skip" |
| `float c [[json::name("a")]];` after a field `a` | "duplicate JSON name 'a' in module 'M'", the second's name, and a note at the first, "previous field of that JSON name is here" |
| `string t [[fminus::default(1)]];` | "'fminus::default' does not apply to a string field: json_read leaves a string field out, so its default would never be used" |
| `int i [[fminus::default(1.5)]];`, `bool o [[fminus::default(2)]];` | "the default of an int field is an int, not 1.5", "the default of a bool field is 0 or 1, not 2" |
| `[[name("x")]]`, `[[skip]]` on a field | clang's warning, ignored, with a note: "did you mean 'json::name'?" (and `[[default]]` on a field that takes one, "did you mean 'fminus::default'?") |

### What the runtime promises

`json_read` is `RuntimeEffect::WaitFree`, as every read is, so a `[[clang::nonblocking]]` function may call it; the
runtime keeps the promise (no allocation, lock or system call: `JsonModuleTest.cpp`'s proof, and `JsonReadTest.cpp`'s
through the JIT, `HeapCalls` and seccomp's strict mode). Wait-free is not cheap: one walk of the object's members,
each compared with the fields not yet named, and each value read whole (recipe 39, "What a read costs"). Its
declaration says what `Json.cpp` does (`RuntimeKind::ReadModule`): a read's, `inaccessiblemem: read` for the document
and its tables read only, but `argmem: readwrite`, since it writes the module, which keeps no pointer (`captures(none)`,
not `writeonly`, so that the facts stay true if the runtime ever reads a field first):

```llvm
declare i32 @fminus_rt_json_module(ptr readonly captures(none), ptr readonly captures(none),
                                   ptr readonly captures(none), i32, ptr readonly captures(none), ptr captures(none)) #1
attributes #1 = { nofree nosync nounwind willreturn memory(argmem: readwrite, inaccessiblemem: read) }
```

So a field stored before the call and read after it is loaded again (`JsonReadTest.cpp`, through `this`), and a
local's zeroing stays before the call at `-O2` (`json-read.fm`): a field the document does not fill keeps it.

### Without `FMINUS_JSON`

`json_read` is a known name (`Feature::Json`): "'json_read' needs the JSON parser, which this build of FMinus leaves out
(FMINUS_JSON=OFF)". The `json::` attributes and a field's `fminus::default` are the language's, checked in every
build.

### What it changes for programs and hosts

- **Programs**: `json_read` is a builtin's name (a function of that name is refused). A field's `fminus::default`, an
  error before, compiles; a `json::` attribute that FMinus does not know, or one of its two misused, is an error
  where it was the host's; and two fields under one JSON name are an error. The message for an unknown `fminus::`
  attribute on a field names `fminus::default(value)` too.
- **Hosts** see the `json::` attributes and a field's `fminus::default` in `Module::Field::attributes` as before;
  `setDefaults` sets parameters only.
- **The VS Code grammar** (0.23.0, the version recipe 49 gave it) colours `json_read`.

### Tests

- `tests/unit/JsonReadTest.cpp` (`[json-read]`): through the JIT, every kind of field (`float`, `double`, `int`,
  `bool`, enumerations of `int` and of `bool` by name and by number, a `char` array, 1-D and 2-D arrays, a `float4`, a
  nested module and an array of them) at the root and at a path with its ints; missing, of another kind and out of
  range, with and without defaults, each element of an array by itself, a member no field names, two of one name, no
  object, no document, no path; a module whose every field has a default; a default that does not initialise; names
  (escaped, UTF-8, the last of two) and `json::skip`; a string, a pointer, a handle and a parameter left alone; `this`
  and what a method reads after the call; 100 rounds allocating nothing and a child under seccomp's strict mode; the
  tables word by word (the handoff's `Filter` and `Osc`, and a module holding both), one table for three calls; the
  errors, each once at its column (modules nested 17 deep and one of 1025 fields among them, beside 16 and 1024, which
  are read), and the warnings' notes; a nonblocking and a nonallocating function; the name in
  every build.
- `tests/unit/FieldAttributeTest.cpp` and `tests/library/FieldAttributeTest.cpp`: the changed messages, and the
  `json::` attributes and a field's default as a host reads them.
- `tests/lit/fminus/json-read.fm`: the table and the call at `-O0`, the declaration's attributes, the zeroing kept at
  `-O2`, a document read through the JIT and as an executable, and `examples/settings/`; `json-read-errors.fm`: the
  errors; `json-off.fm`: the option's error.
- `tests/unit/ServeTest.cpp`, where it meets recipe 48: a route with a `string` parameter of its path and one of its
  query and the request, whose body `json_read` reads into a module, stored by a file name with a hole in a directory
  the test makes (a whole module 201 and the file written as sent; a value out of range 422 and nothing written; a
  hidden name `FILE_INVALID`, 400); and the one rule of a field's and a route parameter's attributes (a `string`
  field's default refused, saying why; a number's default no string; a route's `string` parameter's a string literal;
  `json::name` and `json::skip` unknown on a route's parameter), each error once at its column.

## Recipe 51: a JSON literal of arrays, vectors, enumerations and modules, and JSON as a body and a file

*In the compiler, and an entry point each in the runtime and the web library. Code: `json (expression)` (`Parser.y`,
`Ast.h`'s `JsonExpr`, its dump); the literal's description and values (`src/ir/Shapes.cpp`: `lowerJsonLiteral`,
`describeJson`, `describeJsonValue`, `addJsonShape`, `describeJsonSlice`, `describeJsonThis`, `parameterIn`,
`jsonKey`, `JsonPlan` and `JsonLiteral` in `Lowering.h`; `jsonDescribe` and `lowerJsonReply` are gone from
`Serve.cpp`); the sinks (`src/ir/Calls.cpp`: the JSON forms of `http_post`, `file_write` and `file_append` in
`kRuntimeBuiltins`, `findJsonForm`, `Param::Json` and `Param::Elements`, `http_reply_json` and `json_slice`'s rows,
`jsonWriter`, the facts of the three new functions; `Serve.cpp`'s `replyJson`, which a reply of a literal calls through
`http_reply_json`); the library (`fminus_rt_file_write_json` and `_append_json`, `src/runtime/Files.cpp`,
`Runtime.h`, `RuntimeSymbols.def`; `fminus_web_http_post_json`, `src/web/Web.cpp`, `Web.h`, `WebSymbols.def`); a hole's
default reported once (`checkRouteParameters`). The example `examples/presets/`, the research's service in full. Tests:
`tests/unit/JsonLiteralTest.cpp`, `JsonServeTest.cpp` and `JsonFileTest.cpp` (new), cases of `WebServerTest.cpp`,
`ServeTest.cpp` and `FrontendTest.cpp`, `tests/lit/fminus/json-literal.fm`, `json-literal-web.fm`,
`json-literal-errors.fm` and `json-literal-off.fm` (new), and `serve-errors.fm`'s changed lines. Built with `-Werror`
and tested in the `release` preset on Linux x86-64 only, in two trees, the default and one with the web library, its
server, file writing and absolute names, every test passing but the one this machine cannot run (the smoke test of
LLVM's own `opt`), and since also in the three trees that leave features out ([testing.md](testing.md), "Builds that
leave features out"), without `FMINUS_JSON` and with the web library and not its server among them; not in `dev` or
`asan`, nor on macOS or Windows. The
libraries' new functions and their tests, with the libraries' others, ran under GCC 13's AddressSanitizer with
UndefinedBehaviorSanitizer and under ThreadSanitizer (below). [research/syntax/README.md](research/syntax/README.md)
is the report, §6 and §8; this is the last of its steps. [web.md](web.md), "The JSON literal", and
[files.md](files.md), "A JSON literal as a file", are the manual.*

Recipe 48 wrote a literal's numbers, bools and strings, and refused the rest; the runtime's writer already had a shape
for everything else (track 3's), and recipe 50 a function that makes a module type's. This recipe joins them, and gives
a literal its two other sinks, so the research's data work is now the program it wrote:

```c
module Day { int sensor; float mean; float peak; };

Day day[SENSORS];
// ... each day's mean and peak
file_write("out/summary.json", json { "days": day });                         // an array of modules
http_response sent = http_post("https://api.example.com/summaries", json { "days": day });
```

### One shape-maker

A JSON literal's description was `jsonDescribe`'s, which wrote the kinds `1` to `5` by hand; json_read's table was
`appendShape`'s. Now **`appendShape` makes every shape**, for both: `describeJsonValue` asks it for the shape of each
value's type with `ShapeUse::Write`, and `JsonPlan` holds a `ShapeTable` (its words and its bytes, the literal's own
and the names its shapes carry: a field's JSON name, an enumeration's constants) that `shapeConstant` lays out, as it
does a module's table. What is kept: `appendShape`, `appendField`, `leftOut` and `jsonName` as recipe 50 wrote them,
and the literal's walk (`describeJson`, an object's keys and an array's punctuation as bytes, a key escaped by
`jsonKey`, now `Shapes.cpp`'s, on its UTF-8 check). What went: the hand-written kinds, `lowerJsonReply`'s own layout of
the constant, and its storing of the slots, which `lowerJsonLiteral` does for every sink. So a module type's shape is
the same words in json_read's table and in a literal's description, but for a `string` field (written, not read).

### What a value becomes

Each value is an operation, `1, slot, shape`, and its slots, filled in the order the values are written, once each:

| A value | Its shape | Its slots |
|---|---|---|
| `float`, `double`, `int`, `bool` | 1, 2, 3, 4 | the value, at its slot's start in its own type (a bool a byte) |
| an enumeration's value | 7: its size, its constants and their names (appendShape) | the value (an `enum E : bool`'s a byte) |
| a `string` | 5 | its pointer and its length, two slots |
| `float2`, `float4`, `float8` | 11 and its lanes | its lanes, one, two or four slots (stored at a slot's alignment, 8) |
| an array (a `char` array a string, shape 6; 2-D, an array of rows) | 8, a pointer, around 9 and its element's | its address |
| a module, a value or `*p` | 8 around 12, its fields' records | its address |
| `json_slice(a, n)` | 10, a span: capacity, stride, the element's | the address and the count, two slots |

A value's type comes from `staticType` when it is an array (whose name lowers to its first element's address, the
array's own), else from the value lowered as a string is wanted (so a literal is a string). A pointer is an error
that says to write `*p` or `json_slice(p, n)`; a handle, a poly type, a bool vector, and an array of any of them,
the error that lists what a value may be. A pointer shape costs a level: an array or a module nests 16 deep below it,
the writer's bound (json_read's table, with no pointer, takes one more).

**An enumeration's value is its constant's name** (`"SAW"`), the first constant of that value as written, or its
number when none has it: the error recipe 45 put there ("cast it to int for its number", so that an `int` then would
not change what a program's replies said now) is gone, and a cast still writes the number.

**`json (value)`** is a fourth form of the literal, `"json" "(" expr ")"`, its `JsonValue` a value at the top: no
conflict (507 states, against 504 before), and it dumps as `(json preset)`.

### A module's object, and its parameters

A module is written by its shape, recipe 50's: the same fields as `json_read`'s table, under the same names
(`json::name`), the same left out (`json::skip`, pointers, handles, poly types, a `when`'s state), but a `string`
field, which is written. **A parameter** is the hard case. Its slot is a stream's pointer and a value (the knob's,
which the host stores with an atomic store, or a flat stream's), and a frozen one has none at all: its value is a
constant compiled into its methods. So the writer, which reads memory, cannot write one from the module's bytes, and
the rule FMinus already had decides where it can be written: **a parameter is read only in a method of its own
module, as `cutoff[i]`** (`parameterOf`, recipe 14; `m.cutoff` anywhere is "parameter 'cutoff' is read as
cutoff[i], in a method of 'M'"). So a literal writes a module with a parameter only as `*this` in one of its own
methods, and there field by field (`describeJsonThis`): each field the module's shape has by its address and a pointer
shape, each parameter as `cutoff[0]` reads it (`lowerParameter`: a knob's value, read where the method starts, a
frozen one's constant, a stream's first sample of the call), the object's text the same as the writer would make of
the module's shape with the parameters in their places. Anywhere else (another instance, `*p` outside its methods, a
module or an array holding one: `parameterIn` looks through them) it is an error that says why and how to leave the
parameter out: "a JSON literal writes a parameter's value only where a method of its module reads it, as *this:
'cutoff' of 'Synth' is one (leave it out with [[json::skip]])".

### `json_slice`

`json_slice(array, count)` is a row of `kRuntimeBuiltins` with no function (`Param::Elements`, an array or a pointer:
no one C type, so no prototype can be written), whose call anywhere but in a literal is an error; in a literal,
`describeJsonSlice` makes it a span. **The capacity**: an array's own size, so the writer clamps the count to it (a
negative count is 0); for a pointer, an `int`'s greatest, 2147483647, so the count alone bounds it, the program vouching
for the elements as it does for `bytes_floats`' (the description cannot know more, and a smaller bound would cut a
count the program meant). Its elements are anything a literal writes but a `char` (a `char` array is a string, written
whole; `string_slice` makes part of one).

### The sinks

```llvm
@.json = private unnamed_addr constant { i32, i32, [65 x i32], [57 x i8] } { i32 65, i32 57, [65 x i32] [i32 0, i32 0,
         i32 8, i32 1, i32 0, i32 10, i32 3, i32 12, ... ], [57 x i8] c"{\22days\22:sensormeanmost,\22taps\22:,..." }
  %file_write = call i32 @fminus_rt_file_write_json(ptr %files.context, ptr @.str.1, i32 11, ptr null, i32 0,
                         ptr %format.texts, ptr %format.lengths, i32 1, ptr @.json, ptr %json.values)
  %http_post = call ptr @fminus_web_http_post_json(ptr %web.context12, ptr @.str, i32 27, ptr %format.ints, i32 2,
                         ptr null, ptr null, i32 0, ptr @fminus_rt_json_write, ptr @.json, ptr %json.values)
```

- **A reply**, `reply status, json ...;`, is now a call of **`http_reply_json(request, status, json ...)`**, a builtin
  a program in the `http_is` style may call itself: `lowerReply` hands `lowerRuntimeCall` its row with the request
  given, so the status is checked as every builtin's argument is, and `replyJson` emits recipe 48's sequence
  (`reply_buffer`, `json_write` into it, `reply_written`), unchanged, wait-free.
- **A POST and a file** need a buffer of the text's size, known only by writing it. The writer returns the size the
  whole text takes and writes nothing past its capacity, so **the library's entry point measures first** (a call with
  no buffer), checks the size as it checks `bytes` (`maxBodyBytes`: `HTTP_LIMIT`; `maxFileBytes`, the quotas:
  `FILE_LIMIT`), in the same order, then writes once into a buffer of exactly that size: the request's own body (a
  `std::vector` resized, so no second copy), or the buffer the file is written from (one, besides the copy into the
  file itself). The values are in the frame's slots, stored once before the call, and read twice; a second text of
  another size (what another thread's change between the passes would make) is `HTTP_INVALID` or `FILE_FAILED`.
  `file_append` adds a newline (JSON Lines), counted against `maxFileBytes` with the text.
- **Each takes the address's strings always** (null, null and 0 when the name or the URL has none: the `_s` functions
  showed a format with no `%s` is exactly the plain one's), so there is one function a sink and no `_s` twin.
- **The web library does not link the runtime**, and an executable links the runtime before it (`Aot.cpp`), so a
  call of `fminus_rt_json_write` from `Web.cpp` would not resolve there. So **the program passes the writer's
  address**, `jsonWriter`, a declaration of `fminus_rt_json_write` described as a call of it would be: the JIT defines
  it, an executable's object references it, and the library stays free of the runtime (and of `FMINUS_JSON`).
- The JSON forms are rows of the same names after the plain ones (`findJsonForm`): a call whose second argument is a
  literal takes its row, so the effects (they allocate), the build options (`'http_post' needs the web library`
  first, then the literal's "needs the JSON writer"), the URL's and the name's checks and the host's lists are the
  plain form's. A literal anywhere else is "FMinus has no JSON values: a JSON literal is only the body of a reply,
  http_reply_json, http_post, file_write or file_append".

### What the functions touch, told to LLVM

The three new functions are their kinds' (`WebStart`, `FileWrite`), and read any memory besides (`describeRuntime`:
a name ending in `_json` gains `readOnly()`), since the slots hold addresses of arrays, modules and strings no argument
points to: an argmem-only declaration would let LLVM delete the stores it reads (recipe 48's lesson). At `-O2` the
stores into a local array of modules stay before `fminus_rt_file_write_json` (`json-literal.fm`) and
`fminus_web_http_post_json` (`json-literal-web.fm`):

```llvm
declare i32 @fminus_rt_file_write_json(ptr, ptr readonly captures(none), i32, ptr readonly captures(none), i32,
        ptr readonly captures(none), ptr readonly captures(none), i32, ptr readonly captures(none),
        ptr readonly captures(none)) #1
attributes #1 = { nounwind willreturn memory(read, argmem: readwrite, inaccessiblemem: readwrite, errnomem: readwrite) }
```

### Errors

| The program writes | FMinus, at |
|---|---|
| `json { "p": p }`, `p` a `float*` | "a JSON literal does not write a pointer, 'float *': write what it points to, *p, or its first elements, json_slice(p, count)", the value |
| `json { "h": h }`, `h` an `http_server` (an array of handles, a `poly float`, a `bool4`) | "a JSON literal's value is a float, a double, an int, a bool, a string, an enumeration's value, a float2, float4 or float8, an array or a module, or an object or an array of them, not 'http_server'" |
| `json (s)`, `Synth` having a parameter, outside its methods | "a JSON literal writes a parameter's value only where a method of its module reads it, as *this: 'cutoff' of 'Synth' is one (leave it out with [[json::skip]])" |
| `json_slice(a, 2);` as a statement | "'json_slice' is a JSON literal's value, the first elements of an array written there, and nothing anywhere else", the call |
| `json_slice(text, 2)`, `char text[8]` | "'json_slice' writes elements a JSON literal writes, and a char is none: a char array is a string, written whole" |
| `json_slice(n, 1)`, `json_slice("ab", 1)` | "'json_slice' takes an array or a pointer, and a count: not 'int'" ("... not a string literal") |
| `http_reply_json(r, 200, n)` | "'http_reply_json' takes a JSON literal here: json { ... }, json [ ... ] or json (value)" |
| `int json_slice(float* a, int n);` | clang's "conflicting types for 'json_slice'", as for any builtin with no prototype |
| any sink's literal without `FMINUS_JSON` | "a JSON literal needs the JSON writer, which this build of FMinus leaves out (FMINUS_JSON=OFF)", the literal; `json_slice` "... needs the JSON writer ..." |

And recipe 48's small debt: a hole's `int` parameter with `[[fminus::default("x")]]` had two errors at one column
("takes one number" and "does not apply to a hole's parameter"); `checkRouteParameters` now hands `checkAttributes` a
hole's attributes without its default, a number's as a string's, so the one error is the hole's.

### What it changes for programs and hosts

- **Programs**: a JSON literal writes arrays, vectors, enumerations (by name: what a literal of an enumerated value
  wrote changed from an error to `"SAW"`) and modules; `http_post`, `file_write` and `file_append` take one;
  `http_reply_json` and `json_slice` are builtins' names. Nothing a program compiled before means anything else: every
  value newly written was an error.
- **Hosts** see no new API; `Program::files()` and `webRequests()` list a JSON form's name and URL as the plain one's.
  The runtime and the web library have one new function each sink (`RuntimeSymbols.def`, `WebSymbols.def`), and the
  web library's takes the writer by its address (`fminus_web_json_writer`, `Web.h`).
- **The VS Code grammar** (0.25.0) colours `json_slice` and `http_reply_json`.

### Tests

- `tests/unit/JsonFileTest.cpp` (`[runtime][files][json]`), the runtime's half from C++ with descriptions built as the
  compiler lays them out (`Shapes.h`): a span of modules and a string written whole and appended as JSON Lines; the
  size limit on the text and on its newline; the quota; the policy's refusals, a null context, a description the
  writer refuses (`FILE_INVALID`, counted against no quota). `WebServerTest.cpp`'s last case, the web library's:
  the text measured and written into the body, sent as `application/json`, with and without strings in the URL,
  `maxBodyBytes` one byte short and exact, a description refused, no writer, a `..` segment, a null context.
- `tests/unit/JsonLiteralTest.cpp` (`[json]`): through the JIT, a module of every kind of field written as `json (*a)`
  and read back with `json_read` into one equal to it byte for byte, and the exact text; each kind of value at the
  top, in an object and in an array; `json_slice` of an array (clamped, negative), of a pointer, of rows and of strings;
  JSON names (escaped, UTF-8), `json::skip`, the fields left out, a `char` array of no 0 and of no letter; `*this` with a
  knob, a frozen parameter and a stream; JSON Lines, `FILE_LIMIT` with values evaluated once; every error at its column;
  in a build without file writing, without the web library or without the JSON writer, the error that names it.
- `tests/unit/JsonServeTest.cpp` (`[json]`, with the web server): a reply of a module, an array of them, a slice and an
  enumeration; `http_reply_json` in the `http_is` style and its `HTTP_LIMIT`; the wait-free proof of a reply's module
  in a `[[clang::nonblocking]]` method (`HeapCalls`, and a child under seccomp's strict mode); the research's data
  work, eight sensors' days fetched at once in a `within` from a loopback server the test starts, written with
  `file_write` and posted with `http_post`, the file and the body the same text, which the server reads back with
  `json_read`; a POST's literal over `maxBodyBytes`, `HTTP_LIMIT` with its values evaluated once.
- `tests/unit/FrontendTest.cpp`: `json (value)` and `json_slice` in the dump. `ServeTest.cpp`: the changed messages,
  and a hole's default once.
- `tests/lit/fminus/json-literal.fm`: the description word by word, the slots at `-O0`, the call, the declarations,
  the stores kept at `-O2`, a run through the JIT and as an executable, the file and the line it wrote;
  `json-literal-web.fm`: a POST's call with the writer's address, the declarations, and a program posting an array of
  modules to its own server; `json-literal-errors.fm`, and `json-literal-off.fm` in a build without `FMINUS_JSON`.
- `examples/presets/`: its check stores two presets, each the file of its module written back, and refuses a body out
  of range, without a field or with a wave no constant names.
- **Sanitizers**, outside the tree (a harness of track 3's, GCC 13, the web library, llhttp and the runtime's
  `Files`, `FilesPosix`, `Json`, `JsonWrite`, `Numbers` and `Xml` instrumented, the JIT's tests compiled by clang
  uninstrumented, since `Compile.h`'s vectors are clang's): `JsonWriteTest`, `JsonModuleTest`, `XmlTextTest`,
  `FileTextTest`, `FilesTest`, `JsonFileTest`, `WebTextTest`, `WebTest`, `WebServerTest`, `JsonLiteralTest` and
  `JsonServeTest`, 157 cases, under AddressSanitizer with UndefinedBehaviorSanitizer, no report; and 148 under
  ThreadSanitizer (all but the nine fork-based proofs, which TSan's own thread keeps from exiting), no report. They
  run there since: their helper, `tests/unit/StrictMode.h`, ends that thread itself ([testing.md](testing.md), "A
  wait-free proof"; [web.md](web.md), "What is verified where").

## Pitfalls

Each of these was hit while writing the language or its recipes. [simd.md](simd.md) has the vector ones.

| Symptom | Cause | Fix |
|---|---|---|
| Code lands in a dead block, or the verifier reports a block without a terminator, wherever you ask whether the current block is finished | LLVM 23's `BasicBlock::getTerminator()` assumes a finished block; on an unfinished one it returns `&InstList.back()`, a bogus non-null pointer (the assert is compiled out under `NDEBUG`) | `hasTerminator()`, or `getTerminatorOrNull()` |
| A syntax-error test changes text after you add an unrelated token | Bison's `parse.error detailed` lists the expected tokens only when there are at most four, and a new token that can start a construct joins the list (or pushes it past four) | update the test; or `%define parse.error custom` and write `Parser::report_syntax_error` |
| Bison: "shift/reduce conflict on token "else"" | the dangling else (recipe 4) | `%precedence THEN` below `%precedence "else"`, and `%prec THEN` on the if without an else |
| Bison: "shift/reduce conflict on token "["" after a declarator's attributes, and only a warning, so the build goes on | a rule that ends after the attributes (`declared_name`), before sizes that also begin with `[`: Bison must choose at the `[` whether to end it, and keeps the shift | no rule ending between the two: each alternative spells out `pointer IDENTIFIER attributes` (recipe 35); read `Parser.output` for "conflicts" after a grammar change |
| The build fails in `Ast.cpp` and `src/ir/` after you add a node or an enumerator | `std::visit` needs every alternative; `-Wswitch` every enumerator | handle each one: this is the checklist working |
| An IR test fails after an unrelated change | an exact match on a line that gained an attribute, a name or a flag | match loosely: `{{.*}}`, `[[NAME:%.*]]`, `CHECK-LABEL`, `CHECK-DAG` |
| The verifier: "PHINode should have one entry for each predecessor" | the phi names the block a branch started in, but lowering it (a nested `&&` or `?:`) moved the builder on | take `builder_.GetInsertBlock()` after lowering each operand or branch |
| The verifier: "Instruction does not dominate all uses" | a conversion emitted in the join block for a value from one branch | convert at the end of the branch's own block, before its branch to the join (`lowerConditional`) |
| The verifier: "Basic Block does not have terminator", or two terminators in a block | a branch or loop body that ends in a return, given a second branch, or a path left open | `fallThrough(target)`: branch only when the block has no terminator |
| "non-void function does not return a value" for a function whose end is dead | reachability judged by predecessors: dead code after a `return` can branch | walk from the entry (`llvm::depth_first`), as `reachable()` does |
| A variable stays in memory at `-O2` | its `alloca` is not in the entry block | `allocate()`: an `IRBuilder` positioned at the entry block's start |
| C++ reads a wrong `bool` from JIT'd code | `i1` crossed the C ABI without `zeroext` | `addParamAttr` / `addRetAttr(llvm::Attribute::ZExt)`, as clang does |
| An assignment inside `?:`, `&&` or `||` runs when it shouldn't | the operator lowered to a `select`, which evaluates both sides | blocks and a phi; SimplifyCFG makes the `select` itself when it is safe |
| Bison: "multiple occurrences of $1 with api.value.automove" | automove moves `$n` on its first use | use each `$n` once; copy into a local first (as the declaration rule does) |
| A keyword scans as an identifier, or the reverse | Flex takes the longest match, then the earliest rule | keywords before `{IDENTIFIER}` |
| A runtime function works in `fminus` but is not found once FMinus is embedded | it is missing from `RuntimeSymbols.def`, and the tools found it among their own process symbols | add its `FMINUS_RUNTIME_SYMBOL` line |
| A constant like `0.1f` differs in its last bit from clang's | rounding through `double` first | keep NumberExpr's spelling; `APFloat::convertFromString` straight to the target type |
| LLVM asserts "Insertelement types must match!" in `(float4)(1, 2, 3, 4)` | once integer constants are `int`s, `lowerConstruct` converted each scalar to `arithmeticOf` its type, an `int`, and inserted an `i32` into a float vector | convert a scalar element to `float`; only a vector's lanes keep their own type |
| `typeName(type) + ...` will not build a name such as `'const float *'` | it returned a `const char*`, one per kind | return a `std::string`, built for a pointer |
| A lit test that a loop vectorises without an overlap check passes either way | LLVM 23 folds the check for a simple loop into `iter.check` as `%diff.check`, without a `vector.memcheck` block | check for `diff.check`, and check that it appears when the `restrict` is gone (`pointers.fm` runs both) |
| A C++ reference in a harness runs slower than the same code in FMinus | the examples and tests compile with LLVM's definitions, which turn on libc++'s hardening: a `std::vector`'s `operator[]` checks every index | write the reference on raw pointers, as the kernel is (`examples/delay/bench.cpp`) |
| A test of `sizeof(poly float)` fails to parse, at a function named `poly` | `poly` is a keyword | another name |
| The JIT crashes on a `void` value, or the verifier on a `void` phi | `PoisonValue` of `void`, or a `?:` that joins two `void` calls | `poison(Type::Void)` has no IR; `lowerConditional` builds no phi for `void` |
| An invalid program compiles, to `undef`, with no error (`float x = c ? f() : g();` of two `void` calls) | a stand-in with no error behind it was failed (recipe 32), and every use of a failed value is silent | `poison()` is not failed; only `lower`'s count and `recovery()`, where an operand's or a declaration's error is behind it, fail a value |
| A program compiles with a frozen parameter and is refused with a knob | a decision taken on the IR, where a frozen parameter's read is a constant (`branchOn` on a `ConstantInt`) | decide on the tree (`folds()`), and take only the value from the IR |
| The verifier: "Terminator found in the middle of a basic block" at a `return` after a `defer return;` | the deferred statement ended the block with its error's `ret`, and the exit added its own | after the deferred statements, a fresh block when the last one ended its own (`runDefers`, recipe 44) |
| The compiler crashes, somewhere unrelated (`Instruction::clone`), when a route can end without a reply | a walk of the CFG (`llvm::depth_first`) reached a block not finished yet, the serve's own, and LLVM 23's successors of an unfinished block read past its end | finish the block first, and walk only the route's own blocks, stopping at the serve's (`lowerRoute`) |
| An error in a deferred statement is reported once for each `return` | it is lowered at every exit of its block, and lowering checks | the first lowering reports; the others `Diagnostics::rewind` what they reported (`lowerDeferred`) |
| An `-O0` IR test's `CHECK-NEXT` fails after the code that emits the same instructions was moved, though nothing it emits changed | IRBuilder calls nested in one C++ call's arguments run in an order C++ leaves unspecified, and a helper called in an argument (`at(slot)`) where a local was before runs at another point | build each instruction in a statement of its own, in the order the IR should have (`lowerJsonLiteral`) |
| `json_float(d, ...)` of a `const void* d` "discards qualifiers" once documents are `json_doc`s | a read's document was a `const void*`, and C converts none to a pointer to a struct that is not `const` | the reads take a `const void*` as well (`Param::JsonRead`, `XmlRead`) |
| "arithmetic on a pointer to an incomplete type 'fminus::ast::Stmt'" in `<vector>`, after adding a field to a node | `std::vector<Stmt> init = {};`: a default member initialiser instantiates the vector where `Stmt` is incomplete | no initialiser, as `ForStmt::init` has none; the parser's aggregates pass `{}` |
| The compiler crashes on `p->y`, and `--emit ast` prints a `OnePole*` as `? *` | a pointer built from a type's kind, `pointerTo(type.kind, ...)`, lost its module | pass the `Type`: `pointerTo(type, ...)` keeps `module` |
| A C++ reference differs from FMinus in the last bit, though the formulas match | clang's default `-ffp-contract=on` fuses `y + k * (x - y)` written as one expression into a multiply-add; FMinus never fuses | build the reference with `-ffp-contract=off` (the examples are), or round once a statement (`ModuleTest.cpp`'s `next`) |
| A double accumulator drifts as a float would | what it adds was computed in float before it widened: `float step = 1.0 / rate;`, then `t += step;` (the constant's arithmetic ends at the float `step`) | compute it where it goes to the double, `t += 1.0 / rate;`, whose arithmetic has the double `t` (recipe 33) |
| A constant beside a double is a float after all (`x * 0.1 * d` gave a float product) | the constant was lowered before the operand that makes its arithmetic a double, which only lowering it would have shown | `staticType`, which works the operand's type out from the declarations before anything is lowered; never reorder the lowering, which is the program's order of evaluation |
| A host's `aligned_alloc` returns null for a module | macOS's wants an alignment of at least a pointer's, and every implementation a size that is a multiple of it | round both up to `alignof(std::max_align_t)` (`examples/voices/bench.cpp`) |
| One error in a module hides every other | an error the parser reports (a redefined module) stops the run before the code generator | report it in the code generator (`layOut`), and the parser carries on |
| A method's loop over a field's buffer (`tail[i] += result[i]`) runs at half C++'s speed, and doesn't vectorise | without TBAA, LLVM assumes a `float` store may change the field, a pointer, and loads it again every iteration | the type's tag on every load and store (recipe 17); under `--no-strict-aliasing`, read the field into a local before the loop |
| An arm64 build of a SIMD C library runs at scalar speed, with no error | PFFFT uses NEON only when asked (`PFFFT_ENABLE_NEON`); it says otherwise only in a `#pragma message` | define it (`cmake/FMinusFft.cmake`), and check `pffft_simd_arch()` once |
| A third-party file fills the build log with warnings | PFFFT's SIMD headers announce themselves with `#pragma message`, which clang reports as warnings | `-Wno-#pragma-messages` for that object library only |
| A debug build aborts in `pffft_new_setup` | it asserts on more than 2^26 points | refuse the size before calling it (`Fft.cpp`) |
| A lit test runs a check line as a command | a FileCheck prefix named `RUN` | any other prefix (`fft.fm` uses `PEAK`) |
| `fminus-tests "a module's methods, and ..."` runs no test | Catch2 splits a filter at its commas | run by tag (`"[module]"`), or escape the comma (`\,`) |
| A knob read in a loop is loaded every iteration, and the loop doesn't vectorise | a monotonic atomic load where `cutoff[i]` is, or in a method inlined into the loop: LLVM neither hoists nor merges monotonic loads | load it once, in the entry block (`atEntry`, recipe 14), and unordered, which LLVM may move out of a loop it was inlined into |
| A method's state (`y += ...`) goes back to memory every sample once a parameter is a stream | the stream's pointer comes from the module, so LLVM can't tell its samples from the module's fields | an alias scope for the streams (`scopeStreams`, recipe 14) |
| A frozen parameter moves an output's last bit, against the same value as a knob | LLVM constant-folds `sinf`, `cosf`, `expf` and `tanhf` of a constant with the host's double functions, rounded to float, not with libm's float ones, which differ from them by an ulp for about 1% of arguments | the vector or libm math mode (recipe 15), whose calls LLVM can't fold; in the fast mode, compare within a tolerance |
| FileCheck reports a bad variable on a line that checks `[[fminus::range(0, 1)]]` | `[[...]]` is FileCheck's own syntax | match the brackets as regexes: `{{\[\[}}fminus::range(0, 1){{\]\]}}` (`params.fm`) |
| A loop of a runtime function with a vector variant stays scalar | the variant's declaration had no use, and `GlobalDCE` removed it before the loop vectoriser ran | `llvm::appendToCompilerUsed` (`Math.cpp`, as `InjectTLIMappings` does) |
| A four-lane runtime function's fast path calls libsystem_m and returns, with a stack frame, rather than branching to it | from a `noexcept` function, a call of a function not known not to throw is inside a `std::terminate` landing pad | declare the C functions it calls `noexcept` (`runtime/Math.cpp`) |
| A C++ reference that calls `fminus::math` runs six times slower than with `std::sin` | an opaque function is called every time; clang merged `std::sin`'s repeated calls | `[[gnu::const]]` on the declarations (`FMINUS_MATH_CONST`) |
| C++'s `std::sin(x)` differs from libm's `sinf(x)` by an ulp, in a function that also takes `std::cos(x)`, and only in an optimised build | clang pairs the two into `__sincosf_stret` from `-O1` on Apple systems; a debug build calls the two | compare with `fminus::math` (recipe 15); against libm, call through pointers clang can't see through (`ProgramTest.cpp`) |
| On x86-64 with AVX, a `poly float`'s `sinf` of 8 lanes or more gives its upper four lanes `sinf(0)` when any lane is past 2^16, an infinity or a NaN; arm64, and 4 lanes, are right | the `preserve_all` wrapper `fminus.large.sin` restores `ymm1`-`ymm15`, and LLVM's vzeroupper insertion then puts a `vzeroupper` before its `ret`, zeroing the upper halves it has just restored | compile the wrapper without the tuning feature (`target-features`: the machine's and `-vzeroupper`), and call `llvm.x86.avx.vzeroupper` before the runtime's function by hand (`largeFunction`, recipe 24; `math-large.fm`) |
| A lane's `_simd_sin_f4` changes when its neighbour is 2e6, an infinity or a NaN | libsystem_m takes another path for all four lanes when any is past 1e6 or not finite | the runtime's guard: such vectors lane by lane (`kSinCosLimit`) |
| LLVM asserts on a loop of `sinf`, or removes a call of a program's `fminus_rt_sinf` that may loop forever | the program declared a function of a runtime math name: the four-lane one became the scalar function's vector variant, the scalar one took the runtime's attributes | refuse the names of a called builtin's runtime functions, in every mode (`checkMathNames`, recipe 15) |
| `BranchInst` is deprecated | LLVM 23 splits it into `CondBrInst` and `UncondBrInst` | `llvm::dyn_cast<llvm::CondBrInst>`, and its `getCondition()` |
| A local array goes back to memory in every copy of a versioned method | the split left its alloca after the entry block, where it is dynamic | move the static allocas into the entry block before splitting (`KnobVersioning.cpp`) |
| A copy's values are named `%cutoff.at.cutoff.flat1` | `CloneBasicBlock` suffixes every name | when no value falls back, make the original body one of the copies: the common one keeps its names |
| Knob versioning finds no knob at the pipeline's start | the `-O0` IR keeps a knob copied into a local in a stack slot | run after the first SROA (the early simplification extension point) |
| A loop over a stream that knob versioning did not copy stays scalar, though its stride is 0 or 1 | InstCombine makes `i * stride` a select of 0 and `i`, and the loop vectoriser then sees no strided load | an opaque 1, an empty asm statement's (`opaqueUnit`), on which the vectoriser versions the loop; the pass makes it 1 again in its copies (`foldUnits`) |
| A stream read only before the loop is versioned on, and crowds out of the budget a mode that steers the loop | its read selects on its test outside every loop, and the loop uses what it read | count as steering only a select of where the samples are, the base or the stride, that a loop uses (`addressesStream`) |
| A local array keeps its memset in every copy of a versioned method | the copies shared one alloca, zeroed in each copy and read in all, so no copy's memset dominates the loads | a local of its own in each copy, its lifetime marked so that the frame does not grow (`markLifetimes`) |
| ASan's container-overflow in `std::vector<PassConcept>`, on an `addPass` in a callback | LLVM's vector, built without ASan's annotations, grown by instrumented code | a false positive: the asan preset's `ASAN_OPTIONS` has `detect_container_overflow=0`; set it when running `fminus-tests` by hand |
| A versioned output differs in its last bits | in the fast math mode, a copy's loop vectorised a math call that the method's loop kept scalar | the default math mode (recipe 15); or compare within a tolerance |
| A test that a local reads 0 passes even when its memset is wrongly removed | fresh stack pages are 0, and so is much of a stack a test has not used | run each call on a dirty stack (`dirtyStack()`, `padded()` in `ZeroInitTest.cpp`) |
| The dirty stack reaches all of a callee's local but its top few elements | the painting function's own saved registers and stack canary sit at the top of what it painted | call through a frame of padding |
| The padding frame is 16 bytes, or not there at all | SROA splits a `volatile` array touched at two constant offsets into two bytes, and the call through it became a tail call | escape the padding's address through an empty `asm volatile`, before and after the call |
| A memset zero-init elision proves dead at `-O2` stays at `-O1` | at `-O1` no jump threading has run, so the loop's guard skips into the code that reads | the guarded proof: block the skipping edge, and ask that each load reachable from it need the guard's condition |
| A bound read from a field (`i < count`) keeps the memset | the field's load comes after the declaration, so its definition does not dominate the memset | a value defined after the memset is stable if the order search from its definition reaches no load |
| `t[0]` after a loop keeps its memset at `-O1` but not at `-O2` | GVN at `-O2` forwards the memset's 0 into the path where the loop does not run | correct both ways: check each level's IR separately (`zeroinit.fm`'s `O1` and `O2` prefixes) |
| `fminus -O0` shows fewer memsets than the lowering wrote | LLVM's `-O0` pipeline (`buildO0DefaultPipeline`) runs the passes registered at the scalar-optimiser-late extension point too | add the pass from `-O1` only (`level != O0` in the callback) |
| A test that a function returns null for a refused size starts failing at `-O2` | the block is never used, so LLVM removes it (allocation elision), and a removed block does not fail | give the block to the host, or use it, so the call is made (`HeapTest.cpp`) |
| A loop's `!llvm.loop` is missing from the `-O2` IR | passes that duplicate a latch (JumpThreading unfolding a `select` in a loop with `?:`) do not always copy it | check the metadata at `-O0`, where the lowering wrote it (`progress.fm`) |
| `candidate.symbol == function.getName()` does not compile | `std::string_view == llvm::StringRef` is ambiguous | compare `std::string_view(function.getName())` |
| A frozen parameter's subnormal result differs from a knob's, though every function is `denormal_fpenv(preservesign)` | IRBuilder's `ConstantFolder` folds two constants in IEEE arithmetic as it builds them, and at `-O0` GlobalISel folds what is left in IEEE too | fold as the mode computes, as the IR is built (`DenormalFolder`) |
| An entry point costs about 7 ns a call even in a host that flushes already | it wrote FPCR twice, and an MSR to FPCR costs about 4 ns on Apple cores even when it changes nothing; MRS is free | write only when the caller's mode does not flush: compare first |
| An entry point computes in the host's mode at `-O1`, though it sets the flushing one | its body touches no memory, so LLVM orders its call against nothing, and InstCombine sank it below the write that restores the caller's mode | call the body through its address passed through an empty inline asm: a call of a function LLVM cannot see (`wrapEntry`) |
| `fpclassify` says a subnormal is 0, or `==` says it equals 0, in a test | under FZ (or DAZ), the comparisons C++ compiles them to flush their operand too | classify and compare outside the flushing scope |
| A C++ reference computed under FZ comes out IEEE | the compiler moved the arithmetic across the `msr`, which orders only memory | compute in a `noinline` function from `volatile` inputs into a `volatile` result |
| `1e-38f` is flushed in a test meant to stay normal | 1e-38 is below 2^-126 (1.18e-38): a subnormal | use 1e-37 and up for a normal product |
| `fminus_rt_parallel_stop` waits for ever on a pool thread asleep in `std::atomic::wait`, now and then, under load | the system ran the thread for the first time after the stop had bumped `signal`, and the thread took the bumped value for where it started | hand a thread `signal` as it was when it was made, and look at `stopping` before sleeping (`runtime/Parallel.cpp`'s `work`) |
| A test that a region ran on two threads or more fails now and then, on a busy machine | the pool's threads may not run before the caller has taken every chunk itself | a few tries, and check that one of them ran on two (`ParallelTest.cpp`) |
| A capture's binding goes stale, or a region captures a variable twice | a reference into `regions_` kept across the body's lowering, whose own regions push onto it | take what a region needs (its function) before lowering its body; `endRegion` takes the region off the stack |
| "JIT session error: Unexpected definitions in module ...: [ _fminus_rt_loop_budget ]", then "Failed to materialize symbols", for an external symbol a pass in the pipeline defines | LLJIT takes a module's symbols when it is added, before its IR transform layer optimises it, and refuses an object that defines more | define it in the code generator, as `ir::lower` defines the loop budget's global (recipe 29); an internal function a pass adds (`fminus.large.sin`) is no symbol of the module's |
| A vectorised loop runs slower after a branch that is never taken is added to its branch back | the loop is now two machine blocks, which the code generator schedules and allocates as it would any code, not as a loop of one block | keep the loop one block: put the work before it (recipe 29 charges a counted loop in its preheader) |
| An executable built with `--loop-budget` dies of SIGTRAP (status 133) when its `main` runs out, from `-O1` up | the C main called `main` directly; FunctionAttrs marked a `main` whose loop never ends `noreturn` before the budget's pass, and SimplifyCFG put `unreachable` in place of the flag's check after the call | call `main` through its address passed through an empty inline asm, so that no attribute of it reaches the caller (`addEntryPoint`, recipe 29) |
| A parallel region whose chunks spin takes many budgets before it stops | each chunk reads `left` where it starts; one that read it before another ran out stored its positive count over the -1, and the chunks after it started afresh | write the count back with `atomicrmw min` in every function a region's threads run, so `left` never goes up during a call (recipe 29) |
| A budget that ran out stops nothing after a few calls the host did not set it for | a stop wrote its own count, and charges near 2^62 took `left` past `INT64_MIN`, where the next subtraction wrapped it positive | a stop writes -1, `setLoopBudget` makes a negative budget -1, and no count that may exceed `INT64_MAX` is charged (recipe 29) |
| A branch on `(a \| b)` comes out as two branches | SelectionDAG splits an `and` or `or` of conditions into branches where jumps are cheap (AArch64) | mark the branch `!unpredictable`, which keeps one (recipe 29 tried it), or give the `or` a second use |
| A test of a loop that both the loop budget and a bounds check stop expects the budget to count its iterations, and it counts none from `-O1` | a failed check is a trap to the optimiser, which may bound the loop by it: in `while (s < n) { s += 1.0f + a[i & 7]; i = i + 1; }` over a `float a[4]`, `i` cannot reach 4, so LLVM unrolls the loop whole | index by what the optimiser cannot bound (recipe 30's test indexes by `(int)s`) |
| A bounds check's loop stores a field every iteration, a third slower, where without the check it stores it once after the loop | sinking the store to the loop's exits would add one on the failed check's path, where the failed iteration had stored nothing, which LLVM may not do to memory another thread could see | none needed for a development build; recipe 30's Measurements has the cost |
| A constant index outside its array makes the statements after it unreachable, and a non-void function's missing `return` is no longer an error | an unconditional branch to the check's failure leaves the code after it with no predecessor, and `reachable()` follows the CFG | keep it a conditional branch, on `false`, as for any check |
| A bounds check in a constant of the top level's initialiser crashes the code generator, or leaves a declaration that is then deleted | the initialiser is lowered into a scratch function that is erased afterwards, with every declaration only it used (recipe 28) | no checks at the top level (`atTopLevel()`), where nothing runs |
| With bounds checks on, `(voices + n)->bump()` at `n` one past the end writes the field after the array of modules, and nothing is recorded, while `(voices + n)->level` is caught | the pointer formed may be one past the end (width 0), and the method call's arrow branch (`lowerMethodCall`) used the pointer without the element check that `lvalue`'s arrow branch makes | check every access through a pointer that carries an extent as an element (width 1) where it is lowered: a field, a method's call, a dereference |
| A host re-initialises an instance after a failed bounds check, and it plays half set up, with no violation reported | the host called `init` before clearing the record: each call of a function that may stop then unwinds its caller, so `init` returned after its first one, and the first failure is kept, so nothing new was recorded | clear the record first (`clearBoundsViolation()`), then re-initialise |
| An `#if` reads a macro the file defines as 0, and its group is skipped | the `#define` comes after the `#if`: a macro is defined from its line on, and a name that is not one is 0 in `#if`, as in C | define it above its first use; a default the host may override as `#ifndef NAME`, `#define NAME value`, `#endif` at the top |
| C23's `#elifdef` in a skipped group would end it in C, and FMinus reads on past it to the `#endif`, skipping the group C reads | in a skipped group a line is text unless it is a conditional FMinus knows, as C ignores a skipped group's other lines: a conditional directive left out is a silent difference there, not an error | have every conditional C has (`#elifdef` and `#elifndef` too), or refuse the missing ones in skipped groups as well |
| A skipped group's `/* note */ #else` is text to FMinus and an `#else` to C, so each compiles another group, and neither says so | the directive rule's `^` cannot match after a comment, and `SKIPPED`'s text rule took the `#` | track whether only spaces and comments come before the scanner on its line (`lineStart`), and refuse a `#` there, in both start conditions |
| A `/*` left open in a `#pragma`, or in a skipped group's directive, is swallowed, and C reads the lines after it as the comment | only the directives FMinus evaluated made their comments spaces, and with them the check | check every directive's line first, in `directive()`, quotes aside |
| `#if` on nested macros of nothing (`#define C0`, then a thousand `C0`s in `C1`, and so on) hangs the compiler, though no bound is reached | the bound counted the tokens expanded, and an empty value adds none; a line under a bound of its own can also be repeated | count the work, a macro and its value's bytes, across the compilation (`ScannerState::expanded`) |
| The site's table of scanner rules says a rule "enters yyextra" | `site/scripts/lib/flex.js` took the first word in `BEGIN(...)` for the start condition, and `BEGIN(yyextra->skipping() ? SKIPPED : INITIAL)` starts with an expression | flex.js reads the start conditions a `BEGIN` names, those Lexer.l declares and `INITIAL` |
| The site's table of scanner rules shows `error:` with no message, or a rule that skips its input as an error | the action throws a constant (`kCommentBeforeDirective`), which has no string literal in the action, and its throw is under an `if` | flex.js reads the file's `constexpr const char*` constants, lists each throw's message, and shows a rule whose every throw is under an `if` as skipped, or that error |
| `FileCheck --check-prefix=PICK` fails on a line of another prefix | a variable `[[PICK:[0-9]+]]` on an `O0:` line contains `PICK:`, which FileCheck reads as a check of that prefix | name variables apart from every prefix (`S_PICK`) |
| `CHECK-NOT: bounds` fails on IR with no bounds checks | `getelementptr inbounds` | match a name of the feature's own (`fminus_rt_bounds`, `fminus.bounds`) |
| A library test's `CHECK(view == "line")` does not link: undefined `Catch::StringMaker<std::string_view>` | the library tests are C++17 against a Catch2 built as the build tree's C++23, which compiled no `string_view` support into it | compare `std::string(view)` |
| "'BranchInst' is deprecated: Use UncondBrInst/CondBrInst/Instruction instead" | LLVM 23 split the branch instruction | `llvm::UncondBrInst::Create`, or an `IRBuilder` |
| A new check of an operand's type adds a second error after an undeclared name | the name's stand-in is a poison float, which the check refuses | test `failed` first (`Lowering::Value`), and give a poison of the type the result would have had |
| `-Wswitch` warns on `switch (mode + 0)`, or on a `?:` of two enumerated values, where clang does not | a result type copied from an operand keeps `Type::enumeration`, which `==` ignores | give a promoted result a fresh type (`arithmeticOf` does), or clear `enumeration` |
| A test passes under `fminus-tests` and fails under ctest with "Unrecognised token" | ctest runs each Catch2 test case by its name, and Catch2 reads a name that starts with `-` (`-Wswitch: ...`) as an option | start the name with a word (`clang's -Wswitch: ...`) |
| The names after an enumeration's list cannot use its tag (`enum E { A } e = (enum E)0;` is a forward reference) | the rule that declares the tag reduces after the declarators | reduce the enumeration at its closing brace (`enum_head`), and the declarators after it |
| "invalid application of 'sizeof' to an incomplete type 'fminus::ast::Expr'" in `unique_ptr`, from every file that includes `Ast.h` | a node declared before `Expr` is complete (`SizeofExpr`) gave its `ExprPtr` a default member initializer, `= nullptr`, which instantiates the deleter there | leave those without one (an `ExprPtr` starts null anyway); a node after `Expr` (`DeclStmt`, `Field`) may have it |
| `-Werror,-Wmissing-field-initializers` in `Parser.tab.cpp` after a node gains a field | an action's aggregate initialiser (`ast::SizeofExpr{type}`) leaves the new one out | name every field in the action (`ast::SizeofExpr{type, nullptr, nullptr}`) |
| A test's runaway loop returns at once, and the loop budget is never exceeded | a loop with no effect the optimiser keeps (a store LICM sinks, a counter nothing reads) may be assumed to end (`llvm.loop.mustprogress`, recipe 19), and is deleted | give the loop a value the function returns, which it cannot compute without running it |
| A client's request answered just before the program closes its server (a route's `reply`, then `defer http_close(server)`; or the host's `stopWeb()` or `~Program` as soon as its loader returns) gets an empty response: curl says "Empty reply from server" | closing detached the server from the I/O thread, which closed every connection at once, one whose reply was published and not yet written among them (the I/O thread writes a reply on its next turn, up to a millisecond later) | closing drains: a first command stops listening, gives up what the program has not answered and writes every published reply into its socket as its connection's last response, and the closing thread waits for them to leave, a quarter of a second at most, before a second command closes what is left (`Servers::drain`, `Web.cpp`'s `beginClosingServer` and `endClosingServer`) |
| A test that destroys a context while threads wait on its requests fails now and then, under load (`cancelled.load() == 8` was `3 == 8`) | it counted a thread before the thread called `fminus_web_http_wait`, slept 10 ms, and destroyed the context: a thread the system had not run yet began its wait on a request already freed | wait for the library's own count of the threads in its waits (`fminus::web::waitingThreads`, counted under the context's mutex), then destroy; a server's takers and requests likewise (`fminus::web::serverCounts`) |

## Where to go next

Design notes for growing the C subset beyond the recipes: what each feature involves and which LLVM pieces it uses.
This code is not written or verified. `examples/biquad/README.md` suggests an order driven by a real kernel.

**A type checker, and the rest of C's arithmetic types.** Recipe 3 converts with LLVM's types. Recipe 7 takes the first
step away from that: every lowered expression carries its FMinus type (simd.md, "Every value carries its FMinus type"),
and recipe 10 adds `int` and pointers to it. That still computes types while lowering, which stops scaling once types
multiply, as C's do quickly: `unsigned`, `long`, the narrower integers. Recipe 33's `double` took the first piece of it,
`staticType`, an expression's type from the declarations alone, which its constants need before their arithmetic is
lowered. Give the tree a semantic pass before lowering:

- resolve every name to its declaration;
- compute every expression's type with C's usual arithmetic conversions and integer promotions;
- record each one beside the node (a side table keyed by node address, or a `type` field on `Expr`);
- insert the implicit conversions as explicit nodes;
- report the type errors C has: "invalid operands to binary expression", incompatible types in an assignment.

Lowering then just reads the types. Each integer type lowers to an `iN`, and signedness lives in the operations,
not the types: `udiv` and `urem` beside recipe 10's `sdiv` and `srem`, unsigned `icmp`s, `zext` where `int` uses
`sext`, `uitofp` and `fptoui`, and `lshr` where recipe 25's `>>` is an `ashr`. An `unsigned` has no `nsw`: it wraps, by
definition, which is what a hash or a generator written in FMinus needs (recipe 25 made `hash` a builtin for want of
it).

**More arrays, and more pointers.** Recipe 34's arrays are two dimensions deep, a row described beside its element's
kind (`rowKind`, `rowCount`). A third dimension, pointers to pointers and arrays of pointers need `Type` to nest (its
element a type, not a kind), and so would a parameter of a 2-D array, a pointer to rows: its row's size is an integer
constant expression, so the function's types would be worked out where the function is, not in the first pass, or the
pass would wait for the file's enumerations. `bool*` and arrays of `bool` need `i8` in
memory (recipe 10 says why), and null C's `0` as a pointer constant. Casts between pointer types (`(float4*)samples`, a
float buffer seen as frames; `free((void*)table)`, a `const` pointer freed) are C's and change nothing in the IR, but a
`float4*` cast from a `float*` must still meet a `float4`'s alignment. Pointers to pointers are a `ptr` to a `ptr`, once
a type can nest (`Type` would hold its pointee as a type, not a kind).

**`sizeof` of an expression.** C's `sizeof x` doesn't evaluate `x`. FMinus learns an expression's type only by
lowering it, so it would need a type checker first (above), or a lowering into a block it then throws away.

**The host's allocator.** `fminus_rt_malloc` and `fminus_rt_free` are where a plugin would plug in its own memory:
an arena it hands each instance, freed all at once, or memory it has locked so the audio thread never faults on it. A
runtime function that sets the allocator, or one arena per JIT, would do it. It must keep what recipe 19 tells LLVM of
them true (zeroed blocks, aligned to 64, one allocator family), or change `kRuntimeFacts` with it.

**A run-time check.** Recipe 11's check is static. Clang's RealtimeSanitizer (`-fsanitize=realtime`) catches the rest
at run time: a function marked `nonblocking` that, while it runs, allocates or takes a lock anywhere, in a library
too. FMinus could mark its kernels' LLVM functions as clang does for RTSan's instrumentation.

**More of modules.** Recipe 12's modules leave out what needs code to run where a module is made, or a choice at
run time. *Modules by value*: at the C boundary, AArch64 passes and returns a struct of up to four floats in registers
`s0`–`s3` (a homogeneous floating-point aggregate), and a larger one through memory (`byval`, and `sret` for a
return); clang's IR for the equivalent C++ declaration shows what to emit, and inside FMinus, where calls get inlined,
any representation works. *Const methods*: a `const` after the parameters, and `this` a `const OnePole*` in them, so a
method can run on a read-only module. *Forward declarations* (`module Later;`) would let a module point to one declared
after it, and methods defined outside their module (`float OnePole::tick(float x) { ... }`) would let a module's
declaration be its interface. *Interfaces* (a table of function pointers per module, a vtable) and *generics* (a
module of a type parameter, `Delay<float4>`) are the large ones; for DSP, generics over the vector width are the more
useful, and `poly float` already covers much of it.

**What a call leaves.** Recipe 17's tags tell a `float` store from a field's, but say nothing about calls: a method
that calls another (not inlined) loads its fields again after the call, as `Convolver::process` does every sample. A
summary of the fields each method writes would keep them in registers; FMinus could compute one, since a pointer
field is written only by its name (it has no pointer to a pointer). Struct-path tags (clang's `%struct.S` nodes with a
field offset) would also tell two `float` fields apart where BasicAA can't, and so far no measured loop needed them.

**More of parameters.** Recipe 14's parameters are `float`s, `int`s and `bool`s, and only a `float` streams. *A
smoothed knob*: a binding that ramps from the value the last call read to this call's across the block, computed at
the method's start, would give most of a stream's smoothness at a knob's price. *Events*: a gate or a note, an `int`
or a `bool` that changes at a sample inside a block, is a list of (sample, value) pairs, not a buffer. *Other
instances*: a voice reading its filter's cutoff (`filter.cutoff[i]`) needs the read's own load, an atomic one for a
knob, where the method's own are at its start. *A table of them* (`Program::parameters()`, every module's, with a
stable id each) is what a plugin's parameter list is built from; `Module::parameters` already has what it needs.

**More of parallel regions.** Recipe 22 leaves four things. *Reductions*: a sum over the iterations,
`parallel for (...) reduce(+: total)` in OpenMP's terms, would give each thread its own `total`, combined in a fixed
order at the end so that the result does not depend on the threads; today an iteration writes its own element and the
code after the loop adds them. *Real-time threads*: on macOS a pool thread could join the audio workgroup of the
device's thread (`os_workgroup_join`, given by the host), which tells the scheduler that its work has the audio
thread's deadline; a plugin host would pass its workgroup in through the API. *The checks* see variables, not
elements: an index that is the loop variable (`out[i]`) is each iteration's own, and one that is a constant (`out[0]`)
is every iteration's, which a pass over the indices could tell apart. *Knob versioning inside a region*: the region's
body reads a versioned method's constant through the context, as a value; versioning the body on its knobs, as the
method is, would fold them there too.

**More of knob versioning.** Recipe 16 leaves four things. *A cold fallback*: an int's original body runs only for
values outside its range, yet it is optimised, unrolled and vectorised like the copies, and in the shaper it is the
largest; `llvm.loop.vectorize.enable` false and `llvm.loop.unroll.disable` on its loops would shrink it. *Methods
without a loop of their own*: a voice's `tick()`, inlined into its synth's sample loop, is not versioned, since the
pass runs before the inliner; its knobs' loads leave the loop, but their branches stay, and a stream read there,
though the loop vectorises on its stride, runs LLVM's scalar loop when it is flat. Versioning the caller after
inlining, on the tags its callees brought, would take the branches out, and give a flat stream its copy. *Guarded
freezing*: the host compiles a copy with a knob frozen, behind a test of its value; the dispatch here is that test,
made at compile time for every value. And *a budget per program*, not per method, with a domain for an `enum`.

**More of the math modes.** Recipe 15's mode is the program's. *A mode a function*, as an attribute
(`[[fminus::math(libm)]]`), would let a kernel choose libm for its scalar recursion and the vector functions for its
vector code; each function would still give a float one result, but one `sinf` would give two in one program (and
since recipe 24 the vector mode's scalar kernels cost about what libm's do). *More kernels of FMinus's own* (recipes 23
and 24): `powf`, `atanf`, `exp2f`, `log2f`, `sinhf`, each one sequence of IEEE operations, as builtins with their
three names and a stem the pass knows. *A more accurate sine and cosine*: carrying the reduced argument's rounding
error in a second float would take them from 1.56 ulp to under 1, at about twice the reduction's work in a vector.
*Less compile time*: the kernels inline cost 39% more instructions to compile (recipe 24), most of it the backend's
work on the kernels in every copy a loop is cut into (a vectorised body, its epilogue and remainder) and in every
knob-versioned copy. Inlining only the kernels in loops, calling the runtime elsewhere, saved 9% of the compile in an
experiment; a remainder's or an epilogue's kernels could call the runtime instead, at the cost of a call a sample
there.

**More of zero-init elision.** Recipe 18 removes a whole memset or nothing. *Shortening* it to `[end, size)`, with the
same proof and `MemSetInst::setDest` and `setLength`, would keep only the bytes that can be read, and cover `t[n]`
past a loop. *Writers in more shapes*: several stores an iteration whose offsets tile the step, two loops writing
consecutive ranges, a nest writing a 2-D block. *A front-end analysis* (definite assignment over the syntax tree)
would see a `for` with a constant bound before LLVM unrolls it, at `-O1` as at `-O3`. And *an opt-out*,
`float tmp[256] [[fminus::uninitialized]];` as clang's `[[clang::uninitialized]]`, for the shapes no proof will reach.

**State hygiene.** Recipe 20's flush-to-zero removes the subnormals' slowdown, not their residue: a one-pole gone silent
sticks at a normal number above 0. An attribute on a field (`[[fminus::flush]]`) could zero a tiny value or reset a
non-finite one once a call, where LICM writes the field back after its loop. A `Program` query of which functions set
the mode themselves would tell a host which ones it must call with the mode set.

**More of the FFT.** Recipe 13's transforms give their bins in the usual order. For convolution, PFFFT's own order is
about 10% faster again: `pffft_transform` without its reorder, and `pffft_zconvolve_accumulate` to multiply two
spectra in that order, behind builtins whose spectra a program never indexes. `fft_size(n)`, on
`pffft_nearest_transform_size`, would give the size to pad a block to. The sizes PFFFT can't take could go to a
mixed-radix or Bluestein transform in the runtime, or to FFTW, which takes any (build it with
`--enable-armv8-cntvct-el0` on macOS arm64: without a timer, its measured plans come out slower than its estimated
ones). On Apple systems vDSP is 2–3 times faster than either from about 512 points, but it comes from Accelerate, a
system framework: a backend chosen when FMinus is built, not a library inside it.

**State: globals and `static`.** File-scope variables would lower to LLVM globals, and `static` locals likewise (recipes
26 and 28 are the constant cases already: a `const` local array of constants is a private constant global, and a
top-level constant a private constant global of its name). One global for every instance of a plugin is shared state,
written from every instance's audio thread. For a plugin, prefer state each instance owns: a module (recipe 12), in
memory the host gives it, as `examples/biquad/`'s filter and `examples/voices/`'s synth are, or memory the program
allocates off the audio thread, as `examples/delay/` does (recipe 11). Nothing then allocates on the audio thread, and
every instance has its own copy.

**More of `switch` and enumerations.** Recipe 32 built `-Wswitch`, names declared with an enumeration and C23's fixed
underlying type; four things remain. *clang's other `-Wswitch`*, "case value not in enumerated type 'enum Mode'", a
case for a value none of the enumeration's constants has: `unhandled` has both lists, and the message names the type
as clang does, `'enum Mode'` (an enumeration without a tag is clang's `'enum (unnamed at file:line:column)'`), which
needs the tag kept by id beside the constants. *Warnings a host controls*: a `-Wno-switch`, or `-Werror`, as
`Program::Options` and `fminus` flags. *An enumeration's values as a knob's domain* (recipe 16's list, for that
reason): a `param enum Mode` could be versioned on its constants, which its `Type::enumeration` now names, with no
`fminus::range`. *A `case` range*, GCC's `case 1 ... 4:`, which clang lowers to its cases one by one when the range is
small, and to a comparison when not.

**Macro substitution, if it is ever wanted.** Recipe 31's macros are conditions only. Substituting an object-like
macro in code would happen in the scanner: `{IDENTIFIER}` finds the macro, lexes its value into a queue of tokens
placed at the use (as `evaluate` places a value's tokens at the macro's name), and `nextToken` drains the queue before
it reads on, the macro's name hidden while its own tokens are read (C's rule, which `expand` keeps with `expanding`).
The queue bypasses Flex's rules, so a keyword or a module's name in a value needs the rules' look-ups done by hand.
Function-like macros add arguments collected across lines and C's `#` and `##`. Either gives up what recipe 31 keeps,
that the code a reader sees is the code the parser reads; FMinus's `const`s, enumerations and parameters cover most of
what C's object-like macros are for.

**Contraction and fast-math, C's way.** `#pragma STDC FP_CONTRACT ON` (C99) allows fusing `a * b + c`. Lower it to
`llvm.fmuladd`, or set the `contract` flag through `IRBuilder::setFastMathFlags`. The other fast-math flags
(`reassoc` for vector reductions, `afn`, `nnan`, `ninf`) can follow a clang-style `#pragma clang fp` or a function
attribute. Scope them with `FastMathFlagGuard`.

**Better syntax errors.** Bison recovers through the `error` token: a rule like `statement: error ";"` resynchronises
at the next `;`, so one run reports several syntax errors. `tests/smoke/calc/Calc.y` does exactly this. With
`%define parse.error custom`, you write the message yourself, for example clang's "expected ';' after expression".

**Debug information.** `llvm::DIBuilder` attaches source locations to instructions, so lldb steps through FMinus
source in an AOT executable. Every node already carries a `SourceLocation`. For JIT'd code, ORC can register debug
objects with the debugger (`llvm::orc::enableDebuggerSupport`).

**Compile time.** The JIT compiles on the thread that looks a symbol up, and a plugin needs that to be fast and
predictable. Measure the time from `parse` to `lookup` for real programs. The levers are the optimisation level,
LLJIT's concurrent compilation, and caching objects between runs (`llvm::ObjectCache`). Knob versioning (recipe 16)
is one more: a method it copies takes about twice as long to compile, and `knobVersions` caps it.

## Reference solutions

The recipes in the compiler are their own reference: their code is in `src/`, and their tests in `tests/unit/`
(`FrontendTest.cpp`, and `CodeGenTest.cpp`, `ControlFlowTest.cpp`, `SwitchTest.cpp`, `EnumTest.cpp`, `IntTest.cpp`,
`BitwiseTest.cpp`, `PointerTest.cpp`, `HeapTest.cpp`, `EffectTest.cpp`, `ArrayTest.cpp`, `TableTest.cpp`,
`TopLevelTest.cpp`, `ModuleTest.cpp`, `FftTest.cpp`, `HashTest.cpp`, `ParamTest.cpp`, `VectorTest.cpp` and
`PolyTest.cpp` for the code generator; `MathTest.cpp`, `VersionTest.cpp`, `TbaaTest.cpp`, `ZeroInitTest.cpp`,
`AttributeTest.cpp` and `DenormalTest.cpp` for recipes 15 to 20; `RecoveryTest.cpp` for recipe 32;
`FieldAttributeTest.cpp` for recipe 35), `tests/library/` (`ProgramTest.cpp`, `StrictAliasingTest.cpp`,
`DenormalsTest.cpp`, `FieldAttributeTest.cpp`) and `tests/lit/fminus/`. [recipes/](recipes/) holds
the two that aren't, 5 and 6, as one patch against the compiler as it is ([recipes/README.md](recipes/README.md)):

```bash
git switch -c try-recipes
git apply docs/recipes/5-runtime-function-and-pass.patch
cmake --workflow --preset dev
```

Once your own compiler has moved on, the patch stops applying, but it remains a readable diff of one way to make the
change.
