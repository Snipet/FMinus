// src/ir/Shapes.cpp: shapes, the runtime's description of a value in memory, which its JSON functions walk
// (runtime/Runtime.h, "shapes"; runtime/JsonShapes.h checks one before it reads a byte by it). A type is made into
// words here, once, when the program is compiled: a float is 1, a float[4] is 9, 4, 4, 1 (an array of 4, 4 bytes
// apart, of floats), a module is 12 and its fields, each a record of its JSON name, its offset, its range and default
// and its own shape, nested modules inside it. So the runtime walks a constant table and never needs the program's
// types.
//
// json_read(doc, "path", ..., &preset) passes its module type's table (moduleShape): one private constant a type,
// json.shape.Preset, made at the first call that names the type. A JSON literal's values (a later step's) take the
// same shapes from appendShape: the table of a module type is the same words for both, but for a string field, which
// the writer takes and the reader leaves out (ShapeUse).
//
// What a module's table holds, and under what name (appendField): each field the runtime can fill or write (a float, a
// double, an int, a bool, an enumerated type's value, a char array, a float2, float4 or float8, a module, and arrays of
// one and two dimensions of those), under its own name or its [[json::name("...")]]; not a parameter (the host's value,
// which a program does not write), a field with [[json::skip]], a pointer, a handle, a poly type, nor, for reading, a
// string (it would point into the document, which the module outlives); nor the bools of its methods' `when`s, which
// follow the declared fields and are no field of the tree. The json:: attributes are checked here too
// (checkJsonAttribute, checkJsonNames), as Modules.cpp checks the fminus:: ones.
#include "ir/Lowering.h"

#include <llvm/IR/Constants.h>
#include <llvm/IR/DataLayout.h>
#include <llvm/IR/GlobalVariable.h>

#include <bit>
#include <cstdint>
#include <format>
#include <limits>
#include <map>
#include <set>

namespace fminus::ir::detail
{
    namespace
    {
        // A shape's first word, its kind, as runtime/JsonShapes.h numbers them (Runtime.h lists the words after each).
        // A pointer (8) and a span (10) are the writer's, and a JSON literal's lowering makes them.
        enum : std::int32_t
        {
            ShapeFloat = 1,
            ShapeDouble = 2,
            ShapeInt = 3,
            ShapeBool = 4,
            ShapeString = 5,
            ShapeChars = 6,
            ShapeEnum = 7,
            ShapeArray = 9,
            ShapeVector = 11,
            ShapeModule = 12,
        };

        // A field record's flags: a range follows, a default follows.
        enum : std::int32_t
        {
            FieldRange = 1,
            FieldDefault = 2,
        };

        // The runtime's bounds (JsonShapes.h): shapes nest 16 deep (the outermost at 0, each array and module one level
        // more), and a module has at most 1024 fields. A word is an int32.
        constexpr int kMostShapeDepth = 16;
        constexpr std::int32_t kMostFields = 1024;
        constexpr std::uint64_t kMostWord = std::numeric_limits<std::int32_t>::max();

        // A double as two words, its IEEE bits' low 32 first.
        void addDouble(std::vector<std::int32_t>& code, double value)
        {
            const auto bits = std::bit_cast<std::uint64_t>(value);
            code.push_back(static_cast<std::int32_t>(static_cast<std::uint32_t>(bits & 0xffffffffu)));
            code.push_back(static_cast<std::int32_t>(static_cast<std::uint32_t>(bits >> 32)));
        }

        // `text` added to the table's bytes, and its offset and length to its words.
        void addName(ShapeTable& table, std::string_view text)
        {
            table.code.push_back(static_cast<std::int32_t>(table.bytes.size()));
            table.code.push_back(static_cast<std::int32_t>(text.size()));
            table.bytes += text;
        }

        // The last attribute of `name` a field has, or null: the one that counts, as Program::findAttribute's.
        const ast::Attribute* lastNamed(const ast::Field& field, std::string_view name)
        {
            const ast::Attribute* found = nullptr;
            for (const ast::Attribute& attribute : field.attributes)
            {
                if (attribute.name == name)
                    found = &attribute;
            }
            return found;
        }

        // The numbers of the last attribute of `name` that has `count` of them and nothing else (checkAttributes has
        // reported every other), or none.
        std::optional<std::vector<double>> numbersOf(const ast::Field& field, std::string_view name, std::size_t count)
        {
            std::optional<std::vector<double>> found;
            for (const ast::Attribute& attribute : field.attributes)
            {
                if (attribute.name != name || attribute.args.size() != count)
                    continue;
                std::vector<double> numbers;
                for (const ast::AttributeArg& arg : attribute.args)
                {
                    if (const std::optional<double> number = ast::attributeNumber(arg))
                        numbers.push_back(*number);
                }
                if (numbers.size() == count)
                    found = std::move(numbers);
            }
            return found;
        }


