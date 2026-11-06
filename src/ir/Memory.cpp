// src/ir/Memory.cpp: memory. lvalues, what a program can assign or take the address of: a variable, an element a
// pointer reaches (`p[i]`, `*p`), a module's field (`m.y`, `p->y`, or `y` in a method), or components of any of them
// (`v.x`, `p[i].zw`, `v.xy.x`). lvalue() finds one, read() and write() load and store it; reading a variable, an
// assignment, ++ and & all go through them, so a new kind of lvalue joins every one at once. Also C's arrays (an
// array's name stands for a pointer to its first element; a const one of constants is a constant of the module, stored
// once), and C's arithmetic on pointers, on addresses.
//
// An element's address is a getelementptr, as clang's: inbounds (it stays within the object the pointer points
// into, or one past its end, as C requires), on an index widened to 64 bits with sext, since the index is a signed
// int. Under Options::boundsCheck, an access through a pointer into an array the code generator can see is checked
// against the array's extent first (Bounds.cpp).
#include "ir/Lowering.h"

#include <llvm/IR/Constants.h>
#include <llvm/IR/GlobalVariable.h>

#include <algorithm>
#include <cstdint>
#include <limits>
#include <utility>

namespace fminus::ir::detail
{
    // Whether `expr` names something a program could store to; lvalue() then checks that it does. C's rule, which
    // the grammar leaves to the code generator: `(a ? b : c) = 1`, `f().x = 1` and an enumeration constant's `A = 1`
    // are not assignable.
    bool Lowering::isLValue(const ast::Expr& expr) const
    {
        if (const auto* variable = std::get_if<ast::VariableExpr>(&expr.node))
            return !enumerator(variable->name).has_value();
        if (std::holds_alternative<ast::IndexExpr>(expr.node))
            return true;
        if (const auto* unary = std::get_if<ast::UnaryExpr>(&expr.node))
            return unary->op == ast::UnaryOp::Dereference;
        if (const auto* member = std::get_if<ast::MemberExpr>(&expr.node))
            return member->arrow || isLValue(*member->base);   // p->y always is, as *p is
        return false;
    }

