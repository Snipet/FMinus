# Extending the compiler

How to add to FMinus. First, how a change moves through the compiler, and the few rules that keep that easy. Then
six worked recipes, from a one-line builtin to loops and a custom LLVM pass. Then the pitfalls those recipes ran into,
and design notes for what comes next.

The recipes are not sketches. Each was applied to this repository, built with `-Werror`, and tested in the dev and
asan presets, all 19 tests passing. They live in [recipes/](recipes/) as patches, so you can apply one to see it work,
or compare it with your own version. [architecture.md](architecture.md) describes the stages they touch.

## How a change moves through the compiler

Most language features touch the same places, in this order:

| # | Where | What to add |
|---|---|---|
| 1 | `src/frontend/Lexer.l` | a rule for each new token. Keywords go before `{IDENTIFIER}`. |
| 2 | `src/frontend/Parser.y` | the tokens (`%token`), their precedence if they are operators, and the grammar rules with their actions |
| 3 | `src/frontend/Ast.h` | a node: an alternative of `Expr::node` or `Stmt::node`, or an enumerator of `BinaryOp`, `UnaryOp`, `Type` |
| 4 | `src/frontend/Ast.cpp` | its `dump` |
| 5 | `src/ir/CodeGen.cpp` | its checks and its lowering to LLVM IR |
| 6 | `src/runtime/` | if it needs C++ at run time, a runtime function (recipe 5) |
| 7 | `tests/` | a dump in `FrontendTest.cpp`, results in `CodeGenTest.cpp`, IR or errors in `tests/lit/fminus/` |
| 8 | `README.md` | the language table |

Steps 3 to 5 need no searching. Once the node exists, the build fails at every walk that doesn't handle it yet: a
`std::visit` without a matching lambda fails to compile, and a `switch` without the new enumerator trips `-Wswitch`,
an error under `-Werror`. Fix the errors in order and the feature is wired through.

## Ground rules

- **Let the compiler find the places.** Never write `default:` in a `switch` over an AST enum, nor a catch-all
  lambda (`[](const auto&)`) in a `std::visit`. Either one switches off the errors that list what a new node still
  needs.
- **Keep the syntax tree free of LLVM.** Nodes hold names, spellings, enums and children. Types, values and checks
  belong to the passes.
- **Report and continue.** In the code generator, `error(location, message)` records an error. Then return a poison
  value of the expected type (`poison(ast::Type::Float)`), never a null pointer, so lowering goes on and finds the
  next error. `lower` throws the module away at the end.
- **Leave the IR valid.** Every block ends in exactly one terminator (`ret`, `br`). A phi lists every predecessor
  exactly once. Allocas belong in the entry block. `verifyFunction` runs on every function and reports a violation
  as an internal error.
- **Look at the IR twice.** `fminus file.fm -O0` shows what your lowering wrote; `fminus file.fm` (-O2) shows what
  LLVM made of it. Most lowering bugs are obvious at `-O0`, and most performance questions are answered at `-O2`.
- **Test both halves.** New syntax gets a dump test, new semantics a codegen test (run in the JIT), and new errors a
  test that checks their text and location.

## Recipe 1: a builtin (`tanh`)

*Patch: [recipes/1-builtin-and-operator.patch](recipes/1-builtin-and-operator.patch) (with recipe 2).*

A builtin is a row in `kBuiltins` (`src/ir/CodeGen.cpp`). The name maps to an LLVM intrinsic over `float`, with its
parameter count:

```cpp
            Builtin{"max", llvm::Intrinsic::maxnum, 2}, Builtin{"tanh", llvm::Intrinsic::tanh, 1},
```

That is the whole change. `lowerCall` already declares the intrinsic (`Intrinsic::getOrInsertDeclaration`),
checks the argument count, and refuses a function that tries to reuse the name. LLVM lowers `llvm.tanh.f32` to a
call to libm's `tanhf`, which the JIT resolves from the process.

`IntrinsicEnums.inc` in the LLVM install (`include/llvm/IR/`) lists the intrinsics. Intrinsics such as `exp2`,
`pow`, `copysign`, `fma`, `minimum` and `maximum` are one row each: `pow`, `copysign`, `minimum` and `maximum` take two
parameters, and `fma` takes three.
Update the count in `CodeGenTest.cpp` (`builtins().size()`) and test the value:

```cpp
    auto jit = build("fn f(x: float) -> float { return tanh(x); }", errors);
    auto f = llvm::cantFail(jit->lookup("f")).toPtr<float (*)(float)>();
    CHECK(f(0.5f) == std::tanh(0.5f));
```

## Recipe 2: an operator (`%`)

*Patch: [recipes/1-builtin-and-operator.patch](recipes/1-builtin-and-operator.patch).*

Floating-point remainder, with C's `fmodf` semantics: the result takes the sign of the dividend. It binds like `*`
and `/`.

