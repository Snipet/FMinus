// tests/unit/CodeGenTest.cpp: FMinus v0's code generator (src/ir/CodeGen.cpp), run through the JIT (src/jit/Jit.h).
#include "frontend/Frontend.h"
#include "ir/CodeGen.h"
#include "jit/Jit.h"

#include <catch2/catch_test_macros.hpp>

#include <llvm/IR/LLVMContext.h>
#include <llvm/IR/Module.h>
#include <llvm/Support/Error.h>

#include <cmath>
#include <memory>
#include <string>
#include <string_view>

namespace
{
    // The program JIT-compiled at -O2, or nothing with its errors in `errors`.
    std::unique_ptr<fminus::jit::Jit> compile(std::string_view source, std::string* errors = nullptr)
    {
        fminus::Diagnostics diagnostics("test.fm");
        const auto program = fminus::frontend::parse(source, diagnostics);
        auto context = std::make_unique<llvm::LLVMContext>();
        std::unique_ptr<llvm::Module> module = program ? fminus::ir::lower(*program, *context, diagnostics) : nullptr;
        if (errors != nullptr)
            *errors = diagnostics.format();
        if (!module)
            return nullptr;
        auto jit = llvm::cantFail(fminus::jit::Jit::create(llvm::OptimizationLevel::O2));
        llvm::cantFail(jit->add(std::move(module), std::move(context)));
        return jit;
    }

    template <typename Function>
    Function* lookup(fminus::jit::Jit& jit, std::string_view name)
    {
        return llvm::cantFail(jit.lookup(name)).toPtr<Function*>();
    }

    std::string errorsOf(std::string_view source)
    {
        std::string errors;
        CHECK(compile(source, &errors) == nullptr);
        return errors;
    }
} // namespace

TEST_CASE("functions compute what they say", "[codegen]")
{
    auto jit = compile("fn softclip(x: float, drive: float) -> float {\n"
                       "    let y = x * drive;\n"
                       "    return y / (1.0 + abs(y));\n"
                       "}\n"
                       "fn chain(x: float) -> float { return softclip(half(x), 4); }\n"   // calls before definitions
                       "fn half(x: float) -> float { return x / 2; }\n"
                       "fn constant() -> float { return -(1 - 3) * 2.5; }\n");
    REQUIRE(jit);
    CHECK(lookup<float(float, float)>(*jit, "softclip")(1.0f, 1.0f) == 0.5f);
    CHECK(lookup<float(float)>(*jit, "chain")(1.0f) == 2.0f / 3.0f);
    CHECK(lookup<float()>(*jit, "constant")() == 5.0f);
}

TEST_CASE("builtins lower to LLVM's intrinsics", "[codegen]")
{
    auto jit = compile("fn f(x: float) -> float { return sqrt(x) + abs(-x) + floor(x / 2) + min(x, 1) + max(x, 1); }\n"
                       "fn g(x: float) -> float { return sin(x) * sin(x) + cos(x) * cos(x) + log(exp(x)); }\n");
    REQUIRE(jit);
    CHECK(lookup<float(float)>(*jit, "f")(9.0f) == 3.0f + 9.0f + 4.0f + 1.0f + 9.0f);
    CHECK(std::abs(lookup<float(float)>(*jit, "g")(0.25f) - 1.25f) < 1e-6f);
    CHECK(fminus::ir::builtins().size() == 9);
}

TEST_CASE("a literal is rounded once, straight to float", "[codegen]")
{
    // Just above the midpoint of 1 and the next float: through double it would land on the midpoint and round down.
    auto jit = compile("fn f() -> float { return 1.0000000596046447753906250000000001; }");
    REQUIRE(jit);
    CHECK(lookup<float()>(*jit, "f")() == std::nextafter(1.0f, 2.0f));
}

TEST_CASE("every semantic error is reported, in source order", "[codegen]")
{
    CHECK(errorsOf("fn f(x: float, x: float) -> float {\n"
                   "    let y = x + z;\n"
                   "    let y = nope(x);\n"
                   "    return sqrt(x, y);\n"
                   "    return 1.0;\n"
                   "}\n"
                   "fn f(a: float) -> float { return a; }\n"
                   "fn sin(a: float) -> float { return a; }\n"
                   "fn g() -> float { let k = 1e60; }\n")
          == "test.fm:1:16: error: parameter 'x' is declared twice\n"
             "test.fm:2:17: error: unknown variable 'z'\n"
             "test.fm:3:5: error: 'y' is already defined\n"
             "test.fm:3:13: error: unknown function 'nope'\n"
             "test.fm:4:12: error: 'sqrt' takes 1 argument, not 2\n"
             "test.fm:5:5: error: statement after the return of 'f'\n"
             "test.fm:7:1: error: function 'f' is already defined\n"
             "test.fm:8:1: error: 'sin' is a builtin and cannot be defined\n"
             "test.fm:9:1: error: function 'g' does not end with a return\n"
             "test.fm:9:27: error: number '1e60' is out of range for float\n");
}
