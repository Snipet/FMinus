// src/ir/CodeGen.cpp
//
// Two passes over the program. The first declares every function, from its prototypes and its definition, so calls
// may come before definitions (C needs a prototype for that; FMinus does not). A function with prototypes and no
// definition is external. The second pass lowers each body with an IRBuilder.
//
// Variables have C's semantics: every parameter and local is a stack slot (an alloca in the entry block), read with a
// load and written with a store. The pipeline's mem2reg (-O1 and up) turns the slots back into SSA values, as for
// clang's output, so `fminus -O0` looks like `clang -O0`. Scopes nest as C's do: parameters and the body's
// declarations share the outermost scope, and a block may shadow an outer name. After an error the lowering goes on
// with a poison value, so one run reports every error; the wording follows clang's.
#include "ir/CodeGen.h"

#include <llvm/ADT/APFloat.h>
#include <llvm/ADT/DepthFirstIterator.h>
#include <llvm/IR/CFG.h>
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

        // <math.h>'s float functions -> the LLVM intrinsic each lowers to, overloaded on float. sinf, cosf, expf and
        // logf become calls to libm on most targets; fabsf, sqrtf, floorf, fminf and fmaxf become instructions.
        constexpr std::array kBuiltins{
            Builtin{"fabsf", llvm::Intrinsic::fabs, 1},   Builtin{"sqrtf", llvm::Intrinsic::sqrt, 1},
            Builtin{"sinf", llvm::Intrinsic::sin, 1},     Builtin{"cosf", llvm::Intrinsic::cos, 1},
            Builtin{"expf", llvm::Intrinsic::exp, 1},     Builtin{"logf", llvm::Intrinsic::log, 1},
            Builtin{"floorf", llvm::Intrinsic::floor, 1}, Builtin{"fminf", llvm::Intrinsic::minnum, 2},
            Builtin{"fmaxf", llvm::Intrinsic::maxnum, 2}, Builtin{"tanhf", llvm::Intrinsic::tanh, 1},
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
                    if (function.isDefinition && found != functions_.end() && found->second == &function)
                        define(function);   // the accepted definition, not a rejected duplicate
                }
            }

        private:
            // A variable in scope: its stack slot, whether it may be assigned, and its type.
            struct Binding
            {
                llvm::AllocaInst* slot;
                bool isConst;
                ast::Type type;
            };

            llvm::Type* typeOf(ast::Type type)
            {
                switch (type)
                {
                case ast::Type::Float:
                    return builder_.getFloatTy();
                case ast::Type::Bool:
                    return builder_.getInt1Ty();
                }
                return builder_.getFloatTy();
            }

            // C's implicit conversions between the two types: bool to float is 0 or 1, and float to bool is `x != 0`,
            // so a NaN is true (an unordered comparison). Every value is a float or an i1.
            llvm::Value* convert(llvm::Value* value, ast::Type type)
            {
                llvm::Type* target = typeOf(type);
                if (value->getType() == target)
                    return value;
                if (type == ast::Type::Bool)
                    return builder_.CreateFCmpUNE(value, llvm::ConstantFP::get(value->getType(), 0.0), "tobool");
                return builder_.CreateUIToFP(value, target, "conv");
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
            static bool sameSignature(const ast::Function& a, const ast::Function& b)
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

            void declare(const ast::Function& function)
            {
                if (const Builtin* builtin = findBuiltin(function.name))
                {
                    // A prototype of a builtin, as <math.h> has, declares nothing new; a definition is refused.
                    if (function.isDefinition)
                        error(function.location, quoted(function.name) + " is a builtin and cannot be redefined");
                    else if (function.params.size() != builtin->arity)
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
                std::vector<llvm::Type*> params;
                for (const ast::Param& param : function.params)
                    params.push_back(typeOf(param.type));
                llvm::FunctionType* type = llvm::FunctionType::get(typeOf(function.returnType), params, false);
                llvm::Function* llvmFunction =
                    llvm::Function::Create(type, llvm::Function::ExternalLinkage, function.name, module_);
                // The C ABI passes and returns bool zero-extended: say so, or a C++ caller reads garbage bits.
                for (std::size_t i = 0; i < function.params.size(); ++i)
                {
                    if (function.params[i].type == ast::Type::Bool)
                        llvmFunction->addParamAttr(static_cast<unsigned>(i), llvm::Attribute::ZExt);
                }
                if (function.returnType == ast::Type::Bool)
                    llvmFunction->addRetAttr(llvm::Attribute::ZExt);
                functions_.emplace(function.name, &function);
            }

            // ---- bodies -----------------------------------------------------------------------------------------
            void define(const ast::Function& function)
            {
                llvm::Function* llvmFunction = module_.getFunction(function.name);
                builder_.SetInsertPoint(llvm::BasicBlock::Create(module_.getContext(), "entry", llvmFunction));
                function_ = &function;
                scopes_.assign(1, {});   // the outermost scope: the parameters and the body's own declarations

                // Each parameter gets a slot, as a local does (clang names them x.addr), so it can be assigned.
                for (std::size_t i = 0; i < function.params.size(); ++i)
                {
                    const ast::Param& param = function.params[i];
                    if (param.name.empty())
                    {
                        error(param.location, "parameter name omitted");
                        continue;
                    }
                    llvm::Argument* argument = llvmFunction->getArg(static_cast<unsigned>(i));
                    argument->setName(param.name);
                    llvm::AllocaInst* slot = allocate(typeOf(param.type), param.name + ".addr");
                    builder_.CreateStore(argument, slot);
                    if (!bind(param.name, Binding{slot, param.isConst, param.type}))
                        error(param.location, "redefinition of parameter " + quoted(param.name));
                }

                lowerStatements(function.body);

                if (!builder_.GetInsertBlock()->hasTerminator())
                {
                    if (reachable(builder_.GetInsertBlock()))
                    {
                        error(function.location,
                              "non-void function " + quoted(function.name) + " does not return a value");
                        builder_.CreateRet(poison(function.returnType));
                    }
                    else
                    {
                        builder_.CreateUnreachable();   // the end follows a return: nothing runs here
                    }
                }

                std::string problem;
                llvm::raw_string_ostream os(problem);
                if (llvm::verifyFunction(*llvmFunction, &os))
                    error(function.location, "internal error: the IR of " + quoted(function.name) + " is invalid: "
                                                 + problem);
            }

            // A slot at the top of the entry block, whatever block the builder is in: mem2reg promotes only those.
            llvm::AllocaInst* allocate(llvm::Type* type, const std::string& name)
            {
                llvm::BasicBlock& entry = builder_.GetInsertBlock()->getParent()->getEntryBlock();
                llvm::IRBuilder<> atEntry(&entry, entry.begin());
                return atEntry.CreateAlloca(type, nullptr, name);
            }

            // Whether control can reach `block` from the function's entry. (Having predecessors is not enough: code
            // after a return can branch, and its blocks are all dead.)
            static bool reachable(llvm::BasicBlock* block)
            {
                for (llvm::BasicBlock* live : llvm::depth_first(&block->getParent()->getEntryBlock()))
                {
                    if (live == block)
                        return true;
                }
                return false;
            }

            // Adds `name` to the innermost scope; false when that scope already has it.
            bool bind(const std::string& name, Binding binding)
            {
                return scopes_.back().emplace(name, binding).second;
            }

            // The innermost binding of `name`, or nullptr.
            const Binding* find(const std::string& name) const
            {
                for (auto scope = scopes_.rbegin(); scope != scopes_.rend(); ++scope)
                {
                    const auto found = scope->find(name);
                    if (found != scope->end())
                        return &found->second;
                }
                return nullptr;
            }

            void lowerStatements(const std::vector<ast::Stmt>& statements)
            {
                for (const ast::Stmt& stmt : statements)
                {
                    // C accepts statements after a return (and, once the language has them, a break or a continue).
                    // They still lower, so their errors are found, into a block nothing branches to, which the
                    // optimiser deletes. (Not getTerminator(): since LLVM 23 it assumes a finished block.)
                    if (builder_.GetInsertBlock()->hasTerminator())
                    {
                        llvm::Function* function = builder_.GetInsertBlock()->getParent();
                        builder_.SetInsertPoint(
                            llvm::BasicBlock::Create(module_.getContext(), "unreachable", function));
                    }
                    lowerStatement(stmt);
                }
            }

            void lowerStatement(const ast::Stmt& stmt)
            {
                std::visit(Overloaded{
                               [&](const ast::DeclStmt& decl) { declareVariable(stmt.location, decl); },
                               [&](const ast::ExprStmt& statement) { lower(*statement.expr); },
                               [&](const ast::ReturnStmt& ret) {
                                   builder_.CreateRet(convert(lower(*ret.value), function_->returnType));
                               },
                               [&](const ast::BlockStmt& block) {
                                   scopes_.emplace_back();
                                   lowerStatements(block.items);
                                   scopes_.pop_back();
                               },
                               [&](const ast::EmptyStmt&) {},
                           },
                           stmt.node);
            }

            // The initialiser is lowered before the name is bound, so `float x = x;` reads an outer x. Without one,
            // the variable starts at 0.
            void declareVariable(SourceLocation location, const ast::DeclStmt& decl)
            {
                llvm::Value* value = decl.init ? convert(lower(*decl.init), decl.type)
                                               : llvm::Constant::getNullValue(typeOf(decl.type));
                llvm::AllocaInst* slot = allocate(typeOf(decl.type), decl.name);
                builder_.CreateStore(value, slot);
                if (!bind(decl.name, Binding{slot, decl.isConst, decl.type}))
                    error(location, "redefinition of " + quoted(decl.name));
            }

            // ---- expressions ------------------------------------------------------------------------------------
            llvm::Value* lower(const ast::Expr& expr)
            {
                return std::visit(
                    Overloaded{
                        [&](const ast::NumberExpr& number) { return lowerNumber(expr.location, number); },
                        [&](const ast::VariableExpr& variable) -> llvm::Value* {
                            const Binding* binding = find(variable.name);
                            if (binding == nullptr)
                            {
                                error(expr.location, "use of undeclared identifier " + quoted(variable.name));
                                return poison(ast::Type::Float);
                            }
                            return builder_.CreateLoad(binding->slot->getAllocatedType(), binding->slot,
                                                       variable.name);
                        },
                        [&](const ast::BoolExpr& constant) -> llvm::Value* { return builder_.getInt1(constant.value); },
                        [&](const ast::UnaryExpr& unary) -> llvm::Value* {
                            llvm::Value* operand = lower(*unary.operand);
                            switch (unary.op)
                            {
                            case ast::UnaryOp::Negate:
                                return builder_.CreateFNeg(convert(operand, ast::Type::Float));
                            case ast::UnaryOp::Plus:
                                return convert(operand, ast::Type::Float);
                            case ast::UnaryOp::Not:
                                return builder_.CreateNot(convert(operand, ast::Type::Bool), "lnot");
                            }
                            return poison(ast::Type::Float);
                        },
                        [&](const ast::BinaryExpr& binary) -> llvm::Value* {
                            // C's usual arithmetic conversions, with float the only arithmetic type: a bool is 0 or 1.
                            llvm::Value* lhs = convert(lower(*binary.lhs), ast::Type::Float);
                            llvm::Value* rhs = convert(lower(*binary.rhs), ast::Type::Float);
                            return arithmetic(binary.op, lhs, rhs);
                        },
                        [&](const ast::LogicalExpr& logical) { return lowerLogical(logical); },
                        [&](const ast::ConditionalExpr& conditional) { return lowerConditional(conditional); },
                        [&](const ast::CallExpr& call) { return lowerCall(expr.location, call); },
                        [&](const ast::AssignExpr& assign) { return lowerAssign(expr.location, assign); },
                    },
                    expr.node);
            }

            llvm::Value* arithmetic(ast::BinaryOp op, llvm::Value* lhs, llvm::Value* rhs)
            {
                switch (op)
                {
                case ast::BinaryOp::Add:
                    return builder_.CreateFAdd(lhs, rhs);
                case ast::BinaryOp::Subtract:
                    return builder_.CreateFSub(lhs, rhs);
                case ast::BinaryOp::Multiply:
                    return builder_.CreateFMul(lhs, rhs);
                case ast::BinaryOp::Divide:
                    return builder_.CreateFDiv(lhs, rhs);
                case ast::BinaryOp::Remainder:
                    return builder_.CreateFRem(lhs, rhs);
                // Ordered comparisons: false when either side is NaN. != is unordered: true then.
                case ast::BinaryOp::Less:
                    return builder_.CreateFCmpOLT(lhs, rhs, "cmp");
                case ast::BinaryOp::LessEqual:
                    return builder_.CreateFCmpOLE(lhs, rhs, "cmp");
                case ast::BinaryOp::Greater:
                    return builder_.CreateFCmpOGT(lhs, rhs, "cmp");
                case ast::BinaryOp::GreaterEqual:
                    return builder_.CreateFCmpOGE(lhs, rhs, "cmp");
                case ast::BinaryOp::Equal:
                    return builder_.CreateFCmpOEQ(lhs, rhs, "cmp");
                case ast::BinaryOp::NotEqual:
                    return builder_.CreateFCmpUNE(lhs, rhs, "cmp");
                }
                return poison(ast::Type::Float);
            }

            // a && b and a || b, as clang lowers them: b runs only when a does not decide (land.rhs, lor.rhs), and a
            // phi joins the two ways out.
            llvm::Value* lowerLogical(const ast::LogicalExpr& logical)
            {
                const bool isAnd = logical.op == ast::LogicalOp::And;
                llvm::Value* lhs = convert(lower(*logical.lhs), ast::Type::Bool);
                llvm::BasicBlock* lhsEnd = builder_.GetInsertBlock();
                llvm::Function* function = lhsEnd->getParent();
                llvm::LLVMContext& context = module_.getContext();
                llvm::BasicBlock* rhsBlock =
                    llvm::BasicBlock::Create(context, isAnd ? "land.rhs" : "lor.rhs", function);
                llvm::BasicBlock* endBlock =
                    llvm::BasicBlock::Create(context, isAnd ? "land.end" : "lor.end", function);
                if (isAnd)
                    builder_.CreateCondBr(lhs, rhsBlock, endBlock);
                else
                    builder_.CreateCondBr(lhs, endBlock, rhsBlock);

                builder_.SetInsertPoint(rhsBlock);
                llvm::Value* rhs = convert(lower(*logical.rhs), ast::Type::Bool);
                llvm::BasicBlock* rhsEnd = builder_.GetInsertBlock();   // lowering b may have added blocks
                builder_.CreateBr(endBlock);

                builder_.SetInsertPoint(endBlock);
                llvm::PHINode* phi = builder_.CreatePHI(builder_.getInt1Ty(), 2, isAnd ? "land" : "lor");
                phi->addIncoming(builder_.getInt1(!isAnd), lhsEnd);   // && is false when a is; || is true when a is
                phi->addIncoming(rhs, rhsEnd);
                return phi;
            }

            // c ? a : b, as clang lowers it: only the branch taken runs, so it takes blocks and a phi (a select would
            // run both). SimplifyCFG turns a small one without side effects into a select at -O1 and above.
            llvm::Value* lowerConditional(const ast::ConditionalExpr& conditional)
            {
                llvm::Value* condition = convert(lower(*conditional.condition), ast::Type::Bool);
                llvm::Function* function = builder_.GetInsertBlock()->getParent();
                llvm::LLVMContext& context = module_.getContext();
                llvm::BasicBlock* trueBlock = llvm::BasicBlock::Create(context, "cond.true", function);
                llvm::BasicBlock* falseBlock = llvm::BasicBlock::Create(context, "cond.false", function);
                llvm::BasicBlock* endBlock = llvm::BasicBlock::Create(context, "cond.end", function);
                builder_.CreateCondBr(condition, trueBlock, falseBlock);

                // Each branch may end in another block than it started in (a nested ?:): the phi needs the last. Both
                // are lowered before their type is known, then converted to it at their ends.
                builder_.SetInsertPoint(trueBlock);
                llvm::Value* thenValue = lower(*conditional.thenValue);
                llvm::BasicBlock* thenEnd = builder_.GetInsertBlock();
                builder_.SetInsertPoint(falseBlock);
                llvm::Value* elseValue = lower(*conditional.elseValue);
                llvm::BasicBlock* elseEnd = builder_.GetInsertBlock();

                const bool bothBool = thenValue->getType()->isIntegerTy(1) && elseValue->getType()->isIntegerTy(1);
                const ast::Type type = bothBool ? ast::Type::Bool : ast::Type::Float;
                builder_.SetInsertPoint(thenEnd);
                thenValue = convert(thenValue, type);
                builder_.CreateBr(endBlock);
                builder_.SetInsertPoint(elseEnd);
                elseValue = convert(elseValue, type);
                builder_.CreateBr(endBlock);

                builder_.SetInsertPoint(endBlock);
                llvm::PHINode* phi = builder_.CreatePHI(typeOf(type), 2, "cond");
                phi->addIncoming(thenValue, thenEnd);
                phi->addIncoming(elseValue, elseEnd);
                return phi;
            }

            // The constant rounded once, from its spelling straight to float (no detour through double). APFloat
            // reads decimal and hex floats alike; the f suffix is C's and goes.
            llvm::Value* lowerNumber(SourceLocation location, const ast::NumberExpr& number)
            {
                std::string spelling = number.spelling;
                if (!spelling.empty() && (spelling.back() == 'f' || spelling.back() == 'F'))
                    spelling.pop_back();
                llvm::APFloat value(llvm::APFloat::IEEEsingle());
                auto status = value.convertFromString(spelling, llvm::APFloat::rmNearestTiesToEven);
                if (!status)
                {
                    llvm::consumeError(status.takeError());
                    error(location, "invalid floating-point constant " + quoted(number.spelling));
                    return poison(ast::Type::Float);
                }
                if ((*status & llvm::APFloat::opOverflow) != 0)
                    error(location, "magnitude of floating-point constant too large for type 'float'");
                return llvm::ConstantFP::get(module_.getContext(), value);
            }

            // `target op value`: the target must be a variable that is not const. The value of the expression is
            // the one stored.
            llvm::Value* lowerAssign(SourceLocation location, const ast::AssignExpr& assign)
            {
                llvm::Value* value = lower(*assign.value);
                const auto* variable = std::get_if<ast::VariableExpr>(&assign.target->node);
                if (variable == nullptr)
                {
                    error(location, "expression is not assignable");
                    return poison(ast::Type::Float);
                }
                const Binding* binding = find(variable->name);
                if (binding == nullptr)
                {
                    error(assign.target->location, "use of undeclared identifier " + quoted(variable->name));
                    return poison(ast::Type::Float);
                }
                if (binding->isConst)
                {
                    error(location, "cannot assign to variable " + quoted(variable->name)
                                        + " with const-qualified type "
                                        + quoted(std::string("const ") + ast::typeName(binding->type)));
                    return poison(ast::Type::Float);
                }
                if (assign.op != ast::AssignOp::Assign)
                {
                    llvm::Value* current =
                        builder_.CreateLoad(binding->slot->getAllocatedType(), binding->slot, variable->name);
                    value = arithmetic(compoundOperator(assign.op), convert(current, ast::Type::Float),
                                       convert(value, ast::Type::Float));
                }
                value = convert(value, binding->type);
                builder_.CreateStore(value, binding->slot);
                return value;
            }

            // The operator of a compound assignment: += is +, ...
            static ast::BinaryOp compoundOperator(ast::AssignOp op)
            {
                switch (op)
                {
                case ast::AssignOp::Assign:
                case ast::AssignOp::Add:
                    return ast::BinaryOp::Add;
                case ast::AssignOp::Subtract:
                    return ast::BinaryOp::Subtract;
                case ast::AssignOp::Multiply:
                    return ast::BinaryOp::Multiply;
                case ast::AssignOp::Divide:
                    return ast::BinaryOp::Divide;
                case ast::AssignOp::Remainder:
                    return ast::BinaryOp::Remainder;
                }
                return ast::BinaryOp::Add;
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
                    for (llvm::Value*& arg : args)
                        arg = convert(arg, ast::Type::Float);
                    llvm::Function* intrinsic =
                        llvm::Intrinsic::getOrInsertDeclaration(&module_, builtin->intrinsic, {builder_.getFloatTy()});
                    return builder_.CreateCall(intrinsic, args);
                }
                const auto found = functions_.find(call.callee);
                if (found == functions_.end())
                {
                    error(location, "call to undeclared function " + quoted(call.callee));
                    return poison(ast::Type::Float);
                }
                const ast::Function& callee = *found->second;
                if (args.size() != callee.params.size())
                    return wrongArity(location, call, static_cast<unsigned>(callee.params.size()));
                for (std::size_t i = 0; i < args.size(); ++i)
                    args[i] = convert(args[i], callee.params[i].type);
                return builder_.CreateCall(module_.getFunction(call.callee), args);
            }

            llvm::Value* wrongArity(SourceLocation location, const ast::CallExpr& call, unsigned expected)
            {
                error(location, std::string(call.args.size() > expected ? "too many" : "too few")
                                    + " arguments to function call, expected " + std::to_string(expected) + ", have "
                                    + std::to_string(call.args.size()));
                return poison(ast::Type::Float);
            }

            llvm::Module& module_;
            Diagnostics& diagnostics_;
            llvm::IRBuilder<> builder_;
            std::unordered_map<std::string, const ast::Function*> functions_;   // the program's, by name
            std::vector<std::unordered_map<std::string, Binding>> scopes_;     // the current function's, inner last
            const ast::Function* function_ = nullptr;                          // the function being lowered
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
