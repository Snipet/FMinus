// tests/unit/Array2DTest.cpp: 2-D arrays (extending.md, recipe 34): C's arrays of arrays, `float grid[4][16]`, rows
// side by side, as locals, fields, tables and constants of the top level, of every element type FMinus has arrays of;
// indexed grid[i][j]; a row, grid[i], a float* wherever one goes; the name a pointer to the first row, with C's
// arithmetic; initialiser lists placed as C places them, rows braced or not; sizeof of an array type; the tables'
// shapes in the IR; their loops vectorised; and the errors, in clang's words where clang has them.
#include "Compile.h"

#include "support/HostTarget.h"

#include <llvm/Support/raw_ostream.h>
#include <llvm/Target/TargetMachine.h>

#include <array>
#include <cstring>
#include <regex>
#include <string>
#include <string_view>

using namespace fminus::test;

namespace
{
    // The program's IR, at -O0 (`optimised` false) or optimised at -O2 for this machine, as fminus --emit ll prints it.
    std::string irOf(std::string_view source, bool optimised = false)
    {
        fminus::Diagnostics diagnostics("test.fm");
        const auto program = fminus::frontend::parse(source, diagnostics);
        llvm::LLVMContext context;
        std::unique_ptr<llvm::Module> module =
            program ? fminus::ir::lower(*program, context, diagnostics, {}) : nullptr;
        INFO(diagnostics.format());
        REQUIRE(module);
        if (optimised)
        {
            auto machine = llvm::cantFail(fminus::hostTargetMachine(llvm::CodeGenOptLevel::Default));
            fminus::prepareModule(*module, *machine);
            fminus::optimiseModule(*module, *machine, llvm::OptimizationLevel::O2);
        }
        std::string text;
        llvm::raw_string_ostream stream(text);
        module->print(stream, nullptr);
        return text;
    }

    // One function of the IR, from its `define` to its closing brace (a method's name is quoted: @"Bank::scale").
    std::string functionOf(const std::string& ir, const std::string& name)
    {
        std::size_t at = ir.find("@" + name + "(");
        if (at == std::string::npos)
            at = ir.find("@\"" + name + "\"(");
        REQUIRE(at != std::string::npos);
        const std::size_t begin = ir.rfind("define", at);
        return ir.substr(begin, ir.find("\n}\n", at) - begin);
    }
} // namespace

TEST_CASE("2-D arrays are C's: rows side by side, a[i][j] an element, a[i] a row that stands for a float*",
          "[codegen][array2d]")
{
    std::string errors;
    auto jit = compile("enum { ROWS = 3, COLS = 4 };\n"
                       "float rowSum(const float* row, int n) { float s = 0; for (int i = 0; i < n; ++i) s += row[i];"
                       " return s; }\n"
                       "float grid()\n"
                       "{\n"
                       "    float g[ROWS][COLS];\n"   // zeroed, as every array
                       "    for (int i = 0; i < ROWS; ++i)\n"
                       "        for (int j = 0; j < COLS; ++j)\n"
                       "            g[i][j] = i * 10 + j;\n"
                       "    return g[2][3] + rowSum(g[1], COLS) + g[0][0];\n"   // 23 + (10 + 11 + 12 + 13) + 0
                       "}\n"
                       "float layout()\n"
                       "{\n"
                       "    float g[2][3];\n"
                       "    float* first = g[0];\n"
                       "    for (int k = 0; k < 6; ++k)\n"
                       "        first[k] = k;\n"   // the rows side by side: g[1][0] is the fourth float
                       "    return g[1][0] * 100 + (float)(g[1] - g[0]) * 10 + g[1][2];\n"   // 300 + 30 + 5
                       "}\n"
                       "int pointers()\n"
                       "{\n"
                       "    int g[3][2] = {{1, 2}, {3, 4}, {5, 6}};\n"
                       // the name a pointer to the first row: g + 2 the third, *g the first, two rows apart
                       "    return *(*(g + 2) + 1) * 100 + ((g + 2) - g) * 10 + (*g)[1] + (g + 1 > g) * 1000;\n"
                       "}\n"
                       "float row(int i, int j) { const float t[2][2] = {{1, 2}, {3, 4}}; return (i ? t[1] : t[0])[j];"
                       " }\n"
                       "int sizes() { return sizeof(float[4][16]) + sizeof(double[2][3]) * 1000 + sizeof(float4[2][2])"
                       " * 100000; }\n"
                       "float fresh()\n"
                       "{\n"
                       "    float s = 0;\n"
                       "    for (int r = 0; r < 3; ++r)\n"
                       "    {\n"
                       "        float a[2][2];\n"   // each time it is declared, it starts at 0 again
                       "        s += a[1][1];\n"
                       "        a[1][1] = 5;\n"
                       "    }\n"
                       "    return s;\n"
                       "}\n"
                       "void scale(float* x, int n, float k) { for (int i = 0; i < n; ++i) x[i] *= k; }\n"
                       "float rows()\n"
                       "{\n"
                       "    float g[2][2] = {{1, 2}, {3, 4}};\n"
                       "    scale(g[1], 2, 10);\n"   // a row goes where a float* does
                       "    float* p = g[0] + 1;\n"
                       "    return g[1][1] + *p + poly_lanes() * 0;\n"   // 40 + 2
                       "}\n",
                       &errors);
    INFO(errors);
    REQUIRE(jit);
    CHECK(lookup<float()>(*jit, "grid")() == 69.0f);      // 23 + 46 + 0
    CHECK(lookup<float()>(*jit, "layout")() == 335.0f);   // g[1][0] is 3, the rows 3 apart, g[1][2] is 5
    CHECK(lookup<int()>(*jit, "pointers")() == 1622);     // 6 * 100 + 2 * 10 + 2 + 1000
    CHECK(lookup<float(int, int)>(*jit, "row")(1, 0) == 3.0f);
    CHECK(lookup<float(int, int)>(*jit, "row")(0, 1) == 2.0f);
    CHECK(lookup<int()>(*jit, "sizes")() == 256 + 48 * 1000 + 64 * 100000);
    CHECK(lookup<float()>(*jit, "fresh")() == 0.0f);
    CHECK(lookup<float()>(*jit, "rows")() == 42.0f);
}

