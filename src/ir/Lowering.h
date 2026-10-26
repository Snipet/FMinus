// src/ir/Lowering.h: the code generator's insides, shared by the files of src/ir/ (CodeGen.h is its interface). One
// class, Lowering, lowers one program into one module, and its members are defined across src/ir/ by what they lower:
//     CodeGen.cpp       ir::lower(), and the two passes: declare every function, then define each body
//     Types.cpp         FMinus's types as LLVM's, the conversions between them, the errors' helpers
//     Functions.cpp     declarations and prototypes, bodies and parameters, stack slots and scopes
//     Statements.cpp    blocks, declarations, return, if, while, for, break and continue
//     Expressions.cpp   constants, variables, operators, &&, ||, ?:, casts, assignment, ++ and --
//     Calls.cpp         calls: the builtins (math functions, reductions, poly queries and memory, the heap, the FFT)
//                       and the program's
//     Math.cpp          sinf, cosf, expf, tanhf and logf in the vector and libm math modes: the runtime's functions
//     Vectors.cpp       vector literals, swizzles, and ?: lane by lane
//     Memory.cpp        lvalues (variables, p[i], *p, components) read and written, arrays, &x, pointer arithmetic
//     Modules.cpp       modules: their layout, fields, methods and calls of them, `this`, copies
//     Effects.cpp       [[clang::nonallocating]] and [[clang::nonblocking]], checked over the calls
//     Parallel.cpp      parallel for and parallel { }: what would race refused, each body a function of its own that
//                       the runtime's pool runs, and what the body uses of the function around it captured
//     Tbaa.cpp          C's strict aliasing, told to LLVM: the type tag (!tbaa) on every load and store of memory
//     Denormals.cpp     Denormals::Flush: constants folded as flushing hardware computes them (Denormals.h), every
//                       function marked so for LLVM, and each entry point setting the mode around its body
// A new node is lowered in the file its kind belongs to. The build says which members still need it: a std::visit
// without its lambda fails to compile, and a switch without its enumerator trips -Wswitch.
#pragma once

#include "frontend/Ast.h"
#include "frontend/Diagnostics.h"
#include "ir/CodeGen.h"
#include "ir/Denormals.h"

#include <llvm/IR/IRBuilder.h>
#include <llvm/IR/Intrinsics.h>
#include <llvm/IR/Module.h>

