// src/ir/Functions.cpp: functions. Declarations follow C's rules for prototypes and definitions, with the C ABI's
// attributes; a definition's parameters become stack slots, its body is lowered, and a non-void function that can
// fall off its end is an error (a void one returns there). A method is a function too, whose first parameter is
// `this` (Modules.cpp has the rest). A prototype of libm's, on a system whose libm sets no errno, says it writes no
// memory (describeLibm). Also the stack slots and the scopes variables live in.
#include "ir/Lowering.h"

#include <llvm/ADT/DepthFirstIterator.h>
#include <llvm/Analysis/TargetLibraryInfo.h>
#include <llvm/IR/CFG.h>
#include <llvm/IR/Verifier.h>
#include <llvm/Support/ModRef.h>
#include <llvm/Support/raw_ostream.h>
#include <llvm/TargetParser/Host.h>
#include <llvm/TargetParser/Triple.h>
#include <llvm/Transforms/Utils/BuildLibCalls.h>

namespace fminus::ir::detail
{
    bool Lowering::sameSignature(const ast::Function& a, const ast::Function& b)
    {
        if (a.returnType != b.returnType || a.params.size() != b.params.size())
            return false;
        for (std::size_t i = 0; i < a.params.size(); ++i)
        {
            if (a.params[i].type != b.params[i].type)   // const on a parameter is not part of the type
                return false;
        }
        return true;
    }

    void Lowering::declare(const ast::Function& function)
    {
        // Every declaration's attributes count, a prototype's or the definition's: the strongest is the function's.
        Effect& effect = effects_[function.name];
        effect = std::max(effect, declaredEffect(function));

        if (isReduction(function.name) || isPolyQuery(function.name) || isPolyMemory(function.name))
        {
            error(function.location, quoted(function.name) + " is a builtin and cannot be redefined");
            return;
        }
        const Builtin* builtin = findBuiltin(function.name);
        const Allocation* allocation = findAllocation(function.name);
        const Fft* fft = findFft(function.name);
        if (builtin != nullptr || allocation != nullptr || fft != nullptr)
        {
            // A prototype of a builtin, as <math.h> and <stdlib.h> have, declares nothing new; a definition is
            // refused.
            const unsigned arity = builtin != nullptr ? builtin->arity : allocation != nullptr ? allocation->arity
                                                                                                : fft->arity;
            if (function.isDefinition)
                error(function.location, quoted(function.name) + " is a builtin and cannot be redefined");
            else if (function.params.size() != arity)
                error(function.location, "conflicting types for " + quoted(function.name));
            return;
        }
        if (const auto found = functions_.find(function.name); found != functions_.end())
        {
            const ast::Function& previous = *found->second;
            if (!sameSignature(previous, function))
                error(function.location, "conflicting types for " + quoted(function.name));
            else if (function.isDefinition && previous.isDefinition)
                error(function.location, "redefinition of " + quoted(function.name));
            else if (function.isDefinition)
                found->second = &function;   // the definition, after its prototypes
            return;
        }
        if (function.module != nullptr)
        {
            const std::string member = function.name.substr(function.module->name.size() + 2);
            if (findField(*function.module, member, nullptr) != nullptr)
                error(function.location, "duplicate member " + quoted(member));
        }
        // A module goes to a function and comes back through a pointer: what the C ABI does with an aggregate this size
        // anyway, and what keeps a function from copying one it was only meant to change. (After the error, the
        // declaration takes a ptr, which is what a module's Value is.)
        const auto byValue = [&](SourceLocation location, ast::Type type, bool result) {
            if (!ast::isModule(type))
                return;
            const std::string pointer = quoted(ast::typeName(ast::Type::pointerTo(type, false)));
            error(location, result ? "FMinus returns no modules: write the result through a pointer, " + pointer
                                   : "FMinus passes a module by pointer: make the parameter " + pointer);
        };
        checkType(function.location, function.returnType);
        byValue(function.location, function.returnType, true);
        std::vector<llvm::Type*> params;
        for (const ast::Param& param : function.params)
        {
            if (param.type == ast::Type::Void)
                error(param.location, "argument may not have 'void' type");
            checkType(param.location, param.type);
            byValue(param.location, param.type, false);
            params.push_back(valueTypeOf(param.type == ast::Type::Void ? ast::Type::Float : param.type));
        }
        llvm::FunctionType* type = llvm::FunctionType::get(valueTypeOf(function.returnType), params, false);
        llvm::Function* llvmFunction =
            llvm::Function::Create(type, llvm::Function::ExternalLinkage, function.name, module_);
        // The C ABI passes and returns bool zero-extended: say so, or a C++ caller reads garbage bits. A restrict
        // pointer is noalias: nothing the function reaches otherwise overlaps what it points to.
        for (std::size_t i = 0; i < function.params.size(); ++i)
        {
            if (function.params[i].type == ast::Type::Bool)
                llvmFunction->addParamAttr(static_cast<unsigned>(i), llvm::Attribute::ZExt);
            if (function.params[i].isRestrict)
                llvmFunction->addParamAttr(static_cast<unsigned>(i), llvm::Attribute::NoAlias);
        }
        if (function.returnType == ast::Type::Bool)
            llvmFunction->addRetAttr(llvm::Attribute::ZExt);
        // A method's `this` is never null and points to a whole module, aligned, as clang says of C++'s: what lets the
        // optimiser hoist a field's load out of a loop.
        if (function.module != nullptr)
        {
            const ast::Type module = ast::Type::moduleType(function.module);
            const unsigned size = *sizeOf(module);
            llvm::AttrBuilder attributes(module_.getContext());
            attributes.addAttribute(llvm::Attribute::NonNull);
            attributes.addAlignmentAttr(alignOf(module));
            if (size > 0)
                attributes.addDereferenceableAttr(size);
            llvmFunction->addParamAttrs(0, attributes);
        }
        functions_.emplace(function.name, &function);
    }

