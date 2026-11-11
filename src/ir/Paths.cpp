// src/ir/Paths.cpp: paths into a JSON or an XML document, prepared when the program is compiled. json_float(doc,
// "/osc/%d/detune", k, 0.0f) names its value with a JSON Pointer (RFC 6901): the empty string for the whole document,
// or reference tokens, each after a '/', in which "~1" is a '/' and "~0" a '~'. The literal is also a format
// (Strings.cpp): a %d is the next int argument, written in decimal into its token (an array's index, or part of a
// member's name: "/osc_%d_level"), and %% a percent sign. FMinus parses it here, once, into a private constant table of
// steps (runtime/Runtime.h has its layout): each token's pieces, the literal's bytes with their escapes resolved and
// the places of its ints, so that the runtime never reads a path's text. A malformed path is an error at its own byte.
// json_choice's literals are prepared too, into one table of their lengths and bytes.
//
// xml_float(doc, "/preset/osc[%d]/@detune", k, 0.0f) names its value with an XML path, a small part of XPath's: child
// steps by element name from the root, the first naming the root element, each with an index in brackets or none, and
// a last step that may be an attribute's name after '@'. An index counts from 0, as C's arrays and JSON Pointer's do,
// and is digits or %d. It is prepared the same way (scanXmlPath), into a table of the same layout, and its errors are
// at their own bytes too; what XPath has beyond it (axes, predicates, wildcards, functions) is an error at its first
// byte, since an XML name holds none of it.
#include "ir/Lowering.h"

#include <llvm/IR/Constants.h>
#include <llvm/IR/GlobalVariable.h>

#include <algorithm>
#include <limits>

namespace fminus::ir::detail
{
    namespace
    {
        // A byte as clang shows one in a message (Strings.cpp's shown): itself when it is printable ASCII, else a hex
        // escape.
        std::string shownByte(char c)
        {
            const auto byte = static_cast<unsigned char>(c);
            if (byte >= 0x20 && byte < 0x7f)
                return std::string(1, c);
            constexpr std::string_view kHex = "0123456789abcdef";
            return std::string("\\x") + kHex[byte >> 4] + kHex[byte & 15];
        }

        // A private constant of the module: `words` int32s, then `bytes`, together at 4-byte alignment (the layout
        // of the runtime's tables), named `name`.
        llvm::GlobalVariable* table(llvm::Module& module, const std::vector<std::int32_t>& words,
                                    const std::string& bytes, const char* name)
        {
            llvm::LLVMContext& context = module.getContext();
            llvm::Type* int32 = llvm::Type::getInt32Ty(context);
            std::vector<std::uint32_t> code(words.begin(), words.end());
            llvm::Constant* fields[] = {
                llvm::ConstantInt::get(int32, words.size()),
                llvm::ConstantInt::get(int32, bytes.size()),
                llvm::ConstantDataArray::get(context, code),
                llvm::ConstantDataArray::getString(context, bytes, false),
            };
            llvm::Constant* value = llvm::ConstantStruct::getAnon(context, fields);
            auto* global = new llvm::GlobalVariable(module, value->getType(), true, llvm::GlobalValue::PrivateLinkage,
                                                    value, name);
            global->setUnnamedAddr(llvm::GlobalValue::UnnamedAddr::Global);
            global->setAlignment(llvm::Align(4));
            return global;
        }
    } // namespace

