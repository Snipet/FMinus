// src/ir/Csv.cpp: the CSV reader's part of the code generator (its builtins' table is Calls.cpp's, the reader
// runtime/Csv.cpp; docs/tables.md). In every build:
//
// - The status codes are names a program uses without declaring them, CSV_MISSING (-1: a column the header does not
//   have) and CSV_INVALID (-2: a table or an argument the reader refuses), as FILE_MISSING is (Files.cpp).
// - A column is an int, its index from 0, or a string, its name (a literal among them), passed to the runtime as an
//   index, then a name's bytes and their count: (k, null, 0) or (-1, data, size) (lowerColumn).
// - csv_parse's delimiter is a string literal of one byte, not a quote, a line end or NUL, passed as an int; left out,
//   a comma (Calls.cpp, lowerRuntimeCall).
// - csv_read(doc, rows, cap) fills an array of a module, a row each, by a table of the module type's columns
//   (csvShape): runtime/JsonShapes.h's module shape, its fields flattened to one record a column, each a leaf (a
//   float, a double, an int, a bool, an enumeration's value or a char array) at its offset in the module, under its
//   column's name. A field's name is its own, or its [[csv::name("...")]]; a field of a nested module is
//   "inner.field"; an array's element and a vector's lane is "name[k]" ("name[i][j]" for a 2-D array's), or, for a
//   1-D array or a vector, the k-th of its csv::name's names when it gives one for each: names C's syntax spells, which
//   no field's own can collide with. A field's fminus::range and fminus::default apply to each of its columns; its
//   [[csv::category]] (an int field's, or an array of ints') makes each column's cells category codes, each distinct
//   text a number from 0 in the order the rows first have it. Left out, as json_read leaves them: a parameter, a field
//   with [[csv::skip]], a string, a pointer, a handle and a poly type. The table is one private constant a type, made
//   at its first call; the call passes it, the rows' address and the module's size.
#include "ir/Lowering.h"

#include <llvm/IR/Constants.h>
#include <llvm/IR/DataLayout.h>

#include <array>
#include <bit>
#include <cstdint>
#include <format>
#include <limits>
#include <map>
#include <string>

namespace fminus::ir::detail
{
    namespace
    {
        struct CsvStatus
        {
            std::string_view name;
            std::int32_t value;
        };

        // runtime/Csv.cpp's kMissing and kInvalid.
        constexpr std::array kCsvStatuses{CsvStatus{"CSV_MISSING", -1}, CsvStatus{"CSV_INVALID", -2}};

        // runtime/JsonShapes.h's words: a module's kind, and its fields' flags.
        enum : std::int32_t
        {
            ShapeModule = 12,
        };
        enum : std::int32_t
        {
            FieldRange = 1,
            FieldDefault = 2,
            FieldCategory = 4,
        };

        // The runtime's bounds: a module's table has at most 1024 records, and a word is an int32.
        constexpr std::int32_t kMostColumns = 1024;
        constexpr std::uint64_t kMostWord = std::numeric_limits<std::int32_t>::max();

        void addDouble(std::vector<std::int32_t>& code, double value)
        {
            const auto bits = std::bit_cast<std::uint64_t>(value);
            code.push_back(static_cast<std::int32_t>(static_cast<std::uint32_t>(bits & 0xffffffffu)));
            code.push_back(static_cast<std::int32_t>(static_cast<std::uint32_t>(bits >> 32)));
        }

        void addName(ShapeTable& table, std::string_view text)
        {
            table.code.push_back(static_cast<std::int32_t>(table.bytes.size()));
            table.code.push_back(static_cast<std::int32_t>(text.size()));
            table.bytes += text;
        }

        // The last attribute of `name` a field has, or null: the one that counts.
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

        // The numbers of the last attribute of `name` that has `count` of them and nothing else, or none.
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

        // A csv::name's names, when every argument is a string of a byte or more (checkCsvAttribute reports any
        // other).
        std::optional<std::vector<std::string>> namesOf(const ast::Attribute& attribute)
        {
            if (attribute.args.empty())
                return std::nullopt;
            std::vector<std::string> names;
            for (const ast::AttributeArg& arg : attribute.args)
            {
                if (!arg.isString || arg.text.empty())
                    return std::nullopt;
                names.push_back(arg.text);
            }
            return names;
        }

