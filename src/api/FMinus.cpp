// src/api/FMinus.cpp: the public API (include/fminus/FMinus.h) on the pipeline, run as the fminus driver
// (tools/fminus/main.cpp) runs it: parse, lower for a poly float's width, then hand the module to an LLJIT that
// optimises it at the chosen level and compiles it when one of its functions is first looked up.
#include "fminus/FMinus.h"

#include "frontend/Frontend.h"
#include "ir/CodeGen.h"
#include "jit/Jit.h"
#include "support/HostTarget.h"

#include <llvm/IR/LLVMContext.h>
#include <llvm/IR/Module.h>
#include <llvm/Support/Error.h>
#include <llvm/Target/TargetMachine.h>

#include <algorithm>
#include <array>
#include <cstddef>
#include <functional>
#include <map>
#include <optional>
#include <utility>

namespace fminus
{
    namespace
    {
        // A function's effect, the strongest its declarations state (ir/Effects.cpp): nonblocking includes
        // nonallocating.
        enum class Effect
        {
            None,
            NonAllocating,
            NonBlocking,
        };

        Effect declaredEffect(const ast::Function& function)
        {
            Effect effect = Effect::None;
            for (const ast::Attribute& attribute : function.attributes)
            {
                if (attribute.name == "clang::nonblocking")
                    effect = Effect::NonBlocking;
                else if (attribute.name == "clang::nonallocating" && effect == Effect::None)
                    effect = Effect::NonAllocating;
            }
            return effect;
        }

        bool isPolyWidth(unsigned lanes)
        {
            return lanes >= 1 && lanes <= 64 && (lanes & (lanes - 1)) == 0;
        }
    } // namespace

    struct Program::Impl
    {
        std::unique_ptr<fminus::jit::Jit> jit;
        unsigned polyLanes = 4;
        std::map<std::string, Effect, std::less<>> effects;   // the functions a declaration marks
    };

    Program::Program(std::unique_ptr<Impl> impl) : impl_(std::move(impl)) {}

    Program::~Program() = default;

    std::unique_ptr<Program> Program::compile(std::string_view source, std::string& errors)
    {
        return compile(source, errors, Options{});
    }

    std::unique_ptr<Program> Program::compile(std::string_view source, std::string& errors, const Options& options)
    {
        errors.clear();
        if (options.optimisation < 0 || options.optimisation > 3)
        {
            errors = "fminus::Program: optimisation " + std::to_string(options.optimisation)
                   + " is not a level from 0 to 3\n";
            return nullptr;
        }
        if (options.polyLanes != 0 && !isPolyWidth(options.polyLanes))
        {
            errors = "fminus::Program: polyLanes " + std::to_string(options.polyLanes)
                   + " is not 0 (this machine's) or a power of two from 1 to 64\n";
            return nullptr;
        }

        Diagnostics diagnostics(options.fileName);
        const std::optional<ast::Program> program = frontend::parse(source, diagnostics);
        if (!program)
        {
            errors = diagnostics.format();
            return nullptr;
        }

        const std::array<llvm::OptimizationLevel, 4> levels{llvm::OptimizationLevel::O0, llvm::OptimizationLevel::O1,
                                                            llvm::OptimizationLevel::O2, llvm::OptimizationLevel::O3};
        const llvm::OptimizationLevel level = levels[static_cast<std::size_t>(options.optimisation)];
        ir::Options lowering;
        lowering.polyLanes = options.polyLanes;
        if (lowering.polyLanes == 0)
        {
            // The machine decides a poly float's width, as it does for fminus.
            auto machine = hostTargetMachine(codeGenLevel(level));
            if (!machine)
            {
                errors = llvm::toString(machine.takeError()) + "\n";
                return nullptr;
            }
            lowering.polyLanes = fminus::polyLanes(**machine);
        }

        auto context = std::make_unique<llvm::LLVMContext>();
        std::unique_ptr<llvm::Module> module = ir::lower(*program, *context, diagnostics, lowering);
        if (!module)
        {
            errors = diagnostics.format();
            return nullptr;
        }
        auto created = jit::Jit::create(level);
        if (!created)
        {
            errors = llvm::toString(created.takeError()) + "\n";
            return nullptr;
        }
        if (llvm::Error error = (*created)->add(std::move(module), std::move(context)))
        {
            errors = llvm::toString(std::move(error)) + "\n";
            return nullptr;
        }

        auto impl = std::make_unique<Impl>();
        impl->jit = std::move(*created);
        impl->polyLanes = lowering.polyLanes;
        for (const ast::Function& function : program->functions)
        {
            const Effect effect = declaredEffect(function);
            if (effect == Effect::None)
                continue;
            Effect& known = impl->effects[function.name];
            known = std::max(known, effect);
        }
        return std::unique_ptr<Program>(new Program(std::move(impl)));
    }

    std::uintptr_t Program::address(std::string_view name, std::string& errors)
    {
        auto found = impl_->jit->lookup(llvm::StringRef(name.data(), name.size()));
        if (!found)
        {
            errors = llvm::toString(found.takeError()) + "\n";
            return 0;
        }
        errors.clear();
        return static_cast<std::uintptr_t>(found->getValue());
    }

    unsigned Program::polyLanes() const
    {
        return impl_->polyLanes;
    }

    bool Program::nonBlocking(std::string_view name) const
    {
        const auto found = impl_->effects.find(name);
        return found != impl_->effects.end() && found->second == Effect::NonBlocking;
    }

    bool Program::nonAllocating(std::string_view name) const
    {
        return impl_->effects.find(name) != impl_->effects.end();
    }
} // namespace fminus
