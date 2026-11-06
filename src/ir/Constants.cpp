// src/ir/Constants.cpp: C's integer constant expressions (C11 6.6p6), worked out where they are written, as a C
// compiler works them out: a case label's value, an enumerator's, an array's size (sized). Their operands are integer
// constants, enumeration constants, `true` and `false`, sizeof, and floating constants only as the immediate operands
// of casts (`(int)2.5f`), combined by the unary operators but * and &, the binary ones, &&, || and ?:, and casts to int
// or bool; their type is int or bool. A variable is none, const or not, as in C17 (isConstant, Statements.cpp, says the
// same of a loop's condition): a constant of the top level too, so a size is named by an enumeration constant, C's
// idiom, `enum { ROWS = 17 };`.
//
// Enumerations (declareEnum) are C's: each constant an int, named from just after its own definition on, in the scope
// the enumeration is declared in. In a block, that is the block's scope, where variables are too: C gives the two one
// name space, so a variable of an inner block hides a constant, and one of the same scope is a redefinition. At the
// top level, and in a module (as a C struct's), it is the file's, fileScope_, which the functions and modules after
// the enumeration see, and a module's methods (CodeGen.cpp declares each in its place among the definitions). A name is
// looked up as C++ looks it up in a member function (named(), Functions.cpp): the blocks' scopes, then the module's
// fields and methods, then the file's.
//
// The top level's constants (defineConstant) are C's file-scope const objects, in the file's scope too: `const float
// table[ROWS * COLS] = {...};`, each an LLVM constant global of its name, private to the program, stored once for every
// function and method declared after it.
//
// Each operator computes as FMinus computes it as the program runs (Expressions.cpp), so a constant means what the same
// expression means anywhere: a shift by its count's low five bits, / and % truncating toward zero. Where C makes the
// value an error, it is one: an int that overflows (6.6p4: a constant expression's value is in its type's range;
// clang warns, "overflow in expression"), and a division by zero or the least int by -1, after which the expression is
// no constant at all, as clang finds. Those two are errors only where the expression is evaluated: `0 && 1 / 0` is the
// constant 0, as in C (6.6p3), and so is the arm of a ?: not taken. What is no constant is reported wherever it is.
#include "ir/Lowering.h"

#include <llvm/ADT/APFloat.h>
#include <llvm/ADT/APSInt.h>
#include <llvm/IR/Constants.h>
#include <llvm/IR/GlobalVariable.h>

#include <algorithm>
#include <cstdint>
#include <iterator>
#include <limits>
#include <string_view>
#include <utility>

namespace fminus::ir::detail
{
    namespace
    {
        constexpr std::int64_t kLeast = std::numeric_limits<std::int32_t>::min();
        constexpr std::int64_t kGreatest = std::numeric_limits<std::int32_t>::max();

        // An int's 32 bits, as two's complement wraps `value`.
        std::int32_t wrapped(std::int64_t value)
        {
            return static_cast<std::int32_t>(static_cast<std::uint32_t>(static_cast<std::uint64_t>(value)));
        }

        // The runtime's functions (runtime/RuntimeSymbols.def), which the code generator declares by name when a
        // builtin calls one (fminus_rt_malloc for malloc, fminus_rt_sinf for sinf in the vector math mode).
        constexpr std::string_view kRuntimeNames[] = {
#define FMINUS_RUNTIME_SYMBOL(name) #name,
#include "runtime/RuntimeSymbols.def"
#undef FMINUS_RUNTIME_SYMBOL
        };

        // Whether `name` is a builtin's (Calls.cpp, Hash.cpp): <math.h>'s and <stdlib.h>'s functions are in C's file
        // scope, so clang refuses a variable of the name there.
        bool isBuiltinName(std::string_view name)
        {
            return findBuiltin(name) != nullptr || findAllocation(name) != nullptr || findFft(name) != nullptr
                   || isReduction(name) || isPolyQuery(name) || isPolyMemory(name) || isHash(name);
        }

        // Whether `constant` is poison or undef, or holds one: what LLVM folds an operation C leaves undefined to, a
        // division by zero, the least int divided by -1, a float converted to an int out of its range.
        bool undefined(const llvm::Constant& constant)
        {
            if (llvm::isa<llvm::UndefValue>(constant))
                return true;
            if (llvm::isa<llvm::GlobalValue>(constant))   // an address, whatever the global holds
                return false;
            return std::ranges::any_of(constant.operands(), [](const llvm::Use& operand) {
                return undefined(*llvm::cast<llvm::Constant>(operand.get()));
            });
        }
    } // namespace

    // The value of the enumeration constant `name` names where it is used, or nothing for a name that is not one: a
    // block's constant, unless a variable of an inner block hides it; then, in a method, nothing if the module has a
    // field or a method of the name; then the file's (named()).
    std::optional<std::int32_t> Lowering::enumerator(const std::string& name) const
    {
        const Binding* binding = named(name);
        return binding != nullptr ? binding->enumerator : std::nullopt;
    }

