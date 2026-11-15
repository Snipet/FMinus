// src/ir/Shapes.cpp: shapes, the runtime's description of a value in memory, which its JSON functions walk
// (runtime/Runtime.h, "shapes"; runtime/JsonShapes.h checks one before it reads a byte by it). A type is made into
// words here, once, when the program is compiled: a float is 1, a float[4] is 9, 4, 4, 1 (an array of 4, 4 bytes
// apart, of floats), a module is 12 and its fields, each a record of its JSON name, its offset, its range and default
// and its own shape, nested modules inside it. So the runtime walks a constant table and never needs the program's
// types.
//
// json_read(doc, "path", ..., &preset) passes its module type's table (moduleShape): one private constant a type,
// json.shape.Preset, made at the first call that names the type. A JSON literal (lowerJsonLiteral) is described from
// the same shapes, appendShape's, for the runtime's writer: its keys and punctuation as bytes, and each value an
// operation of its own, the value in 8-byte slots of the frame (a number, a bool, an enumeration's value, a string, a
// vector), or its address there with a pointer shape around its own (an array, a module), or an address and a count
// with a span shape (json_slice). So the table of a module type is the same words whether it is read into or written
// from, but for a string field, which the writer takes and the reader leaves out (ShapeUse). A module's parameter is
// the host's value, which only the module's own methods read: a literal writes one as *this, in such a method, each
// field an operation of its own and each parameter its value there (describeJsonThis), and refuses one elsewhere.
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

