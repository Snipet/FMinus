# Extending the compiler

How to add to FMinus. First, how a change moves through the compiler, and the few rules that keep that easy. Then
nineteen worked recipes. Eleven grow the C subset from a one-line builtin to C's control flow, a custom LLVM pass,
`int`, `void` and pointers, the heap with the attributes that keep it off the audio thread, arrays and modules, FMinus's
classes, an FFT from a C library built into the runtime, and parameters, the values a plugin's host sets. Six more are
about the compiler rather than the language: math that gives a float one result wherever it is computed, knob versioning
and flat streams, C's strict aliasing told to LLVM, zero-init elision, what the runtime's calls and loops promise, and
subnormals flushed for the audio thread. The last two grow the language again: `#include`, which splits a program into
files, and `parallel for` and `parallel { }`, a program's own threads. Then the pitfalls those recipes ran into, and
design notes for what comes next. [simd.md](simd.md) has recipes 7 to 9, for vectors.

Recipes 1 to 4 are in the compiler now (`tanhf` and `%`; `bool` and its operators; `if`, `while` and `for`), and so are
simd.md's 7 to 9, recipe 10 (`int`, `void` and pointers), recipe 11 (the heap), recipe 12 (arrays and modules), recipe
13 (the FFT), recipe 14 (parameters), recipes 15 to 20 (the math modes, knob versioning, TBAA, zero-init elision, the
runtime's attributes and loop progress, denormal control), recipe 21 (`#include`) and recipe 22 (`parallel for` and
`parallel { }`), so their sections describe how the code you have got there: read a section beside the files it names.
Recipes 5 and 6, a runtime function and a pass, are not part of the language; they live in [recipes/](recipes/) as a
patch, so you can apply them to see them work. None of these are sketches: each was applied to this repository, built
with `-Werror`, and tested in the dev and asan presets, every test passing. [architecture.md](architecture.md) describes
the stages they touch.

FMinus follows C. When a recipe has a semantic choice to make, it makes C's: conversions, short-circuiting, which
`if` an `else` belongs to, what `continue` does in a `for`. And it reports errors in clang's words. The README lists
the few deliberate deviations.

## How a change moves through the compiler

Most language features touch the same places, in this order:

| # | Where | What to add |
|---|---|---|
| 1 | `src/frontend/Lexer.l` | a rule for each new token. Keywords go before `{IDENTIFIER}`. |
| 2 | `src/frontend/Parser.y` | the tokens (`%token`), their precedence if they are operators, and the grammar rules with their actions |
| 3 | `src/frontend/Ast.h` | a node: an alternative of `Expr::node` or `Stmt::node`, or an enumerator of `UnaryOp`, `BinaryOp`, `AssignOp`, `IncrementOp`, `Kind` (a type's) |
| 4 | `src/frontend/Ast.cpp` | its `dump` |
| 5 | `src/ir/` | its conversions and checks, and its lowering to LLVM IR, in the file its kind belongs to (`Lowering.h` lists them: types, functions, statements, expressions, calls, the math modes, vectors, memory, modules, effects, parallel regions, TBAA, denormals); a load or store of memory gets its type's tag (`tag`, recipe 17); a statement that holds code walks it in `Parallel.cpp`'s `Walker` too |
| 6 | `src/runtime/` | if it needs C++ at run time, a runtime function (recipe 5); if a builtin calls it, its facts: in `kRuntimeFacts`, through `callRuntime` (recipe 19), or for a math builtin's, in `mathFunction` (`Math.cpp`, recipe 15) |
| 7 | `tools/fminus/main.cpp`, `src/support/Aot.cpp` | for a new type: whether `--run` can pass and print it (its `switch` over the result type), and whether an executable's `main` may take or return it (`addEntryPoint`) |
| 8 | `tests/` | a dump in `FrontendTest.cpp`; results in `CodeGenTest.cpp`, `ControlFlowTest.cpp`, `IntTest.cpp`, `PointerTest.cpp`, `HeapTest.cpp`, `EffectTest.cpp`, `ArrayTest.cpp`, `ModuleTest.cpp`, `FftTest.cpp`, `VectorTest.cpp`, `PolyTest.cpp` or `ParallelTest.cpp` (their helpers are in `Compile.h`); IR or errors in `tests/lit/fminus/` |
| 9 | `examples/` | a program that uses it, and a line in `tests/lit/fminus/examples.test` that runs it |
| 10 | `editors/vscode/syntaxes/fminus.tmLanguage.json` | a new keyword, type or builtin, so VSCode highlights it (its README says where) |
| 11 | `README.md` | the language table, and the deviations from C if any |

Steps 3 to 5 need no searching. Once the node exists, the build fails at every walk that doesn't handle it yet: a
`std::visit` without a matching lambda fails to compile, and a `switch` without the new enumerator trips `-Wswitch`,
an error under `-Werror`. Fix the errors in order and the feature is wired through. `std::visit` names only the
first alternative it can't handle, so after you fix one, the next appears. The same goes for step 7's `switch`: when
recipe 7 added `Float2` to `Bool8`, the build stopped in the driver until `--run` said what to do with a vector, and
when recipe 10 added `Kind::Pointer`, it stopped in `typeOf`, `typeName` and the driver. Steps 9 and 10 are the ones
nothing reminds you of.

Each feature has an example to read beside its recipe: `examples/comparison/` for `bool` (recipe 3),
`examples/waveforms/` for every statement (recipe 4), `examples/chord/` for `float4` and the vector math library
(recipes 7 and 8), `examples/unison/` for `poly float` (recipe 9), `examples/biquad/` for a block of samples a
call, through pointers (recipe 10), `examples/delay/` for state on the heap and a kernel that can't allocate
(recipe 11), `examples/saturator/` and `examples/meter/` for `poly float` through a buffer, against plain loops
(recipes 9 and 10), `examples/convolver/` and `examples/delay/echo.fm` for loops over a module's fields
(recipe 17), and `examples/ensemble/` for `parallel for` and `parallel { }` (recipe 22).

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
  goes on and finds the next error. `lower` throws the module away at the end.
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
`fminus_rt_libm_exp2f`), as recipe 15 gives the others. `powf`, of two arguments, also needs `lowerMath` and
`mathFunction` to take two.

