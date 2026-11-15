// tests/unit/HandleTest.cpp: the handle types (frontend/Ast.h): http_response, http_request, http_server, json_doc,
// xml_doc, file_content, file_listing and fft_plan, C's opaque handles. Through the JIT: a handle in a local, a
// parameter, a result, a field and an array, null until it is set, compared and tested, and converted to and from a
// void*, as a program that said void* before still does. Then the errors, in clang's words: a handle of the wrong kind,
// arithmetic, an element or a field of one, a pointer to one, and a prototype of a builtin.
#include "Compile.h"

#include "fminus/Config.h"

#include <catch2/catch_test_macros.hpp>

#include <string>
#include <vector>

using namespace fminus::test;

TEST_CASE("a handle is null until set, converts to and from a void*, compares and tests as a pointer",
          "[codegen][handles]")
{
    auto jit = compile("fft_plan make(int n) { return fft_create(n); }\n"
                       "void* made(int n) { fft_plan p = fft_create(n); return p; }\n"
                       "void drop(void* p) { fft_destroy(p); }\n"
                       "void release(fft_plan p) { fft_destroy(p); }\n"
                       "int same(fft_plan a, void* b) { return (a == b ? 1 : 0) + (b != a ? 2 : 0); }\n"
                       "int unset()\n"
                       "{\n"
                       "    fft_plan p;\n"
                       "    const fft_plan q[2];\n"
                       "    fft_plan grid[2][3];\n"
                       "    return (p ? 1 : 0) + (q[1] ? 2 : 0) + (p == q[0] ? 4 : 0) + (!grid[1][2] ? 8 : 0);\n"
                       "}\n"
                       "int transform(fft_plan p, const float* in, float2* out)\n"
                       "{\n"
                       "    if (!p)\n"
                       "        return 0;\n"
                       "    fft_forward(p, in, out);\n"
                       "    return 1;\n"
                       "}\n"
                       "void* pick(bool first, fft_plan a, void* b) { return first ? a : b; }\n"
                       "fft_plan either(bool first, fft_plan a, fft_plan b) { return first && a ? a : b; }\n"
                       "int several(int n)\n"
                       "{\n"
                       "    fft_plan plans[3];\n"
                       "    for (int i = 0; i < 3; ++i)\n"
                       "        plans[i] = fft_create(n * (i + 1));\n"
                       "    int made = 0;\n"
                       "    for (int i = 0; i < 3; ++i)\n"
                       "    {\n"
                       "        made += plans[i] != plans[(i + 1) % 3] && plans[i] ? 1 : 0;\n"
                       "        fft_destroy(plans[i]);\n"
                       "    }\n"
                       "    return made;\n"
                       "}\n"
                       "module Holder\n"
                       "{\n"
                       "    fft_plan plan;\n"
                       "    fft_plan spare[2][2];\n"
                       "    int init(int n)\n"
                       "    {\n"
                       "        plan = fft_create(n);\n"
                       "        spare[1][0] = plan;\n"
                       "        return (spare[1][0] == plan ? 1 : 0) + (spare[0][1] ? 2 : 0) + (plan ? 4 : 0);\n"
                       "    }\n"
                       "    void end() { fft_destroy(plan); }\n"
                       "};\n"
                       "int held(int n)\n"
                       "{\n"
                       "    Holder h;\n"
                       "    const int seen = h.init(n);\n"
                       "    h.end();\n"
                       "    return seen;\n"
                       "}\n");
    REQUIRE(jit != nullptr);
    auto* make = lookup<void*(int)>(*jit, "make");
    auto* made = lookup<void*(int)>(*jit, "made");
    auto* drop = lookup<void(void*)>(*jit, "drop");
    auto* release = lookup<void(void*)>(*jit, "release");
    auto* same = lookup<int(void*, void*)>(*jit, "same");
    auto* transform = lookup<int(void*, const float*, float2*)>(*jit, "transform");
    auto* pick = lookup<void*(bool, void*, void*)>(*jit, "pick");
    auto* either = lookup<void*(bool, void*, void*)>(*jit, "either");

    CHECK(lookup<int()>(*jit, "unset")() == 12);   // q[1] and p are null, so equal; grid[1][2] is null too
    void* plan = make(32);
    void* other = made(64);
    REQUIRE(plan != nullptr);
    REQUIRE(other != nullptr);
    CHECK(same(plan, plan) == 1);
    CHECK(same(plan, other) == 2);
    CHECK(pick(true, plan, other) == plan);
    CHECK(pick(false, plan, other) == other);
    CHECK(either(true, plan, other) == plan);
    CHECK(either(true, nullptr, other) == other);

    // A plan of 32 floats through a typed parameter: a constant's spectrum is its sum at bin 0.
    std::vector<float> in(32, 1.0f);
    std::vector<float2> out(17);
    CHECK(transform(plan, in.data(), out.data()) == 1);
    CHECK(out[0].x == 32.0f);
    CHECK(transform(nullptr, in.data(), out.data()) == 0);
    release(plan);
    drop(other);

    CHECK(lookup<int(int)>(*jit, "several")(32) == 3);
    CHECK(lookup<int(int)>(*jit, "held")(32) == 5);
}