    // An enumeration, its constants in order: each one's value its own, or the one before it plus 1 (the first's 0),
    // bound from there on in the innermost scope, or the file's at the top level and in a module (scopes_ is empty
    // then), and kept by its id for a switch over its type (enumerations_, Statements.cpp's unhandled()). A value in
    // error makes its constant the next one's after all, as clang counts it, so the rest still count and its uses
    // report nothing more. Clang's errors: a constant redefined, one past the greatest int (or out of C23's fixed
    // underlying type, `enum Mode : bool`), and, at the top level, one named as a builtin (C's file scope holds
    // <math.h>'s functions, and clang refuses `enum { sinf };` after its #include: "redefinition of 'sinf'", as for a
    // function of the file), a function or a module. In a block, a constant of a builtin's name is the block's, as a
    // variable of one is.
    void Lowering::declareEnum(const ast::Enum& enumeration)
    {
        const bool file = scopes_.empty();
        std::unordered_map<std::string, Binding>& scope = file ? fileScope_ : scopes_.back();
        // C23's fixed underlying type bounds the values: bool's 0 and 1, int's an int. Without one, C restricts them to
        // an int's range too, in other words.
        const bool isBool = enumeration.fixed && enumeration.underlying == ast::Type::Bool;
        const std::string underlying = quoted(ast::typeName(enumeration.underlying));
        std::vector<std::pair<std::int32_t, std::string>>& constants = enumerations_[enumeration.id];
        constants.clear();
        std::int64_t next = 0;
        for (const ast::Enumerator& constant : enumeration.enumerators)
        {
            std::optional<std::int32_t> value = constant.value ? integerConstant(*constant.value) : std::nullopt;
            if (!constant.value && next > (isBool ? 1 : kGreatest))
                error(constant.location, enumeration.fixed
                                             ? "enumerator value " + std::to_string(next)
                                                   + " is not representable in the underlying type " + underlying
                                             : "ISO C restricts enumerator values to range of 'int' ("
                                                   + std::to_string(next) + " is too large)");
            else if (value && isBool && *value != 0 && *value != 1)
                error(constant.location, "enumerator value is not representable in the underlying type 'bool'");
            const std::int32_t bound = value ? *value : wrapped(next);
            next = static_cast<std::int64_t>(bound) + 1;
            const auto [previous, added] =
                scope.try_emplace(constant.name, Binding{nullptr, true, ast::Type::Int, constant.location, bound});
            if (!added)
            {
                redefinition(constant.location, constant.name, true, previous->second);
                continue;
            }
            constants.emplace_back(bound, constant.name);
            if (file && isBuiltinName(constant.name))   // C's file scope has <math.h>'s functions: clang's words
                error(constant.location, "redefinition of " + quoted(constant.name));
            else if (file)
                fileNameClash(constant.location, constant.name, enumeration.position, true);
        }
    }

    // `name`, declared in the file's scope at `location` (an enumeration constant when `enumerator`, else a constant),
    // after `position` of the program's functions: C's file scope holds the functions' names too, and a module's name
    // is a type's (a typedef's, to C), so a function or a module of the name is a redefinition, reported at the later
    // of the two. Clang calls it one "as different kind of symbol", except for an enumeration constant after a
    // function, which is a plain "redefinition of 'f'".
    void Lowering::fileNameClash(SourceLocation location, const std::string& name, std::size_t position,
                                 bool enumerator)
    {
        const std::string message = "redefinition of " + quoted(name) + " as different kind of symbol";
        const auto previous = [](SourceLocation where) {
            return std::vector<Diagnostic>{Diagnostic{where, "previous definition is here", {}}};
        };
        for (std::size_t f = 0; f < program_->functions.size(); ++f)
        {
            const ast::Function& function = program_->functions[f];
            if (function.name != name)
                continue;
            if (f < position)
                diagnostics_.error(location, enumerator ? "redefinition of " + quoted(name) : message,
                                   previous(function.location));
            else
                diagnostics_.error(function.location, message, previous(location));
            break;
        }
        for (const std::unique_ptr<ast::Module>& module : program_->modules)
        {
            if (module->name == name)   // declared after: before, its name is no identifier
                diagnostics_.error(module->location, message, previous(location));
        }
    }