        // The offset of the first byte of `text` that is no part of UTF-8 text (RFC 3629: a lead byte and its
        // continuation bytes, the shortest form, no surrogate, at most U+10FFFF), or none. A JSON member's name is
        // UTF-8, as the parser checks a document's.
        std::optional<std::size_t> notUtf8(std::string_view text)
        {
            for (std::size_t i = 0; i < text.size();)
            {
                const auto byte = static_cast<unsigned char>(text[i]);
                if (byte < 0x80)
                {
                    ++i;
                    continue;
                }
                const std::size_t length = byte >= 0xc2 && byte <= 0xdf   ? 2
                                           : byte >= 0xe0 && byte <= 0xef ? 3
                                           : byte >= 0xf0 && byte <= 0xf4 ? 4
                                                                           : 0;
                if (length == 0 || i + length > text.size())
                    return i;
                for (std::size_t k = 1; k < length; ++k)
                {
                    if ((static_cast<unsigned char>(text[i + k]) & 0xc0) != 0x80)
                        return i;
                }
                const auto second = static_cast<unsigned char>(text[i + 1]);
                if ((byte == 0xe0 && second < 0xa0) || (byte == 0xed && second >= 0xa0)
                    || (byte == 0xf0 && second < 0x90) || (byte == 0xf4 && second >= 0x90))
                    return i;
                i += length;
            }
            return std::nullopt;
        }

        // A json::name's text when it is a good one (checkJsonAttribute reports any other): one string, not empty,
        // UTF-8.
        std::optional<std::string> nameOf(const ast::Attribute& attribute)
        {
            if (attribute.args.size() != 1 || !attribute.args[0].isString || attribute.args[0].text.empty()
                || notUtf8(attribute.args[0].text))
                return std::nullopt;
            return attribute.args[0].text;
        }
    } // namespace

    // Appends the shape of a value of `type` in memory to `table` (its words, and its names' bytes), `depth` levels
    // inside the table's outermost shape: a float, a double, an int and a bool their kinds; an enumerated type's value
    // its enumeration, the value's size (4, or 1 for an enumeration of bool) and its constants as written (`SAW`),
    // each its value and its name; a float2, float4 or float8 its lanes; a char array of n its n bytes; an array its
    // count, its elements' stride in bytes and their shape (a 2-D array an array of rows); a module its fields
    // (appendField); and, for writing, a string. Anything else has none (NoShape), and appends nothing; after any
    // other problem the table is not well formed, and its caller discards it (appendField and moduleShape do). Strides
    // and offsets are the data layout's, as a C compiler lays out the struct a host reads.
    ShapeProblem Lowering::appendShape(ast::Type type, ShapeUse use, ShapeTable& table, int depth)
    {
        if (depth > kMostShapeDepth)
            return ShapeProblem::TooDeep;
        std::vector<std::int32_t>& code = table.code;
        if (type.enumeration != 0 && (type.kind == ast::Kind::Int || type.kind == ast::Kind::Bool))
        {
            const auto found = enumerations_.find(type.enumeration);
            const std::size_t count = found != enumerations_.end() ? found->second.size() : 0;
            code.push_back(ShapeEnum);
            code.push_back(type.kind == ast::Kind::Bool ? 1 : 4);
            code.push_back(static_cast<std::int32_t>(count));
            for (std::size_t k = 0; k < count; ++k)
            {
                code.push_back(found->second[k].first);
                addName(table, found->second[k].second);
            }
            return ShapeProblem::None;
        }
        switch (type.kind)
        {
        case ast::Kind::Float:
            code.push_back(ShapeFloat);
            return ShapeProblem::None;
        case ast::Kind::Double:
            code.push_back(ShapeDouble);
            return ShapeProblem::None;
        case ast::Kind::Int:
            code.push_back(ShapeInt);
            return ShapeProblem::None;
        case ast::Kind::Bool:
            code.push_back(ShapeBool);
            return ShapeProblem::None;
        case ast::Kind::Float2:
        case ast::Kind::Float4:
        case ast::Kind::Float8:
            code.push_back(ShapeVector);
            code.push_back(static_cast<std::int32_t>(ast::lanes(type)));
            return ShapeProblem::None;
        case ast::Kind::String:
            if (use == ShapeUse::Read)
                return ShapeProblem::NoShape;
            code.push_back(ShapeString);
            return ShapeProblem::None;
        case ast::Kind::Array:
        {
            const ast::Type element = ast::pointee(type);   // a row, for a 2-D array
            if (element.kind == ast::Kind::Char)
            {
                if (type.count < 1)
                    return ShapeProblem::NoShape;
                code.push_back(ShapeChars);
                code.push_back(type.count);
                return ShapeProblem::None;
            }
            const std::optional<std::uint64_t> stride = sizeOf(element);
            if (!stride || *stride > kMostWord)
                return ShapeProblem::TooLarge;
            const std::size_t at = code.size();
            code.push_back(ShapeArray);
            code.push_back(type.count);
            code.push_back(static_cast<std::int32_t>(*stride));
            const ShapeProblem problem = appendShape(element, use, table, depth + 1);
            if (problem == ShapeProblem::NoShape)
                code.resize(at);   // an array of what has none (handles, strings for reading) has none
            return problem;
        }
        case ast::Kind::Module:
        {
            const ast::Module& module = *type.module;
            const std::size_t at = code.size();
            code.push_back(ShapeModule);
            code.push_back(0);
            std::int32_t fields = 0;
            for (std::size_t i = 0; i < module.fields.size(); ++i)
            {
                const ShapeProblem problem = appendField(module, static_cast<unsigned>(i), use, table, depth + 1);
                if (problem == ShapeProblem::None)
                    ++fields;
                else if (problem != ShapeProblem::NoShape)
                    return problem;
            }
            if (fields > kMostFields)
                return ShapeProblem::TooManyFields;
            code[at + 1] = fields;
            return ShapeProblem::None;
        }
        case ast::Kind::Void:
        case ast::Kind::Bool2:
        case ast::Kind::Bool4:
        case ast::Kind::Bool8:
        case ast::Kind::PolyFloat:   // as wide as the target's vectors: a document could not say how many
        case ast::Kind::PolyBool:
        case ast::Kind::Pointer:
        case ast::Kind::Handle:
        case ast::Kind::Char:
            break;
        }
        return ShapeProblem::NoShape;
    }