**Token** (`Lexer.l`, beside the other operators):

```lex
"%"             { return Parser::make_PERCENT(yyextra->where); }
```

**Grammar** (`Parser.y`): declare the token, put it on the `*` `/` precedence level, and add the rule.

```yacc
%token ARROW "->" PLUS "+" MINUS "-" STAR "*" SLASH "/" PERCENT "%" ASSIGN "="
...
%left "*" "/" "%"
...
    | expr "%" expr                         { $$ = ast::makeExpr(at(@2), ast::BinaryExpr{ast::BinaryOp::Remainder, $1, $3}); }
```

**Node** (`Ast.h`):

```cpp
    enum class BinaryOp
    {
        Add,
        Subtract,
        Multiply,
        Divide,
        Remainder,   // the sign of the dividend, as C's fmodf
    };
```

Build now, and the compiler lists the rest of the work:

```
src/frontend/Ast.cpp:16:21: error: enumeration value 'Remainder' not handled in switch [-Werror,-Wswitch]
src/ir/CodeGen.cpp:207:37: error: enumeration value 'Remainder' not handled in switch [-Werror,-Wswitch]
```

**Dump** (`Ast.cpp`, in `opName`):

```cpp
            case BinaryOp::Remainder:
                return "%";
```

**Lowering** (`CodeGen.cpp`, in the `BinaryExpr` lambda):

```cpp
                            case ast::BinaryOp::Remainder:
                                return builder_.CreateFRem(lhs, rhs);
```

**Tests.** The precedence shows in the dump: `a + b % a * b` is `(+ a (* (% b a) b))`. The value shows in the JIT:

```cpp
    auto jit = build("fn f(x: float, y: float) -> float { return x % y * 2; }", errors);
    auto f = llvm::cantFail(jit->lookup("f")).toPtr<float (*)(float, float)>();
    CHECK(f(7.5f, 2.0f) == 3.0f);    // (7.5 % 2) * 2: % binds as * and /
    CHECK(f(-7.5f, 2.0f) == -3.0f);  // the sign of the dividend
```

`frem` has no AArch64 instruction; it becomes a call to `fmodf`. Check with `fminus file.fm --emit asm -o -`.

## Recipe 3: comparisons, `bool` and an `if` expression

*Patch: [recipes/2-comparisons-bool-if.patch](recipes/2-comparisons-bool-if.patch).*

This recipe adds:

- `<`, `<=`, `>`, `>=`, `==` and `!=` over floats, which give `bool`;
- `bool` as a type for parameters and return values;
- `if c { a } else { b }`, an expression whose branches give a value of one type.

```
fn magnitude(x: float) -> float { return if x < 0 { -x } else { x }; }
fn positive(x: float) -> bool { return x > 0; }
```

It brings three firsts: a second type, which means type checks; a C ABI detail; and control flow, which means basic
blocks and a phi.

### Tokens and grammar

`Lexer.l`: the keywords go with the others, before `{IDENTIFIER}`. The operators can go in any order, because Flex
takes the longest match, so `<=` wins over `<`.

```lex
"bool"          { return Parser::make_BOOL(yyextra->where); }
"if"            { return Parser::make_IF(yyextra->where); }
"else"          { return Parser::make_ELSE(yyextra->where); }
...
"<="            { return Parser::make_LE(yyextra->where); }
">="            { return Parser::make_GE(yyextra->where); }
"=="            { return Parser::make_EQ(yyextra->where); }
"!="            { return Parser::make_NE(yyextra->where); }
"<"             { return Parser::make_LT(yyextra->where); }
">"             { return Parser::make_GT(yyextra->where); }
```

`Parser.y`: the comparisons are `%nonassoc`, so `a < b < c` is a syntax error rather than a surprise. They bind more
loosely than arithmetic, which puts them above `+` and `-`.

```yacc
%token FN "fn" LET "let" RETURN "return" FLOAT "float" BOOL "bool" IF "if" ELSE "else"
%token LT "<" LE "<=" GT ">" GE ">=" EQ "==" NE "!="
...
%nonassoc "<" "<=" ">" ">=" "==" "!="
%left "+" "-"
%left "*" "/" "%"
%precedence NEG
...
type
    : "float"                               { $$ = ast::Type::Float; }
    | "bool"                                { $$ = ast::Type::Bool; }
    ;
...
    | expr "<" expr                         { $$ = ast::makeExpr(at(@2), ast::BinaryExpr{ast::BinaryOp::Less, $1, $3}); }
    | expr "<=" expr                        { $$ = ast::makeExpr(at(@2), ast::BinaryExpr{ast::BinaryOp::LessEqual, $1, $3}); }
    | expr ">" expr                         { $$ = ast::makeExpr(at(@2), ast::BinaryExpr{ast::BinaryOp::Greater, $1, $3}); }
    | expr ">=" expr                        { $$ = ast::makeExpr(at(@2), ast::BinaryExpr{ast::BinaryOp::GreaterEqual, $1, $3}); }
    | expr "==" expr                        { $$ = ast::makeExpr(at(@2), ast::BinaryExpr{ast::BinaryOp::Equal, $1, $3}); }
    | expr "!=" expr                        { $$ = ast::makeExpr(at(@2), ast::BinaryExpr{ast::BinaryOp::NotEqual, $1, $3}); }
    | "if" expr "{" expr "}" "else" "{" expr "}"
                                            { $$ = ast::makeExpr(at(@1), ast::IfExpr{$2, $4, $8}); }
```

