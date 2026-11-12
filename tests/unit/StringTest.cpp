// tests/unit/StringTest.cpp: string literals as builtins' arguments (extending.md, recipe 38): a literal's bytes reach
// the runtime as an address and a length, a NUL among them, its escapes resolved and adjacent literals joined; a
// format's places for ints, checked as clang checks printf's, at the columns clang reports; a literal anywhere else, an
// error at it in clang's words; the effects' third class, a builtin that blocks; and the builtins of a build option
// this build leaves out, still known names. The builtins that take a literal here are scaffolding,
// __fminus_string_hash and __fminus_string_hash_blocking (ir/Calls.cpp): the hash of the text a format and its ints
// make, which this file computes again in C++. They stay while the real ones are each a build option's (http_wait, the
// one real builtin that blocks, is FMINUS_WEB's, off by default): these test both in every build (Calls.cpp says so).
#include "Compile.h"
#include "fminus/Config.h"
#include "frontend/Preprocessor.h"
#include "ir/Lowering.h"

#include <catch2/catch_test_macros.hpp>

#include <climits>
#include <cstdint>
#include <string>
#include <string_view>
#include <utility>

using namespace fminus::test;

namespace
{
    // The runtime's hash (runtime/Bytes.cpp): FNV-1a, 32 bits, of the text.
    std::int32_t fnv1a(std::string_view text)
    {
        std::uint32_t hash = 2166136261u;
        for (const char c : text)
            hash = (hash ^ static_cast<unsigned char>(c)) * 16777619u;
        return static_cast<std::int32_t>(hash);
    }
} // namespace

TEST_CASE("a string literal reaches the runtime as its bytes and their length: a NUL is one of them",
          "[codegen][strings]")
{
    std::string errors;
    auto jit = compile("int plain() { return __fminus_string_hash(\"cutoff\"); }\n"
                       "int empty() { return __fminus_string_hash(\"\"); }\n"
                       "int nul() { return __fminus_string_hash(\"a\\0b\"); }\n"
                       "int joined() { return __fminus_string_hash(\"/osc\" \"/\" \"detune\"); }\n"
                       "int escapes() { return __fminus_string_hash(\"\\x41\\101\\u00B5\\n\\\"\"); }\n"
                       "int places(int k, int j) { return __fminus_string_hash(\"/osc/%d/%d\", k, j); }\n"
                       "int percent() { return __fminus_string_hash(\"100%% wet\"); }\n"
                       "int split(int k) { return __fminus_string_hash(\"%\" \"d%\" \"%\", k); }\n"
                       "enum Mode { A, B, C };\n"
                       "int converted(enum Mode m, bool b) { return __fminus_string_hash(\"%d,%d\", m, b); }\n"
                       "int inside(int n) [[clang::nonblocking]]\n"   // wait-free: the audio thread may call it
                       "{\n"
                       "    int sum = 0;\n"
                       "    for (int i = 0; i < n; ++i)\n"
                       "        sum += __fminus_string_hash(\"%d\", i) & 1;\n"
                       "    return sum;\n"
                       "}\n",
                       &errors);
    INFO(errors);
    REQUIRE(jit);
    CHECK(lookup<int()>(*jit, "plain")() == fnv1a("cutoff"));
    CHECK(lookup<int()>(*jit, "empty")() == fnv1a(""));
    CHECK(lookup<int()>(*jit, "nul")() == fnv1a(std::string_view("a\0b", 3)));
    CHECK(lookup<int()>(*jit, "joined")() == fnv1a("/osc/detune"));
    CHECK(lookup<int()>(*jit, "escapes")() == fnv1a("AA\xC2\xB5\n\""));
    const auto places = lookup<int(int, int)>(*jit, "places");
    CHECK(places(2, 17) == fnv1a("/osc/2/17"));
    CHECK(places(-3, 0) == fnv1a("/osc/-3/0"));
    CHECK(places(INT_MIN, INT_MAX) == fnv1a("/osc/-2147483648/2147483647"));
    CHECK(lookup<int()>(*jit, "percent")() == fnv1a("100% wet"));
    CHECK(lookup<int(int)>(*jit, "split")(9) == fnv1a("9%"));   // a place may span adjacent literals, as in C
    CHECK(lookup<int(int, bool)>(*jit, "converted")(2, true) == fnv1a("2,1"));
    int odd = 0;
    for (int i = 0; i < 10; ++i)
        odd += fnv1a(std::to_string(i)) & 1;
    CHECK(lookup<int(int)>(*jit, "inside")(10) == odd);
}

