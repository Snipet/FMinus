// src/ir/CodeGen.cpp
//
// Two passes over the program. The first declares every function, so calls may come before definitions. The second
// lowers each body with an IRBuilder. A `let` binds the SSA value of its expression, since bindings never change. After
// an error the lowering goes on with a poison value, so one run reports every error.
#include "ir/CodeGen.h"

#include <llvm/ADT/APFloat.h>
#include <llvm/IR/Constants.h>
#include <llvm/IR/IRBuilder.h>
#include <llvm/IR/Intrinsics.h>
#include <llvm/IR/LLVMContext.h>
#include <llvm/IR/Module.h>
#include <llvm/IR/Verifier.h>
#include <llvm/Support/Error.h>
#include <llvm/Support/raw_ostream.h>

#include <array>
#include <string>
#include <unordered_map>

namespace fminus::ir
{
    namespace
    {
        template <typename... Fs>
        struct Overloaded : Fs...
        {
            using Fs::operator()...;
        };

        struct Builtin
        {
            std::string_view name;
            llvm::Intrinsic::ID intrinsic;
            unsigned arity;
        };

        // name -> the LLVM intrinsic it lowers to, overloaded on float. sin, cos, exp and log become calls to libm on
        // most targets; abs, sqrt, floor, min and max become instructions.
        constexpr std::array kBuiltins{
            Builtin{"abs", llvm::Intrinsic::fabs, 1},   Builtin{"sqrt", llvm::Intrinsic::sqrt, 1},
            Builtin{"sin", llvm::Intrinsic::sin, 1},    Builtin{"cos", llvm::Intrinsic::cos, 1},
            Builtin{"exp", llvm::Intrinsic::exp, 1},    Builtin{"log", llvm::Intrinsic::log, 1},
            Builtin{"floor", llvm::Intrinsic::floor, 1}, Builtin{"min", llvm::Intrinsic::minnum, 2},
            Builtin{"max", llvm::Intrinsic::maxnum, 2},
        };

        const Builtin* findBuiltin(std::string_view name)
        {
            for (const Builtin& builtin : kBuiltins)
            {
                if (builtin.name == name)
                    return &builtin;
            }
            return nullptr;
        }

        std::string quoted(std::string_view name)
        {
            return "'" + std::string(name) + "'";
        }

        class Lowering
        {
        public:
            Lowering(llvm::Module& module, Diagnostics& diagnostics)
                : module_(module)
                , diagnostics_(diagnostics)
                , builder_(module.getContext())
            {
            }

            void lower(const ast::Program& program)
            {
                for (const ast::Function& function : program.functions)
                    declare(function);
                for (const ast::Function& function : program.functions)
                {
                    const auto found = functions_.find(function.name);
                    if (found != functions_.end() && found->second == &function)   // not a rejected duplicate
                        define(function);
                }
            }

        private:
            llvm::Type* typeOf(ast::Type type)
            {
                switch (type)
                {
                case ast::Type::Float:
                    return builder_.getFloatTy();
                }
                return builder_.getFloatTy();
            }

            llvm::Value* poison(ast::Type type)
            {
                return llvm::PoisonValue::get(typeOf(type));
            }

            void error(SourceLocation location, std::string message)
            {
                diagnostics_.error(location, std::move(message));
            }

            // ---- declarations -----------------------------------------------------------------------------------
            void declare(const ast::Function& function)
            {
                if (findBuiltin(function.name) != nullptr)
                {
                    error(function.location, quoted(function.name) + " is a builtin and cannot be defined");
                    return;
                }
                if (functions_.contains(function.name))
                {
                    error(function.location, "function " + quoted(function.name) + " is already defined");
                    return;
                }
                std::vector<llvm::Type*> params;
                for (const ast::Param& param : function.params)
                    params.push_back(typeOf(param.type));
                llvm::FunctionType* type = llvm::FunctionType::get(typeOf(function.returnType), params, false);
                llvm::Function::Create(type, llvm::Function::ExternalLinkage, function.name, module_);
                functions_.emplace(function.name, &function);
            }

            // ---- bodies -----------------------------------------------------------------------------------------
            void define(const ast::Function& function)
            {
                llvm::Function* llvmFunction = module_.getFunction(function.name);
                builder_.SetInsertPoint(llvm::BasicBlock::Create(module_.getContext(), "entry", llvmFunction));
                scope_.clear();
                for (std::size_t i = 0; i < function.params.size(); ++i)
                {
                    const ast::Param& param = function.params[i];
                    llvm::Argument* argument = llvmFunction->getArg(static_cast<unsigned>(i));
                    argument->setName(param.name);
                    if (!scope_.emplace(param.name, argument).second)
                        error(param.location, "parameter " + quoted(param.name) + " is declared twice");
                }

                bool returned = false;
                for (const ast::Stmt& stmt : function.body)
                {
                    if (returned)
                    {
                        error(stmt.location, "statement after the return of " + quoted(function.name));
                        break;
                    }
                    std::visit(Overloaded{
                                   [&](const ast::LetStmt& let) { bind(stmt.location, let); },
                                   [&](const ast::ReturnStmt& ret) {
                                       builder_.CreateRet(lower(*ret.value));
                                       returned = true;
                                   },
                               },
                               stmt.node);
                }
                if (!returned)
                {
                    error(function.location, "function " + quoted(function.name) + " does not end with a return");
                    builder_.CreateRet(poison(function.returnType));
                }

                std::string problem;
                llvm::raw_string_ostream os(problem);
                if (llvm::verifyFunction(*llvmFunction, &os))
                    error(function.location, "internal error: the IR of " + quoted(function.name) + " is invalid: "
                                                 + problem);
            }

