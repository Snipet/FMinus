// src/ir/Modules.cpp: modules. Each is an LLVM struct type, laid out before any function is declared: its fields in
// order, as a C struct's (clang's %struct.S, padding where the data layout puts it). A field is reached by a
// getelementptr from the module's address; a method is a function named OnePole::tick, whose first parameter is
// `this`, the address of the module it was called on; inside it, a name no local or parameter has is looked up among
// the module's fields and methods, as C++ looks in the class (Memory.cpp, Calls.cpp). A module is never loaded whole:
// its Value is its address, and `a = b` copies it with a memcpy, as clang copies a struct.
//
// A parameter (`param float cutoff;`) is a field whose slot the host writes, { const float* stream; T value; }, the
// same whatever the binding the host compiled the program with (ir::Options, CodeGen.h). A method reads it as
// cutoff[i]: a knob's value, loaded once where the method starts with an unordered atomic load (the host may store it
// from another thread at any time); a stream's i-th sample, from the pointer the method also loads where it starts; a
// frozen parameter's constant. Loaded at the start, a knob's value and a stream's pointer are registers through the
// method's loops, which then vectorise.
#include "ir/Lowering.h"

#include <llvm/IR/Constants.h>
#include <llvm/IR/InstIterator.h>
#include <llvm/IR/MDBuilder.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <format>
#include <limits>
#include <unordered_set>

namespace fminus::ir::detail
{
    // Every module's struct type, in declaration order, so a field may be a module declared before (whose layout is
    // known), or point to any. A second module of a name is an error (the parser gave the name to the first). A field
    // is refused when it has an initialiser or is const (nothing runs when a module is made, so it would only ever be
    // 0), when its type is incomplete (void, or a module not yet laid out: its own, or a later one, by value) or one
    // FMinus cannot point to, and when another member has its name. A refused field takes a float's place, so the
    // module's uses report no more errors.
    void Lowering::layOut(const ast::Program& program)
    {
        for (const std::unique_ptr<ast::Module>& module : program.modules)
            structs_.emplace(module.get(), llvm::StructType::create(module_.getContext(), module->name));
        std::unordered_set<const ast::Module*> complete;
        std::unordered_set<std::string> modules;
        for (const std::unique_ptr<ast::Module>& module : program.modules)
        {
            if (!modules.insert(module->name).second)
                error(module->location, "redefinition of " + quoted(module->name));
            std::vector<llvm::Type*> elements;
            std::unordered_set<std::string> names;
            for (const ast::Field& field : module->fields)
            {
                ast::Type type = field.type;
                if (!names.insert(field.name).second)
                    error(field.location, "duplicate member " + quoted(field.name));
                if (field.isParam)
                {
                    checkParameter(*module, field);
                    elements.push_back(
                        llvm::StructType::get(module_.getContext(), {builder_.getPtrTy(), slotTypeOf(type)}));
                    continue;
                }
                if (field.hasInitializer)
                    error(field.location, "a module's fields start at 0 and take no initializer: set "
                                              + quoted(field.name) + " in a method, such as init");
                if (field.isConst)
                    error(field.location, "a module's fields cannot be const: they start at 0, and a method sets them");
                const bool byValue = ast::isModule(type) || (ast::isArray(type) && type.pointee == ast::Kind::Module);
                if (type == ast::Type::Void)
                {
                    error(field.location, "field has incomplete type 'void'");
                    type = ast::Type::Float;
                }
                else if (byValue && !complete.contains(type.module))
                {
                    error(field.location, "field has incomplete type " + quoted(ast::typeName(type))
                                              + ": a module holds only modules declared before it (and pointers)");
                    type = ast::Type::Float;
                }
                else if (!checkType(field.location, type))
                {
                    type = ast::Type::Float;
                }
                elements.push_back(typeOf(type));
            }
            structs_.at(module.get())->setBody(elements);
            complete.insert(module.get());
        }
        // Every binding names a parameter of the program.
        for (const auto& [name, binding] : bindings_)
        {
            bool found = false;
            for (const std::unique_ptr<ast::Module>& module : program.modules)
            {
                for (const ast::Field& field : module->fields)
                    found = found || (field.isParam && module->name + "::" + field.name == name);
            }
            if (!found)
                error({1, 1}, "the host binds " + quoted(name) + ", but the program has no such parameter");
        }
    }

