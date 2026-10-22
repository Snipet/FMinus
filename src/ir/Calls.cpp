// src/ir/Calls.cpp: calls. Inside a method, a method of its module first (`tick(x)` is this->tick(x), as in C++,
// where the class's names hide the others); then the builtins, in the order a call tries them: the reductions,
// poly_lanes and poly_index, poly_load and poly_store, <math.h>'s float functions (LLVM intrinsics, overloaded on
// vectors), <stdlib.h>'s heap (malloc, calloc and free) and the FFT (fft_create, fft_forward, ...), both on the
// runtime's functions; then the program's own functions. The runtime's functions are declared with what they do
// (describeRuntime), so LLVM does not take one for an unknown function that may read and write all memory.
#include "ir/Lowering.h"

#include <llvm/IR/Attributes.h>
#include <llvm/IR/Constants.h>
#include <llvm/Support/ModRef.h>

#include <array>
#include <optional>

namespace fminus::ir::detail
{
    // <math.h>'s float functions -> the LLVM intrinsic each lowers to, overloaded on float. sinf, cosf, expf and
    // logf become calls to libm on most targets; fabsf, sqrtf, floorf, fminf and fmaxf become instructions.
    constexpr std::array kBuiltins{
        Builtin{"fabsf", llvm::Intrinsic::fabs, 1},   Builtin{"sqrtf", llvm::Intrinsic::sqrt, 1},
        Builtin{"sinf", llvm::Intrinsic::sin, 1},     Builtin{"cosf", llvm::Intrinsic::cos, 1},
        Builtin{"expf", llvm::Intrinsic::exp, 1},     Builtin{"logf", llvm::Intrinsic::log, 1},
        Builtin{"floorf", llvm::Intrinsic::floor, 1}, Builtin{"fminf", llvm::Intrinsic::minnum, 2},
        Builtin{"fmaxf", llvm::Intrinsic::maxnum, 2}, Builtin{"tanhf", llvm::Intrinsic::tanh, 1},
    };

    // C's heap, <stdlib.h>'s functions, on the runtime's (runtime/Runtime.h): memory aligned to 64 bytes and zeroed.
    constexpr std::array kAllocations{
        Allocation{"malloc", "fminus_rt_malloc", 1},
        Allocation{"calloc", "fminus_rt_calloc", 2},
        Allocation{"free", "fminus_rt_free", 1},
    };

    // The FFT, on the runtime's (runtime/Fft.cpp): a plan for one size, made off the audio thread (it allocates its
    // tables and buffers, as calloc does), the transforms on it, which allocate nothing and may run on the audio
    // thread, and the plan's end. A real plan transforms n floats into n/2 + 1 bins, float2s (re, im), and back; a
    // complex plan n float2s into n, and back. Neither direction scales: inverse(forward(x)) is n times x.
    constexpr std::array kFfts{
        Fft{"fft_create", 1, true},  Fft{"fft_create_complex", 1, true}, Fft{"fft_destroy", 1, true},
        Fft{"fft_forward", 3, false}, Fft{"fft_inverse", 3, false},
    };

    namespace
    {
        // What a runtime function the builtins call does with memory (describeRuntime gives its declaration the
        // attributes that say so).
        enum class RuntimeKind
        {
            Allocation,    // fminus_rt_malloc and fminus_rt_calloc (runtime/Runtime.cpp): a new block, or null
            Release,       // fminus_rt_free: C's free
            Plan,          // fminus_rt_fft_create, fminus_rt_fft_create_complex (runtime/Fft.cpp): a new plan, or null
            PlanRelease,   // fminus_rt_fft_destroy
            Transform,     // fminus_rt_fft_forward_real and the others: reads one buffer, writes another
        };

        struct RuntimeFacts
        {
            std::string_view symbol;
            RuntimeKind kind;
        };