    // Appends the record of the module's field `index`, `depth` levels in, when its module's shape has it: its size in
    // words (itself among them), its JSON name (jsonName), its offset in the module (the data layout's), its flags, its
    // range when it has one and its values are numbers (float, double, int or an enumeration's, looking through its
    // arrays and a vector's lanes), its default when it has one and its values are numbers or bools, and its shape.
    // NoShape, and nothing appended, for a field the shape leaves out (leftOut, or one of a type with no shape); any
    // other problem is the whole table's.
    ShapeProblem Lowering::appendField(const ast::Module& module, unsigned index, ShapeUse use, ShapeTable& table,
                                       int depth)
    {
        const ast::Field& field = module.fields[index];
        if (leftOut(field))
            return ShapeProblem::NoShape;
        std::vector<std::int32_t>& code = table.code;
        const std::size_t codeAt = code.size();
        const std::size_t bytesAt = table.bytes.size();
        const auto undo = [&](ShapeProblem problem) {
            code.resize(codeAt);
            table.bytes.resize(bytesAt);
            return problem;
        };
        const ast::Type type = fieldType(field);
        const std::uint64_t offset =
            module_.getDataLayout().getStructLayout(structs_.at(&module))->getElementOffset(index).getFixedValue();
        if (offset > kMostWord)
            return undo(ShapeProblem::TooLarge);
        code.push_back(0);   // its size, once its shape is known
        addName(table, jsonName(field));
        code.push_back(static_cast<std::int32_t>(offset));
        const std::size_t flagsAt = code.size();
        code.push_back(0);
        // Its values: an array's elements, a vector's lanes.
        const ast::Type element = ast::isArray(type) ? ast::elementOf(type) : type;
        const ast::Type leaf =
            element.kind == ast::Kind::Float2 || element.kind == ast::Kind::Float4 || element.kind == ast::Kind::Float8
                ? ast::Type::Float
                : element;
        const bool number = leaf.kind == ast::Kind::Float || leaf.kind == ast::Kind::Double
                            || leaf.kind == ast::Kind::Int || (leaf.kind == ast::Kind::Bool && leaf.enumeration != 0);
        const bool truth = leaf.kind == ast::Kind::Bool && leaf.enumeration == 0;
        std::int32_t flags = 0;
        if (const auto range = number ? numbersOf(field, "fminus::range", 2) : std::nullopt)
        {
            flags |= FieldRange;
            addDouble(code, (*range)[0]);
            addDouble(code, (*range)[1]);
        }
        if (const auto fallback = number || truth ? numbersOf(field, "fminus::default", 1) : std::nullopt)
        {
            flags |= FieldDefault;
            addDouble(code, (*fallback)[0]);
        }
        code[flagsAt] = flags;
        if (const ShapeProblem problem = appendShape(type, use, table, depth); problem != ShapeProblem::None)
            return undo(problem);
        code[codeAt] = static_cast<std::int32_t>(code.size() - codeAt);
        return ShapeProblem::None;
    }