    // The lvalue `expr` names (isLValue said it names one), or nothing after an error. Errors about components are
    // reported at `location`, the operator that stores (clang's place for them). `address` is &'s: what is wanted of
    // p[i] or *p is where it is, a pointer formed, which may be one past an array's end (Bounds.cpp).
    std::optional<Lowering::LValue> Lowering::lvalue(const ast::Expr& expr, SourceLocation location, bool address)
    {
        if (const auto* variable = std::get_if<ast::VariableExpr>(&expr.node))
        {
            const Binding* binding = find(variable->name);
            // In a method, a name no local or parameter has may be one of the module's: `y` is this->y, as in C++.
            const ast::Module* module = function_ != nullptr ? function_->module : nullptr;
            if (binding == nullptr && module != nullptr
                && (findField(*module, variable->name, nullptr) != nullptr
                    || std::ranges::find(module->methods, variable->name) != module->methods.end()))
                return field(expr.location, self(), *module, variable->name, false);
            if (binding == nullptr)   // then the file's: a constant of the top level (Constants.cpp)
            {
                const auto found = fileScope_.find(variable->name);
                if (found != fileScope_.end() && !found->second.enumerator)
                    binding = &found->second;
            }
            if (binding == nullptr)
            {
                error(expr.location, "use of undeclared identifier " + quoted(variable->name));
                return std::nullopt;
            }
            return LValue{binding->slot, binding->type, binding->isConst, variable->name, binding->type, {}};
        }
        if (const auto* index = std::get_if<ast::IndexExpr>(&expr.node))
        {
            if (const std::optional<unsigned> field = parameterOf(*index->base))
            {
                error(expr.location,
                      "parameter " + quoted(function_->module->fields[*field].name)
                          + " is the host's: a method reads it, and cannot assign it or take its address");
                return std::nullopt;
            }
            Value base = lower(*index->base);
            Value offset = lower(*index->index);
            const bool swapped = !ast::isPointer(base.type) && ast::isPointer(offset.type);
            if (swapped)
                std::swap(base, offset);   // C's i[p] is p[i]
            // An operand's own error says why (Value): a pointer whose index failed still gives its element, poison.
            if (!ast::isPointer(base.type))
            {
                if (!base.failed && !offset.failed)
                    error(expr.location, "subscripted value is not a pointer");
                return std::nullopt;
            }
            if (offset.failed)
                offset = poison(ast::Type::Int);
            if (offset.type != ast::Type::Int && offset.type != ast::Type::Bool)
            {
                error(expr.location, "array subscript is not an integer");
                return std::nullopt;
            }
            std::optional<LValue> target = element(expr.location, base, offset, address);
            // An element of a const array its variable names, t[i] (or t[i][j], a 2-D one's): clang names the variable
            // when it is assigned.
            const ast::Expr* root = swapped ? index->index.get() : index->base.get();
            if (const auto* row = std::get_if<ast::IndexExpr>(&root->node))
                root = std::holds_alternative<ast::VariableExpr>(row->base->node) ? row->base.get() : row->index.get();
            const auto* array = std::get_if<ast::VariableExpr>(&root->node);
            const Binding* binding = array != nullptr ? named(array->name) : nullptr;
            if (target && target->isConst && binding != nullptr && ast::isArray(binding->type) && binding->isConst)
            {
                target->array = array->name;
                target->arrayType = binding->type;
            }
            return target;
        }
        if (const auto* unary = std::get_if<ast::UnaryExpr>(&expr.node))   // *p
        {
            const Value pointer = lower(*unary->operand);
            if (pointer.failed && !ast::isPointer(pointer.type))
                return std::nullopt;   // its own error says why (Value)
            if (!ast::isPointer(pointer.type))
            {
                error(expr.location,
                      "indirection requires pointer operand (" + quoted(ast::typeName(pointer.type)) + " invalid)");
                return std::nullopt;
            }
            return element(expr.location, pointer, std::nullopt, address);
        }

        // p->y, a field through a pointer to a module.
        const auto& member = std::get<ast::MemberExpr>(expr.node);
        if (member.arrow)
        {
            const Value pointer = lower(*member.base);
            if (pointer.failed && (!ast::isPointer(pointer.type) || pointer.type.pointee != ast::Kind::Module))
                return std::nullopt;   // its own error says why (Value)
            if (!ast::isPointer(pointer.type))
            {
                error(expr.location, "member reference type " + quoted(ast::typeName(pointer.type))
                                         + " is not a pointer"
                                         + (ast::isModule(pointer.type) ? "; did you mean to use '.'?" : ""));
                return std::nullopt;
            }
            if (pointer.type.pointee != ast::Kind::Module)
            {
                error(expr.location, "member reference base type " + quoted(ast::typeName(ast::pointee(pointer.type)))
                                         + " is not a structure or union");
                return std::nullopt;
            }
            if (pointer.extent != nullptr)   // (voices + v)->y: one of the array's modules
                checkBounds(expr.location, *pointer.extent, elementOf(*pointer.extent, nullptr), 1);
            return field(expr.location, pointer.ir, *pointer.type.module, member.name, pointer.type.pointeeConst);
        }
        // m.y, a module's field; or components of an lvalue: of a variable, or of components of one (v.xy.x is v's
        // lane 0).
        std::optional<LValue> base = lvalue(*member.base, location);
        if (!base)
            return std::nullopt;
        if (ast::isModule(base->type))
            return field(expr.location, base->address, *base->type.module, member.name, base->isConst);
        if (ast::isPointer(base->type) && base->type.pointee == ast::Kind::Module)
        {
            error(expr.location, "member reference type " + quoted(ast::typeName(base->type))
                                     + " is a pointer; did you mean to use '->'?");
            return std::nullopt;
        }
        if (!hasComponents(location, base->type))
            return std::nullopt;
        const auto lanes = components(location, member.name, base->type);
        if (!lanes)
            return std::nullopt;
        const auto type = componentsType(location, lanes->size());
        if (!type)
            return std::nullopt;
        std::vector<int> whole;   // the lanes of the vector in memory
        for (const int lane : *lanes)
            whole.push_back(base->components.empty() ? lane : base->components[static_cast<std::size_t>(lane)]);
        base->type = *type;
        base->components = std::move(whole);
        return base;
    }