There are no conflicts: after `"if" expr`, a `{` can only start the branch, because `{` is not an operator. If you
change the syntax and Bison reports a conflict, its `-Wcounterexamples` output shows an input that parses two ways.

### Nodes

`Ast.h`: a type, six operators and a node.

```cpp
    enum class Type
    {
        Float,
        Bool,
    };
    ...
        Remainder,   // the sign of the dividend, as C's fmodf
        Less,        // the comparisons give bool; with a NaN, all but != are false
        LessEqual,
        Greater,
        GreaterEqual,
        Equal,
        NotEqual,
    };
    ...
    // `if condition { thenValue } else { elseValue }`: an expression, so both branches give a value of one type.
    struct IfExpr
    {
        ExprPtr condition;
        ExprPtr thenValue;
        ExprPtr elseValue;
    };

    struct Expr
    {
        SourceLocation location;
        std::variant<NumberExpr, VariableExpr, UnaryExpr, BinaryExpr, CallExpr, IfExpr> node;
    };
```

The build then fails in six places:

- `Ast.cpp`: the operator switch, the dump's `std::visit` ("`std::visit` requires the visitor to be exhaustive"),
  and `typeName`;
- `CodeGen.cpp`: `typeOf`, the operator switch and the lowering's `std::visit`.

The dump is mechanical: an `opName` case per operator, `case Type::Bool: return "bool";` in `typeName`, and:

```cpp
                                  [](const IfExpr& branch) {
                                      return "(if " + dump(*branch.condition) + " " + dump(*branch.thenValue) + " "
                                             + dump(*branch.elseValue) + ")";
                                  },
```

### Types in the code generator

With two types, every operand needs checking. The smallest approach that works is to treat each value's LLVM type
as its FMinus type: `float` is `float`, and `bool` is `i1`. A handful of helpers in `Lowering` cover it:

```cpp
                case ast::Type::Bool:
                    return builder_.getInt1Ty();
...
            // An LLVM type's FMinus name, for messages.
            static std::string describe(const llvm::Type* type)
            {
                return type->isIntegerTy(1) ? "bool" : "float";
            }

            // `value` when it has `type`; otherwise an error and a poison of `type`, so lowering goes on.
            llvm::Value* expect(SourceLocation location, llvm::Value* value, ast::Type type, const std::string& what)
            {
                if (value->getType() == typeOf(type))
                    return value;
                error(location, what + " must be " + ast::typeName(type) + ", not " + describe(value->getType()));
                return poison(type);
            }
```

Then every place that consumes a value says what it expects:

- the operands of `-` and of the binary operators: `float`;
- a `return`: the function's return type;
- each argument: the callee's parameter type (`float` for builtins);
- the condition of an `if`: `bool`.

```cpp
                            llvm::Value* lhs = expect(expr.location, lower(*binary.lhs), ast::Type::Float,
                                                      "the left operand");
                            llvm::Value* rhs = expect(expr.location, lower(*binary.rhs), ast::Type::Float,
                                                      "the right operand");
...
                                       builder_.CreateRet(expect(stmt.location, lower(*ret.value),
                                                                 function.returnType,
                                                                 "the value " + quoted(function.name) + " returns"));
...
                for (std::size_t i = 0; i < args.size(); ++i)
                {
                    args[i] = expect(location, args[i], callee.params[i].type,
                                     "argument " + std::to_string(i + 1) + " of " + quoted(call.callee));
                }
```

`wrongArity` now returns a poison of the callee's return type (a fourth parameter, `ast::Type result`), so a wrong
argument count in a call to a `bool` function causes no second, misleading error.

This stays manageable for a couple of types. When types multiply (integers, buffers, structs) or need conversions,
move the checks into their own pass over the tree that records each expression's type (see "Where to go next").

### Comparisons

LLVM has ordered and unordered float comparisons. Ordered ones are false when either side is NaN; unordered ones
are true. C's operators use ordered comparisons except for `!=`, and FMinus follows C:

