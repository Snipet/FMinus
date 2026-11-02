// tests/unit/TopLevelTest.cpp: the top level's constants (extending.md, recipe 28; src/ir/Constants.cpp,
// defineConstant), C's file-scope const objects, and arrays sized by integer constant expressions (sized), run through
// the JIT: a 17x12 table read by functions, a method and a parallel for through an enumeration's stride; tables and
// scalars of every type, which read what the same list on the stack holds, bit for bit (subnormals preserved or
// flushed), their missing elements 0 and a size from a list; C's file scope (from each declaration on, hidden by a
// local or a field, across an #include); sizes of enumeration constants and operators in fields, locals, tables and
// parameters; and clang's errors. The IR, one constant global per table and no copies, is
// tests/lit/fminus/toplevel.fm's.
#include "Compile.h"

#include <catch2/catch_test_macros.hpp>

#include <llvm/IR/GlobalVariable.h>
#include <llvm/IR/Instructions.h>

#include <array>
#include <bit>
#include <cstdint>
#include <format>
#include <functional>
#include <map>
#include <string>
#include <string_view>

using namespace fminus::test;

namespace
{
    constexpr int kRows = 17;
    constexpr int kColumns = 12;

    // The reporting program's table: 17 rows of 12, each value distinct and exact as a float.
    float entry(int row, int column)
    {
        return static_cast<float>(row * kColumns + column) * 0.125f - 3.0f;
    }

    std::string table()
    {
        std::string list;
        for (int r = 0; r < kRows; ++r)
        {
            for (int c = 0; c < kColumns; ++c)
                list += std::format("{}{:.3f}f", list.empty() ? "" : ", ", entry(r, c));   // exact
            list += "\n";
        }
        return "enum { ROWS = 17, COLS = 12, STRIDE = COLS };\n"
               "const float TABLE[ROWS * COLS] = {" + list + "};\n";
    }

    // The program, lowered: its LLVM module, or nothing with its errors in `errors`.
    std::unique_ptr<llvm::Module> lowered(std::string_view source, llvm::LLVMContext& context, std::string& errors)
    {
        fminus::Diagnostics diagnostics("test.fm");
        const auto program = fminus::frontend::parse(source, diagnostics);
        std::unique_ptr<llvm::Module> module = program ? fminus::ir::lower(*program, context, diagnostics) : nullptr;
        errors = diagnostics.format();
        return module;
    }

    // compile()'s, with `files` what an #include finds, by name.
    std::unique_ptr<fminus::jit::Jit> compileWith(std::string_view source,
                                                  const std::map<std::string, std::string, std::less<>>& files,
                                                  std::string& errors)
    {
        fminus::Diagnostics diagnostics("test.fm");
        const auto program = fminus::frontend::parse(
            source, diagnostics,
            [&](std::string_view name, std::string_view) -> std::optional<fminus::frontend::IncludedFile> {
                const auto found = files.find(name);
                if (found == files.end())
                    return std::nullopt;
                return fminus::frontend::IncludedFile{found->first, found->second};
            });
        auto context = std::make_unique<llvm::LLVMContext>();
        std::unique_ptr<llvm::Module> module = program ? fminus::ir::lower(*program, *context, diagnostics) : nullptr;
        errors = diagnostics.format();
        if (!module)
            return nullptr;
        auto jit = llvm::cantFail(fminus::jit::Jit::create(llvm::OptimizationLevel::O2));
        llvm::cantFail(jit->add(std::move(module), std::move(context)));
        return jit;
    }

    std::uint32_t bits(float x)
    {
        return std::bit_cast<std::uint32_t>(x);
    }

    // The floats' list, as a top-level constant's and as a function's plain array's, which must read alike.
    constexpr std::string_view kFloats =
        "[16] = {1, 2.5f, -0.0f, 0x1p-3f, 1e-40f, -1e-40f, 3.4e38f, 1 << 3, (float)(7 / 2), sizeof(float4),\n"
        "        -(1.5f * 2), 0.0f / 0.0f, -2147483647 - 1, GAIN * 3, N > 2 ? 0.25f : 4, 1e-40f * 0.5f};\n";
} // namespace