    // The element `pointer` points to (*p), or the index-th from it (p[i]): its address, and its type, const when
    // the pointer's is a const T*. Nothing for a pointer to what FMinus cannot point to (its declaration reported
    // that). A pointer to a row, what a 2-D array's name stands for, gives the row, an array (which a read makes a
    // pointer to its first element in turn: grid[i][j]). A pointer into an array the code generator can see is checked
    // first (Bounds.cpp): the element must be one of the array's, or, for its `address` (&p[i]), at most one past the
    // last; the element carries the extent on, for &. A row is checked as an element (grid[4] of a float[4][16] is
    // out, whatever is done with it), and its own elements against the row (Bounds.cpp).
    std::optional<Lowering::LValue> Lowering::element(SourceLocation location, Value pointer,
                                                      std::optional<Value> index, bool address)
    {
        const ast::Type type = ast::pointee(pointer.type);
        if (type == ast::Type::Void)   // an address with no type: it has no elements
        {
            error(location, index ? "subscript of pointer to incomplete type 'void'"
                                  : "indirection not permitted on operand of type " + quoted(ast::typeName(pointer.type)));
            return std::nullopt;
        }
        const bool row = ast::isArray(type);
        if (!ast::canPointTo(type.kind) && !row)
            return std::nullopt;
        if (row)
            address = false;
        llvm::Value* at = pointer.ir;
        llvm::Value* offset = nullptr;
        if (index)
        {
            offset = convert(*index, ast::Type::Int, location, Use::Initializing);   // a bool: 0 or 1
            offset = builder_.CreateSExt(offset, builder_.getInt64Ty(), "idxprom");
        }
        const Extent* extent = nullptr;
        if (pointer.extent != nullptr)
        {
            extent = movedExtent(*pointer.extent, offset);
            checkBounds(location, *extent, elementOf(*extent, nullptr), address ? 0 : 1);
        }
        if (offset != nullptr)
            at = builder_.CreateInBoundsGEP(typeOf(type), pointer.ir, offset, "arrayidx");
        LValue target{at, type, pointer.type.pointeeConst, "", type, {}};
        target.extent = extent;
        return target;
    }

    // Whether `target` may be stored to: not an array, not const, and with no component twice (`v.xx = e` would write
    // lane 0 twice). Reports clang's error when not.
    bool Lowering::writable(SourceLocation location, const LValue& target)
    {
        if (ast::isArray(target.type))
        {
            error(location, "array type " + quoted(ast::typeName(target.type)) + " is not assignable");
            return false;
        }
        if (target.isConst && !target.array.empty())   // t[i] of a const array t
        {
            error(location, "cannot assign to variable " + quoted(target.array) + " with const-qualified type "
                                + quoted("const " + ast::typeName(target.arrayType)));
            return false;
        }
        if (target.isConst && target.name.empty())
        {
            error(location, "read-only variable is not assignable");   // an element through a const T*
            return false;
        }
        if (target.isConst)
        {
            // clang writes a const pointer's type with the const last: 'float *const'
            const std::string type = ast::isPointer(target.whole) ? ast::typeName(target.whole) + "const"
                                                                  : "const " + ast::typeName(target.whole);
            error(location, "cannot assign to variable " + quoted(target.name) + " with const-qualified type "
                                + quoted(type));
            return false;
        }
        for (std::size_t i = 0; i < target.components.size(); ++i)
        {
            for (std::size_t j = i + 1; j < target.components.size(); ++j)
            {
                if (target.components[i] == target.components[j])
                {
                    error(location, "vector is not assignable (contains duplicate components)");
                    return false;
                }
            }
        }
        return true;
    }