TEST_CASE("2-D arrays are a module's fields, of modules, doubles and vectors too, and copied with their module",
          "[codegen][array2d]")
{
    std::string errors;
    auto jit = compile("module Voice { float x; void tick() { x += 1; } };\n"
                       "module Synth\n"
                       "{\n"
                       "    Voice voices[2][3];\n"
                       "    double slow[2][2];\n"
                       "    float4 lanes[2][2];\n"
                       "    int counts[3][5];\n"
                       "    void run()\n"
                       "    {\n"
                       "        for (int i = 0; i < 2; ++i)\n"
                       "            for (int j = 0; j < 3; ++j)\n"
                       "            {\n"
                       "                voices[i][j].tick();\n"
                       "                voices[i][j].x += i;\n"
                       "            }\n"
                       "        slow[1][1] = 0.1;\n"
                       "        slow[1][1] *= 3.0;\n"
                       "        lanes[1][0] = (float4)(1, 2, 3, 4);\n"
                       "        counts[2][4]++;\n"
                       "    }\n"
                       "};\n"
                       "double use()\n"
                       "{\n"
                       "    Synth s;\n"
                       "    s.run();\n"
                       "    s.voices[1][2].tick();\n"
                       "    (s.voices[0] + 1)->tick();\n"   // a row of modules: a Voice*
                       "    Synth t = s;\n"                  // copied whole, its 2-D fields too
                       "    s.counts[2][4] = 100;\n"
                       "    return t.voices[1][2].x * 1000 + t.voices[0][1].x * 100 + t.lanes[1][0].z * 10"
                       " + t.counts[2][4] + t.slow[1][1];\n"
                       "}\n"
                       "int size() { return sizeof(Synth); }\n",
                       &errors);
    INFO(errors);
    REQUIRE(jit);
    CHECK(lookup<double()>(*jit, "use")() == 3000.0 + 200.0 + 30.0 + 1.0 + 0.1 * 3.0);
    // Voice[2][3], 24 bytes at 0; double[2][2], 32 at 24; float4[2][2], 64 at 64 (aligned to 16); int[3][5], 60 at
    // 128: 188, rounded up to the float4s' 16.
    CHECK(lookup<int()>(*jit, "size")() == 192);
}