        // How many elements a field gives one name each: a 1-D array's (not of chars, which is one text) or a
        // vector's lanes; 0 for anything else.
        std::size_t elementsNamed(ast::Type type)
        {
            if (ast::isArray(type))
            {
                const ast::Type element = ast::pointee(type);
                if (element.kind == ast::Kind::Char || ast::isArray(element) || ast::isModule(element))
                    return 0;
                return static_cast<std::size_t>(type.count);
            }
            if (type.kind == ast::Kind::Float2 || type.kind == ast::Kind::Float4 || type.kind == ast::Kind::Float8)
                return ast::lanes(type);
            return 0;
        }

        // Whether a field's values are ints (an array's elements too), not an enumeration's: what a category is.
        bool intValues(ast::Type type)
        {
            const ast::Type element = ast::isArray(type) ? ast::elementOf(type) : type;
            return element.kind == ast::Kind::Int && element.enumeration == 0;
        }
    } // namespace

    // The status code `name` names, as an enumeration constant's binding, or nullptr.
    const Lowering::Binding* Lowering::csvStatus(const std::string& name) const
    {
        static const std::array<Binding, kCsvStatuses.size()> kBindings = [] {
            std::array<Binding, kCsvStatuses.size()> bindings{};
            for (std::size_t i = 0; i < kCsvStatuses.size(); ++i)
                bindings[i] = Binding{nullptr, true, ast::Type::Int, {}, kCsvStatuses[i].value};
            return bindings;
        }();
        for (std::size_t i = 0; i < kCsvStatuses.size(); ++i)
        {
            if (kCsvStatuses[i].name == name)
                return &kBindings[i];
        }
        return nullptr;
    }

    // A csv:: attribute of a module's field or parameter (checkAttributes, Modules.cpp): csv::name("name"), the
    // column the field is read from, or one name for each element of a 1-D array or lane of a vector; csv::skip, no
    // argument, a field csv_read leaves alone; csv::category, no argument, an int field (or an array of ints) whose
    // values are a column's category codes. Another name is unknown: the namespace is FMinus's. Each is passed to the
    // host as written all the same.
    void Lowering::checkCsvAttribute(const ast::Attribute& attribute, const ast::Field& field, ast::Type type)
    {
        if (attribute.name == "csv::name")
        {
            const std::size_t elements = elementsNamed(type);
            if (attribute.args.empty())
                error(attribute.location, "'csv::name' attribute takes a name");
            else if (!namesOf(attribute))
                error(attribute.location, "'csv::name' takes names, each a string of one byte or more: the column the "
                                          "field is read from");
            else if (attribute.args.size() != 1 && attribute.args.size() != elements)
                error(attribute.location,
                      elements == 0 ? std::format("'csv::name' of {} takes one name, the column it is read from",
                                                  quoted(field.name))
                                    : std::format("'csv::name' of {} takes one name, or one for each of its {} "
                                                  "elements, not {}",
                                                  quoted(field.name), elements, attribute.args.size()));
        }
        else if (attribute.name == "csv::skip")
        {
            if (!attribute.args.empty())
                error(attribute.location, "'csv::skip' attribute takes no arguments");
        }
        else if (attribute.name == "csv::category")
        {
            if (!attribute.args.empty())
                error(attribute.location, "'csv::category' attribute takes no arguments");
            else if (!intValues(type))
                error(attribute.location, "'csv::category' applies to a field of ints, which hold a column's category "
                                          "codes, not " + quoted(ast::typeName(type))
                                          + (type.enumeration != 0 ? ": an enumeration reads its constants' names "
                                                                     "without it"
                                                                   : ""));
        }
        else
            error(attribute.location, "unknown attribute " + quoted(attribute.name)
                                          + ": a field takes csv::name(\"name\"), csv::skip and csv::category");
    }

    // A leaf's column: its record (its size, its name, its offset, its flags, its range and default, its shape) at the
    // end of `table`, `count` one more; false after an error (a name a column of the module has already, an offset
    // past an int32).
    bool Lowering::csvLeaf(const ast::Field& field, ast::Type leaf, std::uint64_t offset, const std::string& name,
                           ShapeTable& table, std::int32_t& count, std::map<std::string, SourceLocation>& names)
    {
        if (const auto [previous, added] = names.try_emplace(name, field.location); !added)
        {
            diagnostics_.error(field.location,
                               "duplicate CSV column " + quoted(name) + " in a module that csv_read fills",
                               {Diagnostic{previous->second, "previous field of that column is here", {}}});
            return false;
        }
        if (offset > kMostWord)
        {
            error(field.location,
                  "the column " + quoted(name) + " is past 2 GB into its module, past what csv_read reads");
            return false;
        }
        std::vector<std::int32_t>& code = table.code;
        const std::size_t at = code.size();
        code.push_back(0);   // its size, once its shape is there
        addName(table, name);
        code.push_back(static_cast<std::int32_t>(offset));
        const std::size_t flagsAt = code.size();
        code.push_back(0);
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
        if (lastNamed(field, "csv::category") != nullptr && leaf.kind == ast::Kind::Int && leaf.enumeration == 0)
            flags |= FieldCategory;
        code[flagsAt] = flags;
        appendShape(leaf, ShapeUse::Read, table, 1);   // a leaf's: a kind and its words, never a problem
        code[at] = static_cast<std::int32_t>(code.size() - at);
        ++count;
        return true;
    }