        constexpr std::array kRuntimeFacts{
            RuntimeFacts{"fminus_rt_malloc", RuntimeKind::Allocation},
            RuntimeFacts{"fminus_rt_calloc", RuntimeKind::Allocation},
            RuntimeFacts{"fminus_rt_free", RuntimeKind::Release},
            RuntimeFacts{"fminus_rt_fft_create", RuntimeKind::Plan},
            RuntimeFacts{"fminus_rt_fft_create_complex", RuntimeKind::Plan},
            RuntimeFacts{"fminus_rt_fft_destroy", RuntimeKind::PlanRelease},
            RuntimeFacts{"fminus_rt_fft_forward_real", RuntimeKind::Transform},
            RuntimeFacts{"fminus_rt_fft_inverse_real", RuntimeKind::Transform},
            RuntimeFacts{"fminus_rt_fft_forward_complex", RuntimeKind::Transform},
            RuntimeFacts{"fminus_rt_fft_inverse_complex", RuntimeKind::Transform},
        };

        // The heap's allocator family, named after its first function as LangRef asks: LLVM pairs an allocation with
        // a free only within one family.
        constexpr std::string_view kHeapFamily = "fminus_rt_malloc";

        // Gives the runtime's function `function` (kRuntimeFacts) the attributes that are true of its code, so LLVM
        // knows a call of it touches only what it touches: a call of fft_forward then changes no module's fields, and
        // memory from calloc is fresh and zeroed. Each one must stay true of runtime/Runtime.cpp and runtime/Fft.cpp:
        // an attribute that is not is a miscompile. Every one of them is noexcept (nounwind) and comes back
        // (willreturn). None takes nonnull: a null plan is a plan that does nothing, and the heap's results may be
        // null. Nor does an FFT buffer take noalias, though `out` must not overlap `in`: a program that transforms in
        // place gets what PFFFT gives it, as before.
        void describeRuntime(llvm::Function& function)
        {
            const RuntimeFacts* facts = nullptr;
            for (const RuntimeFacts& candidate : kRuntimeFacts)
            {
                if (candidate.symbol == std::string_view(function.getName()))
                    facts = &candidate;
            }
            if (facts == nullptr)
                return;
            llvm::LLVMContext& context = function.getContext();
            const llvm::Attribute keepsNone = llvm::Attribute::getWithCaptureInfo(context, llvm::CaptureInfo::none());
            function.setDoesNotThrow();
            function.setWillReturn();
            switch (facts->kind)
            {
            case RuntimeKind::Allocation:
                // count * size bytes (malloc's: size), at least, aligned to 64 and zeroed, or null: C's allocator, as
                // LLVM declares calloc (allockind, allocsize, a noalias result), zeroed for malloc too. So LLVM knows a
                // block is fresh and reads 0 until written, and may remove one the program never uses, as it may C's
                // (LangRef's allocation elision: the block comes from a "virtual" allocator, which does not fail).
                // Beyond the new block, it changes only the system allocator's state and errno, which no FMinus code
                // can read.
                function.addFnAttr(
                    llvm::Attribute::getWithAllocKind(context, llvm::AllocFnKind::Alloc | llvm::AllocFnKind::Zeroed));
                function.addFnAttr(llvm::Attribute::getWithAllocSizeArgs(
                    context, 0, function.arg_size() == 2 ? std::optional<unsigned>(1) : std::nullopt));
                function.addFnAttr("alloc-family", kHeapFamily);
                function.setMemoryEffects(
                    llvm::MemoryEffects::inaccessibleOrErrnoMemOnly(llvm::ModRefInfo::ModRef, llvm::ModRefInfo::Mod));
                function.addRetAttr(llvm::Attribute::NoAlias);
                function.addRetAttr(llvm::Attribute::NoUndef);
                function.addRetAttr(llvm::Attribute::getWithAlignment(context, llvm::Align(64)));
                break;
            case RuntimeKind::Release:
                // C's free of a block of the same family (or of null, which does nothing). It keeps no pointer.
                function.addFnAttr(llvm::Attribute::getWithAllocKind(context, llvm::AllocFnKind::Free));
                function.addFnAttr("alloc-family", kHeapFamily);
                function.setMemoryEffects(llvm::MemoryEffects::inaccessibleOrArgMemOnly());
                function.addParamAttr(0, llvm::Attribute::AllocatedPointer);
                function.addParamAttr(0, keepsNone);
                break;
            case RuntimeKind::Plan:
                // A new plan, or null. It allocates, but it is no allocator LLVM may remove: its null says that PFFFT
                // takes no plan of the size, every time, and a removed call would be taken for a plan made. What it
                // changes, beyond the new plan, is the system allocator's state and errno.
                function.setMemoryEffects(
                    llvm::MemoryEffects::inaccessibleOrErrnoMemOnly(llvm::ModRefInfo::ModRef, llvm::ModRefInfo::Mod));
                function.addRetAttr(llvm::Attribute::NoAlias);
                function.addRetAttr(llvm::Attribute::NoUndef);
                break;
            case RuntimeKind::PlanRelease:
                // Reads the plan and frees it, and the plan's own memory. It keeps no pointer.
                function.setMemoryEffects(llvm::MemoryEffects::inaccessibleOrArgMemOnly());
                function.addParamAttr(0, keepsNone);
                break;
            case RuntimeKind::Transform:
                // Reads the plan and `in` and writes `out` (where it reads only what it wrote), keeping no pointer to
                // any of them. Beyond them it touches only the plan's own memory, PFFFT's setup and the plan's three
                // buffers, which only the plan points to: inaccessible memory, since a plan is opaque, a void* that a
                // program hands to the FFT's builtins and reads nothing through. (One that converted it to a module
                // pointer and read the runtime's struct through that would read an object as a type it is not, which
                // C leaves undefined.) It allocates, frees and waits for nothing (the effects let it run on the audio
                // thread), so it is nofree and nosync too.
                function.setMemoryEffects(llvm::MemoryEffects::inaccessibleOrArgMemOnly());
                function.setDoesNotFreeMemory();
                function.setNoSync();
                function.addParamAttr(0, llvm::Attribute::ReadOnly);
                function.addParamAttr(1, llvm::Attribute::ReadOnly);
                function.addParamAttr(2, llvm::Attribute::WriteOnly);
                for (unsigned arg = 0; arg < 3; ++arg)
                    function.addParamAttr(arg, keepsNone);
                break;
            }
        }
    } // namespace