TEST_CASE("a 2-D array's initialiser list is placed as C places it: rows braced, or the values row by row",
          "[codegen][array2d]")
{
    std::string errors;
    auto jit = compile("float at(int which, int i, int j)\n"
                       "{\n"
                       "    float braced[3][3] = {{1, 2}, {3}, {4, 5, 6}};\n"
                       "    float flat[3][3] = {1, 2, 3, 4, 5};\n"           // row by row, the rest 0
                       "    float mixed[3][3] = {{1}, 2, 3, 4, {5}};\n"   // C's brace elision: 2 starts row 1
                       "    float empty[2][2] = {{}, {7}};\n"                 // C23's {}: a row of zeros
                       "    float listed[][2] = {{1}, {2, 3}, 4};\n"          // three rows, from its list
                       "    float none[2][2] = {};\n"
                       "    switch (which)\n"
                       "    {\n"
                       "    case 0: return braced[i][j];\n"
                       "    case 1: return flat[i][j];\n"
                       "    case 2: return mixed[i][j];\n"
                       "    case 3: return empty[i][j];\n"
                       "    case 4: return listed[i][j];\n"
                       "    default: return none[i][j];\n"
                       "    }\n"
                       "}\n",
                       &errors);
    INFO(errors);
    REQUIRE(jit);
    auto* at = lookup<float(int, int, int)>(*jit, "at");
    const auto all = [&](int which, int rows, int columns) {
        std::array<float, 9> values{};
        for (int i = 0; i < rows; ++i)
            for (int j = 0; j < columns; ++j)
                values[static_cast<std::size_t>(i * columns + j)] = at(which, i, j);
        return values;
    };
    CHECK(all(0, 3, 3) == std::array<float, 9>{1, 2, 0, 3, 0, 0, 4, 5, 6});
    CHECK(all(1, 3, 3) == std::array<float, 9>{1, 2, 3, 4, 5, 0, 0, 0, 0});
    CHECK(all(2, 3, 3) == std::array<float, 9>{1, 0, 0, 2, 3, 4, 5, 0, 0});
    CHECK(all(3, 2, 2) == std::array<float, 9>{0, 0, 7, 0});
    CHECK(all(4, 3, 2) == std::array<float, 9>{1, 0, 2, 3, 4, 0});
    CHECK(all(5, 2, 2) == std::array<float, 9>{});
}

TEST_CASE("a const 2-D table of constants is stored once, row by row, a table's shapes in each row",
          "[codegen][array2d][ir]")
{
    const std::string ir = irOf("const float top[2][3] = {{1, 2, 3}, {4}};\n"
                                "const int sparse[16][4] = {{1}};\n"
                                "const float wide[3][10] = {{1, 2}, {3}, {0, 0, 0, 0, 0, 0, 0, 0, 0, 9}};\n"
                                "float f(int i, int j, float x)\n"
                                "{\n"
                                "    const float local[2][2] = {{5, 6}, {7}};\n"
                                "    const float computed[2][2] = {{x, 1}};\n"   // not constants: a local, filled
                                "    return top[i][j] + sparse[i][j] + wide[i][j] + local[i][j] + computed[i][j];\n"
                                "}\n");
    CHECK(ir.find("@top = private constant [2 x [3 x float]] [[3 x float] [float 1.000000e+00, float 2.000000e+00, "
                  "float 3.000000e+00], [3 x float] [float 4.000000e+00, float 0.000000e+00, float 0.000000e+00]], "
                  "align 4")
          != std::string::npos);
    // 15 rows of zeros after the first: one zeroinitializer, as a 1-D table's eight zeros or more are.
    CHECK(ir.find("@sparse = private constant <{ [1 x [4 x i32]], [15 x [4 x i32]] }> <{ [1 x [4 x i32]] [[4 x i32] "
                  "[i32 1, i32 0, i32 0, i32 0]], [15 x [4 x i32]] zeroinitializer }>, align 4")
          != std::string::npos);
    // A row of eight zeros or more is a struct, so the rows are side by side in one.
    CHECK(ir.find("@wide = private constant <{ <{ [2 x float], [8 x float] }>, <{ [1 x float], [9 x float] }>, "
                  "[10 x float] }>")
          != std::string::npos);
    CHECK(ir.find("@__const.f.local = private unnamed_addr constant [2 x [2 x float]] [[2 x float] [float "
                  "5.000000e+00, float 6.000000e+00], [2 x float] [float 7.000000e+00, float 0.000000e+00]]")
          != std::string::npos);
    CHECK(ir.find("%computed = alloca [2 x [2 x float]]") != std::string::npos);
    CHECK(ir.find("getelementptr inbounds [2 x [2 x float]], ptr %computed, i64 0, i64 0, i64 1") != std::string::npos);

    std::string errors;
    auto jit = compile("const float wide[3][10] = {{1, 2}, {3}, {0, 0, 0, 0, 0, 0, 0, 0, 0, 9}};\n"
                       "const float* const nine = &wide[2][9];\n"
                       "float f(int i, int j) { return wide[i][j] + *nine * 100; }\n",
                       &errors);
    INFO(errors);
    REQUIRE(jit);
    auto* f = lookup<float(int, int)>(*jit, "f");
    CHECK(f(0, 1) == 902.0f);
    CHECK(f(1, 0) == 903.0f);
    CHECK(f(1, 9) == 900.0f);
    CHECK(f(2, 9) == 909.0f);
}

