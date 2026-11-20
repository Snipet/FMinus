// tests/unit/AotTest.cpp: an executable's entry (src/support/Aot.h: addEntryPoint). The lit test fminus/main.fm builds
// and runs executables; this checks the IR the entry adds, and what it refuses.
#include "support/Aot.h"

#include <catch2/catch_test_macros.hpp>

#include <llvm/AsmParser/Parser.h>
#include <llvm/IR/Constants.h>
#include <llvm/IR/GlobalVariable.h>
#include <llvm/IR/Instructions.h>
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

TEST_CASE("a main with a double reads each argument as its type says, and prints a double result", "[aot]")
{
    llvm::LLVMContext context;
    std::unique_ptr<llvm::Module> module = parse(
        context, "define double @main(double %t, float %x) {\n  %w = fpext float %x to double\n"
                 "  %s = fadd double %t, %w\n  ret double %s\n}\n");
    REQUIRE_FALSE(llvm::errorToBool(fminus::aot::addEntryPoint(*module)));
    CHECK(module->getFunction("fminus_rt_main_args") == nullptr);
    const llvm::Function* read = module->getFunction("fminus_rt_main_double_args");
    REQUIRE(read != nullptr);
    CHECK(read->arg_size() == 5);   // argc, argv, the count, the kinds ("df") and the doubles
    const llvm::Function* print = module->getFunction("fminus_rt_main_double_result");
    REQUIRE(print != nullptr);
    CHECK(print->getArg(0)->getType()->isDoubleTy());
    CHECK(module->getFunction("fminus_rt_main_result") == nullptr);
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

TEST_CASE("an int main's result is the exit status, and nothing is printed", "[aot]")
{
    llvm::LLVMContext context;
    std::unique_ptr<llvm::Module> module = parse(context, "define i32 @main() {\n  ret i32 3\n}\n");
    REQUIRE_FALSE(llvm::errorToBool(fminus::aot::addEntryPoint(*module)));
    CHECK(module->getFunction("fminus_rt_main_result") == nullptr);
    CHECK(module->getFunction("fminus_rt_main_double_result") == nullptr);
    CHECK(module->getFunction("fminus_rt_main_bool_result") == nullptr);
    const llvm::Function* entry = module->getFunction("main");
    REQUIRE(entry != nullptr);
    // The C main returns what the program's returned: a ret of the call's value, not of a constant.
    bool returnsResult = false;
    for (const llvm::BasicBlock& block : *entry)
    {
        if (const auto* ret = llvm::dyn_cast<llvm::ReturnInst>(block.getTerminator()))
            returnsResult = returnsResult || llvm::isa<llvm::CallInst>(ret->getReturnValue());
    }
    CHECK(returnsResult);
}

TEST_CASE("a main with an int or a string reads every argument into a slot as its type says", "[aot]")
{
    llvm::LLVMContext context;
    std::unique_ptr<llvm::Module> module =
        parse(context, "define i32 @main({ ptr, i32 } %name, i32 %n, float %x, double %d) {\n"
                       "  ret i32 %n\n}\n");
    REQUIRE_FALSE(llvm::errorToBool(fminus::aot::addEntryPoint(*module)));
    CHECK(module->getFunction("fminus_rt_main_args") == nullptr);
    CHECK(module->getFunction("fminus_rt_main_double_args") == nullptr);
    const llvm::Function* read = module->getFunction("fminus_rt_main_slot_args");
    REQUIRE(read != nullptr);
    CHECK(read->arg_size() == 5);   // argc, argv, the count, the kinds ("sifd") and the slots
    const llvm::GlobalVariable* kinds = module->getNamedGlobal("main.kinds");
    REQUIRE(kinds != nullptr);
    const auto* text = llvm::dyn_cast<llvm::ConstantDataArray>(kinds->getInitializer());
    REQUIRE(text != nullptr);
    CHECK(text->getAsCString() == "sifd");
}

TEST_CASE("an executable needs a main of ints, floats, doubles and strings that returns an int, a float, a double or "
          "a bool",
          "[aot]")
{
    const std::string missing = "an executable needs a main function, such as float main(float x)";
    const std::string signature = "'main' must return int, float, double or bool and take only int, float, double and "
                                  "string parameters";
    CHECK(entryError("define float @f() {\n  ret float 1.0\n}\n") == missing);
    CHECK(entryError("declare float @main(float)\n") == missing);   // a prototype is not a program
    CHECK(entryError("define i1 @main() {\n  ret i1 true\n}\n").empty());
    CHECK(entryError("define double @main(double %t, float %x) {\n  ret double %t\n}\n").empty());
    CHECK(entryError("define float @main(double %t) {\n  ret float 1.0\n}\n").empty());
    CHECK(entryError("define i32 @main() {\n  ret i32 0\n}\n").empty());
    CHECK(entryError("define i32 @main(i32 %n, { ptr, i32 } %s) {\n  ret i32 %n\n}\n").empty());
    CHECK(entryError("define float @main(i1 %b) {\n  ret float 1.0\n}\n") == signature);
    CHECK(entryError("define float @main(<4 x float> %v) {\n  ret float 1.0\n}\n") == signature);
    CHECK(entryError("define float @main(ptr %p) {\n  ret float 1.0\n}\n") == signature);
    CHECK(entryError("define float @main({ ptr, i64 } %s) {\n  ret float 1.0\n}\n") == signature);
    CHECK(entryError("define i64 @main() {\n  ret i64 0\n}\n") == signature);
    CHECK(entryError("define float @main(...) {\n  ret float 1.0\n}\n") == signature);
}