    // The array `name` declares, `type` (an array, whose counts the tree leaves at 0), with its size worked out: an
    // integer constant expression from 1 to 2^30, or, for `[]`, as many elements as its list reaches (`listed`, the
    // declaration, when it has a list; placeList, Memory.cpp). A 2-D array's rows' size, `rowSize`, is one too, which
    // C does not let `[]` leave out; its list then reaches as many rows as it starts, and its elements, rows times
    // their size, are 2^30 at most. Clang's errors, and FMinus's where C would allow more: a size of 0 (a GNU
    // extension), one over 2^30, a size no constant (C's variable length array). After an error the array has as many
    // elements as its list reaches, or one without a list, so neither the list nor the array's uses report more. A
    // row whose size is in error, or missing, is as wide as the list's widest braced row (one element without one),
    // and the array has as many rows as written, or as its list reaches with them when that is more: the row's error
    // is the declaration's only one, as clang's type is invalid after it, and nothing measured against a row the array
    // does not have (a list's excess, the size a list gives) is reported.
    ast::Type Lowering::sized(SourceLocation location, const std::string& name, ast::Type type, const ast::Expr* size,
                              const ast::Expr* rowSize, const ast::DeclStmt* listed)
    {
        constexpr std::int64_t kMost = std::int64_t{1} << 30;
        const bool rows = ast::hasRows(type);
        bool rowFailed = false;
        if (rows)
        {
            std::optional<std::int32_t> row;
            if (rowSize == nullptr)
                error(location,
                      "array has incomplete element type " + quoted(ast::typeName(ast::elementOf(type)) + "[]"));
            else
                row = arraySize(name, *rowSize);
            rowFailed = !row;
            type.rowCount = row.value_or(1);
            if (rowFailed && listed != nullptr)
            {
                for (const ast::Brace& brace : listed->braces)
                {
                    if (brace.depth == 1)   // a row's, `{1, 2}`
                        type.rowCount = static_cast<int>(
                            std::clamp<std::uint64_t>(brace.count, static_cast<std::uint64_t>(type.rowCount), kMost));
                }
            }
        }
        const std::uint64_t width = rows ? static_cast<std::uint64_t>(type.rowCount) : 1;   // elements an entry has
        // How many entries (elements, or rows) the list reaches, placed as C places it (its errors are reported
        // where it is lowered); 0 without one.
        std::uint64_t reach = 0;
        bool placed = true;
        if (listed != nullptr)
        {
            ast::Type unbounded = type;
            unbounded.count = 0;
            const Placed list = placeList(unbounded, *listed, false);
            reach = (list.reach + width - 1) / width;
            placed = list.ok;
        }
        const bool fits = reach > 0 && reach * width <= static_cast<std::uint64_t>(kMost);
        type.count = fits ? static_cast<int>(reach) : 1;
        if (rowFailed)
        {
            if (const std::optional<std::int32_t> count = size != nullptr ? arraySize(name, *size) : std::nullopt)
                type.count = static_cast<int>(std::min(std::max(static_cast<std::uint64_t>(*count), fits ? reach : 0),
                                                       static_cast<std::uint64_t>(kMost) / width));
            return type;
        }
        if (size == nullptr)
        {
            if (listed == nullptr)
                error(location, "definition of variable with array type needs an explicit size or an initializer");
            else if (reach == 0 && placed)
                error(location, "zero size arrays are not allowed");
            else if (!fits && placed)
                error(location, "array is too large: FMinus's arrays have at most 2^30 elements");
            return type;
        }
        const std::optional<std::int32_t> count = arraySize(name, *size);
        if (!count)
            return type;
        if (static_cast<std::uint64_t>(*count) * width > static_cast<std::uint64_t>(kMost))
            error(size->location, "array is too large: FMinus's arrays have at most 2^30 elements, and this one has "
                                      + std::to_string(static_cast<std::uint64_t>(*count) * width));
        else
            type.count = *count;
        return type;
    }

    // The array a declaration declares, `type` (its tree's, or the stand-in after an error), sized: by its sizes, or
    // its list (sized). One initialised with `= expr` and no size has one element (and a row of one), and the error
    // its initialiser reports.
    ast::Type Lowering::declaredArray(SourceLocation location, const ast::DeclStmt& decl, ast::Type type)
    {
        if (decl.size == nullptr && decl.init)
        {
            type.count = 1;
            if (ast::hasRows(type))
                type.rowCount = 1;
            return type;
        }
        return sized(location, decl.name, type, decl.size.get(), decl.rowSize.get(), decl.hasList ? &decl : nullptr);
    }

    // An array of `array`'s shape whose elements are floats: what a declaration whose elements' type was refused
    // declares instead, so that its uses report no more errors.
    ast::Type Lowering::floatsLike(ast::Type array)
    {
        if (ast::hasRows(array))
            return ast::Type::arrayOf(ast::Type::arrayOf(ast::Type::Float, array.rowCount), array.count);
        return ast::Type::arrayOf(ast::Type::Float, array.count);
    }