    void Lowering::define(const ast::Function& function)
    {
        llvm::Function* llvmFunction = module_.getFunction(function.name);
        builder_.SetInsertPoint(llvm::BasicBlock::Create(module_.getContext(), "entry", llvmFunction));
        function_ = &function;
        scopes_.assign(1, {});   // the outermost scope: the parameters and the body's own declarations
        entryLoads_.clear();     // a method's module parameters, read where it starts (Modules.cpp)
        streamReads_.clear();
        flatStreams_.clear();

        // Each parameter gets a slot, as a local does (clang names them x.addr), so it can be assigned.
        for (std::size_t i = 0; i < function.params.size(); ++i)
        {
            const ast::Param& param = function.params[i];
            if (param.name.empty())
            {
                error(param.location, "parameter name omitted");
                continue;
            }
            if (param.type == ast::Type::Void)
                continue;   // declare() reported it
            // A pointer to what FMinus cannot point to (declare() reported it) is bound as a float*, and a module
            // passed by value (likewise) as a pointer to it, so their uses report no more errors.
            ast::Type type = ast::isPointer(param.type) && !ast::canPointTo(param.type.pointee)
                                 ? ast::Type::pointerTo(ast::Kind::Float, param.type.pointeeConst)
                                 : param.type;
            if (ast::isModule(type))
                type = ast::Type::pointerTo(type, false);
            llvm::Argument* argument = llvmFunction->getArg(static_cast<unsigned>(i));
            argument->setName(param.name);
            llvm::AllocaInst* slot = allocate(typeOf(type), param.name + ".addr");
            builder_.CreateStore(argument, slot);
            if (!bind(param.name, Binding{slot, param.isConst, type}))
                error(param.location, "redefinition of parameter " + quoted(param.name));
        }

        lowerStatements(function.body);

        if (!builder_.GetInsertBlock()->hasTerminator())
        {
            if (function.returnType == ast::Type::Void)
                builder_.CreateRetVoid();   // a void function may fall off its end, as in C
            else if (reachable(builder_.GetInsertBlock()))
            {
                error(function.location,
                      "non-void function " + quoted(function.name) + " does not return a value");
                builder_.CreateRet(poison(function.returnType).ir);
            }
            else
            {
                builder_.CreateUnreachable();   // the end follows a return: nothing runs here
            }
        }

        if (!streamReads_.empty())
            scopeStreams(*llvmFunction);

        std::string problem;
        llvm::raw_string_ostream os(problem);
        if (llvm::verifyFunction(*llvmFunction, &os))
            error(function.location, "internal error: the IR of " + quoted(function.name) + " is invalid: "
                                         + problem);
    }