```cpp
                            // Ordered comparisons: false when either side is NaN. != is unordered: true then.
                            case ast::BinaryOp::Less:
                                return builder_.CreateFCmpOLT(lhs, rhs);
                            case ast::BinaryOp::LessEqual:
                                return builder_.CreateFCmpOLE(lhs, rhs);
                            case ast::BinaryOp::Greater:
                                return builder_.CreateFCmpOGT(lhs, rhs);
                            case ast::BinaryOp::GreaterEqual:
                                return builder_.CreateFCmpOGE(lhs, rhs);
                            case ast::BinaryOp::Equal:
                                return builder_.CreateFCmpOEQ(lhs, rhs);
                            case ast::BinaryOp::NotEqual:
                                return builder_.CreateFCmpUNE(lhs, rhs);
```

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

Every new type that crosses into C++ raises the same question: integers narrower than 32 bits, structs (passed in
registers or through memory, depending on their size), vectors. The answer is whatever clang emits for the
equivalent C declaration. To see it, write the C version and run `clang -S -emit-llvm`.

### The `if` expression: blocks and a phi

An `if` needs control flow: a conditional branch to two blocks, each computing its value and branching to a join
block, where a phi picks the value of whichever branch ran.

```cpp
                        [&](const ast::IfExpr& branch) { return lowerIf(expr.location, branch); },
...
            // condition ? then : else as a diamond of blocks joined by a phi. (SimplifyCFG turns a small one into a
            // select at -O1 and above.)
            llvm::Value* lowerIf(SourceLocation location, const ast::IfExpr& branch)
            {
                llvm::Value* condition =
                    expect(location, lower(*branch.condition), ast::Type::Bool, "the condition of an if");
                llvm::Function* function = builder_.GetInsertBlock()->getParent();
                llvm::LLVMContext& context = module_.getContext();
                llvm::BasicBlock* thenBlock = llvm::BasicBlock::Create(context, "if.then", function);
                llvm::BasicBlock* elseBlock = llvm::BasicBlock::Create(context, "if.else", function);
                llvm::BasicBlock* endBlock = llvm::BasicBlock::Create(context, "if.end", function);
                builder_.CreateCondBr(condition, thenBlock, elseBlock);

                // Each branch may end in another block than it started in (a nested if): the phi needs the last.
                builder_.SetInsertPoint(thenBlock);
                llvm::Value* thenValue = lower(*branch.thenValue);
                llvm::BasicBlock* thenEnd = builder_.GetInsertBlock();
                builder_.CreateBr(endBlock);

                builder_.SetInsertPoint(elseBlock);
                llvm::Value* elseValue = lower(*branch.elseValue);
                llvm::BasicBlock* elseEnd = builder_.GetInsertBlock();
                builder_.CreateBr(endBlock);

                builder_.SetInsertPoint(endBlock);
                if (thenValue->getType() != elseValue->getType())
                {
                    error(location, "the branches of an if give " + describe(thenValue->getType()) + " and "
                                        + describe(elseValue->getType()));
                    return poison(ast::Type::Float);
                }
                llvm::PHINode* phi = builder_.CreatePHI(thenValue->getType(), 2, "if.value");
                phi->addIncoming(thenValue, thenEnd);
                phi->addIncoming(elseValue, elseEnd);
                return phi;
            }
```

The comment about `thenEnd` matters. With a nested `if` in the then-branch, lowering that branch leaves the builder
in the nested `if.end`, not in `thenBlock`, and the phi must name that block as its predecessor. Taking
`GetInsertBlock()` after lowering each branch gets this right. Naming `thenBlock` instead produces IR the verifier
rejects.

At `-O0` you see the diamond. At `-O2`, SimplifyCFG has flattened it into a `select`, which for DSP code is usually
what you want: no branch to mispredict.

```llvm
; fminus magnitude.fm -O0
define float @magnitude(float %x) {
entry:
  %0 = fcmp olt float %x, 0.000000e+00
  br i1 %0, label %if.then, label %if.else
if.then:
  %1 = fneg float %x
  br label %if.end
if.else:
  br label %if.end
if.end:
  %if.value = phi float [ %1, %if.then ], [ %x, %if.else ]
  ret float %if.value
}

; fminus magnitude.fm -O2
define float @magnitude(float %x) local_unnamed_addr #0 {
entry:
  %0 = fcmp olt float %x, 0.000000e+00
  %1 = fneg float %x
  %if.value = select i1 %0, float %1, float %x
  ret float %if.value
}
```

### Tests

```cpp
    auto positive = llvm::cantFail(jit->lookup("positive")).toPtr<bool (*)(float)>();
    CHECK(positive(2.0f));
    CHECK_FALSE(positive(std::nanf("")));
    ...
    CHECK(errors == "test.fm:1:34: error: the condition of an if must be bool, not float\n"
                    "test.fm:2:36: error: the right operand must be float, not bool\n"
                    "test.fm:3:26: error: the value 'h' returns must be bool, not float\n"
                    "test.fm:4:34: error: the branches of an if give float and bool\n");
```

