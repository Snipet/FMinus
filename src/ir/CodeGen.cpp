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
#include <cctype>
#include <optional>
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

        // poly_lanes(): how many lanes a poly float has, as a float. poly_index(): a poly float whose lanes hold their
        // own indices, 0, 1, 2, ...: what tells the lanes apart.
        bool isPolyQuery(std::string_view name)
        {
            return name == "poly_lanes" || name == "poly_index";
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

        std::string quoted(std::string_view name)
        {
            return "'" + std::string(name) + "'";
        }

        class Lowering
        {
        public:
            Lowering(llvm::Module& module, Diagnostics& diagnostics, const Options& options)
                : module_(module)
                , diagnostics_(diagnostics)
                , builder_(module.getContext())
                , polyLanes_(options.polyLanes)
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
            // A lowered expression: its IR and its FMinus type. The rules read the FMinus type, never the IR's, since
            // two FMinus types may share an LLVM type: one whose width the target decides would be <4 x float> on
            // NEON, as float4 is.
            struct Value
            {
                llvm::Value* ir = nullptr;
                ast::Type type = ast::Type::Float;
            };

            // A variable in scope: its stack slot, whether it may be assigned, and its type.
            struct Binding
            {
                llvm::AllocaInst* slot;
                bool isConst;
                ast::Type type;
            };

            // A type's lanes: a poly type has the target's.
            unsigned lanes(ast::Type type) const
            {
                return ast::isPoly(type) ? polyLanes_ : ast::lanes(type);
            }

            llvm::Type* typeOf(ast::Type type)
            {
                switch (type)
                {
                case ast::Type::Float:
                    return builder_.getFloatTy();
                case ast::Type::Bool:
                    return builder_.getInt1Ty();
                case ast::Type::Float2:
                case ast::Type::Float4:
                case ast::Type::Float8:
                case ast::Type::PolyFloat:
                    return llvm::FixedVectorType::get(builder_.getFloatTy(), lanes(type));
                case ast::Type::Bool2:
                case ast::Type::Bool4:
                case ast::Type::Bool8:
                case ast::Type::PolyBool:
                    return llvm::FixedVectorType::get(builder_.getInt1Ty(), lanes(type));
                }
                return builder_.getFloatTy();
            }

            // Where a value meets the type it must have, for the message when it cannot (clang's wording for each).
            enum class Use
            {
                Initializing,
                Assigning,
                Returning,
                Passing,
                Condition,
                Logical,
            };

            // C's implicit conversions, and OpenCL C's for vectors. A bool is 0 or 1 as a float, and a float is
            // `x != 0` as a bool (so a NaN is true). A scalar becomes a vector by being broadcast to every lane (a
            // poly bool takes a bool), and a lane mask is 0 or 1 per lane as the float vector of its size. Nothing
            // else converts from a vector: that is an error, reported for `use`.
            llvm::Value* convert(Value value, ast::Type type, SourceLocation location, Use use)
            {
                if (value.type == type)
                    return value.ir;
                if (!ast::isVector(value.type))
                {
                    if (ast::isVector(type))
                    {
                        const ast::Type scalarType = ast::isMask(type) ? ast::Type::Bool : ast::Type::Float;
                        llvm::Value* scalar = convert(value, scalarType, location, use);
                        return builder_.CreateVectorSplat(lanes(type), scalar, "splat");
                    }
                    if (type == ast::Type::Bool)
                        return builder_.CreateFCmpUNE(value.ir, llvm::ConstantFP::get(value.ir->getType(), 0.0),
                                                      "tobool");
                    if (type == ast::Type::Float)
                        return builder_.CreateUIToFP(value.ir, typeOf(type), "conv");
                }
                else if (ast::isMask(value.type) && type == ast::arithmeticOf(value.type))
                {
                    return builder_.CreateUIToFP(value.ir, typeOf(type), "conv");
                }
                incompatible(location, use, value.type, type);
                return poison(type).ir;
            }

            void incompatible(SourceLocation location, Use use, ast::Type from, ast::Type to)
            {
                const std::string source = quoted(ast::typeName(from));
                const std::string target = quoted(ast::typeName(to));
                switch (use)
                {
                case Use::Initializing:
                    error(location, "initializing " + target + " with an expression of incompatible type " + source);
                    return;
                case Use::Assigning:
                    error(location, "assigning to " + target + " from incompatible type " + source);
                    return;
                case Use::Returning:
                    error(location, "returning " + source + " from a function with incompatible result type " + target);
                    return;
                case Use::Passing:
                    error(location, "passing " + source + " to parameter of incompatible type " + target);
                    return;
                case Use::Condition:
                    error(location, "statement requires expression of scalar type (" + source + " invalid)");
                    return;
                case Use::Logical:
                    error(location, "a logical operator needs scalar operands, not " + source);
                    return;
                }
            }

            // The type two operands meet in, as OpenCL C's usual arithmetic conversions decide it: float, or the
            // vector type when either one is a vector (the other is broadcast to it); a bool and a lane mask take part
            // as floats. Vectors of two sizes do not meet: clang's error, and false.
            bool commonType(SourceLocation location, ast::Type a, ast::Type b, ast::Type& type)
            {
                const ast::Type ta = ast::arithmeticOf(a);
                const ast::Type tb = ast::arithmeticOf(b);
                if (ast::isVector(ta) && ast::isVector(tb) && ast::isPoly(ta) != ast::isPoly(tb))
                {
                    polyMismatch(location, a, b);
                    return false;
                }
                if (ast::isVector(ta) && ast::isVector(tb) && ta != tb)
                {
                    error(location, "cannot convert between vector values of different size ("
                                        + quoted(ast::typeName(a)) + " and " + quoted(ast::typeName(b)) + ")");
                    return false;
                }
                type = ast::isVector(ta) ? ta : tb;
                return true;
            }

            // A poly type never meets a fixed-size vector, even where the two have as many lanes: on another target
            // they would not, and a program means the same on every target.
            void polyMismatch(SourceLocation location, ast::Type a, ast::Type b)
            {
                error(location, "cannot convert between " + quoted(ast::typeName(a)) + " and "
                                    + quoted(ast::typeName(b))
                                    + ": a poly type has as many lanes as the target's vectors");
            }

            Value poison(ast::Type type)
            {
                return {llvm::PoisonValue::get(typeOf(type)), type};
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
                if (isReduction(function.name) || isPolyQuery(function.name))
                {
                    error(function.location, quoted(function.name) + " is a builtin and cannot be redefined");
                    return;
                }
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
                        builder_.CreateRet(poison(function.returnType).ir);
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
                                   builder_.CreateRet(convert(lower(*ret.value), function_->returnType, stmt.location,
                                                              Use::Returning));
                               },
                               [&](const ast::BlockStmt& block) {
                                   scopes_.emplace_back();
                                   lowerStatements(block.items);
                                   scopes_.pop_back();
                               },
                               [&](const ast::EmptyStmt&) {},
                               [&](const ast::IfStmt& branch) { lowerIf(branch); },
                               [&](const ast::WhileStmt& loop) { lowerWhile(loop); },
                               [&](const ast::ForStmt& loop) { lowerFor(loop); },
                               [&](const ast::BreakStmt&) {
                                   if (loops_.empty())
                                       error(stmt.location, "'break' statement not in loop or switch statement");
                                   else
                                       builder_.CreateBr(loops_.back().breakTarget);
                               },
                               [&](const ast::ContinueStmt&) {
                                   if (loops_.empty())
                                       error(stmt.location, "'continue' statement not in loop statement");
                                   else
                                       builder_.CreateBr(loops_.back().continueTarget);
                               },
                           },
                           stmt.node);
            }

            // A sub-statement (a branch, a loop body) has a scope of its own, as in C.
            void lowerScoped(const ast::Stmt& stmt)
            {
                scopes_.emplace_back();
                lowerStatement(stmt);
                scopes_.pop_back();
            }

            llvm::BasicBlock* newBlock(const char* name)
            {
                return llvm::BasicBlock::Create(module_.getContext(), name, builder_.GetInsertBlock()->getParent());
            }

            // Branches to `target` unless the block already ends (a return, a break, a continue in it).
            void fallThrough(llvm::BasicBlock* target)
            {
                if (!builder_.GetInsertBlock()->hasTerminator())
                    builder_.CreateBr(target);
            }

            // if.then and if.else join at if.end. A branch that returns never gets there; when neither does, if.end
            // is unreachable, and so is what follows.
            void lowerIf(const ast::IfStmt& branch)
            {
                llvm::Value* condition =
                    convert(lower(*branch.condition), ast::Type::Bool, branch.condition->location, Use::Condition);
                llvm::BasicBlock* thenBlock = newBlock("if.then");
                llvm::BasicBlock* elseBlock = branch.elseBranch ? newBlock("if.else") : nullptr;
                llvm::BasicBlock* endBlock = newBlock("if.end");
                builder_.CreateCondBr(condition, thenBlock, elseBlock != nullptr ? elseBlock : endBlock);

                builder_.SetInsertPoint(thenBlock);
                lowerScoped(*branch.thenBranch);
                fallThrough(endBlock);
                if (elseBlock != nullptr)
                {
                    builder_.SetInsertPoint(elseBlock);
                    lowerScoped(*branch.elseBranch);
                    fallThrough(endBlock);
                }
                builder_.SetInsertPoint(endBlock);
            }

            // while.cond tests and branches to while.body or while.end; the body goes back to while.cond. break goes
            // to while.end, continue to while.cond.
            void lowerWhile(const ast::WhileStmt& loop)
            {
                llvm::BasicBlock* condBlock = newBlock("while.cond");
                llvm::BasicBlock* bodyBlock = newBlock("while.body");
                llvm::BasicBlock* endBlock = newBlock("while.end");
                builder_.CreateBr(condBlock);

                builder_.SetInsertPoint(condBlock);
                builder_.CreateCondBr(
                    convert(lower(*loop.condition), ast::Type::Bool, loop.condition->location, Use::Condition),
                    bodyBlock, endBlock);

                builder_.SetInsertPoint(bodyBlock);
                loops_.push_back({endBlock, condBlock});
                lowerScoped(*loop.body);
                loops_.pop_back();
                fallThrough(condBlock);

                builder_.SetInsertPoint(endBlock);
            }

            // for (init; cond; step) body: init, then for.cond -> for.body -> for.inc (the step) -> for.cond, until
            // for.end. continue goes to for.inc, so the step still runs. The init's declarations are the loop's.
            void lowerFor(const ast::ForStmt& loop)
            {
                scopes_.emplace_back();
                lowerStatements(loop.init);
                llvm::BasicBlock* condBlock = newBlock("for.cond");
                llvm::BasicBlock* bodyBlock = newBlock("for.body");
                llvm::BasicBlock* incBlock = newBlock("for.inc");
                llvm::BasicBlock* endBlock = newBlock("for.end");
                builder_.CreateBr(condBlock);

                builder_.SetInsertPoint(condBlock);
                if (loop.condition)
                    builder_.CreateCondBr(
                        convert(lower(*loop.condition), ast::Type::Bool, loop.condition->location, Use::Condition),
                        bodyBlock, endBlock);
                else
                    builder_.CreateBr(bodyBlock);   // no condition: until a break or a return

                builder_.SetInsertPoint(bodyBlock);
                loops_.push_back({endBlock, incBlock});
                lowerScoped(*loop.body);
                loops_.pop_back();
                fallThrough(incBlock);

                builder_.SetInsertPoint(incBlock);
                if (loop.step)
                    lower(*loop.step);
                builder_.CreateBr(condBlock);

                builder_.SetInsertPoint(endBlock);
                scopes_.pop_back();
            }

            // The initialiser is lowered before the name is bound, so `float x = x;` reads an outer x. Without one,
            // the variable starts at 0.
            void declareVariable(SourceLocation location, const ast::DeclStmt& decl)
            {
                llvm::Value* value = decl.init ? convert(lower(*decl.init), decl.type, location, Use::Initializing)
                                               : llvm::Constant::getNullValue(typeOf(decl.type));
                llvm::AllocaInst* slot = allocate(typeOf(decl.type), decl.name);
                builder_.CreateStore(value, slot);
                if (!bind(decl.name, Binding{slot, decl.isConst, decl.type}))
                    error(location, "redefinition of " + quoted(decl.name));
            }

            // ---- expressions ------------------------------------------------------------------------------------
            Value lower(const ast::Expr& expr)
            {
                return std::visit(
                    Overloaded{
                        [&](const ast::NumberExpr& number) {
                            return Value{lowerNumber(expr.location, number), ast::Type::Float};
                        },
                        [&](const ast::VariableExpr& variable) {
                            const Binding* binding = find(variable.name);
                            if (binding == nullptr)
                            {
                                error(expr.location, "use of undeclared identifier " + quoted(variable.name));
                                return poison(ast::Type::Float);
                            }
                            return Value{builder_.CreateLoad(binding->slot->getAllocatedType(), binding->slot,
                                                             variable.name),
                                         binding->type};
                        },
                        [&](const ast::BoolExpr& constant) {
                            return Value{builder_.getInt1(constant.value), ast::Type::Bool};
                        },
                        [&](const ast::UnaryExpr& unary) {
                            const Value operand = lower(*unary.operand);
                            const ast::Type type = ast::arithmeticOf(operand.type);   // a float or a float vector
                            switch (unary.op)
                            {
                            case ast::UnaryOp::Negate:
                            {
                                llvm::Value* value = convert(operand, type, expr.location, Use::Initializing);
                                return Value{builder_.CreateFNeg(value), type};
                            }
                            case ast::UnaryOp::Plus:
                                return Value{convert(operand, type, expr.location, Use::Initializing), type};
                            case ast::UnaryOp::Not:
                            {
                                llvm::Value* test = convert(operand, ast::Type::Bool, expr.location, Use::Logical);
                                return Value{builder_.CreateNot(test, "lnot"), ast::Type::Bool};
                            }
                            }
                            return poison(ast::Type::Float);
                        },
                        [&](const ast::BinaryExpr& binary) {
                            // C's usual arithmetic conversions, and OpenCL C's: a bool is 0 or 1, and a scalar
                            // meeting a vector is broadcast to it. Vector operators work lane by lane; a comparison of
                            // vectors is a lane mask.
                            const Value lhs = lower(*binary.lhs);
                            const Value rhs = lower(*binary.rhs);
                            ast::Type type = ast::Type::Float;
                            if (!commonType(expr.location, lhs.type, rhs.type, type))
                                return poison(ast::Type::Float);
                            llvm::Value* result =
                                arithmetic(binary.op, convert(lhs, type, expr.location, Use::Initializing),
                                           convert(rhs, type, expr.location, Use::Initializing));
                            return Value{result, isComparison(binary.op) ? ast::maskOf(type) : type};
                        },
                        [&](const ast::LogicalExpr& logical) {
                            return Value{lowerLogical(expr.location, logical), ast::Type::Bool};
                        },
                        [&](const ast::ConditionalExpr& conditional) {
                            return lowerConditional(expr.location, conditional);
                        },
                        [&](const ast::ConstructExpr& construct) { return lowerConstruct(expr.location, construct); },
                        [&](const ast::SwizzleExpr& swizzle) { return lowerSwizzle(expr.location, swizzle); },
                        [&](const ast::CallExpr& call) { return lowerCall(expr.location, call); },
                        [&](const ast::AssignExpr& assign) { return lowerAssign(expr.location, assign); },
                    },
                    expr.node);
            }

            static bool isComparison(ast::BinaryOp op)
            {
                return op == ast::BinaryOp::Less || op == ast::BinaryOp::LessEqual || op == ast::BinaryOp::Greater
                       || op == ast::BinaryOp::GreaterEqual || op == ast::BinaryOp::Equal
                       || op == ast::BinaryOp::NotEqual;
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
                return poison(ast::Type::Float).ir;
            }

            // a && b and a || b, as clang lowers them: b runs only when a does not decide (land.rhs, lor.rhs), and a
            // phi joins the two ways out.
            llvm::Value* lowerLogical(SourceLocation location, const ast::LogicalExpr& logical)
            {
                const bool isAnd = logical.op == ast::LogicalOp::And;
                llvm::Value* lhs = convert(lower(*logical.lhs), ast::Type::Bool, location, Use::Logical);
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
                llvm::Value* rhs = convert(lower(*logical.rhs), ast::Type::Bool, location, Use::Logical);
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
            Value lowerConditional(SourceLocation location, const ast::ConditionalExpr& conditional)
            {
                const Value test = lower(*conditional.condition);
                if (ast::isVector(test.type))
                    return lowerSelect(location, test, conditional);
                llvm::Value* condition = convert(test, ast::Type::Bool, location, Use::Condition);
                llvm::Function* function = builder_.GetInsertBlock()->getParent();
                llvm::LLVMContext& context = module_.getContext();
                llvm::BasicBlock* trueBlock = llvm::BasicBlock::Create(context, "cond.true", function);
                llvm::BasicBlock* falseBlock = llvm::BasicBlock::Create(context, "cond.false", function);
                llvm::BasicBlock* endBlock = llvm::BasicBlock::Create(context, "cond.end", function);
                builder_.CreateCondBr(condition, trueBlock, falseBlock);

                // Each branch may end in another block than it started in (a nested ?:): the phi needs the last. Both
                // are lowered before their type is known, then converted to it at their ends.
                builder_.SetInsertPoint(trueBlock);
                const Value thenValue = lower(*conditional.thenValue);
                llvm::BasicBlock* thenEnd = builder_.GetInsertBlock();
                builder_.SetInsertPoint(falseBlock);
                const Value elseValue = lower(*conditional.elseValue);
                llvm::BasicBlock* elseEnd = builder_.GetInsertBlock();

                // Two of one type give that type (two bools give a bool); otherwise the usual arithmetic conversions
                // decide (a scalar and a vector give the vector).
                ast::Type type = thenValue.type;
                const bool meet = thenValue.type == elseValue.type
                                  || commonType(location, thenValue.type, elseValue.type, type);
                if (!meet)
                    type = ast::Type::Float;
                builder_.SetInsertPoint(thenEnd);
                llvm::Value* thenIR = meet ? convert(thenValue, type, location, Use::Initializing) : poison(type).ir;
                builder_.CreateBr(endBlock);
                builder_.SetInsertPoint(elseEnd);
                llvm::Value* elseIR = meet ? convert(elseValue, type, location, Use::Initializing) : poison(type).ir;
                builder_.CreateBr(endBlock);

                builder_.SetInsertPoint(endBlock);
                llvm::PHINode* phi = builder_.CreatePHI(typeOf(type), 2, "cond");
                phi->addIncoming(thenIR, thenEnd);
                phi->addIncoming(elseIR, elseEnd);
                return {phi, type};
            }

            // c ? a : b with a vector c chooses lane by lane, as in OpenCL C: there are no branches, so both sides run,
            // and a select picks each lane from one or the other. A float vector condition is `!= 0` per lane, as a
            // scalar one is. The result is a float vector of the condition's size; scalar branches are broadcast.
            Value lowerSelect(SourceLocation location, Value test, const ast::ConditionalExpr& conditional)
            {
                const ast::Type mask = ast::maskOf(ast::arithmeticOf(test.type));
                llvm::Value* condition = test.ir;
                if (!ast::isMask(test.type))
                    condition = builder_.CreateFCmpUNE(condition, llvm::Constant::getNullValue(typeOf(test.type)),
                                                       "tobool");
                const Value thenValue = lower(*conditional.thenValue);
                const Value elseValue = lower(*conditional.elseValue);
                const ast::Type type = ast::arithmeticOf(mask);
                ast::Type branches = type;
                if (!commonType(location, thenValue.type, elseValue.type, branches))
                    return poison(type);
                if (ast::isVector(branches) && ast::isPoly(branches) != ast::isPoly(type))
                {
                    polyMismatch(location, mask, branches);
                    return poison(type);
                }
                if (ast::isVector(branches) && branches != type)
                {
                    error(location, "vector condition type " + quoted(ast::typeName(mask)) + " and result type "
                                        + quoted(ast::typeName(branches)) + " do not have the same number of elements");
                    return poison(type);
                }
                return {builder_.CreateSelect(condition, convert(thenValue, type, location, Use::Initializing),
                                              convert(elseValue, type, location, Use::Initializing), "select"),
                        type};
            }

            // (float4)(a, b, c, d), OpenCL C's vector literal: one scalar is broadcast; otherwise the arguments' lanes,
            // in order, must fill the vector exactly. (poly float)(x) is a broadcast only: a program does not know how
            // many lanes it would have to fill, and a poly argument cannot fill a fixed number.
            Value lowerConstruct(SourceLocation location, const ast::ConstructExpr& construct)
            {
                const unsigned count = lanes(construct.type);
                std::vector<Value> args;
                for (const ast::ExprPtr& arg : construct.args)
                    args.push_back(lower(*arg));
                if (args.size() == 1 && !ast::isVector(args[0].type))
                    return {convert(args[0], construct.type, location, Use::Initializing), construct.type};
                if (ast::isPoly(construct.type))
                {
                    error(location, "a poly float literal takes one scalar, which it broadcasts");
                    return poison(construct.type);
                }

                std::vector<llvm::Value*> elements;
                for (const Value& arg : args)
                {
                    const ast::Type type = ast::arithmeticOf(arg.type);
                    if (ast::isPoly(type))
                    {
                        polyMismatch(location, arg.type, construct.type);
                        return poison(construct.type);
                    }
                    llvm::Value* floats = convert(arg, type, location, Use::Initializing);
                    if (!ast::isVector(type))
                        elements.push_back(floats);
                    for (unsigned i = 0; ast::isVector(type) && i < lanes(type); ++i)
                        elements.push_back(builder_.CreateExtractElement(floats, i));
                }
                if (elements.size() != count)
                {
                    if (elements.size() < count)
                        error(location, "too few elements in vector initialization (expected " + std::to_string(count)
                                            + " elements, have " + std::to_string(elements.size()) + ")");
                    else
                        error(location, "excess elements in vector initializer");
                    return poison(construct.type);
                }
                llvm::Value* vector = llvm::PoisonValue::get(typeOf(construct.type));
                for (unsigned i = 0; i < count; ++i)
                    vector = builder_.CreateInsertElement(vector, elements[i], i);
                return {vector, construct.type};
            }

            // The lanes a component name names, as in OpenCL C: x, y, z and w for 0 to 3, or s followed by hex digits
            // (s0 ... s7) for any lane; the two do not mix. Reports clang's errors, and returns nothing, for a name
            // that is neither, or that names a lane `vector` does not have.
            std::optional<std::vector<int>> components(SourceLocation location, const std::string& name,
                                                       ast::Type vector)
            {
                std::vector<int> indices;
                const bool numbered = name.size() > 1 && (name[0] == 's' || name[0] == 'S');
                for (std::size_t i = numbered ? 1 : 0; i < name.size(); ++i)
                {
                    const auto c = static_cast<unsigned char>(name[i]);
                    int lane = -1;
                    if (numbered && std::isxdigit(c) != 0)
                        lane = std::isdigit(c) != 0 ? c - '0' : std::tolower(c) - 'a' + 10;
                    else if (!numbered && std::string_view("xyzw").find(static_cast<char>(c)) != std::string_view::npos)
                        lane = static_cast<int>(std::string_view("xyzw").find(static_cast<char>(c)));
                    if (lane < 0)
                    {
                        error(location,
                              "illegal vector component name " + quoted(std::string(1, static_cast<char>(c))));
                        return std::nullopt;
                    }
                    if (lane >= static_cast<int>(lanes(vector)))
                    {
                        error(location, "vector component access exceeds type " + quoted(ast::typeName(vector)));
                        return std::nullopt;
                    }
                    indices.push_back(lane);
                }
                return indices;
            }

            // The type of `count` components: one is a float, 2, 4 or 8 a float vector; anything else is an error.
            std::optional<ast::Type> componentsType(SourceLocation location, std::size_t count)
            {
                if (count == 1)
                    return ast::Type::Float;
                const auto type = ast::vectorOf(static_cast<unsigned>(count));
                if (!type)
                    error(location,
                          std::to_string(count) + " components have no type: FMinus has float2, float4 and float8");
                return type;
            }

            // Whether `type` has components: a float vector does; a scalar, a lane mask or a poly float (whose lanes
            // a program cannot name, not knowing how many there are) does not.
            bool hasComponents(SourceLocation location, ast::Type type)
            {
                if (type == ast::Type::PolyFloat)
                {
                    error(location, "a poly float has no components: its lanes are the target's");
                    return false;
                }
                if (ast::isVector(type) && !ast::isMask(type))
                    return true;
                error(location,
                      "member reference base type " + quoted(ast::typeName(type)) + " is not a structure or union");
                return false;
            }

            // v.x is one lane (extractelement); v.wzyx, v.xxyy, v.s0123 are several, in any order, repeated or not (a
            // shufflevector).
            Value lowerSwizzle(SourceLocation location, const ast::SwizzleExpr& swizzle)
            {
                const Value base = lower(*swizzle.base);
                if (!hasComponents(location, base.type))
                    return poison(ast::Type::Float);
                const auto lanes = components(location, swizzle.components, base.type);
                if (!lanes)
                    return poison(ast::Type::Float);
                const auto type = componentsType(location, lanes->size());
                if (!type)
                    return poison(ast::Type::Float);
                if (lanes->size() == 1)
                    return {builder_.CreateExtractElement(base.ir, static_cast<std::uint64_t>((*lanes)[0]),
                                                          swizzle.components),
                            *type};
                return {builder_.CreateShuffleVector(base.ir, *lanes, swizzle.components), *type};
            }

            // v.x = e, v.zw += e: components of a vector variable, assigned. They must be distinct, and the value has
            // their type (a float for one, a vector of their number otherwise; a scalar is broadcast to it).
            Value assignComponents(SourceLocation location, const ast::AssignExpr& assign,
                                   const ast::SwizzleExpr& swizzle, Value value)
            {
                const Binding* binding = assignable(location, *swizzle.base);
                if (binding == nullptr || !hasComponents(location, binding->type))
                    return poison(ast::Type::Float);
                const auto lanes = components(location, swizzle.components, binding->type);
                if (!lanes)
                    return poison(ast::Type::Float);
                for (std::size_t i = 0; i < lanes->size(); ++i)
                {
                    for (std::size_t j = i + 1; j < lanes->size(); ++j)
                    {
                        if ((*lanes)[i] == (*lanes)[j])
                        {
                            error(location, "vector is not assignable (contains duplicate components)");
                            return poison(ast::Type::Float);
                        }
                    }
                }
                const auto type = componentsType(location, lanes->size());
                if (!type)
                    return poison(ast::Type::Float);

                llvm::AllocaInst* slot = binding->slot;
                llvm::Value* vector = builder_.CreateLoad(slot->getAllocatedType(), slot, slot->getName());
                if (assign.op != ast::AssignOp::Assign)
                {
                    llvm::Value* lanesNow =
                        lanes->size() == 1
                            ? builder_.CreateExtractElement(vector, static_cast<std::uint64_t>((*lanes)[0]))
                            : builder_.CreateShuffleVector(vector, *lanes);
                    const Value current{lanesNow, *type};
                    value = compound(location, assign.op, current, value);
                    if (value.ir == nullptr)
                        return poison(*type);
                }
                llvm::Value* stored = convert(value, *type, location, Use::Assigning);
                for (std::size_t i = 0; i < lanes->size(); ++i)
                {
                    llvm::Value* element = lanes->size() == 1 ? stored : builder_.CreateExtractElement(stored, i);
                    vector = builder_.CreateInsertElement(vector, element, static_cast<std::uint64_t>((*lanes)[i]));
                }
                builder_.CreateStore(vector, slot);
                return {stored, *type};
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
                    return poison(ast::Type::Float).ir;
                }
                if ((*status & llvm::APFloat::opOverflow) != 0)
                    error(location, "magnitude of floating-point constant too large for type 'float'");
                return llvm::ConstantFP::get(module_.getContext(), value);
            }

            // `target op value`: the target must be a variable that is not const. The value of the expression is
            // the one stored.
            Value lowerAssign(SourceLocation location, const ast::AssignExpr& assign)
            {
                Value value = lower(*assign.value);
                if (const auto* swizzle = std::get_if<ast::SwizzleExpr>(&assign.target->node))
                    return assignComponents(location, assign, *swizzle, value);
                const Binding* binding = assignable(location, *assign.target);
                if (binding == nullptr)
                    return poison(ast::Type::Float);
                llvm::AllocaInst* slot = binding->slot;
                if (assign.op != ast::AssignOp::Assign)
                {
                    const Value current{builder_.CreateLoad(slot->getAllocatedType(), slot, slot->getName()),
                                        binding->type};
                    value = compound(location, assign.op, current, value);
                    if (value.ir == nullptr)
                        return poison(binding->type);
                }
                llvm::Value* stored = convert(value, binding->type, location, Use::Assigning);
                builder_.CreateStore(stored, slot);
                return {stored, binding->type};
            }

            // The variable an assignment stores to: one that is declared and not const. Otherwise reports clang's
            // error and returns null.
            const Binding* assignable(SourceLocation location, const ast::Expr& target)
            {
                const auto* variable = std::get_if<ast::VariableExpr>(&target.node);
                if (variable == nullptr)
                {
                    error(location, "expression is not assignable");
                    return nullptr;
                }
                const Binding* binding = find(variable->name);
                if (binding == nullptr)
                {
                    error(target.location, "use of undeclared identifier " + quoted(variable->name));
                    return nullptr;
                }
                if (binding->isConst)
                {
                    error(location, "cannot assign to variable " + quoted(variable->name)
                                        + " with const-qualified type "
                                        + quoted(std::string("const ") + ast::typeName(binding->type)));
                    return nullptr;
                }
                return binding;
            }

            // What `current op= value` stores: the operator, in the type both convert to (a scalar broadcasts to a
            // vector). A null IR after an error.
            Value compound(SourceLocation location, ast::AssignOp op, Value current, Value value)
            {
                ast::Type common = ast::Type::Float;
                if (!commonType(location, current.type, value.type, common))
                    return {nullptr, common};
                return {arithmetic(compoundOperator(op), convert(current, common, location, Use::Initializing),
                                   convert(value, common, location, Use::Initializing)),
                        common};
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

            Value lowerCall(SourceLocation location, const ast::CallExpr& call)
            {
                std::vector<Value> args;
                for (const ast::ExprPtr& arg : call.args)
                    args.push_back(lower(*arg));

                if (isReduction(call.callee))
                    return lowerReduction(location, call, args);
                if (isPolyQuery(call.callee))
                {
                    if (!args.empty())
                        return wrongArity(location, call, 0);
                    if (call.callee == "poly_lanes")
                        return {llvm::ConstantFP::get(builder_.getFloatTy(), static_cast<double>(polyLanes_)),
                                ast::Type::Float};
                    std::vector<llvm::Constant*> indices;
                    for (unsigned i = 0; i < polyLanes_; ++i)
                        indices.push_back(llvm::ConstantFP::get(builder_.getFloatTy(), static_cast<double>(i)));
                    return {llvm::ConstantVector::get(indices), ast::Type::PolyFloat};
                }
                std::vector<llvm::Value*> irArgs;
                if (const Builtin* builtin = findBuiltin(call.callee))
                {
                    if (args.size() != builtin->arity)
                        return wrongArity(location, call, builtin->arity);
                    // Overloaded on its arguments, as OpenCL C's math functions are: with a vector argument it is the
                    // vector intrinsic (llvm.fabs.v4f32), and scalar arguments beside it are broadcast.
                    ast::Type type = ast::Type::Float;
                    for (const Value& arg : args)
                    {
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
                if (args.size() != callee.params.size())
                    return wrongArity(location, call, static_cast<unsigned>(callee.params.size()));
                for (std::size_t i = 0; i < args.size(); ++i)
                    irArgs.push_back(convert(args[i], callee.params[i].type, location, Use::Passing));
                return {builder_.CreateCall(module_.getFunction(call.callee), irArgs), callee.returnType};
            }

            // reduce_add(v), reduce_min(v), reduce_max(v): a float vector to one float. The sum is ordered, lane 0
            // first, as a loop would add them (llvm.vector.reduce.fadd without the reassoc flag), so it is exact and
            // reproducible; reassociating it into a tree is fast-math's to allow. min and max ignore NaN lanes.
            Value lowerReduction(SourceLocation location, const ast::CallExpr& call, const std::vector<Value>& args)
            {
                if (args.size() != 1)
                    return wrongArity(location, call, 1);
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

            Value wrongArity(SourceLocation location, const ast::CallExpr& call, unsigned expected)
            {
                error(location, std::string(call.args.size() > expected ? "too many" : "too few")
                                    + " arguments to function call, expected " + std::to_string(expected) + ", have "
                                    + std::to_string(call.args.size()));
                return poison(ast::Type::Float);
            }

            llvm::Module& module_;
            Diagnostics& diagnostics_;
            llvm::IRBuilder<> builder_;
            unsigned polyLanes_;                                                // a poly float's lanes
            std::unordered_map<std::string, const ast::Function*> functions_;   // the program's, by name
            std::vector<std::unordered_map<std::string, Binding>> scopes_;     // the current function's, inner last
            const ast::Function* function_ = nullptr;                          // the function being lowered

            // The loops around the statement being lowered, innermost last: where break and continue go.
            struct Loop
            {
                llvm::BasicBlock* breakTarget;
                llvm::BasicBlock* continueTarget;
            };
            std::vector<Loop> loops_;
        };
    } // namespace

    std::unique_ptr<llvm::Module> lower(const ast::Program& program, llvm::LLVMContext& context,
                                        Diagnostics& diagnostics, const Options& options)
    {
        auto module = std::make_unique<llvm::Module>(diagnostics.fileName(), context);
        module->setSourceFileName(diagnostics.fileName());
        Lowering(*module, diagnostics, options).lower(program);
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