Update the count in `CodeGenTest.cpp` (`builtins().size()`), then test the value, in the builtins' test case:

```cpp
                       "float h(float x) { return tanhf(x); }\n");
    ...
    CHECK(lookup<float(float)>(*jit, "h")(0.5f) == fminus::math::tanh(0.5f));   // the default math mode (MathTest.cpp)
    CHECK(fminus::ir::builtins().size() == 10);
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
errors: "'break' statement not in loop or switch statement" and "'continue' statement not in loop statement".

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

`src/opt/` holds FMinus's own LLVM passes. Two are in the compiler, registered straight in `optimiseModule`: knob
versioning (recipe 16, at the early simplification extension point) and zero-init elision (recipe 18, at the scalar
optimiser's late one). The example here counts a function's floating-point operations after optimisation and records
the count as a string attribute, `"fminus-flops"="3"`. That's handy for watching what LLVM leaves of a kernel; the
biquad's coefficient math, for instance.

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

A declaration of any other is "FMinus has pointers to float, int, float2, float4 and float8, not to 'bool'". The
variable is then bound as a `float*`, so its uses report nothing more.

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
function pointer type only has to name the same parameters in the same order. `--run` passes floats only, and
refuses an `int` or pointer parameter ("takes a parameter of type 'int'"); it prints an `int` result.

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
C. The size is the parser's to check (a positive integer constant: `arraySize` in `Parser.y`), since the `Type` holds
it.

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
  parser's for sizes included.
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
    void* plan;

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
reads a constant.

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

`checkParameter` checks the rest: the type, the binding (an `int` stream is "parameter 'mode' is an int: only a
float parameter can be a stream"), and the attributes. `fminus::range(minimum, maximum)` takes two numbers in order,
`fminus::default(value)` one, inside the range if there is one, and any other attribute is unknown. A binding that
names no parameter of the program is an error too, at 1:1, as an option that names nothing would be. The range and the
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

The premise was measured. libsystem_m's `_simd_exp_f4`, `_simd_tanh_f4` and `_simd_log_f4` are lane-independent,
checked exhaustively on an M5: every float, in every lane, beside awkward neighbours (NaN, infinities, huge and
subnormal values). `_simd_sin_f4` and `_simd_cos_f4` are not. A vector takes a fast path when every lane is finite and
at most 1e6 in magnitude, and a slow path for all four lanes otherwise. The two paths disagree on about 37% of
arguments, by up to 3 ulp, so one large lane changes its three neighbours' results; each path alone is
lane-independent. So the runtime guards sin and cos: a vector goes to libsystem_m whole only when every lane is within
`kSinCosLimit`, 2^16, and lane by lane otherwise. The limit leaves Apple's edge room to come down by a factor of 15 in
a later macOS, and costs audio nothing: a sine at 65536 radians has already lost its phase to float's spacing there
(2^-7 rad). `MathTest.cpp` checks the premise again on every run, so a macOS that changes it fails a test rather than
a program's last bits.

### The runtime: `src/runtime/Math.cpp`

Fifteen functions, in `Runtime.h` and `RuntimeSymbols.def`: the vector mode's `fminus_rt_sinf` to `fminus_rt_logf`,
their four lanes, `fminus_rt_sin_f4` to `fminus_rt_log_f4`, and the libm mode's `fminus_rt_libm_sinf` to
`fminus_rt_libm_logf`. A four-lane vector is `fminus_rt_float4`, GCC's and clang's `vector_size(16)` float vector,
which the C ABI passes in one register as it does FMinus's `<4 x float>`. On Apple systems:

```cpp
extern "C" float fminus_rt_sinf(float x) noexcept
{
    return _simd_sin_f4(splat(x))[0];
}
...
extern "C" fminus_rt_float4 fminus_rt_sin_f4(fminus_rt_float4 x) noexcept
{
    return withinLimit(x) ? _simd_sin_f4(x) : eachLane(fminus_rt_sinf, x);
}
```

The scalar function is lane 0 of the four-lane one on either path, since four copies are within the limit together or
not at all. On arm64 `withinLimit` is `fabs`, NEON's `fmaxv` (which, unlike `fmaxnmv`, gives NaN when a lane is NaN),
a compare and a branch. `eachLane` is out of line, and libsystem_m's functions are declared `noexcept` (they are C), so
the fast path is a tail call with no stack frame: from a `noexcept` function, a call of one that might throw sits in a
`std::terminate` landing pad. The four-lane exp, tanh and log call libsystem_m's straight. They go through the runtime
all the same, so that the generated IR is the same on every target, and the functions `MathTest.cpp` checks are the
ones programs call. Elsewhere than Apple systems the vector mode's functions are libm's, a lane at a time.

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
  path, and its own lanes are taken back.

So no vector is scalarised to libm, and every lane of every width is the scalar function of that lane. In the libm
mode every lane is a call of `fminus_rt_libm_sinf`.

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
`fminus` and executables need no new argument.

| `Program::Options::math`, `fminus --math` | `sinf` ... `logf` are | One result for a float? | Constants folded? | Loops of them vectorise? | Error |
|---|---|---|---|---|---|
| `Vector`, `vector` (the default) | the runtime's: libsystem_m's vector functions on Apple systems, made lane-independent; libm's elsewhere | yes | no | yes, into four-lane calls | up to about 3 ulp (Apple) |
| `Libm`, `libm` | the runtime's wrappers of libm's, a lane at a time | yes | no | no | libm's (within 1 ulp) |
| `Fast`, `fast` | LLVM's intrinsics, as recipe 8 left them | no | yes, with the host's double functions | yes, into `_simd_*_f4` | either |

`ir::Options::math` (`ir::Math`) is the same choice. The fast mode is the compiler before this recipe: the same IR, the
same pipeline, and the module flag, but for the runtime's math names, which it refuses too (above).

### What changes for programs and hosts

- **Results move in their last bits.** A scalar `sinf` in the default mode was libm's (within 1 ulp of the exact
  result); it is now the vector function's lane (within about 3), the same as a vector call's. `waveforms.fm`'s
  `main 0.25 1000` is 0.48676, where libm gives 0.4867599 (`examples.test` accepts both).
- **C++ references** call `fminus::math::sin` ... `log` (`include/fminus/FMinus.h`), which compute what the default
  mode does, and agree bit for bit at any block size, level, width and binding. They are `[[gnu::const]]` for GCC and
  clang (`FMINUS_MATH_CONST`): without it an opaque function is called every time, and the biquad's reference, which
  recomputes its sine and cosine for each coefficient, went from 2.65 to 16.5 ns a sample.
- **A frozen parameter's `sinf`** is no longer folded. It is a call, which LICM takes out of the loop, once a method
  call, and the output is the knob's bit for bit (`examples/parameters/`'s `--check` now checks it).
- **Vectors of other widths are faster**: a `float8` `sinf` was eight `sinf` calls, and is two four-lane ones.
- **Scalar calls that can't vectorise are slower**: each computes four lanes of a vector function, whose latency is two
  to six times libm's (Measurements).
- **A prototype's libm function** (`atanf`, `powf`) gets the vector library only in the fast mode, and only where its
  calls write no memory: on Apple systems (recipe 19).
- **Elsewhere than Apple systems** the vector mode's results are libm's: the IR is the same, and the four-lane
  functions loop over libm. Neither Linux nor Windows has been run with it.
- **The runtime** has fifteen more functions, which a host that hides or strips symbols must keep
  (`RuntimeSymbols.def`, which `defineRuntimeSymbols` reads).
- **Their names are reserved.** A program that calls `sinf` and declares a function named `fminus_rt_sinf`,
  `fminus_rt_sin_f4` or `fminus_rt_libm_sinf` (or one of another builtin's three that it calls) is an error, in every
  mode: rename the function.

### Tests

- `tests/unit/MathTest.cpp`, the premise and the promise. Each four-lane function against its scalar one, lane by
  lane, over 10^6 vectors of mixed arguments: phases, audio's ranges, zeros of both signs, subnormals, infinities, NaN
  payloads, the floats either side of the limit and of 1e6, and any float at all. libsystem_m's own lane-independence,
  where the runtime relies on it (skipped elsewhere). Then, a test case a function, seven kernels (a loop that
  vectorises, a recursion that doesn't, a parameter's function, `float2`, `float4`, `float8` and `poly float` code),
  at `-O0` and `-O2`, at 1 to 64 lanes, the parameter a knob and frozen, in one call and in blocks of 61: every output
  `fminus::math`'s, bit for bit. The libm mode runs the same matrix against C++'s `std::`, and the fast mode is checked
  as it was. Raising the limit above Apple's edge, or compiling the promise in the fast mode, makes them fail. Then
  the reserved names: five programs that declare one (the four-lane name of an `int` and of a `float`, a prototype of
  it, the scalar name with a body that may loop forever, the libm name), each an error with its note in every mode,
  through the code generator and the public API; and a program's own prototypes of the three, called by name beside a
  loop of `cosf`, left alone.
- `tests/library/ProgramTest.cpp`: through the public API in C++17, a block of all five against `fminus::math` in the
  default mode and against `std::` in the libm mode, at `-O0` and `-O2`.
- `tests/lit/fminus/math.fm`: each mode's IR (the vectorised loop and its remainder, `float2`, `float4`, `float8` and
  16- and 1-lane poly calls, a constant not folded, a frozen parameter's call out of the loop, the attributes and the
  flag, no `_simd_` outside the fast mode), the assembly (two calls where the fast mode has one `__sincosf_stret`),
  `--run` at `-O0` and `-O2`, and `--math`'s error.
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
- **That Apple's libm sets no errno.** A prototype, `float tanf(float x);`, calls libm's function, which LLVM takes to
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

A prototype with no definition calls the process's C function of its name (README.md): `float tanf(float x);` calls
libm's. LLVM knows libm's functions by name and type (`TargetLibraryInfo`), and InferFunctionAttrs, in the pipeline,
gives their declarations what C promises of them: `nounwind willreturn nofree nosync`, and `memory(errnomem: write)`,
since C lets a math function report a domain or range error in `errno`. A call that writes memory stays where it is:
LICM leaves it in its loop, so does each of knob versioning's copies, and the loop vectoriser gives up on the loop. So
in a method whose cutoff is a still dial, `tanf(pi * cutoff[i] / rate)` ran every sample, where the builtins'
`sinf(w) / cosf(w)` ran once a call.

Apple's libm writes no `errno`: its `math_errhandling` is `MATH_ERREXCEPT` (2), and none of thirty calls at their domain
and range errors (`logf(0)`, `expf(1000)`, `powf(0, -1)`, `acosf(2)`, `sqrtf(-1)`, `fmodf(1, 0)`, ...) sets it, even
compiled with `-fmath-errno`. That is why clang defaults to `-fno-math-errno` on Apple systems, and declares
`<math.h>`'s functions `memory(none)` there. `describeLibm` (`Functions.cpp`) does the same for FMinus, after the
definitions. For each prototype that no definition followed, whose name and type are a math function's that
`TargetLibraryInfo` knows and the target has, it runs LLVM's own inference (`inferNonMandatoryLibFuncAttrs`) and takes
`errno` out of what the function may write:

| Prototype | LLVM's attributes | On Darwin |
|---|---|---|
| `tanf`, `powf`, `atan2f`, `exp2f`, `log10f`, `ldexpf`, ... | `memory(errnomem: write)` | `memory(none)` |
| `remquof`, which writes its quotient | `memory(argmem: write, errnomem: write)` | `memory(argmem: write)` |
| `roundf`, `ceilf`, `truncf`, ..., which set no `errno` anywhere | `memory(none)` | the same |
| a name libm doesn't have, a type that isn't libm's (`float atanf(int x)`), a definition | none | none |

Which libm is in the triple (`ir::Options::targetTriple`, the process's when empty). Linux's glibc and Windows' CRT do
set `errno`: there the declarations stay as the program wrote them, and LLVM infers what C allows. Other C functions
that write `errno`, the allocators, also reach memory of their own (`inaccessiblemem`), and keep it: only a function
whose memory, without `errno`, is at most its arguments' loses `errno`.

What LLVM then does, which is what clang does with `<math.h>` on Apple systems:

- **Calls move.** A call on a loop's invariants is made once, before the loop, in every copy knob versioning makes.
  A loop of calls on its samples vectorises: in the vector and libm math modes into `llvm.tan.v2f32` and its kin, which
  the backend makes into one `tanf` call a lane, the same function, so every result is still libm's; in the fast mode
  into libsystem_m's `_simd_tan_f4` (recipe 8), with that mode's caveat that a block's vectorised body and its
  remainder may differ in their last bits (recipe 15: "a prototype's libm function gets the vector library only in
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
  write)`) at `-O0`, a prototype of another type, an unknown name and a definition bare, and at `-O2` a loop's `tanf`
  and `powf` of its invariants before the loop, which vectorises; elsewhere, the declarations bare and the calls in
  the loop.
