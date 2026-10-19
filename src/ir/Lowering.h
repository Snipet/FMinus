// src/ir/Lowering.h: the code generator's insides, shared by the files of src/ir/ (CodeGen.h is its interface). One
// class, Lowering, lowers one program into one module, and its members are defined across src/ir/ by what they lower:
//     CodeGen.cpp       ir::lower(), and the two passes: declare every function, then define each body
//     Types.cpp         FMinus's types as LLVM's, the conversions between them, the errors' helpers
//     Functions.cpp     declarations and prototypes, bodies and parameters, stack slots and scopes
//     Statements.cpp    blocks, declarations, return, if, while, for, break and continue
//     Expressions.cpp   constants, variables, operators, &&, ||, ?:, casts, assignment, ++ and --
//     Calls.cpp         calls: the builtins (math functions, reductions, poly queries) and the program's functions
//     Vectors.cpp       vector literals, swizzles, and ?: lane by lane
//     Memory.cpp        lvalues, what assignments store to: variables and their components
// A new node is lowered in the file its kind belongs to. The build says which members still need it: a std::visit
// without its lambda fails to compile, and a switch without its enumerator trips -Wswitch.
#pragma once

#include "frontend/Ast.h"
#include "frontend/Diagnostics.h"
#include "ir/CodeGen.h"

#include <llvm/IR/IRBuilder.h>
#include <llvm/IR/Intrinsics.h>
#include <llvm/IR/Module.h>