TEST_CASE("a format's places are %d and %%, checked as clang checks printf's", "[strings]")
{
    using fminus::ir::detail::scanFormat;
    CHECK(scanFormat("/osc/%d/detune").places == std::vector<std::size_t>{5});
    CHECK(scanFormat("%d%d").places == std::vector<std::size_t>{0, 2});
    CHECK(scanFormat("100%% %d").places == std::vector<std::size_t>{6});
    CHECK(scanFormat("%%d").places.empty());   // a percent sign, then a d
    CHECK(scanFormat("").places.empty());
    const auto errors = scanFormat("%f %5d %");
    REQUIRE(errors.errors.size() == 3);
    CHECK(errors.errors[0].offset == 1);
    CHECK(errors.errors[0].message == "invalid conversion specifier 'f'");
    CHECK(errors.errors[1].offset == 4);
    CHECK(errors.errors[1].message == "invalid conversion specifier '5d'");
    CHECK(errors.errors[2].offset == 7);
    CHECK(errors.errors[2].message == "incomplete format specifier");
    CHECK(scanFormat(std::string_view("%\x01", 2)).errors.at(0).message == "invalid conversion specifier '\\x01'");

    // Flags, a width, a precision and a length modifier, as clang reads them: a format that ends in them is
    // incomplete, at the %, and a conversion C has no specifier for is invalid, at it.
    for (const char* incomplete : {"%q", "%l", "%hh", "%-", "%5", "%5.3", "%.*", "%I", "%w", "%t"})
    {
        INFO(incomplete);
        const auto scan = scanFormat(incomplete);
        REQUIRE(scan.errors.size() == 1);
        CHECK(scan.errors[0].offset == 0);
        CHECK(scan.errors[0].message == "incomplete format specifier");
    }
    const auto invalid = [](std::string_view text) {
        const auto scan = scanFormat(text);
        REQUIRE(scan.errors.size() == 1);
        return std::pair(scan.errors[0].offset, scan.errors[0].message);
    };
    CHECK(invalid("%y") == std::pair<std::size_t, std::string>(1, "invalid conversion specifier 'y'"));
    CHECK(invalid("%5y") == std::pair<std::size_t, std::string>(2, "invalid conversion specifier 'y'"));
    CHECK(invalid("%lq") == std::pair<std::size_t, std::string>(2, "invalid conversion specifier 'q'"));
    CHECK(invalid("%lll") == std::pair<std::size_t, std::string>(3, "invalid conversion specifier 'l'"));
    CHECK(invalid("%h-d") == std::pair<std::size_t, std::string>(2, "invalid conversion specifier '-'"));
    CHECK(invalid("%I6") == std::pair<std::size_t, std::string>(2, "invalid conversion specifier '6'"));
    // C's own conversions that are not a plain %d: no meaning here, all of the specifier named, after the %.
    CHECK(invalid("%ld") == std::pair<std::size_t, std::string>(1, "invalid conversion specifier 'ld'"));
    CHECK(invalid("%-05.3hhx") == std::pair<std::size_t, std::string>(1, "invalid conversion specifier '-05.3hhx'"));
    CHECK(invalid("%s") == std::pair<std::size_t, std::string>(1, "invalid conversion specifier 's'"));
    CHECK(invalid("%5%") == std::pair<std::size_t, std::string>(1, "invalid conversion specifier '5%'"));
    CHECK(scanFormat("%d%%%d").places == std::vector<std::size_t>{0, 4});
}

