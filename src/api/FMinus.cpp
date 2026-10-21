// src/api/FMinus.cpp: the public API (include/fminus/FMinus.h) on the pipeline, run as the fminus driver
// (tools/fminus/main.cpp) runs it: parse, lower for this machine (a poly float's width, the data layout a module
// follows), then hand the module to an LLJIT that optimises it at the chosen level and compiles it when one of its
// functions is first looked up.
#include "fminus/FMinus.h"

#include "frontend/Frontend.h"
#include "ir/CodeGen.h"
#include "jit/Jit.h"
#include "support/HostTarget.h"

#include <llvm/IR/DataLayout.h>
#include <llvm/IR/DerivedTypes.h>
#include <llvm/IR/LLVMContext.h>
#include <llvm/IR/Module.h>
#include <llvm/Support/Error.h>
#include <llvm/Target/TargetMachine.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <cstddef>
#include <functional>
#include <limits>
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

        ir::ParamBinding lowered(const Program::Binding& binding)
        {
            using Kind = ir::ParamBinding::Kind;
            const Kind kind = binding.kind == Program::Binding::Kind::Stream   ? Kind::Stream
                              : binding.kind == Program::Binding::Kind::Frozen ? Kind::Frozen
                                                                               : Kind::Knob;
            return {kind, binding.value};
        }

        // A parameter's range and default, from its attributes (the code generator has checked them).
        void describe(const ast::Field& field, Program::Parameter& parameter)
        {
            for (const ast::Attribute& attribute : field.attributes)
            {
                std::vector<double> numbers;
                for (const std::string& arg : attribute.args)
                    numbers.push_back(ast::attributeNumber(arg).value_or(0.0));
                if (attribute.name == "fminus::range" && numbers.size() == 2)
                {
                    parameter.minimum = numbers[0];
                    parameter.maximum = numbers[1];
                }
                else if (attribute.name == "fminus::default" && numbers.size() == 1)
                    parameter.defaultValue = numbers[0];
            }
        }

        template <typename T>
        T& at(void* instance, std::size_t offset)
        {
            return *reinterpret_cast<T*>(static_cast<unsigned char*>(instance) + offset);
        }
    } // namespace

    struct Program::Impl
    {
        std::unique_ptr<fminus::jit::Jit> jit;
        unsigned polyLanes = 4;
        std::map<std::string, Effect, std::less<>> effects;   // the functions a declaration marks
        std::vector<Module> modules;
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
        // The machine decides a poly float's width, as it does for fminus, and a module's layout.
        auto machine = hostTargetMachine(codeGenLevel(level));
        if (!machine)
        {
            errors = llvm::toString(machine.takeError()) + "\n";
            return nullptr;
        }
        ir::Options lowering;
        lowering.polyLanes = options.polyLanes != 0 ? options.polyLanes : fminus::polyLanes(**machine);
        lowering.dataLayout = (*machine)->createDataLayout().getStringRepresentation();
        for (const auto& [name, binding] : options.parameters)
            lowering.parameters.emplace(name, lowered(binding));

        auto context = std::make_unique<llvm::LLVMContext>();
        std::unique_ptr<llvm::Module> module = ir::lower(*program, *context, diagnostics, lowering);
        if (!module)
        {
            errors = diagnostics.format();
            return nullptr;
        }
        // Each module's layout, while the module and its context are still here (the JIT takes both).
        std::vector<Module> modules;
        const llvm::DataLayout& layout = module->getDataLayout();
        for (const std::unique_ptr<ast::Module>& declared : program->modules)
        {
            llvm::StructType* type = llvm::StructType::getTypeByName(*context, declared->name);
            const llvm::StructLayout* fields = layout.getStructLayout(type);
            Module& info = modules.emplace_back();
            info.name = declared->name;
            info.size = layout.getTypeAllocSize(type).getFixedValue();
            info.alignment = layout.getABITypeAlign(type).value();
            for (std::size_t i = 0; i < declared->fields.size(); ++i)
            {
                const ast::Field& field = declared->fields[i];
                const auto index = static_cast<unsigned>(i);
                const std::size_t offset = fields->getElementOffset(index).getFixedValue();
                llvm::Type* element = type->getElementType(index);
                info.fields.push_back({field.name, (field.isParam ? "param " : "") + ast::typeName(field.type), offset,
                                       layout.getTypeAllocSize(element).getFixedValue()});
                if (!field.isParam)
                    continue;
                // The slot, { const float* stream; T value; }.
                const llvm::StructLayout* slot = layout.getStructLayout(llvm::cast<llvm::StructType>(element));
                Parameter& parameter = info.parameters.emplace_back();
                parameter.module = declared->name;
                parameter.name = field.name;
                parameter.type = ast::typeName(field.type);
                const auto bound = options.parameters.find(declared->name + "::" + field.name);
                parameter.binding = bound != options.parameters.end() ? bound->second : Binding{};
                parameter.streamOffset = offset + slot->getElementOffset(0).getFixedValue();
                parameter.valueOffset = offset + slot->getElementOffset(1).getFixedValue();
                describe(field, parameter);
            }
            for (const std::string& method : declared->methods)
                info.methods.push_back(declared->name + "::" + method);
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
        impl->modules = std::move(modules);
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

    const Program::Module* Program::module(std::string_view name) const
    {
        for (const Module& module : impl_->modules)
        {
            if (module.name == name)
                return &module;
        }
        return nullptr;
    }

    const std::vector<Program::Module>& Program::modules() const
    {
        return impl_->modules;
    }

    const Program::Parameter* Program::parameter(std::string_view name) const
    {
        for (const Module& module : impl_->modules)
        {
            for (const Parameter& parameter : module.parameters)
            {
                if (name.size() == module.name.size() + 2 + parameter.name.size() && name.starts_with(module.name)
                    && name.substr(module.name.size(), 2) == "::" && name.ends_with(parameter.name))
                    return &parameter;
            }
        }
        return nullptr;
    }

    // The method's load is LLVM's `load atomic unordered`, which can't tear and reads a value some store wrote: these
    // are C++'s relaxed stores to it, which can't tear either.
    void Program::setValue(void* instance, const Parameter& parameter, double value) noexcept
    {
        if (parameter.type == "int")
        {
            // C's conversion toward 0, where an int holds it; past an int's ends, the end (and NaN is 0).
            const double clamped = std::isnan(value) ? 0.0
                                                     : std::clamp(std::trunc(value),
                                                                  double{std::numeric_limits<std::int32_t>::min()},
                                                                  double{std::numeric_limits<std::int32_t>::max()});
            std::atomic_ref(at<std::int32_t>(instance, parameter.valueOffset))
                .store(static_cast<std::int32_t>(clamped), std::memory_order_relaxed);
        }
        else if (parameter.type == "bool")
            std::atomic_ref(at<bool>(instance, parameter.valueOffset)).store(value != 0.0, std::memory_order_relaxed);
        else
            std::atomic_ref(at<float>(instance, parameter.valueOffset))
                .store(static_cast<float>(value), std::memory_order_relaxed);
    }

    double Program::value(const void* instance, const Parameter& parameter) noexcept
    {
        // atomic_ref of a const object is C++26's; the load does not write.
        void* slot = const_cast<void*>(instance);
        if (parameter.type == "int")
            return std::atomic_ref(at<std::int32_t>(slot, parameter.valueOffset)).load(std::memory_order_relaxed);
        if (parameter.type == "bool")
            return std::atomic_ref(at<bool>(slot, parameter.valueOffset)).load(std::memory_order_relaxed) ? 1.0 : 0.0;
        const float stored = std::atomic_ref(at<float>(slot, parameter.valueOffset)).load(std::memory_order_relaxed);
        return static_cast<double>(stored);
    }

    void Program::setStream(void* instance, const Parameter& parameter, const float* samples) noexcept
    {
        at<const float*>(instance, parameter.streamOffset) = samples;
    }

    void Program::setDefaults(void* instance, const Module& module) noexcept
    {
        for (const Parameter& parameter : module.parameters)
        {
            if (parameter.defaultValue)
                setValue(instance, parameter, *parameter.defaultValue);
        }
    }
} // namespace fminus
