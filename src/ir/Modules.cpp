// src/ir/Modules.cpp: modules. Each is an LLVM struct type, laid out before any function is declared: its fields in
// order, as a C struct's (clang's %struct.S, padding where the data layout puts it). A field is reached by a
// getelementptr from the module's address; a method is a function named OnePole::tick, whose first parameter is
// `this`, the address of the module it was called on; inside it, a name no local or parameter has is looked up among
// the module's fields and methods, as C++ looks in the class (Memory.cpp, Calls.cpp). A module is never loaded whole:
// its Value is its address, and `a = b` copies it with a memcpy, as clang copies a struct.
#include "ir/Lowering.h"

#include <algorithm>
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
        return builder_.CreateLoad(builder_.getPtrTy(), find("this")->slot, "this");
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