    // Whether a module's shape leaves the field out whatever its type: a parameter (its value is the host's: a program
    // reads it as cutoff[i] and never writes it, and its slot is a stream's pointer and a value), or one with
    // [[json::skip]].
    bool Lowering::leftOut(const ast::Field& field)
    {
        return field.isParam || lastNamed(field, "json::skip") != nullptr;
    }

    // The name a field is read and written under: its last good [[json::name("...")]]'s, or its own.
    std::string Lowering::jsonName(const ast::Field& field)
    {
        const ast::Attribute* named = lastNamed(field, "json::name");
        const std::optional<std::string> text = named != nullptr ? nameOf(*named) : std::nullopt;
        return text ? *text : field.name;
    }

    // A table laid out as a constant of the program, named `name`, as a path's is (Paths.cpp): its word count and its
    // byte count, its words, then its bytes, together at 4-byte alignment.
    llvm::GlobalVariable* Lowering::shapeConstant(const ShapeTable& table, const std::string& name)
    {
        llvm::LLVMContext& context = module_.getContext();
        llvm::Type* int32 = llvm::Type::getInt32Ty(context);
        const std::vector<std::uint32_t> code(table.code.begin(), table.code.end());
        llvm::Constant* fields[] = {
            llvm::ConstantInt::get(int32, table.code.size()),
            llvm::ConstantInt::get(int32, table.bytes.size()),
            llvm::ConstantDataArray::get(context, code),
            llvm::ConstantDataArray::getString(context, table.bytes, false),
        };
        llvm::Constant* value = llvm::ConstantStruct::getAnon(context, fields);
        auto* global = new llvm::GlobalVariable(module_, value->getType(), true, llvm::GlobalValue::PrivateLinkage,
                                                value, name);
        global->setUnnamedAddr(llvm::GlobalValue::UnnamedAddr::Global);
        global->setAlignment(llvm::Align(4));
        return global;
    }

    // json_read's table of `module`: one shape, the module's, for reading, made at the type's first call and the same
    // constant for every call after it. Null, with the error at the call `location`, for a module json_read cannot
    // fill: one with no field it fills (there is nothing to read), or whose table the runtime would refuse (nested too
    // deep, too many fields, past an int32).
    llvm::Value* Lowering::moduleShape(SourceLocation location, const ast::Module& module)
    {
        if (const auto found = moduleShapes_.find(&module); found != moduleShapes_.end())
            return found->second;
        ShapeTable table;
        const ShapeProblem problem = appendShape(ast::Type::moduleType(&module), ShapeUse::Read, table);
        const std::string cannot = "'json_read' cannot fill " + quoted(module.name);
        switch (problem)
        {
        case ShapeProblem::TooDeep:
            error(location, cannot + ": its modules and arrays nest more than 16 deep, the most its table holds");
            return nullptr;
        case ShapeProblem::TooManyFields:
            error(location, cannot + ": a module in it has more than 1024 fields to fill, the most its table holds");
            return nullptr;
        case ShapeProblem::TooLarge:
            error(location, cannot + ": it is larger than its table can describe, 2 GB");
            return nullptr;
        case ShapeProblem::NoShape:   // a module always has one
        case ShapeProblem::None:
            break;
        }
        if (problem != ShapeProblem::None || table.code.size() < 2 || table.code[1] == 0)
        {
            error(location, "'json_read' has nothing to fill in " + quoted(module.name)
                                + ": it fills no parameter, string, pointer, handle, poly type or field with "
                                  "[[json::skip]], and " + quoted(module.name) + " has no other field");
            return nullptr;
        }
        llvm::GlobalVariable* global = shapeConstant(table, "json.shape." + module.name);
        moduleShapes_.emplace(&module, global);
        return global;
    }