    // A load; for components, a load of the vector and the lanes taken out of it (an extractelement for one, a
    // shufflevector for several). An array is no load: its name stands for a pointer to its first element, const
    // when the array is (C's decay), which carries the array's extent under Options::boundsCheck, the array `named`
    // so in a failed check's message (Bounds.cpp). Nor is a module, whose value is its address. The load carries its
    // type's TBAA tag (Tbaa.cpp), as every load and store of a value does.
    Lowering::Value Lowering::read(const LValue& source, std::string named)
    {
        if (ast::isArray(source.type))
            return {builder_.CreateConstInBoundsGEP2_64(typeOf(source.type), source.address, 0, 0, "arraydecay"),
                    ast::Type::pointerTo(ast::pointee(source.type), source.isConst),
                    arrayExtent(source.type, std::move(named))};
        if (ast::isModule(source.type))
            return {source.address, source.type};
        llvm::LoadInst* loaded = builder_.CreateLoad(typeOf(source.whole), source.address, source.name);
        tag(loaded, source.whole);
        if (source.components.empty())
            return {loaded, source.type};
        if (source.components.size() == 1)
            return {builder_.CreateExtractElement(loaded, static_cast<std::uint64_t>(source.components[0])),
                    source.type};
        return {builder_.CreateShuffleVector(loaded, source.components), source.type};
    }

    // An lvalue expression's value: a variable's, an element's (p[i], *p). A recovery() after an error, which lvalue()
    // reported, or an operand's or a declaration's that it found (element()).
    Lowering::Value Lowering::readLValue(const ast::Expr& expr)
    {
        const std::optional<LValue> source = lvalue(expr, expr.location);
        if (!source)
            return recovery(ast::Type::Float);
        return read(*source, boundsCheck_ && ast::isArray(source->type) ? spelled(expr) : std::string());
    }

    // `value` converted to the target's type and stored: for components, the vector is loaded, the lanes inserted
    // and the vector stored back; a module is copied whole. Returns what was stored, the value of an assignment (a
    // module's: its address).
    llvm::Value* Lowering::write(SourceLocation location, const LValue& target, Value value, Use use)
    {
        llvm::Value* stored = convert(value, target.type, location, use);
        if (ast::isModule(target.type))
        {
            if (value.type == target.type)
                copy(target.address, value.ir, target.type);
            return target.address;
        }
        if (target.components.empty())
        {
            tag(builder_.CreateStore(stored, target.address), target.type);
            return stored;
        }
        llvm::LoadInst* whole = builder_.CreateLoad(typeOf(target.whole), target.address, target.name);
        tag(whole, target.whole);
        llvm::Value* vector = whole;
        for (std::size_t i = 0; i < target.components.size(); ++i)
        {
            llvm::Value* element = target.components.size() == 1 ? stored : builder_.CreateExtractElement(stored, i);
            vector = builder_.CreateInsertElement(vector, element,
                                                  static_cast<std::uint64_t>(target.components[i]));
        }
        tag(builder_.CreateStore(vector, target.address), target.whole);
        return stored;
    }

    // &x: the address of an lvalue, a pointer to its type (const when it is). A variable whose address is taken stays
    // in memory, since mem2reg cannot promote a slot that escapes; after inlining, SROA often can.
    Lowering::Value Lowering::lowerAddressOf(SourceLocation location, const ast::Expr& operand)
    {
        const Value failed = recovery(ast::Type::pointerTo(ast::Kind::Float, false));   // after an error, each
        if (!isLValue(operand))
        {
            const Value value = lower(operand);
            if (!value.failed)   // else its own error says why (Value): &(a + y)
                error(location, "cannot take the address of an rvalue of type " + quoted(ast::typeName(value.type)));
            return failed;
        }
        const std::optional<LValue> target = lvalue(operand, location, true);
        if (!target)
            return failed;
        if (!target->components.empty())
        {
            error(location, "address of vector element requested");
            return failed;
        }
        if (!ast::canPointTo(target->type.kind))
        {
            noPointerTo(location, target->type);
            return failed;
        }
        return {target->address, ast::Type::pointerTo(target->type, target->isConst), target->extent};
    }