    PathScan scanPath(std::string_view text)
    {
        PathScan scan;
        if (text.empty())
            return scan;   // the whole document
        if (text[0] != '/')
        {
            scan.errors.push_back({0, "a JSON Pointer must be empty or begin with '/'"});
            return scan;
        }
        std::size_t step = 0;      // the current token's count, in `code`
        std::int32_t places = 0;   // %d's so far
        const auto literal = [&](char c) {
            // The token's last piece grows when it is the literal's bytes just before this one.
            const std::size_t size = scan.code.size();
            if (scan.code[step] > 0 && scan.code[size - 1] >= 0
                && static_cast<std::size_t>(scan.code[size - 2] + scan.code[size - 1]) == scan.bytes.size())
                ++scan.code[size - 1];
            else
            {
                ++scan.code[step];
                scan.code.push_back(static_cast<std::int32_t>(scan.bytes.size()));
                scan.code.push_back(1);
            }
            scan.bytes.push_back(c);
        };
        for (std::size_t i = 0; i < text.size(); ++i)
        {
            const char c = text[i];
            const bool next = i + 1 < text.size();
            if (c == '/')
            {
                step = scan.code.size();
                scan.code.push_back(0);
            }
            else if (c == '~' && next && (text[i + 1] == '0' || text[i + 1] == '1'))
                literal(text[++i] == '0' ? '~' : '/');
            else if (c == '~')
            {
                scan.errors.push_back(
                    {i, next ? "unknown escape sequence '~" + shownByte(text[i + 1])
                                   + "' in a JSON Pointer: '~0' is '~' and '~1' is '/'"
                             : std::string("incomplete escape sequence '~' at the end of a JSON Pointer: '~0' is '~' "
                                           "and '~1' is '/'")});
                if (next && text[i + 1] != '/')
                    ++i;
            }
            else if (c == '%' && next && text[i + 1] == 'd')
            {
                ++scan.code[step];
                scan.code.push_back(places++);
                scan.code.push_back(-1);
                ++i;
            }
            else if (c == '%' && next && text[i + 1] == '%')
            {
                literal('%');
                ++i;
            }
            else
                literal(c);   // a % before anything else too: scanFormat's error
        }
        return scan;
    }

