// tests/smoke/GrammarSmokeTest.cpp: Bison and Flex through fminus_add_grammar: the calculator in smoke/calc/ parses,
// evaluates, reports errors with their locations and recovers. It came with the project skeleton.
#include "smoke/calc/CalcScanner.h"

#include <catch2/catch_test_macros.hpp>

#include <cstdint>
#include <string>
#include <vector>

using fminus::smoke::calc;

TEST_CASE("the calculator evaluates with precedence and associativity", "[smoke][grammar]")
{
    const auto result = calc("1 + 2 * 3;\n(4 - 6) * -2;\n100 / 7 / 2;");
    CHECK(result.errors.empty());
    CHECK(result.values == std::vector<std::int64_t>{7, 4, 7});
}

TEST_CASE("the calculator reports errors at their locations and recovers at ';'", "[smoke][grammar]")
{
    const auto result = calc("1 + ;\n2 $ 3;\n5 / 0;\n42;");
    REQUIRE(result.errors.size() == 3);
    CHECK(result.errors[0].rfind("1.5: syntax error", 0) == 0);
    CHECK(result.errors[1] == "2.3: unexpected character '$'");
    CHECK(result.errors[2] == "3.5: division by zero");
    CHECK(result.values == std::vector<std::int64_t>{0, 42});
}