Two existing tests also change, though nothing about them broke. With `if` added, five tokens can now start an
expression, and Bison's detailed message lists the expected tokens only when there are at most four (see the
pitfalls). "syntax error, unexpected ;, expecting - or ( or identifier or number" becomes "syntax error, unexpected
;", in `FrontendTest.cpp` and in `tests/lit/fminus/syntax-error.fm`.

## Recipe 4: `var`, assignment and `while`

*Patch: [recipes/3-var-assignment-while.patch](recipes/3-var-assignment-while.patch), on top of recipe 3.*

This recipe adds mutable bindings, `var x = e;`, which `x = e;` may change, and loops,
`while condition { statements }`:

```
fn sum_to(n: float) -> float {
    var i = 0;
    var total = 0;
    while i < n {
        i = i + 1;
        total = total + i;
    }
    return total;
}
```

### Tokens, grammar, nodes

```lex
"var"           { return Parser::make_VAR(yyextra->where); }
"while"         { return Parser::make_WHILE(yyextra->where); }
```

```yacc
%token FN "fn" LET "let" RETURN "return" FLOAT "float" BOOL "bool" IF "if" ELSE "else" VAR "var" WHILE "while"
...
    | "var" IDENTIFIER "=" expr ";"         { $$ = ast::Stmt{at(@1), ast::VarStmt{$2, $4}}; }
    | IDENTIFIER "=" expr ";"               { $$ = ast::Stmt{at(@1), ast::AssignStmt{$1, $3}}; }
    | "while" expr "{" statements "}"       { $$ = ast::Stmt{at(@1), ast::WhileStmt{$2, $4}}; }
```

A statement starting with an identifier can only be an assignment, since FMinus has no expression statements, so
there is no conflict. Add expression statements (`f(x);`) later, and `IDENTIFIER "="` against `expr` becomes the
first thing to think about.

`Ast.h`: three statements. A `while` holds a vector of statements, so `Stmt` is declared before it is defined
(`std::vector` of an incomplete type is fine since C++17).

```cpp
    // `var name = value;`: a binding `name = value;` may change. It lives in memory (an alloca), which mem2reg turns
    // back into SSA values.
    struct VarStmt
    {
        std::string name;
        ExprPtr value;
    };

    struct AssignStmt
    {
        std::string name;
        ExprPtr value;
    };

    struct Stmt;

    // `while condition { body }`. The body's bindings end with it.
    struct WhileStmt
    {
        ExprPtr condition;
        std::vector<Stmt> body;
    };

    struct Stmt
    {
        SourceLocation location;
        std::variant<LetStmt, ReturnStmt, VarStmt, AssignStmt, WhileStmt> node;
    };
```

The dump adds `(var x e)`, `(set x e)` and `(while c s...)`.

### Storage: allocas, and mem2reg

A `let` binds an SSA value, and an SSA value can't change. Building SSA directly for a variable that changes inside
a loop means placing phis yourself. The classic shortcut, and what clang does, is to give each mutable variable a
stack slot, an `alloca` in the entry block, and to `load` and `store` it. LLVM's mem2reg and SROA, which the
pipeline runs from `-O1`, turn those slots back into SSA values with the phis in the right places.

`scope_` now records which kind of binding each name is:

```cpp
            // A name in scope: a let's value or a parameter, or a var's alloca.
            struct Binding
            {
                llvm::Value* value;
                bool isVar;
            };
            std::unordered_map<std::string, Binding> scope_;   // the current function's bindings
            const ast::Function* function_ = nullptr;          // the function being lowered
```

A `var` allocates its slot at the top of the entry block, whatever block the builder is in. Allocas anywhere else
are not promoted, and a slot allocated inside a loop is allocated again on every iteration.

```cpp
            // A var's storage: an alloca at the top of the entry block, where mem2reg (O1 and up) promotes it.
            void declareVar(SourceLocation location, const ast::VarStmt& var)
            {
                llvm::Value* value = lower(*var.value);
                llvm::BasicBlock& entry = builder_.GetInsertBlock()->getParent()->getEntryBlock();
                llvm::IRBuilder<> atEntry(&entry, entry.begin());
                llvm::AllocaInst* slot = atEntry.CreateAlloca(value->getType(), nullptr, var.name);
                builder_.CreateStore(value, slot);
                if (!scope_.emplace(var.name, Binding{slot, true}).second)
                    error(location, quoted(var.name) + " is already defined");
            }
```

Reading a `var` loads it (in the `VariableExpr` lambda):

```cpp
                            if (found != scope_.end() && found->second.isVar)
                            {
                                auto* slot = llvm::cast<llvm::AllocaInst>(found->second.value);
                                return builder_.CreateLoad(slot->getAllocatedType(), slot, variable.name);
                            }
```

Assigning stores, after checking that the name is a `var` and that the type matches. The function is `assign` in
the patch: `unknown variable`, then "'a' is not a var, so it cannot change", then "'c' is float, not bool".

