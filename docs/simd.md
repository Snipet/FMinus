# SIMD

How FMinus does SIMD: several floats computed by one instruction. It starts with the hardware and LLVM's model of
vectors. Then the three recipes that gave FMinus its vectors, built with `-Werror` and tested in the dev and asan
presets. They are in the compiler now, so this is also a guide to that code: each recipe names the files it changed,
and walks through them in the order a change moves through the compiler.

| Recipe | Adds | Where |
|---|---|---|
| 7 | `float2`, `float4`, `float8`: OpenCL C's vector types, lane-wise operators, swizzles, masks, reductions | `Ast.h` (the types), `Lexer.l`, `Parser.y`; in `src/ir/`, `Lowering.h` (`Value`), `Types.cpp` (`convert`, `commonType`), `Expressions.cpp` (operators), `Vectors.cpp` (`lowerSelect`, `lowerConstruct`, `lowerBraces`, `lowerSwizzle`, `assignComponents`), `Calls.cpp` (the overloaded builtins, `lowerReduction`); `fminus --run`'s checks |
| 8 | a vector math library: `sinf` on a `float4` is one call to libsystem_m's `_simd_sin_f4`, not four to `sinf` | `HostTarget.cpp` (`optimiseModule`) |
| 9 | `poly float`: a vector as wide as the target's registers (4 floats on NEON, 16 on AVX-512), and rules that keep a program's meaning the same at any width | `Ast.h`, `Parser.y`, `CodeGen.h` (`ir::Options`), `Types.cpp` (`lanes`, `polyMismatch`), `Calls.cpp` (`poly_lanes`, `poly_index`, `poly_load`, `poly_store`), `HostTarget.cpp` (`polyLanes`), the driver (`--poly-lanes`) |

After the recipes: the examples that use them, scalable vectors (SVE, RISC-V V) and how they relate to
`poly float`, the auto-vectoriser that already runs, the pitfalls, and what to build next. Every IR listing, assembly
listing and measurement here was produced on this repository's toolchain (LLVM 23.1.3) on an Apple M5, unless it says
otherwise.

The recipes build on recipes 3 and 4 ([extending.md](extending.md)): masks need `bool` and the comparisons, `?:`
becomes a lane-wise select, and recipe 9's examples loop with `for`.

## The hardware