- `tests/lit/fminus/progress.fm`: the metadata on a `while`, on a `for`'s `for.inc`, on a `while`'s two branches back,
  and on a loop whose condition reads a `const` variable; none on `while (1)`, `for (;;)`, `while (true)`,
  `for (; 2 > 1 && !0;)` and `while (1.5f)`, and no function marked `mustprogress`; at `-O2`, two unused loops deleted
  and the infinite ones kept.
- `tests/unit/AttributeTest.cpp`, in the JIT, what must not change: a field read across a transform, a module's own
  spectrum as the transform's output, a store into the input just before a transform, a round trip through two
  transforms; `fft_create(7)` still null inside FMinus; a block reads 0 until written; loops with a `continue`, a `?:`,
  `while (1)` with a `break` and `for (;;)` with a `return`; a program's own prototype of `fminus_rt_free`, with other
  types; libm's prototypes lowered for Apple, Linux and Windows triples, each declaration's memory, and a loop of
  `tanf` and `powf` against C++'s calls of libm, bit for bit.
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
more: no macros and no conditional compilation, which a language with modules, `const` and parameters needs less than
C does. Two choices are its own, both because FMinus has no headers:

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
| `#define`, `#if`, `#ifdef`, ... | an error: `#define is not supported: FMinus's directives are #include and #pragma` | |
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