    // json_read's last argument: a pointer to the module it fills (not const: it writes the module's fields), passed
    // as its type's table (moduleShape) and then its address. A string literal, a pointer to anything else, a module
    // itself and a const one are errors; false after any.
    bool Lowering::lowerFilled(SourceLocation location, const RuntimeBuiltin& builtin, const ast::Expr& arg,
                               std::vector<llvm::Value*>& irArgs)
    {
        const std::string must =
            quoted(builtin.name) + " fills a module: its last argument must be a pointer to one, not ";
        if (isString(arg))
        {
            error(begin(arg), must + "a string literal");
            return false;
        }
        const Value value = lower(arg);
        if (value.failed)
            return false;   // its own error says why (Value)
        if (!ast::isPointer(value.type) || value.type.pointee != ast::Kind::Module || value.type.module == nullptr)
        {
            error(begin(arg), must + quoted(ast::typeName(value.type)));
            return false;
        }
        const ast::Module& module = *value.type.module;
        const std::size_t reported = diagnostics_.errors().size();
        llvm::Value* pointer =
            convert(value, ast::Type::pointerTo(ast::Type::moduleType(&module), false), location, Use::Passing);
        if (diagnostics_.errors().size() != reported)
            return false;   // a const module's: C's error, which discards its qualifier
        llvm::Value* shape = moduleShape(location, module);
        if (shape == nullptr)
            return false;
        irArgs.push_back(shape);
        irArgs.push_back(pointer);
        return true;
    }

    // A json:: attribute of a module's field or parameter (checkAttributes): json::name("name"), one string, the
    // name of the JSON member the field is read and written as, not empty and UTF-8; json::skip, no argument: the
    // field is left out of the module's JSON (Shapes.cpp's tables). Another name is unknown: the namespace is
    // FMinus's. Each is passed to the host as written all the same (Program::Attribute). Clang's words where clang
    // has them (its attributes' arguments), FMinus's in the manner of the fminus:: ones' where it has none.
    void Lowering::checkJsonAttribute(const ast::Attribute& attribute)
    {
        if (attribute.name == "json::name")
        {
            if (attribute.args.size() != 1)
                error(attribute.location, "'json::name' attribute takes one argument");
            else if (!attribute.args[0].isString)
                error(attribute.location, "'json::name' attribute requires a string");
            else if (attribute.args[0].text.empty())
                error(attribute.location, "'json::name' takes a name of one byte or more: the JSON member's the field "
                                          "is read and written as");
            else if (const std::optional<std::size_t> bad = notUtf8(attribute.args[0].text))
                error(attribute.location,
                      std::format("'json::name' takes UTF-8 text, as a JSON member's name is, and byte {} of the "
                                  "name is no part of any",
                                  *bad + 1));
        }
        else if (attribute.name == "json::skip")
        {
            if (!attribute.args.empty())
                error(attribute.location, "'json::skip' attribute takes no arguments");
        }
        else
            error(attribute.location, "unknown attribute " + quoted(attribute.name)
                                          + ": a field takes json::name(\"name\") and json::skip");
    }

    // Two fields of `module` read and written under one JSON name: an error at the second, its json::name or its own
    // name, with a note at the first. Every field counts that a JSON literal or json_read could name: not one with
    // [[json::skip]], nor a pointer or a handle (or an array of handles), which neither ever does. A parameter and a
    // string count, so that a module written as a literal has no member twice. A second field of one name is layOut's
    // error ("duplicate member"), and not this one too.
    void Lowering::checkJsonNames(const ast::Module& module)
    {
        std::map<std::string, SourceLocation> seen;
        std::set<std::string> members;
        for (const ast::Field& field : module.fields)
        {
            if (!members.insert(field.name).second)
                continue;
            const ast::Type type = fieldType(field);
            const ast::Type element = ast::isArray(type) ? ast::elementOf(type) : type;
            if (lastNamed(field, "json::skip") != nullptr || ast::isPointer(type) || ast::isHandle(element))
                continue;
            const ast::Attribute* named = lastNamed(field, "json::name");
            const bool renamed = named != nullptr && nameOf(*named).has_value();
            const SourceLocation where = renamed ? named->location : field.location;
            const std::string name = jsonName(field);
            if (const auto [previous, added] = seen.try_emplace(name, where); !added)
                diagnostics_.error(where, "duplicate JSON name " + quoted(name) + " in module " + quoted(module.name),
                                   {Diagnostic{previous->second, "previous field of that JSON name is here", {}}});
        }
    }
} // namespace fminus::ir::detail