#if FMINUS_HAS_JSON
TEST_CASE("a json_doc is what json_parse gives and the reads take; a void* or a const void* still works",
          "[codegen][handles][json]")
{
    auto jit = compile("json_doc json_parse(const void* data, int bytes);\n"   // the builtin's own types
                       "void json_free(void* doc);\n"                          // and as they were written before
                       "void json_free(json_doc doc);\n"
                       "float typed(const void* data, int bytes)\n"
                       "{\n"
                       "    json_doc doc = json_parse(data, bytes);\n"
                       "    const float x = json_float(doc, \"/x\", -1.0f);\n"
                       "    json_free(doc);\n"
                       "    return x;\n"
                       "}\n"
                       "float untyped(const void* data, int bytes)\n"
                       "{\n"
                       "    void* doc = json_parse(data, bytes);\n"
                       "    const void* read = doc;\n"
                       "    const float x = json_float(read, \"/x\", -1.0f) + json_float(doc, \"/x\", -1.0f);\n"
                       "    json_free(doc);\n"
                       "    return x;\n"
                       "}\n"
                       "int kept(const void* data, int bytes)\n"
                       "{\n"
                       "    json_doc docs[2];\n"
                       "    docs[1] = json_parse(data, bytes);\n"
                       "    const int n = docs[0] ? -1 : json_int(docs[1], \"/n\", 0);\n"
                       "    json_free(docs[1]);\n"
                       "    return n;\n"
                       "}\n");
    REQUIRE(jit != nullptr);
    const std::string text = R"({"x": 2.5, "n": 7})";
    const int bytes = static_cast<int>(text.size());
    CHECK(lookup<float(const void*, int)>(*jit, "typed")(text.data(), bytes) == 2.5f);
    CHECK(lookup<float(const void*, int)>(*jit, "untyped")(text.data(), bytes) == 5.0f);
    CHECK(lookup<int(const void*, int)>(*jit, "kept")(text.data(), bytes) == 7);
}
#endif