TEST_CASE("a top-level table is read by the functions and methods after it, through an enumeration's stride",
          "[codegen][toplevel]")
{
    const std::string source = table()
                               + "const float GAIN = 0.5f;\n"
                                 "float at(int r, int c) { return TABLE[r * STRIDE + c]; }\n"
                                 "float rowSum(int r)\n"
                                 "{\n"
                                 "    float s = 0;\n"
                                 "    for (int c = 0; c < COLS; ++c)\n"
                                 "        s += TABLE[r * STRIDE + c];\n"
                                 "    return s;\n"
                                 "}\n"
                                 "module Lookup\n"
                                 "{\n"
                                 "    float gain;\n"
                                 "    void init(float g) { gain = g; }\n"
                                 "    float read(int r, int c) { return TABLE[r * STRIDE + c] * gain; }\n"
                                 "};\n"
                                 "float viaModule(int r, int c) { Lookup l; l.init(2); return l.read(r, c); }\n"
                                 "void scaled(float* restrict out)\n"
                                 "{\n"
                                 "    parallel for (int i = 0; i < ROWS * COLS; ++i)\n"
                                 "        out[i] = TABLE[i] * GAIN;\n"
                                 "}\n"
                                 "const float* row(int r) { return &TABLE[r * STRIDE]; }\n";
    std::string errors;
    auto jit = compile(source, &errors);
    INFO(errors);
    REQUIRE(jit);
    auto at = lookup<float(int, int)>(*jit, "at");
    auto rowSum = lookup<float(int)>(*jit, "rowSum");
    auto viaModule = lookup<float(int, int)>(*jit, "viaModule");
    auto row = lookup<const float*(int)>(*jit, "row");
    std::array<float, kRows * kColumns> out{};
    lookup<void(float*)>(*jit, "scaled")(out.data());
    for (int r = 0; r < kRows; ++r)
    {
        float sum = 0;
        for (int c = 0; c < kColumns; ++c)
        {
            INFO("row " << r << ", column " << c);
            CHECK(at(r, c) == entry(r, c));
            CHECK(viaModule(r, c) == entry(r, c) * 2);
            CHECK(out[static_cast<std::size_t>(r * kColumns + c)] == entry(r, c) * 0.5f);
            CHECK(row(r)[c] == entry(r, c));
            sum += entry(r, c);
        }
        CHECK(rowSum(r) == sum);
    }
    CHECK(row(1) == row(0) + kColumns);   // one table, the same wherever it is read

    // One constant global holds it, which no function copies: no stack slot of floats, no store to it.
    llvm::LLVMContext context;
    const std::unique_ptr<llvm::Module> module = lowered(source, context, errors);
    REQUIRE(module);
    int tables = 0;
    for (const llvm::GlobalVariable& global : module->globals())
    {
        tables += 1;
        CHECK(global.getName() == (tables == 1 ? "TABLE" : "GAIN"));
        CHECK(global.isConstant());
    }
    CHECK(tables == 2);
    for (const llvm::Function& function : *module)
    {
        for (const llvm::BasicBlock& block : function)
        {
            for (const llvm::Instruction& instruction : block)
            {
                const auto* slot = llvm::dyn_cast<llvm::AllocaInst>(&instruction);
                CHECK_FALSE((slot != nullptr && slot->getAllocatedType()->isArrayTy()
                             && slot->getAllocatedType()->getArrayElementType()->isFloatTy()));   // a region's bytes
                const auto* store = llvm::dyn_cast<llvm::StoreInst>(&instruction);
                CHECK_FALSE((store != nullptr && store->getPointerOperand() == module->getNamedGlobal("TABLE")));
            }
        }
    }
}