**NEON** (Arm's Advanced SIMD) is what an Apple M-series core computes vectors with. It has 32 registers of 128 bits,
`v0` to `v31`. A register holds four floats (`v0.4s`, written `.4s` in Apple's assembly syntax), or two in its lower
half (`.2s`, the `d` register). An instruction such as `fmul.4s v0, v0, v1` multiplies four pairs at once. The cores
issue several such instructions per cycle, so independent vector operations also overlap.

**SVE and SME.** Arm's Scalable Vector Extension has registers whose width the hardware chooses, from 128 to 2048
bits. Code is written without knowing it (below). The M5 has no SVE: `sysctl hw.optional.arm` lists no `FEAT_SVE`. It
has SME and SME2, the Scalable Matrix Extension, whose *streaming mode* runs a subset of SVE instructions on a
separate unit, with a streaming vector length of 512 bits (`hw.optional.arm.sme_max_svl_b: 64`). Streaming mode is
entered and left explicitly, and LLVM 23 cannot yet compile all of FMinus's vector operations for it (see "Scalable
vectors").

**x86.** SSE has 16 registers of 128 bits (`xmm`), AVX2 widens them to 256 (`ymm`), and AVX-512 to 512 bits (`zmm`),
32 of them. The same `<16 x float>` operation is one instruction with AVX-512, two with AVX2, and four with SSE.

| Target | Vector registers | floats per register |
|---|---|---|
| Apple M-series (NEON) | 32 × 128 bits | 4 |
| x86-64 with SSE | 16 × 128 bits | 4 |
| x86-64 with AVX2 | 16 × 256 bits | 8 |
| x86-64 with AVX-512 | 32 × 512 bits | 16 |
| Arm with SVE | 32 × 128 to 2048 bits, fixed by the CPU | 4 to 64, unknown when compiling |

## LLVM's model of vectors

LLVM IR has vector types and lane-wise instructions. FMinus's code generator only has to write them, and the backend
maps them onto the target's registers.

- **Types.** `<4 x float>` is a `FixedVectorType`: four floats, a value like any other. `<4 x i1>` is a vector of
  booleans, what comparing two `<4 x float>`s gives: a *lane mask*. `<vscale x 4 x float>` is a `ScalableVectorType`,
  a multiple of four floats fixed by the hardware (SVE).
- **Arithmetic** is the scalar instructions on vector operands: `fadd`, `fsub`, `fmul`, `fdiv`, `frem`, `fneg`, lane
  by lane. `fcmp ogt <4 x float> %a, %b` gives a `<4 x i1>`, and `select <4 x i1> %m, <4 x float> %a, <4 x float> %b`
  picks each lane from `%a` or `%b`.
- **Lanes.** `extractelement` reads one lane and `insertelement` replaces one. `shufflevector` builds a vector from the
  lanes of one or two others, in any order, by a constant list of indices. A *splat* (every lane the same value) is an
  `insertelement` into lane 0 and a `shufflevector` with indices all 0; `IRBuilder::CreateVectorSplat` writes both.
- **Intrinsics** are overloaded on their type: `llvm.fabs.f32` takes a float, `llvm.fabs.v4f32` a `<4 x float>`.
  `Intrinsic::getOrInsertDeclaration(&module, id, {type})` declares the one for a type.
- **Reductions** combine a vector's lanes into one value: `llvm.vector.reduce.fadd`, `fmin`, `fmax`, ... The `fadd`
  reduction is *ordered* (lane 0 first, then 1, 2, ...) unless the call carries the `reassoc` fast-math flag.
- **Legalisation.** The backend makes any vector type fit the target. A type wider than a register is *split*: an
  `<8 x float>` is two `<4 x float>` on NEON, a `<16 x float>` four. A narrower one is *widened* or uses a smaller
  register: a `<2 x float>` lives in NEON's 64-bit `d` registers. An operation the target has no vector instruction for
  is *scalarised*: done lane by lane, often as calls (four calls to `sinf` for an `llvm.sin.v4f32`).

## Four ways to SIMD

1. **The auto-vectorisers**, already running. `optimiseModule` enables LLVM's loop vectoriser and SLP vectoriser with
   the host's cost model. The loop vectoriser needs loops over memory, which FMinus has had since recipe 10's
   pointers: a loop over a buffer, such as `out[i] = in[i] * g`, runs on `<4 x float>`s with no vector type in the
   source. The SLP vectoriser packs independent scalar operations, which small functions rarely have (see "The
   auto-vectorisers").
2. **Fixed-width vector types** (recipe 7): the programmer writes `float4`, and gets exactly four lanes on every
   target. Good for data that is four-wide by nature: four channels, a 4×4 matrix row, a filter's four states.
3. **A vector math library** (recipe 8): `sinf` of a vector becomes one call to a vectorised `sinf`.
4. **Width-agnostic vectors** (recipe 9): the programmer writes `poly float`, and the compiler picks the width for the
   machine it compiles on. Good for data that has no natural width: a buffer of samples, a bank of voices.

Scalable vectors (SVE, RISC-V V) are a fifth way, with the width picked at run time. FMinus has no recipe for them;
"Scalable vectors" shows what one involves.

## Recipe 7: `float2`, `float4` and `float8`

*In the compiler. Tests: `VectorTest.cpp` (`[simd]`), `FrontendTest.cpp`, `tests/lit/fminus/simd.fm`.*

```c
float4 softclip4(float4 x, float drive)
{
    const float4 y = x * drive;              // drive is broadcast to every lane
    return y / (1.0f + fabsf(y));            // fabsf on a float4 is llvm.fabs.v4f32
}

float4 literal(float a) { return (float4)(a, 2, (float2)(3, 4)); }   // the lanes fill in order
float4 reverse(float4 v) { return v.wzyx; }                          // a swizzle
float sum(float4 v) { return reduce_add(v); }                        // ((v.x + v.y) + v.z) + v.w
float4 rectify(float4 x) { return x > 0 ? x : 0; }                   // a mask, then a select per lane
float4 halfwave(float4 x) { return x * (x > 0); }                    // a mask is 0 or 1 per lane
```

### The model: OpenCL C

FMinus follows C, and C has no vector types. OpenCL C, a C dialect for vector hardware, does, and clang implements it,
both as OpenCL C (`clang -x cl`) and, most of it, for C as `ext_vector_type`. So FMinus takes OpenCL C's:

- the names `float2`, `float4`, `float8`;
- the literal `(float4)(a, b, c, d)`, which looks like a cast;
- the lane-wise operators, with a scalar operand broadcast to the vector;
- the swizzles `v.x`, `v.wzyx`, `v.s0` to `v.s7`, readable and assignable;
- comparisons that give a mask, and `?:` that selects lane by lane;
- the math functions, overloaded on vectors.

The alternatives were weaker fits. GCC's `vector_size` attribute has no swizzles, and builds vectors from braces.
C++'s `std::simd` is a library, not syntax. And clang implements OpenCL C's model, which makes clang the reference for
what FMinus should do: `clang -x cl -O0 -S -emit-llvm` on the same function shows the IR to aim for. FMinus's error
messages are clang's: its C-mode ones for conversions, components and assignments (clang adds "(vector of 4 'float'
values)" after a type, FMinus doesn't), and its OpenCL ones for literals ("too few elements in vector initialization
(expected 4 elements, have 3)").

One thing does not carry over to C. In C, even with `ext_vector_type`, `(float4)(a, b, c, d)` is not a literal: it
is a cast of the comma expression `(a, b, c, d)`, so `d` in every lane. Clang only warns that "left operand of comma
operator has no effect", and a C program printing the result gets `4 4 4 4` for `(float4)(1, 2, 3, 4)`. C's vector
literal is a compound literal, `(float4){1, 2, 3, 4}`, which gives `1 2 3 4`. So FMinus has both: OpenCL C's
parentheses, and C's braces, with C's meaning ("Literals", below). A function that uses only braces is C as well.

Two of OpenCL C's rules are left out. OpenCL C's comparisons of float vectors give `int4` (0 or -1 per lane), which
FMinus can't express: it has `int` since recipe 10, but no integer vectors. FMinus gives a lane mask instead, which
converts to 0 or 1 per lane as a float. And OpenCL C allows three-component vectors (`float3`, stored as four);
FMinus doesn't.

### Types and masks

`Ast.h` gets the vector types and the lane masks their comparisons give, and helpers that answer the questions the
rules ask:

```cpp
    // float2, float4 and float8 are OpenCL C's vector types: that many floats side by side, computed on together. A
    // float4 fills one 128-bit NEON register; a float8 is two. Comparing two of them gives a lane mask, a bool2, bool4
    // or bool8, which a program cannot declare (as in OpenCL C) but can select with or use as 0s and 1s.
    enum class Type
    {
        Float,
        Bool,
        Float2,
        Float4,
        Float8,
        Bool2,
        Bool4,
        Bool8,
    };

    inline unsigned lanes(Type type);                    // 1 for a scalar, 2, 4 or 8
    inline bool isVector(Type type);                     // lanes(type) > 1
    inline bool isMask(Type type);                       // bool2, bool4, bool8
    inline std::optional<Type> vectorOf(unsigned count); // 2 -> float2, 4 -> float4, 8 -> float8
    inline Type maskOf(Type type);                       // what comparing two values of `type` gives
    inline Type arithmeticOf(Type type);                 // bool -> float, bool4 -> float4: what a value computes as
```

The lexer gives the three names one token with the type as its value, so the grammar has one rule for all of them:

```lex
"float2"                    { return Parser::make_VECTOR_TYPE(fminus::ast::Type::Float2, yyextra->where); }
"float4"                    { return Parser::make_VECTOR_TYPE(fminus::ast::Type::Float4, yyextra->where); }
"float8"                    { return Parser::make_VECTOR_TYPE(fminus::ast::Type::Float8, yyextra->where); }
...
"."                         { return Parser::make_DOT(yyextra->where); }
```

`.` doesn't take `.5` apart: Flex takes the longest match, and `.5` is a number. `typeOf` maps each type to LLVM:

```cpp
                case ast::Type::Float2:
                case ast::Type::Float4:
                case ast::Type::Float8:
                    return llvm::FixedVectorType::get(builder_.getFloatTy(), ast::lanes(type));
                case ast::Type::Bool2:
                case ast::Type::Bool4:
                case ast::Type::Bool8:
                    return llvm::FixedVectorType::get(builder_.getInt1Ty(), ast::lanes(type));
```

### Every value carries its FMinus type

Recipe 3 reads a value's FMinus type off its LLVM type: an `i1` is a `bool`, a `float` a `float`. With vectors that
still works, since `<4 x i1>` can only be a `bool4`. But it is a hack that breaks the moment two FMinus types share one
LLVM type, and recipe 9's `poly float` is exactly that: `<4 x float>` on NEON, like `float4`. The type rules must
never depend on which LLVM type a value happened to get. So recipe 7 makes every lowered expression carry its FMinus
type:

```cpp
            // A lowered expression: its IR and its FMinus type. The rules read the FMinus type, never the IR's, since
            // two FMinus types may share an LLVM type: one whose width the target decides would be <4 x float> on
            // NEON, as float4 is.
            struct Value
            {
                llvm::Value* ir = nullptr;
                ast::Type type = ast::Type::Float;
            };
```

`lower(const ast::Expr&)` returns a `Value`, and so does each lowering function. Every rule below reads `value.type`.
This is the "type checker" that extending.md's design notes call for, in its smallest form: types are computed while
lowering, not in a pass of their own. Recipe 10 added `int` and pointers the same way; with C's other integer
types, a separate semantic pass becomes worth it.
`poison(type)` now returns a `Value`, and code that needs only the IR takes `.ir`.

### The grammar

```yacc
%token <ast::Type> VECTOR_TYPE "vector type"
%token DOT "."
...
%precedence UNARY
%precedence "."
...
type
    : "float"                               { $$ = ast::Type::Float; }
    | "bool"                                { $$ = ast::Type::Bool; }
    | VECTOR_TYPE                           { $$ = $1; }
    ;
...
    | "(" VECTOR_TYPE ")" "(" args ")"      { $$ = ast::makeExpr(at(@1), ast::ConstructExpr{$2, $5}); }
    | expr "." IDENTIFIER                   { $$ = ast::makeExpr(at(@2), ast::SwizzleExpr{$1, $3}); }
```

- **The literal doesn't conflict with `( expr )`.** After `(`, a vector type can't start an expression, so Bison
  knows which rule it is in. A cast with a scalar type would conflict, which is one reason FMinus has no casts yet.
- **The swizzle binds tightest**, as C's postfix operators do: `-a.yx` is `-(a.yx)`, and `a * b.x` is `a * (b.x)`.
  `"."` gets the highest precedence. It must be `%precedence`, not `%left`: no conflict ever compares `"."` with
  itself (the rule ends in an identifier), and Bison warns "useless associativity for ".", use %precedence".
- **Two nodes** join `Expr::node`:

```cpp
    // `(float4)(a, b, c, d)`: OpenCL C's vector literal. One scalar is broadcast to every lane; otherwise the
    // arguments' lanes (one for a scalar, all of a vector's) fill the vector in order, and must fill it exactly.
    struct ConstructExpr
    {
        Type type = Type::Float4;
        std::vector<ExprPtr> args;
    };

    // `v.x`, `v.wzyx`, `v.s07`: OpenCL C's components. x, y, z, w name lanes 0 to 3, and s followed by hex digits names
    // any lane. One component is a float, two, four or eight a vector of them; a variable's components can be assigned.
    struct SwizzleExpr
    {
        ExprPtr base;
        std::string components;
    };
```

They dump as `(float4 a 2 3)` and `(. v wzyx)`. A vector type is a new way to start a function, so Bison's top-level
syntax error gains it: "expecting end of file or float or bool or vector type".

### Conversions

`convert(value, type, location, use)` is recipe 3's helper, grown. It takes the `Value`, and a `Use` that says where
the value meets its type, so a failure gets clang's message for that place:

```cpp
            // C's implicit conversions, and OpenCL C's for vectors. A bool is 0 or 1 as a float, and a float is
            // `x != 0` as a bool (so a NaN is true). A scalar becomes a vector by being broadcast to every lane, and a
            // lane mask is 0 or 1 per lane as the float vector of its size. Nothing else converts from a vector: that
            // is an error, reported for `use`.
            llvm::Value* convert(Value value, ast::Type type, SourceLocation location, Use use)
            {
                if (value.type == type)
                    return value.ir;
                if (!ast::isVector(value.type))
                {
                    if (ast::isVector(type) && !ast::isMask(type))
                    {
                        llvm::Value* scalar = convert(value, ast::Type::Float, location, use);
                        return builder_.CreateVectorSplat(ast::lanes(type), scalar, "splat");
                    }
                    if (type == ast::Type::Bool)
                        return builder_.CreateFCmpUNE(value.ir, llvm::ConstantFP::get(value.ir->getType(), 0.0),
                                                      "tobool");
                    if (type == ast::Type::Float)
                        return builder_.CreateUIToFP(value.ir, typeOf(type), "conv");
                }
                else if (ast::isMask(value.type) && type == ast::arithmeticOf(value.type))
                {
                    return builder_.CreateUIToFP(value.ir, typeOf(type), "conv");
                }
                incompatible(location, use, value.type, type);
                return poison(type).ir;
            }
```

(This is `convert` as recipe 7 wrote it. Recipe 9 adds a splat to `poly bool` and uses the code generator's own
`lanes(type)`, which knows a poly type's width, and recipe 10 adds `int`, `void` and pointers; `Types.cpp` has that
version. The same goes for `commonType` below, which recipe 9 teaches to refuse a poly type meeting a fixed vector,
and recipe 10 to meet two `int`s as an `int`. Since recipe 10, too, a switch over a type is a switch over its
`kind`: `switch (type.kind)`, with `case ast::Kind::Float4:`.)

| From | To | How |
|---|---|---|
| `float` or `bool` | `float4` | a splat (a `bool` is 0 or 1 first) |
| `bool4` (a mask) | `float4` | `uitofp`: 0 or 1 per lane |
| `float4` | `float`, `bool`, `float8`, ... | an error, worded for the `Use` |
| `float4` | the condition of an `if`, `while` or `for` | "statement requires expression of scalar type ('float4' invalid)" |
| `float4` | an operand of `&&`, `\|\|` or `!` | "a logical operator needs scalar operands, not 'float4'" |

The `Use` values are `Initializing`, `Assigning`, `Returning`, `Passing`, `Condition` and `Logical`. Each call site
passes its own, and the location of the construct, so `float s = v;` reports "initializing 'float' with an expression
of incompatible type 'float4'" at the declared name. (Clang points at the initialiser; FMinus's declarations carry
their name's location.)

### Operators and broadcasting

OpenCL C's usual arithmetic conversions decide the type two operands meet in:

```cpp
            // The type two operands meet in, as OpenCL C's usual arithmetic conversions decide it: float, or the
            // vector type when either one is a vector (the other is broadcast to it); a bool and a lane mask take part
            // as floats. Vectors of two sizes do not meet: clang's error, and false.
            bool commonType(SourceLocation location, ast::Type a, ast::Type b, ast::Type& type)
            {
                const ast::Type ta = ast::arithmeticOf(a);
                const ast::Type tb = ast::arithmeticOf(b);
                if (ast::isVector(ta) && ast::isVector(tb) && ta != tb)
                {
                    error(location, "cannot convert between vector values of different size ("
                                        + quoted(ast::typeName(a)) + " and " + quoted(ast::typeName(b)) + ")");
                    return false;
                }
                type = ast::isVector(ta) ? ta : tb;
                return true;
            }
```

A binary operator converts both operands to that type and applies `arithmetic` unchanged: `CreateFMul` of two
`<4 x float>` is a `<4 x float>` multiply. A comparison's result is `maskOf(type)`, a `bool4` for `float4`s:

```cpp
                            llvm::Value* result =
                                arithmetic(binary.op, convert(lhs, type, expr.location, Use::Initializing),
                                           convert(rhs, type, expr.location, Use::Initializing));
                            return Value{result, isComparison(binary.op) ? ast::maskOf(type) : type};
```

Unary `-` and `+` work in `arithmeticOf(operand.type)`, so `-v` negates every lane, and `-(x > 0)` negates the mask's
0s and 1s (into -0 and -1). `!` stays scalar: a vector is "a logical operator needs scalar operands". Compound
assignments go through the same rules (`v *= 2`), in the helper `compound`.

### Masks and `?:`

A scalar `?:` runs one branch (recipe 3). With a vector condition there is no single branch to run: lane 0 may want
`a` and lane 1 `b`. OpenCL C defines it lane by lane, so both branches are computed and a `select` picks each lane.
`lowerConditional` sends a vector condition to `lowerSelect`:

```cpp
            // c ? a : b with a vector c chooses lane by lane, as in OpenCL C: there are no branches, so both sides run,
            // and a select picks each lane from one or the other. A float vector condition is `!= 0` per lane, as a
            // scalar one is. The result is a float vector of the condition's size; scalar branches are broadcast.
            Value lowerSelect(SourceLocation location, Value test, const ast::ConditionalExpr& conditional)
            {
                const ast::Type mask = ast::maskOf(ast::arithmeticOf(test.type));
                llvm::Value* condition = test.ir;
                if (!ast::isMask(test.type))
                    condition = builder_.CreateFCmpUNE(condition, llvm::Constant::getNullValue(typeOf(test.type)),
                                                       "tobool");
                const Value thenValue = lower(*conditional.thenValue);
                const Value elseValue = lower(*conditional.elseValue);
                const ast::Type type = ast::arithmeticOf(mask);
                ast::Type branches = type;
                if (!commonType(location, thenValue.type, elseValue.type, branches))
                    return poison(type);
                if (ast::isVector(branches) && branches != type)
                {
                    error(location, "vector condition type " + quoted(ast::typeName(mask)) + " and result type "
                                        + quoted(ast::typeName(branches)) + " do not have the same number of elements");
                    return poison(type);
                }
                return {builder_.CreateSelect(condition, convert(thenValue, type, location, Use::Initializing),
                                              convert(elseValue, type, location, Use::Initializing), "select"),
                        type};
            }
```

Both branches run, so `v > 0 ? (a = 1) : (a = 2)` assigns twice, as in OpenCL C. That is the price of lane-wise
choice, and why the scalar form keeps its blocks. A mask in arithmetic is the other way to choose: `x * (x > 0)` keeps
the positive lanes and zeroes the others, without a select. Masks can't be declared, so a mask is used where it is
made. Recipe 9's `poly bool` is a mask a program can declare.

### Literals

`lowerConstruct` takes one scalar and broadcasts it, or concatenates the lanes of its arguments, which must fill the
vector exactly. Each argument converts to its arithmetic type first, so a mask contributes 0s and 1s:

```cpp
            // (float4)(a, b, c, d), OpenCL C's vector literal: one scalar is broadcast; otherwise the arguments' lanes,
            // in order, must fill the vector exactly.
            Value lowerConstruct(SourceLocation location, const ast::ConstructExpr& construct)
            {
                const unsigned lanes = ast::lanes(construct.type);
                std::vector<Value> args;
                for (const ast::ExprPtr& arg : construct.args)
                    args.push_back(lower(*arg));
                if (args.size() == 1 && !ast::isVector(args[0].type))
                    return {convert(args[0], construct.type, location, Use::Initializing), construct.type};

                std::vector<llvm::Value*> elements;
                for (const Value& arg : args)
                {
                    const ast::Type type = ast::arithmeticOf(arg.type);
                    llvm::Value* floats = convert(arg, type, location, Use::Initializing);
                    if (!ast::isVector(type))
                        elements.push_back(floats);
                    for (unsigned i = 0; ast::isVector(type) && i < ast::lanes(type); ++i)
                        elements.push_back(builder_.CreateExtractElement(floats, i));
                }
                ...   // clang's "too few elements in vector initialization (expected 4 elements, have 3)",
                      // "excess elements in vector initializer"
                llvm::Value* vector = llvm::PoisonValue::get(typeOf(construct.type));
                for (unsigned i = 0; i < lanes; ++i)
                    vector = builder_.CreateInsertElement(vector, elements[i], i);
                return {vector, construct.type};
            }
```

The chain of `insertelement`s looks wasteful at `-O0`. At `-O2`, constant lanes fold into the vector:

```llvm
; float4 literal(float a) { return (float4)(a, 2, (float2)(3, 4)); }     fminus -O2
  %0 = insertelement <4 x float> <float poison, float 2.000000e+00, float 3.000000e+00, float 4.000000e+00>, float %a, i64 0
  ret <4 x float> %0
```

**C's form, in braces.** `(float4){a, b, c, d}` is C's compound literal, which clang's `ext_vector_type` gives vectors.
It means what C says, which is not what the parentheses mean:

| | OpenCL C's `(float4)(...)` | C's `(float4){...}` |
|---|---|---|
| one scalar, `(x)` / `{x}` | `x` in every lane | `x`, then three zeros |
| fewer elements than lanes | an error | the rest are 0; `{}` is all zeros |
| a vector element | its lanes fill in, `(float4)(v2, 1, 2)` | an error, except a single one of the literal's own type, `{v}`, which is `v` |
| more elements than lanes | an error | an error (C only warns, and drops them) |
| a trailing comma | a syntax error | allowed, `{a, b,}` |
| `poly float` | `(poly float)(x)`, a broadcast | none: a program can't know how many lanes the zeros fill |

The grammar has a rule for each form, the braces with and without the trailing comma, and a flag on the node tells
them apart (`ConstructExpr::braces`). It needs no precedence: after `(float4)`, a `(` or a `{` decides. The lowering is
its own function, because almost nothing is shared:

```cpp
    Lowering::Value Lowering::lowerBraces(SourceLocation location, const ast::ConstructExpr& construct,
                                          const std::vector<Value>& args)
    {
        if (args.size() > lanes(construct.type))
        {
            error(location, "excess elements in vector initializer");
            return poison(construct.type);
        }
        if (args.size() == 1 && args[0].type == construct.type)
            return args[0];
        llvm::Value* vector = llvm::Constant::getNullValue(typeOf(construct.type));
        for (std::size_t i = 0; i < args.size(); ++i)
        {
            if (ast::isVector(args[i].type))
            {
                // clang's: the first element could initialise the whole vector, any other only a lane
                incompatible(location, Use::Initializing, args[i].type, i == 0 ? construct.type : ast::Type::Float);
                return poison(construct.type);
            }
            llvm::Value* element = convert(args[i], ast::Type::Float, location, Use::Initializing);
            vector = builder_.CreateInsertElement(vector, element, static_cast<std::uint64_t>(i));
        }
        return {vector, construct.type};
    }
```

It starts from a zero vector instead of poison, and the zeros fold into the constant:

```llvm
; float4 braces(float a) { return (float4){a, 2}; }     fminus -O2
  %0 = insertelement <4 x float> <float poison, float 2.000000e+00, float 0.000000e+00, float 0.000000e+00>, float %a, i64 0
```

A function that builds its vectors with braces only is C as well. `examples/biquad/biquad.fm`'s
`lowpass_coefficients` does, and compiles with `clang -std=c17 -Wall -Wextra -Werror` given
`typedef float float4 __attribute__((ext_vector_type(4)));` (the module around it, with its parameters, is FMinus's
own).

### Swizzles

`components(location, name, vector)` turns a component name into lane indices: `x`, `y`, `z`, `w` are 0 to 3, and `s`
followed by hex digits names any lane (`s0` to `s7`, `s0123`). The two styles don't mix (`xs1` is illegal). It reports
clang's errors: "illegal vector component name 'q'", "vector component access exceeds type 'float2'". The result type
is a `float` for one component and `vectorOf(count)` otherwise; three components have no type (FMinus has no
`float3`), and that is an error.

Reading is one instruction. One lane is an `extractelement`; several, in any order and repeated or not, are a
`shufflevector`, whose mask is the list of indices:

```cpp
                if (lanes->size() == 1)
                    return {builder_.CreateExtractElement(base.ir, static_cast<std::uint64_t>((*lanes)[0]),
                                                          swizzle.components),
                            *type};
                return {builder_.CreateShuffleVector(base.ir, *lanes, swizzle.components), *type};
```

```llvm
; float4 reverse(float4 v) { return v.wzyx; }     fminus -O2 (the instruction is named after the swizzle)
  %wzyx = shufflevector <4 x float> %v, <4 x float> poison, <4 x i32> <i32 3, i32 2, i32 1, i32 0>
```

Writing goes through recipe 10's lvalues (`Memory.cpp`; extending.md, "Lvalues: one path for every store"). This
recipe wrote it as `assignComponents`; recipe 10 folded it into `lvalue` and `write`, so components of anything
assignable can be assigned: a variable's, an element's (`frames[i].yz *= 2`), other components' (`v.xy.x = 1`). The
target is checked as any assignment's is (declared, not `const`), and so are the components. They must be distinct:
`v.xx = e` would write lane 0 twice, which is clang's "vector is not assignable (contains duplicate components)".
`write` loads the whole vector, replaces the named lanes with `insertelement`s, and stores it back. A compound
assignment (`v.zw *= 2`) first reads the components with the same `shufflevector`. At `-O2`, the load and store
disappear with mem2reg, and the inserts become shuffles:

```llvm
; float4 set(float4 v) { v.x = 10; v.zw *= 2; return v; }     fminus -O2
  %0 = insertelement <4 x float> %v, float 1.000000e+01, i64 0
  %1 = fmul <4 x float> %v, <float poison, float poison, float 2.000000e+00, float 2.000000e+00>
  %2 = shufflevector <4 x float> %1, <4 x float> poison, <4 x i32> <i32 2, i32 3, i32 poison, i32 poison>
  %3 = shufflevector <4 x float> %0, <4 x float> %2, <4 x i32> <i32 0, i32 1, i32 4, i32 5>
```

Only an lvalue's components can be assigned. `f().x = 1` is "expression is not assignable", as in clang.

### Builtins on vectors

OpenCL C's math functions are overloaded: `fabsf` of a `float4` is a `float4`. The builtins already lower to LLVM
intrinsics, which are overloaded on their type, so `lowerCall` only has to pick the type: the common type of the
arguments, with scalars broadcast. `fminf(v, 1)` is `llvm.minnum.v4f32(v, splat 1)`:

```cpp
                    // Overloaded on its arguments, as OpenCL C's math functions are: with a vector argument it is the
                    // vector intrinsic (llvm.fabs.v4f32), and scalar arguments beside it are broadcast.
                    ast::Type type = ast::Type::Float;
                    for (const Value& arg : args)
                    {
                        if (!commonType(location, type, arg.type, type))
                            return poison(ast::Type::Float);
                    }
                    for (const Value& arg : args)
                        irArgs.push_back(convert(arg, type, location, Use::Passing));
                    llvm::Function* intrinsic =
                        llvm::Intrinsic::getOrInsertDeclaration(&module_, builtin->intrinsic, {typeOf(type)});
                    return {builder_.CreateCall(intrinsic, irArgs), type};
```

`fabsf`, `sqrtf`, `floorf`, `fminf` and `fmaxf` become one NEON instruction each (`fabs.4s`, `fsqrt.4s`, `frintm.4s`,
`fminnm.4s`, `fmaxnm.4s`). `sinf`, `cosf`, `expf`, `logf` and `tanhf` have no instruction, so the backend scalarises
them: four calls to `sinf`, with the lanes moved in and out around each. Recipe 8 replaces those with one call.

The program's own functions are not overloaded (nor recipe 5's runtime functions). Their parameter types are what
they declare.

### Reductions

Three builtins reduce a float vector to one float: `reduce_add`, `reduce_min` and `reduce_max`. They are FMinus's
names (OpenCL C has no reductions for vector types).

```cpp
            // reduce_add(v), reduce_min(v), reduce_max(v): a float vector to one float. The sum is ordered, lane 0
            // first, as a loop would add them (llvm.vector.reduce.fadd without the reassoc flag), so it is exact and
            // reproducible; reassociating it into a tree is fast-math's to allow. min and max ignore NaN lanes.
            ...
                if (call.callee == "reduce_add")
                    return {builder_.CreateFAddReduce(llvm::ConstantFP::getNegativeZero(builder_.getFloatTy()),
                                                      vector.ir),
                            ast::Type::Float};
                if (call.callee == "reduce_min")
                    return {builder_.CreateFPMinReduce(vector.ir), ast::Type::Float};
                return {builder_.CreateFPMaxReduce(vector.ir), ast::Type::Float};
```

- **The sum is ordered.** `reduce_add(v)` is `((v.x + v.y) + v.z) + v.w`, bit for bit, the same as the scalar code.
  A tree, `(v.x + v.y) + (v.z + v.w)`, is faster but rounds differently, so it needs the `reassoc` flag, which FMinus
  never sets. On NEON the ordered sum is `faddp` for the first pair, then two `fadd`s.
- **The start value is -0.0**, the identity of addition: `-0.0 + x` is `x` for every `x`, including `+0.0`. With
  `+0.0`, a sum of `-0.0`s would come out `+0.0`.
- **`reduce_min` and `reduce_max` ignore NaN lanes**, as `fminf` and `fmaxf` ignore a NaN argument
  (`llvm.vector.reduce.fmin`/`fmax` have `minnum`/`maxnum` semantics). They are `fminnmv.4s` and `fmaxnmv.4s`.

The names are refused as function names ("'reduce_add' is a builtin and cannot be redefined"), as a builtin's are.

### What LLVM makes of it

`softclip4` at `-O0` is recipe 3's shape with vector types: slots, loads, stores, and a splat of `drive`.

```llvm
; fminus softclip4.fm -O0
  %drive2 = load float, ptr %drive.addr, align 4
  %splat.splatinsert = insertelement <4 x float> poison, float %drive2, i64 0
  %splat.splat = shufflevector <4 x float> %splat.splatinsert, <4 x float> poison, <4 x i32> zeroinitializer
  %0 = fmul <4 x float> %x1, %splat.splat
  store <4 x float> %0, ptr %y, align 16
  ...
  %1 = call <4 x float> @llvm.fabs.v4f32(<4 x float> %y4)
  %2 = fadd <4 x float> splat (float 1.000000e+00), %1
  %3 = fdiv <4 x float> %y3, %2

; fminus softclip4.fm -O2
  %splat.splatinsert = insertelement <4 x float> poison, float %drive, i64 0
  %splat.splat = shufflevector <4 x float> %splat.splatinsert, <4 x float> poison, <4 x i32> zeroinitializer
  %0 = fmul <4 x float> %x, %splat.splat
  %1 = tail call <4 x float> @llvm.fabs.v4f32(<4 x float> %0)
  %2 = fadd <4 x float> %1, splat (float 1.000000e+00)
  %3 = fdiv <4 x float> %0, %2
  ret <4 x float> %3
```

```asm
_softclip4:                         ; fminus softclip4.fm --emit asm -o -
	fmul.4s	v0, v0, v1[0]           ; the splat folds into a by-element multiply
	fabs.4s	v1, v0
	fmov.4s	v2, #1.00000000
	fadd.4s	v1, v1, v2
	fdiv.4s	v0, v0, v1
	ret

_rectify:                           ; x > 0 ? x : 0
	fcmgt.4s	v1, v0, #0.0        ; the mask: all ones or all zeros per lane
	and.16b	v0, v1, v0              ; select against zero is an and
	ret

_sum:                               ; reduce_add(v), in order
	mov	s1, v0[3]
	mov	s2, v0[2]
	faddp.2s	s0, v0              ; v.x + v.y
	fadd	s0, s0, s2              ; + v.z
	fadd	s0, s0, s1              ; + v.w
	ret
```

`float8` is legal LLVM and NEON has no 256-bit registers, so the backend splits it: `a * b + 1` on two `float8`s is
two `fmul.4s` and two `fadd.4s`, on `v0:v1` and `v2:v3`. `float2` uses the lower half of a register: `fmul.2s`.

### At the C boundary

FMinus functions are C functions, and a C++ caller has to agree on how a vector is passed. Clang's IR for the C
declarations shows what the C ABI expects on arm64 (`clang -O2 -S -emit-llvm`, with `ext_vector_type` typedefs):

```llvm
define <2 x float> @two(<2 x float> noundef %0, float noundef %1)
define <4 x float> @four(<4 x float> noundef %0, float noundef %1)
define void @eight(ptr ... sret(<8 x float>) align 16 ... %0, ptr noundef readonly captures(none) %1, float noundef %2)
define void @sixteen(ptr ... sret(<16 x float>) align 16 ... %0, ptr noundef readonly captures(none) %1, ...)
```

| Type | Clang (the C ABI) | FMinus | Callable from C++ |
|---|---|---|---|
| `float2` | `<2 x float>` in `v0`, direct | the same | yes |
| `float4` | `<4 x float>` in `q0`, direct | the same | yes |
| `float8` | returned through a hidden pointer (`sret`), passed by pointer | `<8 x float>` direct: in `v0:v1` | **no** |

So C++ calls `float2` and `float4` functions as `float4 (*)(float4)`, with clang's `ext_vector_type`. A `float8`
function is fine inside FMinus (calls between FMinus functions agree with each other), but its IR doesn't match the C
ABI. To export one, lower it as clang does: a `ptr sret(<8 x float>)` first parameter, the vector parameters as
`ptr`s, a `void` return. Recipe 3 met the same question for `bool` (`zeroext`); every type that crosses the boundary
asks it.

`fminus --run` crosses the boundary too: it calls the JIT'd function through a C++ pointer type, and its command line
has floats only. So it refuses a vector parameter ("--run: 'g' takes a parameter of type 'float4'; --run passes floats
only") and a vector result ("--run: 'f' returns 'float4'; --run prints floats, bools and ints only"). The second
comes from the
driver's `switch` over the result type (`tools/fminus/main.cpp`), which has no `default:`: when this recipe added
`Float2` to `Bool8`, `-Wswitch` stopped the build there until they were handled, and recipe 9's poly types did the
same. An executable's `main` refuses them the same way (programs.md).

### Tests

C++ gets FMinus's vector types from clang's extension, so the tests call JIT'd vector functions directly:

```cpp
    // FMinus's float2 and float4 are clang's ext_vector_type at the C boundary: <2 x float> and <4 x float>, in one
    // register. (float8 is not: clang passes it through memory, so these tests keep float8 inside FMinus.)
    using float2 = float __attribute__((ext_vector_type(2)));
    using float4 = float __attribute__((ext_vector_type(4)));
    ...
    CHECK(lanesOf(lookup<float4(float4)>(*jit, "reverse")(v)) == std::array{4.0f, 3.0f, 2.0f, 1.0f});
```

`VectorTest.cpp` has four test cases:

- literals, broadcasts, swizzles read and written, an uninitialised vector being zero;
- C's brace literal: full, short, empty, one element, a copy, a mask element, its errors;
- arithmetic with broadcasting, `%=`, builtins, the ordered sum (compared exactly with `((a + b) + c) + d`),
  `reduce_min` and `reduce_max` past a NaN lane, masks in `?:` and in arithmetic, and a `float8` used inside FMinus;
- every error above, with its location.

`FrontendTest.cpp` checks the dumps (a brace literal is `(float4 {a 2})`), that a swizzle binds tighter than unary
minus, and that `(poly float){...}` is a syntax error. `tests/lit/fminus/simd.fm` checks the IR (the splat,
`llvm.fabs.v4f32`, a brace literal's zeros, the shuffle, the ordered reduction, a select and no branch) and, on
arm64, the assembly (`fmul.4s v0, v0, v1[0]`, `fcmgt.4s`, two `fmul.4s` for a `float8`), using lit's
`%if arm64 %{ ... %}` so the IR checks still run elsewhere.

### The errors

| Program | Error (clang's wording where clang has one) |
|---|---|
| `float s = v;` | initializing 'float' with an expression of incompatible type 'float4' |
| `x = v;` | assigning to 'float' from incompatible type 'float4' |
| `return b;` (a `float8` from a `float4` function) | returning 'float8' from a function with incompatible result type 'float4' |
| `g(v)` (`g` takes a float) | passing 'float4' to parameter of incompatible type 'float' |
| `a + b` (a `float4` and a `float8`) | cannot convert between vector values of different size ('float4' and 'float8') |
| `v.q` | illegal vector component name 'q' |
| `(float2)(1, 2).z` | vector component access exceeds type 'float2' |
| `x.y` (a float) | member reference base type 'float' is not a structure or union |
| `v.xxy` | 3 components have no type: FMinus has float2, float4 and float8 |
| `v.xx = e` | vector is not assignable (contains duplicate components) |
| `k.x = 1` (`const float4 k`) | cannot assign to variable 'k' with const-qualified type 'const float4' |
| `(float4)(1, 2, 3)` | too few elements in vector initialization (expected 4 elements, have 3) |
| `(float4)(1, 2, 3, 4, 5)` | excess elements in vector initializer |
| `(float4){1, 2, 3, 4, 5}` | excess elements in vector initializer (C only warns) |
| `(float4){p, 1}`, `(float4){1, p}` (`p` a `float2`) | initializing 'float4' with an expression of incompatible type 'float2'; initializing 'float' ... |
| `reduce_add(x)` (a float) | the argument of 'reduce_add' must be a float vector, not 'float' |
| `if (v)` | statement requires expression of scalar type ('float4' invalid) |
| `v && x` | a logical operator needs scalar operands, not 'float4' |
| `b > 0 ? v : v` (`b` a `float8`) | vector condition type 'bool8' and result type 'float4' do not have the same number of elements |

## Recipe 8: a vector math library

*In the compiler. Tests: `VectorTest.cpp` (`[simd]`), `tests/lit/fminus/veclib.fm`.*

After recipe 7, `sinf` of a `float4` is `llvm.sin.v4f32`, and the backend scalarises it:

```asm
_sines:                             ; float4 sines(float4 x) { return sinf(x); }, before recipe 8
	...
	mov	s0, v0[1]
	blr	x19                     ; sinf, through the GOT
	...                         ; four calls in all, with each result inserted back into a vector
```

A vector math library has `sinf` for four floats at once. LLVM knows several, and maps its intrinsics to them through
the target library information. This recipe turns that on.

### The pieces

- **`TargetLibraryInfoImpl`** describes the C library of a target: which functions exist, and which have vector
  versions. `addVectorizableFunctionsFromVecLib(library, triple)` loads one library's table of vector versions, from
  `llvm/Analysis/VecFuncs.def`. An entry maps a scalar name and a width to a vector function:
  `TLI_DEFINE_VECFUNC("llvm.sin.f32", "_simd_sin_f4", FIXED(4), "_ZGV_LLVM_N4v")`.
- **`TargetLibraryAnalysis`** hands it to the passes. The loop vectoriser consults it when it vectorises a loop that
  calls `sinf`: with a mapping, a loop of `sinf` calls vectorises into calls of the vector function.
- **`ReplaceWithVeclib`** (`llvm/CodeGen/ReplaceWithVeclib.h`) is a function pass that replaces a call to a vector
  intrinsic, such as `llvm.sin.v4f32`, with a call to the vector function the table maps it to, when one has exactly
  that width. That is what recipe 7's `sinf(v)` needs.

The libraries LLVM 23 knows are the `llvm::VectorLibrary` enum, in `llvm/IR/SystemLibraries.h`: `Accelerate`,
`DarwinLibSystemM`, `LIBMVEC` (glibc), `MASSV`, `SVML` (Intel), `SLEEFGNUABI`, `ArmPL` and `AMDLIBM`. This is clang's
`-fveclib=` option.

### Which library

Apple systems have two, and the obvious one is the wrong one here.

**Accelerate** maps `sinf` to `vsinf`, and also maps `fabsf` and `sqrtf`, intrinsics included: `llvm.fabs.f32` to
`vfabsf`, `llvm.sqrt.f32` to `vsqrtf`. `ReplaceWithVeclib` replaces every intrinsic call that has a mapping, without
asking whether the target has an instruction for it. So with Accelerate, recipe 7's `softclip4` changes for the worse:

```llvm
; fminus softclip4.fm -O2, with VectorLibrary::Accelerate
  %1 = call <4 x float> @vfabsf(<4 x float> %0)       ; was llvm.fabs.v4f32: one fabs.4s instruction
```

**libsystem_m** (`DarwinLibSystemM`) maps only what has no instruction: `sinf`, `cosf`, `tanf`, `expf`, `powf`,
`tanhf`, the inverse and hyperbolic functions, `cbrtf`, `erff`. Its functions are `_simd_sin_f4` and friends, the ones
behind `<simd/math.h>`. They live in libSystem, which every process and every executable links already, so nothing
extra is linked for the JIT or for an executable. (libsystem_m's table has no `logf`; Accelerate's maps it to
`vlogf`.)

Both libraries were measured against libm's scalar functions here (an M5, `clang -O2`, `float4` arguments; the worst
error in ulps against the exact result, computed in double, over 4 million points; throughput over independent calls):

| Function | libm `sinf` etc. | libsystem_m `_simd_*_f4` | Accelerate `v*f` | Throughput, libm → libsystem_m |
|---|---|---|---|---|
| `sinf`, [-100, 100] | 0.56 ulp | 2.21 ulp | 2.21 ulp | 1.21 → 0.34 ns a float (3.6×) |
| `expf`, [-80, 80] | 0.51 ulp | 1.74 ulp | 1.74 ulp | 0.98 → 0.40 ns (2.5×) |
| `tanhf`, [-10, 10] | 0.78 ulp | 2.71 ulp | 2.71 ulp | 1.26 → 0.50 ns (2.5×) |

Accelerate's and libsystem_m's results are identical, so they share an implementation; libsystem_m is the one without
the unwanted mappings. The vector functions are 2.5 to 3.6 times faster and up to about 3 ulp from the exact result,
where libm is within 1. For audio that is a good trade, but it is a trade: `sinf(v).x` is no longer bit-identical to
`sinf(v.x)`. In a dependent chain (each call waiting for the last), the two take about the same time; the gain is
throughput.

### The code

It goes into `optimiseModule` (`src/support/HostTarget.cpp`), so the JIT, `fminus` and executables all get it:

```cpp
        // A vector math library. On Apple systems, libsystem_m's _simd_sin_f4, _simd_exp_f4, ... (in libSystem, which
        // every process and executable links already): the target library analysis tells the loop vectoriser about
        // them, and ReplaceWithVeclib, last, turns the vector intrinsics that float4 builtins lower to
        // (llvm.sin.v4f32) into calls to them, where the backend would call sinf once per lane. They are about three
        // times faster, and up to 3 ulp from the exact result where sinf is within 1. They take four lanes only: a
        // float2 or float8 call is still one sinf per lane. (Accelerate's mapping, the other on Apple systems, also
        // replaces llvm.fabs and llvm.sqrt, which NEON does in one instruction, with calls.)
        llvm::TargetLibraryInfoImpl library(machine.getTargetTriple());
        if (machine.getTargetTriple().isOSDarwin())
            library.addVectorizableFunctionsFromVecLib(llvm::VectorLibrary::DarwinLibSystemM,
                                                       machine.getTargetTriple());
        functions.registerPass([&] { return llvm::TargetLibraryAnalysis(library); });   // before the default one
        passes.registerOptimizerLastEPCallback(
            [](llvm::ModulePassManager& modulePasses, llvm::OptimizationLevel, llvm::ThinOrFullLTOPhase) {
                modulePasses.addPass(llvm::createModuleToFunctionPassAdaptor(llvm::ReplaceWithVeclib()));
            });
```

- **Register the analysis first.** `PassBuilder::registerFunctionAnalyses` registers a default
  `TargetLibraryAnalysis` built from the triple alone, and an analysis manager keeps the first registration of a
  type. So the custom one goes in before it.
- **Replace last.** At the `OptimizerLast` extension point, the intrinsic calls have been simplified, inlined and
  vectorised. Replacing them earlier would hide them from the passes that understand intrinsics.
- **Every platform keeps going.** On a triple that isn't Darwin, the analysis has no vector functions and the pass
  replaces nothing. On Linux, `LIBMVEC` (glibc's `_ZGVbN4v_sinf`, ...) or `SLEEFGNUABI` would be the choice, with a link
  to libmvec or SLEEF.

The JIT finds `_simd_sin_f4` among the process's symbols (LLJIT's default process-symbol link order), and an
executable gets it from libSystem; `otool -L` shows nothing new linked. The codegen pipeline of a `TargetMachine` has
its own `ReplaceWithVeclib` (TargetPassConfig adds it), but with a library info built from the triple alone, so it
finds nothing more to replace.

```asm
_sines:                             ; after recipe 8
	stp	x29, x30, [sp, #-16]!
	adrp	x8, __simd_sin_f4@GOTPAGE
	ldr	x8, [x8, __simd_sin_f4@GOTPAGEOFF]
	blr	x8
	ldp	x29, x30, [sp], #16
	ret

_shape:                             ; tanhf(x) + sqrtf(fabsf(x)): only tanhf becomes a call
	...
	blr	x8                      ; __simd_tanh_f4
	ldr	q1, [sp]
	fabs.4s	v1, v1
	fsqrt.4s	v1, v1
	fadd.4s	v0, v0, v1
```

### What it doesn't do

- **Only four lanes.** Every Apple entry is `FIXED(4)`, and `ReplaceWithVeclib` needs the exact width. A `float8`
  `sinf` stays `llvm.sin.v8f32`, which the backend splits and scalarises: eight calls to `sinf`. Splitting the call into
  two `float4` halves before the replacement (a few lines in a pass of your own, recipe 6's pattern) would give two
  calls. The same holds for a `float2`, and for a `poly float` that isn't four wide (recipe 9).
- **Scalar loops.** With recipe 10's pointers, the loop vectoriser can use the table too: a loop of `sinf` calls over
  a buffer becomes one `_simd_sin_f4` call each four samples.
- **No choice.** The recipe turns the library on for every program. A real compiler would make it an option, as clang's
  `-fveclib=` is, or a function attribute, for code that needs libm's accuracy or bit-identical scalar and vector
  results.

### Tests

`VectorTest.cpp`'s `sinf` check from recipe 7 changes from exact equality to Catch2's
`WithinULP(std::sin(x), 3)`, and a new test case sweeps `sinf` and `tanhf` over [-100, 100] the same way. It also checks
that a `float8` `sinf` is still exactly eight calls to `sinf`, in lane order. `tests/lit/fminus/veclib.fm` checks the
IR (`@_simd_sin_f4`, `@_simd_tanh_f4`, `llvm.fabs.v4f32` and `llvm.sqrt.v4f32` left alone, `llvm.sin.v8f32` left
alone) and, on arm64, the calls in the assembly. It is `REQUIRES: system-darwin`, a feature the recipe added to
`tests/lit/lit.cfg.py` (`"system-" + platform.system().lower()`).

## Recipe 9: `poly float`

*In the compiler. Tests: `PolyTest.cpp` (`[poly]`), `FrontendTest.cpp`, `tests/lit/fminus/poly.fm`.*

A `float4` is four lanes everywhere. On an AVX-512 machine, where a register holds sixteen floats, it uses a quarter
of each register; on NEON it is just right. A `poly float` is a float vector as wide as the target's registers. The
same source gets 4 lanes on an M5 and 16 on a Zen 4, and the program is written so that its meaning doesn't depend on
which:

```c
// The sum of 1/k^2 for k = 1 to n, poly_lanes() terms at a time: lane i takes k = base + i, and a mask drops the k past
// n in the last step.
float basel(float n)
{
    poly float sum = 0;
    for (float base = 1; base <= n; base += poly_lanes())
    {
        const poly float k = base + poly_index();
        sum += k <= n ? 1 / (k * k) : 0;
    }
    return reduce_add(sum);
}
```

This is how ISPC's `varying` values and `programCount` work, and Highway's `ScalableTag` and `Lanes(d)`, and
`std::experimental::native_simd`. A synthesizer puts one voice in each lane; a filter, one channel or one sample
block. What all of them have in common is the rule this recipe is mostly about: **a program must type-check and mean
the same thing whatever the width is**. If it compiled on NEON and failed on AVX-512, the abstraction would be worse
than none.

### The language

| | |
|---|---|
| `poly float` | a float per lane, as many lanes as the target's vector registers hold floats |
| `poly bool` | a lane mask: what comparing `poly float`s gives; unlike `bool4`, a program can declare it |
| `poly_lanes()` | the number of lanes, an `int` (a `float` before recipe 10): a constant once the target is known; `sizeof(poly float)` is 4 bytes a lane (recipe 11) |
| `poly_index()` | a `poly float` whose lanes hold their own index: 0, 1, 2, ... |
| `poly_load(p)`, `poly_load(p, active)` | the `poly_lanes()` floats at `p`, a `const float*`, lane i from `p[i]`; with a `poly bool`, only the lanes it sets, the others 0 and not read |
| `poly_store(p, v)`, `poly_store(p, v, active)` | `v` into the `poly_lanes()` floats at `p`, a `float*`; with a `poly bool`, only the lanes it sets |
| `(poly float)(x)` | a broadcast of one scalar |

Everything recipe 7 gives vectors works: the operators with broadcasting, comparisons and masks, `?:` lane by lane,
the builtins (overloaded at the target's width), `reduce_add`, `reduce_min`, `reduce_max`. `poly` is a qualifier on a
scalar type, as ISPC's `varying` is, so `poly int` can follow `int` (recipe 10). A scalar converts to a
`poly float` (broadcast), a `bool` to a `poly bool`, and a `poly bool` to a `poly float` (0 or 1 per lane).

What recipe 7 has and `poly float` doesn't, because each would depend on the width:

| Refused | Why | Error |
|---|---|---|
| `p + v` (a `float4`) | it would work at 4 lanes and fail at 8 | cannot convert between 'poly float' and 'float4': a poly type has as many lanes as the target's vectors |
| `v = p`, `return p` (as a `float4`) | the same, in an assignment or a return | assigning to 'float4' from incompatible type 'poly float' |
| `p.x`, `p.s3` | the lanes have no names, since their number is unknown | a poly float has no components: its lanes are the target's |
| `(poly float)(1, 2)` | which lanes would the arguments fill? | a poly float literal takes one scalar, which it broadcasts |
| `(float4)(p)` | a poly float can't fill a fixed number of lanes | cannot convert between 'poly float' and 'float4': ... |
| `p > 0 ? v : v` | a poly mask choosing between `float4`s | cannot convert between 'poly bool' and 'float4': ... |

The test of these errors compiles at 4 lanes, where `poly float` and `float4` are both `<4 x float>`, so it proves
the rules don't read LLVM's types. Recipe 7's `Value` is what makes that possible: without it, the code generator
couldn't tell the two apart on NEON.

### Choosing the width

The code generator needs one number, the lanes of a `poly float`. It comes in through a new `ir::Options`:

```cpp
    struct Options
    {
        // The lanes of a poly float: the target's vector register width in floats (support/HostTarget.h: polyLanes
        // asks the TargetMachine). 4 is NEON's and SSE's; any power of two from 1 to 64 lowers, wider ones in parts.
        unsigned polyLanes = 4;
    };

    std::unique_ptr<llvm::Module> lower(const ast::Program& program, llvm::LLVMContext& context,
                                        Diagnostics& diagnostics, const Options& options = {});
```

The width should be the one LLVM's own vectorisers would pick, and LLVM's cost model (`TargetTransformInfo`) says what
that is:

```cpp
    unsigned polyLanes(const llvm::TargetMachine& machine)
    {
        // The cost model asks a function, whose attributes may pick another CPU: one without any has the machine's.
        llvm::LLVMContext context;
        llvm::Module module("fminus.poly-lanes", context);
        llvm::Function* function =
            llvm::Function::Create(llvm::FunctionType::get(llvm::Type::getVoidTy(context), false),
                                   llvm::GlobalValue::ExternalLinkage, "f", module);
        const llvm::TargetTransformInfo info = machine.getTargetTransformInfo(*function);
        const llvm::TypeSize bits = info.getRegisterBitWidth(llvm::TargetTransformInfo::RGK_FixedWidthVector);
        return std::max(1u, static_cast<unsigned>(bits.getFixedValue() / 32));
    }
```

Why the cost model, and not the register width: on Intel's AVX-512 CPUs, LLVM *prefers* 256-bit vectors. 512-bit
instructions lower the clock on many of them, so their tuning carries `prefer-256-bit`, and X86's cost model answers
256 (`X86TTIImpl::getRegisterBitWidth`: 512 only when the subtarget has AVX-512 and its preferred width is at least
512). That is right for a `poly float` too. On this machine, `polyLanes` answers 4.

This build's LLVM has only the AArch64 backend, so the x86 column below was checked with Xcode's clang (Apple clang
21), which has the X86 backend: an explicit `<16 x float>` multiply-add, and the loop vectoriser on a simple loop, which
uses the same cost-model width. The `poly float` column follows from that width.

| `-march` | an explicit `<16 x float>` | the loop vectoriser's width | `poly float` lanes |
|---|---|---|---|
| `x86-64` (SSE2) | 4 × `xmm` | `xmm` | 4 |
| `haswell` (AVX2) | 2 × `ymm` | `ymm` | 8 |
| `skylake-avx512`, `sapphirerapids` | `zmm` | `ymm` (prefer-256-bit) | 8 |
| `skylake-avx512 -mprefer-vector-width=512` | `zmm` | `zmm` | 16 |
| `znver4`, `znver5` | `zmm` | `zmm` | 16 |
| `apple-m5` (NEON) | 4 × `q` registers | `q` (128 bits) | 4 |

The driver asks the target before it lowers, and `--poly-lanes` overrides the answer:

```cpp
    // The target: it decides how many lanes a poly float has.
    auto machine = fminus::hostTargetMachine(fminus::codeGenLevel(optimisation));
    if (!machine)
        return fail(machine.takeError());
    fminus::ir::Options options;
    options.polyLanes = polyLanes != 0 ? polyLanes : fminus::polyLanes(**machine);
```

`fminus voices.fm --poly-lanes 16` compiles as an AVX-512 machine would, and runs here: LLVM splits the 16 lanes into
four NEON registers. It takes 1, 2, 4, ... 64.

### The code

Most of the work was recipe 7's. The rest:

- **Tokens and grammar.** `"poly"` is a keyword. `type` gains `"poly" "float"` and `"poly" "bool"`, and `expr` gains
  `"(" "poly" "float" ")" "(" args ")"`. No conflicts.
- **Types.** `Type::PolyFloat` and `Type::PolyBool`, with `isPoly`. `ast::lanes` returns 0 for them: the tree doesn't
  know the target. `isVector` and `isMask` include them, and `maskOf` and `arithmeticOf` map between them.
- **Lanes.** The code generator gets its own `lanes(type)`, which knows the width, and uses it wherever recipe 7 used
  `ast::lanes`: `typeOf`, the splat in `convert`, literals. (A local variable named `lanes` in `components` had to be
  renamed `indices`: it hid the member function.)
- **The rules.** `commonType` refuses a poly type meeting a fixed vector; `lowerSelect` refuses a poly mask with fixed
  branches and the reverse; `lowerConstruct` refuses a poly literal with more than one argument and a poly argument in
  a fixed literal; `hasComponents` refuses a `poly float`. All of them report through one message, `polyMismatch`.
- **`poly_lanes()` and `poly_index()`** are constants: an `i32` of the width (a `ConstantFP` until recipe 10 added
  `int`), and a `ConstantVector` of 0, 1, ... They are refused as function names, like the reductions.

### Writing poly code

`basel` above is the basic pattern, *strip-mining*: a loop that advances by `poly_lanes()`, a `poly_index()` that gives
each lane its own element, and a mask for the last step, where some lanes run past the end. The mask keeps them from
contributing: `k <= n ? term : 0` is a select. Over a buffer, the same loop loads `poly_lanes()` samples at a time
(below).

At 4 lanes the loop is:

```llvm
for.body:                                         ; fminus poly.fm -O2 (4 lanes, this machine)
  %base.019 = phi float [ 1.000000e+00, %for.body.lr.ph ], [ %4, %for.body ]
  %sum.018 = phi <4 x float> [ zeroinitializer, %for.body.lr.ph ], [ %3, %for.body ]
  ...
  %0 = fadd <4 x float> %splat.splat, <float 0.000000e+00, float 1.000000e+00, float 2.000000e+00, float 3.000000e+00>
  %cmp8 = fcmp ole <4 x float> %0, %splat.splat7
  %1 = fmul <4 x float> %0, %0
  %2 = fdiv <4 x float> splat (float 1.000000e+00), %1
  %select = select <4 x i1> %cmp8, <4 x float> %2, <4 x float> zeroinitializer
  %3 = fadd <4 x float> %sum.018, %select
  %4 = fadd float %base.019, 4.000000e+00
```

and at `--poly-lanes 16` the same IR with `<16 x float>`, which NEON computes as four registers: four `fdiv.4s`, four
`fadd.4s` into four accumulators, per iteration.

### Loads and stores

A buffer is a `float*`, and a `poly float` moves through it with two builtins, OpenCL C's `vloadn` and `vstoren` at
the target's width:

```c
// A gain over a block: poly_lanes() samples a step, and the rest under a mask.
void scale(const float* restrict input, float* restrict output, int n, float g)
{
    int i = 0;
    for (; i + poly_lanes() <= n; i += poly_lanes())
        poly_store(output + i, g * poly_load(input + i));
    const poly bool rest = i + poly_index() < n;   // the lanes still in the buffer
    poly_store(output + i, g * poly_load(input + i, rest), rest);
}
```

`poly_load(p)` reads the `poly_lanes()` floats at `p`, lane i from `p[i]`, and `poly_store(p, v)` writes them. A
buffer's floats are aligned to a float, not to a vector, so they are unaligned vector loads and stores (`align 4`),
which today's NEON and x86 cores run as fast as aligned ones unless they cross a cache line. With a `poly bool` after
them, they touch only the lanes it sets: `llvm.masked.load` gives 0 in the others without reading them, and
`llvm.masked.store` leaves them alone, so the last step stays inside the buffer whatever `n` is. At 4 lanes the loop
and its tail are:

```llvm
  %poly.load = load <4 x float>, ptr %add.ptr9, align 4                ; fminus -O2 --poly-lanes 4
  %1 = fmul <4 x float> %.pre47, %poly.load
  store <4 x float> %1, ptr %add.ptr, align 4
  ...
  %poly.load29 = tail call <4 x float> @llvm.masked.load.v4f32.p0(ptr align 4 %add.ptr27, <4 x i1> %cmp18,
                                                                  <4 x float> zeroinitializer)
  tail call void @llvm.masked.store.v4f32.p0(<4 x float> %4, ptr align 4 %add.ptr22, <4 x i1> %cmp18)
```

Why the loop is in two parts, unmasked steps and one masked: x86 with AVX-512 has masked loads and stores in
hardware, and AVX2 has `vmaskmovps`, but NEON has none, so LLVM expands a masked load into one conditional load a
lane. Once, at the end, that costs nothing; on every step it would cost more than the arithmetic. The mask is all
false when `n` is a multiple of the width, and then the tail touches nothing.

What the two builtins take, and refuse:

- `p` is a `const float*` for `poly_load` and a `float*` for `poly_store`: a `float*` converts to the first and a
  `void*` to both, as in C. `poly_store` through a `const float*` is clang's "discards qualifiers", and an `int*` or
  a `float4*` is "incompatible pointer types".
- The value is a `poly float`: a scalar broadcasts, a `float4` is refused as everywhere (a `poly float` never meets a
  fixed width). The mask is a `poly bool`.
- Neither allocates, so a `[[clang::nonblocking]]` function may call them.
- Their results are the width's, so the rule above holds: a loop that does the same thing to every sample gives the
  same output at every width. `examples/saturator/` (below) checks that at widths 1 to 64.

There is no `poly float*`: `p[1]` would sit a different number of floats along on every target ([extending.md](
extending.md), recipe 10). The builtins take the address of a float, and the loop steps it by `poly_lanes()`.

### Results depend on the width

`basel(1000)` at six widths, all on this machine:

| `--poly-lanes` | 1 | 2 | 4 | 8 | 16 | 64 |
|---|---|---|---|---|---|---|
| `basel(1000)` | 1.6439348 | 1.6439347 | 1.6439342 | 1.6439342 | 1.6439346 | 1.643935 |

All are correct to float precision (π²/6 − 1/1000.5 is 1.6439346). They differ because the width decides the order of
the additions: lane i sums every `poly_lanes()`-th term, and then the lanes are summed. Float addition isn't
associative, so each order rounds differently. The test checks each result against a C++ emulation of its own order,
exactly, and against π²/6 within 1e-6.

This is inherent in width-agnostic code, and it is the main thing to design for:

- **Lane-independent work is exact at any width.** Each lane computes on its own data: a voice per lane, a channel per
  lane, a sample per lane. Only how the work is spread over lanes changes, never what any lane computes.
- **Order-independent reductions are exact too**: `reduce_min`, `reduce_max`, and integer sums once FMinus has
  integer vectors. The test's `peak` (a maximum) is bit-identical at every width.
- **Float sums across lanes are not.** If the result must not depend on the machine, fix the number of partial sums in
  the program (accumulate in a `float8`, or in a fixed number of `poly float`-sized blocks), or sum in a wider type.
- **The math library depends on the width too.** With recipe 8, `sinf` of a 4-lane `poly float` calls
  `_simd_sin_f4`, while other widths call `sinf` per lane, and the two differ by up to about 3 ulp. Writing this
  recipe's test ran into it: a maximum of `sinf`s, order-independent, still came out different at 4 lanes.

For a synthesizer that must render the same audio everywhere, this decides where `poly float` goes: across voices or
channels (lane-independent), not across the terms of one sum.

### Performance

`basel` over 3 × 10⁹ terms (`n` = 10⁶, 3000 times), as an executable at each width, on the M5:

| `--poly-lanes` | 1 | 4 (the machine's) | 8 | 16 | 32 |
|---|---|---|---|---|---|
| time | 2.01 s | 0.49 s | 0.34 s | 0.32 s | 0.28 s |
| speed-up | 1× | 4.1× | 5.9× | 6.3× | 7.2× |

Four lanes give the expected four times. Wider still helps, although the registers are full: at 16 lanes each
iteration has four independent accumulators, and the M5's several vector pipes overlap them, where at 4 lanes each
iteration waits for the previous one's `fadd` (the loop-carried dependency). LLVM's loop vectoriser does the same for
scalar loops, and calls it *interleaving*. So the register width is the right choice for a `poly float`'s *meaning*, but
not always the fastest. An option would be `polyLanes × interleave`, with the interleave count from
`TargetTransformInfo::getMaxInterleaveFactor`; measure before choosing.

### At the C boundary

A `poly float` parameter or result is the target's vector type. At 4 lanes on NEON, that is `<4 x float>`, and C++
calls it as a `float4` (the test does). At 8 or 16 lanes it is an `<8 x float>` or a `<16 x float>`, which meets
recipe 7's `float8` problem: the C ABI passes those through memory. And the C++ caller would have to know the width.
Exported functions should take and return scalars and buffers (recipe 10's pointers), and keep `poly float` inside.
`main` refuses `poly` parameters, as it refuses everything but `float` (see [programs.md](programs.md)).

### JIT and ahead of time

The JIT compiles on the user's machine, so a `poly float` always fits the machine it runs on: the same plugin uses 4
lanes on an M5, 8 on a Skylake and 16 on a Zen 4, with no extra work. That is the strongest case for `poly float` in
FMinus. An executable is compiled for the machine that builds it (`hostTargetMachine`), and would need function
multiversioning to ship one binary for several: compile each `poly` function at each width, and pick one at load time
from the CPU's features, as Highway's dynamic dispatch does.

### Tests

`PolyTest.cpp` has five test cases:

- one source compiled at 1, 2, 4, 8, 16 and 64 lanes (through `compile(source, &errors, {.polyLanes = lanes})`):
  `poly_lanes()`, the sum of `poly_index()`, a declared `poly bool`, `basel` against its own order and against
  π²/6, and an order-independent maximum that must not change with the width;
- a `poly float` function called from C++ as a `float4`, at 4 lanes;
- the errors, compiled at 4 lanes;
- `poly_load` and `poly_store` at 1 to 64 lanes: `scale` (above, marked `[[clang::nonblocking]]`) over buffers of 0,
  1, a width less one, a width, a width and one, three widths and two, and 100 floats, with the floats past the end
  checked untouched; a masked load whose other lanes hold NaN, summed (they must not be read); and a scalar stored to
  every lane;
- their errors.

`FrontendTest.cpp` checks the dump of `poly float`, `const poly bool` and a poly literal. Adding `poly` makes five
tokens that can start a function, more than Bison lists, so the top-level syntax error loses its "expecting ..."
part: "syntax error, unexpected identifier". `tests/lit/fminus/poly.fm` checks the IR at 4 and 16 lanes (`scale`'s
vector loads and stores, and its masked tail, too), the assembly (one `fdiv.4s` per iteration at 4 lanes, four at 16),
runs in the JIT and as an executable, and the `--poly-lanes` check.

## The examples

Five examples use what the recipes added, and `tests/lit/fminus/examples.test` and ctest run them.

**`examples/chord/chord.fm`** (recipes 7 and 8): a chord's four notes in a `float4`. One function turns four MIDI note
numbers into four frequencies, with scalars broadcast and one vector `expf`:

```c
float4 frequencies(float4 notes)
{
    const float ln2 = 0.693147181f;
    return 440 * expf((notes - 69) / 12 * ln2);
}
```

At `-O2` that is one `call <4 x float> @_simd_exp_f4`. A literal rearranges the lanes:
`(float4)(chord.yz, chord.w, chord.x * 2)` is the chord's first inversion, from a `float2`, a `float` and a product.
`reduce_max(chord) / reduce_min(chord)` is its span. `main` picks one note with nested `?:` over `chord.x` to
`chord.w`, since a lane can't be chosen by a variable yet. `fminus examples/chord/chord.fm --run main 60 1` prints
`391.99542`, G4.

**`examples/unison/unison.fm`** (recipe 9): detuned sawtooth voices, one per lane, strip-mined as in "Writing poly
code":

```c
    for (float first = 0; first < voices; first += poly_lanes())
    {
        const poly float voice = first + poly_index();   // this step's voices, one a lane
        const poly bool active = voice < voices;         // false in the lanes past the last voice
        ...
        mix += active ? saw(frequency * time) : 0;
    }
    return reduce_add(mix) / voices;
```

`active` is a `poly bool`, a mask a program can name. `--run main 0.01 7` at each width gives the mean of the same
seven voices, with the last digits depending on the order of the additions (Python, in double: -0.79959219):

| `--poly-lanes` | 1 | 2 | 4 | 8 | 16 | 64 |
|---|---|---|---|---|---|---|
| `main(0.01, 7)` | -0.7995922 | -0.79959214 | -0.79959214 | -0.7995922 | -0.7995922 | -0.7995922 |

**`examples/biquad/`** (recipe 7): a `float4` as several results. The filter returns `(y, s1, s2, 0)` from one call,
and computes its coefficients once a sample instead of three times. On the M5, in the dev build, that took it from
6.35 to 3.9 ns a sample (C++: 2.65), with its output still bit-identical to C++'s. Its README has the rest of the plan.

**`examples/saturator/`** (recipe 9, "Loads and stores"): `tanh(drive * x)` over a block, a sample at a time and a
`poly float` at a time, with `<math.h>`'s `tanhf` and with a rational function written in FMinus. Every sample is
independent, so the poly loop gives the same output at every width, which its harness checks from 1 to 64 lanes. On
an Emerald Rapids Xeon (8 lanes), in the dev build:

| ns a sample | a sample at a time | a `poly float` at a time |
|---|---|---|
| `tanhf` | 15.2 | 15.2 |
| the rational function | 0.44 (9.95 at `-O0`) | 0.44 (1.33 at `-O0`) |

glibc has no vector `tanhf`, so a `poly float` of it is a call a lane, and the call is the cost. The rational
function is arithmetic, and 35 times faster. At `-O2` LLVM's loop vectoriser widens the plain loop to the same code
the poly loop has; at `-O0` only the poly loop is vector code.

**`examples/meter/`** (recipe 9): a block's peak and RMS, the same two ways, with `reduce_max` and `reduce_add`. The
plain RMS loop adds one sample at a time, and LLVM may not vectorise it, since that would change the order of the
additions: 0.607 ns a sample, in FMinus and in C++. `rms_poly` keeps 8 sums side by side: 0.077 ns, and more
accurate, but its last digits depend on the width, which its harness checks against a C++ emulation of each width's
order. A maximum doesn't depend on the order, so LLVM vectorises the plain `peak` itself, with four running maxima
(0.085 ns); `peak_poly` has one, and is three times slower, until `--poly-lanes 32` gives it more.

## Scalable vectors

SVE (Arm) and the RISC-V V extension have vector registers whose width the CPU decides: 128 to 2048 bits on SVE. Code
is compiled once, without knowing it, and asks at run time. LLVM represents such a vector as
`<vscale x 4 x float>`: `vscale` times four floats, where `vscale` is a constant of the machine (4 on a 512-bit SVE
CPU).

`poly float`'s rules are exactly the ones such a type needs: no named lanes, no literals of several lanes, no mixing
with fixed sizes, and the width only through `poly_lanes()` and `poly_index()`. So a target with SVE could lower a
`poly float` to a scalable vector instead. The pieces exist in LLVM:

| `poly float` | Scalable lowering |
|---|---|
| the type | `ScalableVectorType::get(float, 4)` |
| `poly_lanes()` | `llvm.vscale.i32` × 4 |
| `poly_index()` | `llvm.stepvector.nxv4i32`, converted to float |
| `poly_load`, `poly_store` | `load` and `store` of `<vscale x 4 x float>`; masked, SVE's predicated `ld1w` and `st1w` |
| a splat | `CreateVectorSplat(ElementCount::getScalable(4), x)` |
| arithmetic, comparisons, `select` | unchanged |
| `reduce_add` (ordered) | `llvm.vector.reduce.fadd.nxv4f32`: SVE's `fadda`, which adds the lanes one by one |

This repository's `llc` compiles basel's loop body that way for an SVE target
(`llc -mtriple=aarch64-linux-gnu -mattr=+sve`):

```asm
step:
	index	z3.s, #0, #1            ; poly_index()
	ptrue	p0.s
	mov	z1.s, s1                ; broadcast base
	fmov	z4.s, #1.00000000
	mov	z2.s, s2                ; broadcast n
	ucvtf	z3.s, p0/m, z3.s
	fadd	z1.s, z1.s, z3.s        ; k
	fmul	z3.s, z1.s, z1.s
	fdivr	z3.s, p0/m, z3.s, z4.s
	fcmge	p0.s, p0/z, z2.s, z1.s  ; the mask is a predicate register
	movi	v1.2d, #0000000000000000
	mov	z1.s, p0/m, z3.s
	fadd	z0.s, z0.s, z1.s
	ret
lanes:
	cntw	x8                      ; poly_lanes(): the number of 32-bit lanes
	ucvtf	s0, w8
	ret
ordered:
	movi	v1.2s, #128, lsl #24    ; -0.0
	ptrue	p0.s
	fadda	s1, p0, s1, z0.s        ; the ordered sum, one lane at a time
```

Two cautions, both checked:

- **The ordered sum is slow.** `fadda` is strictly sequential, so `reduce_add` on a 2048-bit vector is 64 dependent
  additions. With a fixed width the same is true but short. A reassociated reduction (`faddv`) would be fast and
  width-dependent, the trade above.
- **Not on this machine.** The M5 has no SVE, only SME's streaming mode. Outside streaming mode, LLVM 23 rejects
  scalable types without SVE ("Don't know how to legalize this scalable vector type"). Inside it
  (`"aarch64_pstate_sm_enabled"`, `-mattr=+sme`), the ordered reduction crashes it: "Expanding reductions for scalable
  vectors is undefined", because `fadda` is one of the instructions streaming mode lacks. `sinf` of a scalable vector,
  on SVE, becomes a loop calling `sinf` for each lane, since no vector library entry matches an unknown width without
  SVE-specific tables (ArmPL and SLEEF have them).

For FMinus on Apple silicon, fixed-width `poly float` is the practical design. Scalable lowering is an addition for SVE
servers (Graviton, Neoverse) and RISC-V, with its own tests on such a machine.

## The auto-vectorisers

`optimiseModule` turns on LLVM's two vectorisers, with the host's cost model (`PipelineTuningOptions`):

- **The loop vectoriser** turns a loop over memory into a loop over vectors, with a scalar or masked tail. It needs
  loops (recipe 4), and memory to load and store (recipe 10's pointers). `tests/lit/fminus/pointers.fm` shows it on
  FMinus: a gain loop over a buffer gets a `vector.body` of `fmul <4 x float>` on NEON, four of them interleaved,
  with no overlap check when the buffers are `restrict`. (`tests/lit/smoke/opt-vectorize.ll` shows the same on
  hand-written IR.)
- **The SLP vectoriser** packs independent scalar operations of the same kind into vector operations. Four separate
  `fmul`s on four separate values can become one `fmul <4 x float>`, if loading and storing them as a vector is
  cheap. Without memory, that is rare.
- **Reductions need permission.** A loop that sums floats (`total += x[i]`) vectorises only with the `reassoc` flag,
  because a vectorised sum adds in another order. FMinus sets no fast-math flags, so it won't, and that is the same
  trade as `reduce_add`.

`opt -passes='default<O2>' -pass-remarks-missed=loop-vectorize` on `fminus -O0` IR says why a loop didn't vectorise.
Explicit vectors (recipes 7 and 9) and the auto-vectorisers combine: the vectorisers leave already-vector code alone,
and the backend legalises both the same way.

## Pitfalls

| Symptom | Cause | Fix |
|---|---|---|
| A rule works at one width and not another, or `poly float` and `float4` mix on NEON | the code generator read an FMinus type off an LLVM type (`<4 x float>` is both) | carry the FMinus type with the value (`Value`), and read only that |
| Bison: "useless associativity for ".", use %precedence" | `%left "."`: the swizzle rule ends in an identifier, so no conflict compares `"."` with itself | `%precedence "."` |
| `fabsf` on a `float4` becomes a call to `vfabsf` | Accelerate's vector library maps `llvm.fabs` and `llvm.sqrt`, and `ReplaceWithVeclib` replaces every mapped intrinsic | `VectorLibrary::DarwinLibSystemM` |
| The vector library is never used | the custom `TargetLibraryAnalysis` was registered after `registerFunctionAnalyses`, whose default one wins | register it first |
| A `float8` `sinf` is eight calls | the library has only four-lane functions, and `ReplaceWithVeclib` needs the exact width | split into `float4` halves first, or accept it |
| `sinf(v).x != sinf(v.x)` | the vector function and libm round differently (up to about 3 ulp) | a tolerance in tests; an option for code that needs libm's results |
| A `poly` result changes between machines | the width changed the order of a float sum, or which `sinf` ran | lane-independent work, order-independent reductions, a fixed number of partial sums |
| C++ reads garbage from a `float8` function | FMinus passes `<8 x float>` in registers; the C ABI passes it through memory | keep `float8` inside FMinus, or lower exported ones with `sret` as clang does |
| A test of `sinf` on vectors fails by a few ulp after recipe 8 | it compared with `std::sin` exactly | `Catch::Matchers::WithinULP(expected, 3)` |
| The top-level syntax error loses its "expecting ..." list | `poly` is a fifth way to start a function; Bison lists at most four | update the test |
| A local variable stops compiling after adding a member function | `std::vector<int> lanes` hides the member `lanes(type)` | rename the local |
| `llc` crashes on scalable vectors with `-mattr=+sme` | streaming mode lacks `fadda`, and LLVM 23 can't expand an ordered scalable reduction | SVE for scalable code; fixed widths on Apple silicon |
| An FMinus function with `(float4)(1, 2, 3, 4)`, compiled as C with `ext_vector_type`, gives `4 4 4 4` | in C the parentheses are a cast of a comma expression; only OpenCL C reads them as a literal | C's `(float4){1, 2, 3, 4}`, which FMinus has too; or `clang -x cl` to compare with FMinus |
| `(float4){x}` is `x, 0, 0, 0`, where `(float4)(x)` is `x` in every lane | C's braces fill the lanes left out with 0; OpenCL C's parentheses broadcast a single scalar | the form that means what you want: both are there |

## Where to go next

Design notes, not written or verified:

- **Strided and gathered loads.** `poly_load` reads consecutive floats. A stereo buffer's left channel is every
  other float: a strided load (`llvm.masked.gather` with computed addresses, or a load and a `shufflevector`), and a
  `poly int` index would make any pattern a gather. Interleaved channels are what a plugin's host often hands over.
- **Mask operations.** `&`, `|` and `^` on masks (`and`, `or`, `xor` of `<N x i1>`), `!` on a mask, and `any(m)`,
  `all(m)` (`llvm.vector.reduce.or` and `.and`), so a strip-mined loop can stop when no lane is active.
- **Lane movement.** Rotations and broadcasts of one lane across a `poly float`
  (`shufflevector` with indices computed from the width), and prefix sums. They are what filters across lanes need.
- **Voices in lanes.** For a synthesizer: a struct of `poly float` state, one voice per lane, stepped together, with
  a mask for the lanes whose voices are off. The work is lane-independent, so the output is the same at any width.
- **The biquad on four channels.** `examples/biquad/` filters a block a call, its state in a module's fields
  (recipe 12). Four channels' samples, states and coefficients as `float4`s (a `const float4*` of frames) would
  filter four channels at once: in C++, 1.44 ns a channel's sample against 2.8 for one channel, twice the
  throughput, the four lanes' recursions running side by side and `sinf` and `cosf` one vector call each.
- **Overloading.** `examples/saturator/` writes its rational tanh twice, for a `float` and for a `poly float`, since
  C has no overloading. OpenCL C's `__attribute__((overloadable))`, or C11's `_Generic`, would give the two one name.
- **Multiversioning.** Compile `poly` functions at several widths for executables, and dispatch on the CPU at load
  time.
- **Fast-math, scoped.** A per-function or per-block `reassoc` (extending.md, "Contraction and fast-math") would let
  `reduce_add` and vectorised sums use a tree: faster, and different in the last bits.
- **Scalable lowering** for SVE and RISC-V, as above.