### Statements become recursive

Until now, `define` lowered a flat list of statements, with a `returned` flag to catch statements after a
`return`. With loops, statements nest, and a `return` can sit inside a loop body. So the list moves into
`lowerStatements`, a single statement into `lowerStatement`, and the flag becomes a question about the current
block: is it already terminated?

```cpp
                function_ = &function;
                lowerStatements(function.body);
                if (!terminated())
                {
                    error(function.location, "function " + quoted(function.name) + " does not end with a return");
                    builder_.CreateRet(poison(function.returnType));
                }
...
            // Whether the current block already ends (in a return): nothing more may be added to it. (Not
            // getTerminator(): since LLVM 23 it assumes a finished block.)
            bool terminated()
            {
                return builder_.GetInsertBlock()->hasTerminator();
            }

            void lowerStatements(const std::vector<ast::Stmt>& statements)
            {
                for (const ast::Stmt& stmt : statements)
                {
                    if (terminated())
                    {
                        error(stmt.location, "statement after the return of " + quoted(function_->name));
                        return;
                    }
                    lowerStatement(stmt);
                }
            }
```

Read the comment on `terminated()`. The first version called `getTerminator() != nullptr`, which on LLVM 23 returns
a bogus non-null pointer for an unfinished block (its assert is compiled out). Every function then reported
"statement after the return" at its first statement. See the pitfalls.

### The loop

```cpp
            // while.cond tests and branches to while.body or while.end; the body jumps back to while.cond.
            void lowerWhile(SourceLocation location, const ast::WhileStmt& loop)
            {
                llvm::Function* function = builder_.GetInsertBlock()->getParent();
                llvm::LLVMContext& context = module_.getContext();
                llvm::BasicBlock* condBlock = llvm::BasicBlock::Create(context, "while.cond", function);
                llvm::BasicBlock* bodyBlock = llvm::BasicBlock::Create(context, "while.body", function);
                llvm::BasicBlock* endBlock = llvm::BasicBlock::Create(context, "while.end", function);
                builder_.CreateBr(condBlock);

                builder_.SetInsertPoint(condBlock);
                llvm::Value* condition =
                    expect(location, lower(*loop.condition), ast::Type::Bool, "the condition of a while");
                builder_.CreateCondBr(condition, bodyBlock, endBlock);

                builder_.SetInsertPoint(bodyBlock);
                const auto outer = scope_;   // the body's bindings end with it
                lowerStatements(loop.body);
                scope_ = outer;
                if (!terminated())
                    builder_.CreateBr(condBlock);

                builder_.SetInsertPoint(endBlock);
            }
```

- **Scope.** Copying `scope_` around the body gives the body its own scope: a `let` inside a loop is gone after it
  ("unknown variable 'inner'"). A `var` declared outside stays visible, and stores to it stick, because the binding
  holds the slot, not the value. The copy is the simplest way to get block scope; a stack of maps is the usual
  next step.
- **A `return` in the body** terminates the body's block, so the back-edge isn't added. `while.end` is still
  reachable through the condition, so a function still needs a `return` after its loops.

### Before and after mem2reg

```llvm
; fminus sum.fm -O0: slots, loads and stores
define float @sum_to(float %n) #0 {
entry:
  %total = alloca float, align 4
  %i = alloca float, align 4
  store float 0.000000e+00, ptr %i, align 4
  store float 0.000000e+00, ptr %total, align 4
  br label %while.cond
while.cond:
  %i1 = load float, ptr %i, align 4
  %0 = fcmp olt float %i1, %n
  br i1 %0, label %while.body, label %while.end
while.body:
  ...
  br label %while.cond
while.end:
  %total5 = load float, ptr %total, align 4
  ret float %total5
}

; fminus sum.fm -O1: mem2reg placed the phis; the loop is rotated (the test moved to the bottom)
while.body:
  %i.07 = phi float [ %1, %while.body ], [ 0.000000e+00, %entry ]
  %total.06 = phi float [ %2, %while.body ], [ 0.000000e+00, %entry ]
  %1 = fadd float %i.07, 1.000000e+00
  %2 = fadd float %total.06, %1
  %3 = fcmp olt float %1, %n
  br i1 %3, label %while.body, label %while.end
```

### Tests

`sum_to(4) == 10`, nested loops, a `let` inside a body, and a `return` from inside a loop. The errors: assigning to
a `let`, an unknown name, a type mismatch, a non-`bool` condition, and a body's `let` used after the loop.

## Recipe 5: a runtime function (`db_to_gain`)

*Patch: [recipes/4-runtime-function-and-pass.patch](recipes/4-runtime-function-and-pass.patch) (with recipe 6), on
top of recipe 4.*

Some operations are better written in C++ than generated: lookup tables, approximations, calls to the platform. The
FMinus runtime is where they go. Generated code calls a runtime function by its symbol. The JIT resolves the symbol
through `defineRuntimeSymbols`, and an executable links `libfminus_runtime.a`.