    // The reductions of a float vector to one float: its sum (in lane order, so the result does not depend on
    // fast-math), its least and its greatest lane (NaN lanes ignored, as fminf and fmaxf ignore them).
    constexpr std::array<std::string_view, 3> kReductions{"reduce_add", "reduce_min", "reduce_max"};

    bool isReduction(std::string_view name)
    {
        for (std::string_view reduction : kReductions)
        {
            if (reduction == name)
                return true;
        }
        return false;
    }

    // poly_lanes(): how many lanes a poly float has, an int. poly_index(): a poly float whose lanes hold their own
    // indices, 0, 1, 2, ...: what tells the lanes apart.
    bool isPolyQuery(std::string_view name)
    {
        return name == "poly_lanes" || name == "poly_index";
    }

    // poly_load(p) and poly_store(p, v): a poly float from, or to, the poly_lanes() floats at p, lane i at p[i]. With a
    // poly bool after them, only the lanes it sets: a lane it doesn't set is neither read (it loads as 0) nor written,
    // so the last, partial step of a loop over a buffer stays inside it.
    bool isPolyMemory(std::string_view name)
    {
        return name == "poly_load" || name == "poly_store";
    }

    const Allocation* findAllocation(std::string_view name)
    {
        for (const Allocation& allocation : kAllocations)
        {
            if (allocation.name == name)
                return &allocation;
        }
        return nullptr;
    }

    const Fft* findFft(std::string_view name)
    {
        for (const Fft& fft : kFfts)
        {
            if (fft.name == name)
                return &fft;
        }
        return nullptr;
    }

    // What the effects refuse a nonallocating function (Effects.cpp): a builtin that allocates or frees.
    bool allocates(std::string_view name)
    {
        const Fft* fft = findFft(name);
        return findAllocation(name) != nullptr || (fft != nullptr && fft->allocates);
    }

    const Builtin* findBuiltin(std::string_view name)
    {
        for (const Builtin& builtin : kBuiltins)
        {
            if (builtin.name == name)
                return &builtin;
        }
        return nullptr;
    }

