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
// builtins (Calls.cpp), the math builtins' modes (Math.cpp), vectors (Vectors.cpp), memory (Memory.cpp), modules
// (Modules.cpp). This file runs them, and then the check of the functions' effects (Effects.cpp), which needs every
// body's calls, and under Denormals::Flush the entry points' wrappers (Denormals.cpp), which need them too.
#include "ir/CodeGen.h"

#include "ir/Lowering.h"
#include "opt/KnobVersioning.h"
#include "support/HostTarget.h"

#include <llvm/IR/LLVMContext.h>
#include <llvm/IR/Metadata.h>
#include <llvm/IR/Module.h>

#include <memory>

namespace fminus::ir
{
    namespace detail
    {
        Lowering::Lowering(llvm::Module& module, Diagnostics& diagnostics, const Options& options)
            : module_(module)
            , diagnostics_(diagnostics)
            , builder_(module.getContext(), DenormalFolder(options.denormals == Denormals::Flush))
            , polyLanes_(options.polyLanes)
            , math_(options.math)
            , bindings_(options.parameters)
            , strictAliasing_(options.strictAliasing)
            , denormals_(options.denormals)
            , targetTriple_(options.targetTriple)
            , threads_(options.threads)
        {
        }

        // Pass 1 declares every function (a module's methods among them), so calls may come before definitions; pass
        // 2 goes through the file in order, as C's file scope does, so that each declaration sees what is declared
        // before it: the enumerations of the top level and the modules (Constants.cpp), the modules, each laid out
        // where it is (Modules.cpp: a field's array may be sized by an enumeration constant before it), the top
        // level's constants (Constants.cpp), and the functions, whose bodies it lowers there. A prototype that no
        // definition followed and that names libm's function then gets what libm's does (describeLibm), and every
        // function marked nonallocating or nonblocking is checked against its calls.
        // Under Denormals::Flush, last, every definition is marked flushing and the entry points are wrapped, which
        // needs every call known (Denormals.cpp).
        void Lowering::lower(const ast::Program& program)
        {
            program_ = &program;
            prepareModules(program);
            for (const ast::Function& function : program.functions)
                declare(function);
            // The next enumeration, constant and module, each in source order; between them, a constant before the
            // enumerations and the modules declared after it, an enumeration of the top level before the modules
            // after it, a module's own inside it (layOut). Each before the functions after it.
            std::size_t enums = 0;
            std::size_t constants = 0;
            std::size_t modules = 0;
            const auto declareBefore = [&](std::size_t position) {
                for (;;)
                {
                    function_ = nullptr;   // at the file's scope: no function's names, no module's fields
                    scopes_.clear();
                    const ast::Enum* enumeration = enums < program.enums.size() ? &program.enums[enums] : nullptr;
                    const ast::Constant* constant =
                        constants < program.constants.size() ? &program.constants[constants] : nullptr;
                    const ast::Module* module = modules < program.modules.size() ? program.modules[modules].get()
                                                                                 : nullptr;
                    if (constant != nullptr && constant->position <= position && constant->enums <= enums
                        && constant->modules <= modules)
                    {
                        defineConstant(*constant);
                        ++constants;
                    }
                    else if (enumeration != nullptr && enumeration->module == nullptr
                             && enumeration->position <= position && enumeration->modules <= modules)
                    {
                        declareEnum(*enumeration);
                        ++enums;
                    }
                    else if (module != nullptr && module->position <= position)
                    {
                        layOut(*module, enums);
                        ++modules;
                    }
                    else
                        return;
                }
            };
            for (std::size_t f = 0; f < program.functions.size(); ++f)
            {
                declareBefore(f);
                const ast::Function& function = program.functions[f];
                checkSizes(function);
                const auto found = functions_.find(function.name);
                if (function.isDefinition && found != functions_.end() && found->second == &function)
                    define(function);   // the accepted definition, not a rejected duplicate
            }
            declareBefore(program.functions.size());
            describeLibm();
            checkEffects(program);
            if (denormals_ == Denormals::Flush && !diagnostics_.hasErrors())
                flushDenormals(program);
        }
    } // namespace detail

    std::unique_ptr<llvm::Module> lower(const ast::Program& program, llvm::LLVMContext& context,
                                        Diagnostics& diagnostics, const Options& options)
    {
        auto module = std::make_unique<llvm::Module>(diagnostics.fileName(), context);
        module->setSourceFileName(diagnostics.fileName());
        if (!options.dataLayout.empty())
            module->setDataLayout(options.dataLayout);
        // Knob versioning's budget goes with the module: the JIT optimises it later, where only the module arrives.
        module->addModuleFlag(llvm::Module::Min, opt::kVersionsFlag, options.knobVersions);
        // How its math builtins were lowered, for optimiseModule (support/HostTarget.h), which gives the fast mode's
        // pipeline the vector math library and the others none. Two modules lowered in different modes cannot be
        // linked into one.
        const char* math = options.math == Math::Vector ? "vector" : options.math == Math::Libm ? "libm" : "fast";
        module->addModuleFlag(llvm::Module::Error, kMathFlag, llvm::MDString::get(context, math));
        detail::Lowering(*module, diagnostics, options).lower(program);
        if (diagnostics.hasErrors())
            return nullptr;
        return module;
    }
} // namespace fminus::ir
