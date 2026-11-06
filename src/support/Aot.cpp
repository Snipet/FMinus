// src/support/Aot.cpp. The build gives it FMINUS_AOT_DRIVER (this tree's C++ compiler), FMINUS_RUNTIME_LIBRARY (the
// path of libfminus_runtime.a) and FMINUS_AOT_SANITIZE (this tree's -fsanitize= list, or empty): src/CMakeLists.txt.
#include "support/Aot.h"

#include "opt/BoundsCheck.h"
#include "opt/LoopBudget.h"

#include <llvm/ADT/SmallString.h>
#include <llvm/IR/Constants.h>
#include <llvm/IR/GlobalVariable.h>
#include <llvm/IR/IRBuilder.h>
#include <llvm/IR/InlineAsm.h>
#include <llvm/IR/LegacyPassManager.h>
#include <llvm/IR/Module.h>
#include <llvm/IR/Verifier.h>
#include <llvm/Support/FileSystem.h>
#include <llvm/Support/FileUtilities.h>
#include <llvm/Support/Program.h>
#include <llvm/Support/raw_ostream.h>
#include <llvm/Target/TargetMachine.h>

#include <algorithm>
#include <cstdint>
#include <cstdlib>
#include <optional>
#include <vector>

namespace fminus::aot
{
    llvm::Error emitFile(llvm::Module& module, llvm::TargetMachine& machine, const std::string& path, FileKind kind)
    {
        if (module.getDataLayout() != machine.createDataLayout())
            return llvm::createStringError("emitFile: the module is not set up for this target machine "
                                           "(call prepareModule before optimising)");
        std::error_code ec;
        const llvm::sys::fs::OpenFlags flags =
            kind == FileKind::Assembly ? llvm::sys::fs::OF_Text : llvm::sys::fs::OF_None;
        llvm::raw_fd_ostream file(path, ec, flags);
        if (ec)
            return llvm::createFileError(path, ec);
        {
            // An object writer seeks back into its output: buffer a stream that cannot (stdout, a pipe).
            std::optional<llvm::buffer_ostream> buffer;
            llvm::raw_pwrite_stream* out = &file;
            if (!file.supportsSeeking())
                out = &buffer.emplace(file);

            llvm::legacy::PassManager passes;
            const llvm::CodeGenFileType type =
                kind == FileKind::Object ? llvm::CodeGenFileType::ObjectFile : llvm::CodeGenFileType::AssemblyFile;
            if (machine.addPassesToEmitFile(passes, *out, nullptr, type))
                return llvm::createStringError("emitFile: this target machine cannot emit that kind of file");
            passes.run(module);
        }
        file.flush();   // not close(): "-" is stdout, which raw_fd_ostream never closes
        if (file.has_error())
        {
            const std::error_code error = file.error();
            file.clear_error();   // else the stream's destructor reports a fatal error
            return llvm::createFileError(path, error);
        }
        return llvm::Error::success();
    }

    llvm::Error linkExecutable(const LinkOptions& options)
    {
        std::string driver = FMINUS_AOT_DRIVER;
        if (const char* chosen = std::getenv("FMINUS_LINK_DRIVER"); chosen != nullptr && *chosen != '\0')
            driver = chosen;
        llvm::ErrorOr<std::string> program = llvm::sys::findProgramByName(driver);
        if (!program)
            return llvm::createStringError(program.getError(), "linkExecutable: cannot find the driver " + driver);

        std::vector<std::string> args{*program};
        args.insert(args.end(), options.objects.begin(), options.objects.end());
        if (options.linkRuntime)
        {
            args.emplace_back(FMINUS_RUNTIME_LIBRARY);
            if (const std::string sanitize = FMINUS_AOT_SANITIZE; !sanitize.empty())
                args.push_back("-fsanitize=" + sanitize);
        }
        args.insert(args.end(), options.driverArgs.begin(), options.driverArgs.end());
        args.emplace_back("-o");
        args.push_back(options.output);

        const std::vector<llvm::StringRef> argRefs(args.begin(), args.end());
        std::string message;
        const int status = llvm::sys::ExecuteAndWait(*program, argRefs, std::nullopt, {}, 0, 0, &message);
        if (status != 0)
        {
            std::string command;
            for (const std::string& arg : args)
                command += (command.empty() ? "" : " ") + arg;
            return llvm::createStringError("linkExecutable: `" + command + "` failed (" + std::to_string(status) + ")"
                                           + (message.empty() ? "" : ": " + message));
        }
        return llvm::Error::success();
    }

