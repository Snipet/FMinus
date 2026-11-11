// src/ir/Lowering.h: the code generator's insides, shared by the files of src/ir/ (CodeGen.h is its interface). One
// class, Lowering, lowers one program into one module, and its members are defined across src/ir/ by what they lower:
//     CodeGen.cpp       ir::lower(), and the two passes: declare every function, then define each body
//     Types.cpp         FMinus's types as LLVM's, the conversions between them, the errors' helpers
//     Functions.cpp     declarations and prototypes (libm's attributes), bodies and parameters, stack slots and scopes
//     Statements.cpp    blocks, declarations, return, if, while, for, switch and its labels, break and continue
//     Expressions.cpp   constants, variables, operators, &&, ||, ?:, casts, assignment, ++ and --
//     Constants.cpp     C's integer constant expressions, worked out where they are written (a case label's value,
//                       an enumerator's, an array's size), enumerations: their constants, in a block's scope or the
//                       file's, and the top level's constants, the file's const objects
//     Calls.cpp         calls: the builtins (math functions, reductions, poly queries and memory, the heap, the FFT,
//                       the runtime's others: bytes_floats, ...) and the program's
//     Strings.cpp       string literals: a builtin's argument, a private constant passed as its address and length;
//                       a format's places for ints, checked; the errors of a literal anywhere else
//     Paths.cpp         a JSON Pointer literal, json_float's and the others', or an XML path, xml_float's and the
//                       others', prepared into a private table of steps the runtime walks; json_choice's and
//                       xml_choice's literals into another
//     Math.cpp          sinf, cosf, tanf, expf, tanhf and logf in the vector and libm math modes: the runtime's
//                       functions
//     Hash.cpp          hash and randf: an int's bits mixed, and a float uniform in [-1, 1) from them, inline
//     Web.cpp           the web library's builtins' own part: a URL literal checked and recorded for the host
//                       (webRequests), the program's web context, and the status codes' names (HTTP_REFUSED, ...)
//     WebServer.cpp     the web server's builtins' own part: a route literal checked and recorded for the host
//                       (webRoutes), the route table http_listen hands the library, a query's name, a reply's format
//     Vectors.cpp       vector literals, swizzles, and ?: lane by lane
//     Memory.cpp        lvalues (variables, p[i], *p, components) read and written, arrays, &x, pointer arithmetic
//     Modules.cpp       modules: their layout, fields, methods and calls of them, `this`, copies
//     Effects.cpp       [[clang::nonallocating]] and [[clang::nonblocking]], checked over the calls
//     Parallel.cpp      parallel for and parallel { }: what would race refused, each body a function of its own that
//                       the runtime's pool runs, and what the body uses of the function around it captured
//     Tbaa.cpp          C's strict aliasing, told to LLVM: the type tag (!tbaa) on every load and store of memory
//     Denormals.cpp     Denormals::Flush: constants folded as flushing hardware computes them (Denormals.h), every
//                       function marked so for LLVM, and each entry point setting the mode around its body
//     Bounds.cpp        Options::boundsCheck: the arrays an access reaches, and the checks of its index against them
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