TEST_CASE("a top-level constant holds what the same list on a function's stack does, bit for bit",
          "[codegen][toplevel]")
{
    const std::string prelude = "enum { N = 4 };\n"
                                "const float GAIN = 0.5f;\n"
                                "const int COUNT = N * 2 + 1;\n"
                                "const float4 LANES = (float4)(1, 2, 3, 4) * GAIN;\n";
    const std::string tables = prelude + "const float FLOATS" + std::string(kFloats)
                               + "const int INTS[20] = {1, -1, 1 << 31, ~0, 0x7FFFFFFF, hash(3), (int)2.9f, (int)-2.9f,"
                                 " true, 010, N && 0, N || 0, COUNT, 5};\n"
                                 "const float4 VECTORS[4] = {(float4)(1, 2, 3, 4), (float4){5}, (float4)(-0.0f),"
                                 " LANES};\n"
                                 "const float ZEROS[64];\n"
                                 "const float SIZED[] = {9, 8, 7, 6, 5, 4, 3, 2, 1, 0.5f};\n"
                                 "float floatsTable(int i) { return FLOATS[i]; }\n"
                                 "int intsTable(int i) { return INTS[i]; }\n"
                                 "float4 vectorsTable(int i) { return VECTORS[i]; }\n"
                                 "float zerosTable(int i) { return ZEROS[i]; }\n"
                                 "float sizedTable(int i) { return SIZED[i]; }\n";
    const std::string stack =
        "float floatsStack(int i) { float floats" + std::string(kFloats) + "return floats[i]; }\n";
    for (const fminus::ir::Denormals denormals : {fminus::ir::Denormals::Preserve, fminus::ir::Denormals::Flush})
    {
        INFO((denormals == fminus::ir::Denormals::Flush ? "flushed" : "preserved"));
        fminus::ir::Options options;
        options.denormals = denormals;
        std::string errors;
        auto jit = compile(tables + stack, &errors, options);
        INFO(errors);
        REQUIRE(jit);
        for (int i = 0; i < 16; ++i)
        {
            INFO("element " << i);
            CHECK(bits(lookup<float(int)>(*jit, "floatsTable")(i)) == bits(lookup<float(int)>(*jit, "floatsStack")(i)));
        }
    }
    std::string errors;
    auto jit = compile(tables, &errors);
    INFO(errors);
    REQUIRE(jit);
    auto floats = lookup<float(int)>(*jit, "floatsTable");
    CHECK(floats(1) == 2.5f);
    CHECK(bits(floats(2)) == 0x80000000u);
    CHECK(bits(floats(4)) == bits(1e-40f));   // preserved: a subnormal as written
    CHECK(floats(11) != floats(11));          // NaN
    CHECK(floats(13) == 1.5f);                // GAIN * 3: a constant scalar of the top level is a constant
    CHECK(floats(14) == 0.25f);               // ?: on a constant condition
    auto ints = lookup<int(int)>(*jit, "intsTable");
    CHECK(ints(2) == static_cast<int>(0x80000000u));
    CHECK(ints(5) == 1408362973);   // hash(3)
    CHECK(ints(7) == -2);
    CHECK(ints(9) == 8);
    CHECK(ints(10) == 0);   // N && 0
    CHECK(ints(11) == 1);   // N || 0
    CHECK(ints(12) == 9);   // COUNT
    CHECK(ints(13) == 5);
    for (const int i : {14, 19})   // missing elements are 0
        CHECK(ints(i) == 0);
    auto vectors = lookup<float4(int)>(*jit, "vectorsTable");
    CHECK(lanesOf(vectors(1)) == std::array{5.0f, 0.0f, 0.0f, 0.0f});
    CHECK(lanesOf(vectors(3)) == std::array{0.5f, 1.0f, 1.5f, 2.0f});
    for (const int i : {0, 63})
        CHECK(bits(lookup<float(int)>(*jit, "zerosTable")(i)) == 0u);
    CHECK(lookup<float(int)>(*jit, "sizedTable")(9) == 0.5f);   // sized[] has its list's 10 elements

    // The scalars, read as their values, and an address of a table's element, a constant too.
    jit = compile(prelude
                      + "const bool ON = N > 2 && COUNT != 0;\n"
                        "const float HALF = 1 / 2.0f, ZERO;\n"
                        "const int TABLE[3] = {10, 20, 30};\n"
                        "const int* const SECOND = &TABLE[1];\n"
                        "const poly float WIDE = (poly float)(GAIN);\n"
                        "float gain() { return GAIN + HALF + ZERO; }\n"
                        "int count() { return COUNT + *SECOND + ON; }\n"
                        "float4 lanes() { return LANES; }\n"
                        "float wide() { return reduce_add(WIDE); }\n"
                        "const float* address() { return &GAIN; }\n"
                        "bool same() { return &GAIN == address() && SECOND - TABLE == 1; }\n",
                  &errors);
    INFO(errors);
    REQUIRE(jit);
    CHECK(lookup<float()>(*jit, "gain")() == 1.0f);
    CHECK(lookup<int()>(*jit, "count")() == 30);
    CHECK(lanesOf(lookup<float4()>(*jit, "lanes")()) == std::array{0.5f, 1.0f, 1.5f, 2.0f});
    CHECK(lookup<float()>(*jit, "wide")() == 0.5f * static_cast<float>(fminus::ir::Options{}.polyLanes));
    CHECK(*lookup<const float*()>(*jit, "address")() == 0.5f);
    CHECK(lookup<bool()>(*jit, "same")());
}

