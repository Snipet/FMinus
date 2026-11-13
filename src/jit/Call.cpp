// src/jit/Call.cpp: the thunks that call a JIT'd function by its signature (Call.h).
#include "jit/Call.h"

#include "frontend/Ast.h"
#include "jit/Jit.h"

#include <llvm/IR/Attributes.h>
#include <llvm/IR/DerivedTypes.h>
#include <llvm/IR/Function.h>
#include <llvm/IR/IRBuilder.h>
#include <llvm/IR/LLVMContext.h>
#include <llvm/IR/Module.h>
#include <llvm/IR/Verifier.h>
#include <llvm/Support/raw_ostream.h>

#include <memory>
#include <vector>

namespace fminus::jit
{
    namespace
    {
        // The type the function takes or returns: FMinus's, as the code generator declares it (ir/Functions.cpp): a
        // bool an i1, zero-extended at the call, as C's ABI passes one.
        llvm::Type* irType(llvm::LLVMContext& context, CallType type)
        {
            switch (type)
            {
            case CallType::Void:
                return llvm::Type::getVoidTy(context);
            case CallType::Bool:
                return llvm::Type::getInt1Ty(context);
            case CallType::Int:
                return llvm::Type::getInt32Ty(context);
            case CallType::Float:
                return llvm::Type::getFloatTy(context);
            case CallType::Double:
                return llvm::Type::getDoubleTy(context);
            case CallType::Pointer:
                return llvm::PointerType::getUnqual(context);
            }
            return llvm::Type::getVoidTy(context);
        }

        // The type a slot holds it as: a bool a byte.
        llvm::Type* slotType(llvm::LLVMContext& context, CallType type)
        {
            return type == CallType::Bool ? llvm::Type::getInt8Ty(context) : irType(context, type);
        }
    } // namespace

    std::string thunkName(llvm::StringRef function)
    {
        return ("fminus.call." + function).str();
    }

    llvm::Expected<Thunk> callThunk(Jit& jit, llvm::StringRef function, CallType result,
                                    llvm::ArrayRef<CallType> parameters)
    {
        auto context = std::make_unique<llvm::LLVMContext>();
        const std::string name = thunkName(function);
        auto module = std::make_unique<llvm::Module>(name, *context);
        llvm::IRBuilder<> builder(*context);

        // The function, declared as the program defines it, its bools zero-extended.
        std::vector<llvm::Type*> types;
        for (const CallType parameter : parameters)
        {
            if (parameter == CallType::Void)
                return llvm::createStringError("fminus: a thunk's parameter of type void");
            types.push_back(irType(*context, parameter));
        }
        llvm::FunctionType* calleeType = llvm::FunctionType::get(irType(*context, result), types, false);
        llvm::Function* callee =
            llvm::Function::Create(calleeType, llvm::Function::ExternalLinkage, function, module.get());
        for (std::size_t i = 0; i < parameters.size(); ++i)
        {
            if (parameters[i] == CallType::Bool)
                callee->addParamAttr(static_cast<unsigned>(i), llvm::Attribute::ZExt);
        }
        if (result == CallType::Bool)
            callee->addRetAttr(llvm::Attribute::ZExt);

        // void thunk(ptr arguments, ptr result): the slots are the host's, 8 bytes each, aligned to 8.
        llvm::PointerType* pointer = llvm::PointerType::getUnqual(*context);
        llvm::Function* thunk =
            llvm::Function::Create(llvm::FunctionType::get(builder.getVoidTy(), {pointer, pointer}, false),
                                   llvm::Function::ExternalLinkage, name, module.get());
        llvm::Argument* arguments = thunk->getArg(0);
        llvm::Argument* out = thunk->getArg(1);
        arguments->setName("arguments");
        out->setName("result");
        thunk->addParamAttr(0, llvm::Attribute::NoAlias);
        thunk->addParamAttr(1, llvm::Attribute::NoAlias);
        builder.SetInsertPoint(llvm::BasicBlock::Create(*context, "entry", thunk));
        const llvm::Align slotAlign(alignof(CallSlot));
        std::vector<llvm::Value*> values;
        for (std::size_t i = 0; i < parameters.size(); ++i)
        {
            llvm::Value* slot = builder.CreateConstInBoundsGEP1_64(builder.getInt8Ty(), arguments,
                                                                   i * sizeof(CallSlot), "slot");
            llvm::Value* value = builder.CreateAlignedLoad(slotType(*context, parameters[i]), slot, slotAlign, "arg");
            if (parameters[i] == CallType::Bool)   // any byte but 0 is true, as a C++ bool's is
                value = builder.CreateICmpNE(value, builder.getInt8(0), "bool");
            values.push_back(value);
        }
        llvm::CallInst* call = builder.CreateCall(calleeType, callee, values);
        call->setAttributes(callee->getAttributes());
        if (result != CallType::Void)
        {
            llvm::Value* value = call;
            if (result == CallType::Bool)
                value = builder.CreateZExt(value, builder.getInt8Ty(), "byte");
            builder.CreateAlignedStore(value, out, slotAlign);
        }
        builder.CreateRetVoid();

        std::string broken;
        llvm::raw_string_ostream why(broken);
        if (llvm::verifyModule(*module, &why))
            return llvm::createStringError("fminus: the thunk of '" + function.str() + "' is not valid IR: " + broken);
        if (llvm::Error error = jit.add(std::move(module), std::move(context)))
            return error;
        auto address = jit.lookup(name);
        if (!address)
            return address.takeError();
        return address->toPtr<Thunk>();
    }

    std::optional<CallType> callType(const ast::Type& type)
    {
        switch (type.kind)
        {
        case ast::Kind::Void:
            return CallType::Void;
        case ast::Kind::Bool:
            return CallType::Bool;
        case ast::Kind::Int:
            return CallType::Int;
        case ast::Kind::Float:
            return CallType::Float;
        case ast::Kind::Double:
            return CallType::Double;
        case ast::Kind::Pointer:
        case ast::Kind::Handle:   // C's pointer to a struct the program never sees: a void* to a host
            return CallType::Pointer;
        case ast::Kind::Float2:
        case ast::Kind::Float4:
        case ast::Kind::Float8:
        case ast::Kind::Bool2:
        case ast::Kind::Bool4:
        case ast::Kind::Bool8:
        case ast::Kind::PolyFloat:
        case ast::Kind::PolyBool:
        case ast::Kind::Module:
        case ast::Kind::Array:
        case ast::Kind::Char:     // no value is one: an array's element, a pointer's pointee
        case ast::Kind::String:   // a pointer and a length, a struct C's ABI passes by each platform's rules
            return std::nullopt;
        }
        return std::nullopt;
    }
} // namespace fminus::jit