    // Every byte of what is at `address` 0, as a declaration of an array or a module starts.
    void Lowering::zero(llvm::Value* address, ast::Type type)
    {
        builder_.CreateMemSet(address, builder_.getInt8(0), *sizeOf(type), alignOf(type));
    }

    // Where `decl`'s list puts each of its values in an array of `type`, as C's rules place them: the values in order,
    // each in the next element, a 2-D array's row by row; and a braced list, `{1, 2}`, a 2-D array's row, from the
    // row's first element, after which the next value starts the next row (C's brace elision, `{{1}, 2, 3}`, too). A
    // count of 0 is no bound, for an array whose list sizes it. C only warns about excess elements (and drops them),
    // and about braces around one element (and takes the first value in them): FMinus refuses both, in clang's words,
    // where the excess or the brace is. Reported when `report`; the placement is then not `ok`, and holds the values
    // placed before the error.
    Lowering::Placed Lowering::placeList(ast::Type type, const ast::DeclStmt& decl, bool report)
    {
        Placed placed;
        const auto fail = [&](SourceLocation where, const char* message) {
            if (report)
                error(where, message);
            placed.ok = false;
            return placed;
        };
        const bool rows = ast::hasRows(type);
        const std::uint64_t width = rows ? static_cast<std::uint64_t>(type.rowCount) : 1;
        const std::uint64_t total = type.count == 0 ? std::numeric_limits<std::uint64_t>::max()
                                                    : static_cast<std::uint64_t>(type.count) * width;
        const std::vector<ast::ExprPtr>& values = decl.list;
        const std::vector<ast::Brace>& braces = decl.braces;
        std::uint64_t at = 0;   // the next element
        std::size_t brace = 0;
        for (std::size_t i = 0;;)
        {
            if (brace < braces.size() && braces[brace].first == i)
            {
                const ast::Brace& row = braces[brace++];
                if (!rows || row.depth > 1 || at % width != 0)   // around one element: C takes it as the element's
                    return fail(row.location, "braces around scalar initializer");
                if (brace < braces.size() && braces[brace].depth > 1)   // one inside it, which comes next
                    return fail(braces[brace].location, "braces around scalar initializer");
                if (at >= total)
                    return fail(row.location, "excess elements in array initializer");
                if (row.count > width)
                    return fail(values[i + width]->location, "excess elements in array initializer");
                for (std::size_t k = 0; k < row.count; ++k)
                    placed.values.emplace_back(at + k, values[i + k].get());
                at += width;
                i += row.count;
                placed.reach = at;
                continue;
            }
            if (i >= values.size())
                return placed;
            if (at >= total)
                return fail(values[i]->location, "excess elements in array initializer");
            placed.values.emplace_back(at++, values[i++].get());
            placed.reach = at;
        }
    }

    // The address of the element `at` (from the first, row by row) of the array of `array`'s type at `address`.
    llvm::Value* Lowering::elementAt(ast::Type array, llvm::Value* address, std::uint64_t at, const char* name)
    {
        if (!ast::hasRows(array))
            return builder_.CreateConstInBoundsGEP2_64(typeOf(array), address, 0, at, name);
        const auto width = static_cast<std::uint64_t>(array.rowCount);
        return builder_.CreateInBoundsGEP(
            typeOf(array), address, {builder_.getInt64(0), builder_.getInt64(at / width), builder_.getInt64(at % width)},
            name);
    }