#include <array>
#include <cstddef>
#include <cstdint>
#include <deque>
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
    // transcendental one also names its stem (Math.cpp): "sin" for the math intrinsics fminus.sin.f32, fminus.sin.v2f32
    // and fminus.sin.v4f32 and the runtime functions fminus_rt_sinf, fminus_rt_sin_f4 and fminus_rt_libm_sinf, which
    // the vector and libm modes call instead of LLVM's intrinsic. An exact one (fabsf, sqrtf, ...) has none, and is its
    // intrinsic in every mode. `type` is what it computes on: a float's (<math.h>'s float functions, overloaded on the
    // float vectors), or a double's (C's double functions, sqrt, fabs, ..., exact, on doubles only).
    struct Builtin
    {
        std::string_view name;
        llvm::Intrinsic::ID intrinsic;
        unsigned arity;
        std::string_view runtime = {};
        ast::Kind type = ast::Kind::Float;
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

    // A build option a builtin needs (cmake/FMinusOptions.cmake, fminus/Config.h): none, the JSON parser (FMINUS_JSON),
    // the XML parser (FMINUS_XML), the web library (FMINUS_WEB) or its server (FMINUS_WEB_SERVER, which needs
    // FMINUS_WEB). A builtin of an option this build was configured without is still a known name:
    // a call of it is an error that names the option (notBuilt), not an undeclared function, and a definition of one
    // is refused, as any builtin's is (Functions.cpp).
    enum class Feature
    {
        None,
        Json,
        Xml,
        Web,
        WebServer,
    };
    bool isBuilt(Feature feature);                                 // whether this build has it (Calls.cpp)
    std::string notBuilt(std::string_view name, Feature feature);   // the error of a call of `name`, which needs it

    // What a builtin of the runtime may do, for the effects (Effects.cpp): allocate or free, which a nonallocating
    // function may not (and so neither may a nonblocking one); block, waiting for something (a request, a thread),
    // which a nonblocking function may not, and a nonallocating one may; or neither: it is wait-free, and the audio
    // thread may call it. The runtime's code must keep the promise (Runtime.h says it of each function).
    enum class RuntimeEffect
    {
        WaitFree,
        Blocks,
        Allocates,
    };

    // A parameter of a builtin of the runtime (RuntimeBuiltin): an int; bytes, a const void* (any pointer converts, as
    // to C's); floats it writes, a float*; a string, which must be a string literal, passed as its address and its
    // length in bytes (Strings.cpp: no NUL ends it, and it may hold one); or a format, a string literal with places for
    // ints (%d, and %% for a percent sign), checked when the program is compiled (formatArguments), after which the
    // call gives an int for each place, passed as their address and their count. Then the JSON parser's: a document,
    // a const void* (json_parse's, read); a handle, a void* (json_free's); a float, a double or a bool (a read's
    // fallback; a bool goes to the runtime as an int, 0 or 1); a path, a format that is a JSON Pointer, prepared when
    // the program is compiled into a table of steps (Paths.cpp), passed as its address and then its ints' address and
    // count, as a format's; and choices, every argument after the path's ints, string literals (at least one),
    // prepared into one table, passed as its address. The XML parser's take the same, but for its path: an XML path, a
    // format that is a small part of XPath (child steps by name, [n], a last @attribute), prepared as a JSON one is.
    // The web library's take a URL, a format whose ints are the call's last arguments, after every other parameter
    // (as printf's are), checked further (Web.cpp: no byte a URL cannot hold, http or https, no credentials) and
    // recorded for the host, passed after the program's web context as a format is; and a content type, a string of
    // printable ASCII. The web server's take a route, a string that is a method and a path with places for numbers
    // the request's path holds ("PUT /osc/%d/gain"), checked (WebServer.cpp) and recorded for the host and for the
    // route table, passed as a string is; a query's name, a string of letters, digits, '_', '-' and '.'; and a reply's
    // format, a format whose places are C's printf's %d (an int or a bool) and %g or %.Ng (a float or a double),
    // checked as clang checks printf's, after which the call gives a double for each place, passed as their address and
    // their count.
    enum class Param
    {
        Int,
        Bytes,
        Floats,
        String,
        Format,
        Document,
        Handle,
        Float,
        Double,
        Bool,
        Path,
        Choices,
        XmlPath,
        Url,
        ContentType,
        Route,
        QueryName,
        ReplyFormat,
    };

    // What a builtin of the runtime gives: an int, a float, a double, a bool (an int from the runtime, 0 or 1), a void*,
    // a const void* (bytes the program may only read: http_body's) or nothing.
    enum class Result
    {
        Int,
        Float,
        Double,
        Bool,
        Pointer,
        Bytes,
        Void,
    };

    // A builtin that calls a function of the runtime with its arguments, as Param says to pass each (Calls.cpp,
    // kRuntimeBuiltins): its name, the runtime's function, its parameters (the first `arity` of `params`; a format's or
    // a path's ints are not among them), what it may do on the audio thread, the build option it needs, and its
    // result.
    struct RuntimeBuiltin
    {
        std::string_view name;
        std::string_view symbol;
        std::array<Param, 6> params;
        unsigned arity;
        RuntimeEffect effect;
        Feature feature = Feature::None;
        Result result = Result::Int;
    };

    // The C type of a runtime builtin's parameter or result, as its prototype would write it (Calls.cpp): what a
    // prototype of the builtin must say (Functions.cpp), and what an argument converts to. A string, a format, a path
    // and choices have none (std::nullopt): FMinus has no char.
    std::optional<ast::Type> paramType(Param param);
    ast::Type resultType(Result result);

    // A format's places for ints (Strings.cpp, scanFormat): the offset in the literal's bytes of each %d's '%', in
    // order, and its errors, each at the offset of the byte it is about, in clang's words for printf's.
    struct FormatScan
    {
        struct Error
        {
            std::size_t offset;
            std::string message;
        };
        std::vector<std::size_t> places;
        std::vector<Error> errors;
    };
    FormatScan scanFormat(std::string_view text);

    // A reply's format (WebServer.cpp, scanReplyFormat): C's printf's %d, %g, %.Ng (N from 1 to 17) and %%. Each place,
    // in order, the offset of its '%' and whether it takes a double (%g, %.Ng) or an int (%d); and its errors, each at
    // the offset of the byte it is about, in clang's words for printf's where clang has words for it.
    struct ReplyFormatScan
    {
        struct Place
        {
            std::size_t offset;
            bool real;
        };
        std::vector<Place> places;
        std::vector<FormatScan::Error> errors;
    };
    ReplyFormatScan scanReplyFormat(std::string_view text);

    // A route literal's errors (WebServer.cpp, scanRoute): a method of GET, POST, PUT, PATCH or DELETE, one space, and a
    // path of "/" and segments of letters, digits, '-', '.', '_', '~' and %d, none empty, "." or "..", with at most 8
    // %d and never two together; each at the offset of the byte it is about. Empty: the route is good.
    std::vector<FormatScan::Error> scanRoute(std::string_view text);

    // A JSON Pointer (RFC 6901) with a format's places (%d, %%), prepared (Paths.cpp, scanPath): its reference tokens
    // as the runtime's table holds them (runtime/Runtime.h), each its count of pieces and then a pair a piece, (offset,
    // length) of a literal's bytes in `bytes`, or (place, -1) for the place-th %d; and its errors, each at the offset
    // in the literal's bytes of the byte it is about. A format's own errors (a % before anything but d or %) are
    // scanFormat's, and not here again: such a % is a byte of its token.
    struct PathScan
    {
        std::vector<std::int32_t> code;
        std::string bytes;
        std::vector<FormatScan::Error> errors;
    };
    PathScan scanPath(std::string_view text);

    // An XML path with a format's places (%d, %%), prepared (Paths.cpp, scanXmlPath): its steps as the runtime's table
    // holds them (runtime/Runtime.h), each its kind, its name's pieces as a JSON Pointer's token's, and its index; and
    // its errors, as scanPath's. A format's own errors are scanFormat's, and not here again.
    PathScan scanXmlPath(std::string_view text);

    const Builtin* findBuiltin(std::string_view name);   // <math.h>'s float functions, or nullptr
    const Allocation* findAllocation(std::string_view name);   // malloc, calloc, free, or nullptr
    const Fft* findFft(std::string_view name);           // fft_create, fft_forward, ..., or nullptr
    const RuntimeBuiltin* findRuntimeBuiltin(std::string_view name);   // bytes_floats, ..., or nullptr
    bool allocates(std::string_view name);               // a builtin that allocates or frees: the heap's, a plan's
    bool blocks(std::string_view name);                  // a builtin that may wait: http_wait
    bool isReduction(std::string_view name);             // reduce_add, reduce_min, reduce_max
    bool isPolyQuery(std::string_view name);             // poly_lanes, poly_index
    bool isPolyMemory(std::string_view name);            // poly_load, poly_store
    bool isHash(std::string_view name);                  // hash, randf

    // The most a table holds as a constant global, 1 MiB: 262,144 floats, 65,536 float4s. The JIT's object holds every
    // byte of a global, its zeros too (in a read-only section, which it then copies), so a table costs the compiler
    // about twice its size. A local const array over it is a stack slot (Memory.cpp, declareConstArray); a constant of
    // the top level over it is an error (Constants.cpp, defineConstant).
    inline constexpr std::uint64_t kMaxTableBytes = std::uint64_t{1} << 20;

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
        // Under Options::boundsCheck, the array a pointer points into, where the code generator can see it
        // (Bounds.cpp): the pointer an array's name stands for, and what p + i, p - i and &p[i] make of one. Where in
        // it the pointer is, its element from the array's first, an i64 (null for the first itself); how many elements
        // it has; and how the source names it and its type, for the message when a check fails.
        struct Extent
        {
            llvm::Value* offset = nullptr;
            std::int64_t count = 0;
            std::string name;
            ast::Type type;
        };

        // A lowered expression: its IR and its FMinus type. The rules read the FMinus type, never the IR's, since
        // two FMinus types may share an LLVM type: one whose width the target decides would be <4 x float> on
        // NEON, as float4 is. A module's value is its address (a ptr): it is copied from there, never loaded whole.
        // A pointer into an array the code generator can see carries its extent, under Options::boundsCheck (above).
        //
        // `failed` is clang's "contains errors": the expression, or a part of it, reported an error, and it is set only
        // where one is behind it: by lower(), which counts the errors its expression reports, and by recovery(), the
        // stand-in for what an operand's error, or a declaration's, kept from being computed. A plain poison() is not
        // failed: `c ? f() : g()` of two void calls has no value and no error, and each of its uses must report one, as
        // a void call's do. What takes a failed value says nothing more about it: an operator gives a recovery() of the
        // type it would likely have had, and a conversion a poison of its target's type, so the first error is the
        // only one, as clang reports it. Its type is no test for this: the stand-in for an undeclared name is a float,
        // which most operators take.
        struct Value
        {
            llvm::Value* ir = nullptr;
            ast::Type type = ast::Type::Float;
            const Extent* extent = nullptr;
            bool failed = false;
        };

        // A variable in scope: where it is (its stack slot; in a parallel region, the region's own slot for a copy of
        // a variable of the function around it, or that variable's address: Parallel.cpp), whether it may be
        // assigned, and its type. Or an enumeration constant (Constants.cpp), which shares C's name space with the
        // variables: an int with no slot, its value. Where it was declared, for a redefinition's note.
        struct Binding
        {
            llvm::Value* slot;
            bool isConst;
            ast::Type type;
            SourceLocation location = {};
            std::optional<std::int32_t> enumerator = std::nullopt;
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
            std::string array = {};           // an element of a const array a variable names (t[i]): the variable's,
            ast::Type arrayType = {};         // and its type, for clang's message when it is assigned
            const Extent* extent = nullptr;   // an element's array, where it is in it (Options::boundsCheck)
        };

        // A call a function's body makes, for Effects.cpp: where, and of what (a builtin, or a function by name).
        struct CallSite
        {
            SourceLocation location;
            std::string callee;
        };

        // The loops and switches around the statement being lowered, innermost last: where break and continue go, and
        // which switch a case label is of. break leaves the innermost of either, continue goes to the innermost loop's
        // next iteration, and a label belongs to the innermost switch. A parallel region's body is one too, whose
        // break (and a parallel { }'s continue) goes nowhere, and which no label reaches from outside: an error.
        struct Breakable
        {
            enum class Kind
            {
                Loop,
                Switch,   // switches_.back(): a label's
                Region,
            };
            Kind kind;
            llvm::BasicBlock* breakTarget;      // a loop's end, or a switch's; null for a region
            llvm::BasicBlock* continueTarget;   // a loop's next iteration, a parallel for's too; null otherwise
        };

        // A label a switch has (Statements.cpp): where its value is written, and the name it is written as when it is
        // one (`case A:`, (int)A too), for the message about a duplicate.
        struct CaseLabel
        {
            SourceLocation location;
            std::string name;
        };

        // A switch being lowered (Statements.cpp): its instruction, to which each case label adds its value and its
        // block, the first scope of its body (scopes_[base] on hold what the body declares), its labels so far, and
        // whether one was in error (no constant, or a duplicate): clang's -Wswitch then says nothing.
        struct Switch
        {
            llvm::SwitchInst* instruction;
            std::size_t base;
            std::map<std::int32_t, CaseLabel> cases;
            std::optional<SourceLocation> defaultLabel;
            bool erroneous = false;
        };

        // A constant expression's value, as fold() works it out (Constants.cpp): an int's or a bool's (0 or 1), and
        // which, or a float or a double, whose value it does not need. `floating` is where a floating constant takes
        // part other than as a cast's operand, the one place C allows one: a float's and a double's always have it,
        // and an int that has it (1.5f < 2) is no integer constant expression either.
        struct Folded
        {
            std::int32_t value = 0;
            ast::Type type = ast::Type::Int;   // Int, Bool, Float or Double
            std::optional<SourceLocation> floating;
        };

        // An initialiser list's values placed in an array (Memory.cpp, placeList), as C places them: each value and the
        // element it initialises, from the array's first (a 2-D array's row by row), in the list's order; one past the
        // last element it reaches (a braced row reaches its row's end), what sizes an array declared `[]`; and whether
        // it had no error.
        struct Placed
        {
            std::vector<std::pair<std::uint64_t, const ast::Expr*>> values;
            std::uint64_t reach = 0;
            bool ok = true;
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

        // The functions behind a transcendental builtin (Math.cpp): the vector mode's scalar one, its two lanes and its
        // four (the math intrinsics fminus.sin.f32, fminus.sin.v2f32 and fminus.sin.v4f32; the runtime's functions
        // fminus_rt_sinf and fminus_rt_sin_f4 compute the first and the last), and the libm mode's
        // (fminus_rt_libm_sinf).
        enum class MathFunction
        {
            Scalar,
            Two,
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
        std::optional<std::uint64_t> sizeOf(ast::Type type);
        llvm::Align alignOf(ast::Type type);
        bool commonType(SourceLocation location, ast::Type a, ast::Type b, ast::Type& type);
        void polyMismatch(SourceLocation location, ast::Type a, ast::Type b);
        Value poison(ast::Type type);
        Value recovery(ast::Type type);
        void error(SourceLocation location, std::string message);

        // ---- Functions.cpp ------------------------------------------------------------------------------------------
        static bool sameSignature(const ast::Function& a, const ast::Function& b);
        void declare(const ast::Function& function);
        void describeThis(const ast::Module& module);
        void define(const ast::Function& function);
        void describeLibm();
        llvm::AllocaInst* allocate(llvm::Type* type, const std::string& name);
        static bool reachable(llvm::BasicBlock* block);
        bool bind(const std::string& name, Binding binding);
        const Binding* find(const std::string& name);
        const Binding* lookup(const std::string& name) const;
        const Binding* named(const std::string& name) const;

        // ---- Statements.cpp -----------------------------------------------------------------------------------------
        void lowerStatements(const std::vector<ast::Stmt>& statements);
        void lowerStatement(const ast::Stmt& stmt);
        void lowerScoped(const ast::Stmt& stmt);
        void lowerReturn(SourceLocation location, const ast::ReturnStmt& ret);
        llvm::BasicBlock* newBlock(const char* name);
        void fallThrough(llvm::BasicBlock* target);
        void branchOn(const ast::Expr& test, llvm::Value* condition, llvm::BasicBlock* whenTrue,
                      llvm::BasicBlock* whenFalse);
        void lowerIf(const ast::IfStmt& branch);
        void lowerWhile(const ast::WhileStmt& loop);
        void lowerFor(const ast::ForStmt& loop);
        bool isConstant(const ast::Expr& expr, bool folding = false) const;
        bool folds(const ast::Expr& expr) const;
        void lowerSwitch(const ast::SwitchStmt& branch);
        void lowerLabel(SourceLocation location, const ast::Expr* value, const ast::StmtPtr& body);
        void unhandled(SourceLocation location, int enumeration, const Switch& lowered);
        static SourceLocation begin(const ast::Expr& expr);
        llvm::BasicBlock* bypass(const Switch& branch, llvm::BasicBlock* label);
        void declareVariable(SourceLocation location, const ast::DeclStmt& decl);
        void refuseAttributes(const ast::DeclStmt& decl);
        void redefinition(SourceLocation location, const std::string& name, bool enumerator, const Binding& previous);

        // ---- Constants.cpp ------------------------------------------------------------------------------------------
        std::optional<std::int32_t> enumerator(const std::string& name) const;
        void declareEnum(const ast::Enum& enumeration);
        void fileNameClash(SourceLocation location, const std::string& name, std::size_t position, bool enumerator);
        ast::Type sized(SourceLocation location, const std::string& name, ast::Type type, const ast::Expr* size,
                        const ast::Expr* rowSize, const ast::DeclStmt* listed);
        ast::Type declaredArray(SourceLocation location, const ast::DeclStmt& decl, ast::Type type);
        static ast::Type floatsLike(ast::Type array);
        std::optional<std::int32_t> arraySize(const std::string& name, const ast::Expr& size);
        void checkSizes(const ast::Function& function);
        void defineConstant(const ast::Constant& constant);
        bool atTopLevel() const;
        std::optional<std::int32_t> integerConstant(const ast::Expr& expr);
        std::optional<Folded> fold(const ast::Expr& expr, bool evaluated);
        std::optional<Folded> foldBinary(SourceLocation location, ast::BinaryOp op, Folded lhs, Folded rhs,
                                         bool evaluated);
        std::optional<Folded> notConstant(SourceLocation location);

        // ---- Expressions.cpp ----------------------------------------------------------------------------------------
        Value lower(const ast::Expr& expr);
        Value lowerAs(const ast::Expr& expr, ast::Type type);
        Value lowerWith(const ast::Expr& expr, bool doubles);
        static bool isArithmetic(ast::BinaryOp op);
        bool hasDouble(const ast::Expr& expr) const;
        std::optional<ast::Type> staticType(const ast::Expr& expr) const;
        static bool isComparison(ast::BinaryOp op);
        static bool isBitwise(ast::BinaryOp op);
        Value binary(SourceLocation location, ast::BinaryOp op, Value lhs, Value rhs);
        static ast::Type recovered(ast::BinaryOp op, const Value& lhs, const Value& rhs);
        llvm::Value* arithmetic(ast::BinaryOp op, ast::Type type, llvm::Value* lhs, llvm::Value* rhs);
        llvm::Value* lowerLogical(SourceLocation location, const ast::LogicalExpr& logical);
        Value lowerConditional(SourceLocation location, const ast::ConditionalExpr& conditional, bool doubles);
        Value lowerNumber(SourceLocation location, const ast::NumberExpr& number, bool doubles = false);
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
        bool isLValue(const ast::Expr& expr) const;
        std::optional<LValue> lvalue(const ast::Expr& expr, SourceLocation location, bool address = false);
        std::optional<LValue> element(SourceLocation location, Value pointer, std::optional<Value> index,
                                      bool address = false);
        bool writable(SourceLocation location, const LValue& target);
        Value read(const LValue& source, std::string named = {});
        Value readLValue(const ast::Expr& expr);
        llvm::Value* write(SourceLocation location, const LValue& target, Value value, Use use);
        Value lowerAddressOf(SourceLocation location, const ast::Expr& operand);
        Value pointerArithmetic(SourceLocation location, ast::BinaryOp op, Value lhs, Value rhs);
        void zero(llvm::Value* address, ast::Type type);
        Placed placeList(ast::Type type, const ast::DeclStmt& decl, bool report);
        llvm::Value* elementAt(ast::Type array, llvm::Value* address, std::uint64_t at, const char* name);
        void initializeArray(SourceLocation location, llvm::Value* address, ast::Type type, const ast::DeclStmt& decl);
        llvm::Value* declareConstArray(const ast::DeclStmt& decl, ast::Type type);
        llvm::Constant* tableContents(llvm::ArrayType* type, std::vector<llvm::Constant*> given);
        llvm::Constant* tableOf(ast::Type type, const std::vector<std::pair<std::uint64_t, llvm::Constant*>>& given);

        // ---- Calls.cpp ----------------------------------------------------------------------------------------------
        Value lowerCall(SourceLocation location, const ast::CallExpr& call);
        Value lowerReduction(SourceLocation location, const ast::CallExpr& call, const std::vector<Value>& args);
        Value lowerPolyMemory(SourceLocation location, const ast::CallExpr& call, const std::vector<Value>& args);
        Value wrongArity(SourceLocation location, std::size_t have, std::size_t expected);
        Value lowerAllocation(SourceLocation location, const Allocation& allocation, const std::vector<Value>& args);
        Value lowerFft(SourceLocation location, const Fft& fft, const std::vector<Value>& args);
        Value lowerRuntimeBuiltin(SourceLocation location, const RuntimeBuiltin& builtin, const ast::CallExpr& call);
        llvm::Value* formatInts(const std::vector<llvm::Value*>& values);
        llvm::CallInst* callRuntime(const std::string& name, llvm::Type* result, const std::vector<llvm::Value*>& args);

        // ---- Strings.cpp --------------------------------------------------------------------------------------------
        static bool isString(const ast::Expr& expr);
        llvm::Value* stringConstant(const ast::StringExpr& literal);
        std::optional<std::vector<llvm::Value*>> formatArguments(const ast::StringExpr& literal,
                                                                 const std::vector<const ast::Expr*>& ints);
        Value refuseString(const ast::Expr& expr, std::optional<ast::Type> type, Use use, SourceLocation where);
        Value notAValue(const ast::Expr& expr);

        // ---- Web.cpp ------------------------------------------------------------------------------------------------
        bool checkUrl(const ast::StringExpr& literal, SourceLocation where);
        bool checkContentType(const ast::StringExpr& literal, SourceLocation where);
        void recordWebRequest(SourceLocation location, std::string_view method, const ast::StringExpr& literal);
        llvm::Value* webContext(SourceLocation location);
        const Binding* webStatus(const std::string& name) const;
        bool unbuiltWebStatus(SourceLocation location, const std::string& name);

        // ---- WebServer.cpp ------------------------------------------------------------------------------------------
        bool checkRoute(const ast::StringExpr& literal, SourceLocation where);
        bool checkQueryName(const ast::StringExpr& literal, SourceLocation where);
        std::optional<std::vector<llvm::Value*>> replyArguments(const ast::StringExpr& literal,
                                                                const std::vector<const ast::Expr*>& values);
        llvm::Value* replyValues(const std::vector<llvm::Value*>& values);
        void recordWebRoute(SourceLocation location, const ast::StringExpr& literal);
        std::vector<llvm::Value*> listenArguments(SourceLocation location);
        void finishWebRoutes();

        // ---- Paths.cpp ----------------------------------------------------------------------------------------------
        llvm::Value* preparedPath(const ast::StringExpr& literal);
        llvm::Value* preparedXmlPath(const ast::StringExpr& literal, SourceLocation where);
        llvm::Value* preparedChoices(const std::vector<const ast::StringExpr*>& literals);

        // ---- Math.cpp -----------------------------------------------------------------------------------------------
        llvm::Value* lowerMath(const Builtin& builtin, ast::Type type, llvm::Value* x);
        static std::string mathName(const Builtin& builtin, MathFunction which);
        bool checkMathNames(SourceLocation location, const Builtin& builtin);
        llvm::FunctionCallee mathFunction(const Builtin& builtin, MathFunction which);

        // ---- Hash.cpp -----------------------------------------------------------------------------------------------
        llvm::Value* mixBits(llvm::Value* x);
        Value lowerHash(SourceLocation location, const ast::CallExpr& call, const std::vector<Value>& args);

        // ---- Modules.cpp --------------------------------------------------------------------------------------------
        void prepareModules(const ast::Program& program);
        void layOut(const ast::Module& module, std::size_t& enums);
        ast::Type fieldType(const ast::Field& field) const;
        void checkParameter(const ast::Module& module, const ast::Field& field);
        void checkAttributes(const ast::Field& field, ast::Type type, bool refused);
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

        // ---- Bounds.cpp ---------------------------------------------------------------------------------------------
        const Extent* arrayExtent(ast::Type array, std::string name);
        const Extent* movedExtent(const Extent& extent, llvm::Value* index);
        llvm::Value* elementOf(const Extent& extent, llvm::Value* index);
        void checkBounds(SourceLocation location, const Extent& extent, llvm::Value* index, unsigned width);
        void checkLanes(SourceLocation location, const Extent& extent, llvm::Value* mask);
        void failBounds(SourceLocation location, const Extent& extent, unsigned width, llvm::Value* inBounds,
                        llvm::Value* index);
        static std::string spelled(const ast::Expr& expr);

        // ---- the state of one lowering ----------------------------------------------------------------------------
        llvm::Module& module_;
        Diagnostics& diagnostics_;
        llvm::IRBuilder<DenormalFolder> builder_;                          // folds constants as the mode computes
        unsigned polyLanes_;                                                // a poly float's lanes
        Math math_;                                                         // how sinf, cosf, ... compute
        std::unordered_map<std::string, const ast::Function*> functions_;   // the program's, by name
        std::vector<std::unordered_map<std::string, Binding>> scopes_;     // the current function's, inner last
        std::unordered_map<std::string, Binding> fileScope_;   // the enumeration constants and constants so far
        // Each enumeration's constants declared so far, by its id (Type::enumeration): value and name, in order.
        std::unordered_map<int, std::vector<std::pair<std::int32_t, std::string>>> enumerations_;
        const ast::Program* program_ = nullptr;                            // the program being lowered
        const ast::Function* function_ = nullptr;                          // the function being lowered
        const ast::Function topLevel_{};   // what function_ is while a top-level constant's initialiser is lowered
        std::vector<Breakable> breakables_;                                // the loops and switches around
        std::vector<Switch> switches_;                                     // the switches around, inner last
        std::unordered_map<std::string, Effect> effects_;                  // each name's, from all its declarations
        std::unordered_map<std::string, std::vector<CallSite>> calls_;     // each definition's calls, in order
        std::unordered_map<const ast::Module*, llvm::StructType*> structs_;   // each module's layout
        std::unordered_map<const ast::Field*, ast::Type> fieldTypes_;         // each field's, its array's size known
        std::map<std::string, ParamBinding> bindings_;                     // the host's, by qualified name
        std::map<std::pair<unsigned, bool>, llvm::Value*> entryLoads_;     // the method's parameters, read at entry
        std::vector<llvm::Instruction*> streamReads_;                      // its loads of its streams' samples
        bool strictAliasing_;                                              // whether loads and stores carry !tbaa
        std::map<unsigned, StreamRead> flatStreams_;                       // what its streams' reads use
        Denormals denormals_;                                              // subnormals preserved or flushed
        std::string targetTriple_;                                         // the mode register, whether libm sets errno
        unsigned threads_;                                                 // the most threads a parallel region uses
        std::vector<Region> regions_;                                      // the parallel regions being lowered
        std::vector<llvm::Function*> outlined_;                            // every region's function
        std::unordered_map<std::string, unsigned> regionCount_;            // each function's regions so far
        std::optional<std::unordered_map<std::string, bool>> changes_;     // whether each method changes its module
        bool boundsCheck_;                                                 // whether accesses are checked (Bounds.cpp)
        // Whether the arithmetic the next lower() is part of has a double, as the arithmetic around it says: its floating
        // constants are then doubles (Expressions.cpp, lowerAs). None at the top of an arithmetic.
        std::optional<bool> doubleArithmetic_;
        std::deque<Extent> extents_;                                       // the arrays' extents values carry
        bool webContextClash_ = false;   // a function of the web context's name was reported (Web.cpp)
        // The route literals of the program's http_is calls, each once, in the order they were lowered, and the calls
        // of http_listen, which hand the library their table, made once all are known (WebServer.cpp).
        std::vector<std::string> webRoutes_;
        std::vector<llvm::CallInst*> webListens_;
    };
} // namespace fminus::ir::detail