#include <cstdint>
#include <map>
#include <optional>
#include <string>
#include <utility>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
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

    // A builtin function (Calls.cpp): its name, the LLVM intrinsic it lowers to, and its parameter count. A
    // transcendental one also names its runtime functions' stem (Math.cpp): "sin" for fminus_rt_sinf,
    // fminus_rt_sin_f4 and fminus_rt_libm_sinf, which the vector and libm modes call instead of the intrinsic. An
    // exact one (fabsf, sqrtf, ...) has none, and is its intrinsic in every mode.
    struct Builtin
    {
        std::string_view name;
        llvm::Intrinsic::ID intrinsic;
        unsigned arity;
        std::string_view runtime = {};
    };

    // An allocation builtin (Calls.cpp): C's name, the runtime function it calls (runtime/Runtime.h) and its arity.
    struct Allocation
    {
        std::string_view name;
        std::string_view runtime;
        unsigned arity;
    };

    // An FFT builtin (Calls.cpp): its name, its parameter count, and whether it allocates (a plan is made and freed off
    // the audio thread; a transform on one allocates nothing).
    struct Fft
    {
        std::string_view name;
        unsigned arity;
        bool allocates;
    };

    const Builtin* findBuiltin(std::string_view name);   // <math.h>'s float functions, or nullptr
    const Allocation* findAllocation(std::string_view name);   // malloc, calloc, free, or nullptr
    const Fft* findFft(std::string_view name);           // fft_create, fft_forward, ..., or nullptr
    bool allocates(std::string_view name);               // a builtin that allocates or frees: the heap's, a plan's
    bool isReduction(std::string_view name);             // reduce_add, reduce_min, reduce_max
    bool isPolyQuery(std::string_view name);             // poly_lanes, poly_index
    bool isPolyMemory(std::string_view name);            // poly_load, poly_store

    // A loop's branches back to `header`, but not `entry`, marked llvm.loop.mustprogress (Statements.cpp).
    void mustProgress(llvm::BasicBlock* header, const llvm::Instruction* entry);

    class Lowering
    {
    public:
        // What a function promises not to do (Effects.cpp), weakest first: nonblocking includes nonallocating.
        enum class Effect
        {
            None,
            NonAllocating,
            NonBlocking,
        };

        Lowering(llvm::Module& module, Diagnostics& diagnostics, const Options& options);

        // Declares every function, then defines each body (CodeGen.cpp).
        void lower(const ast::Program& program);

    private:
        // A lowered expression: its IR and its FMinus type. The rules read the FMinus type, never the IR's, since
        // two FMinus types may share an LLVM type: one whose width the target decides would be <4 x float> on
        // NEON, as float4 is. A module's value is its address (a ptr): it is copied from there, never loaded whole.
        struct Value
        {
            llvm::Value* ir = nullptr;
            ast::Type type = ast::Type::Float;
        };

        // A variable in scope: where it is (its stack slot; in a parallel region, the region's own slot for a copy of
        // a variable of the function around it, or that variable's address: Parallel.cpp), whether it may be
        // assigned, and its type.
        struct Binding
        {
            llvm::Value* slot;
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

        // Something a program can assign, or take the address of (Memory.cpp): a variable, an element a pointer
        // reaches (p[i], *p), a module's field (m.y, p->y), or components of any of them. read() and write() load
        // and store it.
        struct LValue
        {
            llvm::Value* address = nullptr;   // the variable's stack slot, or the element's address
            ast::Type type;                   // what a read gives, and what a write converts to
            bool isConst = false;             // a const variable, or an element through a const T*
            std::string name;                 // the variable's; empty for an element
            ast::Type whole;                  // the type of what is at `address`
            std::vector<int> components;      // for components, the lanes of `whole` they are
        };

        // A call a function's body makes, for Effects.cpp: where, and of what (a builtin, or a function by name).
        struct CallSite
        {
            SourceLocation location;
            std::string callee;
        };

        // The loops around the statement being lowered, innermost last: where break and continue go. A parallel
        // region's body is one too, whose break (and a parallel { }'s continue) goes nowhere: an error.
        struct Loop
        {
            llvm::BasicBlock* breakTarget;
            llvm::BasicBlock* continueTarget;
        };

        // What a method's reads of one stream use, computed where it starts (Modules.cpp, atEntry): whether it is
        // flat, the slot's value, and where sample i is, base + i * stride: the stream's pointer and 1, or, when the
        // stream is flat, the slot's value field and 0.
        struct StreamRead
        {
            llvm::Value* flat;
            llvm::Value* value;
            llvm::Value* base;
            llvm::Value* stride;
        };

        // What a parallel region's body uses of the function around it (Parallel.cpp), in the context the function
        // fills for it: a variable's value or its address, or what a method read of a parameter where it starts.
        struct Capture
        {
            enum class Kind
            {
                Value,       // a variable the region only reads: a copy
                Reference,   // one it assigns or takes the address of, an array or a module: its address
                Parameter,   // a knob's value, or one of what a stream's reads use (StreamRead's, in order)
            };
            Kind kind = Kind::Value;
            std::string name;           // the variable's, or the parameter's
            unsigned field = 0;         // the parameter's field
            bool stream = false;        // a stream's: then part is 0 flat, 1 value, 2 base, 3 stride
            unsigned part = 0;
            std::uint64_t offset = 0;   // where it is in the context, in bytes
        };

        // A parallel for or a parallel { } being lowered (Parallel.cpp). Its body goes into a function of its own, of
        // the runtime's task type, which reaches what it uses of the function around it through the context.
        struct Region
        {
            SourceLocation location;                       // the statement's
            bool loop = true;                              // a parallel for; else a parallel { }
            std::size_t base = 0;                          // scopes_[base] on are the region's own
            llvm::Function* function = nullptr;            // the body's
            llvm::BasicBlock* body = nullptr;              // where its code starts, after the captures
            std::vector<Capture> captures;
            std::unordered_set<std::string> byReference;   // the variables from outside it assigns or takes & of
            std::uint64_t size = 0;                        // the context's bytes so far
            llvm::Align align = llvm::Align(1);
            llvm::IRBuilderBase::InsertPoint resume;       // where the function around it goes on
            // The function around it's reads of its parameters (atEntry), set aside while the region has its own.
            std::map<std::pair<unsigned, bool>, llvm::Value*> entryLoads;
            std::map<unsigned, StreamRead> flatStreams;
            std::vector<llvm::Instruction*> streamReads;
        };

        // The runtime functions behind a transcendental builtin (Math.cpp): the vector mode's scalar one
        // (fminus_rt_sinf) and its four lanes (fminus_rt_sin_f4), and the libm mode's (fminus_rt_libm_sinf).
        enum class MathFunction
        {
            Scalar,
            Four,
            Libm,
        };

        // ---- Types.cpp ----------------------------------------------------------------------------------------------
        unsigned lanes(ast::Type type) const;
        llvm::Type* typeOf(ast::Type type);
        llvm::Value* convert(Value value, ast::Type type, SourceLocation location, Use use);
        llvm::Value* convertPointer(Value value, ast::Type type, SourceLocation location, Use use);
        llvm::Type* valueTypeOf(ast::Type type);
        void incompatible(SourceLocation location, Use use, ast::Type from, ast::Type to);
        void pointerMismatch(SourceLocation location, Use use, ast::Type from, ast::Type to, bool discardsConst);
        bool checkType(SourceLocation location, ast::Type type);
        void noPointerTo(SourceLocation location, ast::Type pointee);
        std::optional<unsigned> sizeOf(ast::Type type);
        llvm::Align alignOf(ast::Type type);
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
        const Binding* find(const std::string& name);
        const Binding* lookup(const std::string& name) const;

        // ---- Statements.cpp -----------------------------------------------------------------------------------------
        void lowerStatements(const std::vector<ast::Stmt>& statements);
        void lowerStatement(const ast::Stmt& stmt);
        void lowerScoped(const ast::Stmt& stmt);
        void lowerReturn(SourceLocation location, const ast::ReturnStmt& ret);
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
        Value lowerSizeof(SourceLocation location, const ast::SizeofExpr& size);
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
        Value lowerMember(SourceLocation location, const ast::MemberExpr& member);

        // ---- Memory.cpp ---------------------------------------------------------------------------------------------
        static bool isLValue(const ast::Expr& expr);
        std::optional<LValue> lvalue(const ast::Expr& expr, SourceLocation location);
        std::optional<LValue> element(SourceLocation location, Value pointer, std::optional<Value> index);
        bool writable(SourceLocation location, const LValue& target);
        Value read(const LValue& source);
        Value readLValue(const ast::Expr& expr);
        llvm::Value* write(SourceLocation location, const LValue& target, Value value, Use use);
        Value lowerAddressOf(SourceLocation location, const ast::Expr& operand);
        Value pointerArithmetic(SourceLocation location, ast::BinaryOp op, Value lhs, Value rhs);
        void zero(llvm::Value* address, ast::Type type);
        void initializeArray(SourceLocation location, llvm::Value* address, ast::Type type,
                             const std::vector<ast::ExprPtr>& list);

        // ---- Calls.cpp ----------------------------------------------------------------------------------------------
        Value lowerCall(SourceLocation location, const ast::CallExpr& call);
        Value lowerReduction(SourceLocation location, const ast::CallExpr& call, const std::vector<Value>& args);
        Value lowerPolyMemory(SourceLocation location, const ast::CallExpr& call, const std::vector<Value>& args);
        Value wrongArity(SourceLocation location, std::size_t have, std::size_t expected);
        Value lowerAllocation(SourceLocation location, const Allocation& allocation, const std::vector<Value>& args);
        Value lowerFft(SourceLocation location, const Fft& fft, const std::vector<Value>& args);
        llvm::CallInst* callRuntime(const std::string& name, llvm::Type* result, const std::vector<llvm::Value*>& args);

        // ---- Math.cpp -----------------------------------------------------------------------------------------------
        llvm::Value* lowerMath(const Builtin& builtin, ast::Type type, llvm::Value* x);
        static std::string mathName(const Builtin& builtin, MathFunction which);
        bool checkMathNames(SourceLocation location, const Builtin& builtin);
        llvm::FunctionCallee mathFunction(const Builtin& builtin, MathFunction which);

        // ---- Modules.cpp --------------------------------------------------------------------------------------------
        void layOut(const ast::Program& program);
        void checkParameter(const ast::Module& module, const ast::Field& field);
        llvm::Type* slotTypeOf(ast::Type type);
        ParamBinding bindingOf(const ast::Module& module, const ast::Field& field) const;
        std::optional<unsigned> parameterOf(const ast::Expr& base) const;
        Value lowerParameter(SourceLocation location, unsigned field, const ast::IndexExpr& index);
        llvm::Value* atEntry(unsigned field, bool stream);
        void scopeStreams(llvm::Function& function);
        static const ast::Field* findField(const ast::Module& module, const std::string& name, unsigned* index);
        std::optional<LValue> field(SourceLocation location, llvm::Value* address, const ast::Module& module,
                                    const std::string& name, bool isConst);
        llvm::Value* self();
        Value lowerThis(SourceLocation location);
        Value lowerMethodCall(SourceLocation location, const ast::MethodCallExpr& call);
        Value callMethod(SourceLocation location, const ast::Module& module, llvm::Value* address, bool isConst,
                         const std::string& name, const std::vector<Value>& args);
        void copy(llvm::Value* target, llvm::Value* source, ast::Type type);

        // ---- Effects.cpp --------------------------------------------------------------------------------------------
        Effect declaredEffect(const ast::Function& function);
        Effect effectOf(const std::string& name) const;
        std::optional<std::vector<Diagnostic>> unsafe(const std::string& callee, Effect required,
                                                      std::vector<std::string>& looking);
        void checkEffects(const ast::Program& program);

        // ---- Parallel.cpp -------------------------------------------------------------------------------------------
        void lowerParallelFor(SourceLocation location, const ast::ParallelForStmt& loop);
        void lowerParallelBlock(SourceLocation location, const ast::ParallelBlockStmt& block);
        std::unordered_set<std::string> checkParallelFor(const ast::ParallelForStmt& loop, const std::string& counter);
        std::unordered_set<std::string> checkParallelBlock(const ast::ParallelBlockStmt& block);
        bool changesModule(const std::string& method);
        void beginRegion(SourceLocation location, bool loop, std::unordered_set<std::string> byReference);
        void endRegion(llvm::Value* begin, llvm::Value* end);
        const Binding* capture(const std::string& name, const Binding& outer);
        llvm::Value* captured(Capture capture, llvm::Type* type, const std::string& name);
        llvm::Value* capturedParameter(unsigned field, bool stream);
        llvm::Value* outerValue(const Capture& capture);

        // ---- Tbaa.cpp -----------------------------------------------------------------------------------------------
        llvm::MDNode* accessTag(ast::Type type);
        void tag(llvm::Instruction* access, ast::Type type);
        // ---- Denormals.cpp ------------------------------------------------------------------------------------------
        void flushDenormals(const ast::Program& program);
        void wrapEntry(llvm::Function& function, SourceLocation location, bool arm);

        // ---- the state of one lowering ----------------------------------------------------------------------------
        llvm::Module& module_;
        Diagnostics& diagnostics_;
        llvm::IRBuilder<DenormalFolder> builder_;                          // folds constants as the mode computes
        unsigned polyLanes_;                                                // a poly float's lanes
        Math math_;                                                         // how sinf, cosf, ... compute
        std::unordered_map<std::string, const ast::Function*> functions_;   // the program's, by name
        std::vector<std::unordered_map<std::string, Binding>> scopes_;     // the current function's, inner last
        const ast::Function* function_ = nullptr;                          // the function being lowered
        std::vector<Loop> loops_;
        std::unordered_map<std::string, Effect> effects_;                  // each name's, from all its declarations
        std::unordered_map<std::string, std::vector<CallSite>> calls_;     // each definition's calls, in order
        std::unordered_map<const ast::Module*, llvm::StructType*> structs_;   // each module's layout
        std::map<std::string, ParamBinding> bindings_;                     // the host's, by qualified name
        std::map<std::pair<unsigned, bool>, llvm::Value*> entryLoads_;     // the method's parameters, read at entry
        std::vector<llvm::Instruction*> streamReads_;                      // its loads of its streams' samples
        bool strictAliasing_;                                              // whether loads and stores carry !tbaa
        std::map<unsigned, StreamRead> flatStreams_;                       // what its streams' reads use
        Denormals denormals_;                                              // subnormals preserved or flushed
        std::string targetTriple_;                                         // which mode register an entry point sets
        unsigned threads_;                                                 // the most threads a parallel region uses
        std::vector<Region> regions_;                                      // the parallel regions being lowered
        std::vector<llvm::Function*> outlined_;                            // every region's function
        std::unordered_map<std::string, unsigned> regionCount_;            // each function's regions so far
        std::optional<std::unordered_map<std::string, bool>> changes_;     // whether each method changes its module
    };
} // namespace fminus::ir::detail
