// src/ir/Calls.cpp: calls. Inside a method, a method of its module first (`tick(x)` is this->tick(x), as in C++,
// where the class's names hide the others); then the builtins, in the order a call tries them: the reductions,
// poly_lanes and poly_index, <math.h>'s float functions (LLVM intrinsics, overloaded on vectors), and <stdlib.h>'s
// heap (malloc, calloc and free, on the runtime's functions); then the program's own functions.
#include "ir/Lowering.h"

#include <llvm/IR/Constants.h>

#include <array>

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

    const Allocation* findAllocation(std::string_view name)
    {
        for (const Allocation& allocation : kAllocations)
        {
            if (allocation.name == name)
                return &allocation;
        }
        return nullptr;
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
        if (const Allocation* allocation = findAllocation(call.callee))
        {
            calls_[function_->name].push_back({location, call.callee});   // what Effects.cpp checks
            return lowerAllocation(location, *allocation, args);
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

    // malloc(size), calloc(count, size) and free(p): C's, with sizes in ints (C's are size_t), on the runtime's
    // functions, which take 64-bit sizes. malloc and calloc return a void* (null when the size is not positive or the
    // memory is not there), which converts to any pointer; free takes any pointer, or null.
    Lowering::Value Lowering::lowerAllocation(SourceLocation location, const Allocation& allocation,
                                              const std::vector<Value>& args)
    {
        if (args.size() != allocation.arity)
            return wrongArity(location, args.size(), allocation.arity);
        const ast::Type voidPointer = ast::Type::pointerTo(ast::Kind::Void, false);
        llvm::Type* pointerType = builder_.getPtrTy();
        if (allocation.name == "free")
        {
            llvm::Value* pointer = convert(args[0], voidPointer, location, Use::Passing);
            const llvm::FunctionCallee free =
                module_.getOrInsertFunction(allocation.runtime, builder_.getVoidTy(), pointerType);
            return {builder_.CreateCall(free, {pointer}), ast::Type::Void};
        }
        std::vector<llvm::Value*> sizes;
        std::vector<llvm::Type*> sizeTypes;
        for (const Value& arg : args)
        {
            llvm::Value* size = convert(arg, ast::Type::Int, location, Use::Passing);
            sizes.push_back(builder_.CreateSExt(size, builder_.getInt64Ty(), "size"));
            sizeTypes.push_back(builder_.getInt64Ty());
        }
        const llvm::FunctionCallee allocate =
            module_.getOrInsertFunction(allocation.runtime, llvm::FunctionType::get(pointerType, sizeTypes, false));
        return {builder_.CreateCall(allocate, sizes, std::string(allocation.name)), voidPointer};
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
