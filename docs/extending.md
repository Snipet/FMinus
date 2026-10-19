# Extending the compiler

How to add to FMinus. First, how a change moves through the compiler, and the few rules that keep that easy. Then
six worked recipes, which grow the C subset from a one-line builtin to C's control flow and a custom LLVM pass. Then
the pitfalls those recipes ran into, and design notes for what comes next. [simd.md](simd.md) continues with
recipes 7 to 9, for vectors.

Recipes 1 to 4 are in the compiler now (`tanhf` and `%`; `bool` and its operators; `if`, `while` and `for`), and
so are simd.md's 7 to 9, so their sections describe how the code you have got there: read a section beside the files
it names. Recipes 5 and 6, a runtime function and a pass, are not part of the language; they live in
[recipes/](recipes/) as a patch, so you can apply them to see them work. None of these are sketches: each was applied
to this repository, built with `-Werror`, and tested in the dev and asan presets, every test passing.
[architecture.md](architecture.md) describes the stages they touch.

FMinus follows C. When a recipe has a semantic choice to make, it makes C's: conversions, short-circuiting, which
`if` an `else` belongs to, what `continue` does in a `for`. And it reports errors in clang's words. The README lists
the few deliberate deviations.

## How a change moves through the compiler

Most language features touch the same places, in this order:

| # | Where | What to add |
|---|---|---|
| 1 | `src/frontend/Lexer.l` | a rule for each new token. Keywords go before `{IDENTIFIER}`. |
| 2 | `src/frontend/Parser.y` | the tokens (`%token`), their precedence if they are operators, and the grammar rules with their actions |
| 3 | `src/frontend/Ast.h` | a node: an alternative of `Expr::node` or `Stmt::node`, or an enumerator of `UnaryOp`, `BinaryOp`, `AssignOp`, `Type` |
| 4 | `src/frontend/Ast.cpp` | its `dump` |
| 5 | `src/ir/` | its conversions and checks, and its lowering to LLVM IR, in the file its kind belongs to (`Lowering.h` lists them: types, functions, statements, expressions, calls, vectors) |
| 6 | `src/runtime/` | if it needs C++ at run time, a runtime function (recipe 5) |
| 7 | `tools/fminus/main.cpp`, `src/support/Aot.cpp` | for a new type: whether `--run` can pass and print it (its `switch` over the result type), and whether an executable's `main` may take or return it (`addEntryPoint`) |
| 8 | `tests/` | a dump in `FrontendTest.cpp`; results in `CodeGenTest.cpp`, `ControlFlowTest.cpp`, `VectorTest.cpp` or `PolyTest.cpp` (their helpers are in `Compile.h`); IR or errors in `tests/lit/fminus/` |
| 9 | `examples/` | a program that uses it, and a line in `tests/lit/fminus/examples.test` that runs it |
| 10 | `editors/vscode/syntaxes/fminus.tmLanguage.json` | a new keyword, type or builtin, so VSCode highlights it (its README says where) |
| 11 | `README.md` | the language table, and the deviations from C if any |

Steps 3 to 5 need no searching. Once the node exists, the build fails at every walk that doesn't handle it yet: a
`std::visit` without a matching lambda fails to compile, and a `switch` without the new enumerator trips `-Wswitch`,
an error under `-Werror`. Fix the errors in order and the feature is wired through. `std::visit` names only the
first alternative it can't handle, so after you fix one, the next appears. The same goes for step 7's `switch`: when
recipe 7 added `Float2` to `Bool8`, the build stopped in the driver until `--run` said what to do with a vector. Steps
9 and 10 are the ones nothing reminds you of.

Each feature has an example to read beside its recipe: `examples/comparison/` for `bool` (recipe 3),
`examples/waveforms/` for every statement (recipe 4), `examples/chord/` for `float4` and the vector math library
(recipes 7 and 8), `examples/unison/` for `poly float` (recipe 9), and `examples/biquad/` for a `float4` returning
several results.

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

Before the row, a program could already call `tanhf` C's way, through a prototype, `float tanhf(float x);`: an
external function, which the JIT finds in libm. (Any `<math.h>` function that isn't a builtin still works that way,
`atanf` for one.) The builtin makes it part of the language: no declaration is needed, and the call is the intrinsic
that LLVM's passes treat uniformly. `IntrinsicEnums.inc`, in the LLVM install's `include/llvm/IR/`, lists the
intrinsics. `exp2f` (`exp2`), `powf` (`pow`), `copysignf` (`copysign`) and `fmaf` (`fma`, three parameters) are one
row each.

Update the count in `CodeGenTest.cpp` (`builtins().size()`), then test the value, in the builtins' test case:

```cpp
                       "float h(float x) { return tanhf(x); }\n");
    ...
    CHECK(lookup<float(float)>(*jit, "h")(0.5f) == std::tanh(0.5f));
    CHECK(fminus::ir::builtins().size() == 10);
```

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
  type, it goes straight to `float`.
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
  %x4 = load float, ptr %x.addr, align 4
  br label %cond.end
cond.false:
  %y5 = load float, ptr %y.addr, align 4
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

### The loop, before and after