    // The columns of a value of `type` at `offset` in the rows' module, named `name`: a leaf's one, an array's and a
    // vector's each element's (named by `listed` when it is given, else "name[k]"), a module's each field's
    // ("name.field"). None for a type csv_read leaves out. False after an error.
    bool Lowering::csvElements(const ast::Field& field, ast::Type type, std::uint64_t offset, const std::string& name,
                               const std::vector<std::string>* listed, ShapeTable& table, std::int32_t& count,
                               std::map<std::string, SourceLocation>& names, int depth)
    {
        const auto elementName = [&](std::size_t k) {
            return listed != nullptr && k < listed->size() ? (*listed)[k] : name + "[" + std::to_string(k) + "]";
        };
        if (ast::isArray(type))
        {
            const ast::Type element = ast::pointee(type);   // a row, for a 2-D array
            if (element.kind == ast::Kind::Char)
                return csvLeaf(field, type, offset, name, table, count, names);
            const std::optional<std::uint64_t> stride = sizeOf(element);
            if (!stride)
                return true;
            for (std::size_t k = 0; k < static_cast<std::size_t>(type.count); ++k)
            {
                if (!csvElements(field, element, offset + k * *stride, elementName(k), nullptr, table, count, names,
                                 depth))
                    return false;
                if (count > kMostColumns)
                    return true;   // csvShape says so
            }
            return true;
        }
        switch (type.kind)
        {
        case ast::Kind::Float2:
        case ast::Kind::Float4:
        case ast::Kind::Float8:
            for (std::size_t k = 0; k < ast::lanes(type); ++k)
            {
                if (!csvLeaf(field, ast::Type::Float, offset + 4 * k, elementName(k), table, count, names))
                    return false;
            }
            return true;
        case ast::Kind::Float:
        case ast::Kind::Double:
        case ast::Kind::Int:
        case ast::Kind::Bool:
            return csvLeaf(field, type, offset, name, table, count, names);
        case ast::Kind::Module:
            if (depth >= 16)
                return true;   // past a JSON table's depth too: csvShape says so
            return csvColumns(*type.module, offset, name + ".", table, count, names, depth + 1);
        default:
            return true;   // a string, a pointer, a handle, a poly type: no column
        }
    }

    // The columns of `module`'s fields, at `base` in the rows' module, their names after `prefix`.
    bool Lowering::csvColumns(const ast::Module& module, std::uint64_t base, const std::string& prefix,
                              ShapeTable& table, std::int32_t& count, std::map<std::string, SourceLocation>& names,
                              int depth)
    {
        const llvm::StructLayout* layout = module_.getDataLayout().getStructLayout(structs_.at(&module));
        for (std::size_t i = 0; i < module.fields.size(); ++i)
        {
            const ast::Field& field = module.fields[i];
            if (field.isParam || lastNamed(field, "csv::skip") != nullptr)
                continue;
            const ast::Type type = fieldType(field);
            const std::uint64_t offset = base + layout->getElementOffset(static_cast<unsigned>(i)).getFixedValue();
            std::string name = field.name;
            std::optional<std::vector<std::string>> listed;
            if (const ast::Attribute* named = lastNamed(field, "csv::name"))
            {
                listed = namesOf(*named);
                if (listed && listed->size() == 1)
                {
                    name = listed->front();
                    listed.reset();
                }
                else if (listed && listed->size() != elementsNamed(type))
                    listed.reset();   // checkCsvAttribute reported it
            }
            if (!csvElements(field, type, offset, prefix + name, listed ? &*listed : nullptr, table, count, names,
                             depth))
                return false;
            if (count > kMostColumns)
                return true;
        }
        return true;
    }