    // A parameter's type (a float, an int or a bool), its binding (only a float can be a stream), and its attributes:
    // fminus::range(minimum, maximum) and fminus::default(value), numbers, the default within the range.
    void Lowering::checkParameter(const ast::Module& module, const ast::Field& field)
    {
        const ast::Type type = field.type;
        if (type != ast::Type::Float && type != ast::Type::Int && type != ast::Type::Bool)
            error(field.location, "a parameter is a float, an int or a bool, not " + quoted(ast::typeName(type)));
        else if (bindingOf(module, field).kind == ParamBinding::Kind::Stream && type != ast::Type::Float)
        {
            const std::string what = type == ast::Type::Int ? "an int" : "a bool";
            error(field.location,
                  "parameter " + quoted(field.name) + " is " + what + ": only a float parameter can be a stream");
        }
        std::optional<double> minimum, maximum, initial;
        for (const ast::Attribute& attribute : field.attributes)
        {
            std::vector<double> numbers;
            for (const std::string& arg : attribute.args)
            {
                if (const std::optional<double> number = ast::attributeNumber(arg))
                    numbers.push_back(*number);
                else
                    error(attribute.location, "invalid number " + quoted(arg) + " in " + quoted(attribute.name));
            }
            if (numbers.size() != attribute.args.size())
                continue;
            if (attribute.name == "fminus::range" && numbers.size() == 2)
            {
                minimum = numbers[0];
                maximum = numbers[1];
                if (*minimum >= *maximum)
                    error(attribute.location, "fminus::range's minimum must be below its maximum");
            }
            else if (attribute.name == "fminus::default" && numbers.size() == 1)
                initial = numbers[0];
            else if (attribute.name == "fminus::range")
                error(attribute.location, "'fminus::range' takes two numbers, a minimum and a maximum");
            else if (attribute.name == "fminus::default")
                error(attribute.location, "'fminus::default' takes one number");
            else
                error(attribute.location,
                      "unknown attribute " + quoted(attribute.name)
                          + ": a parameter takes fminus::range(min, max) and fminus::default(value)");
        }
        if (minimum && maximum && initial && (*initial < *minimum || *initial > *maximum))
            error(field.location, std::format("the default of {}, {}, is outside its fminus::range({}, {})",
                                              quoted(field.name), *initial, *minimum, *maximum));
    }

    // A parameter's value in its slot: a float, an int, or a bool in a byte (what C++'s bool is in memory, and what an
    // atomic load can read).
    llvm::Type* Lowering::slotTypeOf(ast::Type type)
    {
        if (type == ast::Type::Int)
            return builder_.getInt32Ty();
        if (type == ast::Type::Bool)
            return builder_.getInt8Ty();
        return builder_.getFloatTy();
    }

    // The host's binding of a parameter: a knob unless it says otherwise.
    ParamBinding Lowering::bindingOf(const ast::Module& module, const ast::Field& field) const
    {
        const auto found = bindings_.find(module.name + "::" + field.name);
        return found == bindings_.end() ? ParamBinding{} : found->second;
    }

    // The parameter `base` names, in a method of its module: a name no local or parameter has (cutoff), or this's
    // (this->cutoff). Its index among the fields; nothing for anything else.
    std::optional<unsigned> Lowering::parameterOf(const ast::Expr& base) const
    {
        const ast::Module* module = function_ != nullptr ? function_->module : nullptr;
        if (module == nullptr)
            return std::nullopt;
        std::string name;
        if (const auto* variable = std::get_if<ast::VariableExpr>(&base.node))
        {
            if (find(variable->name) != nullptr)
                return std::nullopt;
            name = variable->name;
        }
        else if (const auto* member = std::get_if<ast::MemberExpr>(&base.node);
                 member != nullptr && member->arrow && std::holds_alternative<ast::ThisExpr>(member->base->node))
            name = member->name;
        else
            return std::nullopt;
        unsigned index = 0;
        const ast::Field* field = findField(*module, name, &index);
        if (field == nullptr || !field->isParam)
            return std::nullopt;
        return index;
    }