TEST_CASE("a format's errors are clang's for printf's, at the columns clang reports", "[codegen][strings]")
{
    CHECK(errorsOf("int a(int n) { return __fminus_string_hash(\"a%y\", n); }\n")   // n is %y's, for clang
          == "test.fm:1:47: error: invalid conversion specifier 'y'\n"
             "test.fm:1:47: note: a format here takes %d, an int, and %%, a percent sign\n");
    // q is a length modifier (BSD's long long) to clang: a format that ends after one is incomplete, at the %.
    CHECK(errorsOf("int a(int n) { return __fminus_string_hash(\"a%q\", n); }\n")
          == "test.fm:1:46: error: incomplete format specifier\n"
             "test.fm:1:46: note: a format here takes %d, an int, and %%, a percent sign\n");
    CHECK(errorsOf("int a(int n) { return __fminus_string_hash(\"a%5ld\", n); }\n")
          == "test.fm:1:47: error: invalid conversion specifier '5ld'\n"
             "test.fm:1:47: note: a format here takes %d, an int, and %%, a percent sign\n");
    CHECK(errorsOf("int b() { return __fminus_string_hash(\"ab%\"); }\n")
          == "test.fm:1:42: error: incomplete format specifier\n"
             "test.fm:1:42: note: a format here takes %d, an int, and %%, a percent sign\n");
    // Too few: at the first place left without an int, its d (clang's caret); in a second literal, there.
    CHECK(errorsOf("int c(int n) { return __fminus_string_hash(\"%d/\"\n"
                   "                                          \"%d\", n); }\n")
          == "test.fm:2:45: error: more '%' conversions than data arguments\n");
    // Too many: at the first int left over.
    CHECK(errorsOf("int d(int n) { return __fminus_string_hash(\"%d\", n, n + 1); }\n")
          == "test.fm:1:53: error: data argument not used by format string\n");
    // An int, an enumeration's or a bool's, is what %d reads: anything else is clang's -Wformat, here an error.
    CHECK(errorsOf("int e(float x, int* p) { return __fminus_string_hash(\"%d %d\", x, p); }\n")
          == "test.fm:1:63: error: format specifies type 'int' but the argument has type 'float'\n"
             "test.fm:1:66: error: format specifies type 'int' but the argument has type 'int *'\n");
    // An escape's bytes are at its backslash. (An int a wrong place was to take is no int left over, as in clang.)
    CHECK(errorsOf("int f() { return __fminus_string_hash(\"\\x25\\x79\"); }\n")
          == "test.fm:1:44: error: invalid conversion specifier 'y'\n"
             "test.fm:1:44: note: a format here takes %d, an int, and %%, a percent sign\n");
    // A literal for %d is C's char array, a char* to printf: clang's -Wformat, at it.
    CHECK(errorsOf("int i(int n) { return __fminus_string_hash(\"%d\", \"s\"); }\n")
          == "test.fm:1:50: error: format specifies type 'int' but the argument has type 'char *'\n");
    // Where a literal is required, anything else is an error, and so is a call without one.
    CHECK(errorsOf("int g(int n) { return __fminus_string_hash(n); }\n")
          == "test.fm:1:44: error: argument to '__fminus_string_hash' must be a string literal\n");
    CHECK(errorsOf("int h() { return __fminus_string_hash(); }\n")
          == "test.fm:1:18: error: too few arguments to function call, expected at least 1, have 0\n");
}