TEST_CASE("loops over a 2-D array vectorise as a 1-D array's do", "[codegen][array2d][ir]")
{
    const std::string ir = irOf("module Bank\n"
                                "{\n"
                                "    float gain[8][64];\n"
                                "    void scale(float k) { for (int i = 0; i < 8; ++i) for (int j = 0; j < 64; ++j)"
                                " gain[i][j] *= k; }\n"
                                "    void mix(float* restrict out, int n)\n"
                                "    {\n"
                                "        for (int v = 0; v < 8; ++v)\n"
                                "            for (int i = 0; i < n; ++i)\n"
                                "                out[i] += gain[v][i & 63];\n"
                                "    }\n"
                                "};\n",
                                true);
    const std::regex vector(R"(load <\d+ x float>)");
    CHECK(std::regex_search(functionOf(ir, "Bank::scale"), vector));
    CHECK(std::regex_search(functionOf(ir, "Bank::mix"), vector));
}

TEST_CASE("a parallel for gives each iteration its rows, and a switch's jump zeroes a 2-D array it passes",
          "[codegen][array2d]")
{
    std::string errors;
    auto jit = compile("module Bank\n"
                       "{\n"
                       "    float gain[8][32];\n"
                       "    void fill()\n"
                       "    {\n"
                       "        parallel for (int v = 0; v < 8; ++v)\n"   // each iteration its own row
                       "            for (int i = 0; i < 32; ++i)\n"
                       "                gain[v][i] = v * 100 + i;\n"
                       "    }\n"
                       "};\n"
                       "float local()\n"
                       "{\n"
                       "    float g[4][4];\n"   // captured by reference, as every array
                       "    parallel for (int r = 0; r < 4; ++r)\n"
                       "        for (int c = 0; c < 4; ++c)\n"
                       "            g[r][c] = r * 4 + c;\n"
                       "    float s = 0;\n"
                       "    for (int r = 0; r < 4; ++r)\n"
                       "        s += g[r][r];\n"
                       "    return s;\n"   // 0 + 5 + 10 + 15
                       "}\n"
                       "float jump(int k)\n"
                       "{\n"
                       "    switch (k)\n"
                       "    {\n"
                       "        float g[2][2] = {{1, 2}, {3, 4}};\n"   // never runs: the jump passes it
                       "    case 1:\n"
                       "        return g[1][1];\n"
                       "    }\n"
                       "    return -1;\n"
                       "}\n",
                       &errors);
    INFO(errors);
    REQUIRE(jit);
    std::array<float, 8 * 32> bank{};
    lookup<void(void*)>(*jit, "Bank::fill")(bank.data());
    CHECK(bank[0] == 0.0f);
    CHECK(bank[7 * 32 + 31] == 731.0f);
    CHECK(bank[3 * 32 + 5] == 305.0f);
    CHECK(lookup<float()>(*jit, "local")() == 30.0f);
    CHECK(lookup<float(int)>(*jit, "jump")(1) == 0.0f);   // C leaves it indeterminate; FMinus starts it at 0
}