    // A prototype without a definition calls the process's C function of its name (README.md): libm's, when its name
    // and its type are a math function's that TargetLibraryInfo knows (tanf, powf, atan2f, ldexpf, ...). LLVM's own
    // attributes for those (InferFunctionAttrs, which the pipeline runs) say that each may write errno and nothing else
    // but its pointers' memory; and a call that writes memory is never hoisted out of a loop, removed, or vectorised.
    // Apple's libm writes no errno (math_errhandling is MATH_ERREXCEPT, which is why clang defaults to -fno-math-errno
    // there), so on Darwin its functions get the same attributes without errno: tanf and powf then write no memory at
    // all, as clang declares them, and tanf(pi * cutoff / rate) of a knob is computed once a call, not every sample.
    // Elsewhere (glibc, the Windows CRT) libm sets errno, and the declarations stay as LLVM sees them. A definition is
    // the program's own code, and keeps its own attributes.
    void Lowering::describeLibm()
    {
        const llvm::Triple triple(targetTriple_.empty() ? llvm::sys::getProcessTriple() : targetTriple_);
        if (!triple.isOSDarwin())
            return;
        llvm::TargetLibraryInfoImpl implementation(triple);
        const llvm::TargetLibraryInfo library(implementation);
        for (const auto& [name, function] : functions_)
        {
            llvm::Function* declaration = module_.getFunction(name);
            llvm::LibFunc libFunc{};
            if (function->isDefinition || declaration == nullptr || !library.getLibFunc(*declaration, libFunc)
                || !library.has(libFunc))
                continue;
            llvm::inferNonMandatoryLibFuncAttrs(*declaration, library);
            // libm's effects are errno's and, for sincosf and remquof, their pointers': without errno, only theirs.
            // (Other C functions that write errno, the allocators, reach memory of their own, and keep it.)
            const llvm::MemoryEffects effects = declaration->getMemoryEffects();
            const llvm::MemoryEffects withoutErrno = effects.getWithoutLoc(llvm::IRMemLocation::ErrnoMem);
            if (withoutErrno != effects && withoutErrno.onlyAccessesArgPointees())
                declaration->setMemoryEffects(withoutErrno);
        }
    }

    // A slot at the top of the entry block, whatever block the builder is in: mem2reg promotes only those.
    llvm::AllocaInst* Lowering::allocate(llvm::Type* type, const std::string& name)
    {
        llvm::BasicBlock& entry = builder_.GetInsertBlock()->getParent()->getEntryBlock();
        llvm::IRBuilder<> atEntry(&entry, entry.begin());
        return atEntry.CreateAlloca(type, nullptr, name);
    }

    // Whether control can reach `block` from the function's entry. (Having predecessors is not enough: code
    // after a return can branch, and its blocks are all dead.)
    bool Lowering::reachable(llvm::BasicBlock* block)
    {
        for (llvm::BasicBlock* live : llvm::depth_first(&block->getParent()->getEntryBlock()))
        {
            if (live == block)
                return true;
        }
        return false;
    }

    // Adds `name` to the innermost scope; false when that scope already has it.
    bool Lowering::bind(const std::string& name, Binding binding)
    {
        return scopes_.back().emplace(name, binding).second;
    }

    // The innermost binding of `name`, or nullptr. In a parallel region's body, a variable of the function around the
    // region is captured (Parallel.cpp), and the binding is the region's.
    const Lowering::Binding* Lowering::find(const std::string& name)
    {
        for (std::size_t i = scopes_.size(); i-- > 0;)
        {
            const auto found = scopes_[i].find(name);
            if (found == scopes_[i].end())
                continue;
            if (regions_.empty() || i >= regions_.back().base)
                return &found->second;
            return capture(name, found->second);
        }
        return nullptr;
    }

    // The innermost binding of `name`, or nullptr, wherever it is: a look that captures nothing.
    const Lowering::Binding* Lowering::lookup(const std::string& name) const
    {
        for (auto scope = scopes_.rbegin(); scope != scopes_.rend(); ++scope)
        {
            const auto found = scope->find(name);
            if (found != scope->end())
                return &found->second;
        }
        return nullptr;
    }
} // namespace fminus::ir::detail