TEST_CASE("a top-level constant is the file's, from its declaration on: a local or a field hides it",
          "[codegen][toplevel]")
{
    std::string errors;
    auto jit = compile("const float K = 1;\n"
                       "float hidden() { float K = 2; return K; }\n"
                       "float outer() { { const int K = 3; } return K; }\n"
                       "module M\n"
                       "{\n"
                       "    float K;\n"
                       "    float field() { K = 4; return K; }\n"   // this->K, as C++ looks names up in a method
                       "    float file() { return LATER; }\n"       // declared after the module: not seen
                       "};\n"
                       "const float LATER = 5;\n"
                       "float viaField() { M m; return m.field(); }\n"
                       "float later() { return LATER + K; }\n",
                       &errors);
    REQUIRE_FALSE(jit);
    CHECK(errors == "test.fm:8:27: error: use of undeclared identifier 'LATER'\n");
    jit = compile("const float K = 1;\n"
                  "float hidden() { float K = 2; return K; }\n"
                  "float outer() { { const int K = 3; } return K; }\n"
                  "module M { float K; float field() { K = 4; return K; } };\n"
                  "const float LATER = 5;\n"
                  "float viaField() { M m; return m.field(); }\n"
                  "float later() { return LATER + K; }\n",
                  &errors);
    INFO(errors);
    REQUIRE(jit);
    CHECK(lookup<float()>(*jit, "hidden")() == 2.0f);
    CHECK(lookup<float()>(*jit, "outer")() == 1.0f);
    CHECK(lookup<float()>(*jit, "viaField")() == 4.0f);
    CHECK(lookup<float()>(*jit, "later")() == 6.0f);

    // An #included file's constants are the including file's after the #include, and its enumerations size them.
    const std::map<std::string, std::string, std::less<>> files{
        {"tables.fm", "enum { TAPS = 3 };\nconst float TAPS_OF[TAPS] = {0.25f, 0.5f, 0.25f};\n"}};
    jit = compileWith("#include \"tables.fm\"\n"
                      "float smooth(const float* x)\n"
                      "{\n"
                      "    float s = 0;\n"
                      "    for (int i = 0; i < TAPS; ++i)\n"
                      "        s += TAPS_OF[i] * x[i];\n"
                      "    return s;\n"
                      "}\n",
                      files, errors);
    INFO(errors);
    REQUIRE(jit);
    const std::array<float, 3> x{4.0f, 8.0f, 12.0f};
    CHECK(lookup<float(const float*)>(*jit, "smooth")(x.data()) == 8.0f);
    CHECK_FALSE(compileWith("float early() { return TAPS_OF[0]; }\n#include \"tables.fm\"\n", files, errors));
    CHECK(errors == "test.fm:1:24: error: use of undeclared identifier 'TAPS_OF'\n");
}