```llvm
; fminus sum.fm -O0: the four blocks, as clang names them (the loads and stores elided)
for.cond:                                         ; preds = %for.inc, %entry
  br i1 %cmp, label %for.body, label %for.end
for.body:                                         ; preds = %for.cond
  br label %for.inc
for.inc:                                          ; preds = %for.body
  br label %for.cond
for.end:                                          ; preds = %for.cond
  ret float %total6

; fminus sum.fm -O1: the slots became phis, and the loop was rotated (the test moved to the bottom)
for.body:                                         ; preds = %entry, %for.body
  %i.09 = phi float [ %1, %for.body ], [ 1.000000e+00, %entry ]
  %total.08 = phi float [ %0, %for.body ], [ 0.000000e+00, %entry ]
  %0 = fadd float %total.08, %i.09
  %1 = fadd float %i.09, 1.000000e+00
  %cmp = fcmp ugt float %1, %n
  br i1 %cmp, label %for.end, label %for.body
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

## Recipe 6: an LLVM pass of your own (`CountFlopsPass`)

*Patch: [recipes/5-runtime-function-and-pass.patch](recipes/5-runtime-function-and-pass.patch).*

`src/opt/` holds FMinus's own LLVM passes. The example counts a function's floating-point operations after
optimisation and records the count as a string attribute, `"fminus-flops"="3"`. That's handy for watching what
LLVM leaves of a kernel; the biquad's coefficient math, for instance.

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
  `sinf`. Why not Accelerate, measured.
- **Recipe 9, `poly float`**: a vector as wide as the target's registers (4 floats on NEON, 16 on AVX-512), chosen by
  LLVM's cost model, with rules that keep a program's meaning the same at every width, `poly_lanes()` and
  `poly_index()`, and what does change with the width (the order of float sums).

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

## Where to go next

Design notes for growing the C subset beyond the recipes: what each feature involves and which LLVM pieces it uses.
This code is not written or verified. `examples/biquad/README.md` suggests an order driven by a real kernel.

**A type checker, and C's integer types.** Recipe 3 converts with LLVM's types. Recipe 7 takes the first step away
from that: every lowered expression carries its FMinus type (simd.md, "Every value carries its FMinus type"). That
still computes types while lowering, which stops scaling once types multiply, as C's do quickly: `int`, `unsigned`,
the narrower integers, `double`. Give the tree a semantic pass before lowering:

- resolve every name to its declaration;
- compute every expression's type with C's usual arithmetic conversions and integer promotions;
- record each one beside the node (a side table keyed by node address, or a `type` field on `Expr`);
- insert the implicit conversions as explicit nodes;
- report the type errors C has: "invalid operands to binary expression", incompatible types in an assignment.

Lowering then just reads the types. Integers lower to `iN` with `add`/`sub`/`mul`/`sdiv`/`udiv`, and `icmp`
compares them. The conversions are `sitofp`, `fptosi`, `zext` and `sext`; signedness lives in the operations, not
the types.

Until then, a loop counts in floats, as the examples' loops do (`for (float k = 1; ...; k += 1)`). That is exact for
whole numbers up to 2^24, 16777216; past it, `k += 1` stops changing `k`, and the loop never ends.

**Pointers and arrays.** `float* buffer`, `buffer[i]`, `*p`. In LLVM, every pointer is an opaque `ptr`, and the
element type rides on each `getelementptr`, `load` and `store`. `buffer[i]` is a GEP and a load, and as a target
it is a GEP and a store. So `lowerAssign`'s "must be a variable" check grows into an *lvalue* check, which is why the
grammar already takes any expression as a target. Two things matter most for DSP: `restrict` (C99), which lowers to
`noalias` on parameters, and `const float*`. They tell the vectoriser that buffers don't overlap.

**Structs.** `struct coefficients { float b0, b1, b2, a1, a2; };` becomes an LLVM struct type, with fields reached by
GEP. At the C boundary, AArch64 passes and returns a struct of up to four floats in registers `s0`–`s3` (a
homogeneous floating-point aggregate), and a larger one through memory (`sret` for a return). Clang's IR for the
equivalent C declaration shows exactly what to emit. Inside FMinus, where calls get inlined, any representation
works.

**State: globals and `static`.** File-scope variables lower to LLVM globals, and `static` locals likewise. For a
plugin, prefer state the host owns: a struct passed by pointer, so nothing allocates on the audio thread and every
instance has its own copy.

**`switch`.** It lowers to LLVM's `switch` instruction, and its `break` reuses recipe 4's loop stack (rename
`Loop`). Fall-through and `default:` are C's.

**`void` functions and expression statements that matter.** With state and pointers, `void process(...)` becomes
useful: `ret void`, and "void function should not return a value".

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
LLJIT's concurrent compilation, and caching objects between runs (`llvm::ObjectCache`).

## Reference solutions

The recipes in the compiler are their own reference: their code is in `src/`, and their tests in `tests/unit/`
(`FrontendTest.cpp`, and `CodeGenTest.cpp`, `ControlFlowTest.cpp`, `VectorTest.cpp` and `PolyTest.cpp` for the code
generator) and `tests/lit/fminus/`. [recipes/](recipes/) holds the two that
aren't, 5 and 6, as one patch against the compiler as it is ([recipes/README.md](recipes/README.md)):

```bash
git switch -c try-recipes
git apply docs/recipes/5-runtime-function-and-pass.patch
cmake --workflow --preset dev
```

Once your own compiler has moved on, the patch stops applying, but it remains a readable diff of one way to make the
change.