TEST_CASE("a string literal anywhere but a builtin's string parameter is an error, where clang reports it",
          "[codegen][strings]")
{
    // Where C would convert a char array with clang's warning, or refuse it: clang's words, as errors, at the
    // declarator, the assignment's operator, or the literal passed or returned, as clang reports them.
    CHECK(errorsOf("float f(float x) { return x; }\n"
                   "int k(int n) { return n; }\n"
                   "float* q(float* p) { return p; }\n"
                   "float a() { float x = \"ab\"; return x; }\n"
                   "int b() { int n = \"ab\"; return n; }\n"
                   "float c() { return f(\"x\"); }\n"
                   "int d() { return k(\"x\"); }\n"
                   "float* e() { return q(\"x\"); }\n"
                   "float g() { return sinf(\"x\"); }\n"
                   "float h() { float y; y = \"a\"; return y; }\n"
                   "float i() { return \"a\"; }\n"
                   "module M { float m(float x) { return x; } };\n"
                   "float j(M* p) { return p->m(\"a\"); }\n")
          == "test.fm:4:19: error: initializing 'float' with an expression of incompatible type 'char[3]'\n"
             "test.fm:5:15: error: incompatible pointer to integer conversion initializing 'int' with an expression "
             "of type 'char[3]'\n"
             "test.fm:6:22: error: passing 'char[2]' to parameter of incompatible type 'float'\n"
             "test.fm:7:20: error: incompatible pointer to integer conversion passing 'char[2]' to parameter of type "
             "'int'\n"
             "test.fm:8:23: error: incompatible pointer types passing 'char[2]' to parameter of type 'float *'\n"
             "test.fm:9:25: error: passing 'char[2]' to parameter of incompatible type 'float'\n"
             "test.fm:10:24: error: assigning to 'float' from incompatible type 'char[2]'\n"
             "test.fm:11:20: error: returning 'char[2]' from a function with incompatible result type 'float'\n"
             "test.fm:13:29: error: passing 'char[2]' to parameter of incompatible type 'float'\n");
    // Where C would take it without a word (a void*, a bool, a condition, an operand), FMinus has no type for it.
    const std::string none =
        "error: FMinus has no strings: a string literal is only the argument of a builtin's string parameter\n";
    CHECK(errorsOf("int a(float* x) { return bytes_floats(\"ab\", 2, x, 1); }\n") == "test.fm:1:39: " + none);
    CHECK(errorsOf("bool b() { bool t = \"a\"; return t; }\n") == "test.fm:1:21: " + none);
    CHECK(errorsOf("float c() { if (\"a\") return 1; return 0; }\n") == "test.fm:1:17: " + none);
    CHECK(errorsOf("float d() { return \"ab\" * 2; }\n") == "test.fm:1:20: " + none);
    CHECK(errorsOf("float e() { return \"ab\"[0]; }\n") == "test.fm:1:20: " + none);
    CHECK(errorsOf("float f() { return (float)\"a\"; }\n") == "test.fm:1:27: " + none);
    CHECK(errorsOf("const float k = \"a\";\nfloat g() { return k; }\n") == "test.fm:1:17: " + none);
    CHECK(errorsOf("float h() { float t[2] = {1, \"a\"}; return t[0]; }\n") == "test.fm:1:30: " + none);
    // A builtin's int parameter (a size, a plan's length, hash's and randf's), and a subscript: clang's words for an
    // int's, and nothing more from the builtin.
    const std::string toInt =
        "error: incompatible pointer to integer conversion passing 'char[2]' to parameter of type 'int'\n";
    CHECK(errorsOf("int i(int n) { return hash(\"a\"); }\n") == "test.fm:1:28: " + toInt);
    CHECK(errorsOf("float j() { return randf(\"a\"); }\n") == "test.fm:1:26: " + toInt);
    CHECK(errorsOf("void k() { void* p = fft_create(\"a\"); fft_destroy(p); }\n") == "test.fm:1:33: " + toInt);
    CHECK(errorsOf("void l() { void* p = fft_create_complex(\"a\"); fft_destroy(p); }\n")
          == "test.fm:1:41: " + toInt);
    CHECK(errorsOf("void m() { void* p = calloc(4, \"a\"); free(p); }\n") == "test.fm:1:32: " + toInt);
    CHECK(errorsOf("void n() { free(\"a\"); }\n") == "test.fm:1:17: " + none);   // a void*
    CHECK(errorsOf("float o(float* p) { return p[\"a\"]; }\n")
          == "test.fm:1:29: error: array subscript is not an integer\n");
    CHECK(errorsOf("float p() { return reduce_add(\"a\"); }\n") == "test.fm:1:31: " + none);
    CHECK(errorsOf("void q(void* plan, float2* out) { fft_forward(plan, \"a\", out); }\n") == "test.fm:1:53: " + none);
    // An integer constant expression has none either.
    CHECK(errorsOf("int j(int n) { switch (n) { case \"a\": return 1; } return 0; }\n")
          == "test.fm:1:34: error: expression is not an integer constant expression\n");
}

TEST_CASE("a builtin that blocks is refused in a nonblocking function, and allowed in a nonallocating one",
          "[codegen][strings][effects]")
{
    // The effects' three classes: a builtin allocates (fft_create), blocks (http_wait, and the scaffolding), or is wait-free.
    using namespace fminus::ir::detail;
    CHECK(allocates("fft_create"));
    CHECK(!blocks("fft_create"));
    CHECK(blocks("__fminus_string_hash_blocking"));
    CHECK(!allocates("__fminus_string_hash_blocking"));
    for (const char* waitFree : {"bytes_floats", "bytes_pcm16", "__fminus_string_hash", "fft_forward"})
    {
        INFO(waitFree);
        CHECK(!allocates(waitFree));
        CHECK(!blocks(waitFree));
    }

    std::string errors;
    auto jit = compile("int wait() [[clang::nonallocating]] { return __fminus_string_hash_blocking(\"x\"); }\n",
                       &errors);
    INFO(errors);
    REQUIRE(jit);
    CHECK(lookup<int()>(*jit, "wait")() == fnv1a("x"));
    CHECK(errorsOf("int helper() { return __fminus_string_hash_blocking(\"x\"); }\n"
                   "int process() [[clang::nonblocking]] { return helper() + __fminus_string_hash_blocking(\"y\"); }\n")
          == "test.fm:2:47: error: function with 'nonblocking' attribute must not call non-'nonblocking' function "
             "'helper'\n"
             "test.fm:1:23: note: function cannot be inferred 'nonblocking' because it calls non-'nonblocking' "
             "function '__fminus_string_hash_blocking'\n"
             "test.fm:2:58: error: function with 'nonblocking' attribute must not call non-'nonblocking' function "
             "'__fminus_string_hash_blocking'\n");
}