TEST_CASE("an array's size is an integer constant expression: in a field, a local, a table and a parameter",
          "[codegen][toplevel][array]")
{
    std::string errors;
    auto jit = compile("enum { A = 3, B = 5 };\n"
                       "module Bank\n"
                       "{\n"
                       "    float hist[A * B];\n"
                       "    enum { TAPS = A + 1 };\n"   // the module's, seen by the fields after it
                       "    float4 taps[TAPS << 1];\n"
                       "    int flags[(A | B) - 1];\n"
                       "    float last[sizeof(float4) / sizeof(float) + (int)2.5f];\n"
                       "};\n"
                       "int bank() { return sizeof(Bank); }\n"
                       "float local()\n"
                       "{\n"
                       "    enum { N = B > A ? B : A };\n"   // a block's enumeration sizes the block's array
                       "    float buf[N * 2];\n"
                       "    for (int i = 0; i < N * 2; ++i)\n"
                       "        buf[i] = i;\n"
                       "    const int list[] = {4, 5, 6, true + 6};\n"
                       "    return buf[N * 2 - 1] + list[3];\n"
                       "}\n"
                       "float viaParameter(float x[A * B], int i) { return x[i]; }\n"
                       "const int TABLE[A * B - 1] = {A, B};\n"
                       "int table(int i) { return TABLE[i]; }\n",
                       &errors);
    INFO(errors);
    REQUIRE(jit);
    struct Mirror   // the C++ layout of Bank's fields
    {
        float hist[15];
        float4 taps[8];
        int flags[6];
        float last[6];
    };
    CHECK(lookup<int()>(*jit, "bank")() == static_cast<int>(sizeof(Mirror)));
    CHECK(lookup<float()>(*jit, "local")() == 9.0f + 7.0f);
    std::array<float, 15> x{0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14};
    CHECK(lookup<float(float*, int)>(*jit, "viaParameter")(x.data(), 14) == 14.0f);
    CHECK(lookup<int(int)>(*jit, "table")(1) == 5);
    CHECK(lookup<int(int)>(*jit, "table")(13) == 0);
}

