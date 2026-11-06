// src/opt/EarlyReturn.cpp: what the passes that make a call return early share (EarlyReturn.h).
#include "opt/EarlyReturn.h"

#include <llvm/IR/Attributes.h>
#include <llvm/IR/Constants.h>
#include <llvm/IR/Function.h>
#include <llvm/IR/IRBuilder.h>
#include <llvm/IR/InstrTypes.h>
#include <llvm/Support/ModRef.h>

namespace fminus::opt
{
    namespace
    {
        // What a function that may return early no longer promises of its result, which may be 0 or null: these go
        // from the function and from its calls, and so does `returned` from its parameters.
        constexpr llvm::Attribute::AttrKind kResultPromises[] = {
            llvm::Attribute::NonNull,
            llvm::Attribute::Dereferenceable,
            llvm::Attribute::Range,
            llvm::Attribute::NoFPClass,
        };
    } // namespace

    void reachedFunctions(llvm::CallBase& call, llvm::SmallVectorImpl<llvm::Function*>& functions,
                          llvm::SmallVectorImpl<llvm::Function*>* handed)
    {
        if (call.isInlineAsm())
            return;
        llvm::Function* callee = call.getCalledFunction();
        if (callee == nullptr)
            return;
        if (!callee->isDeclaration())
        {
            functions.push_back(callee);
            return;
        }
        for (llvm::Use& arg : call.args())
        {
            auto* function = llvm::dyn_cast<llvm::Function>(arg->stripPointerCasts());
            if (function == nullptr || function->isDeclaration())
                continue;
            functions.push_back(function);
            if (handed != nullptr)
                handed->push_back(function);
        }
    }

    void mayReturnEarly(llvm::Function& function)
    {
        const llvm::MemoryEffects memory = function.getMemoryEffects();
        const llvm::MemoryEffects early =
            memory | llvm::MemoryEffects(llvm::IRMemLocation::Other, llvm::ModRefInfo::ModRef);
        if (early != memory)
            function.setMemoryEffects(early);
        for (const llvm::Attribute::AttrKind kind : kResultPromises)
            function.removeRetAttr(kind);
        for (unsigned i = 0; i < function.arg_size(); ++i)
            function.removeParamAttr(i, llvm::Attribute::Returned);
        function.removeFnAttr(llvm::Attribute::NoReturn);
    }

    void mayReturnEarly(llvm::CallBase& call)
    {
        for (const llvm::Attribute::AttrKind kind : kResultPromises)
            call.removeRetAttr(kind);
        for (unsigned i = 0; i < call.arg_size(); ++i)
            call.removeParamAttr(i, llvm::Attribute::Returned);
        call.removeFnAttr(llvm::Attribute::NoReturn);
        const llvm::AttributeList attributes = call.getAttributes();
        if (attributes.hasFnAttr(llvm::Attribute::Memory))   // the call's own, not its callee's
            call.setMemoryEffects(attributes.getMemoryEffects()
                                  | llvm::MemoryEffects(llvm::IRMemLocation::Other, llvm::ModRefInfo::ModRef));
    }

    void returnEarly(llvm::IRBuilderBase& at, const llvm::Function& function)
    {
        llvm::Type* result = function.getReturnType();
        if (result->isVoidTy())
            at.CreateRetVoid();
        else
            at.CreateRet(llvm::Constant::getNullValue(result));
    }
} // namespace fminus::opt