TEST_CASE("a builtin of a build option left out is a known name, and its call names the option", "[strings]")
{
    // The json_ builtins need FMINUS_JSON (JsonTest.cpp), the xml_ ones FMINUS_XML (XmlTest.cpp) and the http_ ones
    // FMINUS_WEB (HttpTest.cpp): the message, and what this build has, as fminus/Config.h says.
    using namespace fminus::ir::detail;
    CHECK(notBuilt("json_float", Feature::Json)
          == "'json_float' needs the JSON parser, which this build of FMinus leaves out (FMINUS_JSON=OFF)");
    CHECK(notBuilt("xml_float", Feature::Xml)
          == "'xml_float' needs the XML parser, which this build of FMinus leaves out (FMINUS_XML=OFF)");
    CHECK(isBuilt(Feature::None));
    CHECK(isBuilt(Feature::Json) == (FMINUS_HAS_JSON != 0));
    CHECK(isBuilt(Feature::Xml) == (FMINUS_HAS_XML != 0));
    CHECK(notBuilt("http_get", Feature::Web)
          == "'http_get' needs the web library, which this build of FMinus leaves out (FMINUS_WEB=OFF)");
    CHECK(isBuilt(Feature::Web) == (FMINUS_HAS_WEB != 0));
    CHECK(notBuilt("http_listen", Feature::WebServer)
          == "'http_listen' needs the web server, which this build of FMinus leaves out (FMINUS_WEB_SERVER=OFF)");
    CHECK(isBuilt(Feature::WebServer) == (FMINUS_HAS_WEB_SERVER != 0));

    // A program's macros for them say the same: defined, to 1, where the feature is built in.
    const auto predefined = fminus::frontend::predefinedMacros();
    REQUIRE(predefined.size() == 6);
    CHECK(predefined[0].name == "__FMINUS_JSON__");
    CHECK(predefined[0].defined == (FMINUS_HAS_JSON != 0));
    CHECK(predefined[1].name == "__FMINUS_XML__");
    CHECK(predefined[1].defined == (FMINUS_HAS_XML != 0));
    CHECK(predefined[2].name == "__FMINUS_WEB__");
    CHECK(predefined[2].defined == (FMINUS_HAS_WEB != 0));
    CHECK(predefined[3].name == "__FMINUS_WEB_SERVER__");
    CHECK(predefined[3].defined == (FMINUS_HAS_WEB_SERVER != 0));
    CHECK(predefined[4].name == "__FMINUS_FILES__");
    CHECK(predefined[4].defined == (FMINUS_HAS_FILES != 0));
    CHECK(predefined[5].name == "__FMINUS_FILES_WRITE__");
    CHECK(predefined[5].defined == (FMINUS_HAS_FILES_WRITE != 0));
}

TEST_CASE("a prototype of a runtime builtin declares nothing new, and a definition is refused", "[codegen][strings]")
{
    std::string errors;
    auto jit = compile("int bytes_floats(const void* data, int bytes, float* out, int cap);\n"
                       "int count(float* x, int n) { return bytes_floats(x, 4 * n, x, 0); }\n",
                       &errors);
    INFO(errors);
    REQUIRE(jit);
    float x[3] = {};
    CHECK(lookup<int(float*, int)>(*jit, "count")(x, 3) == 3);
    CHECK(errorsOf("int bytes_pcm16(void* data, int bytes, float* out, int cap);\n"
                   "int bytes_floats(const void* data, int bytes, float* out);\n"
                   "int __fminus_string_hash(const void* text);\n"
                   "int bytes_floats(const void* d, int b, float* o, int c) { return 0; }\n")
          == "test.fm:1:5: error: conflicting types for 'bytes_pcm16'\n"
             "test.fm:2:5: error: conflicting types for 'bytes_floats'\n"
             "test.fm:3:5: error: conflicting types for '__fminus_string_hash'\n"
             "test.fm:4:5: error: 'bytes_floats' is a builtin and cannot be redefined\n");
}