    // csv_read's table of `module`: one module shape of its columns, made at the type's first call and the same
    // constant for every call after it. Null, with the error at the call `location`, for a module csv_read cannot
    // fill: one with no column, or more than the runtime's 1024.
    llvm::Value* Lowering::csvShape(SourceLocation location, const ast::Module& module)
    {
        if (const auto found = csvShapes_.find(&module); found != csvShapes_.end())
            return found->second;
        ShapeTable table;
        table.code.push_back(ShapeModule);
        table.code.push_back(0);
        std::int32_t count = 0;
        std::map<std::string, SourceLocation> names;
        const std::string cannot = "'csv_read' cannot fill " + quoted(module.name);
        const std::size_t reported = diagnostics_.errors().size();
        if (!csvColumns(module, 0, "", table, count, names, 0))
        {
            if (diagnostics_.errors().size() == reported)
                error(location, cannot + ": its table would be past what the runtime reads");
            csvShapes_.emplace(&module, nullptr);
            return nullptr;
        }
        if (count == 0 || count > kMostColumns)
        {
            error(location, count == 0 ? cannot + ": it has no field csv_read reads (a number, a bool, an "
                                                  "enumeration's value or a char array, or an array or a module of "
                                                  "them, without [[csv::skip]])"
                                       : std::format("{}: its fields make more than {} columns", cannot, kMostColumns));
            csvShapes_.emplace(&module, nullptr);
            return nullptr;
        }
        table.code[1] = count;
        llvm::GlobalVariable* global = shapeConstant(table, "csv.shape." + module.name);
        csvShapes_.emplace(&module, global);
        return global;
    }

    // csv_read's rows: a pointer to a module (not const: it writes the rows), passed as its type's table (csvShape),
    // its address and the module's size, the rows' stride.
    bool Lowering::lowerRows(SourceLocation location, const RuntimeBuiltin& builtin, const ast::Expr& arg,
                             std::vector<llvm::Value*>& irArgs)
    {
        const std::string must =
            quoted(builtin.name) + " fills an array of a module, a row each: its rows must be a pointer to one, not ";
        if (isString(arg))
        {
            error(begin(arg), must + "a string literal");
            return false;
        }
        const Value value = lower(arg);
        if (value.failed)
            return false;
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
        const std::optional<std::uint64_t> size = sizeOf(ast::Type::moduleType(&module));
        if (!size || *size > kMostWord)
        {
            error(begin(arg), "'csv_read' cannot fill " + quoted(module.name) + ": a row of it is past 2 GB");
            return false;
        }
        llvm::Value* shape = csvShape(location, module);
        if (shape == nullptr)
            return false;
        irArgs.push_back(shape);
        irArgs.push_back(pointer);
        irArgs.push_back(builder_.getInt32(static_cast<std::uint32_t>(*size)));
        return true;
    }

    // csv_parse's delimiter: a string literal of one byte, not a quote, a line end or NUL, passed as an int.
    bool Lowering::lowerDelimiter(const RuntimeBuiltin& builtin, const ast::Expr& arg,
                                  std::vector<llvm::Value*>& irArgs)
    {
        if (!isString(arg))
        {
            error(begin(arg), "argument to " + quoted(builtin.name) + " must be a string literal");
            return false;
        }
        const std::string& text = std::get<ast::StringExpr>(arg.node).value;
        if (text.size() != 1 || text[0] == '"' || text[0] == '\n' || text[0] == '\r' || text[0] == '\0')
        {
            error(begin(arg), "a delimiter is one byte, not a quote, a line end or NUL: \",\", \"\\t\" or \";\"");
            return false;
        }
        irArgs.push_back(builder_.getInt32(static_cast<unsigned char>(text[0])));
        return true;
    }

    // A column: an int, its index, passed as itself and no name; or a string, its name (a literal among them), passed
    // as -1 and its bytes and their count.
    bool Lowering::lowerColumn(const ast::Expr& arg, std::vector<llvm::Value*>& irArgs)
    {
        const Value value = isString(arg) ? lowerAs(arg, ast::Type::String) : lower(arg);
        if (value.failed)
            return false;
        if (value.type == ast::Type::String)
        {
            irArgs.push_back(builder_.getInt32(static_cast<std::uint32_t>(-1)));
            irArgs.push_back(builder_.CreateExtractValue(value.ir, 0, "column.name"));
            irArgs.push_back(builder_.CreateExtractValue(value.ir, 1, "column.size"));
            return true;
        }
        if (value.type.kind != ast::Kind::Int || ast::isPointer(value.type) || ast::isArray(value.type))
        {
            error(begin(arg), "a column is an int, its index from 0, or a string, its name, not "
                                  + quoted(ast::typeName(value.type)));
            return false;
        }
        irArgs.push_back(convert(value, ast::Type::Int, arg.location, Use::Passing));
        irArgs.push_back(llvm::ConstantPointerNull::get(builder_.getPtrTy()));
        irArgs.push_back(builder_.getInt32(0));
        return true;
    }
} // namespace fminus::ir::detail