    Lowering::Value Lowering::lowerCall(SourceLocation location, const ast::CallExpr& call)
    {
        std::vector<Value> args;
        for (const ast::ExprPtr& arg : call.args)
            args.push_back(lower(*arg));

        if (const ast::Module* module = function_->module;
            module != nullptr && functions_.contains(module->name + "::" + call.callee))
            return callMethod(location, *module, self(), false, call.callee, args);
        if (isReduction(call.callee))
            return lowerReduction(location, call, args);
        if (isPolyQuery(call.callee))
        {
            if (!args.empty())
                return wrongArity(location, args.size(), 0);
            if (call.callee == "poly_lanes")
                return {builder_.getInt32(polyLanes_), ast::Type::Int};
            std::vector<llvm::Constant*> indices;
            for (unsigned i = 0; i < polyLanes_; ++i)
                indices.push_back(llvm::ConstantFP::get(builder_.getFloatTy(), static_cast<double>(i)));
            return {llvm::ConstantVector::get(indices), ast::Type::PolyFloat};
        }
        if (isPolyMemory(call.callee))
            return lowerPolyMemory(location, call, args);
        if (const Allocation* allocation = findAllocation(call.callee))
        {
            calls_[function_->name].push_back({location, call.callee});   // what Effects.cpp checks
            return lowerAllocation(location, *allocation, args);
        }
        if (const Fft* fft = findFft(call.callee))
        {
            if (fft->allocates)
                calls_[function_->name].push_back({location, call.callee});
            return lowerFft(location, *fft, args);
        }
        std::vector<llvm::Value*> irArgs;
        if (const Builtin* builtin = findBuiltin(call.callee))
        {
            if (args.size() != builtin->arity)
                return wrongArity(location, args.size(), builtin->arity);
            // Overloaded on its arguments, as OpenCL C's math functions are: with a vector argument it is the
            // vector intrinsic (llvm.fabs.v4f32), and scalar arguments beside it are broadcast.
            ast::Type type = ast::Type::Float;
            for (const Value& arg : args)
            {
                if (!ast::isArithmetic(arg.type))
                {
                    incompatible(location, Use::Passing, arg.type, ast::Type::Float);
                    return poison(ast::Type::Float);
                }
                if (!commonType(location, type, arg.type, type))
                    return poison(ast::Type::Float);
            }
            for (const Value& arg : args)
                irArgs.push_back(convert(arg, type, location, Use::Passing));
            llvm::Function* intrinsic =
                llvm::Intrinsic::getOrInsertDeclaration(&module_, builtin->intrinsic, {typeOf(type)});
            return {builder_.CreateCall(intrinsic, irArgs), type};
        }
        const auto found = functions_.find(call.callee);
        if (found == functions_.end())
        {
            error(location, "call to undeclared function " + quoted(call.callee));
            return poison(ast::Type::Float);
        }
        const ast::Function& callee = *found->second;
        calls_[function_->name].push_back({location, call.callee});
        if (args.size() != callee.params.size())
            return wrongArity(location, args.size(), callee.params.size());
        for (std::size_t i = 0; i < args.size(); ++i)
            irArgs.push_back(convert(args[i], callee.params[i].type, location, Use::Passing));
        return {builder_.CreateCall(module_.getFunction(call.callee), irArgs), callee.returnType};
    }

    // reduce_add(v), reduce_min(v), reduce_max(v): a float vector to one float. The sum is ordered, lane 0
    // first, as a loop would add them (llvm.vector.reduce.fadd without the reassoc flag), so it is exact and
    // reproducible; reassociating it into a tree is fast-math's to allow. min and max ignore NaN lanes.
    Lowering::Value Lowering::lowerReduction(SourceLocation location, const ast::CallExpr& call,
                                             const std::vector<Value>& args)
    {
        if (args.size() != 1)
            return wrongArity(location, args.size(), 1);
        const Value& vector = args[0];
        if (!ast::isVector(vector.type) || ast::isMask(vector.type))
        {
            error(location, "the argument of " + quoted(call.callee) + " must be a float vector, not "
                                + quoted(ast::typeName(vector.type)));
            return poison(ast::Type::Float);
        }
        if (call.callee == "reduce_add")
            return {builder_.CreateFAddReduce(llvm::ConstantFP::getNegativeZero(builder_.getFloatTy()),
                                              vector.ir),
                    ast::Type::Float};
        if (call.callee == "reduce_min")
            return {builder_.CreateFPMinReduce(vector.ir), ast::Type::Float};
        return {builder_.CreateFPMaxReduce(vector.ir), ast::Type::Float};
    }