    // `float c[4] = {1, 2};`: the list's values, converted to the element type, in order, each where placeList puts
    // it (`float g[2][3] = {{1, 2}, {3}};`, a 2-D array's rows); the rest stay 0. An array of modules takes no list:
    // its modules start at 0, as every module does.
    void Lowering::initializeArray(SourceLocation location, llvm::Value* address, ast::Type type,
                                   const ast::DeclStmt& decl)
    {
        const ast::Type element = ast::elementOf(type);
        if (ast::isModule(element) && (!decl.list.empty() || !decl.braces.empty()))
        {
            error(location, "an array of modules takes no initializer list: its modules start at 0");
            return;
        }
        for (const auto& [at, item] : placeList(type, decl, true).values)
        {
            const Value value = lowerAs(*item, element);
            tag(builder_.CreateStore(convert(value, element, item->location, Use::Initializing),
                                     elementAt(type, address, at, "arrayinit")),
                element);
        }
    }

    // `const float t[4] = {1, 2, 3, 4};`: a const array, which nothing can write (an element through its name or a
    // const float* is read-only, and a const float* converts to no float*). When every element of its list is a
    // constant once lowered (a number, sizeof, operators and casts of them, a builtin of constants such as hash(3); not
    // ?:, && or ||, which branch), the array is a constant of the module, stored once, as clang stores a static const
    // one: a private unnamed_addr constant global named as clang names a local's (__const.f.t), which every call reads
    // and none fills. Its elements are what the stack's array would hold, bit for bit: the list's values, converted to
    // the element type as initializeArray converts them, then zeros (eight or more of them as one zeroinitializer, as
    // clang writes them, so that the IR holds no constant per element). The one difference C could see is the
    // address: every call's table is the same object, where C's would differ between nested calls of one function.
    // Otherwise it is a local as any array is: a stack slot, zeroed, then the list's values stored, which are lowered
    // once, in order, before it. So is an array over kMaxTableBytes (1 MiB, Lowering.h), whatever its list: a table
    // costs the compiler about twice its size, where a stack slot costs nothing to compile, and an array may have 2^30
    // elements. Returns the global or the slot; `type` is the declaration's (an array of what FMinus can point to, but
    // a module), and the declaration has no `= expr`.
    llvm::Value* Lowering::declareConstArray(const ast::DeclStmt& decl, ast::Type type)
    {
        const ast::Type element = ast::elementOf(type);
        std::vector<std::pair<std::uint64_t, llvm::Value*>> values;
        for (const auto& [at, item] : placeList(type, decl, true).values)
            values.emplace_back(at, convert(lowerAs(*item, element), element, item->location, Use::Initializing));
        auto* arrayType = llvm::cast<llvm::ArrayType>(typeOf(type));
        if (module_.getDataLayout().getTypeAllocSize(arrayType).getFixedValue() <= kMaxTableBytes
            && std::ranges::all_of(values, [](const auto& value) { return llvm::isa<llvm::Constant>(value.second); }))
        {
            std::vector<std::pair<std::uint64_t, llvm::Constant*>> given;
            for (const auto& [at, value] : values)
                given.emplace_back(at, llvm::cast<llvm::Constant>(value));
            llvm::Constant* contents = tableOf(type, given);
            auto* table = new llvm::GlobalVariable(module_, contents->getType(), true,
                                                   llvm::GlobalValue::PrivateLinkage, contents,
                                                   "__const." + function_->name + "." + decl.name);
            table->setUnnamedAddr(llvm::GlobalValue::UnnamedAddr::Global);
            table->setAlignment(alignOf(type));
            return table;
        }
        llvm::AllocaInst* slot = allocate(arrayType, decl.name);
        zero(slot, type);
        for (const auto& [at, value] : values)
            tag(builder_.CreateStore(value, elementAt(type, slot, at, "arrayinit")), element);
        return slot;
    }