**The function** (`src/runtime/Runtime.h`, `Runtime.cpp`, `RuntimeSymbols.def`):

```cpp
    // Decibels to a linear gain: 10^(db / 20). FMinus calls it as db_to_gain (ir/CodeGen.cpp).
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

Keep the `.def` line in step with the header. Without it, the JIT in this repository's tools still finds the
function, but only by accident. LLJIT also searches the process's own symbols, and the tools link the runtime
statically and export it. A plugin that hides its symbols or dead-strips the unused ones would fail at lookup.

**The name in FMinus** (`CodeGen.cpp`): a second table, beside `kBuiltins`, from the FMinus name to the runtime's
symbol.

```cpp
        // name -> a function of the FMinus runtime (runtime/Runtime.h), float(float, ...): JIT'd code finds it through
        // support/JitRuntime.h, an executable links libfminus_runtime.a.
        struct RuntimeFunction
        {
            std::string_view name;
            std::string_view symbol;
            unsigned arity;
        };
        constexpr std::array kRuntimeFunctions{
            RuntimeFunction{"db_to_gain", "fminus_rt_db_to_gain", 1},
        };
```

`lowerCall` checks it after the builtins. `getOrInsertFunction` declares the external function once per module:

```cpp
                if (const RuntimeFunction* runtime = findRuntimeFunction(call.callee))
                {
                    if (args.size() != runtime->arity)
                        return wrongArity(location, call, runtime->arity, ast::Type::Float);
                    for (llvm::Value*& arg : args)
                        arg = expect(location, arg, ast::Type::Float, "an argument of " + quoted(call.callee));
                    std::vector<llvm::Type*> params(runtime->arity, builder_.getFloatTy());
                    llvm::FunctionCallee callee = module_.getOrInsertFunction(
                        runtime->symbol, llvm::FunctionType::get(builder_.getFloatTy(), params, false));
                    return builder_.CreateCall(callee, args);
                }