            void bind(SourceLocation location, const ast::LetStmt& let)
            {
                llvm::Value* value = lower(*let.value);
                // Name the instruction after the binding, for readable IR (a constant or an argument keeps its own).
                auto* instruction = llvm::dyn_cast<llvm::Instruction>(value);
                if (instruction != nullptr && !instruction->hasName())
                    instruction->setName(let.name);
                if (!scope_.emplace(let.name, value).second)
                    error(location, quoted(let.name) + " is already defined");
            }

            llvm::Value* lower(const ast::Expr& expr)
            {
                return std::visit(
                    Overloaded{
                        [&](const ast::NumberExpr& number) { return lowerNumber(expr.location, number); },
                        [&](const ast::VariableExpr& variable) -> llvm::Value* {
                            const auto found = scope_.find(variable.name);
                            if (found != scope_.end())
                                return found->second;
                            error(expr.location, "unknown variable " + quoted(variable.name));
                            return poison(ast::Type::Float);
                        },
                        [&](const ast::UnaryExpr& unary) -> llvm::Value* {
                            switch (unary.op)
                            {
                            case ast::UnaryOp::Negate:
                                return builder_.CreateFNeg(lower(*unary.operand));
                            }
                            return poison(ast::Type::Float);
                        },
                        [&](const ast::BinaryExpr& binary) -> llvm::Value* {
                            llvm::Value* lhs = lower(*binary.lhs);
                            llvm::Value* rhs = lower(*binary.rhs);
                            switch (binary.op)
                            {
                            case ast::BinaryOp::Add:
                                return builder_.CreateFAdd(lhs, rhs);
                            case ast::BinaryOp::Subtract:
                                return builder_.CreateFSub(lhs, rhs);
                            case ast::BinaryOp::Multiply:
                                return builder_.CreateFMul(lhs, rhs);
                            case ast::BinaryOp::Divide:
                                return builder_.CreateFDiv(lhs, rhs);
                            }
                            return poison(ast::Type::Float);
                        },
                        [&](const ast::CallExpr& call) { return lowerCall(expr.location, call); },
                    },
                    expr.node);
            }

            // The literal rounded once, from its decimal spelling straight to float (no detour through double).
            llvm::Value* lowerNumber(SourceLocation location, const ast::NumberExpr& number)
            {
                llvm::APFloat value(llvm::APFloat::IEEEsingle());
                auto status = value.convertFromString(number.spelling, llvm::APFloat::rmNearestTiesToEven);
                if (!status)
                {
                    llvm::consumeError(status.takeError());
                    error(location, "malformed number " + quoted(number.spelling));
                    return poison(ast::Type::Float);
                }
                if ((*status & llvm::APFloat::opOverflow) != 0)
                    error(location, "number " + quoted(number.spelling) + " is out of range for float");
                return llvm::ConstantFP::get(module_.getContext(), value);
            }

            llvm::Value* lowerCall(SourceLocation location, const ast::CallExpr& call)
            {
                std::vector<llvm::Value*> args;
                for (const ast::ExprPtr& arg : call.args)
                    args.push_back(lower(*arg));

                if (const Builtin* builtin = findBuiltin(call.callee))
                {
                    if (args.size() != builtin->arity)
                        return wrongArity(location, call, builtin->arity);
                    llvm::Function* intrinsic =
                        llvm::Intrinsic::getOrInsertDeclaration(&module_, builtin->intrinsic, {builder_.getFloatTy()});
                    return builder_.CreateCall(intrinsic, args);
                }
                const auto found = functions_.find(call.callee);
                if (found == functions_.end())
                {
                    error(location, "unknown function " + quoted(call.callee));
                    return poison(ast::Type::Float);
                }
                const ast::Function& callee = *found->second;
                if (args.size() != callee.params.size())
                    return wrongArity(location, call, static_cast<unsigned>(callee.params.size()));
                return builder_.CreateCall(module_.getFunction(call.callee), args);
            }

            llvm::Value* wrongArity(SourceLocation location, const ast::CallExpr& call, unsigned expected)
            {
                error(location, quoted(call.callee) + " takes " + std::to_string(expected) + " argument"
                                    + (expected == 1 ? "" : "s") + ", not " + std::to_string(call.args.size()));
                return poison(ast::Type::Float);
            }

            llvm::Module& module_;
            Diagnostics& diagnostics_;
            llvm::IRBuilder<> builder_;
            std::unordered_map<std::string, const ast::Function*> functions_;   // the program's, by name
            std::unordered_map<std::string, llvm::Value*> scope_;              // the current function's bindings
        };
    } // namespace

    std::unique_ptr<llvm::Module> lower(const ast::Program& program, llvm::LLVMContext& context,
                                        Diagnostics& diagnostics)
    {
        auto module = std::make_unique<llvm::Module>(diagnostics.fileName(), context);
        module->setSourceFileName(diagnostics.fileName());
        Lowering(*module, diagnostics).lower(program);
        if (diagnostics.hasErrors())
            return nullptr;
        return module;
    }

    std::vector<BuiltinInfo> builtins()
    {
        std::vector<BuiltinInfo> list;
        for (const Builtin& builtin : kBuiltins)
            list.push_back({builtin.name, builtin.arity});
        return list;
    }
} // namespace fminus::ir