TEST_CASE("a handle has no arithmetic, element, field or pointer to it: clang's errors, each once",
          "[codegen][handles]")
{
    CHECK(errorsOf("int f(fft_plan p, const void* c, void* v)\n"
                   "{\n"
                   "    fft_plan a = p + 1;\n"
                   "    p++;\n"
                   "    int n = p[0] != v;\n"
                   "    n += *p;\n"
                   "    n += p->size;\n"
                   "    n += p < p;\n"
                   "    n += 1 - p;\n"
                   "    n += -p;\n"
                   "    n += (int)p;\n"
                   "    int k = p;\n"
                   "    fft_plan q = c;\n"
                   "    fft_plan* r;\n"
                   "    switch (p) { default: break; }\n"
                   "    return n + k + p->count();\n"
                   "}\n"
                   "float g(fft_plan plans[4]) { return 0; }\n")
          == "test.fm:3:20: error: arithmetic on a pointer to an incomplete type 'struct fminus_fft_plan'\n"
             "test.fm:4:6: error: arithmetic on a pointer to an incomplete type 'struct fminus_fft_plan'\n"
             "test.fm:5:14: error: subscript of pointer to incomplete type 'struct fminus_fft_plan'\n"
             "test.fm:6:10: error: incomplete type 'struct fminus_fft_plan' where a complete type is required\n"
             "test.fm:7:11: error: incomplete definition of type 'struct fminus_fft_plan'\n"
             "test.fm:8:12: error: invalid operands to binary expression ('fft_plan' and 'fft_plan')\n"
             "test.fm:9:12: error: invalid operands to binary expression ('int' and 'fft_plan')\n"
             "test.fm:10:10: error: invalid argument type 'fft_plan' to unary expression\n"
             "test.fm:11:10: error: cannot cast from 'fft_plan' to 'int'\n"
             "test.fm:12:9: error: initializing 'int' with an expression of incompatible type 'fft_plan'\n"
             "test.fm:13:14: error: initializing 'fft_plan' with an expression of type 'const void *' discards "
             "qualifiers\n"
             "test.fm:14:15: error: FMinus has pointers to float, double, int, float2, float4, float8, char, "
             "void and modules, not to 'fft_plan'\n"
             "test.fm:15:13: error: statement requires expression of integer type ('fft_plan' invalid)\n"
             "test.fm:16:23: error: incomplete definition of type 'struct fminus_fft_plan'\n"
             "test.fm:18:18: error: FMinus has pointers to float, double, int, float2, float4, float8, char, "
             "void and modules, not to 'fft_plan'\n");
    // An array of void* is no array of handles, and &p would be a pointer to one.
    CHECK(errorsOf("void f() { void* plans[2]; }\n") == "test.fm:1:18: error: FMinus has no arrays of pointers\n");
    CHECK(errorsOf("void f(fft_plan p) { void* q = &p; }\n")
          == "test.fm:1:32: error: FMinus has pointers to float, double, int, float2, float4, float8, char, void and "
             "modules, not to 'fft_plan'\n");
#if FMINUS_HAS_JSON && FMINUS_HAS_XML
    // A handle of another kind: C's incompatible pointer types, naming both.
    CHECK(errorsOf("float f(json_doc doc, fft_plan p, bool c)\n"
                   "{\n"
                   "    xml_doc x = doc;\n"
                   "    x = p;\n"
                   "    float v = xml_float(doc, \"/a\", 0.0f) + (doc == x ? 1.0f : 0.0f);\n"
                   "    fft_destroy(doc);\n"
                   "    void* either = c ? doc : x;\n"
                   "    return v;\n"
                   "}\n"
                   "xml_doc g(json_doc d) { return d; }\n")
          == "test.fm:3:13: error: incompatible pointer types initializing 'xml_doc' with an expression of type "
             "'json_doc'\n"
             "test.fm:4:7: error: incompatible pointer types assigning to 'xml_doc' from 'fft_plan'\n"
             "test.fm:5:15: error: incompatible pointer types passing 'json_doc' to parameter of type 'xml_doc'\n"
             "test.fm:5:49: error: comparison of distinct pointer types ('json_doc' and 'xml_doc')\n"
             "test.fm:6:5: error: incompatible pointer types passing 'json_doc' to parameter of type 'fft_plan'\n"
             "test.fm:7:22: error: incompatible operand types ('json_doc' and 'xml_doc')\n"
             "test.fm:10:25: error: incompatible pointer types returning 'json_doc' from a function with result "
             "type 'xml_doc'\n");
    // A prototype of a builtin says its handle, or void* as before; another handle conflicts.
    CHECK(errorsOf("void json_free(xml_doc doc);\nxml_doc json_parse(const void* data, int bytes);\n")
          == "test.fm:1:6: error: conflicting types for 'json_free'\n"
             "test.fm:2:9: error: conflicting types for 'json_parse'\n");
#endif
#if FMINUS_HAS_WEB_SERVER && FMINUS_HAS_FILES
    // A builtin that takes either of two kinds says both when given a third.
    CHECK(errorsOf("int f(json_doc doc, http_request q, http_server s, file_listing l)\n"
                   "{\n"
                   "    http_close(q);\n"
                   "    file_close(s);\n"
                   "    return http_size(doc) + http_size(q) + file_status(l) + http_port(q);\n"
                   "}\n")
          == "test.fm:3:5: error: incompatible pointer types passing 'http_request' to parameter of type "
             "'http_response' or 'http_server'\n"
             "test.fm:4:5: error: incompatible pointer types passing 'http_server' to parameter of type "
             "'file_content' or 'file_listing'\n"
             "test.fm:5:12: error: incompatible pointer types passing 'json_doc' to parameter of type "
             "'http_response' or 'http_request'\n"
             "test.fm:5:61: error: incompatible pointer types passing 'http_request' to parameter of type "
             "'http_server'\n");
#endif
}