    // cutoff[i]: the parameter's value at sample i of the call, as its binding has it.
    Lowering::Value Lowering::lowerParameter(SourceLocation location, unsigned field, const ast::IndexExpr& index)
    {
        const ast::Module& module = *function_->module;
        const ast::Field& parameter = module.fields[field];
        const bool integral = parameter.type == ast::Type::Int || parameter.type == ast::Type::Bool;
        const ast::Type type = integral ? parameter.type : ast::Type::Float;
        const Value at = lower(*index.index);
        if (at.type != ast::Type::Int && at.type != ast::Type::Bool)
        {
            error(location, "array subscript is not an integer");
            return poison(type);
        }
        const ParamBinding binding = bindingOf(module, parameter);
        ParamBinding::Kind kind = binding.kind;
        if (integral && kind == ParamBinding::Kind::Stream)   // reported in layOut: read as a knob
            kind = ParamBinding::Kind::Knob;
        switch (kind)
        {
        case ParamBinding::Kind::Frozen:
            if (type == ast::Type::Float)
                return {llvm::ConstantFP::get(builder_.getFloatTy(), binding.value), type};   // rounded to float
            if (type == ast::Type::Bool)
                return {builder_.getInt1(binding.value != 0.0), type};
            {
                // C's conversion: toward zero; out of an int's range, the nearest end of it.
                const double truncated = std::trunc(binding.value);
                const double clamped = std::clamp(truncated, double{std::numeric_limits<std::int32_t>::min()},
                                                  double{std::numeric_limits<std::int32_t>::max()});
                return {builder_.getInt32(static_cast<std::uint32_t>(static_cast<std::int32_t>(clamped))), type};
            }
        case ParamBinding::Kind::Knob:
            return {atEntry(field, false), type};
        case ParamBinding::Kind::Stream:
        {
            llvm::Value* offset = builder_.CreateSExt(convert(at, ast::Type::Int, location, Use::Initializing),
                                                      builder_.getInt64Ty(), "idxprom");
            llvm::Value* sample = builder_.CreateInBoundsGEP(builder_.getFloatTy(), atEntry(field, true), offset,
                                                             parameter.name + ".at");
            llvm::LoadInst* load = builder_.CreateLoad(builder_.getFloatTy(), sample, parameter.name);
            tag(load, ast::Type::Float);
            streamReads_.push_back(load);
            return {load, type};
        }
        }
        return poison(type);
    }

    // What the method reads of parameter `field` where it starts, once: its stream's pointer, or its value (an atomic
    // load, a bool's byte compared with 0). From `this` itself, the method's first argument. Each load carries its
    // type's TBAA tag (Tbaa.cpp): the pointer's, and the value's, as the parameter's type.
    //
    // The value's load is unordered, LLVM's weakest atomic ordering: it can't tear, so the method reads a value the
    // host stored, whole, however the host's thread races it; and unlike a monotonic (C++'s relaxed) load, LLVM may
    // move it and merge it with others. That matters once the method is inlined: a voice's filter.tick(), called a
    // sample from its synth's loop, has its knob's load in the loop, and an unordered one leaves the loop, read once
    // for the synth's call, where a monotonic one would stay, loaded every sample and keeping the loop from
    // vectorising.
    llvm::Value* Lowering::atEntry(unsigned field, bool stream)
    {
        const auto key = std::make_pair(field, stream);
        if (const auto found = entryLoads_.find(key); found != entryLoads_.end())
            return found->second;
        const ast::Module& module = *function_->module;
        const ast::Field& parameter = module.fields[field];
        llvm::Function* function = builder_.GetInsertBlock()->getParent();
        llvm::BasicBlock& entry = function->getEntryBlock();
        llvm::IRBuilder<> at(&entry, entry.getFirstInsertionPt());
        llvm::Value* slot =
            at.CreateStructGEP(structs_.at(&module), function->getArg(0), field, parameter.name + ".slot");
        llvm::Type* value = slotTypeOf(parameter.type);
        llvm::StructType* slotType = llvm::StructType::get(module_.getContext(), {builder_.getPtrTy(), value});
        llvm::Value* loaded = nullptr;
        if (stream)
        {
            llvm::LoadInst* load =
                at.CreateLoad(builder_.getPtrTy(), at.CreateStructGEP(slotType, slot, 0), parameter.name + ".stream");
            tag(load, ast::Type::pointerTo(ast::Kind::Float, true));
            loaded = load;
        }
        else
        {
            llvm::LoadInst* load = at.CreateAlignedLoad(value, at.CreateStructGEP(slotType, slot, 1),
                                                        llvm::Align(parameter.type == ast::Type::Bool ? 1 : 4),
                                                        parameter.name);
            load->setAtomic(llvm::AtomicOrdering::Unordered);
            tag(load, parameter.type);
            loaded = parameter.type == ast::Type::Bool ? at.CreateICmpNE(load, at.getInt8(0), parameter.name + ".on")
                                                       : static_cast<llvm::Value*>(load);
        }
        entryLoads_.emplace(key, loaded);
        return loaded;
    }

