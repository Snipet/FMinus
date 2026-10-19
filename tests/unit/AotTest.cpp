// tests/unit/AotTest.cpp: an executable's entry (src/support/Aot.h: addEntryPoint). The lit test fminus/main.fm builds
// and runs executables; this checks the IR the entry adds, and what it refuses.
#include "support/Aot.h"

#include <catch2/catch_test_macros.hpp>

#include <llvm/AsmParser/Parser.h>
#include <llvm/IR/LLVMContext.h>
#include <llvm/IR/Module.h>
#include <llvm/Support/Error.h>
#include <llvm/Support/SourceMgr.h>

#include <memory>
#include <string>

namespace
{
    std::unique_ptr<llvm::Module> parse(llvm::LLVMContext& context, const char* ir)
    {
        llvm::SMDiagnostic diagnostic;
        std::unique_ptr<llvm::Module> module = llvm::parseAssemblyString(ir, diagnostic, context);
        REQUIRE(module);
        return module;
    }

    // addEntryPoint's error, or "" when it succeeds.
    std::string entryError(const char* ir)
    {
        llvm::LLVMContext context;
        std::unique_ptr<llvm::Module> module = parse(context, ir);
        llvm::Error error = fminus::aot::addEntryPoint(*module);
        return error ? llvm::toString(std::move(error)) : "";
    }
} // namespace

TEST_CASE("the program's main becomes internal, and a C main runs it", "[aot]")
{
    llvm::LLVMContext context;
    std::unique_ptr<llvm::Module> module =
        parse(context, "define float @main(float %a, float %b) {\n  %s = fadd float %a, %b\n  ret float %s\n}\n");
    REQUIRE_FALSE(llvm::errorToBool(fminus::aot::addEntryPoint(*module)));

    const llvm::Function* program = module->getFunction("fminus.main");
    REQUIRE(program != nullptr);
    CHECK(program->hasInternalLinkage());
    CHECK(program->arg_size() == 2);

    const llvm::Function* entry = module->getFunction("main");
    REQUIRE(entry != nullptr);
    CHECK(entry->hasExternalLinkage());
    CHECK(entry->getReturnType()->isIntegerTy(32));
    REQUIRE(entry->arg_size() == 2);
    CHECK(entry->getArg(0)->getType()->isIntegerTy(32));
    CHECK(entry->getArg(1)->getType()->isPointerTy());
    CHECK(module->getFunction("fminus_rt_main_args") != nullptr);
    CHECK(module->getFunction("fminus_rt_main_result") != nullptr);
}

TEST_CASE("a bool main's result is printed as true or false", "[aot]")
{
    llvm::LLVMContext context;
    std::unique_ptr<llvm::Module> module =
        parse(context, "define zeroext i1 @main(float %a, float %b) {\n  %e = fcmp oeq float %a, %b\n  ret i1 %e\n}\n");
    REQUIRE_FALSE(llvm::errorToBool(fminus::aot::addEntryPoint(*module)));
    const llvm::Function* print = module->getFunction("fminus_rt_main_bool_result");
    REQUIRE(print != nullptr);
    CHECK(print->getArg(0)->getType()->isIntegerTy(1));
    CHECK(print->hasParamAttribute(0, llvm::Attribute::ZExt));   // the C ABI passes a bool zero-extended
    CHECK(module->getFunction("fminus_rt_main_result") == nullptr);
}

TEST_CASE("an executable needs a main that takes floats and returns a float or a bool", "[aot]")
{
    const std::string missing = "an executable needs a main function, such as float main(float x)";
    const std::string signature = "'main' must return float or bool and take only float parameters";
    CHECK(entryError("define float @f() {\n  ret float 1.0\n}\n") == missing);
    CHECK(entryError("declare float @main(float)\n") == missing);   // a prototype is not a program
    CHECK(entryError("define i1 @main() {\n  ret i1 true\n}\n").empty());
    CHECK(entryError("define i32 @main() {\n  ret i32 0\n}\n") == signature);
    CHECK(entryError("define float @main(i1 %b) {\n  ret float 1.0\n}\n") == signature);
    CHECK(entryError("define float @main(<4 x float> %v) {\n  ret float 1.0\n}\n") == signature);
    CHECK(entryError("define float @main(...) {\n  ret float 1.0\n}\n") == signature);
}
