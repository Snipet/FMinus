// src/ir/CodeGen.cpp: FMinus's syntax tree to LLVM IR (CodeGen.h), where the code generator's parts combine.
//
// Two passes over the program. The first declares every function, from its prototypes and its definition, so calls
// may come before definitions (C needs a prototype for that; FMinus does not). A function with prototypes and no
// definition is external. The second pass lowers each body with an IRBuilder.
//
// Variables have C's semantics: every parameter and local is a stack slot (an alloca in the entry block), read with a
// load and written with a store. The pipeline's mem2reg (-O1 and up) turns the slots back into SSA values, as for
// clang's output, so `fminus -O0` looks like `clang -O0`. Scopes nest as C's do: parameters and the body's
// declarations share the outermost scope, and a block may shadow an outer name. After an error the lowering goes on
// with a poison value, so one run reports every error; the wording follows clang's.
//
// The parts are one class, Lowering (Lowering.h), whose members are defined by what they lower: types and conversions
// (Types.cpp), functions (Functions.cpp), statements (Statements.cpp), expressions (Expressions.cpp), calls and
// builtins (Calls.cpp), vectors (Vectors.cpp), memory (Memory.cpp), modules (Modules.cpp). This file runs them, and
// then the check of the functions' effects (Effects.cpp), which needs every body's calls.
#include "ir/CodeGen.h"

#include "ir/Lowering.h"

#include <llvm/IR/LLVMContext.h>
#include <llvm/IR/Module.h>

#include <memory>

namespace fminus::ir
{
    namespace detail
    {
        Lowering::Lowering(llvm::Module& module, Diagnostics& diagnostics, const Options& options)
            : module_(module)
            , diagnostics_(diagnostics)
            , builder_(module.getContext())
            , polyLanes_(options.polyLanes)
            , bindings_(options.parameters)
            , strictAliasing_(options.strictAliasing)
        {
        }

        // The modules are laid out first, in order, since a function's types may name them. Pass 1 declares every
        // function (a module's methods among them), so calls may come before definitions; pass 2 lowers each
        // definition's body; then every function marked nonallocating or nonblocking is checked against its calls.
        void Lowering::lower(const ast::Program& program)
        {
            layOut(program);
            for (const ast::Function& function : program.functions)
                declare(function);
            for (const ast::Function& function : program.functions)
            {
                const auto found = functions_.find(function.name);
                if (function.isDefinition && found != functions_.end() && found->second == &function)
                    define(function);   // the accepted definition, not a rejected duplicate
            }
            checkEffects(program);
        }
    } // namespace detail

    std::unique_ptr<llvm::Module> lower(const ast::Program& program, llvm::LLVMContext& context,
                                        Diagnostics& diagnostics, const Options& options)
    {
        auto module = std::make_unique<llvm::Module>(diagnostics.fileName(), context);
        module->setSourceFileName(diagnostics.fileName());
        if (!options.dataLayout.empty())
            module->setDataLayout(options.dataLayout);
        detail::Lowering(*module, diagnostics, options).lower(program);
        if (diagnostics.hasErrors())
            return nullptr;
        return module;
    }
} // namespace fminus::ir