## Pitfalls

Each of these was hit while writing the language or its recipes. [simd.md](simd.md) has the vector ones.

| Symptom | Cause | Fix |
|---|---|---|
| Code lands in a dead block, or the verifier reports a block without a terminator, wherever you ask whether the current block is finished | LLVM 23's `BasicBlock::getTerminator()` assumes a finished block; on an unfinished one it returns `&InstList.back()`, a bogus non-null pointer (the assert is compiled out under `NDEBUG`) | `hasTerminator()`, or `getTerminatorOrNull()` |
| A syntax-error test changes text after you add an unrelated token | Bison's `parse.error detailed` lists the expected tokens only when there are at most four, and a new token that can start a construct joins the list (or pushes it past four) | update the test; or `%define parse.error custom` and write `Parser::report_syntax_error` |
| Bison: "shift/reduce conflict on token "else"" | the dangling else (recipe 4) | `%precedence THEN` below `%precedence "else"`, and `%prec THEN` on the if without an else |
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
| The compiler crashes on `p->y`, and `--emit ast` prints a `OnePole*` as `? *` | a pointer built from a type's kind, `pointerTo(type.kind, ...)`, lost its module | pass the `Type`: `pointerTo(type, ...)` keeps `module` |
| A C++ reference differs from FMinus in the last bit, though the formulas match | clang's default `-ffp-contract=on` fuses `y + k * (x - y)` written as one expression into a multiply-add; FMinus never fuses | build the reference with `-ffp-contract=off` (the examples are), or round once a statement (`ModuleTest.cpp`'s `next`) |
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