```

`declare` refuses a function with a runtime name, as it does for builtins:
`findBuiltin(function.name) != nullptr || findRuntimeFunction(function.name) != nullptr`.

Runtime functions run inside the audio thread's call tree. Keep them free of allocation, locks and system calls,
and `noexcept`.

## Recipe 6: an LLVM pass of your own (`CountFlopsPass`)

*Patch: [recipes/4-runtime-function-and-pass.patch](recipes/4-runtime-function-and-pass.patch).*

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
                const bool arithmetic = llvm::isa<llvm::BinaryOperator, llvm::UnaryOperator, llvm::FCmpInst>(instruction);
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

```
// RUN: fminus %s -O2 | FileCheck %s
fn softclip(x: float, drive: float) -> float {
    let y = x * drive;
    return y / (1.0 + abs(y));
}
// CHECK: define float @softclip({{.*}} #[[ATTRS:[0-9]+]]
// CHECK: attributes #[[ATTRS]] = { {{.*}}"fminus-flops"="3"
```

Adding an attribute to every function changed an existing test: `codegen.fm` matched `define float @gain(float %x,
float %g) {` exactly, and at `-O0` the line now ends in `#0 {`. Write IR checks that tolerate attributes:
`define float @gain(float %x, float %g){{.*}} {`.

## Pitfalls

Each of these was hit while writing the recipes.

| Symptom | Cause | Fix |
|---|---|---|
| Every function reports "statement after the return" at its first statement | LLVM 23's `BasicBlock::getTerminator()` assumes a finished block; on an unfinished one it returns `&InstList.back()`, a bogus non-null pointer (the assert is compiled out under `NDEBUG`) | `hasTerminator()`, or `getTerminatorOrNull()` |
| A syntax error test changes text after you add an unrelated token | Bison's `parse.error detailed` lists expected tokens only when there are at most four | update the test; or `%define parse.error custom` and write `Parser::report_syntax_error` |
| The build fails in `Ast.cpp` and `CodeGen.cpp` after you add a node or an enumerator | `std::visit` needs every alternative; `-Wswitch` every enumerator | handle each one: this is the checklist working |
| An IR test fails after an unrelated change | an exact match on a line that gained an attribute, a name or a flag | match loosely: `{{.*}}`, `[[NAME:%.*]]`, `CHECK-LABEL` |
| The verifier: "PHINode should have one entry for each predecessor" | the phi names the block a branch started in, but lowering it (a nested `if`) moved the builder on | take `builder_.GetInsertBlock()` after lowering each branch |
| The verifier: "Basic Block does not have terminator" | a path that falls off a block: an `if` branch, a loop body, a function without a `return` | branch or return at every block's end; `terminated()` before adding |
| A `var` stays in memory at `-O2` | its `alloca` is not in the entry block | allocate with an `IRBuilder` positioned at the entry block's start |
| C++ reads a wrong `bool` from JIT'd code | `i1` crossed the C ABI without `zeroext` | `addParamAttr` / `addRetAttr(llvm::Attribute::ZExt)`, as clang does |
| Bison: "multiple occurrences of $2 with api.value.automove" | automove moves `$n` on its first use | use each `$n` once; copy into a local first if you need it twice |
| A keyword scans as an identifier, or the reverse | Flex takes the longest match, then the earliest rule | keywords before `{IDENTIFIER}` |
| A runtime function works in `fminus` but is not found once FMinus is embedded | it is missing from `RuntimeSymbols.def`, and the tools found it among their own process symbols | add its `FMINUS_RUNTIME_SYMBOL` line |
| A literal like `0.1` differs in its last bit from C++ | rounding through `double` first | keep NumberExpr's spelling; `APFloat::convertFromString` straight to the target type |

## Where to go next

Design notes for features beyond the recipes: what each involves and which LLVM pieces it uses. This code is
not written or verified; `examples/biquad/README.md` suggests an order driven by a real kernel.

**A type checker.** Recipe 3 checks types with LLVM's types, which stops scaling once types multiply or convert
implicitly, or once an error should mention a type that has no LLVM counterpart. Give the tree a semantic pass
before lowering: resolve every name to its declaration, compute every expression's `ast::Type`, store it beside the
node (a side table keyed by node address, or a `type` field on `Expr`), and report every type error there. Lowering
then reads types instead of checking them, and `expect` disappears. Add integers here: `int` as `i32` or `i64`, with
`icmp` and `add`/`sub`/`mul`/`sdiv`, and explicit conversions (`sitofp`, `fptosi`).

**Several results.** A struct or tuple type, `fn lowpass(...) -> Coefficients`. In IR, a function can return a
literal struct (`{float, float, float, float, float}`). At the C boundary, AArch64 returns structs of up to four
floats in registers `s0` to `s3` (a homogeneous floating-point aggregate) and anything larger through a hidden
pointer (`sret`). Clang's IR for the equivalent C declaration shows exactly what to emit. Inside FMinus, where
functions get inlined, any representation works.

**State.** DSP code keeps state between calls: filter memories, phases, smoothed parameters. One design is an
instance struct that the host allocates and the generated code receives as its first parameter: a `ptr`, with its
fields reached by `getelementptr`, and `noalias` so LLVM knows nothing else writes it. A `state` declaration at the
top level would describe its fields, and an `init` function would fill them. Keep the memory the host's, so nothing
allocates on the audio thread.

**Buffers and loops over them.** `fn process(input: buffer, output: buffer, n: int)` lowers to pointers and a loop
over an integer index. This is where the loop vectoriser starts to matter. Mark buffer parameters `noalias` if they
cannot overlap, keep the trip count an integer, and make `fminus file.fm` show a `vector.body`. The smoke test
`tests/lit/smoke/opt-vectorize.ll` is the target shape.

**Fast-math.** `IRBuilder::setFastMathFlags` puts flags on every floating-point instruction it creates: `contract`
lets AArch64 fuse a multiply and an add, `reassoc` allows vector reductions, `afn` approximate functions, `nnan` and
`ninf` drop NaN and infinity handling. Scope the flags per function or per block (`FastMathFlagGuard`), so a
program opts in where it can afford to.

**Better syntax errors.** Bison recovers through the `error` token. A rule like
`statement: error ";"` resynchronises at the next `;`, so one run reports several syntax errors.
`tests/smoke/calc/Calc.y` does exactly this. With `%define parse.error custom`, you write the message yourself and
keep the expected-token list as long as you like.

**Debug information.** `llvm::DIBuilder` attaches source locations to instructions, so lldb steps through FMinus
source in an AOT executable. Every node already carries a `SourceLocation`. For JIT'd code, ORC can register debug
objects with the debugger (`llvm::orc::enableDebuggerSupport`).

**Compile time.** The JIT compiles on the thread that looks a symbol up, and a plugin needs that to be fast and
predictable. Measure the time from `parse` to `lookup` for real programs. The levers are the optimisation level,
LLJIT's concurrent compilation, and caching objects between runs (`llvm::ObjectCache`).

## Reference solutions

[recipes/](recipes/) holds the six recipes as four patches. They apply in order to the commit the documentation was
written against:

```bash
git switch -c try-recipes
git apply docs/recipes/1-builtin-and-operator.patch
git apply docs/recipes/2-comparisons-bool-if.patch
git apply docs/recipes/3-var-assignment-while.patch
git apply docs/recipes/4-runtime-function-and-pass.patch
cmake --workflow --preset dev
```

Each adds its tests to `tests/unit/RecipeTest.cpp`, and the suite then counts 19 tests. Once your own compiler has
moved on, the patches stop applying, but they remain readable diffs of one way to do each step.
