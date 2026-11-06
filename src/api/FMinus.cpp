// src/api/FMinus.cpp: the public API (include/fminus/FMinus.h) on the pipeline, run as the fminus driver
// (tools/fminus/main.cpp) runs it: parse, lower for this machine (a poly float's width, the data layout a module
// follows), then hand the module to an LLJIT that optimises it at the chosen level and compiles it when one of its
// functions is first looked up. For a host that keeps the IR as text (Options::keepIr), print it, as lowered and as
// optimised, the optimised IR as fminus --emit ll prints it: optimised here, and given to a JIT that takes it as it is.
#include "fminus/FMinus.h"

#include "frontend/Frontend.h"
#include "ir/CodeGen.h"
#include "jit/Call.h"
#include "jit/Jit.h"
#include "opt/BoundsCheck.h"
#include "opt/LoopBudget.h"
#include "runtime/Runtime.h"
#include "support/HostTarget.h"

#include <llvm/IR/DataLayout.h>
#include <llvm/IR/DerivedTypes.h>
#include <llvm/IR/LLVMContext.h>
#include <llvm/IR/Module.h>
#include <llvm/Support/Error.h>
#include <llvm/Support/raw_ostream.h>
#include <llvm/Target/TargetMachine.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <format>
#include <functional>
#include <limits>
#include <map>
#include <mutex>
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

        ir::Math lowered(Program::Math math)
        {
            switch (math)
            {
            case Program::Math::Vector:
                return ir::Math::Vector;
            case Program::Math::Libm:
                return ir::Math::Libm;
            case Program::Math::Fast:
                return ir::Math::Fast;
            }
            return ir::Math::Vector;
        }

        ir::ParamBinding lowered(const Program::Binding& binding)
        {
            using Kind = ir::ParamBinding::Kind;
            const Kind kind = binding.kind == Program::Binding::Kind::Stream   ? Kind::Stream
                              : binding.kind == Program::Binding::Kind::Frozen ? Kind::Frozen
                                                                               : Kind::Knob;
            return {kind, binding.value};
        }

        // A field's attributes as the host sees them, its numbers' values worked out (the code generator has checked
        // that each is one), without those FMinus ignored with a warning (`[[range(0, 1)]]`, no namespace's).
        std::vector<Program::Attribute> attributesOf(const ast::Field& field)
        {
            std::vector<Program::Attribute> attributes;
            for (const ast::Attribute& written : field.attributes)
            {
                if (ast::isIgnoredAttribute(written.name))
                    continue;
                Program::Attribute& attribute = attributes.emplace_back();
                attribute.name = written.name;
                for (const ast::AttributeArg& arg : written.args)
                    attribute.arguments.push_back({arg.isString, ast::attributeNumber(arg).value_or(0.0), arg.text});
            }
            return attributes;
        }

        // A parameter's range and default, from its attributes (the code generator has checked them).
        void describe(const ast::Field& field, Program::Parameter& parameter)
        {
            for (const ast::Attribute& attribute : field.attributes)
            {
                std::vector<double> numbers;
                for (const ast::AttributeArg& arg : attribute.args)
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

        // A module as LLVM's textual IR, as fminus --emit ll writes it.
        std::string printed(const llvm::Module& module)
        {
            std::string text;
            llvm::raw_string_ostream stream(text);
            module.print(stream, nullptr);
            return text;
        }

        template <typename T>
        T& at(void* instance, std::size_t offset)
        {
            return *reinterpret_cast<T*>(static_cast<unsigned char*>(instance) + offset);
        }

        // C's conversion toward 0, where an int holds it; past an int's ends, the end (and NaN is 0).
        std::int32_t clampedInt(double value)
        {
            const double clamped = std::isnan(value) ? 0.0
                                                     : std::clamp(std::trunc(value),
                                                                  double{std::numeric_limits<std::int32_t>::min()},
                                                                  double{std::numeric_limits<std::int32_t>::max()});
            return static_cast<std::int32_t>(clamped);
        }

        // ---- calls by name (Program::call) ----

        using Kind = Program::Value::Kind;

        std::string quoted(std::string_view name)
        {
            return "'" + std::string(name) + "'";
        }

        const char* kindName(Kind kind)
        {
            switch (kind)
            {
            case Kind::Void:
                return "void";
            case Kind::Bool:
                return "bool";
            case Kind::Int:
                return "int";
            case Kind::Float:
                return "float";
            case Kind::Double:
                return "double";
            case Kind::Pointer:
                return "void *";
            }
            return "void";
        }

        // How call() passes a value of `type`, as a thunk does (jit::callType, whose switch over FMinus's types makes
        // a new type say how it crosses): Void for void, a scalar as itself, a pointer as a Pointer; nothing for a
        // type no thunk passes.
        std::optional<Kind> kindOf(const ast::Type& type)
        {
            const std::optional<jit::CallType> passed = jit::callType(type);
            if (!passed)
                return std::nullopt;
            switch (*passed)
            {
            case jit::CallType::Void:
                return Kind::Void;
            case jit::CallType::Bool:
                return Kind::Bool;
            case jit::CallType::Int:
                return Kind::Int;
            case jit::CallType::Float:
                return Kind::Float;
            case jit::CallType::Double:
                return Kind::Double;
            case jit::CallType::Pointer:
                return Kind::Pointer;
            }
            return std::nullopt;
        }

        const Program::Module* moduleNamed(const std::vector<Program::Module>& modules, std::string_view name)
        {
            for (const Program::Module& module : modules)
            {
                if (module.name == name)
                    return &module;
            }
            return nullptr;
        }

        // A function's signature as call() passes it, into `callable`; or why call() cannot pass one of its types.
        std::string describeCall(const ast::Function& function, const std::vector<Program::Module>& modules,
                                 Program::Callable& callable)
        {
            callable.name = function.name;
            callable.module = function.module != nullptr ? moduleNamed(modules, function.module->name) : nullptr;
            callable.resultType = ast::typeName(function.returnType);
            callable.parameters.clear();
            std::string unsupported;
            const std::optional<Kind> result = kindOf(function.returnType);
            if (result && *result != Kind::Pointer)
                callable.result = *result;
            else
                unsupported = quoted(function.name) + " returns " + quoted(callable.resultType)
                            + "; call() returns nothing, or a bool, an int, a float or a double";
            for (const ast::Param& declared : function.params)
            {
                Program::Callable::Param& param = callable.parameters.emplace_back();
                param.name = declared.name;
                param.type = ast::typeName(declared.type);
                const std::optional<Kind> kind = kindOf(declared.type);
                bool passes = kind.has_value() && *kind != Kind::Void;
                if (passes)
                    param.kind = *kind;
                if (passes && *kind == Kind::Pointer)
                {
                    // To an int, a float, a double, a module or void (any): not to a vector, nor to a row.
                    const ast::Type pointee = ast::pointee(declared.type);
                    const std::optional<Kind> element = kindOf(pointee);
                    param.readOnly = declared.type.pointeeConst;
                    if (ast::isModule(pointee) && pointee.module != nullptr)
                        param.module = moduleNamed(modules, pointee.module->name);
                    else if (element && *element != Kind::Pointer && *element != Kind::Bool)
                        param.element = *element;
                    else
                        passes = false;
                }
                if (!passes && unsupported.empty())
                    unsupported = quoted(function.name) + " takes a parameter of type " + quoted(param.type)
                                + "; call() passes bools, ints, floats, doubles and pointers to ints, floats, doubles, "
                                  "modules and void only";
            }
            return unsupported;
        }

        jit::CallType callTypeOf(Kind kind)
        {
            switch (kind)
            {
            case Kind::Void:
                return jit::CallType::Void;
            case Kind::Bool:
                return jit::CallType::Bool;
            case Kind::Int:
                return jit::CallType::Int;
            case Kind::Float:
                return jit::CallType::Float;
            case Kind::Double:
                return jit::CallType::Double;
            case Kind::Pointer:
                return jit::CallType::Pointer;
            }
            return jit::CallType::Void;
        }

        // A value's type as FMinus writes it: "float", "const float *", "Osc *".
        std::string typeOf(const Program::Value& value)
        {
            if (value.kind != Kind::Pointer)
                return kindName(value.kind);
            const std::string pointee = value.module != nullptr ? value.module->name : kindName(value.element);
            return (value.readOnly ? "const " : "") + pointee + " *";
        }

        // Why `argument` is no argument for parameter `index` of `function`, or nothing when it is one.
        std::optional<std::string> mismatch(const Program::Callable& function, std::size_t index,
                                            const Program::Value& argument)
        {
            const Program::Callable::Param& param = function.parameters[index];
            const auto wrong = [&](const std::string& what) {
                std::string message = std::format("argument {} of {} {}", index + 1, quoted(function.name), what);
                if (!param.name.empty())
                    message += " (its parameter " + quoted(param.name) + ")";
                return message;
            };
            const auto is = [&] { return wrong("is " + quoted(typeOf(argument)) + ", not " + quoted(param.type)); };
            if (argument.kind != param.kind)
                return is();
            if (param.kind != Kind::Pointer)
                return std::nullopt;
            if (argument.readOnly && !param.readOnly)   // C's "discards qualifiers"
                return is();
            const bool anything = param.element == Kind::Void && param.module == nullptr;   // void*
            if (!anything)
            {
                if (param.module == nullptr ? argument.module != nullptr || argument.element != param.element
                                            : argument.module == nullptr || argument.module->name != param.module->name)
                    return is();
                // The module is the host's word for the memory's layout, which nothing here can see: this program's
                // own, then, as another program's of the name may lay out otherwise a module it holds (by value, in an
                // array, through a pointer) though its own fields are alike.
                if (param.module != nullptr && argument.module != param.module)
                    return wrong("is an instance of another program's module " + quoted(param.module->name));
            }
            if (argument.pointer == nullptr && index == 0 && function.module != nullptr)
                return wrong("is a null " + quoted(param.type) + ": a method runs on an instance");
            const std::size_t alignment = param.module != nullptr      ? param.module->alignment
                                          : param.element == Kind::Double ? alignof(double)
                                          : param.element == Kind::Void   ? 1
                                                                          : 4;
            if (reinterpret_cast<std::uintptr_t>(argument.pointer) % alignment != 0)
                return wrong(std::format("is not aligned to {} bytes, as a {} must be", alignment, quoted(param.type)));
            return std::nullopt;
        }

        jit::CallSlot slotOf(const Program::Value& value)
        {
            switch (value.kind)
            {
            case Kind::Bool:
                return jit::CallSlot::of<std::uint8_t>(value.number != 0.0 ? 1 : 0);
            case Kind::Int:
                return jit::CallSlot::of(clampedInt(value.number));
            case Kind::Float:
                return jit::CallSlot::of(static_cast<float>(value.number));
            case Kind::Double:
                return jit::CallSlot::of(value.number);
            case Kind::Pointer:
                return jit::CallSlot::of(value.pointer);
            case Kind::Void:
                break;
            }
            return {};
        }

        Program::Value resultOf(Kind kind, const jit::CallSlot& slot)
        {
            switch (kind)
            {
            case Kind::Bool:
                return Program::Value::of(slot.as<std::uint8_t>() != 0);
            case Kind::Int:
                return Program::Value::of(slot.as<std::int32_t>());
            case Kind::Float:
                return Program::Value::of(slot.as<float>());
            case Kind::Double:
                return Program::Value::of(slot.as<double>());
            case Kind::Void:
            case Kind::Pointer:   // no result call() returns
                break;
            }
            return {};
        }

        Program::CallResult failure(Program::CallResult::Status status, std::string message)
        {
            Program::CallResult result;
            result.status = status;
            result.error = std::move(message);
            return result;
        }
    } // namespace

    struct Program::Impl
    {
        std::unique_ptr<fminus::jit::Jit> jit;
        unsigned polyLanes = 4;
        std::map<std::string, Effect, std::less<>> effects;   // the functions a declaration marks
        std::vector<Module> modules;
        bool pool = false;   // a user of the runtime's pool, for its parallel regions (runtime/Parallel.cpp)
        std::string loweredIr;     // Options::keepIr's texts, or empty
        std::string optimisedIr;
        std::string warnings;      // as Diagnostics::format() prints them, or empty
        // The program's loop budget (Options::loopBudget), in the JIT's memory; without the option, one nothing reads.
        fminus_rt_loop_budget_state unbudgeted{std::numeric_limits<std::int64_t>::max(), 0};
        fminus_rt_loop_budget_state* budget = &unbudgeted;
        // Its bounds checks' record (Options::boundsCheck), in the JIT's memory, and what each check's number means;
        // without the option, a record nothing writes, and no checks.
        fminus_rt_bounds_state unchecked{0, 0, 0};
        fminus_rt_bounds_state* bounds = &unchecked;
        std::vector<opt::BoundsSite> boundsSites;
        // Each function of the program as call() calls it, or why call() cannot (`unsupported`): its thunk compiled
        // when callable() is first asked for it, under `calls`. The map itself does not change once compile() has
        // returned.
        struct Entry
        {
            Callable callable;
            std::string unsupported;
            bool defined = false;   // a prototype alone (`float atanf(float x);`) is the C library's, no Callable
        };
        std::map<std::string, Entry, std::less<>> callables;
        std::mutex calls;

        Impl() = default;
        Impl(const Impl&) = delete;
        Impl& operator=(const Impl&) = delete;
        ~Impl()
        {
            jit.reset();
            if (pool)
                fminus_rt_parallel_stop();
        }
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

        for (const auto& [name, value] : options.defines)
            if (const std::optional<std::string> wrong = frontend::macroNameError(name))
            {
                errors = "fminus::Program: defines: '" + name + "': " + *wrong + "\n";
                return nullptr;
            }

        Diagnostics diagnostics(options.fileName);
        frontend::IncludeResolver include;
        if (options.include)
            include = [&options](std::string_view name,
                                 std::string_view from) -> std::optional<frontend::IncludedFile> {
                std::optional<IncludedFile> file = options.include(name, from);
                if (!file)
                    return std::nullopt;
                return frontend::IncludedFile{std::move(file->fileName), std::move(file->source)};
            };
        const std::optional<ast::Program> program = frontend::parse(source, diagnostics, include, options.defines);
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
        lowering.math = lowered(options.math);
        for (const auto& [name, binding] : options.parameters)
            lowering.parameters.emplace(name, lowered(binding));
        lowering.strictAliasing = options.strictAliasing;
        lowering.knobVersions = options.knobVersions;
        lowering.denormals = options.denormals == Denormals::Flush ? ir::Denormals::Flush : ir::Denormals::Preserve;
        lowering.targetTriple = (*machine)->getTargetTriple().str();   // which register an entry point sets
        lowering.threads = options.threads;
        lowering.loopBudget = options.loopBudget;
        lowering.boundsCheck = options.boundsCheck;

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
                ast::Type fieldType = field.type;
                std::vector<std::size_t> extents;
                if (ast::isArray(fieldType) && element->isArrayTy())   // its sizes as the code generator worked them
                {                                                       // out (the tree's are 0)
                    fieldType.count = static_cast<int>(element->getArrayNumElements());
                    extents.push_back(element->getArrayNumElements());
                    if (ast::hasRows(fieldType) && element->getArrayElementType()->isArrayTy())
                    {
                        fieldType.rowCount = static_cast<int>(element->getArrayElementType()->getArrayNumElements());
                        extents.push_back(element->getArrayElementType()->getArrayNumElements());
                    }
                }
                info.fields.push_back({field.name, (field.isParam ? "param " : "") + ast::typeName(fieldType), offset,
                                       layout.getTypeAllocSize(element).getFixedValue(),
                                       ast::isArray(fieldType) ? ast::typeName(ast::elementOf(fieldType)) : "",
                                       std::move(extents), attributesOf(field)});
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
                parameter.attributes = info.fields.back().attributes;
            }
            for (const std::string& method : declared->methods)
                info.methods.push_back(declared->name + "::" + method);
        }

        // What each bounds check's number means, for boundsViolation().
        std::vector<opt::BoundsSite> boundsSites = opt::boundsSites(*module);

        // A program with a parallel region runs it on the runtime's pool, from now until it is destroyed.
        const bool parallel = module->getFunction("fminus_rt_parallel_for") != nullptr && options.threads != 1;

        // The IR as text, for a host that keeps it: set up for this machine, as the JIT sets a module up, and printed;
        // then optimised as fminus --emit ll optimises it, by the same calls on a machine made as fminus's and the
        // JIT's are, and printed, and the JIT takes it as it is.
        const bool keepLowered = options.keepIr == KeepIr::Lowered || options.keepIr == KeepIr::Both;
        const bool keepOptimised = options.keepIr == KeepIr::Optimised || options.keepIr == KeepIr::Both;
        std::string loweredIr;
        std::string optimisedIr;
        if (keepLowered || keepOptimised)
            prepareModule(*module, **machine);
        if (keepLowered)
            loweredIr = printed(*module);
        if (keepOptimised)
        {
            optimiseModule(*module, **machine, level);
            optimisedIr = printed(*module);
        }

        auto created =
            jit::Jit::create(level, keepOptimised ? jit::Jit::Modules::Optimised : jit::Jit::Modules::Unoptimised);
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
        // The budget the program defines (opt/LoopBudget.h). Finding it compiles the module, here rather than at the
        // first look-up of a function.
        if (options.loopBudget)
        {
            auto budget = impl->jit->lookup(opt::kLoopBudgetSymbol);
            if (!budget)
            {
                errors = llvm::toString(budget.takeError()) + "\n";
                return nullptr;
            }
            impl->budget = budget->toPtr<fminus_rt_loop_budget_state*>();
        }
        if (options.boundsCheck)   // the record, which the checks write (opt/BoundsCheck.h)
        {
            auto record = impl->jit->lookup(opt::kBoundsSymbol);
            if (!record)
            {
                errors = llvm::toString(record.takeError()) + "\n";
                return nullptr;
            }
            impl->bounds = record->toPtr<fminus_rt_bounds_state*>();
            impl->boundsSites = std::move(boundsSites);
        }
        if (parallel)
        {
            fminus_rt_parallel_start();
            impl->pool = true;
        }
        impl->polyLanes = lowering.polyLanes;
        impl->modules = std::move(modules);
        impl->loweredIr = std::move(loweredIr);
        impl->optimisedIr = std::move(optimisedIr);
        impl->warnings = diagnostics.format();   // no errors: only warnings, if any
        // Each function's signature, for call(): the definition's parameter names, or the first declaration's.
        for (const ast::Function& function : program->functions)
        {
            auto [entry, first] = impl->callables.try_emplace(function.name);
            if (!first && !function.isDefinition)
                continue;
            entry->second.unsupported = describeCall(function, impl->modules, entry->second.callable);
            entry->second.callable.owner_ = impl.get();
            entry->second.defined = entry->second.defined || function.isDefinition;
        }
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

    const std::string& Program::loweredIr() const
    {
        return impl_->loweredIr;
    }

    const std::string& Program::optimisedIr() const
    {
        return impl_->optimisedIr;
    }

    const std::string& Program::warnings() const
    {
        return impl_->warnings;
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

    const Program::Attribute* Program::findAttribute(const std::vector<Attribute>& attributes,
                                                     std::string_view name) noexcept
    {
        for (auto attribute = attributes.rbegin(); attribute != attributes.rend(); ++attribute)
        {
            if (attribute->name == name)
                return &*attribute;
        }
        return nullptr;
    }

    // The method's load is LLVM's `load atomic unordered`, which can't tear and reads a value some store wrote: these
    // are C++'s relaxed stores to it, which can't tear either.
    void Program::setValue(void* instance, const Parameter& parameter, double value) noexcept
    {
        if (parameter.type == "int")
            std::atomic_ref(at<std::int32_t>(instance, parameter.valueOffset))
                .store(clampedInt(value), std::memory_order_relaxed);
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

    // The program's own accesses are relaxed (monotonic) atomics too (opt/LoopBudget.h). The flag is cleared first:
    // a call that starts after both stores sees both. `left` is never below -1, which keeps the program's counts from
    // wrapping (opt/LoopBudget.cpp): any negative budget is -1, a call that stops at its first check.
    void Program::setLoopBudget(std::int64_t backEdges) noexcept
    {
        std::atomic_ref(impl_->budget->stopped).store(0, std::memory_order_relaxed);
        std::atomic_ref(impl_->budget->left).store(std::max<std::int64_t>(backEdges, -1), std::memory_order_relaxed);
    }

    std::int64_t Program::loopBudgetLeft() const noexcept
    {
        return std::atomic_ref(impl_->budget->left).load(std::memory_order_relaxed);
    }

    bool Program::loopBudgetExceeded() const noexcept
    {
        return std::atomic_ref(impl_->budget->stopped).load(std::memory_order_relaxed) != 0;
    }

    // The program's own accesses of the record are relaxed (monotonic) atomics too (opt/BoundsCheck.h): the site's a
    // compare-and-swap from 0, then the index. The index is cleared first, so that a call that starts after both
    // stores and fails a check writes it over 0.
    std::optional<Program::BoundsViolation> Program::boundsViolation() const noexcept
    {
        const std::int32_t site = std::atomic_ref(impl_->bounds->site).load(std::memory_order_relaxed);
        if (site <= 0 || static_cast<std::size_t>(site) > impl_->boundsSites.size())
            return std::nullopt;
        const opt::BoundsSite& check = impl_->boundsSites[static_cast<std::size_t>(site) - 1];
        BoundsViolation violation;
        violation.file = check.file;
        violation.line = check.line;
        violation.column = check.column;
        violation.array = check.array;
        violation.type = check.type;
        violation.extent = check.extent;
        violation.index = std::atomic_ref(impl_->bounds->index).load(std::memory_order_relaxed);
        violation.width = check.width;
        return violation;
    }

    void Program::clearBoundsViolation() noexcept
    {
        std::atomic_ref(impl_->bounds->index).store(0, std::memory_order_relaxed);
        std::atomic_ref(impl_->bounds->site).store(0, std::memory_order_relaxed);
    }

    std::string Program::BoundsViolation::message() const
    {
        const opt::BoundsSite site{std::string(file), line, column, std::string(array), std::string(type), extent,
                                   width};
        return opt::boundsMessage(site, index);
    }

    Program::Value Program::Value::scalar(Kind kind, double number) noexcept
    {
        switch (kind)
        {
        case Kind::Bool:
            return of(number != 0.0);
        case Kind::Int:
            return of(static_cast<int>(clampedInt(number)));
        case Kind::Float:
            return of(static_cast<float>(number));
        case Kind::Double:
            return of(number);
        case Kind::Void:
        case Kind::Pointer:
            break;
        }
        return {};
    }

    // The thunk is compiled once, under the lock: a second thread asking for it waits, then finds it. The function is
    // the program's own, defined in its module, and looked up first, so that the thunk's reference to it resolves (a
    // module that fails to link fails there, as for address()).
    const Program::Callable* Program::callable(std::string_view name, std::string& errors)
    {
        const auto found = impl_->callables.find(name);
        if (found == impl_->callables.end())
        {
            errors = "the program has no function " + quoted(name) + "\n";
            return nullptr;
        }
        Impl::Entry& entry = found->second;
        if (!entry.defined)
        {
            errors = quoted(name) + " is declared in the program but not defined there: call() calls the functions it "
                                    "defines\n";
            return nullptr;
        }
        if (!entry.unsupported.empty())
        {
            errors = entry.unsupported + "\n";
            return nullptr;
        }
        const std::lock_guard<std::mutex> lock(impl_->calls);
        if (entry.callable.thunk_ == 0)
        {
            if (auto function = impl_->jit->lookup(llvm::StringRef(name.data(), name.size())); !function)
            {
                errors = quoted(name) + " cannot be called: " + llvm::toString(function.takeError()) + "\n";
                return nullptr;
            }
            std::vector<jit::CallType> parameters;
            for (const Callable::Param& param : entry.callable.parameters)
                parameters.push_back(callTypeOf(param.kind));
            auto thunk = jit::callThunk(*impl_->jit, llvm::StringRef(name.data(), name.size()),
                                        callTypeOf(entry.callable.result), parameters);
            if (!thunk)
            {
                errors = llvm::toString(thunk.takeError()) + "\n";
                return nullptr;
            }
            entry.callable.thunk_ = reinterpret_cast<std::uintptr_t>(*thunk);
        }
        errors.clear();
        return &entry.callable;
    }

    // The arguments go in slots on the stack (on the heap past 16), the thunk calls the function, and the program's
    // record and flag say whether it stopped early: the bounds checks' first, as a failed check is the reason a call
    // that also ran out of its budget stopped where it did. A stopped call allocates nothing (message() words it), as
    // it may be the audio thread's. `function` is the Program's own, as a Callable cannot be copied: owner_ says whose.
    Program::CallResult Program::call(const Callable& function, const Value* arguments, std::size_t count)
    {
        using Status = CallResult::Status;
        if (function.owner_ != impl_.get() || function.thunk_ == 0)
            return failure(Status::Mismatch, quoted(function.name) + " is not a Callable of this program's: callable() "
                                                                     "gives them");
        if (count != function.parameters.size())
            return failure(Status::Mismatch,
                           std::format("{} takes {} argument{}, not {}", quoted(function.name),
                                       function.parameters.size(), function.parameters.size() == 1 ? "" : "s", count));
        constexpr std::size_t kOnStack = 16;
        std::array<jit::CallSlot, kOnStack> local;
        std::vector<jit::CallSlot> many;
        jit::CallSlot* slots = local.data();
        if (count > kOnStack)
        {
            many.resize(count);
            slots = many.data();
        }
        for (std::size_t i = 0; i < count; ++i)
        {
            if (std::optional<std::string> wrong = mismatch(function, i, arguments[i]))
                return failure(Status::Mismatch, std::move(*wrong));
            slots[i] = slotOf(arguments[i]);
        }
        jit::CallSlot out;
        reinterpret_cast<jit::Thunk>(function.thunk_)(slots, &out);
        CallResult result;
        result.value = resultOf(function.result, out);
        result.function_ = &function;
        result.violation = boundsViolation();
        if (result.violation)
            result.status = Status::OutOfBounds;
        else if (loopBudgetExceeded())
            result.status = Status::OutOfBudget;
        return result;
    }

    std::string Program::CallResult::message() const
    {
        if (!error.empty() || function_ == nullptr)
            return error;
        if (status == Status::OutOfBounds)
            return quoted(function_->name) + " stopped" + (violation ? ": " + violation->message() : std::string());
        if (status == Status::OutOfBudget)
            return quoted(function_->name) + " ran past its loop budget and returned early";
        return error;
    }

    Program::CallResult Program::call(const Callable& function, const std::vector<Value>& arguments)
    {
        return call(function, arguments.data(), arguments.size());
    }

    // Why callable() gave no Callable is the entry's to say, not the error's: a name the program does not define (or
    // only declares), a type call() does not pass, or else a definition whose code, or thunk, did not link.
    Program::CallResult Program::call(std::string_view name, const std::vector<Value>& arguments)
    {
        std::string errors;
        const Callable* function = callable(name, errors);
        if (function == nullptr)
        {
            using Status = CallResult::Status;
            const auto found = impl_->callables.find(name);
            Status status = Status::NoFunction;
            if (found != impl_->callables.end() && found->second.defined)
                status = found->second.unsupported.empty() ? Status::LinkError : Status::Unsupported;
            if (!errors.empty() && errors.back() == '\n')
                errors.pop_back();
            return failure(status, std::move(errors));
        }
        return call(*function, arguments.data(), arguments.size());
    }
} // namespace fminus

// The runtime's scalar functions of the vector math mode, which generated code calls (runtime/Math.cpp).
namespace fminus::math
{
    float sin(float x) noexcept
    {
        return fminus_rt_sinf(x);
    }

    float cos(float x) noexcept
    {
        return fminus_rt_cosf(x);
    }

    float tan(float x) noexcept
    {
        return fminus_rt_tanf(x);
    }

    float exp(float x) noexcept
    {
        return fminus_rt_expf(x);
    }

    float tanh(float x) noexcept
    {
        return fminus_rt_tanhf(x);
    }

    float log(float x) noexcept
    {
        return fminus_rt_logf(x);
    }
} // namespace fminus::math