    llvm::Error emitExecutable(llvm::Module& module, llvm::TargetMachine& machine, LinkOptions options)
    {
        llvm::SmallString<128> object;
        if (const std::error_code ec = llvm::sys::fs::createTemporaryFile("fminus-aot", "o", object))
            return llvm::createStringError(ec, "emitExecutable: cannot create a temporary object file");
        const llvm::FileRemover removeObject(object);
        if (llvm::Error error = emitFile(module, machine, std::string(object), FileKind::Object))
            return error;
        options.objects.insert(options.objects.begin(), std::string(object));
        return linkExecutable(options);
    }

    namespace
    {
        // After main's call, in the C main of a program compiled with bounds checks: when its record holds a failed
        // check, the message (fminus_rt_main_bounds) and exit status 4, else on at `builder`. The message's parts
        // are constant strings, in two tables indexed by the check's number less one.
        void failedCheck(llvm::Module& module, llvm::IRBuilder<>& builder, const std::vector<opt::BoundsSite>& sites,
                         llvm::Value* argc, llvm::Value* argv)
        {
            llvm::LLVMContext& context = module.getContext();
            llvm::GlobalVariable* record = opt::boundsGlobal(module);
            llvm::ArrayType* table = llvm::ArrayType::get(builder.getPtrTy(), sites.size());
            const auto strings = [&](const char* name, auto part) {
                std::vector<llvm::Constant*> parts;
                for (const opt::BoundsSite& site : sites)
                    parts.push_back(builder.CreateGlobalString(part(site), "bounds.message", 0, &module));
                return new llvm::GlobalVariable(module, table, true, llvm::GlobalValue::PrivateLinkage,
                                                llvm::ConstantArray::get(table, parts), name);
            };
            llvm::GlobalVariable* before = strings("fminus.bounds.before", opt::boundsMessageBefore);
            llvm::GlobalVariable* after = strings("fminus.bounds.after", opt::boundsMessageAfter);

            llvm::Function* entry = builder.GetInsertBlock()->getParent();
            llvm::Value* site = builder.CreateAlignedLoad(builder.getInt32Ty(), record, llvm::Align(8),
                                                          /*isVolatile=*/true, "site");
            llvm::BasicBlock* failed = llvm::BasicBlock::Create(context, "check.failed", entry);
            llvm::BasicBlock* passed = llvm::BasicBlock::Create(context, "checks.passed", entry);
            builder.CreateCondBr(builder.CreateICmpNE(site, builder.getInt32(0)), failed, passed);
            builder.SetInsertPoint(failed);
            llvm::Value* recorded = builder.CreateStructGEP(record->getValueType(), record, 2);
            llvm::Value* index =
                builder.CreateAlignedLoad(builder.getInt64Ty(), recorded, llvm::Align(8), /*isVolatile=*/true, "index");
            llvm::Value* number =
                builder.CreateZExt(builder.CreateSub(site, builder.getInt32(1)), builder.getInt64Ty());
            const auto pick = [&](llvm::GlobalVariable* parts) {
                llvm::Value* at = builder.CreateInBoundsGEP(table, parts, {builder.getInt64(0), number});
                return builder.CreateLoad(builder.getPtrTy(), at);
            };
            builder.CreateCall(module.getOrInsertFunction("fminus_rt_main_bounds", builder.getVoidTy(),
                                                          builder.getInt32Ty(), builder.getPtrTy(), builder.getPtrTy(),
                                                          builder.getInt64Ty(), builder.getPtrTy()),
                               {argc, argv, pick(before), index, pick(after)});
            builder.CreateRet(builder.getInt32(4));
            builder.SetInsertPoint(passed);
        }
    } // namespace