    // poly_load(p), poly_load(p, active), poly_store(p, v) and poly_store(p, v, active): OpenCL C's vloadn and vstoren
    // at the target's width, and Highway's masked forms. p is a const float* to load from (a float* or a void*
    // converts) and a float* to store to. The floats need only a float's alignment, as a buffer's do at any index, so
    // the load and store are unaligned vector ones; with a mask, llvm.masked.load (the lanes it leaves out 0) and
    // llvm.masked.store, which touch no memory in the lanes it leaves out.
    Lowering::Value Lowering::lowerPolyMemory(SourceLocation location, const ast::CallExpr& call,
                                              const std::vector<Value>& args)
    {
        const bool load = call.callee == "poly_load";
        const std::size_t arity = load ? 1 : 2;   // without the mask
        if (args.size() < arity)
            return wrongArity(location, args.size(), arity);
        if (args.size() > arity + 1)
            return wrongArity(location, args.size(), arity + 1);
        const llvm::Align align(4);
        llvm::Value* pointer =
            convert(args[0], ast::Type::pointerTo(ast::Kind::Float, load), location, Use::Passing);
        llvm::Value* value = load ? nullptr : convert(args[1], ast::Type::PolyFloat, location, Use::Passing);
        llvm::Value* mask =
            args.size() > arity ? convert(args[arity], ast::Type::PolyBool, location, Use::Passing) : nullptr;
        llvm::Type* vector = typeOf(ast::Type::PolyFloat);
        if (load && mask == nullptr)
            return {builder_.CreateAlignedLoad(vector, pointer, align, "poly.load"), ast::Type::PolyFloat};
        if (load)
            return {builder_.CreateMaskedLoad(vector, pointer, align, mask, llvm::Constant::getNullValue(vector),
                                              "poly.load"),
                    ast::Type::PolyFloat};
        if (mask == nullptr)
            return {builder_.CreateAlignedStore(value, pointer, align), ast::Type::Void};
        return {builder_.CreateMaskedStore(value, pointer, align, mask), ast::Type::Void};
    }

    // malloc(size), calloc(count, size) and free(p): C's, with sizes in ints (C's are size_t), on the runtime's
    // functions, which take 64-bit sizes. malloc and calloc return a void* (null when the size is not positive or the
    // memory is not there), which converts to any pointer; free takes any pointer, or null.
    Lowering::Value Lowering::lowerAllocation(SourceLocation location, const Allocation& allocation,
                                              const std::vector<Value>& args)
    {
        if (args.size() != allocation.arity)
            return wrongArity(location, args.size(), allocation.arity);
        const ast::Type voidPointer = ast::Type::pointerTo(ast::Kind::Void, false);
        const std::string runtime(allocation.runtime);
        if (allocation.name == "free")
        {
            llvm::Value* pointer = convert(args[0], voidPointer, location, Use::Passing);
            return {callRuntime(runtime, builder_.getVoidTy(), {pointer}), ast::Type::Void};
        }
        std::vector<llvm::Value*> sizes;
        for (const Value& arg : args)
        {
            llvm::Value* size = convert(arg, ast::Type::Int, location, Use::Passing);
            sizes.push_back(builder_.CreateSExt(size, builder_.getInt64Ty(), "size"));
        }
        llvm::CallInst* allocate = callRuntime(runtime, builder_.getPtrTy(), sizes);
        allocate->setName(allocation.name);
        return {allocate, voidPointer};
    }