    // A stream's samples are the method's to read only, while it runs: nothing the method writes, or reads through
    // another pointer, overlaps them (the host's promise, as restrict's). LLVM learns it as it learns an inlined
    // restrict argument's: one alias scope, declared where the method starts, on the loads of the streams' samples,
    // and every other access to memory marked as outside it. Without it, a stream's load could read what the method
    // stored to its own fields, and a field (y += k * (x - y)) would go back to memory every sample.
    void Lowering::scopeStreams(llvm::Function& function)
    {
        llvm::MDBuilder md(module_.getContext());
        llvm::MDNode* domain = md.createAnonymousAliasScopeDomain(function.getName());
        llvm::MDNode* scope = md.createAnonymousAliasScope(domain, "streams");
        llvm::MDNode* scopes = llvm::MDNode::get(module_.getContext(), {scope});
        const std::unordered_set<llvm::Instruction*> reads(streamReads_.begin(), streamReads_.end());
        for (llvm::Instruction& instruction : llvm::instructions(function))
        {
            if (!instruction.mayReadOrWriteMemory())
                continue;
            instruction.setMetadata(reads.contains(&instruction) ? llvm::LLVMContext::MD_alias_scope
                                                                 : llvm::LLVMContext::MD_noalias,
                                    scopes);
        }
        llvm::IRBuilder<> at(&function.getEntryBlock(), function.getEntryBlock().getFirstInsertionPt());
        at.CreateNoAliasScopeDeclaration(scopes);
    }

    // The field `name` of `module`, and its index among the fields; nullptr when the module has none.
    const ast::Field* Lowering::findField(const ast::Module& module, const std::string& name, unsigned* index)
    {
        for (std::size_t i = 0; i < module.fields.size(); ++i)
        {
            if (module.fields[i].name != name)
                continue;
            if (index != nullptr)
                *index = static_cast<unsigned>(i);
            return &module.fields[i];
        }
        return nullptr;
    }

    // The field `name` of the module at `address`, an lvalue of the field's type, read-only when the module is (a
    // const variable, or one reached through a const pointer). clang's errors for a name the module has as a method,
    // and for one it does not have at all.
    std::optional<Lowering::LValue> Lowering::field(SourceLocation location, llvm::Value* address,
                                                    const ast::Module& module, const std::string& name, bool isConst)
    {
        unsigned index = 0;
        const ast::Field* found = findField(module, name, &index);
        if (found != nullptr && found->isParam)
        {
            error(location, "parameter " + quoted(name) + " is read as " + name + "[i], in a method of "
                                + quoted(module.name));
            return std::nullopt;
        }
        if (found == nullptr)
        {
            if (std::ranges::find(module.methods, name) != module.methods.end())
                error(location, "reference to non-static member function must be called");
            else
                error(location, "no member named " + quoted(name) + " in " + quoted(module.name));
            return std::nullopt;
        }
        llvm::Value* at = builder_.CreateStructGEP(structs_.at(&module), address, index, name);
        return LValue{at, found->type, isConst, "", found->type, {}};
    }

    // The method's `this`: the address of the module it was called on (its parameter's slot holds it).
    llvm::Value* Lowering::self()
    {
        const Binding* binding = find("this");
        llvm::LoadInst* load = builder_.CreateLoad(builder_.getPtrTy(), binding->slot, "this");
        tag(load, binding->type);
        return load;
    }

    // `this`, a pointer to the method's module; outside a method, clang's error.
    Lowering::Value Lowering::lowerThis(SourceLocation location)
    {
        const ast::Module* module = function_ != nullptr ? function_->module : nullptr;
        if (module == nullptr)
        {
            error(location, "invalid use of 'this' outside of a non-static member function");
            return poison(ast::Type::pointerTo(ast::Kind::Void, false));
        }
        return {self(), ast::Type::pointerTo(ast::Type::moduleType(module), false)};
    }