#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace fminus::ir::detail
{
    template <typename... Fs>
    struct Overloaded : Fs...
    {
        using Fs::operator()...;
    };

    inline std::string quoted(std::string_view name)
    {
        return "'" + std::string(name) + "'";
    }

    // A builtin function (Calls.cpp): its name, the LLVM intrinsic it lowers to, and its parameter count.
    struct Builtin
    {
        std::string_view name;
        llvm::Intrinsic::ID intrinsic;
        unsigned arity;
    };

    const Builtin* findBuiltin(std::string_view name);   // <math.h>'s float functions, or nullptr
    bool isReduction(std::string_view name);             // reduce_add, reduce_min, reduce_max
    bool isPolyQuery(std::string_view name);             // poly_lanes, poly_index

    class Lowering
    {
    public:
        Lowering(llvm::Module& module, Diagnostics& diagnostics, const Options& options);

        // Declares every function, then defines each body (CodeGen.cpp).
        void lower(const ast::Program& program);

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

        // Where a value meets the type it must have, for the message when it cannot (clang's wording for each).
        enum class Use
        {
            Initializing,
            Assigning,
            Returning,
            Passing,
            Condition,
            Logical,
            Casting,
        };

        // Something a program can assign (Memory.cpp): a variable, or components of one. read() and write() load and
        // store it.
        struct LValue
        {
            llvm::Value* address = nullptr;   // the variable's stack slot
            ast::Type type;                   // what a read gives, and what a write converts to
            bool isConst = false;
            std::string name;                 // the variable's
            ast::Type whole;                  // the type of what is at `address`
            std::vector<int> components;      // for components, the lanes of `whole` they are
        };

        // The loops around the statement being lowered, innermost last: where break and continue go.
        struct Loop
        {
            llvm::BasicBlock* breakTarget;
            llvm::BasicBlock* continueTarget;
        };

        // ---- Types.cpp ----------------------------------------------------------------------------------------------
        unsigned lanes(ast::Type type) const;
        llvm::Type* typeOf(ast::Type type);
        llvm::Value* convert(Value value, ast::Type type, SourceLocation location, Use use);
        void incompatible(SourceLocation location, Use use, ast::Type from, ast::Type to);
        bool commonType(SourceLocation location, ast::Type a, ast::Type b, ast::Type& type);
        void polyMismatch(SourceLocation location, ast::Type a, ast::Type b);
        Value poison(ast::Type type);
        void error(SourceLocation location, std::string message);

        // ---- Functions.cpp ------------------------------------------------------------------------------------------
        static bool sameSignature(const ast::Function& a, const ast::Function& b);
        void declare(const ast::Function& function);
        void define(const ast::Function& function);
        llvm::AllocaInst* allocate(llvm::Type* type, const std::string& name);
        static bool reachable(llvm::BasicBlock* block);
        bool bind(const std::string& name, Binding binding);
        const Binding* find(const std::string& name) const;

        // ---- Statements.cpp -----------------------------------------------------------------------------------------
        void lowerStatements(const std::vector<ast::Stmt>& statements);
        void lowerStatement(const ast::Stmt& stmt);
        void lowerScoped(const ast::Stmt& stmt);
        llvm::BasicBlock* newBlock(const char* name);
        void fallThrough(llvm::BasicBlock* target);
        void lowerIf(const ast::IfStmt& branch);
        void lowerWhile(const ast::WhileStmt& loop);
        void lowerFor(const ast::ForStmt& loop);
        void declareVariable(SourceLocation location, const ast::DeclStmt& decl);

        // ---- Expressions.cpp ----------------------------------------------------------------------------------------
        Value lower(const ast::Expr& expr);
        static bool isComparison(ast::BinaryOp op);
        Value binary(SourceLocation location, ast::BinaryOp op, Value lhs, Value rhs);
        llvm::Value* arithmetic(ast::BinaryOp op, ast::Type type, llvm::Value* lhs, llvm::Value* rhs);
        llvm::Value* lowerLogical(SourceLocation location, const ast::LogicalExpr& logical);
        Value lowerConditional(SourceLocation location, const ast::ConditionalExpr& conditional);
        Value lowerNumber(SourceLocation location, const ast::NumberExpr& number);
        llvm::Value* lowerInteger(SourceLocation location, const std::string& spelling, bool hex);
        Value lowerCast(SourceLocation location, const ast::CastExpr& cast);
        Value lowerAssign(SourceLocation location, const ast::AssignExpr& assign);
        Value compound(SourceLocation location, ast::AssignOp op, Value current, Value value);
        static ast::BinaryOp compoundOperator(ast::AssignOp op);
        Value lowerIncrement(SourceLocation location, const ast::IncrementExpr& increment);

        // ---- Vectors.cpp --------------------------------------------------------------------------------------------
        Value lowerSelect(SourceLocation location, Value test, const ast::ConditionalExpr& conditional);
        Value lowerConstruct(SourceLocation location, const ast::ConstructExpr& construct);
        Value lowerBraces(SourceLocation location, const ast::ConstructExpr& construct, const std::vector<Value>& args);
        std::optional<std::vector<int>> components(SourceLocation location, const std::string& name,
                                                   ast::Type vector);
        std::optional<ast::Type> componentsType(SourceLocation location, std::size_t count);
        bool hasComponents(SourceLocation location, ast::Type type);
        Value lowerSwizzle(SourceLocation location, const ast::SwizzleExpr& swizzle);

        // ---- Memory.cpp ---------------------------------------------------------------------------------------------
        static bool isLValue(const ast::Expr& expr);
        std::optional<LValue> lvalue(const ast::Expr& expr, SourceLocation location);
        bool writable(SourceLocation location, const LValue& target);
        Value read(const LValue& source);
        llvm::Value* write(SourceLocation location, const LValue& target, Value value, Use use);

        // ---- Calls.cpp ----------------------------------------------------------------------------------------------
        Value lowerCall(SourceLocation location, const ast::CallExpr& call);
        Value lowerReduction(SourceLocation location, const ast::CallExpr& call, const std::vector<Value>& args);
        Value wrongArity(SourceLocation location, const ast::CallExpr& call, unsigned expected);

        // ---- the state of one lowering ----------------------------------------------------------------------------
        llvm::Module& module_;
        Diagnostics& diagnostics_;
        llvm::IRBuilder<> builder_;
        unsigned polyLanes_;                                                // a poly float's lanes
        std::unordered_map<std::string, const ast::Function*> functions_;   // the program's, by name
        std::vector<std::unordered_map<std::string, Binding>> scopes_;     // the current function's, inner last
        const ast::Function* function_ = nullptr;                          // the function being lowered
        std::vector<Loop> loops_;
    };
} // namespace fminus::ir::detail