    // fft_create(n) and fft_create_complex(n): a plan, a void* (null when the library takes no plan of that size);
    // fft_destroy(plan). fft_forward(plan, in, out) and fft_inverse(plan, in, out) are overloaded on their buffers, as
    // a math builtin is on its arguments: a forward transform of floats is a real one, of float2s a complex one, and
    // an inverse one is real when it gives floats. Each buffer converts as a parameter of its type would, const added
    // to the input's.
    Lowering::Value Lowering::lowerFft(SourceLocation location, const Fft& fft, const std::vector<Value>& args)
    {
        if (args.size() != fft.arity)
            return wrongArity(location, args.size(), fft.arity);
        const ast::Type plan = ast::Type::pointerTo(ast::Kind::Void, false);
        if (fft.name == "fft_create" || fft.name == "fft_create_complex")
        {
            llvm::Value* n = convert(args[0], ast::Type::Int, location, Use::Passing);
            const std::string runtime =
                fft.name == "fft_create" ? "fminus_rt_fft_create" : "fminus_rt_fft_create_complex";
            return {callRuntime(runtime, builder_.getPtrTy(), {n}), plan};
        }
        llvm::Value* setup = convert(args[0], plan, location, Use::Passing);
        if (fft.name == "fft_destroy")
            return {callRuntime("fminus_rt_fft_destroy", builder_.getVoidTy(), {setup}), ast::Type::Void};

        const bool forward = fft.name == "fft_forward";
        const auto pointsTo = [](const Value& value, ast::Kind kind) {
            return ast::isPointer(value.type) && value.type.pointee == kind;
        };
        const Value& decides = forward ? args[1] : args[2];   // the floats' side
        const bool real = pointsTo(decides, ast::Kind::Float);
        if (!real && !pointsTo(decides, ast::Kind::Float2))
        {
            error(location, "no matching function for call to " + quoted(fft.name) + ": it takes "
                                + (forward ? "(void *, const float *, float2 *) for a real plan, or (void *, const "
                                             "float2 *, float2 *) for a complex one"
                                           : "(void *, const float2 *, float *) for a real plan, or (void *, const "
                                             "float2 *, float2 *) for a complex one"));
            return poison(ast::Type::Void);
        }
        const ast::Type in = ast::Type::pointerTo(forward && real ? ast::Kind::Float : ast::Kind::Float2, true);
        const ast::Type out = ast::Type::pointerTo(!forward && real ? ast::Kind::Float : ast::Kind::Float2, false);
        llvm::Value* input = convert(args[1], in, location, Use::Passing);
        llvm::Value* output = convert(args[2], out, location, Use::Passing);
        const std::string runtime = std::string("fminus_rt_fft_") + (forward ? "forward" : "inverse")
                                    + (real ? "_real" : "_complex");
        return {callRuntime(runtime, builder_.getVoidTy(), {setup, input, output}), ast::Type::Void};
    }

    // A call of the runtime's function `name` (runtime/Runtime.h), declared from the arguments' IR types, with the
    // attributes that say what it does (describeRuntime). Not when the program declares a function of that name itself:
    // its prototype may give other types, and its definition would be its own code, not the runtime's.
    llvm::CallInst* Lowering::callRuntime(const std::string& name, llvm::Type* result,
                                          const std::vector<llvm::Value*>& args)
    {
        std::vector<llvm::Type*> types;
        for (llvm::Value* arg : args)
            types.push_back(arg->getType());
        llvm::FunctionCallee function =
            module_.getOrInsertFunction(name, llvm::FunctionType::get(result, types, false));
        if (auto* declared = llvm::dyn_cast<llvm::Function>(function.getCallee());
            declared != nullptr && !functions_.contains(name))
            describeRuntime(*declared);
        return builder_.CreateCall(function, args);
    }

    Lowering::Value Lowering::wrongArity(SourceLocation location, std::size_t have, std::size_t expected)
    {
        error(location, std::string(have > expected ? "too many" : "too few") + " arguments to function call, expected "
                            + std::to_string(expected) + ", have " + std::to_string(have));
        return poison(ast::Type::Float);
    }
} // namespace fminus::ir::detail

namespace fminus::ir
{
    std::vector<BuiltinInfo> builtins()
    {
        std::vector<BuiltinInfo> list;
        for (const detail::Builtin& builtin : detail::kBuiltins)
            list.push_back({builtin.name, builtin.arity});
        return list;
    }
} // namespace fminus::ir