    // An array's size as written, `size`, of the array `name` (none for a type's, sizeof(float[N])): an integer
    // constant expression from 1 to 2^30, or nothing after its error.
    std::optional<std::int32_t> Lowering::arraySize(const std::string& name, const ast::Expr& size)
    {
        constexpr std::int64_t kMost = std::int64_t{1} << 30;
        const std::optional<Folded> folded = fold(size, true);
        if (!folded)
            return std::nullopt;
        if (ast::isFloating(folded->type))
            error(size.location, "size of array has non-integer type " + quoted(ast::typeName(folded->type)));
        else if (folded->floating)   // an int a floating constant took part in, as (1.5f < 2)
            notConstant(*folded->floating);
        else if (folded->value < 0)
            error(size.location, name.empty() ? std::string("array size is negative")
                                              : quoted(name) + " declared as an array with a negative size");
        else if (folded->value == 0)
            error(size.location, "zero size arrays are not allowed");
        else if (folded->value > kMost)
            error(size.location, "array is too large: FMinus's arrays have at most 2^30 elements");
        else
            return folded->value;
        return std::nullopt;
    }

    // A function's parameters declared as arrays, `float x[N]`: pointers, as in C, whose sizes C still checks, as the
    // file's scope has it where the function is (CodeGen.cpp: a method's module's enumerations too), with the
    // parameters before each one, which are no constants (C's variable length array, which FMinus has not).
    void Lowering::checkSizes(const ast::Function& function)
    {
        scopes_.emplace_back();
        for (const ast::Param& param : function.params)
        {
            if (param.size != nullptr)
                sized(param.location, param.name, ast::Type::arrayOf(ast::pointee(param.type), 0), param.size.get(),
                      nullptr, nullptr);
            if (!param.name.empty())
                scopes_.back().try_emplace(param.name, Binding{nullptr, param.isConst, param.type, param.location});
        }
        scopes_.clear();
    }

    // Whether the expression being lowered is a top-level constant's initialiser (defineConstant), which runs nowhere:
    // a ?: or a && or || whose condition is a constant is then the value it picks (Expressions.cpp).
    bool Lowering::atTopLevel() const
    {
        return function_ == &topLevel_;
    }