    llvm::Error addEntryPoint(llvm::Module& module, std::optional<std::int64_t> loopBudget)
    {
        llvm::Function* program = module.getFunction("main");
        if (program == nullptr || program->isDeclaration())
            return llvm::createStringError("an executable needs a main function, such as float main(float x)");
        llvm::LLVMContext& context = module.getContext();
        llvm::Type* floatType = llvm::Type::getFloatTy(context);
        llvm::Type* doubleType = llvm::Type::getDoubleTy(context);
        const llvm::FunctionType* programType = program->getFunctionType();
        llvm::Type* resultType = programType->getReturnType();
        const bool printsBool = resultType->isIntegerTy(1);   // an FMinus bool
        bool numbers = (resultType == floatType || resultType == doubleType || printsBool) && !programType->isVarArg();
        bool doubles = false;   // a double parameter: the arguments are read as their types say
        for (const llvm::Type* param : programType->params())
        {
            numbers = numbers && (param == floatType || param == doubleType);
            doubles = doubles || param == doubleType;
        }
        if (!numbers)
            return llvm::createStringError(
                "'main' must return float, double or bool and take only float and double parameters");
        program->setName("fminus.main");
        program->setLinkage(llvm::GlobalValue::InternalLinkage);

        // int main(int argc, char** argv)
        llvm::IRBuilder<> builder(context);
        llvm::Type* intType = builder.getInt32Ty();
        llvm::Type* pointerType = builder.getPtrTy();
        llvm::Function* entry =
            llvm::Function::Create(llvm::FunctionType::get(intType, {intType, pointerType}, false),
                                   llvm::GlobalValue::ExternalLinkage, "main", module);
        llvm::Argument* argc = entry->getArg(0);
        llvm::Argument* argv = entry->getArg(1);
        argc->setName("argc");
        argv->setName("argv");
        // A main of floats reads them as floats; one with a double among them reads each as its type says, into
        // doubles (a float's widened, exactly), with the kinds in a string, "fd" for (float, double).
        const llvm::FunctionCallee readArgs =
            doubles ? module.getOrInsertFunction("fminus_rt_main_double_args", intType, intType, pointerType, intType,
                                                 pointerType, pointerType)
                    : module.getOrInsertFunction("fminus_rt_main_args", intType, intType, pointerType, intType,
                                                 pointerType);
        // A bool crosses the C ABI zero-extended, as FMinus's own bool parameters do (ir/CodeGen.cpp).
        llvm::FunctionCallee printResult = module.getOrInsertFunction(
            printsBool                  ? "fminus_rt_main_bool_result"
            : resultType == doubleType ? "fminus_rt_main_double_result"
                                       : "fminus_rt_main_result",
            builder.getVoidTy(), resultType);
        if (printsBool)
            llvm::cast<llvm::Function>(printResult.getCallee())->addParamAttr(0, llvm::Attribute::ZExt);

        llvm::BasicBlock* start = llvm::BasicBlock::Create(context, "entry", entry);
        llvm::BasicBlock* run = llvm::BasicBlock::Create(context, "run", entry);
        llvm::BasicBlock* usage = llvm::BasicBlock::Create(context, "usage", entry);
        builder.SetInsertPoint(start);
        const unsigned count = programType->getNumParams();
        llvm::ArrayType* argsType = llvm::ArrayType::get(doubles ? doubleType : floatType, count);
        llvm::Value* args = builder.CreateAlloca(argsType, nullptr, "args");
        std::vector<llvm::Value*> readArgsArgs{argc, argv, builder.getInt32(count)};
        if (doubles)
        {
            std::string kinds;
            for (const llvm::Type* param : programType->params())
                kinds += param == doubleType ? 'd' : 'f';
            readArgsArgs.push_back(builder.CreateGlobalString(kinds, "main.kinds", 0, &module));
        }
        readArgsArgs.push_back(args);
        llvm::Value* status = builder.CreateCall(readArgs, readArgsArgs, "status");
        builder.CreateCondBr(builder.CreateICmpEQ(status, builder.getInt32(0)), run, usage);

        builder.SetInsertPoint(run);
        std::vector<llvm::Value*> values;
        for (unsigned i = 0; i < count; ++i)
        {
            llvm::Value* value = builder.CreateLoad(argsType->getElementType(),
                                                    builder.CreateConstInBoundsGEP2_32(argsType, args, 0, i));
            if (doubles && programType->getParamType(i) == floatType)
                value = builder.CreateFPTrunc(value, floatType);   // exact: it was a float, widened
            values.push_back(value);
        }
        // A program with a parallel region runs it on the runtime's pool, there while main runs.
        const bool parallel = module.getFunction("fminus_rt_parallel_for") != nullptr;
        if (parallel)
            builder.CreateCall(module.getOrInsertFunction("fminus_rt_parallel_start", intType));
        // The loop budget, set for the call and read after it (-1 or more, as Program::setLoopBudget sets it); and the
        // bounds checks' record, read after it, in a module compiled with them (its flag) that has any.
        //
        // Until the passes at the end of the pipeline put the program's checks in, main is a function that may never
        // return (a loop, or a check that always fails, a trap until then), and touches neither: FunctionAttrs would
        // mark it noreturn, and SimplifyCFG would delete what follows its call here, the flag's check and the message,
        // for an `unreachable` that the stopped call then returns into (a trap on arm64); or fold the flag's read to
        // the 0 written before the call. So the C main then calls main through its address passed through an empty
        // inline asm, as Denormals::Flush's wrapper calls its body (ir/Denormals.cpp): a call of a function LLVM
        // cannot see, which may return, and read and write any memory, and which no inliner inlines. The accesses are
        // volatile besides.
        const bool checked = module.getModuleFlag(opt::kBoundsCheckFlag) != nullptr;
        const std::vector<opt::BoundsSite> sites = checked ? opt::boundsSites(module) : std::vector<opt::BoundsSite>{};
        llvm::GlobalVariable* budget = nullptr;
        llvm::Value* callee = program;
        if (loopBudget)
        {
            budget = opt::loopBudgetGlobal(module);
            entry->addFnAttr(opt::kLoopBudgetHostAttribute);
            const std::int64_t left = std::max<std::int64_t>(*loopBudget, -1);
            builder.CreateAlignedStore(builder.getInt64(static_cast<std::uint64_t>(left)), budget, llvm::Align(8),
                                       /*isVolatile=*/true);
            builder.CreateAlignedStore(builder.getInt32(0), builder.CreateStructGEP(budget->getValueType(), budget, 1),
                                       llvm::Align(4), /*isVolatile=*/true);
        }
        if (loopBudget || !sites.empty())
        {
            llvm::InlineAsm* launder = llvm::InlineAsm::get(llvm::FunctionType::get(pointerType, {pointerType}, false),
                                                            "", "=r,0", /*hasSideEffects=*/false);
            llvm::CallInst* address = builder.CreateCall(launder, {program}, "main.address");
            address->setDoesNotAccessMemory();
            address->setDoesNotThrow();
            address->addFnAttr(llvm::Attribute::WillReturn);
            callee = address;
        }
        llvm::CallInst* result = builder.CreateCall(program->getFunctionType(), callee, values, "result");
        if (callee != program)
        {
            // An indirect call has no callee to read the C ABI's attributes from: it carries main's own (a bool's
            // zeroext), and none of its function attributes.
            const llvm::AttributeList attributes = program->getAttributes();
            std::vector<llvm::AttributeSet> parameters;
            for (unsigned i = 0; i < program->arg_size(); ++i)
                parameters.push_back(attributes.getParamAttrs(i));
            result->setCallingConv(program->getCallingConv());
            result->setAttributes(
                llvm::AttributeList::get(context, llvm::AttributeSet(), attributes.getRetAttrs(), parameters));
        }
        if (parallel)
            builder.CreateCall(module.getOrInsertFunction("fminus_rt_parallel_stop", builder.getVoidTy()));
        if (!sites.empty())
            failedCheck(module, builder, sites, argc, argv);
        if (budget != nullptr)
        {
            llvm::Value* stopped =
                builder.CreateAlignedLoad(builder.getInt32Ty(), builder.CreateStructGEP(budget->getValueType(), budget, 1),
                                          llvm::Align(4), /*isVolatile=*/true, "stopped");
            llvm::BasicBlock* ranOut = llvm::BasicBlock::Create(context, "ran.out", entry);
            llvm::BasicBlock* within = llvm::BasicBlock::Create(context, "within", entry);
            builder.CreateCondBr(builder.CreateICmpNE(stopped, builder.getInt32(0)), ranOut, within);
            builder.SetInsertPoint(ranOut);
            builder.CreateCall(module.getOrInsertFunction("fminus_rt_main_loop_budget", builder.getVoidTy(), intType,
                                                          pointerType, builder.getInt64Ty()),
                               {argc, argv, builder.getInt64(static_cast<std::uint64_t>(*loopBudget))});
            builder.CreateRet(builder.getInt32(3));
            builder.SetInsertPoint(within);
        }
        llvm::CallInst* print = builder.CreateCall(printResult, {result});
        if (printsBool)
            print->addParamAttr(0, llvm::Attribute::ZExt);
        builder.CreateRet(builder.getInt32(0));

        builder.SetInsertPoint(usage);
        builder.CreateRet(status);

        std::string problem;
        llvm::raw_string_ostream os(problem);
        if (llvm::verifyFunction(*entry, &os))
            return llvm::createStringError("addEntryPoint: the entry's IR is invalid: " + problem);
        return llvm::Error::success();
    }
} // namespace fminus::aot