    PathScan scanXmlPath(std::string_view text)
    {
        PathScan scan;
        const auto wrong = [&](std::size_t offset, std::string message) {
            scan.errors.push_back({offset, std::move(message)});
            return scan;
        };
        if (text.empty() || text[0] != '/')
            return wrong(0, "an XML path must begin with '/' and the root element's name");
        // A byte of an XML name, as XML 1.0 writes one: its letters, digits, '-', '.', '_' and ':', and any byte of a
        // character past ASCII. (A '%' is the format's: %d an int's digits, %% a byte '%', and anything else after one
        // scanFormat's error.)
        const auto named = [](char c) {
            const auto byte = static_cast<unsigned char>(c);
            return byte >= 0x80 || (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9')
                   || c == '-' || c == '.' || c == '_' || c == ':';
        };
        std::int32_t places = 0;   // %d's so far
        std::size_t i = 0;
        while (i < text.size())
        {
            const bool first = i++ == 0;   // past a '/'
            const bool attribute = i < text.size() && text[i] == '@';
            if (attribute && first)
                return wrong(i, "the first step of an XML path is the root element's name, not an attribute");
            if (attribute)
                ++i;
            const std::size_t kind = scan.code.size();
            scan.code.push_back(attribute ? 3 : 0);
            const std::size_t step = scan.code.size();   // the name's count of pieces
            scan.code.push_back(0);
            bool any = false;
            const auto literal = [&](char c) {
                // The name's last piece grows when it is the literal's bytes just before this one.
                const std::size_t size = scan.code.size();
                if (scan.code[step] > 0 && scan.code[size - 1] >= 0
                    && static_cast<std::size_t>(scan.code[size - 2] + scan.code[size - 1]) == scan.bytes.size())
                    ++scan.code[size - 1];
                else
                {
                    ++scan.code[step];
                    scan.code.push_back(static_cast<std::int32_t>(scan.bytes.size()));
                    scan.code.push_back(1);
                }
                scan.bytes.push_back(c);
            };
            while (i < text.size() && text[i] != '/' && text[i] != '[')
            {
                const char c = text[i];
                const bool next = i + 1 < text.size();
                if (c == '%' && next && text[i + 1] == 'd')
                {
                    ++scan.code[step];
                    scan.code.push_back(places++);
                    scan.code.push_back(-1);
                    i += 2;
                }
                else if (c == '%' && next && text[i + 1] == '%')
                {
                    literal('%');
                    i += 2;
                }
                else if (c == '%')
                    ++i;   // scanFormat's error
                else if (named(c))
                {
                    literal(c);
                    ++i;
                }
                else
                    return wrong(i, "unexpected '" + shownByte(c)
                                        + "' in an XML path: a step is a name, an index '[n]' after it, or a last "
                                          "'@' and an attribute's name");
                any = true;
            }
            if (!any)
            {
                const std::size_t at = i < text.size() ? i : i - 1;   // what follows, or the '/' or '@' at the end
                return wrong(at, attribute ? "expected an attribute's name after '@' in an XML path"
                                           : "expected an element's name after '/' in an XML path");
            }
            if (i < text.size() && text[i] == '[')
            {
                const std::size_t open = i++;
                if (attribute)
                    return wrong(open, "an attribute takes no index in an XML path");
                if (text.substr(i, 2) == "%d")
                {
                    scan.code[kind] = 2;
                    scan.code.push_back(places++);
                    i += 2;
                }
                else if (i < text.size() && text[i] >= '0' && text[i] <= '9')
                {
                    std::int64_t index = 0;
                    for (; i < text.size() && text[i] >= '0' && text[i] <= '9'; ++i)
                        index = std::min<std::int64_t>(index * 10 + (text[i] - '0'), std::int64_t{1} << 32);
                    if (index > std::numeric_limits<std::int32_t>::max())
                        return wrong(open + 1, "an index in an XML path must be at most 2147483647");
                    scan.code[kind] = 1;
                    scan.code.push_back(static_cast<std::int32_t>(index));
                }
                else
                    return wrong(i < text.size() ? i : open,
                                 "expected an index in '[ ]' in an XML path: digits, from 0, or %d");
                if (i == text.size() || text[i] != ']')
                    return wrong(i < text.size() ? i : open, "expected ']' in an XML path");
                ++i;
            }
            if (i < text.size() && attribute)
                return wrong(i, "an attribute must be the last step of an XML path");
            if (i < text.size() && text[i] != '/')
                return wrong(i, "expected '/' after ']' in an XML path");
        }
        return scan;
    }

    // The table of a path's steps (scanPath), its errors reported at their bytes; null when it has any. Its ints are a
    // format's, and formatArguments checks them.
    llvm::Value* Lowering::preparedPath(const ast::StringExpr& literal)
    {
        const PathScan scan = scanPath(literal.value);
        for (const FormatScan::Error& wrong : scan.errors)
            error(literal.at[wrong.offset], wrong.message);
        if (!scan.errors.empty())
            return nullptr;
        return table(module_, scan.code, scan.bytes, ".path");
    }

    // The table of an XML path's steps (scanXmlPath), as preparedPath's; an error about an empty literal's first byte,
    // which it has none of, is at `where`, the literal.
    llvm::Value* Lowering::preparedXmlPath(const ast::StringExpr& literal, SourceLocation where)
    {
        const PathScan scan = scanXmlPath(literal.value);
        for (const FormatScan::Error& wrong : scan.errors)
            error(wrong.offset < literal.at.size() ? literal.at[wrong.offset] : where, wrong.message);
        if (!scan.errors.empty())
            return nullptr;
        return table(module_, scan.code, scan.bytes, ".xmlpath");
    }

    // json_choice's and xml_choice's literals, one table: their lengths, then their bytes one after another.
    llvm::Value* Lowering::preparedChoices(const std::vector<const ast::StringExpr*>& literals)
    {
        std::vector<std::int32_t> lengths;
        std::string bytes;
        for (const ast::StringExpr* literal : literals)
        {
            lengths.push_back(static_cast<std::int32_t>(literal->value.size()));
            bytes += literal->value;
        }
        return table(module_, lengths, bytes, ".choices");
    }
} // namespace fminus::ir::detail