    // m.tick(x) and p->tick(x): the method of m's module (or of the module p points to), called on its address.
    Lowering::Value Lowering::lowerMethodCall(SourceLocation location, const ast::MethodCallExpr& call)
    {
        const ast::Module* module = nullptr;
        llvm::Value* address = nullptr;
        bool isConst = false;
        if (call.arrow)
        {
            const Value pointer = lower(*call.object);
            if (!ast::isPointer(pointer.type))
            {
                error(location, "member reference type " + quoted(ast::typeName(pointer.type)) + " is not a pointer"
                                    + (ast::isModule(pointer.type) ? "; did you mean to use '.'?" : ""));
                return poison(ast::Type::Float);
            }
            if (pointer.type.pointee != ast::Kind::Module)
            {
                error(location, "member reference base type " + quoted(ast::typeName(ast::pointee(pointer.type)))
                                    + " is not a structure or union");
                return poison(ast::Type::Float);
            }
            module = pointer.type.module;
            address = pointer.ir;
            isConst = pointer.type.pointeeConst;
        }
        else
        {
            Value object;
            if (isLValue(*call.object))
            {
                const std::optional<LValue> target = lvalue(*call.object, location);
                if (!target)
                    return poison(ast::Type::Float);
                object = target->components.empty() ? read(*target) : Value{nullptr, target->type};
                isConst = target->isConst;
            }
            else
            {
                object = lower(*call.object);
            }
            if (ast::isPointer(object.type) && object.type.pointee == ast::Kind::Module)
            {
                error(location, "member reference type " + quoted(ast::typeName(object.type))
                                    + " is a pointer; did you mean to use '->'?");
                return poison(ast::Type::Float);
            }
            if (!ast::isModule(object.type))
            {
                error(location, "member reference base type " + quoted(ast::typeName(object.type))
                                    + " is not a structure or union");
                return poison(ast::Type::Float);
            }
            module = object.type.module;
            address = object.ir;
        }
        std::vector<Value> args;
        for (const ast::ExprPtr& arg : call.args)
            args.push_back(lower(*arg));
        return callMethod(location, *module, address, isConst, call.method, args);
    }

    // A call of the method `name` on the module at `address`: `this` first, then the arguments, converted to the
    // parameters' types. FMinus has no const methods, so a method is not called on a module that is read-only here
    // (clang's error for a C++ method not marked const).
    Lowering::Value Lowering::callMethod(SourceLocation location, const ast::Module& module, llvm::Value* address,
                                         bool isConst, const std::string& name, const std::vector<Value>& args)
    {
        const std::string qualified = module.name + "::" + name;
        const auto found = functions_.find(qualified);
        if (found == functions_.end())
        {
            if (const ast::Field* field = findField(module, name, nullptr))
                error(location, "called object type " + quoted(ast::typeName(field->type))
                                    + " is not a function or function pointer");
            else
                error(location, "no member named " + quoted(name) + " in " + quoted(module.name));
            return poison(ast::Type::Float);
        }
        const ast::Function& callee = *found->second;
        if (isConst)
        {
            error(location, "'this' argument to member function " + quoted(name) + " has type "
                                + quoted("const " + module.name) + ", but function is not marked const");
            return poison(callee.returnType);
        }
        calls_[function_->name].push_back({location, qualified});   // what Effects.cpp checks
        if (args.size() + 1 != callee.params.size())
            return wrongArity(location, args.size(), callee.params.size() - 1);
        std::vector<llvm::Value*> irArgs{address};
        for (std::size_t i = 0; i < args.size(); ++i)
            irArgs.push_back(convert(args[i], callee.params[i + 1].type, location, Use::Passing));
        return {builder_.CreateCall(module_.getFunction(qualified), irArgs), callee.returnType};
    }

    // A module copied from `source` to `target`, both addresses of one: a memcpy of its size, as clang copies a
    // struct (the two are the same module, or two that do not overlap).
    void Lowering::copy(llvm::Value* target, llvm::Value* source, ast::Type type)
    {
        const llvm::Align align = alignOf(type);
        builder_.CreateMemCpy(target, align, source, align, *sizeOf(type));
    }
} // namespace fminus::ir::detail
