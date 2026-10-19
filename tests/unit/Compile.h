// tests/unit/Compile.h: what the code generator's tests share. compile() parses, lowers and JIT-compiles a program;
// lookup() gives one of its functions as a C++ function; errorsOf() gives the errors of a program that must fail.
// float2 and float4 are FMinus's at the C boundary.
#pragma once

#include "frontend/Frontend.h"
#include "ir/CodeGen.h"
#include "jit/Jit.h"

#include <catch2/catch_test_macros.hpp>

#include <llvm/IR/LLVMContext.h>
#include <llvm/IR/Module.h>
#include <llvm/Support/Error.h>

#include <array>
#include <memory>
#include <string>
#include <string_view>

namespace fminus::test
{
    // The program JIT-compiled at -O2, or nothing with its errors in `errors`. `options` sets a poly float's width.
    inline std::unique_ptr<jit::Jit> compile(std::string_view source, std::string* errors = nullptr,
                                             const ir::Options& options = {})
    {
        Diagnostics diagnostics("test.fm");
        const auto program = frontend::parse(source, diagnostics);
        auto context = std::make_unique<llvm::LLVMContext>();
        std::unique_ptr<llvm::Module> module = program ? ir::lower(*program, *context, diagnostics, options) : nullptr;
        if (errors != nullptr)
            *errors = diagnostics.format();
        if (!module)
            return nullptr;
        auto jit = llvm::cantFail(jit::Jit::create(llvm::OptimizationLevel::O2));
        llvm::cantFail(jit->add(std::move(module), std::move(context)));
        return jit;
    }

    template <typename Function>
    Function* lookup(jit::Jit& jit, std::string_view name)
    {
        return llvm::cantFail(jit.lookup(name)).toPtr<Function*>();
    }

    inline std::string errorsOf(std::string_view source)
    {
        std::string errors;
        CHECK(compile(source, &errors) == nullptr);
        return errors;
    }

    // FMinus's float2 and float4 are clang's ext_vector_type at the C boundary: <2 x float> and <4 x float>, in one
    // register. (float8 is not: clang passes it through memory, so the tests keep float8 inside FMinus.)
    using float2 = float __attribute__((ext_vector_type(2)));
    using float4 = float __attribute__((ext_vector_type(4)));

    inline std::array<float, 4> lanesOf(float4 v)
    {
        return {v.x, v.y, v.z, v.w};
    }
} // namespace fminus::test