TEST_CASE("top-level constants' and sizes' errors are clang's", "[codegen][toplevel]")
{
    // Writing to a constant, and initialisers that are no constants.
    CHECK(errorsOf("const float T[4] = {1, 2};\n"
                   "const float G = 2;\n"
                   "float f(int i)\n"
                   "{\n"
                   "    T[i] = 1;\n"
                   "    i[T] += 1;\n"
                   "    T[0]++;\n"
                   "    G = 3;\n"
                   "    G *= 2;\n"
                   "    T = T;\n"
                   "    float* p = T;\n"
                   "    return T[i] + G;\n"
                   "}\n"
                   "float g(void);\n"
                   "const float CALL[2] = {g(), 1};\n"
                   "const float ELEMENT = T[1];\n"
                   "const float* const HEAP = malloc(4);\n"
                   "const float SINE = sinf(1);\n"
                   "const int SIZE[2] = {1, 2, 3};\n"
                   "const float LIST = {1};\n"
                   "const float NOT_LIST[2] = G;\n")
          == "test.fm:5:10: error: cannot assign to variable 'T' with const-qualified type 'const float[4]'\n"
             "test.fm:6:10: error: cannot assign to variable 'T' with const-qualified type 'const float[4]'\n"
             "test.fm:7:9: error: cannot assign to variable 'T' with const-qualified type 'const float[4]'\n"
             "test.fm:8:7: error: cannot assign to variable 'G' with const-qualified type 'const float'\n"
             "test.fm:9:7: error: cannot assign to variable 'G' with const-qualified type 'const float'\n"
             "test.fm:10:7: error: array type 'float[4]' is not assignable\n"
             "test.fm:11:12: error: initializing 'float *' with an expression of type 'const float *' discards "
             "qualifiers\n"
             "test.fm:15:24: error: initializer element is not a compile-time constant\n"
             "test.fm:16:24: error: initializer element is not a compile-time constant\n"
             "test.fm:17:27: error: initializer element is not a compile-time constant\n"
             "test.fm:18:20: error: initializer element is not a compile-time constant\n"
             "test.fm:19:11: error: excess elements in array initializer\n"
             "test.fm:20:13: error: FMinus takes an initializer list for an array only\n"
             "test.fm:21:13: error: array initializer must be an initializer list\n");

    // Sizes: no constant (a const object is none, as in C17: an enumeration constant names a size), negative, zero,
    // not an integer, too large, none at all.
    CHECK(errorsOf("const int N = 4;\n"
                   "enum { E = 4 };\n"
                   "const float BY_CONST[N] = {1};\n"
                   "const float NEGATIVE[E - 5] = {1};\n"
                   "const float NONE[];\n"
                   "module M { float a[N]; float b[]; float c[sizeof(M)]; float d[K]; enum { K = 2 }; float e[K]; };\n"
                   "void f(int n, float x[n], float y[-E])\n"
                   "{\n"
                   "    float a[n];\n"
                   "    float b[E - 4];\n"
                   "    float c[1.5f * 2];\n"
                   "    float d[E * 1000000000];\n"
                   "    float e[(1 << 30) + 1];\n"
                   "    float g[];\n"
                   "    int h[] = {};\n"
                   "    switch (n) { case N: break; }\n"
                   "}\n")
          == "test.fm:3:22: error: expression is not an integer constant expression\n"
             "test.fm:4:24: error: 'NEGATIVE' declared as an array with a negative size\n"
             "test.fm:5:13: error: definition of variable with array type needs an explicit size or an initializer\n"
             "test.fm:6:20: error: expression is not an integer constant expression\n"
             "test.fm:6:30: error: field has incomplete type 'float[]'\n"
             "test.fm:6:43: error: invalid application of 'sizeof' to an incomplete type 'M'\n"
             "test.fm:6:63: error: use of undeclared identifier 'K'\n"
             "test.fm:7:23: error: expression is not an integer constant expression\n"
             "test.fm:7:35: error: 'y' declared as an array with a negative size\n"
             "test.fm:9:13: error: expression is not an integer constant expression\n"
             "test.fm:10:15: error: zero size arrays are not allowed\n"
             "test.fm:11:18: error: size of array has non-integer type 'float'\n"
             "test.fm:12:15: error: overflow in expression; result is -294967296 with type 'int'\n"
             "test.fm:13:23: error: array is too large: FMinus's arrays have at most 2^30 elements\n"
             "test.fm:14:11: error: definition of variable with array type needs an explicit size or an initializer\n"
             "test.fm:15:9: error: zero size arrays are not allowed\n"
             "test.fm:16:23: error: expression is not an integer constant expression\n");

    // FMinus's: no variables and no modules at the top level. C's file scope: one name for each thing.
    CHECK(errorsOf("float state;\n"
                   "module M { float y; };\n"
                   "const M m;\n"
                   "const void v;\n"
                   "const float A = 1;\n"
                   "const float A = 2;\n"
                   "enum { B };\n"
                   "const float B = 1;\n"
                   "const float C = 1;\n"
                   "enum { C };\n"
                   "float f(void);\n"
                   "const float f = 1;\n"
                   "const float g = 1;\n"
                   "float g(void) { return 0; }\n"
                   "const float Late = 1;\n"
                   "module Late { float y; };\n")
          == "test.fm:1:7: error: FMinus has no variables at the top level: declare 'state' const, a constant of the "
             "program, or keep it in a module\n"
             "test.fm:3:9: error: a module is no constant of the top level: its fields start at 0, and only its "
             "methods set them\n"
             "test.fm:4:12: error: variable has incomplete type 'void'\n"
             "test.fm:6:13: error: redefinition of 'A'\n"
             "test.fm:8:13: error: redefinition of 'B' as different kind of symbol\n"
             "test.fm:7:8: note: previous definition is here\n"
             "test.fm:10:8: error: redefinition of 'C' as different kind of symbol\n"
             "test.fm:9:13: note: previous definition is here\n"
             "test.fm:12:13: error: redefinition of 'f' as different kind of symbol\n"
             "test.fm:11:7: note: previous definition is here\n"
             "test.fm:14:7: error: redefinition of 'g' as different kind of symbol\n"
             "test.fm:13:13: note: previous definition is here\n"
             "test.fm:16:8: error: redefinition of 'Late' as different kind of symbol\n"
             "test.fm:15:13: note: previous definition is here\n");
}