    // A constant of the top level (Program::constants), C's file-scope const object: `const float table[N] = {...};`
    // or `const float gain = 0.5f;`, an LLVM constant global of its name, which the functions and methods after it read
    // where it is (a table, so no call copies it: Memory.cpp's read()) or as its value (a scalar, Expressions.cpp). An
    // array takes its size from its list when it has none, and its missing elements are 0, in a table's shapes
    // (tableContents, Memory.cpp); a scalar without an initialiser is 0.
    //
    // The global is private, as a C file's `static const` object is internal: only the program's own code reads it.
    // A symbol of its name could take what the generated code calls by that name, a library's function (a float %
    // calls fmodf, and expf in the fast math mode calls expf) or the runtime's, and a host would find it as one of the
    // program's functions. So a constant is never named as a builtin (C's file scope has <math.h>'s functions in it,
    // and clang refuses the name: "redefinition of 'sinf' as different kind of symbol"), nor as one of the runtime's
    // functions (FMinus's error), which the code generator looks up by name in the module.
    //
    // The initialiser is lowered as a function's would be, into a function of no use (deleted after, with whatever
    // declarations only it used), so that it means what it would in a function, bit for bit: the same conversions,
    // and the same folding, flushed subnormals too. Every element must then be a constant: numbers, enumeration
    // constants, sizeof, a constant scalar of the top level, the address of a table's element, hash and randf of them,
    // and operators and casts on those, and ?:, && and || (atTopLevel). What the program would compute as it runs (a
    // call, a table's element, a heap block) is clang's error, "initializer element is not a compile-time constant",
    // and so is what C leaves undefined (1 / 0, (int)1e10f), which LLVM folds to poison. FMinus's: a top-level
    // declaration is const (FMinus has no global variables: a program's state is in its modules), of no module, and of
    // kMaxTableBytes at most.
    void Lowering::defineConstant(const ast::Constant& constant)
    {
        const ast::DeclStmt& decl = constant.declaration;
        const SourceLocation location = constant.location;
        if (!decl.isConst)
            error(location, "FMinus has no variables at the top level: declare " + quoted(decl.name)
                                + " const, a constant of the program, or keep it in a module");
        ast::Type type = decl.type;
        if (type == ast::Type::Void)
        {
            error(location, "variable has incomplete type 'void'");
            type = ast::Type::Float;   // so its uses report no more errors
        }
        if (ast::isModule(type) || (ast::isArray(type) && ast::isModule(ast::elementOf(type))))
        {
            error(location, "a module is no constant of the top level: its fields start at 0, and only its methods set "
                            "them");
            type = ast::isArray(type) ? floatsLike(type) : ast::Type::Float;
        }
        if (ast::isArray(type))
            type = declaredArray(location, decl, type);
        if (!checkType(location, type))
            type = ast::isArray(type) ? floatsLike(type) : ast::Type::pointerTo(ast::Kind::Float, type.pointeeConst);
        if (const std::uint64_t bytes =
                ast::isArray(type) ? module_.getDataLayout().getTypeAllocSize(typeOf(type)).getFixedValue() : 0;
            bytes > kMaxTableBytes)
        {
            error(location, "array is too large for a constant: FMinus's constants of the top level hold at most "
                            "1 MiB, and this one holds " + std::to_string(bytes) + " bytes");
            // As many elements (or rows) as its list reaches, so that the list reports no more.
            const std::uint64_t width = ast::hasRows(type) ? static_cast<std::uint64_t>(type.rowCount) : 1;
            ast::Type unbounded = type;
            unbounded.count = 0;
            const std::uint64_t reach = (placeList(unbounded, decl, false).reach + width - 1) / width;
            type.count = static_cast<int>(std::clamp<std::uint64_t>(reach, 1, std::uint64_t{1} << 30));
        }
        const ast::Type element = ast::isArray(type) ? ast::elementOf(type) : type;

        // The initialiser's values, lowered and converted where a function's would be.
        llvm::Function* last = module_.empty() ? nullptr : &module_.getFunctionList().back();
        llvm::Function* scratch =
            llvm::Function::Create(llvm::FunctionType::get(builder_.getVoidTy(), false),
                                   llvm::GlobalValue::PrivateLinkage, "fminus.top-level", module_);
        builder_.SetInsertPoint(llvm::BasicBlock::Create(module_.getContext(), "entry", scratch));
        function_ = &topLevel_;
        const std::size_t reported = diagnostics_.errors().size();
        std::vector<std::pair<llvm::Value*, SourceLocation>> values;
        std::vector<std::uint64_t> places;   // an array's: each value's element, from the first (placeList)
        if (ast::isArray(type) && decl.init)
            error(location, "array initializer must be an initializer list");
        else if (ast::isArray(type))
        {
            const Placed placed = placeList(type, decl, true);
            for (const auto& [at, item] : placed.values)
            {
                values.emplace_back(convert(lowerAs(*item, element), element, item->location, Use::Initializing),
                                    item->location);
                places.push_back(at);
            }
        }
        else if (decl.hasList)
            error(location, "FMinus takes an initializer list for an array only");
        else if (decl.init)
            values.emplace_back(convert(lowerAs(*decl.init, type), type, location, Use::Initializing),
                                decl.init->location);
        std::vector<llvm::Constant*> constants;
        for (const auto& [value, where] : values)
        {
            auto* folded = llvm::dyn_cast<llvm::Constant>(value);
            if (folded != nullptr && undefined(*folded))
                folded = nullptr;
            if (folded == nullptr && diagnostics_.errors().size() == reported)   // the first, as clang reports it
                error(where, "initializer element is not a compile-time constant");
            constants.push_back(folded != nullptr ? folded : llvm::Constant::getNullValue(typeOf(element)));
        }
        function_ = nullptr;
        calls_.erase(topLevel_.name);   // what a call in it recorded for Effects.cpp
        builder_.ClearInsertionPoint();
        scratch->eraseFromParent();
        for (auto next = last != nullptr ? std::next(last->getIterator()) : module_.begin(); next != module_.end();)
        {
            llvm::Function& declared = *next++;   // an intrinsic, say, of an arm a constant condition did not take
            if (declared.isDeclaration() && declared.use_empty())
                declared.eraseFromParent();
        }

        llvm::Constant* contents = nullptr;
        if (ast::isArray(type))
        {
            std::vector<std::pair<std::uint64_t, llvm::Constant*>> given;
            for (std::size_t i = 0; i < constants.size(); ++i)
                given.emplace_back(places[i], constants[i]);
            contents = tableOf(type, given);
        }
        else
            contents = constants.empty() ? llvm::Constant::getNullValue(typeOf(type)) : constants.front();
        const bool builtin = isBuiltinName(decl.name);
        const bool runtime = std::ranges::find(kRuntimeNames, decl.name) != std::end(kRuntimeNames);
        if (builtin)
            error(location, "redefinition of " + quoted(decl.name) + " as different kind of symbol");
        else if (runtime)
            error(location, quoted(decl.name) + " is the name of a function of FMinus's runtime, which the code it "
                                                "generates calls: rename the constant");
        // Named after an error, the global would be what the runtime's or a builtin's declaration finds.
        auto* global = new llvm::GlobalVariable(module_, contents->getType(), true, llvm::GlobalValue::PrivateLinkage,
                                                contents, builtin || runtime ? "" : decl.name);
        global->setAlignment(alignOf(type));
        const auto [previous, added] = fileScope_.try_emplace(decl.name, Binding{global, true, type, location});
        if (!added)
            redefinition(location, decl.name, false, previous->second);
        else
            fileNameClash(location, decl.name, constant.position, false);
    }