## Where to go next

Design notes for growing the C subset beyond the recipes: what each feature involves and which LLVM pieces it uses.
This code is not written or verified. `examples/biquad/README.md` suggests an order driven by a real kernel.

**A type checker, and the rest of C's arithmetic types.** Recipe 3 converts with LLVM's types. Recipe 7 takes the
first step away from that: every lowered expression carries its FMinus type (simd.md, "Every value carries its
FMinus type"), and recipe 10 adds `int` and pointers to it. That still computes types while lowering, which stops
scaling once types multiply, as C's do quickly: `unsigned`, `long`, the narrower integers, `double`. Give the tree a
semantic pass before lowering:

- resolve every name to its declaration;
- compute every expression's type with C's usual arithmetic conversions and integer promotions;
- record each one beside the node (a side table keyed by node address, or a `type` field on `Expr`);
- insert the implicit conversions as explicit nodes;
- report the type errors C has: "invalid operands to binary expression", incompatible types in an assignment.

Lowering then just reads the types. Each integer type lowers to an `iN`, and signedness lives in the operations,
not the types: `udiv` and `urem` beside recipe 10's `sdiv` and `srem`, unsigned `icmp`s, `zext` where `int` uses
`sext`, `uitofp` and `fptoui`. An `unsigned` has no `nsw`: it wraps, by definition.

**More arrays, and more pointers.** Recipe 12's arrays are one dimension deep. An array of arrays, `float m[4][4]`, is
an `[4 x [4 x float]]` whose rows decay in turn; it needs `Type` to nest (its element a type, not a kind), and so do
pointers to pointers and arrays of pointers. C's `float c[] = {1, 2, 3};` takes its size from its list, and `sizeof` of
an array type (`sizeof(float[16])`) wants the grammar's abstract declarators. `bool*` and arrays of `bool` need `i8` in
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
vector code; each function would still give a float one result, but one `sinf` would give two in one program. *Sine
and cosine together*: libsystem_m's `_simd_sincos_f4` is not `_simd_sin_f4` and `_simd_cos_f4` (they differ in about
7% of vectors), so it would break the promise unless a program took both from it everywhere. *FMinus's own math
functions*, written in FMinus's arithmetic as SLEEF's deterministic ones are, and used to fold constants too, would give
the same results on every platform, at every width, with fast scalar calls. And *Linux's libmvec* behind the same
runtime functions, once the same premise tests pass there.

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