    // A table's contents, of `type`: `given`, constants of its element type (no more than its count), then zeros, in
    // the shapes of clang's EmitArrayConstant. Up to the last element that is not zero; then, when eight or more zeros
    // follow it, the two side by side in a packed struct, the zeros one zeroinitializer, so that the IR holds no
    // constant per element of them (a table of zeros is one zeroinitializer); fewer, each in the array.
    llvm::Constant* Lowering::tableContents(llvm::ArrayType* type, std::vector<llvm::Constant*> given)
    {
        llvm::Type* elementType = type->getElementType();
        const std::uint64_t count = type->getNumElements();
        while (!given.empty() && given.back()->isNullValue())
            given.pop_back();
        const std::uint64_t zeros = count - given.size();
        if (given.empty())
            return llvm::ConstantAggregateZero::get(type);
        if (zeros >= 8)
            return llvm::ConstantStruct::getAnon(
                {llvm::ConstantArray::get(llvm::ArrayType::get(elementType, given.size()), given),
                 llvm::ConstantAggregateZero::get(llvm::ArrayType::get(elementType, zeros))},
                true);   // packed: the two parts side by side, as the array lays them out
        given.resize(count, llvm::Constant::getNullValue(elementType));
        return llvm::ConstantArray::get(type, given);
    }

    // A table's contents, of the array `type`: `given`, each element's constant and its index from the first, in
    // order (a 2-D array's row by row), and zeros, in tableContents' shapes. A 2-D array's rows are each a table of
    // their own, in an array of them, a table of rows; or, when a row's zeros make it a struct, side by side in a
    // packed struct, the rows after the last given one a zeroinitializer. Either way the bytes are the array's, row by
    // row (each row's size a multiple of its elements' alignment, so no padding between them).
    llvm::Constant* Lowering::tableOf(ast::Type type,
                                      const std::vector<std::pair<std::uint64_t, llvm::Constant*>>& given)
    {
        auto* arrayType = llvm::cast<llvm::ArrayType>(typeOf(type));
        if (!ast::hasRows(type))
        {
            std::vector<llvm::Constant*> elements;
            for (const auto& [at, value] : given)
            {
                elements.resize(at, llvm::Constant::getNullValue(arrayType->getElementType()));
                elements.push_back(value);
            }
            return tableContents(arrayType, std::move(elements));
        }
        auto* rowType = llvm::cast<llvm::ArrayType>(arrayType->getElementType());
        const auto width = static_cast<std::uint64_t>(type.rowCount);
        llvm::Constant* zero = llvm::Constant::getNullValue(rowType->getElementType());
        std::vector<std::vector<llvm::Constant*>> rows;   // up to the last row given a value
        for (const auto& [at, value] : given)
        {
            rows.resize(std::max<std::size_t>(rows.size(), at / width + 1));
            std::vector<llvm::Constant*>& row = rows[at / width];
            row.resize(at % width, zero);
            row.push_back(value);
        }
        std::vector<llvm::Constant*> contents;
        bool plain = true;   // every row an array of its type
        for (std::vector<llvm::Constant*>& row : rows)
        {
            contents.push_back(tableContents(rowType, std::move(row)));
            plain = plain && contents.back()->getType() == rowType;
        }
        if (plain)
            return tableContents(arrayType, std::move(contents));
        if (const std::uint64_t after = static_cast<std::uint64_t>(type.count) - rows.size(); after > 0)
            contents.push_back(llvm::ConstantAggregateZero::get(llvm::ArrayType::get(rowType, after)));
        return llvm::ConstantStruct::getAnon(contents, true);
    }