    // The value of the integer constant expression `expr`, or nothing after an error that says why, in clang's words.
    std::optional<std::int32_t> Lowering::integerConstant(const ast::Expr& expr)
    {
        const std::optional<Folded> folded = fold(expr, true);
        if (!folded)
            return std::nullopt;
        if (ast::isFloating(folded->type))
        {
            error(expr.location, "integer constant expression must have integer type, not "
                                     + quoted(ast::typeName(folded->type)));
            return std::nullopt;
        }
        if (folded->floating)   // an int that a floating constant took part in, as 1.5f < 2 or !1.5f
        {
            notConstant(*folded->floating);
            return std::nullopt;
        }
        return folded->value;
    }

    // "expression is not an integer constant expression", at `location`: what a constant expression cannot hold.
    std::optional<Lowering::Folded> Lowering::notConstant(SourceLocation location)
    {
        error(location, "expression is not an integer constant expression");
        return std::nullopt;
    }

    // `expr`'s value as a constant expression, or nothing after its error; `evaluated` is false in the operand of &&,
    // || or ?: that C does not evaluate, where an overflow or a division by zero is no error.
    std::optional<Lowering::Folded> Lowering::fold(const ast::Expr& expr, bool evaluated)
    {
        // An int operator's exact result, `wide`: the int, or an error when it is out of an int's range.
        const auto checked = [&](SourceLocation location, std::int64_t wide) -> std::optional<Folded> {
            if (evaluated && (wide < kLeast || wide > kGreatest))
            {
                error(location, "overflow in expression; result is " + std::to_string(wrapped(wide))
                                    + " with type 'int'");
                return std::nullopt;
            }
            return Folded{wrapped(wide), ast::Type::Int, std::nullopt};
        };
        // A constant that lowers to no instruction (a number, sizeof): its value, or nothing after its own error.
        const auto lowered = [&](auto lowerIt) -> std::optional<Value> {
            const std::size_t reported = diagnostics_.errors().size();
            const Value value = lowerIt();
            if (diagnostics_.errors().size() != reported)
                return std::nullopt;
            return value;
        };
        return std::visit(
            Overloaded{
                [&](const ast::NumberExpr& number) -> std::optional<Folded> {
                    const std::optional<Value> value = lowered([&] { return lowerNumber(expr.location, number); });
                    if (!value)
                        return std::nullopt;
                    if (ast::isFloating(value->type))
                        return Folded{0, value->type, expr.location};
                    return Folded{static_cast<std::int32_t>(llvm::cast<llvm::ConstantInt>(value->ir)->getSExtValue()),
                                  ast::Type::Int, std::nullopt};
                },
                [&](const ast::BoolExpr& constant) -> std::optional<Folded> {
                    return Folded{constant.value ? 1 : 0, ast::Type::Bool, std::nullopt};
                },
                [&](const ast::SizeofExpr& size) -> std::optional<Folded> {
                    const std::optional<Value> value = lowered([&] { return lowerSizeof(expr.location, size); });
                    if (!value)
                        return std::nullopt;
                    return Folded{static_cast<std::int32_t>(llvm::cast<llvm::ConstantInt>(value->ir)->getSExtValue()),
                                  ast::Type::Int, std::nullopt};
                },
                [&](const ast::VariableExpr& variable) -> std::optional<Folded> {
                    // An enumeration constant is one; a variable, a parameter, a field (in a method) or a constant of
                    // the top level is none (a const object, in C17); any other name is undeclared.
                    if (const std::optional<std::int32_t> value = enumerator(variable.name))
                        return Folded{*value, ast::Type::Int, std::nullopt};
                    const ast::Module* module = function_ != nullptr ? function_->module : nullptr;
                    if (named(variable.name) != nullptr
                        || (module != nullptr
                            && (findField(*module, variable.name, nullptr) != nullptr
                                || std::ranges::find(module->methods, variable.name) != module->methods.end())))
                        return notConstant(expr.location);
                    error(expr.location, "use of undeclared identifier " + quoted(variable.name));
                    return std::nullopt;
                },
                [&](const ast::UnaryExpr& unary) -> std::optional<Folded> {
                    if (unary.op == ast::UnaryOp::Dereference || unary.op == ast::UnaryOp::AddressOf)
                        return notConstant(expr.location);
                    const std::optional<Folded> operand = fold(*unary.operand, evaluated);
                    if (!operand)
                        return std::nullopt;
                    if (ast::isFloating(operand->type) && unary.op == ast::UnaryOp::Complement)
                    {
                        error(expr.location, "invalid argument type " + quoted(ast::typeName(operand->type))
                                                 + " to unary expression");
                        return std::nullopt;
                    }
                    const ast::Type type = unary.op == ast::UnaryOp::Not       ? ast::Type::Bool
                                           : ast::isFloating(operand->type) ? operand->type
                                                                            : ast::Type::Int;
                    if (operand->floating)
                        return Folded{0, type, operand->floating};
                    const std::int64_t x = operand->value;   // a bool's is 0 or 1, an int's own
                    switch (unary.op)
                    {
                    case ast::UnaryOp::Negate:
                        return checked(expr.location, -x);
                    case ast::UnaryOp::Plus:
                        return Folded{wrapped(x), type, std::nullopt};
                    case ast::UnaryOp::Not:
                        return Folded{x == 0 ? 1 : 0, type, std::nullopt};
                    case ast::UnaryOp::Complement:
                        return Folded{~wrapped(x), type, std::nullopt};
                    case ast::UnaryOp::Dereference:   // above
                    case ast::UnaryOp::AddressOf:
                        break;
                    }
                    return std::nullopt;
                },
                [&](const ast::BinaryExpr& binary) -> std::optional<Folded> {
                    const std::optional<Folded> lhs = fold(*binary.lhs, evaluated);
                    const std::optional<Folded> rhs = fold(*binary.rhs, evaluated);
                    if (!lhs || !rhs)
                        return std::nullopt;
                    return foldBinary(expr.location, binary.op, *lhs, *rhs, evaluated);
                },
                [&](const ast::LogicalExpr& logical) -> std::optional<Folded> {
                    // C's: the right operand is not evaluated when the left decides, && when it is 0, || when not
                    const bool isAnd = logical.op == ast::LogicalOp::And;
                    const std::optional<Folded> lhs = fold(*logical.lhs, evaluated);
                    const bool decides = lhs && !lhs->floating && (lhs->value != 0) != isAnd;
                    const std::optional<Folded> rhs = fold(*logical.rhs, evaluated && lhs && !decides);
                    if (!lhs || !rhs)
                        return std::nullopt;
                    if (lhs->floating || rhs->floating)
                        return Folded{0, ast::Type::Bool, lhs->floating ? lhs->floating : rhs->floating};
                    if (decides)
                        return Folded{isAnd ? 0 : 1, ast::Type::Bool, std::nullopt};
                    return Folded{rhs->value != 0 ? 1 : 0, ast::Type::Bool, std::nullopt};
                },
                [&](const ast::ConditionalExpr& conditional) -> std::optional<Folded> {
                    // C's: only the arm the condition picks is evaluated
                    const std::optional<Folded> condition = fold(*conditional.condition, evaluated);
                    const bool known = condition && !condition->floating;
                    const bool truth = known && condition->value != 0;
                    const std::optional<Folded> thenValue = fold(*conditional.thenValue, evaluated && known && truth);
                    const std::optional<Folded> elseValue =
                        fold(*conditional.elseValue, evaluated && known && !truth);
                    if (!condition || !thenValue || !elseValue)
                        return std::nullopt;
                    // C's usual arithmetic conversions: two bools give a bool, a double anywhere a double, a float
                    // anywhere else a float
                    const ast::Type type =
                        thenValue->type == ast::Type::Double || elseValue->type == ast::Type::Double ? ast::Type::Double
                        : thenValue->type == ast::Type::Float || elseValue->type == ast::Type::Float ? ast::Type::Float
                        : thenValue->type == ast::Type::Bool && elseValue->type == ast::Type::Bool   ? ast::Type::Bool
                                                                                                     : ast::Type::Int;
                    for (const std::optional<SourceLocation>& floating :
                         {condition->floating, thenValue->floating, elseValue->floating})
                    {
                        if (floating)
                            return Folded{0, type, floating};
                    }
                    return Folded{truth ? thenValue->value : elseValue->value, type, std::nullopt};
                },
                [&](const ast::CastExpr& cast) -> std::optional<Folded> {
                    if (ast::isFloating(cast.type))
                    {
                        const std::optional<Folded> operand = fold(*cast.operand, evaluated);
                        if (!operand)
                            return std::nullopt;
                        return Folded{0, cast.type, operand->floating ? operand->floating : expr.location};
                    }
                    // A floating constant as the cast's own operand is C's one place for one: (int) truncates toward
                    // zero, and a value out of an int's range makes no constant; (bool) is != 0.
                    if (const auto* number = std::get_if<ast::NumberExpr>(&cast.operand->node))
                    {
                        const std::optional<Value> value =
                            lowered([&] { return lowerNumber(cast.operand->location, *number); });
                        if (!value)
                            return std::nullopt;
                        if (ast::isFloating(value->type))
                        {
                            const llvm::APFloat& floating = llvm::cast<llvm::ConstantFP>(value->ir)->getValueAPF();
                            if (cast.type == ast::Type::Bool)
                                return Folded{floating.isZero() ? 0 : 1, ast::Type::Bool, std::nullopt};
                            llvm::APSInt integer(32, false);
                            bool exact = false;
                            if ((floating.convertToInteger(integer, llvm::APFloat::rmTowardZero, &exact)
                                 & llvm::APFloat::opInvalidOp)
                                != 0)
                                return notConstant(expr.location);
                            return Folded{static_cast<std::int32_t>(integer.getExtValue()), ast::Type::Int,
                                          std::nullopt};
                        }
                    }
                    const std::optional<Folded> operand = fold(*cast.operand, evaluated);
                    if (!operand)
                        return std::nullopt;
                    if (operand->floating)
                        return Folded{0, cast.type, operand->floating};
                    if (cast.type == ast::Type::Bool)
                        return Folded{operand->value != 0 ? 1 : 0, ast::Type::Bool, std::nullopt};
                    return Folded{operand->value, ast::Type::Int, std::nullopt};
                },
                [&](const ast::CallExpr&) { return notConstant(expr.location); },
                [&](const ast::AssignExpr&) { return notConstant(expr.location); },
                [&](const ast::IncrementExpr&) { return notConstant(expr.location); },
                [&](const ast::IndexExpr&) { return notConstant(expr.location); },
                [&](const ast::MemberExpr&) { return notConstant(expr.location); },
                [&](const ast::MethodCallExpr&) { return notConstant(expr.location); },
                [&](const ast::ThisExpr&) { return notConstant(expr.location); },
                [&](const ast::ConstructExpr&) { return notConstant(expr.location); },
            },
            expr.node);
    }

