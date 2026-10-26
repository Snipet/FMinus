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
//
// A stream whose pointer is null is flat for the call: cutoff[i] is then the slot's value, read as a knob's is, with
// no branch: sample i is a load from base + i * stride, the base and the stride chosen where the method starts (the
// stream's pointer and 1, or the slot's value field and 0, whose load a select replaces with the value), so a loop
// over a stream vectorises as before. And
// the values a knob can take are tagged where it is read (!fminus.domain: a bool's two, an int's small range, a
// stream's flat or not), for the knob versioning pass (opt/KnobVersioning.h), which gives a method whose loop branches
// on them, or reads a stream, a copy of its body for each.
#include "ir/Lowering.h"
#include "opt/KnobVersioning.h"

#include <llvm/IR/Constants.h>
#include <llvm/IR/InlineAsm.h>
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
    namespace
    {
        // The most values an int knob's domain lists: a range of more is not tagged, and its methods are not
        // versioned on it.
        constexpr double kLargestDomain = 8;

        // Tags a knob's value where the method starts with the values it can take, for knob versioning: a bool's (or a
        // stream's test for null, `isBool`) false and true, or an int's fminus::range when both its ends are integers
        // and it holds at most kLargestDomain values. Anything else stays untagged: a float's values, or a wide int's,
        // are too many to copy a method for. An int outside its range is still read, and still works: the pass sends
        // it to the method's original body.
        void tagDomain(llvm::Instruction& value, const ast::Field& parameter, bool isBool)
        {
            llvm::LLVMContext& context = value.getContext();
            std::vector<llvm::Metadata*> values;
            if (isBool)
            {
                values = {llvm::ConstantAsMetadata::get(llvm::ConstantInt::getFalse(context)),
                          llvm::ConstantAsMetadata::get(llvm::ConstantInt::getTrue(context))};
            }
            else
            {
                std::optional<double> minimum, maximum;
                for (const ast::Attribute& attribute : parameter.attributes)
                {
                    if (attribute.name != "fminus::range" || attribute.args.size() != 2)
                        continue;
                    minimum = ast::attributeNumber(attribute.args[0]);
                    maximum = ast::attributeNumber(attribute.args[1]);
                }
                if (!minimum || !maximum || *minimum != std::trunc(*minimum) || *maximum != std::trunc(*maximum)
                    || *minimum < double{std::numeric_limits<std::int32_t>::min()}
                    || *maximum > double{std::numeric_limits<std::int32_t>::max()} || *maximum < *minimum
                    || *maximum - *minimum + 1 > kLargestDomain)
                    return;
                for (auto v = static_cast<std::int64_t>(*minimum); v <= static_cast<std::int64_t>(*maximum); ++v)
                    values.push_back(llvm::ConstantAsMetadata::get(
                        llvm::ConstantInt::getSigned(llvm::Type::getInt32Ty(context), v)));
            }
            value.setMetadata(opt::kDomainMetadata, llvm::MDNode::get(context, values));
        }

        // 1, as LLVM cannot see it: what an empty asm statement returns when given 1 in the register it returns
        // (opt::kUnitConstraints; the optimisation barrier of benchmark::DoNotOptimize). A stream's stride is 0 when
        // the stream is flat, and this otherwise. A stride LLVM sees is 0 or 1 does not last: its instruction
        // combiner makes i * stride a select of 0 and i, and the loop vectoriser then sees no strided load, so a loop
        // over a stream that knob versioning did not copy would stay scalar. This one stays a multiplication by a
        // value LLVM does not know, on which the vectoriser versions the loop (a stride of 1: the vector loop). The
        // statement emits no instruction and touches no memory. It is made where the method starts; inlined into a
        // caller's loop, it leaves the loop with the rest of a stream's setup, unless the call sits under a condition
        // there (an asm statement is not speculatable, so LICM keeps it). Knob versioning makes it 1 again in the
        // copies it makes for a stream's samples.
        llvm::Value* opaqueUnit(llvm::IRBuilderBase& at, const std::string& name)
        {
            llvm::Type* type = at.getInt64Ty();
            llvm::InlineAsm* identity = llvm::InlineAsm::get(llvm::FunctionType::get(type, {type}, false), "",
                                                             opt::kUnitConstraints, /*hasSideEffects=*/false);
            llvm::CallInst* unit = at.CreateCall(identity, {at.getInt64(1)}, name);
            unit->setDoesNotAccessMemory();
            unit->setDoesNotThrow();
            unit->addFnAttr(llvm::Attribute::WillReturn);
            unit->addFnAttr(llvm::Attribute::NoSync);
            return unit;
        }
    } // namespace

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
            if (lookup(variable->name) != nullptr)
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
            // Sample i of the stream, or, when it is flat (its pointer null), the slot's value, with no branch: a
            // load from base + i * stride, which atEntry chose where the method starts, and a select of the value
            // read there when the stream is flat. The load is always valid: a flat stream's reads all read the slot's
            // value field, and discard what they read. So a loop over a stream vectorises whether or not knob
            // versioning gives the method a copy for each answer of the test (with its budget spent on other knobs,
            // say, or in a method without a loop, inlined into its caller's): LLVM's loop vectoriser versions the
            // loop on the stride being 1. In a copy the stride and the test are constants, and the read is the load
            // alone, or the value.
            llvm::Value* offset = builder_.CreateSExt(convert(at, ast::Type::Int, location, Use::Initializing),
                                                      builder_.getInt64Ty(), "idxprom");
            atEntry(field, true);
            const StreamRead stream = flatStreams_.at(field);
            llvm::Value* step =   // i times 0 or 1: it never wraps
                builder_.CreateMul(offset, stream.stride, parameter.name + ".offset", /*HasNUW=*/true, /*HasNSW=*/true);
            llvm::Value* sample =
                builder_.CreateInBoundsGEP(builder_.getFloatTy(), stream.base, step, parameter.name + ".at");
            llvm::LoadInst* load = builder_.CreateLoad(builder_.getFloatTy(), sample, parameter.name);
            tag(load, ast::Type::Float);
            streamReads_.push_back(load);
            return {builder_.CreateSelect(stream.flat, stream.value, load, parameter.name + ".read"), type};
        }
        }
        return poison(type);
    }

    // What the method reads of parameter `field` where it starts, once: its stream's pointer, or its value (an atomic
    // load, a bool's byte compared with 0). From `this` itself, the method's first argument. With a stream's pointer
    // come its test for null, the slot's value, read as a knob's, and where its samples are (flatStreams_): when the
    // stream is flat, at the slot's value field with a stride of 0, otherwise at the pointer with a stride of 1. A
    // bool's value, an int's and a stream's test carry their domains (tagDomain), for knob versioning. Each load
    // carries its type's TBAA tag (Tbaa.cpp): the pointer's, and the value's, as the parameter's type.
    //
    // A flat stream's reads load the value field only so that the load is valid: lowerParameter's select replaces what
    // they load with the value read here, once, for the whole call. So it does not matter that their loads are plain
    // ones, which the host's thread may race (a plain load that races a store reads an undefined value), nor that they
    // are in the streams' alias scope though the field is no stream's: a store the method makes through a pointer cast
    // from `this` could reach the field unseen, and change only what the select discards. The stride's 1 is an opaque
    // one (opaqueUnit), so that LLVM keeps the multiplication.
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
        if (!regions_.empty())   // the method's read, handed to the region (Parallel.cpp)
            return capturedParameter(field, stream);
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
            llvm::Value* valueField = at.CreateStructGEP(slotType, slot, 1);
            llvm::LoadInst* flatValue =
                at.CreateAlignedLoad(value, valueField, llvm::Align(4), parameter.name + ".value");
            flatValue->setAtomic(llvm::AtomicOrdering::Unordered);
            tag(flatValue, parameter.type);
            auto* flat = llvm::cast<llvm::Instruction>(at.CreateICmpEQ(
                loaded, llvm::ConstantPointerNull::get(builder_.getPtrTy()), parameter.name + ".flat"));
            tagDomain(*flat, parameter, true);
            llvm::Value* base = at.CreateSelect(flat, valueField, loaded, parameter.name + ".base");
            llvm::Value* unit = opaqueUnit(at, parameter.name + ".unit");
            llvm::Value* stride = at.CreateSelect(flat, at.getInt64(0), unit, parameter.name + ".stride");
            flatStreams_.emplace(field, StreamRead{flat, flatValue, base, stride});
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
            if (parameter.type != ast::Type::Float)
                tagDomain(*llvm::cast<llvm::Instruction>(loaded), parameter, parameter.type == ast::Type::Bool);
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