TEST_CASE("2-D arrays' errors are clang's", "[codegen][array2d]")
{
    CHECK(errorsOf("const float t[2][3] = {{1, 2}, {3}};\n"
                   "module M { float g[][4]; float h[2][]; float d[][]; float e[][-1]; };\n"
                   "float s() { return sizeof(float[][3]) + sizeof(float[2][]) + sizeof(bool[2][2])"
                   " + sizeof(float[-1]); }\n"
                   "float w()\n"
                   "{\n"
                   "    float g[2][3];\n"
                   "    float* p = g;\n"
                   "    g[0] = g[1];\n"
                   "    t[1][1] = 2;\n"
                   "    float* q = &g[1];\n"
                   "    bool flags[2][2];\n"
                   "    float big[1 << 16][1 << 15];\n"
                   "    return g[0][0];\n"
                   "}\n")
          == "test.fm:2:18: error: field has incomplete type 'float[][4]'\n"
             "test.fm:2:32: error: array has incomplete element type 'float[]'\n"
             "test.fm:2:46: error: array has incomplete element type 'float[]'\n"
             "test.fm:2:63: error: 'e' declared as an array with a negative size\n"
             "test.fm:3:20: error: invalid application of 'sizeof' to an incomplete type 'float[][3]'\n"
             "test.fm:3:41: error: array has incomplete element type 'float[]'\n"
             "test.fm:3:62: error: FMinus has arrays of float, double, int, float2, float4, float8 and modules, not of "
             "'bool'\n"
             "test.fm:3:96: error: array size is negative\n"
             "test.fm:7:12: error: incompatible pointer types initializing 'float *' with an expression of type "
             "'float (*)[3]'\n"
             "test.fm:8:10: error: array type 'float[3]' is not assignable\n"
             "test.fm:9:13: error: cannot assign to variable 't' with const-qualified type 'const float[2][3]'\n"
             "test.fm:10:16: error: FMinus has pointers to float, double, int, float2, float4, float8, void and "
             "modules, not to 'float[3]'\n"
             "test.fm:11:10: error: FMinus has arrays of float, double, int, float2, float4, float8 and modules, not "
             "of 'bool'\n"
             "test.fm:12:17: error: array is too large: FMinus's arrays have at most 2^30 elements, and this one has "
             "2147483648\n");
    // An initialiser list's errors, where the excess value or the brace is; after a row's size's own error, the list
    // reports nothing more (it is measured against no row the array has).
    CHECK(errorsOf("float f()\n"
                   "{\n"
                   "    float a[2][3] = {{1, 2, 3, 4}};\n"
                   "    float b[2][3] = {{1}, {2}, {3}};\n"
                   "    float c[2][3] = {1, {2}};\n"
                   "    float d[2][3] = {{{1}}};\n"
                   "    float e[3] = {{1}, 2};\n"
                   "    float g[2][3] = {1, 2, 3, 4, 5, 6, 7};\n"
                   "    float h[2][] = {1};\n"
                   "    float m[2][] = {{1, 2}, {3}, 4, 5};\n"
                   "    float p[][] = {{1, 2}, {3, 4}};\n"
                   "    float q[2][0] = {{1, 2}};\n"
                   "    float k[][3] = {};\n"
                   "    float n[2][3] = 5;\n"
                   "    const float t[2][2] = {{1, 2}, {3, 4}, {5}};\n"
                   "    return 0;\n"
                   "}\n"
                   "const float top[1][2] = {{1}, 2};\n"
                   "const float wide[2][] = {{1, 2}};\n")
          == "test.fm:3:32: error: excess elements in array initializer\n"
             "test.fm:4:32: error: excess elements in array initializer\n"
             "test.fm:5:25: error: braces around scalar initializer\n"
             "test.fm:6:23: error: braces around scalar initializer\n"
             "test.fm:7:19: error: braces around scalar initializer\n"
             "test.fm:8:40: error: excess elements in array initializer\n"
             "test.fm:9:11: error: array has incomplete element type 'float[]'\n"
             "test.fm:10:11: error: array has incomplete element type 'float[]'\n"
             "test.fm:11:11: error: array has incomplete element type 'float[]'\n"
             "test.fm:12:16: error: zero size arrays are not allowed\n"
             "test.fm:13:11: error: zero size arrays are not allowed\n"
             "test.fm:14:11: error: array initializer must be an initializer list\n"
             "test.fm:15:44: error: excess elements in array initializer\n"
             "test.fm:18:31: error: excess elements in array initializer\n"
             "test.fm:19:13: error: array has incomplete element type 'float[]'\n");
}