    // C's arithmetic on pointers. p + i and i + p are the element i after p (i may be negative), and p - i the one i
    // before; p - q is how many elements apart two pointers of one type are, an int (C's is a ptrdiff_t, which
    // FMinus does not have); and two pointers of one type compare as addresses. const does not matter to any of
    // them. Nothing else takes a pointer. Under Options::boundsCheck, p + i and p - i into an array the code
    // generator can see are checked to point within it, or one past its end, and carry its extent on (Bounds.cpp).
    Lowering::Value Lowering::pointerArithmetic(SourceLocation location, ast::BinaryOp op, Value lhs, Value rhs)
    {
        const std::string operands =
            "(" + quoted(ast::typeName(lhs.type)) + " and " + quoted(ast::typeName(rhs.type)) + ")";
        const auto integer = [](Value value) { return value.type == ast::Type::Int || value.type == ast::Type::Bool; };
        const bool voidPointee = (ast::isPointer(lhs.type) && lhs.type.pointee == ast::Kind::Void)
                                 || (ast::isPointer(rhs.type) && rhs.type.pointee == ast::Kind::Void);
        if (voidPointee && !isComparison(op) && (op == ast::BinaryOp::Add || op == ast::BinaryOp::Subtract))
        {
            error(location, "arithmetic on a pointer to void");   // a void* has no element size
            return poison(op == ast::BinaryOp::Subtract && ast::isPointer(rhs.type) ? ast::Type::Int : lhs.type);
        }
        if (ast::isPointer(lhs.type) && ast::isPointer(rhs.type))
        {
            const bool comparison = isComparison(op);
            // A void* compares with any pointer, as in C: it converts to the void* first.
            const bool samePointee = ast::samePointee(lhs.type, rhs.type) || voidPointee;
            if (samePointee && op == ast::BinaryOp::Subtract
                && (ast::canPointTo(lhs.type.pointee) || ast::hasRows(lhs.type)))
            {
                llvm::Value* difference =
                    builder_.CreatePtrDiff(typeOf(ast::pointee(lhs.type)), lhs.ir, rhs.ir, "sub.ptr.div");
                return {builder_.CreateTrunc(difference, builder_.getInt32Ty(), "conv"), ast::Type::Int};
            }
            if (samePointee && comparison)
            {
                const auto predicate = [&] {
                    switch (op)
                    {
                    case ast::BinaryOp::Less:
                        return llvm::CmpInst::ICMP_ULT;
                    case ast::BinaryOp::LessEqual:
                        return llvm::CmpInst::ICMP_ULE;
                    case ast::BinaryOp::Greater:
                        return llvm::CmpInst::ICMP_UGT;
                    case ast::BinaryOp::GreaterEqual:
                        return llvm::CmpInst::ICMP_UGE;
                    case ast::BinaryOp::NotEqual:
                        return llvm::CmpInst::ICMP_NE;
                    default:
                        return llvm::CmpInst::ICMP_EQ;
                    }
                }();
                return {builder_.CreateICmp(predicate, lhs.ir, rhs.ir, "cmp"), ast::Type::Bool};
            }
            if (!samePointee && op == ast::BinaryOp::Subtract)
                error(location, quoted(ast::typeName(lhs.type)) + " and " + quoted(ast::typeName(rhs.type))
                                    + " are not pointers to compatible types");
            else if (!samePointee && comparison)
                error(location, "comparison of distinct pointer types " + operands);
            else
                error(location, "invalid operands to binary expression " + operands);
            return poison(comparison ? ast::Type::Bool : ast::Type::Int);
        }

        // A pointer and an integer: p + i, i + p, p - i.
        const bool pointerFirst = ast::isPointer(lhs.type);
        const Value& pointer = pointerFirst ? lhs : rhs;
        const Value& offset = pointerFirst ? rhs : lhs;
        const bool valid = integer(offset)
                           && (op == ast::BinaryOp::Add || (op == ast::BinaryOp::Subtract && pointerFirst));
        if (!valid)
        {
            error(location, "invalid operands to binary expression " + operands);
            return poison(isComparison(op) ? ast::Type::Bool : pointer.type);
        }
        if (!ast::canPointTo(pointer.type.pointee) && !ast::hasRows(pointer.type))
            return recovery(pointer.type);   // its declaration reported it
        llvm::Value* index = builder_.CreateSExt(convert(offset, ast::Type::Int, location, Use::Initializing),
                                                 builder_.getInt64Ty(), "idx.ext");
        if (op == ast::BinaryOp::Subtract)
            index = builder_.CreateNeg(index, "idx.neg");
        const Extent* extent = nullptr;
        if (pointer.extent != nullptr)
        {
            extent = movedExtent(*pointer.extent, index);
            checkBounds(location, *extent, elementOf(*extent, nullptr), 0);
        }
        return {builder_.CreateInBoundsGEP(typeOf(ast::pointee(pointer.type)), pointer.ir, index, "add.ptr"),
                pointer.type, extent};
    }
} // namespace fminus::ir::detail