**State: globals and `static`.** File-scope variables lower to LLVM globals, and `static` locals likewise. For a
plugin, prefer state each instance owns: a module (recipe 12), in memory the host gives it, as `examples/biquad/`'s
filter and `examples/voices/`'s synth are, or memory the program allocates off the audio thread, as
`examples/delay/` does (recipe 11). Nothing then allocates on the audio thread, and every instance has its own copy.

**`switch`.** It lowers to LLVM's `switch` instruction, and its `break` reuses recipe 4's loop stack (rename
`Loop`). Fall-through and `default:` are C's.

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
(`FrontendTest.cpp`, and `CodeGenTest.cpp`, `ControlFlowTest.cpp`, `IntTest.cpp`, `PointerTest.cpp`, `HeapTest.cpp`,
`EffectTest.cpp`, `ArrayTest.cpp`, `ModuleTest.cpp`, `FftTest.cpp`, `ParamTest.cpp`, `VectorTest.cpp` and `PolyTest.cpp`
for the code generator; `MathTest.cpp`, `VersionTest.cpp`, `TbaaTest.cpp`, `ZeroInitTest.cpp`, `AttributeTest.cpp` and
`DenormalTest.cpp` for recipes 15 to 20), `tests/library/` (`ProgramTest.cpp`, `StrictAliasingTest.cpp`,
`DenormalsTest.cpp`) and `tests/lit/fminus/`. [recipes/](recipes/) holds the two that aren't, 5 and 6, as one patch
against the compiler as it is ([recipes/README.md](recipes/README.md)):

```bash
git switch -c try-recipes
git apply docs/recipes/5-runtime-function-and-pass.patch
cmake --workflow --preset dev
```

Once your own compiler has moved on, the patch stops applying, but it remains a readable diff of one way to make the
change.