#include <algorithm>

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
            ShapePointer = 8,
            ShapeArray = 9,
            ShapeSpan = 10,
            ShapeVector = 11,
            ShapeModule = 12,
        };

        // A JSON literal's operations (runtime/Runtime.h, fminus_rt_json_write): bytes as they are, and a value.
        enum : std::int32_t
        {
            WriteBytes = 0,
            WriteValue = 1,
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

        // A key of a JSON literal (or a field's JSON name, in a module written field by field), appended to `out` as
        // a JSON string's contents (RFC 8259, §7: '"', '\\' and the control characters escaped, as the writer escapes
        // a string value and a module's names). The offset of a byte that is no part of UTF-8 text, which no JSON
        // string may hold, or none, and then nothing appended.
        std::optional<std::size_t> jsonKey(std::string_view key, std::string& out)
        {
            if (const std::optional<std::size_t> bad = notUtf8(key))
                return bad;
            constexpr std::string_view kHex = "0123456789abcdef";
            for (const char c : key)
            {
                const auto byte = static_cast<unsigned char>(c);
                switch (c)
                {
                case '"':
                    out += "\\\"";
                    break;
                case '\\':
                    out += "\\\\";
                    break;
                case '\b':
                    out += "\\b";
                    break;
                case '\f':
                    out += "\\f";
                    break;
                case '\n':
                    out += "\\n";
                    break;
                case '\r':
                    out += "\\r";
                    break;
                case '\t':
                    out += "\\t";
                    break;
                default:
                    if (byte < 0x20)
                    {
                        out += "\\u00";
                        out += kHex[byte >> 4];
                        out += kHex[byte & 15];
                    }
                    else
                        out += c;
                    break;
                }
            }
            return std::nullopt;
        }

        // Where the `offset`-th byte of a literal is spelt, or `where`.
        SourceLocation byteAt(const ast::StringExpr& literal, std::size_t offset, SourceLocation where)
        {
            return offset < literal.at.size() ? literal.at[offset] : where;
        }

        // Whether `expr` is `*this`.
        bool isThis(const ast::Expr& expr)
        {
            const auto* unary = std::get_if<ast::UnaryExpr>(&expr.node);
            return unary != nullptr && unary->op == ast::UnaryOp::Dereference
                   && std::holds_alternative<ast::ThisExpr>(unary->operand->node);
        }

        constexpr const char* kJsonValues = "a JSON literal's value is a float, a double, an int, a bool, a string, an "
                                            "enumeration's value, a float2, float4 or float8, an array or a module, or "
                                            "an object or an array of them, not ";
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

    // A parameter in a value of `type` that a JSON literal would write: a module's field that is one (and has no
    // [[json::skip]]), in it, in a module of its fields or in the modules an array holds, named "'cutoff' of 'Osc'";
    // or none. Only a method of its module reads a parameter (cutoff[i]: Modules.cpp), so a literal writes one only
    // there (describeJsonThis).
    std::optional<std::string> Lowering::parameterIn(ast::Type type) const
    {
        if (ast::isArray(type))
            return parameterIn(ast::elementOf(type));
        if (!ast::isModule(type) || type.module == nullptr)
            return std::nullopt;
        for (const ast::Field& field : type.module->fields)
        {
            if (lastNamed(field, "json::skip") != nullptr)
                continue;
            if (field.isParam)
                return quoted(field.name) + " of " + quoted(type.module->name);
            if (std::optional<std::string> inner = parameterIn(fieldType(field)))
                return inner;
        }
        return std::nullopt;
    }

    // A JSON literal lowered where it is written: its description (describeJson), a private constant laid out as a
    // shape's table, `.json`, and its values stored into 8-byte slots of the frame, `json.values`, in the order they
    // were lowered, the order written. Nothing for a literal in error, which reported why.
    std::optional<Lowering::JsonLiteral> Lowering::lowerJsonLiteral(const ast::Expr& literal)
    {
        JsonPlan plan;
        const bool fine = describeJson(std::get<ast::JsonExpr>(literal.node).value, plan);
        plan.flush();
        if (!fine)
            return std::nullopt;
        llvm::Value* values = llvm::ConstantPointerNull::get(builder_.getPtrTy());
        if (plan.slots > 0)
        {
            llvm::AllocaInst* slots =
                allocate(llvm::ArrayType::get(builder_.getInt64Ty(), static_cast<std::uint64_t>(plan.slots)),
                         "json.values");
            slots->setAlignment(llvm::Align(8));
            const auto at = [&](std::int32_t slot, unsigned offset) {
                return builder_.CreateConstInBoundsGEP1_32(builder_.getInt8Ty(), slots,
                                                           static_cast<unsigned>(8 * slot) + offset);
            };
            for (const JsonPlan::Slot& value : plan.values)
            {
                llvm::Value* first = at(value.slot, 0);
                if (value.count != nullptr)   // a span's address and its count
                {
                    tag(builder_.CreateStore(value.ir, first), value.type);
                    llvm::Value* second = at(value.slot, 8);
                    tag(builder_.CreateStore(value.count, second), ast::Type::Int);
                }
                else if (value.type == ast::Type::String)   // its pointer and its length
                {
                    llvm::Value* data = builder_.CreateExtractValue(value.ir, 0);
                    tag(builder_.CreateStore(data, first), ast::Type::pointerTo(ast::Kind::Char, true));
                    llvm::Value* size = builder_.CreateExtractValue(value.ir, 1);
                    llvm::Value* second = at(value.slot, 8);
                    tag(builder_.CreateStore(size, second), ast::Type::Int);
                }
                else if (value.type.kind == ast::Kind::Bool)   // one byte, 0 or 1 (an enumeration of bool's too)
                {
                    llvm::Value* byte = builder_.CreateZExt(value.ir, builder_.getInt8Ty());
                    tag(builder_.CreateStore(byte, first), value.type);
                }
                else if (ast::isVector(value.type))   // its lanes, at a slot's alignment
                    tag(builder_.CreateAlignedStore(value.ir, first, llvm::Align(8)), value.type);
                else
                    tag(builder_.CreateStore(value.ir, first), value.type);
            }
            values = slots;
        }
        return JsonLiteral{shapeConstant(plan.table, ".json"), values};
    }

    // The bytes not yet in an operation, as one: `0, offset, count` of the description's bytes.
    void Lowering::JsonPlan::flush()
    {
        if (pending.empty())
            return;
        table.code.insert(table.code.end(), {WriteBytes, static_cast<std::int32_t>(table.bytes.size()),
                                             static_cast<std::int32_t>(pending.size())});
        table.bytes += pending;
        pending.clear();
    }

    // A JSON literal's value added to `plan`: an object's and an array's text and its keys, escaped, as bytes, and each
    // value an operation of its own (describeJsonValue), lowered in the order written. Each error once, where it is;
    // false after one.
    bool Lowering::describeJson(const ast::JsonValue& value, JsonPlan& plan)
    {
        switch (value.kind)
        {
        case ast::JsonValue::Kind::Value:
            return describeJsonValue(*value.value, plan);
        case ast::JsonValue::Kind::Object:
        {
            bool fine = true;
            plan.pending += '{';
            std::map<std::string, SourceLocation> seen;
            for (std::size_t i = 0; i < value.elements.size(); ++i)
            {
                const ast::StringExpr& key = value.keys[i];
                const SourceLocation where = value.keyLocations[i];
                if (const auto [previous, added] = seen.try_emplace(key.value, where); !added)
                {
                    diagnostics_.error(where, "duplicate key " + quoted(key.value) + " in a JSON object",
                                       {Diagnostic{previous->second, "previous key is here", {}}});
                    fine = false;
                }
                plan.pending += i > 0 ? ",\"" : "\"";
                if (const std::optional<std::size_t> bad = jsonKey(key.value, plan.pending))
                {
                    error(byteAt(key, *bad, where), "a JSON key is UTF-8 text, and this byte is no part of any");
                    fine = false;
                }
                plan.pending += "\":";
                fine = describeJson(value.elements[i], plan) && fine;
            }
            plan.pending += '}';
            return fine;
        }
        case ast::JsonValue::Kind::Array:
        {
            bool fine = true;
            plan.pending += '[';
            for (std::size_t i = 0; i < value.elements.size(); ++i)
            {
                if (i > 0)
                    plan.pending += ',';
                fine = describeJson(value.elements[i], plan) && fine;
            }
            plan.pending += ']';
            return fine;
        }
        }
        return false;
    }

    // One value of a JSON literal, lowered where it is written: its operation, `1, slot, shape`, and its slots. A
    // float, a double, an int, a bool, an enumeration's value (written as its constant's name), a string (a literal is
    // one here), a float2, float4 or float8 (its lanes), each its own shape (appendShape), in slots of its own; an
    // array (a char array a string), or a module (a value, or *p), by its address in a slot and a pointer shape around
    // its own; json_slice(array, count) (describeJsonSlice). A module with a parameter only as *this in its own method
    // (describeJsonThis). A pointer, a handle, a poly type or a bool vector is an error, as is an array of them.
    bool Lowering::describeJsonValue(const ast::Expr& expr, JsonPlan& plan)
    {
        const ast::Module* module = function_ != nullptr ? function_->module : nullptr;
        if (const auto* call = std::get_if<ast::CallExpr>(&expr.node);
            call != nullptr && call->callee == "json_slice"
            && !(module != nullptr && functions_.contains(module->name + "::" + call->callee)))
            return describeJsonSlice(expr.location, *call, plan);
        const std::optional<ast::Type> declared = staticType(expr);
        if (declared && ast::isArray(*declared))
        {
            const Value array = lower(expr);   // its first element's address, the array's own
            if (array.failed)
                return false;
            if (const std::optional<std::string> parameter = parameterIn(*declared))
            {
                error(begin(expr), "a JSON literal writes a parameter's value only where a method of its module reads "
                                   "it, as *this: " + *parameter + " is one (leave it out with [[json::skip]])");
                return false;
            }
            const std::int32_t slot = plan.slots;
            if (!addJsonShape(begin(expr), *declared, true, plan))
                return false;
            plan.values.push_back({slot, array.ir, ast::Type::pointerTo(ast::Kind::Void, true)});
            plan.slots += 1;
            return true;
        }
        const Value lowered = lowerAs(expr, ast::Type::String);
        if (lowered.failed)
            return false;
        const ast::Type type = lowered.type;
        if (ast::isModule(type))
        {
            if (const std::optional<std::string> parameter = parameterIn(type))
            {
                if (isThis(expr) && type.module == module)
                    return describeJsonThis(begin(expr), *module, plan);
                error(begin(expr), "a JSON literal writes a parameter's value only where a method of its module reads "
                                   "it, as *this: " + *parameter + " is one (leave it out with [[json::skip]])");
                return false;
            }
            const std::int32_t slot = plan.slots;
            if (!addJsonShape(begin(expr), type, true, plan))
                return false;
            plan.values.push_back({slot, lowered.ir, ast::Type::pointerTo(ast::Kind::Void, true)});
            plan.slots += 1;
            return true;
        }
        if (ast::isPointer(type))
        {
            error(begin(expr), "a JSON literal does not write a pointer, " + quoted(ast::typeName(type))
                                   + ": write what it points to, *p, or its first elements, json_slice(p, count)");
            return false;
        }
        const bool vector =
            type.kind == ast::Kind::Float2 || type.kind == ast::Kind::Float4 || type.kind == ast::Kind::Float8;
        const bool scalar = type.kind == ast::Kind::Float || type.kind == ast::Kind::Double
                            || type.kind == ast::Kind::Int || type.kind == ast::Kind::Bool
                            || type == ast::Type::String;
        if (!vector && !scalar)
        {
            error(begin(expr), kJsonValues + quoted(ast::typeName(type)));
            return false;
        }
        const std::int32_t slot = plan.slots;
        if (!addJsonShape(begin(expr), type, false, plan))
            return false;
        plan.values.push_back({slot, lowered.ir, type});
        const std::uint64_t bytes = sizeOf(type).value_or(8);
        plan.slots += type == ast::Type::String ? 2 : static_cast<std::int32_t>(std::max<std::uint64_t>(1, bytes / 8));
        return true;
    }

    // Appends `1, slot, shape` for a value of `type` at the plan's next slot, with a pointer shape around its own when
    // the slot holds its `address` (an array's, a module's). An error at `location` for a type with no shape or one
    // the writer cannot take, and false.
    bool Lowering::addJsonShape(SourceLocation location, ast::Type type, bool address, JsonPlan& plan)
    {
        plan.flush();
        std::vector<std::int32_t>& code = plan.table.code;
        const std::size_t codeAt = code.size();
        const std::size_t bytesAt = plan.table.bytes.size();
        code.insert(code.end(), {WriteValue, plan.slots});
        if (address)
            code.push_back(ShapePointer);
        const ShapeProblem problem = appendShape(type, ShapeUse::Write, plan.table, address ? 1 : 0);
        if (problem == ShapeProblem::None)
            return true;
        code.resize(codeAt);
        plan.table.bytes.resize(bytesAt);
        const std::string cannot = "a JSON literal cannot write " + quoted(ast::typeName(type));
        switch (problem)
        {
        case ShapeProblem::NoShape:
            error(location, kJsonValues + quoted(ast::typeName(type)));
            break;
        case ShapeProblem::TooDeep:
            error(location, cannot + ": its modules and arrays nest more than 16 deep, the most the writer takes");
            break;
        case ShapeProblem::TooManyFields:
            error(location, cannot + ": a module in it has more than 1024 fields to write, the most the writer takes");
            break;
        case ShapeProblem::TooLarge:
            error(location, cannot + ": it is larger than its description can say, 2 GB");
            break;
        case ShapeProblem::None:
            break;
        }
        return false;
    }

    // `json_slice(array, count)`, a JSON literal's value: the array's first `count` elements, a span (an address and
    // a count in two slots, the count clamped by the writer to [0, capacity]). Of an array, its size is the capacity;
    // of a pointer, the program vouches for the elements, as it does for bytes_floats', and the capacity is an int's
    // greatest, so that the count alone bounds them. Its elements are anything a literal writes but a char (a char
    // array is a string, written whole).
    bool Lowering::describeJsonSlice(SourceLocation location, const ast::CallExpr& call, JsonPlan& plan)
    {
        if (call.args.size() != 2)
        {
            wrongArity(location, call.args.size(), 2);
            return false;
        }
        const ast::Expr& base = *call.args[0];
        if (isString(base))
        {
            error(begin(base), "'json_slice' takes an array or a pointer, and a count: not a string literal");
            return false;
        }
        const std::optional<ast::Type> declared = staticType(base);
        const Value elements = lower(base);
        if (elements.failed)
            return false;
        ast::Type element;
        std::int32_t capacity = std::numeric_limits<std::int32_t>::max();
        if (declared && ast::isArray(*declared))
        {
            element = ast::pointee(*declared);
            capacity = declared->count;
        }
        else if (ast::isPointer(elements.type) && elements.type.pointee != ast::Kind::Void)
            element = ast::pointee(elements.type);
        else
        {
            error(begin(base), "'json_slice' takes an array or a pointer, and a count: not "
                                   + quoted(ast::typeName(elements.type)));
            return false;
        }
        if (element.kind == ast::Kind::Char)
        {
            error(begin(base), "'json_slice' writes elements a JSON literal writes, and a char is none: a char array "
                               "is a string, written whole");
            return false;
        }
        const Value count = isString(*call.args[1])
                                ? refuseString(*call.args[1], ast::Type::Int, Use::Passing, begin(*call.args[1]))
                                : lowerAs(*call.args[1], ast::Type::Int);
        if (count.failed)
            return false;
        llvm::Value* counted = convert(count, ast::Type::Int, begin(*call.args[1]), Use::Passing);
        if (const std::optional<std::string> parameter = parameterIn(element))
        {
            error(begin(base), "a JSON literal writes a parameter's value only where a method of its module reads it, "
                               "as *this: " + *parameter + " is one (leave it out with [[json::skip]])");
            return false;
        }
        const std::optional<std::uint64_t> stride = sizeOf(element);
        if (!stride || *stride > kMostWord)
        {
            error(begin(base), "a JSON literal cannot write " + quoted(ast::typeName(element))
                                   + ": it is larger than its description can say, 2 GB");
            return false;
        }
        plan.flush();
        std::vector<std::int32_t>& code = plan.table.code;
        const std::size_t codeAt = code.size();
        code.insert(code.end(), {WriteValue, plan.slots, ShapeSpan, capacity, static_cast<std::int32_t>(*stride)});
        const std::size_t bytesAt = plan.table.bytes.size();
        if (const ShapeProblem problem = appendShape(element, ShapeUse::Write, plan.table, 1);
            problem != ShapeProblem::None)
        {
            code.resize(codeAt);
            plan.table.bytes.resize(bytesAt);
            if (problem == ShapeProblem::NoShape)
                error(begin(base), kJsonValues + quoted(ast::typeName(element)));
            else
                error(begin(base), "a JSON literal cannot write " + quoted(ast::typeName(element))
                                       + ": its modules and arrays nest more deeply, or it has more fields, than the "
                                         "writer takes");
            return false;
        }
        plan.values.push_back({plan.slots, elements.ir, ast::Type::pointerTo(ast::Kind::Void, true), counted});
        plan.slots += 2;
        return true;
    }

    // `*this`, in a method of a module that has a parameter: an object of its fields, in their order, under their
    // JSON names, each an operation of its own, since the writer reads memory and a parameter's value is not in the
    // module's (a knob's is the host's to change, a frozen one's a constant, a stream's a sample of the call): each
    // field that a module's shape has, by its address and a pointer shape around its own; each parameter as the
    // method reads it at its first sample, name[0] (Modules.cpp, lowerParameter). The same fields as the module's
    // shape, and the same text the writer would make of them, with the parameters among them.
    bool Lowering::describeJsonThis(SourceLocation location, const ast::Module& module, JsonPlan& plan)
    {
        bool fine = true;
        bool first = true;
        plan.pending += '{';
        for (std::size_t i = 0; i < module.fields.size(); ++i)
        {
            const ast::Field& field = module.fields[i];
            if (lastNamed(field, "json::skip") != nullptr)
                continue;
            const ast::Type type = fieldType(field);
            if (!field.isParam)
            {
                ShapeTable probe;   // a field the module's shape leaves out (a pointer, a handle) is left out here
                if (appendShape(type, ShapeUse::Write, probe, 1) == ShapeProblem::NoShape)
                    continue;
            }
            plan.pending += first ? "\"" : ",\"";
            first = false;
            jsonKey(jsonName(field), plan.pending);   // a json::name is UTF-8, or not used (checkJsonAttribute)
            plan.pending += "\":";
            if (field.isParam)
            {
                const ast::IndexExpr index{ast::makeExpr(location, ast::VariableExpr{field.name}),
                                           ast::makeExpr(location, ast::NumberExpr{"0"})};
                const Value value = lowerParameter(location, static_cast<unsigned>(i), index);
                const std::int32_t slot = plan.slots;
                if (value.failed || !addJsonShape(location, value.type, false, plan))
                {
                    fine = false;
                    continue;
                }
                plan.values.push_back({slot, value.ir, value.type});
                plan.slots += 1;
                continue;
            }
            if (const std::optional<std::string> parameter = parameterIn(type))
            {
                error(location, "a JSON literal writes a parameter's value only where a method of its module reads "
                                "it, as *this: " + *parameter + " is one (leave it out with [[json::skip]])");
                fine = false;
                continue;
            }
            llvm::Value* address = builder_.CreateStructGEP(structs_.at(&module), self(), static_cast<unsigned>(i),
                                                            field.name);
            const std::int32_t slot = plan.slots;
            if (!addJsonShape(location, type, true, plan))
            {
                fine = false;
                continue;
            }
            plan.values.push_back({slot, address, ast::Type::pointerTo(ast::Kind::Void, true)});
            plan.slots += 1;
        }
        plan.pending += '}';
        return fine;
    }
} // namespace fminus::ir::detail
