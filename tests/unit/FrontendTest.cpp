// tests/unit/FrontendTest.cpp: FMinus v0's scanner and parser (src/frontend/), through the syntax tree's dump.
#include "frontend/Frontend.h"

#include <catch2/catch_test_macros.hpp>

#include <string>
#include <string_view>

namespace
{
    // The program's dump, or its errors.
    std::string parsed(std::string_view source)
    {
        fminus::Diagnostics diagnostics("test.fm");
        const auto program = fminus::frontend::parse(source, diagnostics);
        return program ? fminus::ast::dump(*program) : diagnostics.format();
    }
} // namespace

TEST_CASE("the parser builds functions, bindings and returns", "[frontend]")
{
    CHECK(parsed("fn gain(x: float, g: float) -> float {\n"
                 "    let y = x * g; // a comment\n"
                 "    return y;\n"
                 "}\n"
                 "fn one() -> float { return 1.0; }\n")
          == "(fn gain ((x float) (g float)) float\n"
             "  (let y (* x g))\n"
             "  (return y))\n"
             "(fn one () float\n"
             "  (return 1.0))\n");
    CHECK(parsed("") == "");
}

TEST_CASE("operators have the usual precedence and associativity", "[frontend]")
{
    CHECK(parsed("fn f(a: float, b: float, c: float) -> float { return a - b - c * a / b; }")
          == "(fn f ((a float) (b float) (c float)) float\n  (return (- (- a b) (/ (* c a) b))))\n");
    CHECK(parsed("fn f(a: float, b: float) -> float { return -a * -(a + b); }")
          == "(fn f ((a float) (b float)) float\n  (return (* (- a) (- (+ a b)))))\n");
}

TEST_CASE("calls and numbers keep their spelling", "[frontend]")
{
    CHECK(parsed("fn f() -> float { return max(min(1, .5), g(2e3, 1.e-2, 3.25E+1)); }")
          == "(fn f () float\n  (return (call max (call min 1 .5) (call g 2e3 1.e-2 3.25E+1))))\n");
}

TEST_CASE("syntax and scanner errors carry their locations", "[frontend]")
{
    CHECK(parsed("fn f(x: float) -> float {\n    return x + ;\n}")
          == "test.fm:2:16: error: syntax error, unexpected ;, expecting - or ( or identifier or number\n");
    CHECK(parsed("fn f(x: float) float { return x; }")
          == "test.fm:1:16: error: syntax error, unexpected float, expecting ->\n");
    CHECK(parsed("fn f(x: float) -> float {\n  return x $ 2;\n}") == "test.fm:2:12: error: unexpected character '$'\n");
}