    // A binary operator on two constants, as arithmetic() computes it on ints. The bitwise operators take ints, as
    // binary()'s do; a float or a double anywhere else makes one (a double where either is), or a bool that is no
    // constant (a comparison).
    std::optional<Lowering::Folded> Lowering::foldBinary(SourceLocation location, ast::BinaryOp op, Folded lhs,
                                                         Folded rhs, bool evaluated)
    {
        const bool floats = ast::isFloating(lhs.type) || ast::isFloating(rhs.type);
        if (isBitwise(op) && floats)
        {
            error(location, "invalid operands to binary expression (" + quoted(ast::typeName(lhs.type)) + " and "
                                + quoted(ast::typeName(rhs.type)) + ")");
            return std::nullopt;
        }
        const ast::Type type = isComparison(op)                                                  ? ast::Type::Bool
                               : lhs.type == ast::Type::Double || rhs.type == ast::Type::Double ? ast::Type::Double
                               : floats                                                         ? ast::Type::Float
                                                                                                : ast::Type::Int;
        if (lhs.floating || rhs.floating)   // a Float or a Double always has one
            return Folded{0, type, lhs.floating ? lhs.floating : rhs.floating};
        const std::int64_t a = lhs.value;
        const std::int64_t b = rhs.value;
        const auto exact = [&](std::int64_t wide) -> std::optional<Folded> {
            if (evaluated && (wide < kLeast || wide > kGreatest))
            {
                error(location, "overflow in expression; result is " + std::to_string(wrapped(wide))
                                    + " with type 'int'");
                return std::nullopt;
            }
            return Folded{wrapped(wide), type, std::nullopt};
        };
        const auto truth = [&](bool value) { return Folded{value ? 1 : 0, type, std::nullopt}; };
        switch (op)
        {
        case ast::BinaryOp::Add:
            return exact(a + b);
        case ast::BinaryOp::Subtract:
            return exact(a - b);
        case ast::BinaryOp::Multiply:
            return exact(a * b);
        case ast::BinaryOp::Divide:
        case ast::BinaryOp::Remainder:
            if (b == 0 || (a == kLeast && b == -1))
            {
                if (!evaluated)
                    return Folded{0, type, std::nullopt};
                diagnostics_.error(location, "expression is not an integer constant expression",
                                   {Diagnostic{location,
                                               b == 0 ? "division by zero is undefined"
                                                      : "value 2147483648 is outside the range of representable "
                                                        "values of type 'int'",
                                               {}}});
                return std::nullopt;
            }
            return Folded{wrapped(op == ast::BinaryOp::Divide ? a / b : a % b), type, std::nullopt};
        case ast::BinaryOp::Less:
            return truth(a < b);
        case ast::BinaryOp::LessEqual:
            return truth(a <= b);
        case ast::BinaryOp::Greater:
            return truth(a > b);
        case ast::BinaryOp::GreaterEqual:
            return truth(a >= b);
        case ast::BinaryOp::Equal:
            return truth(a == b);
        case ast::BinaryOp::NotEqual:
            return truth(a != b);
        case ast::BinaryOp::BitAnd:
            return Folded{wrapped(a & b), type, std::nullopt};
        case ast::BinaryOp::BitOr:
            return Folded{wrapped(a | b), type, std::nullopt};
        case ast::BinaryOp::BitXor:
            return Folded{wrapped(a ^ b), type, std::nullopt};
        case ast::BinaryOp::ShiftLeft:   // FMinus's: by the count's low five bits, the bits shifted out lost
            return Folded{wrapped(static_cast<std::int64_t>(static_cast<std::uint32_t>(wrapped(a)) << (b & 31))), type,
                          std::nullopt};
        case ast::BinaryOp::ShiftRight:   // the sign shifted in
            return Folded{wrapped(a) >> (b & 31), type, std::nullopt};
        }
        return std::nullopt;
    }
} // namespace fminus::ir::detail
